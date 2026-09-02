#!/usr/bin/env bash
set -euo pipefail

# macOS build + self-contained bundle. The counterpart of deps/build.sh, and structured the same
# way: cmake stages the runtime (deps/cmake/macos_bundle.cmake.in decides WHAT gets copied), this
# script then makes the staged copy relocatable and archives it. Two archives, the same two
# cadences deps/build.jl already implements for Windows and Linux -- the pinned full bundle and the
# rolling library-only one.
#
# Dependencies are ASSUMED PRESENT, from Homebrew. Nothing here builds or vendors VTK/Qt/TBB.
#   brew install vtk qttools tbb
# (`vtk` is already built with Qt6 -- the old `--with-qt` option does not exist any more -- but it
# pulls only qtbase+qtdeclarative, so qttools (Qt6::UiTools) and tbb must be named.)
#
# Run on the OLDEST macOS you intend to support: Homebrew bottles target the OS they were poured
# for, so a bundle staged on macOS 15 will not load on macOS 14.
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

arch=$(uname -m)                                    # arm64 | x86_64
stage_root=${IGMT_MACOS_STAGE:-${XDG_CACHE_HOME:-$HOME/Library/Caches}/igmt}
cmake_dir=$stage_root/cmake
# Staged so that the archive members come out as deps/build/... with no tar path rewriting: that is
# what deps/build.jl extracts relative to SHARED_ROOT. (build.sh needs GNU tar's --transform for
# this; macOS ships bsdtar, whose equivalent flag is spelled differently -- so the layout is simply
# built correctly in the first place.)
pkg_dir=$stage_root/pkg
bundle_dir=$pkg_dir/deps/build
archive=$stage_root/iGMT-macos-$arch-full.tar.gz
lib_archive=$stage_root/gmtvtk-macos-$arch.tar.gz

command -v brew >/dev/null 2>&1 || {
    printf '%s\n' 'Homebrew not found. Install it from https://brew.sh, then: brew install vtk qttools tbb' >&2
    exit 1
}
prefix=$(brew --prefix)

# Fail here, naming the formula, rather than 200 lines into a cmake configure error. The cmake side
# has the same check (deps/CMakeLists.txt) for people who never run this script.
missing=
for formula in vtk qttools tbb; do
    brew --prefix "$formula" >/dev/null 2>&1 || missing="$missing $formula"
done
if [[ -n $missing ]]; then
    printf '%s\n' "Missing Homebrew formulae:$missing" >&2
    printf '%s\n' "  brew install$missing" >&2
    exit 1
fi

cmake -S "$here" -B "$cmake_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$prefix;$prefix/opt/vtk;$prefix/opt/qtbase;$prefix/opt/qttools;$prefix/opt/tbb" \
    -DGMTVTK_MACOS_BUNDLE=ON "$@"
# By name, not `all`: `all` would also drag in gmtvtk_demo, which nothing ships. igmt is the desktop
# launcher (deps/src/launcher.c) — the install step below stages it beside the dylib, so it must
# exist by then. gmtvtk_test is the test-only twin (same source, GMTVTK_TEST_API defined): it is NOT
# part of the runtime bundle, but it rides in the rolling archive, because a machine that did not
# build this repo cannot produce it, and without it every test asserting through the test API fails
# as "not found" — which is every CI runner. Same arrangement as deps/build.sh.
cmake --build "$cmake_dir" --target gmtvtk igmt gmtvtk_test

# Recreate the staging tree after a successful compile, so a library that the bundle policy dropped
# (a retired Qt plugin, a VTK module no longer linked) cannot survive forever and keep being loaded
# in preference to a newer one. Same reasoning as build.sh's rm -rf.
case $bundle_dir in
    */igmt/pkg/deps/build) ;;
    *) printf '%s\n' Refusing_to_clean_unexpected_bundle_path:"$bundle_dir" >&2; exit 1 ;;
