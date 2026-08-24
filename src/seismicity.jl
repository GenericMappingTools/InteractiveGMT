# seismicity.jl — Geophysics > Seismology > Seismicity (port of Mirone's earthquakes.m).
#
# The C++ dialog (PlotSeismicityDialog, 70_window.cpp) hands a newline-separated "key=value"
# block to `_on_seismicity`. The catalog is read by format — the USGS web query is built, downloaded
# and read here (`_seis_usgs`, and see the reasons written there), ISF through GMT.gmtisf, the two
# plain-column layouts and Posit through GMT.gmtread — then filtered by date / magnitude /
# depth / visible map region and stamped as screen-constant symbol layers (add_symbols!, the
# same primitive the Geography point datasets use): one layer per used magnitude-interval ×
# depth-interval combination, sized/coloured per the dialog. Every event carries a hover
# tooltip with its magnitude / depth / date.
#
# As with every C→Julia callback the @cfunction + registration are RUNTIME values, created
# lazily at the first window open (eventloop.jl `_ensure_callbacks`) via an invokelatest
# trampoline — never at top level (a precompiled @cfunction is invalid).

# Mirone's fixed intervals (earthquakes.m push_OK_CB): six magnitude buckets, five depth buckets.
const _SEIS_MAG_EDGES = (3.0, 5.0, 6.0, 7.0, 8.0)      # <3, 3-5, 5-6, 6-7, 7-8, ≥8
const _SEIS_MAG_LABEL = ("M<3", "M3-5", "M5-6", "M6-7", "M7-8", "M≥8")
const _SEIS_DEP_EDGES = (33.0, 70.0, 150.0, 300.0)     # <33, 33-70, 70-150, 150-300, ≥300 km
const _SEIS_DEP_LABEL = ("0-33 km", "33-70 km", "70-150 km", "150-300 km", ">300 km")
const _SEIS_DEF_SIZE  = (4.0, 6.0, 8.0, 10.0, 12.0, 15.0)
const _SEIS_DEF_COLOR = ("red", "green", "blue", "cyan", "yellow")

# USGS's own map convention: the circle DIAMETER grows GEOMETRICALLY with magnitude — their legend
# runs M0 … M8+ over a ~4.4x span, i.e. ~1.24x per magnitude unit — and M5 is 8 POINTS. Both ends
# saturate exactly like that legend ("8+" is one single size), so a M9 cannot blot out the map and a
# M0 stays a visible dot. An event carrying NO magnitude gets the smallest size.
const _SEIS_MAG_REF    = 5.0        # magnitude the scale is anchored on
const _SEIS_MAG_REF_PT = 8.0        # … and its diameter, in points
const _SEIS_MAG_BASE   = 1.24       # diameter ratio per magnitude unit
const _SEIS_MAG_LO     = 0.0        # magnitudes clamp here …
const _SEIS_MAG_HI     = 8.0        # … and at the legend's "8+"

# Symbol diameter for one magnitude, in PIXELS (add_symbols!'s default unit; the scheme above is
# stated in points, so convert here @96 dpi rather than threading a `sizeunit` through the layer).
function _seis_mag_size(m::Float64)::Float64
	mm = isnan(m) ? _SEIS_MAG_LO : clamp(m, _SEIS_MAG_LO, _SEIS_MAG_HI)
	return _SEIS_MAG_REF_PT * _SEIS_MAG_BASE^(mm - _SEIS_MAG_REF) * 96 / 72
end

# Bucket of `v` in the sorted `edges` (1 = below the first edge … length+1 = ≥ the last edge).
# NaN compares false against every edge, so it lands in bucket 1 — exactly the "include unknown
# magnitudes with the smallest events" behaviour the "All magnitudes" box asks for.
_seis_bucket(edges, v) = 1 + count(e -> v >= e, edges)

# ── catalog readers ─────────────────────────────────────────────────────────────────────────
# All return (lon, lat, depth, mag, t) AbstractVector{Float64}s over the read data (views, no
# column copies); t = Unix time in seconds (NaN = unknown). Read errors propagate to the one
# try/catch in _on_seismicity, which reports them in the viewer console.

