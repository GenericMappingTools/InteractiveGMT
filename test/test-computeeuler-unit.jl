# CI-safe unit tests for Plates > Compute Euler pole (src/computeeuler.jl + GMT.jl's hellinger_auto).
# Everything here runs without a window: the ported distmin cost, the brute-force search, the
# residues cube writers and the Hellinger branch. Driving the dialog itself is the :gui item.

@testitem "Compute Euler pole: callback is wired" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	for s in (:_ce_start, :_ce_stop, :_ce_fit, :_ce_setup, :_ce_distmin, :_ce_near_a_line,
	          :_ce_resid_along, :_ce_write_vtk, :_ce_write_netcdf, :_ce_hellinger)
		@test isdefined(IG, s)
	end
	# The dialog pushes progress through this export; without it in the symbol list the live
	# pole/residue boxes would silently never update.
	@test :gmtvtk_compute_euler_progress in IG._LIB_SYMBOLS
end

@testitem "Compute Euler pole: distmin is a distance in km" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	D2R = pi / 180
	lat = collect(-10.0:0.25:0.0)
	lon = fill(-20.0, length(lat))
	l1 = (lon .* D2R, lat .* D2R)
	# The same line against itself: zero cost.
	len1 = IG._ce_seglen(l1[1], l1[2])
	a, = IG._ce_distmin(l1[1], l1[2], len1, copy(l1[1]), copy(l1[2]), len1)
	@test a < 1e-3                              # centimetres: the acos round-off, nothing else

	# A copy shifted 0.5 degrees east at the equator-ish latitudes above: the mean distance must
	# come out near 0.5 deg * 111.195 km/deg * cos(lat), i.e. a few tens of km, and never in radians.
	lon2 = lon .+ 0.5
	l2 = (lon2 .* D2R, lat .* D2R)
	len2 = IG._ce_seglen(l2[1], l2[2])
	b, = IG._ce_distmin(l1[1], l1[2], len1, l2[1], l2[2], len2)
	@test 50.0 < b < 56.0

	# The four-output form fills one entry per vertex of the FIRST line.
	c, xy, dists, w = IG._ce_distmin(l1[1], l1[2], len1, l2[1], l2[2], len2; full = true)
	@test c ≈ b
	@test size(xy) == (length(lat), 2)
	@test length(dists) == length(lat) && length(w) == length(lat)
	@test all(0 .<= w .<= 1)
end

@testitem "Compute Euler pole: the search finds a pole it was given" tags=[:unit] begin
	IG = InteractiveGMT
	lat  = collect(-30.0:0.5:0.0)
	iso1 = [(-20 .+ 3 .* sin.(lat ./ 10)) lat]
	# The second isochron IS the first one rotated by a known pole, so the search has an exact answer.
	iso2 = IG._ce_rotated_line(iso1, 135.0, 55.0, 5.0)
	st   = IG._ce_setup(iso1, iso2, 133.0, 54.0, 4.8, nothing)
	@test st.area0 > 1.0                       # the offset starting pole does not fit

	p_lon = IG._ce_axis(133.0, 8.0, 17)
	p_lat = IG._ce_axis(54.0,  8.0, 17)
	p_ang = IG._ce_axis(4.8,   1.0, 11)
	lon_bf, lat_bf, ang_bf, area, resid = IG._ce_fit(st, p_lon, p_lat, p_ang, 4.8, false)
	@test isempty(resid)
	@test area < st.area0                      # it improved
	@test area < 5.0                           # and lands within a few km of a perfect fit
	@test abs(ang_bf - 5.0) < 0.2

	# The residues cube is filled when asked for, one value per (lat, lon, angle) node.
	_, _, _, _, cube = IG._ce_fit(st, p_lon[1:3], p_lat[1:3], p_ang[1:3], 4.8, true)
	@test size(cube) == (3, 3, 3)
	@test all(isfinite, cube)
end

