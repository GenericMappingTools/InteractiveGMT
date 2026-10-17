```@meta
CurrentModule = InteractiveGMT
```

# Solids and Meshes

## Named Solids

```julia
# Platonic solids
fig = iview("icosahedron"; r=2)
fig = iview("dodecahedron"; r=2)

# Primitives
fig = iview("cube"; r=3)
fig = iview("sphere"; r=2)
fig = iview("torus"; R=2, r=0.5)      # R = ring radius, r = tube radius
fig = iview("cylinder"; r=1, h=3)
```

## Operations

```julia
# Revolve: an M×3 profile (x, 0, z) spun around the z axis
fig = iview("revolve"; curve=[1.0 0 0; 1.5 0 1; 1 0 2])

# Loft: a surface skinned between two closed M×3 curves
fig = iview("loft"; C1=[0 0 0; 1 0 0; 1 1 0; 0 1 0], C2=[0 0 1; 1 0 1; 1 1 1; 0 1 1])

# Extrude: an M×2 outline pushed up by h
fig = iview("extrude"; shape=[0.0 0; 1 0; 1 1; 0 1], h=1.0)
```

## Polygon Meshes

Convert polygon to solid:

```julia
# Convert polygon to solid
D = gmtread("polygon.xyz")
fv = poly2fv(D)
fig = iview(fv; color="orange")
```

## Toolbar

Use the **3-D Bodies** toolbar flyout for quick solid insertion.
