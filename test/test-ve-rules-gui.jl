# THE VE / ILLUMINATION / PER-GRID-UNIQUENESS REGRESSION SUITE.
#
# Every item here exists because the behaviour it asserts was BROKEN on 2026-09-09, found by the
# user, and cost an afternoon. They are written against what the window actually reports
# (gmtvtk_scene_state / _full) and, for illumination, against the PIXELS it renders -- never against
# the shape of the code, so a future rewrite that keeps the behaviour keeps these green.
#
# THE THREE LAWS UNDER TEST
#
#  1. VE IS CALCULATED UNIQUELY FROM THE PLOTTED / DISPLAYED DIMENSIONS.
#        ve = (relief's displayed height) / (kVEReference x map's displayed horizontal size)
#     so the drawn height of a z value is  z * zfac * ve  with
#        zfac = kVEReference * (displayed horizontal size) / (that layer's own z range)
#     kVEReference = 0.1. FORBIDDEN and separately asserted below: any unit reconciliation
#     (the old `1/111111` metres->degrees), and any VE VALUE computed from a grid (the `openingVE`
#     that briefly existed). `ve` opens at exactly 1, always.
#
#  2. EVERY GRID CARRIES ITS OWN PARAMETERS AND NOTHING CROSSES. Its own ve, its own axes box, its
#     own shading set. Touching one layer must leave every other layer's numbers and drawn geometry
#     bit-identical -- including through a session save/restore, which must NOT fall back to another
#     layer's ve for a layer that has none of its own.
#
#  3. THE DEFAULT VIEW IS ILLUMINATED. At ve = 1 a grid stands a tenth of the map's drawn width
#     tall, so the 3-D view has relief and the relief is lit. The regression this guards is a flat
#     plate: geometry drawn at true scale, normals all vertical, N.L constant, no shading at all.
#
# WHAT IS MEASURED, AND WHY IT IS TRUSTWORTHY
#   * `zfac`, `ve`, `ve_<tag>`, `lk_<tag>` come from gmtvtk_scene_state_full -- the same record Save
#     Session writes, so these tests also cover the session format.
#   * `axZ0`/`axZ1` (gmtvtk_scene_state) is the ACTIVE raster's axes box in WORLD units: the drawn
#     height of that layer after every scale the viewer applies. It is the only host-visible number
#     that reflects real drawn geometry, which is exactly what a VE bug corrupts.
#   * `gizve` is what the VE handle's label shows. The handle sitting on a stale layer's number was
#     its own bug, so it is asserted rather than eyeballed in a screenshot.
#   * For illumination the PNG is rendered and its luminance spread measured. A flat plate has
#     essentially none; lit relief has plenty. No code path is consulted at all.

@testitem "VE law 1: zfac is displayed-dimension only, and ve opens at 1" tags=[:gui, :ve] begin
	IG = InteractiveGMT; GMT = IG.GMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	# Same footprint, z spans 1000x apart: `zfac` must track the SPAN (it is the axis mapping) while
	# the VE stays the constant 1 -- nothing may compute an opening exaggeration from the data.
	for zspan in (2.0, 500.0, 20_000.0)
		G = ve_grid(zspan = zspan)
		f = view_grid(G, geographic=true)
		try
			ve_pump()
			st = IG._parse_scene_state(IG._scene_state_full_raw(f.h))
			zfac = parse(Float64, string(st["zfac"]))
			@test parse(Float64, string(st["ve"])) == 1.0            # NO computed opening VE. Ever.
			@test isapprox(zfac, ve_zfac_expected(G), rtol=1e-9)     # 0.1 * H_displayed / zspan
			# ...and the OLD unit reconciliation is gone: 1/111111 would be this, and must not be.
			@test !isapprox(zfac, 1/111111, rtol=1e-6)
		finally
			ve_close(f.h)
		end
	end
end

