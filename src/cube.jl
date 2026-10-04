# cube.jl — CUBE (Combined Uncertainty and Bathymetry Estimator) as a Julia function over the C in
# deps/src/cube/mb_cube.c, a verbatim copy of MB-System's src/mbaux/mb_cube.c (the C port of NOAA OCS
# Hydrography's bathycube; see deps/src/cube/PROVENANCE.md). The SAME engine MB-System's `mbgrid -F9`
# runs, so a grid made here and one made there from the same soundings are the same grid.
#
# `cubegrid` takes `region`/`inc` keywords exactly like the GMT modules beside it, so it is one more
# row of GMT > Interpolate's method table (interpolate.jl) and needs no second code path there.
#
# CUBE works in METRES on DEPTHS (positive down). Geographic data are converted to local metres at the
# grid centre with GMT's own geodesic (mapproject -G), never a hand-rolled sphere; projected data are
# taken to be in metres. A plain x,y,z table carries no per-sounding uncertainty, so each sounding gets
# the IHO S-44 form at 95% confidence, TVU = sqrt(a^2 + (b*depth)^2) and THU = a + b*depth — by default
# the limits of the chosen IHO order, the same model mbgrid -F9 uses.

# Mirror of `mb_cube_params` (mb_cube.h), field for field: Cint for the enums/ints, Cdouble for the
# doubles, in declaration order, so Julia's C layout is the C struct's. Reorder nothing.
mutable struct _CubeParams
	iho_order::Cint
	grid_resolution_x::Cdouble
	grid_resolution_y::Cdouble
	no_data_value::Cdouble
	extractor::Cint
	nodata_depth::Cdouble
	nodata_variance::Cdouble
	dist_exponent::Cdouble
	inv_dist_exponent::Cdouble
	dist_scale::Cdouble
	var_scale::Cdouble
	iho_fixed::Cdouble
	iho_percent::Cdouble
	median_length::Cint
	quotient_limit::Cdouble
	discount::Cdouble
	est_offset::Cdouble
	bayes_factor_threshold::Cdouble
	runlength_threshold::Cint
	min_context::Cdouble
	max_context::Cdouble
	min_context_nodes::Cint
	max_context_nodes::Cint
	stddev_to_conf_scale::Cdouble
	blunder_min::Cdouble
	blunder_percent::Cdouble
	blunder_scalar::Cdouble
	capture_dist_scale::Cdouble
	variance_selection::Cint
	depth_tolerance::Cdouble
	max_hypothesis_ratio::Cdouble
	_CubeParams() = new()
end

const _CUBE_LAYOUT_COLS_SOUTH = Cint(1)    # out[col*ny + iy], iy from the south: GMT's own "BCB"

_cube_check(rc::Cint, what::String)::Nothing = (rc != 0 && error("CUBE: $what: " *
	unsafe_string(ccall(_fn(:mb_cube_strerror), Cstring, (Cint,), rc))); nothing)

# A name the C table knows (IHO order, method, variance selection) -> its enum value.
function _cube_enum(fn::Symbol, v, what::String)::Cint
	s = lowercase(String(strip(string(v))))
	out = Ref{Cint}(0)
	ccall(_fn(fn), Cint, (Cstring, Ref{Cint}), s, out) == 0 || error("CUBE: unknown $what '$s'")
	return out[]
end

# "a/b", (a, b) or [a, b] -> (a, b). `nothing` keeps the IHO default.
_cube_pair(::Nothing) = nothing
_cube_pair(v::Tuple) = (Float64(v[1]), Float64(v[2]))
_cube_pair(v::Vector) = (Float64(v[1]), Float64(v[2]))
function _cube_pair(v::String)
	t = strip(v)
	isempty(t) && return nothing
	p = [parse(Float64, String(s)) for s in split(replace(t, ',' => '/'), '/')]
	length(p) == 2 || error("CUBE: an uncertainty needs two numbers a/b, got \"$v\"")
	return (p[1], p[2])
end

# Geographic or not: what the caller said, else GMT's own guess on the data.
_cube_geog(g::Bool, data) = g
_cube_geog(g::String, data) = _mb_bool(g)
_cube_geog(::Nothing, data::GMTdataset) = GMT.guessgeog(data)
_cube_geog(::Nothing, data::Vector{<:GMTdataset}) = GMT.guessgeog(data[1])
_cube_geog(::Nothing, data) = false

