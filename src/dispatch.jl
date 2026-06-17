# iview front-door dispatcher — single entry over the qtvtk viewers (mirrors GMTF3D iview,
# minus the f3d-only view_lines / view_image, which the self-contained viewer has no standalone
# window for) — plus the built-in synthetic demo.

const _POLY2FV_KW = (:cmap, :zscale, :vfrac, :vexag, :isgeog, :ncolor, :triangulate)
_merge_points(D) = (length(D) == 1) ? D[1] : GMT.mat2ds(reduce(vcat, (d.data for d in D)))

# WKB geometry code -> kind (strip 25D flag bit + fold Z/ZM thousands). 1/4=point, 2/5=line, 3/6=poly.
_geom_kind(g::Integer) = (c = (g & 0x7fffffff) % 1000;
	c in (1, 4) ? :points : c in (2, 5) ? :lines : c in (3, 6) ? :polys : :unknown)

# Resolve a dataset to :points / :lines / :polys (prefer stored geometry; else closure heuristic).
function _ds_kind(D::GMTdataset)
	k = _geom_kind(Int(D.geom))
	(k === :unknown) || return k
	(size(D.data, 1) >= 4 && @views D.data[1, 1:2] == D.data[end, 1:2]) ? :polys : :points
end

"""
	iview(x; kwargs...)

Front-door dispatcher over the Qt + VTK viewers. `x` may be:
- a `GMTgrid`           -> [`view_grid`](@ref)
- a `GMTimage`          -> [`view_image`](@ref) (bare image: flat plane, top-down map)
- a `GMTfv`             -> [`view_fv`](@ref) (faceted z-colour + matching colorbar; `color=:explicit` for baked colours)
- a `GMTdataset` / `Vector{GMTdataset}` -> points -> [`view_points`](@ref); polygons -> `view_fv(poly2fv(...))`
- a `String`            -> `"grid"`/`"peaks"` demo or a grid file -> `view_grid`; else a named
  solid from [`SOLIDS`](@ref) -> [`view_fv`](@ref)

With **no argument**, `iview()` opens an empty drag-and-drop launcher window: drop a grid /
image / table file (anything `GMT.gmtread` reads) onto it — or onto any open viewer window — to
open it in a new window.

E.g. `iview("torus")`, `iview(GMT.peaks())`, `iview(poly_dataset)`, `iview(torus(R=6))`, `iview()`.
Line geometry has no standalone window here — overlay it on a grid with [`add!`](@ref) instead.
"""
iview(G::GMTgrid; kwargs...) = view_grid(G; kwargs...)

iview(fv::GMTfv; kwargs...) = view_fv(fv; kwargs...)   # color=true -> faceted z + matching bar

iview(I::GMTimage; kwargs...) = view_image(I; kwargs...)   # bare image -> flat top-down map

function iview(D::GMTdataset; kwargs...)
	k = _ds_kind(D)
	(k === :points) && return view_points(D; kwargs...)
	(k === :lines)  && error("iview: standalone line viewing is not available in the Qt+VTK viewer; overlay lines on a grid with add!(fig, D)")
	pk = filter(p -> p.first in _POLY2FV_KW, kwargs)        # polys -> mesh
	vk = filter(p -> !(p.first in _POLY2FV_KW), kwargs)
	return view_fv(poly2fv([D]; pk...); vk...)
end

function iview(D::Vector{<:GMTdataset}; kwargs...)
	isempty(D) && error("empty GMTdataset vector")
	k = _ds_kind(D[1])
	(k === :points) && return view_points(_merge_points(D); kwargs...)
	(k === :lines)  && error("iview: standalone line viewing is not available in the Qt+VTK viewer; overlay lines on a grid with add!(fig, D)")
	pk = filter(p -> p.first in _POLY2FV_KW, kwargs)
	vk = filter(p -> !(p.first in _POLY2FV_KW), kwargs)
	return view_fv(poly2fv(D; pk...); vk...)
end

function iview(name::AbstractString; kwargs...)
	lname = lowercase(String(name))
	(lname in ("grid", "peaks")) && return view_grid(GMT.peaks(); kwargs...)
	# A real file -> read it (gmtread auto-detects format) and dispatch by the returned type
	# (grid -> view_grid, image -> view_image, dataset -> points/fv).
	isfile(name)                 && return iview(GMT.gmtread(name); kwargs...)
	return view_fv(name; kwargs...)        # named solid -> the SOLIDS dispatch above
end

"Show the built-in synthetic demo surface (no grid needed)."
function view_demo()
	ccall(_fn(:gmtvtk_view_demo), Cvoid, ())
	_start_pump()
	return nothing
end
