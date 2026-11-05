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

# The ONE name every travel-time result carries in Scene Objects (see the delivery below).
const _TTT_NAME = "Travel time (h)"

# Peak memory per grid node of each step, measured 2026-11-05 (Julia 1.14, Windows, growth of the
# process's peak commit): the Huygens solver (GMT.ttt) 23.8-25.2 bytes (4M-36M nodes), the Mirone
# expansion (GMT.wave_travel_time) 12.1 (4M-36M), the delivery into the window (_gm3d_deliver: the C++
# surface, its z copy, its LOD tiles) ~22 (16M), plus a one-off ~450 MiB the first big surface of a
# window cost; writing the result to disk instead (GMT.gmtwrite, .grd) 12.4 (16M). Rounded up.
const _TTT_BYTES_PER_NODE = (ttt = 26, mirone = 13, deliver = 22, write = 13)
const _TTT_FIXED_BYTES = 512 * 2^20

# What the process can still allocate, in bytes. Windows refuses an allocation when the COMMIT limit
# (RAM + page file) is reached, not when free RAM is, so that is the number asked for there
# (GlobalMemoryStatusEx's ullAvailPageFile); elsewhere the OS's available memory.
mutable struct _MemStatusEx
	dwLength::UInt32; dwMemoryLoad::UInt32
	ullTotalPhys::UInt64; ullAvailPhys::UInt64; ullTotalPageFile::UInt64; ullAvailPageFile::UInt64
	ullTotalVirtual::UInt64; ullAvailVirtual::UInt64; ullAvailExtendedVirtual::UInt64
end
function _mem_available()::Int
	Sys.iswindows() || return Int(Sys.free_memory())
	m = _MemStatusEx(UInt32(sizeof(_MemStatusEx)), 0, 0, 0, 0, 0, 0, 0, 0)
	ccall((:GlobalMemoryStatusEx, "kernel32"), Cint, (Ref{_MemStatusEx},), m) == 0 &&
		return Int(Sys.free_memory())
	return Int(m.ullAvailPageFile)
end

# Refuse a step that would not fit, BEFORE it allocates: running out half-way can abort the whole
# process (a C++ std::bad_alloc, or Julia's own "fatal: out of memory" inside a GC) instead of
# raising an error the dialog can report. GC first, so garbage Julia can free does not count as used.
function _ttt_mem_check(need::Int, what::String)
	GC.gc()
	avail = _mem_available()
	need <= avail && return
	gb(b) = round(b / 2^30; digits = 1)
	error("not enough free memory to $what: it needs about $(gb(need)) GB and $(gb(avail)) GB are free. " *
	      "Close other programs or use a coarser grid.")
end

