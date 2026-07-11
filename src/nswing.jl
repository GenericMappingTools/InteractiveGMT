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
#  - `-J<jump>` doesn't exist; the do-not-output-before-time flag is `-P<jump>`.
#  - `-B<bcfile>` doesn't exist (bare -B collides with GMT's own frame-annotation option); the boundary-
#    condition file flag is `-O<bcfile>`.
#  - `-T<cumint>,<in>[,<out>]` (comma-positional) is wrong syntax; real form is
#    `-T<in>[+o<out>][+t<cumint>]` (verified live, produced the maregraph output file).
#  - `-n<name>` (MOST format) does not exist in the linked module at all — errors immediately if picked.
function _nswing_opts(d::Dict{String,String})
	args = String[]
	msgs = String[]

	name = _get(d, "name")
	grn  = _get(d, "grn", "10")
	mode = _get(d, "outmode", "grids")
	if mode == "grids"
		isempty(name) && push!(msgs, "no output Name stem given")
		stem = isempty(name) ? "tsu_time_" : name
		push!(args, "-G$(stem),$(grn)")                  # nswing -G<name>,<int> — plain stem, no +m
		_get(d, "field") == "total" && push!(args, "-D") # total water depth
		_on(d, "max")      && push!(args, "-M")          # max water level grid
		_on(d, "velocity") && push!(args, "-S")          # velocity grids (_U/_V)
		_on(d, "momentum") && push!(args, "-H")          # momentum grids
	elseif mode == "anuga"
		isempty(name) && error("NSWING: ANUGA output needs a file Name")
		push!(args, "-A$(name)")
	elseif mode == "most"
		error("NSWING: MOST (.nc) output is not supported by the installed nswing module (no -n option) — pick Output grids or ANUGA")
	end

	manning = _get(d, "manning")
	isempty(manning) || push!(args, "-X$(manning)")      # Manning friction coefficient(s)

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

# Resolve a Source field to what nswing should receive. The dialog seeds it with a Scene Objects grid
# NAME (e.g. "Okada z"); look it up in the window's grid registry and hand back the live GMTgrid — nswing
# takes grid objects directly, so no on-disk conversion is needed. Falls back to the raw string (a typed
# file path) when the name is not a registered scene grid.
function _nswing_grid_ref(scene::Ptr{Cvoid}, name::AbstractString)
	isempty(name) && return ""
	G = _find_object(scene, :grid, String(name))
	G isa GMTgrid ? G : String(name)
end

# Every in-scene "Nested grid N" grid for this window, as (N, GMTgrid) sorted by N — the nesting chain
# the user built (Nested grid 1 → -1, Nested grid 2 → -2, …). Read straight from the object registry.
function _nswing_scene_nests(scene::Ptr{Cvoid})
	out = Tuple{Int,GMTgrid}[]
	v = get(_SCENE_OBJS, scene, nothing)
	v === nothing && return out
	for (k, n, dat) in v
		(k === :grid && dat isa GMTgrid) || continue
		m = match(r"^Nested grid (\d+)$", n)
		m === nothing || push!(out, (parse(Int, m.captures[1]), dat))
	end
	sort!(out; by = first)
	return out
end

# ── async run + live progress ─────────────────────────────────────────────────────────────────
# NSWING is ONE long GMT call (a single `ccall`). Running it in this process would freeze the map window
# even on a worker THREAD: a thread inside a long ccall never reaches a GC safepoint, so the main thread
# stalls at its next allocation (the Qt pump allocates every tick) until the whole run ends. The only way
# to keep the window live is a separate PROCESS. Two run modes, chosen by whether the inputs are live grid
# OBJECTS or plain file paths:
#   • in-memory grids  -> a persistent Distributed WORKER process runs GMT.gmt("nswing … -v", grids...);
#                         the grids travel in-memory over the socket (NO temp files).
#   • all file paths   -> detached `gmt nswing … -V` OS process.
# Either way the run happens off-process, so the main iGMT window never blocks (on any thread count). The
# run writes its stdout/stderr to a small text LOG (not a grid file) that a main-thread Timer tails for the
# -V "… NN%" advance, driving a NON-MODAL progress bar. Only one run at a time.
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

