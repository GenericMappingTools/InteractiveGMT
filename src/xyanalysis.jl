# xyanalysis.jl — the X,Y plot tool's Analysis menu (Phase 2). Each menu item runs a transform on
# the Object-Manager-selected series and either ADDS a new line to the same window (mean / trend /
# derivative — same x domain) or opens a NEW page (spectrum / autocorr — different x domain, as
# Mirone's ecran does). Spectra (amplitude / PSD) go through GMT.spectrum1d (Welch's method); the
# Butterworth filter does its forward/inverse transform through GMT's own C FFT (GMT_FFT_1D). NO new
# package deps and NO in-house FFT — everything Fourier goes through the GMT library.

# --- 1-D FFT through GMT (no FFTW.jl, no in-house FFT) ------------------------------------------
# Preferred: GMT.jl's `fft1d` wrapper (added to GMT.jl alongside spectrum1d). For GMT versions that
# predate it, we fall back to the SAME C entry point ourselves — ccall GMT_FFT_1D straight through
# the libgmt that GMT.jl already loaded (`GMT.libgmt`) using its live API session (`GMT.G_API[]`).
# Either way the transform is done by the GMT library. The fallback buffer is single-precision,
# interleaved (re, im) of length 2N; the inverse is 1/N-normalised by GMT; N need not be a power of
# two. Once the GMT release with `fft1d` is the minimum, the fallback below can be deleted.
const _GMT_FFT_FWD     = Cint(0)
const _GMT_FFT_INV     = Cint(1)
const _GMT_FFT_COMPLEX = Cuint(1)   # gmt_resources.h: GMT_FFT_REAL = 0U, GMT_FFT_COMPLEX = 1U

# EVERY GMT transform this package runs is preceded by this. On macOS GMT auto-selects Accelerate's
# vDSP for radix-2 sizes, and its 2-D vDSP path SEGFAULTS on Apple silicon (vDSP_fft2d_zop <-
# gmtfft_2d_vDSP <- GMT_FFT_2D), killing the whole Julia process. FFTW is linked into the same
# GMT_jll. The choice lives on the API SESSION, and GMT.jl replaces that session (gmt_restart,
# resetGMT) — so it is set on the LIVE one right before each transform, never once at startup.
function _gmt_fft_session!()
	Sys.isapple() || return nothing
	ccall((:GMT_Set_Default, GMT.libgmt), Cint, (Ptr{Cvoid}, Cstring, Cstring), GMT.G_API[], "GMT_FFT", "fftw")
	return nothing
end

function _gmt_fft_1d!(buf::Vector{Float32}, N::Integer, direction::Cint)
	return ccall((:GMT_FFT_1D, GMT.libgmt), Cint,
	             (Ptr{Cvoid}, Ptr{Cfloat}, UInt64, Cint, Cuint),
	             GMT.G_API[], buf, UInt64(N), direction, _GMT_FFT_COMPLEX)
end

# Complex vector <-> GMT's interleaved single-precision (re, im) buffer.
function _to_interleaved(z::AbstractVector{<:Complex})
	N = length(z); b = Vector{Float32}(undef, 2N)
	@inbounds for i in 1:N
		b[2i-1] = Float32(real(z[i])); b[2i] = Float32(imag(z[i]))
	end
	return b
end
_from_interleaved(b::Vector{Float32}) =
	ComplexF64[ComplexF64(b[2i-1], b[2i]) for i in 1:(length(b) >> 1)]

# Forward FFT of a real series -> complex spectrum (length N), through GMT.
function _gmt_fft(y::AbstractVector{<:Real})
	_gmt_fft_session!()
	isdefined(GMT, :fft1d) && return GMT.fft1d(y)          # GMT.jl's own wrapper, once available
	b = _to_interleaved(ComplexF64.(y))
	_gmt_fft_1d!(b, length(y), _GMT_FFT_FWD) == 0 || error("GMT_FFT_1D forward failed")
	return _from_interleaved(b)
end

# Inverse FFT of a complex spectrum -> complex series (already 1/N-normalised by GMT).
function _gmt_ifft(F::AbstractVector{<:Complex})
	_gmt_fft_session!()
	isdefined(GMT, :fft1d) && return GMT.fft1d(F; inverse=true)
	b = _to_interleaved(F)
	_gmt_fft_1d!(b, length(F), _GMT_FFT_INV) == 0 || error("GMT_FFT_1D inverse failed")
	return _from_interleaved(b)
end

# Power spectral density via GMT's spectrum1d — the proper estimator (Welch's method: ensemble
# averaging of overlapped, Von-Hann-windowed segments, linear-trend removal, Bendat-&-Piersol error
# bars), replacing the old in-house periodogram. The series `y` is treated as a uniformly-sampled
# time series; `sample_dist` (dt) is taken from the x endpoints (spectrum1d knows only the spacing,
# not the abscissa). `size` is the radix-2 segment length for the averaging — the largest power of
# two ≤ N (max frequency resolution). spectrum1d returns a 3-column table (freq, PSD, 1σ error);
# `want=:psd` returns the PSD column, `:amp` its sqrt as a pseudo-amplitude. (FFT-domain filtering —
# the Butterworth op — stays in-house: spectrum1d only estimates spectra, it cannot inverse-transform.)
function _spectrum1d(x, y; want::Symbol=:psd)
	xv = collect(Float64, x); yv = collect(Float64, y)
	N = length(yv)
	N < 4 && error("need at least 4 points for a spectrum")
	dt = (xv[end] - xv[1]) / (N - 1)
	dt == 0 && error("x has zero span")
	seg = prevpow(2, N)
	seg < 2 && error("series too short for spectrum1d")
	# name=true => write NO per-component files (no stray "spectrum.xpower" in cwd), just return the
	# composite (freq, PSD, 1σ) table.
	D = GMT.spectrum1d(reshape(yv, :, 1); size=seg, sample_dist=abs(dt), name=true)
	M = D isa AbstractVector ? D[1] : D
	#A = M isa GMTdataset ? M.data : Matrix{Float64}(M)
	f = Float64.(@view M[:, 1]); p = Float64.(@view M[:, 2])
	return (want === :amp) ? (f, sqrt.(max.(p, 0.0))) : (f, p)
end

# Finite-difference gradient on a possibly non-uniform x (central inside, one-sided at the ends).
function _grad(x, y)
	n = length(y)
	g = similar(y)
	@inbounds for i in 2:n-1
		g[i] = (y[i+1] - y[i-1]) / (x[i+1] - x[i-1])
	end
	g[1] = (y[2] - y[1])   / (x[2] - x[1])
	g[n] = (y[n] - y[n-1]) / (x[n] - x[n-1])
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

# Zero-phase Butterworth filter applied in the frequency domain, transforming through GMT's C FFT.
# `fc` = cutoff (same 1/x units as the FFT frequency), `ftype` = "low" or "high", fixed order 4.
# Returns the filtered y (length N). dt is the (uniform) sample spacing from the x endpoints.
function _butter(x, y, fc::Float64, ftype)
	y0 = collect(Float64, y); N0 = length(y0)
	N0 < 3 && return y0
	dt = (x[end] - x[1]) / (N0 - 1)
	# EDGES: the FFT treats the series as periodic, so a jump between its two ends rings through the
	# filtered result. Extended first, both ends, by the "reflection method" Mirone's filtfilt uses
	# (filter_butter.m): 2*y(1) - y(reversed start), 2*y(end) - y(reversed end) — slopes match at the
	# joins — then filtered, and the extension cut off again.
	m = N0 - 1
	yv = vcat(2y0[1] .- y0[m+1:-1:2], y0, 2y0[end] .- y0[end-1:-1:end-m])
	N = length(yv)
	F = _gmt_fft(yv)                                  # forward transform via GMT
	order = 4
	@inbounds for k in 0:N-1
		f  = k <= N ÷ 2 ? k / (N * dt) : (k - N) / (N * dt)
		af = abs(f)
		H = if ftype == "low"
			af == 0 ? 1.0 : 1 / sqrt(1 + (af / fc)^(2 * order))
		else
			af == 0 ? 0.0 : 1 / sqrt(1 + (fc / af)^(2 * order))
		end
		F[k+1] *= H
	end
	return real.(_gmt_ifft(F))[m+1:m+N0]              # inverse via GMT (1/N-normalised), extension removed
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

# Linear-interpolated quantile of an ALREADY SORTED vector (no Statistics dep) — the IQR that sizes
# an automatic histogram bin.
function _quantile_sorted(s::Vector{Float64}, q::Float64)
	n = length(s)
	n == 0 && return NaN
	n == 1 && return s[1]
	pos = 1 + q * (n - 1)
	lo  = clamp(floor(Int, pos), 1, n)
	hi  = clamp(lo + 1, 1, n)
	return s[lo] + (pos - lo) * (s[hi] - s[lo])
end

# Histogram of `v`: equal-width bins spanning [min, max]. Returns (centres, counts, binwidth) —
# an (x, y) pair the plotter draws as bars, exactly like any other series.
# `nbins <= 0` picks the count itself by Freedman-Diaconis (bin = 2·IQR·n^(-1/3), the rule that
# adapts to the spread instead of the sample size alone), falling back to the √n rule when the IQR
# degenerates (heavily tied data). A constant series has no width to bin: it becomes one bin.
function _histogram(v, nbins::Int=0)
	y = Float64[t for t in v if isfinite(t)]
	n = length(y)
	n < 2 && error("histogram: need at least 2 finite values")
	s = sort(y)
	lo, hi = s[1], s[end]
	if hi <= lo                                        # constant series -> a single unit-wide bin
		return [lo], [Float64(n)], 1.0
	end
	if nbins <= 0
		iqr = _quantile_sorted(s, 0.75) - _quantile_sorted(s, 0.25)
		h   = 2 * iqr * n^(-1 / 3)
		nbins = h > 0 ? ceil(Int, (hi - lo) / h) : ceil(Int, sqrt(n))
		nbins = clamp(nbins, 1, 1000)
	end
	w = (hi - lo) / nbins
	counts = zeros(Float64, nbins)
	@inbounds for t in y
		k = floor(Int, (t - lo) / w) + 1
		counts[clamp(k, 1, nbins)] += 1              # the max lands in the last bin, not past it
	end
	centres = Float64[lo + (k - 0.5) * w for k in 1:nbins]
	return centres, counts, w
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

