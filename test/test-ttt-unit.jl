# Tsunami travel times: the source parser (tsunamittt.jl).

# A fault line is its vertices PLUS a source half-way along it; a point stays one point.
@testitem "travel times: a fault source gets a point at its middle" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# A point source: just itself.
	M = IG._ttt_source(Dict("srcxy" => "-12,37.5"), true)
	@test size(M) == (1, 2)
	# Geographic: along the equator the geodesic half-way point of 0..2 deg is 1 deg.
	M = IG._ttt_source(Dict("srcxy" => "0,0;2,0"), true)
	@test size(M) == (3, 2)
	@test M[3, 1] ≈ 1.0 atol = 1e-9
	@test M[3, 2] ≈ 0.0 atol = 1e-9
	# Cartesian, two segments of lengths 3 and 1: half of 4 is 2 units into the first one.
	M = IG._ttt_source(Dict("srcxy" => "0,0;3,0;3,1"), false)
	@test size(M) == (4, 2)
	@test M[4, :] ≈ [2.0, 0.0]
	# Cartesian, the half-way mark in the SECOND segment: lengths 1 and 3 -> 1 unit into it.
	M = IG._ttt_source(Dict("srcxy" => "0,0;1,0;1,3"), false)
	@test M[4, :] ≈ [1.0, 1.0]
	# Nothing placed: Mirone's refusal.
	@test_throws ErrorException IG._ttt_source(Dict{String,String}(), true)
end

# The arrival-times table carries tttimes' TEXT column (station name | arrival), both methods' grids
# alike; a table with no text gets no text column at all.
@testitem "travel times: arrival-times table keeps the station names" tags=[:unit] begin
	IG = InteractiveGMT
	G = IG.GMT.mat2grid(fill(-3000.0f0, 31, 41);
		x = collect(range(-14.0, -10.0, length = 41)), y = collect(range(36.0, 39.0, length = 31)))
	st = [-12.0 37.0; -11.0 38.5]
	for R in (IG.GMT.ttt(G, (-13.0, 37.5)), IG.GMT.wave_travel_time(G, (-13.0, 37.5); geo = true))
		D = IG.GMT.tttimes(R, st; names = ["A", "B"])
		t = split(IG._table_text(D), '\n')
		@test length(t) == 2
		@test sort(first.(split.(t, " |"))) == ["A", "B"]
	end
	@test IG._table_text(rand(3, 2)) == ""
	@test IG._table_text(IG.GMT.mat2ds(rand(3, 2))) == ""
end
