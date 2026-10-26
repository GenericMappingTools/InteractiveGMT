# InteractiveGMT — User Manual

**Interactive 3-D viewing for GMT data**

---

## Getting Started

### Installation

With an up-to-date GMT.jl:

```julia
using GMT
iGMTinstall()
```

**Requirements:** Windows 11, macOS or Linux. The viewer ships as a pre-built shared library
(`gmtvtk.dll` / `libgmtvtk.dylib` / `libgmtvtk.so`).

### Quick Example

```julia
using InteractiveGMT

# View a grid
G = GMT.peaks()
fig = viz(G)

# View points
D = GMT.mat2ds(rand(100,3))
fig = viz(D; color="red")

# X,Y plot
t = 0:0.1:10
y = sin.(t)
fig = plot([t y])
```

The window opens immediately. The REPL stays usable.

---

## Window Layout

```
┌────────────────────────────────────────────────────────────────────────┐
│ File Image View Geography Geophysics Satellite Tools GMT Grid Tools    │ Menus
│ T & L Help                                                             │
├────────────────────────────────────────────────────────────────────────┤
│ [Open] [2D/3D ▾] [Base Map] [Ruler] [shapes ▾] … [Swipe ▾] [i ▾]       │ Toolbar
├──────────┬─────────────────────────────────────────────────────────────┤
│ Scene    │                                                             │
│ Objects  │                      3-D View                               │
│ Dock     │                    (VTK Canvas)                             │
│          │                                                             │
├──────────┴─────────────────────────────────────────────────────────────┤
│ ▸ Panels:  Profile │ Julia Console                                     │
└────────────────────────────────────────────────────────────────────────┘
```

**Left panel:** Scene Objects (all elements in your scene) — it can be dragged to the right edge
**Bottom panel ("Panels"):** Profile, Julia Console (type commands)
**Status corner (bottom-right):** CRS chip (`EPSG:4326`, `EPSG:000` = unreferenced) + Messages
button — the speech bubble opens the Messages dock (execution messages); a red dot = unread

---

## 3-D Grid Viewer

### Opening Data

```julia
# From file
G = gmtread("my_grid.nc")
fig = viz(G)

# With options
fig = viz(G;
    cmap="relief",      # Colormap
    drape=image,        # Drape an image
    title="My Grid"     # Window title
)
```

### Navigation

| Mouse | Action |
|-------|--------|
| Left-drag | Rotate view (pans in the flat 2-D map) |
| Middle-drag | Pan (move sideways) |
| Scroll wheel | Zoom in/out |
| Ctrl+left-drag | Draw a profile (on a grid) |
| Ctrl+right-drag | Select points (rubber band) |
| Right-click | Context menu |

**Keyboard shortcuts:**
- `e` — Toggle mesh edges
- `1` – `6` — Snap the camera to a preset view (`2` = straight down from the top)
- `R` — Reset camera (also **View → Reset Camera**)

### Vertical Exaggeration

Adjust relief vertical scale:

1. Menu: **View → Vertical Exaggeration…**
2. Or drag the gizmo's vertical **amber cone** (its two grey rings set tilt and azimuth)

### Flat 2-D View

Switch to map view (top-down, no perspective):

- Toolbar: the **2D/3D** view-mode button — its dropdown offers **2D**, **3D**, and, for
  geographic data, **Globe** and **Cube**
- Or the 3-D view's right-click menu

The relief flattens; grid shows colour only. Left-drag pans instead of rotating.

### Colormap

Change the colour scheme:

1. Left-click the **"Color Bar"** label in Scene Objects
2. Pick a colormap (GMT masters)
3. Or choose **Custom…** and type the name of any GMT CPT
4. **Color Palettes…** (also a toolbar button) opens the full palette editor

**Drag the colour bar** to move it.

### Hillshade

Illuminate relief like a shaded map with the toolbar **Illumination (Hillshade)** button. Its dialog
aims the light and picks the shading method — among them the Lambert hillshade, the GMT
grdimage-style gradient shading, and a PBR method whose panel has a **Cast shadows** checkbox.

### Adding Overlays

Add lines or points to a live grid:

```julia
# Line overlay
coast = gmtread("coastline.gmt")
add!(fig, coast; mode=:lines, color="black", size=1.5)

# Point overlay
stations = gmtread("stations.txt")
add!(fig, stations; mode=:points, color="red", size=10)
```

**Left-click** an overlay's row in Scene Objects for its properties, **right-click** it for its
context menu. Right-clicking the overlay itself in the 3-D view also opens its properties.

### Image Drape

Drape a georeferenced image over the terrain (it must overlap the grid):

