# Memory

> OpenWolf session log. `| HH:MM | description | file(s) | outcome | ~tokens |`

## 2026-06-16 — Migration from GMTF3D/qtvtk_proto

| HH:MM | description | file(s) | outcome | ~tokens |
|-------|-------------|---------|---------|---------|
| —     | Scaffold dirs (src, deps/src, deps/assets, examples, .wolf) | (mkdir) | done | — |
| —     | Slice main.cpp → 00_includes…90_c_api .inc (byte-exact, diff-verified) | deps/src/*.inc | identical | — |
| —     | Umbrella TU + migrated build files | deps/src/gmtvtk.cpp, deps/CMakeLists.txt, deps/build.bat, deps/run.bat | done | — |
| —     | Split bridge.jl → 14 src/*.jl; dlopen/dlsym/@cfunction moved to __init__; `_fn(:sym)` | src/*.jl | parse-clean | — |
| —     | Project.toml deps (GMT, Libdl); examples ported; CI-safe tests | Project.toml, examples/*, test/test-basic-test.jl | parse-clean | — |
| —     | Migrate OpenWolf + QTVTK_PLAN + qtvtk memory | CLAUDE.md, .wolf/*, QTVTK_PLAN.md, .wolf/knowledge/* | done | — |

### Session summary

Migrated the whole Qt+VTK viewer from `GMTF3D/qtvtk_proto` into the new `InteractiveGMT` Julia
package. `bridge.jl` → 14 focused `src/*.jl` files, made into a real package (runtime DLL load in
`__init__`, `_fn(:sym)` ccalls). `main.cpp` → include-based single TU under `deps/src/` with the
C API isolated in `90_c_api.inc`; build files under `deps/`. Demos → `examples/`, CI-safe helper
tests in `test/`. OpenWolf knowledge (cerebrum, anatomy, QTVTK_PLAN, qtvtk memory) carried over.

### Verified 2026-06-16

- `deps/build.bat` builds gmtvtk.dll (223 KB) + gmtvtk_demo.exe clean from the new single-TU
  layout (configure 4s, 2 TU compiles + 2 links, no errors). Run via PowerShell `& cmd /c`.
- `using InteractiveGMT` precompiles + loads; pure helpers pass; DLL dlopens, 16/16 C-API
  symbols resolve. (Mistakenly claimed earlier I couldn't build here — corrected in cerebrum.)

### Still for the user (interactive display only)

- A real `view_grid(GMT.peaks())` / `gmtvtk_demo.exe` opens a window and renders.
- Then delete the source `GMTF3D/qtvtk_proto` (left in place for A/B until confirmed).

## Session: 2026-06-16 20:49

| Time | Action | File(s) | Outcome | ~Tokens |
|------|--------|---------|---------|--------|
| 21:14 | Edited src/InteractiveGMT.jl | inline fix | ~3 |
| 21:14 | Session end: 1 writes across 1 files (InteractiveGMT.jl) | 2 reads | ~4 tok |
| 23:02 | Edited src/libgmtvtk.jl | 1→4 lines | ~44 |
| 23:02 | Edited Project.toml | 2→1 lines | ~12 |
| 23:02 | Session end: 3 writes across 3 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml) | 3 reads | ~64 tok |
| 23:52 | Edited deps/src/20_gizmo.inc | added 2 condition(s) | ~195 |
| 23:52 | Session end: 4 writes across 4 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc) | 5 reads | ~273 tok |
| 23:56 | Session end: 4 writes across 4 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc) | 7 reads | ~273 tok |
| 23:57 | Session end: 4 writes across 4 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc) | 7 reads | ~273 tok |
| 00:02 | Edited deps/src/20_gizmo.inc | added 2 condition(s) | ~244 |
| 00:02 | Session end: 5 writes across 4 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc) | 7 reads | ~534 tok |
| 00:04 | Edited deps/src/20_gizmo.inc | added 2 condition(s) | ~268 |
| 00:04 | Session end: 6 writes across 4 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc) | 7 reads | ~821 tok |
| 00:08 | Edited deps/src/20_gizmo.inc | modified if() | ~152 |
| 00:08 | Session end: 7 writes across 4 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc) | 7 reads | ~984 tok |
| 00:14 | Session end: 7 writes across 4 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc) | 7 reads | ~984 tok |
| 00:27 | Edited deps/src/20_gizmo.inc | modified snap() | ~107 |
| 00:28 | Edited deps/src/20_gizmo.inc | 4→5 lines | ~62 |
| 00:28 | Edited deps/src/20_gizmo.inc | modified if() | ~146 |
| 00:28 | Edited deps/src/20_gizmo.inc | modified if() | ~146 |
| 00:28 | Edited deps/src/20_gizmo.inc | modified if() | ~141 |
| 00:28 | Edited deps/src/20_gizmo.inc | added 1 condition(s) | ~79 |
| 00:28 | Edited deps/src/20_gizmo.inc | added 1 condition(s) | ~117 |
| 00:28 | Edited deps/src/20_gizmo.inc | 4→7 lines | ~112 |
| 00:28 | Edited deps/src/90_c_api.inc | added 1 condition(s) | ~212 |
| 00:29 | Edited src/libgmtvtk.jl | 2→2 lines | ~15 |
| 00:29 | Edited src/eventloop.jl | modified stereo() | ~171 |
| 00:29 | Edited src/InteractiveGMT.jl | inline fix | ~18 |
| 00:30 | Session end: 19 writes across 6 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 9 reads | ~10926 tok |
| 00:38 | Edited deps/src/20_gizmo.inc | added 4 condition(s) | ~453 |
| 00:38 | Session end: 20 writes across 6 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 9 reads | ~11412 tok |
| 00:40 | Session end: 20 writes across 6 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 9 reads | ~11412 tok |
| 01:00 | Edited deps/src/20_gizmo.inc | added 3 condition(s) | ~555 |
| 01:00 | Edited deps/src/20_gizmo.inc | 5→3 lines | ~42 |
| 01:00 | Edited deps/src/20_gizmo.inc | 5→3 lines | ~40 |
| 01:00 | Edited deps/src/20_gizmo.inc | 5→3 lines | ~42 |
| 01:00 | Edited deps/src/20_gizmo.inc | 5→3 lines | ~42 |
| 01:00 | Edited deps/src/20_gizmo.inc | 5→3 lines | ~42 |
| 01:00 | Edited deps/src/10_geometry.inc | added 1 condition(s) | ~133 |
| 01:01 | Session end: 27 writes across 7 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 10 reads | ~12373 tok |
| 01:08 | Edited deps/src/20_gizmo.inc | added 3 condition(s) | ~218 |
| 01:08 | Session end: 28 writes across 7 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 10 reads | ~12607 tok |
| 01:10 | Edited deps/src/20_gizmo.inc | expanded (+10 lines) | ~201 |
| 01:11 | Session end: 29 writes across 7 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 10 reads | ~12823 tok |
| 01:20 | Edited deps/src/20_gizmo.inc | 1→4 lines | ~81 |
| 01:27 | Session end: 30 writes across 7 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 10 reads | ~12910 tok |
| 01:35 | Edited deps/src/20_gizmo.inc | modified if() | ~170 |
| 01:36 | Session end: 31 writes across 7 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 10 reads | ~13092 tok |
| 01:42 | Edited deps/src/10_geometry.inc | added 1 condition(s) | ~174 |
| 01:42 | Session end: 32 writes across 7 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 10 reads | ~22869 tok |
| 01:59 | Edited deps/src/10_geometry.inc | added 1 condition(s) | ~230 |
| 02:00 | Session end: 33 writes across 7 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 10 reads | ~23247 tok |
| 02:04 | Edited deps/src/70_window.inc | 3→2 lines | ~32 |
| 02:04 | Edited deps/src/10_geometry.inc | added 3 condition(s) | ~254 |
| 02:05 | Session end: 35 writes across 8 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 10 reads | ~23609 tok |
| 02:20 | Edited deps/src/10_geometry.inc | modified image() | ~43 |
| 02:20 | Edited deps/src/10_geometry.inc | added 4 condition(s) | ~484 |
| 02:21 | Edited deps/src/70_window.inc | added 1 condition(s) | ~92 |
| 02:21 | Edited deps/src/90_c_api.inc | added 1 condition(s) | ~329 |
| 02:22 | Edited src/grid.jl | 5→5 lines | ~103 |
| 02:22 | Edited src/types.jl | modified window() | ~118 |
| 02:22 | Edited src/types.jl | modified isalive() | ~48 |
| 02:22 | Edited src/grid.jl | modified view_image() | ~506 |
| 02:23 | Edited src/dispatch.jl | 1→3 lines | ~51 |
| 02:23 | Edited src/dispatch.jl | 2→3 lines | ~68 |
| 02:23 | Edited src/InteractiveGMT.jl | 4→4 lines | ~63 |
| 02:24 | Session end: 46 writes across 11 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 14 reads | ~41056 tok |
| 02:28 | Edited deps/src/20_gizmo.inc | modified fitSnapView() | ~51 |
| 02:28 | Edited deps/src/20_gizmo.inc | 4→5 lines | ~119 |
| 02:28 | Edited deps/src/90_c_api.inc | added 1 condition(s) | ~272 |
| 02:29 | Edited src/grid.jl | modified view_image() | ~391 |
| 02:29 | Edited src/grid.jl | modified _is_referenced() | ~171 |
| 02:30 | Session end: 51 writes across 11 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 14 reads | ~42131 tok |
| 02:36 | Edited deps/src/10_geometry.inc | added 1 condition(s) | ~341 |
| 02:36 | Edited src/grid.jl | modified _is_referenced() | ~246 |
| 02:37 | Session end: 53 writes across 11 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 14 reads | ~42760 tok |
| 02:46 | Edited deps/src/00_includes.inc | 3→7 lines | ~39 |
| 02:46 | Edited deps/src/30_app.inc | modified int() | ~118 |
| 02:46 | Edited deps/src/30_app.inc | added 5 condition(s) | ~441 |
| 02:46 | Edited deps/src/00_includes.inc | 2→3 lines | ~20 |
| 02:46 | Edited deps/src/70_window.inc | 4→5 lines | ~55 |
| 02:47 | Edited deps/src/90_c_api.inc | added 1 condition(s) | ~422 |
| 02:47 | Edited src/libgmtvtk.jl | 2→3 lines | ~28 |
| 02:47 | Edited src/types.jl | modified window() | ~119 |
| 02:47 | Edited src/types.jl | modified isalive() | ~48 |
| 02:48 | Created src/drop.jl | — | ~403 |
| 02:48 | Edited src/InteractiveGMT.jl | 7→8 lines | ~81 |
| 02:48 | Edited src/InteractiveGMT.jl | 2→3 lines | ~20 |
| 02:48 | Edited src/dispatch.jl | 3→5 lines | ~104 |
| 02:48 | Edited src/dispatch.jl | 2→6 lines | ~110 |
| 02:49 | Session end: 67 writes across 14 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 16 reads | ~47797 tok |
| 02:56 | Edited deps/src/10_geometry.inc | expanded (+8 lines) | ~159 |
| 02:57 | Edited deps/src/10_geometry.inc | 2→3 lines | ~75 |
| 02:57 | Edited deps/src/50_scene.inc | added 1 condition(s) | ~112 |
| 02:57 | Edited deps/src/30_app.inc | modified void() | ~27 |
| 02:57 | Edited deps/src/30_app.inc | modified DropFilter() | ~462 |
| 02:57 | Edited deps/src/70_window.inc | inline fix | ~27 |
| 02:57 | Edited deps/src/90_c_api.inc | 2→2 lines | ~32 |
| 02:58 | Edited deps/src/90_c_api.inc | added 5 condition(s) | ~853 |
| 02:58 | Edited src/libgmtvtk.jl | 2→2 lines | ~20 |
| 02:59 | Created src/drop.jl | — | ~1340 |
| 02:59 | Edited deps/src/90_c_api.inc | added 1 condition(s) | ~107 |
| 03:00 | Session end: 78 writes across 15 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 18 reads | ~51725 tok |
| 03:09 | Edited deps/src/90_c_api.inc | added 1 condition(s) | ~223 |
| 03:09 | Edited src/libgmtvtk.jl | 2→3 lines | ~30 |
| 03:09 | Edited src/drop.jl | modified _on_drop() | ~245 |
| 03:10 | Session end: 81 writes across 15 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 18 reads | ~53529 tok |
| 03:23 | Edited deps/src/50_scene.inc | added 1 condition(s) | ~115 |
| 03:23 | Edited deps/src/90_c_api.inc | added 1 condition(s) | ~935 |
| 03:23 | Edited src/drop.jl | 6→6 lines | ~111 |
| 03:23 | Edited src/drop.jl | 6→6 lines | ~113 |
| 03:24 | Edited deps/src/90_c_api.inc | 3→4 lines | ~55 |
| 03:24 | Session end: 86 writes across 15 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 18 reads | ~54951 tok |
| 03:33 | Created iview_app.jl | — | ~202 |
| 03:33 | Edited iview_app.jl | added 1 import(s) | ~21 |
| 03:34 | Created iview_app.vbs | — | ~107 |
| 03:34 | Created iview_app.bat | — | ~90 |
| 03:35 | Session end: 90 writes across 18 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 18 reads | ~55400 tok |
| 03:47 | Edited deps/src/90_c_api.inc | modified connect() | ~182 |
| 03:50 | Edited iview_app.jl | expanded (+9 lines) | ~135 |
| 03:50 | Created iview_app.vbs | — | ~157 |
| 03:51 | Session end: 93 writes across 18 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 18 reads | ~55908 tok |
| 03:53 | Created iview_app.vbs | — | ~215 |
| 03:54 | Edited iview_app.jl | added error handling | ~159 |
| 03:55 | Session end: 95 writes across 18 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 18 reads | ~56310 tok |
| 04:05 | Session end: 95 writes across 18 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 19 reads | ~65266 tok |
| 04:06 | Edited deps/src/70_window.inc | 7→9 lines | ~128 |
| 04:06 | Edited deps/src/90_c_api.inc | modified if() | ~194 |
| 04:07 | Session end: 97 writes across 18 files (InteractiveGMT.jl, libgmtvtk.jl, Project.toml, 20_gizmo.inc, 90_c_api.inc) | 20 reads | ~65611 tok |

## Session: 2026-06-17 13:53

| Time | Action | File(s) | Outcome | ~Tokens |
|------|--------|---------|---------|--------|
| 14:01 | Edited deps/src/70_window.inc | added 1 condition(s) | ~64 |
| 14:01 | Edited deps/src/70_window.inc | added 1 condition(s) | ~44 |
| 14:01 | Edited deps/src/70_window.inc | added 1 condition(s) | ~51 |
| 14:01 | Edited deps/src/70_window.inc | added 1 condition(s) | ~55 |
| 14:02 | image colorbar removed (gate !imageOnly + null guards) | 70_window.inc | built OK | ~3k |
| 14:02 | Session end: 4 writes across 1 files (70_window.inc) | 2 reads | ~13610 tok |
| 14:25 | Edited deps/src/00_includes.inc | 2→3 lines | ~19 |
| 14:25 | Edited deps/src/10_geometry.inc | 1→2 lines | ~52 |
| 14:25 | Edited deps/src/10_geometry.inc | 2→4 lines | ~80 |
| 14:26 | Edited deps/src/70_window.inc | added 2 condition(s) | ~150 |
| 14:26 | Edited deps/src/70_window.inc | added 1 condition(s) | ~73 |
| 14:26 | Edited deps/src/70_window.inc | added 1 condition(s) | ~405 |
| 14:26 | Edited deps/src/00_includes.inc | 2→3 lines | ~17 |
| 14:27 | Edited deps/src/90_c_api.inc | added 2 condition(s) | ~244 |
| 14:27 | Edited deps/src/70_window.inc | "iview(raw\" → "InteractiveGMT.iview(raw\" | ~21 |
| 14:28 | toolbar row below menus: Open btn + shared 2D/3D toggle; image auto-2D | 00_includes/10_geometry/70_window/90_c_api.inc | code done, awaiting DLL relink | ~8k |
| 14:28 | Session end: 13 writes across 4 files (70_window.inc, 00_includes.inc, 10_geometry.inc, 90_c_api.inc) | 7 reads | ~34295 tok |
| 14:31 | Session end: 13 writes across 4 files (70_window.inc, 00_includes.inc, 10_geometry.inc, 90_c_api.inc) | 7 reads | ~34295 tok |
| 18:22 | Edited deps/src/10_geometry.inc | modified image() | ~57 |
| 18:23 | Edited deps/src/90_c_api.inc | 2→4 lines | ~80 |
| 18:23 | Edited deps/src/90_c_api.inc | added 3 condition(s) | ~696 |
| 18:24 | iview() empty launcher rebuilt via buildAndShow(imageOnly): full chrome+toolbar, blank 2D canvas, emptyStart promotes on drop | 90_c_api/10_geometry.inc | built OK | ~4k |
| 19:18 | Session end: 16 writes across 4 files (70_window.inc, 00_includes.inc, 10_geometry.inc, 90_c_api.inc) | 8 reads | ~37364 tok |

## Session: 2026-06-17 19:20

| Time | Action | File(s) | Outcome | ~Tokens |
|------|--------|---------|---------|--------|
| 19:23 | Edited deps/src/70_window.inc | modified addAction() | ~156 |
| 19:23 | Shading dock folded by default when imageOnly/pointCloud (no shaded body) | deps/src/70_window.inc | done | ~1200 |
| 19:24 | Session end: 1 writes across 1 files (70_window.inc) | 1 reads | ~11187 tok |
| 19:27 | Edited deps/src/70_window.inc | 4→4 lines | ~135 |
| 19:27 | Edited deps/src/70_window.inc | modified if() | ~64 |
| 19:28 | Edited deps/src/70_window.inc | 2→2 lines | ~52 |
| 19:28 | Edited deps/src/70_window.inc | 2→2 lines | ~50 |
| 19:28 | Edited deps/src/70_window.inc | 2→2 lines | ~54 |
| 19:28 | Edited deps/src/70_window.inc | inline fix | ~37 |
| 19:28 | Edited deps/src/70_window.inc | inline fix | ~36 |
| 19:28 | Edited deps/src/70_window.inc | inline fix | ~36 |
| 19:28 | Edited deps/src/70_window.inc | 2→2 lines | ~53 |
| 19:28 | Edited deps/src/70_window.inc | 2→2 lines | ~62 |
| 19:29 | Edited deps/src/70_window.inc | modified connect() | ~299 |
| 19:29 | Edited deps/src/70_window.inc | modified addAction() | ~631 |
| 19:30 | comment every graphical element in Shading/Scene Objects docks; rebuild DLL ok | deps/src/70_window.inc, .wolf/cerebrum.md | done | ~3000 |
| 19:30 | Session end: 13 writes across 1 files (70_window.inc) | 2 reads | ~12888 tok |
| 19:41 | Session end: 13 writes across 1 files (70_window.inc) | 4 reads | ~21187 tok |
| 19:48 | Edited deps/src/70_window.inc | modified addAction() | ~168 |
| 19:50 | Edited deps/src/70_window.inc | modified show() | ~160 |
| 19:51 | Edited deps/src/70_window.inc | 1→2 lines | ~76 |
| 19:51 | Edited deps/src/70_window.inc | 1→2 lines | ~77 |
| 19:54 | Session end: 17 writes across 1 files (70_window.inc) | 8 reads | ~22572 tok |
| 19:56 | Session end: 17 writes across 1 files (70_window.inc) | 8 reads | ~22572 tok |
| 19:59 | Edited deps/src/70_window.inc | removed 11 lines | ~4 |
| 20:00 | Edited deps/src/70_window.inc | modified addAction() | ~450 |
| 20:01 | Session end: 19 writes across 1 files (70_window.inc) | 10 reads | ~24104 tok |
| 20:10 | Session end: 19 writes across 1 files (70_window.inc) | 10 reads | ~24104 tok |

## Session: 2026-06-17 20:15

| Time | Action | File(s) | Outcome | ~Tokens |
|------|--------|---------|---------|--------|
| 20:23 | Edited deps/src/40_shading.inc | added 5 condition(s) | ~376 |
| 20:23 | Edited deps/src/10_geometry.inc | 2→7 lines | ~109 |
| 20:23 | Edited deps/src/20_gizmo.inc | 3→6 lines | ~117 |
| 20:23 | Edited deps/src/20_gizmo.inc | 3→5 lines | ~79 |
| 20:24 | drop SSAO/post passes during gizmo drag (rotation stall fix) | 40_shading.inc,20_gizmo.inc,10_geometry.inc | DLL rebuilt OK (demo exe locked) | ~6k |
| 20:25 | Session end: 4 writes across 3 files (40_shading.inc, 10_geometry.inc, 20_gizmo.inc) | 5 reads | ~25775 tok |
| 20:25 | Session end: 4 writes across 3 files (40_shading.inc, 10_geometry.inc, 20_gizmo.inc) | 5 reads | ~25775 tok |
| 20:30 | Edited deps/src/70_window.inc | expanded (+7 lines) | ~248 |
| 20:30 | Edited deps/src/40_shading.inc | modified fixed() | ~148 |
| 20:31 | Edited deps/src/10_geometry.inc | 3→5 lines | ~130 |
| 20:31 | Edited deps/src/40_shading.inc | SetDelegatePass() → only() | ~169 |
| 20:31 | Edited deps/src/70_window.inc | added 1 condition(s) | ~128 |
| 20:32 | fix illumination-swim (world-fixed fill light + LightFollowCameraOff) + cache/pre-warm fast pass to kill press stall | 70_window.inc,40_shading.inc,10_geometry.inc | clean build all 4 steps | ~7k |
| 20:32 | Session end: 9 writes across 4 files (40_shading.inc, 10_geometry.inc, 20_gizmo.inc, 70_window.inc) | 5 reads | ~38865 tok |
| 20:35 | Edited deps/src/40_shading.inc | expanded (+6 lines) | ~199 |
| 20:36 | Edited deps/src/70_window.inc | modified Phong() | ~124 |
| 20:36 | RELIEF surface PBR+IBL -> pure-diffuse Phong (view-dependent specular/IBL was the real swim cause) | 40_shading.inc,70_window.inc | clean build, DLL fresh | ~5k |
| 20:37 | Session end: 11 writes across 4 files (40_shading.inc, 10_geometry.inc, 20_gizmo.inc, 70_window.inc) | 5 reads | ~41511 tok |
| 20:41 | Session end: 11 writes across 4 files (40_shading.inc, 10_geometry.inc, 20_gizmo.inc, 70_window.inc) | 5 reads | ~41511 tok |
| 20:43 | Edited deps/src/40_shading.inc | reduced (-6 lines) | ~72 |
| 20:43 | Edited deps/src/40_shading.inc | 8→3 lines | ~27 |
| 20:43 | Edited deps/src/40_shading.inc | removed 25 lines | ~29 |
| 20:44 | Edited deps/src/70_window.inc | reduced (-7 lines) | ~110 |
| 20:44 | Edited deps/src/70_window.inc | 7→4 lines | ~73 |
| 20:44 | Edited deps/src/70_window.inc | 7→2 lines | ~32 |
| 20:44 | Edited deps/src/10_geometry.inc | 6→1 lines | ~26 |
| 20:44 | Edited deps/src/10_geometry.inc | 3→1 lines | ~26 |
| 20:44 | Edited deps/src/20_gizmo.inc | 4→1 lines | ~16 |
| 20:44 | Edited deps/src/20_gizmo.inc | 5→3 lines | ~42 |
| 20:46 | REVERT all illumination changes back to original PBR (user order) | 40_shading.inc,70_window.inc,10_geometry.inc,20_gizmo.inc | clean build, original restored | ~4k |
| 20:46 | Session end: 21 writes across 4 files (40_shading.inc, 10_geometry.inc, 20_gizmo.inc, 70_window.inc) | 5 reads | ~41993 tok |
| 20:52 | Session end: 21 writes across 4 files (40_shading.inc, 10_geometry.inc, 20_gizmo.inc, 70_window.inc) | 5 reads | ~41993 tok |
| 21:08 | Edited deps/src/00_includes.inc | 3→4 lines | ~31 |
| 21:08 | Edited deps/src/10_geometry.inc | 1→3 lines | ~65 |
| 21:08 | Edited deps/src/70_window.inc | added 3 condition(s) | ~313 |
| 21:09 | fix per-hover CPU stall on big grids: add vtkStaticCellLocator to s->picker (was O(cells) brute pick) | 00_includes.inc,10_geometry.inc,70_window.inc | clean build | ~5k |
| 21:09 | Session end: 24 writes across 5 files (40_shading.inc, 10_geometry.inc, 20_gizmo.inc, 70_window.inc, 00_includes.inc) | 6 reads | ~43243 tok |
| 21:18 | Edited deps/src/70_window.inc | added 1 condition(s) | ~234 |
| 21:21 | Edited deps/src/10_geometry.inc | added 1 condition(s) | ~239 |
| 21:21 | Edited deps/src/10_geometry.inc | modified if() | ~173 |
| 21:21 | Edited deps/src/10_geometry.inc | reduced (-7 lines) | ~103 |
| 21:22 | Edited deps/src/70_window.inc | reduced (-22 lines) | ~95 |
| 21:22 | Edited deps/src/10_geometry.inc | removed 3 lines | ~13 |
| 21:22 | Edited deps/src/00_includes.inc | 4→3 lines | ~22 |
| 21:22 | Edited deps/src/10_geometry.inc | vertex() → armed() | ~45 |
| 21:23 | fix 200MB grid silent OOM crash: remove cell locator, switch hover readout to GPU z-buffer pick (O(1)) | 00_includes.inc,10_geometry.inc,70_window.inc | clean build | ~7k |
| 21:23 | Session end: 32 writes across 5 files (40_shading.inc, 10_geometry.inc, 20_gizmo.inc, 70_window.inc, 00_includes.inc) | 7 reads | ~47497 tok |
| 21:36 | Session end: 32 writes across 5 files (40_shading.inc, 10_geometry.inc, 20_gizmo.inc, 70_window.inc, 00_includes.inc) | 8 reads | ~54870 tok |

## Session: 2026-06-17 22:27

| Time | Action | File(s) | Outcome | ~Tokens |
|------|--------|---------|---------|--------|

## Session: 2026-06-17 22:29

| Time | Action | File(s) | Outcome | ~Tokens |
|------|--------|---------|---------|--------|
| 22:32 | Edited src/grid.jl | inline fix | ~37 |
| 22:32 | Edited src/grid.jl | 3→3 lines | ~66 |
| 22:33 | Edited src/grid.jl | inline fix | ~23 |
| 22:33 | Edited src/grid.jl | 3→3 lines | ~63 |
| 22:33 | Edited deps/src/90_c_api.inc | inline fix | ~34 |
| 22:33 | Edited deps/src/10_geometry.inc | inline fix | ~23 |
| 22:33 | Edited deps/src/90_c_api.inc | 2→2 lines | ~31 |
| 22:34 | Edited deps/src/90_c_api.inc | inline fix | ~22 |
| 22:34 | Edited src/drop.jl | 4→4 lines | ~66 |
| 22:34 | Edited src/drop.jl | 5→5 lines | ~80 |
| 22:34 | Edited deps/src/10_geometry.inc | added 1 condition(s) | ~73 |
| 22:35 | Edited deps/src/10_geometry.inc | modified for() | ~37 |
| 22:35 | analyze 200MB grid -> 13GB RAM blowup | grid.jl,drop.jl,90_c_api.inc,10_geometry.inc | found Float64 round-trip + 64-bit cell ids | ~8k |
| 22:50 | fix: z double*->float* (zero-copy Float32) across all 5 callers + cells->Use32BitStorage() | grid.jl,drop.jl,90_c_api.inc,10_geometry.inc | clean DLL build; ~13GB->~9GB | ~3k |
| 22:36 | Session end: 12 writes across 4 files (grid.jl, 90_c_api.inc, 10_geometry.inc, drop.jl) | 5 reads | ~35418 tok |
| 22:53 | Session end: 12 writes across 4 files (grid.jl, 90_c_api.inc, 10_geometry.inc, drop.jl) | 5 reads | ~35418 tok |