# The USGS FDSN event service. The query is built and downloaded HERE instead of through
# GMT.seismicity, because the bounds that arrive here come straight off the CAMERA and that is the
# one caller GMT.seismicity's region handling cannot serve. Measured, all three fatal:
#   * a fitted world view is a little WIDER than the map (-180.5/180.5/-90.5/90.5). GMT.seismicity's
#     ">180" wrap branch rewrites that to minlongitude=-360.5 / minlatitude=-90.5, which the service
#     rejects outright — HTTP 400, no catalog at all;
#   * the same branch turns a legitimate Pacific window (150/210) into -30/30 — the ATLANTIC. The
#     answer then fails this file's own region filter: "no events match" over a map full of quakes;
#   * `orderby=time-asc` with no `limit` is the "doesn't download the most recent data" itself: the
#     service caps an answer at 20000 records and the cap keeps the FRONT of the ordering, so an
#     overrunning query drops the RECENT end (and, with no explicit limit, 400s instead of answering).
#     Newest-first + an explicit limit drops the OLD end, which is the harmless one.
# The answer is downloaded INTO MEMORY and parsed there. No file is written anywhere — not a temp
# file, and not GMT.seismicity's "_query.csv" in whatever directory the app happened to start in.
# It needs no CSV reader either: the five columns wanted (time, latitude, longitude, depth, mag) are
# the FIRST five of the service's fixed csv layout and all of them come BEFORE its one quoted field
# (`place`, column 14), so a plain comma split is exact.
const _SEIS_USGS_URL    = "https://earthquake.usgs.gov/fdsnws/event/1/query.csv"
const _SEIS_USGS_LIMIT  = 20000       # the service's own ceiling on one answer
const _SEIS_USGS_MAGMIN = 3.0         # "Current seismicity" default when the dialog names none

# The query. Only the bounds the dialog actually carries are sent; the service's own defaults (the
# last 30 days) then apply, which is what makes the menu entry "recent" seismicity. An `endtime`
# given as a bare date means MIDNIGHT that morning and would throw away the whole of the end day —
# the dialog's end date means "up to the end of that day", so it is sent as one.
function _seis_usgs_url(d::Dict{String,String}, W::Float64, E::Float64, S::Float64, N::Float64)::String
	io = IOBuffer()
	print(io, _SEIS_USGS_URL, "?format=csv&orderby=time&limit=", _SEIS_USGS_LIMIT)
	print(io, "&minlongitude=", W, "&maxlongitude=", E, "&minlatitude=", S, "&maxlatitude=", N)
	print(io, "&minmagnitude=", something(tryparse(Float64, _get(d, "magmin")), _SEIS_USGS_MAGMIN))
	m1 = tryparse(Float64, _get(d, "magmax"));  m1 === nothing || print(io, "&maxmagnitude=", m1)
	t0 = _seis_datestr(d, "s");  isempty(t0) || print(io, "&starttime=", t0)
	t1 = _seis_datestr(d, "e");  isempty(t1) || print(io, "&endtime=", t1, "T23:59:59")
	z0 = tryparse(Float64, _get(d, "depmin")); (z0 !== nothing && z0 > 0) && print(io, "&mindepth=", z0)
	z1 = tryparse(Float64, _get(d, "depmax")); (z1 !== nothing && z1 > 0) && print(io, "&maxdepth=", z1)
	return String(take!(io))
end

# Fetch `url` into memory and hand back the whole answer as one String. `Downloads` first (in-process,
# no child process to stall the GUI), then `curl`, which reads curl's stdout so that path writes
# nothing either. THREE attempts because the service really does reset the connection mid-answer,
# curl included — measured here repeatedly, with the second attempt getting through.
#
# An EMPTY answer is not a failure: the FDSN service answers "no events in that box/time" with HTTP
# 204 and no body, which is a legitimate result the caller reports as "no events" — never an
# exception. Throwing on it (what this did) turned an ordinary empty query into "Seismicity FAILED:
# the USGS query returned nothing", which reads like a broken tool and hides the real answer. Only a
# genuine TRANSPORT failure (reset, refused, HTTP 400/5xx) still throws, after the three attempts.
function _seis_fetch(url::String)::String
	err = nothing
	for k in 1:3
		try
			if k == 1
				io = IOBuffer();  GMT.Downloads.download(url, io);  return String(take!(io))
			else
				return read(`curl -s --show-error --fail --max-time 180 $url`, String)
			end
		catch e
			err = e
			@warn "seismicity: fetch attempt $k failed, retrying with curl" exception=e url
			# Back off before retrying. The service resets the connection on rapid repeat queries —
			# hammering it again immediately (what this did) is what turns "plot the same catalog twice
			# in a row" into a hard failure; a short pause gets the second call through.
			k < 3 && sleep(k == 1 ? 0.5 : 2.0)
		end
	end
	error("the USGS service could not be reached after 3 attempts ($(sprint(showerror, err)))")
