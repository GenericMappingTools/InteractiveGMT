# Grid Tools > Drainage analysis… — catchments, rivers and flow products from the DEM on display,
# computed by GMT.jl's hydrology functions (GMT.jl/src/hydro: flowdir, flowacc, drainagebasins,
# streamnet, streamorder, flowdistance, twi, ldd).
#
# The dialog (deps/src/76_hydrology.cpp) talks to this file through four eval doors:
#   _hydro_run(scene, "key=value;…")  start the run on a spawned thread; returns "" or an error text
#   _hydro_poll(scene)                "state|percent|text": run / adopt / done / fail / cancel / none
#   _hydro_finish(scene)              main thread, once poll said "adopt": put the results in the window
#   _hydro_stop(scene)                ask the running job to stop (checked at every progress report)
#
# The work runs on a spawned thread (the app is launched `julia -t auto`) so the Qt pump keeps
# ticking; the thread only touches its own job record (atomics + a lock-protected stage text). Every
# viewer call (adding grids and lines) happens in _hydro_finish, on the main thread.

mutable struct _HydroJob
	task::Task
	frac::Threads.Atomic{Float64}
	cancel::Threads.Atomic{Bool}
	lock::ReentrantLock
	stage::String
	err::String
	results::Vector{Tuple{String,Union{GMTgrid,Vector{GMTdataset{Float64,2}}}}}
	src::String
	t0::Float64
end

const _HydroLines = Vector{GMTdataset{Float64,2}}     # what streamnet returns
const _HYDRO_JOBS = Dict{Ptr{Cvoid},_HydroJob}()
# Each window's river networks, by their Scene Objects name: one dataset per reach, as plotted. Purged
# with the window (_forget_window!, savefile.jl).
const _HYDRO_RIVERS = Dict{Ptr{Cvoid},Dict{String,_HydroLines}}()

_hydro_flag(d::Dict{String,String}, k::String)::Bool = get(d, k, "0") == "1"

function _hydro_num(d::Dict{String,String}, k::String, def::Float64)::Float64
	v = tryparse(Float64, strip(get(d, k, "")))
	return v === nothing ? def : v
end

# Are the DEM's cell sizes in metres? Geographic grids: flowdir computes metres itself. Projected
# grids: when their CRS says metres. A bare Cartesian grid has unknown units.
function _hydro_metric(G::GMTgrid, geog::Bool)::Bool
	geog && return true
	G.epsg != 0 && return true
	p = G.proj4 * " " * G.wkt
	return occursin("+units=m", p) || occursin("UNIT[\"metre\"", p) || occursin("UNIT[\"meter\"", p)
end

# An area typed in the dialog -> number of cells.
function _hydro_cells(val::Float64, unit::String, G::GMTgrid, geog::Bool, dx::Vector{Float64}, dy::Float64)::Float64
	unit == "cells" && return val
	_hydro_metric(G, geog) || error("Areas in km² need a georeferenced DEM (geographic, or projected in metres). Use cells.")
	cellarea = sum(dx) / length(dx) * dy               # m², mean over the rows
	return val * 1e6 / cellarea
end

# Report from the worker thread: progress callback handed to every GMT.jl hydro function. `base` and
# `span` map the function's own 0..1 onto its share of the whole run. Returns false to cancel.
function _hydro_reporter(job::_HydroJob, base::Float64, span::Float64, what::String)
	return function (f, stage)
		job.frac[] = base + span * Float64(f)
		lock(job.lock) do
			job.stage = isempty(what) ? String(stage) : what * ": " * String(stage)
		end
		return !job.cancel[]
	end
end

function _hydro_setstage!(job::_HydroJob, f::Float64, s::String)
	job.cancel[] && throw(GMT.HydroCancelled())
	job.frac[] = f
	lock(job.lock) do
		job.stage = s
	end
	return nothing
end

