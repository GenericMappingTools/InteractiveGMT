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

# C callback — the sphere's texture. Two-phase as well: `out == NULL` fills dims = (nlon, nlat,
# comps), the second call copies the bytes.
function _on_magfield_texture(out::Ptr{UInt8}, capacity::Cint, dims::Ptr{Cint},
                              err::Ptr{UInt8}, cap::Cint)::Cint
	try
		if out == C_NULL || isempty(_MAG_TEX[][1])
			_MAG_TEX[] = _mag_texture()
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

# Compile the tracer (and fetch the texture) while the dialog is still being built — warmup.jl's
# contract, the same one the fault demo uses. A tiny trace: two lines, few steps, same code path.
function _magfield_warm()
	try
		_mag_trace(_mag_seeds(2, 1, 60.0, 60.0), 2025.0; rmax = 1.5, maxsteps = 12)
		yield()
		_MAG_TEX[] = _mag_texture()
		yield()
	catch e
		@tool_error "Magnetic field lines warm-up FAILED" exception=(e,)
	end
	precompile(_on_magfield_lines, (Cstring, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Ptr{UInt8}, Cint))
	precompile(_on_magfield_texture, (Ptr{UInt8}, Cint, Ptr{Cint}, Ptr{UInt8}, Cint))
	return nothing
end

function _register_magfield()
	warm_register("magfield", _magfield_warm)
	lptr = @cfunction((p,o,c,n,m,e,x) -> Base.invokelatest(_on_magfield_lines, p,o,c,n,m,e,x),
		Cint, (Cstring,Ptr{Cdouble},Cint,Ptr{Cint},Cint,Ptr{UInt8},Cint))
	tptr = @cfunction((o,c,d,e,x) -> Base.invokelatest(_on_magfield_texture, o,c,d,e,x),
		Cint, (Ptr{UInt8},Cint,Ptr{Cint},Ptr{UInt8},Cint))
	ccall(_fn(:gmtvtk_set_magfield_callback), Cvoid, (Ptr{Cvoid},Ptr{Cvoid}), lptr, tptr)
	return
end
