# warmup.jl — JIT warm-up: compile a tool's code while the user is still filling in its dialog.
#
# THE PROBLEM, and it is general, not specific to one tool. Julia compiles a method the first time
# it RUNS. A tool like Illumination reaches GMT.grdgradient, GMT.kovesi and a pile of iGMT plumbing
# only when the user presses the action button, so that ONE press pays several seconds of compiler
# time that have nothing to do with the maths. Every later press is instant. The cost cannot be
# moved into the precompile image for free — a package workload makes the cache bigger and slower to
# load for every user, whether or not they ever open that tool.
#
# THE FIX. There is dead time between "the dialog opened" and "the user pressed the button": reading
# the controls, picking a model, dragging a light direction. Spend it compiling. The C++ menu action
# calls warmupTool("<tool>") (30_app.cpp) the moment the dialog opens; that lands here and starts a
# background task which runs the tool's real functions on a TINY throw-away input. By the time the
# button is pressed the code is compiled and the press is instant.
#
# WHAT A WARM-UP BODY MAY DO. Run pure computation on its own tiny data — that is what compiles the
# expensive paths. It must NEVER touch the live scene: it runs while a window is on screen, so
# calling anything that adds/hides/reframes an object would change what the user is looking at. For
# those, `precompile(f, types)` infers and generates code WITHOUT running the function, which is
# exactly what is wanted.
#
# THREADS. With more than one Julia thread the task runs on another one and the UI never stutters.
# With the default single thread (the launcher does not pass -t) it runs as a normal task, so a body
# should call `yield()` between steps: the Qt pump timer then gets a slot between compilations and
# the dialog keeps painting. Either way the WHOLE latency has moved off the button press.
#
# RACE. If the user is fast and presses the button while the warm-up is still going, the tool calls
# `warm_wait` first. That blocks until the task is done, so the two never run GMT concurrently — the
# worst case is exactly today's behaviour, never a crash.

const _WARM_BODIES = Dict{String,Function}()      # tool name -> what to compile
const _WARM_TASKS  = Dict{String,Task}()          # tool name -> the one task that ever runs for it

"""
    warm_register(tool::String, body::Function)

Declare how to warm `tool` up. `body` takes no arguments, runs the tool's own maths on tiny data and
must not touch the live scene. Called from the tool's `_register_*` function.
"""
warm_register(tool::String, body::Function) = (_WARM_BODIES[tool] = body; nothing)

"""
    warm_start(tool::String)

Kick off `tool`'s warm-up in the background and return immediately. No-op if the tool has no
registered body, or if it was already started once in this session.
"""
function warm_start(tool::String)
	haskey(_WARM_TASKS, tool) && return nothing        # once per session is the whole point
	body = get(_WARM_BODIES, tool, nothing)
	body === nothing && return nothing
	_WARM_TASKS[tool] = (Threads.nthreads() > 1) ? Threads.@spawn(_warm_run(tool, body)) :
	                                               @async(_warm_run(tool, body))
	return nothing
end

# A warm-up is an optimisation and nothing else: if it throws, the tool still works (it just pays the
# compile time on the button press, as before), so the failure is logged and swallowed.
function _warm_run(tool::String, body::Function)
	t0 = time()
	try
		body()
		@debug "InteractiveGMT: warmed '$tool' in $(round(time() - t0, digits=2)) s"
	catch e
		@debug "InteractiveGMT: warm-up '$tool' failed (harmless)" exception=(e,)
	end
	return nothing
end

"""
    warm_wait(tool::String)

Block until `tool`'s warm-up has finished, if one is running. Call it at the top of the tool's
compute path so the warm-up and the real work never run at the same time.
"""
function warm_wait(tool::String)
	t = get(_WARM_TASKS, tool, nothing)
	(t === nothing || istaskdone(t)) && return nothing
	try
		wait(t)
	catch      # the body threw; _warm_run already logged it and the tool copes on its own
	end
	return nothing
end

# The C callback. Signature is JuliaWarmupFn (30_app.cpp): void fn(const char *tool).
function _on_warmup(tool::Cstring)::Cvoid
	try
		warm_start(unsafe_string(tool))
	catch e
		@debug "InteractiveGMT: warm_start failed" exception=(e,)
	end
	return nothing
end

function _register_warmup()
	fptr = @cfunction((t) -> Base.invokelatest(_on_warmup, t)::Cvoid, Cvoid, (Cstring,))
	ccall(_fn(:gmtvtk_set_warmup_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
