# Geography > Plot coastline / political boundaries / rivers: the lines are TRIMMED to the region the
# menu sends (the active axes' limits). GMT's `coast -M` cuts shorelines at -R itself but returns every
# border and river segment that merely crosses it whole — so this pins the trim for all three.

@testitem "Geography: coastlines, boundaries and rivers are trimmed to the axes" tags=[:unit, :geography] begin
	IG = InteractiveGMT
	W, E, S, N = -10.0, -5.0, 36.0, 42.0
	for kind in ("coast", "borders:1", "borders:a", "rivers:r", "rivers:a")
		D = IG._geo_dataset(kind, :i, W, E, S, N)
		@test D !== nothing && !isempty(D)
		xs = vcat([d.data[:, 1] for d in D]...);  ys = vcat([d.data[:, 2] for d in D]...)
		@test all(W - 1e-9 .<= xs .<= E + 1e-9) && all(S - 1e-9 .<= ys .<= N + 1e-9)
	end
	# No intermittent rivers there: nothing, not an error.
	@test IG._geo_dataset("rivers:5", :i, W, E, S, N) === nothing
	# Each layer is named after its menu entry.
	@test IG._geo_layer_name("coast") == "Coastlines"
	@test IG._geo_layer_name("borders:1") == "National boundaries"
	@test IG._geo_layer_name("rivers:r") == "All permanent rivers"
end
