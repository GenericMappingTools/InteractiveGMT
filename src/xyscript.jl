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

# An arrow curve (kind 11: every NaN-separated run is a shaft from its first point to its last, head on
# the last) as GMT's -Sv...+s rows: x0 y0 x1 y1.
function _xy_arrow_data(h::Ptr{Cvoid}, page::Integer, idx::Integer)
	xy = _xy_series_data(h, page, idx).data
	rows = Vector{Vector{Float64}}()
	i, n = 1, size(xy, 1)
	while i <= n
		isfinite(xy[i, 1]) && isfinite(xy[i, 2]) || (i += 1; continue)
		j = i
		while j < n && isfinite(xy[j+1, 1]) && isfinite(xy[j+1, 2])  j += 1  end
		(xy[j, 1], xy[j, 2]) != (xy[i, 1], xy[i, 2]) && push!(rows, [xy[i, 1], xy[i, 2], xy[j, 1], xy[j, 2]])
		i = j + 1
	end
	isempty(rows) && error("the X,Y window's curve $idx on page $page has no arrow")
	return GMT.mat2ds(permutedims(reduce(hcat, rows)))
end

# A page's own labels (XYPage::labels: a biplot's variable names, a radar's spoke names), in order, read
# off the window's state text. Every label is its own `label=` key, so they are read from the raw lines:
# the section dicts keep only the last of a repeated key.
function _xy_page_labels(h::Ptr{Cvoid}, page::Integer)
	labs = NamedTuple{(:x, :y, :r, :g, :b, :size, :ha, :va, :text, :bold, :owner),
	                  Tuple{Float64,Float64,Float64,Float64,Float64,Float64,Int,Int,String,Bool,String}}[]
	p = -1; sec = ""
	for raw in split(_xy_session_state(h), '\n')
		line = rstrip(raw, '\r')
		if startswith(line, "[") && endswith(line, "]")
			sec = line[2:end-1]
			sec == "page" && (p += 1)
			continue
		end
		(sec == "page" && p == page) || continue
		if startswith(line, "label=")
			f = split(line[7:end], ' '; limit = 9)
			length(f) == 9 || continue
			v = _xy_nums(join(f[1:8], ' '))
			push!(labs, (x = v[1], y = v[2], r = v[3], g = v[4], b = v[5], size = v[6], ha = round(Int, v[7]),
			             va = round(Int, v[8]), text = _xy_unesc(f[9]), bold = false, owner = ""))
		elseif startswith(line, "labelbold=") && !isempty(labs)
			labs[end] = merge(labs[end], (bold = line[11:end] == "1",))
		elseif startswith(line, "labelowner=") && !isempty(labs)
			labs[end] = merge(labs[end], (owner = _xy_unesc(line[12:end]),))
		end
	end
	return labs
end

# Label k of a page as the dataset text! draws.
function _xy_label_data(h::Ptr{Cvoid}, page::Integer, k::Integer)
	l = _xy_page_labels(h, page)[k]
	return GMT.mat2ds([l.x l.y]; text = [l.text])
end

# A page's annotations (XYPage::annots: the tool bar's Text Arrow ... Ellipse), in drawing order, read
# off the window's state text the way `_xy_page_labels` reads the labels. `kind` is the window's
# XY_AN_*: 0 text arrow, 1 arrow, 2 double arrow, 3 line, 4 text box, 5 rectangle, 6 ellipse.
const _XY_ANNOT_NAMES = ("text arrow", "arrow", "double arrow", "line", "text box", "rectangle", "ellipse")
function _xy_page_annots(h::Ptr{Cvoid}, page::Integer)
	ans = NamedTuple[]
	p = -1; sec = ""
	for raw in split(_xy_session_state(h), '\n')
		line = rstrip(raw, '\r')
		if startswith(line, "[") && endswith(line, "]")
			sec = line[2:end-1]
			sec == "page" && (p += 1)
			continue
		end
		(sec == "page" && p == page) || continue
		if startswith(line, "annotbox=") && !isempty(ans)   # of the annotation just read: its box shown or not
			ans[end] = merge(ans[end], (box = line[10:end] != "0",))
			continue
		end
		startswith(line, "annot=") || continue
		f = split(line[7:end], ' '; limit = 18)
		length(f) >= 17 || continue
		v = _xy_nums(join(f[1:17], ' '))
		push!(ans, (kind = round(Int, v[1]), x0 = v[2], y0 = v[3], x1 = v[4], y1 = v[5], r = v[6], g = v[7], b = v[8],
		            width = v[9], ltype = round(Int, v[10]), size = v[11], filled = v[12] != 0, fr = v[13], fg = v[14],
		            fb = v[15], visible = v[16] != 0, text = length(f) == 18 ? _xy_unesc(f[18]) : "", box = true))
	end
	return ans
end

# The window's XY_AN_* kinds as annotation!'s (gmtannot.jl).
const _XY_ANNOT_KIND = (:textarrow, :arrow, :doublearrow, :line, :textbox, :rectangle, :ellipse)

