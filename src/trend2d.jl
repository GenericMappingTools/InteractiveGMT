# trend2d.jl — GMT menu > "trend2d": fit a [weighted] [robust] polynomial z = f(x,y) to an x,y,z
# TABLE, through GMT.jl's own `trend2d`. The grid twin of this is grdtrend (which fits the window's
# grid); this one fits scattered points, so its input is a file, exactly like the Interpolate dialog.
#
# The C++ dialog is Trend2DDialog (70_window.cpp, loads deps/ui/trend2d_dialog.ui).
#
# The table is read by `_interp_read` (interpolate.jl) — THE reader for this dialog family, the one
# that already speaks the two knobs both dialogs offer (header lines, and a file whose columns are
# y,x,z). One reader, not a second copy with the same two flags.

# The -F letters the dialog ticked, in the module's own order, or "p" for "just the parameters".
# GMT takes them in any order; keeping one fixed order means the column names below always match
# what actually comes back.
const _TREND2D_COLS = (("x", "x"), ("y", "y"), ("z", "z"), ("m", "model"), ("r", "residual"),
                       ("w", "weight"))

function _trend2d_F(d::Dict{String,String})::String
	_on(d, "params") && return "p"                  # -Fp: the model parameters, not the points
	s = ""
	for (letter, _) in _TREND2D_COLS
		_on(d, "col_" * letter) && (s *= letter)
	end
	isempty(s) && error("choose at least one output column, or ask for the model parameters")
	return s
end

# -N n_model[+r]: 1 to 10 terms, the same limit grdtrend's surface fit has, and +r for the robust
# (iteratively reweighted) fit.
function _trend2d_N(d::Dict{String,String})::String
	n = parse(Int, _get(d, "model", "3"))
	(1 <= n <= 10) || error("the model takes 1 to 10 terms, not $n")
	return string(n) * (_on(d, "robust") ? "+r" : "")
end

# Column names for the Data Viewer, straight from the -F letters that produced them.
function _trend2d_colnames(F::AbstractString, ncol::Int)::Vector{String}
	F == "p" && return ["Model parameter"][1:min(ncol, 1)]
	names = String[]
	for c in F
		for (letter, name) in _TREND2D_COLS
			c == letter[1] && push!(names, name)
		end
	end
	return length(names) >= ncol ? names[1:ncol] : names
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaTrend2DFn. Returns Cint 1 on success, 0 on failure.
function _on_trend2d(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		infile = _get(d, "infile")
		isempty(infile) && error("no input table")
		isfile(infile) || error("input table not found: $infile")

		F = _trend2d_F(d)
		kw = Dict{Symbol,Any}(:F => F, :N => _trend2d_N(d))
		cond = _get(d, "condition")
		if !isempty(cond)
			(tryparse(Float64, cond) === nothing) &&
				error("the maximum condition number must be a number, not '$cond'")
			kw[:C] = cond
		end
		if _on(d, "iterate")
			# -I alone means "iterate at the default 0.51 confidence"; with a level it must be a
			# probability, which is what the module's own remarks section says.
			conf = _get(d, "confidence")
			if isempty(conf)
				kw[:I] = true
			else
				c = tryparse(Float64, conf)
				(c === nothing || !(0 < c < 1)) &&
					error("the confidence level is a probability between 0 and 1, not '$conf'")
				kw[:I] = conf
			end
		end
		w = _get(d, "weights")                       # "" | "plain" | "+s" | "+w"
		if !isempty(w)
			kw[:W] = (w == "plain") ? true : w       # -W, -W+s (one-sigma) or -W+w (as read)
		end

		D = _interp_read(infile, _get(d, "headers"), _on(d, "toggle"))   # THE reader (interpolate.jl)
		R = GMT.trend2d(D; kw...)
		R === nothing && error("trend2d returned nothing")
		Rd = isa(R, Vector) ? (isempty(R) ? error("trend2d returned no rows") : R[1]) : R
		isa(Rd, GMTdataset) || error("got a $(typeof(Rd)), not a table")

		Rd.colnames = _trend2d_colnames(F, size(Rd.data, 2))
		out = _get(d, "outfile");  isempty(out) || GMT.gmtwrite(String(out), Rd)
		title = F == "p" ? "trend2d parameters" : "trend2d (N=$(kw[:N]))"
		show_table(scene, Rd; name = title)

		# The fitted points can also go ON the map, at the coordinates they were READ from (the input
		# table's own x,y — the output rows are in the same order, and the user may well not have asked
		# for x and y as output columns at all). Each carries its fitted row as a hover tooltip.
		if F != "p" && _on(d, "plotpts")
			mat = D isa Vector ? reduce(vcat, (dd.data for dd in D)) : D.data
			m = Rd.data
			n = min(size(mat, 1), size(m, 1))
			names = Rd.colnames
			infos = [join(("$(names[c]) = $(m[k, c])" for c in 1:size(m, 2)), '\n') for k in 1:n]
			# The layer also CARRIES the fit, one row per point, for "Show data table" — the same table
			# the dialog just opened, reachable again from the symbols themselves. Data, not graphics:
			# the fitted columns, never the symbol's size.
			add_symbols!(scene, view(mat, 1:n, 1), view(mat, 1:n, 2); symbol = :circle, size = 6,
			             fill = :cyan, edge = :black, edgewidth = 0.5,
			             name = "trend2d (" * basename(String(infile)) * ")", info = infos,
			             datanames = String.(names),
			             datarows = [[string(m[k, c]) for c in 1:size(m, 2)] for k in 1:n])
		end
		return Cint(1)
	catch e
		_viewer_log_error(scene, "trend2d FAILED: $(sprint(showerror, e))")
		@warn "trend2d FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_trend2d()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_trend2d, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_trend2d_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
