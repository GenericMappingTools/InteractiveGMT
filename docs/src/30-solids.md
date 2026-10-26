```@meta
CurrentModule = InteractiveGMT
```

# Solids and Meshes

## Named Solids

```julia
# Platonic solids
fig = viz("icosahedron"; r=2)
fig = viz("dodecahedron"; r=2)

# Primitives
fig = viz("cube"; r=3)
fig = viz("sphere"; r=2)
fig = viz("torus"; R=2, r=0.5)      # R = ring radius, r = tube radius
fig = viz("cylinder"; r=1, h=3)
```

## Operations

```julia
# Revolve: an M×3 profile (x, 0, z) spun around the z axis
fig = viz("revolve"; curve=[1.0 0 0; 1.5 0 1; 1 0 2])

# Loft: a surface skinned between two closed M×3 curves
fig = viz("loft"; C1=[0 0 0; 1 0 0; 1 1 0; 0 1 0], C2=[0 0 1; 1 0 1; 1 1 1; 0 1 1])

# Extrude: an M×2 outline pushed up by h
fig = viz("extrude"; shape=[0.0 0; 1 0; 1 1; 0 1], h=1.0)
```

## Polygon Meshes

Convert polygon to solid:

```julia
# Convert polygon to solid
D = gmtread("polygon.xyz")
fv = poly2fv(D)
fig = viz(fv; color="orange")
```

## Toolbar

Use the **3-D Bodies** toolbar flyout for quick solid insertion.
