```@meta
CurrentModule = InteractiveGMT
```

# Tools

## Tiles Tool

Download and view web map tiles (satellite, roads, terrain).

**Menu:** Tools → Tiles Tool

### Usage

1. **Click tiles** to select (toggle selection)
2. **Choose Provider** (e.g., Esri World Imagery, OpenStreetMap)
3. **Set Zoom** level
4. **Click GO**

The mosaic opens in a new window.

### Features

- **Anchor** — Drag the star to center view
- **Pan** — Use scrollbars at edges
- **Cache** — Where tiles are stored (default: `~/.gmt/cache_tileserver`)
- **Mercator** — Toggle Mercator vs Geographic projection
- **Background** — At high zoom, shows coarse preview

### Keyboard/Mouse

- **Click** — Toggle tile selection
- **Drag** — Pan the view
- **Scroll** — Zoom in/out
- **Drag star** — Move anchor point

## Base Map Picker

Quick-add a world base map tile.

**Toolbar:** Click **[Base Map]** button (grid icon before shapes flyout)

### Usage

1. Picker shows 4×8 tile grid over ETOPO
2. **Click a tile** or **drag a region**
3. Tile loads as georeferenced image

### Options

- **World-Map-Tiles** — 45° patches
- **Whole World-Map** — Full globe
- **[-180 180]** vs **[0 360]** — Pacific-centered

## Swipe / Link Tool

One toolbar slot, two ways to compare two rasters — a **▾** dropdown chooses which (before the
**[i]** Info button). **Picking a mode from the dropdown starts it immediately** — there is no
separate arming click. Clicking the slot itself then turns the running mode off (and on again).
The slot is greyed out only when there is nothing at all to pair: one raster in this window and no
other window over the same region.

### Swipe — two layers of this window

Splits two grids/images of the SAME window across a draggable vertical divider. Needs at least two
grids or images in the window.

1. Load a second grid/image into the window (drop, **File → Open**, or `add!`)
2. Pick **Swipe** from the dropdown
   - Two rasters: paired automatically
   - More than two: a dialog asks which layer to pair the displayed one with
3. **Drag the divider** (or its round handle)
4. Click the slot to turn it off

Works in the flat 2-D map and the 3-D perspective view; the divider stays a fixed vertical screen
line while you rotate, zoom or pan. Any pair works — grid/grid, grid/image, image/image, halves
never overlap so draw order is irrelevant. Other rasters are hidden while swiping and restored
when it is switched off; deleting a paired layer switches the tool off by itself.

### Link — hold the right button to peek at the other one

Shows only ONE of a pair at a time. **Hold the right mouse button** to see the other one, **release**
to snap back to the one you started on — a momentary peek, not a toggle, so you compare the two by
flicking the button. The zoom you are already at is kept throughout. Which pair depends on what the
window holds — you never choose the flavour, only **Link**:

- **Two or more rasters in this window** → they pair with each other (the same pairing Swipe uses).
  Holding shows the partner layer, releasing brings the starting one back. The camera never moves.
- **Only one raster here** → it pairs with **another open iGMT window** over the same region (a
  dialog picks it when more than one qualifies). Holding raises that window, framed on the region
  this one is currently showing; releasing raises this one back. The partner's axes and data frame
  are untouched — only its camera moves. The gesture works from either window.

1. Pick **Link** from the dropdown
2. **Press and hold the right button** anywhere in the view; let go to come back
3. Click the slot to turn it off

While Link is on the right button belongs to it: no context menu pops and right-drag does not zoom.
Ctrl+right (rubber-band select) and right-click-while-drawing-a-polygon keep their own meaning.

The two only need to **overlap**, not match — a grid and an image of completely different
resolution and footprint pair fine. No overlap at all is refused, with the reason on the status
bar. Deleting a paired layer, or closing a paired window, switches the tool off by itself.

## Synthetic Terrain

Make a believable but entirely made-up digital terrain model (DTM) on any region and grid spacing:
mountain ranges cut by branching river valleys, with plains and basins between them. Useful for
tests, demos and teaching, and for trying a tool when no real data is at hand.

**Menu:** *Grid Tools → Synthetic terrain*, with two entries:

- **auto** — makes a terrain at once, with no questions: region `-R-13/23/30/60` at 0.05°
  (721 × 601 nodes), heights 0 to 1000, geographic coordinates (WGS84), and a new random seed
  each time.
- **params…** — opens a dialog where every setting can be chosen (below).

The new grid is added to the current window as **Synthetic terrain (seed N)**. The seed is in the
name, so the same terrain can be made again.

### How it is made

