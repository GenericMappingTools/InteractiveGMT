# satellite.jl — SGP4/SDP4 satellite orbit propagation, over the C in deps/src/satellite.cpp
# (which wraps Bill Gray's sat_code, MIT, vendored at deps/src/sat_code/ — PROVENANCE.md there).
#
# Like mbgrid.jl, this is a thin Julia face on a second translation unit of the viewer DLL: the
# numerics are in C, this file is types, time handling and the plotting path. Nothing here
# reimplements any orbital mechanics — if a quantity is wanted, it comes from a `sat_*` call.
#
# The two things this layer really owns:
#   * TIME. Everything public takes `DateTime`s (or Julian Days); the C side takes Julian Days, and
#     the conversion goes through the SAME sat_cal_to_jd/sat_jd_to_cal the C half and test_satellite
#     use, so the two halves cannot drift apart on a calendar edge.
#   * THE DATELINE. A ground track is a line on a map, and a line that runs from +179 to -179 draws
#     a streak straight across the map unless it is cut. `_split_dateline` cuts it. That is the one
#     piece of presentation logic that cannot live in C, which knows nothing about how it is drawn.

# Dates comes through GMT, NOT as a dependency of this package: `GMT.Dates` is how ecmwf.jl and
# ecv.jl already reach it, and Project.toml is not touched for a stdlib the package can already see.
import GMT.Dates: DateTime, Period, Millisecond, Second, Minute, Hour,
                  year, month, day, hour, minute, second, millisecond

# Exports live in InteractiveGMT.jl's one central list, like every other module here — not beside
# the definitions. Only the distinctive names go there: `epoch`, `period`, `model` and `datetime`
# are deliberately NOT exported, because they are far too generic to put in a user's namespace.

# --- error helper ------------------------------------------------------------------------------
_sat_strerror(rc::Int)::String =
	unsafe_string(ccall(_fn(:sat_strerror), Cstring, (Cint,), Cint(rc)))

"""
    TLE(name, line1, line2)

One two-line element set, with the (optional) name line that precedes it in a TLE file.
`line1`/`line2` are kept verbatim — the C side parses them, so nothing here has to know the
column layout, and a file round-trips unchanged.
"""
struct TLE
	name::String
	line1::String
	line2::String
end

Base.show(io::IO, t::TLE) = print(io, "TLE(", isempty(t.name) ? "unnamed" : t.name, ")")

# A TLE line is 69 characters and starts with "1 " or "2 ". Checking the length as well as the
# prefix matters: TLE files routinely carry prose, blank lines and comments, and "1 " on its own is
# far too weak a test (upstream's own test file is full of counter-examples).
_is_tle_line(s::String, which::Char)::Bool =
	length(s) >= 69 && s[1] == which && s[2] == ' '

"""
    read_tle(src) -> Vector{TLE}

Read every TLE in `src`, which is either a path to a TLE file or a string holding the TLE text
itself (what a Celestrak/Space-Track download hands back). Two- and three-line formats are both
read: a non-TLE line immediately above a "1 …" line is taken as the object's name, and anything
else — blanks, comments, prose — is skipped.

Malformed sets are skipped rather than thrown on: these files come off the network, and one bad
entry should not cost the other nine hundred.
"""
function read_tle(src::String)::Vector{TLE}
	txt = (length(src) < 4096 && isfile(src)) ? read(src, String) : src
	out = TLE[]
	pending = ""                      # the most recent line that could be a name
	lines = split(replace(txt, "\r\n" => "\n"), '\n')
	i = 1
	while i <= length(lines)
		l1 = String(rstrip(lines[i], [' ', '\r']))
		if _is_tle_line(l1, '1') && i < length(lines)
			l2 = String(rstrip(lines[i+1], [' ', '\r']))
			if _is_tle_line(l2, '2')
				push!(out, TLE(pending, l1, l2))
				pending = ""
				i += 2
				continue
			end
		end
		# Not a TLE line: remember it as a candidate name, unless it is blank or a comment.
		s = strip(l1)
		pending = (isempty(s) || startswith(s, '#')) ? "" : String(s)
		i += 1
	end
	return out
end

# --- time --------------------------------------------------------------------------------------
# One conversion in each direction, both through the C API, so the Julia half and the C half can
# never disagree about what a Julian Day means.

