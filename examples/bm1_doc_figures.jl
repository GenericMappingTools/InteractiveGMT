#!/usr/bin/env julia
#
# Every figure of the manual's Catalina Benchmark 1 page (docs/src/80-benchmark1.md), from one
# script, so the page and the numbers printed under it can be regenerated after any change to the
# solver, the nest layout or the run.
#
#   julia examples/bm1_doc_figures.jl [rundir] [outdir]
#
#     rundir  a completed 3-level benchmark run: <stem>.nc + <stem>_lev1.nc + <stem>_lev2.nc plus
#             the bathymetry <stem>_bat.grd.  Default C:/TMP/claude/bm1run.
#     outdir  where the PNGs go.  Default docs/src/assets (i.e. straight into the manual).
#
# Curves are ALWAYS blue solid = the reference being tested against, red dashed = the thing under
# test, so the figures all read the same way:
#
#   bm1_setup.png              the flume, and where the three nests sit on it
#   bm1_initial_condition.png  benchmark table t = 0   vs   catalina1(0)     (the model's own start)
#   bm1_table_vs_analytic.png  benchmark table         vs   catalina1(t)     free surface
#   bm1_table_vs_analytic_u.png                        "                     velocity
#   bm1_model_vs_analytic.png  nested model, stitched  vs   catalina1(t)
#   bm1_nesting.png            the stitched curve with each nesting level in its own colour
#
# Titles and labels are DELIBERATELY ASCII-ONLY: non-ASCII passed into a GMT `title=` comes back as
# mojibake in the PNG.

using InteractiveGMT, GMT

const IG = InteractiveGMT
const CB = InteractiveGMT.CatalinaBenchmark1

const TIMES = (160.0, 175.0, 220.0)
const TCOLS = (4, 7, 10)          # first column of each time's (x, eta, u) triple in _BM1_ANALYTIC
const XWIN  = (-200.0, 20000.0)   # the demo's display window

rms(d) = sqrt(sum(abs2, d) / max(length(d), 1))

# Misfit of `ya(xa)` against the reference `yr(xr)`, measured on the REFERENCE's own x -- the one
# place any number quoted on the manual page is computed.
function misfit(xa, ya, xr, yr; xmax = 40000.0)
	yi = IG._bm1_interp1(collect(Float64, xa), collect(Float64, ya), collect(Float64, xr); interp = :linear)
	ok = .!isnan.(yi) .& (collect(Float64, xr) .<= xmax)
	d  = yi[ok] .- collect(Float64, yr)[ok]
	return (n = count(ok), rms = rms(d), max = isempty(d) ? NaN : maximum(abs, d))
end

leg2(a, b) =
	(symbol1 = (marker="-", size=0.6, dx_left=0.0, pen=(1,:blue),      dx_right=0.3, text=a),
	 symbol2 = (marker="-", size=0.6, dx_left=0.0, pen=(1,:red,:dash), dx_right=0.3, text=b))

# Two curves, one panel, auto y-range over the x-window actually drawn.
function pair_panel!(xb, yb, xr, yr, rgx, ttl, xlab, ylab, panel, lg)
	kb = (xb .>= rgx[1]) .& (xb .<= rgx[2])
	kr = (xr .>= rgx[1]) .& (xr .<= rgx[2])
	(!any(kb) || !any(kr)) && return
	ylo = min(minimum(yb[kb]), minimum(yr[kr]));  yhi = max(maximum(yb[kb]), maximum(yr[kr]))
	pad = 0.08 * max(yhi - ylo, eps())
	plot(xb[kb], yb[kb]; panel=panel, region=(rgx[1], rgx[2], ylo - pad, yhi + pad),
	     lc=:blue, lw=1, frame=(axes=:WSen,), xlabel=xlab, ylabel=ylab, title=ttl)
	plot!(xr[kr], yr[kr]; panel=panel, lc=:red, ls=:dash, lw=1)
	legend(lg; position=(inside=:TR, width=0.0))
end

