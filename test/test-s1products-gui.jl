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
		@test st["n_overlays"] == 4                              # coastline + region rectangle + ONE line per frame
		@test IG._RS_REGION_BOX[h] == "Region -10.0/-8.0/38.0/40.0"
		@test IG._s1_plot_footprints!(h, IG._S1_FOUND[h], -10.0, -8.0, 38.0, 40.0) == 2
		IG._pump_once()
		@test IG._scene_state(h)["n_overlays"] == 4              # a new search REPLACES the footprints and the rectangle
		lab = first(k for (k, v) in IG._S1_FRAMES[h] if v == [1, 3])
		@test startswith(lab, "Track 147 asc at 40.") && endswith(lab, "N (2 scenes)")   # centre 39.9 + (1.5+0+0.4+1.9)/4
		# Pointing at a footprint chooses that frame's newest scene in the list.
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=pickscene", "picked=$(lab)"]))
		@test ok == 1 && occursin("set:p_granule=0", r)
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=pickscene", "picked=coastline"]))
		@test ok == 0 && occursin("is not a Sentinel-1 footprint", r)
		@test any(e -> occursin("is not a Sentinel-1 footprint", e), IG._take_tool_errors!())
	finally
		delete!(IG._S1_FOUND, h);  delete!(IG._S1_FRAMES, h);  delete!(IG._S1_GROUP, h);  delete!(IG._RS_REGION_BOX, h)
		RemoteSGui.close_win(h)
	end
end

@testitem "Sentinel-1: the chosen scene's footprint is lit; Pick a scene takes a click inside a footprint" tags=[:gui, :s1] setup=[RemoteSGui, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	h = RemoteSGui.open_empty("s1 highlight")
	box = ["tool=s1", "p_rg_w=-10", "p_rg_e=-8", "p_rg_s=38", "p_rg_n=40", "p_product_idx=0"]
	ring(lat) = [-9.6 lat+1.5; -9.2 lat; -6.2 lat+0.4; -6.5 lat+1.9; -9.6 lat+1.5]
	mk(t, trk, orb, lat) = IG.S1Product("id$(t)", "S1C_IW_GRDH_1SDV_$(t)_x_x_x_x.SAFE", IG.DateTime(t, "yyyymmddTHHMMSS"),
	                                    1.7e9, orb, trk, [ring(lat)])
	pen(nm) = (o = zeros(6); ccall(IG._fn(:gmtvtk_overlay_style_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), h, nm, o) == 1 ? o : o)
	try
		IG._S1_FOUND[h] = [mk("20261013T182658", 147, "ASCENDING", 39.9), mk("20261007T064302", 52, "DESCENDING", 38.5),
		                   mk("20261001T182658", 147, "ASCENDING", 39.95)]
		@test IG._s1_plot_footprints!(h, IG._S1_FOUND[h], -10.0, -8.0, 38.0, 40.0) == 2
		IG._pump_once()
		lab1 = first(k for (k, v) in IG._S1_FRAMES[h] if v == [1, 3])
		lab2 = first(k for (k, v) in IG._S1_FRAMES[h] if v == [2])
		own = pen(lab1)
		# Choosing a scene in the list (the combo's refresh) lights ITS frame, and gives the other its pen back.
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=refresh", "p_granule_idx=0"]))
		@test ok == 1 && IG._S1_LIT[h] == lab1
		@test pen(lab1)[1:4] == [1.0, 0.0, 0.0, 3.0]
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=refresh", "p_granule_idx=1"]))
		@test ok == 1 && IG._S1_LIT[h] == lab2
		@test pen(lab2)[1:4] == [1.0, 0.0, 0.0, 3.0] && pen(lab1) == own
		# A point inside both frame-147 scenes: the first click takes one, the next click the other.
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=pickscene", "px=-8", "py=40.6", "p_granule_idx=1"]))
		@test ok == 1 && occursin("set:p_granule=0", r) && IG._S1_LIT[h] == lab1
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=pickscene", "px=-8", "py=40.6", "p_granule_idx=0"]))
		@test ok == 1 && occursin("set:p_granule=2", r)
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=pickscene", "px=-8", "py=37.0", "p_granule_idx=0"]))
		@test ok == 0 && occursin("click inside a footprint", r)
		IG._take_tool_errors!()
		# The real gesture: the dialog's own "Pick a scene" button, then a click on the view inside frame 52.
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), h, "Sentinel-1 SAR products") == 1
		for _ in 1:5; IG._pump_once(); end
		buf = zeros(UInt8, 128)
		@test ccall(_test_fn(:gmtvtk_dialog_button_test), Cint, (Cstring, Cstring, Cint, Ptr{UInt8}, Cint),
		            "Sentinel-1 SAR products", "pickpoint_pickscene", 1, buf, 128) == 1
		@test unsafe_string(pointer(buf)) == "Pick a scene"
		@test ccall(_test_fn(:gmtvtk_view_press_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble), h, -8.0, 38.9, 0.0) == 1
		for _ in 1:5; IG._pump_once(); end
		@test IG._S1_LIT[h] == lab2
	finally
		delete!(IG._S1_FOUND, h);  delete!(IG._S1_FRAMES, h);  delete!(IG._S1_GROUP, h);  delete!(IG._RS_REGION_BOX, h)
		delete!(IG._S1_PENS, h);  delete!(IG._S1_LIT, h)
		RemoteSGui.close_win(h)
	end