"""
    jd(t::DateTime) -> Float64

Julian Day (UTC) of a `DateTime`.
"""
function jd(t::DateTime)::Float64
	return ccall(_fn(:sat_cal_to_jd), Cdouble,
	             (Cint, Cint, Cdouble, Cint, Cint, Cdouble),
	             Cint(year(t)), Cint(month(t)), Cdouble(day(t)),
	             Cint(hour(t)), Cint(minute(t)),
	             Cdouble(second(t) + millisecond(t) / 1000))
end

"""
    datetime(jd::Float64) -> DateTime

The inverse of [`jd`](@ref), to millisecond resolution.
"""
function datetime(j::Float64)::DateTime
	y = Ref{Cint}(0); mo = Ref{Cint}(0); d = Ref{Cint}(0)
	h = Ref{Cint}(0); mi = Ref{Cint}(0); s = Ref{Cdouble}(0.0)
	ccall(_fn(:sat_jd_to_cal), Cvoid,
	      (Cdouble, Ref{Cint}, Ref{Cint}, Ref{Cint}, Ref{Cint}, Ref{Cint}, Ref{Cdouble}),
	      Cdouble(j), y, mo, d, h, mi, s)
	ms = round(Int, s[] * 1000)
	return DateTime(Int(y[]), Int(mo[]), Int(d[]), Int(h[]), Int(mi[])) + Millisecond(ms)
end

# Anything a caller might hand over as "when", reduced to the Vector{Float64} of Julian Days the C
# side wants. ONE funnel, so every entry point below accepts exactly the same time forms.
_jds(t::DateTime)::Vector{Float64}          = [jd(t)]
_jds(t::Vector{DateTime})::Vector{Float64}  = Float64[jd(x) for x in t]
_jds(t::Float64)::Vector{Float64}           = [t]
_jds(t::Vector{Float64})::Vector{Float64}   = t
_jds(t::StepRange{DateTime,<:Period})::Vector{Float64} = Float64[jd(x) for x in t]
_jds(t::AbstractRange)::Vector{Float64}     = Float64[Float64(x) for x in t]

# --- the handle --------------------------------------------------------------------------------

"""
    Satellite(tle::TLE; model = 0)

A propagator for one TLE. Parsing and the model set-up happen once, here, so propagating N epochs
costs N model steps and not N initialisations.

`model` picks the analytic model by name (`:SGP`, `:SGP4`, `:SDP4`, `:SGP8`, `:SDP8`); the default
`0` lets the C side choose, which is what you want — it uses the near-earth or deep-space model
according to the orbit's actual period, not according to what the TLE claims. Published TLEs almost
all claim type 0, so without that a geostationary or Molniya orbit would be propagated with the
wrong model and be wrong by hundreds of kilometres.

The C handle is freed by a finalizer, and `close!` frees it early if you want determinism.
"""
mutable struct Satellite
	h::Ptr{Cvoid}
	tle::TLE
end

const _SAT_MODELS = Dict{Symbol,Int}(:auto => 0, :SGP => 1, :SGP4 => 2, :SDP4 => 3,
                                     :SGP8 => 4, :SDP8 => 5)

function Satellite(t::TLE; model::Union{Int,Symbol} = 0)
	m = model isa Symbol ? get(_SAT_MODELS, model) do
			error("unknown SGP model :$model — one of $(collect(keys(_SAT_MODELS)))")
		end : model
	err = Ref{Cint}(0)
	h = ccall(_fn(:sat_create_ex), Ptr{Cvoid}, (Cstring, Cstring, Cint, Ref{Cint}),
	          t.line1, t.line2, Cint(m), err)
	h == C_NULL && error("InteractiveGMT: cannot read the TLE for " *
	                     (isempty(t.name) ? "this object" : t.name) * ": " *
	                     _sat_strerror(Int(err[])))
	s = Satellite(h, t)
	finalizer(close!, s)
	return s
end

Satellite(l1::String, l2::String; kw...) = Satellite(TLE("", l1, l2); kw...)

function close!(s::Satellite)
	if s.h != C_NULL
		ccall(_fn(:sat_destroy), Cvoid, (Ptr{Cvoid},), s.h)
		s.h = C_NULL
	end
	return nothing
end

_check_open(s::Satellite) = s.h == C_NULL && error("this Satellite has already been closed")

