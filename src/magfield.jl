# magfield.jl — Geophysics > Magnetics > "Magnetic field lines (3-D)".
#
# THE FIELD ITSELF IS THE IGRF TOOL'S FIELD. Every evaluation here goes through GMT.jl's `magref`
# (mgd77magref), the SAME entry point igrf.jl's point/grid/file paths use — SACRED_LAW.md, "same
# operation, ALWAYS same function": there is no second spherical-harmonic synthesis in this package,
# and the date box below picks the IGRF generation exactly the way the IGRF dialog's date box does.
# `magref` is NOT limited to the Earth's surface: the altitude column is free (verified out to
# 1e5 km), which is what makes tracing a field line out to several Earth radii possible at all.
# Its validity window is the model's: 1900-2030 (mgd77magref errors outside it), so the dialog's
# date box carries those limits.
#
# WHAT IS TRACED. Field lines are streamlines of B: dx/ds = B/|B|. Seeds sit a hair above the
# surface on latitude rings in both hemispheres, and each is integrated OUTWARD (the sign that
# raises r on the first step) until the line comes back down to the surface, escapes past the
# requested maximum radius, or runs out of steps. Midpoint (RK2) with a step proportional to r:
# fine near the Earth where the field turns quickly, coarse out where it barely does.
#
# ONE magref CALL PER SUBSTEP, NOT PER LINE. Every live line is stepped in lockstep, so a substep
# is a single call with an N-row matrix: measured 1.4 ms for 240 rows against ~1.3 ms for one, i.e.
# the cost is the call, not the points. Tracing 100 lines is therefore ~2 evaluations per step
# instead of 200.
#
# GEOMETRY. Spherical Earth of radius `_MAG_R` (6371.2 km, the IGRF reference radius): lon/lat fed
# to magref are geocentric, and the local (north, east, down) triad the X/Y/Z components are turned
# into is the geocentric one. magref's own triad is geodetic, so the two differ by the ellipsoid's
# deflection (<= 0.19 deg, at mid-latitudes). That is a tilt far below the width of a drawn tube and
# buys a tool with no geodetic/geocentric conversion in its inner loop; nothing here is a
# measurement, it is a picture.

const _MAG_R = 6371.2                  # km — IGRF reference radius; the world unit is this radius

# Lines last traced, kept between the C side's size query and its copy (same two-phase protocol as
# the fault demo's mesh callback: ask for the counts, allocate, ask again for the points).
const _MAG_LINES = Ref{Vector{Matrix{Float64}}}(Matrix{Float64}[])
# The Earth texture, likewise: packed once, handed over on the second call.
const _MAG_TEX = Ref{Tuple{Vector{UInt8},Int,Int,Int}}((UInt8[], 0, 0, 0))

# B in GEOCENTRIC CARTESIAN nT at each row of `P` (N x 3, km). Returns (B, |B|, r).
# magref gives X/Y/Z = north/east/DOWN at the point; the triad below is the geocentric one, so
# down = -r_hat (see the header note on the deflection this ignores).
function _mag_B_cart(P::Matrix{Float64}, date::Float64)
	n = size(P, 1)
	r    = Vector{Float64}(undef, n)
	req  = Matrix{Float64}(undef, n, 4)
	@inbounds for i in 1:n
		x, y, z = P[i,1], P[i,2], P[i,3]
		r[i] = sqrt(x*x + y*y + z*z)
		req[i,1] = atand(y, x)                       # lon
		req[i,2] = asind(clamp(z / r[i], -1.0, 1.0)) # lat
		req[i,3] = r[i] - _MAG_R                     # altitude, km
		req[i,4] = date
	end
	D  = GMT.magref(req)
	Dd = D isa AbstractVector ? D[1] : D
	M  = Dd.data                                     # last 7 columns are [F H X Y Z D I]
	B  = Matrix{Float64}(undef, n, 3)
	F  = Vector{Float64}(undef, n)
	@inbounds for i in 1:n
		sla, cla = sincosd(req[i,2])
		slo, clo = sincosd(req[i,1])
		Xn, Ye, Zd = M[i, end-4], M[i, end-3], M[i, end-2]
		# north = (-sin(lat)cos(lon), -sin(lat)sin(lon), cos(lat)); east = (-sin(lon), cos(lon), 0);
		# up = (cos(lat)cos(lon), cos(lat)sin(lon), sin(lat)), and Z is measured DOWN.
		B[i,1] = Xn * (-sla * clo) + Ye * (-slo) - Zd * (cla * clo)
		B[i,2] = Xn * (-sla * slo) + Ye * ( clo) - Zd * (cla * slo)
		B[i,3] = Xn * ( cla)                     - Zd * ( sla)
		F[i]   = M[i, end-6]                     # the model's own total field
	end
	return B, F, r
