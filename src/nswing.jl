# nswing.jl — Geophysics > NSWING tsunami (port of Mirone's swan_options.m, src_figs/swan_options.m).
# The C++ dialog (NswingDialog, 70_window.cpp) hands a newline-separated "key=value" block to
# `_on_nswing`; we turn it into an `nswing` command line and launch the executable.
#
# This is the FIRST iteration: the parameter→option mapping mirrors nswing's usage (mex/nswing.c),
# but the grid roles (Source as the first positional, Nest as a nesting grid) and the exact maregraph
# handling will be refined later. The run is launched DETACHED (run(cmd; wait=false)) so the in-process
# Qt pump (eventloop.jl) is not frozen by a long tsunami simulation; its stdout goes to its own console.
#
# As with every C→Julia callback the @cfunction + registration are RUNTIME values, created lazily at
# the first window open (eventloop.jl `_ensure_callbacks`) via an invokelatest trampoline — never at
# top level (a precompiled @cfunction is invalid).

# Parse the "key=value\n…" block into a Dict{String,String}.
function _nswing_parse(s::AbstractString)
	d = Dict{String,String}()
	for ln in split(s, '\n')
		isempty(ln) && continue
		i = findfirst('=', ln)
		i === nothing && continue
		d[String(ln[1:i-1])] = String(strip(ln[i+1:end]))
	end
	return d
end

_get(d, k, default="") = get(d, k, default)
_on(d, k) = get(d, k, "0") == "1"

# Default output-name stem for the RUN dialog's Name field: "tsu" living beside the bathymetry
# grid's OWN source file (if the window was opened from one), else just "tsu" (cwd). Called
# synchronously from the C++ dialog constructor over the console-eval bridge (same idiom as
# measure.jl's `_fault_lenaz` — prints, returns nothing, so no `show`-quoting reaches the buffer).
function _nswing_default_name(scene::Ptr{Cvoid})
	path = _path_for_handle(scene)
	print(isempty(path) ? "tsu" : joinpath(dirname(path), "tsu"))
	return nothing
end

# Assemble the nswing OPTION vector (everything EXCEPT the Source/Nest grid inputs, which are resolved
# to GMTgrid objects and injected by _on_nswing). Returns (args, msgs) where msgs are non-fatal notes to
# echo to the viewer console. Throws on a missing required field.
#
# (2026-07-10) Re-verified every flag against the LIVE `gmt nswing` usage (the linked GMT module, not
# the old mex/nswing.c comments the iter1 port went by) by actually RUNNING it (GMT.gmt("nswing …", bathy,
# source) on a real earth_relief patch) — several flags in the iter1 port don't exist / mean the opposite
# in the real module, so runs errored out at option-parsing (instant "finish", nothing written, matching
# the user's report). Fixed:
#  - `-f` (bare) is NOT a geographic flag here — it's GMT's generic column-type option and REQUIRES an
#    argument (`-fg`); a bare `-f` is a parse error (GMT error 72, confirmed live) and the run never even
#    starts. The module already auto-detects geographic from the grid's own metadata ("Assuming -fg"
#    info message) — `-fg` only reinforces that, never required, but must be `-fg` if sent at all.
#  - `-G<name>,<int>` alone is the single-3D-netCDF form (this WAS the "3D file" checkbox's meaning);
#    the per-step separate-grids form is `-G<name>+m,<int>` — the OLD code had it backwards (bare -G for
#    "series" when unchecked) and used a nonexistent `-Z` option for the checked case.
#  - `-T<cumint>,<in>[,<out>]` (comma-positional) is wrong syntax; real form is
#    `-T<in>[+o<out>][+t<cumint>]` (verified live, produced the maregraph output file).
#  - `-n<name>` (MOST format) does not exist in the linked module at all — errors immediately if picked.
function _nswing_opts(d::Dict{String,String}; max_nest_level::Int=0)
	args = String[]
	msgs = String[]

	name = _get(d, "name")
	grn  = _get(d, "grn", "10")
	mode = _get(d, "outmode", "grids")
	if (mode == "grids")
		isempty(name) && push!(msgs, "no output Name stem given")
		stem = isempty(name) ? "tsu_time_" : name
		push!(args, "-G$(stem),$(grn)")                  # nswing -G<name>,<int>
		_get(d, "field") == "total" && push!(args, "-D") # total water depth
		_on(d, "max")      && push!(args, "-M")          # max water level grid (Max water checkbox)
		# -M-/-M+ only when at least level 2 nested grids exist (not for 0 or 1 level)
		max_nest_level >= 2 && append!(args, ("-M-", "-M+"))  # min / max-positive water level grids
		_on(d, "velocity") && push!(args, "-S")          # velocity grids (_U/_V)
		_on(d, "momentum") && push!(args, "-H")          # momentum grids
	elseif (mode == "anuga")
		isempty(name) && error("NSWING: ANUGA output needs a file Name")
		push!(args, "-A$(name)")
	elseif (mode == "most")
		error("NSWING: MOST (.nc) output is not supported by the installed nswing module (no -n option) — pick Output grids or ANUGA")
	end

	manning = _get(d, "manning")
	isempty(manning) || push!(args, "-X$(manning)")      # Manning friction coefficient(s)
	_on(d, "coriolis") && push!(args, "-C")              # Coriolis effect (Coriolis checkbox)

	if _on(d, "maregs")
		mi = _get(d, "maregin"); mo = _get(d, "maregout"); ci = _get(d, "cumint", "1")
		if isempty(mi)
			push!(msgs, "Maregraphs checked but no In file given — skipping -T")
		else
			t = "-T$(mi)"
			isempty(mo) || (t *= "+o$(mo)")
			isempty(ci) || (t *= "+t$(ci)")
			push!(args, t)
		end
	end

	ncycles = _get(d, "ncycles", "1010"); push!(args, "-N$(ncycles)")
	jump = tryparse(Float64, _get(d, "jump", "0"))
	(jump !== nothing && jump > 0) && push!(args, "-P$(_get(d, "jump"))")

	dt = _get(d, "dt")
	isempty(dt) && error("NSWING: Time step (sec) is required (nswing -t)")
	push!(args, "-t$(dt)")

	_on(d, "geog") && push!(args, "-fg")                 # geographical coordinates (module auto-detects
	                                                      # from grid metadata anyway; this only reinforces it)

	bc = _get(d, "bc")
	isempty(bc) || push!(args, "-O$(bc)")                # boundary condition file (experimental)

	return args, msgs
