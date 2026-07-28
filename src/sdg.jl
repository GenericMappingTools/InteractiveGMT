# sdg.jl — Grid Tools > SDG: the Second Derivative in the direction of the Gradient (port of
# Mirone's mirone.m `GridToolsSDG_CB`). SDG was the method originally recommended to pick the FOS
# (foot of the continental slope) for an article-76 (Law of the Sea) shelf extension: the change of
# gradient measured along the slope's own steepest direction,
#
#     R = (v' H v) / (v' v),   v = grad f = [df/dx, df/dy],   H = the Hessian of f      (Mirone eq. 2)
#
# THE SMOOTHING IS PART OF THE ALGORITHM, NOT A PRE-STEP. Mirone fits a 2-D CUBIC SMOOTHING SPLINE
# (`csaps`, with the user's `p` parameter) to the grid and then takes the spline's ANALYTIC first and
# second derivatives (`fnder`) — that is what makes second derivatives of bathymetry usable at all.
# So `csaps` is ported here in full, from the copy of C. de Boor's Spline Toolbox that Mirone itself
# ships (utils/spl_fun.m, `csaps`/`csaps1`, the algorithm of (XIV.6)ff of "A Practical Guide to
# Splines"), weights all 1 and no roughness weight `lam` — exactly the call GridToolsSDG_CB makes.
#
# `p` in [0,1]: 0 = the least-squares straight line, 1 = the interpolating natural cubic spline; the
# useful values sit just under 1 (hence the 12 decimals Mirone's input box and ours both use). The
# default offered is csaps's OWN estimate, `p = 1/(1 + trace(R)/(6*trace(Q'Q)))`, computed like
# GridToolsSDG_CB does it — off the first 5 nodes of the Y axis.
#
# Tiling has no counterpart here. Mirone tiles because the MATLAB pp-form of the surface costs
# ~n_row*4*n_col*4*8 bytes; this port never builds a pp-form — it solves the same banded system and
# evaluates the derivatives straight at the nodes, so the peak cost is a handful of grid-sized
# Float64 arrays.
#
# NaNs: same treatment as the .m — holes are filled first (Mirone calls its minimum-curvature
# `fillGridGaps`, we call `GMT.fillgaps`, the same operation rtp3d.jl already uses), the SDG is
# computed on the filled field and the original holes are punched back in at the end ("the law of
# NaN conservation").

# --- csaps, univariate (spl_fun.m `csaps1`, w = 1, no lam) ---------------------------------------
#
# The system solved for the interior second derivatives u is
#
#     (6(1-p)·Q'Q + p·R) u = diff(divdif)                                              (csaps1)
#
# with, for dx = diff(x) and odx = 1/dx,
#     R  = tridiag,  R[i,i] = 2(dx[i]+dx[i+1]),  R[i,i+1] = R[i+1,i] = dx[i+1]
#     Q' = (n-2)×n band, row i = [odx[i], -(odx[i]+odx[i+1]), odx[i+1]] at columns i,i+1,i+2
#
# so 6(1-p)Q'Q + p·R is symmetric PENTAdiagonal — factored below by a banded LDL', never assembled
# as a full matrix (and never as a sparse one: SparseArrays would have to be added to Project.toml).

# Diagonals of the csaps system. Returns (d, a, b): main, first and second superdiagonal, all in the
# (n-2)-sized interior numbering.
function _csaps_bands(dx::Vector{Float64}, p::Float64)
	n  = length(dx) + 1
	N  = n - 2
	c  = 6 * (1 - p)
	d = zeros(Float64, N);  a = zeros(Float64, max(N - 1, 0));  b = zeros(Float64, max(N - 2, 0))
	@inbounds for i = 1:N
		q1 =  1 / dx[i]
		q3 =  1 / dx[i+1]
		q2 = -(q1 + q3)
		d[i] = c * (q1 * q1 + q2 * q2 + q3 * q3) + p * 2 * (dx[i] + dx[i+1])
		if i < N
			r1 =  1 / dx[i+1]
			r2 = -(r1 + 1 / dx[i+2])
			a[i] = c * (q2 * r1 + q3 * r2) + p * dx[i+1]
		end
		if i < N - 1
			b[i] = c * (q3 * (1 / dx[i+2]))
		end
	end
	return d, a, b
end

