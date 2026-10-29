@testitem "X,Y grid toggles: Grid / X-Grid / Y-Grid set the page's Axes properties; geomag is in Analysis" tags=[:gui, :xyplot] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; tf = GmtvtkTest._test_fn
	IG._XY_CURRENT[] = C_NULL
	x = collect(0.0:0.5:10.0)
	p = plot(x, sin.(x); legend="s")
	h = p.h
	click(n) = ccall(tf(:gmtvtk_xyplot_click_test), Cint, (Ptr{Cvoid}, Cstring), h, n)
	menu(n)  = ccall(tf(:gmtvtk_xyplot_menu_test), Cint, (Ptr{Cvoid}, Cstring), h, n)
	page() = only([d for (s, d) in IG._xy_manifest_sections(IG._xy_session_state(h)) if s == "page"])
	axp(k) = (m = match(Regex("axprop=" * replace(k, "." => "\\.") * " (\\S*)"), IG._xy_session_state(h)); m === nothing ? "" : m.captures[1])
	try
		IG._pump_once()
		# the bar code tool left the tool bar for Analysis > Synthetic geomag anomalies
		@test click("xyMagTool") == 0
		@test menu("xyMagTool") == 1
		@test menu("xyMagTool") == 1                       # and off again
		# a new page draws both grids, and the three toggles say so
		@test page()["gridx"] == "1" && page()["gridy"] == "1"
		# the three are one choice. X-Grid: the vertical lines only, written as the Axes properties'
		# South / West Grid boxes
		@test click("xyGridX") == 1
		@test page()["gridx"] == "1" && page()["gridy"] == "0"
		@test axp("MAP_FRAME_AXES.Sg") == "1" && axp("MAP_FRAME_AXES.Wg") == "0"
		steps, _ = IG._xy_script_emit(h)
		kw = Dict(steps[1].kw)
		@test haskey(kw, :xaxis) && haskey(kw[:xaxis], :grid) && !haskey(get(kw, :yaxis, (;)), :grid)
		# Y-Grid: the horizontal ones only; clicked again (it is the active one): no grid at all
		@test click("xyGridY") == 1
		@test page()["gridx"] == "0" && page()["gridy"] == "1"
		@test click("xyGridY") == 1
		@test page()["gridx"] == "0" && page()["gridy"] == "0"
		# Grid: both again; then Y-Grid for the session below
		@test click("xyGrid") == 1
		@test page()["gridx"] == "1" && page()["gridy"] == "1"
		@test click("xyGridX") == 1
		@test page()["gridx"] == "1" && page()["gridy"] == "0" && axp("MAP_FRAME_AXES.Wg") == "0"
		# the page's own grid survives a session
		path = joinpath(tempdir(), "igmt_grid_$(getpid()).igmtz")
		IG._xy_save_session(h, path); p2 = IG._on_load_session(C_NULL, path); IG._pump_once()
		d2 = only([d for (s, d) in IG._xy_manifest_sections(IG._xy_session_state(p2.h)) if s == "page"])
		@test d2["gridx"] == "1" && d2["gridy"] == "0"
		ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), p2.h)
		rm(path; force=true)
		# a page with no data: the whole Tools row is off
		ccall(IG._fn(:gmtvtk_xyplot_add_page), Cint, (Ptr{Cvoid}, Cstring), h, "empty"); IG._pump_once()
		@test all(n -> menu(n) == 0, ("xyTitle", "xyYLabel", "xyGrid", "xyGridX", "xyGridY", "xyAnnot_Arrow", "xyAnnot_Ellipse"))
		plot!(p, x, cos.(x); legend="c"); IG._pump_once()
		@test menu("xyGridX") == 1                         # data on it: on again
	finally
		close(p)
	end
end

