# Crash reports on users' machines (src/crashreport.jl, deps/src/crashreport.cpp): the event-log
# matching, the scrub, the trail, the mailto link, the dead-session sweep, and the real catcher --
# a child process crashes on purpose and must leave stack.txt + crash.dmp + its trail behind.

@testitem "crash report: event record found by pid, in any field order" tags=[:unit, :fast] begin
	CR = InteractiveGMT
	named = """<Event><System><EventID Qualifiers='0'>1000</EventID></System><EventData>""" *
	        """<Data Name='AppName'>julia.exe</Data><Data Name='ModuleName'>gmtvtk.dll</Data>""" *
	        """<Data Name='ExceptionCode'>c0000374</Data><Data Name='ProcessId'>0x1f40</Data></EventData></Event>"""
	plain = """<Event><System><EventID>1002</EventID></System><EventData><Data>julia.exe</Data>""" *
	        """<Data>1.14.0.0</Data><Data>0x2a</Data></EventData></Event>"""
	ev = [named, plain]
	@test CR._crash_event_for(ev, 8000) == named           # 8000 == 0x1f40
	@test CR._crash_event_for(ev, 42) == plain
	@test CR._crash_event_for(ev, 7) === nothing
	f = Dict(CR._crash_event_fields(named))
	@test f["ModuleName"] == "gmtvtk.dll" && f["ExceptionCode"] == "c0000374"
	@test f["EventID"] == "1000 (Application Error)"
	g = Dict(CR._crash_event_fields(plain))
	@test g["AppName"] == "julia.exe" && g["EventID"] == "1002 (Application Hang)"
end

@testitem "crash report: home paths scrubbed in every spelling" tags=[:unit, :fast] begin
	h = get(ENV, "USERPROFILE", homedir())
	s = InteractiveGMT._crash_scrub("a $(h)\\x b $(replace(h, '\\' => '/'))/y c $(lowercase(h))\\z")
	@test !occursin(h, s) && !occursin(replace(h, '\\' => '/'), s) && !occursin(lowercase(h), s)
	@test count("~", s) == 3
end

@testitem "crash report: trail read back in action order" tags=[:unit, :fast] begin
	f = tempname()
	write(f, "12 10:00:02.000 second\n" * " "^40 * "\n3 10:00:01.000 first\n" * " "^40 * "\n")
	@test InteractiveGMT._crash_trail_lines(f) == ["3 10:00:01.000 first", "12 10:00:02.000 second"]
	rm(f)
	@test InteractiveGMT._crash_trail_lines(tempname()) == String[]
end

@testitem "crash report: mailto fits a mail program and keeps the attach request" tags=[:unit, :fast] begin
	CR = InteractiveGMT
	u = CR._crash_mailto("Crash: X in y.dll", "line\n"^3000, "igmt_crash_1.zip")
	@test length(u) <= 1800
	@test startswith(u, "mailto:" * CR._CRASH_EMAIL * "?subject=iGMT%20Crash")
	@test occursin(CR._crash_urlenc("Please attach the file igmt_crash_1.zip"), u)
	@test occursin(CR._crash_urlenc("(cut: the full text is in the attached file)"), u)
	short = CR._crash_mailto("t", "short body", "z.zip")
	@test occursin(CR._crash_urlenc("short body"), short)
	@test CR._crash_urlenc("a b&c=é") == "a%20b%26c%3D%C3%A9"
end

