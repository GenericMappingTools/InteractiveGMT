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
	          :_aqua_global_minmax, :_aquamoto_set_var, :_aqua_mask_image, :_aqua_is_byte_raster)
		@test isdefined(InteractiveGMT, s)
	end
end

# 2026-09-18: an Aquamoto byte mask (LongBeach/ShortBeach — an inundation footprint) read back from
# gdalread as UInt8 with n_colors=0 and an empty colormap: netCDF byte variables carry no GDAL raster
# colour table (verified against GMT.jl's gdal_utils.jl, and against this project's own long_beach.grd
# / short_beach.grd via `gdalinfo` -> ColorInterp=Undefined). `_aqua_mask_image` used to assume the
# file's own palette made the picture black/white; with none, `_pixaccess_img` fell back to the raw
# 0/1 index values as literal RGB — near-black on near-black, present in the scene, invisible on
# screen. The fix is the guard this test locks down: not indexed -> get an explicit B/W palette,
# through the SAME setter every other indexed image in this codebase uses (`_img_set_palette!`).
#
# A MASK IMAGE IS BLACK AND WHITE, and OPAQUE. Nothing here is to be "improved" into transparency or
# a colour scale: the asserts below are the specification, not a snapshot.
@testitem "aquamoto: a mask with no on-disk palette gets an explicit B/W one, never raw 0/1 as RGB" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# Exactly the shape gdalread hands back for a real netCDF byte mask: 2-D UInt8, no colour table.
	I = IG.GMT.mat2img(UInt8[0 1 0; 1 1 0; 0 0 1])
	I.n_colors = 0
	I.colormap = Int32[]
	@test !IG._img_is_indexed(I)
	@test IG._is_mask_image(I)                      # recognised by its VALUES (digitize.jl)
	# The exact guard `_aqua_mask_image` runs on a non-indexed mask.
	IG._img_is_indexed(I) || IG._img_set_palette!(I, UInt8[0 0 0; 255 255 255])
	@test IG._img_is_indexed(I)
	@test I.image == UInt8[0 1 0; 1 1 0; 0 0 1]     # THE BYTES ARE NOT TOUCHED -- only the palette is added

	pix, nb, nlon, nlat, rowmajor = IG._pixaccess_img(I)
	seen = Set{Tuple{UInt8,UInt8,UInt8}}()
	for lat in 1:nlat, lon in 1:nlon
		push!(seen, (pix(lat, lon, 1), pix(lat, lon, 2), pix(lat, lon, 3)))
	end
	# Every pixel is pure black or pure white -- never a near-black raw index value.
	@test seen == Set([(0x00, 0x00, 0x00), (0xff, 0xff, 0xff)])

	buf, iw, ih, ibands = IG._drape_to_bbox(I, I.range[1], I.range[2], I.range[3], I.range[4];
	                                        outside=:transparent, fill=(200,200,200))
	@test ibands == 4
	for p in 0:(iw*ih - 1)
		r, g, b, a = buf[p*4+1], buf[p*4+2], buf[p*4+3], buf[p*4+4]
		@test a == 0xff                                 # opaque -- never see-through where the mask covers
		@test (r,g,b) == (0x00,0x00,0x00) || (r,g,b) == (0xff,0xff,0xff)
	end
end

# "Digitize whites": the outline of the white region, traced by GDAL (GDALPolygonize) on the mask
# itself. One closed ring around a single square block, in the mask's OWN coordinates — a boundary
# that comes back somewhere else is a transposed/flipped read of the buffer.
@testitem "digitize whites: GDAL traces the white region's own outline" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	m = zeros(UInt8, 9, 9)
	m[4:6, 2:3] .= 0x01                                  # a block of 1s, row 1 = south
	I = IG.GMT.mat2img(m; x = collect(1.0:9.0), y = collect(1.0:9.0))
	@test IG._is_mask_image(I)                           # UInt8, two states: a mask
	@test IG._mask_white(I) == 1
	# Not masks: a third state, and a single state.
	@test !IG._is_mask_image(IG.GMT.mat2img(UInt8[0 1 2; 1 1 0; 0 0 1]))
	@test !IG._is_mask_image(IG.GMT.mat2img(zeros(UInt8, 3, 3)))
	xyz, segoff, nseg, npts = IG._digitize_rings(I, 1)
	@test nseg == 1 && npts >= 5                         # one closed ring around one block
	xs = xyz[1:3:end];  ys = xyz[2:3:end]
	@test all(iszero, xyz[3:3:end])                      # a mask boundary has no elevation
	# The ring hugs the block's own cells, to within the half cell a node-registered edge sits at.
	@test extrema(xs) == (1.5, 3.5)
	@test extrema(ys) == (3.5, 6.5)
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
	buf = IG._aqua_pack_rgba(rgb, 0, 3, 2)      # zlayout 0 = "BCB", nx=3, ny=2
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

	# SAME field, composited off a grid read in "TRB" (row-major, north row first): the planes then sit
	# in THAT element order — plane index of node (ix,iy) is (ny-1-iy)*nx + ix — and the packer must
	# produce the byte-identical output. This is the whole point of carrying a layout code instead of
	# transposing (SACRED_LAW.md, grid memory-layout law).
	rgbT = Array{UInt8}(undef, 2, 3, 3)   # dims are nominal; only the element order matters here
	for j in 1:3, i in 1:2                # i = 1 -> south (iy=0), j = 1 -> west (ix=0)
		m = (2 - i) * 3 + (j - 1)         # (ny-1-iy)*nx + ix, 0-based
		rgbT[m + 1]     = UInt8(10i)
		rgbT[m + 1 + 6] = UInt8(20j)
		rgbT[m + 1 + 12] = UInt8(i + j)
	end
	@test IG._aqua_pack_rgba(rgbT, 3, 3, 2) == buf     # zlayout 3 = "TRB"
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

