# lineops.jl — Tools > Vector Operations. Port of Mirone's src_figs/line_operations.m.
#
# Mirone's tool is a command line: pick one or more lines in the figure, type (or choose from the
# popup) one of ~20 commands, hit Apply. That command language is kept VERBATIM — same op names,
# same argument spellings, same defaults — so a Mirone user's muscle memory still works. What
# changed is the engine: nearly every operation Mirone had to carry its own code for is a GMT.jl
# call today, and none of that maths is re-implemented here:
#
#   buffer / closing      GMT.buffergeo (geodesic: circles at each vertex unioned — literally what
#                         Mirone's buffer_j does) or GDAL's planar GMT.buffer for cartesian data
#   polysimplify          GMT.gmtsimplify -T (Douglas-Peucker; km when the window is geographic)
#   polyunion/-intersect  GMT.polyunion / GMT.intersection / GMT.symdifference / GMT.difference
#   /-xor/-minus          (GEOS, in place of Mirone's PolygonClip mex)
#   stitch                the closest-endpoint cascade, ported (GMT's gmtconnect cannot be told
#                         which line to grow FROM, which is the whole point of Mirone's version)
#   toRidge               GMT.mapproject -L onto the ridge extracted by the PPA port below
#   thicken               grid spacing from the window's own grid
#
# What IS ported, because it is the algorithm and no GMT module does it:
#   bezier                the global degree-n Bernstein curve (GMT.bezier is a COMPOSED CUBIC —
#                         a different curve, so it cannot stand in for this one)
#   bspline               csaps, already ported once for Grid Tools > SDG — `_csaps_smooth` there is
#                         reused, never a second copy
#   cspline               utils/spline_interp.m (cardinal/Hermite spline)
#   self-crossings        utils/intersections.m
#   toRidge               mex/grdppa_m.c — the PPA (Profile Recognition and Polygonization) ridge
#                         extractor, ported in full below
#   delete / group /      list surgery on the window's own elements
#   stitch / GMT_DB
#
# The C++ dialog is LineOpsDialog (70_window.cpp, loads deps/ui/line_operations.ui); the "key=value"
# block it sends is described in 30_app.cpp (JuliaLineOpsFn).
#
# RESULT PLACEMENT follows Mirone op by op: the ADDITIVE ops (bezier, buffer, closing, splines,
# simplify, the booleans, pline, self-crossings, toRidge) leave the source alone and add a new named
# element; the CONSUMING ops (delete, group, line2patch, stitch) remove what they consumed. Vector
# results NEVER reframe the window (SACRED_LAW.md, "Vector-import-onto-existing-display law") — they
# land inside the axes that are already there, which is what `_add_dataset_to_scene` does.

# =================================================================================================
#  The window's line elements, and the three primitives every op is built from
# =================================================================================================

# One line element of the window as the C side reports it (gmtvtk_vector_names_h).
struct LineElem
	name::String
	kind::Int            # 0 = imported/plotted line overlay, 1 = drawn polygon/polyline
	closed::Bool
	npts::Int
	rgb::Tuple{Float64,Float64,Float64}
	width::Float64
	style::Int           # 0 solid, 1 dashed, 2 dotted
end

# Every LINE element in the window, whichever door it came in through — the Julia twin of the
# dialog's own list (LineOpsDialog::refillTargets). Ops that work on "all lines" (delete DUP,
# scale, self-crossings with nothing picked, GMT_DB) ask for this; ops that need a selection get
# their names from the dialog.
function _lop_elements(scene::Ptr{Cvoid})::Vector{LineElem}
	out = LineElem[]
	n = ccall(_fn(:gmtvtk_vector_names_h), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), scene, C_NULL, Cint(0))
	n <= 0 && return out
	buf = Vector{UInt8}(undef, n + 1)
	ccall(_fn(:gmtvtk_vector_names_h), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), scene, buf, Cint(n + 1))
	for ln in split(unsafe_string(pointer(buf)), '\n'; keepempty = false)
		f = split(ln, '\t')
		length(f) < 8 && continue
		push!(out, LineElem(String(f[1]), parse(Int, f[2]), f[3] == "1", parse(Int, f[4]),
		                    (parse(Float64, f[5]), parse(Float64, f[6]), parse(Float64, f[7])),
		                    parse(Float64, f[8]), length(f) >= 9 ? parse(Int, f[9]) : 0))
	end
	return out
end

# The vertices of ONE named element. `gmtvtk_serialize_vector_h` answers for a drawn polygon and for
# an imported line overlay alike, so this side never has to care which kind it is — the same reader
# the Euler tool uses, never a second one (SACRED_LAW: one operation, one function).
_lop_segments(scene::Ptr{Cvoid}, name::AbstractString) = _euler_segments(scene, name)

# The element as ONE matrix, segments separated by a NaN row — Mirone's own in-memory shape (its
# XData/YData carry the NaNs), which is what the ops that split/join on NaN expect.
function _lop_xy(scene::Ptr{Cvoid}, name::AbstractString)::Matrix{Float64}
	segs = _lop_segments(scene, name)
	isempty(segs) && return zeros(Float64, 0, 2)
	length(segs) == 1 && return segs[1]
	parts = Matrix{Float64}[]
	for (k, s) in enumerate(segs)
		k > 1 && push!(parts, [NaN NaN])
		push!(parts, s)
	end
	return vcat(parts...)
end

# Remove ONE named element — the same removal its own Scene Objects row performs.
function _lop_remove(scene::Ptr{Cvoid}, name::AbstractString)::Int
	return Int(ccall(_fn(:gmtvtk_remove_vector_h), Cint, (Ptr{Cvoid}, Cstring), scene, String(name)))
end

# Add a result. `M` is n×2 (NaN rows allowed: they become segment breaks). The pen is the window's
# default, exactly as Mirone plots with handles.lc / handles.lt.
function _lop_add(scene::Ptr{Cvoid}, M::Matrix{Float64}, name::AbstractString;
                  group::AbstractString = "", color = nothing)::Bool
	D = _lop_mat2ds(M)
	D === nothing && return false
	return _add_dataset_to_scene(scene, D, String(name); groupName = String(group), color = color,
	                             forceMode = :lines)
end

# NaN-broken matrix -> one GMTdataset per segment (a single segment stays a plain GMTdataset).
function _lop_mat2ds(M::Matrix{Float64})
	size(M, 1) == 0 && return nothing
	segs = Matrix{Float64}[]
	i = 1
	n = size(M, 1)
	while i <= n
		while i <= n && (isnan(M[i, 1]) || isnan(M[i, 2]));  i += 1;  end
		j = i
		while j <= n && !isnan(M[j, 1]) && !isnan(M[j, 2]);  j += 1;  end
		j - i >= 2 && push!(segs, Matrix{Float64}(M[i:j-1, 1:2]))
		i = j
	end
	isempty(segs) && return nothing
	length(segs) == 1 && return GMT.mat2ds(segs[1])
	return [GMT.mat2ds(s) for s in segs]
end

# Is this window geographic? Mirone reads handles.geog; here the window's own CRS answers, with
# GMT's coordinate-range guess as the fallback for an unreferenced window (same two-step
# `_measure_isgeog` uses, so the tools cannot disagree about what "geographic" means).
function _lop_isgeog(scene::Ptr{Cvoid}, M::Matrix{Float64} = zeros(0, 2))::Bool
	proj4 = try _window_crs(scene).proj4 catch; "" end
	isempty(proj4) || return occursin("longlat", proj4) || occursin("latlong", proj4)
	size(M, 1) == 0 && return false
	return try GMT.guessgeog(GMT.mat2ds(M)) catch; false end
end

# =================================================================================================
#  Command parsing — Mirone's own spellings
# =================================================================================================

const _LOP_OPS = ["bezier", "buffer", "bspline", "closing", "cspline", "delete", "group",
                  "line2patch", "polysimplify", "polyunion", "polyintersect", "polyxor",
                  "polyminus", "pline", "scale", "stitch", "thicken", "toRidge", "self-crossings",
                  "hand2Workspace", "GMT_DB"]

# Ops that do NOT need a line picked first (Mirone's own exception list in push_apply_CB).
const _LOP_NOPICK = ["pline", "scale", "GMT_DB", "self-crossings", "delete"]

# "10K" / "250M" / "3N" -> metres. A bare number is DEGREES of arc, Mirone's default for a
# geographic window (and plain map units for a cartesian one).
function _lop_dist(tok::AbstractString, geog::Bool)::Tuple{Float64,Bool}
	s = String(strip(tok))
	isempty(s) && error("this operation needs a distance")
	fac = 0.0
	if geog && length(s) > 1
		c = lowercase(s[end])
		c == 'm' && (fac = 1.0)
		c == 'k' && (fac = 1000.0)
		c == 'n' && (fac = 1852.0)
		fac != 0.0 && (s = s[1:end-1])
	end
	v = tryparse(Float64, s)
	v === nothing && error("\"$tok\" is not a distance")
	v = abs(v)
	# metres given -> metres; a bare number in a geographic window is degrees of arc, converted with
	# the authalic radius Mirone uses (s = R*theta).
	return fac != 0.0 ? (v * fac, true) : (v, false)
end

const _LOP_AUTHALIC = 6371005.076
_lop_deg2m(d::Float64) = d * pi / 180 * _LOP_AUTHALIC
_lop_m2deg(m::Float64) = m / _LOP_AUTHALIC * 180 / pi

# =================================================================================================
#  The operations
# =================================================================================================