end

# Resolve a Source/Nest field to what nswing should receive. The dialog seeds it with a Scene Objects
# grid NAME (e.g. "Okada z"); look it up in the window's grid registry and hand back the live GMTgrid —
# nswing takes grid objects directly, so no on-disk conversion is needed. Falls back to the raw string
# (a typed file path) when the name is not a registered scene grid. Uses the STRICT lookup
# (_find_object_exact) on purpose — the plain _find_object silently falls back to the window's
# PRIMARY (bathymetry) grid on a name miss, which would silently substitute the bathymetry for a
# Source/Nest that didn't resolve, instead of correctly falling through to "treat it as a file path".
function _nswing_grid_ref(scene::Ptr{Cvoid}, name::AbstractString)
	isempty(name) && return ""
	G = _find_object_exact(scene, :grid, String(name))
	G isa GMTgrid ? G : String(name)
end

# Every in-scene "layerN" grid for this window, as (N, GMTgrid) sorted by N — the nesting chain
# the user built (layer1 → -1, layer2 → -2, …). Read straight from the object registry.
function _nswing_scene_nests(scene::Ptr{Cvoid})
	out = Tuple{Int,Any}[]   # value is GMTgrid here, but shares this Vector with dialog-typed paths (String) later
	v = get(_SCENE_OBJS, scene, nothing)
	v === nothing && return out
	for (k, n, dat) in v
		(k === :grid && dat isa GMTgrid) || continue
		m = match(r"^layer(\d+)$", n)
		m === nothing || push!(out, (parse(Int, m.captures[1]), dat))
	end
	sort!(out; by = first)
	return out
end

# ── async run + live progress ─────────────────────────────────────────────────────────────────
# NSWING is ONE long GMT call (a single `ccall`). Running it in this process would freeze the map window
# even on a worker THREAD: a thread inside a long ccall never reaches a GC safepoint, so the main thread
# stalls at its next allocation (the Qt pump allocates every tick) until the whole run ends. The only way
# to keep the window live is a separate PROCESS: ALWAYS a detached `gmt nswing … -v`. Grids that live
# in the scene are written to temp .grd files first (_on_nswing); they used to be serialized to a
# Distributed worker instead, which put tens of MB on the wire BEFORE nswing started and left the
# dialog frozen for that whole time. ONE run path now, and progress starts within a second or two.
# The run happens off-process, so the main iGMT window never blocks (on any thread count). The
# run writes its stdout/stderr to a small text LOG (not a grid file) that a main-thread Timer tails for the
# -v "… NN%" advance, driving a NON-MODAL progress bar. Only one run at a time.
const _NSWING_RUNNING = Ref(false)

_progress_show_async(max::Int, title::String) =
	ccall(_fn(:gmtvtk_progress_show_async), Cint, (Cint, Cstring), max, title) != 0

# value < 0 -> leave the bar value, set only the label (status text). label "" -> leave the text.
_progress_status(value::Int, label::AbstractString) =
	ccall(_fn(:gmtvtk_progress_status), Cvoid, (Cint, Cstring), value, label)

# Seconds -> "M:SS" (or "H:MM:SS"), for the ETA readout.
function _hms(sec)
	t = max(0, round(Int, sec))
	h = t ÷ 3600;  m = (t % 3600) ÷ 60;  s = t % 60
	p2(n) = lpad(string(n), 2, '0')
	h > 0 ? "$(h):$(p2(m)):$(p2(s))" : "$(m):$(p2(s))"
end

# Read text appended to `logf` since byte offset `pos[]`; return (pct, line, errline): the LAST "NN%"
# seen (or nothing), the LAST non-blank text line (or nothing), and the LAST line containing nswing's
# own "[ERROR]" tag (or nothing). nswing writes progress with \r, so we split on both \r and \n to
# recover the latest status.
#
# (2026-07-10) nswing can hit a FATAL internal condition (e.g. "dt is greater than dtCFL … Stopping
# here") that it reports via a plain GMT_Report "[ERROR]" line WITHOUT returning a nonzero module
# status — GMT_Call_Module (and so GMT.gmt(), and so our worker's `fetch(fut)`) sees this as a normal
# return. Confirmed live: passing a too-large `-t` against a real fine-resolution bathymetry made
# nswing abort right after allocating the output file, leaving a "successful" run whose `z` variable is
# the untouched all-zero default for every saved time step — exactly "output is only zeros", with
# nothing in the caller-visible result to say why. `_nswing_watch` now treats ANY "[ERROR]" line as a
# real failure regardless of what the process/module status says.
function _nswing_tail(logf::String, pos::Base.RefValue{Int})
	isfile(logf) || return (nothing, nothing, nothing)
	txt = ""
	try
		open(logf, "r") do f
			seek(f, pos[]); txt = read(f, String); pos[] = position(f)
		end
	catch
		return (nothing, nothing, nothing)
	end
	isempty(txt) && return (nothing, nothing, nothing)
	pct = nothing
	for m in eachmatch(r"(\d{1,3})\s*%", txt)
		pct = clamp(parse(Int, m.captures[1]), 0, 100)
	end
	line = nothing
	errline = nothing
	for ln in split(txt, r"[\r\n]+"; keepempty = false)
		s = strip(ln)
		isempty(s) && continue
		line = String(s)
		occursin("[ERROR]", s) && (errline = String(s))
	end
	return (pct, line, errline)
end

# ── progress channel: nswing -W<file> ─────────────────────────────────────────────────────────
# The run is OFF-PROCESS by design (see the banner above), so nswing's in-process progress API (a
# registered callback, or its pollable statics) cannot reach us — neither crosses a process boundary.
# nswing -W takes "<port>|<file>"; the FILE form is used — nswing rewrites one
# short line in place each tick ("percent cycle n_cycles model_time"), which the watcher reads fresh
# every 0.2 s. The socket form is NOT used: it binds 20 random ephemeral ports and, if every bind
# fails, silently passes no -W at all, leaving a dialog that never moves for the whole run.
_nswing_progress_file() = tempname() * ".nswingprog"

