# magneticisochrons.jl — Geography > Magnetic isochrons > GPlates: seafloor-spreading magnetic
# anomaly isochron polylines, GPlates export format (data/Sutton_isocs.sqlite, one SpatiaLite file,
# read via GMT.jl's `gmtread` -> Vector{GMTdataset}, one GMTdataset per feature, `.attrib["name"]`
# carries the anomaly/basin label). Unlike Plate boundaries (7 files -> 7 overlays needing a shared
# group to fold under one row), this is a SINGLE gmtread + ONE batched
# gmtvtk_add_overlay_ex2_h call, so the single resulting Scene Objects row already IS the master
# handle (SACRED_LAW master-handle-per-file) -- no groupName needed.
#
# 2 pt line (same fixed-constant convention as Plate boundaries — [[default-line-thickness-shared-fallback]]:
# this is an explicit width, not the <=0 "fall back to Preferences" case). Context menu = the same
# as Plate boundaries' lines MINUS "Convert to points" (`noConvertToPoints=1`): scattering a
# spreading isochron to points makes no more sense than for a boundary line, but unlike a SHAPENC
# coverage boundary, Line length…/Azimuth… still apply. The source is a 2-column (lon,lat) GPlates
# export -- the z=0 pushed below is a stored-geometry filler only, so `zIsPlaceholder=1` too:
# "Show data table…" must show #/X/Y, never an invented Z column.

const _ISOC_GPLATES_FILE = "Sutton_isocs.sqlite"
const _ISOC_GPLATES_NAME = "Magnetic isochrons"

# Reads the whole file (whole-earth static dataset, like Plate boundaries — no view-region clip)
# and batches every feature's segment into ONE overlay, hover text = its OGR "name" attribute.
# Returns true if anything was added.
function _load_magnetic_isochrons_gplates(scene::Ptr{Cvoid})::Bool
	path = joinpath(_PKGROOT, "data", _ISOC_GPLATES_FILE)
	isfile(path) || (@warn "magnetic isochrons: data file not found" path; return false)
	D = GMT.gmtread(path)
	segs = D isa AbstractVector ? D : (D,)
	isempty(segs) && return false
	xyz = Float64[]; segoff = Cint[0]; infos = String[]; off = 0
	for seg in segs
		m = seg.data
		(m === nothing || isempty(m)) && continue
		n = size(m, 1)
		n < 2 && continue                              # a lone point is not a line
		for k in 1:n
			push!(xyz, Float64(m[k, 1]), Float64(m[k, 2]), 0.0)
		end
		off += n
		push!(segoff, Cint(off))
		push!(infos, get(seg.attrib, "name", _ISOC_GPLATES_NAME))
	end
	off == 0 && return false
	r, g, b = _ovl_color(:red, :lines)
	packed = join(infos, '\x1e')
	ok = ccall(_fn(:gmtvtk_add_overlay_ex2_h), Cint,
	      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble,
	       Cdouble, Cdouble, Cstring, Cstring, Cstring, Cint, Cint),
	      scene, xyz, Cint(off), segoff, Cint(length(segoff) - 1), Cint(1), r, g, b,
	      2.0, 0.0, _ISOC_GPLATES_NAME, "", packed, Cint(1), Cint(1))
	return ok != 0
end
