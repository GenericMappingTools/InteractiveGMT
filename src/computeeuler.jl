# computeeuler.jl — Plates > "Compute Euler pole": port of Mirone's src_figs/compute_euler.m.
#
# Two ways of fitting the pole that takes isochron 1 onto isochron 2, exactly the two the MATLAB
# dialog offers:
#
#   * "our method"  — a semi-brute-force search: rotate the moving isochron by every pole of a
#                     (nLon x nLat x nAng) box centred on the starting pole and keep the one whose
#                     weighted mean point-to-line distance (the `distmin` cost) is smallest.
#   * Hellinger     — the classical statistical fit. The SOLVER is GMT.jl's `hellinger` (already a
#                     port of Mirone's utils/hellinger.m); this file only feeds it, through
#                     GMT.hellinger_auto, which carries the Douglas-Peucker auto-segmentation, the
#                     stats block, the confidence volume and the error ellipse.
#
# What is ported here and NOT taken from GMT is the cost function itself: Mirone's mex/distmin.c.
# That MEX declares no GMT dependency at link time — it carries its own file-static copies of
# `great_circle_dist`, `geo_to_cart`, `great_circle_intersection` and a speed-tuned
# `near_a_line_spherical` — so there is no missing wrapper to write: the algorithm IS the file, and
# it is ported below line for line, quirks included (they are called out where they matter). It is
# also why it cannot be `mapproject -L`: the search evaluates ~50 000 rotations, each one two full
# passes over both lines.
#
# The C++ dialog is ComputeEulerDialog (70_window.cpp, loads deps/ui/compute_euler.ui); it talks to
# this file through the SAME `gmtvtk_set_euler_callback` channel the other two Plates dialogs use
# (plates.jl's `_on_euler`), with its own `op=` values — one door for the whole menu.

# ---------------------------------------------------------------------------------------------
# distmin.c, ported.  Angles are RADIANS throughout; distances come out in km.

const _CE_KMRAD = 111.19507973463158 * 180 / pi     # distmin.c's KMRAD: km per radian (== 6371 km)
const _CE_ECC   = 0.0818191908426215                # WGS84 first eccentricity
const _CE_R     = 6371.0                            # sphere radius used for the segment lengths

@inline function _ce_geo2cart(lat::Float64, lon::Float64)
	clat = cos(lat)
	return (clat * cos(lon), clat * sin(lon), sin(lat))
end

@inline _ce_cart2geo(a::NTuple{3,Float64}) = (asin(clamp(a[3], -1.0, 1.0)), atan(a[2], a[1]))

@inline _ce_dot(a::NTuple{3,Float64}, b::NTuple{3,Float64}) = a[1]*b[1] + a[2]*b[2] + a[3]*b[3]

@inline _ce_cross(a::NTuple{3,Float64}, b::NTuple{3,Float64}) =
	(a[2]*b[3] - a[3]*b[2], a[3]*b[1] - a[1]*b[3], a[1]*b[2] - a[2]*b[1])

@inline function _ce_unit(a::NTuple{3,Float64})
	r = sqrt(a[1]*a[1] + a[2]*a[2] + a[3]*a[3])
	r == 0.0 && return a
	return (a[1]/r, a[2]/r, a[3]/r)
end

# great_circle_dist(): the coincident-points guard returning a WHOLE radian (KMRAD) is distmin.c's
# own — it only ever feeds the `fraction` denominator, so it is kept verbatim.
@inline function _ce_gc_dist(lon1::Float64, lat1::Float64, lon2::Float64, lat2::Float64)
	(lat1 == lat2 && lon1 == lon2) && return _CE_KMRAD
	c = sin(lat1)*sin(lat2) + cos(lat1)*cos(lat2)*cos(lon1 - lon2)
	return acos(clamp(c, -1.0, 1.0)) * _CE_KMRAD
end

# great_circle_dist2(): sin/cos of the first latitude are hoisted by the caller.
@inline function _ce_gc_dist2(cosa::Float64, sina::Float64, lon1::Float64, lon2::Float64, lat2::Float64)
	c = sina*sin(lat2) + cosa*cos(lat2)*cos(lon1 - lon2)
	return acos(clamp(c, -1.0, 1.0)) * _CE_KMRAD
end

# great_circle_intersection(): X is the point of the great circle (A,B) closest to C. Returns
# (outside, X) — `outside` true when X falls on the EXTENSION of A-B (the C code's "return 1").
@inline function _ce_gc_intersection(A::NTuple{3,Float64}, B::NTuple{3,Float64}, C::NTuple{3,Float64})
	P = _ce_unit(_ce_cross(A, B))
	E = _ce_unit(_ce_cross(C, P))
	X = _ce_unit(_ce_cross(P, E))
	M = _ce_unit((A[1] + B[1], A[2] + B[2], A[3] + B[3]))
	Xn = (-X[1], -X[2], -X[3])
	_ce_dot(M, Xn) > _ce_dot(M, X) && (X = Xn)      # -X is the one nearer the A-B midpoint
	cos_AB = abs(_ce_dot(A, B))
	abs(_ce_dot(A, X)) < cos_AB && return (true, X)
	abs(_ce_dot(B, X)) < cos_AB && return (true, X)
	return (false, X)
end

