# InteractiveGMT — User Manual

**Interactive 3-D viewing for GMT data**

---

## Getting Started

### Installation

```bash
# In Julia
import Pkg; Pkg.add("https://github.com/joa-quim/InteractiveGMT.jl.git")
```

**Requirements:** Windows only. The viewer ships as a pre-built DLL.

### Quick Example

```julia
using InteractiveGMT, GMT

# View a grid
G = GMT.peaks()
fig = view_grid(G)

# View points
D = GMT.mat2ds(rand(100,3))
fig = view_points(D; color="red")

# X,Y plot
t = 0:0.1:10
y = sin.(t)
fig = xyplot([t y])
```

The window opens immediately. The REPL stays usable.

---

## Window Layout

```
┌────────────────────────────────────────────────────────────┐
│ File  View  Geography  Tools  Help           [2D] [Open]   │ Toolbar
├────────────────────────────────────────────────────────────┤
│                                                            │ │
│                                                            │ │
│                     3-D View                              │ │
│                   (VTK Canvas)                            │ │  Scene
│                                                            │ │  Objects
│                                                            │ │  Dock
│                                                            │ │
├────────────────────────────────────────────────────────────┤
│ │ Scene Objects │ │ Julia Console │ │ Profile │          │ Dock Tabs
└────────────────────────────────────────────────────────────┘
```

**Right panel:** Scene Objects (all elements in your scene)
**Bottom panel:** Profile, Julia Console (type commands)
**Status corner (bottom-right):** CRS chip (`EPSG:4326`, `EPSG:000` = unreferenced) + Messages
button — the speech bubble opens the Messages dock (execution messages); a red dot = unread

---

## 3-D Grid Viewer

### Opening Data

```julia
# From file
G = gmtread("my_grid.nc")
fig = view_grid(G)

# With options
fig = view_grid(G;
    cmap="relief",      # Colormap
    ve=5,               # Vertical exaggeration
    drape=image,        # Drape an image
    name="My Grid"      # Name in Scene Objects
)
```

### Navigation

| Mouse | Action |
|-------|--------|
| Left-drag | Rotate view |
| Middle-drag | Pan (move sideways) |
| Scroll wheel | Zoom in/out |
| Ctrl+right-drag | Select points (rubber band) |
| Right-click | Context menu |

**Keyboard shortcuts:**
- `e` — Toggle mesh edges
- `2` — Toggle 2-D/3-D view

### Vertical Exaggeration

Adjust relief vertical scale:

1. Menu: **View → Vertical Scale…**
2. Drag the gizmo ring (blue ring around the gizmo)
3. Double-click the gizmo to reset

### Flat 2-D View

Switch to map view (top-down, no perspective):

- Toolbar: **[2D]** button
- Menu: **View → 2-D View**

The relief flattens; grid shows colour only. Rotations lock.

### Colormap

Change the colour scheme:

1. Right-click **"Color Bar"** in Scene Objects
2. Pick a colormap (16 GMT masters)
3. Or choose **Custom…** to load a .cpt file

**Drag the colour bar** to move it.

### Hillshade

Illuminate relief like a shaded map:

- **View → Hillshade Lambert** — VE-corrected
- **View → Hillshade grdimage** — GMT gradient style
- **View → Cast Shadows** — PBR shadows (slow, experimental)

Hillshade and shadows are mutually exclusive.

### Adding Overlays

Add lines or points to a live grid:

```julia
# Line overlay
coast = gmtread("coastline.gmt")
add!(fig, coast; mode=:lines, color="black", width=1.5)

# Point overlay
stations = gmtread("stations.txt")
add!(fig, stations; mode=:points, color="red", size=10)
```

**Right-click** any overlay in Scene Objects for options.

### Image Drape

Drape a georeferenced image over the terrain:

```julia
# View grid
fig = view_grid(GMT.peaks())

# Drape image
I = gmtread("satellite.tif")
add!(fig, I; drape=true)
```

