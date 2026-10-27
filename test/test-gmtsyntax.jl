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
	@test occursin("kind=6", IG._xy_session_state(p.h))   # a given width is kept: bars on a zero base
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

@testitem "one name per job: error_bars and per-point ms translate" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	o = IG._gmt_xy_kw(:plot, (; error_bars=(y=[0.1, 0.2],), ms=[1.0, 2.0]))
	@test o[:ebars][:y] == ([0.1, 0.2], [0.1, 0.2]) && o[:bubble] == [1.0, 2.0] && !haskey(o, :markersize)
	a = IG._gmt_xy_kw(:plot, (; E=(x=[1 2; 3 4], asym=true)))[:ebars]
	@test a[:x] == ([1.0, 3.0], [2.0, 4.0])
	@test_throws "2-column" IG._gmt_xy_kw(:plot, (; error_bars=(y=[1.0, 2.0], asym=true)))
	@test_throws "NamedTuple" IG._gmt_xy_kw(:plot, (; error_bars="y0.1"))
end

@testitem "X,Y gallery: errorbar, bubblechart and the new-page types" tags=[:gui, :xyplot] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; tf = GmtvtkTest._test_fn
	IG._XY_CURRENT[] = C_NULL
	x = collect(1.0:10.0)
	# carried values: error_bars and a per-point ms draw at once (kinds 9 and 10), and travel in the session
	p = plot(x, sqrt.(x); error_bars=(y=0.1 .* x,), legend="eb")
	plot!(p, x, x ./ 3; ms=x, legend="bub")
	st = IG._xy_session_state(p.h)
	@test occursin("kind=9", st) && occursin("kind=10", st) && occursin("erryl=", st)
	# the gallery is wider than the window: its later icons live in the toolbar's >> menu, so they are
	# chosen as that menu does (the action itself), not clicked as a button
	clickt(nm) = ccall(tf(:gmtvtk_xyplot_menu_test), Cint, (Ptr{Cvoid}, Cstring), p.h, nm)
	npages() = ccall(IG._fn(:gmtvtk_xyplot_page_count), Cint, (Ptr{Cvoid},), p.h)
	try
		# a series without values: the errorbar icon asks, and the answer (another series) draws it
		ccall(IG._fn(:gmtvtk_xyplot_add_page), Cint, (Ptr{Cvoid}, Cstring), p.h, "noerr")
		plot!(p, x, x .^ 2; legend="y"); plot!(p, x, 0.5 .* x; legend="err")
		@test clickt("xyPlotType_errorbar") == 1
		@test ccall(tf(:gmtvtk_dialog_button_test), Cint, (Cstring, Cstring, Cint, Ptr{UInt8}, Cint),
		            "Error bars", "ppOk", 1, C_NULL, 0) == 1
		IG._pump_once()
		st = IG._xy_session_state(p.h)
		@test count("kind=9", st) >= 2                 # the carried one + the one the dialog made
		# the new-page types: each adds a page
		for t in ("polarplot", "polarscatter", "compassplot", "feather")
			n0 = npages()
			@test clickt("xyPlotType_$t") == 1
			IG._pump_once()
			@test npages() == n0 + 1
			ccall(IG._fn(:gmtvtk_xyplot_add_page), Cint, (Ptr{Cvoid}, Cstring), p.h, "src")
			plot!(p, x, sqrt.(x); legend="a"); plot!(p, x, cos.(x); legend="b")
		end
		# pie / donut read the SELECTED series: the same builder the icon calls, with series 0 selected
		for t in ("piechart", "donutchart")
			n0 = npages()
			IG._xy_stat(p.h, p, t, 0)
			@test npages() == n0 + 1
			ccall(IG._fn(:gmtvtk_xyplot_add_page), Cint, (Ptr{Cvoid}, Cstring), p.h, "src")
			plot!(p, x, x; legend="v")
		end
	finally
		close(p)
	end
end

@testitem "X,Y Object Manager: the Axes row draws, hides and opens its Axes properties" tags=[:gui, :xyplot] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; tf = GmtvtkTest._test_fn
	IG._XY_CURRENT[] = C_NULL
	x = collect(0.0:0.5:20.0)
	p = plot(x, sin.(x); legend="s")
	IG._pump_once()                                   # the Object Manager is rebuilt on the pump
	om(t, w) = ccall(tf(:gmtvtk_xyplot_objmgr_test), Cint, (Ptr{Cvoid}, Cstring, Cint), p.h, t, w)
	st() = IG._xy_session_state(p.h)
	try
		@test om("Axes", 2) == 1 && occursin("axesshown=0", st())          # its checkbox hides them
		@test om("Axes", 1) == 1 && occursin("axesshown=1", st())          # ... and draws them again
		# a click on its LABEL opens the Axes properties: the same .ui as the 3-D viewer's
		@test om("Axes", 0) == 1
		IG._pump_once()
		pname = match(r"\[page\]\nname=([^\n]*)", st())
		title = (pname === nothing || isempty(pname[1])) ? "Axes properties" : "Axes properties - " * pname[1]
		btn(nm) = ccall(tf(:gmtvtk_dialog_button_test), Cint, (Cstring, Cstring, Cint, Ptr{UInt8}, Cint), title, nm, 1, C_NULL, 0)
		typ(nm, t) = ccall(tf(:gmtvtk_xyplot_type_test), Cint, (Ptr{Cvoid}, Cstring, Cstring), p.h, nm, t)
		@test ccall(tf(:gmtvtk_dialog_button_test), Cint, (Cstring, Cstring, Cint, Ptr{UInt8}, Cint), title, "btnApply", 0, C_NULL, 0) == 1
		tab(t) = ccall(tf(:gmtvtk_dialog_tab_test), Cint, (Cstring, Cstring), title, t)
		@test tab("X axis") == 1
		@test typ("editXLabel", "distance") == 1
		@test btn("chkDataLimitsX") == 1                                      # stop following the data
		@test typ("editXmin", "0") == 1 && typ("editXmax", "100") == 1
		@test tab("Frame") == 1
		@test typ("cmbSideN", "0") == 1                                       # top side: annotated + ticks
		@test btn("btnApply") == 1
		IG._pump_once()
		s = st()
		@test occursin("xlabel=distance", s) && occursin("xfixed=1", s)
		ap = filter(l -> startswith(l, "axprop"), split(s, '\n'))
		@test any(l -> startswith(l, "axprop=-R.xmax 100"), ap) || (println("stored: ", ap); false)
		@test any(l -> startswith(l, "axprop=MAP_FRAME_AXES.N"), ap) || (println("stored: ", ap); false)
	finally
		close(p)
	end
end