# --- bezier N ------------------------------------------------------------------------------------
# Mirone's is the GLOBAL Bezier: every vertex of the polyline is a control point of one curve of
# degree n-1 (Bernstein basis), so the curve passes only through the first and last. GMT.bezier
# composes CUBIC curves through the control points — a different curve, which is why the Bernstein
# maths (bezier_.m, by way of local_nchoosek) is ported rather than substituted.
function _lop_nchoosek(n::Int, k::Int)::Float64
	k == 0 && return 1.0
	n == 0 && return 0.0
	r = 1.0
	nk = n - k
	for i in 1:k;  r *= (nk + i) / i;  end
	return r
end

function _lop_bezier(xy::Matrix{Float64}, nnodes::Int)::Matrix{Float64}
	np = size(xy, 1)
	np < 2 && error("bezier needs at least two vertices")
	n = np - 1
	bez = zeros(Float64, nnodes, 2)
	binom = [_lop_nchoosek(n, i) for i in 0:n]
	for (row, t) in enumerate(range(0.0, 1.0; length = nnodes))
		sx = 0.0;  sy = 0.0
		for i in 0:n
			b = binom[i+1] * t^i * (1 - t)^(n - i)
			sx += xy[i+1, 1] * b;  sy += xy[i+1, 2] * b
		end
		bez[row, 1] = sx;  bez[row, 2] = sy
	end
	return bez
end

# --- buffer / closing ----------------------------------------------------------------------------
# Mirone's buffer_j builds the buffer by unioning a circle around every vertex — which is exactly
# what GMT.buffergeo does (circgeo at each vertex, polyunion), on a real ellipsoid via GeographicLib.
# So the geodesic buffer IS buffergeo; only the knobs are translated:
#   NPTS  -> np           BASE=0 -> flatstart      TOP=0 -> flatend
#   in|out -> clipped against the source ring afterwards (a buffer "in"/"out" of a CLOSED polygon is
#            the buffer minus / intersected with the polygon — the same two GEOS ops the boolean
#            operations below use, never a second buffering pass)
#   SIDE=left|right -> half_buffer, ported: cut the buffer ring at the two points nearest the seed
#            line's own endpoints (GMT's mapproject -L answers where) and keep one half.
# Cartesian data has no geodesic buffer to speak of: GDAL's planar OGR_G_Buffer (GMT.buffer) is the
# same construction in the plane, and takes a SIGNED width, which is how 'in' is done there.
struct BufferOpts
	dist::Float64        # metres when geog, map units otherwise
	dir::String          # "in" | "out" | "both"
	npts::Int
	side::Char           # 'l' | 'r' | 'b'
	base::Bool           # false -> flat start
	top::Bool            # false -> flat end
end

function _lop_buffer_args(rest::AbstractString, geog::Bool, defdir::String)::BufferOpts
	toks = split(strip(rest), r"\s+"; keepempty = false)
	isempty(toks) && error("the argument \"DIST\" must be replaced by the width of the buffer zone")
	toks[1] == "DIST" && error("the argument \"DIST\" must be replaced by a numeric value representing the width of the buffer zone")
	d, inmetres = _lop_dist(toks[1], geog)
	dist = (geog && !inmetres) ? _lop_deg2m(d) : d
	dir = defdir;  npts = 13;  side = 'b';  base = true;  top = true
	for t in toks[2:end]
		tl = lowercase(String(t))
		if     tl == "in" || tl == "out" || tl == "both";  dir = tl
		elseif startswith(tl, "side=")
			c = length(tl) > 5 ? tl[6] : 'b'
			side = (c == 'l' || c == 'r') ? c : 'b'
		elseif startswith(tl, "base=");  base = !endswith(tl, "0")
		elseif startswith(tl, "top=");   top  = !endswith(tl, "0")
		elseif startswith(tl, "npts=")   # spelled-out form; Mirone takes a bare number, both work
			v = tryparse(Int, tl[6:end])
			v !== nothing && v > 2 && (npts = v)
		elseif tl == "geod"              # WGS84 — buffergeo is ellipsoidal already, so this is a no-op
		else
			v = tryparse(Int, tl)
			v !== nothing && v > 2 && (npts = v)
		end
	end
	return BufferOpts(dist, dir, npts, side, base, top)
end

# One line -> its buffer, as a NaN-broken matrix. `geog` picks the geodesic engine over the planar one.
function _lop_buffer(xy::Matrix{Float64}, o::BufferOpts, geog::Bool)::Matrix{Float64}
	size(xy, 1) < 2 && error("buffer needs a line with at least two vertices")
	closedring = xy[1, 1] == xy[end, 1] && xy[1, 2] == xy[end, 2]
	B = if geog
		GMT.buffergeo(xy; width = o.dist, unit = :m, np = o.npts,
		              flatstart = !o.base, flatend = !o.top)
	else
		# GDAL's planar buffer. 'in' on a closed ring is a NEGATIVE width, which is the planar
		# equivalent of the clip below and cheaper, so it is taken here.
		w = (o.dir == "in" && closedring) ? -o.dist : o.dist
		GMT.buffer(GMT.mat2ds(xy), w, o.npts)
	end
	M = _lop_ds2mat(B)
	# Direction, for the geodesic branch and for 'out' in both: the buffer of a CLOSED ring minus /
	# intersected with the ring itself. GEOS does the clipping — the same ops polyunion & co. use.
	if closedring && o.dir != "both" && !(o.dir == "in" && !geog)
		src = GMT.mat2ds(xy)
		R = o.dir == "in" ? GMT.intersection(GMT.mat2ds(M), src) : GMT.difference(GMT.mat2ds(M), src)
		M = _lop_ds2mat(R)
	end
	# One-sided buffer (half_buffer): only meaningful for an OPEN line.
	(o.side != 'b' && !closedring) && (M = _lop_half_buffer(M, xy, o.side))
	return M
end

# half_buffer, ported. The buffer of an open line is one ring that wraps it; the two points of that
# ring nearest the seed's own endpoints cut it into the left and the right half. GMT's
# `mapproject -L` reports, for each endpoint, the nearest point on the ring AND its fractional
# index — so the cut indices come from GMT's geodesic nearest-point search, never a hand-rolled one.
function _lop_half_buffer(ring::Matrix{Float64}, seed::Matrix{Float64}, side::Char)::Matrix{Float64}
	size(ring, 1) < 4 && return ring
	ends = [seed[1, 1] seed[1, 2]; seed[end, 1] seed[end, 2]]
	i1, i2, flipped = _lop_nearest_idx(ring, ends)
	(i1 == 0 || i2 == 0 || i1 == i2) && return ring
	want = side
	if flipped                                  # the ring runs the other way: left and right swap
		want = side == 'l' ? 'r' : 'l'
	end
	a, b = minmax(i1, i2)
	first_half  = ring[a:b, :]
	second_half = vcat(ring[b:end, :], ring[1:a, :])
	return want == 'l' ? first_half : second_half
end

# Fractional vertex index on `ring` closest to each of the two points of `pts` (mapproject -L+p
# reports it in the last column). `flipped` is Mirone's own heuristic: indices out of order means
# the ring was traced the other way round.
function _lop_nearest_idx(ring::Matrix{Float64}, pts::Matrix{Float64})
	tmp = joinpath(tempdir(), "igmt_lop_ring_$(getpid())_$(round(Int, time()*1000)).txt")
	try
		open(tmp, "w") do io
			for i in 1:size(ring, 1);  println(io, ring[i, 1], "\t", ring[i, 2]);  end
		end
		R = GMT.gmt("mapproject -L$tmp+p", pts)
		D = isa(R, Vector) ? R[1].data : R.data
		size(D, 1) < 2 && return (0, 0, false)
		i1 = floor(Int, D[1, end]) + 1
		i2 = ceil(Int, D[2, end]) + 1
		return i1 > i2 ? (i2, i1, true) : (i1, i2, false)
	catch
		return (0, 0, false)
	finally
		isfile(tmp) && (try rm(tmp) catch end)
	end
end

# A GMTdataset (or a vector of them) -> one NaN-broken matrix.
function _lop_ds2mat(D)::Matrix{Float64}
	D === nothing && return zeros(Float64, 0, 2)
	if isa(D, AbstractVector)
		parts = Matrix{Float64}[]
		for (k, s) in enumerate(D)
			k > 1 && push!(parts, [NaN NaN])
			push!(parts, Matrix{Float64}(s.data[:, 1:2]))
		end
		return isempty(parts) ? zeros(Float64, 0, 2) : vcat(parts...)
	end
	isa(D, GMT.GMTdataset) && return Matrix{Float64}(D.data[:, 1:2])
	isa(D, AbstractMatrix) && return Matrix{Float64}(D[:, 1:2])
	return zeros(Float64, 0, 2)
end

