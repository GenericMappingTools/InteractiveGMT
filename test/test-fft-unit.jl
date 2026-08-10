# test-fft-unit.jl — the FFT tool (src/fftstuff.jl, port of Mirone's src_figs/fft_stuff.m).
#
# Two tiers, and the second one exists because of a real bug: every operation was filing its result
# in the Julia object registry WITHOUT ever building a surface, so Scene Objects grew a tidy row and
# the window stayed EMPTY. A test that only checks the maths, or only checks the registry, passes
# straight through that. The GUI tier therefore clicks the dialog's real buttons and asserts the
# result is a ROW IN THE PANEL — which only exists if the surface was actually added.

using TestItems

@testitem "fft: helpers present" begin
	for f in (:fft_spectrum, :fft_radial_power, :fft_continuation, :fft_derivative, :fft_integrate,
	          :fft_dir_derivative, :fft_analytic_signal, :_on_fftstuff, :_register_fftstuff,
	          :_fft_scltln, :_fft_wavenumbers, :_fft_scaled_inc, :_fft_image_to_grid)
		@test isdefined(InteractiveGMT, f)
	end
	# the padding + good-size table are rtp3d.jl's (one mboard port, not two)
	@test isdefined(InteractiveGMT, :_mboard_taper)
	@test isdefined(InteractiveGMT, :_FFT_GOOD_SIZES)
end

@testitem "fft: the transforms answer what theory says they must" begin
	IG = InteractiveGMT;  GMT = IG.GMT
	nx, ny, d, L = 128, 128, 100.0, 3200.0        # 100 m nodes, a 3.2 km sinusoid in x
	k = 2pi / L
	x = collect(0.0:d:(nx-1)*d);  y = collect(0.0:d:(ny-1)*d)
	Z = [sin(2pi * x[i] / L) for j in 1:ny, i in 1:nx]
	G = GMT.mat2grid(Float32.(Z); x = x, y = y)
	o = IG.FFTOpts(nx, ny, IG.FFT_METERS, false)
	inner = 20:109                                 # away from the wrap-around edges

	Gd, nd = IG.fft_derivative(G, o, 1)            # d/dz of a sinusoid = k * itself
	@test occursin("Vertical Derivative", nd)
	@test maximum(abs.(Gd.z[inner, inner] .- Float32(k) .* Z[inner, inner])) < 1e-4

	Gc, _ = IG.fft_continuation(G, o, 500.0)       # upward continuation attenuates by exp(-k h)
	@test isapprox(maximum(Gc.z[inner, inner]), exp(-k * 500.0); rtol = 1e-3)

	Gi, _ = IG.fft_integrate(G, o)                 # integration is its inverse
	@test maximum(abs.(Gi.z[inner, inner] .- Float32(1 / k) .* Z[inner, inner])) < 1e-3

	Gdd, _ = IG.fft_dir_derivative(G, o, 90)       # azimuth 90 = d/dx
	want = [Float32(k * cos(2pi * x[i] / L)) for j in 1:ny, i in 1:nx]
	@test maximum(abs.(Gdd.z[inner, inner] .- want[inner, inner])) < 1e-4

	Ga, _ = IG.fft_analytic_signal(G, o)           # of a sinusoid it is the constant k
	@test isapprox(minimum(Ga.z[inner, inner]), k; rtol = 1e-3)
	@test isapprox(maximum(Ga.z[inner, inner]), k; rtol = 1e-3)

	Gp, np = IG.fft_spectrum(G, :power, o)         # power peaks at the sinusoid's own wavenumber
	@test np == "Power spectrum"
	pk = argmax(Gp.z)
	@test isapprox(abs(Gp.x[pk[2]]), k; rtol = 1e-2)
	@test isapprox(Gp.y[pk[1]], 0.0; atol = 1e-9)

	f, p, lab = IG.fft_radial_power(G, o)          # radial average peaks at frequency 1/L
	@test isapprox(f[argmax(p)], 1 / L; rtol = 1e-2)
	@test lab == "Frequency (1/m)"
end

@testitem "fft: correlations, cross ops and padding" begin
	IG = InteractiveGMT;  GMT = IG.GMT
	n = 128
	# A deterministic non-periodic field: a bump plus a fixed pseudo-random ripple (no Random dep —
	# the test environment carries only what the package itself does).
	R = [sin(0.7i) * cos(1.3j) + 0.5sin(2.1i + 0.9j) + 5exp(-((i - 64)^2 + (j - 64)^2) / 200)
	     for i in 1:n, j in 1:n]
	x = collect(0.0:100.0:(n-1)*100);  y = copy(x)
	G = GMT.mat2grid(Float32.(R); x = x, y = y)
	o = IG.FFTOpts(n, n, IG.FFT_METERS, false)

	Gac, nac = IG.fft_spectrum(G, :autocorr, o)     # zero lag sits at the centre
	@test nac == "Autocorrelation"
	pc = argmax(Gac.z)
	@test Gac.x[pc[2]] == 0.0 && Gac.y[pc[1]] == 0.0

	Gcc, _ = IG.fft_spectrum(G, :crosscorrel, o; G2 = G)   # against itself = the autocorrelation
	@test isapprox(Gcc.z, Gac.z; rtol = 1e-4)
	Gxp, _ = IG.fft_spectrum(G, :crosspower, o; G2 = G)    # ... and likewise for the power
	Gp, _  = IG.fft_spectrum(G, :power, o)
	@test isapprox(Gxp.z, Gp.z; rtol = 1e-4)
	@test_throws Exception IG.fft_spectrum(G, :crosspower, o)   # a cross op needs its second grid

	# Padding must not move the answer, and must not change the result's geometry either.
	o2 = IG.FFTOpts(160, 160, IG.FFT_METERS, false)
	G1, _ = IG.fft_derivative(G, o, 1)
	G2, _ = IG.fft_derivative(G, o2, 1)
	@test size(G2.z) == size(G1.z)
	@test G2.range[1:4] == G1.range[1:4]
	@test maximum(abs.(G1.z[30:99, 30:99] .- G2.z[30:99, 30:99])) < 1e-3
