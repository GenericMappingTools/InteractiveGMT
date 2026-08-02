# Image > Flip > Up-Down / Left-Right — port of Mirone's mirone.m `Transfer_CB` 'flipUD'/'flipLR'
# (menu built in mirone_uis.m as a "Flip" submenu of the Image menu). Mirone is one line —
# `set(handles.hImg,'CData', flipdim(img,direction))` — and the point of it is what it does NOT do:
# it re-orders the pixels and touches nothing else. The image keeps its axes, so a REFERENCED image
# keeps its coordinates: a flip moves pixels inside the same ground extent, it does not move the
# extent. Here that means the GMTimage's `range` / `x` / `y` / `inc` / `registration` / CRS are left
# strictly alone and only `.image` is rewritten.
#
# In place, under the same name, like Mirone: a flip is not a new quantity (nothing is computed from
# the image), so it gets no new Scene Objects row — the same image is simply shown flipped, and the
# same flip applied twice puts it back.

# Flip the pixel array along its LATITUDE (up-down) or LONGITUDE (left-right) axis. Which array
# dimension that is depends on the GMT layout, so it is resolved here once, the same way the drape
# reader resolves it (`_pixaccess` / `_pixaccess_img`, drape.jl):
#   • row-major ("?R??", disk images): dim 1 = columns (lon), dim 2 = rows (lat)
#   • column-major ("?C??", mat2img):  dim 1 = rows (lat), dim 2 = columns (lon)
#   • pixel-interleaved ("??P?", what gmtread returns for an RGB raster): the bands are the FASTEST
#     axis in memory, so the lon/lat axes are one position further along — reshape to (band, d1, d2),
#     flip there, and reshape back. Reversing dim 1 or 2 of the 3-D array directly would shuffle the
#     colour bands of every pixel instead ("the flipped image came out in false colours").
# Whether row 1 holds north or south is irrelevant: flipping the whole latitude axis is up-down
# either way.
function _flip_pixels(S::Array{UInt8}, layout::String, updown::Bool)
	rowmajor = length(layout) >= 2 && layout[2] == 'R'
	pixinter = ndims(S) == 3 && length(layout) >= 3 && layout[3] == 'P'
	latdim, londim = rowmajor ? (2, 1) : (1, 2)
	dim = updown ? latdim : londim
	if pixinter
		P = reshape(S, size(S, 3), size(S, 1), size(S, 2))     # (band, dim1, dim2), band fastest
		return reshape(reverse(P; dims = dim + 1), size(S))
	end
	return reverse(S; dims = dim)
end

# THE flip. Mutates the image IN PLACE (the viewer's registry, Save… and Save Session all hold this
# very object, so they see the flip without any of them being told). The georef is untouched by
# construction — only `.image` is assigned. Separate from the callback so a script or a test can
# flip without a menu.
function _image_flip!(I::GMTimage, updown::Bool)
	I.image = _flip_pixels(I.image, I.layout, updown)
	return I
end

# C callback: req = "ud;<name>" | "lr;<name>" ("" = the window's primary image).
function _on_image_flip(scene::Ptr{Cvoid}, req::Cstring)::Cvoid
	op = ""
	try
		p    = split(unsafe_string(req), ';')
		op   = String(p[1])
		name = length(p) >= 2 ? String(strip(p[2])) : ""
		key, I = isempty(name) ? _find_object_named(scene, :image) :
		                         (name, _find_object(scene, :image, name))
		(I isa GMTimage) || error("Flip: this window has no image named '$name'")
		updown = (op == "ud")
		(updown || op == "lr") || error("Flip: unknown direction '$op'")
		_image_flip!(I, updown)
		# A 16-bit source stashed for "Auto histogram stretch (new image)" (savefile.jl `_IMG_ORIG`)
		# is the SAME picture at full precision, so it flips with the display — otherwise a stretch
		# taken after a flip would come back un-flipped. Same function, never a second flip.
		d = get(_IMG_ORIG, scene, nothing)
		if d !== nothing
			Iorig = get(d, key, nothing)
			(Iorig isa GMTimage) && (Iorig !== I) && _image_flip!(Iorig, updown)
		end
		# Re-upload the texture from the flipped pixels, through the SAME buffer builder every image
		# is drawn with (`_drape_to_bbox`, palette-aware, so an indexed image stays indexed).
		ir = I.range
		img, iw, ih, ibands = _drape_to_bbox(I, ir[1], ir[2], ir[3], ir[4]; outside=:transparent)
		ok = ccall(_fn(:gmtvtk_image_set_pixels_h), Cint,
		           (Ptr{Cvoid}, Cstring, Ptr{Cuchar}, Cint, Cint, Cint),
		           scene, name, img, Cint(iw), Cint(ih), Cint(ibands))
		ok == 0 && error("Flip: the viewer has no image named '$name' to redraw")
	catch e
		_viewer_log_error(scene, "Flip image ($op) FAILED: $(sprint(showerror, e))")
		@warn "Flip image FAILED" op exception=(e,)
	end
	return
end

function _register_image_flip()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_image_flip, s, c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_image_flip_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