# Sound Velocity Profile — port of ecran's AnalysisSVP_CB (Mirone; code from FEX:35916, Chad Greene,
# BSD), UNESCO (1983) technical papers in marine science 44, Chen & Millero (1977) JASA 62, 1129-1135.
# T = temperature (°C), S = salinity, P = pressure in DECIBARS (as stored); returns sound speed (m/s).
function _svp_chen_millero(T, S, Pdbar)
	T = Float64.(T); S = Float64.(S)
	P  = Float64.(Pdbar) ./ 10                                     # Chen-Millero wants bars
	SR = sqrt.(abs.(S))
	# S^2 TERM
	D  = 1.727e-3 .- 7.9836e-6 .* P
	# S^3/2 TERM
	B1 =  7.3637e-5 .+ 1.7945e-7 .* T
	B0 = -1.922e-2  .- 4.42e-5   .* T
	B  = B0 .+ B1 .* P
	# S^1 TERM
	A3 = (-3.389e-13    .* T .+ 6.649e-12)  .* T .+ 1.100e-10
	A2 = ((7.988e-12    .* T .- 1.6002e-10) .* T .+ 9.1041e-9) .* T .- 3.9064e-7
	A1 = (((-2.0122e-10 .* T .+ 1.0507e-8)  .* T .- 6.4885e-8) .* T .- 1.2580e-5) .* T .+ 9.4742e-5
	A0 = (((-3.21e-8    .* T .+ 2.006e-6)   .* T .+ 7.164e-5)  .* T .- 1.262e-2)  .* T .+ 1.389
	A  = ((A3 .* P .+ A2) .* P .+ A1) .* P .+ A0
	# S^0 TERM
	C3 = (-2.3643e-12   .* T .+ 3.8504e-10) .* T .- 9.7729e-9
	C2 = (((1.0405e-12  .* T .- 2.5335e-10) .* T .+ 2.5974e-8) .* T .- 1.7107e-6)  .* T .+ 3.1260e-5
	C1 = (((-6.1185e-10 .* T .+ 1.3621e-7)  .* T .- 8.1788e-6) .* T .+ 6.8982e-4)  .* T .+ 0.153563
	C0 = ((((3.1464e-9  .* T .- 1.47800e-6) .* T .+ 3.3420e-4) .* T .- 5.80852e-2) .* T .+ 5.03711) .* T .+ 1402.388
	C  = ((C3 .* P .+ C2) .* P .+ C1) .* P .+ C0
	return C .+ (A .+ B .* SR .+ D .* S) .* S
end

# ecran's get_z_from_p (stripped down from TEOS-10): height z (m, NEGATIVE below the sea surface) from
# sea pressure p (dbar) at latitude lat (degrees north).
function _z_from_p(p, lat::Real)
	gamma = 2.26e-7
	sin2  = sind(lat)^2
	B = 9.780327 * (1 + (5.2792e-3 + (2.32e-5 * sin2)) * sin2)
	A = -0.5 * gamma * B
	C = _enthalpy_SSO_0(Float64.(p))
	return -2 .* C ./ (B .+ sqrt.(B .* B .- 4 .* A .* C))
end

# ecran's gsw_enthalpy_SSO_0 (TEOS-10).
function _enthalpy_SSO_0(p)
	z = p .* 1e-4
	h006 = -2.1078768810e-9; h007 = 2.8019291329e-10
	dyn = z .* (9.726613854843870e-4 .+ z .* (-2.252956605630465e-5 .+ z .* (2.376909655387404e-6 .+
	      z .* (-1.664294869986011e-7 .+ z .* (-5.988108894465758e-9 .+ z .* (h006 .+ h007 .* z))))))
	return dyn .* 1e8                                             # 1e8 = db2Pa*1e4
end

# Ops whose result lives on a DIFFERENT y- or x-scale than the source -> land on a fresh, self-scaled
# PAGE (Excel-like tab) in the same window instead of overlaying. Derivatives keep the same x (time)
# but their y is d/dx (e.g. m/s): on the source's shared y-axis a tide-rate of ~5e-5 collapses to a
# flat zero line. Own page = own autoscale = the curve is actually visible. (Name kept for history.)
const _XY_NEWWIN = ("fft_amp", "fft_psd", "autocorr", "deriv1", "deriv2")

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
	op == "fft_amp"      && return (_spectrum1d(xv, yv; want=:amp)..., "amplitude")
	op == "fft_psd"      && return (_spectrum1d(xv, yv; want=:psd)..., "PSD")
	return nothing
end

# Analysis-menu callback (C++ Analysis>item -> here). Runs the op on the selected series; adds the
# result to this window, or opens a new one for a different-domain result. Reentrant + guarded
# like the other callbacks.
# Push a line into the X,Y window's foldable Console dock (and status bar). The window shows errors
# IN the window — silent stderr @warn is invisible to the user (see this op's history). Best-effort.
function _xy_log(plot::Ptr{Cvoid}, msg::AbstractString; err::Bool=false)
	err && _record_tool_error(msg)      # same failure sink the 3-D viewer's _viewer_log_error feeds
	try
		ccall(_fn(:gmtvtk_xyplot_log), Cvoid, (Ptr{Cvoid}, Cstring, Cint), plot, String(msg), Cint(err))
	catch
	end
	return
end

# Open a fresh self-scaled PAGE (Excel-like tab) in THIS window, set its axis titles, and drop one
# (x,y) curve on it. The add_page ccall switches the current page so the following add! lands on the
# new tab. Shared by the GMT-module front-ends whose output lives on a different scale than the source.
function _xy_addpage!(plot::Ptr{Cvoid}, p::QtXYPlot, pagename::AbstractString, x, y, name::AbstractString;
                      xlabel::AbstractString, ylabel::AbstractString,
                      kind::Symbol=:line, barwidth::Real=0)
	ccall(_fn(:gmtvtk_xyplot_add_page), Cint, (Ptr{Cvoid}, Cstring), plot, String(pagename))
	ccall(_fn(:gmtvtk_xyplot_set_labels), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), plot, String(xlabel), String(ylabel))
	_xyadd!(p, x, y; name=String(name), kind, barwidth)
	return
end

# The plot-type gallery's STATISTICAL entries — GMT.jl's density, boxplot, violin, ecdfplot, qqnorm and
# qqplot. The numbers come from GMT.jl's own helpers (kernelDensity, helper2_boxplot, helper1_violin,
# ecdf, qqbuild, erfinv), the ones those GMT.jl functions draw from; the result is a distribution, not
# the parent's quantity, so it lands on a NEW page of this window. Every series of the current page
# takes part (one curve / box / violin each), except qqplot, which compares the first two.
function _xy_stat(plot::Ptr{Cvoid}, p::QtXYPlot, kind::AbstractString, sel::Int=-1)
	f = get(_XY_STAT_KINDS, kind, nothing)
	f === nothing && error("unknown statistical plot '$kind'")
	n = ccall(_fn(:gmtvtk_xyplot_series_count), Cint, (Ptr{Cvoid},), plot)
	vals, names = Vector{Vector{Float64}}(), String[]
	for k in 0:n-1
		v = filter(isfinite, _xy_get_series(plot, k)[2])
		length(v) < 2 && continue
		push!(vals, v)
		nm = _xy_series_name(plot, k)
		push!(names, isempty(nm) ? "series $k" : nm)
	end
	isempty(vals) && error("$kind: no series on this page has two or more values")
	(kind == "qqplot" && length(vals) < 2) && error("qqplot compares two curves: this page has only one")
	page(name, xl, yl) = (ccall(_fn(:gmtvtk_xyplot_add_page), Cint, (Ptr{Cvoid}, Cstring), plot, String(name));
	                      ccall(_fn(:gmtvtk_xyplot_set_labels), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), plot, String(xl), String(yl)))
	f(plot, p, String(kind), sel, vals, names, page)
	_xy_log(plot, "Plot type: $kind of $(length(vals)) series → new page")
	return
end

function _xy_stat_density(plot, p, kind, sel, vals, names, page)
	page("density", "value", "density")
	for (v, nm) in zip(vals, names)
		D = GMT.kernelDensity(v)
		_xyadd!(p, D.data[:, 1], D.data[:, 2]; name="$nm density")
	end
end
function _xy_stat_ecdf(plot, p, kind, sel, vals, names, page)
	page("ECDF", "value", "cumulative probability")
	for (v, nm) in zip(vals, names)
		xF, F = GMT.ecdf(v)
		_xyadd!(p, xF, F; name="$nm ECDF")
	end
	ccall(_fn(:gmtvtk_xyplot_set_plot_type), Cint, (Ptr{Cvoid}, Cstring), plot, "stairs")   # ecdfplot draws stairs
end
function _xy_stat_boxplot(plot, p, kind, sel, vals, names, page)
	nan = NaN
	page("boxplot", "series", "value")
	# Rows of D: [x  q50  q0  q25  q75  q100  n]; Dol: the outliers (beyond 1.5 IQR) as [x y].
	D, Dol = GMT.helper2_boxplot(vals, Float64[], Float64[], 0.0, "", true, true)
	w = 0.25
	for k in axes(D.data, 1)
		c, q50, q0, q25, q75, q100 = D.data[k, 1:6]
		bx = [c-w, c+w, c+w, c-w, c-w, nan, c-w, c+w, nan, c, c, nan, c, c, nan, c-w/2, c+w/2, nan, c-w/2, c+w/2]
		by = [q25, q25, q75, q75, q25, nan, q50, q50, nan, q0, q25, nan, q75, q100, nan, q0, q0, nan, q100, q100]
		_xyadd!(p, bx, by; name=names[k])
	end
	size(Dol.data, 1) > 0 && _xyadd!(p, Dol.data[:, 1], Dol.data[:, 2]; name="outliers", marker=:circle, linestyle=:none)
