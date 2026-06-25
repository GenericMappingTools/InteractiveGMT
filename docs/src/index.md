```@meta
CurrentModule = InteractiveGMT
```

# InteractiveGMT

Interactive 3-D viewing of [GMT.jl](https://github.com/GenericMappingTools/GMT.jl) data — grids, point clouds, and GMTfv solids — in a self-contained **Qt6 + VTK** window. No dependency on F3D.

## Features

- **3-D Grid Viewer** — Hillshade, drape, overlays, tiled LOD
- **Point Clouds** — Coloured points with rubber-band selection
- **Solids** — GMTfv meshes and named geometric solids
- **X,Y Plot Tool** — 2-D plotting with time axes, log axes, analysis
- **Geography Tools** — Coastlines, volcanoes, tide download
- **Web Tiles** — Satellite/road/terrain map downloader
- **In-Window Console** — Type Julia commands directly

## Quick Start

```julia
using InteractiveGMT, GMT

# View a grid
G = GMT.peaks()
fig = iview(G)

# View points
D = GMT.mat2ds(rand(100,3))
fig = iview(D; color="red")

# X,Y plot
t = 0:0.1:10
y = sin.(t)
fig = xyplot([t y])
```

## Installation

```julia
Pkg.add("https://github.com/joa-quim/InteractiveGMT.jl.git")
```

**Requirements:** Windows only. The viewer DLL is pre-built.

## Documentation

- [Getting Started](01-getting-started.md)
- [Grid Viewer](10-grid-viewer.md)
- [Point Clouds](20-point-clouds.md)
- [Solids and Meshes](30-solids.md)
- [X,Y Plot Tool](40-xyplot.md)
- [Utilities](50-utilities.md)
- [Geography Tools](60-geography.md)
- [Tools](70-tools.md)
- [API Reference](95-reference.md)

## Public API

```@index
Modules = [InteractiveGMT]
```
