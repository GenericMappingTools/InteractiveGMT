# CI-safe unit tests for the X,Y plot tool's pure-Julia analysis math (xyanalysis.jl). These never
# touch the Qt+VTK DLL — they run anywhere `using InteractiveGMT` succeeds. The GUI side (the
# actual window) is exercised by the :gui tests in test-scene-gui.jl. Noise is DETERMINISTIC (a
# high-frequency deterministic tone, not randn) so the assertions are reproducible with no RNG dep.

@testitem "xyplot exports present" tags=[:unit, :fast] begin
	for s in (:xyplot, :add!, :clear!, :profile_to_xyplot, :xtime!, :stickplot, :QtXYPlot)
		@test isdefined(InteractiveGMT, s)
	end
end

@testitem "line style / marker keyword codes" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	sc = IG._style_code
	@test sc(IG._LINESTYLE, nothing, "linestyle") == -1     # nothing -> keep default
	@test sc(IG._LINESTYLE, :solid, "linestyle") == 1
	@test sc(IG._LINESTYLE, :dash,  "linestyle") == 2
	@test sc(IG._LINESTYLE, :none,  "linestyle") == 0
	@test sc(IG._MARKER, nothing, "marker") == -1
	@test sc(IG._MARKER, :circle, "marker") == 4
	@test sc(IG._MARKER, :square, "marker") == 3
	@test_throws ErrorException sc(IG._LINESTYLE, :wiggly, "linestyle")
	@test_throws ErrorException sc(IG._MARKER, :triangle, "marker")
end

@testitem "stick diagram geometry" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# azimuth -> (u,v): 0° points +y (North), 90° points +x (East)
	u, v = IG._stick_uv_from_az([0.0, 90.0], [1.0, 2.0])
	@test isapprox(u[1], 0.0; atol=1e-12)
	@test v[1] ≈ 1.0
	@test u[2] ≈ 2.0
	@test isapprox(v[2], 0.0; atol=1e-12)
	# segments: base (t,0) -> tip (t+u*sc, v*sc), NaN break every 3rd entry
	sx, sy = IG._stick_segments([0.0, 10.0], [1.0, 0.0], [0.0, 1.0], 2.0)
	@test length(sx) == 6
	@test sx[1] == 0.0 && sy[1] == 0.0           # base 1
	@test sx[2] == 2.0 && sy[2] == 0.0           # tip 1 = (0+1*2, 0*2)
	@test isnan(sx[3]) && isnan(sy[3])           # break
	@test sx[4] == 10.0 && sy[5] == 2.0          # base 2 / tip 2 v=1*2
	@test isnan(sx[6])
	# auto scale is positive + finite
	@test IG._stick_scale([0.0, 24.0], [1.0, -1.0], [0.0, 1.0]) > 0
end

@testitem "gauss solve" tags=[:unit, :fast] begin
	gs = InteractiveGMT._gauss_solve
	A = [2.0 1.0; 1.0 3.0]; b = [5.0, 10.0]      # solution (1, 3)
	x = gs(A, b)
	@test x[1] ≈ 1.0 atol=1e-10
	@test x[2] ≈ 3.0 atol=1e-10
end

@testitem "polynomial fit reproduces a polynomial" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	x = collect(0.0:0.05:10.0)
	y = 2.0 .- 0.5 .* x .+ 0.3 .* x.^2 .- 0.02 .* x.^3
	yhat = IG._polyfit_eval(x, y, 3)
	@test maximum(abs.(yhat .- y)) < 1e-6
	# a degree-1 fit of a clean line is exact
	yl = 3.0 .+ 1.5 .* x
	@test maximum(abs.(IG._polyfit_eval(x, yl, 1) .- yl)) < 1e-8
end

@testitem "detrend / derivative / autocorr" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	x = collect(0.0:0.01:10.0)
	# detrend removes a linear ramp -> ~0
	@test maximum(abs.(IG._detrend(x, 2.0 .+ 0.7 .* x))) < 1e-8
	# 1st derivative of sin ≈ cos
	d1 = IG._deriv(x, sin.(x), 1)
	@test maximum(abs.(d1 .- cos.(x))) < 0.01
	# autocorrelation at lag 0 is 1
	_, r = IG._autocorr(x, sin.(x))
	@test r[1] ≈ 1.0 atol=1e-10
end

@testitem "Savitzky-Golay reduces noise" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	x = collect(0.0:0.02:20.0)
	clean = sin.(x)
	noisy = clean .+ 0.3 .* sin.(37.0 .* x)       # deterministic high-freq "noise"
	sm = IG._savgol(noisy, 7, 3)
	ein  = sum(abs2, noisy .- clean)
	eout = sum(abs2, sm .- clean)
	@test eout < 0.3 * ein                          # smoothing pulls it back toward the clean signal
end

