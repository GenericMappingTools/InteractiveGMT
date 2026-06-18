# OpenWolf

@.wolf/OPENWOLF.md

This project uses OpenWolf for context management. Read and follow .wolf/OPENWOLF.md every
session. Check .wolf/cerebrum.md before generating code. Check .wolf/anatomy.md before reading
files.

# InteractiveGMT — dev context

Julia package. A self-contained **Qt6 + VTK 9.6** 3-D viewer for GMT.jl data (grids, point
clouds, `GMTfv` solids / polygon meshes). NO dependency on F3D — its own Qt window + VTK
pipeline, gizmo, cube axes, colour bar, shading, vertical curtains, in-window Julia console.
Version `0.1.0`, unregistered. Migrated 2026-06-16 from the GMTF3D `qtvtk_proto` prototype.

## Architecture (do not relearn)

- **Two halves.** `deps/` is the C/C++ viewer (a shared lib `gmtvtk.dll` + a tiny C API);
  `src/` is the Julia bridge that `dlopen`s it. They talk only through the `gmtvtk_*` C API.
- **The C++ is ONE translation unit.** `deps/src/gmtvtk.cpp` `#include`s the ordered fragments
  `00_includes.cpp … 90_c_api.cpp`. They carry the `.cpp` extension only so IDEs give them C++
  language support — they are #included textually and are NOT in the CMake source list, so NOT
  separately compiled. Never add a fragment to `GMTVTK_SRC` (its file-static helpers would
  multiply-define). Every `static` helper and the `Scene`/`Gizmo` structs keep internal linkage
  exactly as in the old single `main.cpp`. The host **C API lives in `90_c_api.cpp`** (the
  `extern "C" gmtvtk_*` exports + the demo `main()`). Edit a fragment → rebuild the one TU with
  `deps/build.bat`.
- **Build is Windows-only.** `deps/build.bat` (Ninja + vcvars64) → `deps/build/gmtvtk.dll`
  (host library) + `deps/build/gmtvtk_demo.exe` (standalone peaks demo). Toolchain paths
  (VTK/Qt/TBB) are hard-coded in `deps/CMakeLists.txt` + `deps/build.bat`.
- **DLL load is RUNTIME, in `__init__`** (`src/libgmtvtk.jl`). The dlopen handle, the dlsym
  pointers (`_LIB_FNS`, fetched per-ccall via `_fn(:gmtvtk_...)`) and the console `@cfunction`
  are runtime values that CANNOT be baked into a precompiled image — so they live in `__init__`,
  never at module top level. `__init__` is tolerant of a missing/unbuilt DLL so `using` still
  succeeds; viewer calls then error on first use. Override toolchain DLL dirs with the
  `INTERACTIVEGMT_VTK_BIN` / `_QT_BIN` / `_QT_PLAT` env vars.
- **In-process, non-blocking.** `view_*` returns a live handle immediately; a Julia `Timer`
  (`src/eventloop.jl`) pumps the Qt loop ~50 Hz so the REPL stays usable. The window lives on
  this process's UI thread, so killing the REPL tears it down — shared-process lifetime, not a
  bug. After rebuilding the DLL, **start a fresh Julia session** (dlopen lock + stale code).

## src/ layout (one concern per file)

`libgmtvtk` (loader) · `types` (QtFigure/QtPoints/QtFV + registry) · `eventloop` (pump,
save_png, wait_windows) · `console` (in-window Julia eval callback) · `colors` · `cpt` ·
`drape` · `grid` (view_grid + add!) · `table` (show_table) · `curtain` (add_curtain!) ·
`points` (view_points + selection) · `fv` (view_fv + SOLIDS + poly2fv) · `dispatch` (f3dview).

## Knowledge carried from GMTF3D

`QTVTK_PLAN.md` (design notes) + `.wolf/knowledge/*.md` (the qtvtk-relevant memory: bridge,
scope, vcurtain, drape). The bulk of UI/axis/gizmo learnings live in `.wolf/cerebrum.md`.

---
*Handoff note (not committed). Delete or keep as you like.*
