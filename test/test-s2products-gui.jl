# :gui scenarios for Satellite > Copernicus > Sentinel-2 MSI products (src/s2products.jl,
# deps/ui/remotes_s2.ui). A REAL window through the built gmtvtk.dll, and the Remote sensing callback
# driven with the key=value block RemoteSDialog sends. Offline: the scenes a search would have found are
# put in the dialog's list directly, with their band trees cached.

@testitem "Sentinel-2: the menu entry builds its dialog" tags=[:gui, :s2] setup=[RemoteSGui, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = viz(IG.GMT.peaks())
	real = IG._RS_TOOLS["s2"]
	asked = String[]
	IG._RS_TOOLS["s2"] = (s, d, io) -> (push!(asked, get(d, "what", "")); true)
	try
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), f.h, "Sentinel-2 MSI products") == 1
		for _ in 1:5; IG._pump_once(); end
		@test "init" in asked                                   # built from remotes_s2.ui and talking
		@test ccall(_test_fn(:gmtvtk_window_owned_test), Cint, (Cstring,), "RemoteSS2") == 1
	finally
		IG._RS_TOOLS["s2"] = real
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "Sentinel-2: footprints per tile and orbit are drawn, lit and picked; the pane follows" tags=[:gui, :s2] setup=[RemoteSGui, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	h = RemoteSGui.open_empty("s2 footprints")
	dir = mktempdir()
	box = ["tool=s2", "p_rg_w=-8.5", "p_rg_e=-7.5", "p_rg_s=37", "p_rg_n=37.5", "p_product_idx=0", "p_bres_idx=0",
	       "p_b_b04=1", "p_folder=$(dir)"]
	ring(x) = [x 36.0; x+1.2 36.0; x+1.2 37.0; x 37.0; x 36.0]
	mk(t, tile, orb, x) = IG.S2Scene(IG.S1Product("id$(t)$(tile)", "S2C_MSIL2A_$(t)_N0513_R$(orb)_T$(tile)_$(t).SAFE",
	                                              IG.DateTime(t, "yyyymmddTHHMMSS"), 4e8, "", 0, [ring(x)]),
	                                 10.0, "", tile, parse(Int, orb))
	pen(nm) = (o = zeros(6); ccall(IG._fn(:gmtvtk_overlay_style_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), h, nm, o); o)
	try
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=init"]))
		@test ok == 1
		@test occursin("items:p_product=L2A: surface reflectance (bottom of atmosphere)\t", r)
		@test occursin("items:p_bres=10 m\t20 m\t60 m", r) && occursin("enable:run_bands=0", r)
		# Three scenes: two dates of tile 29SPA orbit 037, one of 29SNA.
		found = [mk("20261114T112321", "29SPA", "037", -8.0), mk("20261112T111321", "29SNA", "137", -9.0),
		         mk("20261104T112321", "29SPA", "037", -8.0)]
		for s in found
			IG._S2_NODES[s.p.id] = ("g", [("R10m", "T$(s.tile)_x_B04_10m.jp2", 4.5e7)])
		end
		IG._S2_FOUND[h] = found
		@test IG._s2_plot_footprints!(h, found, -8.5, -7.5, 37.0, 37.5) == 3
		IG._pump_once()
		st = IG._scene_state(h)
		@test st["has_surface"] == 1 && st["crs"] == 1           # an empty window got a map round the region
		@test st["n_overlays"] == 5                              # coastline + region rectangle + ONE line per scene
		lab = first(k for (k, v) in IG._S2_FRAMES[h] if v == [1])
		@test lab == IG._s2_base(found[1])                       # each scene its own footprint, its own name
		@test IG._s2_plot_footprints!(h, found, -8.5, -7.5, 37.0, 37.5) == 3
		IG._pump_once()
		@test IG._scene_state(h)["n_overlays"] == 5              # a new search REPLACES the footprints
		# Choosing a scene in the list lights its footprint, fills the band panel and the pane.
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=refresh", "p_granule_idx=1"]))
		@test ok == 1 && IG._S2_LIT[h] == IG._s2_base(found[2])
		@test pen(IG._S2_LIT[h])[1:4] == [1.0, 0.0, 0.0, 6.0]
		@test occursin("enable:p_b_b04=1", r) && occursin("label:lbl_bsize=1 at 10 m: whole tile, 10980x10980 px", r)
		@test occursin("label:lbl_thumbinfo=2026-11-12 11:13  S2C  tile 29SNA  orbit R137", r)
		# Pointing at a footprint chooses its newest scene; a click inside both steps between them.
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=pickscene", "picked=$(lab)"]))
		@test ok == 1 && occursin("set:p_granule=0", r) && IG._S2_LIT[h] == lab
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=clickscene", "px=-7.5", "py=36.5", "p_granule_idx=0"]))
		@test ok == 1 && occursin("set:p_granule=2", r)
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=clickscene", "px=-7.5", "py=40.0", "p_granule_idx=0"]))
		@test ok == 1 && isempty(strip(r))                      # outside every footprint: an ordinary click
		ok, r = RemoteSGui.rs(h, vcat(box, ["what=pickscene", "picked=coastline"]))
		@test ok == 0 && occursin("is not a Sentinel-2 footprint", r)
		IG._take_tool_errors!()
		# The real gesture: the dialog open, a plain left click in the view inside the 29SNA footprint
		# chooses that scene, with no button pressed first.
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), h, "Sentinel-2 MSI products") == 1
		for _ in 1:5; IG._pump_once(); end
		@test ccall(_test_fn(:gmtvtk_view_click_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble), h, -8.7, 36.5, 0.0) == 1
		for _ in 1:5; IG._pump_once(); end
		@test IG._S2_LIT[h] == IG._s2_base(found[2])
		# Another level makes the list stale: it is emptied and the footprints go.
		ok, r = RemoteSGui.rs(h, vcat(box[1:5], ["p_product_idx=1", "what=refresh", "p_granule_idx=0"]))
		@test ok == 1 && occursin("Product changed", r) && !haskey(IG._S2_FOUND, h)
		IG._pump_once()
		@test IG._scene_state(h)["n_overlays"] == 2
	finally
		for k in collect(keys(IG._S2_NODES)); startswith(k, "id2026") && delete!(IG._S2_NODES, k); end
		delete!(IG._S2_FOUND, h);  delete!(IG._S2_FRAMES, h);  delete!(IG._S2_GROUP, h);  delete!(IG._RS_REGION_BOX, h)
		delete!(IG._S2_PENS, h);  delete!(IG._S2_LIT, h)
		RemoteSGui.close_win(h)
	end
end
