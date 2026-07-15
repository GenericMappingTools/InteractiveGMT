```@meta
CurrentModule = InteractiveGMT
```

# Solids and Meshes

## Named Solids

```julia
# Platonic solids
fig = iview("icosahedron"; size=2)
fig = iview("dodecahedron"; size=2)

# Primitives
fig = iview("cube"; size=3)
fig = iview("sphere"; radius=2)
fig = iview("torus"; major=2, minor=0.5)
fig = iview("cylinder"; radius=1, height=3)
fig = iview("cone"; radius=1, height=2)
```

## Operations

```julia
# Revolve
fig = iview("revolve"; profile=[0 0; 1 1; 0.5 2; 0 0])

# Loft
fig = iview("loft"; sections=[[0 0 0; 1 0 0], [0 1 1; 1 1 1]])

# Extrude
fig = iview("extrude"; face=[0 0; 1 0; 1 1; 0 1], height=1)
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
