# Image-drape texture packing + grid sampling. These turn a GMTimage into a VTK-ready RGBA
# buffer (south-first, west->east) and bilinearly sample a grid so (x,y)-only overlays drape
# onto the relief.

# Pack a GMTimage into a VTK texture buffer, honouring I.layout (mirrors GMTF3D's img_to_texbuf).
# layout char 2 = 'R' -> array is [lon,lat] (row-major); char 1 != 'B' -> first lat index is
# north. VTK texture origin is bottom-left, so output row 0 = south, west->east. Grey/2-band
# expand to RGB. Returns (buf, nlon, nlat, comps).
function _drape_buf(I)
	S   = I.image
	d3  = ndims(S) == 3
	nb  = d3 ? size(S, 3) : 1
	comps = nb >= 4 ? 4 : 3
	lay = I.layout
	rowmajor    = length(lay) >= 2 && lay[2] == 'R'   # 'R' -> array is [lon, lat]
	north_first = isempty(lay) || lay[1] != 'B'       # 'T' (default) -> lat index 1 is north
	nlon, nlat  = rowmajor ? (size(S, 1), size(S, 2)) : (size(S, 2), size(S, 1))
	pix(lat, lon, b) = d3 ? (rowmajor ? S[lon, lat, b] : S[lat, lon, b]) :
							(rowmajor ? S[lon, lat]    : S[lat, lon])
	buf = Vector{UInt8}(undef, nlat * nlon * comps)
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
	return buf, nlon, nlat, comps
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
	north_first = isempty(lay) || lay[1] != 'B'       # 'T' (default) -> lat index 1 is north
	nlon_i, nlat_i = rowmajor ? (size(S, 1), size(S, 2)) : (size(S, 2), size(S, 1))
	pix(lat, lon, b) = d3 ? (rowmajor ? S[lon, lat, b] : S[lat, lon, b]) :
							(rowmajor ? S[lon, lat]    : S[lat, lon])
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
	if outside !== :transparent
		fr, fg, fb = UInt8(fill[1]), UInt8(fill[2]), UInt8(fill[3])
		@inbounds for p in 0:(nlat*nlon - 1)
			buf[p*comps+1] = fr; buf[p*comps+2] = fg; buf[p*comps+3] = fb; buf[p*comps+4] = 0xff
		end
	end
	cdx = (gx1 - gx0) / nlon
	cdy = (gy1 - gy0) / nlat
	k = 1
	@inbounds for orow in 0:nlat-1                      # row 0 = SOUTH
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
	return buf, nlon, nlat, comps
end

# Bilinear sample of grid G at (x,y) -> z, so a 2-D GMTdataset (x,y only) overlay sits ON the
# surface. G.z is ny x nx (dim1 = y row, dim2 = x col), y ascending; node spacing from the data
# range. Out-of-range (x,y) clamps to the nearest edge cell.
function _sample_grid(G::GMTgrid, x::Real, y::Real)
	ny, nx = size(G.z)
	r = G.range
	dx = nx > 1 ? (r[2] - r[1]) / (nx - 1) : 1.0
	dy = ny > 1 ? (r[4] - r[3]) / (ny - 1) : 1.0
	fx = (x - r[1]) / dx;  fy = (y - r[3]) / dy
	i = clamp(floor(Int, fx), 0, nx - 2);  tx = clamp(fx - i, 0.0, 1.0)
	j = clamp(floor(Int, fy), 0, ny - 2);  ty = clamp(fy - j, 0.0, 1.0)
	z00 = G.z[j+1, i+1]; z10 = G.z[j+1, i+2]
	z01 = G.z[j+2, i+1]; z11 = G.z[j+2, i+2]
	return (1-tx)*(1-ty)*z00 + tx*(1-ty)*z10 + (1-tx)*ty*z01 + tx*ty*z11
end