@testitem "VE law 1: the same grid declared Cartesian gets the same displayed-dimension rule" tags=[:gui, :ve] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	G = ve_grid(zspan = 500.0)
	for geog in (true, false)
		f = view_grid(G, geographic=geog)
		try
			ve_pump()
			st = IG._parse_scene_state(IG._scene_state_full_raw(f.h))
			# Cartesian differs ONLY through xfac (cos(midlat) vs 1) inside the displayed horizontal
			# size -- never through a unit assumption about z.
			@test isapprox(parse(Float64, string(st["zfac"])), ve_zfac_expected(G, geog), rtol=1e-9)
			@test parse(Float64, string(st["ve"])) == 1.0
		finally
			ve_close(f.h)
		end
	end
end

@testitem "VE law 2: each layer drawn by ITS OWN mapping and ITS OWN ve" tags=[:gui, :ve] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	Gb = ve_grid(zspan = 8000.0)          # base   (a bathymetry-like span)
	Gi = ve_grid(zspan = 5.0)             # dropped (an Okada-like span, 1600x smaller)
	f = view_grid(Gb, geographic=true)
	try
		ve_pump(); h = f.h
		ve_add_layer(h, Gi, "layerB")
		# Active layer is the new one: its drawn height is ITS span x ITS mapping x ITS ve.
		@test ve_active_drawn_span(h) ≈ ve_drawn_span_expected(Gi, 1.0) rtol=1e-6
		ve_set_active(h, 7.0)                                   # the gizmo/dialog door (activeVEPtr)
		st = IG._parse_scene_state(IG._scene_state_full_raw(h))
		@test parse(Float64, string(st["ve_1"])) == 7.0
		@test parse(Float64, string(st["ve"]))   == 1.0         # the base did NOT move
		@test ve_active_drawn_span(h) ≈ ve_drawn_span_expected(Gi, 7.0) rtol=1e-6
		# ...and the base, made active again, is still drawn at its own ve = 1.
		ve_show(h, "layerB", false); ve_show(h, "", true)
		@test ve_active_drawn_span(h) ≈ ve_drawn_span_expected(Gb, 1.0) rtol=1e-6
		ve_set_active(h, 3.0)
		st = IG._parse_scene_state(IG._scene_state_full_raw(h))
		@test parse(Float64, string(st["ve"]))   == 3.0
		@test parse(Float64, string(st["ve_1"])) == 7.0         # the dropped layer kept ITS OWN 7
		@test ve_active_drawn_span(h) ≈ ve_drawn_span_expected(Gb, 3.0) rtol=1e-6
		# ...and back on the dropped layer: still its own 7, geometry included.
		ve_show(h, "layerB", true)
		@test ve_active_drawn_span(h) ≈ ve_drawn_span_expected(Gi, 7.0) rtol=1e-6
	finally
		ve_close(f.h)
	end
end

@testitem "VE law 2: a layer's ve reaches NOTHING else, over many layers" tags=[:gui, :ve] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	f = view_grid(ve_grid(zspan = 8000.0), geographic=true)
	try
		ve_pump(); h = f.h
		ve_add_layer(h, ve_grid(zspan = 5.0),   "L1")
		ve_add_layer(h, ve_grid(zspan = 300.0), "L2")
		ve_add_layer(h, ve_grid(zspan = 40.0),  "L3")
		before = ve_all_ves(h)
		ve_set_active(h, 11.0)                       # L3 is the active (topmost visible) one
		after = ve_all_ves(h)
		moved = [k for k in keys(after) if get(before, k, nothing) != after[k]]
		@test length(moved) == 1                     # EXACTLY ONE number in the whole window changed
		@test after[only(moved)] == 11.0
		@test after["ve"] == 1.0                     # base untouched
	finally
		ve_close(f.h)
	end
end

@testitem "VE law 2: no ve fallback across layers through a session" tags=[:gui, :ve, :session] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	f = view_grid(ve_grid(zspan = 8000.0), geographic=true)
	try
		ve_pump(); h = f.h
		ve_add_layer(h, ve_grid(zspan = 5.0), "L1")
		ve_show(h, "L1", false); ve_show(h, "", true)
		ve_set_active(h, 4.0)                        # base -> 4, L1 keeps its own 1
		st = IG._parse_scene_state(IG._scene_state_full_raw(h))
		@test parse(Float64, string(st["ve"]))   == 4.0
		@test parse(Float64, string(st["ve_1"])) == 1.0
		# A session that carries a base `ve` and NO ve_<tag> for a layer must leave that layer at its
		# own default. Inheriting the base's number was the exact cross-layer leak found in the audit.
		ccall(IG._fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), h, "ve=9;")
		ve_pump()
		st = IG._parse_scene_state(IG._scene_state_full_raw(h))
		@test parse(Float64, string(st["ve"]))   == 9.0
		@test parse(Float64, string(st["ve_1"])) == 1.0        # NOT 9
	finally
		ve_close(f.h)
	end