@testitem "X,Y Title / Subtitle / X-Label / Y-Label buttons type the text in place, into the Axes properties" tags=[:gui, :xyplot] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; tf = GmtvtkTest._test_fn
	IG._XY_CURRENT[] = C_NULL
	x = collect(0.0:0.5:10.0)
	p = plot(x, sin.(x); legend="s")
	h = p.h
	menu(n) = ccall(tf(:gmtvtk_xyplot_menu_test), Cint, (Ptr{Cvoid}, Cstring), h, n)
	typ(t)  = ccall(tf(:gmtvtk_xyplot_type_test), Cint, (Ptr{Cvoid}, Cstring, Cstring), h, "xyTextEdit", t)
	page()  = only([d for (s, d) in IG._xy_manifest_sections(IG._xy_session_state(h)) if s == "page"])
	axp(k)  = (m = match(Regex("axprop=" * replace(k, "." => "\\.", "+" => "\\+") * " (.*)"), IG._xy_session_state(h)); m === nothing ? nothing : m.captures[1])
	try
		IG._pump_once()
		for (btn, txt) in (("xyTitle", "Magnetic anomaly"), ("xySubtitle", "track 7"), ("xyXLabel", "Distance (km)"), ("xyYLabel", "nT"))
			@test menu(btn) == 1                           # the button opens the in-place box...
			@test typ(txt) == 1                            # ...and Enter keeps what was typed
		end
		IG._pump_once()
		pg = page()
		@test IG._xy_unesc(pg["title"]) == "Magnetic anomaly" && IG._xy_unesc(pg["subtitle"]) == "track 7"
		@test IG._xy_unesc(pg["xlabel"]) == "Distance (km)" && IG._xy_unesc(pg["ylabel"]) == "nT"
		# written as the Axes properties' own fields, so the dialog shows them
		@test IG._xy_unesc(axp("-B.+t")) == "Magnetic anomaly" && IG._xy_unesc(axp("-B.+s")) == "track 7"
		# the box opens on the text already there: retyped, the title changes, the subtitle stays
		@test menu("xyTitle") == 1 && typ("Anomaly") == 1
		@test IG._xy_unesc(page()["title"]) == "Anomaly" && IG._xy_unesc(page()["subtitle"]) == "track 7"
		# Plot with GMT.jl: -B+t / -B+s and the axis labels
		steps, _ = IG._xy_script_emit(h)
		kw = Dict(steps[1].kw)
		@test kw[:frame].title == "Anomaly" && kw[:frame].subtitle == "track 7"
		@test kw[:xlabel] == "Distance (km)" && kw[:ylabel] == "nT"
	finally
		close(p)
	end
end

@testitem "X,Y text box: its properties show / hide the box, kept by a session and by Plot with GMT.jl" tags=[:gui, :xyplot] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; tf = GmtvtkTest._test_fn
	IG._XY_CURRENT[] = C_NULL
	x = collect(0.0:0.5:10.0)
	p = plot(x, sin.(x); legend="s")
	h = p.h
	arm(k)  = ccall(tf(:gmtvtk_xyplot_menu_test), Cint, (Ptr{Cvoid}, Cstring), h, "xyAnnot_$k")
	drag(a, b, c, d) = ccall(tf(:gmtvtk_xyplot_drag_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble), h, a, b, c, d)
	typ(t)  = ccall(tf(:gmtvtk_xyplot_type_test), Cint, (Ptr{Cvoid}, Cstring, Cstring), h, "xyTextEdit", t)
	props(i, w) = ccall(tf(:gmtvtk_xyplot_annot_props_test), Cint, (Ptr{Cvoid}, Cint, Cstring), h, i, w)
	path = joinpath(tempdir(), "igmt_tbox_$(getpid()).igmtz"); p2 = nothing
	try
		IG._pump_once()
		@test arm("TextBox") == 1 && drag(2.0, 0.8, 5.0, 0.3) == 1 && typ("note") == 1
		a = only(IG._xy_page_annots(h, 0))
		@test a.kind == 4 && a.text == "note" && a.box             # a new text box shows its box
		# its properties dialog: "Show box" unchecked -> the text alone
		@test props(0, "annotShowBox") == 1
		@test !only(IG._xy_page_annots(h, 0)).box
		@test occursin("annotbox=0", IG._xy_session_state(h))
		steps, ctx = IG._xy_script_emit(h)
		@test occursin("box=false", IG._xy_script_text(steps, ctx))
		# a session keeps it
		IG._xy_save_session(h, path); p2 = IG._on_load_session(C_NULL, path); IG._pump_once()
		@test !only(IG._xy_page_annots(p2.h, 0)).box
		# checked again: the box is back, and the script no longer hides it
		@test props(0, "annotShowBox") == 1
		@test only(IG._xy_page_annots(h, 0)).box
		steps, ctx = IG._xy_script_emit(h)
		@test !occursin("box=false", IG._xy_script_text(steps, ctx))
	finally
		close(p)
		p2 === nothing || ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), p2.h)
		rm(path; force=true)
	end
