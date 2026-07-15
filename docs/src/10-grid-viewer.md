```@meta
CurrentModule = InteractiveGMT
```

# Grid Viewer

## Opening Grids

View a GMT grid surface:

```julia
using InteractiveGMT, GMT

G = GMT.peaks()
fig = iview(G)
```

### Options

| Option | Description |
|--------|-------------|
| `cmap` | Colormap name (e.g., "relief", "geo", "etc") |
| `ve` | Vertical exaggeration (default: 1) |
| `drape` | Image to drape over surface |
| `name` | Name in Scene Objects |
| `shading` | Enable PBR lighting (default: true) |
| `edges` | Show mesh edges (default: false) |

### Examples

```julia
# With colormap
fig = iview(G; cmap="relief")

# With vertical exaggeration
fig = iview(G; ve=5)

# With image drape
I = gmtread("satellite.tif")
fig = iview(G; drape=I)
```

## Navigation

| Mouse | Action |
|-------|--------|
| Left-drag | Rotate view |
| Middle-drag | Pan |
| Scroll wheel | Zoom |
| Right-click | Context menu |

**Keyboard:** `e` toggles mesh edges.

## Colormaps

Available named colormaps:
`relief`, `geo`, `geo2`, `dem`, `mbio`, `world`, `Byrne`, `circular`, `polar`, `etc`, `jet`, `no_green`

```julia
fig = iview(G; cmap="my_colors.cpt")  # Custom CPT file
```

**Right-click "Color Bar"** in Scene Objects to change colormap interactively.

## Hillshade

Illuminate relief like a shaded map:

**Menu:** View → Hillshade Lambert or Hillshade grdimage

## Vertical Exaggeration

```julia
fig = iview(G; ve=5)  # 5x vertical exaggeration
```

**Interactively:** Drag the blue ring on the gizmo, or use **View → Vertical Scale…**

## Adding Overlays

Add line or point overlays to a live grid:

### Line Overlays

```julia
# Add coastline
coast = gmtread("coastline.gmt")
add!(fig, coast; mode=:lines, color="black", width=1.5)
```

### Point Overlays

```julia
# Add stations
stations = gmtread("stations.txt")
add!(fig, stations; mode=:points, color="red", size=10)
```

## Image Drape

Drape a georeferenced image over the terrain:

```julia
# View grid with drape
fig = iview(G; drape=I)

# Or add to existing window
add!(fig, I; drape=true)
```

## Curtains

Hang a vertical seismic profile:

```julia
add_curtain!(fig, "seismic.nc"; x=0, rotation=45)
```

## Saving

```julia
save_png(fig, "output.png")
```

Or **File → Save Grid / Save Image** menu.