# Metres per degree of longitude and of latitude at (lon0, lat0), by GMT's geodesic over a 0.01 deg step.
function _cube_m_per_deg(lon0::Float64, lat0::Float64)::NTuple{2,Float64}
	h = 0.01
	dx = GMT.mapproject([lon0 lat0]; G = "$(lon0 + h)/$(lat0)+ue").data[1, 3] / h
	dy = GMT.mapproject([lon0 lat0]; G = "$(lon0)/$(lat0 + h)+ue").data[1, 3] / h
	return (dx, dy)
end

# --- swath data (Geophysics > MB-System > CUBE gridding) -------------------------------------------
# A swath file or an MB-System datalist is read by THE swath-sounding reader, `_mb_good_dataset`
# (drop.jl: MB-System's mbgetdata), into an ordinary x,y,z table: longitude, latitude, elevation, good
# beams only. From there it is gridded exactly as a table is.

const _CUBE_SWATH_EXT = (".all", ".kmall", ".s7k", ".gsf", ".xtf", ".fbt")

# A datalist (.mb-1) or a swath file (an MB-System .mbNN, or a raw format MBIO reads by its suffix).
function _cube_is_swath(path::AbstractString)::Bool
	ext = lowercase(splitext(String(path))[2])
	return ext == ".mb-1" || occursin(r"^\.mb\d+$", ext) || ext in _CUBE_SWATH_EXT
end


# Region and spacing left empty: what GMT would choose for these soundings (the dialog's own prefill
# for a table, _gridmeta_string), so a swath survey can be gridded without typing a region.
function _cube_fill_geometry!(kw::Dict{Symbol,Any}, D)
	r = string(get(kw, :region, "")); i = string(get(kw, :inc, ""))
	(isempty(strip(r)) || isempty(strip(i))) || return kw
	m = split(_gridmeta_string(D), '/')
	length(m) >= 6 || error("CUBE: could not work out a region and spacing for the data")
	isempty(strip(r)) && (kw[:region] = join(m[1:4], '/'))
	isempty(strip(i)) && (kw[:inc] = m[5] * "/" * m[6])
	return kw
end

# C callback (CUBE dialog's Region block prefill): the geometry _cube_fill_geometry! would grid CUBE's
# input on -- of the swath file / datalist `cpath`, or of window `scene`'s swath point cloud when `cpath`
# is "" -- as "w/e/s/n/dx/dy/nx/ny". "" on failure. Julia-owned buffer, as _on_gridmeta.
const _CUBEMETA_BUF = Ref{Vector{UInt8}}(UInt8[0])
function _on_cube_meta(scene::Ptr{Cvoid}, cpath::Cstring)::Cstring
	s = ""
	try
		path = unsafe_string(cpath)
		D = isempty(path) ? _mb_cloud_dataset(scene) : _mb_good_dataset(path)
		s = _gridmeta_string(D)
	catch e
		_tool_failed(scene, "CUBE region", e)
	end
	_CUBEMETA_BUF[] = Vector{UInt8}(codeunits(s * "\0"))
	return Cstring(pointer(_CUBEMETA_BUF[]))
end

function _register_cubemeta()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_cube_meta, s, c)::Cstring, Cstring, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_cubemeta_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

"""
    cubegrid_all(data; region, inc, kwargs...) -> NamedTuple

Grid scattered soundings with CUBE (Combined Uncertainty and Bathymetry Estimator, Calder & Mayer,
CCOM/JHC) and return every grid it produces:
`(depth, uncertainty, n_hypotheses, ratio, n_points)`, each a `GMTgrid` on the same nodes.

- `depth`: the depth of the hypothesis chosen at each node (in the input's z convention, see `zdown`)
- `uncertainty`: CUBE's uncertainty of that depth at 95% confidence
- `n_hypotheses`: how many depth hypotheses the node holds (more than 1 = conflicting soundings)
- `ratio`: hypothesis strength ratio, 0 with a single hypothesis, larger = less sure of the choice
- `n_points`: soundings in the chosen hypothesis

`data` may be a `GMTdataset`, a vector of them, an N×3 matrix, `x, y, z` vectors, or the PATH of
MB-System swath data: a datalist (`.mb-1`) or a swath file (`.mbNN`, `.all`, `.kmall`, `.s7k`,
`.gsf`, ...), read through MB-System's MBIO. Swath soundings become longitude, latitude and
ELEVATION, so the depth grid comes back negative below sea level, like every other grid in iGMT.

# Geometry
- `region`: `(w, e, s, n)`, a 4-vector, or a `"w/e/s/n"` string.
- `inc`: one number, `(dx, dy)`, or `"dx/dy"`.

Either may be left out: it is then what GMT would choose for the data (its limits, `estimate_RI`).

# Options
- `iho_order = :order1a`  IHO S-44 order: `:exclusive`, `:special`, `:order1a`, `:order1b`, `:order2`.
                          Bounds how far a sounding spreads and sets the default uncertainty.
- `method = :local`       hypothesis selection: `:local` (guided by the nearest single-hypothesis
                          node), `:prior` (most soundings), `:posterior` (both), `:predicted`.
- `tvu = nothing`         sounding vertical uncertainty at 95% as `(a, b)`: sqrt(a^2 + (b*depth)^2) m.
- `thu = nothing`         sounding horizontal uncertainty at 95% as `(a, b)`: a + b*depth m.
                          `nothing` = the limits of `iho_order`.
- `variance = :cube`      reported uncertainty: `:cube` (posterior), `:input` (sounding spread), `:max`.
- `noqueue = false`       skip CUBE's median pre-filter queue.
- `paramfile = ""`        a bathycube CubeParameters JSON file; the options above override it.
- `zdown = false`         `false`: z is elevation (GMT convention, negative below sea level);
                          `true`: z is depth, positive down (MB-System convention).
- `geographic = nothing`  `true`/`false`, or `nothing` to let GMT guess from the data.
- `registration`          `:gridline` (default) or `:pixel`.
- `verbose = false`       CUBE's per-sounding DEBUG trace (very long).

Every option also accepts the string form the Interpolate dialog sends.
"""
function cubegrid_all(x, y, z; kwargs...)
	xx, yy, zz = _mb_xyz(x, y, z)
	return _cubegrid(xx, yy, zz, nothing; kwargs...)
