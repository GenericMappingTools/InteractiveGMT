# :gui scenarios for Satellite > SWOT (src/swot.jl, deps/ui/remotes_swot.ui). A REAL window through the
# built gmtvtk.dll, and the Remote sensing callback driven with the key=value block RemoteSDialog sends.
# Offline: the passes a search would have found are put in the dialog's list directly.

@testitem "SWOT: the menu entry builds its dialog" tags=[:gui, :swot] setup=[RemoteSGui, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = viz(IG.GMT.peaks())
	real = IG._RS_TOOLS["swot"]
	asked = String[]
	IG._RS_TOOLS["swot"] = (s, d, io) -> (push!(asked, get(d, "what", "")); true)
	try
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), f.h, "SWOT") == 1
		for _ in 1:5; IG._pump_once(); end
		@test "init" in asked                                   # built from remotes_swot.ui and talking
		@test ccall(_test_fn(:gmtvtk_window_owned_test), Cint, (Cstring,), "RemoteSSwot") == 1
	finally
		IG._RS_TOOLS["swot"] = real
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "SWOT: init fills the product list; swaths of the passes found are drawn" tags=[:gui, :swot] setup=[RemoteSGui] begin
	IG = InteractiveGMT
	h = RemoteSGui.open_empty("swot swaths")
	box = ["tool=swot", "p_rg_w=-10", "p_rg_e=-6", "p_rg_s=36", "p_rg_n=42", "p_product_idx=0"]
	try
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=init"]))
		@test ok == 1
		@test occursin("items:p_product=LR sea surface height, Basic (2 km)\t", r)
		@test occursin("items:p_var=ssha_karin\t", r)
		# What a search found (offline: the files a September search over this box returns), drawn by
		# the same function the search's own answer calls.
		IG._SWOT_FOUND[h] = [IG._swot_parse(n, 1) for n in (
			"SWOT_L2_LR_SSH_Basic_055_432_20260905T074243_20260905T083312_PID0_01",
			"SWOT_L2_LR_SSH_Basic_055_447_20260905T203326_20260905T212454_PID0_01",
			"SWOT_L2_LR_SSH_Basic_056_432_20260926T042741_20260926T051815_PID0_01")]
		@test IG._swot_plot_swaths!(h, IG._SWOT_FOUND[h], -10.0, -6.0, 36.0, 42.0) == 2
		IG._pump_once()
		st = IG._scene_state(h)
		# An empty window got a geographic map round the region, plus coastline + ONE line per pass.
		@test st["has_surface"] == 1 && st["crs"] == 1
		@test st["n_overlays"] == 3
		@test st["x0"] <= -12 && st["x1"] >= -4 && st["y0"] <= 34 && st["y1"] >= 44
		# A new search REPLACES the previous swaths: still coastline + 2, not 2 more.
		@test IG._swot_plot_swaths!(h, IG._SWOT_FOUND[h], -10.0, -6.0, 36.0, 42.0) == 2
		IG._pump_once()
		@test IG._scene_state(h)["n_overlays"] == 3
		# Each pass line is a guide line of this dialog (its menu's pick item, no clamp/fill/names).
		@test ccall(IG._fn(:gmtvtk_overlay_set_tool_pick_h), Cint, (Ptr{Cvoid}, Cstring, Cstring, Cstring, Cstring),
		            h, "Pass 432 (cycles 55, 56)", "swot", "pickpass", "Show this pass (SWOT)") == 1
		# Pointing at a pass line selects that pass's oldest file in the Pass list.
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=pickpass", "picked=coastline"]))
		@test ok == 0 && occursin("is not a SWOT swath", r)
		@test any(e -> occursin("is not a SWOT swath", e), IG._take_tool_errors!())   # the refusal, claimed
	finally
		delete!(IG._SWOT_FOUND, h);  delete!(IG._SWOT_SWATHS, h)
		RemoteSGui.close_win(h)
	end
end

