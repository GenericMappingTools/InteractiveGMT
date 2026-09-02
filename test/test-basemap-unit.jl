# Pure-Julia unit tests for the Base Map pixel arithmetic and the scene-state parser. No DLL — these
# run in the fast tier. The parser test is the "is the test harness itself correct" layer: it pins
# how a gmtvtk_scene_state dump string decodes, independent of any live window.

@testitem "basemap srcwin pixel math" tags=[:unit, :fast] begin
	sw = InteractiveGMT._etopo4_srcwin
	NX = InteractiveGMT._ETOPO4_NX
	NY = InteractiveGMT._ETOPO4_NY
	# The window AND the extent those pixels really cover come out together — one quantity, one
	# computation. Every case below checks BOTH: an extent that disagrees with the pixels is the
	# "imported points land in the wrong place" bug this second half of the return exists to stop.
	# whole world [-180,180]x[-90,90] -> the whole image, origin at the upper-left.
	@test sw(-180.0, 180.0, -90.0, 90.0) == (0, 0, NX, NY, -180.0, 180.0, -90.0, 90.0)
	# west half [-180,0], full latitude -> left half, full height.
	xoff, yoff, xs, ys, W, E, S, N = sw(-180.0, 0.0, -90.0, 90.0)
	@test xoff == 0 && yoff == 0 && xs == NX ÷ 2 && ys == NY
	@test (W, E, S, N) == (-180.0, 0.0, -90.0, 90.0)
	# NE quadrant [0,180]x[0,90] -> right half, TOP half (y counts down from the north edge).
	xoff, yoff, xs, ys, W, E, S, N = sw(0.0, 180.0, 0.0, 90.0)
	@test xoff == NX ÷ 2 && yoff == 0 && xs == NX ÷ 2 && ys == NY ÷ 2
	@test (W, E, S, N) == (0.0, 180.0, 0.0, 90.0)
	# out-of-range input still clamps to a valid in-image window — and the extent reports the CLAMPED
	# world, never the ±200/±100 that was asked for.
	xoff, yoff, xs, ys, W, E, S, N = sw(-200.0, 200.0, -100.0, 100.0)
	@test 0 <= xoff && xoff + xs <= NX
	@test 0 <= yoff && yoff + ys <= NY
	@test (W, E, S, N) == (-180.0, 180.0, -90.0, 90.0)
	# The extent is exactly the pixel window read back through the same grid spacing, always.
	# Own names inside the loop: reusing the ones assigned above would be an assignment to a global
	# from soft scope, which Julia warns about on every iteration and which reads, correctly, as a
	# bug waiting to happen.
	for r in ((-33.0, 12.5, -8.0, 44.0), (100.0, 140.0, -40.0, -10.0), (-7.0, -6.0, 36.0, 37.0))
		(xo, yo, xw, yh, w, e, s, n) = sw(r...)
		@test (w, e) == (-180.0 + xo / NX * 360.0, -180.0 + (xo + xw) / NX * 360.0)
		@test (n, s) == (90.0 - yo / NY * 180.0,   90.0 - (yo + yh) / NY * 180.0)
	end
end

@testitem "scene-state parser" tags=[:unit, :fast] begin
	p = InteractiveGMT._parse_scene_state
	d = p("alive=1;has_surface=1;emptyStart=0;imageOnly=1;flat2d=1;axes=1;crs=1;" *
	      "x0=-180;x1=180;y0=-90;y1=90;zmin=0;zmax=0;n_extras=1;n_overlays=0;" *
	      "surf_name=;extra0=image:Base image (global);")
	@test d["alive"] == 1 && d["has_surface"] == 1 && d["axes"] == 1
	@test d["imageOnly"] == 1 && d["flat2d"] == 1 && d["crs"] == 1
	@test d["x0"] == -180.0 && d["x1"] == 180.0 && d["y0"] == -90.0 && d["y1"] == 90.0
	@test d["n_extras"] == 1
	@test d["surf_name"] == ""
	@test d["extras"] == [("image", "Base image (global)")]
	# a name carrying spaces/parentheses survives intact; a grid extra keeps its kind.
	d2 = p("alive=1;n_extras=2;extra0=grid:my grid.nc;extra1=image:tile (2x3);")
	@test d2["extras"] == [("grid", "my grid.nc"), ("image", "tile (2x3)")]
	# a dead handle dumps only alive=0 and yields no extras.
	d0 = p("alive=0;")
	@test d0["alive"] == 0 && isempty(d0["extras"])
end
