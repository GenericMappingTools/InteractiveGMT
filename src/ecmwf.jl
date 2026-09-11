# ecmwf.jl — Geophysics > Copernicus > "ERA5 / ECMWF download…": GMT.jl's `ecmwf` driven from the
# viewer.
#
# TWO SOURCES, ONE TOOL, because GMT.jl already made them one function:
#   GMT.ecmwf(; dataset=, params=, region=, …)   the Climate Data Store (ERA5 reanalysis), CDS API.
#   GMT.ecmwf(:forecast; var=, step=, R=, …)     the ECMWF open-data forecasts (no credentials).
# The request building is GMT.jl's, in full — `era5vars`, `era5time` and `ecmwf` itself. Nothing
# here composes a JSON body, a URL or a file name of its own (SACRED_LAW.md: one operation, one
# function). This file is the bridge: parse the dialog's key=value block, call, and hand whatever
# landed on disk to the SHARED file door (`_on_drop`) every dropped/opened file goes through, so a
# downloaded grid arrives with its own axes, its own colour bar and its own Scene Objects handle
# with no private display path.
#
# The variable catalogue the picker shows is GMT's own table (`GMT.helper_ecmwf_vars`), not a copy.

# The CDS datasets this dialog offers. Editable in the dialog: this is the starting list, not a gate.
const _ECMWF_DATASETS = ["reanalysis-era5-single-levels", "reanalysis-era5-pressure-levels",
                         "reanalysis-era5-land", "reanalysis-era5-single-levels-monthly-means",
                         "reanalysis-era5-pressure-levels-monthly-means",
                         "reanalysis-era5-land-monthly-means"]

# "t2m,skt" / "t2m skt" -> ["t2m", "skt"]. Empty in, empty out (the caller decides if that is fatal).
function _ecmwf_varlist(s::String)::Vector{String}
	v = String[]
	for t in split(s, (',', ' ', ';'); keepempty=false)
		tt = strip(t)
		isempty(tt) || push!(v, String(tt))
	end
	return v
end

# A dialog box that takes "one, a list, or a range" -> what GMT.jl's `getdtp` understands. A range
# stays a STRING with colons ("1000:-100:500", "0:3:12") because `getdtp` parses those itself; a
# list becomes a Vector{String}; a single value stays a String. Empty means "the API's default".
function _ecmwf_dtp(s::String)
	t = strip(s)
	isempty(t) && return ""
	occursin(':', t) && return _ecmwf_range(String(t))
	v = _ecmwf_varlist(String(t))
	return length(v) == 1 ? v[1] : v
end

# The Hour box -> what the CDS actually takes, which is "HH:00" and not a bare hour: a request with
# "time": ["12"] is rejected by the server (GMT.jl's own documented example writes "16:00"). The box
# still takes what its tooltip promises — one hour, a list, or a range — and an hour already written
# "12:00" is taken as it stands instead of being read as the range 12:0.
function _ecmwf_hours(s::String)
	t = strip(s)
	isempty(t) && return ""
	hh = String[]
	for part in split(t, (',', ' ', ';'); keepempty=false)
		p = strip(part)
		if occursin(r"^\d{1,2}:0{1,2}$", p)          # already "12:00" / "12:0"
			push!(hh, string(lpad(split(p, ':')[1], 2, '0'), ":00"))
		elseif occursin(':', p)                       # a range, "10:14" or "0:6:18"
			append!(hh, [string(lpad(x, 2, '0'), ":00") for x in _ecmwf_range(String(p))])
		else
			push!(hh, string(lpad(p, 2, '0'), ":00"))
		end
	end
	return length(hh) == 1 ? hh[1] : hh
end

# "1000:-100:500" / "0:3:12" -> the expanded Vector{String}. Written here rather than eval'ing the
# string: a range in a text box is data, never code.
function _ecmwf_range(t::String)::Vector{String}
	p = split(t, ':')
	(length(p) == 2 || length(p) == 3) || error("bad range '$t' — use start:stop or start:step:stop")
	nums = [parse(Float64, strip(x)) for x in p]
	rng = length(p) == 2 ? (nums[1]:nums[2]) : (nums[1]:nums[2]:nums[3])
	isempty(rng) && error("the range '$t' is empty")
	return [string(isinteger(x) ? Int(x) : x) for x in rng]