end
function _xy_stat_violin(plot, p, kind, sel, vals, names, page)
	page("violin", "series", "value")
	Dv, _, xc = GMT.helper1_violin(vals)
	for k in eachindex(Dv)
		P = Dv[k].data
		# Half-width scaled here to GMT.jl's own 1/(2*N_grp) * 0.75 * groupwidth (= 0.28): its
		# helper1_violin computes that scale but its loop never applies it (GMT.jl statplots.jl).
		hw = maximum(abs.(P[:, 1] .- xc[k]))
		xs = hw > 0 ? xc[k] .+ (P[:, 1] .- xc[k]) .* (0.28 / hw) : P[:, 1]
		_xyadd!(p, xs, P[:, 2]; name=names[k])
	end
end
function _xy_stat_qq(plot, p, kind, sel, vals, names, page)
	allqx = Float64[]
	if kind == "qqnorm"           # GMT.jl's qqplot(x): x against the normal quantiles
		page("qqnorm", "sample quantiles", "normal quantiles")
		for (v, nm) in zip(vals, names)
			pr = (0.5:length(v)) ./ length(v)
			qx, qy = GMT.qqbuild(v, sqrt(2) .* GMT.erfinv(collect(2 .* pr .- 1)))
			_xyadd!(p, qx, qy; name=nm, marker=:circle, linestyle=:none)
			append!(allqx, qx)
		end
	else
		page("qqplot", names[1], names[2])
		qx, qy = GMT.qqbuild(vals[1], vals[2])
		_xyadd!(p, qx, qy; name="$(names[1]) vs $(names[2])", marker=:circle, linestyle=:none)
		append!(allqx, qx)
	end
	lo, hi = extrema(allqx)       # GMT.jl's default qqline=:identity: ys = xs over the x quantiles
	_xyadd!(p, [lo, hi], [lo, hi]; name="identity", color=:black, linestyle=:dash)
end
_xy_stat_framed(plot, p, kind, sel, vals, names, page) = _xy_framed(plot, p, kind, sel, page)

# The statistical kinds, each with its own builder. Reached through this `Function`-typed table ON PURPOSE,
# for the same reason as _XY_ANA_OPS: the first boxplot compiles the boxplot, not every kind (3.6 s
# measured 2026-10-27, when they were the branches of one function).
const _XY_STAT_KINDS = Dict{String,Function}(
	"density" => _xy_stat_density, "ecdfplot" => _xy_stat_ecdf, "boxplot" => _xy_stat_boxplot,
	"violin" => _xy_stat_violin, "qqnorm" => _xy_stat_qq, "qqplot" => _xy_stat_qq,
	(k => _xy_stat_framed for k in ("radar", "rose", "parallelplot", "cornerplot", "marginalhist", "biplot", "feather",
	                              "polarscatter", "polarplot", "compassplot", "piechart", "donutchart"))...)

# ---- the gallery's plot types that draw their OWN frame -----------------------------------------------
# radar, rose, parallelplot, cornerplot, marginalhist and biplot are not curves on an x,y frame: GMT.jl
# draws them with polar frames, per-variable rulers or a matrix of panels. Here they are built from the
# same numbers GMT.jl computes (round_wesn, hst_floats, normalizeArray, zscores, pca, and the scaling its
# radar / biplot functions apply), drawn as series on a page whose own axes are hidden (or kept, for the
# biplot), with the frame, rulers and names drawn as series + page labels. Pages that must not be
# stretched (polar, panels) get a SQUARE plot area.
const _XY_GRID_RGB = (0.6, 0.6, 0.6)
const _XY_BLUE     = (0.0, 0.447, 0.741)             # GMT.jl's "#0072BD"

_xy_lbl(plot, x, y, t; ha=0, va=0, size=11, c=(0.0, 0.0, 0.0)) =
	ccall(_fn(:gmtvtk_xyplot_add_label), Cvoid,
	      (Ptr{Cvoid}, Cdouble, Cdouble, Cstring, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cint),
	      plot, x, y, String(t), c[1], c[2], c[3], size, ha, va)
_xy_look(plot, square::Bool, hide::Bool, lim=(NaN, NaN, NaN, NaN)) =
	ccall(_fn(:gmtvtk_xyplot_set_page_look), Cvoid,
	      (Ptr{Cvoid}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble), plot, square, hide, lim[1], lim[2], lim[3], lim[4])

# add! one series, then draw it as XYSeries kind `kind` (8 = filled polygons).
function _xy_addk!(plot, p, x, y, name, kind; kw...)
	_xyadd!(p, x, y; name=String(name), kw...)
	n = ccall(_fn(:gmtvtk_xyplot_series_count), Cint, (Ptr{Cvoid},), plot)
	ccall(_fn(:gmtvtk_xyplot_set_series_kind), Cint, (Ptr{Cvoid}, Cint, Cint), plot, n - 1, kind)
	return
end

# The current page's series as the columns of one matrix (variables), rows = observations: cut to the
# shortest series, rows with any non-finite value dropped.
function _xy_page_matrix(plot)
	n = ccall(_fn(:gmtvtk_xyplot_series_count), Cint, (Ptr{Cvoid},), plot)
	cols, nms = Vector{Vector{Float64}}(), String[]
	for k in 0:n-1
		y = _xy_get_series(plot, k)[2]
		length(y) < 2 && continue
		push!(cols, y)
		nm = _xy_series_name(plot, k)
		push!(nms, isempty(nm) ? "series $k" : nm)
	end
	isempty(cols) && error("this page has no series with two or more values")
	m = minimum(length.(cols))
	M = reduce(hcat, [c[1:m] for c in cols])
	return M[vec(all(isfinite, M; dims=2)), :], nms
end

# A circle of radius r as a NaN-terminated path.
_xy_circle(r) = (t = range(0, 360; length=73); (vcat(r .* cosd.(t), NaN), vcat(r .* sind.(t), NaN)))

# One rectangle as a NaN-terminated polygon (for the bar panels drawn as filled polygons).
_xy_rect(x0, x1, y0, y1) = ([x0, x1, x1, x0, NaN], [y0, y0, y1, y1, NaN])

# A panel's histogram as filled rectangles in the unit cell at (X0, Y0): `horiz` = bars along x.
function _xy_panel_hist(v, lo, hi, X0, Y0, depth, horiz::Bool)
	c, cnt, w = _histogram(v)
	mx = maximum(cnt)
	xs, ys = Float64[], Float64[]
	for (ci, k) in zip(c, cnt)
		a, b = (ci - w / 2 - lo) / (hi - lo), (ci + w / 2 - lo) / (hi - lo)
		h = depth * k / mx
		rx, ry = horiz ? _xy_rect(X0, X0 + h, Y0 + a, Y0 + b) : _xy_rect(X0 + a, X0 + b, Y0, Y0 + h)
		append!(xs, rx); append!(ys, ry)
	end
	return xs, ys
end

_xy_fmt(v) = string(round(v; sigdigits=4))
_xy_lohi(v) = (r = GMT.round_wesn([extrema(v)..., 0.0, 0.0]); r[2] > r[1] ? (r[1], r[2]) : (r[1] - 0.5, r[1] + 0.5))

# ---- polar axes, feather, pie / donut ---------------------------------------------------------------
# MATLAB's polarplot / polarscatter / compassplot read each series' x as the ANGLE in radians and y as
# the RADIUS. One frame for the three (_xy_polar_frame): rings at round_wesn of the largest radius,
# spokes every 30°, angles counted counter-clockwise from the +x direction (MATLAB's polar axes).
function _xy_polar_frame(p, plot, rmax)
	gx, gy = Float64[], Float64[]
	for f in 0.25:0.25:1.0
		cx, cy = _xy_circle(f); append!(gx, cx); append!(gy, cy)
	end
	for a in 0:30:330  append!(gx, [0.0, cosd(a), NaN]); append!(gy, [0.0, sind(a), NaN])  end
	_xyadd!(p, gx, gy; name="polar grid", color=_XY_GRID_RGB)
	for a in 0:30:330
		_xy_lbl(plot, 1.12cosd(a), 1.12sind(a), "$(a)°"; ha=a in (90, 270) ? 0 : (a < 90 || a > 270 ? -1 : 1), size=11)
	end
	for f in 0.25:0.25:1.0
		_xy_lbl(plot, f * cosd(80), f * sind(80), _xy_fmt(f * rmax); ha=-1, va=-1, size=9, c=(0.35, 0.35, 0.35))
	end
	_xy_look(plot, true, true, (-1.3, 1.3, -1.3, 1.3))
end

function _xy_polar(plot, p, kind, page)
	n = ccall(_fn(:gmtvtk_xyplot_series_count), Cint, (Ptr{Cvoid},), plot)
	ser = Tuple{Vector{Float64},Vector{Float64},String}[]
	for k in 0:n-1                                    # read BEFORE page(): the new page becomes current
		x, y = _xy_get_series(plot, k)
		ok = isfinite.(x) .& isfinite.(y)
		any(ok) || continue
		nm = _xy_series_name(plot, k)
		push!(ser, (x[ok], abs.(y[ok]), isempty(nm) ? "series $k" : nm))
	end
	isempty(ser) && error("$kind: no series on this page has finite (angle, radius) points")
	rmax = GMT.round_wesn([0.0 0 0 maximum(maximum(s[2]) for s in ser)])[4]
	rmax > 0 || (rmax = 1.0)
	page(kind, "", "")
	_xy_polar_frame(p, plot, rmax)
	for (θ, ρ, nm) in ser
		u, v = ρ ./ rmax .* cos.(θ), ρ ./ rmax .* sin.(θ)
		if kind == "polarplot"
			_xyadd!(p, u, v; name=nm)
		elseif kind == "polarscatter"
			_xyadd!(p, u, v; name=nm, marker=:circle, linestyle=:none)
		else                                          # compassplot: an arrow from the origin to each point
			ax, ay = Float64[], Float64[]
			for (a, b) in zip(u, v)
				L = hypot(a, b); L > 0 || continue
				t = atan(b, a); h = 0.06
				append!(ax, [0.0, a, NaN, a, a - h * cos(t - 0.4), NaN, a, a - h * cos(t + 0.4), NaN])
				append!(ay, [0.0, b, NaN, b, b - h * sin(t - 0.4), NaN, b, b - h * sin(t + 0.4), NaN])
			end
			_xyadd!(p, ax, ay; name=nm)
		end
	end
end

