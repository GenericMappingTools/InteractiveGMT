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

# A curve as the pen draws it: a NaN in the window's curve is a break (a box plot's box, whiskers and
# caps are one curve), and psxy would join across it, so each run of finite points is its own segment.
function _xy_series_segs(h::Ptr{Cvoid}, page::Integer, idx::Integer, name::AbstractString = "")
	D = _xy_series_data(h, page, idx, name)
	xy = D.data
	runs = _xy_runs([isfinite(xy[i, 1]) && isfinite(xy[i, 2]) for i in 1:size(xy, 1)])
	length(runs) == 1 && length(runs[1]) == size(xy, 1) && return D
	isempty(runs) && error("the X,Y window's curve $idx on page $page has no finite point")
	return [GMT.mat2ds(xy[r, :]; colnames = D.colnames) for r in runs]
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

# The [series] sections of one page, in the window's order.
_xy_page_series(h::Ptr{Cvoid}, page::Integer) =
	[d for (sec, d) in _xy_manifest_sections(_xy_session_state(h)) if sec == "series" && parse(Int, d["page"]) == page]

# What curve `idx` of a page stands on, row by row — xyRestack (65_xyplot.cpp) over the same series in
# the same order: 0 (stack 0), the stack of the series before it (1), its own running sum (2, the
# waterfall), the page's first curve (3, fill_between).
function _xy_series_base(h::Ptr{Cvoid}, page::Integer, idx::Integer)
	run = Float64[]
	for d in _xy_page_series(h, page)
		k = parse(Int, d["idx"]); st = parse(Int, get(d, "stack", "0"))
		n = parse(Int, get(d, "npoints", "0"))
		if st == 0 || n <= 0                   # stands on y = 0, and adds nothing to a stack
			k == idx && return zeros(max(n, 0))
			continue
		end
		y = _xy_series_data(h, page, k).data[:, 2]
		if st == 1
			length(run) < n && append!(run, zeros(n - length(run)))
			base = run[1:n]
			for i in 1:n  isfinite(y[i]) && (run[i] += y[i])  end
		elseif st == 2
			base = zeros(n); c = 0.0
			for i in 1:n  base[i] = c; isfinite(y[i]) && (c += y[i])  end
		else
			y0 = _xy_series_data(h, page, 0).data[:, 2]
			base = [i <= length(y0) ? y0[i] : NaN for i in 1:n]
		end
		k == idx && return base
	end
	error("the X,Y window has no curve $idx on page $page")
end

# The runs of rows where every column is finite (a NaN row breaks a curve, as on the chart).
function _xy_runs(ok::AbstractVector{Bool})
	runs = UnitRange{Int}[]; i = 1; n = length(ok)
	while i <= n
		ok[i] || (i += 1; continue)
		j = i
		while j < n && ok[j+1]  j += 1  end
		push!(runs, i:j); i = j + 1
	end
	return runs
end

# A bar curve as GMT's -Sb (vertical) / -SB (horizontal) rows: position shifted by `shift` (a grouped
# bar's place in its group), value; with `based`, the top and the base it stands on (`+b` reads it
# from the last column) — stacked bars and the waterfall.
function _xy_bar_data(h::Ptr{Cvoid}, page::Integer, idx::Integer, horiz::Bool, shift::Real, based::Bool)
	xy = _xy_series_data(h, page, idx).data
	b = based ? _xy_series_base(h, page, idx) : zeros(size(xy, 1))
	keep = [isfinite(xy[i, 1]) && isfinite(xy[i, 2]) && isfinite(b[i]) for i in 1:size(xy, 1)]
	p = xy[keep, 1] .+ shift; v = xy[keep, 2] .+ b[keep]
	M = based ? (horiz ? [v p b[keep]] : [p v b[keep]]) : (horiz ? [v p] : [p v])
	return GMT.mat2ds(M)
end

# An area as the polygon the chart fills: the curve forward, the base it stands on back (y = 0, the
# curve under it, or — on a log Y, which has no 0 — the lowest point). One polygon per finite run.
function _xy_area_data(h::Ptr{Cvoid}, page::Integer, idx::Integer, logy::Bool)
	xy = _xy_series_data(h, page, idx).data
	b = _xy_series_base(h, page, idx)
	floor = logy && all(==(0), b)
	ok = [isfinite(xy[i, 1]) && isfinite(xy[i, 2]) && (!logy || xy[i, 2] > 0) && isfinite(b[i]) for i in 1:size(xy, 1)]
	floor && (b = fill(minimum(xy[ok, 2]; init = Inf), length(b)))
	polys = [GMT.mat2ds([xy[r, 1] xy[r, 2]; reverse(xy[r, 1]) reverse(b[r])]) for r in _xy_runs(ok) if length(r) >= 2]
	isempty(polys) && error("the X,Y window's curve $idx on page $page has nothing to fill")
	return polys
end

# A stem curve's sticks, base to point: one two-point segment each.
function _xy_stem_data(h::Ptr{Cvoid}, page::Integer, idx::Integer, logy::Bool)
	xy = _xy_series_data(h, page, idx).data
	b = _xy_series_base(h, page, idx)
	ok = [isfinite(xy[i, 1]) && isfinite(xy[i, 2]) && (!logy || xy[i, 2] > 0) for i in 1:size(xy, 1)]
	logy && all(==(0), b) && (b = fill(minimum(xy[ok, 2]; init = Inf), length(b)))
	ok .&= isfinite.(b)
	any(ok) || error("the X,Y window's curve $idx on page $page has no stem")
	return [GMT.mat2ds([xy[i, 1] b[i]; xy[i, 1] xy[i, 2]]) for i in findall(ok)]
end