# --- cspline (utils/spline_interp.m, cardinal spline) --------------------------------------------
# Tangencies from the tridiagonal (1,4,1) system with zero-curvature ends, then the cubic Hermite
# basis evaluated at N subdivisions per interval. Ported literally; the only change is that the
# sparse solve becomes a Thomas sweep (the matrix IS tridiagonal — same solution, no allocation).
function _lop_spline_interp(x::Vector{Float64}, y::Vector{Float64}, nsub::Int)
	n = length(x)
	n <= 2 && return (copy(x), copy(y))
	nsub <= 1 && return (copy(x), copy(y))
	# Right-hand side: -3*(P(1:n-2) - P(3:n)) with the two boundary rows prepended/appended.
	bx = Vector{Float64}(undef, n);  by = Vector{Float64}(undef, n)
	bx[1] = 6 * (x[2] - x[1]);       by[1] = 6 * (y[2] - y[1])
	for i in 2:n-1
		bx[i] = -3 * (x[i-1] - x[i+1]);  by[i] = -3 * (y[i-1] - y[i+1])
	end
	bx[n] = 6 * (x[n] - x[n-1]);     by[n] = 6 * (y[n] - y[n-1])
	# TM: (1,4,1) tridiagonal, first row [4 2 0], last row [0 2 4].
	lo = fill(1.0, n);  di = fill(4.0, n);  up = fill(1.0, n)
	up[1] = 2.0;  lo[n] = 2.0
	tx = _lop_thomas(lo, di, up, bx)
	ty = _lop_thomas(lo, di, up, by)
	uu = collect(range(0.0, 1.0; length = nsub + 1))
	nout = (nsub + 1) * (n - 1)
	ox = Vector{Float64}(undef, nout);  oy = Vector{Float64}(undef, nout)
	k = 0
	for q in 1:n-1
		for u in uu
			h1 = (1 - 3u^2) + 2u^3;  h2 = 3u^2 - 2u^3
			h3 = u - 2u^2 + u^3;     h4 = u^3 - u^2
			k += 1
			ox[k] = x[q]*h1 + x[q+1]*h2 + tx[q]*h3 + tx[q+1]*h4
			oy[k] = y[q]*h1 + y[q+1]*h2 + ty[q]*h3 + ty[q+1]*h4
		end
	end
	# Drop the duplicated knots the per-interval evaluation leaves behind (Mirone's own last step).
	keep = trues(k)
	for i in 2:k
		(ox[i] == ox[i-1] && oy[i] == oy[i-1]) && (keep[i] = false)
	end
	return (ox[keep], oy[keep])
end

# Thomas algorithm for a tridiagonal system (lo/di/up are the sub-, main and super-diagonal, with
# lo[1] and up[n] ignored). Destroys copies only.
function _lop_thomas(lo::Vector{Float64}, di::Vector{Float64}, up::Vector{Float64}, b::Vector{Float64})
	n = length(b)
	c = copy(up);  d = copy(b);  m = copy(di)
	for i in 2:n
		w = lo[i] / m[i-1]
		m[i] -= w * c[i-1]
		d[i] -= w * d[i-1]
	end
	xs = Vector{Float64}(undef, n)
	xs[n] = d[n] / m[n]
	for i in n-1:-1:1;  xs[i] = (d[i] - c[i] * xs[i+1]) / m[i];  end
	return xs
end

# --- self-crossings (utils/intersections.m) -------------------------------------------------------
# Segment-pair intersections with a bounding-box prefilter, collinear overlaps reported at the middle
# of the overlapping stretch. Returns the crossing points and their fractional index along the line
# (which is what lets the caller drop the annoying false positive at index ~1).
function _lop_intersections(x::Vector{Float64}, y::Vector{Float64})
	n = length(x) - 1
	xc = Float64[];  yc = Float64[];  ic = Float64[]
	n < 2 && return (xc, yc, ic)
	dx = diff(x);  dy = diff(y)
	# A CLOSED ring's last segment ends on the first one's start: that shared vertex is not a
	# crossing. (Mirone drops it after the fact, by throwing away a first hit whose fractional index
	# is ~1; skipping the pair outright is the same intent and does not depend on the pair order.)
	ring = x[1] == x[end] && y[1] == y[end]
	for i in 1:n
		(isnan(dx[i]) || isnan(dy[i])) && continue
		xi0, xi1 = minmax(x[i], x[i+1]);  yi0, yi1 = minmax(y[i], y[i+1])
		for j in i+2:n                                   # j <= i+1 are neighbours, never crossings
			(ring && i == 1 && j == n) && continue
			(isnan(dx[j]) || isnan(dy[j])) && continue
			xj0, xj1 = minmax(x[j], x[j+1]);  yj0, yj1 = minmax(y[j], y[j+1])
			(xi0 > xj1 || xi1 < xj0 || yi0 > yj1 || yi1 < yj0) && continue
			den = dx[i] * dy[j] - dy[i] * dx[j]
			rx = x[j] - x[i];  ry = y[j] - y[i]
			if abs(den) < eps(Float64) * max(1.0, abs(dx[i]) + abs(dy[i]))
				# Parallel. Collinear (cross product ~ 0) counts as an overlap, reported at the
				# middle of the common stretch — intersections.m's own convention.
				abs(rx * dy[i] - ry * dx[i]) > 1e-12 * max(1.0, abs(dx[i]) + abs(dy[i])) && continue
				push!(xc, (max(xi0, xj0) + min(xi1, xj1)) / 2)
				push!(yc, (max(yi0, yj0) + min(yi1, yj1)) / 2)
				push!(ic, Float64(i))
				continue
			end
			t1 = (rx * dy[j] - ry * dx[j]) / den
			t2 = (rx * dy[i] - ry * dx[i]) / den
			(t1 < 0 || t1 > 1 || t2 < 0 || t2 > 1) && continue
			push!(xc, x[i] + t1 * dx[i])
			push!(yc, y[i] + t1 * dy[i])
			push!(ic, i + t1)
		end
	end
	return (xc, yc, ic)
end

# --- stitch (find_closestline + do_stitching) -----------------------------------------------------
# Which of `others` has an endpoint within TOL of `me`'s endpoints, and how the two must be joined.
# endType 1..4 exactly as Mirone numbers them.
function _lop_closest(me::Matrix{Float64}, others::Vector{Matrix{Float64}}, tol::Float64)
	best = 0;  bestType = 0;  mind = tol * tol
	x1a, y1a = me[1, 1], me[1, 2]
	x1b, y1b = me[end, 1], me[end, 2]
	for (k, o) in enumerate(others)
		size(o, 1) < 2 && continue
		(o[1, 1] == o[end, 1] && o[1, 2] == o[end, 2]) && continue     # closed polygons are skipped
		d = ((x1a - o[1, 1])^2 + (y1a - o[1, 2])^2,
		     (x1a - o[end, 1])^2 + (y1a - o[end, 2])^2,
		     (x1b - o[1, 1])^2 + (y1b - o[1, 2])^2,
		     (x1b - o[end, 1])^2 + (y1b - o[end, 2])^2)
		m, i = findmin(d)
		if m <= mind
			mind = m;  best = k;  bestType = i
			m < 1e-8 && break
		end
	end
	return (best, bestType)
end

function _lop_join(a::Matrix{Float64}, b::Matrix{Float64}, endType::Int)::Matrix{Float64}
	endType == 1 && return vcat(reverse(b, dims = 1), a)   # both grow away from a shared mid point
	endType == 2 && return vcat(b, a)                      # b ends near a's start
	endType == 3 && return vcat(a, b)                      # a ends near b's start
	return vcat(a, reverse(b, dims = 1))                   # both grow towards a shared mid point
end

# The tail of do_stitching: drop repeated points, undo the "knee" an OGR double-traced segment
# leaves, and close the line when its two ends fall within TOL.
function _lop_stitch_cleanup(M::Matrix{Float64}, tol::Float64)::Matrix{Float64}
	size(M, 1) < 2 && return M
	keep = trues(size(M, 1))
	for i in 2:size(M, 1)
		(M[i, 1] == M[i-1, 1] && M[i, 2] == M[i-1, 2]) && (keep[i] = false)
	end
	M = M[keep, :]
	knees = Int[]
	for k in 1:size(M, 1)-2
		(M[k, 1] == M[k+2, 1] && M[k, 2] == M[k+2, 2]) && push!(knees, k)
	end
	if length(knees) == 2
		cut = collect(knees[1]+1:knees[2]+1)
		M = M[setdiff(1:size(M, 1), cut), :]
	end
	if size(M, 1) > 2 && (M[1, 1] != M[end, 1] || M[1, 2] != M[end, 2]) &&
	   hypot(M[1, 1] - M[end, 1], M[1, 2] - M[end, 2]) <= tol
		M = vcat(M, M[1:1, :])
	end
	return M
end

# stitch SORT: re-order a polyline's vertices by proximity (Mirone's sortline / do_sortline), up to
# five passes over what the previous pass could not reach.
function _lop_sortline(M::Matrix{Float64})::Matrix{Float64}
	pts = copy(M)
	out = Matrix{Float64}[]
	count = 0
	while size(pts, 1) > 0 && count < 6
		idx = _lop_sort_pass(pts)
		isempty(idx) && break
		push!(out, pts[idx, :])
		pts = pts[setdiff(1:size(pts, 1), idx), :]
		count += 1
	end
	return isempty(out) ? M : vcat(out...)
end

function _lop_sort_pass(P::Matrix{Float64})::Vector{Int}
	n = size(P, 1)
	n == 0 && return Int[]
	idx = Int[1]
	k = 1
	while k < n
		best = 0;  bd = Inf
		for j in k+1:n
			d = (P[j, 1] - P[k, 1])^2 + (P[j, 2] - P[k, 2])^2
			d < bd && (bd = d; best = j)
		end
		best == 0 && break
		push!(idx, best)
		k = best
	end
	return idx
end

# --- thicken -------------------------------------------------------------------------------------
# The N+1 parallel lines a thickened track carries, so that "Extract profile" can sample all of them
# and stack the result. Keyed by (window, element name) — the profile path looks the answer up by the
# name the user right-clicked, so a thickened line and an ordinary one go down the SAME code path
# (measure.jl's `_extract_profile`), the thickened one simply finding something here.
const _LOP_THICK = Dict{Tuple{Ptr{Cvoid},String}, Vector{Matrix{Float64}}}()