# The gallery's feather: the page's first two curves are U and V, X taken from the first; the sticks are
# built by the same _stick_segments / _stick_scale `feather` uses.
function _xy_feather_page(plot, p, page)
	n = ccall(_fn(:gmtvtk_xyplot_series_count), Cint, (Ptr{Cvoid},), plot)
	n < 2 && error("feather needs two curves on this page: U and V")
	(t, u), (_, v) = _xy_get_series(plot, 0), _xy_get_series(plot, 1)
	m = min(length(t), length(u), length(v))
	t, u, v = Float64.(t[1:m]), Float64.(u[1:m]), Float64.(v[1:m])
	ok = isfinite.(t) .& isfinite.(u) .& isfinite.(v)
	t, u, v = t[ok], u[ok], v[ok]
	isempty(t) && error("feather: the first two curves share no finite points")
	segx, segy = _stick_segments(t, u, v, _stick_scale(t, u, v))
	page("feather", "", "")
	_xyadd!(p, segx, segy; name="sticks")
	_stick_arrows!(plot)
	_xyadd!(p, [minimum(t), maximum(t)], [0.0, 0.0]; name="baseline", color=:gray)
end

# GMT.jl's piechart: the selected series' values as slices from 12 o'clock, clockwise; a sum of 1 or
# more is normalized, a smaller sum leaves the pie partial. Labelled with the percentage. donutchart is
# the same pie with a hole (MATLAB's default InnerRadius, 0.5). Slices are cut into pieces of at most
# 10° so each piece is convex, which is what the filled-polygon item fills correctly.
function _xy_pie(plot, p, kind, sel, page)
	sel < 0 && error("$kind: select the series of values in the Object Manager first")
	x = filter(isfinite, _xy_get_series(plot, sel)[2])
	any(<(0), x) && error("$kind: the values must not be negative")
	s = sum(x)
	s > 0 || error("$kind: the values sum to zero")
	X = s > 1 ? x ./ s : x
	r0 = kind == "donutchart" ? 0.5 : 0.0
	page(kind, "", "")
	a1 = 90.0
	for (k, f) in enumerate(X)
		a0 = a1 - 360f
		f > 0 || (a1 = a0; continue)
		px, py = Float64[], Float64[]
		nseg = max(1, ceil(Int, 360f / 10))
		for j in 1:nseg                               # a piece: inner arc, out, outer arc (1° steps) back
			b1 = a1 - (j - 1) * 360f / nseg; b0 = a1 - j * 360f / nseg
			arc = range(b1, b0; length=max(2, ceil(Int, abs(b1 - b0)) + 1))
			append!(px, vcat(r0 .* cosd.(reverse(arc)), cosd.(arc), NaN))
			append!(py, vcat(r0 .* sind.(reverse(arc)), sind.(arc), NaN))
		end
		_xy_addk!(plot, p, px, py, "slice $k", 8; linestyle=:none)
		m = (a0 + a1) / 2
		_xy_lbl(plot, 1.15cosd(m), 1.15sind(m), string(round(Int, 100f), "%"); ha=abs(cosd(m)) < 0.2 ? 0 : (cosd(m) > 0 ? -1 : 1), size=11)
		a1 = a0
	end
	_xy_look(plot, true, true, (-1.3, 1.3, -1.3, 1.3))
end

function _xy_framed_radar(plot, p, kind, sel, page)
	# GMT.jl radar(): one polygon per row, one axis per column. Here a row is a SERIES of the page
	# (its points are the axes). Not normalized -> each axis scaled to round_wesn of its max, as radar()
	# does with `axeslimts`; here the limits are the data's own maxima.
	n = ccall(_fn(:gmtvtk_xyplot_series_count), Cint, (Ptr{Cvoid},), plot)
	rows, nms = Vector{Vector{Float64}}(), String[]
	for k in 0:n-1
		y = _xy_get_series(plot, k)[2]
		length(y) < 3 && continue
		push!(rows, y); nm = _xy_series_name(plot, k); push!(nms, isempty(nm) ? "series $k" : nm)
	end
	isempty(rows) && error("radar: a series needs 3 or more points (one per spoke)")
	na = length(rows[1])
	all(length(r) == na for r in rows) || error("radar: every series must have the same number of points (one per spoke)")
	data = reduce(vcat, permutedims.(rows))
	isnorm = all(t -> 0 <= t <= 1, data)
	lims = isnorm ? ones(na) : [GMT.round_wesn([0.0 0 0 maximum(data[:, k])])[4] for k in 1:na]
	i_ang = (na == 5) ? 18.0 : 0.0                # radar(): a pentagon shows its second axis along y
	angs = [i_ang + (k - 1) * 360 / na for k in 1:na]
	page("radar", "", "")
	gx, gy = Float64[], Float64[]
	for r in 0.2:0.2:1.0                          # radar()'s yg0.2 rings
		cx, cy = _xy_circle(r); append!(gx, cx); append!(gy, cy)
	end
	for a in angs  append!(gx, [0.0, cosd(a), NaN]); append!(gy, [0.0, sind(a), NaN])  end
	_xyadd!(p, gx, gy; name="radar grid", color=_XY_GRID_RGB)
	for (i, nm) in enumerate(nms)
		r = data[i, :] ./ lims
		_xy_addk!(plot, p, vcat(r .* cosd.(angs), r[1] * cosd(angs[1])), vcat(r .* sind.(angs), r[1] * sind(angs[1])), nm, 8)
	end
	for (k, a) in enumerate(angs)                 # radar()'s default axis names, and the ring values
		_xy_lbl(plot, 1.12cosd(a), 1.12sind(a), "Label$k"; ha=abs(cosd(a)) < 0.2 ? 0 : (cosd(a) > 0 ? -1 : 1), size=12)
		(k == 1 || !isnorm) || continue
		for f in 0.2:0.2:1.0
			_xy_lbl(plot, f * cosd(a), f * sind(a), _xy_fmt(f * lims[k]); ha=-1, va=-1, size=9, c=(0.35, 0.35, 0.35))
		end
	end
	_xy_look(plot, true, true, (-1.3, 1.3, -1.3, 1.3))
end

function _xy_framed_rose(plot, p, kind, sel, page)
	# GMT's rose (psrose): azimuths, clockwise from north, in 10° sectors, lengths normalized to the
	# largest sector (the GMT.jl example's sector=(width=10, rose=true), normalize=true). The sector
	# counts are GMT.jl's own histogram binning (hst_floats).
	sel < 0 && error("rose: select the series of azimuths in the Object Manager first")
	v = filter(isfinite, _xy_get_series(plot, sel)[2])
	length(v) < 2 && error("rose: the selected series needs two or more values")
	nm = _xy_series_name(plot, sel)
	hst, inc, _ = GMT.hst_floats(mod.(v, 360.0), "10"; min_max=(0.0, 360.0))
	r = hst[:, 2] ./ maximum(hst[:, 2])
	page("rose", "", "")
	gx, gy = Float64[], Float64[]
	for rr in 0.25:0.25:1.0
		cx, cy = _xy_circle(rr); append!(gx, cx); append!(gy, cy)
	end
	for a in 0:30:330  append!(gx, [0.0, sind(a), NaN]); append!(gy, [0.0, cosd(a), NaN])  end
	_xyadd!(p, gx, gy; name="rose grid", color=_XY_GRID_RGB)
	px, py = Float64[], Float64[]
	for k in axes(hst, 1)
		r[k] > 0 || continue
		a = range(hst[k, 1], hst[k, 1] + inc; length=6)
		append!(px, vcat(0.0, r[k] .* sind.(a), NaN)); append!(py, vcat(0.0, r[k] .* cosd.(a), NaN))
	end
	_xy_addk!(plot, p, px, py, isempty(nm) ? "rose" : nm, 8; color=_XY_BLUE)
	for a in 0:30:330
		_xy_lbl(plot, 1.1sind(a), 1.1cosd(a), "$(a)°"; ha=a in (0, 180) ? 0 : (a < 180 ? -1 : 1), size=11)
	end
	_xy_look(plot, true, true, (-1.3, 1.3, -1.3, 1.3))
end

function _xy_framed_parallel(plot, p, kind, sel, page)
	# GMT.jl parallelplot(): one ruler per variable, every observation a line across them, values
	# normalized per ruler to its own range (normalize="range", via its normalizeArray).
	M, nms = _xy_page_matrix(plot)
	nv = size(M, 2)
	nv < 2 && error("parallelplot needs two or more series on this page (one per variable)")
	bbox = reduce(vcat, [collect(extrema(M[:, k])) for k in 1:nv])
	A = GMT.normalizeArray("range", copy(M), bbox)
	page("parallelplot", "", "")
	lx, ly = Float64[], Float64[]
	for i in axes(A, 1)  append!(lx, vcat(1.0:nv, NaN)); append!(ly, vcat(A[i, :], NaN))  end
	_xyadd!(p, lx, ly; name="observations")
	ax, ay = Float64[], Float64[]
	for k in 1:nv  append!(ax, [k, k, NaN]); append!(ay, [0.0, 1.0, NaN])  end
	_xyadd!(p, ax, ay; name="rulers", color=(0.0, 0.0, 0.0))
	for k in 1:nv
		_xy_lbl(plot, k, 1.02, _xy_fmt(bbox[2k]); va=-1, size=10)
		_xy_lbl(plot, k, -0.02, _xy_fmt(bbox[2k-1]); va=1, size=10)
		_xy_lbl(plot, k, -0.09, nms[k]; va=1, size=12)
	end
	_xy_look(plot, false, true, (0.6, nv + 0.4, -0.18, 1.1))
end