```julia
G = gmtread("dem.nc")
I = gmtread("satellite.tif")
fig = viz(G; drape=I)         # the drape is given when the grid is opened
```

### Vertical Curtain

Hang a seismic profile vertically:

```julia
add_curtain!(fig, [x0 y0; x1 y1];   # Track, in the grid's XY
    image="seismic.png",            # GMTimage or image file
    zrange=(-4000, 0)               # Vertical extent, in grid z units
)
```

---

## Point Clouds

### Opening Points

```julia
D = gmtread("points.xyz")
fig = viz(D;
    color="blue",   # One colour (default: coloured by z through cmap)
    size=5          # Point size
)
```

### Selecting Points

1. **Shift+left-drag** (or **Ctrl+right-drag**) a box around points
2. The selected points are highlighted; dragging again over them toggles them; **Ctrl+Z** undoes

```julia
# Get selected rows
sel = selection(fig)
```

---

## GMTfv Solids

### Named Solids

```julia
# Platonic solids (keywords go to the GMT.jl generator: r = circumradius)
fig = viz("icosahedron"; r=2)

# Primitives
fig = viz("torus"; R=2, r=0.5)

# Operations
fig = viz("revolve"; curve=...)
fig = viz("loft"; C1=..., C2=...)
fig = viz("extrude"; shape=..., h=1)
```

### Polygon Meshes

```julia
fv = poly2fv(D)      # closed 3-D polygons (x y z), a GMTdataset or a vector of them
fig = viz(fv; color="orange")
```

**Toolbar:** **3-D Bodies** button (flyout menu) for quick solids.

### Body Placement

A solid built from the toolbar's **3-D Bodies** flyout opens in its own solids window.

---

## X,Y Plot Tool

### Opening the Plotter

```julia
# From data
t = 0:0.1:10
y = sin.(t) .* exp.(-t/5)
fig = plot([t y]; xlabel="Time (s)", ylabel="Amplitude")

# From file (multicol=true: every column after the first is a series)
D = gmtread("data.txt")
fig = plot(D; multicol=true)
```

`plot` takes GMT.jl's `plot` arguments and keywords (`legend`, `lc`, `lw`, `ls`, `marker`, `ms`,
`proj`, ...). As in GMT.jl, a matrix or dataset draws only column 2 against column 1 unless
`multicol=true` is given.

### Adding Series

```julia
plot!(fig, more_data)                               # a GMTdataset or matrix: col 1 = X
plot!(fig, x, y; legend="Series 2", ls=:dash)      # or one series by hand
```

`plot` called while an X,Y window is open adds a new page to that window instead of opening
another one.

### Multiple Pages

Click the **[+]** button beside the tabs to add a new page.

**Right-click tab:**
- New / Duplicate / Rename… / Copy to Page / Delete

### Time Series

```julia
# X is epoch seconds
fig = plot([epoch_seconds values]; xtime=true)     # or :date, :date_ymd, :time, :decyear, :doy
xtime!(fig, :decyear)                               # change it later
```

X-axis auto-formats as dates/times.

**Menu:** **Misc → Time axis (X)** — Linear (numbers), Date — auto, Date — yyyy-mm-dd, Time — HH:MM,
Decimal year, Day-of-Year.

### Log Axes

```julia
# Log X
fig = plot(data; proj=:logx)

# Log Y
fig = plot(data; proj=:logy)

# Both
fig = plot(data; proj=:loglog)

# Later, on a live plot
logscale!(fig; x=true, y=false)
```

Or use **Misc → Log X axis / Log Y axis**.

### Plot Types

