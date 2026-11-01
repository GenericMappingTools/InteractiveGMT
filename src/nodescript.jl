# nodescript.jl — the "Plot with GMT.jl" script as a graph of boxes (the editor's Graph… button).
#
# The graph is a VIEW OF THE EDITOR'S TEXT (docs/NODESCRIPT_PLAN.md), never a second model of the
# window: open = parse the box's current text (`_script_editor_text`), one top-level statement per box;
# an edit in a box rewrites that ONE statement in the text and pushes the text back into the box. Run
# and Save are the editor's own (the graph window clicks its buttons), so there is one runner and one
# save path — SACRED_LAW.md, same operation = same function. A 3-D window's script and an X,Y page's
# go through the same editor, so both get a graph, and so does a script typed in by hand.
#
# Box kinds: :figure (the REG / PROJ / FIGSIZE knobs, one box), :data (any other `X = …`), :palette
# (`C1 = makecpt(…)`), :call (a GMT.jl call), :output (showfig / gmtend), :julia (anything else, shown
# as text). A `penN = "…"` line has no box: it is a field of the call that uses it, and editing that
# field rewrites the pen's own line — where the script says pens are edited.

# One top-level statement: where it sits in the text, and the comments just above it.
struct NGStmt
	ex::Any
	start::Int                  # first byte of the statement's code
	stop::Int                   # last byte of its code (a trailing `# comment` on that line excluded)
	comments::Vector{String}    # comment lines between the previous statement and this one
end

# One editable row of a box. `stmt` is the statement an edit rewrites (0 = not editable); `key` says
# which part of it: a kwarg name, "#i" for the i-th positional argument, "=" for an assignment's RHS.
#
# A field is shown and edited IN PLAIN LANGUAGE: `plain` names it ("Colour palette", "Line"), `shown` says
# its value in words ("turbo", "1 pt, red, solid"), and `type` picks the dialog the graph opens for it, so
# nobody has to know GMT.jl syntax to change it. `extra` carries what that dialog needs, as name/value
# pairs (US-separated name and value, RS between pairs): the choices of a list, the parts of a pen, a
# frame or a tuple of numbers. Types: "ref" (a value made by another box; not edited here), "bool",
# "number", "text", "file", "color", "pen", "nums" (several numbers), "choice", "frame", "code" (anything
# else: edited as Julia code), "none" (shown only).
struct NGField
	stmt::Int
	key::String
	label::String
	value::String
	type::String
	plain::String
	shown::String
	extra::String
end
NGField(stmt, key, label, value) = NGField(stmt, key, label, value, stmt > 0 ? "code" : "none", label, value, "")

mutable struct NGNode
	id::Int
	kind::Symbol
	title::String
	note::String
	chain::Bool                 # a program step (drawn in the main column, linked in order)
	fields::Vector{NGField}
	col::Int                    # 0 = the main column; k = k columns to its left
	anchor::Int                 # the node whose height this one is placed at (0 = none)
	stmt::Int                   # the statement that made this box (the figure box: its first)
end

const _NG_FIGURE_VARS = (:REG, :PROJ, :FIGSIZE)

# Skip blanks and comments from byte `i`, collecting the comment lines. Returns the first byte of code.
function _ng_skip_trivia(src::String, i::Int, comments::Vector{String})::Int
	n = ncodeunits(src)
	while i <= n
		c = src[i]
		if isspace(c)
			i = nextind(src, i)
		elseif c == '#'
			if startswith(SubString(src, i), "#=")             # block comment: to its closing =#
				j = findnext("=#", src, i + 2)
				i = j === nothing ? n + 1 : last(j) + 1
			else
				j = findnext('\n', src, i)
				e = j === nothing ? lastindex(src) : prevind(src, j)
				push!(comments, rstrip(src[i:e]))
				i = j === nothing ? n + 1 : j
			end
		else
			break
		end
	end
	return i
end

# The byte where a `# comment` starts on `line`, outside any string or char literal (0 = none).
function _ng_comment_at(line::AbstractString)::Int
	instr = false
	esc = false
	for (k, c) in pairs(line)
		if instr
			esc ? (esc = false) : c == '\\' ? (esc = true) : c == '"' && (instr = false)
		elseif c == '"'
			instr = true
		elseif c == '#'
			return k
		end
	end
	return 0
end

# The text cut into its top-level statements, each with its byte range and the comments above it.
function _ng_statements(src::String)::Vector{NGStmt}
	out = NGStmt[]
	i = 1
	n = ncodeunits(src)
	while true
		cm = String[]
		i = _ng_skip_trivia(src, i, cm)
		i > n && break
		ex, nxt = Meta.parse(src, i; greedy = true)
		ex === nothing && break
		(ex isa Expr && ex.head in (:error, :incomplete)) && error("the script does not parse: $(ex.args[1])")
		# `parse` also swallows the blank and comment lines AFTER the statement — those belong to the next
		# one. Back off over them: a line that is blank or only a comment can never end a statement.
		stop = prevind(src, nxt)
		while true
			while stop >= i && isspace(src[stop])
				stop = prevind(src, stop)
			end
			ls = max(something(findprev('\n', src, stop), i - 1) + 1, i)
			(ls > i && startswith(lstrip(SubString(src, ls, stop)), '#')) || break
			stop = prevind(src, ls)
		end
		# A trailing `# …` on the statement's last line is the user's note, not code: kept by an edit.
		ls = max(something(findprev('\n', src, stop), i - 1) + 1, i)
		k = _ng_comment_at(SubString(src, ls, stop))
		if k > 0
			stop = prevind(src, ls + k - 1)
			while stop >= i && isspace(src[stop])
				stop = prevind(src, stop)
			end
		end
		push!(out, NGStmt(ex, i, stop, cm))
		# The next statement's comments start on the NEXT line (this line's trailing note is this one's).
		j = findnext('\n', src, stop)
		i = j === nothing ? n + 1 : j
	end
	return out
