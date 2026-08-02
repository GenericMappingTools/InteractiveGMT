# Image-drape texture packing + grid sampling. These turn a GMTimage into a VTK-ready RGBA
# buffer (south-first, west->east) and bilinearly sample a grid so (x,y)-only overlays drape
# onto the relief.

# A `pix(lat, lon, b)` accessor that reads band `b` of the (lat,lon) pixel from a GMTimage's raw
# array `S`, honouring the I.layout INTERLEAVE char (3rd: 'B' band-planar, 'P' pixel-interleaved):
#   - Band-planar (or default): the array is genuinely [lon,lat,band] / [lat,lon,band] -> index it.
#   - Pixel-interleaved ('...P'): gmtread wraps the BIP buffer (R,G,B,R,G,B…, band fastest) into a
#     nominal (n1,n2,nb) Array WITHOUT permuting, so [.,.,b] reads scrambled channels. Recover the
#     true pixels by viewing the band-fastest memory: reshape(S, nb, nlon, nlat)[b,lon,lat]
#     (rowmajor) / reshape(S, nb, nlat, nlon)[b,lat,lon] (colmajor). The row order (lay[1] T/B) and
#     the nlon/nlat dims are unchanged — only the channel indexing differs.
function _pixaccess(S, lay, d3::Bool, nb::Int, rowmajor::Bool, nlon::Int, nlat::Int)
	pixinter = d3 && length(lay) >= 3 && lay[3] == 'P'
	if pixinter
		P = rowmajor ? reshape(S, nb, nlon, nlat) : reshape(S, nb, nlat, nlon)
		return rowmajor ? ((lat, lon, b) -> @inbounds P[b, lon, lat]) : ((lat, lon, b) -> @inbounds P[b, lat, lon])
	end
	return d3 ? (rowmajor ? ((lat, lon, b) -> @inbounds S[lon, lat, b]) : ((lat, lon, b) -> @inbounds S[lat, lon, b])) :
				(rowmajor ? ((lat, lon, b) -> @inbounds S[lon, lat]) : ((lat, lon, b) -> @inbounds S[lat, lon]))
end

# --- INDEXED (palette) images -----------------------------------------------------------------
# An indexed image is ONE band of palette indices plus a colormap. It stays indexed everywhere it is
# stored, saved and re-read — only the VTK TEXTURE is expanded, because a texture has to be RGB(A).
# That expansion happens in exactly one place, `_pixaccess_img` below, so no caller has to know.
#
# GMT.jl's convention (utils_types.jl `cpt2cmap` / `ind2rgb`): `I.colormap` is a flat Vector{Int32}
# written COLUMN-wise — entry (index i, component c) is `colormap[i + (c-1)*n_colors + 1]` — with
# values 0..255 and `ncomp` 3 (RGB) or 4 (RGBA). `n_colors` is multiplied by 1000 as the "the
# palette carries real alpha" flag, so the true colour count has to be recovered from it.
_img_is_indexed(I::GMTimage) =
	ndims(I.image) == 2 && I.n_colors >= 2 && length(I.colormap) >= 6

# The palette as an (ncolors x ncomp) UInt8 table: row = pixel value + 1, columns R,G,B[,A].
function _img_palette(I::GMTimage)::Matrix{UInt8}
	n = I.n_colors >= 1000 ? div(I.n_colors, 1000) : I.n_colors   # undo cpt2cmap's alpha flag
	(n >= 2 && length(I.colormap) >= 3n) || error("this image's colormap is not a palette")
	ncomp = min(div(length(I.colormap), n), 4)
	L = Matrix{UInt8}(undef, n, ncomp)
	@inbounds for c = 1:ncomp, i = 1:n
		L[i, c] = UInt8(clamp(Int(I.colormap[i + (c - 1) * n]), 0, 255))
	end
	return L
end

