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
                         jds::Vector{Float64})::Vector{Matrix{Float64}}
	segs = Matrix{Float64}[]
	n = length(lon)
	n == 0 && return segs
	# Columns: lon, lat, alt_km, jd. Built as rows so the CROSSING POINT can be inserted, which a
	# range of indices into the original samples cannot express.
	cur = Vector{NTuple{4,Float64}}()
	flush!() = (length(cur) > 1 && push!(segs, _rows_to_mat(cur)); empty!(cur))

	push!(cur, (lon[1], lat[1], alt[1], jds[1]))
	for i in 2:n
		l0, l1 = lon[i-1], lon[i]
		if isnan(l1) || isnan(l0)
			flush!()                      # a failed epoch is a real gap: nothing to interpolate
			!isnan(l1) && push!(cur, (l1, lat[i], alt[i], jds[i]))
			continue
		end
		d = l1 - l0
		if abs(d) > 180.0
			# CROSS THE ±180 MERIDIAN EXACTLY, and put a point on BOTH sides of it. Cutting between
			# the two samples (what this did first) throws away up to a whole step of longitude —
			# about 4° at a 30 s cadence. On the flat map that is a small gap; on the GLOBE +180 and
			# -180 are the same meridian, so the two ends should meet, and instead the orbit showed
			# a broken arc with the tube's end-caps hanging in space.
			# `d > 180` means the satellite ran WEST off -180 (e.g. -179 -> +179); `d < -180` means
			# it ran EAST off +180. Unwrap the far sample, find the fraction of the step at which the
			# meridian is reached, and interpolate everything else there.
			west  = d > 0.0
			l1u   = west ? l1 - 360.0 : l1 + 360.0
			edge  = west ? -180.0 : 180.0
			t     = (l1u - l0) == 0.0 ? 0.0 : (edge - l0) / (l1u - l0)
			t     = clamp(t, 0.0, 1.0)
			latc  = lat[i-1] + t * (lat[i] - lat[i-1])
			altc  = alt[i-1] + t * (alt[i] - alt[i-1])
			jdc   = jds[i-1] + t * (jds[i] - jds[i-1])
			push!(cur, (edge, latc, altc, jdc))       # close this segment ON the meridian
			flush!()
			push!(cur, (-edge, latc, altc, jdc))      # ...and open the next one on its far side
		end
		push!(cur, (l1, lat[i], alt[i], jds[i]))
	end
	flush!()
	return segs
end

# Rows -> the (n x 4) matrix the dataset is built from. One place, so the column ORDER is stated once.
function _rows_to_mat(rows::Vector{NTuple{4,Float64}})::Matrix{Float64}
	m = Matrix{Float64}(undef, length(rows), 4)
	for (i, r) in enumerate(rows)
		m[i,1] = r[1];  m[i,2] = r[2];  m[i,3] = r[3];  m[i,4] = r[4]
	end
	return m
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
# THE ORBIT, as an orbit. The sub-satellite point is an EARTH-FIXED quantity: it says which piece of
# ground the satellite is over, and in that frame a geostationary satellite DOES NOT ORBIT — it hangs
# over one longitude, and its subpoint only wanders up and down by the orbit's inclination. Plotted at
# its real altitude that came out as a thin figure-of-eight (the analemma) standing beside the planet,
# 6.6 Earth radii out: geometrically right, and not an orbit. A low orbit merely hid the problem,
# because in one revolution its subpoint really does sweep most of the way round the world.
#
# So the 3-D curve is built from the INERTIAL positions the propagator actually produces (TEME, the
# frame the orbit is a closed path in), turned into the display's Earth-fixed frame by ONE rotation —
# the Earth's rotation angle at a single REFERENCE EPOCH, the end of the track, which is where the
# spacecraft body stands. The shape is then the true orbit, and it is hung correctly over the geography
# for that instant. (Any other choice of reference just spins the ring about the pole.)
#
# The rotation angle is not derived from a formula written here: the C side already knows it, so it is
# read off the SAME propagator by asking for one epoch in both frames and taking the angle between
# them (SACRED_LAW.md — the sidereal-time maths is not reimplemented on this side).
function _orbit_lonlatalt(s::Satellite, j::Vector{Float64})
	pos, _ = propagate(s, j)                      # TEME (inertial), km, 3 x N
	n = size(pos, 2)
	n == 0 && return (Float64[], Float64[], Float64[])
	jref  = j[end]
	pt, _ = propagate(s, [jref])                  # the reference epoch in both frames…
	pe    = propagate_ecef(s, [jref])
	th = atan(pt[2,1], pt[1,1]) - atan(pe[2,1], pe[1,1])    # …so this IS the Earth rotation angle
	c, sn = cos(th), sin(th)
	xyz = Matrix{Float64}(undef, n, 3)
	bad = falses(n)
	for i in 1:n
		x, y, z = pos[1,i], pos[2,i], pos[3,i]
		if !(isfinite(x) && isfinite(y) && isfinite(z))     # a failed epoch stays a gap, never a point
			bad[i] = true
			xyz[i,1] = xyz[i,2] = xyz[i,3] = 0.0
			continue
		end
		xyz[i,1] = ( c * x + sn * y) * 1000.0     # Rz(-th), and km -> metres for mapproject
		xyz[i,2] = (-sn * x + c  * y) * 1000.0
		xyz[i,3] = z * 1000.0
	end
	# ECEF -> geodetic (lon, lat, height) through GMT's own converter, never a hand-rolled ellipsoid.
	G = GMT.mapproject(xyz, E = true, I = true)
	M = G isa GMT.GMTdataset ? G.data : G[1].data
	lon = M[:,1];  lat = M[:,2];  alt = M[:,3] ./ 1000.0
	for i in 1:n
		bad[i] && (lon[i] = lat[i] = alt[i] = NaN)
	end
	return (lon, lat, alt)
end

# WHICH FRAME the 3-D curve is drawn in. The two are different questions and NEITHER answers both:
#
#   :earthfixed — where the satellite is relative to the PLANET, lifted to its altitude. Every
#                 revolution lands ~22.5° further west because the Earth turned under it, which is
#                 the whole story of a low orbit: TERRA is sun-synchronous and must be seen to walk
#                 around the globe, not to retrace one closed loop.
#   :inertial   — the ORBIT itself, the closed path in the frame it is closed in. For a geosynchronous
#                 satellite this is the only one that shows an orbit at all: Earth-fixed, it does not
#                 travel, and its curve collapses to the analemma hanging over one longitude.
#
# :auto picks by the orbit's own physics, not by a list of names: a satellite that keeps station with
# the Earth's rotation (period within 1 % of a sidereal day) has no Earth-fixed path worth drawing, so
# it gets :inertial; everything else gets :earthfixed. The user can always say which one he wants —
# the choice is a control, not a hidden branch, and ONE function serves every satellite either way.
const _SIDEREAL_DAY_MIN = 1436.0682

function _frame_for(s::Satellite, frame::Symbol)::Symbol
	frame === :auto || return frame
	return abs(period(s) - _SIDEREAL_DAY_MIN) <= 0.01 * _SIDEREAL_DAY_MIN ? :inertial : :earthfixed
end

function groundtrack(s::Satellite, when; altitude::Bool = true,
                     frame::Symbol = :auto)::Vector{GMT.GMTdataset}
	j = _jds(when)
	fr = _frame_for(s, frame)
	fr in (:inertial, :earthfixed) || error("groundtrack: frame must be :auto, :inertial or :earthfixed")
	# The FLAT ground track is always the sub-satellite point — that is what the box means when it is
	# unticked, and a flat map has no frame question to answer.
	lon, lat, alt = (altitude && fr === :inertial) ? _orbit_lonlatalt(s, j) : subpoint(s, j)
	segs = _split_dateline(lon, lat, alt, j)   # (n x 4) each: lon, lat, alt_km, jd
	out = GMT.GMTdataset[]
	for seg in segs
		# COLUMN 3 IS THE PLOTTED Z — the height the renderer actually draws the vertex at, and what
		# lifts the orbit off the globe in 3-D. It is the altitude CONVERTED TO WORLD UNITS
		# (_KM_PER_DEG_ARC above), never the raw kilometres: raw km put the line ~420 units over a
		# 360-wide map, which is how this first shipped invisible.
		# `altitude=false` gives a plain ground track, flat on the map.
		zcol = altitude ? view(seg, :, 3) ./ _KM_PER_DEG_ARC : zeros(size(seg, 1))
		D = GMT.mat2ds(hcat(seg[:,1], seg[:,2], zcol, seg[:,3], seg[:,4]))
		D.colnames = ["lon", "lat", "z", "alt_km", "time_jd"]
		D.proj4 = "+proj=longlat +datum=WGS84"
		push!(out, D)
	end
	# `ds_bbox` is what the display path reads for a multi-segment set; mat2ds fills each segment's
	# own bbox but not the collective one when they are built separately like this.
	isempty(out) || GMT.set_dsBB!(out)
	return out
