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

# "Sat img": A SLICE SENDS ONLY THE LAND/WATER CUT, AND THE PICTURE IS THE ONE A FULL BUILD MAKES. After
# the first build a slice rewrites just the texture's alpha from the per-node mask
# (gmtvtk_image_set_alpha_mask_h) instead of rebuilding and re-sending the whole RGBA. That shortcut is
# only allowed if it is invisible: the texture after a fast slice must be byte-for-byte the texture a
# full build of the same slice makes (same colours, same cut, same texel-to-node sampling).
@testitem "Aquamoto Sat img: the per-slice cut equals a full rebuild, byte for byte" tags=[:gui, :aquamoto, :net] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	check(h, name, on) = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_check_test), Cint,
	                           (Ptr{Cvoid}, Cstring, Cint), h, name, Cint(on))
	texhash(h) = (x = Ref{UInt64}(0); w = Ref{Cint}(0); hh = Ref{Cint}(0); nb = Ref{Cint}(0);
	              r = ccall(GmtvtkTest._test_fn(:gmtvtk_image_tex_hash_test), Cint,
	                        (Ptr{Cvoid}, Cstring, Ptr{UInt64}, Ptr{Cint}, Ptr{Cint}, Ptr{Cint}),
	                        h, IG.AQUA_SAT_NAME, x, w, hh, nb);
	              (Int(r), x[], Int(w[]), Int(hh[]), Int(nb[])))
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_satcut.nc"); nx = 160, ny = 120, nt = 6, coast = true)
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(60)
			st = IG._AQUA[f.h]
			@test check(f.h, "splitDryWetCheckBox", 1) == 1
			@test check(f.h, "renderedSatImgCheckBox", 1) == 1
			aqf_pump(40)
			@test st.satimg
			for k in (1, 4)                        # two slices: the cut moves with the wave
				IG._aquamoto_slice(f.h, k, true, false, 0.0);  aqf_pump(5)   # fast path (alpha only)
				fast = texhash(f.h)
				@test fast[1] == 1 && fast[5] == 4
				IG._aqua_sat_drape!(f.h, st; full = true);  aqf_pump(5)   # the whole build, same slice
				whole = texhash(f.h)
				@test whole[1] == 1
				@test fast[3:5] == whole[3:5]      # same texture size and bands
				@test fast[2] == whole[2]          # the same bytes
			end
		finally
			aqf_close(f.h)
		end
	end
end

# ONE CLICK UNCHECKS A GROUP (SACRED_LAW.md, group-uncheck law), on the tsunami's own groups. The `z`
# group's box used to read the Land colour bar's FLAG while its row showed the bar ACTOR (off: one bar
# of the pair is up at a time), so the uncheck cascade skipped the already-unchecked Land row, the flag
# survived, and the box came back checked — two clicks to uncheck it. The file row above it was
# built always-checked. Driven here by clicking the real checkbox, and read off the real panel.
@testitem "Aquamoto Scene Objects: one click on the z group unchecks it, and the file row follows" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	click(h, p) = Int(ccall(GmtvtkTest._test_fn(:gmtvtk_objrow_click_test), Cint, (Ptr{Cvoid}, Cstring), h, p))
	tree(h) = unsafe_string(ccall(GmtvtkTest._test_fn(:gmtvtk_objtree_checks_test), Cstring, (Ptr{Cvoid},), h))
	# The row's own box, from a line "  [x] label" of the checks tree (`nothing` when there is no row).
	function row(t, lbl)
		for ln in split(t, '\n')
			s = strip(ln)
			s == "[x] " * lbl && return true
			s == "[ ] " * lbl && return false
		end
		return nothing
	end
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_group.nc"); coast = true)
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(40)
			IG._aquamoto_slice(f.h, 1, true, false, 0.0);  aqf_pump(10)
			t0 = tree(f.h)
			@test row(t0, "z") == true
			@test row(t0, "tsu_group.nc") == true
			@test click(f.h, "tsu_group.nc/z") == 0          # ONE click: the box is off …
			t1 = tree(f.h)
			@test row(t1, "z") == false
			@test !occursin("[x]", t1)                       # … every row of the file is off …
			@test row(t1, "tsu_group.nc") == false           # … and so is the file's own row
			@test click(f.h, "tsu_group.nc/z") == 1          # and one click brings it back
			@test row(tree(f.h), "tsu_group.nc") == true
		finally
			aqf_close(f.h)
		end
	end
end