# near_a_line_spherical(): shortest distance from one point to a polyline, and where on it.
# `row_s` is the vertex the PREVIOUS point resolved to (1-based here) — the crude first scan starts
# there, which is what makes the whole search affordable and is why both lines must run in the same
# direction (the MEX's documented precondition).
#
# Two faithfully-kept oddities of the C original:
#   * `B` is seeded from vertex 1 whatever `ind_s` is, so the first great-circle segment tested is
#     (vertex 1 -> vertex ind_s) rather than (ind_s-1 -> ind_s);
#   * `aux` is only written when a perpendicular foot beats the nodal distance. C left it
#     uninitialised; here it starts at the nearest VERTEX index, which is what the value is used
#     for downstream (the segment whose length gives the weight, and the next point's scan start).
function _ce_near_a_line(lon::Float64, lat::Float64, llon::Vector{Float64}, llat::Vector{Float64},
                         n::Int, row_s::Int)
	n < 2 && return (false, 0.0, 1, 0.0, 0.0)
	coslat = cos(lat);  sinlat = sin(lat)

	dmin = 1e30;  seg = row_s
	for row in max(1, row_s):n                       # crude, planar first pass
		dy = lat - llat[row];  dx = (lon - llon[row]) * coslat
		d  = dx*dx + dy*dy
		if d < dmin
			dmin = d;  seg = row
		end
	end

	dmin = 1e30
	x_near = 0.0;  y_near = 0.0
	for row in max(1, seg-1):min(n, seg+1)           # true distance to the 3 nearest vertices
		d = _ce_gc_dist2(coslat, sinlat, lon, llon[row], llat[row])
		if d < dmin
			dmin = d;  seg = row;  x_near = llon[row];  y_near = llat[row]
		end
	end

	local ind_s::Int, ind_e::Int
	if seg == 1
		ind_s = 1;  ind_e = 2
	elseif seg == n
		ind_s = n - 1;  ind_e = n
	else
		ind_s = seg - 1;  ind_e = seg + 1
	end

	C = _ce_geo2cart(lat, lon)
	B = _ce_geo2cart(llat[1], llon[1])               # see note above: vertex 1, not ind_s-1
	aux = Float64(seg)
	k = 0
	for row in ind_s:ind_e
		A = B
		B = _ce_geo2cart(llat[row], llon[row])
		outside, X = _ce_gc_intersection(A, B, C)
		outside && continue                          # X is not between A and B
		k += 1
		xlat, xlon = _ce_cart2geo(X)
		d = _ce_gc_dist2(coslat, sinlat, lon, xlon, xlat)
		if d < dmin
			dmin = d
			x_near = xlon;  y_near = xlat
			j0 = row - 1
			if j0 >= 1                               # row == 1 only happens on the degenerate A == B
				dist_AB  = _ce_gc_dist(llon[j0], llat[j0], llon[row], llat[row])
				fraction = dist_AB > 0.0 ? _ce_gc_dist(llon[j0], llat[j0], xlon, xlat) / dist_AB : 0.0
				aux = j0 + fraction
			end
		end
	end

	seg = trunc(Int, aux)
	seg < 1 && (seg = 1)
	(k == 0 && (ind_s == 1 || ind_e == n)) && return (false, dmin, seg, x_near, y_near)
	return (true, dmin, seg, x_near, y_near)
end

# dists_sph(): the weighted mean distance from every vertex of one line to the other line. The
# weight of a vertex is set by the LENGTH of the segment it landed on: densely sampled stretches
# count fully, sparse ones count less, segments longer than the last class count zero — the MEX's
# way of not letting a coarsely digitised piece of isochron dominate the fit.
function _ce_dists_sph(lon::Vector{Float64}, lat::Vector{Float64}, n_pt::Int,
                       rlon::Vector{Float64}, rlat::Vector{Float64}, lenrot::Vector{Float64}, n_rot::Int,
                       class_dist::NTuple{3,Float64},
                       xy::Union{Nothing,Matrix{Float64}}, dists::Union{Nothing,Vector{Float64}},
                       weights::Union{Nothing,Vector{Float64}})
	ind = 1;  soma = 0.0;  pesos = 0.0
	nl = length(lenrot)
	for k in 1:n_pt
		(ind < 1 || ind > n_rot) && (ind = 1)
		ok, dmin, seg, xn, yn = _ce_near_a_line(lon[k], lat[k], rlon, rlat, n_rot, ind)
		ok || continue
		ind = seg
		# The lengths vector is one shorter than the line (it is a diff), while C indexed it with the
		# vertex number — clamped here instead of reading past the end.
		L = lenrot[min(ind, nl)]
		peso = L <= class_dist[1] ? 1.0 :
		       L <= class_dist[2] ? 0.5 :
		       L <= class_dist[3] ? 0.25 : 0.0
		soma  += dmin * peso
		pesos += peso
		if xy !== nothing
			xy[k, 1] = xn;  xy[k, 2] = yn
			dists[k] = dmin;  weights[k] = peso
		end
	end
	return pesos == 0.0 ? NaN : soma / pesos
end

# Whether a weighted sum makes sense at all: if NO segment of either line is shorter than 75 km the
# classes are pushed out of reach so every vertex weighs 1 (distmin.c's `regular` test).
function _ce_class_dist(lengths::Vector{Float64}, lenrot::Vector{Float64})
	regular = any(<(75.0), lengths) || any(<(75.0), lenrot)
	return regular ? (25.0, 50.0, 75.0) : (10000.0, 10000.0, 10000.0)
end

# distmin(): the cost. Symmetric — line A to line B and line B to line A, averaged. `full` asks for
# the per-vertex products the residue plots and the Hellinger sigmas need; they describe the FIRST
# direction only, exactly as the MEX filled its extra outputs.
function _ce_distmin(lon::Vector{Float64}, lat::Vector{Float64}, lengths::Vector{Float64},
                     rlon::Vector{Float64}, rlat::Vector{Float64}, lenrot::Vector{Float64};
                     full::Bool = false, class_dist::NTuple{3,Float64} = (NaN, NaN, NaN))
	n_pt = length(lon);  n_rot = length(rlon)
	cd = isnan(class_dist[1]) ? _ce_class_dist(lengths, lenrot) : class_dist
	xy   = full ? zeros(n_pt, 2) : nothing
	dst  = full ? zeros(n_pt)    : nothing
	wgt  = full ? zeros(n_pt)    : nothing
	soma  = _ce_dists_sph(lon, lat, n_pt, rlon, rlat, lenrot, n_rot, cd, xy, dst, wgt)
	soma += _ce_dists_sph(rlon, rlat, n_rot, lon, lat, lengths, n_pt, cd, nothing, nothing, nothing)
	return (soma / 2, xy, dst, wgt)