end

# N revolutions, as a duration. ONE formula — `groundtrack`'s own default stop and the dialog's
# "revs" span both read it, so the two can never disagree about how long a revolution is.
_revs_duration(s::Satellite, revolutions::Real)::Millisecond =
	Millisecond(round(Int, revolutions * period(s) * 60_000))

function groundtrack(s::Satellite; altitude::Bool = true, start::Union{Nothing,DateTime} = nothing,
                     stop::Union{Nothing,DateTime} = nothing,
                     step::Period = Second(30),
                     revolutions::Real = 1, frame::Symbol = :auto)::Vector{GMT.GMTdataset}
	t0 = start === nothing ? epoch(s) : start
	t1 = stop === nothing ? t0 + _revs_duration(s, revolutions) : stop
	t1 <= t0 && error("groundtrack: `stop` ($t1) must be after `start` ($t0)")
	return groundtrack(s, collect(t0:step:t1); altitude = altitude, frame = frame)
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
function plot_groundtrack!(scene::Ptr{Cvoid}, s::Satellite; color = _TRACK_COLOR,
                           name::String = "", kw...)
	scene == C_NULL && error("plot_groundtrack!: no window")
	D = groundtrack(s; kw...)
	nm = !isempty(name) ? name :
	     !isempty(s.tle.name) ? s.tle.name : "NORAD " * string(norad_number(s))
	return _plot_track!(scene, D, nm; color = color, sat = s)
end

# The add half, split out so a caller that already has the track (the dialog, which then asks
# whether it is on screen) does not propagate it a second time just to plot it. ONE add path —
# `plot_groundtrack!` is this function plus the propagation, never a parallel copy of it.
# `live = true` marks ONE FRAME of the one-day animation: the same plot, minus the two steps that
# only make sense once it has come to rest — the data table (2880 formatted rows rebuilt 120 times is
# the whole cost of the animation) and the Scene Objects unfold (a tree rebuilt under the user's
# cursor at 20 Hz). The last frame is drawn with `live = false`, so what is left on screen at the end
# is exactly what a plain Plot track leaves.
function _plot_track!(scene::Ptr{Cvoid}, D::Vector{GMT.GMTdataset}, nm::String; color = _TRACK_COLOR,
                      replaced::Union{Nothing,Ref{Int}} = nothing,
                      sat::Union{Nothing,Satellite} = nothing, live::Bool = false)
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

	# ONE TRACK PER SATELLITE. Plotting the same object again is an UPDATE of its orbit — a fresh
	# TLE, a different time window — not a second track: two tracks for one satellite cannot both be
	# right, and stacking them leaves the window with rows nobody can tell apart. So its existing
	# element is removed first and the new one takes its place, keeping the name.
	# By ELEMENT name, never by group: every track shares the "Satellite tracks" group, and removing
	# that would take every other satellite with it.
	nrep = Int(ccall(_fn(:gmtvtk_remove_overlay_named_h), Cint, (Ptr{Cvoid}, Cstring), scene, nm))
	replaced === nothing || (replaced[] = nrep)

	# noConvertToPoints: an orbit is a TRAJECTORY — the samples are a continuous path, not a set of
	# places — so "Convert to points" is meaningless on it and the handle does not offer it.
	# NO intermediate group: the track is a plain element whose MASTER is "Satellites" (declared in
	# `_plot_sat_now!`, together with the body that carries the same name). The panel then reads
	# Satellites > <name> (track) > <name> (body) — two sibling rows per satellite, each with its own
	# checkbox, which is exactly the point: the track and the spacecraft are switched separately.
	ok = _add_dataset_to_scene(scene, D, nm;
	                           color = color, forceMode = :lines, noConvertToPoints = true)

	# An orbit is drawn as a TUBE, the same way the magnetic field lines are (69_magfield.cpp): real
	# 3-D geometry, so it takes perspective and occlusion instead of being a constant-width screen
	# stroke. The same call tells the globe how high this line stands, because the globe's clip plane
	# sits at the sphere's CENTRE — right for a coastline lying on the skin, wrong for an orbit, which
	# stays visible well past the ground horizon and was being cut off there.
	if ok
		# ONE key parents BOTH rows: the track overlay and the body layer carry the same name, and the
		# child -> master map is keyed by name. Declared HERE, beside the track's own add, so a window
		# whose body could not be placed still shows its track under "Satellites" and never loose.
		ccall(_fn(:gmtvtk_set_group_master_h), Cint, (Ptr{Cvoid}, Cstring, Cstring), scene, nm, _SAT_MASTER)
		# THE TRACK IS KEPT, because the ground-coverage swath is the SAME curve widened (see
		# `_swath_polys` below): recomputing it from the TLE would be a second propagation of an orbit
		# already propagated, and — worse — a second answer to "where was this satellite", which is the
		# fork SACRED_LAW.md forbids. The key carries the window, so two windows showing the same
		# satellite over different spans cannot read each other's track.
		_SAT_TRACKS[(UInt(scene), nm)] = D
		live || _track_table!(scene, nm, D)
		lift = 0.0
		for d in D
			m = maximum(view(d.data, :, 3))
			m > lift && (lift = m)
		end
				ccall(_fn(:gmtvtk_overlay_tube_h), Cint, (Ptr{Cvoid}, Cstring, Cdouble, Cdouble),
		      scene, nm, _TRACK_TUBE_R, lift)
		# DO WHAT THE BOX SAYS. The control reads "Draw the orbit at its real altitude (3-D / globe)",
		# so ticking it asks for the orbit in its 3-D form — and that includes being shown in a view
		# where a height EXISTS. A flat-2-D window looks straight down, where 420 km projects onto the
		# same pixels as zero; and on a flat map the orbit's height is 1.1% of the map's width, against
		# 6.8% of the radius on the globe. The globe is therefore the view this box is naming, and the
		# window is put on it. Unticked, nothing here touches the view at all.
		if lift > 0
			# The globe is refused when the window holds nothing geographic; 3-D is then the best
			# this box can honour, and it is still a view with a vertical axis.
			if ccall(_fn(:gmtvtk_set_view_mode_h), Cint, (Ptr{Cvoid}, Cint), scene, Cint(2)) == 0
				ccall(_fn(:gmtvtk_set_view_mode_h), Cint, (Ptr{Cvoid}, Cint), scene, Cint(0))
			end
			# …AND THE VIEW HAS TO REACH IT. A geostationary satellite is 35 786 km up — a ring of 6.6
			# Earth radii, which a camera framed on the planet draws entirely outside its own field: the
			# orbit was plotted correctly and seen by nobody ("METEOSAT shows nothing"). This backs the
			# camera off just far enough, and ONLY when the orbit does not already fit, so a LEO track
			# (6 % of the radius) leaves the user's view exactly as it was. Camera only — a vector import
			# never touches the axes (SACRED_LAW.md).
			ccall(_fn(:gmtvtk_fit_camera_for_orbit_h), Cint, (Ptr{Cvoid}, Cdouble), scene, lift)
		end
		# THE SPACECRAFT ITSELF, on the point of the track it occupies RIGHT NOW. The line says where
		# the satellite has been and will be; the model says where it IS, which is the one place on it
		# a reader actually looks for. Same add path for every caller — never plotted beside the track.
		_plot_sat_now!(scene, D, nm, sat, lift)
		live || ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
	end
	return ok
end

# The track's OWN data table, replacing the generic one the vector importer builds from the plotted
# columns. What a satellite track IS, to a reader: where it is, how high, and when. The plotted `z` is
# NOT data — it is the height in the renderer's world units (alt_km / _KM_PER_DEG_ARC), an internal of
# the drawing, and it has no business in a table that claims to show the element's data. The time goes
# in as a UTC stamp rather than a raw Julian Day for the same reason.
# Rows are written in the SAME order the points were packed (segments in order, vertices in order),
# which is what the viewer aligns them by.
function _track_table!(scene::Ptr{Cvoid}, nm::String, D::Vector{GMT.GMTdataset})::Bool
	rows = String[]
	for d in D
		m = d.data
		for i in 1:size(m, 1)
			push!(rows, join((string(round(m[i,1], digits = 6)),
			                  string(round(m[i,2], digits = 6)),
			                  string(round(m[i,4], digits = 3)),
			                  GMT.Dates.format(datetime(m[i,5]), "yyyy-mm-dd HH:MM:SS")), '\x1f'))
		end
	end
	isempty(rows) && return false
	hdr = join(("lon", "lat", "alt_km", "time (UTC)"), '\x1f')
	return ccall(_fn(:gmtvtk_overlay_set_table_h), Cint, (Ptr{Cvoid}, Cstring, Cstring, Cstring),
	             scene, nm, hdr, join(rows, '\x1e')) != 0
end