A row of miniatures under the menus re-plots the current page in another way (MATLAB's PLOTS
gallery, plus GMT.jl's plotting functions):

- **Same page:** plot, area, bar, barh, bar stack, barh stack, waterfall, semilogx, semilogy, loglog,
  comet (animated), stem, stairs, scatter, plotyy, fill_between
- **New page:** histogram (of the selected series), density, boxplot, violin, ecdfplot, qqnorm,
  qqplot, radar, rose (selected series as azimuths), parallelplot, cornerplot, marginalhist (selected
  series), biplot

Buttons that do not fit are under the **»** at the right end of the row.

### Series Properties

Double-click a series name in the Object Manager for **Line properties…**:

- Colour
- Width
- Line style (Solid / Dashed / Dotted / Dash-Dot / Dash-Dot-Dot / None)
- Marker (None / Cross / Plus / Square / Circle / Diamond)
- Marker size
- Bar width (bar series)

Right-click the name for Line properties…, Show in Data Table, Save data…, Rename…, Delete.

### Analysis Menu

Transform your data:

| Menu Item | What It Does |
|----------|--------------|
| Remove Mean | Subtract mean from Y |
| Remove Trend | Remove linear trend |
| FFT — Amplitude Spectrum | Amplitude spectrum, via GMT spectrum1d (new page) |
| FFT — Power Spectrum Density | Power spectral density (new page) |
| Autocorrelation | Autocorrelation (new page) |
| 1st derivative | dY/dX (new page) |
| 2nd derivative | d²Y/dX² (new page) |
| Fit polynomial… | Fit a degree-N polynomial |
| Smoothing (Savitzky-Golay)… | Smooth (window size dialog) |
| Filter (Butterworth)… | Low/high-pass filter (cutoff dialog) |
| Filter outliers (despike)… | Despike (σ threshold dialog) |
| Histogram… | Histogram of Y (new page) |
| Bar graph | The series as bars (new page) |
| Sound Velocity Profile… | Chen & Millero sound velocity (new page) |
| Reference a curve to another… | Pick pairs of points, then stretch one curve onto the other |
| Depth to sources (Spector-Grant) | Depth-to-sources (drag a band on a spectrum) |

The results that are not on the parent's axes go to a **new page** of the same window; the others
are added to the current page.

**Usage:** Select a row in Object Manager first, then choose analysis.

The **GMT** menu has a full **spectrum1d…** dialog (auto- and cross-spectra).

### Saving

**Menu:**
- **File → Save…** — the selected series (or every series of the page if none is selected); the
  format follows the extension
- **File → Session → Open… / Save…** — the whole window (.igmtz)
- **File → Paste** (Ctrl+V) — the clipboard's numbers become new series

---

## Geography Menu

### Coastlines

Add GSHHG shorelines:

**Geography → Plot coastline →** Low / Intermediate / High / Full / Automatic resolution.

Political boundaries and rivers have their own submenus beside it.

### Volcanoes

Plot global volcano locations:

1. **Geography → Volcanoes**
2. Hover over points for metadata

### Tides

Download and plot tide gauge data:

1. **Geography → Tides (download)** plots the IOC gauges as stars
2. Right-click a star: **Download Mareg (2 days)** or **Download Mareg (Calendar)**

Data from IOC sea-level monitoring.

### More layers

The Geography menu also has administrative units (GADM), global seismicity, magnetic isochrons,
meteorite impacts, hydrothermal sites, tide stations, Earth tides, sun and terminators, plate
boundaries and cities.

### CRS (Coordinate Reference)

The Geography menu is enabled in a new window. The window sets its CRS from the data you load;
data without a CRS disables the menu.

---

## File Operations

### Opening Files

```julia
# Via code
fig = viz("my_file.nc")     # Auto-detects type

# Via menu
# File → Open known file types… (or the folder button on the toolbar)
```

**Supported:** GMT grids, images, datasets. **File → Open xy(z)** has the table readers.

### Drag and Drop

Drop a file onto the window — auto-opens.

**Also:** Drag files onto the `igmt` desktop launcher.

### Saving

**Grids:**
- **File → Save Grid…** — Save the window's grid; a dialog picks the format

**Images:**
- **File → Save Image…** — Save the window's image layer (greyed out when there is none)
- **File → Save Screenshot img… / Save Screenshot GeoTIFF…** — what the view shows
- **File → Copy to Clipboard**

**Per-object:**
- Right-click item in Scene Objects
- Choose **Save grid… / Save image… / Save data…** (whichever applies)

**Whole window:** **File → Save Session… / Load Session…**

### Recent Files

**File → Recent Files** — Quick-reopen (21 items, grouped by type).

### Background Region

Create an empty 2-D map with specific bounds:

1. **File → Background region…**
2. Enter W/E/S/N (default: the whole Earth)
3. **Is Geographic?** (checked by default) for lon/lat
4. Click OK

It opens a new window — a blank canvas for adding data.

---

## Tools Menu

### Tiles Tool

Download and view web map tiles (satellite, roads, terrain):

1. **Tools → Tiles Tool**
2. Picker opens with world map
3. **Click tiles** to select (toggle)
4. Choose **Provider** (Bing / Google / OSM / Esri)
5. Set **Zoom** (slider, or the ± buttons)
6. Click **GO** — a mosaic over the selected tiles' bounding box

**Features:**
- **Anchor** — Click the anchor button, then the map, to fix the zoom centre (drag it afterwards)
- **Pan** — Use scrollbars
- **Cache** — Where to store tiles (default `~/.gmt`, i.e. ~/.gmt/cache_tileserver; **…** to pick
  another, recent ones are remembered)
- **Mercator / Geogs** — Mercator or geographic output (Geogs by default)

Result opens as a new window.

### Project

Reproject the window's raster (grid or image) into another coordinate reference system — an
interface to `gdalwarp`:

1. **Tools → Project…** (needs a grid or an image in the window)
2. **Source Referencing System** — pre-filled from the window's own CRS (the same one the
   bottom-right EPSG chip shows).
