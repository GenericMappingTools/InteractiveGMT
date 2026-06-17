# anatomy.md

> Auto-maintained by OpenWolf. Last scanned: 2026-06-17T22:54:29.825Z
> Files: 44 tracked | Anatomy hits: 0 | Misses: 0

## ./

- `iview_app.bat` (~90 tok)
- `iview_app.jl` — Desktop-launcher script: open an empty InteractiveGMT viewer (drag-and-drop launcher) and keep (~422 tok)
- `iview_app.vbs` (~215 tok)
- `Project.toml` (~86 tok)

## ./ (package root)

- `CLAUDE.md` — dev context + OpenWolf pointer. (~700 tok)
- `LICENSE` (~250 tok)
- `Project.toml` — name/uuid/deps (GMT, Libdl). (~80 tok)
- `QTVTK_PLAN.md` — Qt+VTK viewer design/scope notes (carried from GMTF3D). (~1600 tok)
- `README.md` — overview, build, API table, quick start. (~900 tok)

## .wolf/knowledge/ (carried memory)


## deps/ (C/C++ viewer)

- `build.bat` — Ninja + vcvars64 configure/build. (~130 tok)
- `CMakeLists.txt` — builds gmtvtk.dll + gmtvtk_demo.exe from the single TU src/gmtvtk.cpp. (~350 tok)
- `run.bat` — launch gmtvtk_demo.exe with VTK/Qt on PATH. (~80 tok)

## deps/assets/


## deps/src/

- `00_includes.inc` — gmtvtk — self-contained Qt6 + VTK 9.6 3-D viewer for GMT data (native QMenu UI + (~1076 tok)
- `10_geometry.inc` — Declares ProfilePanel (~11407 tok)
- `20_gizmo.inc` — ============================================================================ (~8889 tok)
- `30_app.inc` — (Julia) pumps the loop via gmtvtk_process_events so the REPL stays interactive. (~3019 tok)
- `40_shading.inc` — int: overlay (~1566 tok)
- `50_scene.inc` — Declares QString (~5596 tok)
- `70_window.inc` — Declares double (~12581 tok)
- `90_c_api.inc` — ============================================================================ (~7703 tok)

## deps/src/ (the one TU + its ordered .inc fragments)

- `gmtvtk.cpp` — umbrella; `#include`s the .inc fragments in order. ONLY file compiled. (~250 tok)

## examples/


## src/

- `dispatch.jl` — iview front-door dispatcher — single entry over the qtvtk viewers (mirrors GMTF3D iview, (~998 tok)
- `drop.jl` — File drag-and-drop. The C side calls `_on_drop` with the receiving window's Scene* handle and (~1514 tok)
- `eventloop.jl` — Qt event-loop pump (keeps the REPL alive while a window is open) + window utilities. (~515 tok)
- `grid.jl` — view_grid: show a GMTgrid surface in the Qt + VTK viewer, plus the line/point overlay path (~2834 tok)
- `InteractiveGMT.jl` — are: __init__ (~646 tok)
- `libgmtvtk.jl` — libgmtvtk.jl — load the self-contained Qt6 + VTK viewer DLL and resolve its C API. (~758 tok)
- `types.jl` — Figure handles + the live-figure registry. Each viewer call returns one of these opaque (~564 tok)

## src/ (Julia bridge — one concern per file)

- `colors.jl` — `_NAMED_COLORS`, `_ovl_color`, `_parse_gmt_color`. (~450 tok)
- `console.jl` — in-window Julia console eval callback + `@cfunction` registration. (~600 tok)
- `cpt.jl` — `_isgeographic`, `_cpt_nodes`/`_cpt_nodes_range`, `_z_to_hex`, `_resolve_zscale`. (~750 tok)
- `curtain.jl` — vertical curtains: `add_curtain!`, `_add_curtains!`, densify/texfile helpers. (~1300 tok)
- `dispatch.jl` — `f3dview` front-door, `view_demo`. (~700 tok)
- `drape.jl` — `_drape_buf`, `_drape_to_bbox`, `_sample_grid`. (~1400 tok)
- `eventloop.jl` — Qt pump `Timer`, `save_png`, `wait_windows`. (~350 tok)
- `fv.jl` — `view_fv` (GMTfv + named SOLIDS), `_view_fv`, `colorize_by_z!`, `poly2fv`. (~3200 tok)
- `grid.jl` — `view_grid`, `_pack_dataset`, `_add_overlay!`, `add!`. (~1700 tok)
- `InteractiveGMT.jl` — module: usings, includes, exports, `__init__` (runtime DLL load). (~450 tok)
- `libgmtvtk.jl` — dlopen + dlsym at runtime; `_fn(:sym)` accessor; toolchain PATH/ENV. (~500 tok)
- `points.jl` — `view_points`, `selection` (rubber-band). (~750 tok)
- `table.jl` — `_table_matrix`, `show_table` (Data Viewer tab). (~500 tok)
- `types.jl` — `QtFigure`/`QtPoints`/`QtFV`, `_FIGREG` registry, `isalive`. (~350 tok)

## test/

- `runtests.jl` — TestItemRunner entry. (~30 tok)
- `test-basic-test.jl` — CI-safe unit tests of the pure-Julia helpers (no DLL/window). (~700 tok)