# ---------------------------------------------------------------------------- 1. the flume + nests
function fig_setup(rundir, outdir)
	png = joinpath(outdir, "bm1_setup.png")
	B   = gmtread(joinpath(rundir, "benchmark1_bat.grd"))
	Z   = IG._zmat(B)
	x   = Float64.(B.x);  z = Float64.(Z[max(1, size(Z,1) ÷ 2), :])   # bathymetry varies in x only

	paths = [(0, joinpath(rundir, "benchmark1.nc"),      "level 0  dx = 25 m"),
	         (1, joinpath(rundir, "benchmark1_lev1.nc"), "level 1  dx = 5 m"),
	         (2, joinpath(rundir, "benchmark1_lev2.nc"), "level 2  dx = 1 m")]
	cols  = ("60/60/60", "220/120/0", "200/0/0")

	gmtbegin(png)
	subplot(grid=(2,1), F=(width=22, height=16), margins="0.6c/0.6c/0.4c/1.6c")
	# (a) the whole flume: a plane 1:10 beach, still water at z = 0
	k = x .<= 25000.0
	plot(x[k], z[k]; panel=(1,1), region=(0.0, 25000.0, -2600.0, 200.0), lc=:black, lw=1.25,
	     frame=(axes=:WSen,), xlabel="x (m)", ylabel="bed elevation (m)",
	     title="(a) Catalina BP1 flume: plane beach, slope 1:10")
	plot!([0.0, 25000.0], [0.0, 0.0]; panel=(1,1), lc="100/140/200", lw=0.75, ls=:dot)
	# (b) the beach, with each nest drawn over the span it covers
	k2 = (x .>= -200.0) .& (x .<= 1400.0)
	plot(x[k2], z[k2]; panel=(2,1), region=(-200.0, 1400.0, -160.0, 40.0), lc=:black, lw=1.25,
	     frame=(axes=:WSen,), xlabel="x (m)", ylabel="bed elevation (m)",
	     title="(b) the beach, and the extent of each nested grid")
	plot!([-200.0, 1400.0], [0.0, 0.0]; panel=(2,1), lc="100/140/200", lw=0.75, ls=:dot)
	for (lev, path, label) in paths
		isfile(path) || continue
		G = IG._read_cube_layer(path * "?" * IG._bm1_var(path), 1)
		G === nothing && continue
		y  = 22.0 - 13.0 * lev
		xe = min(G.range[2], 1400.0)
		plot!([G.range[1], xe], [y, y]; panel=(2,1), lc=cols[lev+1], lw=4)
		text!(mat2ds([xe + 25.0 y], [label]); panel=(2,1), font=(7,"Helvetica",cols[lev+1]),
		      justify=:ML, noclip=true)
	end
	for e in IG._BM1_LEVEL_EDGES
		plot!([e, e], [-160.0, 40.0]; panel=(2,1), lc="150/150/150", lw=0.5, ls=:dash)
	end
	subplot("end")
	gmtend()
	println("wrote ", png)
	return png
end

# ------------------------------------------------------------------------ 2. the initial condition
function fig_initial(outdir)
	png = joinpath(outdir, "bm1_initial_condition.png")
	T   = IG._BM1_ANALYTIC
	o   = sortperm(T[:,1])
	xt, et = T[o,1], T[o,2]
	R   = CB.catalina1(0.0; xmax = 30000.0, npoints = 4000)

	m = misfit(R.x, R.eta, xt, et)
	println("initial condition, catalina1(0) vs table:  n=", m.n,
	        "  rms=", round(m.rms, digits=3), "  max=", round(m.max, digits=3))
	it = argmin(et);  ia = argmin(R.eta)
	println("  trough:  table = ", round(et[it], digits=2), " m at x = ", round(xt[it]),
	        "     analytic = ", round(R.eta[ia], digits=2), " m at x = ", round(R.x[ia]))

	lg = leg2("Benchmark table (t = 0)", "catalina1(0)  [model start]")
	gmtbegin(png)
	subplot(grid=(2,1), F=(width=22, height=16), margins="0.6c/0.6c/0.4c/1.6c")
	pair_panel!(xt, et, R.x, R.eta, (0.0, 30000.0),
	            "(a) initial free surface, full profile", "x (m)", "eta (m)", (1,1), lg)
	pair_panel!(xt, et, R.x, R.eta, (5000.0, 14000.0),
	            "(b) the trough, where the two disagree", "x (m)", "eta (m)", (2,1), lg)
	subplot("end")
	gmtend()
	println("wrote ", png)
	return png
end