end

# Source text for a parsed value, in the emitter's spelling (`_lit`): `k=v`, no spaces round `=`.
function _ng_src(x)::String
	x isa String && return repr(x)
	x isa Symbol && return String(x)
	x isa QuoteNode && return x.value isa Symbol ? ":" * String(x.value) : string(x)
	x isa Expr || return repr(x)
	if x.head === :tuple
		parts = String[_ng_src(a) for a in x.args]
		return "(" * join(parts, ", ") * (length(parts) == 1 ? "," : "") * ")"
	end
	x.head in (:kw, :(=)) && x.args[1] isa Symbol && return String(x.args[1]) * "=" * _ng_src(x.args[2])
	x.head === :vect && return "[" * join((_ng_src(a) for a in x.args), ", ") * "]"
	x.head === :call && return _ng_render_call(x)
	return string(x)
end

# `f(a, b, k=v)` — the call split into callee, positional arguments and keyword arguments.
function _ng_call_parts(ex::Expr)
	pos = Any[]
	kws = Pair{Symbol,Any}[]
	for a in ex.args[2:end]
		if a isa Expr && a.head === :parameters
			for p in a.args
				p isa Expr && p.head === :kw && push!(kws, p.args[1] => p.args[2])
			end
		elseif a isa Expr && a.head === :kw
			push!(kws, a.args[1] => a.args[2])
		else
			push!(pos, a)
		end
	end
	return ex.args[1], pos, kws
end

function _ng_render_call(ex::Expr)::String
	f, pos, kws = _ng_call_parts(ex)
	args = String[_ng_src(a) for a in pos]
	for (k, v) in kws
		push!(args, String(k) * "=" * _ng_src(v))
	end
	return string(f) * "(" * join(args, ", ") * ")"
end

# A statement written back out (only ever the one being edited).
function _ng_render(ex)::String
	ex isa Expr || return _ng_src(ex)
	ex.head === :const && return "const " * _ng_render(ex.args[1])
	ex.head === :(=) && return _ng_src(ex.args[1]) * " = " * _ng_src(ex.args[2])
	ex.head === :call && return _ng_render_call(ex)
	return string(ex)
end

_ng_iscall(x) = x isa Expr && x.head === :call
_ng_unconst(ex) = (ex isa Expr && ex.head === :const) ? ex.args[1] : ex
_ng_isassign(ex) = ex isa Expr && ex.head === :(=) && ex.args[1] isa Symbol
_ng_callee(x) = (s = string(x); something(findlast('.', s), 0) > 0 ? s[findlast('.', s)+1:end] : s)
_ng_clean(s::AbstractString) = replace(String(s), r"[\t\r\n]" => " ")

# Every symbol an expression reads.
function _ng_symbols!(acc::Vector{Symbol}, x)
	if x isa Symbol
		push!(acc, x)
	elseif x isa Expr
		x.head === :. && return _ng_symbols!(acc, x.args[1])   # `GMT.plot`: only the module is a name
		for a in (x.head in (:kw, :(=)) && x.args[1] isa Symbol ? x.args[2:end] : x.args)
			_ng_symbols!(acc, a)
		end
	end
	return acc
end

# ── plain language ───────────────────────────────────────────────────────────────────────────────
# What a kwarg IS, in words. Anything not listed keeps its own name.
const _NG_PLAIN = Dict{String,String}(
	"region" => "Map limits", "REG" => "Map limits", "proj" => "Projection", "PROJ" => "Projection",
	"figsize" => "Map width (cm)", "FIGSIZE" => "Map width (cm)", "frame" => "Axes and frame",
	"cmap" => "Colour palette", "C" => "Colour palette", "range" => "Value range", "continuous" => "Smooth colours",
	"pen" => "Line", "shore" => "Coastline", "ml" => "Symbol outline", "fill" => "Fill colour",
	"color" => "Colour", "land" => "Land colour", "water" => "Water colour", "shade" => "Illumination",
	"transparency" => "Transparency (%)", "alpha" => "Transparency (%)", "title" => "Title",
	"pos" => "Position", "position" => "Position", "marker" => "Symbol", "symbol" => "Symbol",
	"size" => "Symbol size", "ms" => "Symbol size", "markersize" => "Symbol size", "font" => "Font",
	"zsize" => "Relief height (cm)", "view" => "Viewpoint (azimuth, elevation)", "resolution" => "Detail level",
	"borders" => "Borders", "rivers" => "Rivers", "angle" => "Angle", "justify" => "Text anchor",
	"data" => "Data", "level" => "Level", "levels" => "Levels", "annot" => "Labelled contours",
	"cont" => "Contour interval", "nan_alpha" => "Holes transparent", "xlabel" => "X label",
	"ylabel" => "Y label", "label" => "Label", "lw" => "Line width (pt)", "lc" => "Line colour",
	"ls" => "Line style", "lt" => "Line width (pt)", "dpi" => "Resolution (dpi)", "zscale" => "Z scale")

