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
