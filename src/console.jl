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
	return _console_write(buf, cap, txt)
end

# Build the C-callable pointer and install it in the DLL. Called once from __init__, after the
# library loads. One @cfunction for the whole session.
function _register_console_eval()
	fptr = @cfunction(_console_eval, Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_fn(:gmtvtk_set_julia_eval), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
