# View > Workspace Panel (deps/src/77_workspace.cpp + src/workspace.jl): the Julia session's variables,
# shown as tables (THE shared table dialog) or pictures (the file-drop door).

@testitem "workspace: menu opens the panel; variables list, tabulate and show" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	pump(n = 10) = for _ in 1:n; sleep(0.05); end
	ERRB = zeros(UInt8, 4096)
	ask(h, req) = GC.@preserve ERRB req IG._on_workspace(h, Base.unsafe_convert(Cstring, req),
		Ptr{UInt8}(C_NULL), Cint(0), pointer(ERRB), Cint(length(ERRB)))
	# 20 columns x 10 rows, z = 100*row + col (row 1 = south), so orientation is checkable
	G = IG.GMT.mat2grid(Float32[100iy + ix for iy in 1:10, ix in 1:20], x = collect(1.0:20.0), y = collect(1.0:10.0))
	I = IG.GMT.mat2img(rand(UInt8, 12, 7, 3))
	Core.eval(Main, :(ws_G = $G; ws_I = $I; ws_M = [1.0 2.0; 3.0 4.0]; ws_x = 3.5))
	fig = IG.viz(G; title = "ZZwsgrid")
	fold() = ccall(_test_fn(:gmtvtk_workspace_fold_test), Cint, (Ptr{Cvoid},), fig.h)
	pump()
	# the test runner is a script host: its globals (made before the first window) are hidden...
	@test IG._ws_listing(true) isa String && !isempty(PROGRAM_FILE)
	push!(IG._WS_BASE, :ws_hostglobal)
	Core.eval(Main, :(ws_hostglobal = 1))
	@test !occursin("ws_hostglobal\t", IG._ws_listing(true))
	# ...but these were made on purpose by this test, so they must show
	setdiff!(IG._WS_BASE, (:ws_G, :ws_I, :ws_M, :ws_x))
	try
		# the status corner's Workspace button opens it (no View-menu row any more)
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), fig.h,
		            "Workspace Panel") == 0
		@test ccall(_test_fn(:gmtvtk_toplevel_widget_test), Cint, (Cstring, Cstring, Cint, Cstring),
		            "ZZwsgrid", "igWorkspaceBtn", 1, "") == 1
		@test fold() < 0               # it was open: this press folds it
		@test fold() > 200             # and this one opens it again
		# the button TOGGLES: a second click hides the panel, a third shows it again
		wsbtn() = ccall(_test_fn(:gmtvtk_toplevel_widget_test), Cint, (Cstring, Cstring, Cint, Cstring),
		                "ZZwsgrid", "igWorkspaceBtn", 1, "")
		@test wsbtn() == 1 && fold() == 0
		@test wsbtn() == 1 && fold() < 0
		fold()
		# ...and hides it while folded too
		fold(); @test wsbtn() == 1 && fold() == 0
		@test wsbtn() == 1 && fold() < 0
		fold()

		# ROOM: a window narrower than the screen GROWS for the panel instead of squeezing the display
		# area, and gives exactly that width back when the panel folds or hides
		widths() = (c = Ref{Cint}(0); w = Ref{Cint}(0);
			ccall(_test_fn(:gmtvtk_window_widths_test), Cint, (Ptr{Cvoid}, Ref{Cint}, Ref{Cint}), fig.h, c, w);
			(Int(c[]), Int(w[])))
		@test wsbtn() == 1 && fold() == 0                      # hidden
		@test ccall(_test_fn(:gmtvtk_toplevel_widget_test), Cint, (Cstring, Cstring, Cint, Cstring),
		            "ZZwsgrid", "", 3, "900,600") == 1
		pump()
		c0, w0 = widths()
		counts(start) = (r = Ref{Cint}(0); p = Ref{Cint}(0);
			ccall(_test_fn(:gmtvtk_paint_count_test), Cint, (Ptr{Cvoid}, Cint, Ref{Cint}, Ref{Cint}), fig.h, start, r, p);
			(Int(r[]), Int(p[])))
		counts(1)
		@test wsbtn() == 1; pump()                             # shown
		nres, npaint = counts(0)
		c1, w1 = widths()
		println("workspace room: display $c0 -> $c1, window $w0 -> $w1; display resized $nres x, painted $npaint x")
		@test nres == 0                                        # NO BLINK: the display never changes size
		@test w1 > w0 + 300 && abs(c1 - c0) <= 4
		counts(1)
		fold(); pump()                                         # folded: the window gives width back
		println("workspace fold: display resized $(counts(0)[1]) x")
		@test counts(0)[1] == 0
		c2, w2 = widths()
		@test w2 < w1 - 250 && abs(c2 - c0) <= 4
		counts(1)
		fold(); pump()                                         # open again: grows again
		println("workspace unfold: display resized $(counts(0)[1]) x")
		@test counts(0)[1] == 0
		c3, w3 = widths()
		@test abs(w3 - w1) <= 4 && abs(c3 - c0) <= 4
		@test wsbtn() == 1; pump()                             # hidden: back to the user's own size
		@test widths() == (c0, w0)
		@test wsbtn() == 1; pump()                             # and shown again for what follows
		pump()
		@test ccall(IG._fn(:gmtvtk_workspace_show_h), Cint, (Ptr{Cvoid},), fig.h) >= 4
		# the dock folds to a thin strip and opens back to its width, as Scene Objects does
		w1 = fold(); pump()
		@test w1 < 0 && -w1 < 60
		w2 = fold(); pump()
		@test w2 > 200
		# the menu opens a folded panel
		@test fold() < 0
		ccall(IG._fn(:gmtvtk_workspace_show_h), Cint, (Ptr{Cvoid},), fig.h)
		@test fold() < 0                 # it was open, so this press folds it again
		fold()
		L = IG._ws_listing(true)
		@test occursin("ws_G\t\t20×10\t", L) && occursin("\tgrid\t1\t", L)
		@test occursin("ws_I\t\t7×12×3\t", L) && occursin("\timage\t3\t", L)
		@test occursin("ws_x\t3.5\t", L)

		# a grid's table: north on top, x across the header, y in the first column
		M, hdr, _ = IG._table_matrix(G)
		@test size(M) == (10, 21) && hdr[2] == "1" && hdr[end] == "20"
		@test IG._table_heads([-179.98333333, -179.96666667]) == ["-179.983", "-179.967"]
		@test M[1, 1] == 10.0 && M[1, 2] == 1001.0 && M[end, end] == 120.0

		@test ask(fig.h, "table\tws_G") == 0
		pump()
		@test IG._scene_state(fig.h)["n_table"] == 10
		@test ask(fig.h, "table\tws_I\t2") == 0
		pump()
		@test IG._scene_state(fig.h)["n_table"] == 12
		@test ask(fig.h, "summary\tws_G") > 0 && startswith(IG._WS_OUT[], "A GMTgrid object")
		@test ask(fig.h, "here\tws_M") == 0
		# "Plot it": a vector and a dataset go to the X,Y tool
		Core.eval(Main, :(ws_v = [1.0, 4.0, 9.0]; ws_D = $(IG.GMT.mat2ds([1.0 2.0; 2.0 3.0; 3.0 5.0]))))
		for nm in ("ws_v", "ws_D")
			IG._XY_CURRENT[] = C_NULL
			@test ask(fig.h, "plot\t$nm") == 0
			xh = IG._XY_CURRENT[]
			@test xh != C_NULL && ccall(IG._fn(:gmtvtk_xyplot_is_alive), Cint, (Ptr{Cvoid},), xh) != 0
			xh != C_NULL && ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), xh)
			pump()
		end
		@test ask(fig.h, "table\tws_nope") == -1
		@test occursin("ws_nope", GC.@preserve ERRB unsafe_string(pointer(ERRB)))
		@test any(e -> occursin("ws_nope", e), IG._take_tool_errors!())   # claimed: it was meant to fail

		# a BIG grid: the read-only table is lazy, so 4 million cells open like 40
		Core.eval(Main, :(ws_B = $(IG.GMT.mat2grid(rand(Float32, 2000, 2000)))))
		t = @elapsed ask(fig.h, "table\tws_B")
		pump()
		@test IG._scene_state(fig.h)["n_table"] == 2000
		println("workspace: 2000x2000 grid table opened in ", round(t; digits=2), " s")
	finally
		Core.eval(Main, :(ws_G = ws_I = ws_M = ws_x = ws_B = ws_hostglobal = ws_v = ws_D = nothing))
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), fig.h)
		pump()
	end
end