end

@testitem "X,Y semilogx / semilogy / loglog on a sine: points <= 0 left out, the axes really go log (MATLAB)" tags=[:gui, :xyplot] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; tf = GmtvtkTest._test_fn
	IG._XY_CURRENT[] = C_NULL
	x = collect(0.0:0.1:10.0)                             # starts at 0: no log X either, as it stands
	y = sin.(x)
	p = plot(x, y; legend="sin")
	h = p.h
	menu(n) = ccall(tf(:gmtvtk_xyplot_menu_test), Cint, (Ptr{Cvoid}, Cstring), h, n)
	page()  = only([d for (s, d) in IG._xy_manifest_sections(IG._xy_session_state(h)) if s == "page"])
	try
		IG._pump_once()
		@test menu("xyPlotType_semilogy") == 1; IG._pump_once()
		@test page()["logyactive"] == "1" && page()["logxactive"] == "0"
		@test menu("xyPlotType_semilogx") == 1; IG._pump_once()
		@test page()["logxactive"] == "1" && page()["logyactive"] == "0"
		@test menu("xyPlotType_loglog") == 1; IG._pump_once()
		@test page()["logxactive"] == "1" && page()["logyactive"] == "1"
		# the series' own data is untouched: every value, negatives included, is still there
		xs, ys = IG._xy_get_series(h, 0)
		@test xs == x && all(isfinite, ys) && isapprox(ys, y; rtol = 1e-6)   # Y is stored as Float32
		@test count(<=(0), ys) > 0                         # the negatives are still in the data
		# plot: both linear again, the whole curve drawn from its own data
		@test menu("xyPlotType_plot") == 1; IG._pump_once()
		@test page()["logxactive"] == "0" && page()["logyactive"] == "0"
	finally
		close(p)
	end
end

@testitem "X,Y biplot without groups: one colour group per variable, its arrow and name in the same colour" tags=[:gui, :xyplot] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; tf = GmtvtkTest._test_fn
	IG._XY_CURRENT[] = C_NULL
	p = plot(collect(1.0:5.0), collect(1.0:5.0))
	h = p.h
	try
		IG._pump_once()
		IG._xy_example(h, p, "biplot"); IG._pump_once()            # 3 variables: length, width, depth
		@test ccall(tf(:gmtvtk_xyplot_menu_test), Cint, (Ptr{Cvoid}, Cstring), h, "xyPlotType_biplot") == 1
		IG._pump_once()
		secs = IG._xy_manifest_sections(IG._xy_session_state(h))
		cur = parse(Int, only([d for (s, d) in secs if s == "window"])["curpage"])
		ser = [d for (s, d) in secs if s == "series" && parse(Int, d["page"]) == cur]
		byname = Dict(IG._xy_unesc(d["name"]) => d for d in ser)
		vars = ("length", "width", "depth")
		@test all(v -> haskey(byname, v) && haskey(byname, "$v loading"), vars)
		# every observation is in exactly one variable's group, and each group is painted as its arrow is
		@test sum(v -> parse(Int, byname[v]["npoints"]), vars) == 150
		@test all(v -> byname[v]["rgb"] == byname["$v loading"]["rgb"], vars)
		@test length(unique(byname[v]["rgb"] for v in vars)) == 3
		# the variable names are drawn in the same colours
		labs = IG._xy_page_labels(h, cur)
		col(v) = (l = only(filter(l -> l.text == v, labs)); join(map(x -> string(round(x; digits = 2)), (l.r, l.g, l.b)), " "))
		@test all(v -> col(v) == join(map(x -> string(round(parse(Float64, x); digits = 2)), split(byname[v]["rgb"])), " "), vars)
		# each name belongs to its arrow's row: unchecking "depth loading" hides the name "depth" too
		@test all(v -> only(filter(l -> l.text == v, labs)).owner == "$v loading", vars)
		om(t, w) = ccall(tf(:gmtvtk_xyplot_objmgr_test), Cint, (Ptr{Cvoid}, Cstring, Cint), h, t, w)
		lblsteps() = [s.comment for s in first(IG._xy_script_emit(h)) if startswith(s.comment, "label ")]
		@test "label \"depth\"" in lblsteps()
		@test om("depth loading", 2) == 1; IG._pump_once()
		@test !("label \"depth\"" in lblsteps()) && "label \"width\"" in lblsteps()
		@test om("depth loading", 1) == 1; IG._pump_once()
		@test "label \"depth\"" in lblsteps()
	finally
		close(p)
	end
