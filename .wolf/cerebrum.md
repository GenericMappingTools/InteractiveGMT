# Cerebrum

> OpenWolf's learning memory. Seeded 2026-06-16 at migration from GMTF3D/qtvtk_proto — the
> qtvtk-relevant learnings were carried over; F3D/libf3d-only entries were dropped (they live
> in the GMTF3D project). Do not edit manually unless correcting an error.

## User Preferences

- **C/C++ code style (qtvtk_proto/InteractiveGMT viewer):** (1) **TABS for indentation** in NEW
  code I write — do NOT retab/rewrite whole existing files to convert them (wastes tokens); tabs
  apply only to code I author. (2) **K&R braces BUT `else` on its OWN line** — `}` newline
  `else {` (NOT `} else {`); same for `else if`. (3) **Line length up to 120 chars.**
- **Wants the SIMPLEST solution; do NOT present elaborate multi-option design menus.** Pick the
  obvious approach and just do it. Don't ask for permission to read/write inside this package dir.
- **DO NOT unilaterally decide what to ADD or REMOVE.** (2026-06-17, emphatic.) Fix ONLY the exact
  thing asked. Do NOT swap materials (PBR↔Phong), lights, passes, or add features/structures on my
  own judgement. If the fix needs a design choice, ASK first. Changing the look/behaviour beyond the
  literal request = unacceptable. Reverted a whole illumination rewrite for exactly this.
- **DON'T spam `.wolf/buglog.json`.** Stop auto-logging every edit/refactor. Only log a real,
  non-obvious bug+fix if explicitly useful — otherwise skip. Keep cerebrum/memory updates.
- **HARD RULE — Z-AXIS TICK LABELS ARE ALWAYS PERPENDICULAR TO THE Z-AXIS** (screen-facing
  horizontal billboards) and are PART OF the Axes Cube — hide "Axes Cube" → Z labels hide too;
  they must NEVER change brightness / fade with the view angle.
- **Overlay "select" = SHOW THE CONTEXT MENU, nothing else.** No highlight, no recolor, no
  persistent selection state — clicking a line/point just pops its per-element menu. Never leave
  stray debug `fprintf`/`println` in shipped code.
- **Axis NAME titles = X/Y ONLY (NO Z name).** User never asked for a Z name title.
- **Colorbar ONLY for grids + point clouds.** When importing/viewing IMAGES, do NOT add a colorbar.
- **Toolbar row below menus (ParaView-style)** built in `70_window.inc` via `win->addToolBar("Main")`,
  text-beside-icon, non-movable. Open btn (SP_DirOpenIcon) → QFileDialog → `g_juliaEval(s,"InteractiveGMT.iview(raw\"path\")")`
  opens a NEW window. `s->act2D` = ONE shared checkable QAction used by BOTH View menu + toolbar.
  Bare images auto-enter flat-2D (`s->flat2d=true` in 90_c_api imageOnly block, sav_* primed for
  perspective 3D restore). `actToggle2D`→`setFlat2D(s,bool)` is state-driven/idempotent + syncs act2D checkmark.
  `hideAnnot = flat2d && !imageOnly` so a referenced image keeps its lon/lat numbers in 2D.
- **(2026-06-17) COMMENT EVERY GRAPHICAL ELEMENT in the C/C++ where it is SET/CONTROLLED.**
  Each QDockWidget / QWidget / QSlider / QCheckBox / QAction / addDockWidget / setAllowedAreas /
  setVisible etc. gets an inline comment saying WHAT UI element it is and WHAT it controls. User
  has asked repeatedly — do this without being told, on every UI edit. Non-negotiable.
- **`iview()` empty launcher = FULL-chrome window, not a bare stub.** `gmtvtk_open_empty` now builds
  via `buildAndShow(imageOnly=true)` on a hidden 2x2 placeholder plane → menus+toolbar+2D map, blank
  canvas (surf+axes hidden, no colorbar). `s->emptyStart=true` makes `gmtvtk_has_surface` return 0 so
  a dropped file PROMOTES to a fresh full window + retires the launcher (drop.jl `_on_drop`).

