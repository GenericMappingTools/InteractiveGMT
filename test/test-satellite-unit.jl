# Satellite orbit geometry. These are the tests that were missing when a sun-synchronous orbit was
# drawn as a closed ring and a geostationary one as a figure-of-eight: each one asserts a PROPERTY OF
# THE ORBIT that a wrong frame, a wrong altitude or a wrong inclination breaks immediately.
#
# No network: the TLEs are inlined, so the suite tests the maths and never Celestrak's uptime.

@testitem "satellite: TLE parsing and orbit classification" tags=[:unit, :satellite] begin
	using InteractiveGMT
	const S = InteractiveGMT

	# TERRA (EOS AM-1): sun-synchronous, 705 km, i = 98.2 deg, ~99 min.
	terra = S.TLE("TERRA",
		"1 25994U 99068A   26250.50000000  .00000521  00000+0  11826-3 0  9993",
		"2 25994  98.1234 250.1234 0001234  90.0000 270.0000 14.57109999999999")
	# METEOSAT-12 (MTG-I1): geostationary, i = 0.78 deg, one sidereal day.
	mtg = S.TLE("METEOSAT-12 (MTG-I1)",
		"1 54743U 22170C   26258.35906899 -.00000015  00000+0  00000+0 0  9992",
		"2 54743   0.7817  26.5572 0003852  85.8370  10.7332  1.00271984 13882")

	st = S.Satellite(terra)
	sm = S.Satellite(mtg)
	try
		@test S.inclination(st) ≈ 98.1234 atol = 1e-4
		@test S.inclination(sm) ≈ 0.7817  atol = 1e-4
		@test 95.0 < S.period(st) < 105.0                 # a ~700 km orbit is about 99 minutes
		@test S.period(sm) ≈ S._SIDEREAL_DAY_MIN rtol = 0.01

		# THE FRAME RULE, by physics and not by a list of names: a satellite that keeps station with
		# the Earth has no Earth-fixed path worth drawing and gets the inertial one; everything else
		# is drawn over the ground.
		@test S._frame_for(st, :auto) === :earthfixed
		@test S._frame_for(sm, :auto) === :inertial
		@test S._frame_for(sm, :earthfixed) === :earthfixed     # an explicit choice always wins
		@test S._frame_for(st, :inertial)   === :inertial
	finally
		S.close!(st);  S.close!(sm)
	end
end

@testitem "satellite: a sun-synchronous orbit walks around the globe, it does not close" tags=[:unit, :satellite] begin
	using InteractiveGMT
	const S = InteractiveGMT

	terra = S.TLE("TERRA",
		"1 25994U 99068A   26250.50000000  .00000521  00000+0  11826-3 0  9993",
		"2 25994  98.1234 250.1234 0001234  90.0000 270.0000 14.57109999999999")
	st = S.Satellite(terra)
	try
		D = S.groundtrack(st; revolutions = 2, step = S.Second(60))
		@test !isempty(D)
		M = reduce(vcat, (d.data for d in D))

		# Altitude: a 700 km orbit, and the PLOTTED z is that altitude in world units, never raw km.
		@test 600.0 < minimum(M[:,4]) < 900.0
		@test all(abs.(M[:,3] .- M[:,4] ./ S._KM_PER_DEG_ARC) .< 1e-9)

		# Latitude reaches the inclination (or its supplement) — a polar orbit must go near the poles.
		@test maximum(M[:,2]) > 80.0
		@test minimum(M[:,2]) < -80.0

		# THE POINT OF THIS TEST. Two revolutions of a LEO are NOT the same ground path: the Earth
		# turns ~22.5 deg under each one. Take the northbound equator crossings and demand that
		# consecutive ones are separated in longitude. Drawing the inertial ring instead (the bug)
		# puts them on top of each other.
		cross = Float64[]
		for i in 2:size(M,1)
			(isnan(M[i,2]) || isnan(M[i-1,2])) && continue
			if M[i-1,2] < 0.0 <= M[i,2] && abs(M[i,1] - M[i-1,1]) < 180.0
				f = (0.0 - M[i-1,2]) / (M[i,2] - M[i-1,2])
				push!(cross, M[i-1,1] + f * (M[i,1] - M[i-1,1]))
			end
		end
		@test length(cross) >= 2
		dl = abs(cross[2] - cross[1]);  dl > 180.0 && (dl = 360.0 - dl)
		@test 15.0 < dl < 30.0                       # ~22.5 deg per revolution, westward
	finally
		S.close!(st)
	end
