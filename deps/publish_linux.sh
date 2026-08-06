#!/usr/bin/env bash
set -euo pipefail

here=$(cd -- $(dirname -- ${BASH_SOURCE[0]}) && pwd)
tag=$(tr -d '[:space:]' <$here/RUNTIME_VERSION)
asset=$here/iGMT-linux-x86_64-full.tar.gz
gh_cmd=gh
command -v gh >/dev/null 2>&1 || gh_cmd=gh.exe

[[ -f $asset ]] || bash $here/build.sh
if $gh_cmd release view $tag --repo GenericMappingTools/InteractiveGMT >/dev/null 2>&1; then
    $gh_cmd release upload $tag $asset --repo GenericMappingTools/InteractiveGMT --clobber
else
    $gh_cmd release create $tag $asset --repo GenericMappingTools/InteractiveGMT \
        --title gmtvtk_runtime_${tag#runtime-} --notes VTK_Qt_TBB_runtime_bundles
fi