function _xy_framed_corner(plot, p, kind, sel, page)
	# GMT.jl cornerplot(): an n x n lower-triangular matrix of panels, each variable's histogram on
	# the diagonal (x range = round_wesn of its extrema, as cornerplot() sets it) and every pair's
	# scatter below it. (cornerplot() switches to hexagon binning above 1000 points; here it is always
	# the scatter its `scatter=true` gives.)
	M, nms = _xy_page_matrix(plot)
	nv = size(M, 2)
	nv < 2 && error("cornerplot needs two or more series on this page (one per variable)")
	g = 0.12
	lohi = [_xy_lohi(M[:, k]) for k in 1:nv]
	nx(k, v) = (v .- lohi[k][1]) ./ (lohi[k][2] - lohi[k][1])
	X0(j) = (j - 1) * (1 + g);  Y0(i) = (nv - i) * (1 + g)
	page("cornerplot", "", "")
	fx, fy = Float64[], Float64[]
	for i in 1:nv, j in 1:i
		rx, ry = _xy_rect(X0(j), X0(j) + 1, Y0(i), Y0(i) + 1)
		append!(fx, vcat(rx[1:4], rx[1], NaN)); append!(fy, vcat(ry[1:4], ry[1], NaN))
	end
	_xyadd!(p, fx, fy; name="panels", color=(0.3, 0.3, 0.3))
	for k in 1:nv
		hx, hy = _xy_panel_hist(M[:, k], lohi[k]..., X0(k), Y0(k), 0.95, false)
		_xy_addk!(plot, p, hx, hy, "$(nms[k]) histogram", 8; color=_XY_BLUE)
	end
	for i in 2:nv, j in 1:i-1
		_xyadd!(p, X0(j) .+ nx(j, M[:, j]), Y0(i) .+ nx(i, M[:, i]); name="$(nms[i]) vs $(nms[j])",
		     marker=:circle, linestyle=:none, markersize=3, color=_XY_BLUE)
	end
	for j in 1:nv                                 # x scales + names under the bottom row
		_xy_lbl(plot, X0(j), Y0(nv) - 0.02, _xy_fmt(lohi[j][1]); ha=-1, va=1, size=9)
		_xy_lbl(plot, X0(j) + 1, Y0(nv) - 0.02, _xy_fmt(lohi[j][2]); ha=1, va=1, size=9)
		_xy_lbl(plot, X0(j) + 0.5, Y0(nv) - 0.12, nms[j]; va=1, size=12)
	end
	for i in 2:nv                                 # y scales + names left of the first column
		_xy_lbl(plot, X0(1) - 0.02, Y0(i), _xy_fmt(lohi[i][1]); ha=1, va=-1, size=9)
		_xy_lbl(plot, X0(1) - 0.02, Y0(i) + 1, _xy_fmt(lohi[i][2]); ha=1, va=1, size=9)
		_xy_lbl(plot, X0(1) - 0.2, Y0(i) + 0.5, nms[i]; ha=1, size=12)
	end
	W = nv * (1 + g) - g
	_xy_look(plot, true, true, (-0.55, W + 0.05, -0.3, W + 0.3))
end

function _xy_framed_marginal(plot, p, kind, sel, page)
	# GMT.jl marginalhist(): the x,y scatter with the histogram of x above it and of y on its right
	# (scatter below 2000 points, hexagons above; here always the scatter).
	sel < 0 && error("marginalhist: select a series in the Object Manager first")
	x, y = _xy_get_series(plot, sel)
	ok = isfinite.(x) .& isfinite.(y)
	x, y = x[ok], y[ok]
	length(x) < 2 && error("marginalhist: the selected series needs two or more points")
	nm = _xy_series_name(plot, sel)
	(xl, xh), (yl, yh) = _xy_lohi(x), _xy_lohi(y)
	g, f = 0.06, 0.3
	page("marginalhist", "", "")
	fx, fy = Float64[], Float64[]
	for (a, b, c, d) in ((0.0, 1.0, 0.0, 1.0), (0.0, 1.0, 1 + g, 1 + g + f), (1 + g, 1 + g + f, 0.0, 1.0))
		rx, ry = _xy_rect(a, b, c, d)
		append!(fx, vcat(rx[1:4], rx[1], NaN)); append!(fy, vcat(ry[1:4], ry[1], NaN))
	end
	_xyadd!(p, fx, fy; name="panels", color=(0.3, 0.3, 0.3))
	_xyadd!(p, (x .- xl) ./ (xh - xl), (y .- yl) ./ (yh - yl); name=isempty(nm) ? "x vs y" : nm,
	     marker=:circle, linestyle=:none, markersize=4, color=_XY_BLUE)
	hx, hy = _xy_panel_hist(x, xl, xh, 0.0, 1 + g, f * 0.95, false)
	_xy_addk!(plot, p, hx, hy, "x histogram", 8; color=_XY_BLUE)
	hx, hy = _xy_panel_hist(y, yl, yh, 1 + g, 0.0, f * 0.95, true)
	_xy_addk!(plot, p, hx, hy, "y histogram", 8; color=_XY_BLUE)
	_xy_lbl(plot, 0.0, -0.02, _xy_fmt(xl); ha=-1, va=1, size=10)
	_xy_lbl(plot, 1.0, -0.02, _xy_fmt(xh); ha=1, va=1, size=10)
	_xy_lbl(plot, -0.02, 0.0, _xy_fmt(yl); ha=1, va=-1, size=10)
	_xy_lbl(plot, -0.02, 1.0, _xy_fmt(yh); ha=1, va=1, size=10)
	_xy_lbl(plot, 0.5, -0.09, "x"; va=1, size=12)
	_xy_lbl(plot, -0.1, 0.5, "y"; ha=1, size=12)
	_xy_look(plot, true, true, (-0.3, 1 + g + f + 0.05, -0.2, 1 + g + f + 0.05))
end

function _xy_framed_biplot(plot, p, kind, sel, page)
	# GMT.jl biplot(): PCA of the z-scored variables (zscores, pca), PC1 vs PC2, with the scaling,
	# sign convention and symmetric limits its biplot(coefs, scores, ...) method applies.
	M, nms = _xy_page_matrix(plot)
	size(M, 2) < 2 && error("biplot needs two or more series on this page (one per variable)")
	scores, coefs, _, explained = GMT.pca(GMT.zscores(M))
	coefs, scores = Float64.(coefs[:, [1, 2]]), Float64.(scores[:, [1, 2]])
	explained = Float64.(explained[[1, 2]])
	signs = sign.(coefs[findmax(abs, coefs, dims=1)[2]])
	coefs = coefs .* signs                        # the largest coefficient of each column positive
	maxlen = sqrt(maximum(sum(coefs .^ 2, dims=2)))
	scores = maxlen .* scores ./ maximum(abs, scores) .* signs
	mima, mima_c = extrema(scores), extrema(coefs)
	mima = (min(mima[1], mima_c[1]), max(mima[2], mima_c[2]))
	r = GMT.round_wesn([mima[1], mima[2], mima_c[1], mima_c[2]])
	r[1] = -max(-r[1], r[2]);  r[2] = -r[1];  r[3] = -max(-r[3], r[4]);  r[4] = -r[3]
	page("biplot", "PC1 ($(round(explained[1], digits=1))%)", "PC2 ($(round(explained[2], digits=1))%)")
	_xyadd!(p, scores[:, 1], scores[:, 2]; name="observations", marker=:circle, linestyle=:none, markersize=4,
	     color=(0.35, 0.35, 0.35))
	axl, ayl = Float64[], Float64[]
	hl = 0.06 * maxlen
	for k in axes(coefs, 1)                       # feather!(…, endpt=true): an arrow from the origin
		cx, cy = coefs[k, 1], coefs[k, 2]
		L = hypot(cx, cy)
		L > 0 || continue
		ux, uy = cx / L, cy / L
		append!(axl, [0.0, cx, NaN, cx - hl * (ux - 0.5uy), cx, cx - hl * (ux + 0.5uy), NaN])
		append!(ayl, [0.0, cy, NaN, cy - hl * (uy + 0.5ux), cy, cy - hl * (uy - 0.5ux), NaN])
		_xy_lbl(plot, cx * 1.08, cy * 1.08, nms[k]; ha=cx >= 0 ? -1 : 1, size=12, c=_XY_BLUE)
	end
	_xyadd!(p, axl, ayl; name="loadings", color=_XY_BLUE, linewidth=2)
	# biplot()'s symmetric limits, widened 15%: its y limits come from the coefficients alone, which
	# leaves an arrow tip on the frame and its name outside it (GMT text is not clipped; ours is).
	_xy_look(plot, true, false, (1.15r[1], 1.15r[2], 1.15r[3], 1.15r[4]))
end

# The framed kinds, each with its own builder: reached through this `Function`-typed table ON PURPOSE,
# like _XY_ANA_OPS — the first radar compiles the radar only (3.0 s measured 2026-10-27, when every
# kind was a branch of one function).
const _XY_FRAMED_KINDS = Dict{String,Function}(
	"radar" => _xy_framed_radar, "rose" => _xy_framed_rose, "parallelplot" => _xy_framed_parallel,
	"cornerplot" => _xy_framed_corner, "marginalhist" => _xy_framed_marginal, "biplot" => _xy_framed_biplot,
	"polarplot" => (plot, p, kind, sel, page) -> _xy_polar(plot, p, kind, page),
	"polarscatter" => (plot, p, kind, sel, page) -> _xy_polar(plot, p, kind, page),
	"compassplot" => (plot, p, kind, sel, page) -> _xy_polar(plot, p, kind, page),
	"feather" => (plot, p, kind, sel, page) -> _xy_feather_page(plot, p, page),
	"piechart" => _xy_pie, "donutchart" => _xy_pie)

function _xy_framed(plot, p, kind, sel, page)
	_XY_FRAMED_KINDS[kind](plot, p, kind, sel, page)
	return
end

# ============================================================================
# GMT menu — GUI front-ends for GMT modules that work on table data.
# ============================================================================
# spectrum1d: a full interface to GMT's spectrum1d (auto- & cross-spectra). The C++ dialog
# (xyAskSpectrum1d, 65_xyplot.cpp) encodes its choices into the op string. This differs from the
# Analysis-menu FFT items (a fixed amplitude/PSD autospectrum) by exposing segment size, sample
# interval, detrend mode, wavelength axis, an optional 2nd series and the full -C output set.

# -C flag -> (GMT.jl output-tuple field, GMT.jl composite column-name stem, display label).
const _SPEC_FIELD = Dict('x'=>:xpower, 'y'=>:ypower, 'c'=>:cpower, 'n'=>:npower,
                         'p'=>:phase,  'a'=>:admitt, 'g'=>:gain,   'o'=>:coh)
const _SPEC_COL   = Dict('x'=>"Xpower", 'y'=>"Ypower", 'c'=>"Cpower", 'n'=>"Npower",
                         'p'=>"Phase",  'a'=>"Admit",  'g'=>"Gain",   'o'=>"Coher")