# What a call DOES, in words (the box title says it first, the GMT.jl name after it).
const _NG_CALLS = Dict{String,String}(
	"grdimage" => "Grid as image", "grdview" => "Grid as 3-D surface", "colorbar" => "Colour bar",
	"plot" => "Lines and symbols", "plot3d" => "3-D lines and symbols", "plot3" => "3-D lines and symbols",
	"scatter" => "Symbols", "coast" => "Coastlines", "text" => "Text labels", "grdcontour" => "Contours",
	"meca" => "Focal mechanisms", "basemap" => "Map frame", "makecpt" => "Colour palette",
	"gmtread" => "Read file", "showfig" => "Show the figure", "gmtend" => "Finish the figure",
	"legend" => "Legend", "arrows" => "Arrows", "grdvector" => "Vector field", "histogram" => "Histogram",
	"bar" => "Bars", "annotation" => "Annotation", "image" => "Picture", "mat2grid" => "Grid from numbers",
	"mat2img" => "Picture from pixels", "subplot" => "Panels", "inset" => "Inset map", "lines" => "Lines")
_ng_callname(fn::String) = (b = rstrip(fn, '!'); get(_NG_CALLS, b, ""))

const _NG_CPTS = ("turbo", "geo", "relief", "topo", "earth", "globe", "etopo1", "ocean", "haxby", "jet",
	"rainbow", "polar", "seis", "viridis", "plasma", "inferno", "magma", "batlow", "roma", "vik", "gray",
	"hot", "cool", "copper", "wysiwyg", "elevation", "dem1", "dem2", "drywet", "nighttime", "red2green")

const _NG_US = '\x1f'                                  # name/value separator inside `extra`
const _NG_RS = '\x1e'                                  # between pairs
_ng_pairs(p) = join((string(a, _NG_US, b) for (a, b) in p), _NG_RS)

_ng_num(x) = (x isa Real && !(x isa Bool)) ? Float64(x) :
             (x isa Expr && x.head === :call && x.args[1] === :- && length(x.args) == 2 && x.args[2] isa Real) ?
             -Float64(x.args[2]) : nothing
_ng_fmt(v::Float64) = (isinteger(v) && abs(v) < 1e15) ? string(Int(v)) : string(round(v; sigdigits = 8))
_ng_nums(x) = (x isa Expr && x.head === :tuple && !isempty(x.args) && all(a -> _ng_num(a) !== nothing, x.args)) ?
              Float64[_ng_num(a) for a in x.args] : nothing
_ng_named(x) = (x isa Expr && x.head === :tuple && all(a -> a isa Expr && a.head in (:(=), :kw), x.args)) ?
               Dict{String,Any}(String(a.args[1]) => a.args[2] for a in x.args) : nothing
_ng_sym(x) = x isa QuoteNode && x.value isa Symbol ? String(x.value) : nothing

# A pen's parts in words: "1p,red,dashed" -> ("1", "red", "dashed").
function _ng_pen_parts(s::String)
	p = split(s, ',')
	w = replace(strip(p[1]), r"p$" => "")
	c = length(p) >= 2 ? strip(p[2]) : "black"
	st = length(p) >= 3 ? strip(join(p[3:end], ',')) : ""
	style = st in ("-", "dashed", "dash") ? "dashed" : st in (".", "dotted", "dot") ? "dotted" :
	        isempty(st) || st == "solid" ? "solid" : st
	return w, c, style
end

# The projections offered by name, each spelled for THIS region (its centre, its parallels).
function _ng_proj_choices(region)
	w, e, s, n = region === nothing ? (-180.0, 180.0, -90.0, 90.0) : region
	lon0 = _ng_fmt(round((w + e) / 2; digits = 3)); lat0 = _ng_fmt(round((s + n) / 2; digits = 3))
	p1 = _ng_fmt(round(s + (n - s) / 6; digits = 3)); p2 = _ng_fmt(round(n - (n - s) / 6; digits = 3))
	geog = -360 <= w <= 360 && -360 <= e <= 360 && -90 <= s <= 90 && -90 <= n <= 90
	auto = geog ? String(strip(GMT.guess_proj([w, e], [s, n]))[3:end]) : "X"
	return [("Automatic — GMT.jl picks for this region (" * _script_proj_name(auto) * ")", repr(auto)),
	        ("Mercator", repr("M")), ("Miller cylindrical", repr("J$lon0")), ("Robinson", repr("N$lon0")),
	        ("Mollweide", repr("W$lon0")), ("Equidistant conic", repr("D$lon0/$lat0/$p1/$p2")),
	        ("Lambert conic conformal", repr("L$lon0/$lat0/$p1/$p2")),
	        ("Lambert azimuthal equal-area", repr("A$lon0/$lat0")), ("Orthographic (globe)", repr("G$lon0/$lat0")),
	        ("Stereographic", repr("S$lon0/$lat0")), ("Cylindrical equidistant", repr("Q$lon0")),
	        ("Sinusoidal", repr("I$lon0")), ("Linear (no projection)", ":X"),
	        ("Select CRS...", _NG_CRS_PICK)]
end
# The value of the "Select CRS..." row: not code, a request to the graph to open the CRS selector (the one
# Tools > Project uses), whose pick is then written as the proj value.
const _NG_CRS_PICK = "@crs"

const _NG_BARPOS = (("Right of the map", "(anchor=:MR, offset=(1, 0))"),
                    ("Left of the map", "(anchor=:ML, offset=(1, 0))"),
                    ("Below the map", "(anchor=:BC, offset=(0, 1), horizontal=true)"),
                    ("Above the map", "(anchor=:TC, offset=(0, 1), horizontal=true)"))

