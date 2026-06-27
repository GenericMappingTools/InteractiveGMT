# CI-safe unit tests for the line/polygon measurement math (measure.jl): the CRS guess, the
# per-vertex distance/azimuth core, and the polygon area. These never open a Qt+VTK window — they
# call the pure helpers directly, so they run anywhere `using InteractiveGMT` succeeds (the GUI side,
# the context-menu wiring + the fault-dialog prefill, is C++ and exercised by the :gui scenarios).
# The numeric expectations were checked against live GMT (mapproject / geodesicarea / geomarea).

@testitem "measure helpers present" tags=[:unit, :fast] begin
	for s in (:_measure_isgeog, :_seg_dist_azim, :_polygon_area, :_line_length, :_line_azimuth, :_poly_area)
		@test isdefined(InteractiveGMT, s)
	end
end

@testitem "measure: geographic guess (proj4 then range)" tags=[:unit, :fast] begin
	IG = InteractiveGMT
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

@testitem "measure: polygon area (cartesian vs geographic)" tags=[:unit, :fast] begin
	IG  = InteractiveGMT
	sq  = [0.0 0.0; 1.0 0.0; 1.0 1.0; 0.0 1.0; 0.0 0.0]           # closed unit square
	@test IG._polygon_area(sq, false) ≈ 1.0                       # planar, data units²
	@test isapprox(IG._polygon_area(sq, true), 1.2309e10; rtol = 0.02)  # 1° box at equator, m²
	# The ring need not arrive closed — the helper closes it.
	open_sq = [0.0 0.0; 1.0 0.0; 1.0 1.0; 0.0 1.0]
	@test IG._polygon_area(open_sq, false) ≈ 1.0
end
