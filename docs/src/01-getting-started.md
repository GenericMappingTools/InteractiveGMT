```@meta
CurrentModule = InteractiveGMT
```

# Getting Started

## Installation

**Requirements:** Windows only. The viewer ships as a Windows DLL.

### 1. Julia

Install Julia — **Julia 1.10 is strongly advised** — and make sure it is on your `PATH`. Follow
the [Windows install guide](https://www.generic-mapping-tools.org/GMTjl_doc/documentation/general/install_julia_win.html#download-the-installer).

### 2. GMT.jl

If not already installed, from the Julia REPL package mode (press `]`):

```julia
] add GMT
```

### 3. InteractiveGMT

```julia
] add https://github.com/GenericMappingTools/InteractiveGMT
```

### 4. The iGMT installer (graphical elements)

Download and run the installer:

[iGMT-0.1.0-win64.exe](https://github.com/GenericMappingTools/InteractiveGMT/releases/download/initial-binary-release/iGMT-0.1.0-win64.exe)

Suggested install location: `c:\programs\iGMT`.

### Known annoyances

- The installer should create a desktop icon; if it doesn't, make one yourself from
  `...\iGMT\iview_app.vbs`.
- After installing, it may be necessary to run `using InteractiveGMT` once from the Julia REPL
  before the viewer works.

## Use as a normal Graphical Program

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