# Filled polygons (a radar's area, a rose petal, a pie slice): one per NaN-separated run of 3+ points.
# `merged`: the pieces that share an edge joined into one outline (a pie slice is cut into convex 10°
# pieces for the chart's filler; filled one by one on the paper, their common edges show as seams).
function _xy_poly_data(h::Ptr{Cvoid}, page::Integer, idx::Integer, merged::Bool = false)
	xy = _xy_series_data(h, page, idx).data
	ok = [isfinite(xy[i, 1]) && isfinite(xy[i, 2]) for i in 1:size(xy, 1)]
	pieces = [xy[r, :] for r in _xy_runs(ok) if length(r) >= 3]
	isempty(pieces) && error("the X,Y window's curve $idx on page $page has no polygon")
	return [GMT.mat2ds(p) for p in (merged ? _xy_poly_merge(pieces) : pieces)]
end

# Polygons -> their union's outlines, for pieces that tile a shape edge to edge: every edge two pieces
# share (one each way) is dropped and what is left is chained into closed loops. A loop inside another
# (a donut's hole) is joined to it by a cut there and back, so the pair fills as one ring.
function _xy_poly_merge(pieces::Vector{Matrix{Float64}})
	pt(p, i) = (p[i, 1] + 0.0, p[i, 2] + 0.0)          # + 0.0: -0.0 and 0.0 are one vertex
	edges = Dict{Tuple{NTuple{2,Float64},NTuple{2,Float64}},Int}()
	for p in pieces
		v = NTuple{2,Float64}[]
		for i in 1:size(p, 1)
			q = pt(p, i); (isempty(v) || v[end] != q) && push!(v, q)
		end
		length(v) > 1 && v[1] == v[end] && pop!(v)
		for i in eachindex(v)
			e = (v[i], v[mod1(i + 1, length(v))])
			if get(edges, reverse(e), 0) > 0
				edges[reverse(e)] -= 1
			else
				edges[e] = get(edges, e, 0) + 1
			end
		end
	end
	next = Dict{NTuple{2,Float64},Vector{NTuple{2,Float64}}}()
	for (e, n) in edges, _ in 1:n
		push!(get!(next, e[1], NTuple{2,Float64}[]), e[2])
	end
	loops = Vector{NTuple{2,Float64}}[]
	while !isempty(next)
		a = first(keys(next)); loop = [a]
		while true
			nb = next[a]; b = pop!(nb); isempty(nb) && delete!(next, a)
			b == loop[1] && break
			push!(loop, b); a = b
			haskey(next, a) || break                     # an open chain: not a tiling, keep what it is
		end
		length(loop) >= 3 && push!(loops, loop)
	end
	isempty(loops) && return pieces
	inside(q, L) = (c = false; n = length(L);
		for i in 1:n
			(x1, y1), (x2, y2) = L[i], L[mod1(i + 1, n)]
			((y1 > q[2]) != (y2 > q[2])) && q[1] < x1 + (q[2] - y1) * (x2 - x1) / (y2 - y1) && (c = !c)
		end; c)
	outer = [k for k in eachindex(loops) if !any(j != k && inside(loops[k][1], loops[j]) for j in eachindex(loops))]
	out = Matrix{Float64}[]
	for k in outer
		L = [loops[k]; [loops[k][1]]]
		for j in eachindex(loops)
			(j in outer || !inside(loops[j][1], loops[k])) && continue
			append!(L, [loops[j]; [loops[j][1], L[1]]])    # the cut to the hole, round it, and back
		end
		push!(out, [first.(L) last.(L)])
	end
	return out
end

# A live fetch of one curve through `fn` (one of the readers above): the bind the script writes, with
# the object the live sink hands over.
function _xy_live(h::Ptr{Cvoid}, var, fn::Function, page::Integer, idx::Integer, args...)
	expr = string("InteractiveGMT.", nameof(fn), "(Ptr{Nothing}(UInt(", UInt(h), ")), ", page, ", ", idx,
	              join(", " .* map(repr, collect(args))), ")")
	return DataBind(ScriptVar(var.name, fn(h, page, idx, args...)), :live, expr)
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

# A page's Axes properties fields changed from the dialog's defaults (XYPage::axProps: "KEY value"), read
# off the raw lines like the labels, each `axprop=` being its own key.
function _xy_page_axprops(h::Ptr{Cvoid}, page::Integer)
	props = Dict{String,String}()
	p = -1; sec = ""
	for raw in split(_xy_session_state(h), '\n')
		line = rstrip(raw, '\r')
		if startswith(line, "[") && endswith(line, "]")
			sec = line[2:end-1]
			sec == "page" && (p += 1)
			continue
		end
		(sec == "page" && p == page && startswith(line, "axprop=")) || continue
		f = split(line[8:end], ' '; limit = 2)
		props[String(f[1])] = length(f) == 2 ? _xy_unesc(f[2]) : ""
	end
	return props
end

# The step between the ticks the chart annotates axis `which` (0 bottom, 1 left, 2 right) at, or
# nothing when it has fewer than two.
function _xy_axis_step(h::Ptr{Cvoid}, which::Integer)
	n = ccall(_fn(:gmtvtk_xyplot_axis_ticks), Cint, (Ptr{Cvoid}, Cint, Ptr{Float64}, Cint), h, which, C_NULL, 0)
	n < 2 && return nothing
	t = Vector{Float64}(undef, n)
	ccall(_fn(:gmtvtk_xyplot_axis_ticks), Cint, (Ptr{Cvoid}, Cint, Ptr{Float64}, Cint), h, which, t, n)
	d = filter(>(0), diff(sort(t)))
	return isempty(d) ? nothing : round(minimum(d); sigdigits = 6)
end