## Key Learnings

- **(2026-06-17) GRID RAM: keep z Float32 + 32-bit cell ids.** GMTgrid.z is Float32; the
  C side stores z as float (vtkPoints float + vtkFloatArray). NEVER `Float64.(G.z)` — it's a
  pointless lossy round-trip that doubles RAM. All grid ccalls use `Ptr{Cfloat}`; pass `G.z`
  directly (zero-copy when already Float32). `vtkCellArray` defaults to 64-bit ids on win64 —
  call `cells->Use32BitStorage()` when `nx*ny < 2e9` to halve cell-array RAM. The real fix
  (pending) is a `vtkStructuredGrid`/`vtkImageData`+`WarpScalar` for regular grids = implicit
  topology, ZERO cell array. A 200MB grid was eating ~13GB (see buglog bug-002).

- **(2026-06-16) MIGRATION: qtvtk_proto → InteractiveGMT package.** C++ split include-based
  (single TU): `deps/src/gmtvtk.cpp` #includes `00_includes..90_c_api.inc` (sliced byte-exact
  from the old main.cpp; reconstruction `diff`-verified). The host C API is isolated in
  `90_c_api.inc`. Only gmtvtk.cpp is compiled, so all `static`/`Scene`/`Gizmo` keep internal
  linkage — NO header extraction, NO linkage refactor. Build = `deps/build.bat` → gmtvtk.dll +
  gmtvtk_demo.exe. To re-split differently, re-slice; to add code, edit the right fragment.
- **(2026-06-16) PACKAGE-IFICATION GOTCHA: dlopen/dlsym/@cfunction are RUNTIME-only.** A package
  precompiles, and a baked `Ptr` is invalid at runtime. So `src/libgmtvtk.jl` keeps `_DLL::Ref`
  + `_LIB_FNS::Dict{Symbol,Ptr}` populated in `_load_library()` called from the module
  `__init__`; every ccall fetches its pointer with `_fn(:gmtvtk_...)` (ccall accepts a runtime
  `Ptr{Cvoid}` first arg). The console `@cfunction` is likewise built in `__init__`
  (`_register_console_eval`). `__init__` swallows a missing/unbuilt-DLL error so `using` still
  succeeds (non-Windows / un-built); viewer calls then error on first use. The OLD bridge.jl was
  a SCRIPT (top-level dlopen) — that can't be a package.
- **In-window JULIA CONSOLE (console.jl + the C++ console dock).** Viewer runs IN-PROCESS, so the
  console hands a typed line back to Julia and `eval`s in `Main`. C++ side: `JuliaEvalFn` typedef
  + `g_juliaEval` + `gmtvtk_set_julia_eval`. Julia side: `_FIGREG::Dict{Ptr,Any}` (Scene*→figure
  via `_register_fig!`) + `_console_eval` `@cfunction`; binds `fig` to the window
  (`Core.eval(Main,:(fig=$fig))`) so `add!(fig,D)` works with no handle. GOTCHA: `redirect_stdout`
  REJECTS an IOBuffer — use a pipe + `@async` reader. Reentrant (Timer→process_events→cfunction→
  eval), one thread, fine.
- **GRAY-TOP-RING bug = GMT.jl `.colormap` APPENDS the CPT FOREGROUND colour as an extra row.**
  `makecpt(range=(z0,z1,(z1-z0)/255),continuous=true)` returns 256 colormap rows but 255 range
  slices — the 256th row is the FG (gray), not a ramp colour. Fix in `_cpt_nodes_range` (cpt.jl):
  `nseg=size(rg,1); size(cm,1)>nseg && (cm=cm[1:nseg,:])` to drop the FG row, then `cz=[rg[:,1];
  rg[end,2]]`, `cmn=[cm; cm[end:end,:]]`. When a colour is literally gray on a non-gray ramp,
  inspect `C.colormap[end,:]` BEFORE theorizing about VTK/lighting.
