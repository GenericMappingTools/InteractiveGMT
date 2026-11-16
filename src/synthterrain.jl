# synthterrain.jl — Grid Tools > "Synthetic terrain": a made-up but believable relief on any region.
#
# A LANDSCAPE-EVOLUTION model, the way geomorphologists make synthetic relief: the land is lifted by a
# tectonic uplift field (smooth seeded noise: fast-rising mountain belts, nearly still basins), rivers
# cut down by the stream-power law (erosion rate = K A^m S, A drainage area, S slope) and hillslopes
# creep (linear diffusion), until uplift and erosion balance. Solved with the implicit O(n) scheme of
# Braun & Willett (2013) on a working lattice of at most _ST_WORK nodes per side, interpolated to the
# asked lattice with grdsample. The result is scaled to the user's [zmin, zmax] and added to the window
# as a new grid. User docs: docs/src/70-tools.md.
#
# References:
#   Braun, J. & Willett, S. D. (2013). A very efficient O(n), implicit and parallel method to solve the
#     stream power equation governing fluvial incision and landscape evolution. Geomorphology 180-181,
#     170-179.
#   Howard, A. D. (1994). A detachment-limited model of drainage basin evolution. Water Resources
#     Research 30(7), 2261-2285  (stream-power incision with hillslope creep).
#   Barnes, R., Lehman, C. & Mulla, D. (2014). Priority-flood: an optimal depression-filling and
#     watershed-labeling algorithm for digital elevation models. Computers & Geosciences 62, 117-127.
#   O'Callaghan, J. F. & Mark, D. M. (1984). The extraction of drainage networks from digital elevation
#     data. Computer Vision, Graphics, and Image Processing 28(3), 323-344  (D8 flow routing).
#   Perlin, K. (2002). Improving noise. ACM Trans. Graph. 21(3), 681-682  (the noise of the uplift field).
#
# The C++ dialog is SynthTerrainDialog (70_window.cpp, deps/ui/synth_terrain.ui). It hands a
# newline-separated "key=value" block to `_on_synthterrain` (30_app.cpp's JuliaSynthTerrainFn).

# ---- A seeded hash: every random number here comes from (seed, integer position), so the same seed
# gives the same terrain on every machine and every run. splitmix64's finaliser.
@inline function _st_mix(h::UInt64)::UInt64
	h = (h ⊻ (h >> 30)) * 0xbf58476d1ce4e5b9
	h = (h ⊻ (h >> 27)) * 0x94d049bb133111eb
	return h ⊻ (h >> 31)
end
@inline _st_hash(ix::Int, iy::Int, seed::UInt64) =
	_st_mix(seed ⊻ (reinterpret(UInt64, Int64(ix)) * 0x9E3779B97F4A7C15) ⊻ (reinterpret(UInt64, Int64(iy)) * 0xC2B2AE3D27D4EB4F))

# ---- 2-D gradient noise (Perlin's, with 8 gradient directions), value in about [-1, 1].
const _ST_GX = (1.0, -1.0, 1.0, -1.0, 1.41421356, -1.41421356, 0.0, 0.0)
const _ST_GY = (1.0, 1.0, -1.0, -1.0, 0.0, 0.0, 1.41421356, -1.41421356)
@inline _st_fade(t::Float64) = t * t * t * (t * (t * 6.0 - 15.0) + 10.0)
@inline function _st_grad(ix::Int, iy::Int, dx::Float64, dy::Float64, seed::UInt64)
	k = Int(_st_hash(ix, iy, seed) & 7) + 1
	return _ST_GX[k] * dx + _ST_GY[k] * dy
end
@inline function _st_noise(x::Float64, y::Float64, seed::UInt64)::Float64
	x0 = floor(Int, x);  y0 = floor(Int, y)
	fx = x - x0;         fy = y - y0
	u = _st_fade(fx);    v = _st_fade(fy)
	n00 = _st_grad(x0,     y0,     fx,       fy,       seed)
	n10 = _st_grad(x0 + 1, y0,     fx - 1.0, fy,       seed)
	n01 = _st_grad(x0,     y0 + 1, fx,       fy - 1.0, seed)
	n11 = _st_grad(x0 + 1, y0 + 1, fx - 1.0, fy - 1.0, seed)
	a = n00 + u * (n10 - n00);  b = n01 + u * (n11 - n01)
	return a + v * (b - a)
