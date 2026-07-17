```@meta
CurrentModule = InteractiveGMT
```

# Getting Started

## Installation

**Requirements:** Windows only. The viewer ships as a Windows DLL.

### 1. Julia

Install Julia — **Julia 1.10 is strongly recommended** — and make sure it is on your `PATH`. Follow
the [Windows install guide](https://www.generic-mapping-tools.org/GMTjl_doc/documentation/general/install_julia_win.html#download-the-installer).

### 2. GMT.jl

If not already installed, from the Julia REPL package mode (press `]`):

```julia
] add GMT
```

Make sure you have at least GMT.jl version 1.41.2 (to update GMT.jl do on the Julia console `] up GMT`)

### 3. InteractiveGMT

```julia
using GMT

iGMTinstall()
```

### 4. The iGMT installer (graphical elements)

You have now a new icon on your desktop `i'GMT`. Use it as a normal Graphical Program.

Drag and drop files on the icon or an empty iGMT display. Or use the normal File -> Open ...

## Quick Start from Julia

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

The window opens immediately. The REPL stays usable.

## Basic Concepts

### Handles

Every viewer function returns a handle:

- `QtFigure` — 3-D grid/image window
- `QtPoints` — Point cloud window
- `QtFV` — Solid/mesh window
- `QtXYPlot` — X,Y plot window

Use these handles to add data, check status, or save.

### Non-Blocking Calls

All viewer calls return immediately. A Julia `Timer` pumps the Qt event loop at ~50 Hz.

In scripts without a REPL, end with `wait_windows()` to keep the process alive:

```julia
fig = iview(G)
wait_windows()  # Blocks until all windows close
```

### In-Window Julia Console

Every 3-D viewer has a **Julia Console** dock at the bottom. Type Julia commands directly:

```julia
# In the console
add!(fig, [1 2 3; 4 5 6]; mode=:lines)
```

The `fig` handle is pre-bound to that window.

## Next Steps

- [Grid Viewer](10-grid-viewer.md) — Display grids with hillshade, draping, overlays
- [Point Clouds](20-point-clouds.md) — View and select points
- [Solids and Meshes](30-solids.md) — GMTfv and named solids
- [X,Y Plot Tool](40-xyplot.md) — 2-D plotting with analysis
- [API Reference](95-reference.md) — Full function listing