# In-place LDL' of the symmetric pentadiagonal (d, a, b). On return d holds D and (a, b) hold the
# unit-lower factor's two subdiagonals (a[i] = L[i+1,i], b[i] = L[i+2,i]).
function _csaps_ldl!(d::Vector{Float64}, a::Vector{Float64}, b::Vector{Float64})
	N = length(d)
	@inbounds for i = 1:N
		if i > 1;      d[i] -= a[i-1] * a[i-1] * d[i-1];  end
		if i > 2;      d[i] -= b[i-2] * b[i-2] * d[i-2];  end
		(d[i] == 0) && error("csaps: singular system (is p in [0,1] and are the coordinates strictly increasing?)")
		if i < N
			t = a[i]
			(i > 1) && (t -= a[i-1] * b[i-1] * d[i-1])
			a[i] = t / d[i]
		end
		(i < N - 1) && (b[i] /= d[i])
	end
	return nothing
end

# Solve (LDL')U = U in place, for every column of U at once.
function _csaps_ldl_solve!(U::Matrix{Float64}, d::Vector{Float64}, a::Vector{Float64}, b::Vector{Float64})
	N, m = size(U)
	@inbounds for k = 1:m
		for i = 1:N                                  # L y = rhs
			t = U[i, k]
			(i > 1) && (t -= a[i-1] * U[i-1, k])
			(i > 2) && (t -= b[i-2] * U[i-2, k])
			U[i, k] = t
		end
		for i = 1:N;  U[i, k] /= d[i];  end          # D z = y
		for i = N:-1:1                               # L' u = z
			t = U[i, k]
			(i < N)     && (t -= a[i] * U[i+1, k])
			(i < N - 1) && (t -= b[i] * U[i+2, k])
			U[i, k] = t
		end
	end
	return nothing
end

"""
    p = csaps_p_guess(x)

csaps's own estimate of the smoothing parameter for data sites `x`: `1/(1 + trace(R)/(6·trace(Q'Q)))`
(spl_fun.m `csaps1`, the `p < 0` branch). It behaves like `1/(1 + x_unit^3/lambda_unit)`, so it is a
pure function of the node spacing — which is why Mirone reads it off a 5-node corner of the grid.
"""
function csaps_p_guess(x::Vector{Float64})
	n = length(x)
	(n >= 3) || error("csaps: need at least 3 data sites")
	dx = diff(x)
	trR = 0.0;  trQQ = 0.0
	@inbounds for i = 1:(n - 2)
		trR += 2 * (dx[i] + dx[i+1])
		q1 = 1 / dx[i];  q3 = 1 / dx[i+1];  q2 = -(q1 + q3)
		trQQ += q1 * q1 + q2 * q2 + q3 * q3
	end
	return 1 / (1 + trR / (6 * trQQ))
end

