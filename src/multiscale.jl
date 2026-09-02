# multiscale.jl — Grid Tools > Terrain Modeling: moving-window ("block") terrain analysis. Port of
# Mirone's `mex/mirblock.c` (the MEX behind `src_figs/multiscale.m`, called from mirone.m's
# 'Multiscale' branch). The dialog is deps/ui/multiscale.ui (MultiscaleDialog, 70_window.cpp): pick
# a Method and a Window size, and the chosen quantity is computed for every node from the n×n
# neighbourhood centred on it.
#
# The first six methods are the Wilson et al. 2007 (Marine Geodesy 30:3-35) family; the rest are the
# plane-fit quantities and the two AGCs. The method numbering below IS mirblock's -A<n> and the
# dialog's popup order — the .m warns in capitals that the two must agree, so they are listed once,
# here, and the .ui repeats the same order.
#
# BORDERS. mirblock computes the interior in one pass and then re-runs itself four more times on
# mirrored strips to fill W/E/N/S, patching the four corners afterwards with a diagonal neighbour
# ("Since this bloody thing still f on the corners I give up", mirblock.c:378). It goes to that
# trouble to avoid copying the array — a real concern in 2009. This port keeps the border condition
# (mirroring, edge row/column NOT repeated) but applies it the straightforward way, by padding a
# copy: identical windows everywhere, and the corners come out of the same code path as everything
# else instead of being faked.

# -A<n> ⇔ popup order. Also the Scene Objects name of the result.
const MIRBLOCK_METHODS = ("Terrain Ruggedness Index", "Topographic Position Index",
                          "Roughness (aka Range)", "Mean", "Min", "Max", "Slope", "Aspect", "RMS",
                          "Trend", "Residue", "RMS of Residue", "AGC (Full Amp)", "AGC (Local Amp)")

# mirblock.c's spherical approximation for degrees -> metres (authalic radius 6371005.076 m). Only
# Slope and Aspect need it, and only for a geographic grid.
const _MB_M_PER_DEG = 111195.01524

# Mirror an out-of-range index back inside 1:n WITHOUT repeating the edge (row 0 -> 2, row n+1 ->
# n-1) — mirblock.c's own padding rule, which copies columns nHalfWin…1 to the left of column 0.
@inline _mb_reflect(k::Int, n::Int) = k < 1 ? 2 - k : (k > n ? 2n - k : k)

# `z` with `h` mirrored rows/columns added on every side.
function _mb_pad(z::Matrix{Float32}, h::Int)
	ny, nx = size(z)
	(ny > h && nx > h) || error("Terrain Modeling: window ($(2h + 1)) is too large for a $(ny)×$(nx) grid")
	P = Matrix{Float32}(undef, ny + 2h, nx + 2h)
	@inbounds for j = 1:(nx + 2h)
		jj = _mb_reflect(j - h, nx)
		for i = 1:(ny + 2h)
			P[i, j] = z[_mb_reflect(i - h, ny), jj]
		end
	end
	return P
end