"""
	_ng_field(stmt, key, label, ex, fn, region) -> NGField

One argument of a statement as a PLAIN-LANGUAGE field: what it is called, what its value says, and which
dialog edits it. `fn` is the call it belongs to (`""` for an assignment), `region` the script's map limits
(the projections are offered centred on them).
"""
function _ng_field(stmt::Int, key::String, label::String, ex, fn::String, region)::NGField
	code = _ng_clean(_ng_src(ex))
	plain = get(_NG_PLAIN, label, label)
	F(type, shown, extra = "") = NGField(stmt, key, label, code, type, plain, _ng_clean(shown), _ng_clean(extra))
	base = rstrip(fn, '!')
	# a switched-off flag: GMT.jl reads `nothing` as "option absent" (a `false` it rejects outright)
	(ex === :nothing && key != "=" && !startswith(key, "#")) && return F("bool", "No", "0")
	ex isa Symbol && return NGField(0, key, label, code, "ref", plain,
	                                ex in _NG_FIGURE_VARS ? "as set in the Figure box" : "comes from box " * code, "")
	ex isa Bool && return F("bool", ex ? "Yes" : "No", ex ? "1" : "0")
	if label in ("pen", "shore", "ml") && ex isa String
		w, c, st = _ng_pen_parts(ex)
		return F("pen", "$w pt, $c, $st", _ng_pairs((("width", w), ("colour", c), ("style", st))))
	end
	label in ("fill", "color", "land", "water", "lc", "G") && ex isa String && return F("color", ex, ex)
	if label in ("cmap", "C") && (s = _ng_sym(ex)) !== nothing
		names = s in _NG_CPTS ? collect(_NG_CPTS) : vcat(s, collect(_NG_CPTS))
		return F("choice", s, _ng_pairs((n, ":" * n) for n in names))
	end
	if label in ("proj", "PROJ") && (ex isa String || ex isa QuoteNode)
		v = ex isa String ? ex : something(_ng_sym(ex), code)
		ch = _ng_proj_choices(region)
		# a system picked in the CRS selector is not among the names: it stays in the list, as the current one
		any(c -> c[2] == code, ch) || insert!(ch, 2, (_script_proj_name(v), code))
		return F("choice", _script_proj_name(v), _ng_pairs(ch))
	end
	if label in ("pos", "position") && base == "colorbar"
		d = _ng_named(ex)
		a = d === nothing ? nothing : _ng_sym(get(d, "anchor", nothing))
		shown = a == "MR" ? "Right of the map" : a == "ML" ? "Left of the map" : a == "BC" ? "Below the map" :
		        a == "TC" ? "Above the map" : code
		return F("choice", shown, _ng_pairs(_NG_BARPOS))
	end
	if label == "frame"
		d = _ng_named(ex)
		if d !== nothing
			on(k) = haskey(d, k) && d[k] !== false && _ng_sym(d[k]) != "none" ? "1" : "0"
			axes = something(_ng_sym(get(d, "axes", nothing)), "WSen")
			title = get(d, "title", "") isa String ? get(d, "title", "") : ""
			parts = (("annot", on("annot")), ("ticks", on("ticks")), ("grid", on("grid")), ("axes", axes),
			         ("title", title))
			shown = join(filter(!isempty, [on("annot") == "1" ? "numbers" : "", on("ticks") == "1" ? "ticks" : "",
			                               on("grid") == "1" ? "grid" : "", isempty(title) ? "" : "title \"$title\""]), ", ")
			return F("frame", isempty(shown) ? "plain box" : shown, _ng_pairs(parts))
		end
		_ng_sym(ex) == "none" && return F("frame", "no frame", _ng_pairs((("axes", "none"),)))
	end
	if (v = _ng_nums(ex)) !== nothing
		labs = label in ("region", "REG") && length(v) == 4 ? ("West", "East", "South", "North") :
		       label == "range" ? ("From", "To", "Step") : label == "view" ? ("Azimuth", "Elevation") :
		       label == "offset" ? ("Right", "Up") : ntuple(i -> "Value $i", length(v))
		shown = join((string(labs[i], " ", _ng_fmt(v[i])) for i in eachindex(v)), ", ")
		return F("nums", shown, _ng_pairs((labs[i], _ng_fmt(v[i])) for i in eachindex(v)))
	end
	(n = _ng_num(ex)) !== nothing && return F("number", _ng_fmt(n), _ng_fmt(n))
	if ex isa String
		key == "#1" && base == "gmtread" && return NGField(stmt, key, label, code, "file", "File", basename(ex), ex)
		return F("text", ex, ex)
	end
	return F("code", code)
end

