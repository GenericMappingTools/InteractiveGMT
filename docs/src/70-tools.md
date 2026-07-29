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