esac
rm -rf -- "$pkg_dir"
mkdir -p "$bundle_dir"
cmake --install "$cmake_dir" --prefix "$bundle_dir"

# The test twin has no install rule — it must never reach a user through the runtime bundle — so it
# is staged by hand, BEFORE the relocation pass below, so its own id and dependencies get pointed at
# the bundle and re-signed exactly like its production twin's.
cp -f -- "$cmake_dir/libgmtvtk_test.dylib" "$bundle_dir/"

# ---------------------------------------------------------------------------
# Make it relocatable. Every Mach-O in the bundle: point its own id and each of its dependencies at
# the bundle instead of at /opt/homebrew, then re-sign.
#
# Two dependency spellings arrive here and they need opposite treatment:
#   * an ABSOLUTE path (Homebrew's plain dylibs: /opt/homebrew/opt/vtk/lib/libvtkCommonCore.9.7.dylib)
#     is rewritten with -change to a @loader_path-relative one.
#   * an @rpath path (Qt, which is built as FRAMEWORKS on macOS:
#     @rpath/QtCore.framework/Versions/A/QtCore) is already relative and must be LEFT ALONE -- what
#     it needs is an LC_RPATH that points at the bundle root, which is added below. Rewriting these
#     to @loader_path as well would work for the direct dependency and then break the next one down,
#     because Qt's own frameworks reference each other by @rpath and we do not edit inside them
#     blindly.
# ---------------------------------------------------------------------------
macho_files() {
    find "$bundle_dir" -type f \( -name '*.dylib' -o -path '*.framework/Versions/*' \) ! -name '*.plist'
}

