#!/usr/bin/env julia
#
# Catalina benchmark 1 — the DIGITISED TABLE against the CARRIER–GREENSPAN ANALYTIC SOLUTION, at the
# three times the table carries (160, 175, 220 s).
#
#   julia examples/bm1_table_vs_analytic.jl [outdir]        free surface  -> bm1_table_vs_analytic.png
#   julia examples/bm1_table_vs_analytic_u.jl [outdir]      velocity      -> bm1_table_vs_analytic_u.png
#
# Both drivers call `bm1_compare` below — one implementation, two quantities, so the two figures can
# never drift apart in what they sample, mask or scale.
#
# WHAT IS BEING COMPARED
#   Table    `InteractiveGMT._BM1_ANALYTIC`, the literal of Mirone's testa_barnabeu.m `analytic()`:
#            (x, eta, u) triples per time. THE BENCHMARK'S PUBLISHED REFERENCE — where it and
#            `catalina1` disagree, the burden is on `catalina1` (docs/src/80-benchmark1.md).
#   Analytic `InteractiveGMT.CatalinaBenchmark1.catalina1(t)` (src/benchmark1_analytic.jl), the
#            nonlinear Carrier–Greenspan solution evaluated at an arbitrary time.
#
# SIGN CONVENTION. The table's velocity is positive OFFSHORE (+x, the same convention nswing's U
# component uses); `catalina1` returns it positive SHOREWARD. The analytic velocity is therefore
# negated here so both curves mean the same thing — checked, not assumed: cos-similarity between the
# two u columns is -0.99 at every benchmark time.

using InteractiveGMT, GMT

const IG = InteractiveGMT
const CB = InteractiveGMT.CatalinaBenchmark1

"""
    bm1_compare(quantity = :eta; outdir = "C:/TMP/claude", npoints = 4000) -> String

Draw the table (solid blue) against the analytic solution (dashed red) at t = 160/175/220 s, two
panels per time: the whole profile out to 25 km, and the beach (-300 … 1500 m), where the two part
company. Prints the rms/max misfit per time and returns the PNG's path.

`quantity` is `:eta` (free surface, metres) or `:u` (depth-averaged velocity, m/s, positive offshore).
"""
function bm1_compare(quantity::Symbol = :eta; outdir::String = raw"C:\TMP\claude",
                     npoints::Int = 4000)::String
	quantity in (:eta, :u) || error("bm1_compare: quantity must be :eta or :u, got :$quantity")
	mkpath(outdir)
	png = joinpath(outdir, quantity === :eta ? "bm1_table_vs_analytic.png"
	                                         : "bm1_table_vs_analytic_u.png")
	T     = IG._BM1_ANALYTIC
	times = (160.0, 175.0, 220.0)
	cols  = (4, 7, 10)                        # first column of each time's (x, eta, u) triple
	off   = quantity === :eta ? 1 : 2         # eta is the 2nd of the triple, u the 3rd
	ylab  = quantity === :eta ? "eta (m)" : "u (m/s)"
	what  = quantity === :eta ? "free surface" : "velocity (positive offshore)"

	# The analytic curve for one time, as (x, value) with the sign convention fixed — the ONE place
	# either quantity is taken off the solution.
	function analytic(t::Float64)
		R = CB.catalina1(t; xmax = 30000.0, npoints = npoints)
		return (R.x, quantity === :eta ? R.eta : -R.velocity)
	end

	leg() = (symbol1 = (marker="-", size=0.6, dx_left=0.0, pen=(1,:blue),      dx_right=0.3, text="Table"),
	         symbol2 = (marker="-", size=0.6, dx_left=0.0, pen=(1,:red,:dash), dx_right=0.3, text="Analytic"))

	println("Catalina benchmark 1 — table vs analytic, ", what)
	gmtbegin(png)
	subplot(grid=(3,2), F=(width=22, height=26), margins="0.6c/0.6c/0.4c/2.4c")
	for k in 1:3
		t, c = times[k], cols[k]
		xa, va = analytic(t)
		xt, vt = T[:, c], T[:, c + off]
		o = sortperm(xt)

		# the misfit, on the TABLE's own x (the analytic sampled onto it with GMT's sample1d)
		vi = IG._bm1_interp1(xa, va, xt; interp = :linear)
		ok = .!isnan.(vi) .& (xt .<= 40000.0)
		d  = vi[ok] .- vt[ok]
		println("  t = ", Int(t), " s   n = ", count(ok),
		        "   rms = ", round(sqrt(sum(abs2, d) / length(d)), digits = 3),
		        "   max|d| = ", round(maximum(abs, d), digits = 3))

		for (col, rgx) in ((1, (0.0, 25000.0)), (2, (-300.0, 1500.0)))
			kt = (xt[o] .>= rgx[1]) .& (xt[o] .<= rgx[2])
			ka = (xa   .>= rgx[1]) .& (xa   .<= rgx[2])
			ylo = min(minimum(vt[o][kt]), minimum(va[ka]))
			yhi = max(maximum(vt[o][kt]), maximum(va[ka]))
			pad = 0.08 * (yhi - ylo)
			plot(xt[o][kt], vt[o][kt]; panel=(k, col), region=(rgx[1], rgx[2], ylo - pad, yhi + pad),
			     lc=:blue, lw=1, frame=(axes=:WSen,), xlabel="x (m)", ylabel=ylab,
			     title = col == 1 ? "t = $(Int(t)) s   (full profile)" : "t = $(Int(t)) s   (beach)")
			plot!(xa[ka], va[ka]; panel=(k, col), lc=:red, ls=:dash, lw=1)
			legend(leg(); position=(inside=:TR, width=0.0))
		end
	end
	subplot("end")
	gmtend()
	println("wrote ", png)
	return png
end

if abspath(PROGRAM_FILE) == @__FILE__
	bm1_compare(:eta; outdir = isempty(ARGS) ? raw"C:\TMP\claude" : ARGS[1])
end
