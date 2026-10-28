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
		@test haskey(kw, :xaxis) && !haskey(kw, :yaxis)
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
