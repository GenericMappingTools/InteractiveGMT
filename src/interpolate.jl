# interpolate.jl — GMT menu > "Interpolate": grid an x,y,z table with one of GMT's gridding modules,
# through GMT.jl.
#
# The C++ dialog is InterpolationDialog (70_window.cpp, loads deps/ui/interpolation_dialog.ui), whose
# layout is Mirone's Surface window (src_figs/griding_mir.m): the input table + header count, the
# shared Griding Line Geometry block, the Near-Neighbor search radius, the Griding Method combo and
# its per-method Options window. blockmode, greenspline and sphinterpolate are added because GMT
# grids with them too. Mirone's "Minimum Curvature - mbgrid" is back: it is not a GMT module, so it
# is not GMT.jl that runs it but `mbgrid` (src/mbgrid.jl, over deps/src/mbgrid.c) — which is a plain
# Julia function taking `region`/`inc` keywords exactly like the GMT modules beside it, so it enters
# the same _interp_module table and needs no fork of the callback.
#
# The dialog sends the method's own options as one "opt_<kwarg>=<value>" line each, and the KEYS ARE
# GMT.jl KEYWORD NAMES — so the common case is a straight copy into `kw`. Only the handful that cannot
# be a plain keyword (surface's two tension boxes, its Clip-cells form of -M, greenspline's spline +
# tension pair) are translated below, each with the reason.

# Read the table ONCE, and let that same GMTdataset serve BOTH the gridding and the "Plot pts"
# overlay — one read, one source of truth for what was gridded. `gmtconvert` (not `gmtread`) because
# it is the reader that parses -h (skip header lines) and -: (the file is y,x,z), which are exactly
# the two knobs the dialog offers.
function _interp_read(infile::AbstractString, nheaders::AbstractString, toggle::Bool)
	kw = Dict{Symbol,Any}()
	isempty(nheaders) || (kw[:h] = parse(Int, nheaders))
	toggle && (kw[:yx] = true)
	D = GMT.gmtconvert(String(infile); kw...)
	D === nothing && error("could not read $infile")
	return D
end

# Method -> the GMT.jl function that grids with it. A method the dialog can offer but this table does
# not know is a bug, not a silent fallback, so it errors.
function _interp_module(method::AbstractString)
	method == "surface"        && return GMT.surface
	method == "nearneighbor"   && return GMT.nearneighbor
	method == "triangulate"    && return GMT.triangulate
	method == "blockmean"      && return GMT.blockmean
	method == "blockmedian"    && return GMT.blockmedian
	method == "blockmode"      && return GMT.blockmode
	method == "greenspline"    && return GMT.greenspline
	method == "sphinterpolate" && return GMT.sphinterpolate
	method == "mbgrid"         && return mbgrid          # ours, not GMT's — see the header note
	error("unknown gridding method '$method'")
end