"""
	_nodegraph_parse(src) -> (nodes, wires, links)

The script `src` as boxes: `wires` are `(from, to, label)` (a variable defined in `from` used by the
field `label` of `to`), `links` are `(from, to)` (consecutive program steps). Pure: no DLL, no window.
"""
function _nodegraph_parse(src::String)
	stmts = _ng_statements(src)
	nodes = NGNode[]
	wires = Tuple{Int,Int,String}[]
	defs = Dict{Symbol,Int}()                       # variable -> the box that assigns it
	pens = Dict{Symbol,Tuple{Int,String}}()         # penN -> (its statement, its literal)
	figure = 0
	heading = ""
	k = 0
	region = nothing                                # the map limits, once known (projections centre on them)
	newnode(kind, title, note, chain) = (push!(nodes, NGNode(length(nodes) + 1, kind, title, note, chain,
	                                                          NGField[], 0, 0, k)); nodes[end])
	# A value's fields: the positional arguments and the kwargs of `call`, each in plain language.
	function addfields!(nd::NGNode, k::Int, call, poslabel)
		f, pos, kws = _ng_call_parts(call)
		fn = _ng_callee(f)
		for (kk, v) in kws
			if region === nothing && kk === :region && (r = _ng_nums(v)) !== nothing && length(r) == 4
				region = Tuple(r)
			end
		end
		# InteractiveGMT's own fetchers (`_display_image`, `_xy_series_data`, …) take window handles and
		# indices nobody should touch: one line saying where the data comes from, nothing to edit.
		if startswith(fn, "_")
			push!(nd.fields, NGField(0, "", "source", "", "none", "Source", "taken from the live iGMT window", ""))
			return
		end
		for (i, a) in enumerate(pos)
			push!(nd.fields, _ng_field(k, "#$i", poslabel(i), a, fn, region))
			use!(nd, a, poslabel(i))
		end
		for (kk, v) in kws
			if v isa Symbol && haskey(pens, v)          # the pen's own line is what is edited
				ps, pv = pens[v]
				push!(nd.fields, _ng_field(ps, "=", String(kk), Meta.parse(pv), fn, region))
			else
				push!(nd.fields, _ng_field(k, String(kk), String(kk), v, fn, region))
				use!(nd, v, String(kk))
			end
		end
	end
	function use!(nd::NGNode, v, label::String)
		for s in unique(_ng_symbols!(Symbol[], v))
			(s in _NG_FIGURE_VARS || !haskey(defs, s) || defs[s] == nd.id) && continue
			(defs[s], nd.id, label) in wires || push!(wires, (defs[s], nd.id, label))
		end
	end
	for (kk, st) in enumerate(stmts)
		k = kk
		notes = String[]
		for c in st.comments
			t = strip(lstrip(c, '#'))
			if startswith(t, "──")
				heading = strip(lstrip(t, '─'))
			elseif !isempty(t)
				push!(notes, t)
			end
		end
		note = join(notes, " ")
		ex = _ng_unconst(st.ex)
		if ex isa Expr && ex.head in (:using, :import)
			continue                                    # set-up, not a step
		elseif _ng_isassign(ex) && ex.args[1] in _NG_FIGURE_VARS
			if figure == 0
				figure = newnode(:figure, "Figure", "", true).id
			end
			if ex.args[1] === :REG && (r = _ng_nums(ex.args[2])) !== nothing && length(r) == 4
				region = Tuple(r)
			end
			push!(nodes[figure].fields, _ng_field(k, "=", String(ex.args[1]), ex.args[2], "", region))
		elseif _ng_isassign(ex) && ex.args[2] isa String && occursin(r"^pen\d+$", String(ex.args[1]))
			pens[ex.args[1]] = (k, _ng_src(ex.args[2]))
		elseif _ng_isassign(ex)
			lhs, rhs = ex.args
			cal = _ng_iscall(rhs) ? _ng_callee(rhs.args[1]) : ""
			pal = cal == "makecpt"
			what = startswith(cal, "_") ? "from the window" : isempty(_ng_callname(cal)) ? cal : _ng_callname(cal)
			title = String(lhs) * (isempty(what) ? "" : " · " * what)
			nd = newnode(pal ? :palette : :data, title, note, false)
			_ng_iscall(rhs) ? addfields!(nd, k, rhs, i -> "arg$i") :
			                  (push!(nd.fields, _ng_field(k, "=", "value", rhs, "", region)); use!(nd, rhs, "value"))
			defs[lhs] = nd.id
		elseif _ng_iscall(ex)
			fn = _ng_callee(ex.args[1])
			out = fn in ("showfig", "gmtend")
			pl = _ng_callname(fn)
			ttl = isempty(pl) ? string(ex.args[1]) : pl * "  (" * string(ex.args[1]) * ")"
			nd = newnode(out ? :output : :call, ttl, isempty(heading) ? note : heading, true)
			heading = ""
			addfields!(nd, k, ex, i -> i == 1 ? "data" : "arg$i")
		else
			# A function definition, a loop, anything else: shown as its first lines, not edited here.
			txt = src[st.start:st.stop]
			lines = split(txt, '\n')
			ttl = ex isa Expr && ex.head in (:function, :macro, :struct, :for, :while, :if, :let, :begin) ?
			      String(ex.head) : "Julia"
			nd = newnode(:julia, ttl, isempty(heading) ? note : heading, true)
			heading = ""
			for l in lines[1:min(end, 4)]
				push!(nd.fields, NGField(0, "", "", _ng_clean(l)))
			end
			length(lines) > 4 && push!(nd.fields, NGField(0, "", "", "… $(length(lines) - 4) more lines"))
			for s in unique(_ng_symbols!(Symbol[], ex))
				haskey(defs, s) && !(s in _NG_FIGURE_VARS) && push!(wires, (defs[s], nd.id, ""))
			end
		end
	end
	# Program steps linked in order; each value placed one column left of the box that uses it (the
	# nearest one), at that box's height. A value nobody uses sits beside the next step.
	chain = [nd.id for nd in nodes if nd.chain]
	links = Tuple{Int,Int}[(chain[i], chain[i+1]) for i in 1:length(chain)-1]
	for nd in Iterators.reverse(nodes)
		nd.chain && continue
		users = [w[2] for w in wires if w[1] == nd.id]
		if isempty(users)
			nxt = findfirst(c -> c > nd.id, chain)
			nd.col = 1
			nd.anchor = nxt === nothing ? (isempty(chain) ? 0 : chain[end]) : chain[nxt]
		else
			best = users[argmin([nodes[u].col for u in users])]
			nd.col = nodes[best].col + 1
			nd.anchor = best
		end
	end
	return nodes, wires, links
