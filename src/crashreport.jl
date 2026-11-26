# crashreport.jl -- a crash on a USER'S machine always leaves a report, and the next start offers to
# send it (Windows, the desktop app). Nothing is shown, written or kept for a session that ends well.
#
#   session    __init__ makes ~/.gmt/igmt_crashes/session_<pid>/ (info.txt) and hands it to the viewer
#              (gmtvtk_crash_install, deps/src/crashreport.cpp): a memory-mapped trail of the last user
#              actions, and -- if native code faults -- stack.txt + crash.dmp. Julia's atexit deletes
#              the folder when the process exits with code 0. Any other ending leaves it there.
#   next start once the first window is up, every session_<pid> whose process is gone is looked at.
#              It crashed if it has a stack.txt, or if the Windows Application log has a julia.exe
#              crash record ("Application Error" / "Application Hang") for that pid -- Windows writes
#              one for EVERY death, heap corruption and __fastfail included, which no handler in the
#              process can see. Neither = ended by Windows shutdown / Task Manager: deleted, no report.
#   report     the folder (+ that event, + the launcher's julia log) becomes igmt_crash_<time>.zip,
#              home paths replaced by "~", and a dialog asks the user to e-mail it to _CRASH_EMAIL:
#              "Send e-mail..." opens the mail program with subject and text filled in, plus Explorer
#              showing the zip to attach. No account of any kind is needed.
#
# Only in the desktop app (iview_app.jl): a REPL or the test suite never makes sessions nor shows the
# dialog. Cost while nothing crashes: one folder + one small file at start, one memcpy per user click.

const _CRASH_SESSION = Ref{String}("")
const _CRASH_EMAIL   = "jluis@ualg.pt"

_crash_root() = joinpath(get(ENV, "USERPROFILE", homedir()), ".gmt", "igmt_crashes")
_crash_has(sym::Symbol) = get(_LIB_FNS, sym, C_NULL) != C_NULL
_crash_app_mode() = Sys.iswindows() && basename(Base.PROGRAM_FILE) == "iview_app.jl"

# A line into the crash trail from the Julia side.
function _crash_trail(s::AbstractString)
	_crash_has(:gmtvtk_crash_trail) && ccall(_fn(:gmtvtk_crash_trail), Cvoid, (Cstring,), s)
	return nothing
end

# The checked-out iGMT commit, read off .git (no LibGit2 repo object at start-up).
function _crash_commit()::String
	g = joinpath(dirname(@__DIR__), ".git")
	try
		h = strip(read(joinpath(g, "HEAD"), String))
		startswith(h, "ref: ") || return h
		ref = h[6:end]
		f = joinpath(g, ref)
		isfile(f) && return strip(read(f, String))
		for l in readlines(joinpath(g, "packed-refs"))
			endswith(l, " " * ref) && return first(split(l))
		end
	catch
	end
	return "?"
end

function _crash_info()::String
	dll = _LIB_USED[]
	io = IOBuffer()
	println(io, "started     ", GMT.Dates.format(GMT.Dates.now(), "yyyy-mm-dd HH:MM:SS"), " (local)")
	println(io, "iGMT        ", _crash_commit())
	println(io, "gmtvtk.dll  ", dll, isfile(dll) ? "  (" * GMT.Dates.format(GMT.Dates.unix2datetime(mtime(dll)), "yyyy-mm-dd HH:MM") * " UTC)" : "")
	println(io, "Julia       ", VERSION, ", ", Threads.nthreads(), " threads")
	println(io, "GMT         ", try GMT.GMTver catch; "?" end)
	println(io, "Windows     ", Sys.iswindows() ? Sys.windows_version() : Sys.KERNEL)
	println(io, "memory      ", round(Sys.total_memory() / 2^30; digits = 1), " GB, ", Sys.CPU_THREADS, " CPU threads")
	return String(take!(io))
end