# --- the spacecraft at the current epoch ---------------------------------------------------------
# ONE master row, "Satellites", and NOTHING between it and the elements: every satellite puts its
# TRACK and its BODY straight under it as two sibling rows carrying the SAME name (they are the same
# object) and different icons — a line for the track, symbols for the body. Separate rows because they
# are separately switchable, which is the whole reason they are not one row.
# Declared through `gmtvtk_set_group_master_h`, the generic child -> master map the tree reads
# (solar.jl does the same with its three elements); nothing here builds a row of its own, and the
# master's Remove takes track and spacecraft together. The track and the body share one map key, so
# ONE declaration covers both.
const _SAT_MASTER = "Satellites"

# THE BODY IS WORLD-SIZED, NOT SCREEN-CONSTANT: it is an object standing in the scene, so zooming in
# makes it bigger exactly as the terrain and the orbit tube under it get bigger. The number is a
# DIAMETER in world units — the unit one degree of equatorial arc is measured in, the same one the
# orbit's own height is converted to (_KM_PER_DEG_ARC above) — pushed to the layer by
# `gmtvtk_symbol_set_world_size_h`.
#
# AND IT IS SIZED AGAINST ITS OWN ORBIT, not fixed. A real spacecraft is metres across and invisible
# at any zoom that shows an orbit, so the size is symbolic either way — but a symbol that is 400 km
# for every orbit reads correctly on a 7000 km LEO ring and becomes a speck on a 42 164 km
# geostationary one, which is exactly how it looked. A constant FRACTION OF THE ORBIT'S RADIUS reads
# the same on both. Note only about a quarter of the glyph's declared width is solid body (the bus):
# the solar wings are one hundredth of a unit thick and disappear edge-on, so the fraction has to
# carry that too.
const _SAT_GLYPH_FRAC = 0.035          # of the orbit's radius: ~1500 km at GEO, ~150 km at LEO…
const _SAT_GLYPH_WORLD = 3.6           # …but never below this (~400 km), which is the LEO look
# The viewer's globe radius in world units — globeR = 180/pi, i.e. one degree of equatorial arc is
# one world unit (the same convention _KM_PER_DEG_ARC above is built on, 10_geometry.cpp).
const _GLOBE_R_WORLD = 180 / pi

# The body's diameter for a track whose highest point stands `lift` world units above the ground.
_sat_glyph_world(lift::Float64)::Float64 =
	max(_SAT_GLYPH_WORLD, _SAT_GLYPH_FRAC * (_GLOBE_R_WORLD + lift))
# The pixel size is what the layer falls back to before the world size is applied (and what a flat-2-D
# window's diamond counterpart is drawn at). In POINTS, converted at the rate the rest of the package
# uses (96/72 dpi: symbols.jl, xyplot.jl).
const _SAT_GLYPH_PT = 20.0
const _SAT_GLYPH_PX = _SAT_GLYPH_PT * 96 / 72

# The body's element name is the SATELLITE'S OWN NAME, with nothing appended: the row in Scene Objects
# reads "LANDSAT 9", not "LANDSAT 9 (now)". It shares that name with its track, which is what they are
# — one object — and the two live in different lists (symbol layers vs overlays), so neither remover
# can reach the other. ONE place names the body; nothing builds this string a second time.
_sat_now_name(nm::String)::String = nm

"""
Plant the 3-D spacecraft glyph ("sat", a real body built in 50_scene.cpp — not a flat marker) on the
LAST point of the already-plotted track `D`, replacing any earlier one for this satellite.

The last point is where the satellite IS at the end of the propagated window — and the dialog anchors
that window so that, asked for "Now (UTC)", the span runs BACKWARD and the end of the track is this
instant. The body is therefore never interpolated, never clamped and never off the line: it stands on
a sample the propagator actually produced, and the hover block carries that sample's own epoch.
"""
function _plot_sat_now!(scene::Ptr{Cvoid}, D::Vector{GMT.GMTdataset}, nm::String,
                        sat::Union{Nothing,Satellite} = nothing, lift::Float64 = 0.0)::Bool
	mk = _sat_now_name(nm)
	# Re-plotting a satellite is an UPDATE of where it is, exactly as the track itself is (above), so
	# the old body goes first. Removing one that is not there is a no-op, so a first plot needs no case.
	ccall(_fn(:gmtvtk_remove_symbols_h), Cint, (Ptr{Cvoid}, Cstring), scene, mk)
	p = _track_end_point(D)
	p === nothing && return false
	lon, lat, z, altkm, tj = p
	xyz = Float64[lon, lat, z]
	# `datetime` is this file's own JD -> DateTime (through the same C calendar `jd` uses), never a
	# second conversion written here.
	# The ORBIT's own identity belongs on the body, not only on the track: inclination is the number
	# that says what kind of orbit this is (0° equatorial, 51.6° ISS-like, 98° sun-synchronous), and it
	# is the first thing anyone checks when a ring does not look the way they expected.
	incl = sat === nothing ? "" :
	       string("\ninclination ", round(inclination(sat), digits = 3), "°   period ",
	              round(period(sat), digits = 1), " min")
	info = string(nm, '\n', GMT.Dates.format(datetime(tj), "yyyy-mm-dd HH:MM:SS"), " UTC\n",
	              "lon ", round(lon, digits = 3), "°   lat ", round(lat, digits = 3), "°\n",
	              "alt ", round(Int, altkm), " km", incl)
	# Light metal grey, lit: the glyph is a solid body and takes real shading, so it reads as a shape
	# rather than as a coloured dot. No edge width — an assembly of primitives drawn with edges shows
	# its whole triangulation (the C side excludes it for the same reason it excludes a sphere).
	ok = ccall(_fn(:gmtvtk_add_symbols_h), Cint,
	           (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cstring, Cdouble, Cint,
	            Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cstring, Cstring),
	           scene, xyz, Cint(1), "sat", _SAT_GLYPH_PX, Cint(1),
	           0.87, 0.88, 0.91, 0.0, 0.0, 0.0, 0.0, mk, info) != 0
	if ok                     # world-sized from here on: the screen-constant rule stops applying to it
		ccall(_fn(:gmtvtk_symbol_set_world_size_h), Cint, (Ptr{Cvoid}, Cstring, Cdouble),
		      scene, mk, _sat_glyph_world(lift))
	end
	return ok
end

# The LAST point of the track: (lon, lat, z, alt_km, jd). Columns are groundtrack's
# (lon, lat, z, alt_km, time_jd) — the z here is the PLOTTED world-unit height, so the body lands at
# exactly the height of the line it stands on, in every view mode, flat map and globe alike.
#
# The segments are cut at the dateline (and at failed epochs) but stay in TIME order, so the last row
# of the last non-empty segment is the end of the propagated window — no search, and nothing that can
# put the body on a sample the propagator did not produce.
function _track_end_point(D::Vector{GMT.GMTdataset})
	for d in Iterators.reverse(D)
		m = d.data
		size(m, 1) == 0 && continue
		return ntuple(k -> m[size(m, 1), k], 5)
	end
	return nothing
end

# Tube radius for an orbit, in EARTH RADII — the unit the C side takes and the one the magnetic
# field-line dialog labels its own thickness box with (kCurveTubeR, 10_geometry.cpp, is that box.s
# own default of 0.006). An orbit is drawn THINNER than a field line on purpose: a track is a
# trajectory to be read against the map, not a flux tube to be looked at, and several satellites are
# often up at once. 0.0022 Earth radii is about 14 km.
const _TRACK_TUBE_R = 0.0022

# ORANGE by default: a track has to read against ocean, land, ice and the night side of a shaded
# globe alike, and black — the overlay default — disappears into bathymetry and into terrain shadow.
# `:orange` is a name colors.jl already defines; this does not introduce a colour of its own.
const _TRACK_COLOR = :orange

plot_groundtrack!(fig::QtFigure, s::Satellite; kw...) = plot_groundtrack!(fig.h, s; kw...)

# --- the one-day animation ------------------------------------------------------------------------
# "Animate 1 day" (the dialog, beside Update orbit) winds ONE satellite's LAST 24 hours onto the map, ending at NOW:
# the track GROWS from now, the spacecraft rides its head, and — when the box is ticked — the swath
# fills in behind it, so what the run really shows is how much of the planet that instrument sees in
# a day. Cumulative, never a moving window: the whole point is what is left at the end.
#
# THE HOST DRIVES THE CLOCK. A loop here would sit on the UI thread that pumps Qt (eventloop.jl) and
# freeze the window it is drawing into, so the dialog owns a QTimer and asks for ONE FRAME per tick.
# Everything expensive is done once, in `_anim_start`: the day is propagated, and the swath edges —
# the only costly part, two geodesic walks per sample — are walked for the WHOLE day and then sliced
# per frame. A frame is a redraw, never a recomputation.
const _ANIM_FRAMES  = 360        # frames for the whole day — 4 minutes of orbit each
const _ANIM_STEP_S  = 60         # track sampling, seconds: 1441 points over 24 h