# One line -> the N+1 parallel copies, `thick` map units apart in total, offset perpendicular to the
# FIRST segment's heading (Mirone's own approximation: a single rotation for the whole line, so the
# copies keep vertex-for-vertex correspondence and can be stacked).
function _lop_thicken_lines(xy::Matrix{Float64}, N::Int, thick::Float64, geog::Bool)
	np = size(xy, 1)
	np < 2 && error("thicken needs a line with at least two vertices")
	local co::Float64, si::Float64
	if geog
		# The heading of the first segment, from GMT's own geodesic inverse — never a hand-rolled
		# spherical azimuth (SACRED_LAW's "one quantity, one function").
		r = GMT.invgeod(Matrix{Float64}(xy[1:1, 1:2]), Matrix{Float64}(xy[2:2, 1:2]))
		az = r[2]
		a = (90 - (isa(az, AbstractArray) ? az[1] : az)) * pi / 180   # azimuth -> trigonometric angle
		co = cos(a);  si = sin(a)
	else
		dx = xy[2, 1] - xy[1, 1];  dy = xy[2, 2] - xy[1, 2]
		h = hypot(dx, dy);  h == 0 && (h = 1.0)
		co = dx / h;  si = dy / h
	end
	dl = thick / N
	out = Matrix{Float64}[]
	for k in 1:N+1
		th = thick / 2 - (k - 1) * dl
		ox = -si * th;  oy = co * th
		push!(out, hcat(xy[:, 1] .+ ox, xy[:, 2] .+ oy))
	end
	return out
end

# --- toRidge: the PPA ridge extractor (mex/grdppa_m.c), ported ------------------------------------
# "Program for automatic extraction of ridge and valley axes from the digital elevation data set":
# target recognition and connection (tgrcon), segment check-out by polygon breaking and branch
# reduction (segko), then line smoothing and output. Ported literally, including the 1-cell padding
# and the 1-based (i = column, j = row) indexing of the original — `Z` comes in with row 1 = SOUTH,
# which is the orientation the Fortran/C original assumes.
mutable struct PPAState
	ncols::Int
	nrows::Int
	ib::Int                        # -L: number of points for polygon recognition [3]
	data::Matrix{Float64}          # [j, i] = [row, col], padded by one cell all round
	w::Array{Float64,3}            # [i, j, k] connection weight
	v::Array{Int8,3}               # [i, j, k] connection status
end

const _PPA_NX = (0, 1, 1, 1, 0, -1, -1, -1)
const _PPA_NY = (1, 1, 0, -1, -1, -1, 0, 1)

_ppa_neb(i::Int) = i > 8 ? i - 8 : (i < 1 ? i + 8 : i)

# con(): read or write the status/weight of the connection leaving (i,j) in direction k. Directions
# 5..8 are stored on the NEIGHBOUR's slots 1..4 — one edge, one record.
function _ppa_con!(S::PPAState, i::Int, j::Int, k::Int, n::Int, jc::Int)
	k <= 0 && return (0, 0.0)
	ii = i;  jj = j;  kk = k
	if k > 4
		ii = i + _PPA_NX[k];  jj = j + _PPA_NY[k];  kk = k - 4
	end
	(ii < 1 || ii > S.ncols || jj < 1 || jj > S.nrows) && return (0, 0.0)
	if jc == 1
		return (Int(S.v[ii, jj, kk]), S.w[ii, jj, kk])
	end
	S.v[ii, jj, kk] = Int8(n)
	S.w[ii, jj, kk] = 2e3
	if n == 2
		S.w[ii, jj, kk] = S.data[j, i] + S.data[j + _PPA_NY[k], i + _PPA_NX[k]]
	end
	return (n, S.w[ii, jj, kk])
end

# kst(): the original's swiss-army routine over the connection tables. jc as documented in the C.
function _ppa_kst!(S::PPAState, i::Int, j::Int, k::Int, jc::Int)::Int
	if jc == 2
		_ppa_con!(S, i, j, k, 2, 2)
		k % 2 == 1 && return 0
		i2 = i + _PPA_NX[k-1];  j2 = j + _PPA_NY[k-1]
		(n, w2) = _ppa_con!(S, i2, j2, _ppa_neb(k + 2), 0, 1)
		n == 0 && return 0
		(_, w1) = _ppa_con!(S, i, j, k, 0, 1)
		w2 > w1 ? _ppa_con!(S, i, j, k, 0, 2) : _ppa_con!(S, i2, j2, _ppa_neb(k + 2), 0, 2)
		return 0
	end
	if abs(jc) == 8
		r = 0
		for m in 1:8
			(n, _) = _ppa_con!(S, i, j, m, 0, 1)
			jc ==  8 && n != 0 && (r += 1)
			jc == -8 && n == 2 && (r += 1)
		end
		return r
	end
	(n, _) = _ppa_con!(S, i, j, k, 0, 1)
	r = 0
	jc ==  1 && n != 0 && (r = 1)
	jc == -1 && n == 2 && (r = 1)
	jc ==  4 && _ppa_con!(S, i, j, k, 0, 2)
	jc == -4 && _ppa_con!(S, i, j, k, 1, 2)
	return r
end

# tgrcon(): recognise the targets along profiles in four directions, then connect them.
function _ppa_tgrcon!(S::PPAState)
	b = zeros(Int8, S.ncols, S.nrows)
	nn = zeros(Int, 8)
	for i in 3:S.ncols-2, j in 2:S.nrows-2
		for k in 1:8
			nn[k] = 0
			for ml in 1:(S.ib ÷ 2)
				ii = i + ml * _PPA_NX[k];  jj = j + ml * _PPA_NY[k]
				(ii < 2 || ii > S.ncols - 1 || jj < 2 || jj > S.nrows - 1) && continue
				S.data[jj, ii] < S.data[j, i] && (nn[k] = 1)
			end
		end
		for k in 1:4
			nn[k] + nn[k+4] > 1 && (b[i, j] = Int8(1))
		end
	end
	for i in 2:S.ncols-1, j in 2:S.nrows-1, k in 1:4
		_ppa_kst!(S, i, j, k, 4)
		i2 = i + _PPA_NX[k];  j2 = j + _PPA_NY[k]
		(i2 < 1 || i2 > S.ncols || j2 < 1 || j2 > S.nrows) && continue
		b[i, j] + b[i2, j2] == 2 && _ppa_kst!(S, i, j, k, 2)
	end
	return
end

# segko(): check out improper segments — polygon breaking, then branch reduction.
function _ppa_segko!(S::PPAState)
	b = zeros(Int8, S.ncols, S.nrows)
	guard = 0
	local ii::Int, jj::Int, kk::Int
	while true                                       # C's L1
		guard += 1
		guard > 4_000_000 && break                   # the original loops until nothing is left
		wn = 1001.0;  ii = 0;  jj = 0;  kk = 0
		for i in 2:S.ncols-1, j in 2:S.nrows-1
			b[i, j] == 1 && continue
			nv = 0
			for k in 1:4
				(_, v) = _ppa_con!(S, i, j, k, 0, 1)
				v == 2e3 && (nv += 1)
				v >= wn && continue
				# skip the end-segment
				if _ppa_kst!(S, i, j, k, -8) == 1 ||
				   _ppa_kst!(S, i + _PPA_NX[k], j + _PPA_NY[k], k, -8) == 1
					_ppa_kst!(S, i, j, k, -4)
					continue
				end
				wn = v;  ii = i;  jj = j;  kk = k
			end
			nv == 4 && (b[i, j] = Int8(1))
		end
		wn == 1001.0 && break                        # -> L4
		# polygon tracing
		_ppa_kst!(S, ii, jj, kk, -4)
		_ppa_kst!(S, ii, jj, kk, -8) == 0 && continue
		inn = ii + _PPA_NX[kk];  jn = jj + _PPA_NY[kk]
		_ppa_kst!(S, inn, jn, kk, -8) == 0 && continue
		id = 1
		traced = false
		while true                                   # C's L6
			i = inn;  j = jn;  k = kk
			closedloop = false
			steps = 0
			while true                               # C's L8 / L11
				steps += 1
				steps > 8 * S.ncols * S.nrows && (closedloop = true; break)
				k = _ppa_neb(k + 4)
				while true
					k = _ppa_neb(k + id)
					_ppa_kst!(S, i, j, k, -1) != 0 && break
				end
				i += _PPA_NX[k];  j += _PPA_NY[k]
				(i < 1 || i > S.ncols || j < 1 || j > S.nrows) && (closedloop = true; break)
				if i == ii && j == jj
					_ppa_kst!(S, ii, jj, kk, 4)
					closedloop = true;  break
				end
				if i == inn && j == jn
					if id == -1;  closedloop = true;  break;  end
					id = -1
					break                            # -> L6 with id = -1
				end
			end
			closedloop && (traced = true; break)
		end
		traced && continue
	end
	# branch reduction (C's L4)
	for _ in 1:(S.ib ÷ 2)
		for i in 2:S.ncols-1, j in 2:S.nrows-1
			b[i, j] = Int8(0)
			_ppa_kst!(S, i, j, 1, 8) != 1 && continue
			for k in 1:8
				_ppa_kst!(S, i, j, k, 1) == 1 && (b[i, j] = Int8(k))
			end
		end
		for i in 2:S.ncols-1, j in 2:S.nrows-1
			_ppa_kst!(S, i, j, Int(b[i, j]), 4)
		end
	end
	return
