# savefile.jl — File > Save Grid / Save Image / Save Screenshot GeoTIFF  +  per-object "Save…" in
# the Scene Objects panel.
# The C++ side opens the format-picker dialog (SaveFormatDialog, 30_app.cpp) and hands
# "<kind>;<fmt>;<path>;<name>" to `_on_save`. We write a scene object — converting to the on-disk
# format the user picked:
#   grids  : netCDF (.nc/.grd) + Surfer 6 (.grd) via GMT.gmtwrite;
#            GeoTIFF / JPEG2000 / Erdas Imagine / ENVI via GMT.gdalwrite (driver from extension)
#   images : ALL formats via GMT.gdalwrite (GeoTIFF/JP2/Erdas/ENVI + generic jpg/png/tif/bmp)
#
# Save Screenshot GeoTIFF is a separate callback (`_on_save_geotiff`, gmtvtk_set_save_geotiff_callback):
# the C++ side hands over the captured RGB pixels directly in memory (no temp file) and we wrap them
# straight into a GMTimage via mat2img before the GDAL write.
#
# WHICH object is saved: every grid/image added to a window is remembered here (`_remember_object!`)
# keyed by the window's Scene* — primary surfaces (view_grid/view_image) AND extras (drops, basemap,
# tiles, iview_image_obj). `name` (the Scene Objects label) selects the exact object; an empty name
# (the File menu "save the window's grid/image") picks the first object of that kind = the primary.
# Falls back to the `_FIGREG` primary figure if the store has no match.
#
# As with the other menu callbacks, the @cfunction + its registration are RUNTIME values (created
# lazily on first window open via eventloop.jl `_ensure_callbacks`), never at module top level.

# Per-window store of saveable objects: scene (Scene*) -> [(kind, name, data), …] in add order
# (primary first). `data` is the live GMTgrid / GMTimage. Keyed by the opaque handle, so it leaks a
# tiny entry when a window closes (same minor pattern as basemap's _BASEMAP_LOADED) — pruning a
# closed Scene* is a follow-up.
const _SCENE_OBJS = Dict{Ptr{Cvoid}, Vector{Tuple{Symbol,String,Any}}}()

# Remember a grid/image just added to `scene` so File>Save / the Scene Objects "Save…" can write it.
# Returns `data` so it can be used inline. name="" is fine for an unnamed primary (lookup falls back
# to first-of-kind). Idempotent enough: a re-added (kind,name,data) just appends a duplicate — callers
# REPLACING a same-named object (e.g. recomputing "North component") must `_forget_object!` first.
function _remember_object!(scene::Ptr{Cvoid}, kind::Symbol, name, data)
	scene == C_NULL && return data
	v = get!(() -> Tuple{Symbol,String,Any}[], _SCENE_OBJS, scene)
	push!(v, (kind, name === nothing ? "" : String(name), data))
	return data
end

# Drop any remembered (kind,name) entries for `scene` — the Julia-side half of "replace, don't pile
# up" for a re-run under the same name (e.g. RTP3D recomputing "North component"): the C++ actor is
# removed via `gmtvtk_remove_grid_h`, and THIS drops the stale GMTgrid/GMTimage reference so nothing
# of the old result lingers in memory or in Save/"selected" lookups. No-op if nothing matches.
function _forget_object!(scene::Ptr{Cvoid}, kind::Symbol, name::AbstractString)
	v = get(_SCENE_OBJS, scene, nothing)
	v === nothing && return
	filter!(t -> !(t[1] === kind && t[2] == name), v)
	return
end

# Resolve the object to save: exact (kind,name) match first; else the first object of `kind` (the
# primary); else the _FIGREG primary figure. Returns the GMTgrid/GMTimage or nothing.
function _find_object(scene::Ptr{Cvoid}, kind::Symbol, name::AbstractString)
	v = get(_SCENE_OBJS, scene, nothing)
	if v !== nothing
		if !isempty(name)
			for (k, n, d) in v
				(k === kind && n == name) && return d
			end
		end
		for (k, n, d) in v
			k === kind && return d                       # first of kind = the primary
		end
	end
	fig = get(_FIGREG, scene, nothing)                   # fallback: the window's primary figure
	kind === :grid  && fig isa QtFigure && return fig.G
	kind === :image && fig isa QtImage  && return fig.I
	return nothing