# Build the module's keyword set from the parsed dialog block. Split out of the callback so the
# mapping can be tested without a window (test/test-interpolate-unit.jl).
function _interp_kwargs(d::Dict{String,String}, method::AbstractString, geog::Bool)
	kw = Dict{Symbol,Any}()
	kw[:region] = _get(d, "region")
	kw[:inc]    = _get(d, "inc")
	_on(d, "pixel")   && (kw[:registration] = :pixel)
	_on(d, "verbose") && (kw[:verbose] = true)
	# -fg: the modules that measure DISTANCES (a search radius, a spherical spline) refuse a distance
	# unit on a grid they think is Cartesian — same trap grdfilter's -D hit. An in-memory dataset does
	# not carry its geographic-ness, so it is stated explicitly. mbgrid is not a GMT module and has no
	# -f to state it to: it measures its Gaussian in CELLS, so there is no distance unit to resolve.
	(geog && method != "mbgrid") && (kw[:f] = :g)
	# Near Neighbor's -S is required and lives in the main dialog (Mirone's "For Nearneighbor only"
	# group), not in the Options window, so it is not an opt_ line.
	r = _get(d, "radius")
	(method == "nearneighbor" && !isempty(r)) && (kw[:search_radius] = r)
	# block* return a TABLE of per-block values unless -A names a field to grid; the dialog's Options
	# window defaults that field, but the module must produce a grid even if it was never opened.
	if method in ("blockmean", "blockmedian", "blockmode")
		haskey(d, "opt_field") || (kw[:field] = method == "blockmean" ? "mean" :
		                                        method == "blockmedian" ? "median" : "mode")
	end

	# A ticked checkbox arrives as the literal word "true" — NOT as "1", which is a legitimate VALUE
	# for the numeric options (greenspline's distance mode, nearneighbor's sector count).
	for (k, v) in d
		startswith(k, "opt_") || continue
		isempty(v) && continue
		kw[Symbol(k[5:end])] = v == "true" ? true : v
	end

	# --- the handful that are NOT a straight keyword ------------------------------------------
	if method == "surface"
		# Two boxes, one option: -M takes either a distance ("50k") or a cell count ("2c"). The
		# Clip-cells box is the second form, so it lands on the same keyword with the 'c' appended.
		if haskey(kw, :maskcells)
			mc = string(pop!(kw, :maskcells))
			haskey(kw, :mask) || (kw[:mask] = endswith(mc, "c") ? mc : mc * "c")
		end
		# -T is given TWICE when interior and boundary tension differ (-Ti0.25 -Tb0.5); GMT.jl's
		# `tension` keyword emits one -T, so the second one rides in the same string. Equal values
		# need no prefix at all — that is the plain -T form which sets both.
		ti = haskey(kw, :tension_i) ? string(pop!(kw, :tension_i)) : ""
		tb = haskey(kw, :tension_b) ? string(pop!(kw, :tension_b)) : ""
		if !isempty(ti) && !isempty(tb)
			kw[:tension] = (ti == tb) ? ti : "i$ti -Tb$tb"
		elseif !isempty(ti)
			kw[:tension] = "i$ti"
		elseif !isempty(tb)
			kw[:tension] = "b$tb"
		end
	elseif method == "greenspline"
		# -S is <directive>[<tension>]: the combo picks the letter, the box the tension it takes.
		if haskey(kw, :tension)
			t = string(pop!(kw, :tension))
			kw[:splines] = string(get(kw, :splines, "t")) * t
		end
	end
	return kw
end

# C callback (Compute button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaInterpolateFn. Returns Cint 1 on success, 0 on failure.
function _on_interpolate(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		method = _get(d, "method")
		infile = _get(d, "infile")
		isempty(infile) && error("no input data file")

		D = _interp_read(infile, _get(d, "headers"), _on(d, "toggle"))
		# "auto" is what the dialog opens with: ask GMT the same question it asks itself elsewhere
		# (GMT.guessgeog), never a private lon/lat range test of our own.
		coords = _get(d, "coords", "auto")
		geog = coords == "geog" ? true : (coords == "cart" ? false : GMT.guessgeog(D))

		kw = _interp_kwargs(d, method, geog)
		R = _interp_module(method)(D; kw...)

		# surface -Q does not grid anything — it only reports the dimensions with a highly composite
		# factor, on GMT's own message stream. Say so instead of reporting a failure.
		if !isa(R, GMTgrid) && haskey(kw, :suggest)
			_viewer_log_error(scene, "surface -Q only REPORTS suggested dimensions (see the Julia console); nothing was gridded")
			return Cint(1)
		end
		isa(R, GMTgrid) || error("got a $(typeof(R)), not a grid")

		ok = _gm3d_deliver(scene, R, "Gridded ($method)", _get(d, "outfile"), false,
		                   "$method " * join(("$k=$v" for (k, v) in kw), ' ');
		                   geographic = (coords == "auto" ? nothing : geog))
		# The data points go ON TOP of the new grid (vectors always ride above rasters), from the very
		# dataset that was gridded — not a second read of the file.
		if ok == Cint(1) && _on(d, "plotpts")
			mat = D isa Vector ? reduce(vcat, (dd.data for dd in D)) : D.data
			add_symbols!(scene, view(mat, :, 1), view(mat, :, 2); symbol = :circle, size = 5,
			             fill = :black, edge = :white, edgewidth = 0.5,
			             name = "Data points (" * basename(String(infile)) * ")")
		end
		return ok
	catch e
		_viewer_log_error(scene, "Interpolate FAILED: $(sprint(showerror, e))")
		@warn "Interpolate FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_interpolate()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_interpolate, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_interpolate_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