end

# One field of a comma-split record as a Float64; an absent or empty field is NaN (the service
# leaves depth/mag blank on an event that carries none).
_seis_num(f::Union{String,SubString{String}})::Float64 =
	(s = strip(f); isempty(s) ? NaN : something(tryparse(Float64, s), NaN))

# The record's ISO-8601 instant ("2026-08-24T00:00:48.085Z") as Unix seconds; NaN if unparsable, so
# one malformed row never kills the catalog. Uses Dates — never hand-rolled calendar math.
function _seis_isotime(f::Union{String,SubString{String}})::Float64
	s = strip(f)
	(isempty(s) || length(s) < 19) && return NaN
	endswith(s, "Z") && (s = s[1:end-1])
	dt = tryparse(GMT.Dates.DateTime, s)
	return dt === nothing ? NaN : GMT.Dates.datetime2unix(dt)
end

function _seis_usgs(d::Dict{String,String}, W::Float64, E::Float64, S::Float64, N::Float64)
	url::String = _seis_usgs_url(d, W, E, S, N)
	txt::String = _seis_fetch(url)
	lon = Float64[]; lat = Float64[]; dep = Float64[]; mag = Float64[]; t = Float64[]
	hdr = true
	for line in eachsplit(txt, '\n')
		s = strip(line)
		isempty(s) && continue
		if hdr;  hdr = false;  continue;  end         # the one header line
		f = split(s, ','; limit=6)                    # time,latitude,longitude,depth,mag,<rest>
		length(f) < 5 && continue
		push!(t,   _seis_isotime(f[1]))
		push!(lat, _seis_num(f[2]))
		push!(lon, _seis_num(f[3]))
		push!(dep, _seis_num(f[4]))
		push!(mag, _seis_num(f[5]))
	end
	length(lon) >= _SEIS_USGS_LIMIT &&
		@warn "seismicity: the USGS answer hit the $(_SEIS_USGS_LIMIT)-event ceiling; older events were dropped, not recent ones"
	return lon, lat, dep, mag, t
end

# ISF catalog, cropped to the visible region by gmtisf itself; `abstime=2` appends the event
# Unix time as the LAST column. depth/mag columns found by NAME (gmtisf sets colnames; the count
# varies with focal-mechanism content).
function _seis_isf(file, W, E, S, N)
	D = GMT.gmtisf(file; R=(W, E, S, N), abstime=2)
	size(D, 1) == 0 && return _seis_none()
	m  = D.data
	ci(name, fallback) = something(findfirst(==(name), D.colnames), fallback)
	return view(m,:,1), view(m,:,2), view(m,:,ci("depth", 3)), view(m,:,ci("mag", 4)), view(m,:,size(m, 2))
end

# Plain-column file: lon,lat,mag,dep,yy,mm,dd[,hh,mm,ss] (magfirst) or lon,lat,dep,mag,yy,mm,dd.
function _seis_table(file, magfirst::Bool)
	m = _seis_matrix(file)
	nc = size(m, 2)
	nc >= 7 || error("expected ≥7 columns (lon,lat,$(magfirst ? "mag,dep" : "dep,mag"),yy,mm,dd), got $nc")
	n = size(m, 1)
	t = Vector{Float64}(undef, n)
	@inbounds for i in 1:n
		t[i] = _seis_unix(m[i,5], m[i,6], m[i,7],
		                  nc >= 8 ? m[i,8] : 0.0, nc >= 9 ? m[i,9] : 0.0, nc >= 10 ? m[i,10] : 0.0)
	end
	mag = view(m,:, magfirst ? 3 : 4)
	dep = view(m,:, magfirst ? 4 : 3)
	return view(m,:,1), view(m,:,2), dep, mag, t
end

# Posit file, numeric layout (earthquakes.m filtro==2 primary branch):
# year julian_day hour minute _ lat lon _ _ _ mag [_] — no depth (0). The alphanumeric Posit
# variant (packed date string) is NOT handled; those files error with a clear message.
function _seis_posit(file)
	m = _seis_matrix(file)
	size(m, 2) >= 11 || error("Posit file: expected ≥11 numeric columns (the packed-date Posit variant is not supported)")
	n = size(m, 1)
	t = Vector{Float64}(undef, n)
	@inbounds for i in 1:n
		yd = _seis_unix(m[i,1], 1.0, 1.0)                     # Jan 1 of the event year…
		t[i] = isnan(yd) ? NaN : yd + 86400.0 * (m[i,2] - 1) + 3600.0 * m[i,3] + 60.0 * m[i,4]
	end
	return view(m,:,7), view(m,:,6), zeros(n), view(m,:,11), t