end

# Sum of octaves: plain fBm and the ridged variant, from the SAME octave noises, so blending the two
# only changes the character of the crests, never where the mountains are.
@inline function _st_octaves(x::Float64, y::Float64, oct::Int, gain::Float64, seed::UInt64)
	x, y = 0.8 * x - 0.6 * y, 0.6 * x + 0.8 * y          # even the first octave off the grid axes
	a = 1.0;  fbm = 0.0;  rid = 0.0;  norm = 0.0;  w = 1.0
	for o in 1:oct
		s = seed + UInt64(o) * 0x632BE59BD9B4E019
		n = _st_noise(x, y, s)
		fbm += a * n
		r = 1.0 - abs(n);  r *= r;  r *= w          # ridged multifractal: crests weighted by the octave above
		w = clamp(r * 2.0, 0.0, 1.0)
		rid += a * r
		norm += a
		# next octave: twice the frequency, ROTATED (~37 deg) and shifted, so no two octaves share a
		# lattice -- aligned lattices stack their creases into straight lines along the grid axes
		x, y = 1.6 * x - 1.2 * y + 17.3, 1.2 * x + 1.6 * y + 31.7
		a *= gain
	end
	return fbm / norm, rid / norm
end

"""
    _st_shape(nx, ny; feature, octaves, gain, ridge, warp, seed) -> Matrix{Float64} (ny, nx), in [0, 1]

The noise stage. `feature` is the size of the largest landforms in NODES, `gain` the amplitude kept
from one octave to the next (roughness), `ridge` the 0..1 blend from rounded hills to ridged crests,
`warp` the 0..1 strength of the domain warp. Row 1 is the SOUTH row.
"""
function _st_shape(nx::Int, ny::Int; feature::Float64, octaves::Int, gain::Float64, ridge::Float64,
                   warp::Float64, seed::Integer)::Matrix{Float64}
	sd = _st_mix(UInt64(abs(seed)) + 0x2545F4914F6CDD1D)
	sw1 = sd ⊻ 0x5851F42D4C957F2D;  sw2 = sd ⊻ 0x14057B7EF767814F
	h = Matrix{Float64}(undef, ny, nx)
	inv = 1.0 / max(feature, 1.0)
	# no octave finer than ~4 nodes: below that the grid cannot carry it, and it only aliases into streaks
	oct = clamp(floor(Int, log2(max(feature, 4.0) / 4.0)) + 1, 1, octaves)
	Threads.@threads for ix in 1:nx
		for iy in 1:ny
			x = ix * inv;  y = iy * inv
			if warp > 0
				wx, _ = _st_octaves(x + 5.2, y + 1.3, 4, 0.5, sw1)
				wy, _ = _st_octaves(x + 1.7, y + 9.2, 4, 0.5, sw2)
				x += 1.5 * warp * wx;  y += 1.5 * warp * wy
			end
			fbm, rid = _st_octaves(x, y, oct, gain, sd)
			h[iy, ix] = (1.0 - ridge) * (0.5 + 0.5 * fbm) + ridge * rid
		end
	end
	lo, hi = extrema(h)
	hi > lo && (h .= (h .- lo) ./ (hi - lo))
	return h
end

# ---- The landscape model: uplift against river incision (stream-power law) and hillslope creep, the
# implicit O(n) scheme of Braun & Willett (2013). Everything in NODE units (one node = one unit of
# length, area counted in cells); the result is rescaled to the user's z range at the end, so the
# absolute values of U and K only set the time scale.

const _ST_DX8 = (1, -1, 0, 0, 1, 1, -1, -1)
const _ST_DY8 = (0, 0, 1, -1, 1, -1, 1, -1)
const _ST_DL8 = (1.0, 1.0, 1.0, 1.0, sqrt(2.0), sqrt(2.0), sqrt(2.0), sqrt(2.0))