@testitem "SWOT: removing the shown grid brings the map back" tags=[:gui, :swot] setup=[RemoteSGui] begin
	IG = InteractiveGMT
	h = RemoteSGui.open_empty("swot remove")
	try
		# The user's sequence: the search's swaths on an empty window (a map is made: an IMAGE base) ->
		# a gridded pass shown (the derived-variable transition hides the map) -> that grid Removed.
		IG._SWOT_FOUND[h] = [IG._swot_parse("SWOT_L2_LR_SSH_Basic_055_432_20260905T074243_20260905T083312_PID0_01", 1)]
		@test IG._swot_plot_swaths!(h, IG._SWOT_FOUND[h], -14.0, -6.0, 36.0, 48.0) == 1
		G = IG.GMT.grdmath("-R-14/-6/36/48 -I0.1 X Y MUL =")
		@test IG._rs_deliver(h, G, "ssha_karin — SWOT test", "test")
		IG._pump_once()
		@test IG._scene_state(h)["surfvis"] == 0                  # the map is hidden under the grid
		@test ccall(IG._fn(:gmtvtk_remove_grid_h), Cint, (Ptr{Cvoid}, Cstring), h, "ssha_karin — SWOT test") == 1
		IG._pump_once()
		st = IG._scene_state(h)
		# ...and comes BACK with the grid gone — picture, frame and axes — not left hidden for good.
		@test st["surfvis"] == 1 && st["axes"] == 1
		@test isempty(st["extras"]) && st["n_overlays"] == 2      # coastline + the swath line still there
	finally
		delete!(IG._SWOT_FOUND, h);  delete!(IG._SWOT_SWATHS, h)
		RemoteSGui.close_win(h)
	end
end

@testitem "SWOT: a swath's menu item picks its pass (clicked with the mouse)" tags=[:gui, :swot] setup=[RemoteSGui, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	h = RemoteSGui.open_empty("swot menu pick")
	# The menu and the dialog it raises run in the test library's copy of the viewer: its Remote sensing
	# dialogs call Julia through its OWN callback slot, mirrored here like the others.
	fptr = @cfunction((s, p, o, n) -> Base.invokelatest(IG._on_remotes, s, p, o, n)::Cint,
	                  Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_test_fn(:gmtvtk_set_remotes_callback), Cvoid, (Ptr{Cvoid},), fptr)
	click(line) = ccall(_test_fn(:gmtvtk_line_menu_click_test), Cint, (Ptr{Cvoid}, Cstring, Cstring),
	                    h, line, "Show this pass (SWOT)")
	try
		IG._SWOT_FOUND[h] = [IG._swot_parse(n, 1) for n in (
			"SWOT_L2_LR_SSH_Basic_055_432_20260905T074243_20260905T083312_PID0_01",
			"SWOT_L2_LR_SSH_Basic_055_447_20260905T203326_20260905T212454_PID0_01",
			"SWOT_L2_LR_SSH_Basic_056_432_20260926T042741_20260926T051815_PID0_01")]
		@test IG._swot_plot_swaths!(h, IG._SWOT_FOUND[h], -10.0, -6.0, 36.0, 42.0) == 2
		IG._pump_once()
		# The pick request is recorded, not run: running it downloads the pass (network).
		picks = String[]
		real = IG._RS_TOOLS["swot"]
		IG._RS_TOOLS["swot"] = (s, d, io) -> get(d, "what", "") == "pickpass" ?
			(push!(picks, get(d, "picked", "")); true) : real(s, d, io)
		# No dialog open yet: the item opens it (visible) and sends ITS pass.
		@test click("Pass 447 (cycle 55)") == 1
		@test ccall(_test_fn(:gmtvtk_window_owned_test), Cint, (Cstring,), "RemoteSSwot") == 1   # visible
		@test picks == ["Pass 447 (cycle 55)"]
		# The dialog already open: the item on another swath sends that one.
		@test click("Pass 432 (cycles 55, 56)") == 1
		@test picks == ["Pass 447 (cycle 55)", "Pass 432 (cycles 55, 56)"]
		IG._RS_TOOLS["swot"] = real
	finally
		delete!(IG._SWOT_FOUND, h);  delete!(IG._SWOT_SWATHS, h)
		RemoteSGui.close_win(h)
	end
end