# --- accessors ---------------------------------------------------------------------------------
epoch(s::Satellite)::DateTime  = datetime(epoch_jd(s))
epoch_jd(s::Satellite)::Float64 = (_check_open(s);
	ccall(_fn(:sat_epoch_jd), Cdouble, (Ptr{Cvoid},), s.h))
norad_number(s::Satellite)::Int = (_check_open(s);
	Int(ccall(_fn(:sat_norad_number), Cint, (Ptr{Cvoid},), s.h)))
mean_motion(s::Satellite)::Float64 = (_check_open(s);
	ccall(_fn(:sat_mean_motion), Cdouble, (Ptr{Cvoid},), s.h))
inclination(s::Satellite)::Float64 = (_check_open(s);
	ccall(_fn(:sat_inclination), Cdouble, (Ptr{Cvoid},), s.h))
eccentricity(s::Satellite)::Float64 = (_check_open(s);
	ccall(_fn(:sat_eccentricity), Cdouble, (Ptr{Cvoid},), s.h))

function model(s::Satellite)::Symbol
	_check_open(s)
	m = ccall(_fn(:sat_model), Cint, (Ptr{Cvoid},), s.h)
	return Symbol(unsafe_string(ccall(_fn(:sat_model_name), Cstring, (Cint,), m)))
end

"""Orbital period, in minutes."""
period(s::Satellite)::Float64 = 1440.0 / mean_motion(s)

function Base.show(io::IO, s::Satellite)
	if s.h == C_NULL
		print(io, "Satellite(closed)");  return
	end
	print(io, "Satellite(", isempty(s.tle.name) ? string(norad_number(s)) : s.tle.name,
	      ", ", model(s), ", ", round(period(s), digits = 1), " min, incl ",
	      round(inclination(s), digits = 2), "°, epoch ", epoch(s), ")")
end

# --- propagation -------------------------------------------------------------------------------

"""
    propagate(sat, when) -> (pos, vel)

State vectors in the TEME frame: `pos` in km and `vel` in km/s, each a 3×N matrix (one column per
epoch). `when` is a `DateTime`, a vector or range of them, or Julian Days as `Float64`.

An epoch the model cannot solve comes back as `NaN` rather than as a plausible-looking point — a
long run is not aborted by one bad step.
"""
function propagate(s::Satellite, when)
	_check_open(s)
	j = _jds(when)
	n = length(j)
	n == 0 && return (zeros(3, 0), zeros(3, 0))
	pos = Matrix{Float64}(undef, 3, n)
	vel = Matrix{Float64}(undef, 3, n)
	rc  = Vector{Cint}(undef, n)
	r = ccall(_fn(:sat_propagate), Cint,
	          (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cint}),
	          s.h, j, Cint(n), pos, vel, rc)
	r != 0 && error("InteractiveGMT: sat_propagate: " * _sat_strerror(Int(r)))
	return (pos, vel)
end

"""
    propagate_ecef(sat, when) -> Matrix

Earth-fixed (ECEF) positions in km, 3×N. Same time forms as [`propagate`](@ref).
"""
function propagate_ecef(s::Satellite, when)::Matrix{Float64}
	_check_open(s)
	j = _jds(when)
	n = length(j)
	n == 0 && return zeros(3, 0)
	out = Matrix{Float64}(undef, 3, n)
	r = ccall(_fn(:sat_propagate_ecef), Cint,
	          (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cdouble}, Ptr{Cint}),
	          s.h, j, Cint(n), out, C_NULL)
	r != 0 && error("InteractiveGMT: sat_propagate_ecef: " * _sat_strerror(Int(r)))
	return out
end

"""
    subpoint(sat, when) -> (lon, lat, alt)

The sub-satellite point: geodetic longitude and latitude in degrees (WGS84) and height above the
ellipsoid in km, as three vectors.
"""
function subpoint(s::Satellite, when)
	_check_open(s)
	j = _jds(when)
	n = length(j)
	n == 0 && return (Float64[], Float64[], Float64[])
	lon = Vector{Float64}(undef, n)
	lat = Vector{Float64}(undef, n)
	alt = Vector{Float64}(undef, n)
	r = ccall(_fn(:sat_groundtrack), Cint,
	          (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cint}),
	          s.h, j, Cint(n), lon, lat, alt, C_NULL)
	r != 0 && error("InteractiveGMT: sat_groundtrack: " * _sat_strerror(Int(r)))
	return (lon, lat, alt)