# Priority-flood (Barnes et al. 2014, with a tiny epsilon gradient): raise every closed depression to its
# spill height so every node drains to the border. Only for the START surface — later pits are taken
# care of by the uplift itself (a pit only goes up until it spills, Braun & Willett 2013 §2.4).
function _st_fill!(h::Matrix{Float64})
	ny, nx = size(h)
	done = falses(ny, nx)
	heap = Tuple{Float64,Int}[]                        # binary min-heap of (height, linear index)
	push_h!(v) = (push!(heap, v); i = length(heap);
		while i > 1 && heap[i >> 1][1] > heap[i][1]; heap[i], heap[i >> 1] = heap[i >> 1], heap[i]; i >>= 1; end)
	function pop_h!()
		top = heap[1];  last = pop!(heap)
		if !isempty(heap)
			heap[1] = last;  i = 1;  n = length(heap)
			while true
				l = 2i;  r = l + 1;  s = i
				l <= n && heap[l][1] < heap[s][1] && (s = l)
				r <= n && heap[r][1] < heap[s][1] && (s = r)
				s == i && break
				heap[i], heap[s] = heap[s], heap[i];  i = s
			end
		end
		return top
	end
	@inbounds for ix in 1:nx, iy in 1:ny
		(ix == 1 || ix == nx || iy == 1 || iy == ny) || continue
		done[iy, ix] = true;  push_h!((h[iy, ix], (ix - 1) * ny + iy))
	end
	@inbounds while !isempty(heap)
		hc, k = pop_h!()
		ix = (k - 1) ÷ ny + 1;  iy = k - (ix - 1) * ny
		for d in 1:8
			jx = ix + _ST_DX8[d];  jy = iy + _ST_DY8[d]
			(1 <= jx <= nx && 1 <= jy <= ny && !done[jy, jx]) || continue
			done[jy, jx] = true
			h[jy, jx] = max(h[jy, jx], hc + 1e-6)
			push_h!((h[jy, jx], (jx - 1) * ny + jy))
		end
	end
	return h
end

# One time step. `rec`/`dist` steepest-descent (D8) receivers, `stk` the Braun-Willett stack (every node
# after its receiver), `A` drainage area. Border nodes are base level (height 0, never move).
function _st_step!(h::Matrix{Float64}, U::Matrix{Float64}, dt::Float64, K::Float64, m::Float64, kd::Float64,
                   rec::Vector{Int}, dist::Vector{Float64}, ndon::Vector{Int}, don::Vector{Int},
                   stk::Vector{Int}, A::Vector{Float64}, tmp::Matrix{Float64})
	ny, nx = size(h);  N = nx * ny
	# uplift
	Threads.@threads for ix in 2:nx-1
		@inbounds for iy in 2:ny-1
			h[iy, ix] += U[iy, ix] * dt
		end
	end
	# receivers: the steepest of the 8 neighbours, or the node itself (border, or a pit)
	Threads.@threads for ix in 1:nx
		@inbounds for iy in 1:ny
			k = (ix - 1) * ny + iy
			rec[k] = k;  dist[k] = 1.0
			(ix == 1 || ix == nx || iy == 1 || iy == ny) && continue
			c = h[iy, ix];  smax = 0.0
			for d in 1:8
				jx = ix + _ST_DX8[d];  jy = iy + _ST_DY8[d]
				s = (c - h[jy, jx]) / _ST_DL8[d]
				s > smax && (smax = s; rec[k] = (jx - 1) * ny + jy; dist[k] = _ST_DL8[d])
			end
		end
	end
	# donors (8 slots per node) and the stack, by depth-first walk up from every base-level node
	fill!(ndon, 0)
	@inbounds for k in 1:N
		r = rec[k];  r != k && (ndon[r] += 1; don[8 * (r - 1) + ndon[r]] = k)
	end
	ns = 0;  work = tmp
	walk = Vector{Int}(undef, 0);  sizehint!(walk, 1024)
	@inbounds for k in 1:N
		rec[k] == k || continue
		push!(walk, k)
		while !isempty(walk)
			j = pop!(walk)
			ns += 1;  stk[ns] = j
			for q in 1:ndon[j]
				push!(walk, don[8 * (j - 1) + q])
			end
		end
	end
	# drainage area, from the tips down
	fill!(A, 1.0)
	@inbounds for i in N:-1:1
		k = stk[i];  r = rec[k];  r != k && (A[r] += A[k])
	end
	# river incision, implicit (n = 1): each node solved after its receiver, unconditionally stable
	hv = vec(h)
	@inbounds for i in 1:N
		k = stk[i];  r = rec[k]
		r == k && continue
		f = K * dt * A[k]^m / dist[k]
		hv[k] = (hv[k] + f * hv[r]) / (1.0 + f)
	end
	# hillslope creep: linear diffusion, explicit; kd is the per-step coefficient (<= 0.2 keeps it stable)
	if kd > 0
		cdif = kd                                        # NOT `c`: the receivers closure above has its own `c`,
		copyto!(work, h)                                 # and a same-named outer local would be shared by threads
		Threads.@threads for ix in 2:nx-1
			@inbounds for iy in 2:ny-1
				h[iy, ix] = work[iy, ix] + cdif * (work[iy-1, ix] + work[iy+1, ix] + work[iy, ix-1] + work[iy, ix+1] - 4 * work[iy, ix])
			end
		end
	end
	return h