end

# STRICT name lookup: returns the object only on an EXACT (kind,name) match, `nothing` otherwise —
# unlike _find_object above, this NEVER falls back to "the first/primary object of that kind". A
# caller resolving a SPECIFIC name (e.g. NSWING's Source/Nest fields, nswing.jl _nswing_grid_ref)
# needs that: a name that doesn't match a live scene grid must be treated as "not a scene grid" (so
# it falls through to being read as a raw file path), never silently swapped for an unrelated grid —
# _find_object's fallback picked the window's PRIMARY (bathymetry) grid for a Source/Nest miss, so a
# name typo/mismatch silently wrote the bathymetry into source.grd/layerN.grd instead of erroring.
function _find_object_exact(scene::Ptr{Cvoid}, kind::Symbol, name::AbstractString)
	isempty(name) && return nothing
	v = get(_SCENE_OBJS, scene, nothing)
	v === nothing && return nothing
	for (k, n, d) in v
		(k === kind && n == name) && return d
	end
	return nothing
end

# Canonical on-disk extension for each format code (kept in sync with the C++ dialog filters).
const _GRID_EXT = Dict("nc"=>".nc", "surfer"=>".grd", "gtiff"=>".tif",
					   "jp2"=>".jp2", "erdas"=>".img", "envi"=>".hdr")
const _IMG_EXT  = Dict("gtiff"=>".tif", "jp2"=>".jp2", "erdas"=>".img", "envi"=>".hdr",
					   "jpg"=>".jpg", "png"=>".png", "tif"=>".tif", "bmp"=>".bmp")

# Append the format's canonical extension if the chosen name carries no recognised one (QFileDialog
# usually appends the filter's default suffix, but the user can type a bare name).
function _ensure_ext(path::String, want::String, known)
	lc = lowercase(path)
	any(endswith(lc, e) for e in values(known)) ? String(path) : String(path) * want
end

# Write a grid: netCDF/Surfer via gmtwrite, everything else via GDAL (driver inferred from the ext).
function _save_grid(G, fmt::AbstractString, path::String)
	if (fmt == "nc")
		GMT.gmtwrite(path, G)                 # netCDF (GMT's default grid format)
	elseif (fmt == "surfer")
		GMT.gmtwrite(path * "=sf", G)         # Surfer 6 binary grid (GMT grid-format code =sf)
	else
		GMT.gdalwrite(path, G)                # GeoTIFF / JPEG2000 / Erdas(HFA) / ENVI — driver by ext
	end
end

# Images always go through GDAL (GeoTIFF/JP2/Erdas/ENVI + generic jpg/png/tif/bmp; driver by ext).
_save_image(I, ::String, path::String) = GMT.gdalwrite(path, I)

# C callback: req = "<kind>;<fmt>;<path>;<name>" (name optional/empty) for kind grid/image. Resolve
# the named scene object (or the primary), write it in the chosen format. Errors are reported in the
# Errors console.
function _on_save(scene::Ptr{Cvoid}, req::Cstring)::Cvoid
	kind = ""
	path = ""
	try
		s = unsafe_string(req)
		parts = split(s, ';', limit=4)
		length(parts) >= 3 || error("Save: malformed request '$s'")
		kind = strip(parts[1])
		fmt = strip(parts[2]); path = String(strip(parts[3]))
		name = length(parts) >= 4 ? String(strip(parts[4])) : ""
		isempty(path) && return
		if (kind == "grid")
			G = _find_object(scene, :grid, name)
			G === nothing && error("No grid to save in this window")
			path = _ensure_ext(path, get(_GRID_EXT, fmt, ".nc"), _GRID_EXT)
			_save_grid(G, fmt, path)
		elseif (kind == "image")
			I = _find_object(scene, :image, name)
			I === nothing && error("No image to save in this window")
			path = _ensure_ext(path, get(_IMG_EXT, fmt, ".tif"), _IMG_EXT)
			_save_image(I, fmt, path)
		else
			error("Save: unknown kind '$kind'")
		end
		_viewer_log_error(scene, "Saved $kind -> $path")
	catch e
		_viewer_log_error(scene, "Save $kind FAILED: $(sprint(showerror, e))")
		@warn "save: could not write file" exception=(e,)
	end
	return
