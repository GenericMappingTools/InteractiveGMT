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

# EVERY METHOD THE DIALOG OFFERS (1..7) BUILDS BOTH HALVES. The combine's half builder used to know
# only the reflectance models 2/3/4, so 5, 6 and 7 — the C++ looks, 7 being the water side's default
# — died with "unknown illumination model" on "Water side" / "Land side" / "Rendered image", and on
# every edit of a model box. Each door is called with each method; none may throw, and each half
# must come back one pixel per node.
@testitem "Aquamoto illumination: every method 1..7 builds the halves and sets a side" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))

	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_methods.nc"))
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			st = IG._AQUA[f.h]
			G  = IG._aqua_layer(st, st.cur)
			nx, ny = IG._grid_dims(G)
			for m in 1:7
				# the model boxes' door, each side
				@test IG._aqua_set_illum_model(f.h, 0, m) == 1
				@test IG._aqua_set_illum_model(f.h, 1, m) == 1
				aqf_pump(5)
				# the combine every button builds
				A = IG._aqua_shaded_rgb(f.h, st, G, true; model_water = m, model_land = m)
				@test A !== nothing && size(A) == (ny, nx, 3)
				iw, il = IG._AQUA_LAST_HALVES[f.h]
				@test size(iw) == (ny, nx, 3)
				@test size(il) == (ny, nx, 3)
				# "Water side" / "Land side"
				for side in (0, 1)
					@test redirect_stdout(devnull) do
						IG._aqua_side_popup(f.h, side, m, m)
					end == 1
					aqf_pump(5)
				end
			end
		finally
			if IG._AQUA_POPUP_WIN[] != C_NULL
				aqf_close(IG._AQUA_POPUP_WIN[]);  IG._AQUA_POPUP_WIN[] = C_NULL
			end
			aqf_close(f.h)
		end
	end
end