# log10 of a positive field, in place (accumulation spans many orders of magnitude).
function _hydro_log10!(G::GMTgrid)
	z = G.z
	@inbounds for k in eachindex(z)
		v = z[k]
		z[k] = (isnan(v) || v <= 0) ? NaN32 : log10(v)
	end
	GMT.setgrdminmax!(G)
	return G
end

function _hydro_scale!(G::GMTgrid, s::Float64)
	z = G.z
	@inbounds for k in eachindex(z);  z[k] = Float32(z[k] * s)  end
	GMT.setgrdminmax!(G)
	return G
end

# The cells `drainagebasins` drops (NaN: their basin is under the min. area) become ONE class, 0 =
# "coastal / residual drainage". Along a coast these are thousands of tiny catchments, each draining
# straight to the sea on its own outlet (measured on algarve50.grd: 4555 of them, all < 1000 cells,
# 10 % of the land), so together they cover large areas that would otherwise be left blank. Cells
# with no data (or the sea, when "Sea is not land" is ticked) stay NaN. `B` is built by GMT.jl's hydro
# on the same (ny,nx) south-first grid as `F.dem`; both are read through their layouts anyway.
function _hydro_residual_class!(B::GMTgrid, F)
	Z = _zmat(B)
	D = F.dem
	size(Z) == size(D) || error("basins grid $(size(Z)) does not match the flow grid $(size(D))")
	@inbounds for I in CartesianIndices(Z)
		(isnan(Z[I]) && !isnan(D[I])) && (Z[I] = 0.0f0)
	end
	GMT.setgrdminmax!(B)
	return B
end

# Colours of a CLASS grid (basin numbers, LDD codes): GMT's categorical palette, one colour per class,
# taken in turn and cycled past its last colour — so neighbouring numbers get different colours and
# any number of classes has one (`makecpt(range=…)` refuses more classes than the palette's 256). Read
# once, on first use: nothing is computed at load time.
const _HYDRO_PALETTE = Ref{Matrix{Float64}}(zeros(0, 3))
function _hydro_class_color(k::Int)::NTuple{3,Float64}
	if isempty(_HYDRO_PALETTE[])
		cm = Float64.(makecpt(cmap = :categorical).colormap)
		maximum(cm) > 1.0 && (cm = cm ./ 255)
		_HYDRO_PALETTE[] = cm[:, 1:3]
	end
	P = _HYDRO_PALETTE[]
	i = mod(k - 1, size(P, 1)) + 1
	return (P[i, 1], P[i, 2], P[i, 3])
end
# The basins: the class 0 (coastal / residual drainage) in a neutral light grey, so it reads as "not a
# basin"; every basin 1..N its own palette colour.
_hydro_basin_color(k::Int)::NTuple{3,Float64} = k == 0 ? (0.82, 0.82, 0.82) : _hydro_class_color(k)

# The rivers through the line simplification (Douglas-Peucker, GMT.simplify = GEOS), so a reach does
# not carry one vertex per cell along its straight runs. `tol` is in the grid's units (half a cell:
# the line stays inside the cells it runs through). GMT.simplify works on x,y and returns geometry
# only, so it is handed one reach at a time (given the whole vector it returns a single dataset) and
# its result is used as the list of vertices KEPT: Douglas-Peucker never moves a vertex, so each kept
# one is found, in order, among the reach's own rows, and those rows — x, y, z and order, header,
# attributes, CRS — are what remains.
function _hydro_simplify_lines!(S::_HydroLines, tol::Float64)::_HydroLines
	tol > 0 || return S
	for d in S
		m = d.data
		n = size(m, 1)
		n <= 2 && continue
		r = try GMT.simplify(GMT.mat2ds(m[:, 1:2]), tol) catch; nothing end
		r === nothing && continue
		s = r isa GMTdataset ? r.data : (isempty(r) ? nothing : first(r).data)
		(s === nothing || size(s, 1) < 2 || size(s, 1) >= n) && continue
		keep = Int[]
		j = 1
		for v = 1:size(s, 1)
			while j <= n && !(m[j, 1] == s[v, 1] && m[j, 2] == s[v, 2])
				j += 1
			end
			j > n && break
			push!(keep, j)
			j += 1
		end
		length(keep) == size(s, 1) || continue       # not a subset of the reach's vertices: leave it whole
		d.data = m[keep, :]
		GMT.set_dsBB!(d)
	end
	return S