@testitem "aquamoto: the Illumination tool lights water and land from their OWN surfaces" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# The tsunami composite stands on TWO surfaces (water on the live stage, land on the static
	# bathymetry), so the tool computes ONE reflectance PER SIDE and pushes each with its own `side`.
	for s in (:_aqua_illuminate!, :_aqua_shaded_rgb, :aqua_shade_image, :_hs_reflectance)
		@test isdefined(InteractiveGMT, s)
	end
	# ...and the two halves are NEVER paired node-by-node in Julia: the composition is the composite's
	# own (bakeAquaShade, by the aquaLandMask the image was painted with). A Julia-side merge re-indexed
	# a BCB reflectance against a TRB mask and striped the water.
	@test !isdefined(InteractiveGMT, :_aqua_merge_half!)
	@test !isdefined(InteractiveGMT, :_aqua_combined_reflectance)
	# The state carries what a per-slice relight needs: which slice is on screen, and the loaded model
	# OF EACH SIDE (two instances of one thing, indexed by the `side` code the viewer speaks).
	@test :cur in fieldnames(IG._AquaState)
	@test :illum in fieldnames(IG._AquaState)
	@test fieldtype(IG._AquaState, :illum) === NTuple{2,Dict{String,String}}

	# ONE reflectance function for every surface: same call, different grid. Two DIFFERENT surfaces
	# must give two DIFFERENT reflectances -- that difference IS the land/ocean split the old
	# single-grid push threw away.
	x = collect(range(0.0, 1.0; length=24))
	bat = IG.GMT.mat2grid(Float32[Float32(-100 + 40 * sin(6xx) * cos(6yy)) for yy in x, xx in x]; x=x, y=x)
	stg = IG.GMT.mat2grid(Float32[Float32(0.4 * sin(20xx + 3yy)) for yy in x, xx in x]; x=x, y=x)
	d = Dict{String,String}("azim" => "315", "elev" => "30")
	# The reflectance models, in the CURRENT numbering: 2 grdgradient classic, 3 grdgradient
	# Lambertian, 4 Lambertian with lighting (its +a/+d/+p/+s tail comes from `num`'s defaults).
	# 1, 5, 6 and 7 are C++ looks and never reach this function — asking it for one is an error.
	for model in (2, 3, 4)
		Rb = IG._hs_reflectance(bat, model, d)
		Rs = IG._hs_reflectance(stg, model, d)
		@test size(Rb) == size(Rs) == (length(x), length(x))
		@test eltype(Rb) === Float32
		@test any(isfinite, Rb) && any(isfinite, Rs)
		@test Rb != Rs
	end
	@test_throws ErrorException IG._hs_reflectance(bat, 99, d)
end

@testitem "aquamoto: a GMT reflectance comes back in a DIFFERENT layout than its source grid" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	# THE MEASUREMENT THAT EXPLAINS THE STRIPES. `grdgradient` returns a PLAIN (BCB) grid whatever
	# layout it was handed, so a reflectance may NEVER be paired node-by-node with a mask built in the
	# source grid's own order. That pairing is what striped the water, and it is why the two halves are
	# composed by the composite itself (bakeAquaShade) and never in Julia.
	x = collect(range(0.0, 1.0; length=16))
	G = IG.GMT.mat2grid(Float32[Float32(sin(6xx) * cos(6yy)) for yy in x, xx in x]; x=x, y=x)
	G.layout = "TRB"
	for kw in (Dict{Symbol,Any}(:A => 45.0, :N => "t"), Dict{Symbol,Any}(:E => "s45.0/30.0"))
		g = IG.GMT.grdgradient(deepcopy(G); kw...)
		@test size(g.z) == size(G.z)
		@test g.layout[1:2] != G.layout[1:2]      # it did NOT keep the source's order
	end
end
