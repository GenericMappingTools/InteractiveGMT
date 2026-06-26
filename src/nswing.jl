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

# Assemble the nswing argument vector from the parsed dialog fields. Returns (args, msgs) where msgs
# are non-fatal notes to echo to the viewer console. Throws on a missing required field.
function _nswing_args(d::Dict{String,String})
	args = String[]
	msgs = String[]

	source = _get(d, "source")
	isempty(source) && error("NSWING: no Source grid provided")
	push!(args, source)                                   # first positional: bathymetry / source grid

	nest  = _get(d, "nest")
	level = tryparse(Int, _get(d, "level", "0"))
	if !isempty(nest) && level !== nothing && level >= 1
		push!(args, "-$(level)$(nest)")                  # nested grid at this level (nswing -1/-2/-3…)
	elseif !isempty(nest)
		push!(msgs, "Nest grid ignored (nesting level is 0 / ready to use)")
	end

	name = _get(d, "name")
	grn  = _get(d, "grn", "10")
	mode = _get(d, "outmode", "grids")
	if mode == "grids"
		isempty(name) && push!(msgs, "no output Name stem given")
		stem = isempty(name) ? "tsu_time_" : name
		if _on(d, "netcdf3d")
			push!(args, "-Z$(stem),$(grn)")             # single 3D netCDF (nswing -Z)
		else
			push!(args, "-G$(stem),$(grn)")             # series of grids (nswing -G)
		end
		_get(d, "field") == "total" && push!(args, "-D") # total water depth
		_on(d, "max")      && push!(args, "-M")          # max water level grid
		_on(d, "velocity") && push!(args, "-S")          # velocity grids (_U/_V)
		_on(d, "momentum") && push!(args, "-H")          # momentum grids
	elseif mode == "anuga"
		isempty(name) && error("NSWING: ANUGA output needs a file Name")
		push!(args, "-A$(name)")
	elseif mode == "most"
		isempty(name) && error("NSWING: MOST output needs a base Name")
		push!(args, "-n$(name)")
	end

	manning = _get(d, "manning")
	isempty(manning) || push!(args, "-X$(manning)")      # Manning friction coefficient(s)

	if _on(d, "maregs")
		mi = _get(d, "maregin"); mo = _get(d, "maregout"); ci = _get(d, "cumint", "1")
		if isempty(mi)
			push!(msgs, "Maregraphs checked but no In file given — skipping -T")
		else
			push!(args, isempty(mo) ? "-T$(ci),$(mi)" : "-T$(ci),$(mi),$(mo)")
		end
	end

	ncycles = _get(d, "ncycles", "1010"); push!(args, "-N$(ncycles)")
	jump = tryparse(Float64, _get(d, "jump", "0"))
	(jump !== nothing && jump > 0) && push!(args, "-J$(_get(d, "jump"))")

	dt = _get(d, "dt")
	isempty(dt) && error("NSWING: Time step (sec) is required (nswing -t)")
	push!(args, "-t$(dt)")

	_on(d, "geog") && push!(args, "-f")                  # geographical coordinates

	bc = _get(d, "bc")
	isempty(bc) || push!(args, "-B$(bc)")                # boundary condition file (experimental)

	return args, msgs
end

# C callback: cparams = "key=value\n…". Build the nswing command and launch it.
function _on_nswing(scene::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		d = _nswing_parse(unsafe_string(cparams))
		args, msgs = _nswing_args(d)
		for m in msgs
			_viewer_log_error(scene, "NSWING: $m")
		end

		exe = Sys.which("nswing")
		if exe === nothing
			exe = "nswing"                               # fall through; report not-found below
		end
		cmdline = "nswing " * join(args, " ")
		_viewer_log_error(scene, "NSWING command: $cmdline")

		if Sys.which("nswing") === nothing
			_viewer_log_error(scene, "NSWING: 'nswing' not found on PATH — command assembled but NOT run.")
		else
			cmd = Cmd(vcat(exe, args))
			run(cmd; wait = false)                       # detached: don't freeze the Qt pump on a long run
			_viewer_log_error(scene, "NSWING: launched (output goes to its own console).")
		end
	catch e
		_viewer_log_error(scene, "NSWING FAILED: $(sprint(showerror, e))")
		@warn "nswing: run failed" exception=(e,)
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