3. **Destination Referencing System** — type a PROJ4 / WKT / `EPSG:nnnn` string, or pick from the
   **Projections** combo (which just fills this box, so it stays editable). Its last row,
   **More…**, and the **Select CRS…** button open the full PROJ system selector.
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

1. Click the **[Base Map]** toolbar button (the world icon, next to the Ruler)
2. The **World Topo Tiles** picker shows a 4×8 tile grid over ETOPO
3. Click a tile — or toggle the picker's rectangle tool and drag a region
4. Tile loads as georeferenced image

**Options:**
- **World Map Tiles** vs **World Map**
- **[-180 180]** vs **[0 360]** (Pacific-centered)

---

## 3-D Bodies Flyout

Quick-insert a geometric solid:

1. Click the **[3-D Bodies]** toolbar button's arrow (flyout menu)
2. Choose a solid (Cube, Sphere, Torus, Cylinder, the Platonic solids, …) — clicking the button
   itself builds the current one
3. The solid opens in its own solids window

**Operations:** **Revolve / Loft / Extrude** build a demo (a sample profile, curves, a star outline).
For your own input use `viz("revolve"; curve=…)`, `viz("loft"; C1=…, C2=…)`,
`viz("extrude"; shape=…, h=…)`.

---

## Swipe / Link Tool

One toolbar slot, two ways to compare two rasters — click the small **▾** arrow to choose
which.

**Toolbar:** the **[Swipe]/[Link]** toggle (immediately before the **[i]** Info button). Its
icon shows the currently selected mode. Picking a mode from the dropdown selects it **and turns it
on**; clicking the slot itself then turns the selected mode off and on.

### Swipe — compare two layers of THIS window

Splits two grids/images of the same window across a draggable vertical divider — the layer on
the left of the divider shows on the left, the layer on the right shows on the right. Greyed
out until the window holds at least two grids or images.

1. Load a second grid/image into the window (drag and drop, **File → Open known file types…**, or
   `add!`)
2. Pick **Swipe** from the dropdown — that turns it on
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

### Link — peek at the other of two layers, keeping the view

Pairs two grids/images of the same window — same pairing as Swipe — but instead of splitting
them across a divider, shows **one at a time**. **Holding the right button** shows the other one,
**without moving the camera at all**: whatever region and zoom you're looking at stays exactly
as it was, only the raster underneath changes. Releasing the button snaps back.

1. Pick **Link** from the dropdown — that turns it on
   - Exactly two rasters: paired automatically
   - More than two: a small dialog asks which layer to pair the currently displayed one with
   - Only one raster: it pairs with another open window whose extent overlaps; the right button
     then raises that window, framed on this window's view
2. **Hold the right button** anywhere in the window to see the other layer; release to come back
3. Click the slot again to turn Link off (restores whatever was visible before)

While Link is on, the right button belongs to it (no context menu).

The two layers only need to **overlap**, not match — a linked pair can be a grid and an image of
completely different resolution and footprint, as long as they describe some of the same
ground. No overlap at all is refused when you turn Link on. Deleting one of the paired layers
switches the tool off by itself.

---

## Scene Objects Panel

Every element in your scene appears here — grids, images, lines, points and point clouds, symbols,
polygons, text, curtains, beachballs, colour bars, axes, … — each row with its own icon.

**Checkbox:** Show/hide element (unchecking a group hides every element in it)

**Left-click a row's label:** its properties

**Right-click menu** (what applies to the element):
- **Line properties… / Text Properties… / Symb properties …** — Edit appearance
- **Show data table…** — View coordinates
- **Save grid… / Save image… / Save data…** — Export to file
- **Remove** — Delete the element

**Stack order** submenu (images, vectors):
- **Place on top / Place at bottom**
- **Bring forward / Send backward**

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

**Messages dock** (speech-bubble button in the bottom-right status corner):
Shows execution errors from menu actions (drop, geography, basemap, etc.).

---

## Common Tasks

### Create a shaded relief map with coastlines