end

# The dialog's Date(s) box -> the three lists `era5time` takes. One date, a comma-separated list, or
# an inclusive range "d1:d2" (what the calendar button and the box's own syntax produce). A CDS
# request IS a year x month x day product — that is how the CDS site's own form works — so a span
# that crosses a month boundary asks for the other month's days too; the box says so in its tooltip.
# Empty in, empty out: `era5time` then falls back to its own default (five days ago).
function _ecmwf_dates(s::String)
	t = strip(s)
	isempty(t) && return ("", "", "")
	ds = GMT.Dates.Date[]
	for part in split(t, (',', ';'); keepempty=false)
		p = strip(part)
		if occursin(':', p)
			ab = split(p, ':')
			length(ab) == 2 || error("bad date range '$p' — use first:last, e.g. 2024-12-06:2024-12-10")
			d1, d2 = _ecmwf_date1(String(strip(ab[1]))), _ecmwf_date1(String(strip(ab[2])))
			d1 <= d2 || error("the date range '$p' ends before it starts")
			append!(ds, d1:GMT.Dates.Day(1):d2)
		else
			push!(ds, _ecmwf_date1(String(p)))
		end
	end
	one(v) = length(v) == 1 ? v[1] : v
	return (one(unique(string.(GMT.Dates.year.(ds)))),
	        one(unique([lpad(GMT.Dates.month(x), 2, '0') for x in ds])),
	        one(unique([lpad(GMT.Dates.day(x),   2, '0') for x in ds])))
end

# One date box entry -> a Date. "2024-12-06", "2024/12/06" and "20241206" all mean the same day.
function _ecmwf_date1(s::String)::GMT.Dates.Date
	t = replace(strip(s), '/' => '-')
	try
		return length(t) == 8 && all(isdigit, t) ? GMT.Dates.Date(t, "yyyymmdd") : GMT.Dates.Date(t)
	catch
		error("'$s' is not a date — use YYYY-MM-DD")
	end
end

# The variable catalogue, as the picker wants it: "id\tlong name\tunits" lines. GMT's own table
# (era5vars2d.jl / era5vars3d.jl / fcvars*.jl), read through GMT's own accessor — never a copy of
# the lists kept here.
function _ecmwf_varcatalog(source::String, pressure::Bool)::String
	d = GMT.helper_ecmwf_vars(!pressure, pressure, source == "fc" ? "fc" : "era5")[1]
	io = IOBuffer()
	for k in sort(collect(keys(d)))
		v = d[k]
		# era5: [long-name, name, units]; forecast: [name, units].
		name  = length(v) >= 3 ? v[2] : v[1]
		units = v[end]
		println(io, k, '\t', name, '\t', units)
	end
	return String(take!(io))
end

# Where the download will land, and what was already there. GMT.jl's forecast path names each
# variable's file ITSELF (and the reanalysis path names the file after the dataset when no name is
# given), so the files that appear are found by LOOKING, not by re-deriving those names here.
#
# NEVER `pwd()`. The working directory of an iGMT session is regularly the PACKAGE's own directory
# (the desktop launcher starts there), and nothing — ever, no exception — may be written inside the
# package. A download the user did not name a place for goes to a per-user temp folder.
_ecmwf_tmpdir()::String = (p = joinpath(tempdir(), "iGMT_ecmwf"); isdir(p) || mkpath(p); p)

function _ecmwf_destdir(out::String)::String
	isempty(out) && return _ecmwf_tmpdir()
	isdir(out) && return out
	d = dirname(out)
	return isempty(d) ? _ecmwf_tmpdir() : d
end

