# CI-safe unit tests for the line/polygon measurement math (measure.jl): the CRS guess, the
# per-vertex distance/azimuth core, and the polygon area. These never open a Qt+VTK window — they
# call the pure helpers directly, so they run anywhere `using InteractiveGMT` succeeds (the GUI side,
# the context-menu wiring + the fault-dialog prefill, is C++ and exercised by the :gui scenarios).
# The numeric expectations were checked against live GMT (mapproject / geodesicarea / geomarea).

@testitem "measure helpers present" tags=[:unit, :fast] begin
	for s in (:_measure_isgeog, :_seg_dist_azim, :_polygon_area, :_line_length, :_line_azimuth,
	          :_poly_area, :_length_scale)
		@test isdefined(InteractiveGMT, s)
	end
end

@testitem "measure: geographic guess (proj4 then range)" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	D  = GMT.mat2ds([0.0 0.0; 1.0 1.0])
	# An explicit projection decides outright, ignoring the coordinates.
	@test IG._measure_isgeog(D, "+proj=longlat +datum=WGS84 +no_defs") == true
	@test IG._measure_isgeog(D, "+proj=utm +zone=29 +datum=WGS84")     == false
	# No projection -> GMT.guessgeog falls back to the [-180 360 -90 90] range test.
	@test IG._measure_isgeog(GMT.mat2ds([0.0 0.0; 1.0 1.0]),         "") == true
	@test IG._measure_isgeog(GMT.mat2ds([0.0 0.0; 5000.0 5000.0]),   "") == false
end

@testitem "measure: cartesian distance + azimuth" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# Azimuth is bearing N(+Y)→CW of the segment ARRIVING at each vertex; vertex 1 has none (NaN).
	inc, az = IG._seg_dist_azim([0.0, 0.0], [0.0, 1.0], false)     # due north
	@test inc ≈ [0.0, 1.0]
	@test isnan(az[1]) && az[2] ≈ 0.0
	inc, az = IG._seg_dist_azim([0.0, 1.0], [0.0, 0.0], false)     # due east
	@test inc ≈ [0.0, 1.0]
	@test isnan(az[1]) && az[2] ≈ 90.0
	# Three-vertex path: distances + per-arrival azimuths, all aligned to the arriving vertex.
	inc, az = IG._seg_dist_azim([0.0, 1.0, 1.0], [0.0, 0.0, 1.0], false)
	@test inc ≈ [0.0, 1.0, 1.0]
	@test isnan(az[1]) && az[2] ≈ 90.0 && az[3] ≈ 0.0
end

@testitem "measure: geographic distance + azimuth (mapproject)" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	inc, az = IG._seg_dist_azim([0.0, 1.0], [0.0, 1.0], true)      # 0,0 -> 1,1 on WGS84
	@test inc[1] == 0.0
	@test isapprox(inc[2], 156.9; atol = 0.5)                      # geodesic km
	@test isnan(az[1])
	@test isapprox(az[2], 45.0; atol = 0.5)                        # forward azimuth
end

@testitem "measure: Preferences Dist/Azim type (Ellipsoidal/Spherical/Flat Earth)" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# Same line, three approximations. Distances are all near the geodesic; Flat Earth differs most
	# in azimuth (it ignores meridian convergence). Reference numbers: docs/geod_vs_mapproject.md.
	x = [-13.63333333, -10.08333333];  y = [35.54088226, 36.17009542]
	de, ae = IG._seg_dist_azim(x, y, true; datype = "Ellipsoidal")
	ds, as_ = IG._seg_dist_azim(x, y, true; datype = "Spherical")
	df, af = IG._seg_dist_azim(x, y, true; datype = "Flat Earth")
	@test isapprox(de[end], 328.155; atol = 0.01) && isapprox(ae[end], 76.6812; atol = 0.01)
	@test isapprox(ds[end], 327.477; atol = 0.01) && isapprox(as_[end], 76.6285; atol = 0.01)
	@test isapprox(df[end], 327.498; atol = 0.01) && isapprox(af[end], 79.9491; atol = 0.01)
end

@testitem "measure: Preferences Dir (backward azimuth = forward + 180)" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# Cartesian due-north segment: forward 0°, backward 180°.
	_, azf = IG._seg_dist_azim([0.0, 0.0], [0.0, 1.0], false; back = false)
	_, azb = IG._seg_dist_azim([0.0, 0.0], [0.0, 1.0], false; back = true)
	@test azf[2] ≈ 0.0 && azb[2] ≈ 180.0
	# Geographic backward azimuth is the forward + 180 (mod 360), per vertex.
	_, gf = IG._seg_dist_azim([-13.63333333, -10.08333333], [35.54088226, 36.17009542], true)
	_, gb = IG._seg_dist_azim([-13.63333333, -10.08333333], [35.54088226, 36.17009542], true; back = true)
	@test isapprox(mod(gf[2] + 180.0, 360.0), gb[2]; atol = 1e-6)
end

@testitem "measure: Preferences Measure units scale" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG._length_scale("kilometers",     true) == (1.0, "km")
	@test IG._length_scale("meters",         true) == (1000.0, "m")
	@test IG._length_scale("nautical miles", true)[1] ≈ 1 / 1.852      && IG._length_scale("nautical miles", true)[2] == "NM"
	@test IG._length_scale("miles",          true)[1] ≈ 1 / 1.609344   && IG._length_scale("miles", true)[2] == "mi"
	@test IG._length_scale("anything",       true) == (1.0, "km")      # unknown -> kilometers
	@test IG._length_scale("meters",         false) == (1.0, "units")  # cartesian: always data units
end

