# CI-safe unit tests for Grid Tools > SDG (sdg.jl, port of Mirone's mirone.m GridToolsSDG_CB).
# Nothing here opens a Qt+VTK window: the ported `csaps` cubic smoothing spline, its banded solver
# and the SDG itself are pure Julia, so they run anywhere `using InteractiveGMT` succeeds. Only the
# `p` prompt and the menu wiring are C++.
#
# The spline is checked against cases whose answer is known in closed form, not against a stored
# reference: p = 1 must INTERPOLATE (and be exact on data that is itself a cubic), p = 0 must give
# the least-squares STRAIGHT LINE, and the default `p` must match csaps's documented estimate. The
# SDG is then checked on surfaces whose second derivative along the gradient is known analytically.

@testitem "sdg: helpers present" tags=[:unit, :fast] begin
	for s in (:_csaps_bands, :_csaps_ldl!, :_csaps_ldl_solve!, :csaps_p_guess, :csaps_nodes,
	          :_csaps_setup, :_csaps_finish, :_csaps_default_p,
	          :spline_smooth, :_on_spline_smooth, :sdg, :_on_sdg)
		@test isdefined(InteractiveGMT, s)
	end
end

@testitem "sdg: the banded LDL' solves the csaps system" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# Solve (6(1-p)Q'Q + pR)u = rhs with the band solver, then multiply back with a DENSE matrix
	# rebuilt from the same bands. Catches any index slip in the factorisation independently of
	# whether the spline it feeds looks plausible.
	dx = [0.4, 0.1, 0.7, 0.3, 0.9, 0.2, 0.5]        # deliberately non-uniform
	for p in (0.0, 0.3, 0.97, 1.0)
		d, a, b = IG._csaps_bands(dx, p)
		N = length(d)
		M = zeros(N, N)
		for i = 1:N
			M[i, i] = d[i]
			(i < N)     && (M[i, i+1] = M[i+1, i] = a[i])
			(i < N - 1) && (M[i, i+2] = M[i+2, i] = b[i])
		end
		rhs = Float64[1.0, -2.0, 0.5, 3.0, -1.5][1:min(5, N)]
		U = reshape(vcat(rhs, zeros(N - length(rhs)))[1:N], N, 1)
		want = copy(U)
		IG._csaps_ldl!(d, a, b)                      # destroys d,a,b -> the factors
		IG._csaps_ldl_solve!(U, d, a, b)
		@test M * U ≈ want  rtol=1e-10
	end
end

@testitem "sdg: csaps with p=1 is the interpolating natural cubic spline" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	x = collect(0.0:0.25:5.0);  n = length(x)
	# A straight line IS a natural cubic spline, so p=1 must reproduce it EXACTLY: values, slope 2
	# and a second derivative that is identically zero.
	V = reshape(3.0 .+ 2.0 .* x, n, 1)
	f, f1, f2 = IG.csaps_nodes(x, V, 1.0)
	@test f  ≈ V            atol=1e-12
	@test all(≈(2.0; atol = 1e-10), f1)
	@test all(≈(0.0; atol = 1e-10), f2)
	# On x^2 the natural end condition (f''=0 at the ends) is WRONG, so only the interior may be
	# checked — but the interpolation of the data itself is still exact everywhere.
	V = reshape(x .^ 2, n, 1)
	f, f1, f2 = IG.csaps_nodes(x, V, 1.0)
	@test f ≈ V  atol=1e-12
	k = 4:(n - 3)
	@test maximum(abs.(f1[k] .- 2 .* x[k])) < 1e-2
	@test maximum(abs.(f2[k] .- 2))         < 1e-1
end

@testitem "sdg: csaps with p=0 is the least-squares straight line" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	x = collect(0.0:0.25:5.0);  n = length(x)
	V = reshape(x .^ 2, n, 1)
	f, f1, f2 = IG.csaps_nodes(x, V, 0.0)
	@test all(≈(0.0; atol = 1e-9), f2)                       # a line has no curvature
	@test maximum(f1) - minimum(f1) < 1e-9                   # ...so its slope is constant
	# The LS fit of x^2 over [0,5] has slope a+b = 5 and passes through the data's centroid.
	@test f1[1] ≈ 5.0  atol=1e-8
	@test sum(f .- V) ≈ 0.0  atol=1e-8
end

