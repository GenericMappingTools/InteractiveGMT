# THE NaN CONTRACT OF THE GRID MESH — the regression suite for 2026-09-23.
#
# What broke, and was not noticed for weeks: the VTK (PBR) method (1) and every other method drawn on
# the 3-D surface mishandled NaN holes, in turn
#   * dropping every cell that touched a NaN node -> a cloud-speckled daily SST grid showed HALF its
#     data (7.8% of the map coloured where the data covers 15.6%);
#   * then (41b5e13) filling each NaN rim node with its neighbours' mean -> data painted INTO the
#     holes, lit by flat normals: pale rings round every cloud gap.
# The rule now: outside a tsunami window a NaN node is NEVER given a value. A grid that has NaNs is
# meshed as one square per real node (makeGridTile `pixelCells`): the footprint the per-pixel bake
# (method 7) paints, and nothing invented. A tsunami window keeps its own rim path, untouched.
#
# Two layers of test, both counting what was really built, never the shape of the code:
#   * MESH items: makeGridTile run through gmtvtk_tile_mesh_test on grids with a known NaN pattern.
#     Exact counts — one square per real node, none on a NaN, no scalar that is not its own node's.
#   * FOOTPRINT item: a speckled grid opened through the real drop/promote door, rendered with method 1
#     and method 7; the coloured area of the two must agree. Coloured pixels are counted, so any future
#     rule that loses data (too few) or invents it (too many) fails here whatever the code looks like.

@testmodule NanMeshFixture begin
	# A deterministic speckle: `frac` of the nodes NaN, from a tiny LCG so no RNG package is needed and
	# the pattern is the same on every machine. Z[i, j]: i along x, j along y (south first).
	function speckle(nx::Int, ny::Int; frac::Float64 = 0.6, seed::UInt64 = 0x9E3779B97F4A7C15,
	                 base::Float32 = 20f0, amp::Float32 = 5f0)
		Z = Matrix{Float32}(undef, nx, ny)
		st = seed
		for j in 1:ny, i in 1:nx
			st = st * 0x5851F42D4C957F2D + 0x14057B7EF767814F
			u = Float64(st >> 11) / Float64(UInt64(1) << 53)
			st = st * 0x5851F42D4C957F2D + 0x14057B7EF767814F
			v = Float64(st >> 11) / Float64(UInt64(1) << 53)
			Z[i, j] = u < frac ? NaN32 : base + amp * Float32(v)
		end
		Z
	end
	# the node lattice a tile at `step` samples: 0, s, 2s, ... plus the grid's last node
	lattice(n::Int, s::Int) = unique(vcat(collect(0:s:n-1), n - 1))
end

@testitem "NaN mesh: each real node is its own square, a NaN node none (full resolution)" tags=[:gui, :nan] setup=[GmtvtkTest, NanMeshFixture] begin
	Z = NanMeshFixture.speckle(60, 40; frac = 0.6)
	r = GmtvtkTest.tile_mesh_stats(Z; step = 1, pixelCells = true)
	@test r.cells == count(!isnan, Z)      # ONE square per real node — none lost…
	@test r.missed == 0                    # …and every real node has one
	@test r.on_nan == 0                    # no square stands on a NaN node
	@test r.invented == 0                  # every square wears its own node's value
	@test r.nan_cells == 0
	@test r.points == 4 * r.cells
end

@testitem "NaN mesh: a coarse tile samples, it never fills (step 4)" tags=[:gui, :nan] setup=[GmtvtkTest, NanMeshFixture] begin
	Z = NanMeshFixture.speckle(61, 45; frac = 0.6)
	for s in (2, 4, 8)
		r = GmtvtkTest.tile_mesh_stats(Z; step = s, pixelCells = true)
		xs = NanMeshFixture.lattice(size(Z, 1), s); ys = NanMeshFixture.lattice(size(Z, 2), s)
		want = count(!isnan, Z[i + 1, j + 1] for i in xs, j in ys)
		@test r.cells == want                # exactly the real SAMPLED nodes
		@test r.missed == 0
		@test r.on_nan == 0
		@test r.invented == 0
		@test r.nan_cells == 0
	end
end

@testitem "NaN mesh: the corner mesh draws only between real nodes and invents nothing" tags=[:gui, :nan] setup=[GmtvtkTest, NanMeshFixture] begin
	Z = NanMeshFixture.speckle(50, 30; frac = 0.3)
	r = GmtvtkTest.tile_mesh_stats(Z; step = 1, pixelCells = false, holeRim = false)
	nx, ny = size(Z)
	nnan = [count(isnan, (Z[i, j], Z[i+1, j], Z[i+1, j+1], Z[i, j+1])) for j in 1:ny-1, i in 1:nx-1]
	quads = count(==(0), nnan); tris = count(==(1), nnan)
	@test r.cells == quads + tris            # 4 real corners: the cell; 3: their triangle; else nothing
	@test r.invented == 0                    # every point carries its OWN node's value (no rim means)
	@test r.nan_cells == 0                   # no NaN node is part of any drawn cell
	@test r.on_nan == 0
end