end

# Built-in global catalog ("Global seismicity (1990-2009)"): the Mirone quakes.dat shipped in
# data/ — columns year mo day lat lon depth mag (earthquakes.m use_default_file branch).
function _seis_default()
	m = _seis_matrix(joinpath(_PKGROOT, "data", "quakes.dat"))
	size(m, 2) >= 7 || error("data/quakes.dat: expected 7 columns (year mo day lat lon depth mag)")
	n = size(m, 1)
	t = Vector{Float64}(undef, n)
	@inbounds for i in 1:n
		t[i] = _seis_unix(m[i,1], m[i,2], m[i,3])
	end
	return view(m,:,5), view(m,:,4), view(m,:,6), view(m,:,7), t
end

_seis_none() = (Float64[], Float64[], Float64[], Float64[], Float64[])

# Read a whitespace/comma table file into one plain matrix (multi-segment files are stacked).
function _seis_matrix(file)::Matrix{Float64}
	D = GMT.gmtread(file; table=true)
	return D isa GMTdataset ? D.data : reduce(vcat, (seg.data for seg in D))
end

# Unix seconds from y,m,d[,h,mi,s] doubles; NaN for a missing/invalid date instead of throwing,
# so one bad row never kills the whole catalog. Uses Dates — never hand-rolled calendar math.
function _seis_unix(y, mo, dy, h=0.0, mi=0.0, s=0.0)
	(isnan(y) || isnan(mo) || isnan(dy)) && return NaN
	yi = trunc(Int, y); moi = trunc(Int, mo); di = trunc(Int, dy)
	(1 <= moi <= 12 && 1 <= di <= GMT.Dates.daysinmonth(yi, moi)) || return NaN
	base = GMT.Dates.datetime2unix(GMT.Dates.DateTime(yi, moi, di))
	return base + 3600.0 * (isnan(h) ? 0.0 : h) + 60.0 * (isnan(mi) ? 0.0 : mi) + (isnan(s) ? 0.0 : s)
end

# ── dialog-field helpers ────────────────────────────────────────────────────────────────────

# "YYYY-MM-DD" from the dialog's year/month/day fields ("" if no year — no bound). Month/day
# default to the interval-appropriate end (Jan 1 / Dec 31), as Mirone's push_OK_CB defaults.
function _seis_datestr(d, pre)
	y = tryparse(Int, _get(d, pre * "year"))
	y === nothing && return ""
	mo = clamp(something(tryparse(Int, _get(d, pre * "month")), pre == "s" ? 1 : 12), 1, 12)
	dy = clamp(something(tryparse(Int, _get(d, pre * "day")), pre == "s" ? 1 : 31), 1, GMT.Dates.daysinmonth(y, mo))
	return string(y, "-", lpad(mo, 2, '0'), "-", lpad(dy, 2, '0'))
end

# Unix-seconds bound of the date filter; -Inf/+Inf when the year field is empty. The end bound
# covers the whole end day (+86399.999 s — Mirone's dec_year(EndDay+0.999)).
function _seis_bound(d, pre, isstart::Bool)
	ds = _seis_datestr(d, pre)
	isempty(ds) && return isstart ? -Inf : Inf
	u = GMT.Dates.datetime2unix(GMT.Dates.DateTime(ds))
	return isstart ? u : u + 86399.999
end

# The visible map region "W/E/S/N" appended by the menu action (in-map crop + USGS query bbox,
# like Mirone's in_map_region). Falls back to the whole world. ONE box for the query AND the
# filter, so what is asked for and what is kept can never disagree.
function _seis_region(d::Dict{String,String})::NTuple{4, Float64}
	p = split(_get(d, "region"), '/')
	(length(p) == 4) || return (-180.0, 180.0, -90.0, 90.0)
	v = tryparse.(Float64, p)
	any(isnothing, v) && return (-180.0, 180.0, -90.0, 90.0)
	return _seis_norm_region(v[1], v[2], v[3], v[4])
end