# (pct, (cycle, n_cycles, model_time)) from that file, or (nothing, nothing) when it has nothing usable
# yet. Tolerant by construction: the file is being rewritten under us, so a torn/short read is normal
# and simply yields nothing this tick rather than an error.
function _nswing_progress_read(path::String)
	isfile(path) || return (nothing, nothing)
	txt = try read(path, String) catch; "" end
	f = split(replace(strip(txt), ',' => ' '))
	length(f) < 1 && return (nothing, nothing)
	p = tryparse(Float64, f[1])
	p === nothing && return (nothing, nothing)
	pct = clamp(round(Int, p), 0, 100)
	length(f) < 4 && return (pct, nothing)
	c = tryparse(Float64, f[2]); n = tryparse(Float64, f[3]); mt = tryparse(Float64, f[4])
	(c === nothing || n === nothing || mt === nothing) && return (pct, nothing)
	return (pct, (c, n, mt))
end
# Main-thread Timer: tail `logf` for nswing's -v "NN %" output → drive the bar + a live ETA label.
# ETA anchors on the FIRST percent seen (time, pct) and extrapolates: elapsed·(100−p)/(p−p0). The raw
# last line is shown whenever no percent is available yet (setup phase), so the dialog is never blank.
# On `isdone()` close the bar and log the outcome (`result()` returns the error string, "" on success).
function _nswing_watch(scene::Ptr{Cvoid}, isdone, result, logf::String, io=nothing; progf::String="")
	t0       = time()                                 # run start: base for both the live ETA and the final total
	pos      = Ref(0)
	anchor   = Ref{Union{Nothing,Tuple{Float64,Int}}}(nothing)  # (time, pct) of the first percent seen
	fatalerr = Ref{Union{String,Nothing}}(nothing)    # nswing's own "[ERROR]" line, if any (see _nswing_tail)
	cycinfo  = Ref{Union{Nothing,NTuple{3,Float64}}}(nothing)   # (cycle, n_cycles, model time) from the -W file
	last     = Ref{Union{Nothing,Tuple{Float64,Int}}}(nothing)  # (time, pct) of the last CHANGE of percentage
	rate     = Ref{Union{Nothing,Float64}}(nothing)             # smoothed percent-per-second (EMA)
	shown    = Ref((-1, 0.0))                                   # (pct, time) last written to the dialog
	Timer(0.2; interval = 0.2) do tm
		try
			pct, line, errline = _nswing_tail(logf, pos)
			(errline === nothing) || (fatalerr[] = errline)
			if !isempty(progf)                          # …and the -W FILE beats both (see _nswing_progress_file)
				fp, fc = _nswing_progress_read(progf)
				fp === nothing || (pct = fp)
				fc === nothing || (cycinfo[] = fc)
			end
			if pct !== nothing
				anchor[] === nothing && (anchor[] = (time(), pct))
				(at, ap) = anchor[]
				now = time()
				# A STEADY ETA. Extrapolating from the anchor on every 0.2 s tick makes the number jump
				# around wildly at the start — the first few percent are quantized to whole numbers, so
				# the implied rate swings by a factor of two between ticks and the readout is unusable.
				# Instead the rate (percent per second) is smoothed with an exponential moving average
				# and only re-derived when the PERCENTAGE ACTUALLY CHANGES, which is the only moment new
				# information arrives; and the text is refreshed at most once a second so it does not
				# flicker. Nothing is shown at all until 3 percent have gone by, because before that
				# there is not enough signal for an honest estimate.
				if last[] === nothing
					last[] = (now, pct)
				elseif pct > last[][2]
					dt_, dp = now - last[][1], pct - last[][2]
					if dt_ > 0
						r = dp / dt_
						rate[] = rate[] === nothing ? r : 0.25r + 0.75rate[]   # EMA, slow to follow spikes
					end
					last[] = (now, pct)
				end
				eta = (rate[] !== nothing && rate[] > 0 && pct - ap >= 3) ? (100 - pct) / rate[] : NaN
				lbl = "NSWING   $(pct)%"
				if (cy = cycinfo[]) !== nothing
					lbl *= "   cycle $(round(Int, cy[1]))/$(round(Int, cy[2]))   t = $(_hms(cy[3]))"
				end
				lbl *= isnan(eta) ? "" : "   ~$(_hms(eta)) left"
				if pct != shown[][1] || now - shown[][2] >= 1.0        # ≤1 Hz text refresh: no flicker
					_progress_status(pct, lbl)
					shown[] = (pct, now)
				end
			elseif (line !== nothing)
				_progress_status(-1, "NSWING: $line")     # setup phase: show raw output, don't move bar
			else
				# NOTHING to report yet — the grids are still being written out, or nswing has not printed
				# its first tick. A frozen label at 0% is indistinguishable from a hang and was read as
				# exactly that, so the elapsed clock ticks instead: the dialog is visibly alive throughout.
				_progress_status(-1, "NSWING: preparing run…   $(_hms(time() - t0)) elapsed")
			end
			isdone() || return
			close(tm)
			io === nothing || (try close(io) catch end)
			isempty(progf) || (try rm(progf; force = true) catch end)   # the -W progress file
			err = ""
			try err = result() catch e; err = sprint(showerror, e) end
			# nswing can abort on its own fatal check (e.g. dt > dtCFL) and still return a clean status —
			# an "[ERROR]" line it printed itself always overrides an otherwise-empty `err`.
			(isempty(err) && fatalerr[] !== nothing) && (err = fatalerr[])
			try rm(logf; force = true) catch end
			_progress_close()
			_NSWING_RUNNING[] = false
			total = _hms(time() - t0)                 # FINAL time estimate: total wall clock for the whole run
			isempty(err) ? _viewer_log_info(scene, "NSWING: run finished in $total.") :
			               _viewer_log_error(scene, "NSWING FAILED after $total: $err")
		catch e
			close(tm); _progress_close(); _NSWING_RUNNING[] = false
			_viewer_log_error(scene, "NSWING watcher error: $(sprint(showerror, e))")
		end
	end
	return