# THE VIEW TURNS ABOUT THE EARTH'S OWN ROTATION AXIS, AND ONLY ABOUT IT. Over the animated day the
# camera makes exactly ONE turn westward — the planet rotating under an orbit plane that stands still
# in space — so every pass comes round onto a face that has turned to meet it, and the motion is a
# steady spin of about 10°/s of screen time. Nothing tumbles, nothing reverses.
#
# THE SUB-SATELLITE POINT IS NOT THE TARGET, and two attempts at making it one are the reason this is
# written down. A polar orbit's subpoint runs pole to pole and flips half the world in longitude
# twice per revolution: aimed straight at it the view spins like a top (15° to 45° per frame), and
# rate-limited it just wanders — measured on a real Aqua day, a median 90° off centre with the
# satellite behind the planet for half the run. The axis is the one thing in the picture that does
# not move; a turn about it is the only view change that reads as the Earth rotating.
const _ANIM_CAM_TURN_PER_DAY = 360.0    # degrees, westward: one rotation of the planet
# How far the camera may turn per 16 ms tick of its own glide (gmtvtk_globe_spin_to_h). The target
# only asks for ~0.17° per tick, so this is a ceiling that keeps a catch-up (the first frames, or a
# window the user has just dragged) from being a jump; it is never the rate in normal running.
const _ANIM_CAM_STEP = 1.0

mutable struct _SatAnim
	scene::Ptr{Cvoid}
	sat::Satellite
	name::String
	times::Vector{DateTime}
	altitude::Bool
	frame::Symbol
	bands::Vector{Tuple{Float64,Float64}}   # empty = the coverage box was not ticked
	# [segment][band] = (edgeA, edgeB) for the WHOLE day, in the segmentation of the whole day's
	# track. A frame's own track is a PREFIX of that track, so its segments are the first ones of
	# this list, the last one cut short — which is exactly a row slice of these matrices.
	edges::Vector{Vector{Tuple{Matrix{Float64},Matrix{Float64}}}}
	# HOW FAR EACH SEGMENT'S BAND IS ALREADY PAINTED, in rows. The band is CUMULATIVE: what is on the
	# map stays on the map, and a frame adds only the stretch flown since the last one — so a pass that
	# comes back over ground an earlier pass covered shows as two bands laid over each other, which is
	# the whole reason to watch a day of it. Nothing is ever removed mid-run.
	done::Vector{Int}
	npolys::Int                             # running count, for the polygons' own names
	lon0::Float64                           # the meridian the view starts on — where the satellite is
	clock::Int                              # the on-screen clock's annotation id (0 = none)
	bar::Int                                # …and its progress bar's
end

# THE CLOCK, on the map itself and not in the dialog: what is being watched is the globe, and a time
# that lives in a status line behind it is a time nobody reads. It is a MOVIE ANNOTATION — the same
# `gmtvtk_anno_*` label and progress indicator the Make-movie tool lays on a frame (movieanno.jl), so
# an animation's clock is not a second kind of on-screen text with its own placement rules.
# Justification 0 = top-left for the label; the bar is the movie tool's own default corner (TR).
function _anim_clock_add!(scene::Ptr{Cvoid})
	white = [1.0, 1.0, 1.0];  black = [0.0, 0.0, 0.0]
	add(name, isprog, style, just, width) =
		Int(ccall(_fn(:gmtvtk_anno_add_h), Cint,
		          (Ptr{Cvoid}, Cstring, Cint, Cint, Cint, Cint, Cint,
		           Cdouble, Cdouble, Cdouble, Cdouble, Ptr{Cdouble},
		           Cint, Ptr{Cdouble}, Cint, Ptr{Cdouble}, Cdouble,
		           Cdouble, Cint, Cint, Ptr{Cdouble}, Cdouble, Cint, Ptr{Cdouble}, Cdouble),
		          scene, name, Cint(isprog), Cint(3), Cint(style), Cint(0), Cint(just),
		          0.0, 0.0, width, 0.0, black,
		          Cint(1), white, Cint(0), black, 0.0,
		          0.0, Cint(1),
		          Cint(0), black, 0.0, Cint(0), black, 0.0))
	return (add("Animation clock", 0, 0, 0, 0.0), add("Animation progress", 1, 0, 2, 0.0))
end

function _anim_clock_drop!(scene::Ptr{Cvoid}, ids...)
	for id in ids
		id > 0 && ccall(_fn(:gmtvtk_anno_remove_h), Cint, (Ptr{Cvoid}, Cint), scene, Cint(id))
	end
	return nothing
end

_anim_clock_set!(scene::Ptr{Cvoid}, id::Int, txt::String, frac::Float64) =
	id > 0 && ccall(_fn(:gmtvtk_anno_set_h), Cint, (Ptr{Cvoid}, Cint, Cstring, Cdouble, Cint),
	                scene, Cint(id), txt, frac, Cint(1))

# The Scene Objects tree is HELD STILL for the whole run and rebuilt once at the end (the thaw does
# it). A run adds a swath piece and re-lays its track on every one of 360 frames, and a panel rebuilt
# that often strobes — the user asked for the handles to settle only when the run does.
_anim_freeze!(scene::Ptr{Cvoid}, on::Bool) =
	ccall(_fn(:gmtvtk_freeze_scene_objects_h), Cint, (Ptr{Cvoid}, Cint), scene, Cint(on))

# ONE animation at a time, for the same reason there is one track per satellite: two of them would be
# two clocks writing the same rows. Starting a second replaces the first.
const _SAT_ANIM = Ref{Union{Nothing,_SatAnim}}(nothing)

function _anim_stop!()
	a = _SAT_ANIM[]
	a === nothing && return nothing
	_SAT_ANIM[] = nothing
	# The clock goes with the run that owns it, and the tree is let go LAST — its one rebuild is what
	# finally shows the rows everything above added, so it has to happen after the clock is gone or
	# the panel would be built around an element about to disappear.
	try; _anim_clock_drop!(a.scene, a.clock, a.bar); catch; end
	# The scratch group is a working surface, not a result: whatever segment was still growing when
	# the run ended is settled into the band proper first, so stopping mid-run leaves ONE group with
	# everything drawn in it and no half-named leftovers.
	try
		if !isempty(a.bands)
			grp  = _sat_cover_group(a.name)
			live = grp * _ANIM_LIVE_SUFFIX
			ccall(_fn(:gmtvtk_remove_polys_h), Cint, (Ptr{Cvoid}, Cstring), a.scene, live)
			for (i, e) in enumerate(a.edges)
				a.done[i] <= 0 && continue               # settled already, or never reached
				r = a.done[i]
				for (A, B) in e, R in _swath_rings(A[1:r, :], B[1:r, :])
					a.npolys += 1
					_add_swath_ring!(a.scene, grp, R, a.npolys)
				end
			end
			_cover_group_master!(a.scene, a.name; unfold = false)
		end
	catch
	end
	try; _anim_freeze!(a.scene, false); catch; end
	try; close!(a.sat); catch; end        # the handle is ours; the finalizer is not predictable
	return nothing
end

"""
Set up the one-day run for `tle` and return the number of frames the host should ask for.
Propagates the day once and walks the swath edges once; draws nothing.
"""
function _anim_start!(scene::Ptr{Cvoid}, tle::TLE, altitude::Bool, frame::Symbol,
                      bands::Vector{Tuple{Float64,Float64}})::Int
	_anim_stop!()
	s  = Satellite(tle)
	nm = isempty(s.tle.name) ? string(norad_number(s)) : s.tle.name
	# THE DAY THAT HAS JUST HAPPENED, not the one to come: the run ENDS AT NOW, so its last frame is
	# the satellite where it actually is at this instant — the same anchoring "Update orbit" and the
	# dialog's "Now (UTC)" span use, and the only one for which the spacecraft standing on the head of
	# the track is a statement of fact rather than a prediction.
	t1 = _sat_now_utc()
	times = collect((t1 - Hour(24)):Second(_ANIM_STEP_S):t1)
	edges = Vector{Vector{Tuple{Matrix{Float64},Matrix{Float64}}}}()
	if !isempty(bands)
		# The whole day's track, in the SAME call every frame will make — so the segmentation the
		# edges are indexed by is the segmentation the frames will see.
		D = groundtrack(s, times; altitude = altitude, frame = frame)
		for d in D
			m = d.data
			lon = Vector{Float64}(m[:,1])
			lat = Vector{Float64}(m[:,2])
			push!(edges, [_swath_edges(lon, lat, a, b) for (a, b) in bands])
		end
	end
	# A NEW RUN STARTS FROM BARE GROUND: whatever band this satellite already had is this run's first
	# frame's business, not something to draw the day's second pass on top of.
	isempty(bands) ||
		ccall(_fn(:gmtvtk_remove_polys_h), Cint, (Ptr{Cvoid}, Cstring), scene, _sat_cover_group(nm))
	# The meridian the view opens on: where the satellite is when the run starts. From `subpoint` —
	# the same sub-satellite point everything else here is built on, never a second derivation.
	lo, _, _ = subpoint(s, [times[1]])
	clock, bar = _anim_clock_add!(scene)
	_anim_freeze!(scene, true)             # …and the panel settles when the run does, not 360 times
	_SAT_ANIM[] = _SatAnim(scene, s, nm, times, altitude, frame, bands, edges,
	                       zeros(Int, length(edges)), 0, isempty(lo) ? 0.0 : lo[1], clock, bar)
	return _ANIM_FRAMES
