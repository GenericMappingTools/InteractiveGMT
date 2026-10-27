# xyscript.jl — the X,Y window's File > Plot with GMT.jl…: the SAME feature as the 3-D viewer's.
#
# Not a second script writer. The window's current page is turned into the very `ScriptStep`s /
# `DataBind`s gmtscript.jl's emitter produces, and from there everything is gmtscript.jl's own: the
# text (`_script_render_text`, its header, the hoisted pens), the editor dialog (the one
# `buildScriptEditor`, 70_window.cpp, with its Run / Save… / Standalone), `_script_run`, and the
# Standalone zip (`_script_save_standalone`, which turns each live fetch below into a data/ member).
# Only WHAT is drawn is X,Y-specific: the page's curves, its axes, its time / log modes.

# One curve of a page, as the script fetches it from the live window (the `:live` rung of the data
# ladder — it exists only in the viewer). `name` names the Standalone zip member.
function _xy_series_data(h::Ptr{Cvoid}, page::Integer, idx::Integer, name::AbstractString = "")
	n = ccall(_fn(:gmtvtk_xyplot_page_series_npoints), Cint, (Ptr{Cvoid}, Cint, Cint), h, page, idx)
	n <= 0 && error("the X,Y window has no curve $idx on page $page")
	x = Vector{Float64}(undef, n); y = Vector{Float64}(undef, n)
	ccall(_fn(:gmtvtk_xyplot_page_get_series), Cint,
	      (Ptr{Cvoid}, Cint, Cint, Ptr{Float64}, Ptr{Float64}, Cint), h, page, idx, x, y, n)
	return GMT.mat2ds(hcat(x, y); colnames = ["x", isempty(name) ? "y" : String(name)])
end

_xy_hex(r, g, b) = "#" * join(string(round(Int, 255 * clamp(v, 0, 1)); base = 16, pad = 2) for v in (r, g, b))

# The X,Y chart's pen / marker codes -> GMT (vtkPen: 1 solid, 2 dash, 3 dot, 4 dash-dot, 5 dash-dot-dot;
# vtkPlotPoints: 1 cross, 2 plus, 3 square, 4 circle, 5 diamond). Widths are px on a 96-dpi chart.
const _XY_GMT_LINE   = Dict(2 => "dashed", 3 => "dotted", 4 => "-.", 5 => "-..")
const _XY_GMT_MARKER = Dict(1 => :x, 2 => :+, 3 => :square, 4 => :circle, 5 => :diamond)
_xy_pt(px) = round(Float64(px) * 72 / 96; sigdigits = 3)