# --- the simple statistics (mirblock.c TRI / TPI / roughness / average / block_min / block_max /
# block_rms). One loop shape for all of them, with the .c's shared conventions:
#   * a NaN CENTRE gives NaN out — the window is not even looked at;
#   * NaN cells inside the window are skipped and the divisor is the number of GOOD cells;
#   * the centre itself is part of its own window (it contributes 0 to TRI, and it is counted).
function _mb_stats(P::Matrix{Float32}, ny::Int, nx::Int, h::Int, kind::Symbol)
	R = Matrix{Float32}(undef, ny, nx)
	# Min/Max are the same sweep keeping one end-member instead of a combination — their own loop, so
	# the general one below stays free of a branch that would run for every cell of every window.
	if kind === :min || kind === :max
		@inbounds for j = 1:nx, i = 1:ny
			c = P[i + h, j + h]
			if isnan(c);  R[i, j] = NaN32;  continue;  end
			e = c
			for jj = j:(j + 2h), ii = i:(i + 2h)
				v = P[ii, jj]
				isnan(v) && continue
				(kind === :min) ? (v < e && (e = v)) : (v > e && (e = v))
			end
			R[i, j] = e
		end
		return R
	end
	@inbounds for j = 1:nx, i = 1:ny
		c = P[i + h, j + h]
		if isnan(c)
			R[i, j] = NaN32
			continue
		end
		acc = 0.0;  acc2 = 0.0;  ngood = 0
		vmin = c;  vmax = c
		for jj = j:(j + 2h), ii = i:(i + 2h)
			v = P[ii, jj]
			isnan(v) && continue
			ngood += 1
			if     kind === :tri;   acc += abs(c - v)
			elseif kind === :rms;   acc += v;  acc2 += Float64(v) * v
			elseif kind === :range
				v < vmin && (vmin = v)
				v > vmax && (vmax = v)
			else                    acc += v                    # :tpi and :mean
			end
		end
		R[i, j] = if kind === :range
			vmax - vmin
		elseif kind === :tri
			Float32(acc / ngood)
		elseif kind === :tpi
			c - Float32(acc / ngood)
		elseif kind === :mean
			Float32(acc / ngood)
		elseif kind === :rms
			ngood == 0 ? 0.0f0 : Float32(sqrt(max(acc2 / ngood - (acc / ngood)^2, 0.0)))
		else
			error("_mb_stats: unknown kind $kind")
		end
	end
	return R
end

# Solve the 3×3 normal equations (Gaussian elimination with partial pivoting, mirblock.c's
# GMT_gauss). Returns the coefficients, or `nothing` when the system is singular — which happens
# only when the window holds too few good nodes to define a plane.
function _mb_solve3(A::Matrix{Float64}, b::Vector{Float64})
	@inbounds for k = 1:3
		p = k
		for i = (k + 1):3;  (abs(A[i, k]) > abs(A[p, k])) && (p = i);  end
		if abs(A[p, k]) < 1.0e-8;  return nothing;  end
		if p != k
			for j = 1:3;  A[k, j], A[p, j] = A[p, j], A[k, j];  end
			b[k], b[p] = b[p], b[k]
		end
		for i = (k + 1):3
			f = A[i, k] / A[k, k]
			for j = k:3;  A[i, j] -= f * A[k, j];  end
			b[i] -= f * b[k]
		end
	end
	@inbounds for i = 3:-1:1
		s = b[i]
		for j = (i + 1):3;  s -= A[i, j] * b[j];  end
		b[i] = s / A[i, i]
	end
	return b
end