"""
    f, f1, f2 = csaps_nodes(x, V, p)

Fit the cubic smoothing spline of `csaps(x, V(:,k), p)` to every COLUMN of `V` (`size(V,1) ==
length(x)`) and return its value, first derivative and second derivative AT THE DATA SITES — the
three things `fnval(fnder(pp,k), x)` gives in the .m, without ever materialising a pp-form.

From csaps1's pp coefficients on interval j (local variable t = x - x[j]):
`f = c1·t³ + 3·c3[j]·t² + c2[j]·t + y[j]`, so at t = 0 the value is `y[j]` (the smoothed data),
`f' = c2[j]` and `f'' = 6·c3[j]`; the last node is evaluated at t = dx[n-1] of interval n-1.
"""
function csaps_nodes(x::Vector{Float64}, V::Matrix{Float64}, p::Float64)
	n, m = size(V)
	(n == length(x)) || error("csaps: V has $n rows but x has $(length(x)) sites")
	(n >= 3) || error("csaps: need at least 3 data sites")
	(0 <= p <= 1) || error("csaps: p must be in [0,1] (got $p)")
	dx = diff(x)

	divdif = Matrix{Float64}(undef, n - 1, m)                 # divided differences
	@inbounds for k = 1:m, i = 1:(n - 1)
		divdif[i, k] = (V[i+1, k] - V[i, k]) / dx[i]
	end
	U = Matrix{Float64}(undef, n - 2, m)                      # rhs = diff(divdif), then u in place
	@inbounds for k = 1:m, i = 1:(n - 2)
		U[i, k] = divdif[i+1, k] - divdif[i, k]
	end
	d, a, b = _csaps_bands(dx, p)
	_csaps_ldl!(d, a, b)
	_csaps_ldl_solve!(U, d, a, b)

	# yi <- yi - 6(1-p)·Q'u, written as csaps1 writes it: diff([0; diff([0;u;0])./dx; 0]).
	f = Matrix{Float64}(undef, n, m)
	c6 = 6 * (1 - p)
	g  = Vector{Float64}(undef, n - 1)                        # diff([0;u;0])./dx
	@inbounds for k = 1:m
		for i = 1:(n - 1)
			ua = (i == 1)     ? 0.0 : U[i-1, k]
			ub = (i == n - 1) ? 0.0 : U[i, k]
			g[i] = (ub - ua) / dx[i]
		end
		for i = 1:n
			ga = (i == 1) ? 0.0 : g[i-1]
			gb = (i == n) ? 0.0 : g[i]
			f[i, k] = V[i, k] - c6 * (gb - ga)
		end
	end

	# c3 = [0; p·u; 0]  ->  f'' = 6·c3 at EVERY node (the natural end condition makes it 0 at both
	# ends, exactly as the pp-form gives).
	f2 = Matrix{Float64}(undef, n, m)
	@inbounds for k = 1:m
		f2[1, k] = 0.0;  f2[n, k] = 0.0
		for i = 2:(n - 1);  f2[i, k] = 6 * p * U[i-1, k];  end
	end

	# c2 = diff(f)./dx - dx.*(2·c3[1:n-1] + c3[2:n]) is f' at the left end of each interval; the last
	# node comes from interval n-1 evaluated at t = dx[n-1].
	f1 = Matrix{Float64}(undef, n, m)
	@inbounds for k = 1:m
		for i = 1:(n - 1)
			c3i = f2[i, k]   / 6
			c3j = f2[i+1, k] / 6
			f1[i, k] = (f[i+1, k] - f[i, k]) / dx[i] - dx[i] * (2 * c3i + c3j)
		end
		c3i = f2[n-1, k] / 6;  c3j = f2[n, k] / 6
		f1[n, k] = f1[n-1, k] + 3 * (c3i + c3j) * dx[n-1]
	end
	return f, f1, f2
end

# --- the SDG itself ------------------------------------------------------------------------------

