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
# longitude periodicity and always returning .data as geographic [lon lat] (so a lat,lon-stored file
# like volcanoes.dat needs no swap). We normalize each kept lon into the map's [W,E] frame with one
# mod (map and data may differ: -180..180 vs 0..360, either direction). `datafile` is a bare name
# under data/. Returns (xs, ys, texts): xs/ys lon/lat in view, texts[k] the row's trailing text
# (Latin-1-fixed: the files have accented names / "±"); each caller turns texts into its own tooltip.
function _geo_points(datafile::AbstractString, W, E, S, N)
	path = joinpath(_PKGROOT, "data", datafile)
	isfile(path) || (@warn "geography: data file not found" path; return (Float64[], Float64[], String[]))
	Sr = GMT.gmtselect(path, R=(W, E, S, N), f=:g)        # read + region/lon-periodic clip in ONE call
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
# values underscore-joined).
function _volcano_data(W, E, S, N)
	xs, ys, texts = _geo_points("volcanoes.dat", W, E, S, N)
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
			_add_geo_overlay(scene, D; name=src)
		end
	catch e
		@warn "geography: could not fetch/add the feature" exception=(e,)
	end
	return
end

# Build the C-callable pointer + register it. Called once from __init__.
function _register_geography()
	fptr = @cfunction(_on_geography, Cvoid, (Ptr{Cvoid}, Cstring))
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
# the station name/country. We plot it in the window's "Profile" panel (x = time, y = sea level).
# `mode` is "2days" for the quick entry, or "calendar/<startISO>/<endISO>" from the C++ calendar
# dialog (two date/time editors, UTC, capped at "now"). The Profile panel is shared with the
# elevation profiler; see the C-side gmtvtk_show_profile_xy comment.
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
		ccall(_fn(:gmtvtk_show_profile_xy), Cint,
		      (Ptr{Cvoid}, Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cstring, Cstring, Cstring, Cint),
		      scene, x, y, Cint(length(x)), title, "Time (UTC)", ylab, Cint(1))
	catch e
		@warn "tides: download callback failed" exception=(e,)
	end
	return
end

# Build the C-callable pointer + register it. Called once from __init__.
function _register_tides()
	fptr = @cfunction(_on_tides_download, Cvoid, (Ptr{Cvoid}, Cstring, Cstring))
	ccall(_fn(:gmtvtk_set_tides_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
