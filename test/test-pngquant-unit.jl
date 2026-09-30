# CI-safe unit tests for the pngquant / libimagequant port (src/pngquant/). The reference numbers
# were produced by the Rust libimagequant 4.5.0 itself (single-threaded build, same RGBA fed as raw
# bytes, output gamma 0.45455) — the port must report the same quality and the same MSE.

@testitem "pngquant: alpha gradient matches libimagequant 4.5" tags=[:unit, :fast] begin
	PQ = InteractiveGMT.PngQuant
	w, h = 300, 200
	px = [PQ.RGBA(UInt8(x % 256), UInt8((y * 3) % 256), UInt8((x + y) % 256), UInt8(clamp(x - 40, 0, 255)))
	      for y in 0:h-1 for x in 0:w-1]
	for (ncol, q, mse) in ((256, 52, 16.473129718198877), (16, 0, 223.41391193960143))
		liq = PQ.LiqAttr()
		PQ.set_max_colors!(liq, ncol)
		img = PQ.LiqImage(liq, px, w, h)
		res = PQ.quantize(liq, img)
		PQ.set_output_gamma!(res, 0.45455)
		pal, idx = PQ.remapped(res, img)
		@test length(pal) == ncol
		@test PQ.quantization_quality(res) == q
		@test PQ.quantization_error(res) ≈ mse rtol=1e-9
		@test length(idx) == w * h && maximum(idx) < ncol
	end
end

@testitem "pngquant: few colours are kept exactly" tags=[:unit, :fast] begin
	PQ = InteractiveGMT.PngQuant
	A = zeros(UInt8, 30, 40, 4)
	for r in 1:30, c in 1:40
		A[r, c, :] .= (50 * (((c - 1) ÷ 10) % 4), 0x80, r <= 15 ? 0 : 200, c <= 5 ? 0 : 255)
	end
	idx, pal, q = PQ.pngquant(A)
	@test size(idx) == (30, 40)
	@test q == 100
	@test length(pal) == 9
	# every opaque pixel comes back exactly; the transparent ones as the one transparent entry
	for r in 1:30, c in 1:40
		p = pal[idx[r, c]+1]
		if c <= 5
			@test p.a == 0
		else
			@test (p.r, p.g, p.b, p.a) == (A[r, c, 1], A[r, c, 2], A[r, c, 3], 0xff)
		end
	end
	@test pal[1].a == 0                 # transparent entries sort first (short tRNS)
end

@testitem "pngquant: quality floor throws, quality syntax" tags=[:unit, :fast] begin
	PQ = InteractiveGMT.PngQuant
	A = rand(UInt8, 64, 64, 3)
	@test_throws PQ.LiqError PQ.pngquant(A; quality="99-100")
	@test PQ.parse_quality("80") == (72, 80)
	@test PQ.parse_quality("-70") == (0, 70)
	@test PQ.parse_quality("60-") == (60, 100)
	@test PQ.parse_quality("65-80") == (65, 80)
	@test_throws ArgumentError PQ.parse_quality("x")
	@test PQ.quality_to_mse(100) == 0.0
	@test PQ.mse_to_quality(PQ.quality_to_mse(70)) == 70
end

@testitem "pngquant: indexed PNG round-trip at every bit depth" tags=[:unit, :fast] begin
	PQ = InteractiveGMT.PngQuant
	w, h = 37, 23                      # odd width: partial bytes at depths 1/2/4
	for ncol in (2, 4, 16, 256)
		pal = [PQ.RGBA(UInt8(k), UInt8(255 - k), UInt8(3k % 256), UInt8(k == 0 ? 0 : 255)) for k in 0:ncol-1]
		idx = UInt8[(x * 7 + y * 3) % ncol for y in 0:h-1 for x in 0:w-1]
		f = tempname() * ".png"
		try
			PQ.write_png8(f, idx, w, h, pal)
			png = PQ.read_png(f)
			@test (png.width, png.height) == (w, h)
			@test png.rgba == [pal[i+1] for i in idx]
		finally
			rm(f; force=true)
		end
	end
end

@testitem "pngquant: file front end" tags=[:unit, :fast] begin
	PQ = InteractiveGMT.PngQuant
	w, h = 64, 48
	pal = [PQ.RGBA(UInt8(4k), UInt8(255 - 4k), 0x40, 0xff) for k in 0:63]
	idx = UInt8[(x ÷ 2 + y) % 64 for y in 0:h-1 for x in 0:w-1]
	d = mktempdir()
	try
		src = joinpath(d, "in.png")
		PQ.write_png8(src, idx, w, h, pal)
		out = PQ.pngquant(src; ncolors=16)
		@test out == joinpath(d, "in-fs8.png")
		@test PQ.pngquant(src; nofs=true) == joinpath(d, "in-or8.png")
		@test_throws ArgumentError PQ.pngquant(src; ncolors=16)        # exists, no force
		png = PQ.read_png(out)
		@test (png.width, png.height) == (w, h)
		@test length(unique(png.rgba)) <= 16
	finally
		rm(d; recursive=true, force=true)
	end
end