@testitem "NaN mesh: a grid without NaNs is meshed exactly as always" tags=[:gui, :nan] setup=[GmtvtkTest, NanMeshFixture] begin
	Z = NanMeshFixture.speckle(40, 25; frac = 0.0)
	r = GmtvtkTest.tile_mesh_stats(Z; step = 1)
	@test r.cells == (size(Z, 1) - 1) * (size(Z, 2) - 1)
	@test r.points == size(Z, 1) * size(Z, 2)
	@test r.invented == 0
	@test r.nan_cells == 0
end

@testitem "NaN mesh: the TSUNAMI path keeps its rim, unchanged" tags=[:gui, :nan, :aquamoto] setup=[GmtvtkTest, NanMeshFixture] begin
	# The water and land halves of an Aquamoto layer are cut from one field by one mask; their rim is
	# what closes the slot along the coast. This pins that behaviour: a cell survives unless ALL four
	# corners are NaN, and the rim nodes carry values (so `invented` > 0 BY DESIGN here). If this item
	# fails, the tsunami display has been changed — which is forbidden without the user's order.
	Z = NanMeshFixture.speckle(50, 30; frac = 0.5)
	r = GmtvtkTest.tile_mesh_stats(Z; step = 1, holeRim = true)
	nx, ny = size(Z)
	want = count(count(isnan, (Z[i, j], Z[i+1, j], Z[i+1, j+1], Z[i, j+1])) < 4 for j in 1:ny-1, i in 1:nx-1)
	@test r.cells == want
	@test r.invented > 0
	@test r.points == nx * ny
end

@testitem "NaN footprint: method 1 covers exactly the data, no less and no more" tags=[:gui, :nan] begin
	IG = InteractiveGMT; GMT = IG.GMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	# A speckled, FLAT grid: 30% real nodes, all at 25, plus two anchors at 0 and 50 in a corner so the
	# palette spans 0..50 and every data pixel is the palette's GREEN middle. A data pixel is then told
	# from everything else by one test -- green clearly above blue -- which the NaN fill (white, or grey
	# under the tone pass) and the transparent background can never pass.
	# The reference is the DATA itself (its real-node fraction), not method 7: the flat image is a
	# texture interpolated on screen, so its green bleeds into the neighbouring NaN pixels and it reads
	# ~40% on this grid. Measured on the window the user sees, through the in-memory view capture.
	nx, ny = 720, 360
	z = let st = UInt64(0x2545F4914F6CDD1D), z = Matrix{Float32}(undef, ny, nx)
		for ix in 1:nx, iy in 1:ny
			st = st * 0x5851F42D4C957F2D + 0x14057B7EF767814F
			u = Float64(st >> 11) / Float64(UInt64(1) << 53)
			z[iy, ix] = u < 0.7 ? NaN32 : 25f0
		end
		z[1, 1] = 0f0; z[1, 2] = 50f0
		z
	end
	G = GMT.mat2grid(z, x = collect(range(-179.75, 179.75, length = nx)),
	                    y = collect(range(-89.75, 89.75, length = ny)))
	h = ccall(IG._fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "nan footprint")
	try
		ve_pump(10)
		IG._drop_into(h, G, "speckle"; promote = true, source = "")   # the File > Open door
		ve_pump(30)
		# share of the drawn (opaque) view that is data-green; the colour bar's strip at the right is left out
		datafrac() = begin
			p = Ref{Ptr{UInt8}}(C_NULL); w = Ref{Cint}(0); hh = Ref{Cint}(0)
			ok = ccall(IG._fn(:gmtvtk_capture_view_rgba), Cint,
			           (Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Cint}, Ptr{Cint}), h, p, w, hh)
			ok == 1 || error("view capture failed")
			A = copy(unsafe_wrap(Array, p[], (4, Int(w[]), Int(hh[]))))   # (band, col, row)
			ccall(IG._fn(:gmtvtk_free_rgb), Cvoid, (Ptr{UInt8},), p[])
			c = 0; t = 0
			for y in 1:size(A, 3), x in 1:floor(Int, 0.9 * size(A, 2))
				A[4, x, y] < 128 && continue
				t += 1
				(Int(A[2, x, y]) - Int(A[3, x, y]) > 25) && (c += 1)
			end
			c / max(t, 1)
		end
		ccall(IG._fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), h,
		      "imgmode=0;look=1;tone=1;ssao=1;fxaa=1;ibl=0;")          # METHOD 1, as the dialog sets it
		ve_pump(20)
		# a camera event, as any real interaction gives, so the tiles are refined for this view
		s = IG._parse_scene_state(IG._scene_state_full_raw(h))
		ps = parse(Float64, string(s["cam_ps"]))
		ccall(IG._fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), h, "cam_ps=$(ps * 0.999);")
		ve_pump(30)
		truth = count(!isnan, z) / length(z)
		m1 = datafrac()
		@info "NaN footprint" data = truth method1 = m1
		@test m1 >= 0.80 * truth       # no data lost   (the four-real-corners rule showed ~1% here)
		@test m1 <= 1.20 * truth       # none invented  (the neighbour-mean rim showed far more)
	finally
		ve_close(h)
	end
end
