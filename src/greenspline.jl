# greenspline.jl — GMT menu > "greenspline": grid (or evaluate) scattered data with the Green's
# function of one of six splines, in 1, 2 or 3 dimensions.
#
# The C++ dialog is GreensplineDialog (70_window.cpp, loads deps/ui/greenspline_dialog.ui).
#
# WHY A DIALOG OF ITS OWN, when the Interpolate dialog already offers greenspline as one of its
# gridding methods: Interpolate treats every method the same way — a table in, -R/-I, a grid out —
# and that is all it can give this module. It is most of greenspline for the common case and none of
# what makes greenspline different: the distance mode that decides whether the data are a line, a
# plane, a sphere or a volume; the eigenvalue truncation that turns an exact interpolator into a
# smoother; surface-gradient constraints; evaluating at listed points instead of on a grid; a mask
# grid's nodes; the directional derivative; the misfit report. That is this dialog. Anyone who just
# wants "grid my x,y,z with a spline in tension" should keep using Interpolate.
#
# The table is read by `_interp_read` (interpolate.jl) — THE reader for this dialog family, the one
# that already speaks the two knobs every table dialog here offers (header lines, and a file whose
# columns are y,x,z). One reader, not a third copy of it. The module then runs through GMT.jl's own
# `greenspline`, the same wrapper the Interpolate dialog drives.
#
# TWO REFUSALS THAT ARE THIS WINDOW'S, NOT THE MODULE'S: a grid can only come back when the problem
# is two-dimensional. -D0 is a line and -D4 is a volume; greenspline will happily make a 1-D series
# or a 3-D cube out of them, but this window shows surfaces, so those two modes are offered only
# through "values at listed points". Everything else refused below is the module's own rule.

# -D<mode>: the distance flag, and with it the DIMENSION of the problem — which is what nearly every
# other rule keys off. 0 is a line, 4 is a volume, and everything else is a surface (1/2/3 as a
# plane, flat Earth and sphere; 5 as a spherical surface addressed by dot products).
_gs_dim(mode::Int)::Int = mode == 0 ? 1 : (mode == 4 ? 3 : 2)

function _gs_mode(d::Dict{String,String})::Int
	s = _get(d, "dmode")
	isempty(s) && error("choose the distance mode (-D) — it is what tells greenspline whether the data are a line, a plane, a sphere or a volume")
	v = tryparse(Int, s)
	(v === nothing || !(0 <= v <= 5)) && error("the distance mode is 0 to 5, not '$s'")
	return v
end

# -S<method>[<tension>]: the six splines. The last two live on a sphere and take -D5, and -D5 takes
# nothing else — the module says so both ways, so this does too.
const _GS_SPLINES = Dict{String,String}(
	"c" => "minimum curvature (Sandwell, 1987)",
	"t" => "continuous curvature in tension (Wessel & Bercovici, 1998)",
	"l" => "linear / bilinear / trilinear",
	"r" => "regularized in tension (Mitasova & Mitas, 1993)",
	"p" => "minimum curvature on a sphere (Parker, 1994)",
	"q" => "continuous curvature in tension on a sphere (Wessel & Becker, 2008)")

# The three that take a tension; the other three take none.
_gs_takes_tension(sp::AbstractString)::Bool = sp in ("t", "r", "q")
# The two that live on a sphere.
_gs_spherical(sp::AbstractString)::Bool = sp in ("p", "q")

function _gs_S(d::Dict{String,String}, mode::Int)::String
	sp = _get(d, "spline")
	isempty(sp) && error("choose a spline (-S)")
	haskey(_GS_SPLINES, sp) || error("unknown spline '$sp'")
	if _gs_spherical(sp)
		mode == 5 || error("the spherical spline -S$sp only works with the spherical distance mode -D5, not -D$mode")
	elseif mode == 5
		error("distance mode -D5 is the spherical surface: it takes only the spherical splines -Sp or -Sq, not -S$sp")
	end
	_gs_takes_tension(sp) || return sp
	t = _get(d, "tension")
	isempty(t) && error("the -S$sp spline needs a tension")
	# A tension may carry a length scale after a slash (-St0.5/10). Only the tension itself is checked.
	head = split(t, '/')[1]
	v = tryparse(Float64, head)
	v === nothing && error("the tension must be a number (optionally followed by /scale), not '$t'")
	(0 <= v < 1) || error("the tension must lie in [0, 1), not $v")
	return sp * t
end