end
# libgmtvtk.jl's _load_library() permanently prepends the VTK/Qt toolchain bin dirs (_VTK_BIN,
# _QT_BIN) onto THIS Julia process's PATH, so gmtvtk.dll's own ccall'd VTK/Qt DLLs resolve. Any
# child process we spawn afterward inherits that same PATH -- including the detached `gmt nswing`
# process below. gmt.exe has its OWN dependencies (netcdf/hdf5/proj/gdal/...) that can share a
# FILENAME with VTK's bundled copies at a different version/ABI; if gmt.exe finds VTK's copy first
# it can crash on load (access violation, exit code 0xC0000005) even though the exact same command
# works fine from a normal terminal with a clean PATH. Give the child process PATH with that prefix
# stripped back off, so it resolves its own dependencies exactly as it would standalone.
function _nswing_clean_env()
	path = get(ENV, "PATH", "")
	prefix = _VTK_BIN * ";" * _QT_BIN * ";"
	if startswith(path, prefix)
		path = path[length(prefix)+1:end]
	end
	env = copy(ENV)
	env["PATH"] = path
	return env
end

# File-path run: launch a detached `gmt nswing …` OS process, output → log the watcher tails. No thread /
# redirect needed — a separate process never blocks the Julia runtime. `dir`, when given, becomes the
# process's OWN working directory: nswing's output names (-G's stem, the -M-/-M+ mask files, a bare
# maregraph out file, …) are relative-only with no directory override in the module itself, so without
# this they land wherever the calling Julia process's cwd happens to be — never the save-files dir the
# user picked, even though the bathymetry/Source grids we save there sit right next to them.
function _nswing_run_external(scene::Ptr{Cvoid}, args::Vector{String}; dir::Union{String,Nothing} = nothing)
	logf = tempname() * ".nswinglog"
	io   = open(logf, "w")
	progf = _nswing_progress_file()               # -W<file>: nswing rewrites it in place each tick
	args  = vcat(args, "-W$(progf)")
	cmd   = Cmd(vcat("gmt", "nswing", args))
	if dir !== nothing
		cmd = Cmd(cmd; dir = dir)
	end
	cmd = Cmd(cmd; env = _nswing_clean_env())
	proc = try
		run(pipeline(cmd; stdout=io, stderr=io); wait=false)
	catch e
		try; close(io) catch end
		try rm(progf; force = true) catch end
		_progress_close();  _NSWING_RUNNING[] = false
		_viewer_log_error(scene, "NSWING: could not launch `gmt nswing` ($(sprint(showerror, e)))")
		return
	end
	# The process's OWN exit code is the only reliable success/failure signal — nswing's "[ERROR]"
	# stdout tag (_nswing_tail) is a bonus catch, not something every failure prints (a missing/wrong
	# `gmt` on PATH, or a build without the nswing supplement, exits nonzero with its own unrelated
	# message). Ignoring this (as before) reported "run finished" success no matter what actually
	# happened — including a `gmt` that doesn't even have nswing, which just does nothing, fast.
	function _nswing_external_result()
		if success(proc)
			return ""
		end
		logtext = ""
		try
			logtext = strip(read(logf, String))
		catch
			# log file already gone or unreadable — fall through with an empty logtext
		end
		msg = "gmt nswing exited with code $(proc.exitcode)"
		if !isempty(logtext)
			msg = msg * ":\n" * logtext
		end
		return msg
	end
	_nswing_watch(scene, () -> !process_running(proc), _nswing_external_result, logf, io; progf = progf)
	return
end

# Region + increment compatibility between a Source grid and the loaded bathymetry — nswing needs
# them on the SAME grid geometry (same extent, same node spacing). `tol` absorbs float roundoff from
# reading/writing grids, not real mismatches.
function _nswing_check_grid_compat(base::GMTgrid, src::GMTgrid, srcname::AbstractString; tol::Float64=1e-8)
	ok = all(isapprox.(base.range[1:4], src.range[1:4]; atol = tol)) &&
	     all(isapprox.(base.inc,        src.inc;        atol = tol))
	ok || error("NSWING: Source \"$srcname\" is not compatible with the bathymetry grid " *
	            "(bathy -R$(join(base.range[1:4], "/")) -I$(join(base.inc, "/"))  vs  " *
	            "source -R$(join(src.range[1:4], "/")) -I$(join(src.inc, "/")))")
	return nothing
end

# One corner of the nesting check — a direct port of nswing/tintol's `check_binning`. Measured from the
# parent origin x0P (parent inc dxP), the daughter corner x0D obeys the rule iff its offset into the
# parent cell equals dxP/2 ± dxD/2. Pass dxD = +child_inc for a west/south (min) edge, -child_inc for an
# east/north (max) edge. Tolerance = |child_inc|/4 (tintol.m threshD). Returns (bad::Bool, suggest): on
# violation, `suggest` is the nearest compliant corner value; else it is unused.
function _nest_binning(x0P, x0D, dxP, dxD)
	tol = abs(dxD) / 4
	n   = trunc((x0D - x0P) / dxP)                    # MATLAB fix()
	dec = x0D - (x0P + n * dxP)
	if abs(dec - (dxP / 2 + dxD / 2)) > tol
		return true, x0P + n * dxP + dxP / 2 + dxD / 2
	end
	return false, 0.0
end

# The exact Mirone/nswing nesting-rule message for a daughter grid vs its parent — "" when it obeys.
# Pure Julia port of nswing's check_paternity/check_binning (mex/nswing.c), the SAME half-cell rule the
# "Nested grids" rectangles use: each daughter corner must sit at parent_node ∓ parent_inc/2 ± child_inc/2.
# The four texts and the "in grid registration" wording (including Mirone's "doughter" spelling) are
# reproduced verbatim from nswing.c so the console/dialog shows precisely what the tool itself prints.
function _nswing_nest_msg(parent::GMTgrid, child::GMTgrid)
	px0, py0 = parent.range[1], parent.range[3]
	pxi, pyi = parent.inc[1], parent.inc[2]
	cx0, cx1, cy0, cy1 = child.range[1], child.range[2], child.range[3], child.range[4]
	cxi, cyi = child.inc[1], child.inc[2]
	parts = String[]
	(bad, s) = _nest_binning(px0, cx0, pxi,  cxi)      # X_MIN (west edge)
	bad && push!(parts, "Lower left corner of doughter grid does not obey to the nesting rules.\nX_MIN should be (in grid registration):\n\t$(_ffmt(s, 6))")
	(bad, s) = _nest_binning(py0, cy0, pyi,  cyi)      # Y_MIN (south edge)
	bad && push!(parts, "Lower left corner of doughter grid does not obey to the nesting rules.\nY_MIN should be (in grid registration):\n\t$(_ffmt(s, 6))")
	(bad, s) = _nest_binning(px0, cx1, pxi, -cxi)      # X_MAX (east edge)
	bad && push!(parts, "Upper right corner of doughter grid does not obey to the nesting rules.\nX_MAX should be (in grid registration):\n\t$(_ffmt(s, 6))")
	(bad, s) = _nest_binning(py0, cy1, pyi, -cyi)      # Y_MAX (north edge)
	bad && push!(parts, "Upper right corner of doughter grid does not obey to the nesting rules.\nY_MAX should be (in grid registration):\n\t$(_ffmt(s, 6))")
	return join(parts, "\n\n")
