# plateboundaries.jl — Geography > Plate boundaries: 7 GMT/OGR plate-boundary-type SpatiaLite
# files (data/D{ccb,crb,ctf,ocb,osr,otf,sub}.sqlite). Each row is one polyline segment carrying OGR
# fields `vel` (mm/yr), `azim_vel` (velocity azimuth) and `plate_pair`. Read with GMT.jl's `gmtread`
# (OGR -> Vector{GMTdataset}, one GMTdataset per segment, `.attrib` carries the field values) — no
# new SQLite dependency. Each boundary type is added as ONE batched overlay (constant colour, 2 pt
# line, per-segment hover text, zIsPlaceholder=1 via gmtvtk_add_overlay_ex2_h), and all seven are tagged with the same
# Scene Objects group name so rebuildSceneObjects (50_scene.cpp) folds them under one collapsible
# "Plate boundaries PB" row whose checkbox cascades to every member.

const _PB_GROUP = "Plate boundaries PB"

# (sqlite basename, human type name shown in the hover block + Scene Objects row, line colour).
const _PB_TYPES = (
	("Dccb", "Continental Convergent Boundary", :magenta),
	("Dcrb", "Continental Rift Boundary",       :blue),
	("Dctf", "Continental Transform Fault",     :yellow),
	("Docb", "Oceanic Convergent Boundary",     :cyan),
	("Dosr", "Oceanic Spreading Ridge",         :red),
	("Dotf", "Oceanic Transform Fault",         :green),
	("Dsub", "Subduction Zone",                 :orange),
)

# Read one boundary-type file + add it as ONE batched overlay (all its segments in a single
# ccall). Returns true if anything was added.
function _pb_load_one(scene::Ptr{Cvoid}, file::AbstractString, typename::AbstractString, color)::Bool
	path = joinpath(_PKGROOT, "data", "$file.sqlite")
	isfile(path) || (@warn "plate boundaries: data file not found" path; return false)
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
		vel  = get(seg.attrib, "vel", "")
		azim = get(seg.attrib, "azim_vel", "")
		pair = get(seg.attrib, "plate_pair", "")
		push!(infos, string(typename, "\nVelocity: ", vel, " mm/yr\nAzimuth: ", azim, "°\nPlate pair: ", pair))
	end
	off == 0 && return false
	r, g, b = _ovl_color(color, :lines)
	packed = join(infos, '\x1e')
	# _ex2_h (not _ex_h): zIsPlaceholder=1 -- source is 2-col OGR (lon,lat), the z=0 pushed above is a
	# stored-geometry filler, not real data, so "Show data table..." must not invent a Z column
	# (see [[noconverttopoints-overlay-flag]]). noConvertToPoints=1 -- a plate-boundary line is not a
	# thing you'd scatter to points either.
	ok = ccall(_fn(:gmtvtk_add_overlay_ex2_h), Cint,
	      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble,
	       Cdouble, Cdouble, Cstring, Cstring, Cstring, Cint, Cint),
	      scene, xyz, Cint(off), segoff, Cint(length(segoff) - 1), Cint(1), r, g, b,
	      2.0, 0.0, typename, _PB_GROUP, packed, Cint(1), Cint(1))
	return ok != 0
end

# Loads all 7 boundary-type files into `scene`, grouped under _PB_GROUP. Returns true if at least
# one type was added (callers use this to decide whether to record a session recipe).
function _load_plate_boundaries(scene::Ptr{Cvoid})::Bool
	any_added = false
	for (file, typename, color) in _PB_TYPES
		_pb_load_one(scene, file, typename, color) && (any_added = true)
	end
	return any_added
end