end

# The graph as the line blob the C++ side reads (`gmtvtk_nodegraph_show_h`), one record per line,
# tab-separated: N id kind col anchor title note / F id stmt key label value / W from to label /
# L from to. Tabs and newlines inside a value are flattened to spaces.
function _nodegraph_blob(nodes::Vector{NGNode}, wires::Vector{Tuple{Int,Int,String}},
                         links::Vector{Tuple{Int,Int}})::String
	io = IOBuffer()
	for nd in nodes
		println(io, join(("N", nd.id, nd.kind, nd.col, nd.anchor, _ng_clean(nd.title), _ng_clean(nd.note)), '\t'))
		for f in nd.fields
			println(io, join(("F", nd.id, f.stmt, f.key, f.label, f.value, f.type, f.plain, f.shown, f.extra), '\t'))
		end
	end
	for (a, b, l) in wires
		println(io, join(("W", a, b, l), '\t'))
	end
	for (a, b) in links
		println(io, join(("L", a, b), '\t'))
	end
	return String(take!(io))
end

"""
	_nodegraph_open(h) -> nothing

The editor's Graph… button (and the graph's Refresh): parse the editor box's CURRENT text and show it
as boxes in the graph window of the window `h` (a 3-D viewer or an X,Y plot).
"""
function _nodegraph_open(h::Ptr{Cvoid}; fresh::Bool = true)::Nothing
	src = _script_editor_text(h)
	isempty(strip(src)) && error("the script editor is empty")
	nodes, wires, links = _nodegraph_parse(src)
	pics, errs = _nodegraph_previews(src, nodes, h, fresh)
	for (id, msg) in errs                               # a step that failed says so IN ITS BOX
		push!(nodes[id].fields, NGField(0, "", "", "ERROR: " * _ng_clean(first(split(msg, '\n')))))
	end
	ccall(_fn(:gmtvtk_nodegraph_preview_h), Cint, (Ptr{Cvoid}, Cint, Cint, Cint, Ptr{UInt8}), h, 0, 0, 0, C_NULL)
	for (id, rgb) in pics
		ccall(_fn(:gmtvtk_nodegraph_preview_h), Cint, (Ptr{Cvoid}, Cint, Cint, Cint, Ptr{UInt8}),
		      h, id, size(rgb, 2), size(rgb, 3), rgb)
	end
	ccall(_fn(:gmtvtk_nodegraph_show_h), Cint, (Ptr{Cvoid}, Cstring), h, _nodegraph_blob(nodes, wires, links))
	return nothing
end

# What GMT's `psxy -T -O` appends to finish a classic-mode figure (GMT.jl's close_PS_file spells it out):
# added to a COPY of the open figure's bytes, so the figure is pictured mid-way and stays open for the
# next `!` call.
const _NG_PS_TRAILER = Vector{UInt8}("\n0 A\nFQ\nO0\n0 0 TM\n\n%%BeginObject PSL_Layer_2\n0 setlinecap\n" *
	"0 setlinejoin\n3.32550952342 setmiterlimit\n%%EndObject\n\ngrestore\n" *
	"PSL_movie_label_completion /PSL_movie_label_completion {} def\n" *
	"PSL_movie_prog_indicator_completion /PSL_movie_prog_indicator_completion {} def\n" *
	"%PSL_Begin_Trailer\n%%PageTrailer\nU\nshowpage\n\n%%Trailer\n\nend\n%%EOF")

# The last preview run of each window: its statements (text and parsed), the module it ran in, which
# statements left their result there, and — per drawing statement — the figure's bytes right after it
# and the picture it got. `stamp` is the figure file as the run left it.
struct NGRun
	texts::Vector{String}
	exprs::Vector{Any}
	mod::Module
	done::Set{Int}
	ps::Dict{Int,Vector{UInt8}}
	pics::Dict{Int,Array{UInt8,3}}
	cpt::Dict{Int,UInt}
	stamp::Tuple{Float64,Int}
end
const _NG_RUNS = Dict{Ptr{Cvoid},NGRun}()

# GMT.jl's current palette, which a call with no `cmap` of its own draws with (`makecpt` sets it), by
# content: GMT.jl also edits it in place.
function _ng_cpt_print()::UInt
	isdefined(GMT, :CURRENT_CPT) || return UInt(0)
	C = GMT.CURRENT_CPT[]
	return hash((C.colormap, C.alpha, C.range, C.minmax, C.bfn, C.hinge, C.cpt, C.label, C.key, C.model))
end

# Does call `ex` draw with that current palette? Not when it names its own, nor when what it shows (its
# data, or what it drapes) is an RGB picture (`mod` holds the variables it names).
function _ng_reads_cpt(ex, mod::Module)::Bool
	_, pos, kws = _ng_call_parts(_ng_unconst(ex))
	isrgb(v) = v isa Symbol && isdefined(mod, v) && (I = getfield(mod, v); I isa GMTimage) &&
	           length(I.image) >= 3 * prod(GMT.getsize(I))
	!isempty(pos) && isrgb(pos[1]) && return false
	for (k, v) in kws
		k in (:cmap, :C) && return false
		k in (:drape, :G, :drapefile) && isrgb(v) && return false
	end
	return true