# ONE SATELLITE IMAGE. "Sat img" puts up exactly one photograph — the "Satellite image" row, at the
# size the "Res" box asks for — and nothing else wears it. It used to also be baked into the layer's
# own land colours at one pixel per node (a second, lower-resolution copy under the first), and a
# third copy was fetched two zoom levels finer for the lit land render. So: the layer's land side is
# the SAME with "Sat img" on as off, and the one image is the one size.
@testitem "Aquamoto Sat img: one satellite image, the layer's own land is untouched" tags=[:gui, :aquamoto, :net] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	check(h, name, on) = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_check_test), Cint,
	                           (Ptr{Cvoid}, Cstring, Cint), h, name, Cint(on))
	side_rgb(h, side) = (out = zeros(Float64, 3);
	                     n = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_side_rgb_test), Cint,
	                               (Ptr{Cvoid}, Cint, Ptr{Cdouble}), h, Cint(side), out); (Int(n), out))
	texdims(h) = (x = Ref{UInt64}(0); w = Ref{Cint}(0); hh = Ref{Cint}(0); nb = Ref{Cint}(0);
	              r = ccall(GmtvtkTest._test_fn(:gmtvtk_image_tex_hash_test), Cint,
	                        (Ptr{Cvoid}, Cstring, Ptr{UInt64}, Ptr{Cint}, Ptr{Cint}, Ptr{Cint}),
	                        h, IG.AQUA_SAT_NAME, x, w, hh, nb); (Int(r), Int(w[]), Int(hh[])))
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_onesat.nc"); nx = 160, ny = 120, nt = 4, coast = true)
		f = iview()
		try
			IG._on_drop(f.h, nc);  aqf_pump(60)
			st = IG._AQUA[f.h]
			@test check(f.h, "splitDryWetCheckBox", 1) == 1
			IG._aquamoto_slice(f.h, 1, true, false, 0.0);  aqf_pump(10)
			nl0, l0 = side_rgb(f.h, 1)
			@test nl0 > 0
			@test check(f.h, "renderedSatImgCheckBox", 1) == 1
			aqf_pump(40)
			@test st.satimg
			IG._aquamoto_slice(f.h, 1, true, false, 0.0);  aqf_pump(10)
			nl1, l1 = side_rgb(f.h, 1)
			@test nl1 == nl0
			@test l1 == l0                              # the layer's land wears NO photograph
			r, w, h = texdims(f.h)
			W, H, _ = IG._aqua_sat_size(st)
			@test r == 1 && (w, h) == (W, H)            # the one image, at the one size
			@test IG._AQUA_SAT_PIC[][1][2:3] == (W, H)  # …and the only picture fetched is that one
		finally
			aqf_close(f.h)
		end
	end
end

# THE "bathymetry" ROW SHOWS THE BATHYMETRY. It is a plain grid of the file, and it used to come up
# wearing the TANK's picture: the tsunami bake (`aquaLandColors`) answered from the window's tsunami
# state for EVERY grid whose nodes matched, so the sea floor was painted with the water's colours. And
# its group rows lied: the "Color Bar water" row read the window's one colour bar — which then showed
# the bathymetry's scale — as the tank's. Read off the real screen and the real panel.
@testitem "Aquamoto Scene Objects: the bathymetry row shows the bathymetry, not the tank" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	click(h, p) = Int(ccall(GmtvtkTest._test_fn(:gmtvtk_objrow_click_test), Cint, (Ptr{Cvoid}, Cstring), h, p))
	tree(h) = unsafe_string(ccall(GmtvtkTest._test_fn(:gmtvtk_objtree_checks_test), Cstring, (Ptr{Cvoid},), h))
	function row(t, lbl)
		for ln in split(t, '\n')
			s = strip(ln)
			s == "[x] " * lbl && return true
			s == "[ ] " * lbl && return false
		end
		return nothing
	end
	# The window's view, straight from the viewer (gmtvtk_capture_view_rgba): (band, col, row) RGBA,
	# copied into Julia. No PNG, no GMT call — a GMT read here corrupted the shared session in the suite.
	capture(h) = (pRgb = Ref{Ptr{UInt8}}(C_NULL); pW = Ref{Cint}(0); pH = Ref{Cint}(0);
	              ok = ccall(IG._fn(:gmtvtk_capture_view_rgba), Cint,
	                         (Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Cint}, Ptr{Cint}), h, pRgb, pW, pH);
	              @assert ok != 0;
	              A = copy(unsafe_wrap(Array, pRgb[], (4, Int(pW[]), Int(pH[]))));
	              ccall(IG._fn(:gmtvtk_free_rgb), Cvoid, (Ptr{UInt8},), pRgb[]); A)
	# share of pixels that differ by more than 40 levels in some colour band between two captures of the
	# same window: under the bug the bathymetry-only view was the TANK's picture again, near identical
	changed_share(A, B) = (n = 0;
	                       for j in axes(A, 3), i in axes(A, 2)
	                           d = maximum(abs(Int(A[b, i, j]) - Int(B[b, i, j])) for b in 1:3)
	                           d > 40 && (n += 1)
	                       end; n / (size(A, 2) * size(A, 3)))
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_bathy.nc"); coast = true)
		f = iview()
		try
			ccall(GmtvtkTest._test_fn(:gmtvtk_view_fixed_size_test), Cint, (Ptr{Cvoid}, Cint, Cint), f.h, 800, 600)
			IG._on_drop(f.h, nc);  aqf_pump(40)
			IG._aquamoto_slice(f.h, 1, true, false, 0.0);  aqf_pump(10)
			tank = capture(f.h)
			@test click(f.h, "tsu_bathy.nc/z") == 0
			@test click(f.h, "tsu_bathy.nc/bathymetry") == 1
			aqf_pump(10)
			t = tree(f.h)
			@test row(t, "z") == false                        # the tank group is off …
			@test row(t, "Color Bar water") == false          # … its water bar is not the window's bar
			@test row(t, "bathymetry") == true
			@test row(t, "tsu_bathy.nc") == true              # the file row follows its checked child
			bat = capture(f.h)
			cs = changed_share(tank, bat);  @info "changed share, tank vs bathymetry" cs
			@test size(tank) == size(bat)
			@test cs > 0.25                                   # a different picture: the bathymetry, not the tank
		finally
			aqf_close(f.h)
		end
	end