# Build the palette of a NEW indexed image from an (ncolors x 3|4) UInt8 table. The inverse of
# `_img_palette`, and the ONE place a palette is written — every tool that produces an indexed
# result (K-means classification today) goes through it, never by hand.
#
# The stored form is GDAL's, the one GMT.jl both writes and reads: ALWAYS 256 entries, ALWAYS 4
# components, column-wise, `n_colors` = 256. Not an implementation detail — `gmt2gd` (GMT/src/
# gdal_utils.jl) reads every entry's alpha at `colormap[k + 3*n_colors]` with no length check, so a
# compact or 3-component palette is not a smaller palette, it is an out-of-bounds read the moment the
# image is saved ("BoundsError: attempt to access 12-element Vector{Int32} at index [13]").
# Unused entries stay black with alpha 255; `cpt2cmap` pads exactly the same way.
#
# Alpha lives in the 4th block, never behind `cpt2cmap`'s `n_colors *= 1000` flag: that flag would
# make `n_colors` the wrong stride for `gmt2gd`, which uses it as one.
function _img_set_palette!(I::GMTimage, L::Matrix{UInt8})
	n = size(L, 1)
	(1 <= n <= 256) || error("a palette holds 1..256 colours (got $n)")
	ncomp = size(L, 2)
	(ncomp == 3 || ncomp == 4) || error("a palette needs 3 (RGB) or 4 (RGBA) components")
	cm = zeros(Int32, 256 * 4)
	@inbounds for c = 1:3, i = 1:n
		cm[i + (c - 1) * 256] = Int32(clamp(Int(L[i, c]), 0, 255))
	end
	@inbounds for i = 1:256
		cm[i + 3 * 256] = (ncomp == 4 && i <= n) ? Int32(clamp(Int(L[i, 4]), 0, 255)) : Int32(255)
	end
	I.colormap = cm
	I.n_colors = 256
	I.color_interp = "Palette"
	return I
end

# The 256-level GREY palette, i.e. "this one-band image is INDEXED and its indices are shown as
# grey". Every single-band derived image that is a QUANTITY (a colour component, a channel) gets one:
# an indexed image carries a palette, and a palette IS that image's colour bar (`_push_image_palette`
# -> a discrete legend + a Color Bar row in its Scene Objects group). Without it the image lands with
# no bar at all and nothing says what its pixel values mean.
function _img_gray_palette!(I::GMTimage)
	L = Matrix{UInt8}(undef, 256, 3)
	@inbounds for c = 1:3, i = 1:256
		L[i, c] = UInt8(i - 1)
	end
	return _img_set_palette!(I, L)
end

# `mat2img(mat, I)` copies the parent's colormap along with its georef. That is right when the new
# matrix is a new set of INDICES into the same palette and wrong whenever it is grey or RGB data (a
# mask, a segmentation, a stretched band) — those pixels are values, not indices, and a leftover
# palette would repaint them. Derived-image builders call this to say "no palette".
function _img_drop_palette!(I::GMTimage)
	I.colormap = zeros(Int32, 3)
	I.n_colors = 0
	(ndims(I.image) == 2) && (I.color_interp = "Gray")
	return I
end

# Hand an indexed image's palette to the viewer, which turns it into that image's DISCRETE colour bar
# (one labelled block per pixel value) plus a Color Bar row in its Scene Objects group. Only the
# entries the image actually uses are sent: a stored palette is padded to 256, and a legend of 256
# blocks for a 4-class image would say nothing. Not indexed -> the legend is cleared, so an image row
# that stops being indexed never keeps a stale bar.
function _push_image_palette(scene::Ptr{Cvoid}, I::GMTimage, name::String)
	n, rgb = 0, UInt8[]
	if _img_is_indexed(I)
		L = _img_palette(I)
		n = clamp(Int(maximum(I.image)) + 1, 1, size(L, 1))     # highest value in use decides the count
		nc = min(size(L, 2), 3)
		rgb = Vector{UInt8}(undef, 3n)
		@inbounds for k = 1:n, b = 1:3
			rgb[3(k - 1) + b] = L[k, b <= nc ? b : nc]
		end
	end
	ccall(_fn(:gmtvtk_image_set_palette_h), Cvoid, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint),
	      scene, String(name), isempty(rgb) ? C_NULL : rgb, Cint(n))
	return
end

# The `pix(lat, lon, b)` accessor plus the band count to read it with, for a whole GMTimage — the
# palette-aware wrapper around `_pixaccess`. For an indexed image the accessor answers the PALETTE
# COLOUR of the index, so every consumer (both drape paths) draws the picture the image describes
# instead of the raw index numbers.
function _pixaccess_img(I::GMTimage)
	S   = I.image
	d3  = ndims(S) == 3
	nb  = d3 ? size(S, 3) : 1
	lay = I.layout
	rowmajor = length(lay) >= 2 && lay[2] == 'R'
	nlon, nlat = rowmajor ? (size(S, 1), size(S, 2)) : (size(S, 2), size(S, 1))
	pix = _pixaccess(S, lay, d3, nb, rowmajor, nlon, nlat)
	_img_is_indexed(I) || return pix, nb, nlon, nlat, rowmajor
	L  = _img_palette(I)
	nL, nc = size(L, 1), size(L, 2)
	lut = (lat, lon, b) -> @inbounds L[clamp(Int(pix(lat, lon, 1)) + 1, 1, nL), b <= nc ? b : nc]
	return lut, nc, nlon, nlat, rowmajor