end

@testitem "X,Y biplot side panel: each component's variance %, the pair picked there redraws the page" tags=[:gui, :xyplot] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; tf = GmtvtkTest._test_fn
	IG._XY_CURRENT[] = C_NULL
	k = 1:60
	f, g = sin.(0.3 .* k), cos.(0.7 .* k)
	M = [f .+ 0.1 .* cos.(2.1 .* k)  0.8 .* f .+ 0.2 .* g  g .+ 0.1 .* sin.(1.9 .* k)  0.5 .* g .- f]
	p = IG.biplot(M; PC=(1, 2)); h = p.h
	pick(w, i) = ccall(tf(:gmtvtk_xyplot_type_test), Cint, (Ptr{Cvoid}, Cstring, Cstring), h, w, string(i))
	function cur(hh = h)
		secs = IG._xy_manifest_sections(IG._xy_session_state(hh))
		c = parse(Int, only([d for (s, d) in secs if s == "window"])["curpage"])
		pg = [d for (s, d) in secs if s == "page"][c + 1]
		ser = [IG._xy_unesc(d["name"]) for (s, d) in secs if s == "series" && parse(Int, d["page"]) == c]
		return pg, ser, c
	end
	path = joinpath(tempdir(), "igmt_biplotpc_$(getpid()).igmtz"); r2 = Ref{Any}(nothing)
	try
		IG._pump_once()
		pg, ser, c = cur()
		names = sort([l.text for l in IG._xy_page_labels(h, c)])
		@test length(names) == 4
		v = parse.(Float64, split(pg["biplot.var"]))
		@test length(v) == 4 && isapprox(sum(v), 100; atol=1e-6) && issorted(v; rev=true)
		@test pg["biplot.pc"] == "1 2" && startswith(IG._xy_unesc(pg["xlabel"]), "PC1 (")
		# X axis -> PC3: the page is redrawn on components 3 and 2, the same series and names
		@test pick("xyBiplotPCX", 2) == 1; IG._pump_once()
		pg, ser2, c = cur()
		@test pg["biplot.pc"] == "3 2" && startswith(IG._xy_unesc(pg["xlabel"]), "PC3 (") &&
		      startswith(IG._xy_unesc(pg["ylabel"]), "PC2 (")
		@test sort(ser2) == sort(ser)
		@test sort([l.text for l in IG._xy_page_labels(h, c)]) == names
		# Y axis -> the component X shows: the two swap
		@test pick("xyBiplotPCY", 2) == 1; IG._pump_once()
		@test cur()[1]["biplot.pc"] == "2 3"
		# the panel is no part of the figure: Plot with GMT.jl draws the chart only
		steps, ctx = IG._xy_script_emit(h)
		@test !occursin("Principal components", IG._xy_script_text(steps, ctx))
		# a session keeps the panel, and its input: the restored page is redrawn on another pair too
		IG._xy_save_session(h, path); p2 = r2[] = IG._on_load_session(C_NULL, path); IG._pump_once()
		@test cur(p2.h)[1]["biplot.pc"] == "2 3"
		@test ccall(tf(:gmtvtk_xyplot_type_test), Cint, (Ptr{Cvoid}, Cstring, Cstring), p2.h, "xyBiplotPCX", "0") == 1
		IG._pump_once()
		@test cur(p2.h)[1]["biplot.pc"] == "1 3"
	finally
		close(p)
		r2[] === nothing || ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), r2[].h)
		rm(path; force=true)
	end
	# a page that is not a biplot has no panel
	IG._XY_CURRENT[] = C_NULL
	q = plot(collect(1.0:5.0), collect(1.0:5.0))
	try
		IG._pump_once()
		@test ccall(tf(:gmtvtk_xyplot_type_test), Cint, (Ptr{Cvoid}, Cstring, Cstring), q.h, "xyBiplotPCX", "1") == 0
	finally
		close(q)
	end