# The page's annotations as script steps: ONE `annotation!` call each (gmtannot.jl, defined at the top
# of the script), its coordinates, pen, head, fill and text written out, so the script draws them with
# GMT.jl alone and a user edits one by editing its call. The text is placed as the window places it: a
# text arrow's at its tail, on the side away from the head (that side decided on the PAPER, where the
# axes' scales differ); a text box's inside its top-left corner.
function _xy_annot_steps!(steps, ctx, h::Ptr{Cvoid}, page::Integer, logx::Bool, logy::Bool)
	xr = ctx.region; w = ctx.figsize; hgt = w * 2 / 3
	sc(v, l) = l ? log10(abs(v)) : v
	for a in _xy_page_annots(h, page)
		a.visible || continue
		col = _xy_hex(a.r, a.g, a.b)
		r8(v) = round(v; sigdigits = 8)               # a dragged point's float noise is not a coordinate
		kw = Pair{Symbol,Any}[:x => (r8(a.x0), r8(a.x1)), :y => (r8(a.y0), r8(a.y1)),
		                      :pen => string(_xy_pt(a.width), "p,", col, haskey(_XY_GMT_LINE, a.ltype) ? "," * _XY_GMT_LINE[a.ltype] : "")]
		0 <= a.kind <= 2 && push!(kw, :head => string(_xy_pt(3 * max(2.5, a.width) + 4), "p"))   # xyAnnotHead's length
		a.kind >= 4 && a.filled && push!(kw, :fill => _xy_hex(a.fr, a.fg, a.fb))
		a.kind == 4 && !a.box && push!(kw, :box => false)        # a text box showing its text only
		if (a.kind == 0 || a.kind == 4) && !isempty(a.text)
			push!(kw, :text => a.text, :font => (a.size, "Helvetica", col))
			if a.kind == 4
				push!(kw, :justify => "TL", :offset => string(_xy_pt(4), "p/-", _xy_pt(4), "p"))
			else
				dx = (sc(a.x1, logx) - sc(a.x0, logx)) / (sc(xr[2], logx) - sc(xr[1], logx)) * w
				dy = (sc(a.y1, logy) - sc(a.y0, logy)) / (sc(xr[4], logy) - sc(xr[3], logy)) * hgt
				L = hypot(dx, dy); ux, uy = L > 0 ? (dx / L, dy / L) : (1.0, 0.0)
				push!(kw, :justify => abs(ux) >= abs(uy) ? (ux >= 0 ? "MR" : "ML") : (uy >= 0 ? "TC" : "BC"),
				          :offset => string(round(-ux * _xy_pt(3); digits = 2), "p/", round(-uy * _xy_pt(3); digits = 2), "p"))
			end
		end
		a.kind == 6 && logx && push!(kw, :logx => true)
		a.kind == 6 && logy && push!(kw, :logy => true)
		push!(steps, ScriptStep("annotation: $(_XY_ANNOT_NAMES[a.kind + 1])", DataBind[], :annotation,
		                        _XY_ANNOT_KIND[a.kind + 1], kw, true))
	end
	return steps
end

# The script's text, with `annotation!`'s definition (gmtannot.jl) right after `using GMT` when any step
# draws an annotation — so the script carries everything it calls and runs with GMT.jl alone.
function _xy_with_annot_helpers(txt::String, steps)::String
	any(s -> s.mod === :annotation, steps) || return txt
	block = "\n# ── annotation!: MATLAB's figure annotations drawn with GMT.jl (used by the calls at the end)\n" *
	        _GMT_ANNOT_SRC
	return replace(txt, "using GMT\n" => "using GMT\n" * block; count = 1)
end

# File > Plot with GMT.jl's text for an X,Y window: the rendered steps, plus what they call.
_xy_script_text(steps, ctx)::String = _xy_with_annot_helpers(_script_render_text(steps, ctx), steps)