# The name a download gets when the user named none: the SOURCE and the VARIABLE, which is what the
# Scene Objects row then says — "ERA5_t2m_2024-12-06_12h.nc", not the dataset name every ERA5
# download would otherwise share. Deterministic on purpose: the same request twice resolves to the
# same path, which is what lets the second one skip the server (see `_ecmwf_download`).
function _ecmwf_autoname(d::Dict{String,String}, mode::String, fmt::String)::String
	ext  = (fmt == "grib") ? ".grib" : ".nc"
	vars = _ecmwf_varlist(_get(d, mode == "fc" ? "fcvars" : "vars"))
	safe(s) = replace(String(strip(s)), ':' => '-', ',' => '-', '/' => '-', '\\' => '-', ' ' => "")
	parts = String[mode == "fc" ? "ECMWF" : "ERA5"]
	push!(parts, isempty(vars) ? "request" : join(vars, '-'))
	lev = safe(_get(d, mode == "fc" ? "fclevels" : "levels"));   isempty(lev) || push!(parts, lev * "hPa")
	when = safe(_get(d, mode == "fc" ? "date" : "dates"));       isempty(when) || push!(parts, when)
	if mode != "fc"
		hr = safe(_get(d, "hour"));   isempty(hr) || push!(parts, hr * "h")
	else
		st = safe(_get(d, "steps"));  isempty(st) || push!(parts, "step" * st)
	end
	# The two DISPLAY choices are part of the name, because they are applied to the file itself: without
	# them, flipping Celsius to Kelvin (or the longitude frame) would silently reuse the file converted
	# the other way instead of fetching one that matches what was asked for.
	_get(d, "lonframe", "180") == "360" && push!(parts, "0-360")
	_get(d, "tunit", "C") == "C" && push!(parts, "degC")
	return join(parts, '_') * ext
end

# ERA5 serves a GLOBAL grid on 0-360. The dialog's Longitude box says which frame it should be shown
# in; -180/180 (the default) is what the rest of iGMT works in, and the shift is `grdedit -S`, GMT's
# own, never a hand-rolled roll of the matrix. A REGIONAL request is left in the frame its own region
# asked for, and anything that is not a plain grid (a cube, an image, a file GMT will not open as a
# grid) is left exactly as it is.
function _ecmwf_to_180!(f::String, want::String = "180")::String
	want == "360" && return f            # ERA5's own frame: nothing to do
	try
		G = GMT.gmtread(f, grd=true)
		(G isa GMT.GMTgrid) || return f
		(size(G.z, 3) > 1) && return f
		x0, x1 = G.range[1], G.range[2]
		(x1 > 180.0 && (x1 - x0) > 350.0) || return f      # global, and in the 0-360 frame
		tmp = joinpath(dirname(f), "_180_" * basename(f))
		try
			# -fg is NOT optional here. A netCDF whose header does not flag the axes geographic reads back
			# with geog = -1, and grdedit then cannot know that 0-359.75 is a longitude span at all: it
			# answers "Shift only allowed for global grids" and refuses a grid that is global. Told that
			# the axes are geographic, GMT shifts the ERA5 frame correctly, gridline registration and all.
			GMT.grdedit(f, S=true, R="-180/180/$(G.range[3])/$(G.range[4])", f="g", G=tmp)
		catch
			# Only if GMT still declines: move the columns west of 180 to the front and drop 360 from
			# their longitudes — an index permutation, not a resampling, so no value changes.
			GMT.gmtwrite(tmp, _ecmwf_roll180(G))
		end
		if isfile(tmp)
			rm(f; force=true)
			mv(tmp, f)
		end
	catch
		# A file that will not read as a grid, or a GMT that refused the shift: show what was downloaded.
	end
	return f
end

# ECMWF serves temperatures in KELVIN, which on a colour bar tells a reader nothing they think in.
# The dialog's Temperature box picks the unit: Celsius (the default) or the kelvin the server sent.
# The test is the grid's OWN declared unit, never the variable name — so `t2m`, `skt`, `sst` and a
# pressure-level `t` are all converted, and a wind component, a pressure or a humidity never is,
# whatever it is called.
const _ECMWF_KELVIN = ("k", "kelvin", "degk", "deg_k", "degreesk", "degrees_k")

# A unit read back from a netCDF header is a FIXED-WIDTH field: it comes as "K" followed by a run of
# NULs, which no amount of whitespace-stripping removes and which makes every == against it false.
_ecmwf_unit(s)::String = lowercase(strip(rstrip(String(s), '\0')))

