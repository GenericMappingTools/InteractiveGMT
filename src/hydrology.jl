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

# The whole computation, on the worker thread. Results are only collected here; nothing touches the
# viewer.
function _hydro_compute!(job::_HydroJob, G::GMTgrid, d::Dict{String,String})
	src = job.src
	sea = _hydro_flag(d, "sea") ? 0.0 : nothing
	F = flowdir(G; method = _hydro_flag(d, "carve") ? :carve : :fill, sealevel = sea,
	            progress = _hydro_reporter(job, 0.0, 0.6, "Flow directions"))
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
			isempty(S) || push!(res, ("$src streams", S))
		elseif k == "basins"
			_hydro_setstage!(job, base, "Drainage basins")
			amin = _hydro_cells(_hydro_num(d, "basin_area", 1000.0), unit, G, F.geog, F.dx, F.dy)
			push!(res, ("$src drainage basins", drainagebasins(F; minarea = amin)))
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

# Colours for a LABEL grid (basin numbers, LDD codes 1-9): one distinct categorical colour per integer
# 1..N. The slices are [k, k+1], so each label sits exactly on its own colour node (no blend with the
# next label's colour); categorical palettes cycle, so neighbouring basins get different colours.
function _hydro_label_cpt(G::GMTgrid)
	n = 1
	@inbounds for v in G.z
		(isnan(v) || v <= n) || (n = Int(v))
	end
	return makecpt(cmap = :categorical, range = (1, n + 1, 1))
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
			labels = occursin("drainage basins", name) || occursin("LDD", name)
			cmap = labels ? _hydro_label_cpt(G) : occursin("Strahler", name) ? :viridis : :turbo
			_add_grid_to_scene(scene, G, name; cmap = cmap) || error("the window was closed")
			_adopt_derived!(scene, name, G)
		end
		for (name, D) in lines
			ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, name)
			_add_dataset_to_scene(scene, D, name; groupName = name, color = (0.0, 0.35, 1.0), forceMode = :lines)
		end
		el = round(time() - job.t0, digits = 1)
		_viewer_log_info(scene, "Drainage analysis of $(job.src): $(length(job.results)) results in $el s.")
		return "done|100|Done in $el s: $(length(job.results)) results added."
	catch e
		_tool_failed(scene, "Drainage analysis", e)
		return "fail|0|$(sprint(showerror, e))"
	end
end

function _hydro_stop(scene::Ptr{Cvoid})::String
	job = get(_HYDRO_JOBS, scene, nothing)
	job === nothing || (job.cancel[] = true)
	return ""
end