# ------------------------------------------------------- 3/4. table vs analytic, eta and velocity
function fig_table_vs_analytic(quantity::Symbol, outdir)
	png  = joinpath(outdir, quantity === :eta ? "bm1_table_vs_analytic.png" : "bm1_table_vs_analytic_u.png")
	T    = IG._BM1_ANALYTIC
	off  = quantity === :eta ? 1 : 2
	ylab = quantity === :eta ? "eta (m)" : "u (m/s)"
	lg   = leg2("Benchmark table", "catalina1(t)")

	println("table vs analytic, ", quantity === :eta ? "free surface" : "velocity (positive offshore)")
	gmtbegin(png)
	subplot(grid=(3,2), F=(width=22, height=26), margins="0.6c/0.6c/0.4c/2.4c")
	for k in 1:3
		t, c = TIMES[k], TCOLS[k]
		R  = CB.catalina1(t; xmax = 30000.0, npoints = 4000)
		# SIGN: the table's u is positive OFFSHORE (nswing's convention), catalina1's positive
		# SHOREWARD. Checked, not assumed: cos-similarity between the two columns is -0.99.
		va = quantity === :eta ? R.eta : -R.velocity
		o  = sortperm(T[:, c])
		xt, vt = T[o, c], T[o, c + off]
		m = misfit(R.x, va, xt, vt)
		println("  t = ", Int(t), " s   n = ", m.n, "   rms = ", round(m.rms, digits=3),
		        "   max = ", round(m.max, digits=3))
		pair_panel!(xt, vt, R.x, va, (0.0, 25000.0), "t = $(Int(t)) s   (full profile)",
		            "x (m)", ylab, (k,1), lg)
		pair_panel!(xt, vt, R.x, va, (-300.0, 1500.0), "t = $(Int(t)) s   (beach)",
		            "x (m)", ylab, (k,2), lg)
	end
	subplot("end")
	gmtend()
	println("wrote ", png)
	return png
end

# ------------------------------------------------------------------ the run's stitched profile
slice_of(t) = round(Int, t / 2.5)      # 101 layers, dt = 2.5 s; t = 160/175/220 land exactly

levcubes(rundir) = [joinpath(rundir, "benchmark1_lev$(i).nc") for i in 1:2]

function chunks_at(rundir, k::Int)
	base = IG._bm1_level_row(joinpath(rundir, "benchmark1.nc"), k)
	return IG._bm1_level_chunks(levcubes(rundir), k, XWIN[1], XWIN[2], base)
end

function stitched(rundir, k::Int)
	base = IG._bm1_level_row(joinpath(rundir, "benchmark1.nc"), k)
	return IG._bm1_stitch_curve(levcubes(rundir), k, XWIN[1], XWIN[2], base)
end

# ---------------------------------------------------------------------- 5. model vs analytic
function fig_model(rundir, outdir)
	png = joinpath(outdir, "bm1_model_vs_analytic.png")
	lg  = leg2("NSWING, nested (stitched)", "catalina1(t)")
	println("nested model vs analytic")
	gmtbegin(png)
	subplot(grid=(3,2), F=(width=22, height=26), margins="0.6c/0.6c/0.4c/2.4c")
	T = IG._BM1_ANALYTIC
	for k in 1:3
		t  = TIMES[k]
		xm, ym = stitched(rundir, slice_of(t))
		xa, ya = IG._bm1_analytic_curve(t, XWIN[1], XWIN[2], 4000)
		m = misfit(xa, ya, xm, ym; xmax = XWIN[2])
		# ...and against the PUBLISHED TABLE, on the table's own nodes inside the display window
		c  = TCOLS[k]
		o  = sortperm(T[:, c])
		kt = (T[o, c] .>= XWIN[1]) .& (T[o, c] .<= XWIN[2])
		mt = misfit(xm, ym, T[o, c][kt], T[o, c + 1][kt]; xmax = XWIN[2])
		println("  t = ", Int(t), " s   vs analytic: n = ", m.n, "  rms = ", round(m.rms, digits=3),
		        "  max = ", round(m.max, digits=3),
		        "   |  vs table: n = ", mt.n, "  rms = ", round(mt.rms, digits=3),
		        "  max = ", round(mt.max, digits=3))
		pair_panel!(xm, ym, xa, ya, (0.0, 20000.0), "t = $(Int(t)) s   (full profile)",
		            "x (m)", "eta (m)", (k,1), lg)
		pair_panel!(xm, ym, xa, ya, (-200.0, 1500.0), "t = $(Int(t)) s   (beach)",
		            "x (m)", "eta (m)", (k,2), lg)
	end
	subplot("end")
	gmtend()
	println("wrote ", png)
	return png