# One series' legend entry as a pslegend "S" line, drawn as the window's legend draws it: an arrow as an
# arrow (its head as on the chart), bars as a box, bubbles as a circle, a marker-only curve as its
# marker, any other curve as its pen.
function _xy_legend_line(d::Dict{String,String}, kind::Integer, width::Real, col::String, name::String)
	pen = string(_xy_pt(width), "p,", col)
	kind == 11 && return string("S 0.4c v", _xy_pt(3 * max(2.5, width) + 4), "p+e+a", round(2atand(0.35); digits = 1),
	                            "+h0 0.8c ", col, " ", pen, " 0.9c ", name)
	kind == 1  && return string("S 0.4c r 0.4c ", col, " - 0.9c ", name)
	kind == 10 && return string("S 0.4c c 0.3c ", col, "@40 0.5p,", col, " 0.9c ", name)
	get(d, "nowcross", "0") == "1" &&
		return string("S 0.4c + ", _xy_pt(2 * _xy_nums(get(d, "cross", "0 0 24"))[3]), "p - ", pen, " 0.9c ", name)
	lt = parse(Int, get(d, "ltype", "1")); mk = parse(Int, get(d, "marker", "0"))
	if lt == 0 && haskey(_XY_GMT_MARKER, mk)
		code = Dict(:x => "x", :+ => "+", :square => "s", :circle => "c", :diamond => "d")[_XY_GMT_MARKER[mk]]
		return string("S 0.4c ", code, " ", _xy_pt(parse(Float64, get(d, "msize", "7"))), "p ", col, " - 0.9c ", name)
	end
	return string("S 0.4c - 0.8c - ", pen, haskey(_XY_GMT_LINE, lt) ? "," * _XY_GMT_LINE[lt] : "", " 0.9c ", name)
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
	# the page's own grid, per direction (Grid / X-Grid / Y-Grid); an older window state has only the
	# window-wide "grid"
	gwin = get(win, "grid", "1")
	gridx = get(pg, "gridx", gwin) == "1"; gridy = get(pg, "gridy", gwin) == "1"
	frame = (axes = :WSen, annot = :auto, ticks = :auto)
	# the page's title and subtitle (Title / Subtitle buttons, Axes properties): -B+t / -B+s
	ttl = _xy_unesc(get(pg, "title", "")); sub = _xy_unesc(get(pg, "subtitle", ""))
	isempty(ttl) || (frame = merge(frame, (title = ttl,)))
	isempty(sub) || (frame = merge(frame, (subtitle = sub,)))
	legendon = get(win, "legend", "1") == "1"

	steps = ScriptStep[]
	legs = String[]                                # the legend's entries, as pslegend "S" lines
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
			gridx && push!(kw, :xaxis => (grid = :auto,))       # -Bpxg: the vertical lines
			gridy && push!(kw, :yaxis => (grid = :auto,))       # -Bpyg: the horizontal ones
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
		elseif kind == 11            # arrows: GMT vectors from start to end point, the window's head shape
			var = ScriptVar(var.name, _xy_arrow_data(h, cur, idx))
			bind = DataBind(var, :live, "InteractiveGMT._xy_arrow_data(Ptr{Nothing}(UInt($(UInt(h)))), $cur, $idx)")
			hl = 3 * max(2.5, width) + 4          # head length px, half width 0.35 of it (vtkXYShapePlot)
			append!(kw, [:symbol => string("v", _xy_pt(hl), "p+e+a", round(2atand(0.35); digits = 1), "+h0+s"),
			             :pen => string(_xy_pt(width), "p,", col), :fill => col])
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
		if legendon && !isempty(name) && get(d, "inlegend", "1") == "1"
			push!(legs, _xy_legend_line(d, kind, width, col, name))
		end
		push!(steps, ScriptStep("$(name) — " * (kind == 1 ? "bars" : kind == 11 ? "arrows" : "curve"),
		                        DataBind[bind], mod, var, kw, !isempty(steps)))
	end
	isempty(steps) && error("gmtscript: this X,Y page has no visible curve to plot")
	# The legend is ALWAYS drawn by GMT's legend module, from the entries written out — every entry, one
	# box, each drawn as the window draws it (an arrow as an arrow) — never by the `legend=` of each call.
	if !isempty(legs)
		lv = _script_var!(ctx, "LEG", nothing)
		lv = ScriptVar(lv.name, GMT.text_record(legs))
		lb = DataBind(lv, :command, "text_record([\n\t" * join(repr.(legs), ",\n\t") * "])")
		push!(steps, ScriptStep("legend", DataBind[lb], :legend, lv,
		                        Pair{Symbol,Any}[:pos => (inside = true, anchor = :TR, offset = 0.2),
		                                         :box => (pen = 0.5, fill = :white)], true))
	end
	# The page's own labels (a biplot's variable names…): one text! each, its colour, size, weight and
	# anchor as on screen, not clipped by the frame (the window draws them just outside it too).
	hidden = Set(_xy_unesc(get(d, "name", "")) for d in series if parse(Int, d["page"]) == cur && get(d, "visible", "1") != "1")
	for (k, l) in enumerate(_xy_page_labels(h, cur))
		l.owner in hidden && continue              # its series is unchecked: hidden with it, as on screen
		var = _script_var!(ctx, "T", nothing)
		var = ScriptVar(var.name, _xy_label_data(h, cur, k))
		bind = DataBind(var, :live, "InteractiveGMT._xy_label_data(Ptr{Nothing}(UInt($(UInt(h)))), $cur, $k)")
		just = string(l.va < 0 ? "B" : l.va > 0 ? "T" : "M", l.ha < 0 ? "L" : l.ha > 0 ? "R" : "C")
		kw = Pair{Symbol,Any}[:font => (l.size, l.bold ? "Helvetica-Bold" : "Helvetica", _xy_hex(l.r, l.g, l.b)),
		                      :justify => just, :noclip => true]
		push!(steps, ScriptStep("label \"$(l.text)\"", DataBind[bind], :text, var, kw, true))
	end
	_xy_annot_steps!(steps, ctx, h, cur, logx, logy)   # the hand-drawn annotations, over everything
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
	txt = _xy_script_text(steps, ctx)
	for n in ctx.notes
		_xy_log(h, "script: NOT EXPORTED: $n"; err = false)
	end
	ccall(_fn(:gmtvtk_script_editor_show_h), Cint, (Ptr{Cvoid}, Cstring), h, txt)
	return nothing
end