# __init__, after the viewer DLL loaded.
function _crash_session_start()
	(_crash_app_mode() && _crash_has(:gmtvtk_crash_install)) || return nothing
	dir = joinpath(_crash_root(), "session_$(getpid())")
	rm(dir; recursive = true, force = true)
	mkpath(dir)
	write(joinpath(dir, "info.txt"), _crash_info())
	ccall(_fn(:gmtvtk_crash_install), Cint, (Cstring,), dir) == 1 || return nothing
	_CRASH_SESSION[] = dir
	atexit(code -> code == 0 && rm(dir; recursive = true, force = true))
	Timer(1.0; interval = 1.0) do t                 # the report dialog needs a window on screen
		_PUMP[] === nothing && return
		close(t)
		try
			_crash_check_previous()
		catch e
			@debug "InteractiveGMT: crash report check failed" exception = (e,)
		end
	end
	return nothing
end

# Is `pid` a live julia.exe? (A pid reused by any other program is not our session.)
function _crash_pid_alive(pid::Int)::Bool
	h = ccall((:OpenProcess, "kernel32"), stdcall, Ptr{Cvoid}, (UInt32, Cint, UInt32), 0x1000, 0, pid)
	h == C_NULL && return false
	code = Ref{UInt32}(0)
	buf  = Vector{UInt16}(undef, 1024)
	len  = Ref{UInt32}(length(buf))
	alive = ccall((:GetExitCodeProcess, "kernel32"), stdcall, Cint, (Ptr{Cvoid}, Ref{UInt32}), h, code) != 0 &&
	        code[] == 259 &&
	        ccall((:QueryFullProcessImageNameW, "kernel32"), stdcall, Cint, (Ptr{Cvoid}, UInt32, Ptr{UInt16}, Ref{UInt32}),
	              h, 0, buf, len) != 0 &&
	        endswith(lowercase(transcode(String, buf[1:len[]])), "julia.exe")
	ccall((:CloseHandle, "kernel32"), stdcall, Cint, (Ptr{Cvoid},), h)
	return alive
end

function _crash_events()::Vector{String}
	_crash_has(:gmtvtk_crash_eventlog) || return String[]
	buf = Vector{UInt8}(undef, 1 << 20)
	n = ccall(_fn(:gmtvtk_crash_eventlog), Cint, (Ptr{UInt8}, Cint), buf, length(buf))
	n <= 0 && return String[]
	return filter(!isempty, split(String(buf[1:n]), '\n'))
end

# The event log record of process `pid`, or nothing. Every <Data> value is compared, so the field
# order (which differs between "Application Error" and "Application Hang", and between Windows
# versions) does not matter.
function _crash_event_for(events::Vector{String}, pid::Int)
	want = "0x" * string(pid; base = 16)
	for x in events
		vals = [lowercase(strip(m[1])) for m in eachmatch(r"<Data[^>]*>([^<]*)</Data>", x)]
		want in vals && return x
	end
	return nothing
end

# "ModuleName: gmtvtk.dll" lines out of an event record (named fields on current Windows, the
# documented order of "Application Error" otherwise).
function _crash_event_fields(x::String)::Vector{Pair{String,String}}
	id = something(match(r"<EventID[^>]*>(\d+)</EventID>", x), (; captures = ["?"])).captures[1]
	named = [m[1] => m[2] for m in eachmatch(r"<Data Name=['\"]([^'\"]+)['\"]>([^<]*)</Data>", x)]
	if isempty(named)
		vals  = [m[1] for m in eachmatch(r"<Data[^>]*>([^<]*)</Data>", x)]
		names = ("AppName", "AppVersion", "AppTimeStamp", "ModuleName", "ModuleVersion", "ModuleTimeStamp",
		         "ExceptionCode", "FaultingOffset", "ProcessId", "ProcessCreationTime", "AppPath", "ModulePath")
		named = [names[i] => vals[i] for i in 1:min(length(names), length(vals))]
	end
	return vcat(["EventID" => (id == "1002" ? "1002 (Application Hang)" : id == "1000" ? "1000 (Application Error)" : id)],
	            String.(first.(named)) .=> String.(last.(named)))