@testitem "Compute Euler pole: search box and line orientation" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	R2D = 180 / pi
	ax = IG._ce_axis(10.0, 4.0, 5) .* R2D
	@test length(ax) == 5
	@test ax[1] ≈ 8.0 && ax[end] ≈ 12.0 && ax[3] ≈ 10.0
	@test IG._ce_axis(10.0, 4.0, 1) .* R2D ≈ [10.0]

	# distmin needs both lines running the same way; a reversed second line is flipped back.
	a = [0.0 -5.0; 0.0 -4.0; 0.0 -3.0]
	b = [1.0 -3.0; 1.0 -4.0; 1.0 -5.0]
	@test IG._ce_same_sense(a, b)[:, 2] == [-5.0, -4.0, -3.0]
	@test IG._ce_same_sense(a, a) == a
end

@testitem "Compute Euler pole: residues cube writers" tags=[:unit] begin
	IG = InteractiveGMT
	lon = [10.0, 11.0, 12.0];  lat = [-5.0, -4.0];  ang = [1.0, 2.0]
	resid = reshape(collect(1.0:12.0), 2, 3, 2)          # (lat, lon, angle)

	vtk = joinpath(tempdir(), "igmt_ce_$(time_ns()).vtk")
	try
		IG._ce_write_vtk(vtk, lon, lat, ang, resid)
		@test isfile(vtk)
		bytes = read(vtk)
		@test occursin("DATASET RECTILINEAR_GRID", String(copy(bytes[1:80])))
		# header text + (3+2+2) axis floats + 12 scalars, all big-endian Float32
		@test length(bytes) > 4 * (3 + 2 + 2 + 12)
	finally
		isfile(vtk) && rm(vtk, force = true)
	end

	nc = joinpath(tempdir(), "igmt_ce_$(time_ns()).nc")
	try
		IG._ce_write_netcdf(nc, lon, lat, ang, resid)
		@test isfile(nc)
	finally
		isfile(nc) && rm(nc, force = true)
	end
end

@testitem "Compute Euler pole: Hellinger branch (GMT.hellinger_auto)" tags=[:unit] begin
	IG = InteractiveGMT
	lat  = collect(-30.0:0.5:0.0)
	iso1 = [(-20 .+ 3 .* sin.(lat ./ 10)) lat]
	iso2 = IG._ce_rotated_line(iso1, 135.0, 55.0, 5.0)

	H = IG.GMT.hellinger_auto(133.0, 54.0, 4.8, iso1, iso2; dp_tol = 60.0)
	@test isfinite(H.along) && isfinite(H.alat) && isfinite(H.rho)
	@test abs(H.rho - 5.0) < 0.5
	@test !isempty(H.stats)
	@test size(H.segments, 2) == 2 && size(H.segments_rot, 2) == 2
	@test length(H.flags_mov) == size(iso1, 1)
	@test length(H.flags_fix) == size(iso2, 1)
	@test maximum(H.flags_mov) >= 2                       # the DP tolerance did segment the line

	# "Force input = output": the statistics of a pole obtained some other way.
	F = IG.GMT.hellinger_auto(135.0, 55.0, 5.0, iso1, iso2; dp_tol = 60.0, force_pole = true)
	@test F.along ≈ 135.0 && F.alat ≈ 55.0 && F.rho ≈ 5.0
end

@testitem "Compute Euler pole: bingham returns a closed boundary" tags=[:unit] begin
	# The 95% confidence region of the pole, from the Bingham matrix a real Hellinger fit produces
	# (icase 1: a cap of admissible axes containing neither pole).
	IG = InteractiveGMT
	lat  = collect(-30.0:0.5:0.0)
	iso1 = [(-20 .+ 3 .* sin.(lat ./ 10)) lat]
	iso2 = IG._ce_rotated_line(iso1, 135.0, 55.0, 5.0)
	H = IG.GMT.hellinger_auto(133.0, 54.0, 4.8, iso1, iso2; dp_tol = 60.0)

	@test length(H.ellipse_lon) == length(H.ellipse_lat) > 12
	@test H.ellipse_lon[end] ≈ H.ellipse_lon[1] atol=1e-9      # the curve closes on itself
	@test H.ellipse_lat[end] ≈ H.ellipse_lat[1] atol=1e-9
	@test all(-360.5 .<= H.ellipse_lon .<= 360.5)
	@test all(-90.5 .<= H.ellipse_lat .<= 90.5)
	@test isfinite(H.vol) && H.vol > 0

	# The region surrounds the pole it describes.
	@test minimum(H.ellipse_lat) <= H.alat <= maximum(H.ellipse_lat)
end