# The visible region as the CAMERA sees it is not a legal geographic query box: `sceneVisibleRegion`
# hands over whatever the viewport covers, which is routinely a little WIDER than the map itself
# (-180.5/180.5/-90.5/90.5 for a fitted world), and a Pacific view legitimately runs past +180. The
# service rejects a latitude past ±90 outright (HTTP 400, whole fetch dead), and a longitude past
# +180 is what GMT.seismicity rewrote wrongly. So: latitudes clamped, a full turn (or more) collapsed
# to the whole world, and a dateline-crossing window shifted WHOLE into the negative half — legal for
# the service (it takes longitudes down to -360) and it keeps `maxlongitude` at or below +180.
function _seis_norm_region(W::Float64, E::Float64, S::Float64, N::Float64)::NTuple{4, Float64}
	S, N = min(S, N), max(S, N)
	S = clamp(S, -90.0, 90.0);  N = clamp(N, -90.0, 90.0)
	W, E = min(W, E), max(W, E)
	(E - W >= 360.0) && return (-180.0, 180.0, S, N)
	if (E > 180.0)
		k = ceil((E - 180.0) / 360.0);  W -= 360.0 * k;  E -= 360.0 * k
	end
	(W < -360.0) && return (-180.0, 180.0, S, N)      # nothing sane left to ask for
	return (W, E, S, N)
end

# Is longitude `l` inside [W,E]? The window may live outside -180…180 (the dateline shift above)
# while the catalog's own longitudes never do, so bring `l` to the first turn at or after W.
function _seis_inlon(l::Float64, W::Float64, E::Float64)::Bool
	isnan(l) && return false
	return l + 360.0 * ceil((W - l) / 360.0) <= E
end

# ── filter + plot ───────────────────────────────────────────────────────────────────────────

# The footprint of the raster ACTUALLY ON DISPLAY, or nothing when the window has none yet. This is
# the SAME question `_clip_to_display` (drop.jl) asks of a dropped table — "on top of the image"
# means bounded BY it — so it goes through the SAME C entry point, `gmtvtk_get_display_bounds_h`,
# never a second reading of the scene state (SACRED_LAW.md: same operation, same function).
function _seis_display_frame(scene::Ptr{Cvoid})
	b = Vector{Cdouble}(undef, 4)
	geog = Ref{Cint}(0)
	ok = ccall(_fn(:gmtvtk_get_display_bounds_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Ptr{Cint}), scene, b, geog)
	return ok == 0 ? nothing : (b[1], b[2], b[3], b[4])
end

# Does this box enclose any ground at all? A camera can hand over a COLLAPSED region — zero width or
# height — and every downstream step then does something wrong and unexplainable: the query asks the
# service for a box with no area (answered with an empty body, i.e. "no events" for a map full of
# them), and the in-map crop rejects every event that does come back. So it is caught HERE, once, at
# the boundary where the camera's answer enters the tool, instead of being diagnosed three steps later.
_seis_box_ok(W::Float64, E::Float64, S::Float64, N::Float64)::Bool =
	isfinite(W) && isfinite(E) && isfinite(S) && isfinite(N) && (E - W > 1e-9) && (N - S > 1e-9)

# The box to work with: what the caller asked for, or — when that has collapsed — the footprint of
# the raster on display, or the whole world. Never a degenerate box.
function _seis_usable_region(W::Float64, E::Float64, S::Float64, N::Float64, frame)::NTuple{4,Float64}
	_seis_box_ok(W, E, S, N) && return (W, E, S, N)
	if frame !== nothing
		f = _seis_norm_region(Float64(frame[1]), Float64(frame[2]), Float64(frame[3]), Float64(frame[4]))
		_seis_box_ok(f...) && return f
	end
	return (-180.0, 180.0, -90.0, 90.0)
end

# Grow the window's Z frame DOWN to the deepest plotted event, so the axes cube, its Z numbers and
# the camera actually contain the hypocentres. Without this the cloud is drawn correctly and lands
# outside the box — invisible on any grid whose own Z span is smaller than the catalog's depth
# range, which is every regional grid (verified: 6-39 km of events against layer0.grd's 8 km box).
# The X/Y frame is NOT touched — it stays the raster's own (SACRED_LAW.md vector-import law: an
# overlay never re-frames the map it lands on); only Z is extended, and only downwards.
function _seis_reframe_z(scene::Ptr{Cvoid}, deepest_km::Float64)
	ccall(_fn(:gmtvtk_grow_z_frame_h), Cvoid, (Ptr{Cvoid}, Cdouble, Cdouble),
	      scene, -deepest_km * 1000.0, 0.0)
	return
end

