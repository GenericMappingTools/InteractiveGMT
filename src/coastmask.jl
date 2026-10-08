# coastmask.jl — a GSHHG coastline's right-click "Mask > Land / Ocean" (Mirone's coastline menu):
# mask the raster this window is showing with grdlandmask, at the SAME shoreline resolution the
# coastline was plotted with.
#
# What a masked node becomes depends on what the raster can hold:
#   float grid            -> NaN
#   integer grid / image  -> 0          (a UInt16 band has no NaN)
#   UInt8 image           -> white      (every colour band 255; an alpha band is left as it is)
#
# The result is a NEW derived element (SACRED_LAW.md derived-variable display law): `_adopt_derived!`,
# the source unchecked.

# Called by the coastline's menu (55_lineprops.cpp) through the console eval — a command, nothing
# comes back. `res` = the coastline's GSHHG resolution letter, `land` = mask land (else ocean),
# `kind` = "grid"/"image" and `name` = the target raster's Scene Objects name ("" = the primary).
function _on_coast_mask(scene::Ptr{Cvoid}, res::String, land::Bool, kind::String, name::String)
	try
		k   = kind == "image" ? :image : :grid
		src = isempty(name) ? _find_object(scene, k, "") : _find_object_exact(scene, k, name)
		src === nothing && error("cannot find the $kind \"$name\" this window is showing")
		R   = src isa GMTgrid ? _coast_mask_grid(src, res, land) : _coast_mask_image(src, res, land)
		base  = isempty(name) ? (src isa GMTgrid ? "Grid" : "Image") : name
		title = base * (land ? " (land masked)" : " (ocean masked)")
		if R isa GMTgrid
			# REPLACE, never pile up: masking twice the same way trashes the previous result first.
			ccall(_fn(:gmtvtk_remove_grid_h), Cint, (Ptr{Cvoid}, Cstring), scene, title)
			_forget_object!(scene, :grid, title)
			has_surface = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene)
			_add_grid_to_scene(scene, R, title; promote = (has_surface == 0)) ||
				error("window closed, grid not added")
		else
			ccall(_fn(:gmtvtk_remove_image_h), Cint, (Ptr{Cvoid}, Cstring), scene, title)
			_forget_object!(scene, :image, title)
			_add_image_to_scene(scene, R, title; promote = false)
		end
		_adopt_derived!(scene, title, R)             # the ONE derived-variable transition (grid.jl)
	catch e
		_tool_failed(scene, "Mask", e)
	end
	return nothing
end

# THE land/ocean mask on a raster's own nodes: an (ny,nx) matrix, row 1 = south, NaN on the masked
# side. `hdr` is the raster (grid or image) whose region, spacing, registration and CRS it follows.
#
# Geographic raster: grdlandmask is asked for that region, spacing AND registration directly.
# GMT.jl's grdlandmask(G) is not used for it: it passes -R/-I but not -r, so a pixel-registered
# raster gets a gridline mask one node larger in each direction.
# Projected raster: only GMT.jl's grdlandmask(G) knows how to bring the shoreline mask into the
# raster's projection, so it is handed a 1-filled stand-in grid of the raster's geometry.
function _coast_landmask(hdr, nx::Int, ny::Int, res::String, land::Bool)
	mv  = land ? "1/NaN" : "NaN/1"                   # -N wet/dry
	prj = GMT.getproj(hdr, proj4 = true)
	geog = prj == "" || contains(prj, "=lon") || contains(prj, "=lat")
	if geog
		M = GMT.grdlandmask(R = join(string.(Float64.(hdr.range[1:4])), '/'),
		                    I = join(string.(Float64.(hdr.inc[1:2])), '/'),
		                    registration = (hdr.registration == 1 ? :pixel : :gridline),
		                    res = Symbol(res), maskvalues = mv)
	else
		Gd = GMT.mat2grid(ones(Float32, ny, nx), hdr)
		Gd.layout = "BCB"
		M = GMT.grdlandmask(Gd; res = Symbol(res), maskvalues = mv)
	end
	Z = _zmat(M)
	size(Z) == (ny, nx) ||
		error("the land mask came back $(size(Z, 2))x$(size(Z, 1)), the raster is $(nx)x$(ny)")
	return Z
end

# A grid: a copy, masked where it lies through `_zmat` (no transposition, any layout). A float grid
# takes NaN; an integer one cannot, so its masked nodes are 0.
function _coast_mask_grid(G::GMTgrid, res::String, land::Bool)::GMTgrid
	nx, ny = _grid_dims(G)
	Z  = _coast_landmask(G, nx, ny, res, land)
	G2 = deepcopy(G)
	Zg = _zmat(G2)
	T  = eltype(G2.z)
	v0 = T <: AbstractFloat ? T(NaN) : zero(T)
	@inbounds for ix in 1:nx, iy in 1:ny
		isnan(Z[iy, ix]) && (Zg[iy, ix] = v0)
	end
	if T <: AbstractFloat
		G2.hasnans = 2
		zmn, zmx = GMT.extrema_nan(G2.z)
	else
		zmn, zmx = extrema(G2.z)
	end
	length(G2.range) >= 6 && (G2.range[5] = zmn; G2.range[6] = zmx)
	return G2
end

# An image: masked pixel by pixel through the same row-order rule the texture packing uses
# (`_north_first`, drape.jl). A palette image is expanded to RGB first — a white that is not in its
# palette cannot be painted into it.
function _coast_mask_image(I::GMTimage, res::String, land::Bool)::GMTimage
	J   = _img_is_indexed(I) ? GMT.ind2rgb(I) : I
	Ib  = _to_band_planar(J)
	Ib === J && (Ib = deepcopy(J))                   # never paint the source image's own buffer
	S   = Ib.image
	lay = Ib.layout
	rowmajor = length(lay) >= 2 && lay[2] == 'R'
	nx, ny = rowmajor ? (size(S, 1), size(S, 2)) : (size(S, 2), size(S, 1))
	nf  = _north_first(lay, rowmajor)
	Z   = _coast_landmask(J, nx, ny, res, land)
	nb   = ndims(S) == 3 ? size(S, 3) : 1
	ncol = nb >= 3 ? 3 : 1                           # colour bands; a 4th (alpha) band is untouched
	fill_v = eltype(S) == UInt8 ? 0xff : zero(eltype(S))
	npl  = nx * ny
	@inbounds for ix in 1:nx, iy in 1:ny
		isnan(Z[iy, ix]) || continue
		r = nf ? ny - iy + 1 : iy
		k = rowmajor ? ix + (r - 1) * nx : r + (ix - 1) * ny
		for b in 0:ncol-1
			S[k + b * npl] = fill_v
		end
	end
	return Ib
end