end

# ---------------------------------------------------------------------------------------------
# Geometry helpers shared by both branches.

# compute_seg_len(): the flat-Earth-per-segment length in km of a polyline given in radians.
function _ce_seglen(lon::Vector{Float64}, lat::Vector{Float64})
	n = length(lon)
	out = Vector{Float64}(undef, max(n - 1, 0))
	for k in 1:n-1
		xd = (lon[k+1] * cos(lat[k+1]) - lon[k] * cos(lat[k])) * _CE_R
		yd = (lat[k+1] - lat[k]) * _CE_R
		out[k] = sqrt(xd*xd + yd*yd)
	end
	return out
end

# Geodetic <-> geocentric latitude, radians (the rotation is a rotation of the SPHERE).
@inline _ce_geocentric(lat::Float64) = atan((1 - _CE_ECC^2) * sin(lat), cos(lat))
@inline _ce_geodetic(lat::Float64)   = atan(sin(lat), (1 - _CE_ECC^2) * cos(lat))

# rot_euler(..., 'rad', 0): rotate points about an Euler pole, no latitude conversion (the caller
# has already made them geocentric). This is the very body fit_pEuler inlines in its hot loop.
function _ce_rot!(rlon::Vector{Float64}, rlat::Vector{Float64},
                  lon::Vector{Float64}, lat::Vector{Float64},
                  p_lon::Float64, p_lat::Float64, omega::Float64)
	p_sin = sin(p_lat);  p_cos = cos(p_lat)
	s_om  = sin(omega);  c_om  = cos(omega)
	@inbounds for k in eachindex(lon)
		s_lat = sin(lat[k]);  c_lat = cos(lat[k])
		s_lon = sin(lon[k] - p_lon);  c_lon = cos(lon[k] - p_lon)
		cc = c_lat * c_lon
		tlon  = atan(c_lat * s_lon, p_sin * cc - p_cos * s_lat)
		s_lat_ = p_sin * s_lat + p_cos * cc
		c_lat_ = sqrt(max(0.0, 1 - s_lat_ * s_lat_))
		s_lon_ = sin(tlon) * c_om + cos(tlon) * s_om     # sin(tlon + omega)
		c_lon_ = cos(tlon) * c_om - sin(tlon) * s_om     # cos(tlon + omega)
		cc_ = c_lat_ * c_lon_
		rlat[k] = asin(clamp(p_sin * s_lat_ - p_cos * cc_, -1.0, 1.0))
		rl = p_lon + atan(c_lat_ * s_lon_, p_sin * cc_ + p_cos * s_lat_)
		rl > pi && (rl -= 2pi)
		rlon[k] = rl
	end
	return
end

# azimuth_geo(..., 'radians'): spherical azimuth, in [0, 2pi). Part of the residue-SIGN algorithm
# (which side of the ridge a point falls on), so it is the polyline maths of the original, not a
# distance a user reads — ported rather than swapped for a geodesic call, which would answer a
# slightly different question.
@inline function _ce_azimuth(lat1::Float64, lon1::Float64, lat2::Float64, lon2::Float64)
	f1 = cos(lat2) * sin(lon2 - lon1)
	f2 = cos(lat1) * sin(lat2)
	f3 = sin(lat1) * cos(lat2) * cos(lon2 - lon1)
	az = atan(f1, f2 - f3)
	az < 0 && (az += 2pi)
	return az
end

# compute_EulerAzim(): the azimuth of the plate-motion direction the pole implies at a point.
@inline function _ce_euler_azim(alat::Float64, alon::Float64, plat::Float64, plon::Float64)
	x = cos(plat)*sin(plon)*sin(alat) - cos(alat)*sin(alon)*sin(plat)
	y = cos(alat)*cos(alon)*sin(plat) - cos(plat)*cos(plon)*sin(alat)
	z = cos(plat)*cos(alat)*sin(alon - plon)
	vlon = -sin(alon)*x + cos(alon)*y
	vlat = -sin(alat)*cos(alon)*x - sin(alat)*sin(alon)*y + cos(alat)*z
	az = pi/2 - atan(vlat, vlon)
	az < 0 && (az += 2pi)
	return az
end

# ---------------------------------------------------------------------------------------------
# Getting the two lines. A name is looked up in the window first (the dialog lists what the window
# holds, same as the Euler rotations dialog); anything that is not a window element is read as a
# file, which is how the MATLAB dialog's two "..." buttons worked.
function _ce_line(scene::Ptr{Cvoid}, spec::AbstractString)::Matrix{Float64}
	isempty(spec) && error("no line given")
	if scene != C_NULL
		segs = _euler_segments(scene, spec)
		if !isempty(segs)
			return length(segs) == 1 ? segs[1] : vcat(segs...)
		end
	end
	isfile(spec) || error("no line named \"$spec\" in this window, and no such file")
	D = GMT.gmtread(spec)
	m = isa(D, Vector) ? vcat([d.data for d in D]...) : D.data
	size(m, 2) < 2 && error("$spec: need at least two columns (lon lat)")
	return Matrix{Float64}(m[:, 1:2])
end

# The distmin search assumes both lines run the same way (see _ce_near_a_line). Mirone tested the
# latitude of the end points and flipped the second line when they disagreed; same test here.
function _ce_same_sense(iso1::Matrix{Float64}, iso2::Matrix{Float64})
	d1 = iso1[end, 2] - iso1[1, 2]
	d2 = iso2[end, 2] - iso2[1, 2]
	return sign(d1) == sign(d2) ? iso2 : iso2[end:-1:1, :]
end