end

# Does the array's first lat index hold the NORTH row? The layout 1st char (T/B) is the nominal
# answer ('T' -> north-first), BUT gmtread's disk image reader hands back the raw GDAL buffer
# UN-flipped — i.e. actually TOP-first (row 1 = north) — yet tags it 'B' (bottom-first). Verified:
# gmtread "BRBa"/"BRPa" is the SAME memory gdalread returns as "TRBa" (genuinely top-first). Those
# disk buffers are always ROW-MAJOR ('R', 2nd char), whether single-band ("BRBa"), grey, or
# pixel-interleaved RGB ("BRPa"); the 'B' is a mislabel. So ANY row-major image is north-first,
# regardless of the T/B char. Grid-derived images are COLUMN-MAJOR ('C', e.g. mat2img -> "BCBa")
# and are genuinely bottom-first, so those honour the nominal T/B label.
_north_first(lay, rowmajor::Bool) = rowmajor ? true : (isempty(lay) || lay[1] != 'B')

# De-interleave a PIXEL-INTERLEAVED image ('...P', what `gmtread` returns for any RGB raster read
# from disk: "BRPa") into a genuine band-planar array ('...B'), leaving everything else untouched.
#
# The drape/texture path reads such an image correctly (see `_pixaccess`: it recovers the true pixels
# by viewing the band-fastest memory). GMT/GDAL entry points do NOT — a row-major image sends
# `GMT.crop` down its `gdaltranslate` branch (GMT/src/crop.jl), and the in-memory GDAL wrapper reads
# the buffer as if it were band-planar, so an RGB disk image comes back as colour noise (measured:
# correlation ~0 against the true pixels; a de-interleaved copy of the SAME image gives 1.0). Anything
# handing a GMTimage to GMT/GDAL must pass it through here first.
function _to_band_planar(I::GMTimage)
	lay = I.layout
	S   = I.image
	(ndims(S) == 3 && length(lay) >= 3 && lay[3] == 'P') || return I
	nb = size(S, 3)
	# Band-fastest memory, whatever the row/column-major char is (the SAME view `_pixaccess` takes):
	# P[band, dim1, dim2] -> permute to the band-planar (dim1, dim2, band) the nominal shape claims.
	P  = reshape(S, nb, size(S, 1), size(S, 2))
	I2 = GMT.mat2img(permutedims(P, (2, 3, 1)), I)
	I2.layout = lay[1:2] * "B" * lay[4:end]
	return I2
end

# Give an image a COMPLETE, self-consistent georeference over [W,E]x[S,N]: range, x/y coordinate
# vectors, increment and registration, all agreeing with the pixel dims.
#
# Setting `I.range` ALONE is not enough and silently breaks everything downstream. The viewer's own
# drape path places an image from `I.range`, so a range-only patch LOOKS right on screen — but every
# GMT/GDAL entry point reads `I.x`/`I.y`/`I.inc` instead (`GMT.crop` -> `axes2pix`/`gdaltranslate`),
# and those still held the pixel coordinates the image was born with. The Base Map tile was the live
# repro: range said -100/-20/-60/20 while x still ran 1..1200 with inc=1, so cropping a 30x30-degree
# rectangle out of it returned a 31x31-PIXEL scrap (one pixel per "unit" of a coordinate system that
# no longer existed) instead of the 450x450 crop the data holds.
#
# Pixel registration (`reg=1`, x/y carrying nx+1 / ny+1 node boundaries) is what `gmtread` itself
# reports for a disk image, so this matches how a genuinely-referenced image arrives.
function _georef_image!(I::GMTimage, W, E, S, N)
	lay = I.layout
	rowmajor = length(lay) >= 2 && lay[2] == 'R'
	nx, ny = rowmajor ? (size(I.image, 1), size(I.image, 2)) : (size(I.image, 2), size(I.image, 1))
	I.registration = 1
	I.inc   = [(E - W) / nx, (N - S) / ny]
	I.x     = collect(range(Float64(W), Float64(E); length = nx + 1))
	I.y     = collect(range(Float64(S), Float64(N); length = ny + 1))
	I.range = [Float64(W), Float64(E), Float64(S), Float64(N), I.range[5], I.range[6]]
	return I