end
function cubegrid_all(data; kwargs...)
	xx, yy, zz = _mb_xyz(data)
	kw = _cube_fill_geometry!(Dict{Symbol,Any}(kwargs), data)
	return _cubegrid(xx, yy, zz, data; kw...)
end
function cubegrid_all(path::String; kwargs...)
	D = _cube_is_swath(path) ? _mb_good_dataset(String(path)) : GMT.gmtread(path; data = true)
	kw = Dict{Symbol,Any}(kwargs)
	_cube_is_swath(path) && (haskey(kw, :geographic) || (kw[:geographic] = true))
	return cubegrid_all(D; kw...)
end

"""
    cubegrid(data; region, inc, kwargs...) -> GMTgrid

The depth grid of [`cubegrid_all`](@ref), same arguments.
"""
cubegrid(x, y, z; kwargs...) = cubegrid_all(x, y, z; kwargs...).depth
cubegrid(data; kwargs...) = cubegrid_all(data; kwargs...).depth

function _cubegrid(x::Vector{Float64}, y::Vector{Float64}, z::Vector{Float64}, data;
                   region = nothing, inc = nothing, iho_order = :order1a, method = :local,
                   tvu = nothing, thu = nothing, variance = :cube, noqueue = false, paramfile = "",
                   zdown = false, geographic = nothing, registration = :gridline, verbose = false)
	region === nothing && error("cubegrid needs a region")
	inc    === nothing && error("cubegrid needs an inc")
	length(x) == length(y) == length(z) ||
		error("x, y and z have different lengths ($(length(x)), $(length(y)), $(length(z)))")

	(w, e, s, n) = _mb_region(region)
	(dx, dy)     = _mb_inc(inc)
	reg = _mb_reg(registration)
	iho = _cube_enum(:mb_cube_iho_from_name, iho_order, "IHO order")
	mth = _cube_enum(:mb_cube_method_from_name, method, "method")
	var = _cube_enum(:mb_cube_variance_from_name, variance, "variance selection")
	geog = _cube_geog(geographic, data)
	down = _mb_bool(zdown)

	# Node counts: the SAME function mbgrid uses (mbgrid_dims) — one quantity, one function.
	pd = _MBParams(w, e, s, n, dx, dy, 1.0, 0.0, 0.0, Cint(0), Cint(0), _MB_INTERP[:none], Cint(0), Cint(0), reg)
	nxr, nyr = Ref{Cint}(0), Ref{Cint}(0)
	_mb_check(ccall(_fn(:mbgrid_dims), Cint, (Ref{_MBParams}, Ref{Cint}, Ref{Cint}), pd, nxr, nyr))
	nx, ny = Int(nxr[]), Int(nyr[])

	# First node (south-west) and the metric frame CUBE runs in: x, y relative to that node.
	x0 = reg == 1 ? w + dx / 2 : w
	y0 = reg == 1 ? s + dy / 2 : s
	(mx, my) = geog ? _cube_m_per_deg((w + e) / 2, (s + n) / 2) : (1.0, 1.0)
	cdx, cdy = dx * mx, dy * my

	p = _CubeParams()
	ccall(_fn(:mb_cube_params_default), Cvoid, (Ref{_CubeParams},), p)
	pf = String(strip(string(paramfile)))
	if !isempty(pf)
		isfile(pf) || error("CUBE parameter file not found: $pf")
		_cube_check(ccall(_fn(:mb_cube_params_read), Cint, (Ref{_CubeParams}, Cstring, Ptr{Bool}), p, pf, C_NULL),
		            "reading $pf")
	end
	p.variance_selection = var
	_cube_check(ccall(_fn(:mb_cube_params_initialize), Cint, (Ref{_CubeParams}, Cint, Cdouble, Cdouble),
	                  p, iho, cdx, cdy), "node spacing $cdx x $cdy m")

	# Sounding uncertainty, S-44 form at 95%, handed to CUBE as variances.
	ta, tb = Ref{Cdouble}(0), Ref{Cdouble}(0)
	ha, hb = Ref{Cdouble}(0), Ref{Cdouble}(0)
	ccall(_fn(:mb_cube_iho_limits), Cint, (Cint, Ref{Cdouble}, Ref{Cdouble}), iho, ta, tb)
	ccall(_fn(:mb_cube_iho_thu_limits), Cint, (Cint, Ref{Cdouble}, Ref{Cdouble}), iho, ha, hb)
	tv = _cube_pair(tvu); th = _cube_pair(thu)
	(tva, tvb) = tv === nothing ? (ta[], tb[]) : tv
	(tha, thb) = th === nothing ? (ha[], hb[]) : th
	conf = p.stddev_to_conf_scale

	depth = down ? z : -z
	tvar = [(sqrt(tva^2 + (tvb * d)^2) / conf)^2 for d in depth]
	hvar = [((tha + thb * abs(d)) / conf)^2 for d in depth]
	xm = (x .- x0) .* mx
	ym = (y .- y0) .* my

	g = ccall(_fn(:mb_cube_grid_new), Ptr{Cvoid},
	          (Cdouble, Cdouble, Cint, Cint, Cdouble, Cdouble, Ref{_CubeParams}, Bool, Ptr{Cchar}, Bool),
	          -cdx / 2, (ny - 0.5) * cdy, Cint(nx), Cint(ny), cdx, cdy, p, !_mb_bool(noqueue), C_NULL,
	          _mb_bool(verbose))
	g == C_NULL && error("CUBE: could not allocate a $nx x $ny grid")
	zd = Matrix{Float32}(undef, ny, nx); zu = similar(zd); zr = similar(zd); zh = similar(zd); zn = similar(zd)
	try
		_cube_check(ccall(_fn(:mb_cube_grid_insert), Cint,
		                  (Ptr{Cvoid}, Csize_t, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}),
		                  g, length(depth), depth, hvar, tvar, xm, ym), "inserting soundings")
		# the median pre-filter queues must be flushed before any depth is extracted
		ccall(_fn(:mb_cube_grid_flush), Cvoid, (Ptr{Cvoid},), g)
		_cube_check(ccall(_fn(:mb_cube_grid_get_values), Cint,
		                  (Ptr{Cvoid}, Cint, Ptr{Cfloat}, Ptr{Cfloat}, Ptr{Cfloat}, Ptr{Cfloat}, Ptr{Cfloat}, Cint),
		                  g, mth, zd, zu, zr, zh, zn, _CUBE_LAYOUT_COLS_SOUTH), "extracting the grid")
	finally
		gr = Ref(g)
		ccall(_fn(:mb_cube_grid_free), Cvoid, (Ref{Ptr{Cvoid}},), gr)
	end
	down || (zd .= .-zd)                       # back to the input's own z convention (NaN stays NaN)

	# GMTgrid's coordinate convention (as mbgrid.jl): nx node centres for gridline, nx+1 cell edges for pixel.
	xv = collect(range(w, step = dx, length = nx + Int(reg)))
	yv = collect(range(s, step = dy, length = ny + Int(reg)))
	cmd = "InteractiveGMT cubegrid region=$w/$e/$s/$n inc=$dx/$dy iho_order=$(lowercase(string(iho_order))) " *
	      "method=$(lowercase(string(method))) tvu=$tva/$tvb thu=$tha/$thb variance=$(lowercase(string(variance)))"
	mk(Z) = GMT.mat2grid(Z; x = xv, y = yv, reg = Int(reg), cmd = cmd)
	nset = count(!isnan, zd)
	G = mk(zd)
	G.remark = "CUBE: $nset of $(nx * ny) nodes set from $(length(z)) soundings"
	return (depth = G, uncertainty = mk(zu), n_hypotheses = mk(zh), ratio = mk(zr), n_points = mk(zn))
end
