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
file if present, regardless of which cpack binary you're running:
```
echo set(CPACK_COMPONENTS_ALL "dll")> CPackProperties.cmake && cpack -G ZIP
```
(swap `"dll"` for `"full"` for the runtime zip).

An NSIS installer (`iGMT-<version>-win64.exe`) also gets built alongside — it's monolithic
(always the full set), unaffected by the component split.

## 2. Upload to GitHub Releases

**Runtime release** — tag = whatever's in `deps/RUNTIME_VERSION` (currently `runtime-0.2`).
Bump the tag + that file ONLY when the VTK/Qt/TBB module set changes (rare).

```
gh release create runtime-0.2 deps/build/iGMT-win64-full.zip --repo GenericMappingTools/InteractiveGMT --title "gmtvtk runtime 0.2" --notes "Windows and Linux x86_64 VTK/Qt/TBB runtime bundles"
```

The Linux assets have TWO routes, both running the same `deps/build.sh` and producing the same two
archives. CI is the normal one: `.github/workflows/LinuxBinaries.yml` builds on every push that
touches the C++ side, leaves both tarballs as workflow artifacts, and uploads to the two release
tags only on a manual run that asks for it (`gh workflow run LinuxBinaries.yml -f
publish=rolling|full`, or the "Run workflow" button). It builds on ubuntu-22.04 against a
conda-forge Miniforge environment -- the same package set the WSL machine uses -- because the
runner's glibc is the floor for every machine that installs the result.

The WSL route below stays as the backup, and is what to reach for when CI is down or a bundle has
to be tested on a real display.

The Linux assets are built and uploaded from WSL by `bash deps/publish_linux.sh`, which runs
`deps/build.sh` if needed and re-uploads with `--clobber`. Nothing it produces lives in the
checkout: the cmake dir, the staged runtime and both tarballs are all under `~/.cache/igmt` (see
`deps/linux_paths.sh`), because a WSL symlink inside the package tree makes every Windows
`Pkg.test` run die on `stat(): permission denied` before a single test starts.

One build produces BOTH Linux archives, and `publish_linux.sh` takes which to send:

```
bash deps/publish_linux.sh          # both
bash deps/publish_linux.sh --so     # the rolling libgmtvtk.so only -- the usual case
bash deps/publish_linux.sh --full   # the runtime bundle only
```

The macOS assets are built by CI (`.github/workflows/MacBinaries.yml`, one job per architecture —
there is no Mac on this desk) and left as workflow ARTIFACTS, whose URL changes with every run and
expires. Moving them to the two release tags is `julia deps/publish_mac.jl`, which looks up the
newest successful run itself, downloads both architectures' artifacts, checks inside each archive
and uploads:

```
julia deps/publish_mac.jl                    # rolling libgmtvtk.dylib, both arches -- the usual case
julia deps/publish_mac.jl --full             # the runtime bundles too (the rare one)
julia deps/publish_mac.jl --arch arm64       # one architecture
julia deps/publish_mac.jl --run 33347252202  # a specific run instead of the newest green one
julia deps/publish_mac.jl --dry              # download + verify, upload nothing
```

The workflow can also publish on its own, but only on a manual run that says so up front
(`gh workflow run MacBinaries.yml -f publish=rolling|full`, or the "Run workflow" button in the
Actions tab). `publish_mac.jl` needs no such run — it publishes the artifacts of the ordinary push
build that already happened.

**Rolling library release** — fixed tag `dll-latest` (hardcoded as `DLL_TAG` in `deps/build.jl`,
never retagged), one asset per platform: `gmtvtk-win64.zip` and `gmtvtk-linux-x86_64.tar.gz`. Both
carry the library plus its requires-manifest (`.dll_requires` / `.so_requires`) and nothing else, so
a C++ change costs a couple of MB instead of the whole runtime. Linux goes up through
`LinuxBinaries.yml -f publish=rolling` or, from WSL, `publish_linux.sh --so`; Windows, by hand.
First time:

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