end

# smooth(): the target's position, weighted by the connected neighbours.
function _ppa_smooth(S::PPAState, i::Int, j::Int, xmin::Float64, ymin::Float64,
                     xinc::Float64, yinc::Float64)
	w = S.data[j, i];  x = 0.0;  y = 0.0
	for k in 1:8
		f = Float64(_ppa_kst!(S, i, j, k, 1))
		i2 = i + _PPA_NX[k];  j2 = j + _PPA_NY[k]
		(i2 < 1 || i2 > S.ncols || j2 < 1 || j2 > S.nrows) && continue
		d = S.data[j2, i2]
		w += d * f
		x += _PPA_NX[k] * d * f
		y += _PPA_NY[k] * d * f
	end
	w == 0 && (w = 1.0)
	return (xmin + ((i - 2) + x / w) * xinc, ymin + ((j - 2) + y / w) * yinc)
end

"""
    _ppa_ridges(Z, xmin, ymin, xinc, yinc; npoints=3) -> Matrix{Float64}

Ridge axes of the grid `Z` (row 1 = SOUTH), as an n×2 matrix of NaN-broken polylines. Port of
Mirone's `grdppa_m` mex (the PPA algorithm); `npoints` is its `-L` option.
"""
function _ppa_ridges(Z::AbstractMatrix{<:Real}, xmin::Float64, ymin::Float64,
                     xinc::Float64, yinc::Float64; npoints::Int = 3)::Matrix{Float64}
	ny, nx = size(Z)
	(ny < 3 || nx < 3) && return zeros(Float64, 0, 2)
	ncols = nx + 2;  nrows = ny + 2
	data = zeros(Float64, nrows, ncols)
	for r in 1:ny, c in 1:nx
		z = Float64(Z[r, c])
		data[r+1, c+1] = isnan(z) ? 0.0 : z
	end
	S = PPAState(ncols, nrows, max(npoints, 2), data,
	             zeros(Float64, ncols, nrows, 4), zeros(Int8, ncols, nrows, 4))
	_ppa_tgrcon!(S)
	_ppa_segko!(S)
	out = Float64[]
	first = true
	xsave = 0.0;  ysave = 0.0
	for i in 2:ncols-1, j in 2:nrows-1, k in 1:4
		_ppa_kst!(S, i, j, k, 1) == 0 && continue
		(x, y)   = _ppa_smooth(S, i, j, xmin, ymin, xinc, yinc)
		(x1, y1) = _ppa_smooth(S, i + _PPA_NX[k], j + _PPA_NY[k], xmin, ymin, xinc, yinc)
		if !first && abs(x - xsave) < 1e-5 && abs(y - ysave) < 1e-5
			append!(out, (x1, y1))
		elseif !first && abs(x1 - xsave) < 1e-5 && abs(y1 - ysave) < 1e-5
			append!(out, (x, y))
		else
			append!(out, (NaN, NaN, x, y, x1, y1))
		end
		xsave = x1;  ysave = y1
		first = false
	end
	isempty(out) && return zeros(Float64, 0, 2)
	return permutedims(reshape(out, 2, :))
end

# =================================================================================================
#  The command dispatcher — one function per Mirone `case`
# =================================================================================================

# The last thing the tool has to say, kept Julia-side too (the C channel is write-only from here and
# the tests need to see it).
const _lineops_last_result = Ref{String}("")

function _lineops_result(txt::AbstractString)
	_lineops_last_result[] = String(txt)
	try
		ccall(_fn(:gmtvtk_lineops_result), Cvoid, (Cstring,), String(txt))
	catch
	end
	return
end

function _lop_targets(d::Dict{String,String})::Vector{String}
	t = String[]
	one = _get(d, "target")
	isempty(one) || push!(t, one)
	i = 1
	while true
		v = _get(d, "target$i")
		isempty(v) && break
		push!(t, v)
		i += 1
	end
	return t
end

# --- bezier / bspline / cspline / polysimplify: one new line per selected line --------------------
function _lop_do_bezier(scene, targets, rest)
	nn = 100
	s = strip(rest)
	if !isempty(s) && uppercase(s) != "N"
		v = tryparse(Float64, split(s)[1])
		v !== nothing && (nn = max(round(Int, abs(v)), 2))
	end
	n = 0
	for t in targets
		xy = _lop_xy(scene, t)
		size(xy, 1) < 2 && continue
		_lop_add(scene, _lop_bezier(xy, nn), "$t bezier"; group = "bezier") && (n += 1)
	end
	n == 0 && error("nothing to fit a Bezier curve to")
	return "bezier: $n curve(s) added"
end

# bspline: the cubic SMOOTHING spline (csaps) with the parameter p. Mirone opens a slider window to
# hunt for a nice p; here the default is csaps's OWN estimate and an explicit p can be given
# ("bspline 0.001") to try another. The spline itself is `_csaps_smooth` — the port already made for
# Grid Tools > SDG, reused, never copied (SACRED_LAW: one operation, one function).
function _lop_do_bspline(scene, targets, rest)
	pgiven = nothing
	s = strip(rest)
	if !isempty(s)
		v = tryparse(Float64, split(s)[1])
		v !== nothing && (pgiven = clamp(v, 0.0, 1.0))
	end
	n = 0
	msgs = String[]
	for t in targets
		xy = _lop_xy(scene, t)
		size(xy, 1) < 4 && continue
		x = Float64.(xy[:, 1]);  y = Float64.(xy[:, 2])
		# csaps is a spline y(x): it needs strictly increasing abscissae, which a polyline that
		# doubles back does not have. Say so instead of returning nonsense.
		if any(diff(x) .<= 0)
			push!(msgs, "$t: x is not strictly increasing (bspline fits y(x))")
			continue
		end
		p = pgiven === nothing ? csaps_p_guess(x) : pgiven
		ys = csaps_nodes(x, reshape(y, :, 1), p)[1][:, 1]
		_lop_add(scene, hcat(x, ys), "$t bspline p=$(round(p, sigdigits = 4))"; group = "bspline") && (n += 1)
	end
	n == 0 && error(isempty(msgs) ? "nothing to smooth" : join(msgs, "\n"))
	return "bspline: $n line(s) smoothed" * (isempty(msgs) ? "" : "\n" * join(msgs, "\n"))
end

function _lop_do_cspline(scene, targets, rest)
	toks = split(strip(rest), r"\s+"; keepempty = false)
	isempty(toks) && error("must provide N (the decimation interval)")
	toks[1] == "N" && error("the argument \"N\" is not to be taken literally: it must contain the decimation interval")
	N = tryparse(Float64, toks[1])
	N === nothing && error("cspline argument is nonsense")
	N = max(round(Int, abs(N)), 1)
	res = 10
	if length(toks) > 1
		v = tryparse(Float64, toks[2])
		v !== nothing && (res = max(round(Int, abs(v)), 2))
	end
	n = 0
	for t in targets
		xy = _lop_xy(scene, t)
		np = size(xy, 1)
		np < 3 && continue
		N >= np && error("N ($N) is larger than the number of vertices of \"$t\" — the smoothed line would disappear")
		idx = collect(1:N:np)
		idx[end] != np && push!(idx, np)
		xs, ys = _lop_spline_interp(Float64.(xy[idx, 1]), Float64.(xy[idx, 2]), res)
		_lop_add(scene, hcat(xs, ys), "$t cspline $N"; group = "cspline") && (n += 1)
	end
	n == 0 && error("nothing to spline")
	return "cspline: $n line(s) added"
end

function _lop_do_polysimplify(scene, targets, rest)
	s = strip(rest)
	(isempty(s) || s == "TOL") && error("the argument \"TOL\" must be replaced by the tolerance")
	tol = tryparse(Float64, split(s)[1])
	tol === nothing && error("polysimplify argument is nonsense")
	tol = abs(tol)
	geog = _lop_isgeog(scene)
	n = 0
	for t in targets
		xy = _lop_xy(scene, t)
		size(xy, 1) < 3 && continue
		# TOL is in km when the data is geographic (Mirone's own convention) — gmtsimplify's -T
		# takes the unit right there, so no distance is computed on this side.
		D = geog ? GMT.gmtsimplify(GMT.mat2ds(xy), T = "$(tol)k") : GMT.gmtsimplify(GMT.mat2ds(xy), T = tol)
		_lop_add(scene, _lop_ds2mat(D), "$t simplified $tol"; group = "polysimplify") && (n += 1)
	end
	n == 0 && error("nothing to simplify")
	return "polysimplify: $n line(s) added"
end

# --- buffer / closing ----------------------------------------------------------------------------
function _lop_do_buffer(scene, targets, rest, closing::Bool)
	geog = _lop_isgeog(scene)
	o = _lop_buffer_args(rest, geog, closing ? "out" : "both")
	n = 0
	for t in targets
		xy = _lop_xy(scene, t)
		size(xy, 1) < 2 && continue
		M = _lop_buffer(xy, o, geog)
		if closing
			# "Closing" is buffer OUT followed by buffer IN by the same amount — the image-processing
			# morphological closing, done with the SAME buffer function, twice.
			size(M, 1) < 3 && continue
			M = _lop_buffer(M, BufferOpts(o.dist, "in", o.npts, 'b', o.base, o.top), geog)
		end
		size(M, 1) < 3 && continue
		_lop_add(scene, M, closing ? "$t closing" : "$t buffer"; group = closing ? "closing" : "buffer") && (n += 1)
	end
	n == 0 && error("no buffer could be computed")
	return (closing ? "closing" : "buffer") * ": $n zone(s) added"