end

"""
Draw frame `k` (1-based, `k == _ANIM_FRAMES` is the last): the track from the start of the day up to
this frame's instant, the spacecraft on its head, and the swath behind it when one was asked for.
Returns the UTC stamp of that instant, for the dialog's status line.
"""
function _anim_frame!(k::Int)::String
	a = _SAT_ANIM[]
	a === nothing && error("no animation is running")
	n = length(a.times)
	last = (k >= _ANIM_FRAMES)
	m = clamp(round(Int, k * n / _ANIM_FRAMES), 2, n)
	# THE LAST FRAME IS DRAWN THAWED, so the rows it leaves behind — the track's, the swath's, the
	# data table, the unfold — are built by the same code any other plot goes through. Thawing after
	# it would rebuild the panel around an unfold that had nothing to unfold.
	last && _anim_freeze!(a.scene, false)
	D = groundtrack(a.sat, a.times[1:m]; altitude = a.altitude, frame = a.frame)
	_plot_track!(a.scene, D, a.name; sat = a.sat, live = !last)
	if !isempty(a.bands)
		# ONE RIBBON PER TRACK SEGMENT, exactly as the menu's own band is built — never a patch per
		# frame. A per-frame patch was tried and is wrong twice over: it left 455 polygons for a day
		# (a handle, an actor and a mapper each, for what is one band), and the short ones that
		# happened to straddle a pole were 4-to-12-point rings spanning up to 276° of longitude,
		# which triangulate into sheets cutting clean through the planet — the patches seen floating
		# in the equatorial plane. A whole segment's ribbon has none of that: it is the same ring the
		# static path draws, pole stitch and all.
		#
		# So a segment is RE-LAID WHOLE while it grows, in a scratch group of its own that is thrown
		# away and rebuilt each frame, and is moved into the band's real group ONCE, when the track
		# has passed beyond it and its shape can no longer change. Only ever one segment is live, so
		# a frame costs one removal and one or two polygons — and the day ends with ~20, the count
		# the non-animated band has.
		grp  = _sat_cover_group(a.name)
		live = grp * _ANIM_LIVE_SUFFIX
		ccall(_fn(:gmtvtk_remove_polys_h), Cint, (Ptr{Cvoid}, Cstring), a.scene, live)
		added = false
		nlive = 0
		for (i, d) in enumerate(D)
			i > length(a.edges) && break                 # a segmentation this frame grew past
			a.done[i] < 0 && continue                    # settled: its shape is final, leave it alone
			rfull = size(a.edges[i][1][1], 1)
			r = min(size(d.data, 1), rfull)
			r < 2 && continue
			settle = (r >= rfull)                        # the track has moved past this segment
			for (A, B) in a.edges[i]
				for R in _swath_rings(A[1:r, :], B[1:r, :])
					if settle
						a.npolys += 1
						_add_swath_ring!(a.scene, grp, R, a.npolys) && (added = true)
					else
						nlive += 1
						_add_swath_ring!(a.scene, live, R, nlive)
					end
				end
			end
			a.done[i] = settle ? -1 : r
		end
		added && _cover_group_master!(a.scene, a.name; unfold = last)
	end
	# TURN THE PLANET UNDER THE ORBIT. The meridian the camera looks down walks steadily westward, one
	# full turn over the animated day — the Earth's own rotation, and nothing else (see
	# `_ANIM_CAM_TURN_PER_DAY`, and `gmtvtk_globe_spin_to_h` for why it is not aimed at the satellite).
	# The FIRST frame goes there at once: a run should open on its subject, not creep towards it from
	# wherever the window happened to be pointing. Camera only — a vector plot never touches the axes
	# (SACRED_LAW.md) — and a no-op off the globe.
	let elapsed = (a.times[m] - a.times[1]).value / 86_400_000,       # Millisecond period -> days
	    lonc = a.lon0 - _ANIM_CAM_TURN_PER_DAY * elapsed
		ccall(_fn(:gmtvtk_globe_spin_to_h), Cint, (Ptr{Cvoid}, Cdouble, Cdouble),
		      a.scene, lonc, k <= 1 ? 0.0 : _ANIM_CAM_STEP)
	end
	# THE NIGHT SIDE DOES NOT WALK WITH THE CLOCK YET, and that is deliberate, not an oversight: on the
	# globe one repaint of it measures 3.76 s (0.163 s on a flat map), because the night region is a
	# hemisphere-sized POLYGON whose pole-closing edge is 360° long and `globeDensifyPD` subdivides it
	# 1->4 up to eight times to bend it onto the sphere. Thirty repaints in a row took the process out.
	# A terminator that moves with the animation needs the night side to stop being a polygon — see the
	# note at `_daynight_paint!` (solar.jl).
	# THE CLOCK, on the map: the instant being drawn, and how far through the day the run is.
	_anim_clock_set!(a.scene, a.clock,
	                 GMT.Dates.format(a.times[m], "yyyy-mm-dd HH:MM:SS") * " UTC", k / _ANIM_FRAMES)
	_anim_clock_set!(a.scene, a.bar, "", k / _ANIM_FRAMES)
	last && _anim_stop!()
	return GMT.Dates.format(a.times[m], "yyyy-mm-dd HH:MM") * " UTC"
end

# --- ground coverage: the swath the instrument actually sees --------------------------------------
# Clicking the spacecraft (symbolLayerMenu, 50_scene.cpp) toggles a grey, half-transparent band on the
# ground: the strip of Earth the satellite's main instrument images along the track that is plotted.
#
# THE TRACK IS THE TRACK. The band is built from the ground track already on screen (`_SAT_TRACKS`),
# never from a second propagation: one satellite, one answer to where it was (SACRED_LAW.md).
#
# The plotted track per window+satellite, kept by `_plot_track!`. Key = (window, name): the same
# satellite may be up in two windows over two different spans, and each band must follow its own.
const _SAT_TRACKS = Dict{Tuple{UInt,String},Vector{GMT.GMTdataset}}()

# The polygons' Scene Objects group tag, and the name BOTH SIDES build: the C++ menu reads it to know
# whether this satellite's band is already on screen (that is the checkbox), and removes it by the
# same tag. ONE convention, written here and mirrored at exactly one place in 50_scene.cpp.
_sat_cover_group(nm::String)::String = nm * " swath"

# The scratch group the animation re-lays the GROWING segment into, thrown away and rebuilt every
# frame. It never outlives a run (`_anim_stop!` drops it) and is never seen: the Scene Objects tree
# is frozen while the run is on, and by the time it is rebuilt this group is gone.
const _ANIM_LIVE_SUFFIX = " (in progress)"

# PUBLISHED SWATH WIDTHS, per mission, as SIGNED ACROSS-TRACK OFFSETS in km: each entry is a list of
# (near, far) bands, negative = left of the flight direction. A plain imager is ONE symmetric band;
# SWOT's KaRIn is TWO, because it measures on both sides of a 20 km nadir gap and a single 120 km
# band would claim coverage it does not have.
#
# The number is the instrument's own nadir swath — the ground arc, which is what a map wants. Where a
# platform carries several instruments the entry is the one that defines its coverage (Terra/Aqua =
# MODIS, not ASTER's 60 km; Sentinel-3 = OLCI, not SLSTR's 1420 km; Sentinel-1 = the IW mode the
# mission acquires land and coast in).
#
# Matching is by PREFIX of the normalised name, longest first, so the series entry serves every member
# of it: "SENTINEL 2" covers 2A/2B/2C, "LANDSAT 8" is its own because 8 and 9 are named separately in
# the element sets. A mission that is not here is NOT guessed at — the menu asks for the width.
const _SAT_SWATH = Tuple{String,Vector{Tuple{Float64,Float64}},String}[
	("TERRA",        [(-1165.0, 1165.0)], "MODIS, 2330 km"),
	("AQUA",         [(-1165.0, 1165.0)], "MODIS, 2330 km"),
	("LANDSAT 8",    [( -92.5,   92.5)],  "OLI/TIRS, 185 km"),
	("LANDSAT 9",    [( -92.5,   92.5)],  "OLI/TIRS, 185 km"),
	("SENTINEL 1",   [(-125.0,  125.0)],  "SAR interferometric wide, 250 km"),
	("SENTINEL 2",   [(-145.0,  145.0)],  "MSI, 290 km"),
	("SENTINEL 3",   [(-635.0,  635.0)],  "OLCI, 1270 km"),
	("SENTINEL 5",   [(-1300.0, 1300.0)], "TROPOMI, 2600 km"),
	("SENTINEL 6",   [(  -6.0,    6.0)],  "Poseidon-4 altimeter footprint, 12 km"),
	("SUOMI NPP",    [(-1530.0, 1530.0)], "VIIRS, 3060 km"),
	("NOAA 20",      [(-1530.0, 1530.0)], "VIIRS, 3060 km"),
	("NOAA 21",      [(-1530.0, 1530.0)], "VIIRS, 3060 km"),
	("JPSS",         [(-1530.0, 1530.0)], "VIIRS, 3060 km"),
	("SWOT",         [(-60.0, -10.0), (10.0, 60.0)], "KaRIn, 2 x 50 km either side of a 20 km gap"),
]

