```@meta
CurrentModule = InteractiveGMT
```

# API Reference

## Front-Door Dispatcher

```@docs
iview
```

## Grid Viewer

```@docs
view_grid
view_image
```

## Overlays

```@docs
add!
```

## Curtains

```@docs
add_curtain!
```

## Point Clouds

```@docs
view_points
```

## Selection

```@docs
selection
```

## Solids

```@docs
view_fv
poly2fv
```

## X,Y Plot

```@docs
xyplot
clear!
```

## Time & Log Axes

```@docs
xtime!
logscale!
```

## Stick Diagrams

```@docs
stickplot
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

## Types

### QtFigure

Handle for a 3-D grid/image window.

```julia
fig = view_grid(G)  # Returns QtFigure
```

### QtPoints

Handle for a point cloud window.

```julia
fig = view_points(D)  # Returns QtPoints
```

### QtFV

Handle for a solid/mesh window.

```julia
fig = view_fv("torus")  # Returns QtFV
```

### QtImage

Handle for an image window.

### QtEmpty

Handle for an empty launcher window.

### QtXYPlot

Handle for an X,Y plot window.

```julia
fig = xyplot([x y])  # Returns QtXYPlot
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