end

@testitem "X,Y data tip: a click on a point (biplot, scatter) leaves nothing plotted; a line still takes the marker" tags=[:gui, :xyplot] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; tf = GmtvtkTest._test_fn
	click(h, x, y) = (o = zeros(3); s = ccall(tf(:gmtvtk_xyplot_mark_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cint, Ptr{Cdouble}), h, x, y, 0, o); (s, o))
	IG._XY_CURRENT[] = C_NULL
	k = 1:60
	M = [sin.(0.3 .* k)  cos.(0.7 .* k)  sin.(0.3 .* k) .+ 0.2 .* cos.(0.7 .* k)]
	p = IG.biplot(M); h = p.h
	try
		IG._pump_once()
		x, y = IG._xy_get_series(h, 1)                          # an observations series (0 is x = 0, y = 0)
		s, o = click(h, x[1], y[1])
		@test s == -1 && isnan(o[1]) && o[3] == 0               # no marker, no label drawn
		x0, y0 = IG._xy_get_series(h, 0)                        # not on the axis lines either
		s, o = click(h, 0.0, 0.5 * y0[end])
		@test s == -1 && o[3] == 0
	finally
		close(p)
	end
	IG._XY_CURRENT[] = C_NULL
	x = collect(0.0:0.5:10.0)
	q = plot(x, sin.(x)); h = q.h
	try
		IG._pump_once()
		@test ccall(tf(:gmtvtk_xyplot_menu_test), Cint, (Ptr{Cvoid}, Cstring), h, "xyPlotType_scatter") == 1; IG._pump_once()
		s, o = click(h, x[5], sin(x[5]))
		@test s == -1 && o[3] == 0                              # a scatter: no data tip
		@test ccall(tf(:gmtvtk_xyplot_menu_test), Cint, (Ptr{Cvoid}, Cstring), h, "xyPlotType_plot") == 1; IG._pump_once()
		s, o = click(h, x[5], sin(x[5]))
		@test s == 0 && isapprox(o[1], x[5]; atol=1e-6)         # a line keeps its curve marker
	finally
		close(q)
	end
end

# The X,Y window's annotation tools (MATLAB's ANNOTATIONS: Text Arrow, Arrow, Double Arrow, Line, Text
# Box, Rectangle, Ellipse): armed from the tool bar, drawn with the mouse, typed in place, listed in the
# Object Manager, kept by a session and drawn by Plot with GMT.jl.

