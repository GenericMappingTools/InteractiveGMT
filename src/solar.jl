# solar.jl — Geography > "Sun and terminators": the day/night terminator, the three twilights, the
# night side they enclose, and the sun's own position, through GMT.jl's own `solar` (pssolar). No
# astronomy lives here — the module does all of it; this file turns the dialog's key=value block into
# `solar` keywords, draws what comes back on the window and writes the sun report the dialog shows.
#
# The C++ dialog is SolarDialog (70_window.cpp, loads deps/ui/solar_dialog.ui).
#
# WHY THE PICTURE IS NEVER ASKED FOR. `solar` can emit PostScript (pssolar) or the terminator itself
# as data (-M). Only the DATA is of any use here: a picture cannot be recoloured, restyled, toggled
# or removed, and this window's whole contract is that everything on it is an object the user owns.
# So the module is always asked with -M, and the two things pssolar's plot options would have done
# are done HERE, on real Scene Objects elements:
#
#   the pen (-W)  -> a line overlay per terminator, with the usual right-click Line properties
#   the fill (-G) -> a filled polygon per painted terminator, with fill colour AND transparency,
#                    built through the SAME drawn-polygon constructor the Draw tool and line2patch
#                    use (`gmtvtk_add_poly_full`), so it arrives with the polygon properties dialog
#                    (fill, opacity, area) already on it — never a bespoke painted actor.
#
# A terminator RING and the region it bounds are two different geometries, exactly as they are in
# pssolar: the ring is an arc drawn across the map, the region is a closed area that has to be shut
# along the map's edge and around whichever pole is in darkness. `_solar_split_seam` makes the first,
# `_solar_night_polys` the second, and both start from the one ring the module handed over.

# The four terminators in the order the module lists them: the -T letter, the Scene Objects name of
# their line layer, the line colour, and the DEFAULT paint colour of the region beyond them. Line and
# paint colours are STARTING points only — every element made here is a normal Scene Objects object,
# recoloured per-object afterwards through its own properties.
const _SOLAR_TERMS = (("d", "Day/night terminator",  (0.00, 0.00, 0.00), (0.10, 0.13, 0.22)),
                      ("c", "Civil twilight",        (0.15, 0.30, 0.85), (0.16, 0.20, 0.34)),
                      ("n", "Nautical twilight",     (0.55, 0.15, 0.75), (0.22, 0.26, 0.44)),
                      ("a", "Astronomical twilight", (0.75, 0.10, 0.10), (0.28, 0.33, 0.54)))

# One Compute makes several elements, so they are tagged into Scene Objects GROUPS instead of being
# left as loose rows (SACRED_LAW: nothing goes on screen anonymously, and one action = one group).
# Two tags, not one, because the tree builds overlay rows and polygon rows in separate passes: the
# lines fold under the first, the painted regions under the second.
const _SOLAR_GROUP      = "Sun & terminators"
const _SOLAR_FILL_GROUP = "Sun & terminators — night"
const _SOLAR_SUN_LAYER  = "Sub-solar point"

# The +d<date>[+z<TZ>] tail shared by -T and -I. Empty date = "now", which is the module's own
# default, so nothing is passed and the module stays in charge of what "now" means.
function _solar_mods(d::Dict{String,String})::String
	s = ""
	date = _get(d, "date");  isempty(date) || (s *= "+d" * date)
	tz = _get(d, "tz")
	if !isempty(tz)
		occursin(r"^[+-]?\d{1,2}(:\d{2})?$", tz) ||
			error("the time zone must be an offset from UTC, like -03:00, -03 or 02:00 (got '$tz')")
		s *= "+z" * tz
	end
	return s
end

# lon/lat for -I, when the dialog was given one. Both boxes or neither: half a position is a typo,
# not a request, and GMT would silently report the times for (0,0) instead.
function _solar_pos(d::Dict{String,String})::String
	lon, lat = _get(d, "lon"), _get(d, "lat")
	(isempty(lon) && isempty(lat)) && return ""
	(isempty(lon) || isempty(lat)) && error("give BOTH longitude and latitude, or neither")
	(tryparse(Float64, lon) === nothing || tryparse(Float64, lat) === nothing) &&
		error("longitude/latitude must be numbers (got '$lon' / '$lat')")
	return lon * "/" * lat
end

# "r/g/b" (0-255 each, the dialog's colour button) -> the 0-1 triple every add takes. Falls back to
# `dflt` for an empty or unparseable value, so a missing key is never a failure.
function _solar_rgb(s::AbstractString, dflt::NTuple{3,Float64})::NTuple{3,Float64}
	p = split(strip(String(s)), '/')
	length(p) == 3 || return dflt
	v = tryparse.(Float64, p)
	any(x -> x === nothing, v) && return dflt
	return (clamp(v[1]::Float64 / 255, 0, 1), clamp(v[2]::Float64 / 255, 0, 1), clamp(v[3]::Float64 / 255, 0, 1))
