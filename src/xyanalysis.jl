# xyanalysis.jl — the X,Y plot tool's Analysis menu (Phase 2). Each menu item runs a pure-Julia
# transform on the Object-Manager-selected series and either ADDS a new line to the same window
# (mean / trend / derivative — same x domain) or opens a NEW plot window (spectrum / autocorr —
# different x domain, as Mirone's ecran does). No new package deps: the FFT is a small in-house
# radix-2 (Project.toml is off-limits; profiles are modest so this is plenty fast).

# --- in-house iterative radix-2 FFT (length MUST be a power of two) --------------------------
function _fft_radix2!(a::Vector{ComplexF64})
	n = length(a)
	j = 0                                            # bit-reversal permutation
	for i in 1:n-1
		bit = n >> 1
		while (j & bit) != 0
			j ⊻= bit; bit >>= 1
		end
		j |= bit
		if i < j
			a[i+1], a[j+1] = a[j+1], a[i+1]
		end
	end
	len = 2                                          # butterflies
	while len <= n
		wlen = cis(-2pi / len)
		half = len >> 1
		i = 0
		while i < n
			w = ComplexF64(1.0, 0.0)
			for k in 0:half-1
				u = a[i+k+1]
				v = a[i+k+half+1] * w
				a[i+k+1]      = u + v
				a[i+k+half+1] = u - v
				w *= wlen
			end
			i += len
		end
		len <<= 1
	end
	return a
end

# Zero-pad y to the next power of two and FFT it. Returns (F, N, dx) where N is the real sample
# count and dx the (assumed uniform) sample spacing taken from the x endpoints.
function _padded_fft(x, y)
	N = length(y)
	N < 4 && error("need at least 4 points")
	dx = (x[end] - x[1]) / (N - 1)
	dx == 0 && error("x has zero span")
	M = nextpow(2, N)
	a = zeros(ComplexF64, M)
	@inbounds for i in 1:N
		a[i] = y[i]
	end
	_fft_radix2!(a)
	return a, N, dx, M
end

# One-sided amplitude spectrum: (frequency, amplitude).
function _amp_spectrum(x, y)
	F, N, dx, M = _padded_fft(x, y)
	half = M >> 1
	freqs = Float64[k / (M * dx)      for k in 0:half]
	amp   = Float64[2 * abs(F[k+1]) / N for k in 0:half]
	amp[1] = abs(F[1]) / N                           # DC term is not doubled
	return freqs, amp
end

# One-sided power spectral density: (frequency, power).
function _psd(x, y)
	F, N, dx, M = _padded_fft(x, y)
	half = M >> 1
	fs = 1 / dx
	freqs = Float64[k / (M * dx)              for k in 0:half]
	psd   = Float64[2 * abs2(F[k+1]) / (N * fs) for k in 0:half]
	psd[1] = abs2(F[1]) / (N * fs)
	return freqs, psd
end

# Finite-difference gradient on a possibly non-uniform x (central inside, one-sided at the ends).
function _grad(x, y)
	n = length(y)
	g = similar(y)
	@inbounds for i in 2:n-1
		g[i] = (y[i+1] - y[i-1]) / (x[i+1] - x[i-1])
	end
	g[1] = (y[2]   - y[1])   / (x[2]   - x[1])
	g[n] = (y[n]   - y[n-1]) / (x[n]   - x[n-1])
	return g
end

# `order`-th derivative by repeated finite differencing.
function _deriv(x, y, order::Int)
	d = copy(y)
	for _ in 1:order
		d = _grad(x, d)
	end
	return d
end

# Normalised autocorrelation vs lag (lag expressed in x units). Returns (lag, r).
function _autocorr(x, y)
	N = length(y)
	N < 4 && error("need at least 4 points")
	m = sum(y) / N
	yc = y .- m
	denom = sum(abs2, yc)
	denom == 0 && error("series is constant")
	dx = (x[end] - x[1]) / (N - 1)
	lags = 0:N-1
	r  = Float64[sum(@views yc[1:N-k] .* yc[1+k:N]) / denom for k in lags]
	xl = Float64[k * dx for k in lags]
	return xl, r