end

@testitem "VE law 2: the handle always states the ACTIVE layer's own ve" tags=[:gui, :ve, :gizmo] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	f = view_grid(ve_grid(zspan = 8000.0), geographic=true)
	try
		ve_pump(); h = f.h
		ccall(IG._fn(:gmtvtk_set_view_mode_h), Cint, (Ptr{Cvoid}, Cint), h, Cint(0))   # 3-D
		ve_pump()
		ve_add_layer(h, ve_grid(zspan = 5.0), "L1")
		ve_set_active(h, 6.0)
		@test ve_gizmo_shows(h) == 6.0                 # the active layer's own
		# SELECTION: the base becomes active -> the handle must re-read, with no drag involved.
		ve_show(h, "L1", false); ve_show(h, "", true)
		@test ve_gizmo_shows(h) == 1.0
		ve_set_active(h, 3.0)
		@test ve_gizmo_shows(h) == 3.0
		# SWAP: adopting a new layer changes the ACTIVE layer without touching a visibility flag --
		# the case the visibility hooks alone missed, and the reason the sync lives in the render path.
		ve_add_layer(h, ve_grid(zspan = 120.0), "L2")
		@test ve_gizmo_shows(h) == 1.0                 # the newcomer's own default
		ve_show(h, "L2", false)
		@test ve_gizmo_shows(h) == 3.0                 # back to the base's own 3
	finally
		ve_close(f.h)
	end
end

@testitem "VE law 2: each layer's SHADING set is its own" tags=[:gui, :ve, :shading] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	f = view_grid(ve_grid(zspan = 8000.0), geographic=true)
	try
		ve_pump(); h = f.h
		ve_add_layer(h, ve_grid(zspan = 300.0), "L1")     # active
		# The look keys (`look`, `sunaz`, ...) and the per-layer `lk_<tag>` records are in the FULL
		# state record -- the same one Save Session writes -- not in the short one.
		base0 = ve_state_full(h)
		# THE ACTIVE LAYER'S DOOR: sceneSetReliefLook writes activeLookPtr, i.e. whichever layer is
		# selected -- the same door the Shading dock uses. (NOT the session keys `sunaz`/`sunel`: those
		# ARE the base relief's own fields by definition of the save format, so writing them through
		# apply_scene_state moves the base and proves nothing about layer independence. Using them here
		# is what made the first run of this item fail, correctly.)
		ccall(IG._fn(:gmtvtk_set_relief_look_h), Cvoid, (Ptr{Cvoid}, Cint, Cint), h, Cint(3), Cint(0))
		ve_pump()
		st = ve_state_full(h)
		# The dropped layer carries a shading record OF ITS OWN...
		@test haskey(st, "lk_1")
		@test parse(Int, split(string(st["lk_1"]), ',')[1]) == 3      # ...on the grdimage hillshade
		# ...and the BASE's own shading keys did not move with it.
		for k in ("look", "noshade", "sunaz", "sunel", "hillgain", "hillamb")
			haskey(base0, k) && @test st[k] == base0[k]
		end
		# The per-layer SESSION record is per layer too: writing layer 1's own lk_ record must leave the
		# base's sun where it was (this is the save/restore half of the same law).
		ccall(IG._fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), h,
		      ";lk_1=3,0,90,20,2,0.25,0.3,0;")
		ve_pump()
		st2 = ve_state_full(h)
		@test isapprox(parse(Float64, split(string(st2["lk_1"]), ',')[3]), 90.0, atol=1e-9)  # ITS sun
		@test st2["sunaz"] == base0["sunaz"]                          # the base's, untouched
		@test st2["sunel"] == base0["sunel"]
	finally
		ve_close(f.h)
	end