end

# --- the dateline ------------------------------------------------------------------------------
# A ground track is drawn as a polyline, so a step from +179.9 to -179.9 must become a BREAK and not
# a segment straight back across the map. The test is on the size of the jump, not on the sign: a
# real step is at most a few degrees (an orbit moves ~4 deg/min at worst), so anything over 180 is
# a wrap. Nothing is inserted at the edge — the cut is between the two points, which is honest
# about where the data actually is.
function _split_dateline(lon::Vector{Float64}, lat::Vector{Float64}, alt::Vector{Float64},
                         jds::Vector{Float64})::Vector{UnitRange{Int}}
	segs = UnitRange{Int}[]
	n = length(lon)
	n == 0 && return segs
	start = 1
	for i in 2:n
		# A NaN (a failed epoch) also breaks the line; it is not a place.
		if isnan(lon[i]) || isnan(lon[i-1]) || abs(lon[i] - lon[i-1]) > 180.0
			i - 1 >= start && push!(segs, start:(i-1))
			start = i
		end
	end
	start <= n && push!(segs, start:n)
	# Drop single-point runs: a one-vertex "line" is not drawable and only clutters the table.
	return filter(r -> length(r) > 1, segs)
end

# Altitude -> the viewer's vertical world units.
#
# The globe engine (10_geometry.cpp) puts a point at `r = globeR + z * (zfac * ve)` with
# `globeR = 180/pi`, chosen so that ONE DEGREE OF EQUATORIAL ARC IS ONE WORLD UNIT — the same
# horizontal unit the flat lon/lat map already uses. So a height expressed in degrees-of-arc is
# geometrically true in both views, and needs no second formula for the globe.
#
# One degree of arc = 2*pi*R/360 with R the Earth's mean radius. An ISS orbit at 420 km therefore
# becomes 3.78 units: on the globe that is a radius of 57.3 + 3.78, i.e. 6.6 % above the surface,
# which is exactly 420/6371. On the flat map it is 3.78 units over a 360-wide world — visible as a
# 3-D curve, negligible as a horizontal offset when looked at from above.
#
# (Where the window holds a RELIEF grid, `zfac * ve` is that grid's own exaggeration and the orbit
# rides it like everything else vertical in this app. That is deliberate: an orbit drawn to true
# scale beside terrain exaggerated 20x would be the one thing in the window not on the same
# vertical scale as its neighbours.)
const _EARTH_MEAN_R_KM = 6371.0
const _KM_PER_DEG_ARC  = 2pi * _EARTH_MEAN_R_KM / 360    # 111.195 km

"""
    groundtrack(sat; start, stop, step) -> Vector{GMTdataset}
    groundtrack(sat, when)              -> Vector{GMTdataset}

The sub-satellite track, as a multi-segment dataset ready to plot: columns are
`lon`, `lat`, `alt_km`, `time` (Julian Day), and the track is **cut at the dateline** so it draws
as a map track instead of streaking back across the plot.

With keywords, `start` defaults to the TLE's own epoch (a TLE is only good for a few days either
side of it), `stop` to one orbital period later — i.e. exactly one revolution — and `step` to 30
seconds, which keeps a LEO track smooth.

    groundtrack(sat; start = now(UTC), stop = now(UTC) + Hour(6), step = Minute(1))
"""
function groundtrack(s::Satellite, when; altitude::Bool = true)::Vector{GMT.GMTdataset}
	j = _jds(when)
	lon, lat, alt = subpoint(s, j)
	segs = _split_dateline(lon, lat, alt, j)
	out = GMT.GMTdataset[]
	for r in segs
		# COLUMN 3 IS THE PLOTTED Z — the height the renderer actually draws the vertex at, and what
		# lifts the orbit off the globe in 3-D. It is the altitude CONVERTED TO WORLD UNITS
		# (_KM_PER_DEG_ARC above), never the raw kilometres: raw km put the line ~420 units over a
		# 360-wide map, which is how this first shipped invisible.
		# `altitude=false` gives a plain ground track, flat on the map.
		zcol = altitude ? alt[r] ./ _KM_PER_DEG_ARC : zeros(length(r))
		D = GMT.mat2ds(hcat(lon[r], lat[r], zcol, alt[r], j[r]))
		D.colnames = ["lon", "lat", "z", "alt_km", "time_jd"]
		D.proj4 = "+proj=longlat +datum=WGS84"
		push!(out, D)
	end
	# `ds_bbox` is what the display path reads for a multi-segment set; mat2ds fills each segment's
	# own bbox but not the collective one when they are built separately like this.
	isempty(out) || GMT.set_dsBB!(out)
	return out