end

# Does `child` nest correctly inside `parent`? Throws the Mirone nesting message on violation (used by
# the RUN pre-flight so a bad nest can't reach a run). Same authority as the load-time file check.
function _nswing_check_nest_fits(parent::GMTgrid, child::GMTgrid, parent_label::AbstractString, child_label::AbstractString)
	msg = _nswing_nest_msg(parent, child)
	isempty(msg) && return nothing
	error("NSWING: \"$child_label\" does not nest inside \"$parent_label\".\n$msg")
end

# Resolve a nest reference (a Scene Objects grid NAME or a raw file path) to a live GMTgrid, or `nothing`.
function _nswing_resolve_grid(scene::Ptr{Cvoid}, ref::AbstractString)
	isempty(ref) && return nothing
	g = _nswing_grid_ref(scene, String(ref))          # scene name -> GMTgrid, else String(path)
	g isa GMTgrid && return g
	return try _gmtread_trb(String(g)) catch; nothing end      # grids are READ in "TRB"
end

# Fired the INSTANT a nest-grid file name is loaded/typed in the dialog (C++ NswingDialog, gated on a
# real file). Reads the daughter header, resolves its parent (the window's bathymetry for level 1, the
# previous level's grid otherwise via `parentref`), and PRINTS the Mirone nesting message ("" if it
# obeys) so the C++ side can pop it immediately — the user never has to reach RUN to learn a nest is bad.
function _nswing_check_nest_file(scene::Ptr{Cvoid}, level::Integer, childpath::AbstractString, parentref::AbstractString)
	child = _nswing_resolve_grid(scene, childpath)
	child isa GMTgrid || (print(""); return nothing)  # unreadable / not a grid -> stay silent
	parent = isempty(parentref) ? _find_object(scene, :grid, "") : _nswing_resolve_grid(scene, parentref)
	parent isa GMTgrid || (print(""); return nothing) # no parent to check against yet -> silent
	print(_nswing_nest_msg(parent, child))
	return nothing
end

# Resolve the dialog's OWN Nest chain -- every "nestL<n>=<name>" key (one per level the user has
# visited/typed into, C++ NswingDialog's `nestNames` map, ALL of them, not just the one currently
# showing in the box) -- the same way Source is resolved (_nswing_grid_ref): a Scene Objects grid
# NAME, or a raw file path. This is what lets a manually browsed/typed nest grid -- one that never
# became a live "layerN" scene object -- actually reach nswing. Returns [(level, grid-or-path)…].
function _nswing_dialog_nests(scene::Ptr{Cvoid}, d::Dict{String,String})
	out = Tuple{Int,Any}[]
	for (k, v) in d
		isempty(v) && continue
		m = match(r"^nestL(\d+)$", k)
		m === nothing && continue
		push!(out, (parse(Int, m.captures[1]), _nswing_grid_ref(scene, v)))
	end
	sort!(out; by = first)
	return out
end

# Every blocking check a run needs to pass, shared by the RUN button's synchronous pre-flight
# (_nswing_check, below — lets the C++ dialog pop an error and stay open instead of launching+
# vanishing) and _on_nswing itself (the real run performs the SAME checks, never a subset, so the
# pre-flight can't pass something the real run then rejects). Throws with a human-readable message
# on the FIRST problem found; returns (opts, msgs, nests, base, src) for _on_nswing to build the run
# from — `src` is already resolved here (Scene Objects grid or raw path) so callers never re-resolve.
# `nests` entries are (level, GMTgrid-or-path), same either/or convention as `src`.
function _nswing_validate(scene::Ptr{Cvoid}, d::Dict{String,String})
	# Get nests first to count levels for -M-/-M+ option
	nests = _nswing_scene_nests(scene)               # [(N, G)…] from the live "layerN" scene chain
	# Count max nested grid level (level 2+ means at least layer2 exists)
	max_nest_level = length(nests) > 0 ? maximum(first(n) for (n, _) in nests) : 0

	opts, msgs = _nswing_opts(d; max_nest_level=max_nest_level)
	for m in msgs
		_viewer_log_error(scene, "NSWING: $m")
	end

	# Process nests (already fetched above)
	# The dialog's own Nest chain only fills GAPS: a level the live scene chain doesn't already cover
	# (e.g. a file picked directly instead of built in-scene). A level the scene chain already has
	# wins — it's the live, editable one.
	have = Set(n for (n, _) in nests)
	for extra in _nswing_dialog_nests(scene, d)
		extra[1] in have || push!(nests, extra)
	end
	sort!(nests; by=first)

	# EVERY nest level gets the SAME checks below, whether it came from a live scene grid or a
	# dialog-typed/browsed file — a typed path is loaded here (grdread, header + data) SPECIFICALLY so
	# it obeys the identical nesting rules a scene-built one already does. No exemption for typed
	# files: that was the bug — a bad nest file used to sail through unchecked.
	loaded_nests = Tuple{Int,GMTgrid}[]
	for (n, G) in nests
		push!(loaded_nests, (n, G isa GMTgrid ? G : _gmtread_trb(String(G))))
	end

	# Each SCENE "layerN" starts as a literal all-zero placeholder (_nested_blank_grid, nested.jl)
	# until Transplant fills it with real bathymetry. Feeding a still-blank one to nswing produces a
	# real (non-error) run over zero bathymetry there — reads as "output is all zeros" with nothing
	# in the console to explain why. Fail loud instead of silently simulating on placeholder data.
	for (n, G) in loaded_nests
		all(iszero, G.z) && error("NSWING: \"layer$n\" is still blank (all zero) — " *
		                          "fill it with real bathymetry via Transplant before running NSWING")
	end
	base = _find_object(scene, :grid, "")             # layer0: the window's base bathymetry grid

	# Every nest level must fit inside its PARENT — bathy for layer1, the previous layer for the rest
	# (_nswing_check_nest_fits, same rule the "Nested grids" rectangle tool enforces when it builds the
	# chain). Checked against a REAL bathymetry only.
	if base isa GMTgrid
		parent, parent_label = base, "bathymetry"
		for (n, G) in loaded_nests
			_nswing_check_nest_fits(parent, G, parent_label, "layer$n")
			parent, parent_label = G, "layer$n"
		end
	end

	srcname = _get(d, "source")
	if base isa GMTgrid
		# nswing REQUIRES exactly base+Source (confirmed live: given only 1 grid it returns instantly
		# with an empty log and no error -- a silent no-op, not a failure). Fail loud instead.
		isempty(srcname) && error("NSWING: no Source grid selected -- pick a Source (e.g. \"Okada z\") before running")
	else
		isempty(srcname) && error("NSWING: no Source grid provided")
	end

	src = isempty(srcname) ? "" : _nswing_grid_ref(scene, srcname)
	(base isa GMTgrid && src isa GMTgrid) && _nswing_check_grid_compat(base, src, srcname)

	return opts, msgs, nests, base, src