end

# --- the four boolean operations ------------------------------------------------------------------
function _lop_do_boolean(scene, targets, op::AbstractString)
	length(targets) < 2 && error("you want to make the $op of a single line??? Weird!")
	D = GMT.mat2ds(_lop_xy(scene, targets[1]))
	for k in 2:length(targets)
		P = GMT.mat2ds(_lop_xy(scene, targets[k]))
		D = op == "polyunion"     ? GMT.polyunion(D, P)     :
		    op == "polyintersect" ? GMT.intersection(D, P)  :
		    op == "polyxor"       ? GMT.symdifference(D, P) :
		                            GMT.difference(D, P)
	end
	M = _lop_ds2mat(D)
	size(M, 1) < 3 && error("$op produced nothing")
	_lop_add(scene, M, join(targets, " $op "); group = op) || error("could not add the result")
	return "$op: 1 polygon added"
end

# --- delete DUP | SMALL N | SPUR -------------------------------------------------------------------
function _lop_do_delete(scene, targets, rest)
	r = strip(rest)
	els = _lop_elements(scene)
	if uppercase(r) == "DUP"
		xys = Dict{String,Matrix{Float64}}(e.name => _lop_xy(scene, e.name) for e in els)
		killed = String[]
		for a in 1:length(els), b in a+1:length(els)
			na = els[a].name;  nb = els[b].name
			(na in killed || nb in killed) && continue
			A = xys[na];  B = xys[nb]
			size(A) != size(B) && continue
			(A == B || A == reverse(B, dims = 1)) && push!(killed, nb)
		end
		for nm in killed;  _lop_remove(scene, nm);  end
		return "delete DUP: removed $(length(killed)) duplicated line(s)"
	elseif startswith(uppercase(r), "SMALL")
		toks = split(r, r"\s+"; keepempty = false)
		length(toks) < 2 && error("bad usage — the correct syntax is \"delete SMALL N\"")
		nv = tryparse(Float64, toks[2])
		(nv === nothing || nv <= 0) && error("bad usage — N must be a valid positive integer")
		nv = round(Int, nv)
		killed = 0
		for e in els
			e.npts < nv && (_lop_remove(scene, e.name); killed += 1)
		end
		return "delete SMALL $nv: removed $killed line(s)"
	elseif uppercase(r) == "SPUR"
		# A spur is a stretch where the line walks out and comes straight back: vertex k and k+2
		# coincide. Remove the whole out-and-back.
		work = isempty(targets) ? [e.name for e in els] : targets
		fixed = 0
		for nm in work
			M = _lop_xy(scene, nm)
			size(M, 1) < 3 && continue
			N = _lop_despur(M)
			size(N, 1) == size(M, 1) && continue
			_lop_remove(scene, nm)
			size(N, 1) >= 2 && _lop_add(scene, N, nm)
			fixed += 1
		end
		return "delete SPUR: $fixed line(s) had spurs removed"
	elseif r == "DUP|SMALL" || r == "DUP|SMALL N|SPUR"
		error("bad usage: \"DUP|SMALL N|SPUR\" is not to be used literally — choose ONE of DUP, SMALL N or SPUR")
	end
	error("unknown delete option \"$r\" (use DUP, SMALL N or SPUR)")
end

# Ported literally from line_operations.m's `delete SPUR` branch, quirks included: a vertex whose
# neighbours-once-removed coincide (dif == 0) is only cut when the SURROUNDING dif entries vanish
# too, i.e. when the out-and-back is at least three mirrored pairs long. A lone dif == 0 (a line that
# merely doubles back over one vertex) is left alone — that is what the .m does, and changing it here
# would be inventing a different operation.
function _lop_despur(M::Matrix{Float64})::Matrix{Float64}
	x = Float64.(M[:, 1]);  y = Float64.(M[:, 2])
	n = length(x)
	n < 3 && return M
	difx = x[1:n-2] .- x[3:n]
	all(difx .!= 0) && return M                      # no spurs here for sure
	dify = y[1:n-2] .- y[3:n]
	ind = findall((difx .== 0) .& (dify .== 0))
	for k in ind
		n1 = k - 1;  n2 = k + 1;  c = 0
		while n1 > 0 && n2 <= length(difx) && difx[n1] == 0 && difx[n2] == 0 &&
		      dify[n1] == 0 && dify[n2] == 0
			n1 += 1;  n2 += 1;  c += 1
		end
		c == 0 && continue
		lo = max(k - c, 1);  hi = min(k + c, length(x))
		keep = setdiff(1:length(x), lo:hi)
		x = x[keep];  y = y[keep]
		isempty(x) && break
		# The dif arrays describe the OLD vertex list; the .m has the same shape, so like it we stop
		# after the first cut rather than index a stale table.
		break
	end
	return hcat(x, y)
end

# --- group lines -----------------------------------------------------------------------------------
# Many separate lines with the SAME pen become ONE multi-segment element. Visually identical, but the
# window then carries one actor instead of hundreds.
function _lop_do_group(scene, targets)
	els = _lop_elements(scene)
	isempty(targets) && error("pick a line first — its pen says which lines are grouped")
	seed = nothing
	for e in els;  e.name == targets[1] && (seed = e);  end
	seed === nothing && error("no line named \"$(targets[1])\" in this window")
	same = String[]
	if length(targets) > 1
		same = copy(targets)                       # an explicit multi-selection IS the group
	else
		for e in els
			(e.rgb == seed.rgb && e.width == seed.width && e.style == seed.style) && push!(same, e.name)
		end
	end
	length(same) < 2 && error("found no other line with the same pen to group with")
	parts = Matrix{Float64}[]
	for (k, nm) in enumerate(same)
		M = _lop_xy(scene, nm)
		size(M, 1) < 2 && continue
		k > 1 && push!(parts, [NaN NaN])
		push!(parts, M)
	end
	isempty(parts) && error("nothing to group")
	for nm in same;  _lop_remove(scene, nm);  end
	_lop_add(scene, vcat(parts...), seed.name) || error("could not add the grouped line")
	return "group: $(length(same)) lines merged into \"$(seed.name)\""
end

# --- line2patch --------------------------------------------------------------------------------------
# A line becomes a closed PATCH — which, unlike a line, accepts a fill colour. The patch is created
# through the drawn-polygon constructor, so it arrives with the polygon properties (fill, opacity,
# area) already on its context menu.
function _lop_do_line2patch(scene, targets)
	n = 0
	for t in targets
		M = _lop_xy(scene, t)
		size(M, 1) < 3 && continue
		# One ring: a patch is a single closed shape (the NaN-broken pieces of a multi-segment line
		# are separate rings, each of which becomes its own patch).
		rings = _lop_split_nan(M)
		for (k, R) in enumerate(rings)
			size(R, 1) < 3 && continue
			(R[1, 1] != R[end, 1] || R[1, 2] != R[end, 2]) && (R = vcat(R, R[1:1, :]))
			xyz = vec(permutedims(hcat(R, zeros(size(R, 1)))))
			nm = length(rings) == 1 ? "$t (patch)" : "$t (patch $k)"
			idx = ccall(_fn(:gmtvtk_add_poly_full), Cint,
			            (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cint, Cint, Cdouble, Cdouble, Cdouble,
			             Cdouble, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cstring, Cstring),
			            scene, xyz, Cint(size(R, 1)), Cint(1), Cint(0),
			            0.0, 0.0, 0.0, 2.0, Cint(0), 0.8, 0.8, 0.8, 0.0, nm, "")
			idx >= 0 && (n += 1)
		end
		n > 0 && _lop_remove(scene, t)              # Mirone deletes the line it converted
	end
	n == 0 && error("nothing to convert")
	return "line2patch: $n patch(es) created"
end

function _lop_split_nan(M::Matrix{Float64})::Vector{Matrix{Float64}}
	out = Matrix{Float64}[]
	i = 1;  n = size(M, 1)
	while i <= n
		while i <= n && (isnan(M[i, 1]) || isnan(M[i, 2]));  i += 1;  end
		j = i
		while j <= n && !isnan(M[j, 1]) && !isnan(M[j, 2]);  j += 1;  end
		j - i >= 2 && push!(out, Matrix{Float64}(M[i:j-1, :]))
		i = j
	end
	return out
end

# --- pline [x1 .. xn; y1 .. yn] ----------------------------------------------------------------------
function _lop_do_pline(scene, rest)
	s = strip(rest)
	i1 = findfirst('[', s);  i2 = findfirst(']', s);  i3 = findfirst(';', s)
	(i1 === nothing || i2 === nothing) && error("wrong syntax: the brackets [ ] must appear once each")
	i3 === nothing && error("wrong syntax: the ';' separating the X from the Y row must appear once")
	xs = [parse(Float64, t) for t in split(s[i1+1:i3-1], r"[\s,]+"; keepempty = false)]
	ys = [parse(Float64, t) for t in split(s[i3+1:i2-1], r"[\s,]+"; keepempty = false)]
	isempty(xs) && error("there are no X points inside the brackets")
	length(xs) == length(ys) || error("got $(length(xs)) X but $(length(ys)) Y coordinates")
	_lop_add(scene, hcat(xs, ys), "pline") || error("could not add the line")
	return "pline: 1 line of $(length(xs)) vertices added"
