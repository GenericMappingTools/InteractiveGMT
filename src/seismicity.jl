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
function _seis_usgs_url(d, W, E, S, N)::String
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

# Fetch `url` into memory and hand back the whole answer as one String. `Downloads` first
# (in-process, no child process to stall the GUI), `curl` only as the fallback for a transient
# failure of the service — and it reads curl's stdout, so that path writes nothing either.
function _seis_fetch(url::String)::String
	io = IOBuffer()
	txt = try
		GMT.Downloads.download(url, io)
		String(take!(io))
	catch e
		@warn "seismicity: Downloads failed, retrying with curl" exception=e url
		read(`curl -s --show-error --fail --max-time 180 $url`, String)
	end
	isempty(txt) && error("the USGS query returned nothing")
	return txt
end

# One field of a comma-split record as a Float64; an absent or empty field is NaN (the service
# leaves depth/mag blank on an event that carries none).
_seis_num(f) = (s = strip(f); isempty(s) ? NaN : something(tryparse(Float64, s), NaN))

# The record's ISO-8601 instant ("2026-08-24T00:00:48.085Z") as Unix seconds; NaN if unparsable, so
# one malformed row never kills the catalog. Uses Dates — never hand-rolled calendar math.
function _seis_isotime(f)::Float64
	s = strip(f)
	(isempty(s) || length(s) < 19) && return NaN
	endswith(s, "Z") && (s = s[1:end-1])
	dt = tryparse(GMT.Dates.DateTime, s)
	return dt === nothing ? NaN : GMT.Dates.datetime2unix(dt)
end

function _seis_usgs(d, W, E, S, N)
	url = _seis_usgs_url(d, W, E, S, N)
	txt = _seis_fetch(url)
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
function _seis_region(d)
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
function _seis_norm_region(W, E, S, N)
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
function _seis_inlon(l, W, E)
	isnan(l) && return false
	return l + 360.0 * ceil((W - l) / 360.0) <= E
end

# ── filter + plot ───────────────────────────────────────────────────────────────────────────

# One BitVector pass: visible region ∩ date range ∩ magnitude ∩ depth. NaN magnitudes/depths
# fail their comparison (excluded); "All magnitudes"/"All depths" re-admits them. Undated
# events (t = NaN) pass the date filter — the file simply carried no time.
function _seis_filter(d, lon, lat, dep, mag, t)
	W, E, S, N = _seis_region(d)
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

# Stamp the kept events as symbol layers. Simple case = one red size-4 layer (Mirone's marker);
# "different sizes" splits by magnitude bucket, "different colors" by depth bucket, both = the
# used (mag, depth) combinations. Depth-colours-only uses Mirone's size 5.
function _seis_plot(scene::Ptr{Cvoid}, d, lon, lat, dep, mag, t, keep)
	bysize  = _on(d, "magsizes")
	bycolor = _on(d, "depcolors")
	sizes  = ntuple(k -> something(tryparse(Float64, _get(d, "s$k")), _SEIS_DEF_SIZE[k]), 6)
	colors = ntuple(k -> (c = _get(d, "c$k"); isempty(c) ? _SEIS_DEF_COLOR[k] : c), 5)
	idx = findall(keep)
	if !bysize && !bycolor
		_seis_layer(scene, "Seismicity", idx, lon, lat, dep, mag, t, 4.0, "red")
		return
	end
	mb = [bysize  ? _seis_bucket(_SEIS_MAG_EDGES, mag[i]) : 1 for i in idx]
	db = [bycolor ? _seis_bucket(_SEIS_DEP_EDGES, dep[i]) : 1 for i in idx]
	for kb in 1:(bysize ? 6 : 1), jb in 1:(bycolor ? 5 : 1)
		sel = [idx[q] for q in eachindex(idx) if mb[q] == kb && db[q] == jb]
		isempty(sel) && continue
		name = "Seismicity" * (bysize ? " " * _SEIS_MAG_LABEL[kb] : "") * (bycolor ? " " * _SEIS_DEP_LABEL[jb] : "")
		_seis_layer(scene, name, sel, lon, lat, dep, mag, t, bysize ? sizes[kb] : 5.0, bycolor ? colors[jb] : "red")
	end
	return
end

# One layer: circles with a black edge (Mirone's marker style) + per-event hover tooltip.
# z is the event depth converted to world metres, NEGATIVE (down): the 3-D perspective view then
# actually shows events at their hypocentre; the flat-2D view is a top-down ORTHOGRAPHIC camera
# (sceneSetFlat2D, 70_window.cpp), whose projection is along Z, so a nonzero Z never shifts the
# on-screen lon/lat — events stay exactly projected to the surface there for free. NaN depth (a
# catalog entry that carries none) falls back to z=0 (surface).
function _seis_layer(scene::Ptr{Cvoid}, name, sel, lon, lat, dep, mag, t, sizepx, color)
	infos = [_seis_info(mag[i], dep[i], t[i]) for i in sel]
	zv = [(isnan(dep[i]) ? 0.0 : -dep[i] * 1000.0) for i in sel]
	# :sphere is a true lit 3-D glyph (symbols.jl) so an event stays visible from any oblique 3-D
	# angle at its real depth; a flat :circle glyph goes edge-on invisible there (it only reads well
	# top-down). In flat-2D it still projects to a plain-looking dot like before (ortho top-down cam).
	add_symbols!(scene, view(lon, sel), view(lat, sel); z=zv, symbol=:sphere, size=sizepx,
	             fill=color, edge=:black, edgewidth=1.0, name=name, info=infos)
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
		W, E, S, N = _seis_region(d)
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
			_viewer_log_error(scene, "Seismicity: the catalog returned no events")
			return
		end
		keep = _seis_filter(d, lon, lat, dep, mag, t)
		nk = count(keep)
		if nk == 0
			_viewer_log_error(scene, "Seismicity: no events match the selected filters")
			return
		end
		_seis_plot(scene, d, lon, lat, dep, mag, t, keep)
		# Say WHEN the newest plotted event is. "Recent seismicity" that is quietly stale is the one
		# failure this tool cannot show on its own, so it is reported rather than assumed.
		tk = [t[i] for i in eachindex(t) if keep[i] && !isnan(t[i])]
		last = isempty(tk) ? "" :
		       ", most recent " * GMT.Dates.format(GMT.Dates.unix2datetime(maximum(tk)), "yyyy-mm-dd HH:MM") * " UTC"
		_viewer_log_error(scene, "Seismicity: plotted $nk of $(length(lon)) events$last")
	catch e
		_viewer_log_error(scene, "Seismicity FAILED: $(sprint(showerror, e))")
		@warn "seismicity: failed" exception=(e, catch_backtrace())
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