# One BitVector pass: plot box ∩ date range ∩ magnitude ∩ depth. NaN magnitudes/depths
# fail their comparison (excluded); "All magnitudes"/"All depths" re-admits them. Undated
# events (t = NaN) pass the date filter — the file simply carried no time.
#
# The PLOT BOX is the requested region ∩ the RASTER's own extent: an event is drawn only where
# there is a map under it. The two boxes are deliberately different things — the request follows
# the CAMERA (sceneVisibleRegion, so zooming out asks for more), while the drawing is cropped to
# the DATA, because a symbol floating in the empty background outside the grid is not a plot.
# `frame` = nothing (no raster in the window) leaves the request box as the only crop.
function _seis_filter(d, lon, lat, dep, mag, t, frame=nothing,
                     box::Union{Nothing,NTuple{4,Float64}}=nothing)
	# `box` = the region the CATALOG WAS ASKED FOR, handed in so the query and the crop can never be
	# two different boxes (a collapsed camera region is repaired once, at the door — _seis_usable_region
	# — and both halves must see the repaired one). Absent = derive it, for callers that only filter.
	W, E, S, N = box === nothing ? _seis_region(d) : box
	if frame !== nothing
		W = max(W, frame[1]);  E = min(E, frame[2])
		S = max(S, frame[3]);  N = min(N, frame[4])
	end
	t0 = _seis_bound(d, "s", true)
	t1 = _seis_bound(d, "e", false)
	m0 = something(tryparse(Float64, _get(d, "magmin")), -Inf)
	m1 = something(tryparse(Float64, _get(d, "magmax")),  Inf)
	z0 = something(tryparse(Float64, _get(d, "depmin")), -Inf)
	z1 = something(tryparse(Float64, _get(d, "depmax")),  Inf)
	allm = _on(d, "allmags"); alld = _on(d, "alldeps")
	n = length(lon)
	keep = BitVector(undef, n)
	@inbounds for i in 1:n
		keep[i] = _seis_inlon(lon[i], W, E) && (S <= lat[i] <= N) &&
		          (isnan(t[i]) || (t0 <= t[i] <= t1)) &&
		          ((m0 <= mag[i] <= m1) || (allm && isnan(mag[i]))) &&
		          ((z0 <= dep[i] <= z1) || (alld && isnan(dep[i])))
	end
	return keep
end

# Stamp the kept events as symbol layers. Simple case = ONE red layer whose symbols are sized by
# MAGNITUDE (_seis_mag_size, the USGS scheme) — one actor carrying per-point size factors, never a
# layer per event. "different sizes" splits by magnitude bucket and honours the dialog's OWN six
# sizes (an explicit user setting, never overridden here); "different colors" splits by depth
# bucket, both = the used (mag, depth) combinations. Colours-only keeps the magnitude sizing.
function _seis_plot(scene::Ptr{Cvoid}, d, lon, lat, dep, mag, t, keep)
	bysize  = _on(d, "magsizes")
	bycolor = _on(d, "depcolors")
	sizes  = ntuple(k -> something(tryparse(Float64, _get(d, "s$k")), _SEIS_DEF_SIZE[k]), 6)
	colors = ntuple(k -> (c = _get(d, "c$k"); isempty(c) ? _SEIS_DEF_COLOR[k] : c), 5)
	idx = findall(keep)
	if !bysize && !bycolor
		_seis_layer(scene, "Seismicity", idx, lon, lat, dep, mag, t,
		            [_seis_mag_size(Float64(mag[i])) for i in idx], "red")
		return
	end
	mb = [bysize  ? _seis_bucket(_SEIS_MAG_EDGES, mag[i]) : 1 for i in idx]
	db = [bycolor ? _seis_bucket(_SEIS_DEP_EDGES, dep[i]) : 1 for i in idx]
	for kb in 1:(bysize ? 6 : 1), jb in 1:(bycolor ? 5 : 1)
		sel = [idx[q] for q in eachindex(idx) if mb[q] == kb && db[q] == jb]
		isempty(sel) && continue
		name = "Seismicity" * (bysize ? " " * _SEIS_MAG_LABEL[kb] : "") * (bycolor ? " " * _SEIS_DEP_LABEL[jb] : "")
		_seis_layer(scene, name, sel, lon, lat, dep, mag, t,
		            bysize ? sizes[kb] : [_seis_mag_size(Float64(mag[i])) for i in sel],
		            bycolor ? colors[jb] : "red")
	end
	return
end

