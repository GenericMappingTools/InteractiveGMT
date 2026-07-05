# iview front-door dispatcher — single entry over the qtvtk viewers (mirrors GMTF3D iview,
# minus the f3d-only view_lines / view_image, which the self-contained viewer has no standalone
# window for) — plus the built-in synthetic demo.

const _POLY2FV_KW = (:cmap, :zscale, :vfrac, :vexag, :isgeog, :ncolor, :triangulate)
const _XYPLOT_KW  = (:name, :color, :linewidth, :title, :xlabel, :ylabel, :xtime)
_merge_points(D) = (length(D) == 1) ? D[1] : GMT.mat2ds(reduce(vcat, (d.data for d in D)))

# A plain X,Y curve table: exactly 2 columns, NOT geographic, not a closed polygon. Such a dataset
# has no natural home on the 3-D map (a 2-col line even errors there), so the front door sends it to
# the standalone X,Y plot tool. 3-col / geographic data keeps the 3-D map path.
function _is_xy_table(D::GMTdataset)
	size(D.data, 2) == 2 || return false
	_isgeog(D) == 0      || return false       # _isgeog (drop.jl): 1 = geographic
	return _ds_kind(D) !== :polys
end
_xyplot_only(kwargs) = (; (p for p in kwargs if p.first in _XYPLOT_KW)...)

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
  solid from SOLIDS -> [`view_fv`](@ref)

With **no argument**, `iview()` opens an empty drag-and-drop launcher window: drop a grid /
image / table file (anything `GMT.gmtread` reads) onto it — or onto any open viewer window — to
open it in a new window.

E.g. `iview("torus")`, `iview(GMT.peaks())`, `iview(poly_dataset)`, `iview(torus(R=6))`, `iview()`.
Line geometry has no standalone window here — overlay it on a grid with [`add!`](@ref) instead.
"""
iview(G::GMTgrid; kwargs...) = view_grid(G; kwargs...)

iview(fv::GMTfv; kwargs...) = view_fv(fv; kwargs...)   # color=true -> faceted z + matching bar

iview(I::GMTimage; kwargs...) = view_image(I; kwargs...)   # bare image -> flat top-down map

# `xy` forces the routing: xy=true -> X,Y tool, xy=false -> the 3-D map path; the default (nothing)
# auto-routes a plain 2-col non-geographic table to the X,Y tool (see _is_xy_table).
function iview(D::GMTdataset; xy=nothing, kwargs...)
	(xy === true || (xy === nothing && _is_xy_table(D))) && return xyplot(D; _xyplot_only(kwargs)...)
	k = _ds_kind(D)
	(k === :points) && return view_points(D; kwargs...)
	(k === :lines)  && error("iview: standalone line viewing is not available in the Qt+VTK viewer; overlay lines on a grid with add!(fig, D), or open it in the X,Y plot tool with xyplot(D)")
	pk = filter(p -> p.first in _POLY2FV_KW, kwargs)        # polys -> mesh
	vk = filter(p -> !(p.first in _POLY2FV_KW), kwargs)
	return view_fv(poly2fv([D]; pk...); vk...)
end

function iview(D::Vector{<:GMTdataset}; xy=nothing, kwargs...)
	isempty(D) && error("empty GMTdataset vector")
	(xy === true || (xy === nothing && _is_xy_table(D[1]))) && return xyplot(D; _xyplot_only(kwargs)...)
	k = _ds_kind(D[1])
	(k === :points) && return view_points(_merge_points(D); kwargs...)
	(k === :lines)  && error("iview: standalone line viewing is not available in the Qt+VTK viewer; overlay lines on a grid with add!(fig, D), or open it in the X,Y plot tool with xyplot(D)")
	pk = filter(p -> p.first in _POLY2FV_KW, kwargs)
	vk = filter(p -> !(p.first in _POLY2FV_KW), kwargs)
	return view_fv(poly2fv(D; pk...); vk...)
end

function iview(name::AbstractString; kwargs...)
	lname = lowercase(String(name))
	(lname in ("grid", "peaks")) && return view_grid(GMT.peaks(); kwargs...)
	# A real file -> read it (gmtread auto-detects format), record it in the Recent Files list,
	# and dispatch by the returned type (grid -> view_grid, image -> view_image, dataset -> points/fv).
	if isfile(name)
		# Never open the same file twice: if it is already shown in a live window, raise that
		# window and silently ignore this (and every later) request for the same path.
		_open_window_for(name) != C_NULL && return nothing
		data = GMT.gmtread(name)
		_record_recent(name, data)
		# Titlebar shows which file is loaded, unless the caller already asked for a specific title.
		kw = merge((; title="i'GMT -- $(basename(name))"), NamedTuple(kwargs))
		fig = iview(data; kw...)
		# The open-once dedup is keyed on the 3-D Scene* alive/raise C API; an X,Y plot window uses a
		# different handle type, so don't register it there (a repeat open just makes a 2nd window).
		fig isa QtXYPlot || _mark_file_open(name, _fig_handle(fig))
		return fig
	end
	return view_fv(name; kwargs...)        # named solid -> the SOLIDS dispatch above
end

# Files currently shown in a live viewer window: abspath => the window's C handle. Used to refuse
# opening the same file twice (raise the existing window instead). Entries are pruned when their
# window dies, so a file can be reopened after its window is closed.
const _OPEN_FILES = Dict{String,Ptr{Cvoid}}()

_filekey(path::AbstractString) = try abspath(String(path)) catch; String(path) end

# If `path` is already shown in a live window, raise that window and return its handle; else C_NULL
# (pruning a stale entry whose window has since closed).
function _open_window_for(path::AbstractString)::Ptr{Cvoid}
	key = _filekey(path)
	h = get(_OPEN_FILES, key, C_NULL)
	h === C_NULL && return C_NULL
	if ccall(_fn(:gmtvtk_is_alive), Cint, (Ptr{Cvoid},), h) != 0
		ccall(_fn(:gmtvtk_raise), Cvoid, (Ptr{Cvoid},), h)
		return h
	end
	delete!(_OPEN_FILES, key)
	return C_NULL
end

# Remember that `path` is now shown in window `handle` (so a repeat open is ignored).
_mark_file_open(path::AbstractString, handle::Ptr{Cvoid}) = (_OPEN_FILES[_filekey(path)] = handle; nothing)

# Push a just-opened file onto the viewer's persistent Recent Files list (File > Recent Files),
# tagged by category (0 = grid, 1 = image, 2 = dataset/fv) so the menu can group it. Best-effort:
# a missing DLL symbol or bad path never blocks the open.
function _record_recent(path::AbstractString, data)
	cat = data isa GMTgrid  ? 0 :
	      data isa GMTimage ? 1 :
	      (data isa GMTdataset || data isa AbstractVector{<:GMTdataset} || data isa GMTfv) ? 2 : -1
	cat < 0 && return nothing
	try
		ccall(_fn(:gmtvtk_add_recent), Cvoid, (Cstring, Cint), abspath(String(path)), cat)
	catch
	end
	return nothing
end

"Show the built-in synthetic demo surface (no grid needed)."
function view_demo()
	ccall(_fn(:gmtvtk_view_demo), Cvoid, ())
	_start_pump()
	return nothing
end