end

"""
    _st_landscape(nx, ny; feature, lowlands, hillslope, concavity, seed, steps) -> Matrix (ny, nx) in [0, 1]

Grow a landscape: tectonic uplift, with a pattern of `feature` nodes (some areas rise fast, others
— `lowlands`, 0..1 — barely at all), against river incision by the stream-power law with exponent
`concavity` (m, with n = 1) and hillslope creep (`hillslope`, 0..1). Run on a domain padded on every
side (its border is the sea level the rivers drain to) and cropped back, so no edge of the asked grid
is pinned to zero. 120 steps of dt = 1 with K = 0.05 bring it to (near) steady state, where uplift and
erosion balance.
"""
function _st_landscape(nx::Int, ny::Int; feature::Float64, lowlands::Float64, hillslope::Float64,
                       concavity::Float64, seed::Integer, steps::Int = 120)::Matrix{Float64}
	pad = max(8, round(Int, 0.06 * max(nx, ny)))
	NX = nx + 2pad;  NY = ny + 2pad;  N = NX * NY
	# the uplift pattern: smooth fBm, its lowest `lowlands` fraction nearly still
	s = _st_shape(NX, NY; feature = feature, octaves = 4, gain = 0.5, ridge = 0.0, warp = 0.5, seed = seed)
	U = Matrix{Float64}(undef, NY, NX)
	@inbounds for k in eachindex(s)
		u = max(s[k] - lowlands, 0.0) / max(1.0 - lowlands, 1e-6)
		U[k] = 0.02 + 0.98 * u * u
	end
	# start surface: a low copy of the uplift pattern plus a little roughness, flood-filled so it drains
	h = Matrix{Float64}(undef, NY, NX)
	sd = _st_mix(UInt64(abs(seed)) + 0x7F4A7C159E3779B9)
	@inbounds for ix in 1:NX, iy in 1:NY
		h[iy, ix] = (ix == 1 || ix == NX || iy == 1 || iy == NY) ? 0.0 :
		            5.0 * U[iy, ix] + 0.5 * ((_st_hash(ix, iy, sd) & 0xffff) / 65535.0)
	end
	_st_fill!(h)
	rec = Vector{Int}(undef, N);  dist = Vector{Float64}(undef, N);  ndon = Vector{Int}(undef, N)
	don = Vector{Int}(undef, 8N);  stk = Vector{Int}(undef, N);  A = Vector{Float64}(undef, N)
	tmp = similar(h)
	dt = 1.0;  K = 0.05;  kd = 0.2 * clamp(hillslope, 0.0, 1.0)
	for _ in 1:steps
		_st_step!(h, U, dt, K, concavity, kd, rec, dist, ndon, don, stk, A, tmp)
	end
	z = h[pad+1:pad+ny, pad+1:pad+nx]
	lo, hi = extrema(z)
	z .= (z .- lo) ./ max(hi - lo, eps())
	return z
