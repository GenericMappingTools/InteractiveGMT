#!/usr/bin/env bash
set -euo pipefail

# WSL compiles directly from the Windows checkout. The cache is private; build_linux is the
# distributable Linux runtime and never overlaps deps/build used by build.bat.
here=$(cd -- $(dirname -- ${BASH_SOURCE[0]}) && pwd)
conda_prefix=${CONDA_PREFIX:-/home/j/anaconda3}
cmake_dir=$here/.cmake_linux
bundle_dir=$here/build_linux
cxx=
cxx_link=
probe=$(mktemp /tmp/igmt-cxx-XXXXXX)
trap 'rm -f $probe' EXIT

# Qt and VTK may live in a named Conda environment rather than base. Locate their existing CMake
# packages across the Conda tree instead of installing, copying, or assuming another version.
qt6_config=$(find -L $conda_prefix /usr /usr/local /home/j/programs -type f -name Qt6Config.cmake -print -quit 2>/dev/null || true)
vtk_config=$(find -L $conda_prefix -type f \( -name VTKConfig.cmake -o -name vtk-config.cmake \) -print -quit 2>/dev/null || true)
if [[ -z $qt6_config ]]; then
    $conda_prefix/bin/conda install --yes --freeze-installed qtbase-devel
    qt6_config=$(find -L $conda_prefix /usr /usr/local /home/j/programs -type f -name Qt6Config.cmake -print -quit 2>/dev/null || true)
    if [[ -z $qt6_config ]]; then
        printf '%s\n' qtbase-devel_did_not_provide_Qt6Config.cmake >&2
        exit 1
    fi
fi

# VTK 9.6 exports the COMPILE_ONLY generator expression, which needs CMake 3.27 or newer.
# Keep build tools in the same Conda prefix as VTK/Qt so package discovery is consistent.
if [[ -x $conda_prefix/bin/cmake ]]; then
    cmake_exe=$conda_prefix/bin/cmake
else
    cmake_exe=$(command -v cmake)
fi
cmake_version=$($cmake_exe --version | sed -n '1s/.* //p')
if [[ $(printf '%s\n' 3.27 $cmake_version | sort -V | head -1) != 3.27 ]] || \
   [[ -z $(find -L $conda_prefix -type f -iname 'pugixml*Config.cmake' -print -quit 2>/dev/null) ]] || \
   [[ ! -x $conda_prefix/bin/patchelf ]]; then
    $conda_prefix/bin/conda install --yes --freeze-installed 'cmake>=3.27' pugixml patchelf
    cmake_exe=$conda_prefix/bin/cmake
fi
if [[ -z $vtk_config ]]; then
    printf '%s\n' VTK_CMake_config_not_found_under_$conda_prefix >&2
    exit 1
fi
qt6_dir=$(dirname $qt6_config)
vtk_dir=$(dirname $vtk_config)

# Do not trust names or inherited Windows values: accept only a driver that really compiles and
# links a C++ program inside this WSL process.
try_cxx() {
    candidate=$1
    link_arg=${2:-}
    resolved=$(command -v $candidate 2>/dev/null || true)
    [[ -n $resolved ]] || return 1
    if printf '%s\n' 'int main() { return 0; }' | $resolved -x c++ - $link_arg -o $probe >/dev/null 2>&1; then
        cxx=$resolved
        cxx_link=$link_arg
        return 0
    fi
    return 1
}

[[ -z ${CXX:-} ]] || try_cxx $CXX || true
[[ -n $cxx ]] || try_cxx g++ || true
[[ -n $cxx ]] || try_cxx c++ || true
[[ -n $cxx ]] || try_cxx clang++ || true
[[ -n $cxx ]] || try_cxx x86_64-conda-linux-gnu-c++ || true
[[ -n $cxx ]] || try_cxx x86_64-linux-gnu-g++ || true
[[ -n $cxx ]] || try_cxx $conda_prefix/bin/mpic++ || true
[[ -n $cxx ]] || try_cxx $conda_prefix/bin/h5c++ || true
[[ -n $cxx ]] || try_cxx gcc -lstdc++ || true

if [[ -z $cxx ]]; then
    printf '%s\n' GCC_C++_probe_failed: >&2
    printf '%s\n' 'int main() { return 0; }' | gcc -x c++ - -lstdc++ -o $probe
    exit 1
fi

printf 'Linux C++ compiler: %s\n' $cxx

$cmake_exe -S $here -B $cmake_dir -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DCMAKE_CXX_COMPILER=$cxx -DCMAKE_PREFIX_PATH=$conda_prefix \
    -DQt6_DIR=$qt6_dir -DVTK_DIR=$vtk_dir \
    $cxx_link -DGMTVTK_LINUX_BUNDLE=ON $@
$cmake_exe --build $cmake_dir --target gmtvtk

# Recreate the staging tree after a successful compile. Otherwise libraries belonging to plugins
# removed from the bundle policy (WebEngine, QML, SQL, Wayland, etc.) survive forever.
if [[ $bundle_dir != $here/build_linux ]]; then
    printf '%s\n' Refusing_to_clean_unexpected_bundle_path:$bundle_dir >&2
    exit 1
fi
rm -rf -- $bundle_dir
$cmake_exe --install $cmake_dir --prefix $bundle_dir

# Conda's shared objects retain large symbol/debug tables. Strip only the staged copies; the
# libraries in the Conda environment remain untouched.
strip_exe=$(command -v strip)
find $bundle_dir -type f -name '*.so*' -exec $strip_exe --strip-unneeded {} +

patchelf_exe=$conda_prefix/bin/patchelf
find $bundle_dir -maxdepth 1 -type f -name '*.so*' -exec $patchelf_exe --set-rpath '$ORIGIN' {} +
find $bundle_dir/plugins -type f -name '*.so*' -exec $patchelf_exe --set-rpath '$ORIGIN/../..' {} +

missing=$(env -u LD_LIBRARY_PATH ldd $bundle_dir/libgmtvtk.so | grep 'not found' || true)
if [[ -n $missing ]]; then
    printf '%s\n' $missing >&2
    exit 1
fi
printf '%s\n' Linux_bundle_verified:$bundle_dir/libgmtvtk.so

archive=$here/iGMT-linux-x86_64-full.tar.gz
tar -C $here/.. --transform='s,^deps/build_linux,deps/build,' -czf $archive deps/build_linux
printf '%s\n' Linux_archive_created:$archive
