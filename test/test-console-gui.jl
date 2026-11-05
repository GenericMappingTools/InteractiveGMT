# The in-window Julia console as the user drives it: ONE pane (ReplConsole, 30_app.cpp) holding the
# prompt, what was typed and what it printed, like a terminal REPL. Every keystroke below is a real
# QKeyEvent sent to the widget (gmtvtk_console_history_test), so the prompt, the continuation lines
# and the history walk are proved by the widget's own key handling.

@testitem "console: one REPL pane — runs, continues an open block, recalls history" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT; tf = GmtvtkTest._test_fn
	f = viz(GMT.mat2grid(Float32[x + y for y in 1:20, x in 1:30]))
	buf = zeros(UInt8, 1 << 16)
	# type `cmd` ('\n' = Enter) and press Enter, then Up/Down; returns the current input,
	# or the whole pane with ups = -1
	function con(cmd, ups = 0, downs = 0)
		r = ccall(tf(:gmtvtk_console_history_test), Cint, (Ptr{Cvoid}, Cstring, Cint, Cint, Ptr{UInt8}, Cint),
		          f.h, cmd, ups, downs, buf, length(buf))
		r == 1 || error("no Julia console in the window")
		return unsafe_string(pointer(buf))
	end
	pane() = con("", -1)
	# the console saves into the Julia REPL's own history file
	@test IG._repl_history_file() == get(ENV, "JULIA_HISTORY", joinpath(first(DEPOT_PATH), "logs", "repl_history.jl"))
	# here a temp file stands in for it, never the user's own; it already holds a julia-mode command
	# from a "previous session" and, newer, a shell-mode one that the console must not recall
	hfile = joinpath(tempdir(), "igmt_console_hist_$(getpid()).jl")
	write(hfile, "# time: 2026-01-01 00:00:00 GMT\n# mode: julia\n\tprev = \"from last session\"\n" *
	             "# time: 2026-01-01 00:00:01 GMT\n# mode: shell\n\tls -l\n")
	sethist(p) = ccall(IG._fn(:gmtvtk_set_console_history_path), Cvoid, (Cstring,), p)
	sethist(hfile)
	try
		IG._pump_once()
		# a command from the previous session is there before anything is typed
		@test con("", 1) == "prev = \"from last session\""
		@test con("", 0, 1) == ""
		# a complete line runs, and its value lands under it in the SAME pane, then a fresh prompt
		@test con("6 * 7") == ""
		@test occursin("julia> 6 * 7\n42\njulia> ", pane())
		# an open block does NOT run: Enter gives an indented continuation line instead
		@test con("for i in 1:2") == "for i in 1:2\n"
		@test !occursin("row1", pane())
		# ...the lines typed into it, and the `end` that closes it, run it as ONE input
		con("println(\"row\", i)\nend")
		@test occursin("julia> for i in 1:2\n       println(\"row\", i)\n       end\nrow1\nrow2\njulia> ", pane())
		# a syntax error is complete (running it shows the message), and an error is shown inline
		con("error(\"boom-console\")")
		@test occursin("boom-console", pane())
		# Up walks back through what RAN, a multi-line input as one entry; Down comes back to the draft
		@test con("", 1) == "error(\"boom-console\")"
		@test con("", 1) == "for i in 1:2\nprintln(\"row\", i)\nend"
		@test con("", 0, 2) == ""
		# what ran is SAVED as it ran, a multi-line command as one entry...
		saved = read(hfile, String)
		@test occursin("\t6 * 7\n", saved)
		@test occursin("\tfor i in 1:2\n\tprintln(\"row\", i)\n\tend\n", saved)
		# ...and a new session (the file read afresh) recalls it, newest first
		sethist(hfile)
		@test con("", 1) == "error(\"boom-console\")"
		@test con("", 1) == "for i in 1:2\nprintln(\"row\", i)\nend"
		@test con("", 0, 2) == ""
	finally
		sethist(IG._repl_history_file())
		rm(hfile; force = true)
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "console: ']' opens pkg>, '?' opens help?>, Backspace goes back" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT; tf = GmtvtkTest._test_fn
	f = viz(GMT.mat2grid(Float32[x + y for y in 1:20, x in 1:30]))
	buf = zeros(UInt8, 1 << 16)
	function con(cmd, ups = 0, downs = 0)
		r = ccall(tf(:gmtvtk_console_history_test), Cint, (Ptr{Cvoid}, Cstring, Cint, Cint, Ptr{UInt8}, Cint),
		          f.h, cmd, ups, downs, buf, length(buf))
		r == 1 || error("no Julia console in the window")
		return unsafe_string(pointer(buf))
	end
	pane() = con("", -1)
	hfile = joinpath(tempdir(), "igmt_console_modes_$(getpid()).jl")
	write(hfile, "")
	sethist(p) = ccall(IG._fn(:gmtvtk_set_console_history_path), Cvoid, (Cstring,), p)
	sethist(hfile)
	try
		IG._pump_once()
		# '?' typed on an empty "julia> " is the help prompt, not a character: the query runs as help
		con("?sin")
		@test occursin("help?> sin\n", pane())
		@test occursin("Compute sine", pane())
		@test endswith(pane(), "help?> ")                # the mode stays, as in the REPL
		# Backspace on the empty help prompt goes back to julia>
		con("\b")
		@test endswith(pane(), "julia> ")
		# ']' is the Pkg prompt; its command runs through Pkg
		con("]st")
		@test occursin("pkg> st\n", pane())
		@test occursin("Status", pane())
		@test endswith(pane(), "pkg> ")
		con("\b")
		@test endswith(pane(), "julia> ")
		# history: each entry comes back in the prompt it ran in...
		@test con("", 1) == "st"
		@test endswith(pane(), "pkg> st")
		@test con("", 1) == "sin"
		@test endswith(pane(), "help?> sin")
		@test con("", 0, 2) == ""
		@test endswith(pane(), "julia> ")
		# ...and is saved with the REPL's own mode, without the mark
		saved = read(hfile, String)
		@test occursin("# mode: help\n\tsin\n", saved)
		@test occursin("# mode: pkg\n\tst\n", saved)
	finally
		sethist(IG._repl_history_file())
		rm(hfile; force = true)
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end