end

function groundtrack(s::Satellite; altitude::Bool = true, start::Union{Nothing,DateTime} = nothing,
                     stop::Union{Nothing,DateTime} = nothing,
                     step::Period = Second(30),
                     revolutions::Real = 1)::Vector{GMT.GMTdataset}
	t0 = start === nothing ? epoch(s) : start
	t1 = stop === nothing ? t0 + Millisecond(round(Int, revolutions * period(s) * 60_000)) : stop
	t1 <= t0 && error("groundtrack: `stop` ($t1) must be after `start` ($t0)")
	return groundtrack(s, collect(t0:step:t1); altitude = altitude)
end

# --- plotting ----------------------------------------------------------------------------------

"""
    plot_groundtrack!(fig, sat; color = nothing, name = "", kw...)

Draw `sat`'s ground track into an open iGMT window and register it in Scene Objects.

`fig` is a window handle (a `QtFigure`, or the raw pointer). Keywords not listed are passed to
[`groundtrack`](@ref), so `start`, `stop`, `step` and `revolutions` all work here.

This is a VECTOR overlay landing on whatever is already displayed, so — per SACRED_LAW.md's
vector-import law — it does NOT reframe the axes and does not spawn a set of its own: it goes on top
of the map that is already there. It reaches the scene through `_add_dataset_to_scene`, the same
function every other vector import uses, which is what gives it its Scene Objects row (one row per
satellite, carrying every dateline segment) with properties and Remove.
"""
function plot_groundtrack!(scene::Ptr{Cvoid}, s::Satellite; color = nothing,
                           name::String = "", kw...)
	scene == C_NULL && error("plot_groundtrack!: no window")
	D = groundtrack(s; kw...)
	nm = !isempty(name) ? name :
	     !isempty(s.tle.name) ? s.tle.name : "NORAD " * string(norad_number(s))
	return _plot_track!(scene, D, nm; color = color)
end

# The add half, split out so a caller that already has the track (the dialog, which then asks
# whether it is on screen) does not propagate it a second time just to plot it. ONE add path —
# `plot_groundtrack!` is this function plus the propagation, never a parallel copy of it.
function _plot_track!(scene::Ptr{Cvoid}, D::Vector{GMT.GMTdataset}, nm::String; color = nothing)
	isempty(D) && (@warn "plot_groundtrack!: the track is empty"; return false)

	# An EMPTY window has nothing to overlay ONTO, so the vector-import law does not apply here:
	# that law governs a vector dropped on a window that ALREADY shows a grid or an image. Applied
	# to an empty launcher it leaves the track with no frame from anywhere — plotted correctly and
	# invisible, which is exactly how this read as "Plot does nothing".
	#
	# THE BASE MAP IS THE WHOLE WORLD, ALWAYS — never the track's own bounding box. A satellite
	# orbits the entire planet; framing the map to the latitudes this particular pass happens to
	# reach CLIPS THE GLOBE (an ISS track cut the world off at ±57° and threw the poles away), and
	# the next satellite would want a different crop of the same map. The track is what varies; the
	# Earth is not. `_on_basemap` is the same call `_promote_for_vector` makes internally — given
	# the global region instead of a data-derived one.
	if ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
		# W/E/S/N/wrap/tag. Tag "global" (not "region") is deliberate: it names the layer
		# "Base image (global)", which is also the dedup key — so plotting a second satellite into
		# the same window reuses the one Earth instead of stacking another copy of it.
		_on_basemap(scene, "-180.0/180.0/-90.0/90.0/0/global")
	end

	ok = _add_dataset_to_scene(scene, D, nm; groupName = "Satellite tracks",
	                           color = color, forceMode = :lines)
	ok && ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
	return ok
end