end

# What a GMT call carries over to the calls after it besides the figure bytes: the frame it sets.
const _NG_STATE_KW = (:region, :R, :limits, :proj, :J, :figsize, :figscale, :view, :p, :zsize, :JZ, :Jz)
_ng_sets_state(x) = x isa Expr && ((x.head === :kw && x.args[1] in _NG_STATE_KW) || any(_ng_sets_state, x.args))

"""
	_nodegraph_previews(src, nodes, h, fresh) -> (pics, errs)

What each step DRAWS: the script is run once, statement by statement, in a fresh module of its own (as
Run does, `_script_module`), and after every call that adds to the figure the figure so far is pictured
in memory (GMT.jl's `ps2image` on a copy of the open PostScript — nothing is written). `pics[id]` is that
picture as 3×w×h RGB bytes for box `id`; the `showfig` box gets the finished figure. `showfig` itself is
never run (it would open a viewer). A statement that throws stops the run; its box gets the message in
`errs`. A script drawn in GMT's modern mode, or by a function that shows its own figure (the climate
records), has no classic figure to picture mid-way and gets no pictures.

After a FIELD EDIT (`fresh = false`) the previous run of window `h` is reused: its module, and only
what the edit reaches runs again — the changed statements and every statement reading a variable they
(re)assign. An untouched assignment is not run again, so editing a colour does not re-fetch every layer
from the window (a lit surface's capture takes seconds). Drawing is ordered, so every call after the
first one the edit reaches is redrawn; the untouched calls BEFORE it are not: the figure is put back as
it stood after the last of them, pictures included. That is only done when nothing else wrote the
figure since, and no call after that point set the frame (region, projection, size, view) the later
calls inherit. A changed `const` (REG, PROJ, FIGSIZE) cannot be redefined in that module, so it starts
a fresh run, as do the graph's open and its Refresh.
"""
function _nodegraph_previews(src::String, nodes::Vector{NGNode}, h::Ptr{Cvoid} = C_NULL, fresh::Bool = true)
	pics = Dict{Int,Array{UInt8,3}}()
	errs = Dict{Int,String}()
	isdefined(GMT, :ps2image) || return pics, errs
	stmts = _ng_statements(src)
	texts = String[src[st.start:st.stop] for st in stmts]
	prev = fresh ? nothing : get(_NG_RUNS, h, nothing)
	n = length(stmts)
	# same statement count: compared one by one; otherwise statements moved, so all from the first difference
	samelen = prev !== nothing && length(texts) == length(prev.texts)
	firstdiff = prev === nothing ? 1 :
	            something(findfirst(j -> j > length(prev.texts) || texts[j] != prev.texts[j], 1:n), n + 1)
	changed(k) = prev === nothing || (samelen ? texts[k] != prev.texts[k] : k >= firstdiff) || !(k in prev.done)
	# a const the edit reaches cannot be redefined in the old module: run from scratch
	prev !== nothing && any(k -> stmts[k].ex isa Expr && stmts[k].ex.head === :const && changed(k), 1:n) &&
		(prev = nothing)
	# what the edit reaches: a changed statement, and whatever reads a variable one of those wrote
	dirty = falses(n)
	tainted = Set{Symbol}()
	for k in 1:n
		ex = _ng_unconst(stmts[k].ex)
		reads = _ng_symbols!(Symbol[], _ng_isassign(ex) ? ex.args[2] : ex)
		dirty[k] = changed(k) || any(in(tainted), reads)
		dirty[k] || continue
		_ng_isassign(ex) ? push!(tainted, ex.args[1]) : (_ng_iscall(ex) || union!(tainted, reads))
	end
	isdraw(k) = _ng_iscall(_ng_unconst(stmts[k].ex)) && !_is_showfig(stmts[k].ex)
	ps = GMT.PSname[]
	stamp() = isfile(ps) ? (mtime(ps), filesize(ps)) : (0.0, 0)
	# the untouched calls in front: the figure is put back as it stood after the last of them
	resume = 0
	if prev !== nothing && stamp() == prev.stamp
		for k in 1:n
			isdraw(k) || continue
			(dirty[k] || !haskey(prev.ps, k)) && break
			resume = k
		end
		resume > 0 && any(k -> _ng_sets_state(prev.exprs[k]), resume+1:length(prev.exprs)) && (resume = 0)
	end
	calls = Set{String}()
	for st in stmts
		ex = _ng_unconst(st.ex)
		_ng_iscall(ex) && push!(calls, _ng_callee(ex.args[1]))
	end
	(("gmtbegin" in calls) || any(c -> c in calls, _XY_CLIMATE_FNS)) && return pics, errs
	bystmt = Dict(nd.stmt => nd for nd in nodes)
	spics = Dict{Int,Array{UInt8,3}}()                   # by statement, for the next run to reuse
	snapps = Dict{Int,Vector{UInt8}}()
	snap!(k, id) = try
		GMT.PS_is_open(ps) || return
		img = GMT.ps2image(vcat(read(ps), _NG_PS_TRAILER); dpi = 50, crop = true)
		pics[id] = spics[k] = permutedims(img, (3, 2, 1))    # (h,w,3) -> 3×w×h: RGB bytes, row by row
	catch e
		errs[id] = "preview: " * sprint(showerror, e)
	end
	mod = prev === nothing ? _script_module() : prev.mod
	done = Set{Int}()
	scpt = Dict{Int,UInt}()
	replayed = 0                                        # the last call whose figure is taken from `prev`
	pictured = 0                                        # the call whose picture shows the figure as it stands
	last = stamp()
	# the figure as the replayed calls left it, before anything draws on it or pictures it
	function unreplay!()
		replayed > 0 || return
		write(ps, prev.ps[replayed])
		last = stamp()
		replayed = 0
	end
	for (k, st) in enumerate(stmts)
		nd = get(bystmt, k, nothing)
		if _is_showfig(st.ex)
			unreplay!()
			if nd !== nothing                             # nothing drawn since the last picture: it is the figure
				haskey(spics, pictured) ? (pics[nd.id] = spics[k] = spics[pictured]) : snap!(k, nd.id)
			end
			continue
		end
		isdraw(k) && (scpt[k] = _ng_cpt_print())
		# a call the edit does not reach, in front of every one it does, drawn with the same palette as then
		if k <= resume && isdraw(k) && (get(prev.cpt, k, UInt(0)) == scpt[k] || !_ng_reads_cpt(st.ex, mod))
			replayed = k
			snapps[k] = prev.ps[k]
			if haskey(prev.pics, k)
				spics[k] = prev.pics[k]
				nd === nothing || (pics[nd.id] = prev.pics[k])
			end
			pictured = k
			push!(done, k)
			continue
		end
		k <= resume && isdraw(k) && (resume = 0)         # it does differ: everything from here is drawn
		# kept from the previous run: an assignment the edit does not reach
		if prev !== nothing && !dirty[k] && !isdraw(k)
			push!(done, k)
			continue
		end
		isdraw(k) && unreplay!()                          # a call drawn after replayed ones continues their figure
		try
			Core.eval(mod, Expr(:toplevel, st.ex))
			push!(done, k)
		catch e
			nd === nothing || (errs[nd.id] = sprint(showerror, e))
			break
		end
		# a box that drew something (the figure file changed) is pictured as the figure stands now
		if isdraw(k) && stamp() != last
			snapps[k] = read(ps)
			nd !== nothing && nd.kind === :call && snap!(k, nd.id)
			pictured = k
			last = stamp()
		end
	end
	unreplay!()
	_NG_RUNS[h] = NGRun(texts, Any[st.ex for st in stmts], mod, done, snapps, spics, scpt, stamp())
	return pics, errs