end

# --- small dense linear solve (Gaussian elimination, partial pivot) -------------------------
# For the tiny normal-equation systems of poly-fit / Savitzky-Golay (no LinearAlgebra dep here).
function _gauss_solve(A::Matrix{Float64}, b::Vector{Float64})
	n = length(b)
	M = copy(A); v = copy(b)
	@inbounds for k in 1:n
		p = k                                        # partial pivot
		for i in k+1:n
			abs(M[i, k]) > abs(M[p, k]) && (p = i)
		end
		if p != k
			M[k, :], M[p, :] = M[p, :], M[k, :]
			v[k], v[p] = v[p], v[k]
		end
		piv = M[k, k]
		piv == 0 && (piv = eps())
		for i in k+1:n
			f = M[i, k] / piv
			M[i, k:n] .-= f .* M[k, k:n]
			v[i] -= f * v[k]
		end
	end
	x = zeros(n)                                     # back-substitution
	@inbounds for k in n:-1:1
		s = v[k]
		for j in k+1:n
			s -= M[k, j] * x[j]
		end
		x[k] = s / (M[k, k] == 0 ? eps() : M[k, k])
	end
	return x
end

# Least-squares polynomial coefficients (c[1] + c[2]u + …) via the normal equations on `u`.
function _polyfit_raw(u, yy, deg::Int)
	n = deg + 1
	s = zeros(2deg + 1); p = ones(length(u))         # power sums Σ u^k
	for k in 0:2deg
		s[k+1] = sum(p); p .*= u
	end
	t = zeros(n); p = ones(length(u))                # Σ y u^k
	for k in 0:deg
		t[k+1] = sum(yy .* p); p .*= u
	end
	A = [s[i+j-1] for i in 1:n, j in 1:n]
	return _gauss_solve(A, copy(t))
end

# Fit a degree-`deg` polynomial to (x,y) and return the fitted ŷ. x is centered+scaled first so the
# Vandermonde stays well-conditioned even for large-magnitude x (e.g. distance in metres).
function _polyfit_eval(x, y, deg::Int)
	deg = clamp(deg, 1, length(x) - 1)
	mx = sum(x) / length(x)
	sx = maximum(abs.(x .- mx)); sx = sx == 0 ? 1.0 : sx
	c = _polyfit_raw((x .- mx) ./ sx, y, deg)
	return [sum(c[k] * ((xi - mx) / sx)^(k - 1) for k in eachindex(c)) for xi in x]
end

# Savitzky-Golay smoothing: at each sample fit a local polynomial over a centered window (clamped
# at the ends) and take its value at the center. `half` = window half-width, `order` = poly degree.
function _savgol(y, half::Int, order::Int)
	N = length(y); out = similar(y)
	@inbounds for i in 1:N
		lo = max(1, i - half); hi = min(N, i + half)
		u = Float64.((lo:hi) .- i)
		d = min(order, length(u) - 1)
		c = _polyfit_raw(u, y[lo:hi], d)
		out[i] = c[1]                                # polynomial value at u = 0
	end
	return out
end

# In-place inverse radix-2 FFT (conjugate trick).
function _ifft_radix2!(a::Vector{ComplexF64})
	a .= conj.(a)
	_fft_radix2!(a)
	a .= conj.(a)
	a ./= length(a)
	return a
end