Or drape on open:

```julia
fig = view_grid(G; drape=I)
```

### Vertical Curtain

Hang a seismic profile vertically:

```julia
add_curtain!(fig, "seismic.nc"; 
    x=0,          # X position
    rotation=45   # Azimuth (degrees)
)
```

---

## Point Clouds

### Opening Points

```julia
D = gmtread("points.xyz")
fig = view_points(D;
    color="blue",   # Colour
    size=5,         # Point size
    name="Stations"
)
```

### Selecting Points

1. Hold **Ctrl**
2. Right-drag a box around points
3. Selection appears in Data Viewer

```julia
# Get selected rows
sel = selection(fig)
```

---

## GMTfv Solids

### Named Solids

```julia
# Platonic solids
fig = view_fv("icosahedron"; size=2)

# Primitives
fig = view_fv("torus"; major=2, minor=0.5)

# Operations
fig = view_fv("revolve"; profile=...)
fig = view_fv("loft"; sections=[...])
fig = view_fv("extrude"; face=..., height=1)
```

### Polygon Meshes

```julia
fv = poly2fv(D)      # Convert polygon to solid
fig = view_fv(fv; color="orange")
```

**Toolbar:** **3-D Bodies** button (flyout menu) for quick solids.

### Body Placement

- Empty launcher: solid replaces the blank space
- Existing solid: new solid replaces it
- Grid window: solid opens in new window

---

## X,Y Plot Tool

### Opening the Plotter

```julia
# From data
t = 0:0.1:10
y = sin.(t) .* exp.(-t/5)
fig = xyplot([t y]; xlabel="Time (s)", ylabel="Amplitude")

# From file
D = gmtread("data.txt")
fig = xyplot(D)
```

### Adding Series

```julia
add!(fig, more_data; label="Series 2", linestyle="--")
```

### Multiple Pages

Click **[+]** tab to add a new page.

**Right-click tab:**
- New / Duplicate / Rename / Delete

### Time Series

```julia
# X is epoch seconds
fig = xyplot([epoch_seconds values]; xtime=true)
```

X-axis auto-formats as dates/times.

**Menu:** **Misc → Time Format** — choose format.

### Log Axes

```julia
# Log X
fig = xyplot(data; xscale=:log)

# Log Y
fig = xyplot(data; yscale=:log)

# Both
fig = xyplot(data; xscale=:log, yscale=:log)
```

Or use **Misc → Log X / Log Y** menu.

### Series Properties

Double-click a series name in Object Manager (or right-click):

- Colour
- Width
- Style (solid/dashed/dotted)
- Marker (circle/square/triangle/…)
- Marker size

### Analysis Menu

Transform your data:

| Menu Item | What It Does |
|----------|--------------|
| Remove Mean | Subtract mean from Y |
| Remove Trend | Remove linear trend |
| 1st Derivative | dY/dX (new window) |
| 2nd Derivative | d²Y/dX² (new window) |
| FFT Amplitude | Power spectrum (via GMT.spectrum1d) |
| FFT PSD | Power spectral density |
| Autocorrelation | Autocorrelation (new window) |
| Fit Polynomial | Fit degree-N polynomial |
| Savitzky-Golay | Smooth (window size dialog) |
| Butterworth | Low/high-pass filter (cutoff dialog) |
| Filter Outliers | Despike (σ threshold dialog) |
| Spector-Grant | Depth-to-sources (drag band on spectrum) |

**Usage:** Select a row in Object Manager first, then choose analysis.

### Saving

**Menu:**
- **File → Save** — Save current page
- **File → Save As…** — Choose filename

---

## Geography Menu

### Coastlines

Add GSHHG shorelines:

1. **Geography → Plot Coastline…**
2. Choose resolution (crude/low/intermediate/full/high)
3. Click OK

### Volcanoes

Plot global volcano locations:

1. **Geography → Plot Volcanoes**
2. Hover over points for metadata