end

"""
	_nodegraph_rewrite(src, stmt, key, val) -> String

`src` with ONE part of statement `stmt` replaced by the Julia expression `val`: the kwarg `key`, the
positional argument `"#i"`, or (`"="`) an assignment's right-hand side. Only that statement's code is
rewritten; every other byte, its trailing comment included, stays as it was. Pure.
"""
function _nodegraph_rewrite(src::String, stmt::Int, key::String, val::String)::String
	stmts = _ng_statements(src)
	1 <= stmt <= length(stmts) || error("the script changed: statement $stmt is gone — press Refresh")
	vex = Meta.parse(val; raise = false)
	(vex isa Expr && vex.head in (:error, :incomplete)) && error("'$val' is not a complete Julia expression")
	st = stmts[stmt]
	ex = deepcopy(st.ex)
	inner = _ng_unconst(ex)
	if key == "="
		_ng_isassign(inner) || error("statement $stmt is not an assignment")
		inner.args[2] = vex
	else
		call = _ng_iscall(inner) ? inner : (_ng_isassign(inner) && _ng_iscall(inner.args[2])) ? inner.args[2] : nothing
		call === nothing && error("statement $stmt is not a call")
		done = false
		if startswith(key, "#")
			i = parse(Int, key[2:end])
			for (j, a) in enumerate(call.args)
				j == 1 && continue
				(a isa Expr && a.head in (:kw, :parameters)) && continue
				i -= 1
				i == 0 && (call.args[j] = vex; done = true; break)
			end
		else
			k = Symbol(key)
			vex === false && (vex = :nothing)     # a flag turned off: GMT.jl asserts on `false`, skips `nothing`
			for a in call.args
				a isa Expr || continue
				if a.head === :kw && a.args[1] === k
					a.args[2] = vex; done = true; break
				elseif a.head === :parameters
					for p in a.args
						(p isa Expr && p.head === :kw && p.args[1] === k) && (p.args[2] = vex; done = true)
					end
				end
			end
		end
		done || error("statement $stmt has no argument '$key' — press Refresh")
	end
	out = src[1:prevind(src, st.start)] * _ng_render(ex) * src[nextind(src, st.stop):end]
	Meta.parseall(out)                                  # never hand the editor a script that does not parse
	return out
end

"""
	_nodegraph_edit(h, stmt, key, val) -> nothing

A field edited in a box: rewrite that part of the editor's text (`_nodegraph_rewrite`), put the text
back in the editor box, and redraw the graph from it.
"""
function _nodegraph_edit(h::Ptr{Cvoid}, stmt::Int, key::String, val::String)::Nothing
	out = _nodegraph_rewrite(_script_editor_text(h), stmt, key, val)
	ccall(_fn(:gmtvtk_script_editor_set_text_h), Cint, (Ptr{Cvoid}, Cstring), h, out)
	_nodegraph_open(h; fresh = false)
	return nothing
end