end

# A terminator is a small circle in ABSOLUTE longitude and a window may be drawn in any 360-wide
# frame, so the ring is normalized into the map's own [W, W+360] frame. Where two consecutive points
# jump the seam the ring is CUT — and the exact crossing point is INSERTED on both sides, so a piece
# really reaches the edge of the map instead of stopping one sample short of it (which is also what
# lets `_solar_night_polys` close a painted region along that edge). Without the cut the ring draws
# as one horizontal streak straight across the map whenever the sun is near the seam.
# Returns a vector of matrices, the multi-segment form `_add_geo_overlay` already takes.
function _solar_split_seam(D, W::Float64)
	segs = D isa GMTdataset ? (D,) : collect(D)
	out = Matrix{Float64}[]
	for seg in segs
		m = seg isa GMTdataset ? seg.data : seg
		size(m, 1) < 2 && continue
		piece = Vector{Float64}[]
		prevlon = NaN;  prevlat = NaN
		for k in axes(m, 1)
			lon = mod(Float64(m[k, 1]) - W, 360.0) + W
			lat = Float64(m[k, 2])
			# Two consecutive NORMALIZED longitudes more than half a world apart can only be
			# neighbours across the frame's edge.
			if !isnan(prevlon) && abs(lon - prevlon) > 180.0
				# Jumped the seam. Split the crossing segment at the meridian itself: walk it in the
				# UNWRAPPED direction, so the fraction is the real one, and give both pieces that point.
				east = lon < prevlon                      # left-to-right jump = crossed the eastern edge
				edge = east ? W + 360.0 : W
				far  = east ? lon + 360.0 : lon - 360.0   # the same point, unwrapped past the edge
				t    = (edge - prevlon) / (far - prevlon)
				latx = prevlat + t * (lat - prevlat)
				push!(piece, [edge, latx])
				length(piece) >= 2 && push!(out, permutedims(reduce(hcat, piece)))
				piece = Vector{Float64}[[east ? W : W + 360.0, latx]]
			end
			push!(piece, [lon, lat])
			prevlon = lon;  prevlat = lat
		end
		length(piece) >= 2 && push!(out, permutedims(reduce(hcat, piece)))
	end
	return out
end

# Does the ring go all the way ROUND the world, or is it a closed loop that does not? Both shapes
# come back from the module at different instants — the day/night ring wraps whenever both poles are
# not in the same state, while a deep-twilight ring can be a cap that never reaches a pole — and the
# region they bound has to be closed differently. Summing each step's SHORTEST longitude change gives
# ±360 for a ring that wraps and ~0 for one that does not.
function _solar_wraps(m::AbstractMatrix)::Bool
	size(m, 1) < 3 && return false
	tot = 0.0
	for k in 1:size(m, 1)-1
		tot += rem(Float64(m[k+1, 1]) - Float64(m[k, 1]), 360.0, RoundNearest)
	end
	return abs(tot) > 180.0
end

# THE night side, as closed polygons in the map's own frame — what pssolar's -G paints.
#
# The dark region is the spherical cap on the far side of the terminator, so it always contains the
# ANTI-solar point; which of the two shapes it takes on a flat map follows from the ring:
#
#   ring wraps the world  -> the cap swallows a pole. The region is everything between the ring and
#                            that pole, so the polygon is the ring (sorted west to east, its two ends
#                            already sitting exactly on the frame edges after the seam cut) closed
#                            down the eastern edge, along the pole, and back up the western edge.
#                            WHICH pole comes from the sun's own latitude: the sun in the south
#                            leaves the north pole dark, and the other way round.
#   ring does not wrap    -> the cap is a closed blob and the ring already bounds it; each seam piece
#                            is simply closed on itself (its ends lie on the frame edge, so the
#                            closing segment runs along that edge).
function _solar_night_polys(m::AbstractMatrix, W::Float64, sunlat::Float64)::Vector{Matrix{Float64}}
	pieces = _solar_split_seam([m], W)
	isempty(pieces) && return Matrix{Float64}[]
	if _solar_wraps(m)
		pts  = reduce(vcat, pieces)
		p    = sortslices(pts, dims = 1, by = r -> r[1])
		pole = sunlat < 0 ? 90.0 : -90.0
		poly = vcat(p, [W + 360.0 pole], [W pole], p[1:1, :])
		return [poly]
	end
	return [vcat(pc, pc[1:1, :]) for pc in pieces if size(pc, 1) >= 3]
end

# Hand the dialog the report it shows in its own box. Same write-only text channel the Euler dialog
# uses; kept Julia-side too so the tests and a console user can read what the last run said.
const _solar_last_report = Ref{String}("")

