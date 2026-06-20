# crs.jl — the single place InteractiveGMT deals with coordinate reference systems (CRS).
#
# A figure's georeferencing is kept in ALL THREE interchangeable forms — PROJ4 string, WKT and
# (when derivable) EPSG code — so the viewer and any later GMT call can grab whichever it needs.
# `crs_from` pulls whatever a GMT object already carries and fills the missing forms with GMT.jl's
# proj/wkt/epsg converters (best-effort — not every direction is implemented yet). The resolved CRS
# is pushed down to the C viewer with `_apply_crs!`, which also drives the Geography menu: an empty
# (unreferenced) CRS keeps it hidden, since placing GSHHG features needs a reference frame.

"""
	CRS(proj4, wkt, epsg)

A coordinate reference system in its three interchangeable forms. An empty `proj4` and `wkt`
with `epsg == 0` means UNREFERENCED data. Build one from a GMT object with [`crs_from`](@ref).
"""
struct CRS
	proj4::String
	wkt::String
	epsg::Int
end

"The empty (unreferenced) CRS."
const NO_CRS = CRS("", "", 0)

"True if this CRS carries any referencing at all."
hascrs(c::CRS) = !isempty(c.proj4) || !isempty(c.wkt) || c.epsg != 0

# Read a string / integer property off a GMT object if it has a usable one (else "" / 0).
_prop_str(O, s::Symbol) = (hasproperty(O, s) && getfield(O, s) isa AbstractString) ? String(getfield(O, s)) : ""
_prop_int(O, s::Symbol) = (hasproperty(O, s) && getfield(O, s) isa Integer)        ? Int(getfield(O, s))    : 0

# Run a GMT converter that may throw or return junk; coerce to String / Int, "" / 0 on any failure.
_try_str(f)::String = try (r = f(); r isa AbstractString ? String(r) : "") catch; "" end
_try_int(f)::Int    = try (r = f(); r isa Integer ? Int(r) : 0)            catch; 0  end

"""
	crs_from(O; geographic=false) -> CRS

Build the [`CRS`](@ref) for a GMT object (`GMTgrid`, `GMTimage`, `GMTdataset`, `GMTfv`, …): read
whatever PROJ4 / WKT / EPSG it carries, then derive the missing forms with GMT.jl's converters,
using PROJ4 as the pivot. `geographic=true` resolves a plain lon/lat object with no explicit
projection to WGS84 (EPSG 4326). Returns [`NO_CRS`](@ref) for unreferenced data.
"""
function crs_from(O; geographic::Bool=false)
	p = _prop_str(O, :proj4)
	w = _prop_str(O, :wkt)
	e = _prop_int(O, :epsg)
	if isempty(p) && isempty(w) && e == 0          # nothing explicit on the object
		geographic || return NO_CRS                # ...and not geographic -> unreferenced
		return CRS("+proj=longlat +datum=WGS84 +no_defs", "", 4326)   # plain lon/lat == WGS84
	end
	# Establish PROJ4 as the pivot, then derive the other two forms from it.
	if isempty(p)
		!isempty(w)            && (p = _try_str(() -> GMT.wkt2proj(w)))
		isempty(p) && e != 0   && (p = _try_str(() -> GMT.epsg2proj(e)))
	end
	isempty(w) && !isempty(p)  && (w = _try_str(() -> GMT.proj2wkt(p)))
	isempty(w) && e != 0       && (w = _try_str(() -> GMT.epsg2wkt(e)))
	e == 0     && !isempty(p)  && (e = _try_int(() -> GMT.proj2epsg(p)))
	return CRS(p, w, e)
end

"""
	_apply_crs!(fig, crs::CRS) -> fig

Push a resolved [`CRS`](@ref) down to the C viewer window behind `fig`. The viewer stores all
three forms and shows / hides its Geography menu depending on whether the CRS is referenced.
"""
function _apply_crs!(fig, crs::CRS)
	h = _fig_handle(fig)
	h == C_NULL && return fig
	ccall(_fn(:gmtvtk_set_crs), Cvoid,
		  (Ptr{Cvoid}, Cstring, Cstring, Cint),
		  h, crs.proj4, crs.wkt, Cint(crs.epsg))
	return fig
end