end

# A PLAIN GRID OPENED INTO A TSUNAMI WINDOW IS LIT AS THE PLAIN GRID IT IS. Open a tsunami, then open
# a second grid file into the same window (the user's repro: tsu_time.nc, then layer0.grd). Every
# Aquamoto branch of the Illumination tool used to fire on the WINDOW, so a request aimed at that grid
# lit the TANK; and the grid's reflectance shared the window's one slot, which is the tank's water
# light, so the tank's per-slice re-light overwrote it. Its illumination came out wrong and the dialog
# could not change it. Measured on the pixels: the grid's own view, and the tank's two sides.
@testitem "Aquamoto: a grid opened into a tsunami window is lit by its own light, and only it" tags=[:gui, :aquamoto] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "aquamoto_fixture.jl"))
	capture(h) = (pRgb = Ref{Ptr{UInt8}}(C_NULL); pW = Ref{Cint}(0); pH = Ref{Cint}(0);
	              ok = ccall(IG._fn(:gmtvtk_capture_view_rgba), Cint,
	                         (Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Cint}, Ptr{Cint}), h, pRgb, pW, pH);
	              @assert ok != 0;
	              A = copy(unsafe_wrap(Array, pRgb[], (4, Int(pW[]), Int(pH[]))));
	              ccall(IG._fn(:gmtvtk_free_rgb), Cvoid, (Ptr{UInt8},), pRgb[]); A)
	side_rgb(h, side) = (out = zeros(Float64, 3);
	                     n = ccall(GmtvtkTest._test_fn(:gmtvtk_aqua_side_rgb_test), Cint,
	                               (Ptr{Cvoid}, Cint, Ptr{Cdouble}), h, Cint(side), out); (Int(n), out))
	mktempdir() do dir
		nc = aqf_make_tsunami_nc(joinpath(dir, "tsu_mix.nc"); coast = true)
		f = iview()
		try
			ccall(GmtvtkTest._test_fn(:gmtvtk_view_fixed_size_test), Cint, (Ptr{Cvoid}, Cint, Cint), f.h, 800, 600)
			IG._on_drop(f.h, nc);  aqf_pump(40)
			IG._aquamoto_slice(f.h, 1, true, false, 0.0);  aqf_pump(10)
			# the tank's water lit by a model of its own, so its per-slice re-light is live
			IG._on_hillshade(f.h, "model=2\ngrid=z\nazim=45\nelev=30\nside=0\n");  aqf_pump(10)
			_, w0 = side_rgb(f.h, 0);  _, l0 = side_rgb(f.h, 1)
			# a plain grid FILE opened into the same window
			G = IG.GMT.grdmath("-R-9.5/-9.0/38.5/38.9 -I0.004 X 20 MUL SIN Y 30 MUL COS MUL 500 MUL =")
			gf = joinpath(dir, "layer0.grd");  IG.GMT.gmtwrite(gf, G)
			IG._on_drop(f.h, gf);  aqf_pump(40)
			v0 = capture(f.h)
			# ITS light: model 2, aimed at it by name, as the dialog sends it
			IG._on_hillshade(f.h, "model=2\ngrid=layer0.grd\nazim=45\nelev=30\n");  aqf_pump(10)
			v2 = capture(f.h)
			@test v2 != v0                                   # the grid really was lit
			_, w1 = side_rgb(f.h, 0);  _, l1 = side_rgb(f.h, 1)
			@test w1 == w0 && l1 == l0                       # …and the tank did not move
			# the dialog CAN change it
			IG._on_hillshade(f.h, "model=3\ngrid=layer0.grd\nazim=300\nelev=10\n");  aqf_pump(10)
			v3 = capture(f.h)
			@test v3 != v2
			# the tank's per-slice re-light never reaches it
			IG._aquamoto_slice(f.h, 2, true, false, 0.0);  aqf_pump(10)
			@test capture(f.h) == v3
		finally
			aqf_close(f.h)
		end
	end
end
