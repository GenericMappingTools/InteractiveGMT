# InteractiveGMT.RemoteS — the port of RemoteS.jl. Synthetic bands and an inlined TLE: no data
# files, no network.

@testitem "RemoteS: helpers and scene names" tags=[:unit, :remotes] begin
	using InteractiveGMT, InteractiveGMT.GMT
	import InteractiveGMT.GMT.Dates: DateTime, datetime2julian
	const R = InteractiveGMT.RemoteS

	@test R.guess_increment_from_coordvecs([1., 1, 1, 1], [1., 1, 1, 1]) == [1.0, 1.0]
	@test R.helper_find_sds("AA", "xxxxxxxx:AA", findall("\n", "aA\nbbbbbnnn\n")) == "xxx:AA"
	@test R.assign_description(["LC08_B2.TIF", "LC08_B5.TIF"], String[])[1] == "Band2 - Blue [0.45-0.51]"
	@test R.assign_description(["AC08_B2.TIF", "AC08_B5.TIF"], String[])  == ["AC08_B2", "AC08_B5"]
	@test R.assign_description(["AC08_B2.TIF", "AC08_B5.TIF"], ["B2", "B5"]) == ["B2", "B5"]
	@test R.reportbands(mat2img(rand(UInt16, 4,4,3), names=["Band1", "Band2", "Band3"]), 3)[1] == "Band3"
	@test R.reportbands(mat2img(rand(UInt16, 4,4,3), names=["Band1", "Band2", "Band3"]), layers=[1,3]) == ["Band1", "Band3"]
	@test_throws ErrorException("Bad input type Symbol. Must be a DateTime, a String or a Tuple(Int)") R.getitDTime(:val)
	@test R.get_sat_name(Dict{Symbol,Any}(:sat => "TERRA")) == "TERRA"
	@test R.get_MODIS_scene_name(datetime2julian(DateTime("2020-09-20")), "A") == "AQUA_MODIS.20200920T000001.L2.SST.NRT.nc"
	@test R.get_MODIS_scene_name(datetime2julian(DateTime("2020-09-20")), "A", false) == "AQUA_MODIS.20200920T000001.L2.OC.NRT.nc"
end

@testitem "RemoteS: spectral indices, one engine for every input form" tags=[:unit, :remotes] begin
	using InteractiveGMT, InteractiveGMT.GMT
	const R = InteractiveGMT.RemoteS

	names = [R.Lsat8_desc[k] for k = 1:7]
	cube = mat2img(rand(UInt16, 24, 20, 7) .÷ UInt16(2) .+ UInt16(1000), names=names, noconv=1)
	band(k) = mat2img(cube.image[:,:,k], cube)
	B, G, Rd, NIR, SW1, SW2 = band(2), band(3), band(4), band(5), band(6), band(7)

	# The cube path and the explicit-band path must agree (they share `_sp_compute`).
	@test R.ndvi(cube).z == R.ndvi(Rd, NIR).z
	@test R.evi(cube).z  == R.evi(B, Rd, NIR).z
	@test R.savi(cube).z == R.savi(Rd, NIR).z
	@test R.gli(cube).z  == R.gli(Rd, G, B).z
	@test R.satvi(cube).z == R.satvi(Rd, SW1, SW2).z
	# The (1st - 2nd) differences: explicit bands are passed in their documented order.
	@test R.ndwi(cube).z  == R.ndwi(G, NIR).z
	@test R.mndwi(cube).z == R.mndwi(G, SW2).z
	@test R.nbri(cube).z  == R.nbri(NIR, SW2).z
	@test R.ndwi2(cube).z == R.ndwi2(NIR, SW2).z

	# The published formulas, on plain Float matrices (no scaling).
	r = fill(0.2f0, 3, 3);  n = fill(0.6f0, 3, 3)
	@test R.ndvi(r, n)[1] ≈ (0.6 - 0.2) / (0.6 + 0.2)
	@test R.savi(r, n)[1] ≈ (0.6 - 0.2) * 1.5 / (0.6 + 0.2 + 0.5)
	@test R.evi2(r, n)[1] ≈ 2.5 * (0.6 - 0.2) / (0.6 + 2.4 * 0.2 + 1)
	@test R.msavi(r, n)[1] ≈ (2 * 0.6 + 1 - sqrt((2 * 0.6 + 1)^2 - 8 * (0.6 - 0.2))) / 2
	@test R.ndvi(r, n) isa Matrix{Float32}

	# threshold / mask / classes
	nd = R.ndvi(cube).z
	m = R.ndvi(cube, threshold=0.0, mask=true)
	@test m isa GMTimage{UInt8,2}
	@test count(==(0xff), m.image) == count(>(0), nd)
	@test count(==(0xff), R.ndvi(cube, threshold=0.0, mask=-1).image) == count(<(0), nd)
	@test count(isnan, R.ndvi(cube, threshold=0.0).z) == count(<(0), nd)
	c = R.ndvi(cube, classes=[-0.5, 0.0, 0.5])
	@test c isa GMTimage{UInt8,2}
	@test c.image == UInt8[(v >= -0.5) + (v >= 0.0) + (v >= 0.5) for v in nd]
	@test_throws ErrorException R.ndvi(cube, mask=true)				# a mask needs a threshold
end