# ---------------------------------------------------------------------------------------------
# Outward displacement (Mirone's OPTcontrol.txt "MIR_OD" entry — a table of latitude vs km). Both
# isochrons are un-rotated to the ridge, pushed out by the tabulated amount along the local
# azimuth and rotated back, which takes the outward displacement of the magnetic anomaly picks out
# of the fit. Hidden config there, hidden config here: an `odfile=` key, or IGMT_OD_FILE in the
# environment. `interp1` is GMT's sample1d; the point offsets are GMT's geodesic (`geod`), never
# hand-rolled sphere maths.
function _ce_od_table(d::Dict{String,String})
	f = _get(d, "odfile")
	isempty(f) && (f = get(ENV, "IGMT_OD_FILE", ""))
	isempty(f) && return nothing
	isfile(f) || error("outward displacement file not found: $f")
	D = GMT.gmtread(f)
	m = isa(D, Vector) ? vcat([x.data for x in D]...) : D.data
	size(m, 2) < 2 && error("$f: outward displacement needs two columns (lat km)")
	return Matrix{Float64}(m[:, 1:2])
end

function _ce_sample1d(x::Vector{Float64}, y::Vector{Float64}, xi::Vector{Float64})
	p = sortperm(xi)
	R = GMT.gmt("sample1d -T" * join(string.(xi[p]), ','), [x y])
	D = isa(R, Vector) ? R[1].data : R.data
	out = zeros(length(xi))
	out[p] = D[:, 2]
	lo, hi = extrema(x)
	@inbounds for k in eachindex(xi)
		(xi[k] < lo || xi[k] > hi) && (out[k] = 0.0)     # interp1's extrapval = 0
	end
	return out
end

# rot_euler(..., 'rad', -1): geodetic in, geodetic out, rotation done on geocentric latitudes.
function _ce_rot_geodetic(lon::Vector{Float64}, lat::Vector{Float64}, plon, plat, ang)
	rlon = similar(lon);  rlat = similar(lat)
	_ce_rot!(rlon, rlat, lon, _ce_geocentric.(lat), plon, plat, ang)
	rlat .= _ce_geodetic.(rlat)
	return rlon, rlat
end

# Move each point `shift` km along the azimuth that separates it from its un-rotated position.
function _ce_push_out(lon::Vector{Float64}, lat::Vector{Float64},
                      rlon::Vector{Float64}, rlat::Vector{Float64}, shifts::Vector{Float64})
	R2D = 180 / pi
	olon = copy(rlon);  olat = copy(rlat)
	for k in eachindex(rlon)
		shifts[k] == 0 && continue
		# vdist gives the azimuth from the picked point to its rotated image, vreckon the point that
		# far along it. GMT's geodesic answers both (inverse problem, then direct problem).
		inv = GMT.geod([lon[k]*R2D lat[k]*R2D], [rlon[k]*R2D rlat[k]*R2D])
		az  = isa(inv, Tuple) ? inv[2][1] : inv[1]
		dir = GMT.geod([rlon[k]*R2D rlat[k]*R2D], az, shifts[k] * 1000)
		p   = isa(dir, Tuple) ? dir[1] : dir
		pm  = isa(p, Matrix) ? p : p.data
		olon[k] = pm[1, 1] / R2D;  olat[k] = pm[1, 2] / R2D
	end
	return olon, olat
end

function _ce_outward(lon1, lat1, lon2, lat2, plon, plat, pang, OD)
	D2R = pi / 180
	rlon1, rlat1 = _ce_rot_geodetic(lon1, lat1, plon, plat,  pang)
	rlon2, rlat2 = _ce_rot_geodetic(lon2, lat2, plon, plat, -pang)
	s1 = _ce_sample1d(OD[:, 1] .* D2R, OD[:, 2], rlat1)
	s2 = _ce_sample1d(OD[:, 1] .* D2R, OD[:, 2], rlat2)
	rlon1, rlat1 = _ce_push_out(lon1, lat1, rlon1, rlat1, s1)
	rlon2, rlat2 = _ce_push_out(lon2, lat2, rlon2, rlat2, s2)
	o1lon, o1lat = _ce_rot_geodetic(rlon1, rlat1, plon, plat, -pang)   # back to the "original" frame
	o2lon, o2lat = _ce_rot_geodetic(rlon2, rlat2, plon, plat,  pang)
	return o1lon, o1lat, o2lon, o2lat
end

# ---------------------------------------------------------------------------------------------
# calca_pEuler's front half: everything that does not depend on the search box — the geocentric
# radians copies of both lines, their segment lengths, the starting rotation and its residue.
function _ce_setup(iso1::Matrix{Float64}, iso2::Matrix{Float64},
                   plon::Float64, plat::Float64, pang::Float64, OD)
	D2R = pi / 180
	lon1 = iso1[:, 1] .* D2R;  lat1 = iso1[:, 2] .* D2R
	lon2 = iso2[:, 1] .* D2R;  lat2 = iso2[:, 2] .* D2R
	if OD !== nothing
		lon1, lat1, lon2, lat2 = _ce_outward(lon1, lat1, lon2, lat2, plon*D2R, plat*D2R, pang*D2R, OD)
	end
	lat1 = _ce_geocentric.(lat1);  lat2 = _ce_geocentric.(lat2)
	lenRot2 = _ce_seglen(lon2, lat2)
	rlon = similar(lon1);  rlat = similar(lat1)
	_ce_rot!(rlon, rlat, lon1, lat1, plon*D2R, plat*D2R, pang*D2R)
	lenRot1 = _ce_seglen(rlon, rlat)
	cd = _ce_class_dist(lenRot2, lenRot1)
	area0, = _ce_distmin(lon2, lat2, lenRot2, rlon, rlat, lenRot1; class_dist = cd)
	return (lon1 = lon1, lat1 = lat1, lon2 = lon2, lat2 = lat2,
	        s_lat = sin.(lat1), c_lat = cos.(lat1),
	        lenRot1 = lenRot1, lenRot2 = lenRot2, cd = cd,
	        rlon = rlon, rlat = rlat, area0 = area0)
end

