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
