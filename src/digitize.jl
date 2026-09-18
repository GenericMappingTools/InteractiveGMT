# digitize.jl — "Digitize whites": the boundary of a MASK's white region, traced by GDAL and drawn as
# a red line overlay.
#
# A MASK IS AN INTEGER IMAGE. Two states, "here" and "not here", one byte per node — never a 0/1
# grid: a grid is a field of numbers, with a z range, a colour scale and a colour bar, which is not
# what a mask is and not how one is stored, read or displayed here. Nothing in this file builds,
# accepts or produces a mask as a grid.
#
# Digitizing means recovering the OUTLINE of the white (1) region as vector geometry, which is
# exactly what `GDALPolygonize` answers: one pass over the raster, one OGR POLYGON per connected run
# of equal pixel values, outer rings and holes included — a topological object, which a hand-rolled
# edge walk or a contour of a smoothed copy is not. The source raster is the in-memory MEM dataset
# `GMT.gmt2gd` makes from the mask IMAGE itself (no file round-trip, no new dependency: GDAL already
# ships with GMT.jl, `GMT.libgdal`), and the rings come back through the raw OGR C API straight into
# the flat xyz/segoff pair the viewer's overlay export takes — never through `gd2gmt` and a
# Vector{GMTdataset} we would immediately flatten again.
#
# RED by default, never black: the line lies ON a black-and-white picture, and a black line on the
# mask's own blacks cannot be seen. An ordinary overlay otherwise — its pen stays editable through
# Line Properties like any other line.

const _DIGITIZE_COLOR = :red
const _DIGITIZE_WIDTH = 1.5

# Is this image a MASK? UInt8 AT MOST — a mask is a boolean thing, one byte per node is already the
# widest it may be — and exactly TWO states, both present (an all-zero array is empty, not a
# footprint). The values are flags, not a range: NSWING writes 0/1, a thresholded picture may carry
# 0/255, and a third distinct value means this is a picture, not a mask.
function _is_mask_image(I::GMTimage)::Bool
	(ndims(I.image) == 2 && eltype(I.image) == UInt8) || return false
	a = 0x00;  b = 0x00;  n = 0
	@inbounds for v in I.image
		(n >= 1 && v == a) && continue
		(n >= 2 && v == b) && continue
		n += 1
		n == 1 ? (a = v) : n == 2 ? (b = v) : return false
	end
	return n == 2
end

# The white (foreground) state of a mask: its HIGHER value — 1 for a boolean footprint, 255 for a
# thresholded picture. "Whites" is what the user sees; this is what it is in the bytes.
_mask_white(I::GMTimage)::Int = Int(maximum(I.image))