# The search box: nInt points spanning the range CENTRED on the starting value (Mirone's linspace),
# in radians. Latitudes that walk over a pole are dropped, as the original sanitises p_lat.
function _ce_axis(centre::Float64, range::Float64, n::Int)
	n <= 1 && return [centre * pi / 180]
	d = range / 2
	return [(centre - d + (k - 1) * range / (n - 1)) * pi / 180 for k in 1:n]
end

# ---------------------------------------------------------------------------------------------
# Live state of a run. The worker task only ever writes these plain Refs; the main-thread Timer
# reads them and is the ONLY side that ccalls into the window.
const _CE_STOP    = Ref(false)
const _CE_RUNNING = Ref(false)
const _CE_PROG    = Ref((0, 1))                                  # (longitudes done, total)
const _CE_BEST    = Ref((NaN, NaN, NaN, NaN))                    # lon, lat, angle, residue

# fit_pEuler: the search. One chunk of the longitude axis, run by one task. Everything the loop
# needs is precomputed by _ce_setup; nothing here touches the window (worker threads never ccall).
function _ce_fit_chunk!(rng::UnitRange{Int}, st, p_lon::Vector{Float64}, p_lat::Vector{Float64},
                        p_omeg::Vector{Float64}, resid::Array{Float64,3}, saveresid::Bool,
                        best::Base.RefValue{NTuple{4,Float64}}, lk::ReentrantLock,
                        done::Threads.Atomic{Int})
	n = length(st.lon1);  nLat = length(p_lat);  nAng = length(p_omeg)
	s_lon  = Vector{Float64}(undef, n);  cc     = Vector{Float64}(undef, n)
	tlon   = Vector{Float64}(undef, n);  s_lat_ = Vector{Float64}(undef, n)
	c_lat_ = Vector{Float64}(undef, n)
	rlon   = Vector{Float64}(undef, n);  rlat   = Vector{Float64}(undef, n)
	s_lat = st.s_lat;  c_lat = st.c_lat
	loc = best[]
	published = loc
	for i in rng
		_CE_STOP[] && break
		@inbounds for k in 1:n
			dl = st.lon1[k] - p_lon[i]
			s_lon[k] = sin(dl)
			cc[k] = c_lat[k] * cos(dl)
		end
		for j in 1:nLat
			p_sin = sin(p_lat[j]);  p_cos = cos(p_lat[j])
			@inbounds for k in 1:n
				tlon[k] = atan(c_lat[k] * s_lon[k], p_sin * cc[k] - p_cos * s_lat[k])
				s = p_sin * s_lat[k] + p_cos * cc[k]
				s_lat_[k] = s
				c_lat_[k] = sqrt(max(0.0, 1 - s * s))
			end
			for m in 1:nAng
				om = p_omeg[m]
				@inbounds for k in 1:n
					s_lon_ = sin(tlon[k] + om);  c_lon_ = cos(tlon[k] + om)
					cc_ = c_lat_[k] * c_lon_
					rlat[k] = asin(clamp(p_sin * s_lat_[k] - p_cos * cc_, -1.0, 1.0))
					rl = p_lon[i] + atan(c_lat_[k] * s_lon_, p_sin * cc_ + p_cos * s_lat_[k])
					rl > pi && (rl -= 2pi)
					rlon[k] = rl
				end
				area, = _ce_distmin(st.lon2, st.lat2, st.lenRot2, rlon, rlat, st.lenRot1;
				                    class_dist = st.cd)
				saveresid && (resid[j, i, m] = area)
				area < loc[1] && (loc = (area, Float64(i), Float64(j), Float64(m)))
			end
		end
		Threads.atomic_add!(done, 1)
		if loc[1] < published[1]                          # publish this chunk's improvement
			lock(lk) do
				b = best[]
				if loc[1] < b[1] || (loc[1] == b[1] && (loc[2], loc[3], loc[4]) < (b[2], b[3], b[4]))
					best[] = loc
					_CE_BEST[] = (p_lon[Int(loc[2])] * 180/pi, p_lat[Int(loc[3])] * 180/pi,
					              p_omeg[Int(loc[4])] * 180/pi, loc[1])
				end
			end
			published = loc
		end
	end
	return
end

# The whole search. Same answer as the sequential original: a candidate must be STRICTLY better
# than the running best (which starts at the starting pole's own residue) and ties keep the
# earliest (i, j, k), so chunking the longitude axis never changes the result.
function _ce_fit(st, p_lon::Vector{Float64}, p_lat::Vector{Float64}, p_omeg::Vector{Float64},
                 pang_ini::Float64, saveresid::Bool)
	nLon = length(p_lon);  nLat = length(p_lat);  nAng = length(p_omeg)
	resid = saveresid ? fill(NaN, nLat, nLon, nAng) : Array{Float64,3}(undef, 0, 0, 0)
	best  = Ref((st.area0, 1.0, 1.0, 0.0))
	lk    = ReentrantLock()
	done  = Threads.Atomic{Int}(0)
	_CE_PROG[] = (0, nLon)

	nt = min(max(Threads.nthreads() - 1, 1), nLon)
	if nt == 1
		# Single thread: the loop must hand the event loop back regularly or neither the progress
		# bar nor the STOP button would ever be seen. One yield per longitude step is plenty.
		for i in 1:nLon
			_CE_STOP[] && break
			_ce_fit_chunk!(i:i, st, p_lon, p_lat, p_omeg, resid, saveresid, best, lk, done)
			_CE_PROG[] = (done[], nLon)
			yield()
		end
	else
		# `wait` on the spawned chunks yields, so the coordinating task never pins a thread.
		bnds = [(round(Int, (k - 1) * nLon / nt) + 1):round(Int, k * nLon / nt) for k in 1:nt]
		tasks = [Threads.@spawn _ce_fit_chunk!(r, st, p_lon, p_lat, p_omeg, resid, saveresid, best, lk, done)
		         for r in bnds if !isempty(r)]
		for t in tasks
			wait(t)
			_CE_PROG[] = (done[], nLon)
		end
	end
	_CE_PROG[] = (nLon, nLon)

	a, i, j, k = best[]
	lon_bf = p_lon[Int(i)] * 180/pi
	lat_bf = p_lat[Int(j)] * 180/pi
	ang_bf = k > 0 ? p_omeg[Int(k)] * 180/pi : pang_ini      # nothing beat the starting angle
	return (lon_bf, lat_bf, ang_bf, a, resid)