end

# Pack a GMTimage into a VTK texture buffer, honouring I.layout (mirrors GMTF3D's img_to_texbuf).
# layout char 2 = 'R' -> array is [lon,lat] (row-major); see _north_first for the lat direction.
# VTK texture origin is bottom-left, so output row 0 = south, west->east. Grey/2-band expand to
# RGB. Returns (buf, nlon, nlat, comps).
function _drape_buf(I)
	# _pixaccess_img, not _pixaccess: an INDEXED image answers with its palette colours here (see
	# there), so `nb` is the palette's component count and everything below is unchanged.
	pix, nb, nlon, nlat, rowmajor = _pixaccess_img(I)
	comps = nb >= 4 ? 4 : 3
	north_first = _north_first(I.layout, rowmajor)
	buf = Vector{UInt8}(undef, nlat * nlon * comps)
	_db_fill!(buf, pix, nlon, nlat, comps, nb, north_first)
	return buf, nlon, nlat, comps
end

# Function barrier for _drape_buf's hot loop (see the identical note on `_dtb_fill!` below):
# `pix`'s CONCRETE closure type depends on a runtime branch in `_pixaccess`, so the caller can't
# infer it -> calling it inline there is a dynamic dispatch PER PIXEL. Passing `pix` as an
# argument here lets Julia specialize this whole function on its actual (concrete) type.
function _db_fill!(buf, pix, nlon, nlat, comps, nb, north_first)
	k = 1
	@inbounds for orow in 0:nlat-1               # texture row 0 = SOUTH
		lat = north_first ? (nlat - orow) : (orow + 1)
		for lon in 1:nlon                        # west -> east
			buf[k]   = pix(lat, lon, 1)
			buf[k+1] = nb >= 2 ? pix(lat, lon, 2) : pix(lat, lon, 1)
			buf[k+2] = nb >= 3 ? pix(lat, lon, 3) : pix(lat, lon, 1)
			comps == 4 && (buf[k+3] = pix(lat, lon, 4))
			k += comps
		end
	end
	return nothing
end

# Place a GMTimage onto a canvas covering the FULL grid bbox [gx0,gx1]×[gy0,gy1], at the image's
# increment, RGBA with alpha 0 (transparent) everywhere the image does NOT reach. Mirrors
# GMTF3D's drape_to_bbox: the grid is NOT cropped; only the grid ∩ image overlap carries the
# picture, the rest stays transparent so the CPT-coloured base surface shows through. Output is
# C-ready: row 0 = SOUTH, west->east, comps = 4.
function _drape_to_bbox(I, gx0, gx1, gy0, gy1; outside::Symbol=:shademesh, fill=(200,200,200))
	# Palette-aware accessor (see `_pixaccess_img`): an indexed image draws its colours, not indices.
	pix, nb, nlon_i, nlat_i, rowmajor = _pixaccess_img(I)
	north_first = _north_first(I.layout, rowmajor)
	ir = I.range
	ix0, ix1, iy0, iy1 = ir[1], ir[2], ir[3], ir[4]
	dxi = (ix1 - ix0) / nlon_i                         # image increment
	dyi = (iy1 - iy0) / nlat_i
	nlon = clamp(round(Int, (gx1 - gx0) / dxi), 16, 8192)   # canvas spans the FULL grid bbox
	nlat = clamp(round(Int, (gy1 - gy0) / dyi), 16, 8192)
	comps = 4
	buf = zeros(UInt8, nlat * nlon * comps)            # alpha 0 => transparent outside image
	# GMTF3D `outside` mode for the grid area the image does NOT cover:
	#   :transparent       -> leave alpha 0 (CPT base shows through; original behaviour)
	#   :shade / :shademesh -> opaque `fill` grey (a flat shaded sheet). :shademesh adds
	#                          mesh edges viewer-side (the `edges` flag in view_grid).
	if (outside !== :transparent)
		fr, fg, fb = UInt8(fill[1]), UInt8(fill[2]), UInt8(fill[3])
		@inbounds for p in 0:(nlat*nlon - 1)
			buf[p*comps+1] = fr; buf[p*comps+2] = fg; buf[p*comps+3] = fb; buf[p*comps+4] = 0xff
		end
	end
	cdx = (gx1 - gx0) / nlon
	cdy = (gy1 - gy0) / nlat
	_dtb_fill!(buf, pix, nlon, nlat, comps, nb, north_first, gx0, gy0, cdx, cdy,
	           ix0, ix1, iy0, iy1, nlon_i, nlat_i)
	return buf, nlon, nlat, comps
