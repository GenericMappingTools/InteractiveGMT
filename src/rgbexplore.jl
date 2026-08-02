# Image > Explore RGB — port of Mirone's mirone.m `Transfer_CB` 'RGBexp' (menu entry in
# mirone_uis.m), whose display half is utils/montage.m.
#
# Mirone splits the RGB image into THIRTEEN one-band components and shows them as a montage of
# thumbnails, each labelled in its own colour; clicking a thumbnail opens THAT component, at full
# resolution and carrying the parent's georeference, as a real image. The components, in Mirone's
# order (mirone.m lines 5777-5784) and with Mirone's labels:
#
#   1 Gray               cvlib_mex('color', img, 'rgb2gray')
#   2 Red     3 Green    4 Blue        the three bands themselves
#   5 Hue     6 Saturation  7 Value    cvlib_mex('color', img, 'rgb2hsv')
#   8 Luminance  9 Red Chrominance  10 Blue Chrominance    'rgb2YCrCb'  (Y, Cr, Cb)
#  11 L*a*b* => L*  12 => a*  13 => b*                     'rgb2lab'
#
# The conversions are NOT re-implemented here: GMT.jl already ships them (`rgb2gray`, `rgb2hsv`,
# `rgb2YCbCr`, `rgb2lab`, img_funs.jl / utils.jl) and they are the same transforms cvlib_mex (OpenCV)
# applies. The HSV triple is the one exception in packing only: GMT returns H in [0,360] and S/V in
# [0,1] as Float32, so it is scaled to 8 bits exactly the way OpenCV's 8-bit HSV does (H/2 -> 0..179,
# S and V *255) — same numbers Mirone displays.
#
# iGMT lands the clicked component where the user asked for it: as a NEW IMAGE IN THIS WINDOW's list
# (Scene Objects), not in a new window — through `_commit_derived_image!`, the one landing every
# derived image uses (new named row, checked, sources unchecked, Scene Objects unfolded).

const _RGBX_LABELS = ("Gray", "Red", "Green", "Blue", "Hue", "Saturation", "Value",
                      "Luminance", "Red Chrominance", "Blue Chrominance",
                      "L*a*b* => L*", "L*a*b* => a*", "L*a*b* => b*")

# Is this image something Explore RGB can split? Mirone's own test is `ndims(img) < 3` on the
# displayed CData, so an indexed or grey image is out (its menu entry is hidden for those).
_rgbx_is_rgb(I::GMTimage) = ndims(I.image) == 3 && size(I.image, 3) >= 3 && !_img_is_indexed(I)

# The parent as a plain BAND-PLANAR 3-band RGB image: what every component below is computed from.
# GMT's converters read band-planar and pixel-interleaved images alike, but a 4-band (RGBA) pixel-
# interleaved buffer would be walked with a stride of 3 — so de-interleave first and drop alpha,
# once, here, instead of each component having to know.
function _rgbx_rgb3(I::GMTimage)
	B = _to_band_planar(I)
	size(B.image, 3) == 3 && return B
	J = GMT.mat2img(B.image[:, :, 1:3], B)
	J.layout = B.layout
	return J
end

# ONE component, by Mirone's index (1..13). Same function for the montage thumbnails (fed a
# downsampled parent) and for the full-resolution image a click commits — the thumbnail is never a
# different calculation from the thing it previews.
function _rgbx_component(I::GMTimage, k::Int)::GMTimage
	(1 <= k <= 13) || error("Explore RGB: component $k does not exist (1..13)")
	B = _rgbx_rgb3(I)
	J = if k == 1
		GMT.rgb2gray(B)
	elseif k <= 4
		GMT.mat2img(B.image[:, :, k - 1], B)                      # 2 Red, 3 Green, 4 Blue
	elseif k <= 7
		HSV = GMT.rgb2hsv(B.image)                                # H in [0,360], S/V in [0,1]
		# OpenCV's 8-bit packing, the one cvlib_mex hands Mirone: hue halved to fit a byte.
		sc  = (k == 5) ? 0.5f0 : 255f0
		M   = Matrix{UInt8}(undef, size(HSV, 1), size(HSV, 2))
		c   = k - 4
		@inbounds for j = 1:size(HSV, 2), i = 1:size(HSV, 1)
			M[i, j] = round(UInt8, clamp(HSV[i, j, c] * sc, 0f0, 255f0))
		end
		GMT.mat2img(M, B)
	elseif k == 8
		GMT.rgb2YCbCr(B; Y = true)[1]                             # Luminance
	elseif k == 9
		GMT.rgb2YCbCr(B; Cr = true)[3]                            # Red Chrominance
	elseif k == 10
		GMT.rgb2YCbCr(B; Cb = true)[2]                            # Blue Chrominance
	elseif k == 11
		GMT.rgb2lab(B; L = true)[1]
	elseif k == 12
		GMT.rgb2lab(B; a = true)[2]
	else
		GMT.rgb2lab(B; b = true)[3]
	end
	J.layout = B.layout                    # a one-band result keeps the parent's row/column order
	# A component is ONE BAND of values — an INDEXED image, shown through the 256-level grey palette
	# Mirone displays it with (montage.m's figure colormap). Giving it that palette is what gives it a
	# COLOUR BAR: an indexed image's palette IS its legend (`_push_image_palette` -> discrete bar +
	# Color Bar row). Never `_img_drop_palette!` here — that would land the component bare, with
	# nothing on screen saying what its values are.
	return _img_gray_palette!(J)
