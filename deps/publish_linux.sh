#!/usr/bin/env bash
set -euo pipefail

here=$(cd -- $(dirname -- ${BASH_SOURCE[0]}) && pwd)
. $here/linux_paths.sh
tag=$(tr -d '[:space:]' <$here/RUNTIME_VERSION)
asset=$igmt_archive

[[ -f $asset ]] || bash $here/build.sh

# Resolved only AFTER the build, so the path being translated exists.
gh_cmd=gh
asset_arg=$asset
if ! command -v gh >/dev/null 2>&1; then
    # No gh inside WSL: fall back to the Windows one, which cannot open a Linux path
    # ("CreateFile /home/...: The system cannot find the path specified") -- it needs the Windows
    # form, so translate. For a file in the WSL filesystem that is a \\wsl.localhost\... UNC path,
    # which gh.exe opens fine.
    gh_cmd=gh.exe
    asset_arg=$(wslpath -w $asset)
fi

if $gh_cmd release view $tag --repo GenericMappingTools/InteractiveGMT >/dev/null 2>&1; then
    $gh_cmd release upload $tag $asset_arg --repo GenericMappingTools/InteractiveGMT --clobber
else
    $gh_cmd release create $tag $asset_arg --repo GenericMappingTools/InteractiveGMT \
        --title gmtvtk_runtime_${tag#runtime-} --notes VTK_Qt_TBB_runtime_bundles
fi