# --- the plane-fit family (mirblock.c surface_fit): Slope, Aspect, Trend, Residue, RMS of Residue.
#
# With n_model = 3 the "Chebyshev" basis of the .c degenerates to {1, u, v} — a plain LEAST-SQUARES
# PLANE over the window, in normalised coordinates u (along rows/Y) and v (along columns/X), both
# running -1 … 1 across the window. So the fit is z ≈ a0 + a_u·u + a_v·v, and
#
#   Trend    = a0                     (the plane at the window centre, where u = v = 0)
#   Residue  = z_centre - a0
#   dz/dY    = 2·a_u / y_span,  dz/dX = 2·a_v / x_span      (what GMT_cheb_to_pol does for degree 1)
#   Slope    = atand(hypot(dz/dY, dz/dX))
#   Aspect   = -(90 + atand(dz/dY, dz/dX)), wrapped into [0,360)
#
# `y_span` = (w-1)·y_inc and `x_span` = (w-1)·x_inc, in metres for a geographic grid (the .c's
# spherical degree->metre factor, with x also scaled by cos(latitude of the window's centre row)).
function _mb_surface_fit(P::Matrix{Float32}, ny::Int, nx::Int, h::Int, kind::Symbol,
                         y0::Float64, yinc::Float64, xinc::Float64, geog::Bool)
	R = Matrix{Float32}(undef, ny, nx)
	w = 2h + 1
	t = [-1.0 + (k - 1) * (2.0 / (w - 1)) for k = 1:w];  t[w] = 1.0     # xval/yval of the .c
	mpd = geog ? _MB_M_PER_DEG : 1.0
	yspan = (w - 1) * yinc * mpd
	A = Matrix{Float64}(undef, 3, 3);  b = Vector{Float64}(undef, 3)
	@inbounds for i = 1:ny
		# Latitude of THIS output row (the window's centre row), as in the .c's co[] table.
		xspan = (w - 1) * xinc * mpd * (geog ? cos((y0 + (i - 1) * yinc) * pi / 180) : 1.0)
		for j = 1:nx
			c = P[i + h, j + h]
			if isnan(c)
				R[i, j] = NaN32
				continue
			end
			# Normal equations. NaN nodes are simply not accumulated (load_gtg_and_gtd skips them),
			# so the plane is fitted to whatever good nodes the window holds.
			n = 0.0;  su = 0.0;  sv = 0.0;  suu = 0.0;  suv = 0.0;  svv = 0.0
			sz = 0.0;  szu = 0.0;  szv = 0.0
			for jj = 1:w
				v = t[jj]
				for ii = 1:w
					zv = P[i + ii - 1, j + jj - 1]
					isnan(zv) && continue
					u = t[ii]
					n += 1;  su += u;  sv += v
					suu += u * u;  suv += u * v;  svv += v * v
					sz += zv;  szu += zv * u;  szv += zv * v
				end
			end
			A[1, 1] = n;    A[1, 2] = su;   A[1, 3] = sv
			A[2, 1] = su;   A[2, 2] = suu;  A[2, 3] = suv
			A[3, 1] = sv;   A[3, 2] = suv;  A[3, 3] = svv
			b[1] = sz;  b[2] = szu;  b[3] = szv
			sol = _mb_solve3(A, b)
			a0, au, av = sol === nothing ? (n > 0 ? sz / n : Float64(c), 0.0, 0.0) :
			                               (sol[1], sol[2], sol[3])

			if kind === :trend
				R[i, j] = Float32(a0)
			elseif kind === :residue
				R[i, j] = Float32(c - a0)
			elseif kind === :res_rms
				# The .c sums over the WHOLE window here (n_win2), NaNs included — so a hole anywhere
				# in the window makes the node NaN, unlike the fit above which just ignores it. Kept
				# as it is: it is what mirblock returns, and AGC (Local Amp) is built on top of it.
				m = 0.0;  q = 0.0
				for jj = 1:w, ii = 1:w
					d = P[i + ii - 1, j + jj - 1] - (a0 + t[ii] * au + t[jj] * av)
					m += d;  q += d * d
				end
				m /= w * w;  q /= w * w
				R[i, j] = Float32(sqrt(max(q - m * m, 0.0)))
			else                                                  # :slope / :aspect
				dzdy = 2 * au / yspan
				dzdx = 2 * av / xspan
				if kind === :slope
					R[i, j] = Float32(atand(hypot(dzdy, dzdx)))
				else
					a = -(90 + atand(dzdy, dzdx))
					R[i, j] = Float32(a < 0 ? a + 360 : a)
				end
			end
		end
	end
	return R
end