```julia
using InteractiveGMT

# Load DEM
G = gmtread("dem.nc")

# View with hillshade
fig = viz(G; cmap="geo")    # vertical exaggeration: the gizmo's amber cone

# Turn on hillshade
# (In window: the Illumination (Hillshade) toolbar button)

# Add coastlines
coast = gmtread("coastline.gmt")
add!(fig, coast; mode=:lines, color="black", size=1)
```

### Plot a time series with spectrum

```julia
# Load tide data
D = gmtread("tide.txt")  # epoch_seconds, sea_level

# Plot
fig = plot(D; xlabel="Time", ylabel="Sea level (m)", xtime=true)

# Compute spectrum
# (In window: select series → Analysis → FFT — Power Spectrum Density)
```

### Drape satellite imagery over terrain

```julia
# Load DEM and image
G = gmtread("dem.nc")
I = gmtread("satellite.tif")

# View with drape
fig = viz(G; drape=I, cmap="gray")

# Adjust lighting
# (In window: the Illumination (Hillshade) toolbar button → Cast shadows)
```

### Compare two grids

```julia
G1 = gmtread("model.nc")
G2 = gmtread("observation.nc")

# View first
fig = viz(G1)

# Add second as a second layer
add!(fig, G2)

# In the window: pick Swipe from the Swipe/Link toolbar dropdown, then drag the divider
```

Or blend them instead of swiping:

```julia
# Add the second as a named layer and make it half transparent
add!(fig, G2; name="observation")
setopacity!(fig, 0.5; layer="observation")
```

---

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `1` – `6` | Snap to a preset view (`2` = top) |
| `R` | Reset camera |
| `e` | Toggle mesh edges |
| `x` | Toggle the gizmo |
| `c` | Recenter on the point under the cursor |
| `+` / `-` | Zoom |
| Arrow keys | Pan |
| `Ctrl+V` | Paste a table from the clipboard |
| `Ctrl+Z` | Undo the last point selection |
| Left-drag | Rotate (3-D) / pan (flat 2-D) |
| Ctrl+left-drag | Draw a profile |
| Middle-drag | Pan |
| Scroll | Zoom |
| Shift+left-drag, Ctrl+right-drag | Select points (point clouds) |
| Right-click | Context menu (Link's peek while Link is on) |

---

## Tips

- **Large grids:** Grids are drawn as tiles with level of detail, so big grids stay responsive.
- **Empty launcher:** Drag files onto it — promotes to real data in-place.
- **Multi-page X,Y:** Use tabs for related plots. They share the same window.
- **Time axes:** Set `xtime=true` for epoch seconds. Auto-formats.
- **Vector stacking:** All vectors (lines, symbols, polygons) share one draw order. Use the
  **Stack order** submenu.
- **Colour bar:** Drag it to move. Left-click its Scene Objects label to change colormap.
- **Gizmo:** Drag the amber cone for vertical exaggeration, the grey rings for tilt and azimuth.
- **Error messages:** Check the Messages dock (status-corner bubble), not the REPL.
- **Tool images:** Open via tool dialogs (Tiles Tool, Base Map) → ExtraObj, not primary surface.

---

## Getting Help

- **Help → InteractiveGMT Manual (online)** — this documentation
- **Help → About** — Version info
- **Help → Check for Updates…**

**Julia REPL:**

```julia
?viz           # Docstring
?plot          # Docstring
```

---

## File Format Support

**Via GMT.jl / gmtread:**
- **Grids:** .nc, .grd (GMT netCDF, GeoTIFF)
- **Images:** .tif, .jpg, .png (georeferenced)
- **Tables:** .txt, .dat, .xyz (text, GMT dataset)

**Via VTK (read natively, no GMT/GDAL involved):**
- **VTK XML:** .vti, .vtr, .vts, .vtp, .vtu (+ the .pvti/.pvtr/.pvts/.pvtp/.pvtu parallel forms)
- **VTK multiblock:** .vtm, .vtmb (a raster composite shows its first non-empty block)
- **VTK legacy:** .vtk (any dataset type)
- **VTKHDF:** .vtkhdf
- **3-D meshes:** .ply, .obj, .stl, .off, .byu, .gltf, .glb

A glTF file is a whole scene, so **every** mesh in it is displayed, merged into one layer — and its
Y-up geometry is rotated upright, because iGMT (like every VTK scene) is Z-up. BYU's other
extension `.g` is deliberately not claimed — it is BRL-CAD's too, which VTK cannot read.

A mesh keeps **its own colours** when the file carries them, per vertex (PLY red/green/blue, glTF
COLOR_0) or per face, and a file with none is shaded by height as before. Materials and UV textures
(glTF PBR, an OBJ's `.mtl`) are not read.

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

*Version 0.1.0*
*Windows, macOS and Linux*