end

# A region that fits lon/lat limits is taken as geographic.
function _st_lonlat(region)::Bool
	x0, x1, y0, y1 = Float64.(Tuple(region))
	return x0 >= -180 && x1 <= 360 && x1 - x0 <= 360 && y0 >= -90 && y1 <= 90
end

# The landscape model runs on a working lattice of at most this many nodes along the longer side, and is
# then interpolated (grdsample, bicubic) onto the asked lattice. Valley spacing in the model is a few
# NODES, so running it at a fine output spacing would cover every slope with one-node gullies; the fixed
# working size gives the same landscape at any output resolution, in a time that does not grow with it.
const _ST_WORK = 320

"""
    synth_terrain(region, inc; zrange=(0,1000), feature=0.3, lowlands=0.1, hillslope=0.3,
                  concavity=0.45, seed=1, geog=<region fits lon/lat>) -> GMTgrid

A synthetic DTM on `region` = (x0, x1, y0, y1) at spacing `inc` = (dx, dy), heights spanning
`zrange`, grown by a landscape-evolution model: tectonic uplift against river incision (stream-power
law) and hillslope creep (Braun & Willett 2013). `feature`: size of the uplift pattern (the mountain
belts and basins) as a fraction of the larger grid side; `lowlands` (0..0.9): share of the area that
barely rises (plains, basins); `hillslope` (0..1): how strongly slopes creep, rounding the ridges;
`concavity` (0.2..0.8): the stream-power area exponent m, how fast river profiles flatten downstream.
`geog` = true gives the grid a geographic CRS (WGS84 lon/lat).
"""
function synth_terrain(region, inc; zrange=(0.0, 1000.0), feature::Real=0.3, lowlands::Real=0.1,
                       hillslope::Real=0.3, concavity::Real=0.45, seed::Integer=1,
                       geog::Bool=_st_lonlat(region))::GMTgrid
	x0, x1, y0, y1 = Float64.(Tuple(region));  dx, dy = Float64.(Tuple(inc))
	(x1 > x0 && y1 > y0) || error("the region is empty: x_min must be below x_max and y_min below y_max")
	(dx > 0 && dy > 0) || error("the increments must be positive")
	nx = round(Int, (x1 - x0) / dx) + 1;  ny = round(Int, (y1 - y0) / dy) + 1
	# the lattice must be the one typed: an increment that does not divide the region would be quietly
	# replaced by another spacing
	abs((nx - 1) * dx - (x1 - x0)) <= 1e-6 * dx || error("x inc $dx does not divide x max - x min = $(x1 - x0)")
	abs((ny - 1) * dy - (y1 - y0)) <= 1e-6 * dy || error("y inc $dy does not divide y max - y min = $(y1 - y0)")
	(nx >= 3 && ny >= 3) || error("the grid needs at least 3x3 nodes (got $(nx)x$(ny))")
	nx * ny <= 64_000_000 || error("$(nx)x$(ny) nodes is too large for a synthetic terrain")
	(0 < feature <= 2) || error("Feature must be between 0 and 200 %")
	(0 <= lowlands <= 0.9) || error("Lowlands must be between 0 and 0.9")
	(0 <= hillslope <= 1) || error("Hillslope must be between 0 and 1")
	(0.2 <= concavity <= 0.8) || error("Concavity must be between 0.2 and 0.8")
	z0, z1 = Float64.(Tuple(zrange));  z1 > z0 || error("z max must be above z min")

	# the working lattice: the output one if it is small enough, else the same region at fewer nodes
	f = min(1.0, _ST_WORK / max(nx, ny))
	wx = max(3, round(Int, nx * f));  wy = max(3, round(Int, ny * f))
	h = _st_landscape(wx, wy; feature = Float64(feature) * max(wx, wy), lowlands = Float64(lowlands),
	                  hillslope = Float64(hillslope), concavity = Float64(concavity), seed = seed)
	# x/y off the region and the node COUNT (never an x0:dx:x1 range that can lose a node to rounding)
	xo = collect(range(x0, x1, length = nx));  yo = collect(range(y0, y1, length = ny))
	if (wx, wy) != (nx, ny)
		Gw = GMT.mat2grid(Float32.(h); x = collect(range(x0, x1, length = wx)), y = collect(range(y0, y1, length = wy)))
		Gs = GMT.grdsample(Gw, region = (x0, x1, y0, y1), inc = (dx, dy))
		size(Gs.z) == (ny, nx) || error("grdsample gave $(size(Gs.z)) nodes, expected ($ny, $nx)")
		h = Float64.(collect(_zmat(Gs)))
	end
	lo, hi = extrema(h)                                # bicubic can overshoot a little: span z exactly
	z = Float32.(z0 .+ (z1 - z0) .* ((h .- lo) ./ max(hi - lo, eps())))
	G = GMT.mat2grid(z; x = xo, y = yo)
	geog && (G.proj4 = GMT.prj4WGS84; G.geog = 1)
	return G