@testitem "GMT FFT round-trip + spectrum1d peak" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# forward+inverse through GMT_FFT_1D recovers the input (single precision -> ~1e-6)
	v = ComplexF64[ complex(sin(0.3k), 0.0) for k in 0:60 ]   # not a power of two on purpose
	back = IG._gmt_ifft(IG._gmt_fft(real.(v)))
	@test maximum(abs.(back .- v)) < 1e-5
	# spectrum via GMT.spectrum1d: a tone lands at its frequency bin
	t = collect(0.0:0.01:5.11)                      # dt=0.01, N=512 (power of two)
	f0 = 5.0
	fr, amp = IG._spectrum1d(t, sin.(2π * f0 .* t); want=:amp)
	ipk = argmax(amp)
	@test abs(fr[ipk] - f0) < 0.5
end

@testitem "Butterworth low-pass attenuates a high tone" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	t = collect(0.0:0.01:10.23)                     # N=1024
	slow = sin.(2π * 0.5 .* t)
	raw  = slow .+ 0.5 .* sin.(2π * 20.0 .* t)
	filt = IG._butter(t, raw, 2.0, "low")
	# interior (dodge FFT edge ringing): filtered follows the slow signal, raw does not
	mid = (length(t) ÷ 4):(3 * length(t) ÷ 4)
	@test sum(abs2, filt[mid] .- slow[mid]) < 0.2 * sum(abs2, raw[mid] .- slow[mid])
end

@testitem "Spector-Grant depth to sources" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	h = 500.0
	# magnetic power spectrum P(k) = C·exp(-4π·h·k); slope of ln P = -4π h -> depth = h
	k = collect(0.0002:0.0002:0.02)                  # wavenumber in 1/m
	P = 1000.0 .* exp.(-4π * h .* k)
	_, _, depth = IG._spector_grant(k, P, 0.0, 0.0, 1.0)
	@test isapprox(depth, 500.0; rtol=1e-6)
	# same physical depth with k in 1/km uses xfac=1000
	k2 = k .* 1000
	P2 = 1000.0 .* exp.(-4π * (h / 1000) .* k2)
	_, _, d2 = IG._spector_grant(k2, P2, 0.0, 0.0, 1000.0)
	@test isapprox(d2, 500.0; rtol=1e-6)
	# band restriction works (returns a sub-range)
	fx, _, _ = IG._spector_grant(k, P, 0.004, 0.012, 1.0)
	@test all(0.004 .<= fx .<= 0.012)
	@test_throws ErrorException IG._spector_grant(k, fill(-1.0, length(k)), 0.0, 0.0, 1.0)  # no positive power
end

@testitem "despike removes spikes, keeps inliers" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	x = collect(0.0:0.05:20.0)
	clean = sin.(x)
	y = copy(clean)
	spikes = [50, 120, 250, 333]
	for i in spikes
		y[i] += iseven(i) ? 6.0 : -5.0
	end
	cl = IG._despike(x, y; nsigma=2.0)
	@test maximum(abs.(cl[spikes] .- clean[spikes])) < 0.2     # spikes pulled back to the signal
	good = setdiff(1:length(x), spikes)
	@test maximum(abs.(cl[good] .- y[good])) < 0.1             # inliers essentially untouched
	# helpers
	@test IG._median([3.0, 1.0, 2.0]) == 2.0
	@test IG._median([1.0, 2.0, 3.0, 4.0]) == 2.5
end

