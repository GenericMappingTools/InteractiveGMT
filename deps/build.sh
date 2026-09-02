#!/usr/bin/env bash
set -euo pipefail

# WSL compiles directly from the Windows checkout, but writes NOTHING back into it -- the cmake
# cache, the staged runtime and the archive all live under the WSL cache (see linux_paths.sh for
# why). The staged tree never overlaps deps/build, which build.bat owns on the Windows side.
here=$(cd -- $(dirname -- ${BASH_SOURCE[0]}) && pwd)
. $here/linux_paths.sh
igmt_drop_in_tree_build
# Dependency root, most explicit first: IGMT_CONDA_PREFIX (a fixed build environment that must not
# change when a shell happens to have another env activated), then the activated env, then this
# machine's base Conda. Never guess further than that -- a wrong prefix produces a bundle that
# links half of one environment and half of another.
conda_prefix=${IGMT_CONDA_PREFIX:-${CONDA_PREFIX:-/home/j/anaconda3}}
if [[ ! -d $conda_prefix/lib ]]; then
    printf '%s\n' Conda_prefix_has_no_lib_dir:$conda_prefix >&2
    printf '%s\n' 'Set IGMT_CONDA_PREFIX=/path/to/env (needs qt6-main, vtk, tbb, cmake>=3.27, patchelf).' >&2
    exit 1
fi
cmake_dir=$igmt_cmake_dir
bundle_dir=$igmt_bundle_dir
mkdir -p $igmt_stage_root
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
# By name, not `all`: `all` would also drag in gmtvtk_demo, which nothing ships. igmt is the desktop
# launcher (deps/src/launcher.c) — the install step below stages it beside the .so, so it must exist
# by then. gmtvtk_test is the test-only twin (same source, GMTVTK_TEST_API defined): it is NOT part
# of the runtime bundle, but it rides in the rolling .so archive, because without it every test that
# asserts through the test API can only fail on a machine that did not build the library itself —
# which is every CI runner.
$cmake_exe --build $cmake_dir --target gmtvtk igmt gmtvtk_test

# Recreate the staging tree after a successful compile. Otherwise libraries belonging to plugins
# removed from the bundle policy (WebEngine, QML, SQL, Wayland, etc.) survive forever.
if [[ $bundle_dir != $igmt_stage_root/build_linux ]]; then
    printf '%s\n' Refusing_to_clean_unexpected_bundle_path:$bundle_dir >&2
    exit 1
fi
rm -rf -- $bundle_dir
$cmake_exe --install $cmake_dir --prefix $bundle_dir

# The test twin has no install rule — it must never reach a user through the runtime bundle — so it
# is staged by hand, and BEFORE the strip/patchelf pass below, so it ends up with the same $ORIGIN
# rpath as its production twin and finds the bundled Qt/VTK sitting beside it.
cp -f -- $cmake_dir/libgmtvtk_test.so $bundle_dir/

# Conda's shared objects retain large symbol/debug tables. Strip only the staged copies; the
# libraries in the Conda environment remain untouched.
strip_exe=$(command -v strip)
find $bundle_dir -type f -name '*.so*' -exec $strip_exe --strip-unneeded {} +

patchelf_exe=$conda_prefix/bin/patchelf
find $bundle_dir -maxdepth 1 -type f -name '*.so*' -exec $patchelf_exe --set-rpath '$ORIGIN' {} +
find $bundle_dir/plugins -type f -name '*.so*' -exec $patchelf_exe --set-rpath '$ORIGIN/../..' {} +