@testitem "sdg: csaps smooths monotonically in p" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	x = collect(0.0:0.1:6.0);  n = length(x)
	V = reshape(3.0 .+ 2.0 .* x .+ 0.1 .* sin.(37 .* x), n, 1)     # a line plus rough noise
	resid(p) = (f = IG.csaps_nodes(x, V, p)[1];  sqrt(sum((f .- V) .^ 2) / n))
	rough(p) = (f2 = IG.csaps_nodes(x, V, p)[3];  maximum(abs.(f2)))
	r = resid.((1.0, 0.999, 0.99, 0.5, 0.0))
	@test r[1] < 1e-12                                        # p=1 interpolates: no residual
	@test issorted(r)                                         # less p  ->  more residual
	@test issorted(rough.((0.0, 0.5, 0.99, 0.999, 1.0)))      # ...and more roughness allowed
end

@testitem "sdg: csaps_p_guess matches csaps's own estimate" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# For uniform spacing h the estimate 1/(1 + trace(R)/(6·trace(Q'Q))) collapses to 1/(1 + h^3/9),
	# independently of how many nodes it is read off — which is why GridToolsSDG_CB takes it from a
	# 5-node corner of the grid.
	for h in (0.25, 1.0, 1/60, 3.0)
		@test IG.csaps_p_guess(collect(0.0:h:(4h)))  ≈ 1 / (1 + h^3 / 9)  rtol=1e-12
		@test IG.csaps_p_guess(collect(0.0:h:(40h))) ≈ 1 / (1 + h^3 / 9)  rtol=1e-12
	end
	@test_throws ErrorException IG.csaps_p_guess([0.0, 1.0])          # needs at least 3 sites
end

@testitem "sdg: the SDG of surfaces with a known answer" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	x = collect(0.0:0.1:4.0);  y = collect(-2.0:0.2:2.0)
	mk(f) = GMT.mat2grid(Float32[f(xx, yy) for yy in y, xx in x]; x = x, y = y)
	# The ends of a natural spline are pinned to zero curvature, so compare away from them.
	k = 6:(length(y) - 5);  l = 6:(length(x) - 5)
	# Tolerances are set by the grid's Float32 STORAGE, not by the method: one Float32 ulp on a z of
	# ~10 is ~1e-6, and a second difference divides it by dx^2 = 0.01 -> ~1e-4 of unavoidable noise.
	# (Run in Float64 the same three cases come out at 1e-13.)

	# Paraboloid: the gradient is radial, the Hessian is 2I  ->  SDG == 2 everywhere.
	R = IG.sdg(mk((a, b) -> a^2 + b^2); p = 1.0)
	@test size(R.z) == (length(y), length(x))
	@test all(v -> abs(v - 2) < 5e-3, R.z[k, l])

	# A plane has no second derivative at all  ->  SDG == 0, borders included.
	R = IG.sdg(mk((a, b) -> 3a + 2b); p = 1.0)
	@test maximum(abs.(R.z)) < 1e-3

	# Saddle f = x*y:  v = [y,x], H = [0 1; 1 0]  ->  SDG = 2xy/(x^2+y^2).
	R = IG.sdg(mk((a, b) -> a * b); p = 1.0)
	T = Float64[2 * xx * yy / (xx^2 + yy^2) for yy in y, xx in x]
	@test maximum(abs.(R.z[k, l] .- T[k, l])) < 1e-3
end

@testitem "sdg: smoothing is what makes the SDG usable" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	x = collect(0.0:0.1:4.0);  y = collect(-2.0:0.2:2.0)
	k = 6:(length(y) - 5);  l = 6:(length(x) - 5)
	# Deterministic node-to-node "noise" — an LCG written out rather than Random.seed!, so the test
	# needs no Random in the test environment and is bit-identical on every run.
	Z = Float32[xx^2 + yy^2 for yy in y, xx in x]
	# (the multiplier/increment MUST stay UInt32: an Int literal would promote the state to Int64 and
	# the "noise" would grow without wrapping)
	let seed = UInt32(12345)
		for i in eachindex(Z)
			seed = UInt32(1664525) * seed + UInt32(1013904223)
			Z[i] += Float32(0.04) * (Float32(seed >> 8) / Float32(1 << 24) - 0.5f0)   # ±0.02
		end
	end
	G = GMT.mat2grid(Z; x = x, y = y)
	raw = IG.sdg(G; p = 1.0).z[k, l]                          # p=1 interpolates the noise too
	sm  = IG.sdg(G).z[k, l]                                   # default p = csaps_p_guess
	@test maximum(abs.(raw .- 2)) > 10                        # unusable without smoothing
	@test maximum(abs.(sm  .- 2)) < 2                         # ...and around the true value with it
