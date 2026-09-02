# gridcalc.jl — Grid Tools > "Grid calculator" (port of Mirone's src_figs/grid_calculator.m).
# Combine the grids of one window with an arithmetic expression, node by node.
#
# The C++ dialog is GridCalculatorDialog (70_window.cpp, loads deps/ui/grid_calculator.ui). Two
# entry points:
#
#   _gridcalc_names(scene, basename) — fills that list: every grid of the window whose limits AND
#       increments match the window's base grid EXACTLY (anything else cannot be combined node by
#       node, so it is not offered).
#   _on_gridcalc(scene, params)      — the Compute button: substitute every grid name by its data,
#       evaluate the expression element-wise, add the result as a new derived grid.
#
# HOW A GRID IS NAMED IN AN EXPRESSION. Mirone had to preface every name with `&` because it parsed
# and eval'd the raw string; here the names are substituted TEXTUALLY, before anything is parsed, so
# a BARE name is enough — `layer B + 1` works, blanks and all. `&name` / `&{name}` stay valid (that
# is what an old expression looks like, and Mirone muscle memory) and are resolved first.
# The one case that still NEEDS the `&`: a grid whose label is exactly a function name this
# calculator offers (a grid called `abs`), where the bare form would eat the function call. Those
# labels are left alone in the bare pass — see `_GC_FUNCS`.
# A bare name is matched only when it stands alone: the characters around it must not be part of an
# identifier, so a grid called `a` never matches the `a` inside `atan`. Longest names are tried
# first, so `layer0.grd` wins over a grid also called `layer0`.

# `&{any label}` or `&plain_name`.
const _GC_TOKEN = r"&\{([^}]*)\}|&([A-Za-z_][A-Za-z0-9_.]*)"

# Function names the keypad offers (plus the ones Base makes reachable in the same breath). A grid
# whose label is one of these can only be written `&label`.
const _GC_FUNCS = Set(["sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
                       "exp", "log", "log2", "log10", "sqrt", "abs", "sign", "floor", "ceil",
                       "round", "min", "max", "pi", "e"])

# The grid name written inside a token (braced or not).
function _gc_token_name(tok::AbstractString)::String
	s = String(tok)[2:end]                       # drop the '&'
	return (startswith(s, '{') && endswith(s, '}')) ? s[2:prevind(s, lastindex(s))] : s
end

# Can these two grids be combined node by node? Same node count, same limits, same increments and
# same registration — "EXACT SAME LIMITS and increments", with only a floating-point epsilon of
# slack (a grid that went through a file round-trip can differ in the last bit).
function _gc_same_geom(G::GMTgrid, ref::GMTgrid)::Bool
	size(G.z) == size(ref.z) || return false
	G.registration == ref.registration || return false
	tolx = abs(ref.inc[1]) * 1e-6;  toly = abs(ref.inc[2]) * 1e-6
	(abs(G.inc[1] - ref.inc[1]) <= tolx && abs(G.inc[2] - ref.inc[2]) <= toly) || return false
	for (k, tol) in ((1, tolx), (2, tolx), (3, toly), (4, toly))
		abs(G.range[k] - ref.range[k]) <= tol || return false
	end
	return true
end

# Every grid this window knows about, in Scene Objects order: [(label, GMTgrid), …]. The window's
# base grid is remembered under the empty name (grid.jl `_remember_object!(h, :grid, "", G)`), so it
# takes the label the dialog sees in Scene Objects (`basename`, handed over by the C++ side).
function _gridcalc_objects(scene::Ptr{Cvoid}, basenm::AbstractString)
	out = Tuple{String,GMTgrid}[]
	for (kind, nm, data) in get(_SCENE_OBJS, scene, Tuple{Symbol,String,Any}[])
		kind === :grid && isa(data, GMTgrid) || continue
		label = isempty(nm) ? String(basenm) : nm
		any(t -> t[1] == label, out) || push!(out, (label, data))
	end
	if isempty(out)                                   # never registered? fall back to the figure itself
		fig = get(_FIGREG, scene, nothing)
		fig isa QtFigure && push!(out, (String(basenm), fig.G))
	end
	return out
end

# Print (one per line) the grids of `scene` usable in one expression: the base grid and every other
# grid sharing its limits/increments. Called from the dialog through the g_juliaEval bridge, which
# hands back whatever this prints — hence `print`, and `nothing` as the value.
function _gridcalc_names(scene::Ptr{Cvoid}, basenm::AbstractString)
	objs = _gridcalc_objects(scene, basenm)
	isempty(objs) && return nothing
	ref = objs[1][2]
	print(join((nm for (nm, G) in objs if _gc_same_geom(G, ref)), '\n'))
	return nothing
end