# The source the dialog placed (TttDialog::sourceXY): "x,y;x,y;…" — ONE point ("Plot point source"),
# or the vertices of a fault line ("Draw Fault source"), each vertex a point source. Returned as an
# Mx2 matrix, the shape `ttt` takes for a multi-point source. A fault line also gets a source at its
# MIDDLE (half its length along the line), so a two-vertex fault is not just its two ends.
function _ttt_source(d::Dict{String,String}, geog::Bool)
	s = _get(d, "srcxy")
	isempty(s) && error("Yes I compute, but ... WHAT?")
	pts = split(s, ';'; keepempty = false)
	M = Matrix{Float64}(undef, length(pts), 2)
	for (k, p) in enumerate(pts)
		xy = split(p, ',')
		length(xy) == 2 || error("bad source point \"$p\"")
		M[k, 1] = parse(Float64, xy[1]);  M[k, 2] = parse(Float64, xy[2])
	end
	size(M, 1) >= 2 && (M = vcat(M, _ttt_midpoint(M[:, 1], M[:, 2], geog)'))
	return M
end

# The point half-way ALONG a line. Lengths come from THE line-length function (`_seg_dist_azim`,
# measure.jl — the Measure tools' own, never a second one): geographic = geodesic km plus each
# segment's forward azimuth, and the step into the segment that holds the half-way mark is the direct
# geodesic (`_geod_fwd`, deform.jl); Cartesian = plain interpolation in data units.
function _ttt_midpoint(x::Vector{Float64}, y::Vector{Float64}, geog::Bool)
	inc, az = _seg_dist_azim(x, y, geog)          # inc[i], az[i]: the segment (i-1) -> i
	half = sum(inc) / 2
	acc = 0.0
	for i in 2:length(x)
		if acc + inc[i] >= half && inc[i] > 0
			r = half - acc
			geog && return collect(_geod_fwd(x[i-1], y[i-1], az[i], r))
			t = r / inc[i]
			return [x[i-1] + t * (x[i] - x[i-1]), y[i-1] + t * (y[i] - y[i-1])]
		end
		acc += inc[i]
	end
	return [x[1], y[1]]                            # zero-length line: its one point
end

# The stations for `tttimes`: same file shape as the sources plus an optional trailing NAME, which
# the table's last (text) column shows with each arrival time. A GMTdataset carries those names in `.text` already, so they
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
		# An arrival-time run reads the travel-time grid ON DISPLAY (the dialog sends the active layer's
		# label). A travel-time run is computed on LEVEL 0 ONLY — the window's base bathymetry, the grid
		# NSWING calls layer0 (`_nswing_validate`) — whatever is on display: a nested grid (layerN), the
		# previous travel-time grid (hours read as depths -> all "land"), or any other layer.
		G = what == "eta" ? _active_grid(scene, _get(d, "grid")) :   # THE Grid Tools resolver (savefile.jl)
		                    _nested_level0(scene)
		G === nothing && error("no grid in this window")

		if what == "eta"
			stations, names = _ttt_stations(d)
			origin = GMT.Dates.DateTime(0)
			os = _get(d, "origin")
			isempty(os) || (origin = GMT.Dates.DateTime(os))   # ISO 8601, e.g. 2011-03-11T05:46:24
			Dtab = GMT.tttimes(G, stations; names = names, origin = origin, utc = _on(d, "utc"))
			# Its text column ("name | arrival") is named in the comment; give it to the table's header.
			(length(Dtab.colnames) == size(Dtab.data, 2) && !isempty(Dtab.comment)) &&
				push!(Dtab.colnames, Dtab.comment[1])
			return _gm3d_deliver(scene, Dtab, "Arrival times", _get(d, "outfile"), true,
			                     "tttimes(travel_time_grid, stations)")
		end

		mirone = (_get(d, "method", "ttt") == "mirone")
		M = _ttt_source(d, _isgeographic(G))
		src = size(M, 1) == 1 ? (M[1, 1], M[1, 2]) : M
		# A result the user asked to SAVE goes to disk only and is never displayed: building its surface
		# is what costs the window the most memory, and a grid that big is the reason to ask for a file.
		outfile = _get(d, "outfile")
		tofile = !isempty(outfile)
		# The run AND what follows it (the write, or the delivery) must fit: the result stays alive
		# meanwhile. A Mirone fault source holds two wavefronts at once (R and Rk below); a non-Float32
		# grid is first copied to Float32 (4 bytes a node).
		nodes = length(G.z)
		per = mirone ? _TTT_BYTES_PER_NODE.mirone * (size(M, 1) > 1 ? 2 : 1) + (eltype(G.z) == Float32 ? 0 : 4) :
		               _TTT_BYTES_PER_NODE.ttt
		_ttt_mem_check(tofile ? nodes * (per + _TTT_BYTES_PER_NODE.write) :
		                        nodes * (per + _TTT_BYTES_PER_NODE.deliver) + _TTT_FIXED_BYTES,
		               "compute the travel times on this $(round(nodes / 1e6; digits = 1)) M-node grid")
		local R, recipe
		if mirone
			# `wave_travel_time`'s Mirone path is typed on GMTgrid{Float32,2}; a grid of any other
			# element type is rebuilt through mat2grid, which keeps the header, the x/y, the proj AND
			# the memory layout (never a transposition — SACRED_LAW.md's grid-layout law).
			Gf = (G isa GMT.GMTgrid{Float32,2}) ? G : GMT.mat2grid(Float32.(G.z), G)
			# Geographic by THE grid test the window itself uses (`_isgeographic`, cpt.jl) — never a second
			# one: a CRS-less lon/lat bathymetry is geographic to the window, and must be to the solver.
			geo = _isgeographic(G);  fv = _on(d, "fillvoids")
			# A fault source is Mirone's: one wavefront per vertex, and the travel time is the EARLIEST
			# arrival from any of them (tsu_funs.m TTT: tt = min(tt, tt_)).
			R = GMT.wave_travel_time(Gf, (M[1, 1], M[1, 2]); geo = geo, fill_voids = fv)
			for k in 2:size(M, 1)
				Rk = GMT.wave_travel_time(Gf, (M[k, 1], M[k, 2]); geo = geo, fill_voids = fv)
				# MATLAB's min skips NaN (a node one wavefront never reached); Julia's propagates it.
				R.z .= ((a, b) -> isnan(a) ? b : isnan(b) ? a : min(a, b)).(R.z, Rk.z)
			end
			size(M, 1) > 1 && (R.range[5:6] .= extrema(filter(!isnan, R.z)))
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
		tofile && (GMT.gmtwrite(outfile, R); return Cint(1))
		_ttt_mem_check(nodes * _TTT_BYTES_PER_NODE.deliver + _TTT_FIXED_BYTES, "display the travel-time grid")
		ok = _gm3d_deliver(scene, R, _TTT_NAME, "", false, recipe)
		# 0 from a window that is still open: the viewer ran out of memory building the surface (its
		# add/promote exports catch the std::bad_alloc and return 0 instead of aborting the process).
		(ok == 0 && ccall(_fn(:gmtvtk_is_alive), Cint, (Ptr{Cvoid},), scene) != 0) &&
			error("not enough memory to display the travel-time grid")
		return ok
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