"""
    _xy_script_emit(h) -> (steps, ctx)

The X,Y window's CURRENT page as gmtscript.jl call steps: one `plot`/`bar` per visible curve on the
page's own axes (region = what the chart shows, log axes in the projection, a time X through GMT's
own time columns), titled and labelled as on screen. What it cannot draw is listed in the header.
"""
function _xy_script_emit(h::Ptr{Cvoid}; figsize::Real = 15.0)
	ccall(_fn(:gmtvtk_xyplot_is_alive), Cint, (Ptr{Cvoid},), h) == 0 && error("gmtscript: that X,Y window is closed")
	secs = _xy_manifest_sections(_xy_session_state(h))
	win = Dict{String,String}(); pages = Dict{String,String}[]; series = Dict{String,String}[]
	for (sec, d) in secs
		sec == "window" && (win = d)
		sec == "page"   && push!(pages, d)
		sec == "series" && push!(series, d)
	end
	cur = parse(Int, get(win, "curpage", "0"))
	pg = pages[cur + 1]
	xr = _xy_nums(pg["xrange"]); yr = _xy_nums(pg["yrange"])
	logx = get(pg, "logx", "0") == "1"; logy = get(pg, "logy", "0") == "1"
	w = Float64(figsize); hgt = round(w * 2 / 3; digits = 2)
	proj = string("X", w, logx ? "l" : "", "/", hgt, logy ? "l" : "")
	ctx = ScriptCtx(h, (xr[1], xr[2], yr[1], yr[2]), false, proj, w, nothing, nothing, false,
	                Dict{String,Int}(), String[], false, 0, 96.0)
	length(pages) > 1 && push!(ctx.notes, "only the page shown ('$(_xy_unesc(pg["name"]))') is exported; " *
	                                      "the window has $(length(pages)) pages")
	timefmt = parse(Int, get(pg, "xtime", "0"))
	gridon = get(win, "grid", "1") == "1"
	frame = gridon ? (axes = :WSen, annot = :auto, ticks = :auto, grid = :auto) :
	                 (axes = :WSen, annot = :auto, ticks = :auto)
	legendon = get(win, "legend", "1") == "1"

	steps = ScriptStep[]
	for d in series
		parse(Int, d["page"]) == cur || continue
		get(d, "visible", "1") == "1" || continue
		idx = parse(Int, d["idx"]); name = _xy_unesc(get(d, "name", ""))
		r, g, b = _xy_nums(get(d, "rgb", "0 0 0")); col = _xy_hex(r, g, b)
		var = _script_var!(ctx, "D", nothing)
		bind = DataBind(var, :live, "InteractiveGMT._xy_series_data(Ptr{Nothing}(UInt($(UInt(h)))), $cur, $idx, $(repr(name)))")
		var = ScriptVar(var.name, _xy_series_data(h, cur, idx, name))   # the live sink's object
		bind = DataBind(var, bind.tier, bind.expr)
		kw = Pair{Symbol,Any}[]
		if isempty(steps)
			append!(kw, [:region => ScriptVar(:REG, ctx.region), :proj => ScriptVar(:PROJ, ctx.proj), :frame => frame])
			xl = _xy_unesc(get(pg, "xlabel", "")); yl = _xy_unesc(get(pg, "ylabel", ""))
			isempty(xl) || push!(kw, :xlabel => xl)
			isempty(yl) || push!(kw, :ylabel => yl)
			if timefmt != 0          # X is Unix epoch seconds: GMT reads it as time from that epoch
				push!(kw, :f => "0T")
				push!(kw, :par => (TIME_EPOCH = "1970-01-01T00:00:00", TIME_UNIT = "s"))
			end
		end
		kind = parse(Int, get(d, "kind", "0"))
		width = parse(Float64, get(d, "width", "1.5"))
		if kind == 1                 # a bar series (histogram / bar graph): GMT's bar symbol, width in x units
			# `plot` with -Sb, not GMT.jl's `bar`: that one always adds `-C -i0-1,1` and paints each bar
			# its own colour whatever `fill` says (GMT.jl bug, reported, not patched here).
			push!(kw, :symbol => string("b", parse(Float64, get(d, "barwidth", "1")), "u+b0"))
			push!(kw, :fill => col)
			mod = :plot
		elseif get(d, "nowcross", "0") == "1"
			cx, cy, csz = _xy_nums(get(d, "cross", "0 0 24"))
			var = ScriptVar(var.name, GMT.mat2ds([cx cy]))
			bind = DataBind(var, :live, bind.expr)
			append!(kw, [:marker => :+, :ms => string(_xy_pt(2csz), "p"), :ml => string(_xy_pt(width), "p,", col)])
			mod = :plot
		elseif kind == 10            # bubbles: one circle per point, its diameter as the window maps it
			sz = _xy_nums(get(d, "bubble", ""))
			lo, hi = isempty(sz) ? (0.0, 0.0) : extrema(filter(isfinite, sz))
			d0, d1 = 5.0, 20.0       # points: the window's bubble range (XY_BUBBLE_PT_LO/HI)
			dia = [isfinite(v) ? sqrt(d0^2 + (hi > lo ? (v - lo) / (hi - lo) : 0.5) * (d1^2 - d0^2)) * 2.54 / 72 : 0.0 for v in sz]
			append!(kw, [:marker => :circle, :ms => dia, :mc => col * "@40", :ml => string("0.5p,", col)])
			mod = :plot
		else
			if kind == 9             # error bars: GMT.jl's error_bars, deltas from the point
				eb = [(ax, _xy_nums(get(d, l, "")), _xy_nums(get(d, h, ""))) for (ax, l, h) in
				      ((:x, "errxl", "errxh"), (:y, "erryl", "erryh"))]
				filter!(e -> !isempty(e[2]), eb)
				if !isempty(eb)       # one error_bars: symmetric vectors, or [lower upper] with asym=true
					asym = any(e -> e[2] != e[3], eb)
					ks = Tuple(vcat([e[1] for e in eb], asym ? [:asym] : Symbol[]))
					vs = Tuple(vcat([asym ? [e[2] e[3]] : e[2] for e in eb], asym ? [true] : Any[]))
					push!(kw, :error_bars => NamedTuple{ks}(vs))
				end
			end
			lt = parse(Int, get(d, "ltype", "1")); mk = parse(Int, get(d, "marker", "0"))
			lt != 0 && push!(kw, :pen => string(_xy_pt(width), "p,", col, haskey(_XY_GMT_LINE, lt) ? "," * _XY_GMT_LINE[lt] : ""))
			if mk != 0 && haskey(_XY_GMT_MARKER, mk)
				append!(kw, [:marker => _XY_GMT_MARKER[mk], :ms => string(_xy_pt(parse(Float64, get(d, "msize", "7"))), "p"), :mc => col])
			end
			mod = :plot
		end
		(legendon && !isempty(name)) && push!(kw, :legend => name)
		push!(steps, ScriptStep("$(name) — " * (kind == 1 ? "bars" : "curve"), DataBind[bind], mod, var, kw, !isempty(steps)))
	end
	isempty(steps) && error("gmtscript: this X,Y page has no visible curve to plot")
	# Drawn by the X,Y window itself, not curves: say so rather than drop them silently.
	get(pg, "mag.barOn", "0") == "1" && push!(ctx.notes, "the geomagnetic bar code strip, its chron names and picks")
	(get(pg, "mag.starIdx", "-1") != "-1" || get(pg, "mag.ageLineX", "nan") != "nan" || !isempty(strip(get(pg, "mag.pinned", "")))) &&
		push!(ctx.notes, "the isochron fit marks (red star, first guess, fitted and pinned lines)")
	!isempty(strip(get(pg, "sglines", ""))) && push!(ctx.notes, "the Spector-Grant fit lines")
	(get(win, "infovisible", "0") == "1") && push!(ctx.notes, "the header strip above the chart")
	return steps, ctx
end

"""
    _xy_script_open_editor(h) -> nothing

File > Plot with GMT.jl… of an X,Y window: build the page's script and show it in the SAME editor the
3-D viewer opens (gmtvtk_script_editor_show_h). What it does not reproduce goes to the Errors tab too,
as the viewer's does.
"""
function _xy_script_open_editor(h::Ptr{Cvoid})::Nothing
	steps, ctx = _xy_script_emit(h)
	txt = _script_render_text(steps, ctx)
	for n in ctx.notes
		_xy_log(h, "script: NOT EXPORTED: $n"; err = false)
	end
	ccall(_fn(:gmtvtk_script_editor_show_h), Cint, (Ptr{Cvoid}, Cstring), h, txt)
	return nothing
end