end

@testitem "sdg: Positive / Negative / Both are the same field, sign-masked" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	x = collect(0.0:0.1:4.0);  y = collect(-2.0:0.2:2.0)
	G = GMT.mat2grid(Float32[xx * yy for yy in y, xx in x]; x = x, y = y)     # saddle: both signs
	both = IG.sdg(G; p = 1.0, sign = :both).z
	pos  = IG.sdg(G; p = 1.0, sign = :positive).z
	neg  = IG.sdg(G; p = 1.0, sign = :negative).z
	@test any(both .> 0) && any(both .< 0)                    # the test surface really has both
	@test all(pos .>= 0) && all(neg .<= 0)
	@test pos .+ neg ≈ both                                   # nothing else was touched
	@test_throws ErrorException IG.sdg(G; sign = :sideways)
end

@testitem "spline_smooth: p=1 returns the data, p=0 the least-squares trend" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	x = collect(0.0:0.1:4.0);  y = collect(-2.0:0.2:2.0)
	mk(f) = GMT.mat2grid(Float32[f(xx, yy) for yy in y, xx in x]; x = x, y = y)
	# p = 1 is an INTERPOLATING spline: evaluated at its own data sites it gives the data back.
	G = mk((a, b) -> a^2 + b^2)
	@test IG.spline_smooth(G; p = 1.0).z ≈ G.z  atol=1e-4
	# p = 0 is the least-squares straight line in EACH direction, i.e. the bilinear trend — which a
	# plane already is, so the plane survives p = 0 untouched...
	P = mk((a, b) -> 3a + 2b)
	@test IG.spline_smooth(P; p = 0.0).z ≈ P.z  atol=1e-3
	# ...while the paraboloid is flattened onto that trend (no curvature left in either direction).
	S0 = IG.spline_smooth(G; p = 0.0)
	@test maximum(abs.(S0.z .- G.z)) > 1
	col = Float64.(S0.z[:, 10])                                   # a column: constant second difference
	@test maximum(abs.(diff(diff(col)))) < 1e-3
end

@testitem "spline_smooth: it actually smooths, and keeps geometry + NaNs" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	x = collect(0.0:0.1:4.0);  y = collect(-2.0:0.1:2.0)
	Z = Float32[xx^2 + yy^2 for yy in y, xx in x]
	clean = copy(Z)
	let seed = UInt32(777)                                        # UInt32 throughout (see above)
		for i in eachindex(Z)
			seed = UInt32(1664525) * seed + UInt32(1013904223)
			Z[i] += Float32(0.2) * (Float32(seed >> 8) / Float32(1 << 24) - 0.5f0)   # ±0.1
		end
	end
	Z[10:14, 12:18] .= NaN32                                      # a hole, as a real grid has
	G = GMT.mat2grid(Z; x = x, y = y)
	S = IG.spline_smooth(G)                                       # default p = csaps's estimate
	@test S isa GMT.GMTgrid
	@test size(S.z) == size(Z)
	@test S.range[1:4] ≈ G.range[1:4]
	@test isnan.(S.z) == isnan.(Z)                                # law of NaN conservation
	# The whole point: closer to the underlying surface than the noisy data was.
	ok = .!isnan.(Z)
	@test sum(abs2, S.z[ok] .- clean[ok]) < sum(abs2, Z[ok] .- clean[ok])
end

@testitem "sdg: the result keeps the grid's geometry, and its NaNs" tags=[:unit, :fast] begin
	IG = InteractiveGMT; GMT = IG.GMT
	x = collect(0.0:0.1:4.0);  y = collect(-2.0:0.1:2.0)
	Z = Float32[xx^2 + yy^2 for yy in y, xx in x]
	Z[10:14, 12:18] .= NaN32                                  # a hole, as in a real bathymetry grid
	G = GMT.mat2grid(Z; x = x, y = y)
	R = IG.sdg(G; p = 1.0)
	@test R isa GMT.GMTgrid
	@test size(R.z) == size(Z)
	@test R.range[1:4] ≈ G.range[1:4]
	@test R.inc ≈ G.inc
	# "The law of NaN conservation": the holes are filled to compute, then punched back exactly.
	@test isnan.(R.z) == isnan.(Z)
end
