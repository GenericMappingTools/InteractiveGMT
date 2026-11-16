# CI-safe unit tests for Grid Tools > "Synthetic terrain" (src/synthterrain.jl): the maths only, no
# window. The dialog (SynthTerrainDialog, 70_window.cpp) only forwards its boxes.

@testitem "synthterrain: lattice, heights and determinism" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	G = IG.synth_terrain((10, 30, -5, 5), (0.1, 0.1); zrange = (-200, 800), seed = 3)
	@test size(G.z) == (101, 201)                         # (ny, nx): 20/0.1+1 by 10/0.1+1
	@test G.x[1] == 10 && G.x[end] == 30 && G.y[1] == -5 && G.y[end] == 5
	@test G.inc ≈ [0.1, 0.1]
	@test G.registration == 0
	@test minimum(G.z) ≈ -200 && maximum(G.z) ≈ 800       # the whole span is used
	@test all(isfinite, G.z)
	# the same seed and settings give the same terrain; another seed another one
	G2 = IG.synth_terrain((10, 30, -5, 5), (0.1, 0.1); zrange = (-200, 800), seed = 3)
	@test G2.z == G.z
	G3 = IG.synth_terrain((10, 30, -5, 5), (0.1, 0.1); zrange = (-200, 800), seed = 4)
	@test G3.z != G.z
end

@testitem "synthterrain: the shape stage" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	h = IG._st_shape(128, 96; feature = 40.0, octaves = 8, gain = 0.5, ridge = 0.5, warp = 0.4, seed = 1)
	@test size(h) == (96, 128)
	@test minimum(h) == 0 && maximum(h) == 1                # normalised
	# it is a relief, not white noise: neighbours are far closer than random pairs
	d1 = sum(abs, diff(h, dims = 2)) / length(diff(h, dims = 2))
	@test d1 < 0.05
	# no octave finer than ~4 nodes: the same terrain with 8 or 16 requested octaves
	h16 = IG._st_shape(128, 96; feature = 40.0, octaves = 16, gain = 0.5, ridge = 0.5, warp = 0.4, seed = 1)
	@test h16 == h
end

@testitem "synthterrain: the landscape drains like real land" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	z = IG._st_landscape(160, 120; feature = 48.0, lowlands = 0.1, hillslope = 0.3, concavity = 0.45, seed = 2)
	@test size(z) == (120, 160)
	@test minimum(z) == 0 && maximum(z) == 1
	# rivers reach the border: almost no interior node is a closed pit (lower than all 8 neighbours)
	ispit(ix, iy) = all(z[iy, ix] < z[iy + oy, ix + ox] for ox in -1:1, oy in -1:1 if (ox, oy) != (0, 0))
	pits = count(ispit(ix, iy) for ix in 2:159, iy in 2:119)
	@test pits / (158 * 118) < 0.002
	# a coarse-model terrain is interpolated onto EXACTLY the asked lattice
	G = IG.synth_terrain((0, 10, 0, 6), (0.01, 0.01))               # 1001 x 601 > the working size
	@test size(G.z) == (601, 1001) && G.x[1] == 0 && G.x[end] == 10 && G.y[1] == 0 && G.y[end] == 6
	@test minimum(G.z) ≈ 0 && maximum(G.z) ≈ 1000
end

@testitem "synthterrain: geographic CRS and the auto defaults" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# a region inside lon/lat limits is geographic; one outside is not
	G = IG.synth_terrain((-13, 23, 30, 60), (0.5, 0.5))
	@test G.geog == 1 && G.proj4 == IG.GMT.prj4WGS84
	C = IG.synth_terrain((0, 1000, 0, 500), (10, 10))
	@test C.geog != 1 && C.proj4 == ""                      # mat2grid's own "not set" (-1)
	@test IG.synth_terrain((-13, 23, 30, 60), (0.5, 0.5); geog = false).geog != 1
	# "auto" fills every box from _ST_DEFAULTS: the region is -R-13/23/30/60 at 0.05
	d = IG._ST_DEFAULTS
	@test (d["x0"], d["x1"], d["y0"], d["y1"], d["dx"], d["dy"]) == ("-13", "23", "30", "60", "0.05", "0.05")
	@test IG._st_lonlat((-13, 23, 30, 60))
end

@testitem "synthterrain: bad settings are refused" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test_throws Exception IG.synth_terrain((1, 0, 0, 1), (0.1, 0.1))           # empty region
	@test_throws Exception IG.synth_terrain((0, 1, 0, 1), (0, 0.1))             # zero increment
	@test_throws Exception IG.synth_terrain((0, 1, 0, 1), (1, 1))               # under 3x3 nodes
	@test_throws Exception IG.synth_terrain((0, 1, 0, 1), (0.6, 0.6))           # 0.6 does not divide 1
	@test_throws Exception IG.synth_terrain((0, 1, 0, 1), (0.1, 0.1); hillslope = 1.5)
	@test_throws Exception IG.synth_terrain((0, 1, 0, 1), (0.1, 0.1); lowlands = 2)
	@test_throws Exception IG.synth_terrain((0, 1, 0, 1), (0.1, 0.1); concavity = 0.9)
	@test_throws Exception IG.synth_terrain((0, 1, 0, 1), (0.1, 0.1); zrange = (5, 5))
end