const _SPEC_LABEL = Dict('x'=>"X power", 'y'=>"Y power", 'c'=>"Cross power", 'n'=>"Noise power",
                         'p'=>"Phase",   'a'=>"Admittance", 'g'=>"Gain",     'o'=>"Coherency")

# Parse a ":"-joined "key=value" arg string (the op payload) into a Dict.
function _parse_kv(s::AbstractString)
	d = Dict{String,String}()
	for tok in split(s, ':')
		isempty(tok) && continue
		i = findfirst(==('='), tok)
		if i === nothing
			d[String(tok)] = ""
		else
			d[String(tok[1:prevind(tok, i)])] = String(tok[nextind(tok, i):end])
		end
	end
	return d
end

# Resolve a Data-Viewer column code (from the spectrum1d dialog) to (values, abscissa, label).
#   code == -1 -> the abscissa (X) column itself (X of series 0); no separate abscissa -> dt by index.
#   code >=  0 -> series `code`'s value (Y) column; abscissa = its own X (used for dt).
function _xy_col(plot::Ptr{Cvoid}, code::Int)
	if code == -1
		xy = _xy_get_series(plot, 0)
		isempty(xy[1]) && return nothing
		return (xy[1], nothing, "X")
	end
	xy = _xy_get_series(plot, code)
	isempty(xy[1]) && return nothing
	nm = _xy_series_name(plot, code); isempty(nm) && (nm = "series $code")
	return (xy[2], xy[1], nm)
end

# Analysis > Sound Velocity Profile (ecran AnalysisSVP_CB): the dialog names the Temperature, Salinity
# and Pressure (dbar) columns (Data-Viewer codes, as _xy_col reads them) and the latitude. The result
# lands on its own "SVP diagram" page: X = sound speed, Y = depth, as ecran plots it.
function _xy_svp(plot::Ptr{Cvoid}, p::QtXYPlot, args::AbstractString)
	d = _parse_kv(args)
	cols = map(k -> _xy_col(plot, parse(Int, d[k])), ("T", "S", "P"))
	for (c, k) in zip(cols, ("Temperature", "Salinity", "Pressure"))
		c === nothing && error("Sound Velocity Profile: could not read the $k column")
	end
	T, S, P = (c[1] for c in cols)
	length(T) == length(S) == length(P) ||
		error("Sound Velocity Profile: Temperature, Salinity and Pressure columns differ in length " *
		      "($(length(T)), $(length(S)), $(length(P)))")
	lat = parse(Float64, d["lat"])
	svp = _svp_chen_millero(T, S, P)
	z   = _z_from_p(Float64.(P), lat)
	_xy_addpage!(plot, p, "SVP diagram", svp, z, "SVP"; xlabel="Sound speed (m/s)", ylabel="Depths (m)")
	_xy_log(plot, "Analysis: Sound Velocity Profile → page 'SVP diagram' ($(length(svp)) pts)")
	return
end

# Run GMT.spectrum1d on the chosen column (input X(t)) and, for cross-spectra, a second column
# (output Y(t)); land each requested output component on its own self-scaled page. `args` is the
# encoded dialog payload "S=…:D=…:L=…:C=<flags>:y1=<code>:y2=<code>:W=<0|1>" where the column codes
# are -1 (abscissa), -2 (none, y2 only) or a series index. All spectra go through GMT — no in-house
# FFT here (the Analysis-menu items keep the lighter autospectrum path).
function _xy_spectrum1d(plot::Ptr{Cvoid}, p::QtXYPlot, args::AbstractString, sel::Int)
	d = _parse_kv(args)
	c1 = _xy_col(plot, parse(Int, get(d, "y1", string(sel))))   # 1st-column pulldown -> input X(t)
	c1 === nothing && return _xy_log(plot, "spectrum1d: could not read the 1st column"; err=true)
	y, xs, nm = c1
	N = length(y)
	N < 4 && return _xy_log(plot, "spectrum1d: need at least 4 points"; err=true)

	flags = get(d, "C", "x"); isempty(flags) && (flags = "x")
	y2c   = parse(Int, get(d, "y2", "-2"))
	cross = y2c != -2
	if !cross && any(c -> c in "ycnpago", flags)
		return _xy_log(plot, "spectrum1d: cross-spectra outputs (Y/Cross/Noise/Coh/Admit/Gain/Phase) need a 2nd column"; err=true)
	end

	# input: 1 column (autospectrum) or 2 columns X,Y (cross — 2nd column supplies Y(t)).
	local inmat
	if cross
		c2 = _xy_col(plot, y2c)
		c2 === nothing && return _xy_log(plot, "spectrum1d: could not read the 2nd column"; err=true)
		y2 = c2[1]
		n  = min(length(y), length(y2))
		n < 4 && return _xy_log(plot, "spectrum1d: 2nd column too short"; err=true)
		inmat = hcat(Float64.(@view y[1:n]), Float64.(@view y2[1:n]))
	else
		inmat = reshape(Float64.(y), :, 1)
	end
	Nrows = size(inmat, 1)

	# sample interval: explicit, else from the 1st column's abscissa (index spacing if it IS the
	# abscissa); segment: explicit (>0) else largest 2^n ≤ N.
	dt = let v = strip(get(d, "D", ""))
		isempty(v) ? (xs === nothing ? 1.0 : abs((xs[end] - xs[1]) / (N - 1))) : abs(parse(Float64, v))
	end
	dt == 0 && return _xy_log(plot, "spectrum1d: zero sample interval"; err=true)
	seg = let v = parse(Int, get(d, "S", "0")); v > 0 ? v : prevpow(2, Nrows) end
	seg < 2 && return _xy_log(plot, "spectrum1d: series too short for a segment"; err=true)

	mode = parse(Int, get(d, "L", "0"))            # 0 remove-trend(default) 1 leave 2 mean 3 mid
	wl   = get(d, "W", "0") == "1"
	# -C switches spectrum1d into cross-spectral (2-column) mode; an autospectrum must pass NO -C, so
	# `output` is set only for the cross case (matching the lighter Analysis-menu autospectrum path).
	kw = Dict{Symbol,Any}(:size => seg, :sample_dist => dt)
	if cross
		syms = [_SPEC_FIELD[c] for c in flags if haskey(_SPEC_FIELD, c)]
		kw[:output] = (; (sym => true for sym in syms)...)
	end
	mode == 1 && (kw[:leave_trend] = true)
	mode == 2 && (kw[:leave_trend] = "m")
	mode == 3 && (kw[:leave_trend] = "h")
	wl && (kw[:wavelength] = true)

	D = GMT.spectrum1d(inmat; kw...)
	M = D isa AbstractVector ? D[1] : D
	A = M isa GMTdataset ? M.data : Matrix{Float64}(M)
	cols = (M isa GMTdataset && !isempty(M.colnames)) ? M.colnames : String[]
	f  = Float64.(@view A[:, 1])
	xl = wl ? "wavelength" : "frequency"

	k = 0; nadded = 0
	for c in flags
		haskey(_SPEC_COL, c) || continue
		k += 1
		col = isempty(cols) ? nothing : findfirst(n -> occursin(_SPEC_COL[c], n), cols)
		col === nothing && (col = 2k)              # positional fallback: freq, then (value,σ) pairs
		if col > size(A, 2)
			_xy_log(plot, "spectrum1d: output '$(_SPEC_LABEL[c])' not in result"; err=true)
			continue
		end
		lab = _SPEC_LABEL[c]; pname = "$nm — $lab"
		_xy_addpage!(plot, p, pname, f, Float64.(@view A[:, col]), pname; xlabel=xl, ylabel=lab)
		nadded += 1
	end
	_xy_log(plot, "spectrum1d: $nadded page(s) from '$nm'" * (cross ? " ⨯ 2nd series" : "") *
	              " (seg=$seg, dt=$(round(dt; sigdigits=4)))")
	return
end

# ecran's wbm_dynSlope fit: ln(power) vs k over the band [xa, xb] of series `sel`, by GMT's trend1d —
# ROBUST (`-N2r` in ecran's GMT 4 spelling, `-Np1+r` now) above two points, plain with exactly two.
# The model column (-Fxm) carries the line, so the slope/intercept are read off it (GMT.jl's own -Fp
# coefficient output errors on a Matrix input). Answers through gmtvtk_xyplot_sg_set_fit.
function _xy_sgfit(plot::Ptr{Cvoid}, d::Dict{String,String}, sel::Int)
	xa = parse(Float64, d["xa"]); xb = parse(Float64, d["xb"])
	x, y = _xy_get_series(plot, sel)
	k = [i for i in eachindex(x) if xa <= x[i] <= xb && y[i] > 0]
	length(k) < 2 && return
	xy = hcat(x[k], log.(y[k]))
	(xy[end, 1] == xy[1, 1]) && return
	D = GMT.trend1d(xy; N = (length(k) > 2 ? "p1+r" : "p1"), F = "xm")
	M = (D isa AbstractVector ? D[1] : D).data
	m = (M[end, 2] - M[1, 2]) / (M[end, 1] - M[1, 1])
	ccall(_fn(:gmtvtk_xyplot_sg_set_fit), Cvoid, (Ptr{Cvoid}, Cdouble, Cdouble), plot, m, M[1, 2] - m * M[1, 1])
	return
end

"""
    _xy_reference(x, offs, xm) -> Vector{Float64}

ecran's `do_reference`: stretch the abscissa `x` of a curve piece by piece so that its picked rows
`offs` (1-based, in pick order) land on `xm`, the x of their partners on the reference curve. Each
segment between two picks is scaled to its partner segment; everything after a segment is shifted with
it, and the part before the first pick is shifted onto the first partner.
"""
function _xy_reference(x::AbstractVector{<:Real}, offs::Vector{Int}, xm::Vector{Float64})
	x = Float64.(x)
	n = length(x)
	(length(offs) == length(xm) && length(offs) >= 2) || error("Reference: the two curves need the same number of picks, at least two")
	all(1 .<= offs .<= n) || error("Reference: a pick lies outside the curve")
	lead = xm[1] - x[offs[1]]                            # how far the first pick moves (before it moves)
	for k in 1:length(xm)-1
		a, b = offs[k], offs[k+1]
		x[b] == x[a] && error("Reference: two picks on the same point of the curve")
		dx = xm[k+1] - x[b]                              # the segment's end, relative to its partner
		f  = (xm[k+1] - xm[k]) / (x[b] - x[a])          # piecewise factor
		lo, hi = minmax(a, b)
		x0 = x[a]
		x[lo:hi] .= (x[lo:hi] .- x0) .* f .+ xm[k]
		b < n && (x[b+1:end] .+= dx)                     # shift all the points after it by the same DX
	end
	# The chunk before the first pick moves with it (ecran's own comment says so, but its line moves one
	# point, `x(1, offset-1)`, by a distance taken after the loop has already put that pick in place —
	# i.e. by zero).
	offs[1] > 1 && (x[1:offs[1]-1] .+= lead)
	return x
