# geography.jl — the Geography menu's "Plot coastline" path (GSHHG shorelines for the view).
#
# The C++ Geography menu (70_window.cpp) computes the CURRENT visible region (W/E/S/N at the
# present zoom) and hands "<kind>/<res>/W/E/S/N" to `_on_geography`. We run GMT.coast over that
# region at the chosen resolution (M=true -> dump the shoreline as a GMTdataset, no plotting) and
# add it to the SAME window as a black line overlay (drawn at z=0 -> sea level on the flat map).
#
# As with the console / drop / basemap callbacks, the @cfunction and its registration are RUNTIME
# values, created in __init__ (a precompiled @cfunction is invalid), never at module top level.

# GSHHG dataset for a (kind, resolution, region). Only "coast" is wired today; political
# boundaries / rivers reuse this once their menus pass a kind. `res` is :l/:i/:h/:f.
function _geo_dataset(kind::AbstractString, res::Symbol, W, E, S, N)
	R = (W, E, S, N)
	kind == "coast" && return GMT.coast(R=R, D=res, M=true)
	@warn "geography: unknown feature kind '$kind'"
	return nothing
end

# Generic reader for a Mirone/NOAA point dataset overlaid by the Geography menu (volcanoes,
# meteorites, tide gauges, hydrothermal vents, …). ONE GMT call: GMT.gmtselect(path, R=region, f=:g)
# reads the data file AND clips it to the visible view, with GMT doing the region test INCLUDING
# longitude periodicity and always returning .data as geographic [lon lat]. Most of these files are
# lon,lat already (meteoritos.dat, hydrothermal_vents.dat); volcanoes.dat is the odd one stored
# lat,lon (Mirone legacy format), so its caller passes `latlon=true` -> `-:i` tells GMT the INPUT
# columns are (lat,lon) and swaps them on read, so the region test and .data below see (lon,lat)
# like every other file (never hand-swap the columns ourselves post-hoc — GMT's own periodic-lon
# region test needs the swap BEFORE it runs, not after). We normalize each kept lon into the map's
# [W,E] frame with one mod (map and data may differ: -180..180 vs 0..360, either direction).
# `datafile` is a bare name under data/. Returns (xs, ys, texts): xs/ys lon/lat in view, texts[k]
# the row's trailing text (Latin-1-fixed: the files have accented names / "±"); each caller turns
# texts into its own tooltip.
function _geo_points(datafile::AbstractString, W, E, S, N; latlon::Bool=false)
	path = joinpath(_PKGROOT, "data", datafile)
	isfile(path) || (@warn "geography: data file not found" path; return (Float64[], Float64[], String[]))
	Sr = latlon ? GMT.gmtselect(path, R=(W, E, S, N), f=:g, yx="i") :
	              GMT.gmtselect(path, R=(W, E, S, N), f=:g)         # read + region/lon-periodic clip in ONE call
	(Sr === nothing || isempty(Sr)) && return (Float64[], Float64[], String[])
	Sel = Sr isa AbstractVector ? Sr[1] : Sr
	(Sel.data === nothing || isempty(Sel.data)) && return (Float64[], Float64[], String[])
	txt = Sel.text
	n = size(Sel.data, 1)
	xs = Vector{Float64}(undef, n); texts = Vector{String}(undef, n)
	for k in 1:n
		xs[k] = mod(Sel.data[k, 1] - W, 360.0) + W            # lon -> normalize into the map's [W,E] frame
		rest = (txt !== nothing && k <= length(txt)) ? txt[k] : ""
		isvalid(rest) || (rest = String(Char.(codeunits(rest))))   # Latin-1 bytes -> chars (valid UTF-8)
		texts[k] = rest
	end
	return xs, Sel.data[:, 2], texts
end

# Tooltip blocks for the "N positional fields" datasets: split each trailing text into
# length(labels) whitespace tokens and label them (underscores -> spaces, missing token -> blank).
function _labeled_infos(texts, labels)
	infos = Vector{String}(undef, length(texts))
	for k in eachindex(texts)
		flds = split(texts[k])
		lines = String[]
		for i in eachindex(labels)
			val = i <= length(flds) ? replace(String(flds[i]), '_' => ' ') : ""
			push!(lines, string(labels[i], ": ", val))
		end
		infos[k] = join(lines, '\n')
	end
	return infos
