# The in-window Julia console's C entry point (src/console.jl). It is called from the Qt pump, so
# the one thing it must never do is THROW: an exception that escapes it unwinds through the C++
# pump frame, and the window then stops processing events while still looking alive. That is what
# broke CI on 2026-09-12 — `redirect_stdout()` dups the real file descriptor and a headless runner
# had none to dup, so the throw happened before any handler in that file could see it.

@testitem "console: an erroring command is a message, never a throw" tags=[:unit] begin
	IG = InteractiveGMT
	buf = zeros(UInt8, 4096)
	call(code) = IG._console_eval(Ptr{Cvoid}(C_NULL),
		Base.unsafe_convert(Cstring, Base.cconvert(Cstring, code)), pointer(buf), Cint(length(buf)))
	n = call("1 + 1")
	@test n > 0
	@test occursin("2", unsafe_string(pointer(buf)))
	# A failing command comes back as a NEGATIVE byte count carrying the error text.
	n = call("error(\"boom\")")
	@test n < 0
	@test occursin("boom", unsafe_string(pointer(buf)))
	# ...and so does one that cannot even be parsed.
	@test call("function") < 0
	# stdout the command produced is captured and returned with the value.
	n = call("println(\"hello-console\"); 42")
	@test n > 0
	txt = unsafe_string(pointer(buf))
	@test occursin("hello-console", txt) && occursin("42", txt)
end

@testitem "console: it survives a process with no stdout to capture" tags=[:unit] begin
	# THE CI CONDITION, reproduced: file descriptor 1 is closed, so `redirect_stdout()` throws
	# SystemError("dup"). The console must still run the command and answer — capturing output is a
	# convenience, running what the user typed is the job. Done in a CHILD process, because closing
	# fd 1 is not something to do to the test runner.
	script = """
	using InteractiveGMT
	IG = InteractiveGMT
	buf = zeros(UInt8, 4096)
	ccall($(Sys.iswindows() ? ":_close" : ":close"), Cint, (Cint,), 1)   # no stdout left to dup
	n = IG._console_eval(Ptr{Cvoid}(C_NULL),
		Base.unsafe_convert(Cstring, Base.cconvert(Cstring, "6 * 7")), pointer(buf), Cint(4096))
	txt = unsafe_string(pointer(buf))
	print(stderr, "RESULT n=\$n txt=\$txt")
	"""
	err = IOBuffer()
	p = run(pipeline(`$(Base.julia_cmd()) --startup-file=no -e $script`;
	                 stdout = devnull, stderr = err); wait = false)
	wait(p)
	out = String(take!(err))
	@test occursin("RESULT", out)                 # it returned at all: nothing was thrown
	m = match(r"RESULT n=(-?\d+) txt=(.*)"s, out)
	@test m !== nothing
	@test parse(Int, m[1]) > 0                    # a plain result, not an error
	@test occursin("42", m[2])                    # ...and it is the command's own answer
end

@testitem "console: InteractiveUtils is there, as in the REPL (versioninfo)" tags=[:unit] begin
	# A session with NO REPL (the desktop launcher) never gets `using InteractiveUtils` in Main; the
	# console answered `versioninfo()` with an UndefVarError. Run in a CHILD started with -e, which is
	# exactly such a session.
	script = """
	using InteractiveGMT
	print(stderr, "BEFORE=", isdefined(Main, :versioninfo), " ")
	buf = zeros(UInt8, 1 << 16)
	n = InteractiveGMT._console_eval(Ptr{Cvoid}(C_NULL),
		Base.unsafe_convert(Cstring, Base.cconvert(Cstring, "versioninfo()")), pointer(buf), Cint(length(buf)))
	print(stderr, "RESULT n=\$n txt=", unsafe_string(pointer(buf)))
	"""
	err = IOBuffer()
	run(pipeline(`$(Base.julia_cmd()) --startup-file=no -e $script`; stdout = devnull, stderr = err))
	out = String(take!(err))
	@test occursin("BEFORE=false", out)            # the condition is real: no InteractiveUtils in Main
	m = match(r"RESULT n=(-?\d+) txt=(.*)"s, out)
	@test m !== nothing
	@test parse(Int, m[1]) > 0                     # not an error
	@test occursin("Julia Version", m[2])          # ...and it is versioninfo's own report
end

@testitem "console: `ans` is the last result, as in the REPL" tags=[:unit] begin
	IG = InteractiveGMT
	buf = zeros(UInt8, 4096)
	call(code) = IG._console_eval(Ptr{Cvoid}(C_NULL),
		Base.unsafe_convert(Cstring, Base.cconvert(Cstring, code)), pointer(buf), Cint(length(buf)))
	@test call("6 * 7") > 0
	@test call("ans + 1") > 0
	@test occursin("43", unsafe_string(pointer(buf)))
	@test call("error(\"boom\")") < 0               # a failure leaves `ans` alone
	@test call("ans") > 0
	@test occursin("43", unsafe_string(pointer(buf)))
	@test call(":sym") > 0                          # a Symbol is stored, not evaluated
	@test call("ans === :sym") > 0
	@test occursin("true", unsafe_string(pointer(buf)))
end