function _ecmwf_to_unit!(f::String, want::String)::String
	(want == "K" || isempty(want)) && return f
	try
		G = GMT.gmtread(f, grd=true)
		(G isa GMT.GMTgrid) || return f
		(_ecmwf_unit(G.z_unit) in _ECMWF_KELVIN) || return f
		G.z  = Float32.(G.z .- 273.15f0)
		G.z_unit = "degrees_C"
		G.range[5], G.range[6] = extrema(G.z)
		GMT.gmtwrite(f, G)
	catch
		# Not readable as a grid, or no unit to trust: show exactly what the server sent.
	end
	return f
end

# 0-360 -> -180/180 by moving columns, for the grids GMT's own -S refuses. `_zmat` is THE accessor
# for a grid's samples (grid memory-layout law: never index `G.z` directly), so this works on a grid
# whatever layout it was read in, and hands back a plain column-major grid that says so.
function _ecmwf_roll180(G::GMT.GMTgrid)
	x = collect(Float64, G.x)
	k = findfirst(>=(180.0), x)       # 180 itself belongs to the WEST side, as -180
	k === nothing && return G
	Z = _zmat(G)                                   # z[iy, ix], row 1 = south
	xs = vcat(x[k:end] .- 360.0, x[1:k-1])
	return GMT.mat2grid(hcat(Z[:, k:end], Z[:, 1:k-1]); x=xs, y=collect(Float64, G.y),
	                    proj4=isempty(G.proj4) ? GMT.prj4WGS84 : G.proj4)
end

# Everything in `dir` that was not in `before`, plus `explicit` if it exists (an explicitly named
# file that was overwritten was already in `before`).
function _ecmwf_new_files(dir::String, before::Set{String}, explicit::String)::Vector{String}
	files = String[]
	for f in readdir(dir)
		p = joinpath(dir, f)
		(f in before || isdir(p)) && continue
		push!(files, p)
	end
	if !isempty(explicit) && isfile(explicit) && !(explicit in files)
		pushfirst!(files, explicit)
	end
	return files
end

# The ERA5 (CDS) half. Returns the file GMT.jl was told to write, or "" when the name was left to it.
function _ecmwf_era5(d::Dict{String,String}, region, fmt::String, out::String)::String
	dataset = _get(d, "dataset")
	isempty(dataset) && error("give me the CDS dataset name")
	# A file name is only forced when the user typed one: leaving it to GMT.jl is the documented
	# behaviour (dataset name + the extension the server announced) and must stay available.
	fname = (isempty(out) || isdir(out)) ? "" : out

	if _on(d, "clipboard")
		GMT.ecmwf(; dataset=dataset, cb=true, filename=fname, format=fmt, verbose=true)
		return fname
	end

	raw = _get(d, "params")
	if !isempty(raw)
		GMT.ecmwf(; dataset=dataset, params=raw, filename=fname, format=fmt, verbose=true)
		return fname
	end

	pressure = _on(d, "pressure")
	vars = _ecmwf_varlist(_get(d, "vars"))
	isempty(vars) && error("give me at least one variable (or paste a request / use the clipboard)")
	y, m, dy = _ecmwf_dates(_get(d, "dates"))
	pars = [GMT.era5vars(vars; single=!pressure, pressure=pressure),
	        GMT.era5time(year=y, month=m, day=dy, hour=_ecmwf_hours(_get(d, "hour")))]
	levlist = pressure ? _ecmwf_dtp(_get(d, "levels")) : ""
	GMT.ecmwf(; dataset=dataset, params=pars, levlist=levlist, region=region, format=fmt,
	          filename=fname, dryrun=_get(d, "what") == "dryrun", verbose=true)
	return fname
end

