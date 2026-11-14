# THE ZOOM PYRAMID MESHES WHAT IS ON SCREEN — the regression suite for 2026-11-13.
#
# What was wrong, measured on a Landsat-size temperature grid (7721 x 7841, ~30% NaN), process memory:
#   * opening it meshed 40 tiles / 9.5 M cells / ~1 GB in a camera state that was never drawn (the
#     pyramid was refined on EVERY camera event of the window build), for a view that needed one tile;
#   * every off-screen neighbour of the zoomed area was drawn by its FULL tile (up to 513 x 513 samples),
#     so a 16x zoom held 40 full tiles / 33 M points for ~0.24 M nodes on screen: 6.4 GB in all;
#   * each NaN-grid square owned four private vertices; the off-screen cache could keep 1 GiB.
# The rules now, each counted here off the real pyramid (gmtvtk_lod_stats_test), never off the code:
#   * a freshly opened grid holds ONE coarse tile;
#   * at a deep zoom only a bounded number of points is meshed, and the off-screen nodes are tiny
#     stand-ins, not full tiles;
#   * the surface still COVERS THE WHOLE GRID at every zoom (its bounds — what the axes, the VE and
#     the camera fit read — never shrink to the view);
#   * the cache stays within its budget.

@testitem "LOD: a new grid holds one coarse tile; a deep zoom meshes what is on screen" tags=[:gui, :lod] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	# 4097 x 4097 nodes (16.8 M), a smooth relief with a deterministic ~30% NaN speckle: the NaN-square
	# (pixelCells) path, the one a Landsat product takes.
	n = 4097
	z = let st = UInt64(0x2545F4914F6CDD1D), z = Matrix{Float32}(undef, n, n)
		for ix in 1:n, iy in 1:n
			st = st * 0x5851F42D4C957F2D + 0x14057B7EF767814F
			u = Float64(st >> 11) / Float64(UInt64(1) << 53)
			z[iy, ix] = u < 0.3 ? NaN32 : Float32(100 * sin(ix / 300) * cos(iy / 250))
		end
		z
	end
	G = GMT.mat2grid(z, x = collect(0.0:Float64(n - 1)), y = collect(0.0:Float64(n - 1)))
	stats(h) = begin
		o = zeros(Float64, 11)
		ok = ccall(GmtvtkTest._test_fn(:gmtvtk_lod_stats_test), Cint, (Ptr{Cvoid}, Ptr{Cdouble}), h, o)
		ok == 1 || error("no tiled base surface")
		(full = Int(o[1]), standins = Int(o[2]), points = Int(o[3]), resident = o[4], budget = o[5], bounds = o[6:11])
	end
	h = ccall(IG._fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "lod memory")
	try
		ve_pump(10)
		IG._drop_into(h, G, "speckled relief"; promote = true, source = "")
		ve_pump(30)
		s0 = stats(h)
		@info "LOD at open" s0.full s0.standins s0.points resident_MB = s0.resident / 2^20
		@test s0.full == 1                      # the whole grid on screen: its one coarse root tile
		@test s0.standins == 0
		@test s0.resident < 64 * 2^20           # nothing meshed for camera states that were never drawn
		b0 = s0.bounds
		for k in 1:4                            # 2x, 4x, 8x, 16x
			ccall(IG._fn(:gmtvtk_orbit), Cvoid, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble), h, 0.0, 0.0, 2.0)
			ve_pump(15)
			sk = stats(h)
			@info "LOD at zoom x$(2^k)" sk.full sk.standins sk.points resident_MB = sk.resident / 2^20
			# COVERAGE: the surface spans the whole grid at every zoom (stand-ins cover what is off screen).
			# X and Y only: the tiles' z extent follows which nodes each level samples (it always has), and
			# no frame reads it -- surfGetBounds takes Z from the active grid's own range.
			for j in 1:4
				@test isapprox(sk.bounds[j], b0[j]; atol = 1e-6 * (1 + abs(b0[j])))
			end
			@test sk.resident <= sk.budget + 128 * 2^20   # cache within budget (+ what is on screen)
		end
		s16 = stats(h)
		@test s16.standins > 0                  # off-screen nodes ARE stand-ins at a deep zoom…
		# …and what is meshed is bounded by the view, not by the grid: 1/256 of the area is on screen;
		# a whole-grid mesh at full resolution would be ~n^2 points.
		@test s16.points < n * n ÷ 10
	finally
		ve_close(h)
	end
end