# Element-set names are written every way ("NOAA 20", "NOAA-20", "SUOMI-NPP"), so the key and the name
# are compared in ONE normalised form: upper case, separators collapsed to single blanks.
function _sat_norm(s::String)::String
	t = uppercase(strip(s))
	t = replace(t, '-' => ' ', '_' => ' ', '(' => ' ', ')' => ' ')
	return strip(replace(t, r"\s+" => " "))
end

# The bands + the label for a satellite name, or `nothing` when the mission is not in the table above.
function _swath_for(nm::String)
	n = _sat_norm(nm)
	best = nothing
	for (k, bands, label) in _SAT_SWATH
		startswith(n, k) || continue
		(best === nothing || length(k) > length(best[1])) && (best = (k, bands, label))
	end
	return best === nothing ? nothing : (best[2], best[3])
end

# One point offset ACROSS TRACK by `dkm` (signed: + right of the flight direction, - left) from the
# sub-satellite point at `(lon, lat)` whose forward azimuth is `az`. The step is `GMT.geod` — the same
# direct geodesic deform.jl and computeeuler.jl walk with; no sphere formula is written here.
function _geod_off(lon::Float64, lat::Float64, az::Float64, dkm::Float64)
	dkm == 0.0 && return (lon, lat)
	dest, = GMT.geod([lon, lat], az + (dkm >= 0.0 ? 90.0 : -90.0), abs(dkm); unit = :km)
	return (Float64(dest[1]), Float64(dest[2]))
end

# The forward azimuth AT each track point (the last one inherits the previous leg's, there being no
# next point to aim at). ONE `GMT.invgeod` call for the whole track — it is vectorised over rows.
function _track_azimuths(lon::Vector{Float64}, lat::Vector{Float64})::Vector{Float64}
	n = length(lon)
	_, a1, _ = GMT.invgeod(hcat(lon[1:n-1], lat[1:n-1]), hcat(lon[2:n], lat[2:n]))
	az1 = a1 isa Real ? Float64[a1] : Vector{Float64}(a1)
	az = Vector{Float64}(undef, n)
	az[1:n-1] = az1
	az[n] = az1[end]
	return az
end

# Clip a ring against the half-plane x <= c (`keepLE`) or x >= c — Sutherland-Hodgman against ONE
# edge, which is all the dateline ever needs. Returns the (possibly empty) clipped ring.
function _clip_x(P::Matrix{Float64}, c::Float64, keepLE::Bool)::Matrix{Float64}
	n = size(P, 1)
	n < 3 && return zeros(0, 2)
	inside(x) = keepLE ? x <= c : x >= c
	out = NTuple{2,Float64}[]
	for i in 1:n
		j = (i == n) ? 1 : i + 1
		xi, yi = P[i,1], P[i,2]
		xj, yj = P[j,1], P[j,2]
		ii, ij = inside(xi), inside(xj)
		ii && push!(out, (xi, yi))
		if ii != ij && xj != xi
			t = (c - xi) / (xj - xi)
			push!(out, (c, yi + t * (yj - yi)))
		end
	end
	isempty(out) && return zeros(0, 2)
	R = Matrix{Float64}(undef, length(out), 2)
	for (k, p) in enumerate(out)
		R[k,1] = p[1];  R[k,2] = p[2]
	end
	return R
end

# Cut a ring whose longitudes run past ±180 into the pieces that live on the map, each shifted back
# into [-180,180]. Same duty `_split_dateline` does for the track line: a band that runs from +179 to
# -179 streaks straight across the map unless it is cut, and a filled band streaks as a painted bar.
function _wrap_pieces(P::Matrix{Float64})::Vector{Matrix{Float64}}
	out = Matrix{Float64}[]
	stack = Matrix{Float64}[P]
	guard = 0
	while !isempty(stack) && (guard += 1) < 64
		Q = pop!(stack)
		size(Q, 1) < 3 && continue
		lo, hi = extrema(view(Q, :, 1))
		if hi > 180.0 || lo < -180.0
			c = hi > 180.0 ? 180.0 : -180.0
			keep = _clip_x(Q, c, hi > 180.0)          # the half already on the map
			over = _clip_x(Q, c, !(hi > 180.0))       # …and the half that has run off its edge
			size(keep, 1) >= 3 && push!(stack, keep)
			if size(over, 1) >= 3
				over[:,1] .+= (hi > 180.0) ? -360.0 : 360.0
				push!(stack, over)
			end
			continue
		end
		push!(out, Q)
	end
	return out
end

# ONE band of the swath along ONE track segment, as closed rings ready to paint: the two edges walked
# out to `d0`/`d1` km either side of each sub-satellite point, joined into a ribbon.
#
# THE RIBBON IS BUILT WHOLE, IN CONTINUOUS LONGITUDE, and cut at the very end — never assembled piece
# by piece. A sun-synchronous descending node can run NEARLY PARALLEL to the dateline for a quarter of
# a pass, and a builder that ends the ribbon at each sample whose edge steps off the map turns that
# stretch into one four-corner scrap per sample (measured: 40 polygons for 21 track points). Cut once,
# the same stretch is the two halves it really is.
#
# The unwrap is about the TRACK's own longitude, point by point: an edge is at most a half swath from
# the sub-satellite point it belongs to, so "the representation nearest the centre line" is always the
# true one — including where a wide scanner sweeps PAST A POLE and its edge genuinely does jump to the
# far side of the world.
#
# A POLE IS WALKED THROUGH, NOT JUMPED OVER (`_stitch_pole` below).
_stitch_pole(E::Matrix{Float64})::Matrix{Float64} = begin
	n = size(E, 1)
	out = NTuple{2,Float64}[]
	for i in 1:n
		push!(out, (E[i,1], E[i,2]))
		i == n && continue
		# The signature of a pole crossing, and nothing else: half the world in longitude between two
		# consecutive samples, both of them close to the same pole. At 84° N, 120° of longitude is
		# 1400 km — a step no sampling of an orbit produces.
		(abs(E[i+1,1] - E[i,1]) > 120.0 && min(abs(E[i,2]), abs(E[i+1,2])) > 60.0) || continue
		p = (E[i,2] >= 0.0 ? 90.0 : -90.0)
		push!(out, (E[i,1],   p))          # up the meridian to the pole…
		push!(out, (E[i+1,1], p))          # …across it (one point on the globe) and down the far side
	end
	R = Matrix{Float64}(undef, length(out), 2)
	for (k, q) in enumerate(out)
		R[k,1] = q[1];  R[k,2] = q[2]
	end
	R
end

# THE TWO EDGES of a band, as (near, far) matrices of (lon, lat) — the only expensive half of the
# job (two geodesic walks per track point). Split out from the ring builder below because the one-day
# ANIMATION redraws the same band 120 times over a growing prefix of the same track: the edges are
# walked ONCE for the whole day and sliced per frame, never re-walked. Same function, one set of
# numbers — the animated band and the static one cannot come out different.
function _swath_edges(lon::Vector{Float64}, lat::Vector{Float64}, d0::Float64, d1::Float64)
	n = length(lon)
	n < 2 && return (zeros(0,2), zeros(0,2))
	az = _track_azimuths(lon, lat)
	A = Matrix{Float64}(undef, n, 2)
	B = Matrix{Float64}(undef, n, 2)
	c = copy(lon)                                  # the track's own longitude, made continuous
	for i in 2:n
		c[i] += 360.0 * round((c[i-1] - c[i]) / 360.0)
	end
	for i in 1:n
		A[i,1], A[i,2] = _geod_off(lon[i], lat[i], az[i], d0)
		B[i,1], B[i,2] = _geod_off(lon[i], lat[i], az[i], d1)
		A[i,1] += 360.0 * round((c[i] - A[i,1]) / 360.0)
		B[i,1] += 360.0 * round((c[i] - B[i,1]) / 360.0)
	end
	return (A, B)
end