end

# Volcanoes: trailing text is "<name> <country> <type> <age>", 4 whitespace tokens (multi-word
# values underscore-joined). File is stored lat,lon (not lon,lat like the others) -> latlon=true.
function _volcano_data(W, E, S, N)
	xs, ys, texts = _geo_points("volcanoes.dat", W, E, S, N; latlon=true)
	return xs, ys, _labeled_infos(texts, ("Name", "Country", "Type", "Age"))
end

# Hydrothermal vents (NOAA PMEL): trailing text is 5 tokens "<name> <site_activity>
# <tectonic_setting> <spreading_rate> <depth>" (multi-word underscore-joined, empty="-").
function _hydro_data(W, E, S, N)
	xs, ys, texts = _geo_points("hydrothermal_vents.dat", W, E, S, N)
	return xs, ys, _labeled_infos(texts, ("Name", "Site activity", "Tectonic setting", "Spreading rate", "Depth"))
end

# Meteorite-impact craters: trailing text is "<name> <diameter(km)> <age> <exposed> <type>", 5
# whitespace tokens. Like _labeled_infos but with the exposed Y/N decoded to Yes/No and type "-" ->
# "unknown", so it spells out its own loop.
function _meteorite_data(W, E, S, N)
	xs, ys, texts = _geo_points("meteoritos.dat", W, E, S, N)
	labels = ("Name", "Diameter (km)", "Age", "Exposed", "Type")
	infos = Vector{String}(undef, length(texts))
	for k in eachindex(texts)
		flds = split(texts[k])
		lines = String[]
		for i in 1:5
			raw = i <= length(flds) ? String(flds[i]) : ""
			if i == 4
				val = raw == "Y" ? "Yes" : raw == "N" ? "No" : raw
			elseif i == 5
				val = raw == "-" ? "unknown" : replace(raw, '_' => ' ')
			else
				val = replace(raw, '_' => ' ')
			end
			push!(lines, string(labels[i], ": ", val))
		end
		infos[k] = join(lines, '\n')
	end
	return xs, ys, infos
end

# Tide-gauge stations: trailing text is "<name> Code: <code> Country: <country>" (name/country
# underscore-joined). The label tokens "Code:"/"Country:" end in ':' -> drop colon-tokens and the 3
# values left are name/code/country (no regex needed).
function _tides_data(W, E, S, N)
	xs, ys, texts = _geo_points("mareg_online.dat", W, E, S, N)
	infos = Vector{String}(undef, length(texts))
	for k in eachindex(texts)
		vals = [replace(String(t), '_' => ' ') for t in split(texts[k]) if !endswith(t, ':')]
		infos[k] = string("Name: ", get(vals, 1, ""), "\nCode: ", get(vals, 2, ""), "\nCountry: ", get(vals, 3, ""))
	end
	return xs, ys, infos
end

# Push a GMTdataset (single or multi-segment) onto `scene` as a line overlay at z=0. Direct ccall
# (not _add_overlay!) because the callback only has the raw Scene*, not a QtFigure with a grid to
# sample z from — coastlines sit at sea level anyway, and the C overlay polygon-offsets them toward
# the camera so they don't z-fight the map.
function _add_geo_overlay(scene::Ptr{Cvoid}, D; color=(0.0, 0.0, 0.0), linewidth=1.0, name::AbstractString="")
	segs = D isa GMTdataset ? (D,) : collect(D)
	xyz = Float64[]
	segoff = Cint[0]
	off = 0
	for seg in segs
		m = seg isa GMTdataset ? seg.data : seg
		n = size(m, 1)
		for k in 1:n
			push!(xyz, Float64(m[k, 1]), Float64(m[k, 2]), 0.0)
		end
		off += n
		push!(segoff, Cint(off))
	end
	off == 0 && return false
	cr, cg, cb = color
	ok = ccall(_fn(:gmtvtk_add_overlay_h), Cint,
		  (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cstring),
		  scene, xyz, Cint(off), segoff, Cint(length(segs)), Cint(1), cr, cg, cb, Float64(linewidth), 0.0, name)
	return ok != 0