# One layer: circles with a black edge (Mirone's marker style) + per-event hover tooltip.
# z is the event depth converted to world metres, NEGATIVE (down): the 3-D perspective view then
# actually shows events at their hypocentre; the flat-2D view is a top-down ORTHOGRAPHIC camera
# (sceneSetFlat2D, 70_window.cpp), whose projection is along Z, so a nonzero Z never shifts the
# on-screen lon/lat — events stay exactly projected to the surface there for free. NaN depth (a
# catalog entry that carries none) falls back to z=0 (surface).
# Is the window showing a FLAT 2-D MAP right now? THE VIEW MODE decides this, never the raster kind:
# a flat-2D image and a flat-2D grid are the same display — a map seen straight down — and a catalog
# must be drawn the same way on both. Gating on `imageOnly` instead (an earlier cut of this) made an
# image behave differently from a grid in the identical view, which is exactly the fork SACRED_LAW.md
# forbids. In flat 2-D there is no depth to read, so events lie ON the map (z = 0, disc glyph); in
# 3-D they carry their hypocentre and the Z frame grows to hold them, image or grid alike.
function _seis_flat_view(scene::Ptr{Cvoid})::Bool
	st = try _scene_state(scene) catch; nothing end
	st === nothing && return false
	return get(st, "flat2d", 0) == 1
end

function _seis_layer(scene::Ptr{Cvoid}, name, sel, lon, lat, dep, mag, t,
                     sizepx::Union{Float64,Vector{Float64}}, color)
	infos = [_seis_info(mag[i], dep[i], t[i]) for i in sel]
	# EVERY event sits at its own HYPOCENTRE: z = -depth, in metres, down negative. A seismicity
	# cloud IS its depth distribution — a subduction zone has to dip — so the depth is never thrown
	# away here. What used to make the layer invisible was not this z, it was the FRAME: a catalog
	# 6-40 km deep hangs far below a bathymetry grid whose own box is ~8 km tall, so the events fell
	# outside the axes cube and the camera. `_seis_reframe_z` (called after plotting) grows the
	# window's Z frame to cover them, which is the actual fix. NaN depth (an entry that carries
	# none) sits on the surface.
	# The depth is ALWAYS stored, on every window kind. Flattening it for a 2-D map is the VIEWER's
	# job — applyVE (10_geometry.cpp) scales a symbol layer's Z by 0 in flat-2D and by zfac*ve in 3-D,
	# so the SAME layer reads as a plain map dot looking straight down and as a hypocentre when the
	# view tilts, and it follows the 2D/3D toggle live. Deciding it here (an earlier cut) threw the
	# third dimension away permanently: plot in 2-D, tilt to 3-D, and the cloud had no depth left to
	# restore. NaN depth (an entry that carries none) sits on the surface.
	zv = [(isnan(dep[i]) ? 0.0 : -dep[i] * 1000.0) for i in sel]
	# :sphere is a true lit 3-D glyph (symbols.jl) so an event stays visible from any oblique 3-D
	# angle at its real depth; squashed by applyVE in flat-2D it reads as a plain dot on the map.
	# …and the layer carries THE CATALOG ITSELF, one row per event, for "Show data table": the event's
	# own lon / lat / depth / magnitude / date. That is what the data IS. How big its circle is drawn
	# is a property of the picture, not of the earthquake, and has no business in a data table.
	add_symbols!(scene, view(lon, sel), view(lat, sel); z=zv, symbol=:sphere,
	             size=sizepx, fill=color, edge=:black, edgewidth=1.0, name=name, info=infos,
	             datanames=_SEIS_TABLE_COLS,
	             datarows=[_seis_row(lon[i], lat[i], dep[i], mag[i], t[i]) for i in sel])
end

# The catalog's own columns, as shown by "Show data table". Formatted HERE because only this file
# knows what they mean — a date is a date, a magnitude carries one decimal, an unknown value is
# blank rather than "NaN".
const _SEIS_TABLE_COLS = ["Lon", "Lat", "Depth (km)", "Mag", "Time (UTC)"]

_seis_cell(v::Float64, nd::Int)::String = isnan(v) ? "" : string(round(v; digits=nd))

function _seis_row(lo::Float64, la::Float64, dp::Float64, m::Float64, ti::Float64)::Vector{String}
	tstr = isnan(ti) ? "" : GMT.Dates.format(GMT.Dates.unix2datetime(ti), "yyyy-mm-dd HH:MM:SS")
	return [_seis_cell(lo, 4), _seis_cell(la, 4), _seis_cell(dp, 1), _seis_cell(m, 1), tstr]
end