end

# Build the C-callable pointer + register it. Lazy (first window) via _ensure_callbacks — the
# @cfunction is a thin invokelatest trampoline so it drags no GMT into compile.
function _register_save()
	fptr = @cfunction((s,c)->Base.invokelatest(_on_save,s,c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_save_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# C callback for File > Save Screenshot GeoTIFF: `rgb` is a packed row-major RGB buffer (top row
# first, w*h*3 bytes) straight out of VTK's frame grab — no temp file, no PNG encode/decode. Valid
# only for the duration of this call, so wrap-then-copy immediately (`permutedims` below both
# reorders VTK's (band,col,row) memory layout into the (row,col,band) `mat2img` expects AND makes
# the owned copy). Builds the GMTimage in memory and writes the real GeoTIFF via GDAL.
function _on_save_geotiff(scene::Ptr{Cvoid}, rgb::Ptr{UInt8}, w::Cint, h::Cint, path::Cstring,
						  x0::Cdouble, x1::Cdouble, y0::Cdouble, y1::Cdouble,
						  proj4::Cstring, wkt::Cstring)::Cvoid
	outpath = ""
	try
		outpath = unsafe_string(path)
		view = unsafe_wrap(Array, rgb, (3, Int(w), Int(h)))   # (band, col, row), C memory, borrowed
		mat = permutedims(view, (3, 2, 1))                    # (row, col, band), owned copy
		Iout = mat2img(mat; x=[x0, x1], y=[y0, y1], proj4=unsafe_string(proj4), wkt=unsafe_string(wkt))
		GMT.gdalwrite(outpath, Iout)
		_viewer_log_error(scene, "Saved geotiff -> $outpath")
	catch e
		_viewer_log_error(scene, "Save geotiff FAILED: $(sprint(showerror, e))")
		@warn "save: could not write screenshot GeoTIFF" exception=(e,)
	end
	return
end

# Build the C-callable pointer + register it. Lazy (first window) via _ensure_callbacks — the
# @cfunction is a thin invokelatest trampoline so it drags no GMT into compile.
function _register_save_geotiff()
	fptr = @cfunction((sc,rgb,w,h,p,x0,x1,y0,y1,pj,wk)->Base.invokelatest(_on_save_geotiff,sc,rgb,w,h,p,x0,x1,y0,y1,pj,wk),
					   Cvoid, (Ptr{Cvoid}, Ptr{UInt8}, Cint, Cint, Cstring, Cdouble, Cdouble, Cdouble, Cdouble, Cstring, Cstring))
	ccall(_fn(:gmtvtk_set_save_geotiff_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# C callback: req = "<kind>;<name>" (kind = "grid"). Resolve the live scene grid (same lookup as Save)
# and re-open it in a NEW iGMT window (view_grid). Return 1 on success so the C++ side then removes it
# from the source window (= a MOVE); 0 on any failure leaves the source untouched. Grids only for now.
function _on_move(scene::Ptr{Cvoid}, req::Cstring)::Cint
	kind = ""
	try
		parts = split(unsafe_string(req), ';', limit=2)
		kind = strip(parts[1])
		name = length(parts) >= 2 ? String(strip(parts[2])) : ""
		if kind == "grid"
			G = _find_object(scene, :grid, name)
			G === nothing && error("No grid to move in this window")
			fig = view_grid(G; title = isempty(name) ? "i'GMT" : name)
			# Carry the grid's NAME to the new window's base surface, so it keeps every name-driven
			# per-row option (a "layerN" blank grid keeps "Transplant 2nd grid…"). view_grid
			# opens the base unnamed ("Surface"); this relabels it without re-registering under the
			# name (base stays registered as "" — _on_nested_transplant's base/extra branch relies on that).
			isempty(name) || ccall(_fn(:gmtvtk_set_surface_name_h), Cvoid,
								   (Ptr{Cvoid}, Cstring), _fig_handle(fig), name)
		else
			error("Move: unknown kind '$kind'")
		end
		return Cint(1)
	catch e
		_viewer_log_error(scene, "Move to new window FAILED: $(sprint(showerror, e))")
		@warn "move: could not open new window" exception=(e,)
		return Cint(0)
	end
end

# Build the C-callable pointer + register it. Lazy (first window) via _ensure_callbacks — the
# @cfunction is a thin invokelatest trampoline so it drags no GMT into compile.
function _register_move()
	fptr = @cfunction((s,c)->Base.invokelatest(_on_move,s,c), Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_move_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# ── "Auto histogram stretch" image handle -> new percentile-stretched 8-bit image ────────────────
# A non-8-bit dropped image (e.g. a 16-bit satellite band) is shown as a fast linear MIN-MAX 8-bit
# copy (drop.jl `_stretch_to_u8`). Its full-precision original is stashed here, per window/name, so
# the row's "Auto histogram stretch" handle can build a NEW image with GMT's percentile histogram
# stretch (`mat2img(I; stretch=true)` -> find_histo_limits), which usually gives much better contrast
# than a full-range min-max map. Only populated for images that actually needed conversion.
const _IMG_ORIG = Dict{Ptr{Cvoid}, Dict{String, GMTimage}}()

_remember_img_orig!(scene::Ptr{Cvoid}, name::String, I::GMTimage) =
	(scene != C_NULL && (get!(() -> Dict{String,GMTimage}(), _IMG_ORIG, scene)[name] = I); I)

# C callback: req = "image;<name>". Look up the stashed full-precision source (fall back to the live
# scene image if none), histogram-stretch it to 8-bit, and add the result as a NEW image row in the
# same window. Errors go to the Errors console.
function _on_img_stretch(scene::Ptr{Cvoid}, req::Cstring)::Cvoid
	name = ""
	try
		parts = split(unsafe_string(req), ';', limit=2)
		name = length(parts) >= 2 ? String(strip(parts[2])) : ""
		d = get(_IMG_ORIG, scene, nothing)
		Isrc = (d === nothing) ? nothing : get(d, name, nothing)
		(Isrc === nothing) && (Isrc = _find_object(scene, :image, name))   # fall back to the live image
		(Isrc === nothing) && error("No source image named '$name' to stretch")
		# mat2img only histogram-stretches a UInt16 image (it returns an 8-bit one unchanged). So the
		# stretch is meaningful only when a full-precision source was stashed; on a plain 8-bit image
		# there is nothing to re-stretch — say so instead of adding an identical duplicate row.
		eltype(Isrc.image) != UInt16 &&
			(_viewer_log_error(scene, "Stretch: '$name' is already 8-bit (no wider-range source to stretch)"); return)
		Istr = GMT.mat2img(Isrc; stretch=true)           # percentile histogram stretch -> 8-bit
		newname = isempty(name) ? "stretched" : "$name (stretched)"
		_add_image_to_scene(scene, Istr, newname)        # new row in THIS window
		_viewer_log_error(scene, "Histogram-stretched image -> '$newname'")
	catch e
		_viewer_log_error(scene, "Stretch image FAILED: $(sprint(showerror, e))")
		@warn "img-stretch: could not build stretched image" exception=(e,)
	end
	return
end

function _register_img_stretch()
	fptr = @cfunction((s,c)->Base.invokelatest(_on_img_stretch,s,c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_img_stretch_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