end

# The montage thumbnails: all 13 components of a DOWNSAMPLED parent, packed back to back as RGBA
# rows (row 0 = south, the viewer's own convention — the C side flips them for screen). Downsampling
# first is what keeps this instant on a big image, and it goes through `_image_resize`, the same
# gdalwarp resampler Image resize uses. Returns (bytes, w, h, n).
function _rgbx_thumbs(I::GMTimage, maxside::Int = 180)
	w = length(I.x) > 1 ? length(I.x) - Int(I.registration == 0) : size(I.image, 2)
	h = length(I.y) > 1 ? length(I.y) - Int(I.registration == 0) : size(I.image, 1)
	f = max(w, h) / maxside
	Ismall = (f > 1) ? _image_resize(I, max(1, round(Int, w / f)), max(1, round(Int, h / f)),
	                                 "average") : I
	buf, tw, th = UInt8[], 0, 0
	for k = 1:13
		J = _rgbx_component(Ismall, k)
		jr = J.range
		px, nlon, nlat, _ = _drape_to_bbox(J, jr[1], jr[2], jr[3], jr[4]; outside=:transparent)
		if isempty(buf)
			tw, th = nlon, nlat
			buf = Vector{UInt8}(undef, 13 * tw * th * 4)
		end
		(nlon == tw && nlat == th) || error("Explore RGB: component $k came out $(nlon)x$(nlat), expected $(tw)x$(th)")
		copyto!(buf, (k - 1) * tw * th * 4 + 1, px, 1, tw * th * 4)
	end
	return buf, tw, th, 13
end

# C callback: params = "init;<name>" (build the montage) | "pick;<name>;<k>" (commit component k).
# "" = the window's primary image. Returns 1/0.
function _on_rgbexplore(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cint
	op = ""
	try
		p    = split(unsafe_string(cparams), ';')
		op   = String(p[1])
		name = length(p) >= 2 ? String(strip(p[2])) : ""
		I = isempty(name) ? _find_object_named(scene, :image)[2] : _find_object(scene, :image, name)
		(I isa GMTimage) || error("Explore RGB: this window has no image named '$name'")
		_rgbx_is_rgb(I) || error("Explore RGB: '$(isempty(name) ? "the image" : name)' is not an RGB image")
		if op == "init"
			buf, w, h, n = _rgbx_thumbs(I)
			ccall(_fn(:gmtvtk_rgbexp_set_thumbs), Cvoid,
			      (Ptr{Cvoid}, Ptr{Cuchar}, Cint, Cint, Cint), dlg, buf, Cint(w), Cint(h), Cint(n))
			return Cint(1)
		elseif op == "pick"
			k = parse(Int, strip(p[3]))
			J = _rgbx_component(I, k)
			# Derived-variable display law: a new named row in THIS window, checked, the parent
			# unchecked, Scene Objects unfolded — the landing every derived image uses.
			_commit_derived_image!(scene, J,
			                       (isempty(name) ? "Image" : name) * " ($(_RGBX_LABELS[k]))")
			return Cint(1)
		end
		error("Explore RGB: unknown op '$op'")
	catch e
		_viewer_log_error(scene, "Explore RGB ($op) FAILED: $(sprint(showerror, e))")
		@warn "Explore RGB FAILED" op exception=(e,)
		return Cint(0)
	end
end

function _register_rgbexplore()
	fptr = @cfunction((s, d, c) -> Base.invokelatest(_on_rgbexplore, s, d, c)::Cint, Cint,
	                  (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_rgbexplore_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