end

# ---------------------------------------------------------------------------------------------
# resid_along_isoca: the SIGNED residues of the starting pole, point by point, for the "Plot
# residues only" box. The sign says which side of the other line a point fell on, decided by
# comparing the azimuth to the nearest point with the azimuth the Euler pole implies there;
# points whose two azimuths agree with neither (about 60-120 degrees off) are dropped — those are
# the ones edging fracture zones.
function _ce_resid_along(st, plon::Float64, plat::Float64, threshold::Float64 = 0.25)
	R2D = 180 / pi
	pla = plat / R2D;  plo = plon / R2D

	function side(xy, dists, w, olon, olat, flip::Bool)
		keep = findall(>=(threshold), w)                      # ad-hoc criterium, Mirone's own
		isempty(keep) && return (Float64[], Float64[])
		xy = xy[keep, :];  dists = dists[keep]
		olon = olon[keep];  olat = olat[keep]
		out_lat = Float64[];  out_d = Float64[]
		for k in eachindex(dists)
			azp = _ce_azimuth(xy[k, 2], xy[k, 1], olat[k], olon[k]) * R2D
			aze = _ce_euler_azim(xy[k, 2], xy[k, 1], pla, plo) * R2D
			difa = abs(azp - aze)
			s = difa < 60 ? 1.0 : (difa > 120 ? -1.0 : 0.0)
			s == 0 && continue
			flip && (s = -s)                                  # the B pass has the lines swapped
			push!(out_d, dists[k] * s)
			push!(out_lat, _ce_geodetic(xy[k, 2]) * R2D)
		end
		return (out_lat, out_d)
	end

	_, xyA, dA, wA = _ce_distmin(st.lon2, st.lat2, st.lenRot2, st.rlon, st.rlat, st.lenRot1;
	                             full = true, class_dist = st.cd)
	latA, resA = side(xyA, dA, wA, st.lon2, st.lat2, false)

	_, xyB, dB, wB = _ce_distmin(st.rlon, st.rlat, st.lenRot1, st.lon2, st.lat2, st.lenRot2;
	                             full = true, class_dist = st.cd)
	latB, resB = side(xyB, dB, wB, st.rlon, st.rlat, true)

	lat = vcat(latA, latB);  dists = vcat(resA, resB)
	p = sortperm(lat)
	return (lat[p], dists[p])
end

# ---------------------------------------------------------------------------------------------
# The residues cube: every rotation's cost, as a (lat, lon, angle) volume. netCDF through GMT's own
# 3-D grid writer; VTK written here, byte for byte the layout of Mirone's write_vtk (big-endian
# binary rectilinear grid) — except that the scalars go out x-fastest, which is the order the
# DIMENSIONS line promises (MATLAB's fwrite of a column-major slice wrote them y-fastest).
function _ce_write_netcdf(fname::AbstractString, lon::Vector{Float64}, lat::Vector{Float64},
                          ang::Vector{Float64}, resid::Array{Float64,3})
	cube = GMT.mat2grid(Float32.(resid), x = lon, y = lat, v = ang)
	GMT.gmtwrite(String(fname), cube)
	return
end

function _ce_write_vtk(fname::AbstractString, lon::Vector{Float64}, lat::Vector{Float64},
                       ang::Vector{Float64}, resid::Array{Float64,3})
	nx = length(lon);  ny = length(lat);  nz = length(ang)
	open(String(fname), "w") do io
		write(io, "# vtk DataFile Version 2.0\n")
		write(io, "converted from A B\n")
		write(io, "BINARY\nDATASET RECTILINEAR_GRID\n")
		write(io, "DIMENSIONS $nx $ny $nz\n")
		write(io, "X_COORDINATES $nx float\n")
		for v in lon;  write(io, hton(Float32(v)));  end
		write(io, "Y_COORDINATES $ny float\n")
		for v in lat;  write(io, hton(Float32(v)));  end
		write(io, "Z_COORDINATES $nz float\n")
		for v in ang;  write(io, hton(Float32(v)));  end
		write(io, "POINT_DATA $(nx * ny * nz)\n")
		write(io, "SCALARS dono float 1\nLOOKUP_TABLE default\n")
		for k in 1:nz
			Z = permutedims(view(resid, :, :, k))            # (lon, lat) => longitude varies fastest
			for v in Z;  write(io, hton(Float32(v)));  end
		end
	end
	return
end

# ---------------------------------------------------------------------------------------------
# What the window gets back: the moving isochron rotated by a pole, as plain geodetic degrees.
function _ce_rotated_line(iso1::Matrix{Float64}, plon::Float64, plat::Float64, pang::Float64)
	D2R = pi / 180
	rlon, rlat = _ce_rot_geodetic(iso1[:, 1] .* D2R, iso1[:, 2] .* D2R, plon*D2R, plat*D2R, pang*D2R)
	return hcat(rlon ./ D2R, rlat ./ D2R)
end

# One tab-separated line of the five numbers the dialog shows: pole lon/lat/angle, starting residue,
# best-fit residue. Empty fields are left alone by the dialog.
function _ce_fields(best::NTuple{4,Float64}, stres::Float64)
	f(x, n) = isnan(x) ? "" : _ffmt(x, n)
	return join((f(best[1], 3), f(best[2], 3), f(best[3], 3), f(stres, 4), f(best[4], 4)), '\t')
end

# A second sink for the same push, so a dialog living in another copy of the library can be driven
# too — the test suite builds its dialogs inside gmtvtk_test.dll, which cannot see the production
# dll's channel (the same reason `_euler_last_result` is mirrored). nothing in normal use.
const _CE_EXTRA_PUSH = Ref{Union{Nothing,Function}}(nothing)