end

@testitem "fft: geographic scaling and the image door" begin
	IG = InteractiveGMT;  GMT = IG.GMT
	sclat, sclon = IG._fft_scltln(45.0)             # WGS-84 metres per degree at 45 N (Snyder)
	@test isapprox(sclat, 111132.0; rtol = 1e-4)
	@test isapprox(sclon, 78846.8;  rtol = 1e-4)
	@test IG._fft_scltln(0.0)[2] > IG._fft_scltln(60.0)[2]     # meridians converge

	# Geogs must scale the node spacing; Kilometers must be 1000x Meters.
	G = GMT.mat2grid(Float32[i+j for i in 1:16, j in 1:16]; x = collect(0.0:15), y = collect(0.0:15))
	dxm, _ = IG._fft_scaled_inc(G, IG.FFTOpts(16, 16, IG.FFT_METERS, false))
	dxk, _ = IG._fft_scaled_inc(G, IG.FFTOpts(16, 16, IG.FFT_KM, false))
	dxg, _ = IG._fft_scaled_inc(G, IG.FFTOpts(16, 16, IG.FFT_GEOG, false))
	@test dxm == 1.0 && dxk == 1000.0 && dxg > 80_000

	# An image is transformable: RGB collapses to grey, and a PIXEL-REGISTERED one (x/y hold nx+1
	# coordinates) must still come through — building the grid from those vectors by hand does not.
	I = GMT.mat2img(UInt8[(i+j+b) % 255 for i in 1:21, j in 1:30, b in 1:3]; x = [0.0, 30.0], y = [0.0, 21.0])
	Gi = IG._fft_image_to_grid(I)
	@test Gi isa GMT.GMTgrid && size(Gi.z) == (21, 30)
end

# ---------------------------------------------------------------------------------------------
# GUI tier: the dialog's own buttons, clicked. This is the tier that catches "the row is there but
# the window is empty" — `gmtvtk_objrows_test` reads the REAL Scene Objects panel, and a row for the
# result only appears once the surface has actually been built.
@testitem "fft: every dialog button puts its result in the window" setup=[GmtvtkTest] begin
	IG = InteractiveGMT;  GMT = IG.GMT
	G = GMT.mat2grid(Float32[sin(2pi*i/32) + 0.4cos(2pi*j/50) + 0.01i for j in 1:97, i in 1:143];
	                 x = collect(1.0:143), y = collect(1.0:97))
	fig = IG.iview(G);  sleep(2)
	GmtvtkTest.register_fftstuff_test()                    # the test dll keeps its own callback slot
	GmtvtkTest.scene_adopt(fig.h)                          # ... and its own idea of which scenes live

	# The size boxes must open on the next good FFT number ABOVE the grid (Mirone's mboard default),
	# not on the grid's own size: round(97*1.2)=116 -> 120, round(143*1.2)=172 -> 180.
	@test GmtvtkTest.fft_open(fig.h) == 1
	@test GmtvtkTest.fft_sizes(fig.h) == (120, 180)

	rows0 = length(split(GmtvtkTest.objrows(fig.h), '\n'))
	cases = (("push_powerSpectrum", "Power spectrum"), ("push_autoCorr", "Autocorrelation"),
	         ("push_integrate", "Integrated grid"), ("push_faa2geoid", "Geoid height"),
	         ("push_geoid2faa", "Gravity anomaly"), ("push_AnalyticSig", "3D Analytic Signal"),
	         ("push_goUDcont", "U/D Continuation"), ("push_goDerivative", "Vertical Derivative"),
	         ("push_goDirDerivative", "Azimuthal Derivative"))
	for (btn, want) in cases
		@test GmtvtkTest.fft_click(fig.h, btn) == 1
		sleep(0.4)
		@test occursin(want, GmtvtkTest.objrows(fig.h))
	end
	# ... and the panel really grew by one row per operation (the empty-window bug grew none).
	@test length(split(GmtvtkTest.objrows(fig.h), '\n')) > rows0 + length(cases) - 1

	# MINIMISE parks it in Scene Objects — the shared parkTool row Color Palettes and Load Bands
	# use — and it comes back from there. A dialog that merely hides is a dialog nobody can reopen.
	@test GmtvtkTest.fft_park(fig.h, true) == 1
	@test occursin("FFT tool", GmtvtkTest.objrows(fig.h))
	@test GmtvtkTest.fft_park(fig.h, false) == 0
end
