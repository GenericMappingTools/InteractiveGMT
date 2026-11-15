# Image Enhance ("1 - Indexed and RGB") and the image handle's "Auto histogram stretch".

# An RGB composition gets ONE HISTOGRAM PER BAND (R, G, B) — also when an Enhance dialog was opened
# earlier on a single band of the same window. The window keeps one Enhance dialog; it used to bring
# that one back (built for the band, one histogram) whatever image was asked for (2026-11-12).
@testitem "Image Enhance on an RGB composition shows R, G and B histograms" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	pump(n) = for _ in 1:n  IG._pump_once()  end
	nx, ny = 120, 80
	for T in (UInt16, UInt8)
		mk(k) = IG.GMT.mat2img(T[clamp(round(Int, (T == UInt8 ? 60 : 7000) + 40k + 20randn()), 0, typemax(T))
		                         for _ in 1:ny, _ in 1:nx];
		                       x = [0.0, Float64(nx)], y = [0.0, Float64(ny)], noconv = true)
		fig = IG.iview_image_obj(mk(0), "band1")
		h = getfield(fig, :h)
		try
			pump(20)
			IG._drop_into(h, mk(1), "band2"; promote = false);  pump(5)
			IG._drop_into(h, mk(2), "band3"; promote = false);  pump(5)
			trig(p) = ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), h, p)
			# Enhance on a single band first: one histogram.
			@test trig("Image/Image Enhance/1 - Indexed and RGB") >= 1
			pump(10)
			@test any(s -> s.scene == h && s.srcname == "band3" && s.nbands == 1, values(IG._ENHSTATE))
			# Load Bands -> RGB composition, then Enhance again through the same menu.
			@test IG._bands_from_loaded!(h) == 3
			IG._bands_show!(h, IG._bands_get_rgb(h, 1, 2, 3), "RGB composition");  pump(10)
			@test trig("Image/Image Enhance/1 - Indexed and RGB") >= 1
			pump(10)
			@test any(s -> s.scene == h && s.srcname == "RGB composition" && s.nbands == 3, values(IG._ENHSTATE))
		finally
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
			pump(5)
		end
	end
end

# A REMOVED ROW GIVES ITS MEMORY BACK (2026-11-15: VRT -> RGB -> Adjust Contrast -> remove every row
# left julia holding the 16-bit originals). The full-precision original (_IMG_ORIG), the Adjust
# Contrast state built on it (_ENHSTATE) and, for the base row, the window's figure (_FIGREG) all go
# with the row.
@testitem "Removing a row releases its original, its dialog state and the base figure" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	pump(n) = for _ in 1:n  IG._pump_once()  end
	nx, ny = 120, 80
	mk(k) = IG.GMT.mat2img(UInt16[clamp(round(Int, 7000 + 40k + 20randn()), 0, typemax(UInt16)) for _ in 1:ny, _ in 1:nx];
	                       x = [0.0, Float64(nx)], y = [0.0, Float64(ny)], noconv = true)
	# An extra: the row's Remove (the production removal) takes the original and the dialog state.
	fig = IG.iview_image_obj(mk(0), "band1")
	h = getfield(fig, :h)
	try
		pump(20)
		IG._drop_into(h, mk(1), "band2"; promote = false);  pump(5)
		@test haskey(IG._IMG_ORIG[h], "band2")
		trig(p) = ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), h, p)
		@test trig("Image/Image Enhance/1 - Indexed and RGB") >= 1
		pump(10)
		@test any(s -> s.scene == h && s.srcname == "band2", values(IG._ENHSTATE))
		@test ccall(IG._fn(:gmtvtk_remove_image_h), Cint, (Ptr{Cvoid}, Cstring), h, "band2") == 1
		pump(5)
		@test !haskey(get(IG._IMG_ORIG, h, Dict{String,IG.GMTimage}()), "band2")
		@test !any(s -> s.scene == h && s.srcname == "band2", values(IG._ENHSTATE))
		@test !any(t -> t[2] == "band2", get(IG._SCENE_OBJS, h, []))
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
		pump(5)
	end
	# The base: promoted into an empty window, it lives in the window's figure; its removal leaves an
	# empty launcher, registered as one. Production fires the forget callback from sceneRemoveSurface
	# BEFORE the actors go; the test DLL's copy of that callback is unset, so it is fired here, in order.
	fig2 = IG.viz()
	h2 = getfield(fig2, :h)
	try
		pump(20)
		IG._drop_into(h2, mk(2), "base"; promote = true);  pump(10)
		@test IG._FIGREG[h2] isa IG.QtImage
		@test unsafe_string(ccall(IG._fn(:gmtvtk_base_name_h), Cstring, (Ptr{Cvoid},), h2)) == "base"
		kind, nm = "image", "base"
		GC.@preserve kind nm IG._on_forget(h2, Base.unsafe_convert(Cstring, kind), Base.unsafe_convert(Cstring, nm))
		ccall(_test_fn(:gmtvtk_scene_remove_surface_test), Cint, (Ptr{Cvoid},), h2)
		pump(5)
		@test IG._FIGREG[h2] isa IG.QtEmpty
		@test !haskey(get(IG._IMG_ORIG, h2, Dict{String,IG.GMTimage}()), "base")
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h2)
		pump(5)
	end
end

# "Auto histogram stretch" REPLACES the image in place, under its own name — no new row.
@testitem "Auto histogram stretch replaces the image in place" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	pump(n) = for _ in 1:n  IG._pump_once()  end
	nx, ny = 120, 80
	for T in (UInt16, UInt8)
		A = cat((T[clamp(round(Int, (T == UInt8 ? 60 : 7000) + 40k + 20randn()), 0, typemax(T)) for _ in 1:ny, _ in 1:nx]
		         for k in 0:2)...; dims = 3)
		fig = IG.iview_image_obj(IG.GMT.mat2img(A; x = [0.0, Float64(nx)], y = [0.0, Float64(ny)], noconv = true), "rgb")
		h = getfield(fig, :h)
		try
			pump(20)
			IG._drop_into(h, IG.GMT.mat2img(A; x = [0.0, Float64(nx)], y = [0.0, Float64(ny)], noconv = true), "extra";
			              promote = false)
			pump(5)
			nimg() = count(t -> t[1] === :image, IG._SCENE_OBJS[h])
			n0 = nimg()
			I0 = IG._find_object(h, :image, "extra")
			px0 = copy(I0.image)
			req = "image;extra"
			GC.@preserve req IG._on_img_stretch(h, Base.unsafe_convert(Cstring, req))
			pump(5)
			@test nimg() == n0                                         # no new image row
			@test !any(t -> occursin("stretched", t[2]), IG._SCENE_OBJS[h])
			I1 = IG._find_object(h, :image, "extra")
			@test I1 === I0                                            # the same object, rewritten
			@test eltype(I1.image) == UInt8 && I1.image != px0         # its pixels stretched
		finally
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
			pump(5)
		end
	end
end
