# CI-safe unit tests for Plates > Euler rotations (src/plates.jl). Everything here runs without a
# window: the callback wiring, the two pole-file column orders in circulation, and the two pole-algebra
# tabs (Add poles / Interpolate poles), which are pure GMT calls and need no scene. The rotation of a
# real line needs a live window — that is the :gui item in test-plates-gui.jl.

@testitem "Euler rotations: callback is wired" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	for s in (:_on_euler, :_register_euler, :_euler_rotate, :_euler_add, :_euler_interp,
	          :_euler_segments, :_euler_read_poles)
		@test isdefined(IG, s)
	end
	# A registration without its export in the symbol list stays silently "not wired" at runtime.
	for sym in (:gmtvtk_set_euler_callback, :gmtvtk_euler_result, :gmtvtk_serialize_vector_h)
		@test sym in IG._LIB_SYMBOLS
	end
end

@testitem "Euler poles: both column orders in circulation are read" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# GMT writes total reconstruction poles "lon lat age angle", Mirone "lon lat angle age".
	gmt_order = [129.9 68.0 37.0 -7.8; 142.8 50.8 48.0 -9.8; 145.0 40.0 53.0 -11.4]
	mir_order = [129.9 68.0 -7.8 37.0; 142.8 50.8 -9.8 48.0; 145.0 40.0 -11.4 53.0]
	@test IG._euler_poles_gmt_order(gmt_order) == gmt_order
	@test IG._euler_poles_gmt_order(mir_order) == gmt_order

	f = joinpath(tempdir(), "igmt_poles_$(time_ns()).dat")
	try
		write(f, "#EUR-NAM\n129.9  68.0   -7.8   37   !An13\n142.8  50.8   -9.8   48\n")
		P = IG._euler_read_poles(f)
		@test size(P) == (2, 4)
		@test P[1, :] ≈ [129.9, 68.0, 37.0, -7.8]      # age moved into GMT's column 3
		@test P[2, :] ≈ [142.8, 50.8, 48.0, -9.8]
	finally
		isfile(f) && rm(f, force = true)
	end
end

@testitem "Euler: Add poles == rotconverter's composition" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	d = Dict("p1lon" => "30", "p1lat" => "50", "p1ang" => "10",
	         "p2lon" => "20", "p2lat" => "40", "p2ang" => "5", "showcmd" => "0")
	@test IG._euler_add(d) == 1
	# Same command, run here: the tab must not massage the answer on its way through.
	R = IG.GMT.gmt("rotconverter 30/50/10 + 20/40/5 -N")
	D = isa(R, Vector) ? R[1].data : R.data
	@test D[1, 1] ≈ 25.8532 atol = 1e-3        # == Mirone add_poles(p1, p2) for the same input
	@test D[1, 2] ≈ 46.9853 atol = 1e-3
	@test D[1, 3] ≈ 14.9241 atol = 1e-3
end

@testitem "Euler: interpolated poles land ON the model's own poles" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# Cox's EUR-NAM finite poles, in Mirone's column order (lon lat angle age).
	poles = "129.9 68.0 -7.8 37; 142.8 50.8 -9.8 48; 145.0 40.0 -11.4 53; " *
	        "150.1 70.5 -20.3 83; 152.9 75.5 -24.2 90"
	out = joinpath(tempdir(), "igmt_interp_$(time_ns()).dat")
	try
		d = Dict("poles" => poles, "polesfile" => "", "ages" => "10,37,48,95",
		         "outfile" => out, "showcmd" => "0")
		@test IG._euler_interp(d) == 1
		@test isfile(out)
		rows = [split(l) for l in eachline(out) if !startswith(l, '#')]
		@test length(rows) == 3                       # 95 Ma is past the model: no extrapolation
		v(r) = parse.(Float64, r)
		# Younger than the first pole: that pole with its angle scaled by age/t1 (Mirone's branch).
		@test v(rows[1])[1:2] ≈ [129.9, 68.0] atol = 1e-4
		@test v(rows[1])[3] ≈ -7.8 * 10 / 37 atol = 1e-3
		# Asked exactly AT a pole's age, the interpolation must return that pole unchanged.
		@test v(rows[2]) ≈ [129.9, 68.0, -7.8, 37.0] atol = 1e-3
		@test v(rows[3]) ≈ [142.8, 50.8, -9.8, 48.0] atol = 1e-3
	finally
		isfile(out) && rm(out, force = true)
	end
end

