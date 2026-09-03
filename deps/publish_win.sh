#!/usr/bin/env bash
# Publish the runtime-0.3 artifacts: Windows (built here by hand) + macOS (built by CI).
#
#   bash deps/publish_win.sh
#
# WHY runtime-0.3 EXISTS. a20c4da made gmtvtk link two new VTK modules -- vtkIOGeometry (.obj/.stl/
# .off/.g/.gltf) and vtkIOPLY (.ply). Verified 2026-09-03: the PUBLISHED runtime-0.2 bundle carries
# NEITHER, on Windows or macOS. Ship the new library against it and LoadLibrary/dlopen fails on
# every existing install -- the 2026-07-29 vtkIOHDF trap. Hence a new full bundle + the
# deps/RUNTIME_VERSION bump, which is what makes machines that already have 0.2 re-fetch.
#
# ORDER MATTERS. deps/RUNTIME_VERSION on master already says runtime-0.3, so the full bundle has to
# be up BEFORE the rolling library: a machine that takes the new gmtvtk first has no runtime to
# load it against.
#
# LINUX IS DEFERRED. LinuxBinaries fails before it compiles anything -- the conda vtk-9.7 package in
# the CI env ships .../cmake/vtk-9.7/VTK-targets.cmake referencing libvtkIOFFMPEG-9.7.so.9.7, which
# the package does not contain, so find_package(VTK) dies at CMakeLists.txt:101. That is an
# environment/pin problem, not a source one. When it builds, publish it the same way:
#     gh workflow run LinuxBinaries.yml -f publish=full
set -euo pipefail

cd "$(dirname "$0")/.."
TAG=$(tr -d '[:space:]' < deps/RUNTIME_VERSION)
FULL="deps/build/iGMT-win64-full.zip"
DLL="deps/build/gmtvtk-win64.zip"

for f in "$FULL" "$DLL"; do
	[ -f "$f" ] || { echo "missing $f -- run cpack -G ZIP in deps/build first"; exit 1; }
done

# Refuse to publish a gmtvtk.dll the installed runtime cannot load.
powershell -ExecutionPolicy Bypass -File deps/check_dll_deps.ps1 -DllZip "$DLL"

echo "==> windows full bundle -> $TAG"
gh release view "$TAG" >/dev/null 2>&1 || gh release create "$TAG" \
	--title "gmtvtk runtime ${TAG#runtime-}" \
	--notes "VTK/Qt/TBB runtime bundle for the gmtvtk viewer.

Adds the 3-D mesh-exchange reader modules gmtvtk now links, which runtime-0.2 does not carry:
  - vtkIOGeometry (.obj / .stl / .off / .g, plus vtkGLTFReader for .gltf/.glb)
  - vtkIOPLY (.ply)

Pinned by deps/RUNTIME_VERSION. Linux assets follow once its CI env is repaired."
gh release upload "$TAG" "$FULL" --clobber

echo "==> windows rolling dll -> dll-latest"
gh release upload dll-latest "$DLL" --clobber

# macOS builds green again since 5892935 (the unguarded Win32 GetEnvironmentVariableA in envFlag()).
# 'full' (not 'rolling') so each arch also uploads its own iGMT-macos-<arch>-full.tar.gz to $TAG.
echo "==> macos full builds (arm64 + x86_64)"
gh workflow run MacBinaries.yml -f publish=full

echo "done. watch: gh run list --limit 5"