# -C[[n|r|v]<value>][+f<file>]: solve by SVD and throw away the small eigenvalues, which is what
# turns an exact interpolator into a smoother. Three ways of saying how many to keep.
#   (blank) / r   the smallest eigenvalue RATIO to the largest that is still used, 0 to 1
#   n             the NUMBER of largest eigenvalues to use
#   v             the fraction of the VARIANCE to explain, 0 to 1
# The +c, +i and +n modifiers the manual also lists are NOT offered: they are not covered here
# rather than guessed at (the green ? button opens the module's own page).
function _gs_C(d::Dict{String,String})
	_on(d, "approx") || return nothing
	kind = _get(d, "ckind", "r")
	kind in ("r", "n", "v") || error("unknown eigenvalue cut '$kind'")
	s = _get(d, "cvalue")
	isempty(s) && error("the eigenvalue cut (-C) needs a value")
	if kind == "n"
		v = tryparse(Int, s)
		(v === nothing || v < 1) && error("the number of eigenvalues to keep must be a positive whole number, not '$s'")
	else
		v = tryparse(Float64, s)
		(v === nothing || !(0 <= v <= 1)) &&
			error("the eigenvalue $(kind == "v" ? "variance fraction" : "ratio") must lie in [0, 1], not '$s'")
	end
	out = (kind == "r" ? "" : kind) * s
	f = _get(d, "cfile")
	isempty(f) || (out *= "+f" * _gmt_quote_path(f))     # a path that travels inside the option STRING
	return out
end

# -A<gradfile>+f<format>: surface gradient constraints. The record layout is passed THROUGH as the
# number the manual lists (1 to 5) rather than described here in words that could drift from it.
function _gs_A(d::Dict{String,String})
	f = _get(d, "gradfile")
	isempty(f) && return nothing
	isfile(f) || error("the gradient file was not found: $f")
	fmt = _get(d, "gradformat", "1")
	v = tryparse(Int, fmt)
	(v === nothing || !(1 <= v <= 5)) && error("the gradient record format is 1 to 5, not '$fmt'")
	return _gmt_quote_path(f) * "+f" * string(v)
end

# -L[t][r]: greenspline removes a linear (1-D) or planar (2-D) trend before solving and restores it
# after. The two ticks are the two halves of that, each switchable on its own.
function _gs_L(d::Dict{String,String})
	s = (_on(d, "notrend") ? "t" : "") * (_on(d, "norestore") ? "r" : "")
	return isempty(s) ? nothing : s
end

# -E[<misfitfile>][+r<reportfile>]: evaluate the spline at the data points and report how far it
# missed. A bare tick sends the statistics to GMT's own message stream; a file gets the full table.
function _gs_E(d::Dict{String,String})
	_on(d, "misfit") || return nothing
	f = _get(d, "misfitfile")
	r = _get(d, "reportfile")
	out = isempty(f) ? "" : _gmt_quote_path(f)
	isempty(r) || (out *= "+r" * _gmt_quote_path(r))
	return isempty(out) ? true : out
end

# -Q[<az>|<x/y/z>]: return the directional derivative instead of the surface. A surface takes ONE
# azimuth; a volume takes three direction cosines. Getting that pair the wrong way round is the
# mistake worth catching before the module runs.
function _gs_Q(d::Dict{String,String}, dim::Int)
	_on(d, "deriv") || return nothing
	s = _get(d, "derivdir")
	isempty(s) && error("the directional derivative (-Q) needs a direction")
	slashes = count(==('/'), s)
	if dim == 3
		slashes == 2 || error("in three dimensions -Q takes the direction cosines as x/y/z, not '$s'")
	else
		slashes == 0 || error("on a surface -Q takes a single azimuth, not '$s'")
	end
	all(p -> tryparse(Float64, p) !== nothing, split(s, '/')) ||
		error("the -Q direction must be numbers, not '$s'")
	return s
end

