```@meta
CurrentModule = InteractiveGMT
```

# API Reference

## Front-Door Dispatcher

```@docs
viz
```

## Overlays

```@docs
add!
scatter!
orbits!
```

## Curtains

```@docs
add_curtain!
```

## Iso-surfaces

```@docs
add_isosurface!
```

## Selection

```@docs
selection
```

## Solids

```@docs
poly2fv
```

## X,Y Plot

```@docs
plot
plot!
bar!
clear!
```

## Time & Log Axes

```@docs
xtime!
logscale!
```

## Stick Diagrams

```@docs
feather
```

## Movies

`movie` renders a live window frame by frame and encodes the result — the same call the
**Tools → Make movie** dialog makes. `orbit!`, `set_layer!` and `replace_grid!` are the mutations a
frame callback applies; `add_label!` / `add_progress!` are the GMT `-L` / `-P` annotations.

```@docs
movie
orbit!
set_layer!
replace_grid!
nlayers
add_label!
add_progress!
remove_annotation!
movie_annotations
```

## Utilities

```@docs
isalive
save_png
wait_windows
stereo!
```

## Data Viewing

```@docs
show_table
```

## CUBE Gridding

See [CUBE — Combined Uncertainty and Bathymetry Estimator](76-cube.md).

```@docs
cubegrid
cubegrid_all
```

## Types

### QtFigure

Handle for a 3-D grid/image window.

```julia
fig = viz(G)  # Returns QtFigure
```

### QtPoints

Handle for a point cloud window.

```julia
fig = viz(D)  # Returns QtPoints
```

### QtFV

Handle for a solid/mesh window.

```julia
fig = viz("torus")  # Returns QtFV
```

### QtImage

Handle for an image window.

### QtEmpty

Handle for an empty launcher window.

### QtXYPlot

Handle for an X,Y plot window.

```julia
fig = plot([x y])  # Returns QtXYPlot
```

## Index

```@index
```

## Internal

```@autodocs
Modules = [InteractiveGMT]
Filter = function(x)
    try
        n = string(nameof(x))
        return n != "InteractiveGMT" && !startswith(n, "_")
    catch
        true
    end
end
```