# Label k of a page as the dataset text! draws. A degree sign is GMT's own escape (@.): the UTF-8 one
# comes out as two Latin-1 characters.
function _xy_label_data(h::Ptr{Cvoid}, page::Integer, k::Integer)
	l = _xy_page_labels(h, page)[k]
	return GMT.mat2ds([l.x l.y]; text = [replace(l.text, "°" => "@.")])
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
function _xy_annot_steps!(steps, ctx, h::Ptr{Cvoid}, page::Integer, logx::Bool, logy::Bool,
                          mpt::Function, tpt::Function)
	xr = ctx.region; w = ctx.figsize; hgt = parse(Float64, match(r"/([0-9.]+)", ctx.proj)[1])   # PROJ's height
	sc(v, l) = l ? log10(abs(v)) : v
	for a in _xy_page_annots(h, page)
		a.visible || continue
		col = _xy_hex(a.r, a.g, a.b)
		r8(v) = round(v; sigdigits = 8)               # a dragged point's float noise is not a coordinate
		kw = Pair{Symbol,Any}[:x => (r8(a.x0), r8(a.x1)), :y => (r8(a.y0), r8(a.y1)),
		                      :pen => string(mpt(a.width), "p,", col, haskey(_XY_GMT_LINE, a.ltype) ? "," * _XY_GMT_LINE[a.ltype] : "")]
		0 <= a.kind <= 2 && push!(kw, :head => string(mpt(3 * max(2.5, a.width) + 4), "p"))   # xyAnnotHead's length
		a.kind >= 4 && a.filled && push!(kw, :fill => _xy_hex(a.fr, a.fg, a.fb))
		a.kind == 4 && !a.box && push!(kw, :box => false)        # a text box showing its text only
		if (a.kind == 0 || a.kind == 4) && !isempty(a.text)
			push!(kw, :text => a.text, :font => (tpt(a.size), "Helvetica", col))
			if a.kind == 4
				push!(kw, :justify => "TL", :offset => string(mpt(4), "p/-", mpt(4), "p"))
			else
				dx = (sc(a.x1, logx) - sc(a.x0, logx)) / (sc(xr[2], logx) - sc(xr[1], logx)) * w
				dy = (sc(a.y1, logy) - sc(a.y0, logy)) / (sc(xr[4], logy) - sc(xr[3], logy)) * hgt
				L = hypot(dx, dy); ux, uy = L > 0 ? (dx / L, dy / L) : (1.0, 0.0)
				push!(kw, :justify => abs(ux) >= abs(uy) ? (ux >= 0 ? "MR" : "ML") : (uy >= 0 ? "TC" : "BC"),
				          :offset => string(round(-ux * mpt(3); digits = 2), "p/", round(-uy * mpt(3); digits = 2), "p"))
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

# File > Plot with GMT.jl's text for an X,Y window: the rendered steps, plus what they call. A figure
# ended by gmtend(show=true) (a corner plot's, _xy_gmtjl_steps) or drawn by a climate function that
# shows it itself (enso(), nao(), soi(), keeling()) is shown by it: no showfig() after it, which would show the LAST
# CLASSIC figure instead (a radar drawn earlier...).
function _xy_script_text(steps, ctx)::String
	txt = _xy_with_annot_helpers(_script_render_text(steps, ctx), steps)
	any(s -> s.mod === :gmtend || String(s.mod) in _XY_CLIMATE_FNS, steps) &&
		(txt = replace(txt, r"\n+showfig\(\)\s*$" => "\n"))
	return txt
end

# One series' legend entry as a pslegend "S" line, drawn as the window's legend draws it: an arrow as an
# arrow (its head as on the chart), bars as a box, bubbles as a circle, a marker-only curve as its
# marker, a line with markers (a stem) as its pen with the marker on it, any other curve as its pen.
# Most are one line; a pen with a marker is three. `spt` turns a symbol's chart pixels into its points on the paper.
function _xy_legend_line(d::Dict{String,String}, kind::Integer, width::Real, col::String, name::String, spt::Function,
                         mpt::Function, k::Real = 1.0)
	c(v) = string(round(v * k; sigdigits = 3), "c")  # the entry's geometry, scaled with its font (k = font / 12p)
	pen = string(spt(width), "p,", col)
	kind == 11 && return string("S ", c(0.4), " v", spt(3 * max(2.5, width) + 4), "p+e+a", round(2atand(0.35); digits = 1),
	                            "+h0 ", c(0.8), " ", col, " ", pen, " ", c(0.9), " ", name)
	box = string(" s ", round(0.4 * k * sqrt(2); sigdigits = 3), "c ")   # a square of side 0.4 (GMT sizes it by its circle)
	kind == 1  && return string("S ", c(0.4), box, col, " ", spt(width), "p,black ", c(0.9), " ", name)
	kind == 10 && return string("S ", c(0.4), " c ", c(0.3), " ", col, "@40 0.5p,", col, " ", c(0.9), " ", name)
	# an area, stacked bars, filled polygons: a box in the fill, outlined by the pen (vtkXYShapePlot's)
	kind in (2, 6, 7, 8) && return string("S ", c(0.4), box, col, kind == 2 ? "@41" : kind == 8 ? _xy_fill_transp(d) : "", " ",
	                                      kind == 8 && get(d, "ltype", "1") == "0" ? "-" : pen, " ", c(0.9), " ", name)
	get(d, "nowcross", "0") == "1" &&
		return string("S ", c(0.4), " + ", spt(2 * _xy_nums(get(d, "cross", "0 0 24"))[3]), "p - ", pen, " ", c(0.9), " ", name)
	lt = parse(Int, get(d, "ltype", "1")); mk = parse(Int, get(d, "marker", "0"))
	kind == 5 && (lt = 0; mk == 0 && (mk = 4))       # a scatter: its points, a circle unless it has a marker
	kind == 3 && mk == 0 && (mk = 4)                 # a stem: its heads, circles unless it has a marker
	line = string("S ", c(0.4), " - ", c(0.8), " - ", pen, haskey(_XY_GMT_LINE, lt) ? "," * _XY_GMT_LINE[lt] : "")
	haskey(_XY_GMT_MARKER, mk) || return string(line, " ", c(0.9), " ", name)
	code = Dict(:x => "x", :+ => "+", :square => "s", :circle => "c", :diamond => "d")[_XY_GMT_MARKER[mk]]
	ms = parse(Float64, get(d, "msize", "7")) * (code == "s" ? sqrt(2) : 1.0)   # a square: by its circumscribing circle
	mark = string("S ", c(0.4), " ", code, " ", mpt(ms), "p ", col, " - ", c(0.9), " ", name)
	lt == 0 && return mark
	# a line with markers: its pen, then (one line back up, G -1l) its marker over the pen's middle
	return [line, "G -1l", mark]