### Tides

Download and plot tide gauge data:

**Geography → Download Mareg:**
- **Last 2 Days** — Auto-fetch recent
- **Calendar…** — Choose date range

Data from IOC sea-level monitoring.

### CRS (Coordinate Reference)

When you load georeferenced data (images/grids), the Geography menu unhides. The window sets its CRS from the data.

---

## File Operations

### Opening Files

```julia
# Via code
fig = iview("my_file.nc")   # Auto-detects type

# Via menu
# File → Open... (or click [Open] toolbar button)
```

**Supported:** GMT grids, images, datasets.

### Drag and Drop

Drop a file onto the window — auto-opens.

**Also:** Drag files onto `iview.bat` (desktop launcher).

### Saving

**Grids:**
- **File → Save Grid…** — Save the surface as .nc

**Images:**
- **File → Save Image…** — Export screenshot

**Per-object:**
- Right-click item in Scene Objects
- Choose **Save…**

### Recent Files

**File → Recent Files** — Quick-reopen (21 items, grouped by type).

### Background Region

Create an empty 2-D map with specific bounds:

1. **File → Background Region…**
2. Enter W/E/S/N
3. Check **Is Geographic?** for lon/lat
4. Click OK

Useful as a blank canvas for adding data.

---

## Tools Menu

### Tiles Tool

Download and view web map tiles (satellite, roads, terrain):

1. **Tools → Tiles Tool**
2. Picker opens with world map
3. **Click tiles** to select (toggle)
4. Choose **Provider** (e.g., Esri World Imagery)
5. Set **Zoom** level
6. Click **GO**

**Features:**
- **Anchor** — Drag star to center view
- **Pan** — Use scrollbars
- **Cache** — Where to store tiles (default: ~/.gmt/cache_tileserver)
- **Mercator** — Toggle Mercator vs Geographic projection

Result opens as a new window.

### Project

Reproject the window's raster (grid or image) into another coordinate reference system — an
interface to `gdalwarp`:

1. **Tools → Project…**
2. **Source Referencing System** — pre-filled from the window's own CRS (the same one the
   bottom-right EPSG chip shows). Leave blank to let GDAL read it off the data.
3. **Destination Referencing System** — type a PROJ4 / WKT / `EPSG:nnnn` string, or pick from the
   **Projections** combo (which just fills this box, so it stays editable).
4. Choose an **Interpolation method** (nearest neighbour / bilinear / cubic / cubicspline).
5. Optionally force the output **Rows + Columns**, or an **x inc / y inc** resolution in target
   units (resolution wins when both are given; blank = GDAL's own guess).
6. Click **OK**.

The result lands in the SAME window as a new **Reprojected (…)** row in Scene Objects — checked,
with its own axes in the target's units, the source layer unchecked — and the window's CRS (and so
the EPSG chip) is re-stamped to the target system.

The window's **vector content follows the raster**: line/point overlays, symbol layers, drawn
polygons / polylines / rectangles / fault traces and text labels are all moved into the target
system in place (fault planes re-derive). Focal beachballs, rulers and image curtains are *not*
moved — a beachball is a fixed-radius glyph, a ruler's labels are measured lengths and a curtain is
a texture hung on a track; the Messages dock says how many were left behind.

### Make movie

Render this window frame by frame and encode the result. Works on **any** window — it animates
whatever the window is showing.

1. **Tools → Make movie**
2. Pick where the frames come from (**Frames come from**):

| Source | What each frame does | Needs |
|--------|---------------------|-------|
| **Camera orbit** | Spins the camera by a fixed step | Any window |
| **Cube layers** | Steps through the layers of the 3-D cube the window is showing | A cube open (netCDF cube or an Aquamoto/NSWING tsunami cube) — the radio is disabled otherwise, and the panel says so |
| **Grid sequence** | Shows one grid per frame | A list of grids with the **same** size, registration, range and increment as the window's current grid |

3. Fill in that source's own fields:
   - **Camera orbit** — **Frames** (how many), **Azimuth per frame** and **Elevation per frame**,
     in degrees. 120 frames × 3° = one full turn.
   - **Cube layers** — **From**, **to**, **Step** (layer numbers, clamped to the cube's real layer
     count, which the panel shows).
   - **Grid sequence** — **Add…** the files (they play in list order), **Remove** / **Clear** to
     edit the list.
4. Optionally tick the **Annotations**:
   - **Label** — frame number / elapsed time / percent, plus a corner (`TL`, `TC`, … the GMT `+j`
     justification codes). Drawn white-boxed, like GMT's own default.
   - **Progress** — one of GMT's six indicators (disc + wedge, ring + arc, circular arrow,
     line + cross-mark, plain axis, axis + triangle), plus a corner.