end

# Synchronous pre-flight check for the RUN button, called via g_juliaEval (NswingDialog's accepted
# handler, 70_window.cpp) BEFORE the dialog closes. Throws on the first blocking problem — the
# console-eval bridge turns that into a negative-length buffer the C++ side pops as a QMessageBox,
# keeping the dialog open instead of launching a doomed run and vanishing. Returns silently (nothing
# printed) when the fields are good to run.
function _nswing_check(scene::Ptr{Cvoid}, cparams::AbstractString)
	d = _nswing_parse(cparams)
	_nswing_validate(scene, d)
	return nothing
end

# The bathymetry (layer0) grid's own known file — the window's originating file, if it was opened
# from one (_path_for_handle, dispatch.jl). Called via g_juliaEval to seed NswingDialog's save-dir
# edit box the FIRST time (before anything is remembered in QSettings); prints "" if unknown.
function _nswing_bathy_dir(scene::Ptr{Cvoid})
	p = _path_for_handle(scene)
	print(isempty(p) ? "" : dirname(p))
	return nothing
end

# Where "Save files & RUN" writes its grids: the dialog's own "savedir" field (its save-dir edit box —
# normally already pre-filled/remembered, see _nswing_bathy_dir) if given, else beside the bathymetry's
# own known file, else pwd().
function _nswing_save_dir(scene::Ptr{Cvoid}, d::Dict{String,String})
	savedir = _get(d, "savedir")
	isempty(savedir) || return savedir
	path = _path_for_handle(scene)
	return isempty(path) ? pwd() : dirname(path)
end

# Decide, for every grid a run needs, where it would live on disk WITHOUT writing anything — shared by
# the existence pre-check (_nswing_existing_files) and the actual save step (_nswing_save_files), so
# the two can never name a file differently. Only grids generated INSIDE this session (a Source
# resolved to a live Scene Objects grid, and the Transplant-filled "layerN" nests) get a NEW path to
# save to; the bathymetry (layer0) reuses its own known file untouched, and a Source already typed/
# resolved as a raw file path is left alone too — this dialog only ever writes what didn't already
# exist on disk. Source/bathymetry compatibility (region+increment) is checked once, in
# _nswing_validate — every caller here already gets a `src` that passed that check.
function _nswing_plan_paths(scene::Ptr{Cvoid}, d::Dict{String,String})
	opts, msgs, nests, base, src = _nswing_validate(scene, d)
	# A CLI run always needs a REAL bathymetry grid to reference — nswing's own usage lists it as the
	# first required positional. Fail loud instead of trying to guess one.
	base isa GMTgrid || error("NSWING: no bathymetry grid in this window to save (open a base grid first)")
	dir = _nswing_save_dir(scene, d)

	bathy_known = _path_for_handle(scene)
	bathy_needs_save = isempty(bathy_known)
	bathy_path = bathy_needs_save ? joinpath(dir, "bathy.grd") : bathy_known

	src_needs_save = src isa GMTgrid
	src_path = src_needs_save ? joinpath(dir, "source.grd") : String(src)

	# A nest already resolved to a raw file path (dialog-typed, not a live scene grid) reuses that path
	# in place — same "already on disk, don't rewrite it" rule Source's typed-path branch follows.
	# Only a live in-memory nest grid gets a NEW path under `dir`.
	nest_needs_save = [G isa GMTgrid for (_, G) in nests]
	nest_paths = [(n, G isa GMTgrid ? joinpath(dir, "layer$n.grd") : String(G)) for (n, G) in nests]

	# Every (path, in-memory grid) this run will actually WRITE — built here, ONCE, and shared verbatim
	# by the existence pre-check and the real save step below, so the two can never name different sets.
	# The grid travels alongside the path so the existence check can tell a REAL overwrite (different
	# content) from a harmless re-save of the same grid (_nswing_grid_differs, grdinfo -C compare).
	write_targets = Tuple{String,GMTgrid}[]
	bathy_needs_save && push!(write_targets, (bathy_path, base))
	src_needs_save   && push!(write_targets, (src_path, src))
	for (needs, (n, G), (_, p)) in zip(nest_needs_save, nests, nest_paths)
		needs && push!(write_targets, (p, G))
	end

	return (; opts, base, bathy_path, bathy_needs_save, src, src_path, src_needs_save,
	          nests, nest_paths, nest_needs_save, dir, write_targets)
end

# Column order of `grdinfo -C`'s single numeric row (verified live against GMT.grdinfo(G, C=true)).
const _NSWING_GRDINFO_FIELDS = ("x_min", "x_max", "y_min", "y_max", "z_min", "z_max",
                                 "dx", "dy", "n_cols", "n_rows", "reg", "isgeog")

