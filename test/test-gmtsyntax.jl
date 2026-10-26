# ONE NAME PER JOB (gmtsyntax.jl): iGMT does a job GMT.jl already names under GMT.jl's name and
# syntax. Unit items: the names, the re-export of GMT.jl, the keyword translation. GUI item: the
# front doors draw into real windows.

@testitem "one name per job: iGMT names and the GMT.jl re-export" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test parentmodule(plot) === IG && parentmodule(plot!) === IG && parentmodule(feather) === IG
	@test viz !== GMT.viz && iview === viz
	@test scatter! === GMT.scatter! && orbits! === GMT.orbits! && bar! === GMT.bar!   # extended by dispatch, not shadowed
	ex = names(IG)
	@test :grdimage in ex && :coast in ex && :makecpt in ex          # the rest of GMT.jl comes along
	for old in (:xyplot, :stickplot, :view_grid, :view_image, :view_points, :view_fv,
	            :add_symbols!, :plot_groundtrack!)
		@test !(old in ex)
	end
	@test :groundtrack in ex                                          # computes data: no GMT.jl twin
end

@testitem "one name per job: GMT.jl keywords translate, unknown ones are named" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	o = IG._gmt_xy_kw(:plot, (; lc=:red, lw=1, ls="--", marker=:circ, ms=0.2, legend="a", proj=:loglog))
	@test o[:color] === :red && o[:linewidth] ≈ 96 / 72 && o[:linestyle] === :dash
	@test o[:marker] === :circle && o[:markersize] ≈ 0.2 * 96 / 2.54 && o[:name] == "a"
	@test o[:xscale] === :log && o[:yscale] === :log
	p = IG._gmt_xy_kw(:plot, (; pen=(2, :blue, :dot)))
	@test p[:linewidth] ≈ 2 * 96 / 72 && p[:color] === :blue && p[:linestyle] === :dot
	@test IG._gmt_xy_kw(:plot, (; lw="3p"))[:linewidth] ≈ 4.0
	@test_throws "keyword `region`" IG._gmt_xy_kw(:plot, (; region=(0, 1, 0, 1)))
	@test_throws "proj=:merc" IG._gmt_xy_kw(:plot, (; proj=:merc))
	@test_throws "keyword `title`" IG._gmt_xy_kw(:plot!, (; title="x"); adding=true)
	@test IG._gmt_xy_kw(:plot, (; ls=:none))[:linestyle] === :none
	@test_throws "keyword `shade`" viz(GMT.peaks(); shade=true)
end

@testitem "one name per job: plot reads its data as GMT.jl does" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	D = GMT.mat2ds([1.0 2 3; 2 3 4; 3 4 5], colnames=["t", "a", "b"])
	s = IG._gmt_xy_segs(D, false)                 # column 2 against column 1 ...
	@test length(s) == 1 && length(s[1][2]) == 1 && s[1][3] == ["a"] && s[1][4] == "t"
	s = IG._gmt_xy_segs(D, true)                  # ... every further column with multicol=true
	@test length(s[1][2]) == 2 && s[1][3] == ["a", "b"]
	@test length(IG._gmt_xy_segs([1.0, 2], [3.0 4; 5 6], false)[1][2]) == 1
	@test IG._gmt_xy_segs([5.0, 6, 7], false)[1][1] == [1.0, 2, 3]
	@test length(IG._gmt_xy_segs([D, D], false)) == 2
end

@testitem "one name per job: the front doors draw in real windows" tags=[:gui, :xyplot] begin
	IG = InteractiveGMT
	IG._XY_CURRENT[] = C_NULL
	t = collect(range(0, 4π; length=100))
	p = plot(t, sin.(t); legend="sin", title="gmtsyntax", lw=1.5)
	@test p isa QtXYPlot && isalive(p)
	plot!(p, t, cos.(t); legend="cos", lc=:blue, ls=:dash)
	plot!(t, t ./ 10; legend="ramp")              # no handle: the current X,Y window
	bar!(p, t[1:10:end], t[1:10:end] ./ 10; width=0.5, legend="bars")
	@test [s[3] for s in p.series] == ["sin", "cos", "ramp", "bars"]
	f = feather(t[1:10:end], [cos.(t[1:10:end]) sin.(t[1:10:end])]; lc=:red)
	@test f isa QtXYPlot && any(s -> s[3] == "sticks", f.series)
	fig = viz(GMT.peaks())
	@test scatter!(fig, [0.0, 1.0], [0.0, 1.0]; marker=:star, ms=0.4, mc=:red, legend="pts") === fig
	@test scatter!(fig, [0.5, 1.5], [0.5, 1.5]; marker=:circle, ms=[0.2, 0.4], legend="sized") === fig   # per-point ms, as GMT.jl
	IG._pump_once()
	@test isalive(fig)
	close(p); ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), fig.h)
end