end

# Unit direction of B (rows of `B`), times the per-line sign in `sgn`.
function _mag_dirs(B::Matrix{Float64}, sgn::Vector{Float64})
	n = size(B, 1)
	U = Matrix{Float64}(undef, n, 3)
	@inbounds for i in 1:n
		m = sqrt(B[i,1]^2 + B[i,2]^2 + B[i,3]^2)
		m = m > 0 ? m : 1.0
		U[i,1] = sgn[i] * B[i,1] / m;  U[i,2] = sgn[i] * B[i,2] / m;  U[i,3] = sgn[i] * B[i,3] / m
	end
	return U
end

# Seed points (N x 3, km) on latitude rings: `nlon` longitudes on each of `nring` rings between
# `lat0` and `lat1` degrees, mirrored into both hemispheres.
function _mag_seeds(nlon::Int, nring::Int, lat0::Float64, lat1::Float64)
	lats = nring <= 1 ? [0.5 * (lat0 + lat1)] : collect(range(lat0, lat1; length = nring))
	P = Matrix{Float64}(undef, 2 * nring * nlon, 3)
	k = 0
	rs = _MAG_R * 1.0005                       # a hair above the surface, so step one is outside
	for la in lats, hemi in (1.0, -1.0), j in 0:(nlon-1)
		lon = 360.0 * j / nlon
		sla, cla = sincosd(hemi * la)
		slo, clo = sincosd(lon)
		k += 1
		P[k,1] = rs * cla * clo;  P[k,2] = rs * cla * slo;  P[k,3] = rs * sla
	end
	return P[1:k, :]
end

# Trace every seed at once. Returns one (npts x 4) matrix per line: x, y, z in EARTH RADII and the
# IGRF total field |B| in nT at that point (what the tubes are coloured by).
function _mag_trace(P0::Matrix{Float64}, date::Float64; rmax::Float64 = 5.0,
                    maxsteps::Int = 1500, hfrac::Float64 = 0.02)
	n = size(P0, 1)
	P = copy(P0)
	rmaxkm = rmax * _MAG_R
	rmin   = _MAG_R                                     # the surface: a returning line stops there
	# Sign that walks OUTWARD from the seed: +1 where B already points up, -1 where it dives in.
	B, F, _ = _mag_B_cart(P, date)
	sgn = Vector{Float64}(undef, n)
	@inbounds for i in 1:n
		d = (B[i,1]*P[i,1] + B[i,2]*P[i,2] + B[i,3]*P[i,3])
		sgn[i] = d >= 0 ? 1.0 : -1.0
	end
	pts = [Vector{NTuple{4,Float64}}() for _ in 1:n]
	@inbounds for i in 1:n
		push!(pts[i], (P[i,1]/_MAG_R, P[i,2]/_MAG_R, P[i,3]/_MAG_R, F[i]))
	end
	live = collect(1:n)
	for _ in 1:maxsteps
		isempty(live) && break
		Q = P[live, :]
		B1, _, r1 = _mag_B_cart(Q, date)
		s1 = sgn[live]
		U1 = _mag_dirs(B1, s1)
		h  = hfrac .* r1                                 # step grows with distance: same angular detail
		Qm = Q .+ (0.5 .* h) .* U1                       # midpoint
		B2, _, _ = _mag_B_cart(Qm, date)
		U2 = _mag_dirs(B2, s1)
		Qn = Q .+ h .* U2
		_, Fn, rn = _mag_B_cart(Qn, date)
		keep = Int[]
		for (k, i) in enumerate(live)
			P[i,1] = Qn[k,1];  P[i,2] = Qn[k,2];  P[i,3] = Qn[k,3]
			push!(pts[i], (Qn[k,1]/_MAG_R, Qn[k,2]/_MAG_R, Qn[k,3]/_MAG_R, Fn[k]))
			(rn[k] <= rmin || rn[k] >= rmaxkm) && continue      # landed or escaped: this line is done
			push!(keep, i)
		end
		live = keep
	end
	lines = Vector{Matrix{Float64}}(undef, n)
	for i in 1:n
		v = pts[i]
		M = Matrix{Float64}(undef, length(v), 4)
		for (k, p) in enumerate(v)
			M[k,1] = p[1];  M[k,2] = p[2];  M[k,3] = p[3];  M[k,4] = p[4]
		end
		lines[i] = M
	end
	return lines