function _solar_report(txt::AbstractString)
	_solar_last_report[] = String(txt)
	try
		ccall(_fn(:gmtvtk_solar_report), Cvoid, (Cstring,), String(txt))
	catch                                   # no viewer library around (tests) — the text still stands
	end
	return
end

# "06:12" from a fraction of a day (what -C reports for sunrise/sunset/noon), and from a number of
# minutes (what it reports for the day length). NaN is the polar case, where there is no such instant
# at all.
function _solar_hhmm(dayfrac::Real)
	isfinite(dayfrac) || return "--:--"
	return _solar_hhmm_min(dayfrac * 1440.0)
end
function _solar_hhmm_min(minutes::Real)
	isfinite(minutes) || return "--:--"
	t = round(Int, minutes)
	h, m = divrem(mod(t, 1440), 60)
	return string(lpad(h, 2, '0'), ':', lpad(m, 2, '0'))
end

_solar_fmt(v::Real, n::Int = 4) = isfinite(v) ? string(round(v, digits = n)) : "n/a"

# The ten numbers -C reports, turned into the report the dialog shows. Column order is the module's
# own: sun lon, sun lat, azimuth, elevation, sunrise, sunset, noon, day length (minutes), elevation
# corrected for refraction, equation of time (minutes).
function _solar_report_text(v::AbstractVector{<:Real}, pos::AbstractString, when::AbstractString,
                            tz::AbstractString)::String
	length(v) >= 4 || error("solar -C returned $(length(v)) values, expected 10")
	io = IOBuffer()
	stamp = isempty(when) ? "now" : when
	println(io, "Sun position — ", stamp, isempty(tz) ? " UTC" : " (UTC" * tz * ")")
	println(io, "  Longitude = ", _solar_fmt(v[1]))
	println(io, "  Latitude  = ", _solar_fmt(v[2]))
	println(io, "  Azimuth   = ", _solar_fmt(v[3]))
	print(io,   "  Elevation = ", _solar_fmt(v[4]))
	length(v) >= 9 && print(io, "   (refraction-corrected ", _solar_fmt(v[9]), ")")
	println(io)
	length(v) >= 10 && println(io, "  Equation of time = ", _solar_fmt(v[10], 2), " min")
	# Without a position the module still fills these in, but for the point (0,0) — which is not what
	# anyone reading a report would assume, so they are only shown when a position was actually given.
	if !isempty(pos) && length(v) >= 8
		println(io, "At ", replace(pos, '/' => " / "), ":")
		if isfinite(v[5]) && isfinite(v[6])
			println(io, "  Sunrise = ", _solar_hhmm(v[5]), "   Noon = ", _solar_hhmm(v[7]),
			            "   Sunset = ", _solar_hhmm(v[6]))
			println(io, "  Day length = ", _solar_hhmm_min(v[8]))
		else
			println(io, "  The sun neither rises nor sets there on that date (polar day or night).")
		end
	end
	return String(take!(io))
end

# The single row -C produces, whatever wrapper it arrives in.
function _solar_row(R)::Vector{Float64}
	D = isa(R, Vector) ? (isempty(R) ? error("solar returned no sun report") : R[1]) : R
	isa(D, GMTdataset) || error("got a $(typeof(D)), not a table")
	m = D.data
	(size(m, 1) >= 1 && size(m, 2) >= 4) || error("solar -C returned an unusable report")
	return Float64.(vec(m[1, :]))
end

# The sub-solar latitude, which is what decides WHICH pole a wrapping night region closes around.
# Asked of the module with the same -I -C the report uses (one instant, one source of truth), and
# only when something actually needs it — a run that paints nothing never pays for it.
function _solar_sunlat(mods::AbstractString)::Float64
	R = GMT.solar(I = isempty(mods) ? true : mods, C = true)
	v = _solar_row(R)
	return length(v) >= 2 ? v[2] : 0.0
end