@testitem "measure: polygon area (cartesian vs geographic)" tags=[:unit, :fast] begin
	IG  = InteractiveGMT
	sq  = [0.0 0.0; 1.0 0.0; 1.0 1.0; 0.0 1.0; 0.0 0.0]           # closed unit square
	@test IG._polygon_area(sq, false) ≈ 1.0                       # planar, data units²
	@test isapprox(IG._polygon_area(sq, true), 1.2309e10; rtol = 0.02)  # 1° box at equator, m²
	# The ring need not arrive closed — the helper closes it.
	open_sq = [0.0 0.0; 1.0 0.0; 1.0 1.0; 0.0 1.0]
	@test IG._polygon_area(open_sq, false) ≈ 1.0
end

@testitem "measure: polygon area by Dist/Azim type (geodesicarea vs gmtspatial)" tags=[:unit, :fast] begin
	IG  = InteractiveGMT
	box = [-13.0 35.0; -12.0 35.0; -12.0 36.0; -13.0 36.0; -13.0 35.0]   # 1°×1° box near 36°N
	a_ell = IG._polygon_area(copy(box), true, "Ellipsoidal")            # GMT.geodesicarea, m²
	a_sph = IG._polygon_area(copy(box), true, "Spherical")              # GMT.gmtspatial -Q, m²
	@test a_ell > 0 && a_sph > 0
	@test isapprox(a_ell / 1e6, 10066.0; atol = 2.0)                    # ~10066 km²
	@test isapprox(a_ell, a_sph; rtol = 0.01)                          # ellipsoid vs sphere within ~1%
	# Cartesian path also routes through gmtspatial -Q now; unit square stays 1.0.
	@test IG._polygon_area([0.0 0.0; 1.0 0.0; 1.0 1.0; 0.0 1.0; 0.0 0.0], false) ≈ 1.0
end

# "Digitize whites" outline of a mask -> "Area under polygon…": EVERY ring is measured and the holes are
# taken off (user's order, 2026-11-17). Exact check: the traced area equals white pixels x pixel area.
@testitem "measure: area of a traced mask outline (several rings, holes subtracted)" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	n = 200
	G = IG.GMT.mat2grid(zeros(Float32, n, n), x = [0.0, 6000.0], y = [0.0, 6000.0], reg = 1)   # 30 m pixels
	m = [(20 <= hypot(ix - 70.5, iy - 70.5) <= 45) || (hypot(ix - 160.5, iy - 150.5) <= 25) for iy in 1:n, ix in 1:n]
	xyz, segoff, nseg, _ = IG._digitize_rings(IG._gc_mask_image(m, G), 1)
	rings = [reshape(xyz, 3, :)'[segoff[s]+1:segoff[s+1], 1:2] |> Matrix{Float64} for s in 1:nseg]
	@test nseg == 3
	@test IG._ring_holes(rings) |> count == 1                        # the ring field's hole
	tbl = tempname() * ".txt"
	open(tbl, "w") do io
		for r in rings
			println(io, ">");  for i in axes(r, 1); println(io, r[i, 1], " ", r[i, 2]); end
		end
	end
	out = mktemp() do p, io
		redirect_stdout(() -> IG._poly_area(C_NULL, tbl, "", true, "Ellipsoidal"), io);  flush(io);  read(p, String)
	end
	a = parse(Float64, match(r"Area = ([0-9.eE+]+)", out)[1])
	@test a ≈ count(m) * 900.0
	@test occursin("1 hole(s) subtracted", out)
	rm(tbl; force = true)
end

# The POLYGON a clicked ring belongs to (user's order, 2026-11-17: Area / Delete on a traced outline act
# on the selected polygon only): an outer ring answers with itself and its own holes, a hole with the
# ring around it and that ring's holes, a lone blob with itself. Its area is that polygon's alone.
@testitem "measure: the polygon of a clicked ring, and its area alone" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	n = 200
	G = IG.GMT.mat2grid(zeros(Float32, n, n), x = [0.0, 6000.0], y = [0.0, 6000.0], reg = 1)
	donut(ix, iy) = 20 <= hypot(ix - 70.5, iy - 70.5) <= 45
	blob(ix, iy)  = hypot(ix - 160.5, iy - 150.5) <= 25
	m = [donut(ix, iy) || blob(ix, iy) for iy in 1:n, ix in 1:n]
	xyz, segoff, nseg, _ = IG._digitize_rings(IG._gc_mask_image(m, G), 1)
	rings = [Matrix{Float64}(reshape(xyz, 3, :)'[segoff[s]+1:segoff[s+1], 1:2]) for s in 1:nseg]
	tbl(rs) = (p = tempname() * ".txt";
		open(p, "w") do io
			for r in rs; println(io, ">"); for i in axes(r, 1); println(io, r[i, 1], " ", r[i, 2]); end; end
		end; p)
	capture(f) = mktemp() do p, io; redirect_stdout(f, io); flush(io); read(p, String); end
	all_tbl = tbl(rings)
	region(k) = sort(parse.(Int, split(capture(() -> IG._ring_region(all_tbl, k)))))
	regs = [region(k) for k in 0:nseg-1]
	isblob = [length(r) == 1 for r in regs]
	@test count(isblob) == 1                                   # the blob alone
	donutreg = regs[findfirst(!, isblob)]
	@test length(donutreg) == 2 && all(r -> r == donutreg, regs[.!isblob])   # outer ring OR its hole -> both
	area(rs) = parse(Float64, match(r"Area = ([0-9.eE+]+)", capture(() -> IG._poly_area(C_NULL, tbl(rs), "", true, "Ellipsoidal")))[1])
	@test area(rings[donutreg .+ 1]) ≈ count(donut(ix, iy) for iy in 1:n, ix in 1:n) * 900.0
	@test area(rings[findall(isblob)]) ≈ count(blob(ix, iy) for iy in 1:n, ix in 1:n) * 900.0
end