end

# The Analysis op "reference:offs=…:xm=…:xl=…:yl=…" (xyRefDo, 65_xyplot.cpp): the referenced curve on
# a page of its own, named after it.
function _xy_reference_op(plot::Ptr{Cvoid}, p::QtXYPlot, d::Dict{String,String}, sel::Int)
	x, y = _xy_get_series(plot, sel)
	isempty(x) && error("Reference: could not read the curve")
	offs = [parse(Int, v) + 1 for v in split(d["offs"])]          # 0-based rows -> 1-based
	xm = parse.(Float64, split(d["xm"]))
	xr = _xy_reference(x, offs, xm)
	nm = _xy_series_name(plot, sel); isempty(nm) && (nm = "curve")
	_xy_addpage!(plot, p, "Referenced", xr, y, "$nm referenced";
	             xlabel = _mag_unesc(get(d, "xl", "")), ylabel = _mag_unesc(get(d, "yl", "")))
	_xy_log(plot, "Reference: '$nm' stretched onto $(length(offs)) picks -> page 'Referenced'")
	return
end

# ecran's do_bandFilter: the frequency band [flo, fhi] (cycles per metre) of a Spector-Grant fit line,
# passed on the grid of the 3-D viewer this X,Y window belongs to (the one its radial spectrum was
# computed from), through the FFT tool's own engine (fftstuff.jl). The result is a new derived grid
# in that viewer, through the ONE derived-variable transition.
function _xy_sg_bandpass(plot::Ptr{Cvoid}, d::Dict{String,String})
	flo = parse(Float64, d["flo"]); fhi = parse(Float64, d["fhi"])
	(0 <= flo < fhi) || error("the pass band is empty")
	scene = ccall(_fn(:gmtvtk_xyplot_owner), Ptr{Cvoid}, (Ptr{Cvoid},), plot)
	scene == C_NULL && error("this X,Y window belongs to no 3-D viewer, so there is no grid to filter " *
	                         "(the spectrum must come from a viewer's Grid Tools > FFT > Radial average)")
	G = _active_grid(scene)                     # the layer ON DISPLAY in that viewer (the one-question rule)
	G isa GMTgrid || error("the viewer this window belongs to shows no grid to filter")
	nx, ny = _grid_dims(G)
	o = FFTOpts(nx, ny, _isgeographic(G) ? FFT_GEOG : FFT_METERS, false)
	Gout, name = fft_bandpass(G, o, flo, fhi)
	name = "$name ($(round(1 / fhi; sigdigits=4)) - $(round(1 / max(flo, eps()); sigdigits=4)) m)"
	_add_grid_to_scene(scene, Gout, name; promote = false, record = false) || error("window closed, grid not added")
	_adopt_derived!(scene, name, Gout)
	_xy_log(plot, "Bandpass: '$name' added to the viewer")
	return
end

# The gallery's right-click "Synthetic example" (65_xyplot.cpp): a new page "<type> example" holding
# made-up data of the shape that plot type reads. The C side then applies the type to it exactly as a
# click on its icon does (xySetPlotType), so the example is drawn by the type's own code.
# One small function per type in _XY_EXAMPLES, one data series each:
# only the requested one is ever compiled and computed.
struct _XYEx
	plot::Ptr{Cvoid}
	p::QtXYPlot
	kind::String
end
_ex_page(e::_XYEx, xl, yl) = (ccall(_fn(:gmtvtk_xyplot_add_page), Cint, (Ptr{Cvoid}, Cstring), e.plot, "$(e.kind) example");
                             ccall(_fn(:gmtvtk_xyplot_set_labels), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), e.plot, String(xl), String(yl)))
_ex_add(e::_XYEx, x, y, nm) = _xyadd!(e.p, collect(Float64, x), collect(Float64, y); name=nm)
function _ex_vals(e::_XYEx, what, a::Vector{Float64})          # per-point values of the last series added
	k = ccall(_fn(:gmtvtk_xyplot_series_count), Cint, (Ptr{Cvoid},), e.plot) - 1
	ccall(_fn(:gmtvtk_xyplot_set_point_values), Cint, (Ptr{Cvoid}, Cint, Cint, Ptr{Float64}, Ptr{Float64}, Cint),
	      e.plot, k, what, a, C_NULL, length(a))
end

function _ex_curves(e)
	x = range(0, 4π; length=e.kind == "scatter" ? 50 : 120)
	_ex_page(e, "x", "y")
	_ex_add(e, x, sin.(x), "sin(x)")
end
function _ex_steps(e)
	x = range(0, 2π; length=25)
	_ex_page(e, "x", "y")
	_ex_add(e, x, sin.(x), "sin(x)")
end
function _ex_bars(e)
	_ex_page(e, "group", "value")
	_ex_add(e, 1:6, [4, 7, 3, 8, 5, 6], "2024")
end
_ex_waterfall(e) = (_ex_page(e, "step", "change"); _ex_add(e, 1:8, [10, -3, 5, -2, 4, -6, 3, 2], "change"))
function _ex_semilogx(e)
	f = 10 .^ range(-1, 3; length=200)
	_ex_page(e, "frequency (Hz)", "phase (deg)")
	_ex_add(e, f, -atand.(f ./ 10), "phase")
end
function _ex_semilogy(e)
	t = range(0, 10; length=100)
	_ex_page(e, "time", "population")
	_ex_add(e, t, 100 .* exp.(0.8 .* t), "growth")
end
function _ex_loglog(e)
	f = 10 .^ range(-1, 3; length=200)
	_ex_page(e, "frequency (Hz)", "gain")
	_ex_add(e, f, 1 ./ sqrt.(1 .+ (f ./ 10) .^ 4), "low-pass")
end
function _ex_plotyy(e)
	t = range(0, 20; length=200)
	_ex_page(e, "time", "temperature / pressure")
	_ex_add(e, t, 20 .+ 5 .* sin.(t ./ 3), "temperature")
	_ex_add(e, t, 1013 .+ 8 .* cos.(t ./ 4), "pressure")
end
function _ex_fillbetween(e)
	x = range(0, 4π; length=150)
	_ex_page(e, "x", "y")
	_ex_add(e, x, sin.(x), "sin(x)")
	_ex_add(e, x, 0.5 .* cos.(x), "0.5 cos(x)")
end
function _ex_errorbar(e)
	y = sqrt.(1.0:10.0)
	_ex_page(e, "x", "y")
	_ex_add(e, 1:10, y, "sqrt(x)")
	_ex_vals(e, 0, 0.1 .* y)
end
function _ex_bubble(e)
	_ex_page(e, "x", "y")
	_ex_add(e, rand(20), rand(20), "bubbles")
	_ex_vals(e, 2, rand(20))
end
function _ex_samples(e)
	n = 300
	_ex_page(e, "sample", "value")
	_ex_add(e, 1:n, randn(n), "normal")
end
function _ex_qqplot(e)
	n = 300
	_ex_page(e, "sample", "value")
	_ex_add(e, 1:n, randn(n), "normal")
	_ex_add(e, 1:n, -log.(rand(n)), "exponential")
end
function _ex_radar(e)
	_ex_page(e, "spoke", "score")
	_ex_add(e, 1:6, [8, 6, 7, 4, 9, 5], "scores")
end
function _ex_rose(e)
	n = 400
	_ex_page(e, "sample", "azimuth (deg)")
	_ex_add(e, 1:n, mod.(vcat(40 .+ 20 .* randn(n ÷ 2), 220 .+ 30 .* randn(n - n ÷ 2)), 360), "azimuths")
end
function _ex_feather(e)
	t = range(0, 2π; length=30)
	_ex_page(e, "time", "U, V")
	_ex_add(e, t, cos.(t), "U")
	_ex_add(e, t, sin.(t), "V")
end
function _ex_polar(e)
	θ = range(0, 2π; length=e.kind == "polarscatter" ? 60 : 300)
	_ex_page(e, "angle (rad)", "radius")
	_ex_add(e, θ, 1 .+ 0.5 .* sin.(3 .* θ), "r = 1 + 0.5 sin(3θ)")
end
_ex_compass(e) = (_ex_page(e, "angle (rad)", "radius");
                  _ex_add(e, [0.3, 1.2, 2.5, 4.0, 5.2], [3.0, 2.0, 4.0, 1.5, 2.5], "vectors"))
_ex_pie(e) = (_ex_page(e, "slice", "value"); _ex_add(e, 1:5, [35, 25, 20, 12, 8], "share"))
function _ex_multivar(e)
	n = 150
	a = randn(n); b = 0.7 .* a .+ 0.5 .* randn(n); c = -0.4 .* a .+ 0.8 .* randn(n)
	_ex_page(e, "observation", "value")
	_ex_add(e, 1:n, a, "length")
	_ex_add(e, 1:n, b, "width")
	_ex_add(e, 1:n, c, "depth")
end
function _ex_marginal(e)
	x = randn(400)
	_ex_page(e, "x", "y")
	_ex_add(e, x, 0.6 .* x .+ 0.6 .* randn(400), "y vs x")
end

