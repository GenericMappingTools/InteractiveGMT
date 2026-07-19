# CI-safe unit tests for the Aquamoto dry/wet compositing math (aquamoto.jl). These call the pure
# helpers directly — no netCDF file, no Qt+VTK window — so they run anywhere `using InteractiveGMT`
# succeeds. The file I/O (_aquamoto_open/_aquamoto_slice/_aquamoto_runin) and the C++ dialog are
# exercised live (see the plan's smoke-test step), not here.
#
# What they lock down:
#   * `_aqua_composite_rgb`'s indLand mask (abs(bat-Z) < tol) correctly separates dry from wet, and
#     the HARD land overwrite always wins regardless of the transparency slider (Mirone's
#     mixe_images: land never bleeds water colour, no matter how much of the water tint was
#     cross-blended first).
#   * `_aqua_colorize` produces the right shape/dtype and spans the colour ramp (no NaN handling --
#     this file class is guaranteed clean Float32 data).
#   * `_aqua_pack_rgba`'s row-major/south-first/west-east/opaque packing (the exact convention
#     bakeLayerRGBA's own texture output uses, 40_shading.cpp) — a silent row/column swap here would
#     misdraw the whole texture without ever throwing.
#   * `_aqua_range` picks the right value on both branches (global vs local) and never returns a
#     degenerate (zlo==zhi) range.

@testitem "aquamoto helpers present" tags=[:unit, :fast] begin
	for s in (:_aqua_find_all_varnames, :_aqua_colorize, :_aqua_pack_rgba, :_aqua_range,
	          :_aqua_composite_rgb, :_aquamoto_open, :_aquamoto_slice, :_aquamoto_runin,
	          :_aqua_global_minmax, :_aquamoto_set_var)
		@test isdefined(InteractiveGMT, s)
	end
end

@testitem "aqua_range: local extrema vs global, degenerate range nudged" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	@test IG._aqua_range([1.0, 5.0, 3.0], false, -99.0, -99.0) == (1.0, 5.0)
	@test IG._aqua_range([1.0, 5.0, 3.0], true, -2.0, 8.0) == (-2.0, 8.0)
	@test IG._aqua_range(Float64[], false, -1.0, -1.0) == (0.0, 1.0)      # empty -> safe default
	# All-equal -> reset to a clean SYMMETRIC span around the value (not a one-sided nudge, which
	# left a confusing near-zero end untouched in the near-noise case below).
	lo, hi = IG._aqua_range([4.0, 4.0, 4.0], false, 0.0, 0.0)
	@test lo < 4.0 < hi && isapprox(hi - lo, 0.2)
	# REGRESSION (colourbar showed "-0 / 0 / 0"): a t=0 tsunami frame's wet cells are essentially
	# zero but not EXACTLY equal (floating-point noise) -- must still be caught, not just lo==hi.
	lo2, hi2 = IG._aqua_range([-1e-14, 2e-15, 5e-15], false, 0.0, 0.0)
	@test (hi2 - lo2) >= 0.19   # widened to the clean fallback span, not left near-zero-width
end

@testitem "aqua_colorize: shape, dtype, distinct colours across the ramp" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	Z = Float32[0.0 1.0 2.0; 3.0 4.0 5.0]     # 2x3, the real dtype tsunami grids carry (no NaN -- never occurs)
	rgb = IG._aqua_colorize(Z, 0.0, 5.0, :turbo)
	@test size(rgb) == (2, 3, 3)
	@test eltype(rgb) === UInt8
	@test rgb[1, 1, :] != rgb[2, 3, :]   # low end vs high end of the ramp actually differ
end

@testitem "aqua_pack_rgba: row-major south-first west-east, opaque" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# 2 (ny) x 3 (nx) x 3, GMT native layout: row 1 = south, row 2 = north; col 1..3 = west->east.
	rgb = Array{UInt8}(undef, 2, 3, 3)
	for j in 1:3, i in 1:2
		rgb[i, j, 1] = UInt8(10i); rgb[i, j, 2] = UInt8(20j); rgb[i, j, 3] = UInt8(i + j)
	end
	buf = IG._aqua_pack_rgba(rgb)
	@test length(buf) == 2 * 3 * 4
	# Output row 0 (bytes 1..12) must be GMT row 1 (south), west->east; row 1 (bytes 13..24) = GMT row 2.
	for j in 1:3
		b = (j - 1) * 4
		@test buf[b+1] == UInt8(10 * 1) && buf[b+2] == UInt8(20j) && buf[b+3] == UInt8(1 + j) && buf[b+4] == 0xff
	end
	for j in 1:3
		b = 12 + (j - 1) * 4
		@test buf[b+1] == UInt8(10 * 2) && buf[b+2] == UInt8(20j) && buf[b+3] == UInt8(2 + j) && buf[b+4] == 0xff
	end
end

@testitem "aqua_composite_rgb: dry/wet split, hard land overwrite beats transparency" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# 3x3: a dry strip (row 1, matches bathymetry exactly -> land) and wet cells elsewhere.
	bat = Float32[ 10.0 10.0 10.0;  -5.0 -5.0 -5.0;  -5.0 -5.0 -5.0 ]
	Z   = Float32[ 10.0 10.0 10.0;   2.0  3.0  4.0;   2.0  3.0  4.0 ]

	noimg = Array{UInt8}(undef, 0, 0, 0)   # "no cache yet" sentinel (see _AquaState.imgbat)
	landhi = 10.0                          # known max land elevation for `bat` (no scan -- caller's job now)

	# Not split: plain colourisation of Z over its own extrema, no land/water distinction.
	rgb_flat, _ = IG._aqua_composite_rgb(bat, Z, false, 2.0, 4.0, 0.5, noimg, landhi)
	@test size(rgb_flat) == (3, 3, 3)

	# Split, alfa=0 (opaque water, no land tint blended in) -- land row still shows the LAND colour,
	# never Z's own colourisation (Z there was clamped to 0 before colourising in Mirone's mixe_images;
	# land pixels are hard-overwritten regardless of alfa).
	rgb0, imgbat = IG._aqua_composite_rgb(bat, Z, true, 2.0, 4.0, 0.0, noimg, landhi)
	@test !isempty(imgbat)
	@test rgb0[1, 1, :] == imgbat[1, 1, :]
	@test rgb0[1, 2, :] == imgbat[1, 2, :]
	@test rgb0[1, 3, :] == imgbat[1, 3, :]

	# Split, alfa=1 (fully cross-blended toward land tint EVERYWHERE) -- land pixels are STILL exactly
	# the land colour (the hard overwrite runs after the blend), proving land never depends on alfa.
	rgb1, imgbat2 = IG._aqua_composite_rgb(bat, Z, true, 2.0, 4.0, 1.0, imgbat, landhi)
	@test rgb1[1, 1, :] == imgbat2[1, 1, :]
	@test rgb1[2, 1, :] == imgbat2[2, 1, :]   # a WET cell at alfa=1 also equals the land colour (100% blend)

	# The cached imgbat is reused byte-for-byte across calls (only depends on bathymetry).
	@test imgbat === imgbat2

	# A wet cell at alfa=0 must NOT equal the land colour (no blending at all towards land).
	rgb_wet0, _ = IG._aqua_composite_rgb(bat, Z, true, 2.0, 4.0, 0.0, imgbat, landhi)
	@test rgb_wet0[2, 1, :] != imgbat[2, 1, :]
end