# Field-by-field diff between the on-disk grid at `path` and the in-memory grid `G` — both read via
# `grdinfo -C` (one numeric row, no full-data diff or temp file needed). Returns "" when every field
# matches; otherwise one "field: disk=… memory=…" line per field that doesn't. A read failure on
# either side is reported as its own line (NOT swallowed) so "differs" always has a stated reason —
# never a bare "trust me" file-exists popup.
function _nswing_grid_diff_report(path::String, G::GMTgrid)::String
	local a, b
	try
		a = GMT.grdinfo(path, C = true).data
	catch e
		return "  (could not read $path with grdinfo: $(sprint(showerror, e)))"
	end
	try
		b = GMT.grdinfo(G, C = true).data
	catch e
		return "  (could not grdinfo the in-memory grid: $(sprint(showerror, e)))"
	end
	lines = String[]
	for (i, fld) in enumerate(_NSWING_GRDINFO_FIELDS)
		isapprox(a[i], b[i]; atol = 1e-6, rtol = 1e-6) && continue
		push!(lines, "  $fld: disk=$(a[i])  memory=$(b[i])")
	end
	return join(lines, "\n")
end

_nswing_grid_differs(path::String, G::GMTgrid) = !isempty(_nswing_grid_diff_report(path, G))

# Which of the files a save WOULD write already exist on disk AND differ from what's about to be
# written — printed "\n"-joined so the C++ side can pop an overwrite confirmation BEFORE any writing
# happens. A same-content file on disk (grdinfo -C match) is skipped: re-saving it is a no-op, not a
# real overwrite, so it never needs asking about. Empty output = nothing would collide.
function _nswing_existing_files(scene::Ptr{Cvoid}, cparams::AbstractString)
	d = _nswing_parse(cparams)
	plan = _nswing_plan_paths(scene, d)
	targets = [path for (path, G) in plan.write_targets if isfile(path) && _nswing_grid_differs(path, G)]
	print(join(targets, "\n"))
	return nothing
end

# The full field-by-field report for every file _nswing_existing_files flagged — "\n\n"-joined blocks,
# each headed by its path then its differing grdinfo -C fields (_nswing_grid_diff_report). Popped in a
# read-only text box (C++ showInfoText) alongside the short Yes/No confirmation, so a flagged overwrite
# can be checked instead of taken on faith.
function _nswing_existing_files_report(scene::Ptr{Cvoid}, cparams::AbstractString)
	d = _nswing_parse(cparams)
	plan = _nswing_plan_paths(scene, d)
	blocks = String[]
	for (path, G) in plan.write_targets
		isfile(path) || continue
		rep = _nswing_grid_diff_report(path, G)
		isempty(rep) || push!(blocks, "$path\n$rep")
	end
	print(join(blocks, "\n\n"))
	return nothing
end

# Write to disk only the grids that need it (per _nswing_plan_paths) and return the CLI's positional/
# flag pieces plus a plain-language note per file actually written: (opts, bathy_path, src_path,
# nestflags, dir, saved).
#
# `saved` exists because a CLI command line is useless to a user who does not know which of the files
# in it this dialog just created, and WHY it had to: a grid that only ever lived in the window (an
# Okada source, a Transplant-filled nest) has no file for the command to name until we make one, while
# anything already on disk (the bathymetry the window was opened from, a typed-in path) is referenced
# untouched. Both _nswing_show_cli and the RUN path report it, from this one list — never re-derived.
function _nswing_save_files(scene::Ptr{Cvoid}, d::Dict{String,String})
	plan = _nswing_plan_paths(scene, d)
	mkpath(plan.dir)
	saved = String[]
	if plan.bathy_needs_save
		_save_grid(plan.base, "nc", plan.bathy_path)
		push!(saved, "$(basename(plan.bathy_path)) — the window's bathymetry grid, which had no file of its own")
	end
	if plan.src_needs_save
		_save_grid(plan.src, "nc", plan.src_path)
		push!(saved, "$(basename(plan.src_path)) — the Source, a grid that existed only in this session")
	end
	for ((n, G), needs, (_, p)) in zip(plan.nests, plan.nest_needs_save, plan.nest_paths)
		needs || continue
		_save_grid(G, "nc", p)
		push!(saved, "$(basename(p)) — nesting level $n, a grid that existed only in this session")
	end
	nestflags = ["-$(n)$(p)" for (n, p) in plan.nest_paths]   # nswing CLI: -1<grd> -2<grd> … (path ATTACHED, no space)
	return plan.opts, plan.bathy_path, plan.src_path, nestflags, plan.dir, saved
end

# Validate, save every grid this run needs, and assemble the CLI arg vector plus the save dir — shared
# by both "Save files & RUN" sub-options (_nswing_show_cli just prints it; _on_nswing_save_run also
# launches it, running the process IN that dir so its own relative-only outputs land there too — see
# _nswing_run_external). CLI positional order confirmed against the LIVE `gmt nswing` usage: `nswing
# <bathy> <source> [-1<grd> -2<grd> …] <options…>` (see _nswing_opts's mapping notes above).
#
# Grid paths go in RELATIVE to the save dir (`relpath`), not absolute: everything this dialog writes
# lands in `dir`, so they come out as bare "bathy.grd"/"layer1.grd" names instead of a wall of repeated
# directory. A grid that lives somewhere ELSE (a typed-in path, a bathymetry with its own file away
# from `dir`) still gets whatever relpath needs to reach it, so the command stays correct either way.
# Both consumers execute in `dir` — _nswing_run_external passes `dir` as the process's own working
# directory, and the printed command is documented as being run from the grids folder.
function _nswing_prepare_cli(scene::Ptr{Cvoid}, cparams::AbstractString)
	d = _nswing_parse(cparams)
	opts, bathy_path, src_path, nestflags, dir, saved = _nswing_save_files(scene, d)
	rel(p) = try relpath(p, dir) catch; p end          # different drive letter -> keep the absolute path
	nestrel = [f[1:2] * rel(f[3:end]) for f in nestflags]   # "-N" + path, level is always one digit
	return vcat([rel(bathy_path), rel(src_path)], nestrel, opts, ["-v"]), dir, saved
end