# The open-data forecast half. `prefix` is GMT.jl's own naming knob (which_name): the per-variable
# file names stay GMT.jl's, we only say where they go.
function _ecmwf_forecast(d::Dict{String,String}, region, fmt::String, out::String)::String
	vars = _ecmwf_varlist(_get(d, "fcvars"))
	isempty(vars) && error("give me at least one forecast variable (e.g. 10u, 10v, 2t)")
	kw = Dict{Symbol,Any}(:var => vars, :cube => _on(d, "cube") ? 1 : 0, :format => fmt)
	(region == "") || (kw[:R] = region)
	fname = ""
	if !isempty(out) && !isdir(out)
		if length(vars) == 1  fname = out                       # one variable, one file: that name
		else                  kw[:prefix] = splitext(out)[1]    # many: GMT.jl names each after the var
		end
	elseif !isempty(out)
		kw[:prefix] = joinpath(out, "ecmwf")
	end
	s = _get(d, "steps");   isempty(strip(s)) || (kw[:step] = _ecmwf_dtp(s))
	l = _get(d, "fclevels")
	if !isempty(strip(l))
		kw[:levlist] = (strip(l) == "all") ? "all" : _ecmwf_dtp(l)
	end
	dt = _get(d, "date")
	if !isempty(strip(dt))
		kw[:date] = all(isdigit, strip(dt)) && length(strip(dt)) <= 2 ? parse(Int, strip(dt)) : String(strip(dt))
	end
	t = _get(d, "time");    isempty(strip(t)) || (kw[:time] = parse(Int, strip(t)))
	m = _get(d, "model");   isempty(m) || (kw[:model] = m)
	st = _get(d, "stream"); isempty(st) || (kw[:stream] = st)
	ty = _get(d, "type");   isempty(ty) || (kw[:type] = ty)
	_get(d, "what") == "dryrun" && (kw[:dryrun] = 1)
	levs = get(kw, :levlist, "")
	haskey(kw, :levlist) && delete!(kw, :levlist)
	GMT.ecmwf(:forecast; filename=fname, levlist=levs, kw...)
	return fname
end

# Run `f` with stdout on a pipe and give back what it printed. ONLY the reanalysis DRY RUN uses it:
# that path posts nothing and downloads nothing, it just prints the request it would send, and the
# dialog is where the user asked to see it. A pipe (drained by a task) and not an IOBuffer, which
# `redirect_stdout` does not take, and never around anything that fetches.
function _ecmwf_capture_stdout(f::Function)::String
	p = Pipe()
	Base.link_pipe!(p; reader_supports_async=true)
	t = @async read(p, String)
	try
		redirect_stdout(f, p)
	finally
		close(p.in)
	end
	return fetch(t)
end