end

# The DEM as GMT.jl's hydro functions can read it. Their reader (GMT._hydro_canon) takes a grid whose
# z has Julia dims (ny,nx) as COLUMN-major whatever its layout says, and only recognises a row-major
# buffer when its dims are (nx,ny). Every grid iGMT reads is "TRB" (row-major, (ny,nx) dims — the grid
# layout law), so a non-square DEM was handed over scrambled and every result was garbage. Same
# buffer, reshaped to (nx,ny) — no copy, no transposition of ours; GMT.jl then de-rows it itself,
# as it must anyway to build its own matrix. A column-major grid is read right as it is.
function _hydro_dem(G::GMTgrid)::GMTgrid
	(_grid_layout_code(G) & 1) == 0 && return G
	nx, ny = _grid_dims(G)
	return GMT.GMTgrid(; proj4 = G.proj4, wkt = G.wkt, epsg = G.epsg, geog = G.geog, range = G.range,
	                   inc = G.inc, registration = G.registration, nodata = G.nodata,
	                   x = G.x, y = G.y, z = reshape(G.z, nx, ny), layout = G.layout)
end

# The outlines of the basins of a label grid, as polygons in the grid's own coordinates: one closed
# line per connected region of one basin number (GMT.polygonize = GDALPolygonize, exact cell edges).
# The labels are first framed by a one-cell border of -1, and the cells without data (NaN: no data,
# the sea) are set to -1 too; the polygons of value -1 are dropped. (Not 0: that is a real class, the
# coastal / residual drainage of `_hydro_residual_class!`, and it gets its outline.) The frame is
# not cosmetic: GMT.polygonize drops its LAST polygon when that polygon's bbox equals the grid's
# (meant for GDAL's spurious whole-area ring), and a basin touching all four edges — a single basin
# covering the DEM is the extreme case — is exactly such a polygon. Framed, no basin can span the
# padded grid, so no real basin is ever taken for that ring.
function _hydro_basin_outlines(B::GMTgrid)::_HydroLines
	Z = _zmat(B)                                        # (ny, nx), row 1 = south
	ny, nx = size(Z)
	M = fill(-1.0f0, ny + 2, nx + 2)
	@inbounds for c = 1:nx, r = 1:ny
		v = Z[r, c]
		M[r + 1, c + 1] = isnan(v) ? -1.0f0 : Float32(v)
	end
	reg = Int(B.registration)
	dx, dy = B.inc[1], B.inc[2]
	x = collect(range(B.x[1] - dx, step = dx, length = nx + 2 + reg))
	y = collect(range(B.y[1] - dy, step = dy, length = ny + 2 + reg))
	P = GMT.mat2grid(M; x = x, y = y, reg = reg, proj4 = B.proj4, wkt = B.wkt, epsg = B.epsg)
	D = GMT.polygonize(P)
	out = GMTdataset{Float64,2}[]
	(D === nothing || isempty(D)) && return out
	for d in (D isa GMTdataset ? [D] : D)
		lab = tryparse(Float64, string(get(d.attrib, "Px", "-1")))
		(lab === nothing || lab < 0) && continue
		push!(out, d)
	end
	return out
end

