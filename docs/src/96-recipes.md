```@meta
CurrentModule = InteractiveGMT
```

# Recipes

!!! warning "Experimental"
    The recipe system is new and experimental. Its name, its signature and what it accepts may
    change in a future release without a deprecation period. Do not build on it yet anything
    you cannot easily update.

`iview` knows how to show the GMT types — `GMTgrid`, `GMTimage`, `GMTdataset` (or a vector of
them), `GMTfv` — plus a matrix and a file name. A **recipe** teaches it any other type, with
**one method** of one function:

```julia
InteractiveGMT.iview_recipe(x::MyType) = <a GMTgrid, GMTimage, GMTdataset, Vector{GMTdataset} or GMTfv>
```

The recipe only says *what your object is*, in GMT terms. Everything else — the window, the axes,
the colour bar, the Scene Objects rows, the tools — is what `iview` does for that GMT type anyway.
A recipe never draws anything itself.

Without a recipe, `iview(x)` on an unknown type stops with an error that shows the line to write.

## Who writes a recipe

Whoever owns the type:

- **a user**, in a script, for a struct of their own;
- **a package author**, in the package's own code, so that its users can call `iview` on its
  objects without writing anything.

InteractiveGMT itself ships no recipes.

## The three forms

**Data only.**

```julia
using InteractiveGMT, GMT

struct Survey
    lon::Vector{Float64}
    lat::Vector{Float64}
    depth::Matrix{Float32}
end

InteractiveGMT.iview_recipe(s::Survey) = mat2grid(s.depth, x=s.lon, y=s.lat)

iview(s)                     # opens exactly as iview(mat2grid(s.depth, x=s.lon, y=s.lat))
```

**Data and default keywords.** Return a tuple: the data and a `NamedTuple` of `iview` keywords.
The keywords the user passes to `iview` win over the recipe's.

```julia
InteractiveGMT.iview_recipe(s::Survey) = (mat2grid(s.depth, x=s.lon, y=s.lat), (; cmap=:batlow))

iview(s)                     # batlow
iview(s; cmap=:turbo)        # turbo: the caller's keyword wins
```

**Several layers in one window.** Return a `Vector`. The first element opens the window, each
following one is added to it with [`add!`](@ref) — one Scene Objects row each.

```julia
InteractiveGMT.iview_recipe(v::Vector{Survey}) = [mat2grid(s.depth, x=s.lon, y=s.lat) for s in v]

iview([survey2019, survey2023])
```

An element of the vector may itself be a `(data, keywords)` tuple, whose keywords go to its own
`add!`.

## Example: a Plots.jl/Makie recipe, written here

### Rasters.jl

Rasters.jl's Plots recipes (`src/plotrecipes.jl`) are about 300 lines. Its 2-D map alone sets the
title, the axis guides, the tick rotation, the frame style, the colour map, the limits and the
aspect ratio, then hands the coordinates and the values to a heatmap:

```julia
@recipe function f(::RasterPlot, A::AbstractRaster{T,2,<:Tuple{D1,D2}}) where {T,D1<:SpatialDim,D2<:SpatialDim}
    yguide, xguide = label(dims(A))
    y, x = map(_prepare_plots, dims(A))
    :title --> _maybename(A)
    :xguide --> xguide
    :xrotation --> -45
    :yguide --> yguide
    :framestyle --> :box
    :seriescolor --> :batlow
    # ... limits and aspect ratio from the dimensions ...
    :seriestype --> :heatmap
    lookup(x), lookup(y), parent(A)
end
```

…plus a dispatcher recipe, a 3-D one, and the `RasterStack`/`RasterSeries` ones that lay out a
grid of subplots. In iGMT:

```julia
InteractiveGMT.iview_recipe(A::Raster) = (mat2grid(A), (; cmap=:batlow, title=string(name(A))))
InteractiveGMT.iview_recipe(S::RasterStack) = [mat2grid(S[k]) for k in keys(S)]
```

GMT.jl's `mat2grid` already recognises a `Raster` (a 2-D grid or a 3-D cube) and carries its
coordinates over. The rest of the Plots recipe has no counterpart because the viewer already does
it: each grid gets its own axes, framed to its own extent; a stack becomes one window with one
Scene Objects row per layer instead of a grid of subplots.

### Shapefile.jl

Shapefile.jl's recipes (two package extensions) forward a `Shapefile.Table` to its shapes and enable
the GeoInterface plotting of each shape type, for Plots and for Makie separately. In iGMT, one method
turns the shapes into `GMTdataset`s:

```julia
using GeoInterface: coordinates, trait, MultiPolygonTrait, PolygonTrait

ring2mat(r) = permutedims(reduce(hcat, r))                 # [[x,y], ...] -> N×2

function InteractiveGMT.iview_recipe(t::Shapefile.Table)
    D = GMTdataset[]
    for g in Shapefile.shapes(t)
        g === missing && continue
        c = coordinates(g)
        rings = trait(g) isa MultiPolygonTrait ? reduce(vcat, c) :
                trait(g) isa PolygonTrait      ? c : [c]
        append!(D, [mat2ds(ring2mat(r)) for r in rings])
    end
    D
end
```

A shapefile *on disk* needs no recipe at all: `iview("file.shp")` reads it with GMT.jl. The recipe
is for a `Shapefile.Table` already in memory.

## Matrix-like types

A type that is an `AbstractMatrix` (a 2-D `Raster`, a `DimArray`, …) reaches its recipe as well:
`iview(M)` asks for one first. A matrix with no recipe is shown as a grid, through `mat2grid`.

## Reference

```@docs
iview_recipe
```
