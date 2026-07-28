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

## Swipe Tool

Compare **two** grids/images in one window across a draggable vertical divider: the paired
layers show on either side of the line.

**Toolbar:** the **[Swipe]** toggle (split-tile icon, immediately before the **[i]** Info
button). Greyed out until the window holds at least two grids or images.

### Usage

1. Load a second grid/image into the window (drop, **File → Open**, or `add!`)
2. Click **[Swipe]**
   - Two rasters: paired automatically
   - More than two: a dialog asks which layer to pair the displayed one with
3. **Drag the divider** (or its round handle)
4. Click **[Swipe]** again to turn it off

### Notes

- Works in the flat 2-D map and the 3-D perspective view; the divider stays a fixed vertical
  screen line while you rotate, zoom or pan
- Any pair works — grid/grid, grid/image, image/image. The halves never overlap, so draw order
  is irrelevant
- Other rasters are hidden while swiping and restored when it is switched off
- Deleting a paired layer switches the tool off by itself

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