end

# --- scale to [-0.5 0.5] ------------------------------------------------------------------------------
# Every line in the window, scaled into the unit square a GMT custom symbol lives in, in a NEW window
# framed exactly on [-0.5 0.5] — Mirone opens a background-region figure for this, and so do we,
# through the SAME blank-canvas builder File > Background region uses.
function _lop_do_scale(scene)
	els = _lop_elements(scene)
	isempty(els) && error("there are no lines in this window")
	xmin = Inf;  xmax = -Inf;  ymin = Inf;  ymax = -Inf
	lines = Tuple{String,Matrix{Float64}}[]
	for e in els
		M = _lop_xy(scene, e.name)
		size(M, 1) < 2 && continue
		push!(lines, (e.name, M))
		for i in 1:size(M, 1)
			isnan(M[i, 1]) && continue
			xmin = min(xmin, M[i, 1]);  xmax = max(xmax, M[i, 1])
			ymin = min(ymin, M[i, 2]);  ymax = max(ymax, M[i, 2])
		end
	end
	isempty(lines) && error("there are no lines in this window")
	(xmax <= xmin || ymax <= ymin) && error("the lines have no extent to scale")
	sc = min(1 / (xmax - xmin), 1 / (ymax - ymin))
	h = _blank_canvas(C_NULL, -0.5, 0.5, -0.5, 0.5, false, "GMT custom symbol")
	h == C_NULL && error("could not open the custom-symbol window")
	for (nm, M) in lines
		S = hcat((M[:, 1] .- xmin) .* sc .- 0.5, (M[:, 2] .- ymin) .* sc .- 0.5)
		_lop_add(h, S, nm)
	end
	return "scale: $(length(lines)) line(s) scaled into a new [-0.5 0.5] window"
end

# --- self-crossings -----------------------------------------------------------------------------------
function _lop_do_selfcrossings(scene, targets)
	work = isempty(targets) ? [e.name for e in _lop_elements(scene)] : targets
	total = 0
	for nm in work
		M = _lop_xy(scene, nm)
		size(M, 1) < 3 && continue
		xc, yc, ic = _lop_intersections(Float64.(M[:, 1]), Float64.(M[:, 2]))
		# Mirone drops a first crossing that sits on the very first vertex — a false positive of a
		# line whose ends touch.
		if !isempty(ic) && (ic[1] - 1) < 1e-5
			popfirst!(xc);  popfirst!(yc);  popfirst!(ic)
		end
		isempty(xc) && continue
		_add_dataset_to_scene(scene, GMT.mat2ds(hcat(xc, yc)), "$nm self-crossings";
		                      groupName = "self-crossings", forceMode = :points)
		total += length(xc)
	end
	total == 0 && return "self-crossings: none found"
	return "self-crossings: $total found"
end

# --- stitch TOL [ALL] | SORT ---------------------------------------------------------------------------
function _lop_do_stitch(scene, targets, rest)
	r = strip(rest)
	toks = split(r, r"\s+"; keepempty = false)
	stitch_all = any(t -> uppercase(t) == "ALL", toks)
	geog = _lop_isgeog(scene)
	if any(t -> uppercase(t) == "SORT", toks)
		isempty(targets) && error("pick the line to sort first")
		M = _lop_xy(scene, targets[1])
		size(M, 1) < 3 && error("nothing to sort")
		_lop_add(scene, _lop_sortline(M), "$(targets[1]) sorted"; group = "stitch") ||
			error("could not add the sorted line")
		return "stitch SORT: 1 line re-ordered"
	end
	tol = Inf
	dtok = ""
	for t in toks
		u = uppercase(t)
		(u == "ALL" || u == "TOL") && continue
		dtok = String(t);  break
	end
	if !isempty(dtok)
		d, inmetres = _lop_dist(dtok, geog)
		tol = (geog && inmetres) ? _lop_m2deg(d) : d      # the comparison is in map units
	end
	els = _lop_elements(scene)
	isempty(els) && error("there is nothing to stitch")
	seeds = stitch_all ? [e.name for e in els] : targets
	isempty(seeds) && error("pick the line to stitch onto, or add ALL to stitch everything")
	pool = Dict{String,Matrix{Float64}}(e.name => _lop_xy(scene, e.name) for e in els)
	# A single multi-segment line stitches its OWN pieces (Mirone's polysplit branch).
	if length(els) == 1 && !isempty(seeds)
		nm = seeds[1]
		pieces = _lop_split_nan(pool[nm])
		length(pieces) < 2 && error("there is only one line in town and with no NaNs breaking it — so stitch where?")
		cur = pieces[1]
		rest_pieces = pieces[2:end]
		while true
			k, et = _lop_closest(cur, rest_pieces, tol)
			k == 0 && break
			cur = _lop_join(cur, rest_pieces[k], et)
			deleteat!(rest_pieces, k)
		end
		out = _lop_stitch_cleanup(cur, tol)
		for p in rest_pieces;  out = vcat(out, [NaN NaN], p);  end
		_lop_remove(scene, nm)
		_lop_add(scene, out, nm)
		return "stitch: the segments of \"$nm\" were joined"
	end
	joined = 0
	dead = Set{String}()
	for seed in seeds
		seed in dead && continue
		haskey(pool, seed) || continue
		cur = pool[seed]
		size(cur, 1) < 2 && continue
		while true
			names = [n for n in keys(pool) if n != seed && !(n in dead)]
			isempty(names) && break
			mats = [pool[n] for n in names]
			k, et = _lop_closest(cur, mats, tol)
			k == 0 && break
			cur = _lop_join(cur, mats[k], et)
			push!(dead, names[k])
			_lop_remove(scene, names[k])
			joined += 1
		end
		cur = _lop_stitch_cleanup(cur, tol)
		if joined > 0
			_lop_remove(scene, seed)
			_lop_add(scene, cur, seed)
			pool[seed] = cur
		end
	end
	joined == 0 && return "stitch: no line was close enough to be stitched"
	return "stitch: $joined line(s) assimilated"
end

# --- thicken N ------------------------------------------------------------------------------------------
function _lop_do_thicken(scene, targets, rest)
	isempty(targets) && error("pick the line to thicken first")
	s = strip(rest)
	N = 10
	if !isempty(s) && uppercase(s) != "N"
		v = tryparse(Float64, split(s)[1])
		v === nothing && error("the thicken argument is nonsense")
		N = max(round(Int, abs(v)), 1)
	end
	G = _find_object(scene, :grid, "")
	G === nothing && error("thicken measures the thickness in GRID CELLS — this window has no grid")
	thick = N * (abs(G.inc[1]) + abs(G.inc[2])) / 2
	geog = _lop_isgeog(scene)
	nm = targets[1]
	xy = _lop_xy(scene, nm)
	size(xy, 1) < 2 && error("nothing to thicken")
	lines = _lop_thicken_lines(xy, N, thick, geog)
	_LOP_THICK[(scene, String(nm))] = lines
	# Show the thickness: the line is drawn as many pixels wide as `thick` map units cover on screen.
	wpp = try ccall(_fn(:gmtvtk_world_per_pixel_h), Cdouble, (Ptr{Cvoid},), scene) catch; 0.0 end
	if wpp > 0
		px = clamp(thick / wpp, 1.0, 100.0)
		ccall(_fn(:gmtvtk_set_overlay_style_h), Cint,
		      (Ptr{Cvoid}, Cstring, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cdouble),
		      scene, String(nm), 0.0, 0.0, 0.0, px, Cint(0), 1.0)
	end
	return "thicken: \"$nm\" now carries $(N+1) parallel tracks $(round(thick, sigdigits=4)) map units apart;\n" *
	       "Extract profile on it will stack them."
end

# --- toRidge N --------------------------------------------------------------------------------------------
function _lop_do_toridge(scene, targets, rest)
	isempty(targets) && error("pick the line to move onto the ridge first")
	N = 5
	s = strip(rest)
	if !isempty(s) && uppercase(s) != "N"
		v = tryparse(Float64, split(s)[1])
		v === nothing && error("the N argument is nonsense")
		N = max(round(Int, abs(v)), 1)
	end
	G = _find_object(scene, :grid, "")
	G === nothing && error("this operation is only possible with grids, not images")
	dx = abs(G.inc[1]);  dy = abs(G.inc[2])
	nm = targets[1]
	xy = _lop_xy(scene, nm)
	size(xy, 1) < 1 && error("nothing to move")
	out = copy(xy)
	moved = 0
	for k in 1:size(xy, 1)
		(isnan(xy[k, 1]) || isnan(xy[k, 2])) && continue
		w = max(G.range[1], xy[k, 1] - N * dx);  e = min(G.range[2], xy[k, 1] + N * dx)
		s0 = max(G.range[3], xy[k, 2] - N * dy);  n0 = min(G.range[4], xy[k, 2] + N * dy)
		(e <= w || n0 <= s0) && continue
		local sub
		try
			sub = GMT.grdcut(G, R = "$w/$e/$s0/$n0")
		catch
			continue
		end
		Z = _zmat(sub)
		(size(Z, 1) < 3 || size(Z, 2) < 3) && continue
		R = _ppa_ridges(Z, Float64(sub.range[1]), Float64(sub.range[3]),
		                Float64(abs(sub.inc[1])), Float64(abs(sub.inc[2])))
		size(R, 1) == 0 && continue
		p = _lop_nearest_on(R, xy[k, 1], xy[k, 2])
		p === nothing && continue
		out[k, 1] = p[1];  out[k, 2] = p[2]
		moved += 1
	end
	moved == 0 && error("no ridge was found near any vertex")
	_lop_add(scene, out, "$nm on ridge"; group = "toRidge") || error("could not add the ridge line")
	return "toRidge: $moved of $(size(xy, 1)) vertices moved onto a ridge"