The terrain is not drawn, it is **grown** by a landscape-evolution model, the way geomorphologists
make synthetic relief. Three processes act together, step after step:

- **Tectonic uplift** lifts the land. Its rate varies smoothly across the region (a seeded random
  pattern): some areas rise fast and become mountain belts, others barely move and become plains
  and basins.
- **Rivers cut down** by the *stream-power law*: the erosion rate at a point is
  `K · A^m · S`, where `A` is the area draining through it and `S` its slope. Water is routed down
  the steepest of the 8 neighbours (D8), so every point belongs to a river network that reaches the
  border. Big rivers carve deep, gentle valleys; small ones, steep headwaters.
- **Hillslopes creep**: soil moves downhill in proportion to the curvature (linear diffusion),
  rounding ridges and smoothing slopes between the channels.

The model runs until uplift and erosion nearly balance (steady state). It is solved with the fast
implicit O(n) scheme of Braun & Willett (2013) on a working grid of at most 320 nodes along the
longer side, then interpolated (bicubic, GMT `grdsample`) onto the asked lattice. Valley spacing is
therefore the same at any output resolution, and the run time does not grow with it (under a second on a multi-core machine).
Finally the heights are scaled so the lowest node is exactly *z min* and the highest *z max*.

The border of the model domain is the base level (sea level) every river drains to. The domain is
padded beyond the asked region, so no edge of the result is pinned to zero.

### Dialog settings

**Output grid**

| Setting | Meaning |
|---|---|
| x min, x max, y min, y max | Region. Pre-filled from the window's grid when it has one, else `-13/23/30/60` |
| x inc, y inc | Node spacing. It must divide the region exactly |
| OR Ref grid / OR Window grid | Copy the region and spacing of a grid file, or of a grid in this window |

**Landscape**

| Setting | Default | Meaning |
|---|---|---|
| Feature (%) | 30 | Size of the uplift pattern (mountain belts and basins), in % of the larger side of the grid |
| Lowlands | 0.1 | 0–0.9: share of the area that barely rises (plains, basins). 0 = mountains everywhere |
| Hillslope | 0.3 | 0–1: soil creep. Low = sharp ridges and narrow valleys, high = rounded hills |
| Concavity | 0.45 | 0.2–0.8: the stream-power exponent `m`. Higher = rivers flatten faster downstream. 0.45 is typical of real rivers |
| Seed | 1 | Any whole number. **New seed** puts a random one in the box |
| z min, z max | 0, 1000 | Height of the lowest and the highest node |

The same seed and the same settings always give the same terrain, on any machine.

A region inside longitude/latitude limits (x within −180 to 360, y within −90 to 90) is marked as
a geographic grid (WGS84 lon/lat). Any other region gets no coordinate system.

### From Julia

```julia
using InteractiveGMT
G = InteractiveGMT.synth_terrain((-13, 23, 30, 60), (0.05, 0.05); zrange = (0, 1000), seed = 7)
```

Keywords: `zrange`, `feature` (a fraction, `0.3` = 30 %), `lowlands`, `hillslope`, `concavity`,
`seed`, and `geog` (`true`/`false`, to override the lon/lat rule).

### References

- Braun, J., & Willett, S. D. (2013). A very efficient O(n), implicit and parallel method to solve
  the stream power equation governing fluvial incision and landscape evolution. *Geomorphology*,
  180–181, 170–179.
- Howard, A. D. (1994). A detachment-limited model of drainage basin evolution. *Water Resources
  Research*, 30(7), 2261–2285.
- O'Callaghan, J. F., & Mark, D. M. (1984). The extraction of drainage networks from digital
  elevation data. *Computer Vision, Graphics, and Image Processing*, 28(3), 323–344.
- Barnes, R., Lehman, C., & Mulla, D. (2014). Priority-flood: an optimal depression-filling and
  watershed-labeling algorithm for digital elevation models. *Computers & Geosciences*, 62,
  117–127.
- Perlin, K. (2002). Improving noise. *ACM Transactions on Graphics*, 21(3), 681–682 (the random
  uplift pattern).

## 3-D Bodies Flyout

Quick-insert geometric solids.

**Toolbar:** Click **[3-D Bodies]** button (flyout menu)

### Solids

- **Platonic** — Tetrahedron, Octahedron, Dodecahedron, Icosahedron
- **Primitives** — Cube, Sphere, Cylinder, Cone, Torus
- **Operations** — Revolve, Loft, Extrude

### Placement

- Empty launcher: Solid replaces blank space
- Existing solid: New solid replaces it
- Grid window: Solid opens in new window