# Column names for the Data Viewer when the answer is a table (-N): the coordinates the chosen
# dimension carries, then the value the spline put there.
function _gs_colnames(dim::Int, ncol::Int)::Vector{String}
	names = dim == 1 ? ["x"] : dim == 3 ? ["x", "y", "z"] : ["x", "y"]
	push!(names, "value")
	return length(names) >= ncol ? names[1:ncol] : names
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGreensplineFn. Returns Cint 1 on success, 0 on failure.
function _on_greenspline(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		infile = _get(d, "infile")
		isempty(infile) && error("no input table")
		isfile(infile) || error("input table not found: $infile")

		mode = _gs_mode(d)
		dim  = _gs_dim(mode)
		what = _get(d, "what", "grid")

		kw = Dict{Symbol,Any}(:D => mode, :splines => _gs_S(d, mode))
		# -D2, -D3 and -D5 all say the coordinates are DEGREES, so the module is told so explicitly:
		# an in-memory dataset does not carry its geographic-ness (the same trap the Interpolate
		# dialog's -fg note describes).
		geog = mode in (2, 3, 5)
		geog && (kw[:f] = :g)

		c = _gs_C(d);  c === nothing || (kw[:approx] = c)
		a = _gs_A(d);  a === nothing || (kw[:gradient] = a)
		l = _gs_L(d);  l === nothing || (kw[:leave_trend] = l)
		e = _gs_E(d);  e === nothing || (kw[:misfit] = e)
		q = _gs_Q(d, dim);  q === nothing || (kw[:dir_derivative] = q)
		# -W[w]: the last column is a one-sigma uncertainty, or (with w) a weight already.
		_on(d, "uncert") && (kw[:uncertainties] = _on(d, "isweight") ? "w" : true)
		_on(d, "verbose") && (kw[:verbose] = true)

		# Everything about WHERE the answer goes is settled before the table is read: these are
		# parameter checks, and a run that cannot work should say so before it spends time reading.
		what == "nodes" || what == "grid" || error("unknown output kind '$what'")
		nodes = ""
		if what == "nodes"
			nodes = _get(d, "nodefile")
			isempty(nodes) && error("choose the file of output locations (-N)")
			isfile(nodes) || error("the output-location file was not found: $nodes")
			kw[:nodes] = _gmt_quote_path(nodes)
		else
			# A grid is a SURFACE, so only the two-dimensional distance modes can make one.
			dim == 2 ||
				error("-D$mode is $(dim == 1 ? "one-dimensional (a line)" : "three-dimensional (a volume)"), which this window has no surface to show — use \"values at listed points\" instead")
			mask = _get(d, "maskgrid")
			if isempty(mask)
				region = _get(d, "region")
				inc    = _get(d, "inc")
				isempty(region) && error("give the region, or a mask grid whose nodes to use")
				isempty(inc)    && error("give the grid spacing, or a mask grid whose nodes to use")
				kw[:region] = region
				kw[:inc]    = inc
				_on(d, "pixel") && (kw[:registration] = :pixel)
			else
				isfile(mask) || error("the mask grid was not found: $mask")
				kw[:mask] = _gmt_quote_path(mask)
			end
		end

		D = _interp_read(infile, _get(d, "headers"), _on(d, "toggle"))

		if what == "nodes"
			R = GMT.greenspline(D; kw...)
			T = isa(R, Vector) ? (isempty(R) ? error("greenspline returned nothing") : R[1]) : R
			isa(T, GMTdataset) || error("got a $(typeof(T)), not a table")
			T.colnames = _gs_colnames(dim, size(T.data, 2))
			out = _get(d, "outfile")
			isempty(out) || GMT.gmtwrite(String(out), T)
			show_table(scene, T; name = "greenspline at " * basename(String(nodes)))
			return Cint(1)
		end

		R = GMT.greenspline(D; kw...)
		isa(R, GMTgrid) || error("got a $(typeof(R)), not a grid")
		sp = _get(d, "spline")
		title = q === nothing ? "greenspline (-S$sp -D$mode)" : "greenspline d/d($q) (-S$sp -D$mode)"
		ok = _gm3d_deliver(scene, R, title, _get(d, "outfile"), false,
		                   "greenspline " * join(("$k=$v" for (k, v) in kw), ' '); geographic = geog)
		# The data points go ON TOP of the new grid (vectors always ride above rasters), from the very
		# dataset that was gridded — not a second read of the file. The same thing the Interpolate
		# dialog does, for the same reason.
		if ok == Cint(1) && _on(d, "plotpts")
			mat = D isa Vector ? reduce(vcat, (dd.data for dd in D)) : D.data
			add_symbols!(scene, view(mat, :, 1), view(mat, :, 2); symbol = :circle, size = 5,
			             fill = :black, edge = :white, edgewidth = 0.5,
			             name = "Data points (" * basename(String(infile)) * ")")
		end
		return ok
	catch e
		_tool_failed(scene, "greenspline", e)
		return Cint(0)
	end
end

function _register_greenspline()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_greenspline, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_greenspline_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