end

# The Earth picture draped on the sphere: GMT's own blue-marble raster, packed by the SAME
# `_drape_buf` every other texture in this viewer goes through (row 0 = south, west->east, RGB).
function _mag_texture(res::String = "@earth_day_06m")
	I = GMT.gmtread(res)
	buf, nlon, nlat, comps = _drape_buf(I)
	return buf, nlon, nlat, comps
end

# The OTHER skin the sphere can wear: the IGRF total field at the surface for this date, as a
# coloured image. `magref`'s own grid mode builds it (the same mode igrf.jl's "Compute grid" button
# uses — no second synthesis, no hand-rolled mesh loop), and the colours are a plain CPT over the
# field's own range. Packed by `_drape_buf` like every other texture.
function _mag_intensity_texture(date::Float64; inc::Float64 = 0.5, cmap = :turbo)
	G = GMT.magref(; R = "-180/180/-90/90", I = "$inc/$inc", alt = 0.0, onetime = date, T = true)
	zmn, zmx = Float64(G.range[5]), Float64(G.range[6])
	zmx > zmn || error("the IGRF total field is constant over the globe ($zmn nT)")
	C = GMT.makecpt(cmap = cmap, range = (zmn, zmx), continuous = true)
	buf, nlon, nlat, comps = _drape_buf(GMT.mat2img(G; cmap = C))
	return buf, nlon, nlat, comps, zmn, zmx
end

# The MAGNETIC (dip) POLES: where the field is vertical, i.e. |inclination| = 90 deg. There is no
# closed form for them in a field with more than a dipole term, so they are found the way they are
# defined — by looking for the extremum — on `magref`'s own inclination grid, over the polar cap of
# each hemisphere and then on successively finer grids around the best node. Four passes take the
# spacing from 1 deg to 0.001 deg for a few milliseconds of grid work.
#
# These are the DIP poles (where a compass needle stands on end), not the geomagnetic poles of the
# best-fitting dipole, and not the same point in the two hemispheres — they are not antipodal, which
# is exactly why both are computed instead of mirroring one.

# WHERE THE SEARCH STARTS, measured rather than guessed. Over the whole IGRF window 1900-2030 the
# north dip pole stays between 70.5 and 86.6 deg N and wanders through EVERY longitude (a point that
# close to the rotation axis does); the south stays between 71.7 and 63.7 deg S and inside
# 134.3-148.6 deg E. So the north cap keeps all longitudes and starts at 65 N, and the south search
# is a BOX — 110-175 E, 80-55 S — which is what makes the year-by-year track affordable: the first
# pass lands 6x finer in the south for the same node count, and four passes then reach the same
# answer five used to (checked against an 8-pass reference over 1900-2030: worst case 0.27 km in the
# north, 0.45 km in the south, i.e. nothing a marker or a track can show).
const _MAG_CAP_N = (-180.0, 180.0,  65.0, 90.0)
const _MAG_CAP_S = ( 110.0, 175.0, -80.0, -55.0)

function _mag_dip_poles(date::Float64)
	north = _mag_dip_pole(date, _MAG_CAP_N...; nx = 61, ny = 41)
	south = _mag_dip_pole(date, _MAG_CAP_S...; nx = 41, ny = 31)
	return north, south
