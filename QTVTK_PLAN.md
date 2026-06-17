# GMTF3D — Qt + VTK viewer plan

A distributable interactive 3-D viewer for GMT data (grids, points, images, GMTfv solids)
with native UI and the in-scene tools (profile, axes, picking), built on **Qt + VTK directly**
— not on f3d injection.

## Why move off the f3d-injection approach

The current viewer drives **f3d** (libf3d) and reaches its internals through a private C
sidecar (`f3d_ext` / `f3dx`) to do everything f3d's public API cannot: cube axes, scale gizmo,
coord readout, line overlays, point recolour, rubber-band picking, and the new grid **profile
tool**.

This can never become a distributable plugin, and the menu work made that concrete:

- **A custom ImGui menu can only be drawn inside `f3d.dll`** (ImGui is statically compiled there
  with hidden symbols + its own global context; a separate DLL can't call it). So an ImGui menu
  means editing f3d's own source — off limits.
- **`f3d_ext` cannot be compiled separately and linked against the official `f3d.lib`.** Proven
  by dumping the shipped `f3d.dll` export table (2436 symbols): it exports **none** of
  `window_impl::GetRenderWindow`, `vtkF3DRenderer`, `vtkF3DMetaImporter`, `vtkF3DUIActor`. Those
  internal classes carry no export macro, so they're absent from the export table → linking
  against stock `f3d.lib` fails with `LNK2019` on every private symbol. The private headers
  aren't installed either, and f3d_ext also needs the exact matching VTK dev SDK. That is why
  f3d_ext is built **inside** the f3d source tree.

**Conclusion:** needing a custom f3d build is not meaningfully different from modifying f3d core.
A genuinely distributable tool with native UI requires either (a) **owning VTK directly** (this
plan), or (b) **upstreaming a real extension API into f3d** (slow, maintainer-dependent).

## Architecture

- **Window / UI = Qt.** `QMainWindow` + a VTK render widget. Native `QMenu` menubar and
  **context menus** (perfect anti-aliasing, DPI, submenus, shortcuts — all programmable),
  dialogs, docks, status bar.
- **3-D = VTK directly.** `vtkGenericOpenGLRenderWindow` in a Qt GL widget, our own
  `vtkRenderer`, our own actors. Full renderer / actor / picking access — the very thing f3d
  hides.
- **Data.** GMT grid → `vtkPolyData` built directly (the structured mesh is already produced by
  `grid2fv_direct`; emit `vtkPoints` + cells + scalars). Colour via `vtkLookupTable` /
  `vtkColorTransferFunction`.
- **f3d.** Optional file importer via its public API for 3-D model files; for GMT data, bypass
  f3d. Most likely **dropped** from the interactive viewer entirely.

## What ports as-is (already plain VTK)

The f3dx tools already operate on a `vtkRenderer`, so they move over with little change:

- Profiler: `vtkCellLocator` vertical-ray surface sampling, the ordered `(s, z)` profile, the
  drape-line `vtkActor`, the 2-D `vtkActor2D` panel.
- L1 cutter / implicit / result actor.
- Mesh building (grid → polydata).

## What rebuilds on our renderer (public VTK, mostly straightforward)

These f3d_ext features currently lean on f3d's renderer/importer and get redone directly:

| Feature | VTK replacement |
|---|---|
| Cube axes | `vtkCubeAxesActor` |
| Colour bar | `vtkScalarBarActor` |
| Coord readout | `vtkPointPicker` + Qt status bar |
| Vertical-scale / scale gizmo | our own `vtkInteractorStyle` (direct actor transform) |
| Line overlays | `vtkPolyData` line actor (already have) |
| Point clouds + per-point colour | `vtkActor` + scalars |
| Rubber-band selection | `vtkHardwareSelector` |
| Round point sprites | `vtkPointGaussianMapper` |
| Drape image | `vtkTexture` on the polydata |
| Vertical exaggeration | actor transform |

## What's lost / must be reimplemented (f3d freebies)

PBR materials, IBL/HDRI lighting, raytracing — VTK has them (`vtkOpenGLRenderer` PBR,
`vtkOSPRayPass`) but we wire them ourselves. Polished default lighting/camera: ours to set up.
f3d's animation and broad file-format support: dropped.

## The menu (the original problem) → native `QMenu`

Right-click → `QMenu::exec(globalPos)`. Submenus, icons, shortcuts, native look — all
programmable, no styling fight. The problem dissolves.

## Julia boundary — the key design choice

- **Q2 — in-process Qt + VTK library with a C API, `ccall`ed from Julia (recommended).** This
  mirrors today's model (f3d ran in-process; callbacks via `@cfunction`); we swap f3d for our own
  lib. Live Julia callbacks stay easy and in-process; menu items + callbacks are supplied from
  Julia exactly like the profiler design. Challenge: Qt's `QApplication::exec()` blocks like the
  current interactor loop — drive it blocking + GC-safe (already done for `f3d_interactor_start`)
  or pump `processEvents`. Keeps the Julian, in-process control we want.
- **Q1 — separate Qt + VTK executable, launched by GMTF3D.** Julia → exe via a data file + a JSON
  menu/config; exe → Julia callbacks via IPC (socket/pipe). Cleaner isolation, but callbacks need
  IPC and marshaling.

## Prerequisites (verified 2026-06-11)

- **No Qt on the machine.** Install Qt6.
- **The VTK in the f3d-superbuild was built without Qt** (no `GuiSupportQt` / `QVTK` modules;
  `VTK_MODULE_ENABLE_VTK_GUISupportQt` defaulted off because Qt was absent). We need a VTK built
  **with** `GuiSupportQt` for `QVTKOpenGLNativeWidget`. Since we are leaving f3d, the clean move is
  a **standalone VTK + Qt build** for the new viewer, independent of the f3d-superbuild (a
  distributable viewer ships its own VTK + Qt anyway). Alternative that avoids `GuiSupportQt`: a
  `QWindow` / `QOpenGLWindow` wired by hand to a `vtkGenericOpenGLRenderWindow` (more manual).

## In-window Julia console (built)

The in-process Q2 boundary buys a feature an IPC viewer could not have cheaply: a **Julia
console dock** that evals in the host session. Enter → C++ `returnPressed` → registered
`@cfunction` (`gmtvtk_set_julia_eval`) → `Core.eval(Main, …)` → result string back to the
dock. Same `Main` as the REPL (shared variables, both ways); `fig` is auto-bound to the
window via a `Scene*` → figure registry. Reentrant on the one Julia thread (Timer → ccall →
Qt → cfunction → eval). Details + the `redirect_stdout`-needs-a-pipe gotcha live in
`qtvtk_proto/README.md` (“Julia Console panel”).

## Phases

0. **Prototype.** Minimal Qt + VTK lib (C API): `QMainWindow` + VTK widget + a `vtkRenderer`
   showing a grid polydata + a native right-click `QMenu`; Julia `ccall`s it, passes a grid, runs
   an `@cfunction` on menu click. Proves Q2 end-to-end (including the Qt-loop-in-Julia + callback).
1. Port the profiler (ray-sample, drape line, 2-D panel) onto the owned renderer.
2. Port the f3d_ext features (cube axes, colorbar, scale gizmo, lines, points, selection).
3. Lighting / PBR / raytracing parity (`vtkOSPRayPass`), camera + defaults polish.
4. Packaging (Qt + VTK DLLs + the lib + bundle).

**Cost: multi-week — effectively a new viewer app.** It is, however, the only distributable path
that delivers native UI plus the in-scene tools.

## Status

Scoped, not started. Next concrete step is the Phase-0 prototype, **after** the Qt + VTK-with-Qt
prerequisite is sorted.