@testitem "Euler: finite poles -> a stage-pole file the rotations can read" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	poles = "129.9 68.0 -7.8 37; 142.8 50.8 -9.8 48; 145.0 40.0 -11.4 53"
	base = Dict("poles" => poles, "half" => "0", "inverse" => "0", "side" => "1",
	            "geodetic" => "0", "showcmd" => "0")
	@test IG._euler_stages(base) == 1
	# The answer is "<path>\n<table>"; the file must be GMT's own 5-column stage format, i.e. exactly
	# what backtracker -E reads back.
	path = first(split(IG._euler_last_result[], '\n'))
	try
		@test isfile(path)
		rows = [parse.(Float64, split(l)) for l in eachline(path) if !startswith(l, '#')]
		# rotconverter writes one stage per pole — the youngest pole's stage runs down to 0 Ma.
		@test length(rows) == 3 && all(length(r) == 5 for r in rows)
		@test all(r -> r[3] > r[4], rows)                 # tstart older than tend
		ref = IG.GMT.gmt("rotconverter $path -Fs")        # readable as a rotation file
		@test size((isa(ref, Vector) ? ref[1].data : ref.data), 2) == 5
		# Half angles halve every opening angle, nothing else.
		half = merge(base, Dict("half" => "1"))
		@test IG._euler_stages(half) == 1
		hpath = first(split(IG._euler_last_result[], '\n'))
		hrows = [parse.(Float64, split(l)) for l in eachline(hpath) if !startswith(l, '#')]
		@test all(i -> isapprox(hrows[i][5], rows[i][5] / 2; atol = 1e-3), 1:length(rows))
		@test all(i -> isapprox(hrows[i][1:4], rows[i][1:4]; atol = 1e-3), 1:length(rows))
		# Inverse stages = the reverse rotation: same poles, opposite angles.
		inv = merge(base, Dict("inverse" => "1"))
		@test IG._euler_stages(inv) == 1
		ipath = first(split(IG._euler_last_result[], '\n'))
		irows = [parse.(Float64, split(l)) for l in eachline(ipath) if !startswith(l, '#')]
		@test all(i -> isapprox(irows[i][5], -rows[i][5]; atol = 1e-3), 1:length(rows))
		rm(hpath, force = true); rm(ipath, force = true)
	finally
		isfile(path) && rm(path, force = true)
	end
	# One pole cannot make a stage — refused through the callback, not a crash.
	@test IG._on_euler(Ptr{Cvoid}(UInt(0xDEADDEAD)), Base.unsafe_convert(Cstring, Base.cconvert(Cstring,
	      "op=stages\npoles=129.9 68.0 -7.8 37\nhalf=0\ninverse=0\nside=1\ngeodetic=0\nshowcmd=0"))) == 0
end

@testitem "Euler: the dialog's target<i> list is read back in order" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG._euler_targets(Dict("target1" => "a", "target2" => "b", "target3" => "c")) == ["a", "b", "c"]
	@test IG._euler_targets(Dict("target" => "solo")) == ["solo"]      # the single-target spelling
	@test IG._euler_targets(Dict("target1" => "a", "target3" => "c")) == ["a"]   # stops at the gap
	@test isempty(IG._euler_targets(Dict{String,String}()))
end

@testitem "Plate calculator: every model's poles table is there and readable" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test length(IG._PLATE_MODELS) == 7
	for (key, file) in IG._PLATE_MODELS
		@test isfile(joinpath(IG._PKGROOT, "data", "plates", file))
		v = IG._plate_poles(key)
		@test !isempty(v)
		@test all(p -> !isempty(p.abbrev) && !isempty(p.name), v)
	end
	# Mirone's column order is "AB Name lat lon rate" — a swapped lat/lon would pass unnoticed unless
	# a known row is checked.
	nu = IG._plate_poles("Nuvel1A")
	af = nu[findfirst(p -> p.abbrev == "AF", nu)]
	@test af.name == "Africa"
	@test af.lat ≈ 59.160 atol = 1e-3
	@test af.lon ≈ -73.174 atol = 1e-3
	@test af.rate ≈ 0.9270 atol = 1e-4
	@test_throws Exception IG._plate_poles("NoSuchModel")
end

@testitem "Plate calculator: relative pole is the angular velocity difference" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# NUVEL-1A Africa relative to Eurasia: the published pole is 21.0N, 20.6W, 0.12 deg/Ma.
	d = Dict("model" => "Nuvel1A", "fix" => "EU", "mov" => "AF")
	@test IG._plate_pole(d) == 1
	c = parse.(Float64, split(IG._euler_last_result[]))
	@test c[1] ≈ -20.61 atol = 0.02
	@test c[2] ≈ 21.03 atol = 0.02
	@test c[3] ≈ 0.1228 atol = 1e-3
	# Swapping the two plates is the SAME rotation the other way: antipodal pole, same rate.
	@test IG._plate_pole(Dict("model" => "Nuvel1A", "fix" => "AF", "mov" => "EU")) == 1
	r = parse.(Float64, split(IG._euler_last_result[]))
	@test r[2] ≈ -c[2] atol = 0.02
	@test abs(abs(r[1] - c[1]) - 180.0) < 0.05
	@test r[3] ≈ c[3] atol = 1e-3
	# A plate against itself does not move: the answer is empty, which clears the dialog's boxes.
	@test IG._plate_pole(Dict("model" => "Nuvel1A", "fix" => "AF", "mov" => "AF")) == 1
	@test isempty(IG._euler_last_result[])
