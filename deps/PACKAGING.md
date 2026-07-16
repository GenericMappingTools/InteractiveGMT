# Packaging & releasing gmtvtk binaries

Two release streams, two cadences. `deps/build.jl` (the Julia `Pkg.build` hook) pulls from both.

## 1. Build the packages (CMake + CPack)

Configure once with packaging on:

```
cmake -B deps/build -S deps -DGMTVTK_PACKAGE=ON
cmake --build deps/build
```

Then, from `deps/build/` (cmd.exe):

**Full zip** (gmtvtk.dll + bundled VTK/Qt/TBB runtime + Qt plugins + Julia bridge):
```
cpack -G ZIP -D CPACK_COMPONENTS_ALL=full
```
→ `iGMT-win64-full.zip`

**DLL-only zip** (just gmtvtk.dll):
```
cpack -G ZIP -D CPACK_COMPONENTS_ALL=dll
```
→ `gmtvtk-win64.zip`

Use the standalone CMake's `cpack` (`C:\programs\CMake\bin`, v3.31.6) — that's what's on PATH.
The VS-bundled `cpack.exe` (VS2026/18 install, v4.2.3-msvc3) has a broken `-D` flag: it never
propagates into the generated `CPackConfig.cmake`, so `-D CPACK_COMPONENTS_ALL=...` silently
does nothing and both components get built every time. If `-D` ever seems to stop filtering,
check `where cpack` first — you're probably invoking the wrong one.

Fallback if you ever are stuck on the VS-bundled cpack: write `CPACK_COMPONENTS_ALL` into
`CPackProperties.cmake` (in the build dir) instead of passing `-D` — CPack always includes that
file if present, regardless of which cpack binary you're running. Two templates are checked in
for this: `deps/cpack-full.cmake` / `deps/cpack-dll.cmake` (copy one over `CPackProperties.cmake`
before running plain `cpack -G ZIP`).

An NSIS installer (`iGMT-<version>-win64.exe`) also gets built alongside — it's monolithic
(always the full set), unaffected by the component split.

## 2. Upload to GitHub Releases

**Runtime release** — tag = whatever's in `deps/RUNTIME_VERSION` (currently `runtime-0.1`).
Bump the tag + that file ONLY when the VTK/Qt/TBB module set changes (rare).

```
gh release create runtime-0.1 deps/build/iGMT-win64-full.zip --repo GenericMappingTools/InteractiveGMT --title "gmtvtk runtime 0.1" --notes "VTK/Qt/TBB runtime bundle"
```

**DLL release** — fixed tag `dll-latest` (hardcoded as `DLL_TAG` in `deps/build.jl`, never
retagged). First time:

```
gh release create dll-latest deps/build/gmtvtk-win64.zip --repo GenericMappingTools/InteractiveGMT --title "gmtvtk.dll (rolling)" --notes "Always the latest DLL build"
```

Every rebuild after that, same tag, overwrite the asset in place:

```
gh release upload dll-latest deps/build/gmtvtk-win64.zip --repo GenericMappingTools/InteractiveGMT --clobber
```

## 3. How build.jl finds them

- `REPO` const — the GitHub repo.
- `runtime_tag()` — reads `deps/RUNTIME_VERSION` for the full-zip tag (first install only,
  marker-gated: `deps/build/.full_runtime_installed`).
- `DLL_TAG = "dll-latest"` — hardcoded, used on every `Pkg.build("InteractiveGMT")` after the
  first install.

No other coordination needed — just keep `deps/RUNTIME_VERSION` in sync with whichever runtime
tag you actually created.