end

# THE SEARCH RUNS ON H, NOT ON |I|, and they are the same point: the field is vertical exactly where
# its HORIZONTAL component vanishes, so "maximum |inclination|" and "minimum H" define one place. The
# difference is conditioning. |I| = atand(Z/H) saturates at 90 deg over a wide cap — around the north
# pole it sits within 0.1 deg of vertical across tens of kilometres, so a grid search on it wanders
# over a plateau and stops wherever rounding says. Measured against the published IGRF dip poles, the
# |I| search landed ~70 km off in the north (the south, where the field is not as flat, was right).
# H has a clean V-shaped zero at the same point and pins it. |I| is still what gets REPORTED — it is
# the number that says "this is where a compass needle stands on end".
#
# The refinement box is scaled by 1/cos(lat) in longitude: at 86 deg a degree of longitude is 7 km, so
# a box measured in plain degrees collapses in one direction and starves the search in the other.
function _mag_dip_pole(date::Float64, lon0::Float64, lon1::Float64, lat0::Float64, lat1::Float64;
                       nx::Int = 61, ny::Int = 41, passes::Int = 4)
	w, e, s, n = lon0, lon1, lat0, lat1
	lon = 0.5 * (lon0 + lon1);  lat = 0.5 * (lat0 + lat1)
	for _ in 1:passes                     # nodes per pass; the box then shrinks by 6/(n-1) each time
		# THE INCREMENT IS DERIVED FROM THE BOX, never chosen beside it: GMT requires the span to be an
		# exact whole number of increments and warns ("x_max-x_min must equal (NX + eps) * x_inc") on
		# every grid where it is not — which a fixed increment over a shrinking box cannot promise.
		dlon = (e - w) / (nx - 1)
		dlat = (n - s) / (ny - 1)
		G = GMT.magref(; R = "$w/$e/$s/$n", I = "$dlon/$dlat", alt = 0.0, onetime = date, H = true)
		k = argmin(G.z)                            # column-major (iy, ix), y ascending: magref's own grid
		iy, ix = Tuple(k)
		lon, lat = Float64(G.x[ix]), Float64(G.y[iy])
		# ...and the next pass searches a box three nodes wide around it.
		w = lon - 3dlon;  e = lon + 3dlon
		s = max(-90.0, lat - 3dlat);  n = min(90.0, lat + 3dlat)
		n - s < 1e-9 && break
	end
	lon > 180.0 && (lon -= 360.0)
	lon < -180.0 && (lon += 360.0)
	# The inclination AT the answer, which is what the label quotes.
	D = GMT.magref([lon lat 0.0 date])
	Dd = D isa AbstractVector ? D[1] : D
	return (lon, lat, Float64(Dd.data[1, end]))
end

# C callback — params = "date/nlon/nring/lat0/lat1/rmax/maxsteps".
# Two-phase, like the fault demo's mesh: `out == NULL` traces and reports the per-line point counts,
# the second call copies x,y,z,|B| into the buffer.
function _on_magfield_lines(cparams::Cstring, out::Ptr{Cdouble}, capacity::Cint,
                            counts::Ptr{Cint}, maxlines::Cint, err::Ptr{UInt8}, cap::Cint)::Cint
	try
		if out == C_NULL
			warm_wait("magfield")
			p = split(unsafe_string(cparams), '/')
			date = parse(Float64, p[1])
			nlon = parse(Int, p[2]);  nring = parse(Int, p[3])
			lat0 = parse(Float64, p[4]);  lat1 = parse(Float64, p[5])
			rmax = parse(Float64, p[6])
			msteps = length(p) >= 7 ? parse(Int, p[7]) : 1500
			(1900.0 <= date <= 2030.0) ||
				error("IGRF is defined for 1900-2030 only (asked for $date)")
			seeds = _mag_seeds(nlon, nring, lat0, lat1)
			size(seeds, 1) <= Int(maxlines) ||
				error("$(size(seeds,1)) field lines asked for, $(Int(maxlines)) is this view's limit")
			_MAG_LINES[] = _mag_trace(seeds, date; rmax = rmax, maxsteps = msteps)
		end
		L = _MAG_LINES[]
		for (i, M) in enumerate(L)
			i > Int(maxlines) && break
			unsafe_store!(counts, Cint(size(M, 1)), i)
		end
		out == C_NULL && return Cint(length(L))
		need = sum(length, L)
		capacity >= need || error("Field-line buffer is too small ($need doubles needed)")
		k = 1
		for M in L, i in axes(M, 1), j in 1:4
			unsafe_store!(out, M[i,j], k)
			k += 1
		end
		return Cint(length(L))
	catch e
		n = _console_write(err, cap, sprint(showerror, e))
		cap > 0 && unsafe_store!(err, 0x00, Int(n)+1)
		return Cint(0)
	end