function _ce_push_raw(cur::Int, mx::Int, txt::AbstractString)
	try
		ccall(_fn(:gmtvtk_compute_euler_progress), Cvoid, (Cint, Cint, Cstring),
		      Cint(cur), Cint(mx), String(txt))
	catch
	end
	f = _CE_EXTRA_PUSH[]
	f === nothing || try f(cur, mx, String(txt)) catch end
	return
end

_ce_push(cur::Int, mx::Int, best::NTuple{4,Float64}, stres::Float64) =
	_ce_push_raw(cur, mx, _ce_fields(best, stres))

# ---------------------------------------------------------------------------------------------
# push_compute_CB: read what the dialog holds, then branch the three ways it can branch.
function _ce_read(scene::Ptr{Cvoid}, d::Dict{String,String})
	iso1 = _ce_line(scene, _get(d, "line1"))
	iso2 = _ce_line(scene, _get(d, "line2"))
	(size(iso1, 1) < 2 || size(iso2, 1) < 2) && error("both lines need at least two vertices")
	iso2 = _ce_same_sense(iso1, iso2)
	plon = _euler_num(d, "polelon");  plat = _euler_num(d, "polelat");  pang = _euler_num(d, "poleang")
	(plon === nothing || plat === nothing || pang === nothing) &&
		error("I need a first guess of the Euler pole you are seeking for (Starting Pole Section)")
	return (iso1, iso2, plon, plat, pang)
end

function _ce_start(scene::Ptr{Cvoid}, d::Dict{String,String})::Cint
	_CE_RUNNING[] && error("a Compute Euler pole run is already going — press STOP first")
	iso1, iso2, plon, plat, pang = _ce_read(scene, d)
	st = _ce_setup(iso1, iso2, plon, plat, pang, _ce_od_table(d))

	if _on(d, "plotres")            # "Plot residues only": no pole is computed
		lat, dists = _ce_resid_along(st, plon, plat)
		isempty(lat) && error("no residue survived the weight and azimuth filters")
		xyplot(lat, dists; name = "residues", title = "Residues along isoc",
		       xlabel = "Latitude", ylabel = "Residue (km)")
		_euler_result("$(length(lat)) signed residues plotted for pole $plon/$plat/$pang")
		_ce_push(-1, 1, (NaN, NaN, NaN, NaN), st.area0)
		return Cint(1)
	end
	_on(d, "hellinger") && return _ce_hellinger(scene, d, iso1, iso2, plon, plat, pang, st)
	return _ce_launch(scene, d, iso1, iso2, plon, plat, pang, st)
end

# STOP. The search polls this between longitudes and returns its best-so-far — which is exactly
# what the dialog is already showing, since it has been updated live all along.
function _ce_stop()::Cint
	_CE_STOP[] = true
	return Cint(1)
end

# The search itself never runs on the calling (Qt event) thread: the callback returns at once and
# the window stays usable, the way nswing.jl runs its worker. Progress, the running best pole and
# everything that touches the scene happen in the main-thread Timer below.
function _ce_launch(scene::Ptr{Cvoid}, d::Dict{String,String}, iso1::Matrix{Float64},
                    iso2::Matrix{Float64}, plon::Float64, plat::Float64, pang::Float64, st)::Cint
	nlon = parse(Int, _get(d, "nlon", "61"));  nlat = parse(Int, _get(d, "nlat", "41"))
	nang = parse(Int, _get(d, "nang", "21"))
	p_lon  = _ce_axis(plon, something(_euler_num(d, "lonrange"), 30.0), nlon)
	p_lat  = _ce_axis(plat, something(_euler_num(d, "latrange"), 20.0), nlat)
	p_omeg = _ce_axis(pang, something(_euler_num(d, "angrange"),  1.0), nang)
	filter!(x -> -pi/2 <= x <= pi/2, p_lat)          # sanitize: no pole beyond the N/S poles
	isempty(p_lat) && error("the latitude range walks over the poles")

	saveresid = !isempty(_get(d, "residfile"))
	out = Ref{Any}(nothing)
	_CE_STOP[] = false;  _CE_RUNNING[] = true
	_CE_PROG[] = (0, length(p_lon));  _CE_BEST[] = (NaN, NaN, NaN, NaN)

	body = () -> begin
		try
			out[] = _ce_fit(st, p_lon, p_lat, p_omeg, pang, saveresid)
		catch e
			out[] = e
		finally
			_CE_RUNNING[] = false
		end
	end
	tsk = Threads.nthreads() > 1 ? Threads.@spawn(body()) : @async(body())
	errormonitor(tsk)

	tm = Timer(0.15; interval = 0.15) do t
		try
			if _CE_RUNNING[]
				cur, mx = _CE_PROG[]
				_ce_push(cur, mx, _CE_BEST[], st.area0)
			else
				close(t)
				_ce_finish(scene, d, st, iso1, iso2, plon, plat, pang, p_lon, p_lat, p_omeg, out[])
			end
		catch e
			close(t);  _CE_RUNNING[] = false
			_ce_push(-1, 1, _CE_BEST[], st.area0)
			_tool_failed(scene, "Compute Euler pole", e)
		end
	end
	return Cint(1)
end