# Zero-phase Butterworth filter applied in the frequency domain. `fc` = cutoff (same 1/x units as
# the FFT frequency), `ftype` = "low" or "high", fixed order 4. Returns the filtered y (length N).
function _butter(x, y, fc::Float64, ftype)
	F, N, dt, M = _padded_fft(x, y)
	const_order = 4
	@inbounds for k in 0:M-1
		f  = k <= M ÷ 2 ? k / (M * dt) : (k - M) / (M * dt)
		af = abs(f)
		H = if ftype == "low"
			af == 0 ? 1.0 : 1 / sqrt(1 + (af / fc)^(2 * const_order))
		else
			af == 0 ? 0.0 : 1 / sqrt(1 + (fc / af)^(2 * const_order))
		end
		F[k+1] *= H
	end
	_ifft_radix2!(F)
	return real.(@view F[1:N])
end

# Spector & Grant depth-to-sources: on a radially-averaged (or 1-D) magnetic POWER spectrum the
# log-power vs wavenumber is ~linear with slope ≈ -4π·h, where h is the depth to the top of the
# source ensemble. Fit ln(power) over the band [f1,f2] (f2<=f1 -> whole series), then
# depth = |slope|/(4π) · xfac (xfac converts the wavenumber unit to metres: 1/m→1, 1/km→1000,
# 1/NM→1852). Returns (band-x, fitted power curve, depth_metres).
function _spector_grant(x, y, f1::Real, f2::Real, xfac::Real)
	xv = Float64.(x); yv = Float64.(y)
	mask = f2 > f1 ? ((xv .>= f1) .& (xv .<= f2)) : trues(length(xv))
	keep = mask .& (yv .> 0)                          # log needs positive power
	xs = xv[keep]; ys = yv[keep]
	length(xs) < 2 && error("Spector-Grant: need ≥ 2 positive-power points in the band")
	ly = log.(ys)
	mx = sum(xs) / length(xs); my = sum(ly) / length(ly)
	sxx = sum((xs .- mx) .^ 2)
	slope = sxx == 0 ? 0.0 : sum((xs .- mx) .* (ly .- my)) / sxx
	b = my - slope * mx
	depth = abs(slope) / (4π) * xfac
	return xs, exp.(slope .* xs .+ b), depth          # fit line back in power units, to overlay
end

# Median of a vector (no Statistics dep).
function _median(v)
	s = sort(v); n = length(s)
	return isodd(n) ? s[(n + 1) ÷ 2] : 0.5 * (s[n ÷ 2] + s[n ÷ 2 + 1])
end

# Replace the y values flagged `bad` by linear interpolation between the nearest good neighbours
# (flat-extrapolate at the ends). In place on `out`.
function _interp_fill!(x, out, bad)
	gi = findall(.!bad)
	isempty(gi) && return out
	@inbounds for i in findall(bad)
		j = searchsortedfirst(gi, i)
		if j == 1
			out[i] = out[gi[1]]                       # before the first good point
		elseif j > length(gi)
			out[i] = out[gi[end]]                     # after the last good point
		else
			l = gi[j-1]; r = gi[j]
			t = (x[i] - x[l]) / (x[r] - x[l])
			out[i] = out[l] + t * (out[r] - out[l])
		end
	end
	return out
end

# Despike (ecran "Filter Outliers"): flag points whose residual from a smooth baseline exceeds
# `nsigma` robust σ (MAD-based — the spikes don't inflate it as a plain std would), then replace
# them by interpolation from the inliers. Baseline = Savitzky-Golay (ecran uses a csaps spline).
function _despike(x, y; nsigma::Real=2.0, window::Int=11)
	n = length(y)
	n < 5 && return copy(y)
	sm  = _savgol(y, max(1, window ÷ 2), 3)
	res = y .- sm
	m   = _median(res)
	mad = _median(abs.(res .- m))
	thr = mad > 0 ? nsigma * 1.4826 * mad : nsigma * sqrt(sum(abs2, res .- m) / n)
	bad = abs.(res .- m) .> thr
	out = copy(y)
	any(bad) && _interp_fill!(x, out, bad)
	return out
end

# Subtract the least-squares linear trend.
function _detrend(x, y)
	N = length(x)
	sx = sum(x); sy = sum(y); sxx = sum(abs2, x); sxy = sum(x .* y)
	den = N * sxx - sx^2
	den == 0 && return y .- sy / N                   # degenerate x -> just remove the mean
	b = (N * sxy - sx * sy) / den
	a = (sy - b * sx) / N
	return y .- (a .+ b .* x)