end

@testitem "ILLUMINATION law 3: the default 3-D view is relief, not a flat plate" tags=[:gui, :ve, :illum] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	G = ve_grid(zspan = 8000.0)
	f = view_grid(G, geographic=true)
	try
		ve_pump(); h = f.h
		ccall(IG._fn(:gmtvtk_set_view_mode_h), Cint, (Ptr{Cvoid}, Cint), h, Cint(0))   # 3-D
		ve_pump()
		st = ve_state_full(h)
		@test parse(Float64, string(st["ve"])) == 1.0
		# GEOMETRY: at ve = 1 the relief stands kVEReference of the map's displayed width tall. A true-
		# scale (flat-plate) regression makes this collapse by three orders of magnitude.
		drawn = ve_active_drawn_span(h)
		@test drawn ≈ 0.1 * ve_H_displayed(G) rtol=1e-6
		@test drawn > 0.01 * ve_H_displayed(G)      # blunt guard: never a sheet again
		# ...and the shading defaults are the ones the eye expects (no "Remove illumination", a sun).
		@test parse(Int, string(st["noshade"])) == 0
	finally
		ve_close(f.h)
	end
end

@testitem "ILLUMINATION law 3: the rendered default view really is lit" tags=[:gui, :ve, :illum] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	f = view_grid(ve_grid(zspan = 8000.0), geographic=true)
	png = joinpath(tempdir(), "ig_ve_illum_$(getpid()).png")
	try
		ve_pump(); h = f.h
		ccall(IG._fn(:gmtvtk_set_view_mode_h), Cint, (Ptr{Cvoid}, Cint), h, Cint(0))   # 3-D
		ve_pump()
		if IG.save_png(f, png) && isfile(png)
			# PIXELS, not code: a lit relief spreads luminance across the surface; a flat plate under a
			# single sun is one nearly uniform tone. The threshold is deliberately far below what lit
			# relief gives (measured ~40+ on this scene) and far above a flat sheet (~a few).
			@test ve_luminance_spread(png) > 12.0
		else
			@test_skip "save_png unavailable in this environment"
		end
	finally
		ve_close(f.h)
		isfile(png) && rm(png, force=true)
	end
end

@testitem "ILLUMINATION law 3: a layer's ve does not re-light another layer" tags=[:gui, :ve, :illum] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	Gb = ve_grid(zspan = 8000.0)
	f = view_grid(Gb, geographic=true)
	png1 = joinpath(tempdir(), "ig_ve_illum_a_$(getpid()).png")
	png2 = joinpath(tempdir(), "ig_ve_illum_b_$(getpid()).png")
	try
		ve_pump(); h = f.h
		ccall(IG._fn(:gmtvtk_set_view_mode_h), Cint, (Ptr{Cvoid}, Cint), h, Cint(0))
		ve_pump()
		ve_add_layer(h, ve_grid(zspan = 5.0), "L1")
		ve_show(h, "L1", false)          # only the BASE is on screen, at its own ve = 1
		ve_pump()
		ok1 = IG.save_png(f, png1)
		# Move the HIDDEN layer's exaggeration by a factor of a thousand. The base is what is on
		# screen and nothing about it may change -- this is the "one grid re-scaled and re-LIT another"
		# regression, in the form the user first hit it (an Okada layer over layer0).
		ve_show(h, "L1", true); ve_set_active(h, 1000.0); ve_show(h, "L1", false)
		ve_pump()
		ok2 = IG.save_png(f, png2)
		if ok1 && ok2 && isfile(png1) && isfile(png2)
			@test ve_frames_match(png1, png2)        # pixel-identical: same geometry, same lighting
		else
			@test_skip "save_png unavailable in this environment"
		end
		st = ve_state_full(h)
		@test parse(Float64, string(st["ve"])) == 1.0
		@test isapprox(parse(Float64, string(st["zfac"])), ve_zfac_expected(Gb), rtol=1e-9)
	finally
		ve_close(f.h)
		for p in (png1, png2); isfile(p) && rm(p, force=true); end
	end
end