# Main-thread Timer: tail `logf` for nswing's -V "NN %" output → drive the bar + a live ETA label.
# ETA anchors on the FIRST percent seen (time, pct) and extrapolates: elapsed·(100−p)/(p−p0). The raw
# last line is shown whenever no percent is available yet (setup phase), so the dialog is never blank.
# On `isdone()` close the bar and log the outcome (`result()` returns the error string, "" on success).
function _nswing_watch(scene::Ptr{Cvoid}, isdone, result, logf::String, io = nothing)
	t0       = time()                                 # run start: base for both the live ETA and the final total
	pos      = Ref(0)
	anchor   = Ref{Union{Nothing,Tuple{Float64,Int}}}(nothing)  # (time, pct) of the first percent seen
	fatalerr = Ref{Union{String,Nothing}}(nothing)    # nswing's own "[ERROR]" line, if any (see _nswing_tail)
	Timer(0.2; interval = 0.2) do tm
		try
			pct, line, errline = _nswing_tail(logf, pos)
			errline === nothing || (fatalerr[] = errline)
			if pct !== nothing
				anchor[] === nothing && (anchor[] = (time(), pct))
				(at, ap) = anchor[]
				el  = time() - at
				eta = pct > ap ? el * (100 - pct) / (pct - ap) : NaN
				lbl = "NSWING   $(pct)%" * (isnan(eta) ? "" : "   ~$(_hms(eta)) left")
				_progress_status(pct, lbl)
			elseif line !== nothing
				_progress_status(-1, "NSWING: $line")     # setup phase: show raw output, don't move bar
			end
			isdone() || return
			close(tm)
			io === nothing || (try close(io) catch end)
			err = ""
			try err = result() catch e; err = sprint(showerror, e) end
			# nswing can abort on its own fatal check (e.g. dt > dtCFL) and still return a clean status —
			# an "[ERROR]" line it printed itself always overrides an otherwise-empty `err`.
			(isempty(err) && fatalerr[] !== nothing) && (err = fatalerr[])
			try rm(logf; force = true) catch end
			_progress_close()
			_NSWING_RUNNING[] = false
			total = _hms(time() - t0)                 # FINAL time estimate: total wall clock for the whole run
			isempty(err) ? _viewer_log_error(scene, "NSWING: run finished in $total.") :
			               _viewer_log_error(scene, "NSWING FAILED after $total: $err")
		catch e
			close(tm); _progress_close(); _NSWING_RUNNING[] = false
			_viewer_log_error(scene, "NSWING watcher error: $(sprint(showerror, e))")
		end
	end
	return
end

# The one persistent worker process id (0 = none yet). Reused across runs.
const _NSWING_WORKER = Ref(0)

# Lazily spawn ONE worker Julia process with GMT loaded, reused across runs. addprocs + `using GMT` use
# async I/O (they yield), so calling this from an @async task never blocks the main event loop. The worker
# runs in this same project (same dev GMT) and does NOT load InteractiveGMT (no window, no gmtvtk DLL) —
# only GMT, to run the nswing module.
function _nswing_ensure_worker()
	(_NSWING_WORKER[] != 0 && _NSWING_WORKER[] in workers()) && return _NSWING_WORKER[]
	id = addprocs(1)[1]
	remotecall_eval(Main, id, :(using GMT))
	_NSWING_WORKER[] = id
	return id
end

# Worker run: send the resolved GMTgrids to the worker PROCESS (in-memory over the Distributed socket — NO
# temp files) which runs nswing there, so the main iGMT process never blocks on any thread count. A
# main-thread Timer tails the worker's log for the -V percentage. Spawn/setup + remotecall run in an @async
# task (they yield, never freeze).
#
# The run is shipped as an EXPRESSION evaluated by Core.eval on the worker (never a named function/closure
# from THIS module — the worker has no InteractiveGMT, so such a value could not be deserialized there). The
# grids + command + log path are interpolated into the expression, so the grids travel as plain data; the
# expression redirects the worker's own stdout/stderr (incl. libgmt's C output) into the log we tail, runs
# nswing, and yields the error string ("" on success) back through fetch(fut).
function _nswing_run_worker(scene::Ptr{Cvoid}, cmdstr::String, grids::Vector{GMTgrid})
	@async begin
		try
			wid  = _nswing_ensure_worker()
			logf = tempname() * ".nswinglog"
			expr = quote
				e_ = ""
				open($logf, "w") do io
					redirect_stdout(io) do
						redirect_stderr(io) do
							try
								GMT.gmt($cmdstr, $(grids)...)
							catch ex
								e_ = sprint(showerror, ex)
							end
						end
					end
				end
				e_
			end
			fut = remotecall(Core.eval, wid, Main, expr)
			_nswing_watch(scene, () -> isready(fut), () -> fetch(fut), logf)
		catch e
			_progress_close();  _NSWING_RUNNING[] = false
			_viewer_log_error(scene, "NSWING worker setup FAILED: $(sprint(showerror, e))")
		end
	end
	return
end

