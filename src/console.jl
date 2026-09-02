# In-window Julia console. The viewer runs IN-PROCESS in this Julia session, so the console
# dock hands a typed command straight back here to eval in `Main`. `scene` is the window's C
# handle: we bind `fig` to that window's figure object before eval, so `add!(fig, D)` etc. just
# work. stdout produced by the command is captured and returned alongside the value's repr.
#
# The @cfunction pointer and its registration are RUNTIME values, so they are created in the
# module __init__ via _register_console_eval (NOT at top level — a precompiled @cfunction is
# invalid).

# Copy `s` into the C result buffer (cap-1 bytes max, NUL room); return bytes written.
function _console_write(buf::Ptr{UInt8}, cap::Cint, s::AbstractString)::Cint
	str = String(s)
	n = min(sizeof(str), Int(cap) - 1)
	n > 0 && GC.@preserve str unsafe_copyto!(buf, pointer(str), n)
	return Cint(n)
end

function _console_eval(scene::Ptr{Cvoid}, cmd::Cstring, buf::Ptr{UInt8}, cap::Cint)::Cint
	code = unsafe_string(cmd)
	fig  = get(_FIGREG, scene, nothing)
	fig !== nothing && Core.eval(Main, :(fig = $fig))   # console's `fig` = this window
	# Capture the command's stdout through a real pipe (redirect_stdout rejects an IOBuffer);
	# an async reader drains it so a chatty command can't deadlock on a full pipe buffer.
	old = stdout
	rd, wr = redirect_stdout()
	reader = @async read(rd, String)
	val = nothing;  err = nothing
	try
		val = Core.eval(Main, Meta.parseall(code))
	catch e
		err = e
	finally
		redirect_stdout(old);  close(wr)
	end
	txt = fetch(reader);  close(rd)
	if err !== nothing
		(!isempty(txt) && !endswith(txt, "\n")) && (txt *= "\n")
		txt *= sprint(showerror, err)
	elseif val !== nothing
		(!isempty(txt) && !endswith(txt, "\n")) && (txt *= "\n")
		txt *= sprint(show, MIME("text/plain"), val)
	end
	# Byte count back to C; NEGATIVE flags an error so the caller (e.g. the X,Y tool's collapsed
	# Console) can pop open / highlight it. Callers that ignore the sign just read |n| bytes.
	n = _console_write(buf, cap, txt)
	return err === nothing ? n : Cint(-n)
end

# Push one execution-error line into a 3-D viewer window's read-only "Errors" tab (gmtvtk_log_error).
# `scene` is the window's C handle. The X,Y tool has its own twin (_xy_log -> gmtvtk_xyplot_log).
# Best-effort + NEVER throws, so a catch block can call it without masking the original error.
function _viewer_log_error(scene::Ptr{Cvoid}, msg::AbstractString)
	_record_tool_error(msg)
	try
		ccall(_fn(:gmtvtk_log_error), Cvoid, (Ptr{Cvoid}, Cstring), scene, String(msg))
	catch
	end
	return
end

# ---------------------------------------------------------------------------------------------
# The failure sink.
#
# EVERY tool in this package ends its catch block the same way: `_viewer_log_error(scene, "X
# FAILED: …")` (+ a `@warn`), then returns 0. That is right for the GUI -- an exception must not
# cross the C callback boundary -- but it made the test suite BLIND: an item that calls the
# callback and does not check its return value passes while the tool did nothing at all, and the
# only trace is a warning nobody reads. Disguised errors, in the user's words.
#
# So the funnel keeps a record. It lives HERE, in the one function every failure already passes
# through (never a second copy per tool, per SACRED_LAW.md), and the test tier asserts on it:
# `IG._clear_tool_errors!()` before the code under test, `@test isempty(IG._tool_errors())` after.
# A test that EXPECTS a refusal (bad input, missing file -- most of the "FAILED" noise in CI is
# exactly that) asserts the message instead, so an intended refusal and a real crash stop looking
# identical.
#
# Bounded, and recording never throws: a catch block calls this, and it must not mask the error it
# is reporting.
const _TOOL_ERRORS = Vector{String}()
const _TOOL_ERRORS_MAX = 500

"""
    _tool_errors() -> Vector{String}

The failure messages logged into viewer windows since the last `_clear_tool_errors!()`, in order.
"""
_tool_errors() = copy(_TOOL_ERRORS)

"""
    _clear_tool_errors!()

Empty the failure sink. Call it right before the code under test.
"""
_clear_tool_errors!() = (empty!(_TOOL_ERRORS); nothing)

function _record_tool_error(msg::AbstractString)
	try
		length(_TOOL_ERRORS) >= _TOOL_ERRORS_MAX && popfirst!(_TOOL_ERRORS)
		push!(_TOOL_ERRORS, String(msg))
		if _is_internal_failure(msg)
			length(_INTERNAL_ERRORS) >= _TOOL_ERRORS_MAX && popfirst!(_INTERNAL_ERRORS)
			push!(_INTERNAL_ERRORS, String(msg))
		end
	catch
	end
	return
end

# The second sink, and the one the test suite fails on. A tool refusing bad input with a sentence a
# user can read ("West must be smaller than East", "input table not found: …") is WORKING -- most of
# the "FAILED" noise a test run prints is exactly that, from items that feed garbage on purpose. An
# exception that got as far as GMT's own C error, or that is a plain Julia type/lookup fault, is a
# BUG: either the tool skipped a check it owes the user, or it broke. Those two look identical in a
# log and identical to a `@test call(...) == 0`, which is how "Earth regions FAILED: Something went
# wrong when calling the module. GMT error number = 72" sat green in CI.
#
# So they are separated HERE, once, and `runtests.jl` asserts this list is empty at the end of the
# run. Unlike _TOOL_ERRORS it is NOT cleared between items -- it is the run's verdict.
const _INTERNAL_ERRORS = Vector{String}()

const _INTERNAL_MARKS = (
	"Something went wrong when calling the module",   # GMT's own C-level failure, whatever it was
	"MethodError", "UndefVarError", "BoundsError", "KeyError", "DimensionMismatch",
	"StackOverflowError", "InexactError", "no method matching", "UndefRefError",
	"AssertionError", "SegmentationFault",
	# Qt's own diagnostics, drained by _drain_qt_messages. Qt files the first level under "warning";
	# it is an ERROR — this code did something wrong (a teardown out of order, a widget given two
	# layouts, a connect to nothing) and merely did not die on the spot. Captured as "Qt ERROR".
	"Qt ERROR",
)

_is_internal_failure(msg::AbstractString) = any(m -> occursin(m, msg), _INTERNAL_MARKS)

"""
    _internal_tool_errors() -> Vector{String}

Failures recorded this session that are NOT a tool refusing bad input politely: a GMT C-level error,
or a raw Julia type/lookup fault. Each one is a bug. `runtests.jl` asserts this is empty.
"""
_internal_tool_errors() = copy(_INTERNAL_ERRORS)

"Forget the recorded internal failures (used by the tests that deliberately provoke one)."
_clear_internal_tool_errors!() = (empty!(_INTERNAL_ERRORS); nothing)

# Build the C-callable pointer and install it in the DLL. Called once from __init__, after the
# library loads. One @cfunction for the whole session.
function _register_console_eval()
	fptr = @cfunction((s,c,b,n)->Base.invokelatest(_console_eval,s,c,b,n), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_julia_eval), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