end

# C callback — the sphere's skin. `which` = 0 the Earth picture, 1 the IGRF total field at the
# surface for `date`. Two-phase as well: `out == NULL` computes it and fills dims = (nlon, nlat,
# comps), the second call copies the bytes.
function _on_magfield_texture(which::Cint, date::Cdouble, out::Ptr{UInt8}, capacity::Cint,
                              dims::Ptr{Cint}, err::Ptr{UInt8}, cap::Cint)::Cint
	try
		if out == C_NULL || isempty(_MAG_TEX[][1])
			_MAG_TEX[] = if which == 0
				_mag_texture()
			else
				(1900.0 <= date <= 2030.0) ||
					error("IGRF is defined for 1900-2030 only (asked for $date)")
				b, nx, ny, nc, _, _ = _mag_intensity_texture(Float64(date))
				(b, nx, ny, nc)
			end
		end
		buf, nlon, nlat, comps = _MAG_TEX[]
		unsafe_store!(dims, Cint(nlon), 1)
		unsafe_store!(dims, Cint(nlat), 2)
		unsafe_store!(dims, Cint(comps), 3)
		out == C_NULL && return Cint(1)
		capacity >= length(buf) || error("Texture buffer is too small")
		GC.@preserve buf unsafe_copyto!(out, pointer(buf), length(buf))
		return Cint(1)
	catch e
		n = _console_write(err, cap, sprint(showerror, e))
		cap > 0 && unsafe_store!(err, 0x00, Int(n)+1)
		return Cint(0)
	end
end

# C callback — the two dip poles for `date`, as [lonN, latN, incN, lonS, latS, incS].
function _on_magfield_poles(date::Cdouble, out::Ptr{Cdouble}, err::Ptr{UInt8}, cap::Cint)::Cint
	try
		(1900.0 <= Float64(date) <= 2030.0) ||
			error("IGRF is defined for 1900-2030 only (asked for $date)")
		(lonN, latN, incN), (lonS, latS, incS) = _mag_dip_poles(Float64(date))
		for (k, v) in enumerate((lonN, latN, incN, lonS, latS, incS))
			unsafe_store!(out, v, k)
		end
		return Cint(1)
	catch e
		n = _console_write(err, cap, sprint(showerror, e))
		cap > 0 && unsafe_store!(err, 0x00, Int(n)+1)
		return Cint(0)
	end
end


# The polar-cap COASTLINE the 2-D track plot draws under the track, from the SAME GSHHG dump the
# Geography menu's "Plot coastline" uses (`GMT.coast(..., M=true)`) — there is no second shoreline
# source in this package. Handed over as lon/lat pairs with a (NaN, NaN) between segments, which is
# how a polyline says "lift the pen".
const _MAG_COAST = Ref{Tuple{Float64,Float64,Vector{Float64}}}((0.0, 0.0, Float64[]))

function _mag_cap_coast(lat0::Float64, lat1::Float64)
	k0, k1, V = _MAG_COAST[]
	(k0 == lat0 && k1 == lat1 && !isempty(V)) && return V
	D = GMT.coast(R = (-180.0, 180.0, lat0, lat1), D = :l, M = true)
	segs = D isa AbstractVector ? D : [D]
	out = Float64[]
	for d in segs
		xy = d.data
		size(xy, 1) < 2 && continue
		isempty(out) || append!(out, (NaN, NaN))
		for i in axes(xy, 1)
			push!(out, Float64(xy[i, 1]), Float64(xy[i, 2]))
		end
	end
	_MAG_COAST[] = (lat0, lat1, out)
	return out
