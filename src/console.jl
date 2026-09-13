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

# THE C ENTRY POINT NEVER THROWS. It is called from the Qt pump's callback, so an exception that
# escapes it does not land on a user's `try` — it lands in the pump's `Timer`, which then DIES and
# takes the whole window's event pumping with it ("Error in Timer: SystemError: dup: Bad file
# descriptor", seen on every headless CI runner: see `_console_capture` below). A failure here is a
# message in the console like any other, reported through the negative-byte-count convention.
function _console_eval(scene::Ptr{Cvoid}, cmd::Cstring, buf::Ptr{UInt8}, cap::Cint)::Cint
	try
		return _console_eval_run(scene, cmd, buf, cap)
	catch e
		n = _console_write(buf, cap, sprint(showerror, e))
		return Cint(-n)
	end
end

# Is file descriptor 1 something this process can still redirect through? `stat` on it fails with
# EBADF when it is closed, which is the one question worth asking BEFORE touching redirect_stdout.
#
# ASKED FIRST BECAUSE THE FAILURE IS NOT CATCHABLE ON EVERY PLATFORM. With fd 1 closed, Windows
# throws out of `redirect_stdout()` (SystemError: dup), which a try/catch handles — but Linux does
# NOT throw: dup2 onto a closed descriptor succeeds, the pipe is created, and the process dies later
# inside libuv when the reader task touches it:
#     signal 6 (-6): Aborted ... uv__epoll_ctl_flush ... wait_readnb ... read(rd, String)
# (seen on the ubuntu runner, 2026-09-12). There is no handler for an abort. So the capture is not
# attempted at all unless the descriptor is known good.
function _stdout_capturable()
	try
		stat(RawFD(1))
		return true
	catch
		return false
	end
end

# Start capturing this process's stdout, and say whether it worked. Capturing output is a
# convenience; running the command the user typed is the job, so a process with no usable stdout
# runs it uncaptured and says so in the console.
function _console_capture()
	_stdout_capturable() ||
		return nothing, nothing, nothing, "[this process has no stdout to capture]\n"
	try
		rd, wr = redirect_stdout()
		# PARENTHESISED: `@async f(x), ""` would hand the macro the whole tuple and return three
		# elements instead of four.
		return rd, wr, (@async read(rd, String)), ""
	catch e
		# Said out loud in the console itself (not to stderr, where nobody is looking, and not into
		# the failure sink, which would turn a headless run into a suite failure).
		return nothing, nothing, nothing, "[this process has no stdout to capture: " *
		                                  sprint(showerror, e) * "]\n"
	end
end

function _console_eval_run(scene::Ptr{Cvoid}, cmd::Cstring, buf::Ptr{UInt8}, cap::Cint)::Cint
	code = unsafe_string(cmd)
	fig  = get(_FIGREG, scene, nothing)
	fig !== nothing && Core.eval(Main, :(fig = $fig))   # console's `fig` = this window
	# Capture the command's stdout through a real pipe (redirect_stdout rejects an IOBuffer);
	# an async reader drains it so a chatty command can't deadlock on a full pipe buffer.
	old = stdout
	rd, wr, reader, note = _console_capture()
	val = nothing;  err = nothing
	try
		val = Core.eval(Main, Meta.parseall(code))
	catch e
		err = e
	finally
		if wr !== nothing
			try redirect_stdout(old) catch end
			close(wr)
		end
	end
	txt = reader === nothing ? note : fetch(reader)
	rd === nothing || close(rd)
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
	_record_tool_error(msg)        # so the test suite fails on anything no test claimed
	# Loud the instant it happens for a user; under the test suite the verdict testsets print every
	# unclaimed one at the end instead, so a run does not bury the terminal in expected refusals.
	_TEST_MODE[] ? (@debug msg) : (@error msg)
	# ...and so does the window that was asked to do the work. gmtvtk_log_error, NOT the info twin:
	# the two write the same line into the same log, and what separates them is the RED DOT on the
	# status corner's bubble. A failure raises it; a notice does not.
	try
		ccall(_fn(:gmtvtk_log_error), Cvoid, (Ptr{Cvoid}, Cstring), scene, String(msg))
	catch
	end
	return
end

# The SAME line in the SAME window console, for a message that is NOT a failure: "Saved session ->
# …", "Pasted 412 points", "grdseamount: wrote the time-step grids to …", "already in this window;
# nothing was downloaded". They travelled through _viewer_log_error only because it was the only way
# to put a line in that console — and the day the failures started being RECORDED, every one of these
# notices became a phantom error in the sink (`earthregions` reporting success and "logging an error"
# was the first one to trip `_errored`). Display is shared, intent is not: this one does not record,
# and on the C side it goes to gmtvtk_log_info, which writes the line WITHOUT lighting the status
# corner's red dot — a dot raised by "nothing was downloaded" is a dot nobody reads twice.
function _viewer_log_info(scene::Ptr{Cvoid}, msg::AbstractString)
	try
		ccall(_fn(:gmtvtk_log_info), Cvoid, (Ptr{Cvoid}, Cstring), scene, String(msg))
	catch
	end
	return
end


# Build the C-callable pointer and install it in the DLL. Called once from __init__, after the
# library loads. One @cfunction for the whole session.
function _register_console_eval()
	fptr = @cfunction((s,c,b,n)->Base.invokelatest(_console_eval,s,c,b,n), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_julia_eval), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