end

# A filled polygon's fill transparency as GMT's @t suffix: the chart's (alpha 110/255) unless the series
# carries its own opacity (XYSeries::fillAlpha).
_xy_fill_transp(d::Dict{String,String}) =
	haskey(d, "fillalpha") ? (t = round(Int, 100 * (1 - clamp(parse(Float64, d["fillalpha"]), 0, 1))); t == 0 ? "" : "@$t") : "@57"

_xy_hex(r, g, b) ="#" * join(string(round(Int, 255 * clamp(v, 0, 1)); base = 16, pad = 2) for v in (r, g, b))

# The X,Y chart's pen / marker codes -> GMT (vtkPen: 1 solid, 2 dash, 3 dot, 4 dash-dot, 5 dash-dot-dot;
# vtkPlotPoints: 1 cross, 2 plus, 3 square, 4 circle, 5 diamond). Widths are px on a 96-dpi chart.
const _XY_GMT_LINE   = Dict(2 => "dashed", 3 => "dotted", 4 => "-.", 5 => "-..")
const _XY_GMT_MARKER = Dict(1 => :x, 2 => :+, 3 => :square, 4 => :circle, 5 => :diamond)
_xy_pt(px) = round(Float64(px) * 72 / 96; sigdigits = 3)

# The data a page that IS one GMT.jl function was drawn from (XYPage::gjSpec), as that function takes it:
# one named column per variable (cornerplot, parallelplot), or one row per polygon (radar). The script
# fetches it from the live window.
function _xy_gmtjl_data(h::Ptr{Cvoid}, page::Integer)
	for (sec, d) in _xy_manifest_sections(_xy_session_state(h))
		sec == "page" || continue
		page == 0 || (page -= 1; continue)
		fn, M, nms = _xy_gmtjl_parse(_xy_unesc(d["gmtjl.spec"]))
		return fn == "radar" ? GMT.mat2ds(M) : GMT.mat2ds(M; colnames = nms)
	end
	error("the X,Y window has no page $page drawn by a GMT.jl function")
end

# A page that IS one GMT.jl function is that ONE call on the page's own data — cornerplot(), radar(),
# parallelplot() — never the panels, polygons and rulers the window draws it with, followed by the
# window's legend (`steps` holds what the generic emitter made of the page; only its legend is kept). The
# legend sits in the corner of the figure the window's sits in.
function _xy_gmtjl_steps(h::Ptr{Cvoid}, pg::Dict{String,String}, cur::Integer, steps, ctx, lcorner::Symbol)
	spec = _xy_unesc(pg["gmtjl.spec"])
	clim = match(r"^fn=(\w+)\s*$", spec)
	clim !== nothing && String(clim[1]) in _XY_CLIMATE_FNS && return _xy_climate_steps(String(clim[1]), ctx)
	fn, M, nms = _xy_gmtjl_parse(spec)
	v = _script_var!(ctx, "D", nothing)
	bind = DataBind(ScriptVar(v.name, _xy_gmtjl_data(h, cur)), :live,
	                "InteractiveGMT._xy_gmtjl_data(Ptr{Nothing}(UInt($(UInt(h)))), $cur)")
	kw = Pair{Symbol,Any}[]
	out = ScriptStep[]
	what = fn
	lbox = Pair{Symbol,Any}[]                    # the box the legend is placed in (default: the last call's)
	if fn == "cornerplot"
		# its pairs as the side panel says (scatter=true, hexbin=true, density=true, contour=true; Auto is
		# cornerplot()'s own default, so no keyword)
		style = _XY_CORNER_STYLES[clamp(parse(Int, get(pg, "gmtjl.style", "0")), 0, length(_XY_CORNER_STYLES) - 1) + 1]
		style == "auto" || push!(kw, Symbol(style) => true)
		push!(ctx.notes, "each histogram's and each pair's own colour: GMT.jl's cornerplot() has ONE histcolor " *
		                 "and one point colour for all of them")
		style in ("density", "contour") &&
			push!(ctx.notes, "the $style pairs: GMT.jl's cornerplot() has no `$style` keyword yet and draws its default")
		what = "cornerplot ($(style == "auto" ? "scatter up to 1000 points, hexbin above" : style))"
		# cornerplot() is a subplot that ends its own figure: inside gmtbegin()...gmtend() it only ends the
		# subplot, so the legend can still go on the figure. Its panel size is subplot()'s own guess.
		n = size(M, 2)
		W = round(n * (n == 2 ? 8.0 : n == 3 ? 6.0 : 20 / n); digits = 2)
		lbox = Pair{Symbol,Any}[:region => (0, 1, 0, 1), :proj => "X$(W)/$(W)"]
		# The file: in GMT.jl's own temp folder (a bare gmtbegin() writes GMTplot.png into the CURRENT
		# folder), the previous Run's removed first — a viewer still showing it makes Ghostscript fail
		# ("Could not open the file"), measured on a second Run.
		fv = _script_var!(ctx, "FIG", nothing)
		fexpr = "joinpath(GMT.TMPDIR_USR.dir, \"GMTjl_cornerplot.png\")"
		fig = DataBind(ScriptVar(fv.name, joinpath(GMT.TMPDIR_USR.dir, "GMTjl_cornerplot.png")), :command,
		               "(f = $fexpr; rm(f, force=true); f)")
		push!(out, ScriptStep("the figure: cornerplot() ends its subplot inside it, the legend goes on top",
		                      DataBind[fig], :gmtbegin, fig.var, Pair{Symbol,Any}[], false))
	elseif fn == "radar"
		# radar()'s own reading: data in [0, 1] is normalized; anything else gets each axis' limit (its
		# maximum, which radar() rounds as the window does) and every axis annotated
		if !all(t -> 0 <= t <= 1, M)
			push!(kw, :axeslimts => vec(maximum(M; dims = 1)), :annotall => true)
		end
		push!(kw, :fill => true)
	end
	ttl = _xy_unesc(get(pg, "title", ""))
	isempty(ttl) || push!(kw, :title => ttl)
	push!(out, ScriptStep(what, DataBind[bind], Symbol(fn), bind.var, kw, false))
	k = findfirst(s -> s.mod === :legend, steps)
	if k !== nothing
		lk = [p for p in steps[k].kw if !(first(p) in (:pos, :region, :proj))]
		push!(out, ScriptStep("legend", steps[k].binds, :legend, steps[k].pos,
		                      vcat(lbox, Pair{Symbol,Any}[:pos => (anchor = lcorner, inside = true, offset = 0.2)], lk), true))
	end
	fn == "cornerplot" && push!(out, ScriptStep("show the figure", DataBind[], :gmtend, nothing,
	                                            Pair{Symbol,Any}[:show => true], false))
	return out