end

# Ops that produce a DIFFERENT x domain -> open a fresh plot window instead of overlaying.
const _XY_NEWWIN = ("fft_amp", "fft_psd", "autocorr")

# op -> (xout, yout, suffix). nothing if the op is unknown.
function _xy_compute(op, x, y)
	xv = collect(Float64, x); yv = collect(Float64, y)
	# parameterised ops (Phase 3): the C++ dialog encodes the parameter into the op string.
	if startswith(op, "fitpoly:")
		deg = parse(Int, op[9:end])
		return (xv, _polyfit_eval(xv, yv, deg), "fit$deg")
	elseif startswith(op, "savgol:")
		w = parse(Int, op[8:end])
		return (xv, _savgol(yv, max(1, w ÷ 2), 3), "smooth")
	elseif startswith(op, "butter:")
		parts = split(op, ':')                       # butter : type : fc
		return (xv, _butter(xv, yv, parse(Float64, parts[3]), parts[2]), "butter $(parts[2])")
	elseif startswith(op, "despike:")
		return (xv, _despike(xv, yv; nsigma=parse(Float64, op[9:end])), "despiked")
	elseif startswith(op, "specgrant:")
		pp = split(op, ':')                          # specgrant : f1 : f2 : xfac
		fx, fy, depth = _spector_grant(xv, yv, parse(Float64, pp[2]), parse(Float64, pp[3]), parse(Float64, pp[4]))
		return (fx, fy, "S&G depth=$(round(Int, depth)) m")
	end
	op == "remove_mean"  && return (xv, yv .- sum(yv) / length(yv), "− mean")
	op == "remove_trend" && return (xv, _detrend(xv, yv),           "detrend")
	op == "deriv1"       && return (xv, _deriv(xv, yv, 1),          "d/dx")
	op == "deriv2"       && return (xv, _deriv(xv, yv, 2),          "d²/dx²")
	op == "autocorr"     && return (_autocorr(xv, yv)...,           "autocorr")
	op == "fft_amp"      && return (_amp_spectrum(xv, yv)...,       "amplitude")
	op == "fft_psd"      && return (_psd(xv, yv)...,                "PSD")
	return nothing
end

# Analysis-menu callback (C++ Analysis>item -> here). Runs the op on the selected series; adds the
# result to this window, or opens a new one for a different-domain result. Reentrant + guarded
# like the other callbacks.
function _on_xy_analysis(plot::Ptr{Cvoid}, cop::Cstring, sel::Cint)::Cvoid
	op = unsafe_string(cop)
	try
		p = get(_FIGREG, plot, nothing)
		(p isa QtXYPlot) || return
		s = Int(sel)
		(0 <= s < length(p.series)) || return
		x, y, nm = p.series[s+1]
		out = _xy_compute(op, x, y)
		out === nothing && return
		nx, ny, suffix = out
		if op in _XY_NEWWIN
			xl = op == "autocorr" ? "lag" : "frequency"
			yl = op == "fft_psd" ? "power" : (op == "fft_amp" ? "amplitude" : "autocorrelation")
			xyplot(nx, ny; name="$nm $suffix", title="$nm — $suffix", xlabel=xl, ylabel=yl)
		else
			add!(p, nx, ny; name="$nm $suffix")
		end
	catch e
		_dbg("xy-ana", op, "ERR", sprint(showerror, e))
		@warn "xyplot analysis '$op' failed" exception=e
	end
	return
end

# Build the C-callable pointer and install it. Called once from __init__.
function _register_xy_analysis()
	fptr = @cfunction(_on_xy_analysis, Cvoid, (Ptr{Cvoid}, Cstring, Cint))
	ccall(_fn(:gmtvtk_xyplot_set_analysis_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
