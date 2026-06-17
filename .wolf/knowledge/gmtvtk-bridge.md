---
name: gmtvtk-bridge
description: "gmtvtk = self-contained Qt+VTK viewer (no f3d); Julia bridge via gmtvtk.dll C API, verified"
metadata: 
  node_type: memory
  type: project
  originSessionId: cd809aef-dac3-4c16-b60b-bedb14b7135f
---

The Qt+VTK viewer in `qtvtk_proto/` is now a **self-contained, f3d-independent**
3-D viewer with a working **Julia bridge** (Phase-0 of [[gmtf3d-qtvtk-scope]] / QTVTK_PLAN.md done).

**User constraint (2026-06-12):** this VTK+Qt path must NOT reference f3d in function
or file names. Prefix is `gmtvtk_`. Dropped f3d from titles/About/comments.

**Build (`qtvtk_proto/CMakeLists.txt`):** `main.cpp` compiles into BOTH
- `gmtvtk.dll` (SHARED) — the C API bridge, and
- `qtvtk_proto.exe` (WIN32) — standalone demo (synthetic MATLAB peaks).

**C API (extern "C", `__declspec(dllexport)` via `GMTVTK_API`) — NON-BLOCKING:**
- `gmtvtk_view_grid(const double* z, int nx, int ny, double x0,x1,y0,y1, int geographic, const char* title)`
- `gmtvtk_view_demo(void)`
- `gmtvtk_process_events(void)` → pumps the Qt loop once, returns # open windows (0 = all closed)

`buildAndShow(...)` builds the scene (surface + cube axes + scalar bar + gizmo),
SHOWS a heap `QMainWindow` (`WA_DeleteOnClose`, destroyed-signal deletes the Scene+Gizmo,
decrements `g_openWindows`) and RETURNS. One shared `QApplication g_app` created lazily
by `ensureApp()` (static dummy argc/argv). The host drives the loop by calling
`gmtvtk_process_events` repeatedly. The standalone exe `main()` instead calls `g_app->exec()`.

**Non-blocking from Julia:** `bridge.jl` starts a `Timer(interval=0.02)` that ccalls
`gmtvtk_process_events`; when it returns 0 the timer stops. REPL stays usable while the
window is open. `z` is COPIED into VTK arrays inside the ccall, so it can be GC'd after return.

**Geographic vertical exaggeration (`computeScales` in main.cpp):** VE factor is relative
to TRUE scale (VE 1 = 1:1). geographic → `xfac = cos(midlat)` (lon aspect), `zfac = 1/111111`
(z metres → lat-degree base unit, `kMetersPerDegLat=111111`). cartesian → xfac=zfac=1.
**10% floor:** if true relief `zmax-zmin` (metres) < 10% of the horizontal footprint
(max(width,height) in metres, lon width uses `cos(midlat)`), initial VE is bumped to
`0.10*Hm/zspanM` so the surface is never a flat sheet. Also rescues non-metre z. z assumed
metres. Cube-axis labels stay TRUE via `SetX/Y/ZAxisRange(true ranges)` despite the actor scale.

**Geographic detection:** `bridge.jl` `_isgeographic(G) = GMT.isgeog(G)` (GMTgrid field is
`G.geog`; GMT exports `isgeog`). Do NOT use a lon/lat range heuristic — it mis-flags small
cartesian grids near (0,0) as geographic (bug-090). User can override `view_grid(G; geographic=…)`.

**Grid layout (matches GMT.jl):** `makeGridFromArray` reads `z` column-major,
`ny` rows × `nx` cols, element (iy,ix) at `z[ix*ny + iy]` → point `(x[ix], y[iy])`,
y ascending. NaN nodes: points still emitted (orphans), quads touching a NaN corner
skipped. Julia passes `ny,nx = size(G.z)`, `r = G.range`.

**Julia side (`qtvtk_proto/bridge.jl`):** sets VTK + Qt `bin` on `ENV["PATH"]` and
`QT_QPA_PLATFORM_PLUGIN_PATH` (same dirs as `run.bat`) BEFORE `Libdl.dlopen("build/gmtvtk.dll")`,
then `ccall` `view_grid(G::GMTgrid)` / `view_demo()`. `Float64.(G.z)` to match `Ptr{Cdouble}`.

