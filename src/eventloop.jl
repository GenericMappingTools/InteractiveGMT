# Qt event-loop pump (keeps the REPL alive while a window is open) + window utilities.
#
# NON-BLOCKING: the viewer calls return immediately and this Julia Timer pumps the Qt loop, so
# the REPL stays usable. The window lives IN-PROCESS (the DLL is dlopen'd into this session), so
# killing the REPL tears down the process and the window with it — that is shared-process
# lifetime, not a bug.

const _PUMP = Ref{Union{Timer,Nothing}}(nothing)

# ---------------------------------------------------------------------------------------------
# Debug log for C-invoked callbacks (_on_geography / _on_drop / console / basemap …). Their
# stdout is the Julia process stdout, which is LOST under the detached .vbs/.bat launcher, so a
# print there vanishes. `_dbg` appends to a file instead. OFF by default (zero I/O): set the env
# var INTERACTIVEGMT_DEBUG to enable — "1" -> %TEMP%/igmt.log, any other value -> that path.
# Toggle live at the REPL: `ENV["INTERACTIVEGMT_DEBUG"]="1"` (no rebuild/restart). Usage:
#   _dbg("volc", "W=$W E=$E S=$S N=$N", "n=", length(xs))   # one line, append + flush per call
# Tail it from another shell:  Get-Content $env:TEMP\igmt.log -Wait -Tail 20
_dbg_path() = (v = get(ENV, "INTERACTIVEGMT_DEBUG", ""); isempty(v) ? "" : v == "1" ? joinpath(tempdir(), "igmt.log") : v)
function _dbg(args...)
	path = _dbg_path()
	isempty(path) && return                          # disabled -> no file touch, no allocation cost
	try
		open(path, "a") do io                        # append-only + closed each call: survives a REPL crash
			println(io, round(time(); digits=3), "  ", join(string.(args), "  "))
		end
	catch                                            # logging must never break a callback
	end
	return
end

# Lazy, one-shot registration of every host callback (console, drop, menu actions). RUNTIME ONLY —
# each registration runs `@cfunction` whose inference reaches deep into GMT, so doing it in `__init__`
# made `using InteractiveGMT` pay that cost up front for every feature. Instead we defer to the first
# window open (every opener calls `_start_pump`): no window => no menus => no need for the callbacks.
# Guarded by `_CB_DONE`; idempotent. The `@cfunction`s themselves are thin `invokelatest` trampolines
# (see each `_register_*`) so this stays cheap and never drags GMT into a precompiled image.
const _CB_DONE = Ref(false)
function _ensure_callbacks()
	_CB_DONE[] && return
	_CB_DONE[] = true                                # set first: a failing reg must not retry forever
	for (name, fn) in (("console",     _register_console_eval),
	                    ("drop",        _register_drop_callback),
	                    ("xy",          _register_xy_callback),
	                    ("xy-analysis", _register_xy_analysis),
	                    ("xy-seed",     _register_xy_seed),
	                    ("xy-new",      _register_xy_new),
	                    ("basemap",     _register_basemap),
	                    ("tiles",       _register_tiles),
	                    ("bgregion",    _register_bgregion),
	                    ("geography",   _register_geography),
	                    ("tides",       _register_tides),
	                    ("earthtide",   _register_earthtide))
		try
			fn()
		catch e
			@warn "InteractiveGMT: registration '$name' failed; that feature will be \"not wired\" in the viewer. Rebuild the DLL (deps/build.bat) and restart Julia if the export is missing." exception=(e,)
		end
	end
	return
end

function _start_pump()
	_ensure_callbacks()                              # wire menu/console/drop callbacks once, lazily
	_PUMP[] === nothing || return                    # already pumping
	_PUMP[] = Timer(0.0; interval=0.02) do t         # ~50 Hz
		n = ccall(_fn(:gmtvtk_process_events), Cint, ())
		if n <= 0                                     # all windows closed
			close(t); _PUMP[] = nothing
		end
	end
	return
end

"""
	save_png(path) -> Bool

Save a PNG of the most-recently-opened viewer window to `path`. Returns `true` on success.
"""
function save_png(path::AbstractString)
	ok = ccall(_fn(:gmtvtk_save_png), Cint, (Cstring,), String(path))
	return ok != 0
end

"""
	stereo!(fig, on=-1) -> Bool

Toggle red/cyan **anaglyph** stereo on a viewer window (use cheap red/cyan 3-D glasses to see
real depth on the relief). `on=true` enables, `on=false` disables, default `-1` flips the
current state. Sets the stereo type to anaglyph so it actually renders on a normal monitor.
Returns the new state (`true` = on). `fig` is any viewer handle (`QtFigure`/`QtPoints`/`QtFV`).
"""
function stereo!(fig, on::Union{Bool,Integer}=-1)
	r = ccall(_fn(:gmtvtk_set_stereo), Cint, (Ptr{Cvoid}, Cint), fig.h, Cint(Int(on)))
	r < 0 && error("stereo!: the viewer window is closed")
	return r != 0
end

"""
	_gmtwrite_line(tmp, out, ispoly) -> nothing

Save a line/polygon the C++ viewer wrote as a temp multisegment text file (`tmp`) into `out`,
in whatever vector format the **extension** implies (`.gpkg`/`.shp`/`.kml`/… via `GMT.gmtwrite`).
`ispoly` tags the geometry as polygon (else line). Invoked from the viewer's "Save line/polygon…"
when an OGR extension is chosen (C++ `g_juliaEval` → here). Deletes `tmp` when done.
"""
function _gmtwrite_line(tmp::AbstractString, out::AbstractString, ispoly::Bool)
	try
		D = GMT.gmtread(String(tmp))
		geom = ispoly ? 3 : 2                         # wkbPolygon : wkbLineString
		if D isa AbstractVector
			for d in D
				hasproperty(d, :geom) && (d.geom = geom)
			end
		elseif hasproperty(D, :geom)
			D.geom = geom
		end
		GMT.gmtwrite(String(out), D)
	catch err
		@warn "gmtwrite failed to save the line/polygon" out exception=err
	finally
		try; rm(String(tmp); force=true); catch; end
	end
	return nothing
end

"""
	wait_windows()

Block (yielding, so the Qt pump keeps running) until every viewer window is closed. Useful at
the tail of a `julia script.jl` run, where there is no REPL to keep the process alive.
"""
function wait_windows()
	while _PUMP[] !== nothing
		sleep(0.05)
	end
	return
end
