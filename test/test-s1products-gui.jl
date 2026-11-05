# :gui scenarios for Satellite > Copernicus > Sentinel-1 SAR products (src/s1products.jl,
# deps/ui/remotes_s1.ui). A REAL window through the built gmtvtk.dll, and the Remote sensing callback
# driven with the key=value block RemoteSDialog sends. Offline: the scenes a search would have found are
# put in the dialog's list directly.

@testitem "Sentinel-1: the menu entry builds its dialog" tags=[:gui, :s1] setup=[RemoteSGui, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = viz(IG.GMT.peaks())
	real = IG._RS_TOOLS["s1"]
	asked = String[]
	IG._RS_TOOLS["s1"] = (s, d, io) -> (push!(asked, get(d, "what", "")); true)
	try
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), f.h, "Sentinel-1 SAR products") == 1
		for _ in 1:5; IG._pump_once(); end
		@test "init" in asked                                   # built from remotes_s1.ui and talking
		@test ccall(_test_fn(:gmtvtk_window_owned_test), Cint, (Cstring,), "RemoteSS1") == 1
	finally
		IG._RS_TOOLS["s1"] = real
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "Sentinel-1: init fills the lists; footprints of the scenes found are drawn and picked" tags=[:gui, :s1] setup=[RemoteSGui] begin
	IG = InteractiveGMT
	h = RemoteSGui.open_empty("s1 footprints")
	box = ["tool=s1", "p_rg_w=-10", "p_rg_e=-8", "p_rg_s=38", "p_rg_n=40", "p_product_idx=0"]
	ring(lat) = [-9.6 lat+1.5; -9.2 lat; -6.2 lat+0.4; -6.5 lat+1.9; -9.6 lat+1.5]
	mk(t, trk, orb, lat) = IG.S1Product("id$(t)", "S1C_IW_GRDH_1SDV_$(t)_x_x_x_x.SAFE", IG.DateTime(t, "yyyymmddTHHMMSS"),
	                                    1.7e9, orb, trk, [ring(lat)])
	try
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=init"]))
		@test ok == 1
		@test occursin("items:p_product=GRD: ground range, detected (Level-1)\t", r)
		@test occursin("enable:gb_show=1", r)
		ok, r = RemoteSGui.rs(h, vcat(box[1:end-1], ["p_product_idx=2", "what=refresh"]))
		@test ok == 1 && occursin("enable:gb_show=0", r)        # an SLC cannot be shown here
		# Three scenes: two dates of one frame (track 147 asc), one of another track.
		IG._S1_FOUND[h] = [mk("20261013T182658", 147, "ASCENDING", 39.9), mk("20261007T064302", 52, "DESCENDING", 38.5),
		                   mk("20261001T182658", 147, "ASCENDING", 39.95)]
		@test IG._s1_plot_footprints!(h, IG._S1_FOUND[h], -10.0, -8.0, 38.0, 40.0) == 2
		IG._pump_once()
		st = IG._scene_state(h)
		@test st["has_surface"] == 1 && st["crs"] == 1           # an empty window got a map round the region
		@test st["n_overlays"] == 3                              # coastline + ONE line per frame
		@test IG._s1_plot_footprints!(h, IG._S1_FOUND[h], -10.0, -8.0, 38.0, 40.0) == 2
		IG._pump_once()
		@test IG._scene_state(h)["n_overlays"] == 3              # a new search REPLACES the footprints
		lab = first(k for (k, v) in IG._S1_FRAMES[h] if v == [1, 3])
		@test startswith(lab, "Track 147 asc at 40.") && endswith(lab, "N (2 scenes)")   # centre 39.9 + (1.5+0+0.4+1.9)/4
		# Pointing at a footprint chooses that frame's newest scene in the list.
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=pickscene", "picked=$(lab)"]))
		@test ok == 1 && occursin("set:p_granule=0", r)
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=pickscene", "picked=coastline"]))
		@test ok == 0 && occursin("is not a Sentinel-1 footprint", r)
		@test any(e -> occursin("is not a Sentinel-1 footprint", e), IG._take_tool_errors!())
	finally
		delete!(IG._S1_FOUND, h);  delete!(IG._S1_FRAMES, h);  delete!(IG._S1_GROUP, h)
		RemoteSGui.close_win(h)
	end
end