# Rings of every polygon whose pixel value is `want`, packed flat: (xyz, segoff, nseg, npts). One
# SEGMENT per ring — an outer boundary and each hole are separate closed lines, which is what they
# are on screen. z is 0 for every vertex: a mask boundary has no elevation (the caller says so with
# zIsPlaceholder, so no data table ever invents a Z column).
function _digitize_rings(I::GMTimage, want::Int)
	gdl  = GMT.Gdal
	dsr  = GMT.gmt2gd(_to_band_planar(I))    # MEM raster over the mask IMAGE — no copy to disk
	band = ccall((:GDALGetRasterBand, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid}, Cint), dsr.ptr, Cint(1))
	(band == C_NULL) && error("Digitize whites: could not wrap the mask as a GDAL raster")

	ds  = gdl.create(gdl.getdriver("MEM"))   # in-memory OGR sink for the traced polygons
	lyr = gdl.createlayer(name = "mask", dataset = ds, geom = gdl.wkbPolygon)
	gdl.addfielddefn!(lyr, "DN", gdl.OFTInteger)

	# hMaskBand = NULL: polygonize EVERY pixel, both states, and pick the wanted value off the DN
	# field below. Handing the band itself in as the mask would drop the 0s — and with them the
	# outline of any hole whose own value is 0 but which is not connected to the outside.
	err = GC.@preserve dsr ds ccall((:GDALPolygonize, GMT.libgdal), Cint,
	          (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cvoid}, Cint, Ptr{Ptr{UInt8}}, Ptr{Cvoid}, Ptr{Cvoid}),
	          band, C_NULL, lyr.ptr, Cint(0), C_NULL, C_NULL, C_NULL)
	if err != 0
		msg = unsafe_string(ccall((:CPLGetLastErrorMsg, GMT.libgdal), Cstring, ()))
		error("Digitize whites: GDALPolygonize failed" * (isempty(msg) ? "" : " ($msg)"))
	end

	xyz = Float64[];  segoff = Cint[0];  tot = 0
	ccall((:OGR_L_ResetReading, GMT.libgdal), Cvoid, (Ptr{Cvoid},), lyr.ptr)
	while true
		f = ccall((:OGR_L_GetNextFeature, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid},), lyr.ptr)
		f == C_NULL && break
		try
			dn = Int(ccall((:OGR_F_GetFieldAsInteger, GMT.libgdal), Cint, (Ptr{Cvoid}, Cint), f, Cint(0)))
			dn == want || continue
			gp = ccall((:OGR_F_GetGeometryRef, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid},), f)
			gp == C_NULL && continue
			nring = Int(ccall((:OGR_G_GetGeometryCount, GMT.libgdal), Cint, (Ptr{Cvoid},), gp))
			for k in 0:(nring - 1)
				ring = ccall((:OGR_G_GetGeometryRef, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid}, Cint), gp, Cint(k))
				ring == C_NULL && continue
				n = Int(ccall((:OGR_G_GetPointCount, GMT.libgdal), Cint, (Ptr{Cvoid},), ring))
				n < 2 && continue
				xs = Vector{Float64}(undef, n);  ys = Vector{Float64}(undef, n)
				GC.@preserve xs ys ccall((:OGR_G_GetPoints, GMT.libgdal), Cint,
					(Ptr{Cvoid}, Ptr{Cvoid}, Cint, Ptr{Cvoid}, Cint, Ptr{Cvoid}, Cint),
					ring, xs, Cint(8), ys, Cint(8), C_NULL, Cint(0))
				@inbounds for i in 1:n
					push!(xyz, xs[i], ys[i], 0.0)
				end
				tot += n
				push!(segoff, Cint(tot))
			end
		finally
			ccall((:OGR_F_Destroy, GMT.libgdal), Cvoid, (Ptr{Cvoid},), f)
		end
	end
	GC.@preserve dsr ds nothing
	return xyz, segoff, length(segoff) - 1, tot
end

# The mask behind a handle. A mask is an IMAGE, so this resolves images only: a name that is not one
# is not a mask, and nothing here turns anything else into one.
function _digitize_source(scene::Ptr{Cvoid}, name::AbstractString)
	if !isempty(name)
		I = _find_object_exact(scene, :image, name)
		(I isa GMTimage) && return I
	end
	_, I = _find_object_named(scene, :image)
	(I isa GMTimage) && return I
	error("Digitize whites: this window has no mask image to digitize")
end

# A mask handle's "Digitize whites" (its Scene Objects row / its right-click menu, 50_scene.cpp via
# g_juliaEval). `name` = that handle's Scene Objects name ("" = the window's primary image).
function _digitize_whites(scene::Ptr{Cvoid}, name::AbstractString)
	I = _digitize_source(scene, name)
	_is_mask_image(I) || error("Digitize whites: '$(name)' is not a mask (a two-state integer image)")
	xyz, segoff, nseg, npts = _digitize_rings(I, _mask_white(I))
	(nseg == 0) && error("Digitize whites: this mask has no white region to digitize")
	r, g, b = _ovl_color(_DIGITIZE_COLOR, :lines)
	nm = (isempty(name) ? "Mask" : String(name)) * " boundary"
	# noConvertToPoints=1 (scattering an outline to points is meaningless), zIsPlaceholder=1 (the z
	# pushed above is a filler, so "Show data table…" shows only #/X/Y).
	ok = GC.@preserve xyz segoff ccall(_fn(:gmtvtk_add_overlay_ex2_h), Cint,
		(Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble,
		 Cdouble, Cdouble, Cstring, Cstring, Cstring, Cint, Cint),
		scene, xyz, Cint(npts), segoff, Cint(nseg), Cint(1), r, g, b,
		_DIGITIZE_WIDTH, 0.0, nm, nm, "", Cint(1), Cint(1))
	(ok == 0) && error("Digitize whites: could not draw the boundary (window closed?)")
	return nothing
end