**Verified 2026-06-12:** `julia bridge.jl` (GMT.peaks()) opened a live window titled
"GMT 3-D Viewer — peaks (via Julia)" — bridge works end-to-end.

Interaction (Fledermaus-style): LEFT-drag horizontal=azimuth, vertical=tilt — handled by
the DEFAULT trackball-camera style (do NOT replace it). MIDDLE-click=set centre of rotation
to picked SURFACE point. RIGHT-drag/wheel=zoom (default).

GOTCHA: do NOT `iren->SetInteractorStyle(custom)` to add the middle-click — swapping the
QVTKOpenGLNativeWidget's default style KILLED left-drag rotate entirely (regression, reverted).
Instead the recenter is a `vtkCallbackCommand` MiddleCB observer on the interactor at priority
10 with SetAbortFlagOnExecute(1) on MiddleButtonPress+Release (always-abort → default
middle-pan never starts; left rotate untouched). MiddleCB: vtkCellPicker PickFromListOn +
AddPickList(s->surf) (never the gizmo); set cam focalpt=pick, move camera keeping view
dir+dist (recentres on the point); Render → PlaceCB(StartEvent) pins gizmo to new focal pt.

Gizmo: amber-cone=vert.exag, tip-ring=tilt, compass-ring=azimuth, 'x'=toggle (its left
observers priority 10 grab handles first; on a miss DragCB does NOT abort so the event falls
through to the default style rotate). Default camera = world +Z up, oblique.

**Gizmo cone hit-test (2026-06-12 fix):** grab the cone via the base->apex SEGMENT
(distToSeg) with the THIN base radius — NOT a radius from the cone centre. The old
centre-radius ballooned when the cone is tall (large `curSz`, i.e. big VE on geographic
grids) and swallowed the surrounding compass ring, so azimuth clicks read as VScale
("ring does nothing, stretches instead"; cartesian peaks at VE×1 had a short cone so it
worked). VE clamp widened `[0.1,50]→[0.01,1e4]` (geographic auto-VE exceeds 50).

**Surface quality (2026-06-12):** raw grid has no normals → VTK flat-shades each quad
(faceted). Fix: `vtkPolyDataNormals` (SplittingOff, smooth) + mapper
`InterpolateScalarsBeforeMappingOn` (per-fragment colour).

**Colormap = GMT CPT (2026-06-12).** The user's "horrible, only 4 colours" was the
hardwired HSV jet (blue→cyan→green→yellow→red) — a garish rainbow, NOT a banding bug
(offscreen `gmtvtk_save_png` always rendered it smooth on this GPU). Fix: pass a real
GMT colormap. C API `gmtvtk_view_grid(..., const double* rgb, int ncolor, title)` —
`rgb` is ncolor×3 in 0..1 row-major; `buildAndShow` builds the vtkLookupTable from those
table values (ncolor=0 → built-in HSV fallback, used by the demo). `bridge.jl` `_cpt_rgb`
calls `makecpt(cmap=cmap, range=(zmn,zmx,(zmx-zmn)/256), continuous=true)`, takes
`C.colormap` (N×3, 0..1), flattens `vec(permutedims(cm))`. `view_grid(G; cmap=:turbo)`
default; pass `:geo`, `:rainbow`, `:roma`, … or `nothing`. zmn/zmx via `extrema(filter(isfinite,z))`
to match the C side's NaN-ignoring TableRange. Verified cmap=:geo renders smooth.

NOTE: live on-screen GL == offscreen `vtkWindowToImageFilter` capture on this machine
(W2I reads the real render window). GDI CopyFromScreen / PrintWindow do NOT reliably
capture the GL surface — use `gmtvtk_save_png` for verification.

**Debug API:** `gmtvtk_save_png(const char* path)` snaps the most-recent window via
vtkWindowToImageFilter (GDI CopyFromScreen does NOT capture the GL surface reliably).
Render N frames first (pump gmtvtk_process_events in a loop) before saving.