# Hover tooltip: magnitude, depth and date — whichever the event actually carries.
function _seis_info(m, z, ti)
	parts = String[]
	isnan(m)  || push!(parts, "M $(round(m; digits=1))")
	isnan(z)  || push!(parts, "Depth: $(round(z; digits=1)) km")
	isnan(ti) || push!(parts, GMT.Dates.format(GMT.Dates.unix2datetime(ti), "yyyy-mm-dd HH:MM"))
	return isempty(parts) ? "earthquake" : join(parts, "\n")
end

# ── C callback ──────────────────────────────────────────────────────────────────────────────

# cparams = "key=value\n…" (the same block format the NSWING dialog uses → same parser).
function _on_seismicity(scene::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		d = _nswing_parse(unsafe_string(cparams))
		# The raster's footprint FIRST: it is both the in-map crop and the fallback box when the region
		# that arrived is unusable (see _seis_usable_region — a collapsed camera region asked the service
		# for a zero-area box and cropped every answer away, with nothing on screen to explain it).
		frame = _seis_display_frame(scene)
		W, E, S, N = _seis_usable_region(_seis_region(d)..., frame)
		fmt  = something(tryparse(Int, _get(d, "format", "1")), 1)
		file = _get(d, "file")
		(fmt in 2:5 && isempty(file)) && error("catalog format $fmt needs a file")
		lon, lat, dep, mag, t =
			fmt == 1 ? _seis_usgs(d, W, E, S, N) :
			fmt == 2 ? _seis_isf(file, W, E, S, N) :
			fmt == 3 ? _seis_table(file, true) :
			fmt == 4 ? _seis_table(file, false) :
			fmt == 5 ? _seis_posit(file) :
			fmt == 6 ? _seis_default() : error("unknown catalog format $fmt")
		if isempty(lon)
			_viewer_log_error(scene, "Seismicity: the catalog returned no events for " *
				"$(round(W;digits=2))/$(round(E;digits=2))/$(round(S;digits=2))/$(round(N;digits=2))")
			return
		end
		box   = (W, E, S, N)                       # the box the catalog was ASKED for — crop with THAT
		keep  = _seis_filter(d, lon, lat, dep, mag, t, frame, box)
		nk = count(keep)
		if nk == 0
			# Say WHY nothing is drawn: an empty catalog and a catalog whose every event fell outside
			# the map are different situations and the user cannot tell them apart from a blank map.
			nout = frame === nothing ? 0 : count(_seis_filter(d, lon, lat, dep, mag, t, nothing, box)) - nk
			_viewer_log_error(scene, nout > 0 ?
				"Seismicity: $nout event(s) found, but all of them lie OUTSIDE this map's area — nothing to draw over. Load a wider grid or a Base Map to see them." :
				"Seismicity: no events match the selected filters")
			return
		end
		_seis_plot(scene, d, lon, lat, dep, mag, t, keep)
		# …then make the window's Z frame contain what was just plotted, or the deep half of the
		# catalog hangs below the axes cube where nothing can see it.
		dk = [dep[i] for i in eachindex(dep) if keep[i] && !isnan(dep[i])]
		isempty(dk) || _seis_reframe_z(scene, maximum(dk))
		# Say WHEN the newest plotted event is. "Recent seismicity" that is quietly stale is the one
		# failure this tool cannot show on its own, so it is reported rather than assumed.
		tk = [t[i] for i in eachindex(t) if keep[i] && !isnan(t[i])]
		last = isempty(tk) ? "" :
		       ", most recent " * GMT.Dates.format(GMT.Dates.unix2datetime(maximum(tk)), "yyyy-mm-dd HH:MM") * " UTC"
		_viewer_log_error(scene, "Seismicity: plotted $nk of $(length(lon)) events$last")
	catch e
		# Name the exception TYPE and the line it came from, in the window itself. "Seismicity FAILED:"
		# followed by a bare message is unreportable — every failure looks the same to whoever sees it,
		# and the only copy of the useful half (the backtrace) goes to a stderr no GUI user reads.
		bt = catch_backtrace()
		fr = ""
		for f in Base.stacktrace(bt)
			s = string(f.file)
			if occursin("seismicity.jl", s) || occursin("symbols.jl", s)
				fr = "  [$(basename(s)):$(f.line) in $(f.func)]";  break
			end
		end
		_viewer_log_error(scene, "Seismicity FAILED: $(typeof(e)): $(sprint(showerror, e))$fr")
		@warn "seismicity: failed" exception=(e, bt)
	end
	return
end

# Build the C-callable pointer + register it. Lazy (first window) via _ensure_callbacks — the
# @cfunction is a thin invokelatest trampoline so it drags no GMT into compile.
function _register_seismicity()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_seismicity, s, c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_seismicity_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