end

@testitem "satellite: a geostationary orbit is a full ring, not an analemma" tags=[:unit, :satellite] begin
	using InteractiveGMT
	const S = InteractiveGMT

	mtg = S.TLE("METEOSAT-12 (MTG-I1)",
		"1 54743U 22170C   26258.35906899 -.00000015  00000+0  00000+0 0  9992",
		"2 54743   0.7817  26.5572 0003852  85.8370  10.7332  1.00271984 13882")
	sm = S.Satellite(mtg)
	try
		# Default (automatic) => the inertial ring: it spans the whole 360 deg of longitude and its
		# latitude reaches the orbit's inclination.
		D = S.groundtrack(sm; revolutions = 1, step = S.Minute(10))
		M = reduce(vcat, (d.data for d in D))
		@test maximum(M[:,1]) > 170.0 && minimum(M[:,1]) < -170.0
		@test maximum(M[:,2]) ≈ 0.764 atol = 0.05
		@test minimum(M[:,2]) ≈ -0.764 atol = 0.05
		@test all(35_600.0 .< M[:,4] .< 35_950.0)          # geostationary altitude

		# Earth-fixed, on demand: the SAME satellite hangs over one longitude — that is what makes the
		# inertial frame the right default for it, and the test states the contrast rather than
		# assuming it.
		E = S.groundtrack(sm; revolutions = 1, step = S.Minute(10), frame = :earthfixed)
		ME = reduce(vcat, (d.data for d in E))
		@test maximum(ME[:,1]) - minimum(ME[:,1]) < 5.0    # a slot, not a circumnavigation
		@test maximum(ME[:,2]) > 0.5                       # …while the analemma still reaches ±i
	finally
		S.close!(sm)
	end
end

@testitem "satellite: the flat ground track is the sub-satellite point" tags=[:unit, :satellite] begin
	using InteractiveGMT
	const S = InteractiveGMT

	terra = S.TLE("TERRA",
		"1 25994U 99068A   26250.50000000  .00000521  00000+0  11826-3 0  9993",
		"2 25994  98.1234 250.1234 0001234  90.0000 270.0000 14.57109999999999")
	st = S.Satellite(terra)
	try
		D = S.groundtrack(st; revolutions = 1, step = S.Second(60), altitude = false)
		M = reduce(vcat, (d.data for d in D))
		@test all(M[:,3] .== 0.0)                       # flat: the plotted z is zero, by construction
		lon, lat, alt = S.subpoint(st, S._jds(collect(S.epoch(st):S.Second(60):S.epoch(st)+S.Minute(5))))
		@test M[1,1] ≈ lon[1] atol = 1e-9               # …and it IS the subpoint, not something else
		@test M[1,2] ≈ lat[1] atol = 1e-9
	finally
		S.close!(st)
	end
end

@testitem "satellite: the dateline cut keeps the track continuous" tags=[:unit, :satellite] begin
	using InteractiveGMT
	const S = InteractiveGMT

	terra = S.TLE("TERRA",
		"1 25994U 99068A   26250.50000000  .00000521  00000+0  11826-3 0  9993",
		"2 25994  98.1234 250.1234 0001234  90.0000 270.0000 14.57109999999999")
	st = S.Satellite(terra)
	try
		D = S.groundtrack(st; revolutions = 2, step = S.Second(60))
		@test length(D) > 1                             # it really did cross ±180
		for d in D
			M = d.data
			# No segment may contain a jump across the meridian: that is the whole job of the cut.
			for i in 2:size(M,1)
				@test abs(M[i,1] - M[i-1,1]) < 180.0
			end
			# Every segment ends (or starts) exactly ON the meridian where it was cut.
			@test size(M,1) >= 2
		end
		ends = [abs(abs(d.data[end,1]) - 180.0) for d in D]
		@test minimum(ends) < 1e-6
	finally
		S.close!(st)
	end
end