# Replace the BARE occurrences of the known grid labels in `code` by whatever `tovar(name)` returns.
# Longest label first (so `layer0.grd` is consumed before a grid also called `layer0`), and a label
# only counts when it stands alone — the characters on both sides must not be identifier characters,
# which is what keeps a grid named `a` out of the `a` in `atan`. Labels that ARE function names are
# skipped: those can only be written `&label` (see _GC_FUNCS).
function _gc_replace_bare(code::String, names::Vector{String}, tovar::Function)::String
	isident(c::Char) = isletter(c) || isdigit(c) || c == '_' || c == '.'
	out = code
	for nm in sort(unique(names), by = length, rev = true)
		(isempty(nm) || nm in _GC_FUNCS) && continue
		occursin(nm, out) || continue
		res = IOBuffer();  i = firstindex(out)
		while i <= lastindex(out)
			r = findnext(nm, out, i)
			r === nothing && break
			pre = first(r) > firstindex(out) ? out[prevind(out, first(r))] : ' '
			nxt = last(r)  < lastindex(out)  ? out[nextind(out, last(r))]  : ' '
			print(res, SubString(out, i, prevind(out, first(r))))
			print(res, (isident(pre) || isident(nxt)) ? nm : tovar(nm))
			i = nextind(out, last(r))
		end
		print(res, SubString(out, i))
		out = String(take!(res))
	end
	return out
end

# The maths, apart from any window: substitute every grid name in `expr` by the matching grid and
# evaluate the expression element-wise. `objs` is [(label, grid), …] (the window's grids, first one
# = the reference geometry), `files` maps a file NAME to its path (the "Load Grid" entries, read
# here — Mirone's button likewise only stores the name). Returns the result array.
# Kept separate from the callback so the whole compute path is testable without a live window.
function _gridcalc_eval(objs::Vector{Tuple{String,GMTgrid}}, files::Dict{String,String}, expr::AbstractString)
	isempty(objs) && error("this window has no grid")
	ref = objs[1][2]
	# Each name becomes a generated variable, the grid behind it read (and geometry-checked) the first
	# time it appears. Same name twice = same variable, so a grid is never read twice. ONE resolver for
	# both spellings (`&name` and bare `name`) — they must never diverge in what they accept.
	vars = Dict{String,Symbol}()
	arrays = Pair{Symbol,Any}[]
	local badname = ""
	resolve = function (nm::AbstractString)
		haskey(vars, nm) && return string(vars[nm])
		local G = nothing
		for (label, g) in objs
			if label == nm
				G = g
				break
			end
		end
		if G === nothing
			path = get(files, nm, "")
			if isempty(path)
				badname = "unknown grid: " * nm
				return "nothing"
			end
			G = _gmtread_trb(path)      # grids are READ in "TRB" — THE reader
			if !isa(G, GMTgrid)
				badname = nm * " is not a grid"
				return "nothing"
			end
		end
		if !_gc_same_geom(G, ref)
			badname = nm * ": limits/increments differ from " * objs[1][1]
			return "nothing"
		end
		v = Symbol("_gc", length(vars) + 1)
		vars[nm] = v
		# One element order for the whole expression — `ref`'s. A grid that already lies that way
		# (the normal case) is taken as it is; one that does not is re-ordered by `_z_as`, because
		# broadcasting buffers that run different ways would pair up points that are not the same
		# point and quietly return a vertically mirrored result. The result therefore comes out in
		# `ref`'s own order, which is the layout `mat2grid(z, ref)` then labels it with.
		push!(arrays, v => _z_as(G, ref))
		return string(v)
	end

	# Pass 1: the explicit `&name` / `&{name}` tokens.
	code = replace(String(expr), _GC_TOKEN => tok -> resolve(_gc_token_name(tok)))
	isempty(badname) || error(badname)
	# Pass 2: the same names written bare. Only labels we actually know are looked for, so nothing
	# else in the expression can be touched.
	known = String[nm for (nm, _) in objs]
	append!(known, keys(files))
	code = _gc_replace_bare(code, known, resolve)
	isempty(badname) || error(badname)
	isempty(vars) && error("the expression uses no grid (nothing to compute)")

	# Evaluate in a throwaway module holding only the grid variables. `@.` broadcasts the WHOLE
	# expression, so `&a * &b + sin(&c)` is element-wise without the user having to type any dot.
	m = Module(:GridCalcSandbox)
	for (v, z) in arrays
		Core.eval(m, :($v = $z))
	end
	z = Core.eval(m, Meta.parse("@. " * code))
	isa(z, AbstractArray) || error("the expression produced a $(typeof(z)), not a grid")
	size(z) == size(ref.z) || error("the expression produced a $(size(z)) array, not $(size(ref.z))")
	return z
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGridCalcFn — expr / base / file1…fileN.
# Returns Cint 1 on success, 0 on failure — the dialog turns this into its own modal answer.
function _on_gridcalc(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		expr = strip(_get(d, "expr"))
		isempty(expr) && error("empty expression")
		occursin('`', expr) && error("backquotes are not allowed in an expression")

		objs = _gridcalc_objects(scene, _get(d, "base"))
		files = Dict{String,String}()
		for k in 1:1000
			p = _get(d, "file$k");  isempty(p) && break
			files[basename(p)] = p
		end

		z = _gridcalc_eval(objs, files, expr)
		G = GMT.mat2grid(Float32.(z), objs[1][2])
		_grid_command!(G, "InteractiveGMT grid calculator: " * String(expr))
		title = length(expr) <= 60 ? String(expr) : String(expr)[1:60] * "…"
		return _gm3d_deliver(scene, G, title, "", false, "gridcalc")
	catch e
		_tool_failed(scene, "Grid calculator", e)
		return Cint(0)
	end
end

function _register_gridcalc()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_gridcalc, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_gridcalc_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
