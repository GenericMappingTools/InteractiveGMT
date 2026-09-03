# tsunamittt.jl — Geophysics > Tsunamis > "Tsunami travel times…": GMT.jl's travel-time API, driven
# from the viewer.
#
# THREE GMT.jl entry points, ONE tool, because they are one workflow:
#   GMT.ttt(G, source; …)              Wessel's Huygens/Dijkstra wavefront (ttt API 4.0.1) — a grid
#                                      of travel times in HOURS.
#   GMT.wave_travel_time(G, source; …) the faster Mirone expansion, same output, `method=:mirone`.
#   GMT.tttimes(Gtt, stations; …)      the arrival times read off that grid, as a table.
#
# The maths is GMT.jl's, in full — nothing here re-implements a wavefront, a distance or a gravity
# (SACRED_LAW.md: one quantity, one function, and the projection/geodesy always comes from GMT).
# This file is the bridge: parse the dialog's key=value block, resolve the grid the window is
# showing, call, and hand the result to the SHARED delivery (`_gm3d_deliver`, gravmag3d.jl) that
# every other derived-grid tool goes through — so the travel-time grid arrives as a new named
# variable with its own axes and its own colour bar, and the station table opens in the shared table
# window, with no private copy of either transition.

# The Huygens stencil sizes the ttt API accepts. The dialog offers exactly these; this is the backstop.
const _TTT_NODES = (8, 16, 32, 48, 64, 120)

# The source, as `ttt`/`wave_travel_time` want it: a file of "lon lat" rows wins over the two boxes,
# so a multi-source run needs no second control. `ttt` takes an Mx2 matrix; the Mirone method is
# single-source by construction (its wavefront starts at ONE node), so it is told so rather than
# silently using the first row of a file the user chose deliberately.
function _ttt_source(d::Dict{String,String}, mirone::Bool)
	f = _get(d, "srcfile")
	if !isempty(f)
		isfile(f) || error("source file not found: $f")
		D = GMT.gmtread(f)
		M = isa(D, Vector) ? D[1].data : D.data
		size(M, 2) >= 2 || error("the source file needs at least two columns (lon lat)")
		mirone && size(M, 1) > 1 &&
			error("the Mirone method takes ONE source point — pick the ttt method for a multi-point source")
		return size(M, 1) == 1 ? (Float64(M[1,1]), Float64(M[1,2])) : Float64.(M[:, 1:2])
	end
	lon, lat = _get(d, "lon"), _get(d, "lat")
	(isempty(lon) || isempty(lat)) && error("give me the source longitude and latitude")
	return (parse(Float64, lon), parse(Float64, lat))
end

# The stations for `tttimes`: same file shape as the sources plus an optional trailing NAME, which is
# what the table's last column shows. A GMTdataset carries those names in `.text` already, so they
# are taken from there rather than re-parsed.
function _ttt_stations(d::Dict{String,String})
	f = _get(d, "stations")
	isempty(f) && error("give me a stations file (lon lat [name], one per line)")
	isfile(f) || error("stations file not found: $f")
	D = GMT.gmtread(f)
	Ds = isa(D, Vector) ? D[1] : D
	M = Ds.data
	size(M, 2) >= 2 || error("the stations file needs at least two columns (lon lat)")
	names = (hasproperty(Ds, :text) && length(Ds.text) == size(M, 1)) ? String.(Ds.text) : String[]
	return Float64.(M[:, 1:2]), names
end

# C callback (both buttons). `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaTttFn. Returns Cint 1 on success, 0 on failure — TttDialog reports it on the
# window the user is looking at.
function _on_ttt(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		what = _get(d, "what", "grid")
		# The grid to work on is the one ON DISPLAY (the dialog sends the active layer's label) — for a
		# travel-time run that is the bathymetry, for an arrival-time run the travel-time grid itself.
		G = _find_object(scene, :grid, _get(d, "grid"))
		G === nothing && error("no grid in this window")

		if what == "eta"
			stations, names = _ttt_stations(d)
			origin = GMT.Dates.DateTime(0)
			os = _get(d, "origin")
			isempty(os) || (origin = GMT.Dates.DateTime(os))   # ISO 8601, e.g. 2011-03-11T05:46:24
			Dtab = GMT.tttimes(G, stations; names = names, origin = origin, utc = _on(d, "utc"))
			return _gm3d_deliver(scene, Dtab, "Arrival times", _get(d, "outfile"), true,
			                     "tttimes(travel_time_grid, stations)")
		end

		mirone = (_get(d, "method", "ttt") == "mirone")
		src = _ttt_source(d, mirone)
		local R, recipe
		if mirone
			# `wave_travel_time`'s Mirone path is typed on GMTgrid{Float32,2}; a grid of any other
			# element type is rebuilt through mat2grid, which keeps the header, the x/y, the proj AND
			# the memory layout (never a transposition — SACRED_LAW.md's grid-layout law).
			Gf = (G isa GMT.GMTgrid{Float32,2}) ? G : GMT.mat2grid(Float32.(G.z), G)
			R = GMT.wave_travel_time(Gf, src; geo = _isgeog(G), fill_voids = _on(d, "fillvoids"))
			recipe = "wave_travel_time(bathymetry, $(src); method=:mirone)"
		else
			nodes = parse(Int, _get(d, "nodes", "120"))
			(nodes in _TTT_NODES) || error("stencil nodes must be one of $(join(_TTT_NODES, ", "))")
			search  = _on(d, "search")
			radius  = parse(Float64, _get(d, "radius", "0"))
			srcdep  = parse(Float64, _get(d, "srcdepth", "0"))
			mindep  = parse(Float64, _get(d, "mindepth", "0"))
			# The two depth thresholds are DEPTHS: negative down, and the API refuses a positive one
			# outright. Say which box is wrong here instead of letting an error code surface.
			srcdep > 0 && error("the source depth threshold is a depth — use 0 or a negative value")
			mindep > 0 && error("the shallow depth threshold is a depth — use 0 or a negative value")
			R = GMT.ttt(G, src; nodes = nodes, search = search, search_radius = radius,
			            source_depth = srcdep, min_depth = mindep,
			            bias = _get(d, "bias", "1") == "1")     # the API's own default is bias ON
			recipe = "ttt(bathymetry, $(src); nodes=$nodes)"
		end
		# ONE name for the quantity, whichever method produced it: recomputing REPLACES the previous
		# result (that is what _gm3d_deliver's remove+forget does) instead of piling up near-identical
		# layers the user then has to tell apart.
		return _gm3d_deliver(scene, R, "Travel time (h)", _get(d, "outfile"), false, recipe)
	catch e
		_tool_failed(scene, "Tsunami travel times", e)
		return Cint(0)
	end
end

function _register_ttt()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_ttt, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_ttt_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