end

# Home paths out: a report may be posted publicly.
function _crash_scrub(s::AbstractString)::String
	t = String(s)
	for h in unique(filter(!isempty, [get(ENV, "USERPROFILE", ""), homedir()]))
		for v in (h, replace(h, '\\' => '/'), replace(h, '\\' => "\\\\"))
			t = replace(t, v => "~"; count = typemax(Int))
			t = replace(t, lowercase(v) => "~"; count = typemax(Int))
		end
	end
	return t
end

_crash_trail_lines(f::String) = isfile(f) ?
	[last(p) for p in sort([(something(tryparse(Int, first(split(l))), 0), String(rstrip(l)))
	                        for l in readlines(f) if !isempty(strip(l))]; by = first)] : String[]

# The launcher's log of this session (launchers since 2026-11-15 keep it when julia dies).
function _crash_launcher_log(started::Float64)
	logs = filter(f -> startswith(f, "igmt_julia_") && endswith(f, ".log"), readdir(tempdir()))
	paths = filter(p -> mtime(p) >= started, joinpath.(tempdir(), logs))
	isempty(paths) && return nothing
	return paths[argmax(mtime.(paths))]
end

function _crash_summary(sdir::String, fields, logtail::String)::Tuple{String,String}
	io = IOBuffer()
	stack = joinpath(sdir, "stack.txt")
	first_line = isfile(stack) ? readline(stack) : ""     # readline closes it; first(eachline()) does not
	title = "Crash: "
	if !isempty(first_line)
		println(io, "**", first_line, "**")
		title *= replace(first_line, r" \(thread \d+\)$" => "")
	elseif fields !== nothing
		d = Dict(fields)
		title *= string(get(d, "ExceptionCode", "hang"), " in ", get(d, "ModuleName", get(d, "AppName", "?")))
	else
		title *= "iGMT closed unexpectedly"
	end
	if fields !== nothing
		println(io, "\n### Windows crash record")
		for (k, v) in fields
			k in ("AppPath", "ModulePath", "AppName", "AppVersion", "AppTimeStamp", "ModuleName", "ModuleVersion",
			      "ModuleTimeStamp", "ExceptionCode", "FaultingOffset", "ProcessId", "EventID") && println(io, "- ", k, ": ", v)
		end
	end
	trail = _crash_trail_lines(joinpath(sdir, "trail.txt"))
	if !isempty(trail)
		println(io, "\n### Last actions")
		println(io, "```")
		foreach(l -> println(io, l), trail[max(1, end - 24):end])
		println(io, "```")
	end
	if isfile(stack)
		frames = filter(l -> startswith(l, "  #"), readlines(stack))
		println(io, "\n### Native stack (top)")
		println(io, "```")
		foreach(l -> println(io, l), frames[1:min(end, 20)])
		println(io, "```")
	end
	if !isempty(logtail)
		println(io, "\n### Julia log (end)")
		println(io, "```")
		println(io, logtail)
		println(io, "```")
	end
	info = joinpath(sdir, "info.txt")
	if isfile(info)
		println(io, "\n### System")
		println(io, "```")
		print(io, read(info, String))
		println(io, "```")
	end
	return _crash_scrub(title), _crash_scrub(String(take!(io)))
end

_crash_urlenc(s::AbstractString) = sprint() do io
	for b in codeunits(s)
		c = Char(b)
		(isascii(c) && (isletter(c) || isdigit(c) || c in "-_.~")) ? print(io, c) :
			print(io, '%', uppercase(string(b; base = 16, pad = 2)))
	end
end