# The whole computation, on the worker thread. Results are only collected here; nothing touches the
# viewer.
function _hydro_compute!(job::_HydroJob, G::GMTgrid, d::Dict{String,String})
	src = job.src
	sea = _hydro_flag(d, "sea") ? 0.0 : nothing
	F = flowdir(_hydro_dem(G); method = _hydro_flag(d, "carve") ? :carve : :fill, sealevel = sea,
	            geog = _isgeographic(G), progress = _hydro_reporter(job, 0.0, 0.6, "Flow directions"))
	unit   = get(d, "unit", "cells")
	metric = _hydro_metric(G, F.geog)
	want = [k for k in ("acc", "twi", "dist", "ldd", "order", "streams", "basins") if _hydro_flag(d, k)]
	n = length(want);  step = 0.4 / max(n, 1)
	res = job.results
	for (i, k) in enumerate(want)
		base = 0.6 + (i - 1) * step
		if k == "acc"
			m = Symbol(get(d, "acc_method", "d8"))
			_hydro_setstage!(job, base, "Flow accumulation ($(uppercase(String(m))))")
			A = flowacc(F; method = m, area = metric, progress = _hydro_reporter(job, base, step, ""))
			metric && _hydro_scale!(A, 1e-6)
			_hydro_log10!(A)
			push!(res, ("$src log10 upstream area ($(metric ? "km²" : "cells"), $(uppercase(String(m))))", A))
		elseif k == "twi"
			m = Symbol(get(d, "twi_method", "mfd"))
			_hydro_setstage!(job, base, "Wetness index")
			T = twi(F; method = m, progress = _hydro_reporter(job, base, step, "Wetness index"))	# twi's own stages are "Flow accumulation (…)" / "Slope and wetness index"
			push!(res, ("$src wetness index (TWI, $(uppercase(String(m))))", T))
		elseif k == "dist"
			_hydro_setstage!(job, base, "Flow distance")
			D = flowdistance(F)
			metric && _hydro_scale!(D, 1e-3)
			push!(res, ("$src flow distance to outlet$(metric ? " (km)" : "")", D))
		elseif k == "ldd"
			_hydro_setstage!(job, base, "Drain directions (LDD)")
			push!(res, ("$src LDD (PCRaster)", ldd(F)))
		elseif k == "order"
			_hydro_setstage!(job, base, "Strahler order")
			thr = _hydro_cells(_hydro_num(d, "stream_area", 1000.0), unit, G, F.geog, F.dx, F.dy)
			push!(res, ("$src Strahler order", streamorder(F; threshold = thr)))
		elseif k == "streams"
			_hydro_setstage!(job, base, "Stream network")
			thr = _hydro_cells(_hydro_num(d, "stream_area", 1000.0), unit, G, F.geog, F.dx, F.dy)
			S = streamnet(F; threshold = thr)
			isempty(S) || push!(res, ("$src streams", _hydro_simplify_lines!(S, 0.5 * min(abs(G.inc[1]), abs(G.inc[2])))))
		elseif k == "basins"
			_hydro_setstage!(job, base, "Drainage basins")
			amin = _hydro_cells(_hydro_num(d, "basin_area", 1000.0), unit, G, F.geog, F.dx, F.dy)
			B = drainagebasins(F; minarea = amin)
			_hydro_residual_class!(B, F)
			push!(res, ("$src drainage basins", B))
			# The basin LIMITS as vectors, plotted like the rivers are.
			_hydro_setstage!(job, base + 0.5 * step, "Basin outlines")
			O = _hydro_basin_outlines(B)
			isempty(O) || push!(res, ("$src basin outlines", O))
		end
	end
	_hydro_setstage!(job, 1.0, "Finished")
	return nothing
end