end

# C callback: creq = "<kind>/<res>/W/E/S/N". Fetch the feature for the visible region + add it.
function _on_geography(scene::Ptr{Cvoid}, creq::Cstring)::Cvoid
	try
		p = split(unsafe_string(creq), '/')
		length(p) >= 6 || return
		kind = String(p[1])
		res  = Symbol(strip(p[2]))
		W, E, S, N = parse.(Float64, p[3:6])
		# Preferences "Coastlines color" (Black|White): the colour for coast/borders/rivers lines.
		# Only black or white here — any other colour is set per-object via Line properties.
		coastrgb = (length(p) >= 7 && lowercase(strip(p[7])) == "white") ? (1.0, 1.0, 1.0) : (0.0, 0.0, 0.0)
		if kind == "volcano"
			# Mirone style: yellow filled triangles with a thin black edge, constant on-screen size.
			# Each symbol carries its 4-field metadata, shown as a tooltip when the mouse hovers it.
			xs, ys, infos = _volcano_data(W, E, S, N)
			isempty(xs) && return
			add_symbols!(scene, xs, ys; symbol=:triangle, size=12, fill=:yellow, edge=:black, edgewidth=1.0,
			             name="Volcanoes", info=infos)
		elseif kind == "meteorite"
			# Mirone style: red filled diamonds with a thin black edge, constant on-screen size.
			# Each carries its 5-field metadata (name/diameter/age/exposed/type) as a hover tooltip.
			xs, ys, infos = _meteorite_data(W, E, S, N)
			isempty(xs) && return
			add_symbols!(scene, xs, ys; symbol=:diamond, size=11, fill=:red, edge=:black, edgewidth=1.0,
			             name="Meteorite Impacts", info=infos)
		elseif kind == "tides"
			# Mirone's tide-gauge stations: red stars with a thin black edge, constant on-screen size.
			# Each carries its Name/Code/Country as a hover tooltip. Right-click a star for the
			# "Download Mareg (2 days)" / "(Calendar)" entries -> _on_tides_download.
			xs, ys, infos = _tides_data(W, E, S, N)
			isempty(xs) && return
			add_symbols!(scene, xs, ys; symbol=:star, size=8, sizeunit=:pt, fill=:red, edge=:black, edgewidth=1.0,
			             name="Tide Stations", info=infos)
		elseif kind == "hydro"
			# NOAA PMEL hydrothermal vents: orange filled circles with a thin black edge, screen-constant.
			# Each carries its 5-field metadata (name/site activity/tectonic/spreading rate/depth) as a tooltip.
			xs, ys, infos = _hydro_data(W, E, S, N)
			isempty(xs) && return
			add_symbols!(scene, xs, ys; symbol=:circle, size=9, fill=:orange, edge=:black, edgewidth=1.0,
			             name="Hydrothermal Vents", info=infos)
		else
			D = _geo_dataset(kind, res, W, E, S, N)
			(D === nothing || isempty(D)) && return
			# Source-identity naming: the line layer is named for the GMT feature it came from, so the
			# Scene Objects list reads "Coastlines"/"Boundaries"/"Rivers" instead of an anonymous "Line N".
			src = kind == "coast"   ? "Coastlines" :
			      kind == "borders" ? "Boundaries" :
			      kind == "rivers"  ? "Rivers"      :
			      uppercasefirst(kind)
			_add_geo_overlay(scene, D; color=coastrgb, name=src)
		end
	catch e
		_viewer_log_error(scene, "Geography FAILED: $(sprint(showerror, e))")
		@warn "geography: could not fetch/add the feature" exception=(e,)
	end
	return
end

