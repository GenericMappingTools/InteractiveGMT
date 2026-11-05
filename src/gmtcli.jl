# The `gmtjl` command line launcher (GMT.gmt_cli, GMT.jl >= 1.44.0): (re)written once per session, so it
# never points at GMT_jll artifact directories that an update replaced. Started by `_start_pump` when the
# first window is on screen, and run a second later, so the user never waits for it. A failure (an older
# GMT.jl, no `julia` in the PATH, a read-only directory) is silent: the launcher is a convenience.

const _GMTCLI_DONE = Ref(false)

function _gmtcli_schedule()
	_GMTCLI_DONE[] && return nothing
	_GMTCLI_DONE[] = true
	Timer(1.0) do _
		try
			v = pkgversion(GMT)
			(v === nothing || v < v"1.44.0") && return
			GMT.gmt_cli(verbose = false)
		catch e
			@debug "InteractiveGMT: could not write the gmtjl launcher (harmless)" exception = (e,)
		end
	end
	return nothing
end

# Can a command line run `gmtjl`? "" when it can, else WHY not, in words for the user. Asked by every
# place that hands the user a `gmtjl …` command (NSWING's "Save files & RUN"). The launcher lives next
# to the `julia` found in the PATH, so that is where it is looked for; a missing or stale one is
# (re)written once with GMT.gmt_cli and tried again before giving up.
function _gmtjl_problem()
	v = pkgversion(GMT)
	(v === nothing || v < v"1.44.0") &&
		return "GMT.jl $(something(v, "of unknown version")) is too old: the `gmtjl` launcher needs GMT.jl 1.44.0 or newer. Update it with  ] up GMT"
	jl = Sys.which("julia")
	jl === nothing &&
		return "Julia is not in the PATH, so the `gmtjl` launcher (written next to the julia executable) cannot be found from a command line. Add Julia's bin folder to the PATH."
	launcher = joinpath(dirname(jl), Sys.iswindows() ? "gmtjl.bat" : "gmtjl")
	err = _gmtjl_try(launcher)
	isempty(err) && return ""
	try
		GMT.gmt_cli(verbose = false)
	catch e
		return "Could not write the `gmtjl` launcher in $(dirname(jl)): $(sprint(showerror, e))"
	end
	err = _gmtjl_try(launcher)
	isempty(err) && return ""
	return "`gmtjl` does not run: $err"
end

# Run `<launcher> --version`; "" when it answers, else what went wrong. Killed after 30 s.
function _gmtjl_try(launcher::String)
	isfile(launcher) || return "$launcher does not exist"
	buf = IOBuffer()
	p = try
		run(pipeline(Cmd([launcher, "--version"]); stdout = buf, stderr = buf); wait = false)
	catch e
		return sprint(showerror, e)
	end
	if timedwait(() -> !process_running(p), 30.0) === :timed_out
		kill(p)
		return "$launcher --version did not answer within 30 s"
	end
	success(p) && return ""
	out = strip(String(take!(buf)))
	return "$launcher --version exited with code $(p.exitcode)" * (isempty(out) ? "" : ":\n" * out)
end