# Does the window's current view actually contain any of `D`? A ground track is global, so dropping
# one on a window framed to a small region puts it off-screen — correctly plotted and invisible.
# The law forbids reframing over an existing raster, so the honest thing left is to SAY so.
# Returns `nothing` when the window has no bounds to compare against (nothing to warn about).
function _track_offscreen(scene::Ptr{Cvoid}, D::Vector{GMT.GMTdataset})::Union{Nothing,Bool}
	b = zeros(Float64, 4);  g = zeros(Cint, 1)
	ok = ccall(_fn(:gmtvtk_get_display_bounds_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Ptr{Cint}), scene, b, g)
	ok == 0 && return nothing
	(b[1] == b[2] || b[3] == b[4]) && return nothing
	for d in D
		for i in 1:size(d.data, 1)
			x, y = d.data[i, 1], d.data[i, 2]
			(isnan(x) || isnan(y)) && continue
			(b[1] <= x <= b[2] && b[3] <= y <= b[4]) && return false   # at least one point visible
		end
	end
	return true
end

plot_groundtrack!(fig::QtFigure, s::Satellite; kw...) = plot_groundtrack!(fig.h, s; kw...)

# --- the Satellite menu's dialog -----------------------------------------------------------------
# SatelliteDialog (70_window.cpp, deps/ui/satellite.ui) talks to this file through ONE callback, with
# the newline "key=value" block documented at JuliaSatelliteFn (30_app.cpp). Same shape as the
# Copernicus/Sentinel Hub tool: `what` says what is being asked, `out`/`cap` carry text back.

# The TLEs the dialog last listed. The dialog sends back ROW INDICES into the list it was given, so
# the vector it indexes has to be the same one — keyed by source string so two windows pointed at
# different files cannot read each other's rows.
const _SAT_LOADED = Dict{String,Vector{TLE}}()

_sat_kv(params::String)::Dict{String,String} = begin
	d = Dict{String,String}()
	for ln in split(params, '\n')
		i = findfirst('=', ln)
		i === nothing && continue
		d[String(strip(ln[1:i-1]))] = String(strip(ln[i+1:end]))
	end
	d
end

# Read the source named by the dialog's radio pair. A URL is fetched to a scratch file and read from
# there, so the same `read_tle` handles both and there is one parser, not two.
function _sat_read_source(d::Dict{String,String})::Vector{TLE}
	src = get(d, "src", "file")
	if src == "url"
		url = get(d, "url", "")
		isempty(url) && error("give me a URL that returns TLE text")
		# Straight into memory — a TLE list is tens of kB, and this tool has no business leaving
		# scratch files on the user's disk.
		io = IOBuffer()
		Downloads.download(url, io)
		return read_tle(String(take!(io)))
	end
	path = get(d, "path", "")
	isempty(path) && error("pick a TLE file first")
	isfile(path)  || error("no such file: " * path)
	return read_tle(read(path, String))
end

# A stable key for _SAT_LOADED: whichever of the two source fields is actually in use.
_sat_key(d::Dict{String,String})::String =
	get(d, "src", "file") == "url" ? "url:" * get(d, "url", "") : "file:" * get(d, "path", "")