end

# A climate record's page (T & L > ENSO / NAO / SOI / Keeling curve, climate.jl): GMT.jl's function that downloads
# the record and draws it, called with nothing — the page's data is the download, its look the
# function's own. Such a function shows its figure itself (show=true), so the script ends with it.
const _XY_CLIMATE_FNS = ("enso", "nao", "soi", "keeling")
const _XY_CLIMATE_WHAT = Dict(
	"enso"    => "the Oceanic Niño Index, downloaded from NOAA CPC and drawn by GMT.jl",
	"nao"     => "the North Atlantic Oscillation index, downloaded from NOAA CPC and drawn by GMT.jl",
	"soi"     => "the Southern Oscillation Index, downloaded from NOAA PSL and drawn by GMT.jl",
	"keeling" => "the Keeling curve (Mauna Loa CO2), downloaded from NOAA GML and drawn by GMT.jl")
function _xy_climate_steps(fn::String, ctx)
	push!(ctx.notes, "the page's own look (colours, legend, axes): GMT.jl's $fn() downloads the record and draws it its own way")
	what = _XY_CLIMATE_WHAT[fn]
	return ScriptStep[ScriptStep(what, DataBind[], Symbol(fn), nothing, Pair{Symbol,Any}[], false)]
end

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
	# a page that draws its own frame (radar, rose, polar, pie, corner, biplot…) has a square plot area
	square = get(pg, "square", "0") == "1"; hidden_axes = get(pg, "axeshidden", "0") == "1"
	# The plot area as on screen: its shape is the figure's. Pens and symbols are sized in chart pixels: on
	# the paper they take the share of the plot width they take on screen, not their 96-dpi size. A pen
	# width, an arrow head, the now-cross and the error caps are device pixels; a marker size and the bubble
	# range are logical ones, times the pixel ratio.
	plotw, ploth, dpr, dpi = Ref(0.0), Ref(0.0), Ref(1.0), Ref(72.0)
	ccall(_fn(:gmtvtk_xyplot_plot_width), Cint, (Ptr{Cvoid}, Ref{Cdouble}, Ref{Cdouble}, Ref{Cdouble}, Ref{Cdouble}),
	      h, plotw, ploth, dpr, dpi)
	w = Float64(figsize)
	hgt = square ? w : round(plotw[] > 0 && ploth[] > 0 ? w * ploth[] / plotw[] : w * 2 / 3; digits = 2)
	proj = string("X", w, logx ? "l" : "", "/", hgt, logy ? "l" : "")
	ctx = ScriptCtx(h, (xr[1], xr[2], yr[1], yr[2]), false, proj, w, nothing, nothing, false,
	                Dict{String,Int}(), String[], false, 0, 96.0)
	ptpx = plotw[] > 0 ? w / plotw[] * 72 / 2.54 : 72 / 96
	spt(px) = round(Float64(px) * ptpx; sigdigits = 3)
	mpt(px) = spt(px * dpr[])
	tpt(sz) = spt(sz * dpi[] / 72)                 # a text of size sz: sz * DPI / 72 chart pixels tall
	legf = tpt(parse(Float64, get(win, "legendfont", "12")))
	axp = _xy_page_axprops(h, cur)
	# the axes' text as the chart draws it: annotations 11, axis titles 13 bold, or the size, face and
	# colour Axes properties gave them (a size in points is the chart's font size, axpApplyFont)
	function axfont(key, sz, face)
		mt = match(r"^\s*([0-9.]+)\s*p?\s*$", get(axp, key * ".size", ""))
		mt === nothing || (sz = parse(Float64, mt[1]))
		fn = get(axp, key * ".name", ""); fc = get(axp, key * ".color", "")
		return string(tpt(sz), "p,", isempty(fn) ? face : fn, isempty(fc) ? "" : "," * fc)
	end
	axfonts = (FONT_ANNOT_PRIMARY = axfont("FONT_ANNOT_PRIMARY", 11, "Helvetica"),
	           FONT_LABEL = axfont("FONT_LABEL", 13, "Helvetica-Bold"))
	length(pages) > 1 && push!(ctx.notes, "only the page shown ('$(_xy_unesc(pg["name"]))') is exported; " *
	                                      "the window has $(length(pages)) pages")
	timefmt = parse(Int, get(pg, "xtime", "0"))
	# the page's own grid, per direction (Grid / X-Grid / Y-Grid); an older window state has only the
	# window-wide "grid"
	gwin = get(win, "grid", "1")
	gridx = get(pg, "gridx", gwin) == "1" && !hidden_axes; gridy = get(pg, "gridy", gwin) == "1" && !hidden_axes
	pser = [d for d in series if parse(Int, d["page"]) == cur]
	shown(d) = get(d, "visible", "1") == "1"
	# plotyy: the curves read against the right-hand Y axis, which then carries its own annotations
	rights = [d for d in pser if shown(d) && get(d, "right", "0") == "1"]
	frame = (axes = isempty(rights) ? :WSen : :WSn, annot = :auto, ticks = :auto)
	# the page's title and subtitle (Title / Subtitle buttons, Axes properties): -B+t / -B+s
	ttl = _xy_unesc(get(pg, "title", "")); sub = _xy_unesc(get(pg, "subtitle", ""))
	isempty(ttl) || (frame = merge(frame, (title = ttl,)))
	isempty(sub) || (frame = merge(frame, (subtitle = sub,)))
	hidden_axes && (frame = :none)                 # its own frame is drawn by its curves and labels
	legendon = get(win, "legend", "1") == "1"
	# The right-hand axis' range: what its curves span (the window's is automatic).
	yr2 = if isempty(rights) nothing else
		v = vcat([filter(isfinite, _xy_series_data(h, cur, parse(Int, d["idx"])).data[:, 2]) for d in rights]...)
		lo, hi = isempty(v) ? (0.0, 1.0) : extrema(v)
		hi > lo ? (lo, hi) : (lo - 1, hi + 1)
	end
	# Bars (kind 1) are grouped as vtkChartXY::CalculateBarPlots groups them: every visible bar curve of
	# the page gets 0.8 of the FIRST one's x spacing / their number, side by side around each x.
	bars = [parse(Int, d["idx"]) for d in pser if shown(d) && get(d, "kind", "0") == "1" && get(d, "nowcross", "0") != "1"]
	barw = 0.1
	if !isempty(bars)
		x1 = _xy_series_data(h, cur, bars[1]).data[:, 1]
		length(x1) > 1 && (barw = abs(x1[2] - x1[1]) * 0.8 / length(bars))
	end

	steps = ScriptStep[]
	legs = String[]                                # the legend's entries, as pslegend "S" lines
	lcorner = :TR                                  # the corner the window's legend sits in
	for d in pser
		shown(d) || continue
		idx = parse(Int, d["idx"]); name = _xy_unesc(get(d, "name", ""))
		r, g, b = _xy_nums(get(d, "rgb", "0 0 0")); col = _xy_hex(r, g, b)
		kind = parse(Int, get(d, "kind", "0"))
		width = parse(Float64, get(d, "width", "1.5"))
		lt = parse(Int, get(d, "ltype", "1")); mk = parse(Int, get(d, "marker", "0"))
		pen = string(spt(width), "p,", col, haskey(_XY_GMT_LINE, lt) ? "," * _XY_GMT_LINE[lt] : "")
		msz = string(mpt(parse(Float64, get(d, "msize", "7"))), "p")
		# GMT's square is sized by its circumscribing circle, the chart's by its side
		msq = string(mpt(sqrt(2) * parse(Float64, get(d, "msize", "7"))), "p")
		# the curve's points as they are, or through one of the readers above
		curve() = (v = _script_var!(ctx, "D", nothing);
		           DataBind(ScriptVar(v.name, _xy_series_data(h, cur, idx, name)), :live,
		                    "InteractiveGMT._xy_series_data(Ptr{Nothing}(UInt($(UInt(h)))), $cur, $idx, $(repr(name)))"))
		# the same points split at their NaN breaks, for a call that draws them with a pen
		segs() = through(_xy_series_segs, name)
		through(fn, args...) = _xy_live(h, _script_var!(ctx, "D", nothing), fn, cur, idx, args...)
		marks(m) = Pair{Symbol,Any}[:marker => _XY_GMT_MARKER[m], :ms => m == 3 ? msq : msz, :mc => col]
		# What the series is drawn as, in drawing order: (what, data, keywords). Most kinds are one call; an
		# area is its fill then its curve, a stem its sticks then its heads, error bars their curve then bars.
		calls = Tuple{String,DataBind,Vector{Pair{Symbol,Any}}}[]
		if get(d, "nowcross", "0") == "1"
			cx, cy, csz = _xy_nums(get(d, "cross", "0 0 24"))
			bd = curve()
			push!(calls, ("cross", DataBind(ScriptVar(bd.var.name, GMT.mat2ds([cx cy])), :live, bd.expr),
			              Pair{Symbol,Any}[:marker => :+, :ms => string(spt(2csz), "p"), :ml => string(spt(width), "p,", col)]))
		elseif kind == 1             # bars (histogram / bar graph): GMT's bar symbol, width in x units
			# `plot` with -Sb/-SB, not GMT.jl's `bar`: that one always adds `-C -i0-1,1` and paints each bar
			# its own colour whatever `fill` says (GMT.jl bug, reported, not patched here). vtkPlotBar's
			# outline is its default black pen.
			horiz = get(d, "barh", "0") == "1"
			j = something(findfirst(==(idx), bars), 1)
			shift = -(length(bars) - 1 - 2 * (j - 1)) * barw / 2      # vtkPlotBar's offset, subtracted
			push!(calls, ("bars", through(_xy_bar_data, horiz, shift, false),
			              Pair{Symbol,Any}[:symbol => string(horiz ? "B" : "b", round(barw; sigdigits = 8), "u+b0"),
			                               :fill => col, :ml => string(spt(width), "p,black")]))
		elseif kind == 6 || kind == 7   # stacked bars / waterfall: each bar from what it stands on
			bw = round(parse(Float64, get(d, "barwidth", "1")); sigdigits = 8)
			push!(calls, ("bars", through(_xy_bar_data, kind == 7, 0.0, true),
			              Pair{Symbol,Any}[:symbol => string(kind == 7 ? "B" : "b", bw, "u+b"), :fill => col, :ml => pen]))
		elseif kind == 2             # an area: see-through fill down to its base, then the curve on it
			push!(calls, ("area", through(_xy_area_data, logy), Pair{Symbol,Any}[:fill => col * "@41"]))
			lt != 0 && push!(calls, ("curve", segs(), Pair{Symbol,Any}[:pen => pen]))
		elseif kind == 3             # a stem: a stick from the base to each point, a head on it
			push!(calls, ("stems", through(_xy_stem_data, logy), Pair{Symbol,Any}[:pen => pen]))
			push!(calls, ("heads", curve(), marks(mk == 0 ? 4 : mk)))
		elseif kind == 4             # stairs: GMT's -Ax, along x first, then up/down
			lt != 0 && push!(calls, ("stairs", segs(), Pair{Symbol,Any}[:pen => pen, :steps => "x"]))
			haskey(_XY_GMT_MARKER, mk) && push!(calls, ("markers", curve(), marks(mk)))
		elseif kind == 5             # a scatter: its points only, circles unless it has a marker
			push!(calls, ("points", curve(), marks(haskey(_XY_GMT_MARKER, mk) ? mk : 4)))
		elseif kind == 8             # filled polygons (radar, rose, pie…): see-through fill, outlined by the pen
			# the fill over the pieces joined (no seams), the pen round each piece as the chart draws it
			push!(calls, ("polygons", through(_xy_poly_data, true), Pair{Symbol,Any}[:fill => col * _xy_fill_transp(d)]))
			lt != 0 && push!(calls, ("outlines", through(_xy_poly_data, false), Pair{Symbol,Any}[:pen => pen, :close => true]))
		elseif kind == 10            # bubbles: one circle per point, its diameter as the window maps it
			sz = _xy_nums(get(d, "bubble", ""))
			lo, hi = isempty(sz) ? (0.0, 0.0) : extrema(filter(isfinite, sz))
			d0, d1 = 5.0 * 96 / 72, 20.0 * 96 / 72   # px: the window's bubble range (XY_BUBBLE_PT_LO/HI)
			dia = [isfinite(v) ? mpt(sqrt(d0^2 + (hi > lo ? (v - lo) / (hi - lo) : 0.5) * (d1^2 - d0^2))) * 2.54 / 72 : 0.0 for v in sz]
			push!(calls, ("bubbles", curve(), Pair{Symbol,Any}[:marker => :circle, :ms => dia, :mc => col * "@40", :ml => string("0.5p,", col)]))
		elseif kind == 11            # arrows: GMT vectors from start to end point, the window's head shape
			hl = 3 * max(2.5, width) + 4          # head length px, half width 0.35 of it (vtkXYShapePlot)
			push!(calls, ("arrows", through(_xy_arrow_data),
			              Pair{Symbol,Any}[:symbol => string("v", spt(hl), "p+e+a", round(2atand(0.35); digits = 1), "+h0+s"),
			                               :pen => string(spt(width), "p,", col), :fill => col]))
		elseif kind == 9             # error bars: the curve, then GMT.jl's error_bars (deltas) in its colour
			eb = [(ax, _xy_nums(get(d, l, "")), _xy_nums(get(d, u, ""))) for (ax, l, u) in
			      ((:x, "errxl", "errxh"), (:y, "erryl", "erryh"))]
			filter!(e -> !isempty(e[2]), eb)
			# psxy -E draws no line, only the bars (and the symbols): the curve is a call of its own
			lt != 0 && push!(calls, ("curve", segs(), Pair{Symbol,Any}[:pen => pen]))
			if !isempty(eb)          # one error_bars: symmetric vectors, or [lower upper] with asym=true
				asym = any(e -> e[2] != e[3], eb)
				# the caps as wide as the chart's (vtkXYShapePlot: CapPx 4 logical px each side)
				ks = Tuple(vcat([e[1] for e in eb], asym ? [:asym] : Symbol[], [:pen, :cap]))
				vs = Tuple(vcat([asym ? [e[2] e[3]] : e[2] for e in eb], asym ? [true] : Any[],
				                [string(spt(width), "p,", col), string(mpt(8), "p")]))
				kw = Pair{Symbol,Any}[:error_bars => NamedTuple{ks}(vs)]
				haskey(_XY_GMT_MARKER, mk) && append!(kw, marks(mk))
				push!(calls, ("error bars", curve(), kw))
			end
		else                         # a curve: its pen and its markers
			kw = Pair{Symbol,Any}[]
			lt != 0 && push!(kw, :pen => pen)
			haskey(_XY_GMT_MARKER, mk) && append!(kw, marks(mk))
			push!(calls, ("curve", segs(), kw))
		end
		right = get(d, "right", "0") == "1"
		for (k, (what, bind, kw)) in enumerate(calls)
			if isempty(steps)        # the first call sets up the figure: region, frame, grid, labels, time
				pre = Pair{Symbol,Any}[:region => ScriptVar(:REG, ctx.region), :proj => ScriptVar(:PROJ, ctx.proj), :frame => frame]
				# a log axis as the chart's: 1-2-5 annotated, ticks (and grid) at every 1..9 of each decade; a
				# linear one annotated, ticked and gridded at the chart's own tick step (a time axis: GMT's)
				function ax(lg, g, which, time)
					lg && return merge((annot = 2, ticks = 3), g ? (grid = 3,) : (;))
					st = time ? nothing : _xy_axis_step(h, which)
					st === nothing && return g ? (grid = :auto,) : (;)
					return merge((annot = st, ticks = st), g ? (grid = st,) : (;))
				end
				xa = ax(logx, gridx, 0, timefmt != 0); ya = ax(logy, gridy, 1, false)
				isempty(xa) || hidden_axes || push!(pre, :xaxis => xa)   # -Bpxg: the vertical lines
				isempty(ya) || hidden_axes || push!(pre, :yaxis => ya)   # -Bpyg: the horizontal ones
				xl = _xy_unesc(get(pg, "xlabel", "")); yl = _xy_unesc(get(pg, "ylabel", ""))
				hidden_axes || isempty(xl) || push!(pre, :xlabel => xl)
				hidden_axes || isempty(yl) || push!(pre, :ylabel => yl)
				par = (;)
				if gridx || gridy    # the grid's pen as the chart's: 1 px of 190 grey, or what Axes properties set
					gw = get(axp, "MAP_GRID_PEN_PRIMARY.width", ""); gc = get(axp, "MAP_GRID_PEN_PRIMARY.color", "")
					par = merge(par, (MAP_GRID_PEN_PRIMARY = string(isempty(gw) ? string(spt(1), "p") : gw, ",",
					                                                isempty(gc) ? "190/190/190" : gc),))
				end
				par = merge(par, axfonts)
				if timefmt != 0      # X is Unix epoch seconds: GMT reads it as time from that epoch
					push!(pre, :f => "0T")
					par = merge(par, (TIME_EPOCH = "1970-01-01T00:00:00", TIME_UNIT = "s"))
				end
				isempty(par) || push!(pre, :par => par)
				kw = vcat(pre, kw)
			end
			if right && k == 1       # plotyy: onto the right-hand axis' own range, annotated on the E side
				kw = vcat(Pair{Symbol,Any}[:region => (ctx.region[1], ctx.region[2], yr2...), :proj => ScriptVar(:PROJ, ctx.proj),
				                           :frame => (axes = :E, annot = something(_xy_axis_step(h, 2), :auto),
				                                      ticks = something(_xy_axis_step(h, 2), :auto)),
				                           :par => axfonts], kw)
			end
			push!(steps, ScriptStep("$(name) — $what", DataBind[bind], :plot, bind.var, kw, !isempty(steps)))
		end
		if legendon && !isempty(name) && get(d, "inlegend", "1") == "1"
			append!(legs, vcat(_xy_legend_line(d, kind, width, col, name, spt, mpt, legf / 12)))
		end
	end
	isempty(steps) && error("gmtscript: this X,Y page has no visible curve to plot")
	# The legend is ALWAYS drawn by GMT's legend module, from the entries written out — every entry, one
	# box, each drawn as the window draws it (an arrow as an arrow) — never by the `legend=` of each call.
	if !isempty(legs)
		lv = _script_var!(ctx, "LEG", nothing)
		lv = ScriptVar(lv.name, GMT.text_record(legs))
		lb = DataBind(lv, :command, "text_record([\n\t" * join(repr.(legs), ",\n\t") * "])")
		# its font the chart legend's, at the share of the plot it takes on screen
		# Where the window has it — its default corner or wherever it was dragged, from the plot's
		# bottom-left at the figure's scale. Pinned by the box corner nearest the plot's own corner, so GMT's
		# box, a little larger than the chart's, grows inwards. No box on screen (never painted): the
		# default corner, beside the plot for a page drawing its own frame (radar, corner, parallel…).
		lbox = zeros(4)
		pos = if ccall(_fn(:gmtvtk_xyplot_legend_box), Cint, (Ptr{Cvoid}, Ptr{Cdouble}), h, lbox) == 1 &&
		         plotw[] > 0 && ploth[] > 0
			right = lbox[1] + lbox[3] / 2 > plotw[] / 2;  top = lbox[2] + lbox[4] / 2 > ploth[] / 2
			lcorner = Symbol(top ? "T" : "B", right ? "R" : "L")
			(paper = (round((right ? lbox[1] + lbox[3] : lbox[1]) * w / plotw[]; digits = 2),
			          round((top ? lbox[2] + lbox[4] : lbox[2]) * hgt / ploth[]; digits = 2)),
			 justify = Symbol(top ? "T" : "B", right ? "R" : "L"))
		else
			hidden_axes ? (outside = :TR, justify = :TL, offset = 0.2) : (inside = true, anchor = :TR, offset = 0.2)
		end
		kw = Pair{Symbol,Any}[:pos => pos,
		                      :box => (pen = 0.5, fill = :white),
		                      :par => (FONT_ANNOT_PRIMARY = string(legf, "p"),)]
		# after plotyy's right-hand curves: back to the page's own axes, which everything below is placed on
		isempty(rights) || (kw = vcat(Pair{Symbol,Any}[:region => ScriptVar(:REG, ctx.region), :proj => ScriptVar(:PROJ, ctx.proj)], kw))
		push!(steps, ScriptStep("legend", DataBind[lb], :legend, lv, kw, true))
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
		kw = Pair{Symbol,Any}[:font => (tpt(l.size), l.bold ? "Helvetica-Bold" : "Helvetica", _xy_hex(l.r, l.g, l.b)),
		                      :justify => just, :noclip => true]
		push!(steps, ScriptStep("label \"$(l.text)\"", DataBind[bind], :text, var, kw, true))
	end
	_xy_annot_steps!(steps, ctx, h, cur, logx, logy, mpt, tpt)   # the hand-drawn annotations, over everything
	# Drawn by the X,Y window itself, not curves: say so rather than drop them silently.
	get(pg, "mag.barOn", "0") == "1" && push!(ctx.notes, "the geomagnetic bar code strip, its chron names and picks")
	(get(pg, "mag.starIdx", "-1") != "-1" || get(pg, "mag.ageLineX", "nan") != "nan" || !isempty(strip(get(pg, "mag.pinned", "")))) &&
		push!(ctx.notes, "the isochron fit marks (red star, first guess, fitted and pinned lines)")
	!isempty(strip(get(pg, "sglines", ""))) && push!(ctx.notes, "the Spector-Grant fit lines")
	(get(win, "infovisible", "0") == "1") && push!(ctx.notes, "the header strip above the chart")
	haskey(pg, "gmtjl.spec") && return _xy_gmtjl_steps(h, pg, cur, steps, ctx, lcorner), ctx
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