const _XY_EXAMPLES = Dict{String,Function}(
	"plot" => _ex_curves, "area" => _ex_curves, "comet" => _ex_curves, "scatter" => _ex_curves,
	"stem" => _ex_steps, "stairs" => _ex_steps,
	"bar" => _ex_bars, "barh" => _ex_bars, "bar stack" => _ex_bars, "barh stack" => _ex_bars,
	"waterfall" => _ex_waterfall, "semilogx" => _ex_semilogx, "semilogy" => _ex_semilogy, "loglog" => _ex_loglog,
	"plotyy" => _ex_plotyy, "fill_between" => _ex_fillbetween, "errorbar" => _ex_errorbar, "bubblechart" => _ex_bubble,
	"histogram" => _ex_samples, "density" => _ex_samples, "boxplot" => _ex_samples, "violin" => _ex_samples,
	"ecdfplot" => _ex_samples, "qqnorm" => _ex_samples, "qqplot" => _ex_qqplot,
	"radar" => _ex_radar, "rose" => _ex_rose, "feather" => _ex_feather,
	"polarplot" => _ex_polar, "polarscatter" => _ex_polar, "compassplot" => _ex_compass,
	"piechart" => _ex_pie, "donutchart" => _ex_pie,
	"parallelplot" => _ex_multivar, "cornerplot" => _ex_multivar, "biplot" => _ex_multivar,
	"marginalhist" => _ex_marginal)

function _xy_example(plot::Ptr{Cvoid}, p::QtXYPlot, kind::AbstractString)
	f = get(_XY_EXAMPLES, kind, nothing)
	f === nothing && error("no synthetic example for '$kind'")
	f(_XYEx(plot, p, String(kind)))
	_xy_log(plot, "Synthetic example: '$kind' → page '$kind example'")
	return
end

_xy_ana_failed(plot, e) = _xy_request_failed(plot, e isa ErrorException ? e.msg : sprint(showerror, e))

# Every Analysis request, by op prefix, tried in this order ("" = the series ops, last). The handlers are
# reached through this `Function`-typed table ON PURPOSE: a call through it is not inferred ahead, so the
# first request compiles ONLY its own handler. Called directly from _on_xy_analysis, the first click of
# anything (a 0.09 s synthetic example) compiled every handler and all they call -- the magnetic
# synthetic, the grid FFT, trend1d, ... -- 467 methods, 8.6 s (measured 2026-10-27).
const _XY_ANA_OPS = Pair{String,Function}[
	# GMT-module front-ends (GMT menu) carry their own multi-output / multi-series logic.
	"spectrum1d:" => (plot, p, op, s) -> _xy_spectrum1d(plot, p, op[12:end], s),
	"svp:" => (plot, p, op, s) -> try
			_xy_svp(plot, p, op[5:end])
		catch e
			_dbg("xy-svp", op, "ERR", sprint(showerror, e))
			_xy_ana_failed(plot, e)
		end,
	# The "Geomagnetic bar code" strip (bar, synthetic RTP, age fit) — xymag.jl.
	"mag" => (plot, p, op, s) -> _xy_mag(plot, op, s),
	# The Spector-Grant fit (live, on every drag move): GMT trend1d on the band.
	"sgfit:" => (plot, p, op, s) -> try
			_xy_sgfit(plot, _parse_kv(op[7:end]), s)
		catch e
			_dbg("xy-sg", op, "ERR", sprint(showerror, e))     # no fit: the C side keeps the last one
		end,
	# "Reference me" (the reference picks): the stretched curve on its own page.
	"reference:" => (plot, p, op, s) -> try
			_xy_reference_op(plot, p, _parse_kv(op[11:end]), s)
		catch e
			_dbg("xy-ref", op, "ERR", sprint(showerror, e))
			_xy_ana_failed(plot, e)
		end,
	# The plot-type gallery's statistical plots (density, boxplot, violin, ecdfplot, qqnorm, qqplot).
	"stat:" => (plot, p, op, s) -> try
			_xy_stat(plot, p, op[6:end], s)
		catch e
			_dbg("xy-stat", op, "ERR", sprint(showerror, e))
			_xy_ana_failed(plot, e)
		end,
	# The gallery's right-click "Synthetic example": a page of made-up data for that plot type.
	"example:" => (plot, p, op, s) -> try
			_xy_example(plot, p, op[9:end])
		catch e
			_dbg("xy-example", op, "ERR", sprint(showerror, e))
			_xy_ana_failed(plot, e)
		end,
	# A Spector-Grant fit line's "Bandpass Filter": the band on the grid the spectrum came from.
	"sgbandpass:" => (plot, p, op, s) -> try
			_xy_sg_bandpass(plot, _parse_kv(op[12:end]))
		catch e
			_dbg("xy-sg", op, "ERR", sprint(showerror, e))
			_xy_request_failed(plot, "Bandpass: " * (e isa ErrorException ? e.msg : sprint(showerror, e)))
		end,
	"hist:" => (plot, p, op, s) -> _xy_series_hist(plot, p, op, s),
	"bargraph" => (plot, p, op, s) -> _xy_series_bars(plot, p, op, s),
	"" => (plot, p, op, s) -> _xy_series_op(plot, p, op, s),
]

function _on_xy_analysis(plot::Ptr{Cvoid}, cop::Cstring, sel::Cint)::Cvoid
	op = unsafe_string(cop)
	try
		p = get(_FIGREG, plot, nothing)
		if !(p isa QtXYPlot)
			_xy_log(plot, "Analysis '$op': no Julia mirror for this window (open it via xyplot/iview, not File>Open)"; err=true)
			return
		end
		for (pre, f) in _XY_ANA_OPS
			if startswith(op, pre)
				f(plot, p, op, Int(sel))
				return
			end
		end
	catch e
		_dbg("xy-ana", op, "ERR", sprint(showerror, e))
		_xy_log(plot, "Analysis '$op' FAILED: $(sprint(showerror, e))"; err=true)
		@tool_error "xyplot analysis '$op' failed" exception=e
	end
	return
end

# The selected series of the current page and its name; nothing (logged) when it cannot be read.
function _xy_series_get(plot::Ptr{Cvoid}, op::String, s::Int)
	# Pull the selected series straight from the C side (the live CURRENT page); the Julia mirror
	# no longer tracks per-page series now that windows have pages/tabs.
	xy = _xy_get_series(plot, s)
	if isempty(xy[1])
		_xy_log(plot, "Analysis '$op': could not read series #$s from the current page"; err=true)
		return nothing
	end
	nm = _xy_series_name(plot, s); isempty(nm) && (nm = "series $s")
	return xy[1], xy[2], nm
end

# Bar-drawn ops (ecran's "Show histogram" / "Show Bar graph"): their result is not a curve on
# the parent's axes, so — like every other different-domain op here — it lands on its OWN page.
function _xy_series_hist(plot::Ptr{Cvoid}, p::QtXYPlot, op::String, s::Int)
	xyn = _xy_series_get(plot, op, s)
	xyn === nothing && return
	x, y, nm = xyn
	c, k, w = _histogram(y, parse(Int, op[6:end]))
	_xy_addpage!(plot, p, "$nm histogram", c, k, "$nm histogram";
	             xlabel=nm, ylabel="count", kind=:bar, barwidth=0.9w)
	_xy_log(plot, "Analysis: histogram of '$nm' → new page ($(length(c)) bins, width $(round(w; sigdigits=4)))")
	return
end
function _xy_series_bars(plot::Ptr{Cvoid}, p::QtXYPlot, op::String, s::Int)
	xyn = _xy_series_get(plot, op, s)
	xyn === nothing && return
	x, y, nm = xyn
	# Same x as the parent — so if that was a time axis, the bars' page must read x as dates too
	# (same reasoning as the derivative pages below).
	pfmt = ccall(_fn(:gmtvtk_xyplot_get_xtime), Cint, (Ptr{Cvoid},), plot)
	_xy_addpage!(plot, p, "$nm bars", x, y, "$nm bars";
	             xlabel=(pfmt != 0 ? "time" : "x"), ylabel=nm, kind=:bar)
	pfmt != 0 && ccall(_fn(:gmtvtk_xyplot_set_xtime), Cvoid, (Ptr{Cvoid}, Cint), plot, pfmt)
	_xy_log(plot, "Analysis: bar graph of '$nm' → new page ($(length(x)) bars)")
	return
end

# Every _xy_compute op on the selected series of the current page.
function _xy_series_op(plot::Ptr{Cvoid}, p::QtXYPlot, op::String, s::Int)
	xyn = _xy_series_get(plot, op, s)
	xyn === nothing && return
	x, y, nm = xyn
	out = _xy_compute(op, x, y)
	if out === nothing
		_xy_log(plot, "Analysis: unknown op '$op'"; err=true)
		return
	end
	nx, ny, suffix = out
	if op in _XY_NEWWIN
		# Units differ from the parent axes (d/dx, frequency, lag) -> own PAGE (Excel-like tab) in
		# THIS window, not an overlay and not a new window. add_page switches the current page so
		# the following add! lands on the fresh tab.
		isderiv = startswith(op, "deriv")
		# A derivative keeps the SAME x as the parent — if that was a time axis, the derivative's x
		# is still time, so the new page must inherit the parent's time mode and read its x as dates.
		parent_fmt = ccall(_fn(:gmtvtk_xyplot_get_xtime), Cint, (Ptr{Cvoid},), plot)
		xl = isderiv ? (parent_fmt != 0 ? "time" : "x") : (op == "autocorr" ? "lag" : "frequency")
		yl = isderiv ? suffix : (op == "fft_psd" ? "power" : (op == "fft_amp" ? "amplitude" : "autocorrelation"))
		ccall(_fn(:gmtvtk_xyplot_add_page), Cint, (Ptr{Cvoid}, Cstring), plot, "$nm $suffix")
		if isderiv && parent_fmt != 0
			ccall(_fn(:gmtvtk_xyplot_set_xtime), Cvoid, (Ptr{Cvoid}, Cint), plot, parent_fmt)
		end
		ccall(_fn(:gmtvtk_xyplot_set_labels), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), plot, String(xl), String(yl))
		_xyadd!(p, nx, ny; name="$nm $suffix")
		_xy_log(plot, "Analysis: $suffix → new page ($(length(nx)) pts)")
	else
		_xyadd!(p, nx, ny; name="$nm $suffix")
		_xy_log(plot, "Analysis: added '$nm $suffix' ($(length(nx)) pts)")
	end
	return
end

# Build the C-callable pointer and install it. Called once from __init__.
function _register_xy_analysis()
	fptr = @cfunction((s,a,i)->Base.invokelatest(_on_xy_analysis,s,a,i), Cvoid, (Ptr{Cvoid}, Cstring, Cint))
	ccall(_fn(:gmtvtk_xyplot_set_analysis_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
