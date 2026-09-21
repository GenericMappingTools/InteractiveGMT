# THE CATALINA BENCHMARK 1 REFERENCE SOLUTION, AND WHAT IT COSTS.
#
# `catalina1` is evaluated INSIDE the Aquamoto slice loop — the η(x) figure asks for it once per
# settled slice, over a blocking bridge — so its cost is not an academic number: it is time the
# transport does not have. It is also the one curve the whole demo is judged against, so it may not
# be made cheap by being made different.
#
# Both halves are pinned here: the mode sum is truncated where it contributes nothing, and the
# profile that comes out must still equal the untruncated one.

@testitem "benchmark 1: the spectrum drops only modes that contribute nothing" tags=[:unit, :fast] begin
	M = InteractiveGMT.CatalinaBenchmark1
	P = M.Parameters()
	S = M.default_spectrum()
	# TRUNCATED, and by its own tail: every mode kept is above the 1e-12 relative floor, and the last
	# one kept is the last one that is.
	@test length(S.omega) == length(S.Bw)
	@test length(S.Bw) < 3001                       # the untruncated quadrature's nomega
	mx = maximum(abs, S.Bw)
	@test abs(S.Bw[end]) > 1e-12 * mx
	# …AND THE PROFILE IS THE SAME CURVE. The untruncated spectrum comes from the SAME function, with
	# the same quadrature and the same arithmetic — only the tail is kept — so what this compares is
	# the truncation and nothing else.
	full = M.spectrum(P; truncate = false)
	@test length(full.Bw) == 3001
	@test full.Bw[1:length(S.Bw)] == S.Bw          # the kept head is untouched, bit for bit
	# `snapshot` returns METRES already: column 1 is x*reference_length, column 2 eta*zscale.
	for t in (137.0, 175.0, 220.0)
		A = M.snapshot(S,    t, P; xmax = 4.2, npoints = 400)
		F = M.snapshot(full, t, P; xmax = 4.2, npoints = 400)
		# OVER THE FLUME, which is the whole of what is ever drawn. The sweep's sigma = 0 row is the
		# shoreline point and the Newton solve there lands wherever it lands — both spectra put it at
		# x = +208 km, eta = -4.2 m at t = 137 s — so it is not a point of the curve: it falls outside
		# the window `_bm1_analytic_curve` samples over, and is left out of the comparison here too.
		keep = (A[:, 1] .>= -500) .& (A[:, 1] .<= 21_000)
		@test count(keep) > 300
		# Measured over the three benchmark times: 1.4 mm in x, 0.14 mm in eta, both at the stiff
		# near-shore rows where the Newton continuation is most sensitive; everywhere else it is
		# 1e-13 m. The demo's own model-vs-analytic figure is quoted in centimetres (0.02 - 0.10 m
		# rms, benchmark1.jl), so this is three orders below anything the comparison can see — and
		# the budget still fails at once if the truncation ever starts cutting real modes.
		@test maximum(abs.(A[keep, 1] .- F[keep, 1])) < 1e-2   # x, metres
		@test maximum(abs.(A[keep, 2] .- F[keep, 2])) < 1e-3   # eta, metres
	end
end

@testitem "benchmark 1: one reference curve is affordable inside the slice loop" tags=[:unit, :fast] begin
	M = InteractiveGMT.CatalinaBenchmark1
	M.catalina1(10.0; xmax = 21000.0, npoints = 400)          # warm: this is not a compile-time test
	t0 = time()
	R  = M.catalina1(137.0; xmax = 21000.0, npoints = 400)
	dt = time() - t0
	@test length(R.x) == 400
	# The figure asks for this between two presses of `<` / `>`. Measured 0.021 s on the development
	# machine (0.147 s before the mode sum was truncated); the budget is loose enough for a slower one
	# and still fails long before the transport feels it.
	@test dt < 0.10
end
