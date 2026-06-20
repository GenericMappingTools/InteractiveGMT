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

# Volcanoes clipped to the visible region, WITH the per-point metadata for hover tooltips. Read via
# the universal reader (GMT.gmtread) instead of a hand-rolled parser: the bundled Mirone file
# data/volcanoes.dat is whitespace/tab-separated with LAT col 1, LON col 2, then 4 text columns
# (name, country, type, age) whose multi-word values are underscore-joined into single tokens. So
# gmtread returns `.data = [lat lon]` and `.text[k]` = the 4 tokens; we split on whitespace and
# turn underscores back into spaces. Lons in the file are -180..180; the map region may be 0..360
# (Pacific-centred whole earth), so a point counts if any of lon±360 lands in [W,E], plotted there.
# Returns (xs, ys, infos): xs/ys lon/lat in view, infos[k] a 4-line "<label>: <value>" block.
function _volcano_data(W, E, S, N)
	path = joinpath(_PKGROOT, "data", "volcanoes.dat")
	isfile(path) || (@warn "geography: volcanoes.dat not found" path; return (Float64[], Float64[], String[]))
	Dr = GMT.gmtread(path)
	D  = Dr isa AbstractVector ? Dr[1] : Dr        # no segment headers -> single GMTdataset
	lat = D.data[:, 1]; lon = D.data[:, 2]
	txt = D.text                                   # one trailing-text string per row (the 4 tokens)
	labels = ("Name", "Country", "Type", "Age")
	xs = Float64[]; ys = Float64[]; infos = String[]
	for k in eachindex(lat)
		S <= lat[k] <= N || continue
		x = nothing
		for d in (-360.0, 0.0, 360.0)
			(W <= lon[k] + d <= E) && (x = lon[k] + d; break)
		end
		x === nothing && continue
		flds = (txt !== nothing && k <= length(txt)) ? split(txt[k]) : SubString{String}[]
		lines = ntuple(4) do i                     # underscores -> spaces; missing field -> blank value
			v = i <= length(flds) ? replace(String(flds[i]), '_' => ' ') : ""
			string(labels[i], ": ", v)
		end
		push!(xs, x); push!(ys, lat[k]); push!(infos, join(lines, '\n'))
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
			add_symbols!(scene, xs, ys;
			             symbol=:triangle, size=12, fill=:yellow, edge=:black, edgewidth=1.0,
			             name="Volcanoes", info=infos)
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
