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
const _GMT_FFT_COMPLEX = Cuint(0)

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
	isdefined(GMT, :fft1d) && return GMT.fft1d(y)          # GMT.jl's own wrapper, once available
	b = _to_interleaved(ComplexF64.(y))
	_gmt_fft_1d!(b, length(y), _GMT_FFT_FWD) == 0 || error("GMT_FFT_1D forward failed")
	return _from_interleaved(b)
end

# Inverse FFT of a complex spectrum -> complex series (already 1/N-normalised by GMT).
function _gmt_ifft(F::AbstractVector{<:Complex})
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
	A = M isa GMTdataset ? M.data : Matrix{Float64}(M)
	f = Float64.(@view A[:, 1]); p = Float64.(@view A[:, 2])
	return want === :amp ? (f, sqrt.(max.(p, 0.0))) : (f, p)
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

# Zero-phase Butterworth filter applied in the frequency domain, transforming through GMT's C FFT.
# `fc` = cutoff (same 1/x units as the FFT frequency), `ftype` = "low" or "high", fixed order 4.
# Returns the filtered y (length N). dt is the (uniform) sample spacing from the x endpoints.
function _butter(x, y, fc::Float64, ftype)
	yv = collect(Float64, y); N = length(yv)
	N < 2 && return yv
	dt = (x[end] - x[1]) / (N - 1)
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
	return real.(_gmt_ifft(F))                        # inverse transform via GMT (already 1/N-normalised)
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
	try
		ccall(_fn(:gmtvtk_xyplot_log), Cvoid, (Ptr{Cvoid}, Cstring, Cint), plot, String(msg), Cint(err))
	catch
	end
	return
end

function _on_xy_analysis(plot::Ptr{Cvoid}, cop::Cstring, sel::Cint)::Cvoid
	op = unsafe_string(cop)
	try
		p = get(_FIGREG, plot, nothing)
		if !(p isa QtXYPlot)
			_xy_log(plot, "Analysis '$op': no Julia mirror for this window (open it via xyplot/iview, not File>Open)"; err=true)
			return
		end
		s = Int(sel)
		# Pull the selected series straight from the C side (the live CURRENT page); the Julia mirror
		# no longer tracks per-page series now that windows have pages/tabs.
		xy = _xy_get_series(plot, s)
		if xy === nothing
			_xy_log(plot, "Analysis '$op': could not read series #$s from the current page"; err=true)
			return
		end
		x, y = xy
		nm = _xy_series_name(plot, s); isempty(nm) && (nm = "series $s")
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
			add!(p, nx, ny; name="$nm $suffix")
			_xy_log(plot, "Analysis: $suffix → new page ($(length(nx)) pts)")
		else
			add!(p, nx, ny; name="$nm $suffix")
			_xy_log(plot, "Analysis: added '$nm $suffix' ($(length(nx)) pts)")
		end
	catch e
		_dbg("xy-ana", op, "ERR", sprint(showerror, e))
		_xy_log(plot, "Analysis '$op' FAILED: $(sprint(showerror, e))"; err=true)
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