# "Save files and show GMT command" sub-option (g_juliaEval, synchronous): save the grids, then PRINT
# what it wrote and why, plus the equivalent `gmt nswing …` command line, for the C++ side to pop in a
# read-only info box. Does NOT run anything.
#
# The command itself also goes to comm.txt IN THE GRIDS FOLDER, because a command that only ever
# appears in a dialog is lost the moment the box is closed — it belongs next to the files it names.
# Grid names in it are relative (see _nswing_prepare_cli), and there is deliberately no `cd` prefix:
# the command is meant to be run FROM that folder, which is where comm.txt sits, so the `cd` was both
# noise and the longest thing on the line.
function _nswing_show_cli(scene::Ptr{Cvoid}, cparams::AbstractString)
	args, dir, saved = _nswing_prepare_cli(scene, cparams)
	cmd = "gmt nswing " * join(args, " ")

	commfile = joinpath(dir, "comm.txt")
	wrote_comm = true
	try
		open(commfile, "w") do io; println(io, cmd); end
	catch e
		wrote_comm = false
		push!(saved, "could NOT write comm.txt: $(sprint(showerror, e))")
	end

	io = IOBuffer()
	if isempty(saved)
		println(io, "Nothing had to be saved — every grid this run needs was already on disk.")
	else
		println(io, "Saved to $dir :")
		for s in saved; println(io, "    ", s); end
	end
	wrote_comm && println(io, "    comm.txt — the command below, kept beside the grids")
	println(io)
	println(io, "Run it from $dir (the grid names below are relative to that folder):")
	println(io)
	print(io, cmd)
	print(String(take!(io)))
	return nothing
end

# "Save files and RUN" sub-option (g_juliaEval, synchronous call — but the launch itself is
# non-blocking, same as the rest of this file's async runs): save the grids, then hand the SAME
# detached-OS-process path the typed-file-paths branch of _on_nswing already uses
# (_nswing_run_external) — its own stdout (-v) feeds the live progress bar via _nswing_watch, run
# INSIDE the save dir so nswing's own relative output names land there too.
function _on_nswing_save_run(scene::Ptr{Cvoid}, cparams::AbstractString)
	if _NSWING_RUNNING[]
		error("NSWING: a run is already in progress.")
	end
	args, dir, saved = _nswing_prepare_cli(scene, cparams)
	isempty(saved) || _viewer_log_info(scene, "NSWING saved to $dir: " * join(saved, "; "))
	_viewer_log_info(scene, "NSWING command (run in $dir): gmt nswing " * join(args, " "))
	_NSWING_RUNNING[] = true
	_progress_show_async(100, "NSWING running…")
	_nswing_run_external(scene, args; dir)
	return nothing
end

# C callback: cparams = "key=value\n…". Assemble the CORRECT nswing invocation and run it asynchronously
# with a live progress bar. The nswing GMT module takes its grids as trailing OBJECTS (never as `?`/paths
# baked into the string): the command carries only bare nesting flags -1 -2 … (one per nest) plus the run
# options + -v, and the grids ride in the arg list in this order —
#     gmt("nswing -1 -2 … -G… -N… -t… -f -v",  base_bathy,  source,  nest1,  nest2, …)
# base_bathy = the window's base grid (layer0), source = the dialog Source grid (e.g. "Okada z"), then the
# scene's "layerN" chain. `-v` makes nswing print its advance as "NN%" which the watcher parses.
function _on_nswing(scene::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		if _NSWING_RUNNING[]
			_viewer_log_error(scene, "NSWING: a run is already in progress.")
			return
		end
		d = _nswing_parse(unsafe_string(cparams))
		opts, msgs, nests, base, src = _nswing_validate(scene, d)

		if base isa GMTgrid
			# ── object run (the normal case): every grid is a live GMTgrid, passed as a trailing arg ──
			# IN-SCENE GRIDS GO TO DISK, then the SAME detached-process run the file branch below uses.
			# They used to travel to a Distributed worker as serialized objects, which is where the run
			# disappeared for half a minute: a base bathymetry plus a nesting chain is tens of MB, the
			# transfer happens before nswing is even launched, and until it ends there is no percentage
			# to show — the dialog sits on "sending grids…" at 0% and reads exactly like a hang. Writing
			# the same grids as temp .grd files takes a fraction of that (a straight disk write, no
			# serialization, no second Julia process to spawn and precompile GMT in), and nswing starts
			# reporting immediately. ONE run path for every case, which is also one less thing to break.
			tmpd = mktempdir()
			gpath(G, nm) = (p = joinpath(tmpd, nm * ".grd"); gmtwrite(p, G); p)
			args = String[gpath(base, "bat")]                                   # positional #1: bathymetry
			push!(args, src isa GMTgrid ? gpath(src, "src") : String(src))      # positional #2: source
			for (n, G) in nests                                                 # -1<file>, -2<file>, …
				push!(args, "-$(n)" * (G isa GMTgrid ? gpath(G, "layer$(n)") : String(G)))
			end
			append!(args, opts);  push!(args, "-v")
			_viewer_log_info(scene, "NSWING command: gmt nswing " * join(args, " "))
			_NSWING_RUNNING[] = true
			_progress_show_async(100, "NSWING running…")
			_nswing_run_external(scene, args)
		else
			# ── file run: no scene base grid; Source/Nest are file paths -> detached `gmt nswing` (paths) ──
			srcpath = _get(d, "source")
			nestpath = _get(d, "nest");  level = tryparse(Int, _get(d, "level", "0"))
			args = String[srcpath]
			(!isempty(nestpath) && level !== nothing && level >= 1) && push!(args, "-$(level)$(nestpath)")
			append!(args, opts);  push!(args, "-v")
			_viewer_log_info(scene, "NSWING command: gmt nswing " * join(args, " "))
			_NSWING_RUNNING[] = true
			_progress_show_async(100, "NSWING running…")
			_nswing_run_external(scene, args)
		end
	catch e
		_progress_close();  _NSWING_RUNNING[] = false
		_tool_failed(scene, "NSWING", e)
	end
	return
end

# Build the C-callable pointer + register it. Lazy (first window) via _ensure_callbacks — the
# @cfunction is a thin invokelatest trampoline so it drags no GMT into compile.
function _register_nswing()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_nswing, s, c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_nswing_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
