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

@testitem "_xy_compute op-string dispatch" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	x = collect(0.0:0.05:10.0); y = sin.(x)
	for op in ("remove_mean", "remove_trend", "deriv1", "deriv2",
	           "fitpoly:2", "savgol:5", "butter:low:1.0", "despike:2.0", "autocorr", "fft_amp", "fft_psd")
		out = IG._xy_compute(op, x, y)
		@test out !== nothing
		@test length(out) == 3                       # (xout, yout, suffix)
		@test length(out[1]) == length(out[2])
	end
	@test IG._xy_compute("nonsense", x, y) === nothing
end