# Bundle-root-relative prefix for a file: how many levels up from ITS directory the bundle root is.
loader_prefix() {
    local rel=${1#"$bundle_dir"/} dir up
    dir=$(dirname "$rel")
    if [[ $dir == . ]]; then
        printf '%s' '@loader_path'
        return
    fi
    up='@loader_path'
    while [[ $dir != . ]]; do
        up="$up/.."
        dir=$(dirname "$dir")
    done
    printf '%s' "$up"
}

# Where a dependency path would live INSIDE the bundle: the framework-relative suffix for a
# framework, the bare filename for a plain dylib. Empty if the bundle does not carry it.
bundle_rel() {
    local dep=$1 rel=
    case $dep in
        *.framework/*) rel=$(printf '%s' "$dep" | sed -n 's|.*/\([^/]*\.framework/.*\)$|\1|p') ;;
        *)             rel=${dep##*/} ;;
    esac
    [[ -n $rel && -e $bundle_dir/$rel ]] && printf '%s' "$rel"
    return 0
}

while IFS= read -r f; do
    prefix_rel=$(loader_prefix "$f")
    self_rel=${f#"$bundle_dir"/}
    case $f in
        *.framework/*) install_name_tool -id "@rpath/$(bundle_rel "$f")" "$f" 2>/dev/null || true ;;
        *)             install_name_tool -id "$prefix_rel/$self_rel" "$f" 2>/dev/null || true ;;
    esac
    # An LC_RPATH per file rather than one on libgmtvtk.dylib alone: @rpath is resolved against the
    # rpaths of the file DOING the loading, so a Qt framework loading another Qt framework needs its
    # own. Duplicates are harmless; install_name_tool refuses them, hence the || true.
    install_name_tool -add_rpath "$prefix_rel" "$f" 2>/dev/null || true
    otool -L "$f" | tail -n +2 | awk '{print $1}' | while IFS= read -r dep; do
        case $dep in
            /System/*|/usr/lib/*|@rpath/*|@loader_path/*|@executable_path/*) continue ;;
        esac
        rel=$(bundle_rel "$dep")
        [[ -n $rel ]] || continue
        install_name_tool -change "$dep" "$prefix_rel/$rel" "$f"
    done
done < <(macho_files)

# install_name_tool invalidates a code signature, and on Apple Silicon an invalid signature is not a
# warning -- dyld refuses to load the image at all. Ad-hoc (-s -) is enough: this is not distributed
# through the App Store and is not notarized; it only has to be internally consistent. Frameworks
# are signed as the BUNDLE, not as the bare binary inside them.
while IFS= read -r fw; do
    codesign --force --sign - "$fw"
done < <(find "$bundle_dir" -type d -name '*.framework')
while IFS= read -r f; do
    case $f in *.framework/*) continue ;; esac
    codesign --force --sign - "$f"
done < <(macho_files)

# Verification, not decoration: a single surviving absolute Homebrew path means the bundle works on
# THIS machine (where that path exists) and fails on the user's. That is the exact failure this
# whole script exists to prevent, so it stops the build.
leaked=$(macho_files | while IFS= read -r f; do otool -L "$f" | tail -n +2 | grep -F "$prefix/" || true; done | sort -u)
if [[ -n $leaked ]]; then
    printf '%s\n' 'Bundle still references Homebrew paths:' >&2
    printf '%s\n' "$leaked" >&2
    exit 1
fi
printf '%s\n' macOS_bundle_verified:"$bundle_dir/libgmtvtk.dylib"

# `.dylib_requires` -- the CONTRACT between the two archives, and the exact counterpart of Windows'
# `.dll_requires` and Linux's `.so_requires`. The rolling library can outrun the installed bundle:
# link one module the published full tarball predates and dlopen fails naming a path that says
# nothing about the real cause. Only what THE BUNDLE ITSELF PROVIDES is listed -- the system
# frameworks under /usr/lib and /System are the OS's, not ours to ship or to check.
{
    printf '%s\n' '# libgmtvtk.dylib bundle dependencies, generated by deps/build_mac.sh. One path per line.'
    otool -L "$bundle_dir/libgmtvtk.dylib" | tail -n +2 | awk '{print $1}' | while IFS= read -r dep; do
        rel=${dep#@loader_path/}
        rel=${rel#@rpath/}
        # `if`, NOT `[[ ... ]] && printf`: this is the LAST command of the loop body, so with a bare
        # && a false test makes the whole `while` exit 1, pipefail carries that through `| sort -u`
        # and set -e kills the script. The list always ends on a dep that is NOT in the bundle --
        # /usr/lib/libc++, libSystem -- so it failed every single time (2026-08-31). bundle_rel()
        # above dodges the same shape with an explicit `return 0`.
        if [[ -e $bundle_dir/$rel ]]; then printf '%s\n' "$rel"; fi
    done | sort -u
} >"$bundle_dir/.dylib_requires"

# --exclude the test twin: it is staged in this tree only so the relocation pass reaches it and the
# ROLLING archive can carry it. The runtime bundle is what a user installs.
tar -C "$pkg_dir" --exclude='libgmtvtk_test.dylib' -czf "$archive" deps/build
printf '%s\n' macOS_archive_created:"$archive"

# ...and the rolling one: the dylib plus its manifest, and the desktop launcher. Same member paths,
# so it extracts over an installed bundle exactly the way the Windows dll zip does.
#
# igmt rides in the ROLLING archive, exactly as igmt.exe does in gmtvtk-win64.zip: a machine that
# already has the pinned runtime only ever fetches this one, so leaving the launcher out of it would
# mean no desktop icon until the next RUNTIME_VERSION bump. It links nothing from the bundle (Cocoa
# and libSystem only), so it cannot desync from the runtime it travels with.
# libgmtvtk_test.dylib rides here too — see the target list above; it is the test API's only home.
tar -C "$pkg_dir" -czf "$lib_archive" deps/build/libgmtvtk.dylib deps/build/.dylib_requires \
    deps/build/igmt deps/build/libgmtvtk_test.dylib
printf '%s\n' macOS_lib_archive_created:"$lib_archive"