# C callback. `cparams` is the newline-separated "key=value" block documented in 30_app.cpp's
# JuliaEcmwfFn. `out`/`cap` carry text back: the variable catalogue for the picker, the dry-run
# request, or the list of files that were downloaded. Returns 1 on success, 0 on failure.
#
# The body is SPLIT, and the split is the point. Julia compiles a whole method the first time it
# runs, so while the catalogue and the download lived in one function, opening the variable picker
# paid for inferring the download path too — `GMT.ecmwf`, `_on_drop` and everything under them:
# 8.3 s measured, for a lookup that is itself 15 ms. The picker branch is now its own small
# function, and the heavy one is reached through `invokelatest`, which is a dynamic call: inference
# stops there, so pressing "…" compiles the picker and NOTHING of the download.
function _on_ecmwf(scene::Ptr{Cvoid}, cparams::Cstring, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		what = _get(d, "what", "download")
		if what == "listvars"
			_ecmwf_reply(out, cap, _ecmwf_varcatalog(_get(d, "source", "era5"), _on(d, "pressure")))
			return Cint(1)
		end
		# Where an unnamed download will land — the dialog shows it greyed in the "Save as" box, so the
		# answer comes from the ONE function that decides it, never from a second guess on the C++ side.
		if what == "destdir"
			_ecmwf_reply(out, cap, _ecmwf_tmpdir())
			return Cint(1)
		end
		# The two halves of a running download, both light: a poll gives the task its slice of time and
		# reports bytes, "finish" loads what landed. Neither may drag the heavy path into inference.
		if what == "poll"
			_ecmwf_reply(out, cap, _ecmwf_job_poll())
			return Cint(1)
		end
		what == "finish" && return Base.invokelatest(_ecmwf_job_finish, scene, out, cap)::Cint
		return Base.invokelatest(_ecmwf_run, scene, d, out, cap)::Cint
	catch e
		_tool_failed(scene, "Copernicus / ECMWF", e)
		_ecmwf_reply(out, cap, sprint(showerror, e))
		return Cint(0)
	end
end

# The dry run and the download: everything that reaches GMT.jl's `ecmwf` and the file door. Reached
# only through `invokelatest` (see above), so none of it is compiled until a button is pressed.
function _ecmwf_run(scene::Ptr{Cvoid}, d::Dict{String,String}, out::Ptr{UInt8}, cap::Cint)::Cint
	txt = ""
	try
		what   = _get(d, "what", "download")
		mode   = _get(d, "mode", "era5")
		fmt    = _get(d, "format", "netcdf")
		outbox = String(strip(_get(d, "out")))
		r      = String(strip(_get(d, "region")))
		region = isempty(r) ? "" : r
		dir    = _ecmwf_destdir(outbox)
		isdir(dir) || error("the output directory does not exist: $dir")

		if what == "dryrun"
			# The reanalysis dry run only PRINTS the request it would post — no network at all, so its
			# stdout is captured and shown in the dialog. The forecast dry run resolves the most recent
			# run on the server first, so its printing is left alone (never a redirect around a fetch)
			# and the URLs land in the window's console.
			if mode == "era5"
				txt = _ecmwf_capture_stdout() do
					_ecmwf_era5(d, region, fmt, outbox)
				end
				isempty(strip(txt)) && (txt = "Nothing to show — a clipboard or pasted request is posted as it is.")
			else
				_ecmwf_forecast(d, region, fmt, outbox)
				txt = "The forecast URLs were printed to the Julia console."
			end
			_ecmwf_reply(out, cap, txt)
			return Cint(1)
		end
		# Same split again, one level down: a DRY RUN must not pay for inferring the download — the file
		# scan and `_on_drop`, which is the whole file door (grid readers, the cube slider, the picker).
		return Base.invokelatest(_ecmwf_download, scene, d, mode, region, fmt, outbox, dir, out, cap)::Cint
	catch e
		_tool_failed(scene, "Copernicus / ECMWF", e)
		_ecmwf_reply(out, cap, sprint(showerror, e))
		return Cint(0)
	end
end

# ------------------------------------------------------------------------------------------------
# The download runs as a TASK, and the dialog is driven by polling it. Why: GMT.jl fetches with
# `run(curl … -o file)`, which blocks the calling TASK but not the thread — Julia's scheduler keeps
# running everything else. So the C++ side starts the job, then calls back here every ~100 ms
# ("what=poll"); each poll yields, which is when the download actually advances, and returns how many
# bytes have landed. Between polls C++ pumps Qt, so the progress bar moves and the window stays
# alive, instead of a frozen modal for the minutes a CDS request takes.
#
# The task NEVER touches the scene: loading the result is a separate step ("what=finish") that C++
# asks for once the poll says DONE, so no VTK call is ever re-entered from inside a poll.
const _ECMWF_JOB    = Ref{Union{Nothing,Task}}(nothing)
const _ECMWF_TARGET = Ref{String}("")      # where the bytes are landing, for the byte count
const _ECMWF_DIR    = Ref{String}("")
const _ECMWF_BEFORE = Ref{Set{String}}(Set{String}())
const _ECMWF_ARGS   = Ref{Dict{String,String}}(Dict{String,String}())

function _ecmwf_job_start(d::Dict{String,String}, mode::String, region, fmt::String, target::String,
                          dir::String)::Nothing
	_ECMWF_TARGET[] = target
	_ECMWF_DIR[]    = dir
	_ECMWF_BEFORE[] = Set{String}(readdir(dir))
	_ECMWF_ARGS[]   = d
	_ECMWF_JOB[] = @async cd(() -> begin
		mode == "era5" ? _ecmwf_era5(d, region, fmt, target) : _ecmwf_forecast(d, region, fmt, target)
	end, dir)
	return nothing
end

# One poll: give the download its slice of time, then say where it stands. "BUSY <bytes>" while it
# runs, "DONE" / "FAIL <why>" when it is over.
function _ecmwf_job_poll()::String
	t = _ECMWF_JOB[]
	t === nothing && return "FAIL no download is running"
	istaskdone(t) || (sleep(0.1); istaskdone(t) || return string("BUSY ", _ecmwf_job_bytes()))
	if istaskfailed(t)
		_ECMWF_JOB[] = nothing
		return "FAIL " * sprint(showerror, t.exception isa TaskFailedException ? t.exception.task.exception : t.exception)
	end
	return "DONE"
end

_ecmwf_job_bytes()::Int = (f = _ECMWF_TARGET[]; (isempty(f) || !isfile(f)) ? 0 : Int(filesize(f)))

# After DONE: see what landed, put it in the -180/180 frame if it is a global grid, and hand each file
# to the shared file door. Runs OUTSIDE any poll, so the scene is only ever touched from here.
function _ecmwf_job_finish(scene::Ptr{Cvoid}, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		t = _ECMWF_JOB[];   _ECMWF_JOB[] = nothing
		d, dir = _ECMWF_ARGS[], _ECMWF_DIR[]
		explicit = (t === nothing || istaskfailed(t)) ? "" : String(fetch(t))
		exp_abs = isempty(explicit) ? "" : (isabspath(explicit) ? explicit : joinpath(dir, explicit))
		files = _ecmwf_new_files(dir, _ECMWF_BEFORE[], exp_abs)
		isempty(files) && error("the request returned no file — see the Julia console for the server's answer")
		# The two display choices, applied to what landed: the longitude frame (global grids only) and
		# the temperature unit (fields that really are in kelvin only).
		if isempty(String(strip(_get(d, "region"))))
			files = String[_ecmwf_to_180!(f, _get(d, "lonframe", "180")) for f in files]
		end
		files = String[_ecmwf_to_unit!(f, _get(d, "tunit", "C")) for f in files]
		if _on(d, "load")
			for f in files
				_on_drop(scene, f)
			end
		end
		_ecmwf_reply(out, cap, "Downloaded:\n" * join(files, '\n'))
		return Cint(1)
	catch e
		_tool_failed(scene, "Copernicus / ECMWF", e)
		_ecmwf_reply(out, cap, sprint(showerror, e))
		return Cint(0)
	end
end

# The download proper: fetch, see what landed, and hand each file to the shared file door.
function _ecmwf_download(scene::Ptr{Cvoid}, d::Dict{String,String}, mode::String, region, fmt::String,
                         outbox::String, dir::String, out::Ptr{UInt8}, cap::Cint)::Cint
	try
		# An unnamed download gets a name of its own (source + variable), in the temp folder. That name
		# is a FUNCTION OF THE REQUEST, so asking twice for the same thing finds the first answer still
		# on disk and does not go back to the server for it.
		target = outbox
		if isempty(target) || isdir(target)
			target = joinpath(isempty(target) ? dir : target, _ecmwf_autoname(d, mode, fmt))
		end
		if isfile(target)
			# Asked for before, and still on disk: the server is not asked again. THE shared file door
			# (`_on_drop`) shows it, the same one a drag-and-drop goes through (SACRED_LAW.md).
			_ecmwf_reply(out, cap, "Already downloaded, reused:\n" * target)
			_on(d, "load") && _on_drop(scene, target)
			return Cint(1)
		end
		_ecmwf_job_start(d, mode, region, fmt, target, dir)
		_ecmwf_reply(out, cap, "STARTED")      # C++ now polls, with a progress bar (see _ecmwf_job_poll)
		return Cint(1)
	catch e
		_tool_failed(scene, "Copernicus / ECMWF", e)
		_ecmwf_reply(out, cap, sprint(showerror, e))
		return Cint(0)
	end
end

# Copy `txt` into the C buffer, NUL-terminated and never past `cap`.
function _ecmwf_reply(out::Ptr{UInt8}, cap::Cint, txt::String)::Nothing
	(out == C_NULL || cap <= 0) && return nothing
	b = codeunits(txt)
	n = min(length(b), Int(cap) - 1)
	n > 0 && GC.@preserve b unsafe_copyto!(out, pointer(b), n)
	unsafe_store!(out, UInt8(0), n + 1)
	return nothing
end

function _register_ecmwf()
	fptr = @cfunction((s, c, o, n) -> Base.invokelatest(_on_ecmwf, s, c, o, n)::Cint,
	                  Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_ecmwf_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
