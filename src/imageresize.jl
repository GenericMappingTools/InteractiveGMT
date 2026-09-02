# Image > Image resize — the compute half of Mirone's src_figs/imageresize.m.
#
# The dialog (deps/src/70_window.cpp `ImageResizeDialog`) does only Photoshop's bookkeeping — pixels
# vs percent, document size at a resolution, constrain proportions — and hands over a target size in
# PIXELS plus a resampling method. Every pixel is resampled by GDAL, through GMT.jl's `gdalwarp`:
# `-ts <w> <h> -r <method>`. Mirone resamples with cvlib_mex('resize') = OpenCV; the methods line up
# one to one, "Area Relation" (OpenCV INTER_AREA, "pixel area relation") being gdalwarp's `average`
# — both define the output pixel as the mean of the source pixels its footprint covers.

# gdalwarp reports the warped extent grid-registered and drops half a cell at each edge, so the
# result would sit slightly inside its parent and read as a different area. The resized image covers
# EXACTLY the source's ground extent — only its cell size changed — so the georef is rebuilt from the
# source range and the new pixel counts (memory: an image's georef is range + x + y + inc +
# registration, never `.range` alone).
function _resize_fix_georef!(J::GMTimage, I::GMTimage, w::Int, h::Int)
	x0, x1, y0, y1 = I.range[1], I.range[2], I.range[3], I.range[4]
	J.range = [x0, x1, y0, y1, J.range[5], J.range[6]]
	J.inc   = [(x1 - x0) / w, (y1 - y0) / h]
	J.registration = 1                               # pixel registration: w cells span [x0,x1]
	J.x = collect(range(x0, x1; length = w + 1))
	J.y = collect(range(y0, y1; length = h + 1))
	J.proj4, J.wkt, J.epsg = I.proj4, I.wkt, I.epsg
	return J
end

# THE resize. Separate from the callback so a script (or a test) can resize without a dialog.
function _image_resize(I::GMTimage, w::Int, h::Int, method::String)
	(w < 1 || h < 1) && error("Resize: target size must be at least 1x1 (got $(w)x$(h))")
	J = GMT.gdalwarp(I, ["-ts", string(w), string(h), "-r", String(method)])
	return _resize_fix_georef!(J, I, w, h)
end

function _on_image_resize(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cint
	op = ""
	try
		p  = split(unsafe_string(cparams), ';')
		op = p[1]
		name = length(p) >= 2 ? String(strip(p[2])) : ""
		I = isempty(name) ? _find_object_named(scene, :image)[2] : _find_object(scene, :image, name)
		(I isa GMTimage) || error("Image resize: this window has no image named '$name'")
		if op == "init"
			# The dialog cannot measure the image itself (what it displays is padded to the window
			# bbox), so it asks. GMT images are stored row-major (layout "TRB"): dim 1 is then the
			# COLUMN count. Go through the georef instead, which says the same thing either way.
			w = length(I.x) > 1 ? length(I.x) - Int(I.registration == 0) : size(I.image, 2)
			h = length(I.y) > 1 ? length(I.y) - Int(I.registration == 0) : size(I.image, 1)
			ccall(_fn(:gmtvtk_resize_set_size), Cvoid, (Ptr{Cvoid}, Cint, Cint), dlg, Cint(w), Cint(h))
			return Cint(1)
		elseif op == "resize"
			w, h = parse(Int, strip(p[3])), parse(Int, strip(p[4]))
			method = String(strip(p[5]))
			J = _image_resize(I, w, h, method)
			# Derived-variable display law: a new named row, checked, the source unchecked, Scene
			# Objects unfolded — the same landing every derived image uses.
			_commit_derived_image!(scene, J,
			                       (isempty(name) ? "Image" : name) * " (resized $(w)x$(h))")
			return Cint(1)
		end
		error("Image resize: unknown op '$op'")
	catch e
		_tool_failed(scene, "Image resize ($op)", e)
		return Cint(0)
	end
end

function _register_image_resize()
	fptr = @cfunction((s, d, c) -> Base.invokelatest(_on_image_resize, s, d, c)::Cint, Cint,
	                  (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_image_resize_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