@testitem "X,Y annotations: drawn with the tools, typed, listed, hidden, saved, scripted, edited after a double-click" tags=[:gui, :xyplot] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; tf = GmtvtkTest._test_fn
	IG._XY_CURRENT[] = C_NULL
	x = collect(0.0:0.5:10.0)
	p = plot(x, sin.(x); legend="s")
	h = p.h
	arm(k)  = ccall(tf(:gmtvtk_xyplot_menu_test), Cint, (Ptr{Cvoid}, Cstring), h, "xyAnnot_$k")
	drag(a, b, c, d) = ccall(tf(:gmtvtk_xyplot_drag_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble), h, a, b, c, d)
	typ(t)  = ccall(tf(:gmtvtk_xyplot_type_test), Cint, (Ptr{Cvoid}, Cstring, Cstring), h, "xyTextEdit", t)
	om(t, w) = ccall(tf(:gmtvtk_xyplot_objmgr_test), Cint, (Ptr{Cvoid}, Cstring, Cint), h, t, w)
	rclick(x, y, it) = ccall(tf(:gmtvtk_xyplot_rclick_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cstring), h, x, y, it)
	path = joinpath(tempdir(), "igmt_annot_$(getpid()).igmtz"); p2 = nothing
	png = joinpath(tempdir(), "igmt_annot_$(getpid()).png")
	try
		# every tool is on the tool bar
		for k in ("TextArrow", "Arrow", "DoubleArrow", "Line", "TextBox", "Rectangle", "Ellipse")
			@test arm(k) == 1
		end
		@test arm("Ellipse") == 1                                  # disarm the last one again
		@test isempty(IG._xy_page_annots(h, 0))
		# an arrow, dragged out on the chart
		@test arm("Arrow") == 1
		@test drag(2.0, 0.5, 4.0, -0.5) == 1
		a = IG._xy_page_annots(h, 0)
		@test length(a) == 1 && a[1].kind == 1
		@test isapprox(a[1].x0, 2.0; atol=0.05) && isapprox(a[1].x1, 4.0; atol=0.05) && isapprox(a[1].y1, -0.5; atol=0.05)
		# the tool disarms after one, as MATLAB's: the next drag is the chart's, not a second arrow
		drag(6.0, 0.5, 7.0, 0.0)
		@test length(IG._xy_page_annots(h, 0)) == 1
		# a text arrow: drawn, then its text typed in place
		@test arm("TextArrow") == 1
		@test drag(5.0, 0.8, 7.0, 0.2) == 1
		@test typ("peak") == 1
		a = IG._xy_page_annots(h, 0)
		@test length(a) == 2 && a[2].kind == 0 && a[2].text == "peak"
		# a rectangle and an ellipse
		@test arm("Rectangle") == 1;  @test drag(1.0, 0.9, 3.0, 0.6) == 1
		@test arm("Ellipse") == 1;    @test drag(7.0, -0.2, 9.0, -0.8) == 1
		@test [b.kind for b in IG._xy_page_annots(h, 0)] == [1, 0, 5, 6]
		# listed in the Object Manager; the group's box hides them all
		@test om("Annotations", 2) == 1
		IG._pump_once()
		@test all(b -> !b.visible, IG._xy_page_annots(h, 0))
		@test om("Annotations", 1) == 1
		IG._pump_once()
		@test all(b -> b.visible, IG._xy_page_annots(h, 0))
		# Plot with GMT.jl draws every one with ONE annotation! call, coordinates and text written out, and
		# the script carries annotation!'s definition (GMT.jl only) — no fetch from the live window
		steps, ctx = IG._xy_script_emit(h)
		@test count(s -> s.mod === :annotation, steps) == 4
		txt = IG._xy_script_text(steps, ctx)
		@test count("function annotation!(", txt) == 1
		@test occursin("annotation!(:arrow, x=(", txt) && occursin("annotation!(:ellipse, x=(", txt)
		@test occursin(r"annotation!\(:textarrow, .*text=\"peak\"", txt)
		@test !occursin("_xy_annot", txt)
		# ...and the script GMT runs makes the figure (the editor's Run, finishing into a file)
		stmts = Tuple{LineNumberNode,Any}[]; loc = LineNumberNode(1, :script)
		for e in Meta.parseall(txt).args
			e isa LineNumberNode ? (loc = e) : push!(stmts, (loc, e))
		end
		IG._script_handoff!(stmts, png, 0)
		for (l, e) in stmts
			Core.eval(Main, Expr(:toplevel, l, e))
		end
		@test isfile(png) && filesize(png) > 0
		# a session keeps them
		IG._xy_save_session(h, path); p2 = IG._on_load_session(C_NULL, path); IG._pump_once()
		an(t) = [l for l in split(t, '\n') if startswith(l, "annot=")]
		@test an(IG._xy_session_state(p2.h)) == an(IG._xy_session_state(h))
		# on the chart an annotation is inert until DOUBLE-CLICKED: a drag on the arrow is the chart's
		dbl(x, y) = ccall(tf(:gmtvtk_xyplot_mark_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cint, Ptr{Cdouble}), h, x, y, 1, fill(NaN, 3))
		a0 = IG._xy_page_annots(h, 0)[1]
		drag(3.0, 0.0, 3.0, 0.4)
		@test IG._xy_page_annots(h, 0)[1] == a0
		# double-clicked, it is in edit: now the drag moves it
		dbl(3.0, 0.0)
		@test drag(3.0, 0.0, 3.0, 0.4) == 1
		a1 = IG._xy_page_annots(h, 0)[1]
		@test isapprox(a1.y0 - a0.y0, 0.4; atol=0.05) && isapprox(a1.y1 - a0.y1, 0.4; atol=0.05)
		# no menu on the chart: its properties and Delete are on its Object Manager row only
		@test rclick(3.0, 0.4, "Delete") == 0
		@test [b.kind for b in IG._xy_page_annots(h, 0)] == [1, 0, 5, 6]
	finally
		close(p)
		p2 === nothing || ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), p2.h)
		rm(path; force=true); rm(png; force=true)
	end
end