# Dialog Run. Returns "" when the run has started, otherwise the reason it could not.
function _hydro_run(scene::Ptr{Cvoid}, kv::String)::String
	try
		j = get(_HYDRO_JOBS, scene, nothing)
		(j !== nothing && !istaskdone(j.task)) && return "A drainage analysis is already running in this window."
		G = _displayed_raster(scene)
		G isa GMTgrid || return "Show a grid (a DEM) in this window first."
		ndims(G.z) == 2 || return "Drainage analysis needs a 2-D grid, not a cube."
		d = Dict{String,String}()
		for it in split(kv, ';')
			p = split(it, '=', limit = 2)
			length(p) == 2 && (d[String(strip(p[1]))] = String(strip(p[2])))
		end
		any(k -> _hydro_flag(d, k), ("acc", "twi", "dist", "ldd", "order", "streams", "basins")) ||
			return "Tick at least one result."
		k, nm = _active_element(scene)
		src = isempty(nm) ? "DEM" : String(nm)
		job = _HydroJob(Task(() -> nothing), Threads.Atomic{Float64}(0.0), Threads.Atomic{Bool}(false),
		                ReentrantLock(), "Starting", "",
		                Tuple{String,Union{GMTgrid,_HydroLines}}[], src, time())
		job.task = Threads.@spawn begin
			try
				_hydro_compute!(job, G, d)
			catch e
				job.err = e isa GMT.HydroCancelled ? "cancelled" : sprint(showerror, e)
			end
		end
		_HYDRO_JOBS[scene] = job
		return ""
	catch e
		_tool_failed(scene, "Drainage analysis", e)
		return sprint(showerror, e)
	end
end

# Dialog timer (every ~200 ms): "state|percent|text".
function _hydro_poll(scene::Ptr{Cvoid})::String
	job = get(_HYDRO_JOBS, scene, nothing)
	job === nothing && return "none|0|"
	el = round(Int, time() - job.t0)
	if !istaskdone(job.task)
		st = lock(() -> job.stage, job.lock)
		return "run|$(round(Int, 100 * job.frac[]))|$st   ($(el) s)"
	end
	if job.err == "cancelled"
		delete!(_HYDRO_JOBS, scene)
		return "cancel|0|Stopped after $(el) s."
	elseif !isempty(job.err)
		delete!(_HYDRO_JOBS, scene)
		_tool_failed(scene, "Drainage analysis", ErrorException(job.err))
		return "fail|0|$(job.err)"
	end
	return "adopt|100|Adding $(length(job.results)) results to the window…"
end

# Put the results in the window (main thread). Each grid goes through the file-drop door
# (_add_grid_to_scene) and the derived-variable transition (_adopt_derived!): a NEW named handle,
# checked, the rest unchecked, own axes. The basins are adopted last, so they are what the window
# shows. The stream network is vector data: an overlay group on top, never a reframe. A result of
# the same name from an earlier run is replaced, not piled up.
function _hydro_finish(scene::Ptr{Cvoid})::String
	job = get(_HYDRO_JOBS, scene, nothing)
	job === nothing && return "none|0|"
	delete!(_HYDRO_JOBS, scene)
	try
		grids = [(n, r) for (n, r) in job.results if r isa GMTgrid]
		lines = [(n, r) for (n, r) in job.results if r isa _HydroLines]
		for (name, G) in grids
			ccall(_fn(:gmtvtk_remove_grid_h), Cint, (Ptr{Cvoid}, Cstring), scene, name)
			_forget_object!(scene, :grid, name)
			# Basins and LDD hold CLASSES: one flat colour each, drawn as classes (no blend, no light).
			cls = occursin("drainage basins", name) ? _hydro_basin_color :
			      occursin("LDD", name) ? _hydro_class_color : nothing
			cmap = occursin("Strahler", name) ? :viridis : :turbo
			_add_grid_to_scene(scene, G, name; cmap = cmap, classes = cls) || error("the window was closed")
			_adopt_derived!(scene, name, G)
		end
		for (name, D) in lines
			ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, name)
			col = occursin("basin outlines", name) ? (0.0, 0.0, 0.0) : (0.0, 0.35, 1.0)
			ok = _add_dataset_to_scene(scene, D, name; groupName = name, color = col, forceMode = :lines)
			# The rivers are a NETWORK: kept here with their topology so a right-click on one can save
			# it with everything upstream of it (_hydro_save_upstream).
			if ok && endswith(name, "streams")
				get!(Dict{String,_HydroLines}, _HYDRO_RIVERS, scene)[name] = D
				ccall(_fn(:gmtvtk_overlay_set_river_net_h), Cint, (Ptr{Cvoid}, Cstring), scene, name)
			end
		end
		el = round(time() - job.t0, digits = 1)
		_viewer_log_info(scene, "Drainage analysis of $(job.src): $(length(job.results)) results in $el s.")
		return "done|100|Done in $el s: $(length(job.results)) results added."
	catch e
		_tool_failed(scene, "Drainage analysis", e)
		return "fail|0|$(sprint(showerror, e))"
	end