end

# The settings of Grid Tools > Synthetic terrain > auto, and what the dialog's boxes hold at open
# (the region when the window has no grid: 70_window.cpp prefills the same numbers).
const _ST_DEFAULTS = Dict("x0" => "-13", "x1" => "23", "y0" => "30", "y1" => "60", "dx" => "0.05",
                          "dy" => "0.05", "z0" => "0", "z1" => "1000", "feature" => "30", "lowlands" => "0.1",
                          "hillslope" => "0.3", "concavity" => "0.45")

# C callback. Keys: x0 x1 y0 y1 dx dy z0 z1 feature lowlands hillslope concavity seed (the
# dialog's Compute), or "auto=1" alone (the menu's "auto": _ST_DEFAULTS and a new seed). 1 = added to
# the window, 0 = failed (reason in the Errors console).
function _on_synthterrain(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))
		num(k) = (v = tryparse(Float64, strip(_get(d, k, get(_ST_DEFAULTS, k, ""))));
		          v === nothing ? error("`$k` is not a number") : v)
		seed = haskey(d, "seed") ? round(Int, num("seed")) : rand(1:999983)
		haskey(d, "seed") || (d["seed"] = string(seed))
		for (k, v) in _ST_DEFAULTS  haskey(d, k) || (d[k] = v)  end
		G = synth_terrain((num("x0"), num("x1"), num("y0"), num("y1")), (num("dx"), num("dy"));
		                  zrange = (num("z0"), num("z1")), feature = num("feature") / 100,
		                  lowlands = num("lowlands"), hillslope = num("hillslope"),
		                  concavity = num("concavity"), seed = seed)
		recipe = "synthetic terrain: " * join(("$k=$(strip(_get(d, k)))" for k in ("feature", "lowlands",
		          "hillslope", "concavity", "seed")), ' ')
		return _gm3d_deliver(scene, G, "Synthetic terrain (seed $seed)", "", false, recipe)
	catch e
		_tool_failed(scene, "Synthetic terrain", e)
		return Cint(0)
	end
end

# NOTHING OF THIS RUNS AT START-UP: the menu entry fires warmupTool("synthterrain") and waits for the
# callback; the same warm-up compiles the maths on a tiny grid while the dialog is being filled in.
const _ST_WIRED = Ref(false)
function _register_synthterrain()
	if !_ST_WIRED[] && haskey(_LIB_FNS, :gmtvtk_set_synthterrain_callback)
		f = @cfunction((s, c) -> Base.invokelatest(_on_synthterrain, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
		ccall(_fn(:gmtvtk_set_synthterrain_callback), Cvoid, (Ptr{Cvoid},), f)
		_ST_WIRED[] = true
	end
	synth_terrain((0, 1, 0, 1), (0.0025, 0.0025))    # JIT the maths (401 nodes: the grdsample path too)
	return
end
warm_register("synthterrain", _register_synthterrain)
