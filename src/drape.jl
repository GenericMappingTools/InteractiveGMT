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

# Does the array's first lat index hold the NORTH row? The layout 1st char (T/B) is the nominal
# answer ('T' -> north-first), BUT gmtread tags disk-read RGB images "BRPa" while GMT's get_image
# hands back the raw GDAL buffer UN-flipped, i.e. actually TOP-first (row 1 = north) — verified by
# matching gmtread row 1 against gdalread (TRBa, genuinely top-first). So the 'B' on a
# pixel-interleaved image is a mislabel; treat those as north-first. Band-planar images keep the
# nominal T/B rule (the previously-working path, left untouched).
_north_first(lay, pixinter::Bool) = pixinter ? true : (isempty(lay) || lay[1] != 'B')

# Pack a GMTimage into a VTK texture buffer, honouring I.layout (mirrors GMTF3D's img_to_texbuf).
# layout char 2 = 'R' -> array is [lon,lat] (row-major); see _north_first for the lat direction.
# VTK texture origin is bottom-left, so output row 0 = south, west->east. Grey/2-band expand to
# RGB. Returns (buf, nlon, nlat, comps).
function _drape_buf(I)
	S   = I.image
	d3  = ndims(S) == 3
	nb  = d3 ? size(S, 3) : 1
	comps = nb >= 4 ? 4 : 3
	lay = I.layout
	rowmajor    = length(lay) >= 2 && lay[2] == 'R'   # 'R' -> array is [lon, lat]
	pixinter    = d3 && length(lay) >= 3 && lay[3] == 'P'
	north_first = _north_first(lay, pixinter)
	nlon, nlat  = rowmajor ? (size(S, 1), size(S, 2)) : (size(S, 2), size(S, 1))
	pix = _pixaccess(S, lay, d3, nb, rowmajor, nlon, nlat)
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
	S  = I.image
	d3 = ndims(S) == 3
	nb = d3 ? size(S, 3) : 1
	lay = I.layout
	rowmajor    = length(lay) >= 2 && lay[2] == 'R'   # 'R' -> array is [lon, lat]
	pixinter    = d3 && length(lay) >= 3 && lay[3] == 'P'
	north_first = _north_first(lay, pixinter)
	nlon_i, nlat_i = rowmajor ? (size(S, 1), size(S, 2)) : (size(S, 2), size(S, 1))
	pix = _pixaccess(S, lay, d3, nb, rowmajor, nlon_i, nlat_i)
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
				buf[k+3] = 0xff                        # opaque where the image covers
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