5. Set the **Output**: **Movie file** (double-click the box, or **…**, to browse), **Format**
   (`mp4`, `webm`, `gif`, `png`), **Frames / s**, and whether to **delete the PNG frames
   afterwards**. Leave the name empty to get a stem named after what the window is showing, written
   to the working directory.
6. Click **Make movie**.

The shared progress bar counts the frames and then announces the encode; the dialog's status line
ends on *Done.* or points at the Julia console.

**`ffmpeg` must be on PATH** for `mp4`, `webm` and `gif`. `png` needs nothing — the numbered frame
sequence *is* the product, and "delete the frames" is ignored for it.

Closing the dialog with **X** parks it: it becomes a row in Scene Objects (double-click to bring it
back), so a movie's settings survive while you go and adjust the scene. It never parks mid-render.

Everything the dialog does is one `movie(...)` call, so the same movie can be made from the console
— with a callback when a frame has to do more than the dialog offers. `movie` and the frame
mutations (`orbit!`, `set_layer!`, `replace_grid!`) and annotations (`add_label!`, `add_progress!`)
are documented under **[Movies](95-reference.md#Movies)** in the API reference, and in the REPL with
`?movie`.

```julia
# Every layer of the cube this window has open
movie(fig; name="cube", frame_rate=15, clean=true)

# Layers 20 to 80 only (frames count LAYER NUMBERS in this form)
movie(fig; frames=20:80, name="sweep")

# ... and with a callback, when a frame must do more than change the layer
movie(fig; frames=nlayers(fig)) do fig, f
    set_layer!(fig, f.index); orbit!(fig, 0.5)
end
```

---

## Base Map Picker

Quick-add a world base map:

1. Click **[Base Map]** toolbar button (grid icon before shapes)
2. Picker shows 4×8 tile grid over ETOPO
3. Click a tile or drag a region
4. Tile loads as georeferenced image

**Options:**
- **World-Map-Tiles** vs **Whole World-Map**
- **[-180 180]** vs **[0 360]** (Pacific-centered)

---

## 3-D Bodies Flyout

Quick-insert a geometric solid:

1. Click **[3-D Bodies]** toolbar button (flyout menu)
2. Choose solid (Cube/Sphere/Torus/etc.)
3. Solid appears in scene

**For operations:** Choose **Revolve / Loft / Extrude** — these open dialogs.

---

## Swipe / Link Tool

One toolbar slot, two ways to compare two rasters — click the small **▾** arrow to choose
which.

**Toolbar:** the **[Swipe]/[Link]** toggle (immediately before the **[i]** Info button). Its
icon shows the currently selected mode; the dropdown arrow switches between them. Clicking the
slot itself turns the SELECTED mode on/off.

### Swipe — compare two layers of THIS window

Splits two grids/images of the same window across a draggable vertical divider — the layer on
the left of the divider shows on the left, the layer on the right shows on the right. Greyed
out until the window holds at least two grids or images.

1. Load a second grid/image into the window (drag and drop, **File → Open**, or `add!`)
2. Pick **Swipe** from the dropdown, then click the slot to turn it on
   - Exactly two rasters: they are paired automatically
   - More than two: a small dialog asks which layer to pair the currently displayed one with
3. **Drag the divider** (or its round handle) left and right
4. Click the slot again to turn it off

Works in both the flat 2-D map and the 3-D perspective view — the divider stays a fixed
vertical line on screen while you rotate, zoom or pan. Any pair works: grid against grid, grid
against image, image against image; the two halves never overlap, so draw order never matters.
While on, every **other** raster in the window is hidden (Scene Objects shows this); turning it
off restores exactly what was visible before. Deleting one of the paired layers switches the
tool off by itself.

### Link — switch between two layers of THIS window, keeping the view

Pairs two grids/images of the same window — same pairing as Swipe — but instead of splitting
them across a divider, shows **one at a time**. Right-click swaps which of the two is visible,
**without moving the camera at all**: whatever region and zoom you're looking at stays exactly
as it was, only the raster underneath changes.

1. Pick **Link** from the dropdown, then click the slot to turn it on
   - Exactly two rasters: paired automatically
   - More than two: a small dialog asks which layer to pair the currently displayed one with
2. **Right-click** anywhere in the window: the other layer of the pair becomes visible instead
3. Click the slot again to turn Link off (restores whatever was visible before)

The two layers only need to **overlap**, not match — a linked pair can be a grid and an image of
completely different resolution and footprint, as long as they describe some of the same
ground. No overlap at all is refused when you turn Link on. Deleting one of the paired layers
switches the tool off by itself.

---

## Scene Objects Panel

Every element in your scene appears here:

| Icon | Element |
|------|---------|
| 🌐 | Grid surface |
| 🖼️ | Image |
| 〰️ | Line overlay |
| ⚪ | Symbol layer |
| 🔷 | Polygon |
| 📊 | Profile |

**Checkbox:** Show/hide element

**Right-click menu:**
- **Properties** — Edit appearance
- **Show data table** — View coordinates
- **Save…** — Export to file
- **Delete** — Remove

**Stacking** (images, vectors):
- **Stack up / down** — Move in draw order
- **Place on top / at bottom**

---

## Julia Console

Type Julia commands directly in the window:

```julia
# In the Julia Console dock
add!(fig, [1 2 3; 4 5 6]; mode=:lines)
G2 = GMT.peaks()
add!(fig, G2)
```

**`fig` is pre-bound** to the window.

**Messages dock** (speech-bubble button in the bottom-right status corner, or View > Messages Panel):
Shows execution errors from menu actions (drop, geography, basemap, etc.).

---

## Common Tasks

### Create a shaded relief map with coastlines

```julia
using InteractiveGMT, GMT

# Load DEM
G = gmtread("dem.nc")

# View with hillshade
fig = view_grid(G; cmap="geo", ve=5)

# Turn on hillshade
# (In window: View → Hillshade Lambert)

# Add coastlines
coast = gmtread("coastline.gmt")
add!(fig, coast; mode=:lines, color="black", width=1)
```

### Plot a time series with spectrum

```julia
# Load tide data
D = gmtread("tide.txt")  # epoch_seconds, sea_level

# Plot
fig = xyplot(D; xlabel="Time", ylabel="Sea level (m)", xtime=true)

# Compute spectrum
# (In window: select series → Analysis → FFT PSD)
```

### Drape satellite imagery over terrain

```julia
# Load DEM and image
G = gmtread("dem.nc")
I = gmtread("satellite.tif")

# View with drape
fig = view_grid(G; drape=I, cmap="gray")

# Adjust lighting
# (In window: View → Cast Shadows)
```

### Compare two grids

```julia
G1 = gmtread("model.nc")
G2 = gmtread("observation.nc")

# View first
fig = view_grid(G1)

# Add second as a second layer
add!(fig, G2)

# In the window: click the [Swipe] toolbar toggle, then drag the divider
```

Or blend them instead of swiping:

```julia
# Add second as image overlay (for transparency)
add!(fig, G2; drape=true, opacity=0.5)
```

---

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `Ctrl+O` | Open file |
| `2` | Toggle 2-D/3-D |
| `e` | Toggle mesh edges |
| Ctrl+left-drag | Rotate (alternative) |
| Middle-drag | Pan |
| Scroll | Zoom |
| Ctrl+right-drag | Select points |
| Right-click | Context menu |

---

## Tips

- **Large grids:** Tiled LOD auto-kicks in >512 nodes. Memory drops ~9×.
- **Empty launcher:** Drag files onto it — promotes to real data in-place.
- **Multi-page X,Y:** Use tabs for related plots. They share the same window.
- **Time axes:** Set `xtime=true` for epoch seconds. Auto-formats.
- **Vector stacking:** All vectors (lines, symbols, polygons) share one draw order. Use Stack up/down.
- **Colour bar:** Drag it to move. Right-click to change colormap.
- **Gizmo:** Drag blue ring for vertical exaggeration. Drag arrows to pan.
- **Error messages:** Check the Messages dock (status-corner bubble), not the REPL.
- **Tool images:** Open via tool dialogs (Tiles Tool, Base Map) → ExtraObj, not primary surface.

---

## Getting Help

- **Help → Controls** — Show this help in-window
- **Help → About** — Version info

**Julia REPL:**

```julia
?view_grid     # Docstring
?xyplot        # Docstring
```

---

## File Format Support

**Via GMT.jl / gmtread:**
- **Grids:** .nc, .grd (GMT netCDF, GeoTIFF)
- **Images:** .tif, .jpg, .png (georeferenced)
- **Tables:** .txt, .dat, .xyz (text, GMT dataset)

**Via VTK (read natively, no GMT/GDAL involved):**
- **VTK XML:** .vti, .vtr, .vts, .vtp, .vtu (+ the .pvti/.pvtr/.pvts/.pvtp/.pvtu parallel forms)
- **VTK multiblock:** .vtm (first non-empty block is displayed)
- **VTK legacy:** .vtk (any dataset type)
- **VTKHDF:** .vtkhdf

What the file holds decides how it is shown, using the same display paths as any other source:

| VTK dataset | Shown as |
|---|---|
| 2-D structured, axis-aligned, uniform spacing | a **grid** layer (colorbar, shading, hillshade, readout) |
| anything with polygon cells | a **mesh** layer (the view switches to 3-D for it) |
| line cells only | a **line overlay** |
| points only | a **point cloud** |

A volume, a curvilinear grid or an unstructured mesh is reduced to its bounding surface first. Plain
`.hdf` / `.h5` files are **not** VTK — those still go through GDAL.

### Every opened file lands in the same window

Opening a file never spawns a second window. Whatever it holds becomes a new row in **Scene
Objects**, and:

- the new layer is **shown and checked**
- everything that was displayed before is **hidden and unchecked** (its rows, and all their
  children, follow)
- the **axes cube and camera re-frame onto the new layer's own X, Y *and* Z extent** — a new file is
  a new quantity in its own units and never keeps the previous layer's frame

Nothing is deleted: re-tick any earlier row in Scene Objects to bring it back.

**Saving:**
- **Grids:** GMT netCDF (.nc), GeoTIFF, JPEG2000, Erdas, Surfer 6, ENVI, VTK (.vti / .vtp / .vtk)
- **Images:** GeoTIFF, JPEG2000, Erdas, ENVI, JPEG, PNG, TIFF, BMP, VTK (.vti / .vtk)
- **Tables:** Text, OGR (.shp, .gpkg, .kml)

The VTK save entries are written in-process by VTK itself: `.vti` writes a grid's z (or an image
layer's own RGB raster), `.vtp` writes the layer's rendered polydata, `.vtk` writes either.

---

*Version 0.1.0 (unregistered)*
*Windows-only*