end

# THE LAND ITSELF, not its outline. The shoreline dump above comes back as OPEN ARCS, cut at the
# region's tile edges (six pieces for the Antarctic sector, none of them closed) — painting those as
# polygons is what turned Antarctica into scribble. A land/sea MASK has no such problem: GMT's own
# `grdlandmask` answers "is this node land" for every node of the sector, and the plot fills the
# nodes that say yes. Same GSHHG data, no polygon reconstruction anywhere.
#
# The buffer is GMT's own grid order, handed over as it lies (grids read TRB law): column-major with
# y ascending, so node (ix, iy) is at `ix*ny + iy`, iy counted from the south.
const _MAG_MASK = Ref{Tuple{NTuple{5,Float64},Vector{UInt8},Int,Int}}(((0.,0.,0.,0.,0.), UInt8[], 0, 0))

function _mag_land_mask(lon0::Float64, lon1::Float64, lat0::Float64, lat1::Float64, inc::Float64)
	key = (lon0, lon1, lat0, lat1, inc)
	k, V, nx, ny = _MAG_MASK[]
	(k == key && !isempty(V)) && return V, nx, ny
	G = GMT.grdlandmask(R = (lon0, lon1, lat0, lat1), I = inc, D = :l, N = "0/1/0/1/0")
	z = G.z
	out = Vector{UInt8}(undef, length(z))
	@inbounds for i in eachindex(z)
		out[i] = z[i] > 0.5 ? 0x01 : 0x00
	end
	ny2, nx2 = size(z)                      # magref/grdlandmask hand back (ny, nx), y ascending
	_MAG_MASK[] = (key, out, nx2, ny2)
	return out, nx2, ny2
end

# C callback — the land mask for a sector. Two-phase: `out == NULL` builds it and reports
# dims = (nx, ny), the second call copies one byte per node.
function _on_magfield_mask(lon0::Cdouble, lon1::Cdouble, lat0::Cdouble, lat1::Cdouble, inc::Cdouble,
                           out::Ptr{UInt8}, capacity::Cint, dims::Ptr{Cint},
                           err::Ptr{UInt8}, cap::Cint)::Cint
	try
		V, nx, ny = _mag_land_mask(Float64(lon0), Float64(lon1), Float64(lat0), Float64(lat1), Float64(inc))
		unsafe_store!(dims, Cint(nx), 1)
		unsafe_store!(dims, Cint(ny), 2)
		out == C_NULL && return Cint(1)
		capacity >= length(V) || error("The land-mask buffer is too small ($(length(V)) bytes needed)")
		GC.@preserve V unsafe_copyto!(out, pointer(V), length(V))
		return Cint(1)
	catch e
		n = _console_write(err, cap, sprint(showerror, e))
		cap > 0 && unsafe_store!(err, 0x00, Int(n)+1)
		return Cint(0)
	end
end

# C callback — the cap coastline, two-phase: `out == NULL` builds it and reports the point count
# (a NaN separator counts as a point), the second call copies lon/lat pairs.
function _on_magfield_coast(lat0::Cdouble, lat1::Cdouble, out::Ptr{Cdouble}, capacity::Cint,
                            count::Ptr{Cint}, err::Ptr{UInt8}, cap::Cint)::Cint
	try
		V = _mag_cap_coast(Float64(lat0), Float64(lat1))
		unsafe_store!(count, Cint(length(V) ÷ 2), 1)
		out == C_NULL && return Cint(1)
		capacity >= length(V) || error("The coastline buffer is too small ($(length(V)) doubles needed)")
		GC.@preserve V unsafe_copyto!(out, pointer(V), length(V))
		return Cint(1)
	catch e
		n = _console_write(err, cap, sprint(showerror, e))
		cap > 0 && unsafe_store!(err, 0x00, Int(n)+1)
		return Cint(0)
	end
end


