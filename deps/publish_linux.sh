#!/usr/bin/env bash
set -euo pipefail

here=$(cd -- $(dirname -- ${BASH_SOURCE[0]}) && pwd)
tag=$(tr -d '[:space:]' <$here/RUNTIME_VERSION)
asset=$here/iGMT-linux-x86_64-full.tar.gz
gh_cmd=gh
asset_arg=$asset
if ! command -v gh >/dev/null 2>&1; then
    # No gh inside WSL: fall back to the Windows one, which cannot open a /mnt/c path
    # ("CreateFile /mnt/c/...: The system cannot find the path specified") -- it needs the
    # drive-letter form, so translate the asset path for it.
    gh_cmd=gh.exe
    asset_arg=$(wslpath -w $asset 2>/dev/null || printf '%s' $asset)
fi

[[ -f $asset ]] || bash $here/build.sh
if $gh_cmd release view $tag --repo GenericMappingTools/InteractiveGMT >/dev/null 2>&1; then
    $gh_cmd release upload $tag $asset_arg --repo GenericMappingTools/InteractiveGMT --clobber
else
    $gh_cmd release create $tag $asset_arg --repo GenericMappingTools/InteractiveGMT \
        --title gmtvtk_runtime_${tag#runtime-} --notes VTK_Qt_TBB_runtime_bundles
fi
