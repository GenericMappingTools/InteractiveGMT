# The IPMA radar dialog's OpenStreetMap background: the ocean must be ONE colour.
#
# OSM serves an all-sea tile as a 1-bit PALETTE PNG (one colour). GMT.mosaic reads every tile with
# gdalread + ind2rgb; when the palette stride is wrong, green and blue come from the padding and the
# sea turns cyan (170, 255, 255) instead of (170, 211, 223) — two shades of blue in one map. Fixed in
# GMT.jl (get_cpt_from_colortable returns the stride 256) on 2026-11-08, and LOST again on 2026-11-18
# without any GMT.jl change: a stray Manifest.toml in this package dir pinned the REGISTERED GMT.jl,
# so the launcher stopped loading the dev checkout that carries the fix. Both tests below would have
# caught it. No network, no window.

@testitem "ipma background: a 1-bit palette OSM sea tile decodes to its real colour" tags=[:unit, :ipma] begin
	using InteractiveGMT
	# The real OSM tile 06/031323.png (all sea, 256x256, 1-bit palette, one entry (170, 211, 223)).
	png = UInt8[
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
		0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x66, 0xbc, 0x3a,
		0x25, 0x00, 0x00, 0x00, 0x03, 0x50, 0x4c, 0x54, 0x45, 0xaa, 0xd3, 0xdf, 0xcf, 0xec, 0xbc, 0xf5,
		0x00, 0x00, 0x00, 0x1f, 0x49, 0x44, 0x41, 0x54, 0x68, 0x43, 0xed, 0xc1, 0x01, 0x0d, 0x00, 0x00,
		0x00, 0xc2, 0xa0, 0xf7, 0x4f, 0x6d, 0x0e, 0x37, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0xbe, 0x0d, 0x21, 0x00, 0x00, 0x01, 0xcb, 0xe3, 0x3d, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x49,
		0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82]
	mktempdir() do dir
		f = joinpath(dir, "osm_sea_1bit.png")
		write(f, png)
		J = GMT.gdalread(f)
		RGB = GMT.ind2rgb(J)
		# Every pixel is the one sea colour; the cyan of the stride bug is (170, 255, 255).
		@test RGB.image[1, 1, 1] == 0xaa
		@test RGB.image[1, 1, 2] == 0xd3
		@test RGB.image[1, 1, 3] == 0xdf
		@test all(==(0xd3), view(RGB.image, :, :, 2))
		@test all(==(0xdf), view(RGB.image, :, :, 3))
	end
end

@testitem "no Manifest.toml in the package dir (it pins the registered GMT.jl)" tags=[:unit, :ipma] begin
	using InteractiveGMT
	# A Manifest here makes this dir its own environment, resolved against the REGISTERED GMT.jl; the
	# launcher then loads that one and every fix that lives only in the dev GMT.jl checkout is gone.
	@test !isfile(joinpath(pkgdir(InteractiveGMT), "Manifest.toml"))
	# Where a dev checkout of GMT.jl exists, it is the one loaded.
	devgmt = joinpath(first(DEPOT_PATH), "dev", "GMT")
	isdir(devgmt) && @test normpath(pkgdir(GMT)) == normpath(devgmt)
end