# The closed rings of a band, from its two edges.
#
# A WIDE SCANNER REACHES OVER THE POLE: at 81.8° N a 1530 km half swath lands at 95.6°, which is
# 84.4° on the FAR side — the edge really does step half the world in longitude. Left as a bare
# step, the polygon's boundary runs straight along that latitude instead of over the pole, so the
# cap above it falls OUTSIDE the ring (the round hole in the band) and the ±180 cut below then
# slices the ribbon in two along a meridian (the gap). Both are the same missing vertex pair.
function _swath_rings(A::Matrix{Float64}, B::Matrix{Float64})::Vector{Matrix{Float64}}
	size(A, 1) < 2 && return Matrix{Float64}[]
	return _wrap_pieces(vcat(_stitch_pole(A), _stitch_pole(B)[end:-1:1, :]))   # out along A, back along B
end

_swath_polys(lon::Vector{Float64}, lat::Vector{Float64}, d0::Float64, d1::Float64)::Vector{Matrix{Float64}} =
	_swath_rings(_swath_edges(lon, lat, d0, d1)...)

# GREY, HALF TRANSPARENT: the band is a mask over the map, not a layer with data of its own — the
# geography under it has to stay readable, which is the whole reason it is not painted solid.
const _SWATH_RGB = (0.5, 0.5, 0.5)
const _SWATH_OPACITY = 0.55        # 45 % transparency

"""
Paint `nm`'s ground coverage into `scene` as one Scene Objects group (`_sat_cover_group(nm)`), under
the "Satellites" master beside the track it belongs to. `bands` are signed across-track (near, far)
offsets in km. Returns the number of polygons painted.

A vector overlay landing on a map that is already framed: it does NOT reframe and does not spawn axes
of its own (SACRED_LAW.md, vector-import law).
"""
#
# THE ONE PAINTER. Both callers — the menu's toggle and every frame of the one-day animation — hand
# their rings to this, so a band drawn while the animation runs is the same object, the same colour
# and the same group as the one the toggle leaves behind.
# ONE RING ONTO THE MAP. Every band polygon this file draws — the menu's toggle, and every frame of
# the animation — is added HERE, so they are the same colour, the same opacity and the same group.
function _add_swath_ring!(scene::Ptr{Cvoid}, grp::String, R::Matrix{Float64}, idx::Int)::Bool
	size(R, 1) < 3 && return false
	xyz = vec(permutedims(hcat(R, zeros(size(R, 1)))))
	ccall(_fn(:gmtvtk_add_poly_full), Cint,
	      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cint, Cint, Cdouble, Cdouble, Cdouble,
	       Cdouble, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cstring, Cstring),
	      scene, xyz, Cint(size(R, 1)), Cint(1), Cint(0),
	      _SWATH_RGB[1], _SWATH_RGB[2], _SWATH_RGB[3], 0.0, Cint(0),
	      _SWATH_RGB[1], _SWATH_RGB[2], _SWATH_RGB[3], _SWATH_OPACITY,
	      string(grp, " (", idx, ")"), grp)
	return true
end

# The band's group belongs to the SATELLITE, and the panel is opened on it once it exists.
function _cover_group_master!(scene::Ptr{Cvoid}, nm::String; unfold::Bool = true)
	grp = _sat_cover_group(nm)
	ccall(_fn(:gmtvtk_set_group_master_h), Cint, (Ptr{Cvoid}, Cstring, Cstring), scene, grp, nm)
	unfold && ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
	return nothing
end

function _paint_coverage!(scene::Ptr{Cvoid}, nm::String, rings::Vector{Matrix{Float64}};
                          unfold::Bool = true)::Int
	grp = _sat_cover_group(nm)
	# REPLACE, never pile up — the same rule the track itself follows on a re-plot. (The ANIMATION
	# does NOT come through here: a run that repainted the whole band every frame would blink the
	# band off and redraw forty polygons per tick, which is exactly how it looked. It ADDS the new
	# stretch only — `_anim_frame!`.)
	ccall(_fn(:gmtvtk_remove_polys_h), Cint, (Ptr{Cvoid}, Cstring), scene, grp)
	n = 0
	for R in rings
		_add_swath_ring!(scene, grp, R, n + 1) && (n += 1)
	end
	n == 0 && return 0
	_cover_group_master!(scene, nm; unfold = unfold)
	return n
end

function _plot_coverage!(scene::Ptr{Cvoid}, nm::String, bands::Vector{Tuple{Float64,Float64}})::Int
	D = get(_SAT_TRACKS, (UInt(scene), nm), nothing)
	D === nothing && error("no ground track for \"$nm\" in this window — plot the satellite first")
	rings = Matrix{Float64}[]
	for d in D
		m = d.data
		size(m, 1) < 2 && continue
		lon = Vector{Float64}(m[:,1])
		lat = Vector{Float64}(m[:,2])
		for (a, b) in bands
			append!(rings, _swath_polys(lon, lat, a, b))
		end
	end
	# THE BAND IS PART OF THE SATELLITE, not a plot of its own: `_paint_coverage!` declares its master
	# to be the SATELLITE'S OWN NAME, so its row lands inside that satellite's group as a BROTHER of
	# Track and Spacecraft — never a cousin one level up under "Satellites".
	n = _paint_coverage!(scene, nm, rings)
	n == 0 && error("the track of \"$nm\" is too short to give it a swath")
	return n
end

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

# --- the downloaded-TLE cache ---------------------------------------------------------------------
# CELESTRAK RATE-LIMITS, AND A TLE DOES NOT CHANGE BY THE MINUTE. The curated preset alone pulls FIVE
# files every time it is picked (its missions are spread across `resource`, `weather`, `goes`, `geo`
# and a NAME query), so re-opening the dialog a few times is enough to be answered 403 — which is
# what happened. Elements are issued a couple of times a day and are good for days either side of
# their epoch, so a download held for two hours costs accuracy nothing and costs the service five
# requests instead of fifty.
#
# The cache is also the ANSWER TO BEING RATE-LIMITED: when a fetch fails, a stale copy is used and
# the user is told how old it is. A failed download with a perfectly good yesterday's file on disk
# helps nobody.
const _TLE_CACHE_DIR = joinpath(tempdir(), "iGMT_tle")
const _TLE_CACHE_SECS = 2 * 3600.0

# One file per URL, named by a hash of it — a URL is not a filename (query strings, '?', '&', '/').
_tle_cache_file(url::String)::String =
	joinpath(_TLE_CACHE_DIR, string(hash(url), base = 16) * ".tle")

"""
The text of `url`, from the cache when the copy there is younger than `_TLE_CACHE_SECS`, otherwise
downloaded and cached. A download that fails falls back to whatever is on disk, however old.
"""
function _tle_fetch(url::String)::String
	f = _tle_cache_file(url)
	if isfile(f)
		age = time() - mtime(f)
		age < _TLE_CACHE_SECS && return read(f, String)
	end
	try
		io = IOBuffer()
		Downloads.download(url, io)
		txt = String(take!(io))
		# Write only something that looks like a TLE set: a 403 or an error page is not a cache entry,
		# and caching one would keep serving the failure for two hours.
		if occursin(r"(?m)^1 \d{5}", txt)
			try
				mkpath(_TLE_CACHE_DIR)
				write(f, txt)
			catch                       # a cache that cannot be written is not a reason to fail
			end
		end
		return txt
	catch e
		isfile(f) || rethrow()
		@warn string("Satellite: the download failed (", sprint(showerror, e),
		             ") — using the copy cached ", round(Int, (time() - mtime(f)) / 60), " min ago.")
		return read(f, String)
	end
end

# Read the source named by the dialog's radio pair. A URL is fetched (through the cache above) and
# parsed by the same `read_tle` a file goes through, so there is one parser, not two.
function _sat_read_source(d::Dict{String,String})::Vector{TLE}
	src = get(d, "src", "file")
	if src == "url"
		url = get(d, "url", "")
		isempty(url) && error("give me a URL that returns TLE text")
		# SEVERAL URLs, joined by '|', are read as one list. Celestrak splits its catalogue across
		# per-purpose files — the Earth-observation group has no GOES and no Meteosat in it, those live
		# in `goes` and `geo` — so a curated list that spans them has to name more than one file. The
		# alternative, pulling the whole `active` catalogue and throwing 99 % of it away, is 1.5 MB per
		# refresh and gets the user rate-limited (Celestrak answers 403), which is not a thing a preset
		# may do to someone. '|' is not a legal URL character, so it cannot split a real one.
		tles = TLE[]
		for u in split(url, '|')
			u = strip(u)
			isempty(u) && continue
			# Through the cache (above): the same five files re-read every time the preset is picked is
			# what got this rate-limited in the first place.
			append!(tles, read_tle(_tle_fetch(String(u))))
		end
		return _sat_filter(tles, d)
	end
	path = get(d, "path", "")
	isempty(path) && error("pick a TLE file first")
	isfile(path)  || error("no such file: " * path)
	return _sat_filter(read_tle(read(path, String)), d)
end

