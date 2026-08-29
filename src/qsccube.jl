# =============================================================================================
# THE CUBE VIEW MODE'S PROJECTION — PROJ's quadrilateralized spherical cube, sampled once
# =============================================================================================
# The viewer's fourth view mode (2D / 3D / Globe / Cube) wraps lon/lat/z onto a CUBE instead of a
# sphere, each face carrying `+proj=qsc`. QSC and not `+proj=s2`: QSC is EQUAL-AREA (a constant area
# scale of 6/pi over the whole planet), so a square kilometre draws as the same number of pixels
# wherever it lands on the cube, while s2's default UVtoST warp is Google's cell-size heuristic —
# neither equal-area nor conformal — and emits [0,1] texture coordinates instead of metres.
#
# WHY THIS FILE EXISTS. The C++ viewer links no PROJ and must never grow a hand-rolled lookalike of
# one (SACRED_LAW.md; the standing rule that a projection/geodesic comes from GMT's own maths). PROJ
# is already here, through GDAL, on this side: `GMT.lonlat2xy` / `GMT.xy2lonlat` take an arbitrary
# `+proj=` string. So the projection is computed HERE, once per session, and handed over as a table.
#
# WHAT IS TABULATED is the smallest invariant piece of QSC — the face-local warp
#
#     W : (a, b)  ->  (u, v),   both in [-1, 1]^2
#
# where (a, b) are a cube face's GNOMONIC coordinates (where the ray from the planet's centre pierces
# the flat face) and (u, v) are QSC's own face coordinates (its metres divided by the sphere radius,
# which puts a full face at exactly [-1,1]^2 — verified: lon=45,lat=0 lands on x = R to the last
# digit, and the true face corner lon=45, lat=35.26439 on (R, R)).
#
# ONE table serves all six faces. PROJ treats every cube side alike, so the six lat_0/lon_0 settings
# differ only by which axes the face's (u, v) run along — checked here against all six to 1e-15, and
# encoded on the C++ side as six integer frames (kCubeFaces, 10_geometry.cpp).
#
# AND ONE OCTANT SERVES THE WHOLE FACE. W is C0 but NOT C1: PROJ's QSC picks a quadrant inside each
# face, and the derivative jumps across the face DIAGONALS (measured: du/db at a = 0.6 goes 0.135 to
# -0.304 as b crosses 0.6). Sampling on a plain (a,b) grid cuts that kink at 45 degrees, so cells
# straddling a diagonal interpolate across a corner and the error stops falling like h^2 — a 257x257
# square table is off by 2.3e-4 (1.3e-2 degrees of arc, ~1.5 km), and bicubic is WORSE, overshooting
# the kink. W's own symmetries fix it exactly:
#
#     W(-a,  b) = (-u,  v)        W(a, -b) = ( u, -v)        W(b, a) = (v, u)
#
# (all three verified against PROJ to machine precision). Folding every point into `0 <= b <= a` and
# tabulating on `(a, b/a)` puts the kink exactly on the `b/a = 1` EDGE of the table: the interior is
# smooth, the error is back to h^2 — 2.8e-6, i.e. 1.6e-4 degrees, about 18 m — and the table is a
# quarter the size. The viewer folds the same way on lookup (cubeWarpApply, 10_geometry.cpp).

const _QSC_SRS  = "+proj=qsc +ellps=sphere +lat_0=0 +lon_0=0 +units=m +no_defs"
const _QSC_R    = 6370997.0        # the +ellps=sphere radius, i.e. the unit QSC's metres divide by
const _QSC_N    = 257              # samples per side of the warp table
const _QSC_DONE = Ref(false)

"""
    _qsc_warp_tables(n::Int = _QSC_N) -> (fwd::Vector{Float64}, inv::Vector{Float64})

Sample PROJ's QSC face warp and its inverse over the folded first octant, each on a regular `n`x`n`
grid of `[0,1]^2`, laid out as the viewer reads them: `tab[(j*n + i)*2 + c]`, `i` running over the
first coordinate, `j` over the second, `c` = 0/1 for the two output components.

- `fwd` is sampled at `(a, b/a)` and holds gnomonic -> QSC `(u, v)`.
- `inv` is sampled at `(u, v/u)` and holds QSC -> gnomonic `(a, b)`.

Both are computed on the FRONT face (`lat_0 = lon_0 = 0`), whose frame is `n = +X`, `e1 = +Y`,
`e2 = +Z` — so a face point's direction is simply `(1, a, b)` and the gnomonic coordinates of a
direction are `(dy/dx, dz/dx)`.
"""
function _qsc_warp_tables(n::Int = _QSC_N)
	t = collect(range(0.0, 1.0, length=n))       # both axes of both tables run over [0,1]

	# --- forward: (a, s=b/a) -> lon/lat on the front face -> qsc metres -> /R ------------------
	ll = Matrix{Float64}(undef, n * n, 2)
	k = 0
	for j in 1:n, i in 1:n                       # i = a (fastest), j = s — the C layout above
		a = t[i];  b = a * t[j]                  # 0 <= b <= a: the first octant
		r = sqrt(1.0 + a * a + b * b)            # |(1, a, b)|
		k += 1
		ll[k, 1] = atand(a, 1.0)                 # lon
		ll[k, 2] = asind(b / r)                  # lat
	end
	uv = Matrix{Float64}(GMT.lonlat2xy(ll, t_srs=_QSC_SRS) ./ _QSC_R)

	# --- inverse: (u, S=v/u)*R -> lon/lat -> that direction's gnomonic (a,b) -------------------
	xy = Matrix{Float64}(undef, n * n, 2)
	k = 0
	for j in 1:n, i in 1:n
		k += 1
		xy[k, 1] = t[i] * _QSC_R
		xy[k, 2] = t[i] * t[j] * _QSC_R
	end
	lli = GMT.xy2lonlat(xy, s_srs=_QSC_SRS)
	ab  = Matrix{Float64}(undef, n * n, 2)
	for k in 1:(n * n)
		lon, lat = lli[k, 1], lli[k, 2]
		cl = cosd(lat)
		dx, dy, dz = cl * cosd(lon), cl * sind(lon), sind(lat)
		# Front face: a = e1-component / normal-component, b = e2-component / normal-component.
		# dx is the normal component and is >= 1/sqrt(3) everywhere on this face, so it never vanishes.
		ab[k, 1] = dx != 0.0 ? dy / dx : 0.0
		ab[k, 2] = dx != 0.0 ? dz / dx : 0.0
	end

	return _qsc_flatten(uv), _qsc_flatten(ab)
end

# (npts x 2) -> the interleaved Vector{Float64} the C side indexes as tab[(j*n + i)*2 + c].
function _qsc_flatten(M::Matrix{Float64})
	v = Vector{Float64}(undef, 2 * size(M, 1))
	@inbounds for k in 1:size(M, 1)
		v[2k - 1] = M[k, 1];  v[2k] = M[k, 2]
	end
	return v
end

"""
    _push_qsc_warp()

Compute the QSC warp tables (once per session) and hand them to the viewer library. Called from
`_ensure_callbacks` (eventloop.jl) alongside the callback registrations, so the Cube view mode is
ready before any window can offer it. Costs one GDAL call in each direction, a few tens of ms.
"""
function _push_qsc_warp()
	_QSC_DONE[] && return
	fwd, inv = _qsc_warp_tables()
	ccall(_fn(:gmtvtk_set_cube_warp), Cvoid, (Cint, Ptr{Cdouble}, Ptr{Cdouble}),
	      Cint(_QSC_N), fwd, inv)
	_QSC_DONE[] = true
	return
end
