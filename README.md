# InteractiveGMT

[![Stable Documentation](https://img.shields.io/badge/docs-stable-blue.svg)](https://joa-quim.github.io/InteractiveGMT.jl/stable)
[![Development documentation](https://img.shields.io/badge/docs-dev-blue.svg)](https://joa-quim.github.io/InteractiveGMT.jl/dev)
[![Test workflow status](https://github.com/joa-quim/InteractiveGMT.jl/actions/workflows/Test.yml/badge.svg?branch=main)](https://github.com/joa-quim/InteractiveGMT.jl/actions/workflows/Test.yml?query=branch%3Amain)
[![Coverage](https://codecov.io/gh/joa-quim/InteractiveGMT.jl/branch/main/graph/badge.svg)](https://codecov.io/gh/joa-quim/InteractiveGMT.jl)
[![Docs workflow Status](https://github.com/joa-quim/InteractiveGMT.jl/actions/workflows/Docs.yml/badge.svg?branch=main)](https://github.com/joa-quim/InteractiveGMT.jl/actions/workflows/Docs.yml?query=branch%3Amain)
[![BestieTemplate](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/JuliaBesties/BestieTemplate.jl/main/docs/src/assets/badge.json)](https://github.com/JuliaBesties/BestieTemplate.jl)

## NOTE: This is Claude generated README that has more technical details than need.

Interactive 3-D viewing of [GMT.jl](https://github.com/GenericMappingTools/GMT.jl) data —
grids, point clouds, and `GMTfv` solids / polygon meshes — in a **self-contained Qt6 + VTK**
window. Its own Qt window, VTK render pipeline, interaction gizmo, cube
axes, colour bar, shading, vertical curtains, in-window Julia console and data viewer.

> Windows-only (the viewer ships as a Windows DLL). But there is nothing that prevents future Linux and MacOS versions


## Install

See more extended instructions at [docs]() but basically, have a GMT.jl updated version and do

```julia
using GMT

iGMTinstall()
```

## Quick start

Use the `i'GMT` icon that you now have on your desktop. Or, from a Julia REPL

```julia
using InteractiveGMT, GMT
G   = GMT.peaks()
fig = view_grid(G)                 # opens a window, returns a QtFigure handle
```

The call is **non-blocking**: it returns immediately and a Julia `Timer` pumps the Qt loop
(~50 Hz) so the REPL stays usable while the window is open. (In a `julia script.jl` run with no
REPL, end the script with `wait_windows()` to keep the process alive until the window closes.)

## API

| function | shows |
|----------|-------|
| `view_grid(G; …)`   | a `GMTgrid` surface (CPT colour or image `drape`, `vcurtain`, overlays) |
| `view_points(D; …)` | a coloured point cloud (Ctrl+right-drag rubber-band selection) |
| `view_fv(fv; …)` / `view_fv("torus"; …)` | a `GMTfv` solid / named solid / polygon mesh |
| `f3dview(x; …)`     | front-door dispatch over all of the above |
| `add!(fig, D; …)`   | add line/point overlays to a live grid window |
| `add_curtain!(fig, path; …)` | hang a vertical image curtain (seismic / midwater profile) |
| `show_table(fig, D)` | display tabular data in the window's Data Viewer tab |
| `selection(fig)`     | read back the rubber-band-selected point rows |
| `isalive(fig)` · `save_png(path)` · `wait_windows()` | window utilities |

The functions are documented in their docstrings (and, in depth, in `QTVTK_PLAN.md`). Each
overlay/curtain is interactive: right-click for a context menu; the **Scene Objects** dock lists
every element with a show/hide checkbox.

## In-window Julia console

A **Julia Console** dock runs commands straight in the host session (the viewer is in-process),
with `fig` pre-bound to that window — so `add!(fig, [x y z]; mode=:points)` works with no handle
typed. See the docstrings / `QTVTK_PLAN.md` for the C++↔Julia callback mechanism.

## Examples

```julia
include(joinpath(pkgdir(InteractiveGMT), "examples", "solids.jl"))
include(joinpath(pkgdir(InteractiveGMT), "examples", "curtain.jl"))   # needs network (grdcut)
```