# A mailto: link with subject and text filled in. Mail programs refuse long ones (Outlook near 2000
# characters), so the text is cut until the whole link fits; the full text is in the attached file.
# The attach reminder comes FIRST, so no cut can lose it.
function _crash_mailto(title::String, body::String, zipname::String)::String
	head = "Please attach the file $zipname (iGMT opened the folder that holds it).\n" *
	       "What were you doing when iGMT closed? (optional)\n\n\n---\n"
	base = "mailto:$_CRASH_EMAIL?subject=" * _crash_urlenc("iGMT " * title) * "&body="
	b = body
	while true
		u = base * _crash_urlenc(head * b)
		(length(u) <= 1800 || isempty(b)) && return u
		b = first(b, max(0, length(b) - 200)) * "\n...(cut: the full text is in the attached file)"
		length(b) < 250 && (b = "")
	end
end

# Turn the dead session folder into igmt_crash_<time>.zip and offer to report it.
function _crash_report(sdir::String, event)
	started = isfile(joinpath(sdir, "info.txt")) ? mtime(joinpath(sdir, "info.txt")) : 0.0
	fields  = event === nothing ? nothing : _crash_event_fields(event)
	event === nothing || write(joinpath(sdir, "eventlog.xml"), event)
	logtail = ""
	lf = _crash_launcher_log(started)
	if lf !== nothing
		txt = read(lf, String)
		write(joinpath(sdir, "julia_log.txt"), txt[max(1, prevind(txt, max(1, ncodeunits(txt) - 400_000))):end])
		lines = split(txt, '\n')
		k = findlast(l -> occursin("Please submit a bug report", l) || occursin("Exception:", l) ||
		                  occursin("signal (", l) || startswith(l, "ERROR:"), lines)
		logtail = join(lines[(k === nothing ? max(1, length(lines) - 15) : k):min(end, (k === nothing ? length(lines) : k + 30))], '\n')
	end
	for f in ("info.txt", "trail.txt", "stack.txt", "julia_log.txt", "eventlog.xml")      # may be posted publicly
		p = joinpath(sdir, f)
		isfile(p) && write(p, _crash_scrub(read(p, String)))
	end
	title, body = _crash_summary(sdir, fields, logtail)
	root  = dirname(sdir)
	stamp = GMT.Dates.format(GMT.Dates.unix2datetime(started > 0 ? started : time()), "yyyymmdd-HHMMSS")
	zip   = joinpath(root, "igmt_crash_$stamp.zip")
	tar   = joinpath(get(ENV, "SystemRoot", "C:\\Windows"), "System32", "tar.exe")
	run(`$tar -a -cf $zip -C $sdir .`)
	rm(sdir; recursive = true, force = true)
	olds = sort(filter(f -> startswith(f, "igmt_crash_") && endswith(f, ".zip"), readdir(root)))
	foreach(f -> rm(joinpath(root, f); force = true), olds[1:max(0, length(olds) - 10)])   # keep the last 10
	_crash_has(:gmtvtk_crash_report_dialog) || return nothing
	url = _crash_mailto(title, body, basename(zip))
	ccall(_fn(:gmtvtk_crash_report_dialog), Cint, (Cstring, Cstring, Cstring, Cstring), body, zip, _CRASH_EMAIL, url)
	return nothing
end

function _crash_check_previous()
	root = _crash_root()
	isdir(root) || return nothing
	events = nothing
	for d in readdir(root)
		m = match(r"^session_(\d+)$", d)
		m === nothing && continue
		pid = parse(Int, m[1])
		(pid == getpid() || _crash_pid_alive(pid)) && continue
		sdir = joinpath(root, d)
		events === nothing && (events = _crash_events())
		ev = _crash_event_for(events, pid)
		if !isfile(joinpath(sdir, "stack.txt")) && ev === nothing
			rm(sdir; recursive = true, force = true)     # no sign of a crash: Windows shut down, Task Manager
			continue
		end
		_crash_report(sdir, ev)
	end
	return nothing
end