"""
    R = mirblock(G::GMTgrid; method=0, win=3, geog=nothing)

Moving-window terrain analysis of grid `G` — Grid Tools > Terrain Modeling, the port of Mirone's
`mirblock` MEX. `method` is the same number the MEX's `-A` takes and the same order the dialog
lists (see `InteractiveGMT.MIRBLOCK_METHODS`); `win` is the (odd) neighbourhood width. `geog` tells
Slope/Aspect whether x,y are degrees and must be converted to metres; `nothing` reads it off the
grid.

Returns a new `GMTgrid` with `G`'s geometry.
"""
function mirblock(G::GMTgrid; method::Int=0, win::Int=3, geog=nothing)
	(0 <= method < length(MIRBLOCK_METHODS)) ||
		error("Terrain Modeling: unknown method $method (0…$(length(MIRBLOCK_METHODS) - 1))")
	(win >= 3 && isodd(win)) || error("Terrain Modeling: window size must be an odd number >= 3 (got $win)")
	h = win ÷ 2
	z = Matrix{Float32}(_zmat(G))   # (ny,nx), row 1 = south, for ANY layout the grid was read in
	ny, nx = size(z)
	P = _mb_pad(z, h)
	isgeog = geog === nothing ? (_isgeog(G) != 0) : Bool(geog)
	y0 = Float64(G.range[3]);  yinc = Float64(G.inc[2]);  xinc = Float64(G.inc[1])
	fit(kind) = _mb_surface_fit(P, ny, nx, h, kind, y0, yinc, xinc, isgeog)

	R = if     method == 0;   _mb_stats(P, ny, nx, h, :tri)
	elseif method == 1;   _mb_stats(P, ny, nx, h, :tpi)
	elseif method == 2;   _mb_stats(P, ny, nx, h, :range)
	elseif method == 3;   _mb_stats(P, ny, nx, h, :mean)
	elseif method == 4;   _mb_stats(P, ny, nx, h, :min)
	elseif method == 5;   _mb_stats(P, ny, nx, h, :max)
	elseif method == 6;   fit(:slope)
	elseif method == 7;   fit(:aspect)
	elseif method == 8;   _mb_stats(P, ny, nx, h, :rms)
	elseif method == 9;   fit(:trend)
	elseif method == 10;  fit(:residue)
	elseif method == 11;  fit(:res_rms)
	elseif method == 12
		# AGC, Full Amplitude: scale every node so its local RMS reaches the largest local RMS found
		# anywhere, amplification capped at 10 (mirblock.c callAlgo's tail).
		rms = _mb_stats(P, ny, nx, h, :rms)
		rmax = _mb_maxfinite(rms)
		out = similar(rms)
		@inbounds for k in eachindex(rms)
			out[k] = isnan(rms[k]) ? NaN32 : z[k] * Float32(_mb_cap(rmax / rms[k], 10.0))
		end
		out
	else
		# AGC, Local Amplitude: same idea applied to the DETRENDED field — the local trend is put
		# back afterwards, so only the wiggles around it are amplified. Cap 20.
		rms = fit(:res_rms)
		tr  = fit(:trend)
		rmax = _mb_maxfinite(rms)
		out = similar(rms)
		@inbounds for k in eachindex(rms)
			out[k] = isnan(z[k]) ? NaN32 :
			         Float32((z[k] - tr[k]) * _mb_cap(rmax / rms[k], 20.0) + tr[k])
		end
		out
	end

	# mirone.m turns whatever Inf came out of those divisions into NaN before displaying.
	@inbounds for k in eachindex(R);  isinf(R[k]) && (R[k] = NaN32);  end
	return GMT.mat2grid(R, G)
end

# The .c's amplification cap is the C MIN() MACRO, i.e. a bare `<` comparison — so a NaN ratio (which
# a 0/0 gives on a perfectly flat window) does NOT propagate there, it falls through to the cap.
# Julia's `min` propagates NaN instead, which would turn those nodes into holes. Same comparison as
# the macro, so the same numbers come out.
@inline _mb_cap(a::Float64, cap::Float64) = (a < cap) ? a : cap

# Largest finite value, the maximum mirblock accumulates over its five passes (NaNs skipped). 0 when
# there is nothing finite, exactly like the .c's `rmsMax = 0` starting point.
function _mb_maxfinite(A::Matrix{Float32})
	m = 0.0
	@inbounds for v in A
		(isnan(v) || isinf(v)) && continue
		v > m && (m = Float64(v))
	end
	return m
end

# Menu entry (g_juliaEval, like the other Grid Tools tools). The result is published through the
# SHARED derived-grid path `_gm3d_deliver` — new named handle, checked, siblings unchecked, Scene
# Objects unfolded, axes re-framed (SACRED_LAW.md derived-variable laws). The handle is named after
# the method, so two different methods coexist and re-running one replaces its own result.
function _on_multiscale(scene::Ptr{Cvoid}, method::Int, win::Int)
	try
		G = _find_object(scene, :grid, "")
		(G isa GMTgrid) || error("No grid loaded in this window")
		R = mirblock(G; method=method, win=win)
		name = MIRBLOCK_METHODS[method + 1]
		method == 6 && (name = "Slope in degrees")        # mirone.m relabels this one
		_grid_command!(R, "InteractiveGMT mirblock -A$method -W$win")
		_gm3d_deliver(scene, R, "$name ($(win)x$(win))", "", false, "mirblock")
	catch e
		_tool_failed(scene, "Terrain Modeling", e)
	end
	return nothing
end
