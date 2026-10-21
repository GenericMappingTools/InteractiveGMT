# Recipes for other packages' types (see recipes.jl). Each recipe here is a plain function that takes
# the package's module `P` as its first argument, so this file never needs the package loaded.
# `_PKG_RECIPES` maps a package name to its recipe; the package an object comes from is the root module
# of its type, so `iview(x)` finds the recipe the moment the user has done `using <Package>`.
# A recipe returns `nothing` for a type of its package it does not show.
#
# A new package: write its recipe function below and add one entry to `_PKG_RECIPES`.

_pkg_recipe(x) = get(_PKG_RECIPES, nameof(Base.moduleroot(parentmodule(typeof(x)))), nothing)

# ---------------------------------------------------------------------------------------------------
# Rasters.jl: GMT.mat2grid already reads a Raster (2-D grid or 3-D cube), coordinates and CRS included.
_recipe_rasters(P::Module, A) = GMT.mat2grid(A)

# A RasterStack: one window, one layer per raster.
_recipe_rasters_stack(P::Module, S) = [GMT.mat2grid(S[k]) for k in keys(S)]

_recipe_rasters_any(P::Module, x) = x isa P.AbstractRasterStack ? _recipe_rasters_stack(P, x) :
                                    x isa P.AbstractRaster      ? _recipe_rasters(P, x) : nothing

# ---------------------------------------------------------------------------------------------------
# Meshes.jl: a solid (Box, Ball, Cylinder, ...) is shown by its surface; a surface (Sphere, a 2-D Box,
# ...) and a mesh as they are. Units (Meshes stores metres) are stripped; a 2-D mesh lies at z = 0.
_recipe_meshes(P::Module, g) = _mesh2fv(P, P.paramdim(g) == 3 ? P.discretize(P.boundary(g)) : P.discretize(g))

_recipe_meshes_any(P::Module, x) = x isa P.Mesh     ? _mesh2fv(P, x) :
                                   x isa P.Geometry ? _recipe_meshes(P, x) : nothing

function _mesh2fv(P::Module, m)
	V = reduce(vcat, [Float64.(P.ustrip.(P.to(v)))' for v in P.vertices(m)])
	(size(V, 2) == 2) && (V = [V zeros(size(V, 1))])
	faces = P.paramdim(m) == 3 ? _mesh_outer_faces(P, m) : [collect(P.indices(c)) for c in P.elements(P.topology(m))]
	F = [reduce(vcat, [f' for f in faces if length(f) == n]) for n in unique(length.(faces))]  # one matrix per face size
	return GMT.fv2fv(F, V)
end

# A volume mesh (hexahedra, tetrahedra, a 3-D CartesianGrid): its surface is every cell face that no
# other cell shares. Each cell's own boundary gives its faces, outward; vertices map back to the mesh's.
function _mesh_outer_faces(P::Module, m)
	id = Dict(v => i for (i, v) in enumerate(P.vertices(m)))
	face, count = Dict{Vector{Int},Vector{Int}}(), Dict{Vector{Int},Int}()
	for e in P.elements(m)
		b = P.boundary(e)
		bm = b isa P.Mesh ? b : P.discretize(b)
		bv = P.vertices(bm)
		for c in P.elements(P.topology(bm))
			f = [id[bv[i]] for i in P.indices(c)]
			k = sort(f)
			face[k] = f;  count[k] = get(count, k, 0) + 1
		end
	end
	return [face[k] for (k, n) in count if n == 1]
end

# ---------------------------------------------------------------------------------------------------
const _PKG_RECIPES = Dict{Symbol,Function}(:Rasters => _recipe_rasters_any, :Meshes => _recipe_meshes_any)