end

@testitem "Plate calculator: an absolute model answers with the plate's own pole" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	nnr = IG._plate_poles("NNR")
	au = nnr[findfirst(p -> p.abbrev == "AU", nnr)]
	# No fixed plate = Mirone's absolute_motion branch: the pole is used verbatim, not composed.
	@test IG._plate_pole(Dict("model" => "NNR", "fix" => "", "mov" => "AU")) == 1
	c = parse.(Float64, split(IG._euler_last_result[]))
	@test c[1] ≈ au.lon atol = 1e-2
	@test c[2] ≈ au.lat atol = 1e-2
	@test c[3] ≈ au.rate atol = 1e-4
	# …and with a fixed plate given, the same model becomes relative ("Make it relative").
	@test IG._plate_pole(Dict("model" => "NNR", "fix" => "EU", "mov" => "AU")) == 1
	@test parse.(Float64, split(IG._euler_last_result[]))[3] != c[3]
end

@testitem "Plate calculator: speed and azimuth at a point" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# Africa/Eurasia off Gibraltar. Mirone's own formula gives 4.20 mm/yr at azimuth 307.6 deg;
	# gmtpmodeler does the same job (and the same geodetic->geocentric conversion) to 0.13%.
	d = Dict("polelon" => "-20.61", "polelat" => "21.03", "polerate" => "0.1228",
	         "lon" => "-9", "lat" => "36", "showcmd" => "0")
	@test IG._plate_velocity(d) == 1
	v = parse.(Float64, split(IG._euler_last_result[]))
	@test v[1] ≈ 4.2 atol = 0.05
	@test v[2] ≈ 307.6 atol = 0.2
	# Azimuth is always reported in 0-360, never gmtpmodeler's -180/180.
	@test 0.0 <= v[2] <= 360.0
	# Doubling the rate doubles the speed and leaves the direction alone.
	@test IG._plate_velocity(merge(d, Dict("polerate" => "0.2456"))) == 1
	v2 = parse.(Float64, split(IG._euler_last_result[]))
	@test v2[1] ≈ 2 * v[1] atol = 0.05
	@test v2[2] ≈ v[2] atol = 0.05
	# On the pole itself there is no motion at all.
	@test IG._plate_velocity(merge(d, Dict("lon" => "-20.61", "lat" => "21.03"))) == 1
	@test parse(Float64, split(IG._euler_last_result[])[1]) ≈ 0.0 atol = 1e-6
end

@testitem "Plate calculator: refuses what it cannot compute" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# GC.@preserve: the cconvert buffer is what the Cstring points at, and nothing else roots it.
	function call(kv)
		s = Base.cconvert(Cstring, join(kv, "\n"))
		GC.@preserve s IG._on_euler(Ptr{Cvoid}(UInt(0xDEADDEAD)), Base.unsafe_convert(Cstring, s))
	end
	@test call(["op=plates", "model=NoSuchModel"]) == 0
	@test call(["op=platepole", "model=Nuvel1A", "fix=EU", "mov=ZZ"]) == 0
	@test call(["op=platevel", "polelon=", "polelat=", "polerate=", "lon=0", "lat=0"]) == 0
	@test call(["op=platevel", "polelon=0", "polelat=0", "polerate=1", "lon=", "lat="]) == 0
	# The plate list is what fills the dialog's two combos: "AB<tab>Name", one per plate.
	@test call(["op=plates", "model=Nuvel1A"]) == 1
	rows = split(IG._euler_last_result[], '\n', keepempty = false)
	@test length(rows) == length(IG._plate_poles("Nuvel1A"))
	@test all(r -> length(split(r, '\t')) == 2, rows)
end

@testitem "Euler: refuses what it cannot rotate" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	call(kv) = IG._on_euler(Ptr{Cvoid}(UInt(0xDEADDEAD)),
	                        Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n")))) |> (r -> IG._errored(r, "FAILED"))
	@test call(["op=rotate", "target=nothing here", "usepole=1", "polelon=0", "polelat=0", "poleang=1"]) == 0
	@test call(["op=add", "p1lon=30", "p1lat=50", "p1ang=10", "p2lon=", "p2lat=", "p2ang="]) == 0
	@test call(["op=interp", "polesfile=no_such_poles.dat", "poles=", "ages=10"]) == 0
	@test call(["op=whatever"]) == 0
end