# End of a run, on the main thread: the residues cube, the fitted line, the answer text, and — if
# "Loop until..." is ticked and this run actually improved on its own starting pole — the next run
# with the pole just found as the new starting point (Mirone's push_reciclePole + re-Compute).
function _ce_finish(scene::Ptr{Cvoid}, d::Dict{String,String}, st, iso1::Matrix{Float64},
                    iso2::Matrix{Float64}, plon::Float64, plat::Float64, pang::Float64,
                    p_lon::Vector{Float64}, p_lat::Vector{Float64}, p_omeg::Vector{Float64}, out)
	if out isa Exception
		_ce_push(-1, 1, (NaN, NaN, NaN, NaN), st.area0)
		_euler_result("Compute Euler pole failed: $(sprint(showerror, out))")
		_tool_failed(scene, "Compute Euler pole", out)
		return
	end
	lon_bf, lat_bf, ang_bf, area, resid = out
	R2D = 180 / pi

	residfile = _get(d, "residfile")
	if !isempty(residfile) && !isempty(resid)
		lonv = p_lon .* R2D;  latv = p_lat .* R2D;  angv = p_omeg .* R2D
		age = something(_euler_num(d, "age"), 0.0)
		age > 0 && (angv = angv ./ age)              # -A<age>: store angular velocity instead
		if lowercase(_get(d, "residfmt", "nc")) == "vtk"
			_ce_write_vtk(residfile, lonv, latv, angv, resid)
		else
			_ce_write_netcdf(residfile, lonv, latv, angv, resid)
			# The cube the MATLAB version could only write to disk can be looked at right here.
			_on(d, "showcube") && _load_cube_element(scene, String(residfile),
			                                         "Euler residues (" * basename(residfile) * ")", false)
		end
	end

	# The fitted line lands as a named element next to the isochrons it was fitted to; the sources
	# stay visible — a reconstruction is read against the thing it was reconstructed from.
	if scene != C_NULL
		xy = _ce_rotated_line(iso1, lon_bf, lat_bf, ang_bf)
		nm = "Fitted Line (" * _ffmt(lon_bf, 2) * "/" * _ffmt(lat_bf, 2) * "/" * _ffmt(ang_bf, 3) * ")"
		try
			_add_dataset_to_scene(scene, GMT.mat2ds(xy), nm; groupName = "Compute Euler pole")
			ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
		catch
		end
	end

	best = (lon_bf, lat_bf, ang_bf, area)
	_CE_BEST[] = best
	_ce_push(-1, length(p_lon), best, st.area0)
	stopped = _CE_STOP[] ? " (stopped early)" : ""
	_euler_result("pole $(_ffmt(lon_bf,3)) $(_ffmt(lat_bf,3)) $(_ffmt(ang_bf,3))   " *
	              "residue $(_ffmt(area,4)) km (start $(_ffmt(st.area0,4)))$stopped")

	# Loop mode: go again from the pole just found, and stop when a run stops improving.
	if _on(d, "loop") && !_CE_STOP[] && area < st.area0
		d2 = copy(d)
		d2["polelon"] = string(lon_bf);  d2["polelat"] = string(lat_bf);  d2["poleang"] = string(ang_bf)
		try
			_ce_start(scene, d2)
		catch e
			_tool_failed(scene, "Compute Euler pole (loop)", e)
		end
	end
	return
end

# ---------------------------------------------------------------------------------------------
# The Hellinger branch. GMT.hellinger_auto does the whole method (Douglas-Peucker segmentation of
# the moving isochron, the segment flags for both lines, the amoeba fit, the covariance, the
# confidence volume and the 95% error ellipse); this side only feeds it and lands the answers.
# Residues before and after come from the SAME `distmin` cost the other branch minimises, so the
# two methods' "St Residue"/"BF Residue" numbers mean the same thing and can be compared.
function _ce_hellinger(scene::Ptr{Cvoid}, d::Dict{String,String}, iso1::Matrix{Float64},
                       iso2::Matrix{Float64}, plon::Float64, plat::Float64, pang::Float64, st)::Cint
	dptol = something(_euler_num(d, "dptol"), 8.0)          # km, the DP tolerance
	H = GMT.hellinger_auto(plon, plat, pang, iso1, iso2; dp_tol = dptol,
	                       force_pole = _on(d, "forcepole"))

	# Same cost function as the brute-force branch, evaluated at the pole Hellinger found.
	st2   = _ce_setup(iso1, iso2, H.along, H.alat, H.rho, nothing)
	best  = (H.along, H.alat, H.rho, st2.area0)
	_CE_BEST[] = best

	if scene != C_NULL
		xy = _ce_rotated_line(iso1, H.along, H.alat, H.rho)
		grp = "Compute Euler pole (Hellinger)"
		try
			_add_dataset_to_scene(scene, GMT.mat2ds(xy),
			                      "Fitted Line (" * _ffmt(H.along, 2) * "/" * _ffmt(H.alat, 2) * "/" *
			                      _ffmt(H.rho, 3) * ")"; groupName = grp)
			if _on(d, "colorseg")           # the automatic segmentation, as Mirone's broken_dp lines
				_add_dataset_to_scene(scene, GMT.mat2ds(H.segments), "DP segmentation"; groupName = grp)
				_add_dataset_to_scene(scene, GMT.mat2ds(H.segments_rot), "DP segmentation rotated";
				                      groupName = grp)
			end
			if _on(d, "ellipse") && !isempty(H.ellipse_lon)
				# The 95% confidence region of the POLE: a geographic outline, so it belongs on the
				# map next to the pole it describes (Mirone could only draw it in a plain XY window).
				_add_dataset_to_scene(scene, GMT.mat2ds(hcat(H.ellipse_lon, H.ellipse_lat)),
				                      "Pole 95% error ellipse"; groupName = grp)
			end
			ccall(_fn(:gmtvtk_unfold_scene_objects_h), Cvoid, (Ptr{Cvoid},), scene)
		catch
		end
	end

	txt = "pole $(_ffmt(H.along,3)) $(_ffmt(H.alat,3)) $(_ffmt(H.rho,3))   " *
	      "residue $(_ffmt(st2.area0,4)) km (start $(_ffmt(st.area0,4)))   " *
	      "conf. volume $(_ffmt(H.vol,4)) km3"
	_on(d, "showstats") && (txt *= "\n" * H.stats)
	_euler_result(txt)
	# The dialog's own boxes: pole, both residues, and the volume in the field the MATLAB dialog
	# recycled for it.
	_ce_push_raw(-1, 1, _ce_fields(best, st.area0) * "\t" * _ffmt(H.vol, 4))
	return Cint(1)
end