end

# ------------------------------------------------------- 6. the stitch itself, level by level
function fig_nesting(rundir, outdir)
	png  = joinpath(outdir, "bm1_nesting.png")
	t    = 175.0                     # maximum draw-down: the levels differ most here
	k    = slice_of(t)
	chunks = chunks_at(rundir, k)
	xa, ya = IG._bm1_analytic_curve(t, XWIN[1], XWIN[2], 4000)
	cols = Dict(0 => "60/60/60", 1 => "220/120/0", 2 => "200/0/0")
	incs = Dict(0 => "25 m", 1 => "5 m", 2 => "1 m")

	println("nesting figure at t = ", Int(t), " s, slice ", k)
	for (lev, cx, _) in chunks
		isempty(cx) && continue
		println("  level ", lev, "  dx = ", incs[lev], "   x = ", round(minimum(cx)), " .. ",
		        round(maximum(cx)), "   n = ", length(cx))
	end

	lg = (symbol1 = (marker="-", size=0.6, dx_left=0.0, pen=(2.0,"120/120/120"), dx_right=0.3, text="catalina1(175)"),
	      symbol2 = (marker="-", size=0.6, dx_left=0.0, pen=(1.25,"200/0/0"),    dx_right=0.3, text="level 2  dx = 1 m"),
	      symbol3 = (marker="-", size=0.6, dx_left=0.0, pen=(1.25,"220/120/0"),  dx_right=0.3, text="level 1  dx = 5 m"),
	      symbol4 = (marker="-", size=0.6, dx_left=0.0, pen=(1.25,"60/60/60"),   dx_right=0.3, text="level 0  dx = 25 m"))

	gmtbegin(png)
	subplot(grid=(2,1), F=(width=22, height=17), margins="0.6c/0.6c/0.4c/1.8c")
	for (row, rgx) in ((1, (-200.0, 2000.0)), (2, (-200.0, 700.0)))
		ka = (xa .>= rgx[1]) .& (xa .<= rgx[2])
		ys = Float64[]
		for (_, cx, cy) in chunks
			m = (cx .>= rgx[1]) .& (cx .<= rgx[2]);  append!(ys, cy[m])
		end
		append!(ys, ya[ka])
		isempty(ys) && continue
		ylo, yhi = minimum(ys), maximum(ys);  pad = 0.10 * max(yhi - ylo, eps())
		plot(xa[ka], ya[ka]; panel=(row,1), region=(rgx[1], rgx[2], ylo - pad, yhi + pad),
		     lc="120/120/120", lw=2.0, frame=(axes=:WSen,), xlabel="x (m)", ylabel="eta (m)",
		     title = row == 1 ? "t = 175 s: the model curve, one colour per nesting level"
		                      : "the same, over the two finest levels")
		for (lev, cx, cy) in chunks
			m = (cx .>= rgx[1]) .& (cx .<= rgx[2])
			any(m) && plot!(cx[m], cy[m]; panel=(row,1), lc=cols[lev], lw=1.25)
		end
		for e in IG._BM1_LEVEL_EDGES
			(e >= rgx[1] && e <= rgx[2]) &&
				plot!([e, e], [ylo - pad, yhi + pad]; panel=(row,1), lc="150/150/150", lw=0.5, ls=:dash)
		end
		legend(lg; position=(inside=:TR, width=0.0))
	end
	subplot("end")
	gmtend()
	println("wrote ", png)
	return png
end

function main(rundir::String, outdir::String)
	mkpath(outdir)
	println("Catalina Benchmark 1 -- manual figures")
	println("  run    ", rundir)
	println("  out    ", outdir)
	println()
	fig_setup(rundir, outdir);                println()
	fig_initial(outdir);                      println()
	fig_table_vs_analytic(:eta, outdir);      println()
	fig_table_vs_analytic(:u, outdir);        println()
	fig_model(rundir, outdir);                println()
	fig_nesting(rundir, outdir)
	return nothing
end

if abspath(PROGRAM_FILE) == @__FILE__
	rd = length(ARGS) >= 1 ? ARGS[1] : "C:/TMP/claude/bm1run"
	od = length(ARGS) >= 2 ? ARGS[2] : joinpath(dirname(@__DIR__), "docs", "src", "assets")
	main(rd, od)
end