function _register_magfield_poles()
	pptr = @cfunction((d,o,e,c) -> Base.invokelatest(_on_magfield_poles, d,o,e,c),
		Cint, (Cdouble,Ptr{Cdouble},Ptr{UInt8},Cint))
	ccall(_fn(:gmtvtk_set_magfield_poles_callback), Cvoid, (Ptr{Cvoid},), pptr)
	cptr = @cfunction((a,b,o,c,n,e,x) -> Base.invokelatest(_on_magfield_coast, a,b,o,c,n,e,x),
		Cint, (Cdouble,Cdouble,Ptr{Cdouble},Cint,Ptr{Cint},Ptr{UInt8},Cint))
	ccall(_fn(:gmtvtk_set_magfield_coast_callback), Cvoid, (Ptr{Cvoid},), cptr)
	mptr = @cfunction((a,b,c2,d,i,o,cp,dm,e,x) -> Base.invokelatest(_on_magfield_mask, a,b,c2,d,i,o,cp,dm,e,x),
		Cint, (Cdouble,Cdouble,Cdouble,Cdouble,Cdouble,Ptr{UInt8},Cint,Ptr{Cint},Ptr{UInt8},Cint))
	ccall(_fn(:gmtvtk_set_magfield_mask_callback), Cvoid, (Ptr{Cvoid},), mptr)
	return
end

# Warm-up for this tool — AND IT MUST NOT CALL GMT. `warm_start` (warmup.jl) runs the body with
# `Threads.@spawn` whenever the session has more than one thread, so anything GMT this body does runs
# on a WORKER thread while the dialog, on the UI thread, is already calling GMT for its own first
# texture and trace. The GMT C API is not re-entrant and not thread-safe: the two sessions corrupt
# each other's heap and the whole viewer dies on the spot — measured, `julia -t auto`, exit
# 0xC0000374 (STATUS_HEAP_CORRUPTION) the instant the menu entry was triggered. A single-threaded
# session survives, which is exactly why this passed every test here and killed the tool on the
# user's desk.
#
# So the warm-up is type-level only: it asks for the callbacks to be compiled for their signatures
# and calls nothing. The first Compute pays for the tracer's own compilation, which is a second, not
# a crash. Any future warm body for a GMT-backed tool has the same rule — see the fault demo's, which
# is pure Julia geometry and therefore safe to run off-thread.
function _magfield_warm()
	precompile(_on_magfield_lines, (Cstring, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Ptr{UInt8}, Cint))
	precompile(_on_magfield_texture, (Cint, Cdouble, Ptr{UInt8}, Cint, Ptr{Cint}, Ptr{UInt8}, Cint))
	precompile(_on_magfield_poles, (Cdouble, Ptr{Cdouble}, Ptr{UInt8}, Cint))
	precompile(_on_magfield_coast, (Cdouble, Cdouble, Ptr{Cdouble}, Cint, Ptr{Cint}, Ptr{UInt8}, Cint))
	precompile(_on_magfield_mask, (Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Ptr{UInt8}, Cint,
	                               Ptr{Cint}, Ptr{UInt8}, Cint))
	precompile(_mag_B_cart, (Matrix{Float64}, Float64))
	precompile(_mag_trace, (Matrix{Float64}, Float64))
	precompile(_mag_dip_poles, (Float64,))
	precompile(_mag_intensity_texture, (Float64,))
	return nothing
end

function _register_magfield()
	warm_register("magfield", _magfield_warm)
	lptr = @cfunction((p,o,c,n,m,e,x) -> Base.invokelatest(_on_magfield_lines, p,o,c,n,m,e,x),
		Cint, (Cstring,Ptr{Cdouble},Cint,Ptr{Cint},Cint,Ptr{UInt8},Cint))
	tptr = @cfunction((w,t,o,c,d,e,x) -> Base.invokelatest(_on_magfield_texture, w,t,o,c,d,e,x),
		Cint, (Cint,Cdouble,Ptr{UInt8},Cint,Ptr{Cint},Ptr{UInt8},Cint))
	ccall(_fn(:gmtvtk_set_magfield_callback), Cvoid, (Ptr{Cvoid},Ptr{Cvoid}), lptr, tptr)
	return
end