@testitem "crash report: dead sessions -- crashed one packed, clean-ended one dropped, live one kept" tags=[:unit] begin
	# NOT `Sys.iswindows() || return`: a testitem's body runs statement by statement at top level, where
	# `return` ends only that statement and the rest still runs (Linux CI, 2026-11-27).
	if !Sys.iswindows()
		@test_skip "crash reports are Windows-only"
	else
		CR = InteractiveGMT
		prof = mktempdir()
		root = joinpath(prof, ".gmt", "igmt_crashes")
		# a julia.exe that stays alive: its session must not be touched
		live = run(`$(Base.julia_cmd()) --startup-file=no -e "sleep(120)"`; wait = false)
		livepid = getpid(live)
		events = CR._crash_events()
		deadpid = first(p for p in 4_000_000:4_001_000 if CR._crash_event_for(events, p) === nothing)
		crashed = joinpath(root, "session_$deadpid")
		gone    = joinpath(root, "session_$(deadpid + 4)")
		alive   = joinpath(root, "session_$livepid")
		for d in (crashed, gone, alive)
			mkpath(d)
			write(joinpath(d, "info.txt"), "started  test\nhome $(prof)\\x\n")
		end
		write(joinpath(crashed, "stack.txt"), "exception C0000005 ACCESS_VIOLATION in gmtvtk.dll+0x10 (thread 1)\n\n  #00 gmtvtk.dll+0x10\n")
		write(joinpath(crashed, "trail.txt"), "1 10:00:00.000 click QPushButton 'Compute' [Grid Calculator]\n")
		try
			withenv("USERPROFILE" => prof) do
				CR._crash_check_previous(; dialog = false)
			end
			zips = filter(f -> startswith(f, "igmt_crash_") && endswith(f, ".zip"), readdir(root))
			@test length(zips) == 1
			@test !isdir(crashed) && !isdir(gone) && isdir(alive)
			tar = joinpath(ENV["SystemRoot"], "System32", "tar.exe")
			@test sort(filter(!=("./"), readlines(`$tar -tf $(joinpath(root, zips[1]))`))) ==
			      ["./info.txt", "./stack.txt", "./trail.txt"]
		finally
			kill(live)
			rm(prof; recursive = true, force = true)
		end
	end
end

@testitem "crash report: a native crash leaves stack, dump and trail; a clean exit leaves nothing" tags=[:unit] begin
	if !Sys.iswindows()                     # see the item above: never `|| return` in a testitem
		@test_skip "crash reports are Windows-only"
	else
		CR = InteractiveGMT
		@test CR._crash_has(:gmtvtk_crash_install)
		dll = CR._LIB_USED[]
		dir = mktempdir()
		sess = joinpath(dir, "session")
		script = joinpath(dir, "crash.jl")
		write(script, """
			using Libdl
			h = dlopen($(repr(dll)))
			ccall(dlsym(h, :gmtvtk_crash_install), Cint, (Cstring,), $(repr(sess))) == 1 || exit(7)
			ccall(dlsym(h, :gmtvtk_crash_trail), Cvoid, (Cstring,), "click QPushButton 'Compute' [Grid Calculator]")
			ARGS[1] == "crash" && ccall((:strlen, "msvcrt"), Csize_t, (Ptr{UInt8},), C_NULL)
			""")
		path = join(unique([CR._VTK_BIN, CR._QT_BIN, dirname(dll), ENV["PATH"]]), ';')
		mkpath(sess)
		p = run(pipeline(ignorestatus(addenv(`$(Base.julia_cmd()) --startup-file=no $script crash`, "PATH" => path));
		                 stdout = devnull, stderr = devnull))
		@test p.exitcode != 0 && p.exitcode != 7
		st = joinpath(sess, "stack.txt")
		@test isfile(st)
		@test occursin("ACCESS_VIOLATION in msvcrt.dll", readline(st))
		@test any(l -> startswith(l, "  #00 msvcrt.dll+"), readlines(st))
		@test filesize(joinpath(sess, "crash.dmp")) > 10_000
		t = CR._crash_trail_lines(joinpath(sess, "trail.txt"))
		@test occursin("click QPushButton 'Compute' [Grid Calculator]", t[1])
		@test occursin("*** ACCESS_VIOLATION in msvcrt.dll", t[end])
		# no fault: nothing but the trail is written
		rm(sess; recursive = true)
		mkpath(sess)
		q = run(pipeline(ignorestatus(addenv(`$(Base.julia_cmd()) --startup-file=no $script ok`, "PATH" => path));
		                 stdout = devnull, stderr = devnull))
		@test q.exitcode == 0
		@test readdir(sess) == ["trail.txt"]
		rm(dir; recursive = true, force = true)
	end
end

@testitem "crash report: only the desktop app makes sessions" tags=[:unit, :fast] begin
	# the test run is not iview_app.jl: no session folder, no catcher, no dialog
	@test !InteractiveGMT._crash_app_mode()
	@test InteractiveGMT._CRASH_SESSION[] == ""
end
