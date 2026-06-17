# Qt event-loop pump (keeps the REPL alive while a window is open) + window utilities.
#
# NON-BLOCKING: the viewer calls return immediately and this Julia Timer pumps the Qt loop, so
# the REPL stays usable. The window lives IN-PROCESS (the DLL is dlopen'd into this session), so
# killing the REPL tears down the process and the window with it — that is shared-process
# lifetime, not a bug.

const _PUMP = Ref{Union{Timer,Nothing}}(nothing)

function _start_pump()
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
