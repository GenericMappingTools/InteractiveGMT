```@meta
CurrentModule = InteractiveGMT
```

# Point Clouds

## Opening Points

```julia
using InteractiveGMT, GMT

# Create random points
D = GMT.mat2ds(rand(100,3))

# View
fig = iview(D; color="red", size=5)

# From file
D = gmtread("points.xyz")
fig = iview(D)
```

### Options

| Option | Description |
|--------|-------------|
| `color` | Colour name or RGB |
| `size` | Point size (pixels) |
| `name` | Name in Scene Objects |
| `mode` | `:points` (default) or `:symbols` |

## Selecting Points

Use **Ctrl + right-drag** to draw a selection box.

```julia
# After Ctrl+right-drag selection
sel = selection(fig)  # Returns selected rows
```

## Data Viewer

Right-click any overlay in Scene Objects → **Show data table** to view coordinates.

```julia
show_table(fig, D)
```
