# errors.jl -- THE FAILURE SINK.
#
# Included FIRST (before every file that reports a failure), because @tool_error is a macro: a
# macro must exist before the code that uses it is parsed, and the tools/loader/self-update paths
# that report errors are included ahead of console.jl.

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

# Set by test/runtests.jl for the length of a suite run. It changes WHERE an error is announced, not
# WHETHER: under the suite the ~200 refusals the negative items provoke on purpose are recorded and
# claimed silently instead of each screaming a block of @error + backtrace across the terminal, and
# ANY error that no test claimed is printed — loudly, in full — by the verdict testsets at the end,
# which then fail the run. Outside the suite (a user at the REPL, the viewer running normally) every
# failure prints the moment it happens, exactly as before.
const _TEST_MODE = Ref{Bool}(false)

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

"""
    _take_tool_errors!() -> Vector{String}

The recorded errors, and clears them: the CLAIM half of the sink. A test that provokes an error
takes it here, which is how the end-of-run verdict can insist that anything left behind is an error
nobody was expecting.
"""
_take_tool_errors!() = (v = copy(_TOOL_ERRORS); empty!(_TOOL_ERRORS); v)

"""
    _errored(ret) -> ret

Wrap ONE call into a tool callback that the test expects to fail. Every one of those messages got
there by an exception being raised and swallowed by a `catch` — it is an ERROR, and this is what
makes the test say so out loud:

  * the call returned 0 (refused) and left exactly one error behind -> that error is claimed
    (removed from the sink) and the return value is passed through;
  * it returned 0 and left NOTHING -> the tool failed silently, which the test cannot see. Throws.
  * it returned 1 (success) and still logged an error -> something broke on the way. Throws.

Anything not claimed this way is still in the sink when the run ends, and `runtests.jl` fails on it.
"""
function _errored(ret, tool::AbstractString = "")
	all = _take_tool_errors!()
	# A claim takes ONLY what it is claiming. Anything else in the sink belongs to somebody else --
	# an error raised by an earlier item that no test ever accounted for -- and it goes straight back,
	# so the end-of-run "no unclaimed errors" check still fails on it. Swallowing those here would be
	# the sink hiding failures instead of catching them, which is the whole thing it exists to stop.
	# `tool` may name several ("Euler rotations|Plate calculator"): one callback, several tools behind
	# it. Never a substring so loose it matches everybody -- a claim of "FAILED" would take Roi Crop's
	# errors off the sink from inside a Plates test, and that is the sink hiding failures again.
	mine, foreign = if isempty(tool)
		all, String[]
	else
		names = [lowercase(strip(t)) for t in split(tool, '|') if !isempty(strip(t))]
		hit(m) = any(t -> occursin(t, lowercase(m)), names)
		(filter(hit, all), filter(m -> !hit(m), all))
	end
	for m in foreign
		_record_tool_error(m)
	end
	if ret == 0 && isempty(mine)
		error("`$tool` refused but logged NO error of its own" *
		      (isempty(foreign) ? " — a failure with no message is a failure nobody can see" :
		                          "; the sink held only:\n  " * join(foreign, "\n  ")))
	elseif ret != 0 && !isempty(mine)
		error("`$tool` reported success and still logged an error:\n  " * join(mine, "\n  "))
	end
	# ...and an internal fault is never claimable: `_INTERNAL_ERRORS` keeps its own copy, which
	# nothing here clears, so a MethodError/GMT-C failure inside a "the tool refuses" test still
	# fails the run at the end. Taking it off _TOOL_ERRORS does not take it off the verdict.
	return ret
end

"""
    _tool_failed(scene, what, e)

THE report of a tool blowing up, from inside its `catch`. One call, four jobs:

  * prints `@error "<what> FAILED: …" exception=(e, catch_backtrace())` — the severity it deserves
    AND the exception with its stack, which is the part a developer actually needs;
  * records it in the failure sink, so a test that does not claim it fails the run;
  * puts the same line in the window's console, where the user is looking;
  * returns nothing, so the catch block goes on to return its own 0.

It replaces the two-line `_viewer_log_error(...)` + `@warn ... exception=(e,)` pair every tool used
to carry: same information, one reporter, at error severity instead of warning.
"""
function _tool_failed(scene::Ptr{Cvoid}, what::AbstractString, e)
	msg = "$what FAILED: $(sprint(showerror, e))"
	_record_tool_error(msg)
	_TEST_MODE[] ? (@debug msg exception = (e, catch_backtrace())) :
	               (@error msg exception = (e, catch_backtrace()))
	_viewer_log_info(scene, msg)
	return
end

"""
    @tool_error "message" key=value…

Report a failure from a code path that has NO window handle to log into (a helper below the tools,
a load-time step, a background task). Prints at ERROR severity — it IS an error, it was raised and
caught — and records it in the failure sink, so a test that does not claim it fails the run.

`@warn` was what every one of these used to be, and that is precisely how they stayed invisible:
severity said "carry on", nothing recorded them, and the suite went green over them.
"""
macro tool_error(msg, kws...)
	quote
		_record_tool_error(string($(esc(msg))))
		_TEST_MODE[] ? (@debug $(esc(msg)) $(map(esc, kws)...)) : (@error $(esc(msg)) $(map(esc, kws)...))
	end
end

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