# File-path run: launch a detached `gmt nswing …` OS process, output → log the watcher tails. No thread /
# redirect needed — a separate process never blocks the Julia runtime.
function _nswing_run_external(scene::Ptr{Cvoid}, args::Vector{String})
	logf = tempname() * ".nswinglog"
	io   = open(logf, "w")
	cmd  = Cmd(vcat("gmt", "nswing", args))
	proc = try
		run(pipeline(cmd; stdout = io, stderr = io); wait = false)
	catch e
		try; close(io) catch end
		_progress_close();  _NSWING_RUNNING[] = false
		_viewer_log_error(scene, "NSWING: could not launch `gmt nswing` ($(sprint(showerror, e)))")
		return
	end
	_nswing_watch(scene, () -> !process_running(proc), () -> "", logf, io)
	return
end

# C callback: cparams = "key=value\n…". Assemble the CORRECT nswing invocation and run it asynchronously
# with a live progress bar. The nswing GMT module takes its grids as trailing OBJECTS (never as `?`/paths
# baked into the string): the command carries only bare nesting flags -1 -2 … (one per nest) plus the run
# options + -V, and the grids ride in the arg list in this order —
#     gmt("nswing -1 -2 … -G… -N… -t… -f -v",  base_bathy,  source,  nest1,  nest2, …)
# base_bathy = the window's base grid (layer0), source = the dialog Source grid (e.g. "Okada z"), then the
# scene's "Nested grid N" chain. `-V` makes nswing print its advance as "NN%" which the watcher parses.
function _on_nswing(scene::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		if _NSWING_RUNNING[]
			_viewer_log_error(scene, "NSWING: a run is already in progress.")
			return
		end
		d = _nswing_parse(unsafe_string(cparams))
		opts, msgs = _nswing_opts(d)
		for m in msgs
			_viewer_log_error(scene, "NSWING: $m")
		end

		nests = _nswing_scene_nests(scene)               # [(N, G)…] -> one -N flag + one object each
		# Each "Nested grid N" starts as a literal all-zero placeholder (_nested_blank_grid, nested.jl)
		# until Transplant fills it with real bathymetry. Feeding a still-blank one to nswing produces a
		# real (non-error) run over zero bathymetry there — reads as "output is all zeros" with nothing
		# in the console to explain why. Fail loud instead of silently simulating on placeholder data.
		for (n, G) in nests
			all(iszero, G.z) && error("NSWING: \"Nested grid $n\" is still blank (all zero) — " *
			                          "fill it with real bathymetry via Transplant before running NSWING")
		end
		base  = _find_object(scene, :grid, "")           # layer0: the window's base bathymetry grid

		if base isa GMTgrid
			# ── object run (the normal case): every grid is a live GMTgrid, passed as a trailing arg ──
			grids = GMTgrid[base]                        # primary #1: base bathymetry (layer0)
			srcname = _get(d, "source")
			# nswing REQUIRES exactly base+Source (confirmed live: given only 1 grid it returns instantly
			# with an empty log and no error -- a silent no-op, not a failure). The file-run branch below
			# already guarded this; this branch didn't, so a missing Source used to look exactly like "it
			# ran fine" with a dead progress bar. Fail loud instead.
			isempty(srcname) && error("NSWING: no Source grid selected -- pick a Source (e.g. \"Okada z\") before running")
			src = _nswing_grid_ref(scene, srcname)       # primary #2: the Source grid
			push!(grids, src isa GMTgrid ? src : GMT.gmtread(String(src)))   # typed path -> load (no temp file)
			cmd = "nswing"
			for (n, _) in nests                          # bare nesting flags: Nested grid N -> -N
				cmd *= " -$(n)"
			end
			isempty(opts) || (cmd *= " " * join(opts, " "))
			cmd *= " -v"
			append!(grids, GMTgrid[G for (_, G) in nests])   # nest objects, in -1,-2,… order

			_viewer_log_error(scene, "NSWING command: $cmd   [+ $(length(grids)) grid objects]")
			_NSWING_RUNNING[] = true
			_progress_show_async(100, "NSWING — starting worker…")
			_nswing_run_worker(scene, cmd, grids)
		else
			# ── file run: no scene base grid; Source/Nest are file paths -> detached `gmt nswing` (paths) ──
			srcpath = _get(d, "source");  isempty(srcpath) && error("NSWING: no Source grid provided")
			nestpath = _get(d, "nest");  level = tryparse(Int, _get(d, "level", "0"))
			args = String[srcpath]
			(!isempty(nestpath) && level !== nothing && level >= 1) && push!(args, "-$(level)$(nestpath)")
			append!(args, opts);  push!(args, "-V")
			_viewer_log_error(scene, "NSWING command: gmt nswing " * join(args, " "))
			_NSWING_RUNNING[] = true
			_progress_show_async(100, "NSWING running…")
			_nswing_run_external(scene, args)
		end
	catch e
		_progress_close();  _NSWING_RUNNING[] = false
		_viewer_log_error(scene, "NSWING FAILED: $(sprint(showerror, e))")
		@warn "nswing: run failed" exception = (e,)
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