"""
    R = sdg(G::GMTgrid; p=nothing, sign=:both)

Second Derivative in the direction of the Gradient of grid `G`, returned as a new `GMTgrid` with
`G`'s geometry.

`p` is the cubic-smoothing-spline parameter (0 = least-squares plane, 1 = interpolation); `nothing`
takes csaps's own estimate, [`csaps_p_guess`](@ref), off the Y axis — Mirone's default. `sign` is
`:both` (keep everything), `:positive` (negative values zeroed) or `:negative` (positive values
zeroed) — Mirone's three SDG menu entries.
"""
function sdg(G::GMTgrid; p=nothing, sign::Symbol=:both)
	(sign in (:both, :positive, :negative)) || error("sdg: `sign` must be :both, :positive or :negative")

	# hasnans: 0 = "don't know" (check for real), 1 = confirmed clean, 2 = confirmed has NaNs.
	# The .m fills the gaps first because the spline's reaction to NaNs "makes a hell out of the
	# result with lots of propagation effects".
	has_nans = (G.hasnans == 2) || (G.hasnans == 0 && any(isnan, G.z))
	mask = nothing							# GMTimage{Bool,2} of the filled holes, or nothing
	Gw = G
	if has_nans
		Gw = deepcopy(G);  Gw.hasnans = 2	# fillgaps must not warn about a grid we already checked
		Gw, mask = GMT.fillgaps(Gw)
	end

	# GMT.jl grid layout: z is [ny, nx], row 1 = southernmost. Dimension 1 is Y, dimension 2 is X —
	# the same {Y,X} order csaps is called with in the .m.
	Z = Float64.(Gw.z)
	y = Float64.(collect(Gw.y));  x = Float64.(collect(Gw.x))
	ny, nx = size(Z)
	(length(y) == ny && length(x) == nx) ||
		error("sdg: grid axes ($(length(y))×$(length(x))) do not match z ($(ny)×$(nx))")
	pv = Float64(p === nothing ? csaps_p_guess(y) : p)

	# Tensor-product spline: the surface is the X-smoothing of the Y-smoothing of Z, so every mixed
	# derivative at the nodes is one 1-D operator per dimension applied in turn. Ax/D1x/D2x below are
	# "smooth along x and evaluate the value / 1st / 2nd derivative"; likewise along y.
	#   f_x = Ay·Z·D1x'   f_y  = D1y·Z·Ax'   f_xx = Ay·Z·D2x'   f_yy = D2y·Z·Ax'   f_xy = D1y·Z·D1x'
	Zt = permutedims(Z)                                    # nx × ny: columns run along X
	P0, P1, P2 = csaps_nodes(x, Zt, pv)                    # Z·Ax', Z·D1x', Z·D2x'  (transposed)
	_,  vy, Hyy = csaps_nodes(y, permutedims(P0), pv)      # D1y·(Z Ax'), D2y·(Z Ax')
	vx, Hxy, _  = csaps_nodes(y, permutedims(P1), pv)      # Ay·(Z D1x'), D1y·(Z D1x')
	Hxx, _,  _  = csaps_nodes(y, permutedims(P2), pv)      # Ay·(Z D2x')

	R = Matrix{Float32}(undef, ny, nx)
	@inbounds for j = 1:nx, i = 1:ny
		gx = vx[i, j];  gy = vy[i, j]
		g2 = gx * gx + gy * gy
		# Flat node: the gradient has NO direction, so "the second derivative along it" is undefined.
		# MATLAB's [0 0]/norm([0 0]) makes NaN and poisons the node; 0 is the honest answer (nothing
		# changes along a direction that does not exist) and leaves no speckle holes behind.
		r = (g2 == 0) ? 0.0 :
			(Hxx[i, j] * gx * gx + 2 * Hxy[i, j] * gx * gy + Hyy[i, j] * gy * gy) / g2
		if     (sign === :positive && r < 0);  r = 0.0
		elseif (sign === :negative && r > 0);  r = 0.0
		end
		R[i, j] = Float32(r)
	end

	# `mask` is `nothing` whenever there was nothing to fill (no NaNs at all, or fillgaps found none
	# despite hasnans — its docstring allows that), so the nothing check MUST short-circuit first:
	# isempty(nothing) throws. Same guard as rtp3d.jl.
	(mask !== nothing && !isempty(mask)) && (R[mask.image] .= NaN32)
	return GMT.mat2grid(R, G)
end

# g_juliaEval round-trip for the menu's smoothing prompt: prints the default `p`. GridToolsSDG_CB
# gets it from `csaps({Y(1:5),X(1:5)}, Z(1:5,1:5))` and offers the Y one — p depends on the node
# spacing alone, so 5 nodes are as good as all of them, and it stays this cheap on a huge grid.
function _sdg_default_p(scene::Ptr{Cvoid})
	G = _find_object(scene, :grid, "")
	(G isa GMTgrid) || error("No grid loaded in this window")
	yv = Float64.(collect(G.y))
	(length(yv) >= 3) || error("Grid has too few rows for a smoothing spline")
	print(csaps_p_guess(yv[1:min(5, length(yv))]))
	return nothing
end

# Menu entry (g_juliaEval, like Transplant). `opt` is Mirone's own "positive" / "negative" / "both";
# `p` the smoothing-spline parameter from the prompt. The result is published through the SHARED
# derived-grid path `_gm3d_deliver` — new named handle, checked, siblings unchecked, Scene Objects
# unfolded, axes re-framed (SACRED_LAW.md derived-variable laws).
function _on_sdg(scene::Ptr{Cvoid}, opt::String, p::Float64)
	try
		G = _find_object(scene, :grid, "")
		(G isa GMTgrid) || error("No grid loaded in this window")
		sgn = opt == "positive" ? :positive : opt == "negative" ? :negative : :both
		R = sdg(G; p=p, sign=sgn)
		title = sgn === :both ? "SDG field" : "SDG field ($opt)"
		_grid_command!(R, "InteractiveGMT SDG sign=$opt csaps_p=$p")
		_gm3d_deliver(scene, R, title, "", false, "sdg")
	catch e
		_viewer_log_error(scene, "SDG FAILED: $(sprint(showerror, e))")
		@warn "SDG FAILED" exception=(e,)
	end
	return nothing
end
