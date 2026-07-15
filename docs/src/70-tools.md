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
