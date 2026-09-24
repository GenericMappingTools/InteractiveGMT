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
			water = Dict{Int,Vector{Float64}}()          # mean water colour of the LAYER, per method
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
				# the Rendered image IS the layer: its water nodes are the water side's, pixel for pixel
				dm = IG._aqua_indland(IG._zmat(st.bat), IG._zmat(G)); nyz = size(dm, 1)
				wet = [!dm[nyz - r + 1, c] for r in 1:nyz, c in 1:size(dm, 2)]
				@test all(A[:, :, b][wet] == iw[:, :, b][wet] for b in 1:3)
				water[m] = [sum(Float64.(A[:, :, b])[wet]) / count(wet) for b in 1:3]
				# "Water side" / "Land side"
				for side in (0, 1)
					@test redirect_stdout(devnull) do
						IG._aqua_side_popup(f.h, side, m, m)
					end == 1
					aqf_pump(5)
				end
			end
			# 1 is VTK's render, 7 the CPU bake: two different pictures, never the same code path
			@test water[1] != water[7]
			# every method really changes what the layer shows
			@test length(unique(values(water))) == 7
			# 1, then 7, then THE NEXT LAYER: it stays 7 (it used to come back as 1)
			for sd in (0, 1); IG._aqua_set_illum_model(f.h, sd, 1); end
			for sd in (0, 1); IG._aqua_set_illum_model(f.h, sd, 7); end
			IG._aquamoto_slice(f.h, 1, true, false, 0.0);  aqf_pump(10)
			@test IG._aqua_side_method(f.h, 0) == 7
			@test IG._aqua_side_method(f.h, 1) == 7
			# …and 7, then 1, then the next layer: it stays 1
			for sd in (0, 1); IG._aqua_set_illum_model(f.h, sd, 1); end
			IG._aquamoto_slice(f.h, 2, true, false, 0.0);  aqf_pump(10)
			@test IG._aqua_side_method(f.h, 0) == 1
			@test IG._aqua_side_method(f.h, 1) == 1
			# "Sat img": the land shows its picture UNLIT — the same land pixels whatever its method
			ccall(IG._fn(:gmtvtk_aqua_set_land_plain_h), Cvoid, (Ptr{Cvoid}, Cint), f.h, Cint(1))
			land = Dict{Int,Array{UInt8,3}}()
			for m in (5, 7)
				IG._aqua_set_illum_model(f.h, 1, m);  aqf_pump(5)
				land[m] = IG._aqua_layer_picture(f.h, 1)
			end
			@test land[5] == land[7]
			ccall(IG._fn(:gmtvtk_aqua_set_land_plain_h), Cvoid, (Ptr{Cvoid}, Cint), f.h, Cint(0))
			IG._aqua_set_illum_model(f.h, 1, 5);  aqf_pump(5)
			@test IG._aqua_layer_picture(f.h, 1) != land[5]    # …and lit again once it is off
		finally
			if IG._AQUA_POPUP_WIN[] != C_NULL
				aqf_close(IG._AQUA_POPUP_WIN[]);  IG._AQUA_POPUP_WIN[] = C_NULL
			end
			aqf_close(f.h)
		end
	end
end

# THE 3-D AXES NEVER PIERCE THE TANK. The box is a fixed frame through the animation, and its floor
# sits below the lowest point the surface reaches in ANY slice (it used to be framed on one slice's
# range, so a deeper trough later came up through the floor's grid lines).
@testitem "Aquamoto axes: the box floor is below the surface in every slice" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	state(h, fn = :gmtvtk_scene_state) = (buf = Vector{UInt8}(undef, 1 << 16);
	            ccall(IG._fn(fn), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), h, buf, length(buf));
	            Dict(split(kv, '=')[1] => split(kv, '=')[2] for kv in split(unsafe_string(pointer(buf)), ';') if occursin('=', kv)))
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_axes.nc"))
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			st = IG._AQUA[f.h]
			floors = Float64[]
			for k in 0:st.nsteps-1
				IG._aquamoto_slice(f.h, k, true, false, 0.0);  aqf_pump(5)
				d = state(f.h)
				@test d["cubeZLock"] == "1"
				ax0 = parse(Float64, d["axZ0"])
				df  = state(f.h, :gmtvtk_scene_state_full)
				zs  = parse(Float64, df["zfac"]) * parse(Float64, df["ve"])   # the tank's drawn z scale
				push!(floors, ax0)
				# the drawn floor strictly below this slice's drawn lowest point
				@test ax0 < parse(Float64, d["zmin"]) * zs
			end
			@test length(unique(floors)) == 1                  # a FIXED frame
		finally
			aqf_close(f.h)
		end
	end
end

# THE "Res" BOX, DRIVEN AS A USER DRIVES IT: typed into, edit finished, hovered. Its hover must state
# the metres the factor asks for, and the Sat img call the dialog makes with that factor must run —
# both used to die on a factor typed as a plain integer ("1"), reaching Julia as Int64.
@testitem "Aquamoto Sat img: the Res box hover states the metres, the call runs" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	tip(h, txt) = (buf = Vector{UInt8}(undef, 4096);
	               r = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_edit_tip_test), Cint,
	                         (Ptr{Cvoid}, Cstring, Cstring, Ptr{UInt8}, Cint), h, "renderedSatResEdit", txt, buf, length(buf));
	               (Int(r), unsafe_string(pointer(buf))))
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_satres.nc"))
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			st = IG._AQUA[f.h]
			for (txt, fac) in (("1", 1.0), ("2", 2.0), ("0.5", 0.5))
				r, t = tip(f.h, txt)
				@test r == 1
				m = round(IG._aqua_sat_res_m(st.bat, fac); sigdigits = 3)
				@test occursin("≈ $(m) m", t)
			end
			# the exact call the "Sat img" box makes (factor formatted the way the dialog formats it)
			@test redirect_stdout(devnull) do
				IG._aquamoto_sat_img(f.h, false, 1.000000)
			end === nothing
		finally
			aqf_close(f.h)
		end
	end
end

# …AND THE REAL DOWNLOAD, at two factors: the zoom must follow the factor, and the land albedo must
# land on the bathymetry's own nodes (a pixel-registered grid used to fail here with "size of x,y
# vectors incompatible with 2D array size"). Needs the network.
@testitem "Aquamoto Sat img: the download follows the Res factor" tags=[:gui, :aquamoto, :net] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_satnet.nc"))
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			st = IG._AQUA[f.h]
			nx, ny = IG._grid_dims(st.bat)
			for fac in (1.0, 2.0)
				out = joinpath(dir, "msg.txt")
				open(out, "w") do io
					redirect_stdout(io) do; IG._aquamoto_sat_img(f.h, true, fac); end
				end
				msg = read(out, String)
				@test occursin("zoom $(IG._aqua_sat_zoom(st.bat, fac))", msg)
				@test size(st.imgbat, 3) == 3 && length(st.imgbat) == nx * ny * 3
			end
			@test IG._aqua_sat_zoom(st.bat, 2.0) == IG._aqua_sat_zoom(st.bat, 1.0) + 1
		finally
			aqf_close(f.h)
		end
	end
end