# macOS/arm64 dies with `signal (6): Abort trap` inside the op-dispatch item below (CI run
# 33657361347) -- no message, a one-frame backtrace, so the log says neither WHICH op nor WHY. GMT's
# C source has no `abort()` call at all and its asserts are compiled out of a Release build, which
# left two candidates, and this item was written to separate them BEFORE the loop runs:
#   * the mode argument. GMT_FFT_1D/2D open with `assert (mode & GMT_FFT_COMPLEX)` and
#     gmt_resources.h defines GMT_FFT_COMPLEX = 1U (GMT_FFT_REAL = 0U) -- but GMT.jl's `fft1d`, and
#     this package's own fallback constant, both pass 0, i.e. the value the assert rejects. Harmless
#     under NDEBUG; SIGABRT the moment the library ships with asserts live.
#   * the transform SIZE: N=201 = 3*67 sends the backend down its generic-butterfly path, where
#     N=1024 is plain radix-2.
#
# BOTH ARE RULED OUT, by this item's own output in run 33679278640 (macOS arm64, GMT_FFT=fftw set
# by CI): every GMT_FFT_1D returned status 0 for BOTH modes at BOTH sizes, fft1d round-tripped, and
# `_spectrum1d` -- the exact call `fft_amp` makes, same series, same segment -- printed "all probes
# survived". FORTY-SEVEN MILLISECONDS later the op-dispatch item aborted on `fft_amp` -- that same
# call, same arguments. So the crash is not a function of the mode, the size, or the op: the same
# call succeeds and then dies later in the SAME process, which makes it cumulative state, not a bad
# argument -- and the GUI tier had been opening real windows in that process for 43 minutes before
# it. Switching the backend does not move it either (vDSP segfaulted, KissFFT and FFTW3f abort), so
# the next thing to establish is whether GMT is even the caller of that abort().
# Every line is flushed, so the last one printed names the call that died. macOS only: no other
# platform has ever crashed here and none needs the noise.
@testitem "macOS FFT probe (mode + size)" tags=[:unit, :fast] begin
	# Bound UNCONDITIONALLY, outside the platform guard. On Julia 1.13 this item errored on LINUX
	# with `UndefVarError: G not defined` (run 33679278640) even though Sys.isapple() is false
	# there: the whole `if` block is one top-level thunk, and a binding that exists in no branch is
	# not safe to reference from inside it. A name that is always defined cannot raise that error
	# on any version, whatever made the reference reachable.
	IG = InteractiveGMT; G = IG.GMT
	if Sys.isapple()
		for n in (1024, 201), mode in (UInt32(1), UInt32(0))   # 1 = GMT_FFT_COMPLEX, 0 = GMT_FFT_REAL
			println("FFTPROBE: GMT_FFT_1D n=$n mode=$mode"); flush(stdout)
			b = Float32[Float32(sin(i)) for i = 1:2n]          # interleaved (re, im), n complex points
			st = GC.@preserve b ccall((:GMT_FFT_1D, G.libgmt), Cint,
			                          (Ptr{Cvoid}, Ptr{Cfloat}, UInt64, Cint, UInt32),
			                          G.G_API[], pointer(b), UInt64(n), Cint(0), mode)
			println("FFTPROBE:   -> status $st"); flush(stdout)
			@test st == 0
		end
		x = collect(0.0:0.05:10.0)
		println("FFTPROBE: GMT.jl fft1d, N=$(length(x))"); flush(stdout)
		F = G.fft1d(sin.(x))
		println("FFTPROBE:   -> forward ok"); flush(stdout)
		G.fft1d(F; inverse=true)
		println("FFTPROBE:   -> inverse ok"); flush(stdout)
		println("FFTPROBE: spectrum1d (seg 128 over N=$(length(x)))"); flush(stdout)
		IG._spectrum1d(x, sin.(x); want=:psd)
		println("FFTPROBE: all probes survived"); flush(stdout)
	end
	@test true
end

@testitem "_xy_compute op-string dispatch" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	x = collect(0.0:0.05:10.0); y = sin.(x)
	for op in ("remove_mean", "remove_trend", "deriv1", "deriv2",
	           "fitpoly:2", "savgol:5", "butter:low:1.0", "despike:2.0", "autocorr", "fft_amp", "fft_psd")
		Sys.isapple() && (println("XYOP: $op"); flush(stdout))   # names the op that aborts (see probe above)
		out = IG._xy_compute(op, x, y)
		@test out !== nothing
		@test length(out) == 3                       # (xout, yout, suffix)
		@test length(out[1]) == length(out[2])
	end
	@test IG._xy_compute("nonsense", x, y) === nothing
end

@testitem "histogram binning" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# Explicit bin count: 100 values evenly spread over [0,1) -> 10 bins of 10 counts each.
	v = collect(0.0:0.01:0.99)
	c, k, w = IG._histogram(v, 10)
	@test length(c) == 10 && length(k) == 10
	@test sum(k) == length(v)                       # every value binned exactly once
	@test all(k .== 10.0)
	@test isapprox(w, 0.099; atol=1e-12)            # (max-min)/nbins, max = 0.99
	@test isapprox(c[1], v[1] + w / 2; atol=1e-12)  # centres, not edges
	# The maximum lands in the LAST bin, not one past it.
	c2, k2, _ = IG._histogram([0.0, 1.0], 2)
	@test sum(k2) == 2 && k2[end] == 1.0
	# Automatic bin count (Freedman-Diaconis) and NaN rejection.
	c3, k3, w3 = IG._histogram([v; NaN; NaN], 0)
	@test sum(k3) == length(v)
	@test length(c3) == length(k3) && w3 > 0
	# Degenerate cases.
	c4, k4, w4 = IG._histogram(fill(3.0, 5), 0)     # constant series -> one bin
	@test c4 == [3.0] && k4 == [5.0] && w4 == 1.0
	@test_throws ErrorException IG._histogram([1.0], 0)
	# Quantiles of a sorted vector (the IQR that sizes an automatic bin).
	s = [1.0, 2.0, 3.0, 4.0, 5.0]
	@test IG._quantile_sorted(s, 0.0) == 1.0
	@test IG._quantile_sorted(s, 0.5) == 3.0
	@test IG._quantile_sorted(s, 1.0) == 5.0
	@test isapprox(IG._quantile_sorted(s, 0.25), 2.0; atol=1e-12)
end