end

@testitem "Sentinel-1: Use the window's limits is locked unless the window is lon/lat or in a known CRS" tags=[:gui, :s1] setup=[RemoteSGui] begin
	IG = InteractiveGMT
	box = ["tool=s1", "p_rg_w=-10", "p_rg_e=-8", "p_rg_s=38", "p_rg_n=40", "p_product_idx=0"]
	# An empty window: nothing to take.
	h = RemoteSGui.open_empty("s1 lock")
	try
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=init"]))
		@test ok == 1 && occursin("enable:run_rgwindow=0", r)
	finally
		RemoteSGui.close_win(h)
	end
	# A grid in plain metres, no CRS: its limits are not longitudes and latitudes.
	G = IG.GMT.mat2grid(rand(Float32, 40, 50); x = collect(range(1000.0, 5000.0, length = 50)), y = collect(range(2000.0, 6000.0, length = 40)))
	f = viz(G)
	try
		ok, r = RemoteSGui.rs(f.h, vcat(box, ["what=init"]))
		@test ok == 1 && occursin("enable:run_rgwindow=0", r) && !occursin("set:p_rg_w=", r)
		ok, r = RemoteSGui.rs(f.h, vcat(box, ["what=rgwindow"]))
		@test ok == 0 && occursin("neither geographic nor in a known coordinate system", r)
		IG._take_tool_errors!()
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
	# A grid in UTM 29N: its limits come back as the lon/lat box that covers them.
	U = IG.GMT.mat2grid(rand(Float32, 40, 50); x = collect(range(500000.0, 600000.0, length = 50)),
	                    y = collect(range(4.40e6, 4.48e6, length = 40)), proj4 = "+proj=utm +zone=29 +datum=WGS84 +units=m +no_defs")
	f = viz(U)
	try
		ok, r = RemoteSGui.rs(f.h, vcat(box, ["what=refresh"]))
		@test ok == 1 && occursin("enable:run_rgwindow=1", r)
		ok, r = RemoteSGui.rs(f.h, vcat(box, ["what=rgwindow"]))
		@test ok == 1
		v(k) = parse(Float64, match(Regex("set:$(k)=([-0-9.]+)"), r)[1])
		@test -9.1 < v("p_rg_w") < -8.9 && -7.9 < v("p_rg_e") < -7.7      # 500..600 km E of the 9W meridian zone 29
		@test 39.6 < v("p_rg_s") < 39.8 && 40.4 < v("p_rg_n") < 40.6
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
	# A geographic window: its limits as they are.
	h = RemoteSGui.open_empty("s1 lock geog")
	try
		IG._swot_map_if_empty(h, -10.0, -8.0, 38.0, 40.0, "Sentinel-1 scenes")
		IG._pump_once()
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=refresh"]))
		@test ok == 1 && occursin("enable:run_rgwindow=1", r)
	finally
		RemoteSGui.close_win(h)
	end
end