# Two different failures wear the same "not found" text, so separate them. A SONAME listed in
# .host_requires (the GLVND set, deliberately not bundled) missing here says the BUILD MACHINE lacks
# a runtime package -- the archive is still correct, so say what to install and carry on. Anything
# else missing is a real hole in the bundle and must stop the build.
host_owned=$(grep -v '^#' $bundle_dir/.host_requires | grep . || true)
missing=$(env -u LD_LIBRARY_PATH ldd $bundle_dir/libgmtvtk.so $bundle_dir/plugins/*/*.so 2>/dev/null |
          sed -n 's/^\s*\([^ ]*\) => not found$/\1/p' | sort -u || true)
host_missing=
bundle_missing=
for lib in $missing; do
    if printf '%s\n' $host_owned | grep -qx $lib; then
        host_missing="$host_missing $lib"
    else
        bundle_missing="$bundle_missing $lib"
    fi
done
if [[ -n $bundle_missing ]]; then
    printf '%s\n' Missing_from_bundle:$bundle_missing >&2
    exit 1
fi
if [[ -n $host_missing ]]; then
    printf '%s\n' "WARNING: this machine lacks host GL libraries:$host_missing" >&2
    printf '%s\n' 'The archive is fine, but the viewer will not run here until you install them:' >&2
    printf '%s\n' '  sudo apt install libgl1 libglx0 libegl1 libopengl0' >&2
fi
printf '%s\n' Linux_bundle_verified:$bundle_dir/libgmtvtk.so

# `.so_requires` -- the CONTRACT between the two archives, and the exact counterpart of the Windows
# `.dll_requires` deps/CMakeLists.txt writes. The rolling .so can outrun the installed bundle: link
# one library the published full tarball predates and dlopen fails with a message about a SONAME
# that says nothing about the real cause. Listing only the SONAMEs THE BUNDLE ITSELF PROVIDES (a
# name resolved by ldd for which a file of that name is staged here) is what makes the check on the
# other side meaningful -- libc/libm/libstdc++ and the GLVND set in .host_requires come from the
# host and are not ours to ship. Written BEFORE both tars, so the full bundle carries it too.
needed=$(env -u LD_LIBRARY_PATH ldd $bundle_dir/libgmtvtk.so 2>/dev/null |
         sed -n 's/^[[:space:]]*\([^ ]*\) =>.*/\1/p' | sort -u || true)
{
    printf '%s\n' '# libgmtvtk.so bundle dependencies, generated by deps/build.sh. One SONAME per line.'
    for lib in $needed; do
        [[ -e $bundle_dir/$lib ]] && printf '%s\n' $lib
    done
} >$bundle_dir/.so_requires

# Members must come out as deps/build/... -- that is what deps/build.jl extracts relative to
# SHARED_ROOT. The staged tree is no longer under deps/, so the transform renames the single
# top-level component instead of rewriting a path prefix.
archive=$igmt_archive
# --exclude the test twin: it is staged in this tree only so the strip/patchelf pass reaches it and
# the ROLLING archive can carry it. The runtime bundle is what a user installs, and the test API has
# no business on a user's machine.
tar -C $igmt_stage_root --transform='s,^build_linux,deps/build,' --exclude='libgmtvtk_test.so' \
    -czf $archive build_linux
printf '%s\n' Linux_archive_created:$archive

# ...and the rolling one: the .so plus its manifest, and the desktop launcher. Same member paths, so
# it extracts over an installed bundle exactly the way the Windows dll zip does.
#
# igmt rides in the ROLLING archive, exactly as igmt.exe does in gmtvtk-win64.zip: a machine that
# already has the pinned runtime only ever fetches this one, so leaving the launcher out of it would
# mean no desktop icon until the next RUNTIME_VERSION bump. It links nothing from the bundle (X11 and
# libc only), so it cannot desync from the runtime it travels with.
# libgmtvtk_test.so rides here too — see the target list above. It is the test API's only home, and
# a runner cannot build it (that needs the Qt/VTK SDK, not the runtime bundle).
tar -C $igmt_stage_root --transform='s,^build_linux,deps/build,' -czf $igmt_so_archive \
    build_linux/libgmtvtk.so build_linux/.so_requires build_linux/igmt build_linux/libgmtvtk_test.so
printf '%s\n' Linux_so_archive_created:$igmt_so_archive

# A tarball left by the older scripts sits in the package tree, where nothing Linux belongs.
rm -f -- $here/iGMT-linux-x86_64-full.tar.gz