# Build the C-callable pointer + register it. Called once from __init__.
function _register_geography()
	fptr = @cfunction((s,c)->Base.invokelatest(_on_geography,s,c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_geography_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# Turn a [start,end] ISO8601 range (from the C++ calendar dialog) into the (starttime, days) pair
# GMT.maregrams wants. Rejects any instant in the future — the IOC feed has no future data — and a
# non-positive span. UTC throughout (the dialog and the Profile time axis are both UTC). Pure (no
# network), so the test suite can exercise the future-date guard without a download. GMT re-exports
# DateTime/now; format/UTC come fully-qualified from GMT.Dates (Dates isn't a direct dep here).
function _tide_range(start_iso::AbstractString, end_iso::AbstractString,
                     nowt::GMT.Dates.DateTime = GMT.Dates.now(GMT.Dates.UTC))
	t0 = GMT.Dates.DateTime(start_iso)
	t1 = GMT.Dates.DateTime(end_iso)
	(t0 > nowt || t1 > nowt) && error("tides: date/time cannot be in the future")
	t1 > t0 || error("tides: end date/time must be after the start")
	days = (t1 - t0).value / 86_400_000          # milliseconds -> days (decimal accepted)
	return GMT.Dates.format(t0, "yyyy-mm-ddTHH:MM:SS"), days
end

# C callback for the two "Download Mareg …" entries on a Tide Stations star's right-click menu.
# `mode` is "2days" | "calendar"; `station` is the clicked star's "Name:/Code:/Country:" hover block,
# from which we pull the station code. The actual download + parse is GMT.jl's `maregrams` (same IOC
# Sea Level Monitoring service Mirone's mareg_online used) — we do NOT re-implement it. `maregrams`
# returns a GMTdataset: col 1 = time (epoch seconds), col 2 = sea level (prs/rad, m); attribs carry
# the station name/country. We open it in a fresh standalone X,Y plot window (x = time, y = sea
# level) rather than the shared 3-D Profile panel, so each download stands alone with its own
# Object Manager / Analysis / save. `mode` is "2days" for the quick entry, or
# "calendar/<startISO>/<endISO>" from the C++ calendar dialog (two date/time editors, UTC, capped
# at "now").
function _on_tides_download(scene::Ptr{Cvoid}, cmode::Cstring, cstation::Cstring)::Cvoid
	try
		station = unsafe_string(cstation)
		mode    = unsafe_string(cmode)
		m    = match(r"Code:\s*(\S+)", station)
		code = m === nothing ? "" : String(m.captures[1])
		isempty(code) && (@warn "tides: no station code found" station; return)
		# "calendar/<startISO>/<endISO>" -> a user-picked range; anything else -> the last 2 days.
		if startswith(mode, "calendar/")
			parts = split(mode, '/')
			length(parts) >= 3 || (@warn "tides: malformed calendar request" mode; return)
			starttime, days = _tide_range(parts[2], parts[3])
			D = GMT.maregrams(code = code, starttime = starttime, days = days)
		else
			D = GMT.maregrams(code = code, days = 2)    # last 2 days; GMT.jl does the IOC download
		end
		(D === nothing || size(D.data, 1) < 2) && (@warn "tides: nothing to plot" code; return)
		# Drop NaN sea-level samples (gaps) so the panel's min/max stay finite.
		t  = Float64.(view(D.data, :, 1)); v = Float64.(view(D.data, :, 2))
		ok = isfinite.(t) .& isfinite.(v)
		x  = t[ok]; y = v[ok]
		length(x) < 2 && (@warn "tides: no finite samples" code; return)
		nm    = get(D.attrib, "ST_name", "")
		title = isempty(nm) ? code : "$nm ($code)"
		ylab  = (length(D.colnames) >= 2) ? D.colnames[2] : "Sea level (m)"
		# Open the series in its OWN standalone X,Y plot window (Object Manager + Analysis + save)
		# instead of overwriting the 3-D viewer's shared bottom-dock Profile panel. X is epoch
		# seconds -> xtime=:date paints date/time labels (matches the old isDate=1 path).
		xyplot(x, y; name=title, title=title, xlabel="Time (UTC)", ylabel=ylab, xtime=:date)
	catch e
		_viewer_log_error(scene, "Tides download FAILED: $(sprint(showerror, e))")
		@warn "tides: download callback failed" exception=(e,)
	end
	return
end

# Build the C-callable pointer + register it. Called once from __init__.
function _register_tides()
	fptr = @cfunction((s,a,b)->Base.invokelatest(_on_tides_download,s,a,b), Cvoid, (Ptr{Cvoid}, Cstring, Cstring))
	ccall(_fn(:gmtvtk_set_tides_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# Earth tides (solid-Earth tidal displacement) via GMT.jl's `earthtide` — NOT re-implemented here.
# The C++ "Earth Tides" dialog (70_window.cpp) hands
# "<mode>/<startISO>/<endISO>/<lon>/<lat>/<comp>/<W>/<E>/<S>/<N>":
#   mode = "series" | "grid";  comp = subset of "VEN" (Vertical/East/North).
# `earthtide` maps components to GMT's -C letters x=East, y=North, z=Vertical.
const _ET_COMP = Dict('V' => ("z", "Vertical", "vert"), 'E' => ("x", "East", "east"),
                      'N' => ("y", "North", "north"))

# Time-series mode: GMT.earthtide(L=(lon,lat), T="start/stop/inc") returns a GMTdataset whose
# columns are Time/East/North/Vertical (Time = Unix epoch seconds). Plot each checked component in a
# standalone X,Y window with a date/time X axis (xtime=:date matches the epoch-seconds column).
function _earthtide_series(scene, start, stop, lon, lat, comp)
	t0 = GMT.Dates.DateTime(start); t1 = GMT.Dates.DateTime(stop)
	t1 > t0 || error("earthtide: end date/time must be after the start")
	span_min = (t1 - t0).value / 60_000                  # ms -> minutes
	inc = max(1, round(Int, span_min / 2000))            # aim ~2000 samples; never finer than 1 min
	D = GMT.earthtide(L=(lon, lat), T="$start/$stop/$(inc)m")
	(D === nothing || size(D.data, 1) < 2) && (@warn "earthtide: empty time series"; return)
	t = Float64.(view(D.data, :, 1))
	colidx = Dict("East" => 2, "North" => 3, "Vertical" => 4)
	pl = nothing
	for ch in comp
		haskey(_ET_COMP, ch) || continue
		label = _ET_COMP[ch][2]
		y = Float64.(view(D.data, :, colidx[label]))
		if pl === nothing
			pl = xyplot(t, y; name=label, title="Earth tides @ ($lon, $lat)",
			            xlabel="Time (UTC)", ylabel="Displacement (m)", xtime=:date)
		else
			add!(pl, t, y; name=label)
		end
	end
	return
end

# Grid mode: GMT.earthtide(R=global, I=1°, T=startISO, C=<letter>) returns a GMTgrid of the chosen
# component's displacement at one instant. Grids are ALWAYS global (a solid-Earth tide is a whole-
# planet field). Only the first checked component is gridded (one grid). Opens in its OWN viewer
# window (iview) — NOT overlaid on the current scene: the tidal displacement is sub-metre, so adding
# it as a second surface buries it flat under the existing relief (invisible). The surface row in the
# new window's Scene Objects is named "earth tide <vert|east|north>".
function _earthtide_grid(scene, start, comp, inc=0.5)
	letter, _, short = _ET_COMP[first(comp)]
	G = GMT.earthtide(R=(-180.0, 180.0, -90.0, 90.0), I=inc, T=start, C=letter)
	(G === nothing) && error("earthtide: grid not produced")
	iview(G; title="earth tide $short")
	return
end

# C callback for the Earth Tides dialog. Parses the request and runs the chosen mode.
function _on_earthtide(scene::Ptr{Cvoid}, creq::Cstring)::Cvoid
	try
		p = split(unsafe_string(creq), '/')
		length(p) >= 6 || return
		mode = String(p[1]); start = String(p[2]); stop = String(p[3])
		lon = parse(Float64, p[4]); lat = parse(Float64, p[5])
		comp = String(p[6]); isempty(comp) && (comp = "V")
		if mode == "grid"
			inc = (length(p) >= 7 && !isempty(p[7])) ? parse(Float64, p[7]) : 0.5
			_earthtide_grid(scene, start, comp, inc)     # always global; ignores the region fields
		else
			_earthtide_series(scene, start, stop, lon, lat, comp)
		end
	catch e
		_viewer_log_error(scene, "Earth Tides FAILED: $(sprint(showerror, e))")
		@warn "earthtide: callback failed" exception=(e,)
	end
	return
end

# Build the C-callable pointer + register it. Called once from __init__.
function _register_earthtide()
	fptr = @cfunction((s,c)->Base.invokelatest(_on_earthtide,s,c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_earthtide_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