end

# Function barrier for the hot loop above. MEASURED: `pix`'s concrete closure type depends on a
# runtime branch in `_pixaccess` (band-planar vs pixel-interleaved vs 2-D), so `_drape_to_bbox`
# can't infer it -> `pix::Function` (abstract) there -> every `pix(...)` call in the loop was a
# DYNAMIC DISPATCH. On the full etopo4 world image (5400x2700) that was ~2.6-3.1 s, EVERY call,
# not just the first (i.e. not a JIT-compile artifact — this is what actually made Base Map /
# Global seismicity feel frozen). Passing `pix` as an argument to a separate function lets Julia
# specialize THIS function on its actual (concrete) closure type -> plain inlined calls.
function _dtb_fill!(buf, pix, nlon, nlat, comps, nb, north_first, gx0, gy0, cdx, cdy,
                     ix0, ix1, iy0, iy1, nlon_i, nlat_i)
	k = 1
	@inbounds for orow in 0:nlat-1                     # row 0 = SOUTH
		yc  = gy0 + (orow + 0.5) * cdy
		inY = (yc >= iy0) && (yc <= iy1)
		for col in 1:nlon                              # west -> east
			xc = gx0 + (col - 0.5) * cdx
			if inY && xc >= ix0 && xc <= ix1           # inside the image footprint
				fx = (xc - ix0) / (ix1 - ix0)          # 0..1 west->east
				fy = (yc - iy0) / (iy1 - iy0)          # 0..1 south->north
				ilon   = clamp(floor(Int, fx * nlon_i) + 1, 1, nlon_i)
				ilat_s = clamp(floor(Int, fy * nlat_i) + 1, 1, nlat_i)   # 1 = south
				lat = north_first ? (nlat_i - ilat_s + 1) : ilat_s
				buf[k]   = pix(lat, ilon, 1)
				buf[k+1] = nb >= 2 ? pix(lat, ilon, 2) : pix(lat, ilon, 1)
				buf[k+2] = nb >= 3 ? pix(lat, ilon, 3) : pix(lat, ilon, 1)
				# Opaque where the image covers — UNLESS the image carries its own alpha band (an
				# RGBA drop, or Binarize's "Apply to original + Alpha" mask), which then rules.
				buf[k+3] = nb >= 4 ? pix(lat, ilon, 4) : 0xff
			end
			k += comps
		end
	end
	return nothing
end

# Bilinear sample of grid G at (x,y) -> z, so a 2-D GMTdataset (x,y only) overlay sits ON the
# surface. G.z is ny x nx (dim1 = y row, dim2 = x col), y ascending; node spacing from the data
# range. Out-of-range (x,y) clamps to the nearest edge cell.
function _sample_grid(G::GMTgrid, x::Real, y::Real)
	ny, nx = size(G.z)
	r = G.range
	#dx = nx > 1 ? (r[2] - r[1]) / (nx - 1) : 1.0
	dx = (r[2] - r[1]) / (nx - 1)
	#dy = ny > 1 ? (r[4] - r[3]) / (ny - 1) : 1.0
	dy = (r[4] - r[3]) / (ny - 1)
	fx = (x - r[1]) / dx;  fy = (y - r[3]) / dy
	i = clamp(floor(Int, fx), 0, nx - 2);  tx = clamp(fx - i, 0.0, 1.0)
	j = clamp(floor(Int, fy), 0, ny - 2);  ty = clamp(fy - j, 0.0, 1.0)
	z00 = G.z[j+1, i+1]; z10 = G.z[j+1, i+2]
	z01 = G.z[j+2, i+1]; z11 = G.z[j+2, i+2]
	return (1-tx)*(1-ty)*z00 + tx*(1-ty)*z10 + (1-tx)*ty*z01 + tx*ty*z11
end
