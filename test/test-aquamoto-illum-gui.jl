# WATER AND LAND ARE INDEPENDENT. THIS FILE MEASURES IT ON THE PIXELS.
#
# A tsunami layer is two images standing on two surfaces, and each side's illumination is its own.
# Every regression here was the same shape: an operation aimed at ONE side changed the OTHER — a
# bake that re-stamped the unselected side's light, a button that moved the Shade Water / Shade Land
# radio (which is also what the colour bar and `_aquamoto_slice` read), a look setter that copied the
# method to both sides. So the assertion is never "the code took the side argument": it is the MEAN
# RGB OF THE TEXELS THEMSELVES, split by the same `aquaLandMask` the composite was painted with
# (`gmtvtk_aqua_side_rgb_test`), before and after. The untouched side must come back bit-identical.
#
# The door used is the one the two Illumination buttons knock on: `_on_hillshade` with a `side=` line
# in the request block — the same string the dialog builds. Nothing is assembled around the code
# under test.

@testitem "Aquamoto illumination: lighting the WATER leaves the LAND pixel-identical" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))

	side_rgb(h, side) = begin
		out = zeros(Float64, 3)
		n = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_side_rgb_test), Cint,
		          (Ptr{Cvoid}, Cint, Ptr{Cdouble}), h, Cint(side), out)
		(Int(n), out)
	end

	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_illum.nc"))
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			# The composite must be on screen, with both sides really present — otherwise every
			# comparison below is between two empty sets and passes for the wrong reason.
			nw, w0 = side_rgb(f.h, 0)
			nl, l0 = side_rgb(f.h, 1)
			@test nw > 0
			@test nl > 0

			# WATER ONLY.
			IG._on_hillshade(f.h, "model=2\ngrid=z\nazim=45\nelev=30\nside=0\n");  aqf_pump(20)
			nw1, w1 = side_rgb(f.h, 0)
			nl1, l1 = side_rgb(f.h, 1)
			@test nw1 == nw && nl1 == nl          # the split itself did not move
			@test l1 == l0                        # THE LAND IS UNTOUCHED, byte for byte
			@test w1 != w0                        # …and the water really was lit
		finally
			aqf_close(f.h)
		end
	end
end

@testitem "Aquamoto illumination: lighting the LAND leaves the WATER pixel-identical" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))

	side_rgb(h, side) = begin
		out = zeros(Float64, 3)
		n = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_side_rgb_test), Cint,
		          (Ptr{Cvoid}, Cint, Ptr{Cdouble}), h, Cint(side), out)
		(Int(n), out)
	end

	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_illum2.nc"))
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			nw, w0 = side_rgb(f.h, 0)
			nl, l0 = side_rgb(f.h, 1)
			@test nw > 0
			@test nl > 0

			# LAND ONLY.
			IG._on_hillshade(f.h, "model=2\ngrid=z\nazim=45\nelev=30\nside=1\n");  aqf_pump(20)
			nw1, w1 = side_rgb(f.h, 0)
			nl1, l1 = side_rgb(f.h, 1)
			@test nw1 == nw && nl1 == nl
			@test w1 == w0                        # THE WATER IS UNTOUCHED, byte for byte
			@test l1 != l0                        # …and the land really was lit
		finally
			aqf_close(f.h)
		end
	end
end

@testitem "Aquamoto illumination: a side's method survives the other side being re-lit" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))

	side_rgb(h, side) = begin
		out = zeros(Float64, 3)
		n = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_side_rgb_test), Cint,
		          (Ptr{Cvoid}, Cint, Ptr{Cdouble}), h, Cint(side), out)
		(Int(n), out)
	end

	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_illum3.nc"))
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			IG._on_hillshade(f.h, "model=2\ngrid=z\nazim=45\nelev=30\nside=0\n");  aqf_pump(20)
			_, wA = side_rgb(f.h, 0)
			# Now light the LAND with a DIFFERENT model. The water was lit a moment ago and must be
			# exactly as it was — this is the case that kept coming back, because the land's act ran
			# through code that wrote "the window's" light.
			IG._on_hillshade(f.h, "model=3\ngrid=z\nazim=300\nelev=10\nside=1\n");  aqf_pump(20)
			_, wB = side_rgb(f.h, 0)
			@test wB == wA

			# …and the reverse: re-lighting the water must not disturb the land it just gained.
			_, lB = side_rgb(f.h, 1)
			IG._on_hillshade(f.h, "model=3\ngrid=z\nazim=120\nelev=60\nside=0\n");  aqf_pump(20)
			_, lC = side_rgb(f.h, 1)
			@test lC == lB
		finally
			aqf_close(f.h)
		end
	end
end