# Paint ONE terminator's night side: the closed region(s) as filled polygons, through the drawn-
# polygon constructor. `tr` is transparency in PERCENT, the number pssolar takes after the @ in -G;
# the constructor wants opacity, so it is 1 - tr/100. The outline is given the fill's own colour at
# zero width: the visible terminator line is the line overlay beside this, and a painted region must
# not draw a second one over the map's edge and the pole, where this polygon's own boundary runs.
function _solar_paint!(scene::Ptr{Cvoid}, polys::Vector{Matrix{Float64}}, name::AbstractString,
                       rgb::NTuple{3,Float64}, tr::Float64)::Int
	n = 0
	op = clamp(1.0 - tr / 100.0, 0.0, 1.0)
	for (k, R) in enumerate(polys)
		size(R, 1) >= 4 || continue
		xyz = vec(permutedims(hcat(R, zeros(size(R, 1)))))
		nm  = length(polys) == 1 ? name : "$name ($k)"
		idx = ccall(_fn(:gmtvtk_add_poly_full), Cint,
		            (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cint, Cint, Cdouble, Cdouble, Cdouble,
		             Cdouble, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cstring, Cstring),
		            scene, xyz, Cint(size(R, 1)), Cint(1), Cint(0),
		            rgb[1], rgb[2], rgb[3], 0.0, Cint(0), rgb[1], rgb[2], rgb[3], op, nm, _SOLAR_FILL_GROUP)
		idx >= 0 && (n += 1)
	end
	return n
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaSolarFn. Returns Cint 1 on success, 0 on failure — the dialog turns this into its
# own modal answer.
function _on_solar(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		mods  = _solar_mods(d)
		terms = _get(d, "terms")
		fills = _get(d, "fill")
		wantsun = _on(d, "sun") || _on(d, "marksun")
		(isempty(terms) && !wantsun) &&
			error("nothing to do — tick a terminator, or the sun report")

		lw = something(tryparse(Float64, _get(d, "width")), 1.5)
		W  = something(tryparse(Float64, _get(d, "mapw")), -180.0)
		# Painting needs to know which pole is in the dark, and that is the sun's own latitude. Asked
		# once, for the whole run, and only when a region is actually being painted.
		painting = any(t -> occursin(t[1], fills), _SOLAR_TERMS) && !isempty(terms)
		sunlat = painting ? _solar_sunlat(mods) : 0.0

		# REPLACE, never pile up: the terminator of 10:00 and the one of 11:00 are the same layers seen
		# at two instants, not two sets of layers. Both groups go before anything is added.
		ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, _SOLAR_GROUP)
		ccall(_fn(:gmtvtk_remove_polys_h), Cint, (Ptr{Cvoid}, Cstring), scene, _SOLAR_FILL_GROUP)

		dumped = GMTdataset[]
		for (letter, name, linergb, fillrgb) in _SOLAR_TERMS
			occursin(letter, terms) || continue
			D = GMT.solar(T = letter * mods, M = true)     # -M: hand over the polygon, plot nothing
			(D === nothing || isempty(D)) && error("solar returned no polygon for terminator '$letter'")
			ring = (D isa GMTdataset ? D : D[1]).data
			# The PAINT goes down first, so the line lands on top of the region it bounds.
			if occursin(letter, fills)
				rgb = _solar_rgb(_get(d, "fillrgb_" * letter), fillrgb)
				tr  = something(tryparse(Float64, _get(d, "filltr_" * letter)), 65.0)
				_solar_paint!(scene, _solar_night_polys(ring, W, sunlat), "$name (night)", rgb, tr)
			end
			pieces = _solar_split_seam(D, W)
			isempty(pieces) && error("the $name has no points inside this map")
			_add_geo_overlay(scene, pieces; color = linergb, linewidth = lw, name = name,
			                 noConvertToPoints = true, group = _SOLAR_GROUP) ||
				error("$name: window closed, nothing added")
			append!(dumped, D isa GMTdataset ? [D] : collect(D))
		end

		if wantsun
			pos = _solar_pos(d)
			arg = pos * mods
			# -I takes the position, the modifiers, both or neither; `true` is the bare -I.
			R = GMT.solar(I = isempty(arg) ? true : arg, C = true)
			v = _solar_row(R)
			_on(d, "sun") && _solar_report(_solar_report_text(v, pos, _get(d, "date"), _get(d, "tz")))
			if _on(d, "marksun")
				lon = mod(v[1] - W, 360.0) + W             # the map's own longitude frame
				ccall(_fn(:gmtvtk_remove_symbols_h), Cint, (Ptr{Cvoid}, Cstring), scene, _SOLAR_SUN_LAYER)
				add_symbols!(scene, [lon], [v[2]]; symbol = :star, size = 16, sizeunit = :pt,
				             fill = :yellow, edge = :black, edgewidth = 1.0, name = _SOLAR_SUN_LAYER,
				             info = ["Sub-solar point\nlon $(_solar_fmt(lon))\nlat $(_solar_fmt(v[2]))"])
			end
		end

		# The dump is also what gets saved, so the file holds exactly the polygons that were drawn —
		# with the module's own segment headers ("Day/night terminator", …).
		out = _get(d, "outfile")
		if !isempty(out)
			isempty(dumped) && error("there is no terminator to save — tick at least one")
			GMT.gmtwrite(String(out), dumped)          # a Vector{GMTdataset} = one multi-segment table
		end
		return Cint(1)
	catch e
		_viewer_log_error(scene, "solar FAILED: $(sprint(showerror, e))")
		@warn "solar FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_solar()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_solar, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_solar_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