- **DRAPE MUST BE PHONG/LightingOff, NEVER PBR — and `applyShading()` MUST NOT touch it.** VTK's
  PBR shader samples a texture ONLY via `SetBaseColorTexture`; a plain `SetTexture()` is IGNORED
  under PBR → flat grey. Drape is a FINISHED PICTURE: `LightingOff()` renders it flat at full
  albedo. `applyShading` applies PBR/metallic/roughness to `s->surf` ONLY (it runs at startup AND
  on every shading slider, so fixing only buildAndShow does nothing — fix applyShading too).
  RULE: shading/material/lighting edits apply to the relief surface ONLY; leave the drape alone.
- **Axis labels (X/Y/Z) are ALL identical freetype billboards; the cube's native labels are OFF.**
  Mixing vtkCubeAxesActor follower labels (stroke font, ScreenSize) with billboard freetype
  (FontSize px) can NEVER match size/font. All three axes drawn via `placeTickBillboards`
  (Arial, same FontSize); cube label visibility off, lines+major ticks on. Labels sit on the
  CAMERA-NEAREST edge, justified OUTWARD (never spill into the cube), offset PERPENDICULAR to the
  axis past a SINGLE outward tick (cube native ticks are doubled across two faces → off, we draw
  our own). Constant-brightness scene text lives in the OVERLAY renderer `s->axesRen` (layer 1,
  own headlight + depth), never `s->ren` (the directional sun dims billboard quads at some tilts).
- **FLAT-2D toggle (one button).** `Scene::flat2d` + saved 3D state. →2D: `ve=0; applyVE` (relief
  → plane, z by COLOUR only), surf LightingOff, hide gizmo, camera ParallelProjectionOn + top-down.
  Rotation lock is the crux: rotate/tilt come from the gizmo `DragCB` (NOT the trackball style), so
  gate THERE on `flat2d`. This 2D/3D experiment is a core reason the package exists.
- **GMTimage layout: `TCBa`/`TRBa` etc.** char 2 = 'R' → array is [lon,lat] (row-major);
  char 1 != 'B' → first lat index is NORTH. VTK texture origin is bottom-left → output row 0 =
  SOUTH, west→east. `_drape_buf`/`_drape_to_bbox` (drape.jl) handle this.
- **GMT.jl grid layout: `G.z[i,j]` ↔ `(x[j], y[i])`, y ASCENDING.** dim1=ny, dim2=nx. Viewer reads
  column-major `z[ix*ny+iy]`.
- **Grid surface TRIANGULATED by default, mesh wires HIDDEN by default, `e` hotkey toggles.**
  `triangulate=true` → 2 tris/cell. The C `edges` arg is the INITIAL mesh visibility (default 0).
- **Rotation = camera orbit (gizmo DragCB), body stays world-fixed.** Left-drag is owned by the
  gizmo (`20_gizmo.inc DragCB`), NOT the trackball style → trackball Start/EndInteractionEvent never
  fire on rotate; hook drag-time work in DragCB's LeftButtonPress/Release instead.