end

# The reach `i` of a river network and every reach upstream of it, `i` first. The network's topology
# is in its coordinates: a reach runs from its upstream end down to the junction where it meets the
# next reach, and that next reach STARTS at that same junction cell (streamnet), so "the reaches that
# flow into R" are those whose LAST vertex is R's FIRST. The simplification keeps both ends of every
# reach, so these are the junction cells' own coordinates, compared exactly.
function _hydro_upstream(S::_HydroLines, i::Int)::Vector{Int}
	into = Dict{NTuple{2,Float64},Vector{Int}}()       # junction -> the reaches ending there
	for (k, d) in enumerate(S)
		size(d.data, 1) < 2 && continue
		push!(get!(Vector{Int}, into, (d.data[end, 1], d.data[end, 2])), k)
	end
	out = [i]
	seen = Set(out)
	q = 1
	while q <= length(out)
		d = S[out[q]]
		for k in get(into, (d.data[1, 1], d.data[1, 2]), Int[])
			k in seen && continue
			push!(seen, k);  push!(out, k)
		end
		q += 1
	end
	return out
end

# The RIVER SYSTEM of every reach: the outlet reach (the one that flows into no other — the sea, a
# grid edge) its water finally reaches, numbered 1.. by system size (1 = most reaches). Same topology
# as _hydro_upstream: the reach below R is the one that STARTS where R ends.
function _hydro_systems(S::_HydroLines)::Vector{Int}
	n = length(S)
	starts = Dict{NTuple{2,Float64},Int}()
	for (k, d) in enumerate(S)
		size(d.data, 1) >= 2 && (starts[(d.data[1, 1], d.data[1, 2])] = k)
	end
	down(k) = size(S[k].data, 1) < 2 ? 0 : get(starts, (S[k].data[end, 1], S[k].data[end, 2]), 0)
	outlet = zeros(Int, n)
	path = Int[]
	for k in 1:n
		outlet[k] != 0 && continue
		empty!(path);  j = k
		while j != 0 && outlet[j] == 0 && !(j in path)
			push!(path, j)
			nxt = down(j)
			(nxt == 0 || nxt in path) && break
			j = nxt
		end
		o = outlet[j] != 0 ? outlet[j] : path[end]
		for p in path;  outlet[p] = o;  end
	end
	cnt = Dict{Int,Int}()
	for o in outlet;  cnt[o] = get(cnt, o, 0) + 1;  end
	order = sort(collect(keys(cnt)), by = o -> (-cnt[o], o))
	rank = Dict(o => r for (r, o) in enumerate(order))
	return [rank[o] for o in outlet]
end

