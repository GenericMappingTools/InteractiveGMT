#!/usr/bin/env bash
set -euo pipefail

# Publish the Linux binaries. TWO streams, exactly like Windows (see deps/PACKAGING.md):
#
#   * iGMT-linux-x86_64-full.tar.gz -> the tag in deps/RUNTIME_VERSION. The whole VTK/Qt/TBB
#     runtime; re-uploaded in place under the same tag, bumped only when the module set changes.
#   * gmtvtk-linux-x86_64.tar.gz    -> the fixed `dll-latest` tag. Just libgmtvtk.so + its
#     `.so_requires` manifest, overwritten in place on every rebuild. This is the light update
#     path: a daily C++ change costs ~2 MB instead of the ~200 MB bundle.
#
#   bash deps/publish_linux.sh          # both
#   bash deps/publish_linux.sh --so     # the rolling .so only (the usual case)
#   bash deps/publish_linux.sh --full   # the runtime bundle only

here=$(cd -- $(dirname -- ${BASH_SOURCE[0]}) && pwd)
. $here/linux_paths.sh
tag=$(tr -d '[:space:]' <$here/RUNTIME_VERSION)
so_tag=dll-latest            # fixed, never retagged -- must match DLL_TAG in deps/build.jl
repo=GenericMappingTools/InteractiveGMT

do_full=1
do_so=1
case ${1:-} in
    --so)   do_full=0 ;;
    --full) do_so=0 ;;
    "")     ;;
    *)      printf '%s\n' "usage: $(basename $0) [--so|--full]" >&2; exit 2 ;;
esac

# One build produces both archives, so a missing one means the build has not run yet.
if [[ ! -f $igmt_archive || ! -f $igmt_so_archive ]]; then
    bash $here/build.sh
fi

# Resolved only AFTER the build, so the path being translated exists.
gh_cmd=gh
translate=0
if ! command -v gh >/dev/null 2>&1; then
    # No gh inside WSL: fall back to the Windows one, which cannot open a Linux path
    # ("CreateFile /home/...: The system cannot find the path specified") -- it needs the Windows
    # form, so translate. For a file in the WSL filesystem that is a \\wsl.localhost\... UNC path,
    # which gh.exe opens fine.
    gh_cmd=gh.exe
    translate=1
fi
gh_path() { [[ $translate -eq 1 ]] && wslpath -w $1 || printf '%s\n' $1; }

# Upload one asset to one tag, creating the release the first time.
publish() {
    local rel_tag=$1 asset=$2 title=$3 notes=$4
    local arg=$(gh_path $asset)
    if $gh_cmd release view $rel_tag --repo $repo >/dev/null 2>&1; then
        $gh_cmd release upload $rel_tag $arg --repo $repo --clobber
    else
        $gh_cmd release create $rel_tag $arg --repo $repo --title $title --notes $notes
    fi
}

[[ $do_full -eq 1 ]] && publish $tag $igmt_archive \
    gmtvtk_runtime_${tag#runtime-} VTK_Qt_TBB_runtime_bundles
[[ $do_so -eq 1 ]] && publish $so_tag $igmt_so_archive \
    gmtvtk_rolling_library Always_the_latest_gmtvtk_build
exit 0
