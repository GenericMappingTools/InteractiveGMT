---
name: gmtf3d-qtvtk-scope
description: "Scope for the Qt+VTK direction — a distributable GMT 3D viewer owning VTK directly (f3d as importer or dropped), native Qt menus, in-process Julia control"
metadata: 
  node_type: memory
  type: project
  originSessionId: 66606494-1e50-4172-a78e-db5a1cdb9734
---

**Why (see [[f3dext-not-distributable]]):** f3d_ext/f3dx need f3d internals that stock f3d.dll
doesn't export → never a stock-f3d plugin, and ImGui custom UI needs f3d core. The distributable
path = OWN VTK directly + native Qt UI; use f3d only as a file importer (public API) or not at all
(GMT data → vtkPolyData directly). Decided with the user 2026-06-11.

## Architecture
- **Window/UI = Qt** (QMainWindow + a VTK widget). Native QMenu menubar + **context menus** (the
  Matlab-quality look the user wants, trivial + fully programmable), dialogs, docks, status bar.
- **3D = VTK directly** — `vtkGenericOpenGLRenderWindow` in a Qt GL widget, OUR `vtkRenderer`, OUR
  actors. Full renderer/actor/picking access (the thing f3d hides).
- **Data** — GMT grid → vtkPolyData built directly (we already build the structured mesh in
  `grid2fv_direct`; emit vtkPoints+cells+scalars). Colour via vtkLookupTable/vtkColorTransferFunction.
- **f3d** — optional file importer via PUBLIC API; for GMT data, bypass f3d. Likely DROP f3d from
  the interactive viewer entirely.

## What PORTS as-is (already plain VTK on a vtkRenderer we now own)
- f3dx profiler: vtkCellLocator vertical-ray sampling, (s,z), drape-line vtkActor, 2D vtkActor2D
  panel. Direct port.
- f3dx L1 cutter/implicit/result-actor.
- Mesh building (grid → polydata).

## What REBUILDS on our renderer (f3d_ext features; currently lean on f3d's renderer/importer)
- Cube axes → `vtkCubeAxesActor` (public). Colorbar → `vtkScalarBarActor` (public).
- Coord readout → `vtkPointPicker` + Qt status bar. Vertical-scale/scale gizmo → our own
  vtkInteractorStyle (simpler — direct actor transform, no model_scale option dance).
- Lines overlay (already have). Point clouds + per-point colour + rubber-band → vtkActor +
  `vtkHardwareSelector` (public). Round sprites → `vtkPointGaussianMapper`. Drape → `vtkTexture`.
- Vertical exaggeration → actor transform (we own it).

## What's LOST / must be reimplemented (f3d freebies)
- PBR + IBL/HDRI + raytracing: VTK has them (vtkOpenGLRenderer PBR, `vtkOSPRayPass`) but we wire them
  ourselves. Polished default lighting/camera: set up ourselves. f3d animation/format breadth: drop.

## Menus (the original problem) → native `QMenu`
Right-click → `QMenu::exec(globalPos)`. Submenus, icons, shortcuts, perfect AA/DPI — all native, all
programmable. Problem dissolves.

## Julia boundary — the crux (TWO options)
- **Q1 — separate Qt+VTK exe, GMTF3D launches it.** Julia→exe: grid as a file + JSON menu/config.
  exe→Julia callbacks: IPC (socket/pipe) — e.g. "item X clicked + data" → Julia runs closure. Clean
  separation, distributable exe, but callbacks need IPC + marshaling.
- **Q2 — in-process Qt+VTK lib with a C API, ccalled from Julia (RECOMMENDED).** Mirrors today's
  model (f3d ran in-process; callbacks via `@cfunction`). Swap "f3d in-process" → "our Qt+VTK lib
  in-process". Live Julia callbacks are EASY (`@cfunction`, in-process). Menu spec + callbacks come
  from Julia exactly like the profiler design. Challenge: Qt's `QApplication::exec()` blocks like
  f3d's interactor_start — drive it blocking+GC-safe (we already do this for the interactor) or
  pump `processEvents`. This keeps the Julian in-process control the user wants.

## PREREQUISITES — NOW SATISFIED (re-verified 2026-06-11, superseding the "no Qt" note)
- **Qt6 6.11.1 msvc2022_64 IS installed** at `C:\programs\Qt6\6.11.1\msvc2022_64`.
- **A standalone VTK 9.6.2 built WITH Qt6 EXISTS** at `C:\programs\compa_libs\VTK-9.6.2`
  (source), built to `compileds\` — `compa.bat` has `VTK_GROUP_ENABLE_Qt=YES`,
  `GUISupportQt=YES`, `VTK_QT_VERSION=6`, plus `RenderingRayTracing=YES` (ospray). Verified
  present: `QVTKOpenGLNativeWidget.h`, `vtkGUISupportQt.lib`, `vtkRenderingRayTracing.lib`,
  169 DLLs in `compileds\bin`. Independent of the f3d-superbuild — exactly the standalone
  VTK+Qt the plan wanted.
- Toolchain: MSVC 2022 Community (`vcvars64.bat`), VS-bundled cmake at
  `...\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`, ninja `c:\j\bin\ninja.exe`.

## Phases (rough)
0. Prototype: minimal Qt+VTK lib (C API) — QMainWindow + VTK widget + a vtkRenderer showing a grid
   polydata + a native right-click QMenu; Julia ccalls it, passes a grid, `@cfunction` on menu click.
   Proves Q2 end-to-end (incl. the Qt-loop-in-Julia + callback).
1. Port the profiler (ray-sample, drape line, 2D panel) onto the owned renderer.
2. Port f3d_ext features (cube axes, colorbar, scale gizmo, lines, points, selection).
3. Lighting/PBR/raytracing parity (vtkOSPRayPass), camera/defaults polish.
4. Packaging (Qt+VTK dlls + the lib + bundle).

**Cost: multi-week — essentially a new viewer app.** It's the only distributable path with native UI
+ the in-scene tools.

## STATUS — PHASE 0 PROTOTYPE BUILT (2026-06-11, look-gauge variant)
Standalone exe in repo `qtvtk_proto/` (`main.cpp`, `CMakeLists.txt`, `build.bat`, `run.bat`).
Look-gauge ONLY — NO Julia / C-API boundary yet (that's the real Phase-0 Q2 plumbing, still TODO).
Shows: peaks surface built as vtkPolyData (points+quad cells+z scalar, mirrors `grid2fv_direct`),
jet LUT, `vtkScalarBarActor`, `vtkCubeAxesActor` w/ gridlines, gradient bg, native `QMenuBar`
(File/View/Help), native right-click `QMenu` over the view, live coord readout in QStatusBar via
`vtkCellPicker` on MouseMove, vertical-exaggeration via actor Z-scale. Builds clean (MSVC2022+ninja),
runs. Build = `qtvtk_proto\build.bat`; run = `run.bat` (puts VTK+Qt bins on PATH).
Next: Q2 in-process C-API lib + Julia ccall + @cfunction menu callback; then port f3dx profiler.