end

# The point of `ridge` closest to (x, y). GMT's `mapproject -L` owns the nearest-point search (it is
# geodesic when the data is), exactly as Mirone's own toRidge does it.
function _lop_nearest_on(ridge::Matrix{Float64}, x::Float64, y::Float64)
	tmp = joinpath(tempdir(), "igmt_lop_ridge_$(getpid())_$(round(Int, time()*1e6)).txt")
	try
		open(tmp, "w") do io
			for i in 1:size(ridge, 1)
				if isnan(ridge[i, 1]);  println(io, ">");  continue;  end
				println(io, ridge[i, 1], "\t", ridge[i, 2])
			end
		end
		R = GMT.gmt("mapproject -L$tmp", [x y])
		D = isa(R, Vector) ? R[1].data : R.data
		size(D, 1) == 0 && return nothing
		size(D, 2) < 5 && return nothing
		isfinite(D[1, 3]) || return nothing
		return (D[1, 4], D[1, 5])
	catch
		return nothing
	finally
		isfile(tmp) && (try rm(tmp) catch end)
	end
end

# --- hand2Workspace ------------------------------------------------------------------------------------
# Mirone drops the picked line HANDLES into the MATLAB workspace so the user can poke at their
# properties. The workspace here is the Julia session, and what is worth having there is the data:
# `Main.lineHandles` gets a Vector{GMTdataset}, one entry per picked line, named after it.
function _lop_do_hand2workspace(scene, targets)
	isempty(targets) && error("pick the line(s) to send to the workspace first")
	out = GMT.GMTdataset[]
	for t in targets
		for s in _lop_segments(scene, t)
			D = GMT.mat2ds(s)
			D.header = String(t)
			push!(out, D)
		end
	end
	isempty(out) && error("the picked line(s) have no vertices")
	Core.eval(Main, :(lineHandles = $out))
	return "hand2Workspace: $(length(out)) segment(s) bound to Main.lineHandles"
end

# --- GMT_DB -----------------------------------------------------------------------------------------------
# Update the GSHHG-derived polygons on display (Coastlines / Boundaries / Rivers) with the ordinary
# polylines also in the window: for each updater line, find the DB polygon whose vertices come within
# DS of BOTH of the updater's ends and splice the updater in between them. The updater is consumed.
const _LOP_DB_NAMES = ("Coastlines", "Boundaries", "Rivers")
const _LOP_DB_DS = 0.02      # degrees; Mirone's own search radius

function _lop_do_gmtdb(scene)
	els = _lop_elements(scene)
	dbs = [e.name for e in els if any(n -> startswith(e.name, n), _LOP_DB_NAMES)]
	isempty(dbs) && error("this window holds no GMT database polygons (Coastlines / Boundaries / Rivers)")
	upd = [e.name for e in els if !(e.name in dbs)]
	isempty(upd) && error("there are no ordinary polylines to update the database with")
	done = 0
	for un in upd
		U = _lop_xy(scene, un)
		size(U, 1) < 2 && continue
		U = _lop_ccw(U)                       # the GMT database wants land on the port side (CCW)
		hit = false
		for dn in dbs
			Gm = _lop_xy(scene, dn)
			size(Gm, 1) < 3 && continue
			is = _lop_near_index(Gm, U[1, 1], U[1, 2], _LOP_DB_DS)
			is == 0 && continue
			ie = _lop_near_index(Gm, U[end, 1], U[end, 2], _LOP_DB_DS)
			ie == 0 && continue
			if ie < is
				# Mirone's own bail-out: the updater runs against the polygon's sense.
				continue
			end
			New = vcat(Gm[1:is-1, :], U, Gm[ie+1:end, :])
			_lop_remove(scene, dn)
			_lop_add(scene, New, dn)
			_lop_remove(scene, un)
			done += 1;  hit = true
			break
		end
		hit && continue
	end
	done == 0 && error("nothing updated — the end points of the updating polylines were not within " *
	                   "$(_LOP_DB_DS) degrees of any database polygon")
	return "GMT_DB: $done polygon(s) updated"
end

# check_bombordo: the GMT database coastlines are traced counter-clockwise (land to port). Reverse an
# updater that runs the other way.
function _lop_ccw(M::Matrix{Float64})::Matrix{Float64}
	n = size(M, 1)
	n < 3 && return M
	xt = M[1:2:end-1, 1];  yt = M[1:2:end-1, 2]
	m = length(xt)
	m < 3 && return M
	xt = xt .- sum(xt) / m
	a = 0.0
	for k in 1:m
		i = k == m ? 1 : k + 1
		j = k >= m - 1 ? k - m + 3 : k + 2
		a += xt[i] * (yt[j] - yt[k])
	end
	return a < 0 ? reverse(M, dims = 1) : M
end

# Index of the vertex of `M` closest to (x,y), but only among those inside the ±ds box (Mirone's own
# two-step: box filter first, then the closest of what survived).
function _lop_near_index(M::Matrix{Float64}, x::Float64, y::Float64, ds::Float64)::Int
	best = 0;  bd = Inf
	for i in 1:size(M, 1)
		(isnan(M[i, 1]) || isnan(M[i, 2])) && continue
		(abs(M[i, 1] - x) > ds || abs(M[i, 2] - y) > ds) && continue
		d = (M[i, 1] - x)^2 + (M[i, 2] - y)^2
		d < bd && (bd = d; best = i)
	end
	return best
end

# =================================================================================================
#  Apply
# =================================================================================================

function _lop_apply(scene::Ptr{Cvoid}, cmd::AbstractString, targets::Vector{String})::String
	c = strip(cmd)
	isempty(c) && error("Apply WHAT????")
	sp = findfirst(isspace, c)
	op   = sp === nothing ? String(c) : String(c[1:sp-1])
	rest = sp === nothing ? "" : String(strip(c[sp:end]))
	op in _LOP_OPS || error("unknown operation \"$op\"")
	(isempty(targets) && !(op in _LOP_NOPICK)) && error("Apply WHERE???? — pick a line first")

	op == "bezier"         && return _lop_do_bezier(scene, targets, rest)
	op == "buffer"         && return _lop_do_buffer(scene, targets, rest, false)
	op == "closing"        && return _lop_do_buffer(scene, targets, rest, true)
	op == "bspline"        && return _lop_do_bspline(scene, targets, rest)
	op == "cspline"        && return _lop_do_cspline(scene, targets, rest)
	op == "delete"         && return _lop_do_delete(scene, targets, rest)
	op == "group"          && return _lop_do_group(scene, targets)
	op == "line2patch"     && return _lop_do_line2patch(scene, targets)
	op == "polysimplify"   && return _lop_do_polysimplify(scene, targets, rest)
	op in ("polyunion", "polyintersect", "polyxor", "polyminus") &&
		return _lop_do_boolean(scene, targets, op)
	op == "pline"          && return _lop_do_pline(scene, rest)
	op == "scale"          && return _lop_do_scale(scene)
	op == "stitch"         && return _lop_do_stitch(scene, targets, rest)
	op == "thicken"        && return _lop_do_thicken(scene, targets, rest)
	op == "toRidge"        && return _lop_do_toridge(scene, targets, rest)
	op == "self-crossings" && return _lop_do_selfcrossings(scene, targets)
	op == "hand2Workspace" && return _lop_do_hand2workspace(scene, targets)
	op == "GMT_DB"         && return _lop_do_gmtdb(scene)
	error("unknown operation \"$op\"")
end

# ---------------------------------------------------------------------------------------------
# C callback (Apply): `cparams` is the newline-separated "key=value" block described in 30_app.cpp's
# JuliaLineOpsFn. Returns Cint 1 on success, 0 on failure.
function _on_lineops(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		msg = _lop_apply(scene, _get(d, "cmd"), _lop_targets(d))
		# A new element the user cannot see is the same as no element at all.
		try ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene) catch end
		_lineops_result(msg)
		return Cint(1)
	catch e
		msg = sprint(showerror, e)
		_viewer_log_error(scene, "Vector Operations FAILED: $msg")
		_lineops_result("Vector Operations failed: $msg")
		@warn "Vector Operations FAILED" exception = (e,)
		return Cint(0)
	end
end

# The first call into GEOS / gmtsimplify / buffergeo carries the usual GMT.jl compile cost. Run them
# once on a three-point line while the user is still reading the operations list.
function _lop_warm()
	try
		L = GMT.mat2ds([0.0 0.0; 1.0 0.5; 2.0 0.0])
		GMT.gmtsimplify(L, T = 0.01)
		S = GMT.mat2ds([0.0 0.0; 1.0 0.0; 1.0 1.0; 0.0 1.0; 0.0 0.0])
		GMT.polyunion(S, S)
		_lop_spline_interp([0.0, 1.0, 2.0], [0.0, 1.0, 0.0], 4)
		_lop_bezier([0.0 0.0; 1.0 1.0; 2.0 0.0], 20)
	catch
	end
	return
end

function _register_lineops()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_lineops, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_lineops_callback), Cvoid, (Ptr{Cvoid},), fptr)
	warm_register("lineops", _lop_warm)      # C++ fires this when the dialog opens (70_window.cpp)
	return
end