# The curated preset's name filter (`filter=` in the dialog's block, 70_window.cpp): a comma-separated
# list of NAME PREFIXES, kept in the order the FILTER names them so the short list reads as it was
# written rather than as the download happens to be ordered. Applied HERE, in the one reader both
# `list` and `plot` go through, which is what keeps the dialog's row indices and this vector the same
# thing — filtering in the lister alone would have `plot` indexing the unfiltered set.
#
# A prefix ON A WORD BOUNDARY, never a bare substring: the name must continue with something that is
# not a letter or a digit, or stop there. "TERRA" then takes TERRA and not TERRASAR-X, "AQUA" takes
# AQUA and not AQUARIUS, while "SENTINEL" still takes SENTINEL-1A and "GOES" takes GOES 16.
_sat_name_hit(name::String, w::String)::Bool =
	startswith(name, w) && (length(name) == length(w) || !isletter(name[length(w)+1]) && !isdigit(name[length(w)+1]))

function _sat_filter(tles::Vector{TLE}, d::Dict{String,String})::Vector{TLE}
	want = [uppercase(strip(x)) for x in split(get(d, "filter", ""), ',') if !isempty(strip(x))]
	isempty(want) && return tles
	out = TLE[]
	for w in want, t in tles
		_sat_name_hit(uppercase(t.name), w) && !(t in out) && push!(out, t)
	end
	isempty(out) && error("none of the wanted missions is in that list: " * join(want, ", "))
	return out
end

# A stable key for _SAT_LOADED: whichever of the two source fields is actually in use.
_sat_key(d::Dict{String,String})::String =
	get(d, "src", "file") == "url" ? "url:" * get(d, "url", "") : "file:" * get(d, "path", "")

function _on_satellite(scene::Ptr{Cvoid}, params::Cstring, out::Ptr{UInt8}, cap::Cint)::Cint
	what = ""                      # hoisted: the catch below needs to know WHICH request failed
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
			# The combo sends the WORD (70_window.cpp); anything unknown falls back to the automatic
			# rule rather than erroring, because a frame is a view choice and never a reason not to plot.
			fw = get(d, "frame", "auto")
			frame = fw == "earthfixed" ? :earthfixed : fw == "inertial" ? :inertial : :auto

			done = String[]
			nupd = 0
			rep  = Ref(0)
			# The instant the last plotted track ENDS at — what the night side is drawn for, below.
			tend = Ref(_sat_now_utc())
			for i in sel
				(i < 0 || i >= length(tles)) && continue
				s = Satellite(tles[i+1])
				try
					# THE TRACK ENDS AT THE ANCHOR, and the spacecraft stands on its LAST point
					# (`_plot_sat_now!`). Anchored at "now" the span therefore runs BACKWARD — the orbit
					# just flown, ending exactly where the satellite is at this instant, which is the
					# whole point of drawing the body. (Running it forward put the body at a future
					# position it is not at yet, and the track nowhere near "now".) Anchored at the TLE's
					# own epoch it still runs forward from there: that anchor means "the revolution this
					# element set describes", and its last point is the satellite at the end of it.
					t0  = useNow ? _sat_now_utc() : epoch(s)
					dur = mode == "minutes" ? Minute(round(Int, spanv)) :
					      mode == "hours"   ? Minute(round(Int, spanv * 60)) :
					                          _revs_duration(s, spanv)
					kw  = useNow ? (; start = t0 - dur, stop = t0) : (; start = t0, stop = t0 + dur)
					tend[] = useNow ? t0 : t0 + dur
					nm = isempty(s.tle.name) ? string(norad_number(s)) : s.tle.name
					D  = groundtrack(s; step = step, altitude = useAlt, frame = frame, kw...)   # propagated ONCE, used twice
					if _plot_track!(scene, D, nm; replaced = rep, sat = s)
						push!(done, nm);  nupd += rep[]
					end
				finally
					close!(s)          # the finalizer would get it, but not predictably
				end
			end
			isempty(done) && error("nothing could be plotted")
			# The night side belongs to the INSTANT the track ends at — which is where the spacecraft is
			# standing. Re-derived on every plot so "Update orbit" moves the terminator too; only when
			# the window is already showing it.
			if _daynight_on(scene)
				try; _daynight_paint!(scene, tend[]); catch; end
			end
			msg = (nupd > 0 ? "Updated: " : "Plotted: ") * join(done, ", ")
			_sat_reply(out, cap, msg)
			return Cint(1)
		end

		# GROUND COVERAGE — asked by the click on the spacecraft itself (symbolLayerMenu, 50_scene.cpp).
		# Only the PAINT half is here: the band is toggled OFF on the C++ side, which can see whether
		# the polygons are on screen and needs no round trip to delete what it is already holding.
		# `width` (km, the full swath) arrives only for a mission the table does not carry, after the
		# menu has asked the user for it — a swath is a published instrument number, never a guess.
		if what == "coverage"
			scene == C_NULL && error("no window")
			nm = get(d, "name", "")
			isempty(nm) && error("no satellite named")
			w = _sat_num(get(d, "width", ""), 0.0)
			bands, label = if w > 0
				([(-w / 2, w / 2)], string(round(w, digits = 1), " km"))
			else
				sw = _swath_for(nm)
				# The one answer that is not a failure: the menu turns it into the width question.
				sw === nothing && (_sat_reply(out, cap, "NEEDWIDTH"); return Cint(1))
				sw
			end
			n = _plot_coverage!(scene, nm, bands)
			_sat_reply(out, cap, string("Ground coverage: ", nm, " — ", label,
			                            " (", n, n == 1 ? " polygon)" : " polygons)"))
			return Cint(1)
		end

		# DAY / NIGHT on the Earth — the dialog's own checkbox. The maths is the Sun tool's night region
		# (solar.jl's `_daynight_paint!`), asked for ONE instant; nothing about where the dark side is
		# is worked out here. `when` is an ISO stamp, or absent for this instant — which is the hook a
		# date picker needs later and the reason the time is a parameter rather than a call to `now`
		# buried inside the painter.
		if what == "daynight"
			scene == C_NULL && error("no window")
			if get(d, "on", "0") != "1"
				_daynight_clear!(scene)
				_sat_reply(out, cap, "Day/night off.")
				return Cint(1)
			end
			ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0 &&
				error("open a map first — there is nothing to darken")
			w = strip(get(d, "when", ""))
			t = isempty(w) ? _sat_now_utc() : DateTime(String(w))
			_daynight_paint!(scene, t)
			_sat_reply(out, cap, "Night side shown for " *
			           GMT.Dates.format(t, "yyyy-mm-dd HH:MM") * " UTC.")
			return Cint(1)
		end

		# Does this mission have a published swath? The dialog asks on every change of selection, to
		# decide whether its "Ground coverage" box can be ticked at all. A table lookup — no work.
		if what == "swathknown"
			_sat_reply(out, cap, _swath_for(get(d, "name", "")) === nothing ? "0" : "1")
			return Cint(1)
		end

		# --- the one-day animation, driven frame by frame by the dialog's timer ----------------------
		if what == "animstart"
			scene == C_NULL && error("no window to plot into")
			tles = get(_SAT_LOADED, key) do
				t = _sat_read_source(d);  _SAT_LOADED[key] = t;  t
			end
			sel = [parse(Int, s) for s in split(get(d, "sel", ""), ',') if !isempty(strip(s))]
			length(sel) == 1 || error("the animation runs one satellite at a time")
			i = sel[1]
			(i < 0 || i >= length(tles)) && error("that satellite is no longer in the list")
			useAlt = get(d, "altitude", "1") != "0"
			fw = get(d, "frame", "auto")
			fr = fw == "earthfixed" ? :earthfixed : fw == "inertial" ? :inertial : :auto
			bands = Tuple{Float64,Float64}[]
			if get(d, "coverage", "0") == "1"
				sw = _swath_for(isempty(tles[i+1].name) ? "" : tles[i+1].name)
				# The box cannot be ticked for a mission with no published swath (the dialog greys it
				# out), so this is a guard, not a branch the user can reach.
				sw === nothing && error("no published swath is known for that satellite")
				bands = sw[1]
			end
			_sat_reply(out, cap, string(_anim_start!(scene, tles[i+1], useAlt, fr, bands)))
			return Cint(1)
		end
		if what == "animframe"
			_sat_reply(out, cap, _anim_frame!(round(Int, _sat_num(get(d, "k", "1"), 1.0))))
			return Cint(1)
		end
		if what == "animstop"
			_anim_stop!()
			_sat_reply(out, cap, "Animation stopped.")
			return Cint(1)
		end

		error("unknown request: " * what)
	catch e
		# A FAILING FRAME ENDS THE RUN. The host stops its timer on a failed request, but the state
		# here is dropped too — otherwise a half-built animation sits holding a Satellite handle, and
		# one broken frame would raise one error window per tick.
		startswith(what, "anim") && (try; _anim_stop!(); catch; end)
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