@testitem "RemoteS: an N/S flip is a layout change, never a reversed buffer" tags=[:unit, :remotes] begin
	using InteractiveGMT, InteractiveGMT.GMT
	const R = InteractiveGMT.RemoteS
	@test R._flip_ns_layout("TRB") == "BRB"
	@test R._flip_ns_layout("BRB") == "TRB"
	@test R._flip_ns_layout("") == "BRB"			# gd2gmt's default is TRB
	G = gd2gmt(gmt2gd(mat2grid(rand(Float32, 7, 5), x=collect(0.0:4), y=collect(10.0:16))))
	old = deepcopy(G);  old.z = old.z[:, end:-1:1]		# what grid_at_sensor used to do
	new = deepcopy(G);  new.layout = R._flip_ns_layout(new.layout)
	@test new.z === G.z || new.z == G.z					# buffer untouched
	@test InteractiveGMT._zmat(new) == InteractiveGMT._zmat(old)
end

@testitem "RemoteS: truecolor" tags=[:unit, :remotes] begin
	using InteractiveGMT, InteractiveGMT.GMT
	const R = InteractiveGMT.RemoteS
	I = R.truecolor(mat2img(rand(UInt16,32,32)), mat2img(rand(UInt16,32,32)), mat2img(rand(UInt16,32,32)))
	@test I isa GMTimage{UInt8,3} && size(I) == (32, 32, 3)
	@test size(R.truecolor(mat2img(rand(UInt16, 16, 16, 3), noconv=1), [1,2,3])) == (16, 16, 3)
	G1 = mat2grid(rand(Float32, 16,16)); G2 = mat2grid(rand(Float32, 16,16)); G3 = mat2grid(rand(Float32, 16,16))
	@test R.truecolor(G1, G2, G3) isa GMTimage{UInt8,3}
end

@testitem "RemoteS: CART classification tree" tags=[:unit, :remotes] begin
	using InteractiveGMT, InteractiveGMT.GMT
	const R = InteractiveGMT.RemoteS

	# Two features, three classes cut by x1 < 0.3, x1 >= 0.3 & x2 < 0.5, else.
	X = [0.1 0.1; 0.2 0.9; 0.25 0.4; 0.4 0.1; 0.6 0.2; 0.5 0.45; 0.4 0.8; 0.7 0.9; 0.9 0.6]
	y = UInt8[1, 1, 1, 2, 2, 2, 3, 3, 3]
	m = R.fit_tree(X, y)
	@test R.predict(m, X) == y
	@test R.predict(m, [0.05 0.5; 0.8 0.3; 0.8 0.7]) == UInt8[1, 2, 3]
	P = R.predict_proba(m, X)
	@test all(sum(P, dims=2) .≈ 1)
	@test P[1, 1] == 1.0
	m1 = R.fit_tree(X, y; max_depth=1)								# a stump: one split, two leaves
	@test count(==(0), m1.feature) == 2

	# classify() reads each pixel's band vector straight out of the cube.
	cube = mat2img(cat(fill(UInt16(10), 4, 4), fill(UInt16(20), 4, 4), dims=3), noconv=1)
	cube.image[1:2, :, 1] .= 90
	mc = R.fit_tree([10.0 20; 90 20], UInt8[1, 2])
	I = R.classify(cube, mc)
	@test I.image == [fill(0x02, 2, 4); fill(0x01, 2, 4)]
end

@testitem "RemoteS: orbits on InteractiveGMT's SGP4" tags=[:unit, :remotes, :satellite] begin
	using InteractiveGMT, InteractiveGMT.GMT
	import InteractiveGMT.GMT.Dates: DateTime
	const R = InteractiveGMT.RemoteS
	tle = ["1 27424U 02022A   21245.83760660  .00000135  00000-0  39999-4 0  9997",
	       "2 27424  98.2123 186.0654 0002229  67.6025 313.3829 14.57107527 28342"]	# AQUA, Sept 2021

	orb = R.sat_tracks(tle=tle, start=DateTime("2021-09-02T13:30:00"), duration=100)
	@test size(orb) == (201, 4) && orb.colnames == ["lon", "lat", "alt", "JulianDay"]
	@test all(690e3 .< orb.data[:,3] .< 740e3)		# AQUA: ~705 km, +~25 km over the poles (ellipsoidal height)
	g = R.sat_tracks(geocentric=true, tle=tle, start=DateTime("2021-09-02T13:30:00"), duration="10m", step="1m")
	@test size(g) == (11, 4)
	@test all(7050e3 .< sqrt.(sum(g.data[:,1:3].^2, dims=2)) .< 7100e3)		# |r| = R_earth + ~705 km
	@test size(R.sat_tracks(position=true, tle=tle, start=DateTime("2021-09-02T13:30:00"))) == (1, 4)

	o1 = R.sat_tracks(tle=tle, start=DateTime("2021-09-02T13:30:00"), stop=DateTime("2021-09-02T13:40:00"), step="1m")
	S = R.sat_scenes(o1, "AQUA")
	@test [s.header for s in S] == ["AQUA_MODIS.20210902T133001.L2.SST.NRT.nc", "AQUA_MODIS.20210902T133501.L2.SST.NRT.nc"]

	# The example in findscenes' own docstring, answer and all: the two day passes over (-8, 36) in the
	# two days BEFORE 2021-09-07T17:00, in OB.DAAC's current naming (the seconds are the guess; the GUI
	# resolves them). The original RemoteS searched [start, start+2 days] instead — the future.
	@test R.findscenes(-8, 36, start="2021-09-07T17:00:00", sat=:aqua, day=true, duration=-2, oc=1, tle=tle) ==
	      ["AQUA_MODIS.20210906T131001.L2.OC.NRT.nc", "AQUA_MODIS.20210907T135001.L2.OC.NRT.nc"]
end