function _on_satellite(scene::Ptr{Cvoid}, params::Cstring, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		d   = _sat_kv(unsafe_string(params))
		key = _sat_key(d)
		what = get(d, "what", "")

		if what == "list"
			tles = _sat_read_source(d)
			isempty(tles) && error("no TLE found in that source")
			_SAT_LOADED[key] = tles
			# One name per line, in the order the dialog's row indices will refer to. A nameless
			# TLE (a two-line file) still needs a row, so fall back to its catalogue number.
			names = String[]
			for t in tles
				nm = isempty(t.name) ? "NORAD " * strip(t.line1[3:7]) : t.name
				push!(names, nm)
			end
			_sat_reply(out, cap, join(names, '\n'))
			return Cint(1)
		end

		if what == "plot"
			scene == C_NULL && error("no window to plot into")
			tles = get(_SAT_LOADED, key) do
				# The dialog can only send `plot` after a `list`, but a re-read costs nothing and
				# means a stale/evicted cache is never an error the user has to understand.
				t = _sat_read_source(d);  _SAT_LOADED[key] = t;  t
			end
			sel = [parse(Int, s) for s in split(get(d, "sel", ""), ',') if !isempty(strip(s))]
			isempty(sel) && error("no satellite selected")

			step = Second(max(1, round(Int, _sat_num(get(d, "step", "30"), 30.0))))
			spanv = _sat_num(get(d, "span", "2"), 2.0)
			spanv <= 0 && error("the duration must be greater than zero")
			mode = get(d, "spanmode", "revs")
			useNow = get(d, "start", "epoch") == "now"
			# Default ON: an orbit is a 3-D object, and the checkbox is checked in the .ui to match.
			useAlt = get(d, "altitude", "1") != "0"

			done = String[]
			offscreen = 0
			for i in sel
				(i < 0 || i >= length(tles)) && continue
				s = Satellite(tles[i+1])
				try
					t0 = useNow ? _sat_now_utc() : epoch(s)
					kw = mode == "minutes" ? (; start = t0, stop = t0 + Minute(round(Int, spanv))) :
					     mode == "hours"   ? (; start = t0, stop = t0 + Minute(round(Int, spanv * 60))) :
					                         (; start = t0, revolutions = spanv)
					nm = isempty(s.tle.name) ? string(norad_number(s)) : s.tle.name
					D  = groundtrack(s; step = step, altitude = useAlt, kw...)   # propagated ONCE, used twice
					if _plot_track!(scene, D, nm)
						push!(done, nm)
						# Checked AFTER the add, so an empty window that was just promoted is judged
						# on the frame it actually ended up with, not on the nothing it had before.
						_track_offscreen(scene, D) === true && (offscreen += 1)
					end
				finally
					close!(s)          # the finalizer would get it, but not predictably
				end
			end
			isempty(done) && error("nothing could be plotted")
			msg = "Plotted: " * join(done, ", ")
			# A track that is correctly plotted but outside the current view looks exactly like a
			# tool that did nothing. Say it instead of leaving the user to guess.
			offscreen > 0 && (msg *= offscreen == length(done) ?
				"  — but it is OUTSIDE the area this window is showing, so you will not see it. Open a world map, or a window with no data in it, and plot there." :
				"  — $offscreen of them fall outside the area this window is showing.")
			_sat_reply(out, cap, msg)
			return Cint(1)
		end

		error("unknown request: " * what)
	catch e
		# NEVER let this throw: an exception escaping a Julia callback wedges the event pump.
		try; _tool_failed(scene, "Satellite", e); catch; end
		_sat_reply(out, cap, sprint(showerror, e))
		return Cint(0)
	end
end

# A number from a dialog box, with the box's own default when it is blank or nonsense — a stray
# keystroke in an edit field must not be an exception the user has to read.
function _sat_num(s::String, dflt::Float64)::Float64
	v = tryparse(Float64, strip(s))
	return v === nothing ? dflt : v
end

# "Now", as UTC, to the second. Built from the same calendar the rest of this file uses.
_sat_now_utc()::DateTime = datetime(jd(GMT.Dates.now(GMT.Dates.UTC)))

# Copy `txt` into the C buffer, NUL-terminated and never past `cap`.
function _sat_reply(out::Ptr{UInt8}, cap::Cint, txt::String)::Nothing
	(out == C_NULL || cap <= 0) && return nothing
	b = codeunits(txt)
	n = min(length(b), Int(cap) - 1)
	GC.@preserve b unsafe_copyto!(out, pointer(b), n)
	unsafe_store!(out, UInt8(0), n + 1)
	return nothing
end

# Fired by warmupTool("satellite") when the dialog opens (70_window.cpp), so the first Load/Plot
# does not pay the JIT in front of the user. A real TLE is propagated here — compiling the path
# is the whole point, and one revolution of a LEO costs well under a millisecond once compiled.
function _sat_warm()
	try
		t = read_tle("ISS (ZARYA)\n" *
		    "1 25544U 98067A   24015.50000000  .00016717  00000-0  30777-3 0  9005\n" *
		    "2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.49514637 10110\n")
		s = Satellite(t[1])
		try
			groundtrack(s; step = Second(60), revolutions = 1)
		finally
			close!(s)
		end
	catch
	end
	precompile(_on_satellite, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	precompile(_sat_read_source, (Dict{String,String},))
	precompile(read_tle, (String,))
	return nothing
end

function _register_satellite()
	fptr = @cfunction((s, p, o, n) -> Base.invokelatest(_on_satellite, s, p, o, n)::Cint,
	                  Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_satellite_callback), Cvoid, (Ptr{Cvoid},), fptr)
	warm_register("satellite", _sat_warm)
	return
end