- **Relief surface = glossy PBR + IBL — this is DESIGN, do NOT change it.** `SetInterpolationToPBR`
  (roughness 0.45, metallic 0, IOR 1.5) at creation (70_window.inc) + re-asserted in applyShading's
  non-matte branch (40_shading.inc). PBR specular/IBL are view-dependent (sheen slides as camera
  orbits) and that is INTENDED. Lights: keyLight = world-fixed SceneLight, fillLight = Headlight.
  (2026-06-17: a "fix the swim" rewrite to diffuse-Phong + world-fixed fill + interactive pass-swap
  was REVERTED on the user's order — leave illumination as-is unless explicitly told.)
- **Hover coord readout = GPU z-buffer pick, NOT vtkCellPicker.** `onMouseMove` (10_geometry.inc)
  runs on every hover move. vtkCellPicker is O(cells) brute force without a spatial index → dozens
  of sec/move on a ~200 MB grid (CPU-bound, GPU idle). A `vtkStaticCellLocator` fixes the speed but
  its `BuildLocator` OOMs on tens-of-M cells → Julia is KILLED silently (no exception — classic OOM).
  CORRECT FIX: drop the software pick entirely, use `renderWindow->GetZbufferDataAtPoint(x,y)` then
  `ren->SetDisplayPoint(x,y,z); ren->DisplayToWorld()` → O(1) for ANY grid size, no locator, no
  memory. z (true elev) = world z / (zfac*ve). Codebase already used this z-buffer idiom for
  middle-drag pan (30_app.inc) and profiles (60_profile.inc). Drag/rotate doesn't pick (gizmo aborts
  the event), so the stall was purely HOVER. <<lesson: a fix that allocates O(grid) memory can turn a
  stall into a silent OOM crash — prefer O(1) GPU reads on huge data.>>
- **When a DLL change "does nothing": the process didn't reload it.** dlopen caches gmtvtk.dll for
  the process lifetime → exit Julia, start a FRESH session (or relaunch gmtvtk_demo.exe). Tell the
  user to reload after a rebuild; "nothing changed" can = stale DLL.

## Decision Log

- **(2026-06-16) C++ split = include-based single TU, NOT separate compilation.** User chose the
  safe path: I can't compile-test here (needs VS2022+Qt6+VTK), and a botched header extraction =
  broken build found later. Include-based keeps byte-identical compile semantics.
- **(2026-06-16) Left GMTF3D/qtvtk_proto in place** (copy, don't delete) until the new package is
  verified to build. Build files → `deps/`, C/C++ → `deps/src/`, demos → `examples/`.
- **(2026-06-16) DLL load + console registration in `__init__`, tolerant of failure** so `using`
  never hard-fails on a box without the built DLL.

## Do-Not-Repeat

- **(2026-06-16) Don't put dlopen/dlsym/@cfunction at module top level** — precompile bakes invalid
  pointers. Always `__init__`.
- **(2026-06-16) The C++ build toolchain IS on this machine — BUILD IT, don't claim you can't.**
  (User corrected me: I wrongly said I couldn't compile here.) Run `deps/build.bat` via the
  PowerShell tool: `& cmd.exe /c "...\deps\build.bat" *> build.log` (Git Bash mangles the cmd
  args — use PowerShell). vcvars64 + Ninja + cmake. VERIFIED 2026-06-16: gmtvtk.dll (223 KB) +
  gmtvtk_demo.exe build clean from the include-based single TU; dlopen + 16/16 C-API symbols
  resolve. The proto was built this way all the time. After rebuild → fresh Julia session.

- [2026-06-17] User pref: ALWAYS auto-kill the process holding gmtvtk.dll before rebuilding (no confirm). Find holder via Get-Process Modules match '*gmtvtk.dll*', Stop-Process -Force, then run deps/build.bat. Rebuild needs the DLL unlocked; a live Julia session locks it.

- [2026-06-17] TERMINOLOGY (do not confuse): axis ANNOTATIONS = the coordinate NUMBERS (tick values; code: s->xlabels/ylabels billboards). axis LABELS = the axis NAME/title (e.g. 'lon'/'lat'; code: s->axTitle). Use these terms precisely.
- [2026-06-17] Images are 2D: a referenced image shows coordinate ANNOTATIONS only, NEVER an axis-NAME LABEL. A plain (un-georeferenced) image shows no axes at all. Referenced = isgeog OR proj4/wkt/epsg present OR range differs from raw pixel dimensions (_is_referenced in grid.jl).