# Right-click "Paint each river system in its own colour" (55_lineprops.cpp): `on` paints every system
# of the network `name` one palette colour (_hydro_class_color, by system number, so neighbouring
# systems differ), off puts the network back in its line colour. "" when done, else why not.
function _hydro_paint_systems(scene::Ptr{Cvoid}, name::AbstractString, on::Bool)::String
	try
		if !on
			ccall(_fn(:gmtvtk_overlay_seg_colors_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint),
			      scene, String(name), C_NULL, Cint(0))
			return ""
		end
		S = get(get(_HYDRO_RIVERS, scene, Dict{String,_HydroLines}()), String(name), nothing)
		S === nothing && return "This river network is no longer known. Run Drainage analysis again."
		sys = _hydro_systems(S)
		rgb = Vector{UInt8}(undef, 3 * length(S))
		for (k, v) in enumerate(sys)
			c = _hydro_class_color(v)
			for b in 1:3;  rgb[3(k - 1) + b] = round(UInt8, clamp(c[b], 0.0, 1.0) * 255);  end
		end
		ok = ccall(_fn(:gmtvtk_overlay_seg_colors_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint),
		           scene, String(name), rgb, Cint(length(S)))
		ok == 0 && return "The plotted rivers no longer match their network ($(length(S)) reaches)."
		return ""
	catch e
		_tool_failed(scene, "River systems", e)
		return "Could not paint the river systems: " * sprint(showerror, e)
	end
end

# Right-click "Save this river and its upstream network…" (riverSaveUpstream, 55_lineprops.cpp): the
# network `name` of this window, the clicked segment `seg` (0-based, in plotting order) and its two
# ends as the viewer has them, the file `path`. The reach is taken by its index when its ends agree,
# else found by its ends (a viewer that dropped or reordered a segment still points at the right
# one). Written by `_gmtwrite`, which picks the format from the extension: a GMT ASCII multisegment
# table (one '>' segment per reach, its Strahler order in the header) or GeoPackage / Shapefile / KML /
# GeoJSON, every reach keeping its x, y, z and order. Returns "ok|<message>" or the reason it failed.
function _hydro_save_upstream(scene::Ptr{Cvoid}, name::AbstractString, seg::Integer, x0::Real, y0::Real,
                              x1::Real, y1::Real, path::AbstractString)::String
	try
		S = get(get(_HYDRO_RIVERS, scene, Dict{String,_HydroLines}()), String(name), nothing)
		S === nothing && return "This river network is no longer known. Run Drainage analysis again."
		tol = 1e-6 * max(1.0, abs(x0), abs(y0))
		near(d) = size(d.data, 1) >= 2 &&
		          abs(d.data[1, 1] - x0) <= tol && abs(d.data[1, 2] - y0) <= tol &&
		          abs(d.data[end, 1] - x1) <= tol && abs(d.data[end, 2] - y1) <= tol
		i = (1 <= seg + 1 <= length(S) && near(S[seg + 1])) ? seg + 1 : something(findfirst(near, S), 0)
		i == 0 && return "Could not find the clicked river in its network."
		idx = _hydro_upstream(S, i)
		D = [S[k] for k in idx]
		# A vector format (GDAL/OGR) takes the geometry as x,y,z; each reach's Strahler order is its
		# attribute (streamnet sets `attrib["order"]`) and goes into the file as a field. Handed the 4th
		# column as well, GMT.jl's writer (helper_gmt2gd_xyz) passes GDAL a 0.0 for the z array and fails.
		ext = lowercase(splitext(String(path))[2])
		if !(ext in (".txt", ".dat", ".gmt", ".xyz", ""))
			D = [GMTdataset{Float64,2}(data = d.data[:, 1:3], attrib = copy(d.attrib), colnames = ["x", "y", "z"],
			                           header = d.header, proj4 = d.proj4, wkt = d.wkt, epsg = d.epsg, geom = d.geom)
			     for d in D]
			GMT.set_dsBB!(D)
		end
		_gmtwrite(String(path), D)
		return "ok|Saved the river and $(length(idx) - 1) upstream reaches ($(length(idx)) segments) to $(basename(String(path)))"
	catch e
		_tool_failed(scene, "Save river", e)
		return "Could not save the river: " * sprint(showerror, e)
	end
end

function _hydro_stop(scene::Ptr{Cvoid})::String
	job = get(_HYDRO_JOBS, scene, nothing)
	job === nothing || (job.cancel[] = true)
	return ""
end
