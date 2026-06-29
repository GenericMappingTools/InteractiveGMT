# File drag-and-drop. The C side calls `_on_drop` with the receiving window's Scene* handle and
# each dropped file's path; we read it (GMT.gmtread, which auto-detects a large range of formats)
# and add it INTO that window — a new surface/image actor or a line/point overlay, listed in the
# window's "Scene Objects" panel.
#
# The @cfunction pointer + its registration are RUNTIME values, created in __init__ (NOT at top
# level — a precompiled @cfunction is invalid), exactly like the in-window console callback.

# Called on the UI thread from inside the Qt pump when a file is dropped on a viewer window.
# An EMPTY launcher window (no primary surface) is PROMOTED IN PLACE: the SAME window is
# reconfigured into a full viewer (real scales + axes + colorbar + 3-D view + Scene Objects panel)
# — the window is reused, NOT replaced. A populated window gets the file ADDED into it (extra
# surface/image/overlay). `promote` is true exactly when the receiving window is the bare launcher.
function _on_drop(scene::Ptr{Cvoid}, cpath::Cstring)::Cvoid
	path = unsafe_string(cpath)
	try
		# Already shown in a live window -> raise that window and ignore the duplicate drop.
		_open_window_for(path) != C_NULL && return
		data  = GMT.gmtread(path)
		_record_recent(path, data)                             # remember it in File > Recent Files
		empty = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
		_drop_into(scene, data, basename(path); promote=empty)
		_mark_file_open(path, scene)                           # remember it so a re-drop is ignored
	catch e
		_viewer_log_error(scene, "Open '$(basename(path))' FAILED: $(sprint(showerror, e))")
		@warn "drop: could not read/open file" path exception=e
	end
	return
end

# Dispatch the dropped object by type into the window `scene`. `promote` reuses the empty launcher.
_drop_into(scene::Ptr{Cvoid}, G::GMTgrid,  name; promote=false) = _add_grid_to_scene(scene, G, name; promote)
_drop_into(scene::Ptr{Cvoid}, I::GMTimage, name; promote=false) = _add_image_to_scene(scene, I, name; promote)
function _drop_into(scene::Ptr{Cvoid}, D::GMTdataset, name; promote=false)
	promote ? _promote_dataset(scene, D) : _add_dataset_to_scene(scene, D, name)
end
function _drop_into(scene::Ptr{Cvoid}, D::Vector{<:GMTdataset}, name; promote=false)
	promote ? _promote_dataset(scene, D) : _add_dataset_to_scene(scene, D, name)
end
_drop_into(scene::Ptr{Cvoid}, x, name; promote=false) = @warn "drop: unsupported data type" type=typeof(x)

# A pure table has no surface to promote the launcher's scales onto. Until in-place dataset
# promotion exists, fall back to opening it in a fresh full window and retiring the launcher.
function _promote_dataset(scene::Ptr{Cvoid}, D)
	iview(D)
	ccall(_fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), scene)
end

# Add a dropped grid as a CPT-coloured surface in the window. On the empty launcher `promote`
# reconfigures THAT window in place (gmtvtk_promote_surface_h); otherwise it is added as an extra.
function _add_grid_to_scene(scene::Ptr{Cvoid}, G::GMTgrid, name; cmap=:auto, color=nothing, promote=false)
	cmap === :auto && (cmap = _default_cmap(G))   # geo only for topo/bathymetry grids, else turbo
	z = eltype(G.z) === Float32 ? G.z : Float32.(G.z); ny, nx = size(z); r = G.range
	# `color` (r,g,b in 0..1) forces a SOLID-colour 2-node CPT (used by the flat zero nested grids, whose
	# all-equal z would otherwise collapse _cpt_nodes to the viewer's blue ramp). Else build the CPT.
	cz, crgb, ncolor = color === nothing ? _cpt_nodes(G, cmap) :
		([-1.0, 1.0], Float64[color[1], color[2], color[3], color[1], color[2], color[3]], 2)
	# guessgeog (not isgeog): a plain lon/lat grid with no embedded proj still reads geographic via
	# the [-180 360 -90 90] range heuristic, so the Geography menu can be activated (see _apply_crs! below).
	geog = _isgeographic(G)
	fn = promote ? :gmtvtk_promote_surface_h : :gmtvtk_add_surface_h
	ok = promote ?
		ccall(_fn(fn), Cint,
		  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
		  scene, z, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4], Cint(geog),
		  cz, crgb, Cint(ncolor), C_NULL, Cint(0), Cint(0), Cint(0), Cint(0), String(name)) :
		ccall(_fn(fn), Cint,
		  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble,
		   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
		  scene, z, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4],
		  cz, crgb, Cint(ncolor), C_NULL, Cint(0), Cint(0), Cint(0), Cint(0), String(name))
	ok == 0 && @warn "drop: window is closed; grid not added"
	ok != 0 && _remember_object!(scene, :grid, name, G)   # Scene Objects "Save…" / File>Save can write it
	# Store the CRS + reveal the Geography menu if referenced (incl. guessed lon/lat -> WGS84).
	if ok != 0
		crs = crs_from(G; geographic=geog)
		hascrs(crs) && ccall(_fn(:gmtvtk_set_crs), Cvoid, (Ptr{Cvoid}, Cstring, Cstring, Cint),
		                     scene, crs.proj4, crs.wkt, Cint(crs.epsg))
	end
	# A promoted launcher reuses the SAME handle but its _FIGREG entry is still the QtEmpty launcher.
	# Re-register it as a grid figure so `fig` (console / colorbar _recolor) carries the grid + works.
	promote && ok != 0 && _register_fig!(QtFigure(scene, G))
	return ok != 0
end

# Add a dropped image as a flat textured plane in the window (promote = reuse the empty launcher).
function _add_image_to_scene(scene::Ptr{Cvoid}, I::GMTimage, name; promote=false)
	ir = I.range
	z = zeros(Float32, 2, 2)
	fillu = (UInt8(200), UInt8(200), UInt8(200))
	img, iw, ih, ibands = _drape_to_bbox(I, ir[1], ir[2], ir[3], ir[4]; outside=:transparent, fill=fillu)
	# guessgeog (not isgeog): a plain lon/lat image with no embedded proj still reads geographic via
	# the lon/lat-range heuristic, so geographic axes + the Geography menu activate (set_crs below).
	geog = try GMT.guessgeog(I) catch; GMT.isgeog(I) end
	fn = promote ? :gmtvtk_promote_surface_h : :gmtvtk_add_surface_h
	ok = promote ?
		ccall(_fn(fn), Cint,
		  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
		  scene, z, Cint(2), Cint(2), ir[1], ir[2], ir[3], ir[4], Cint(geog),
		  C_NULL, C_NULL, Cint(0), img, Cint(iw), Cint(ih), Cint(ibands), Cint(1), String(name)) :
		ccall(_fn(fn), Cint,
		  (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble,
		   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
		  scene, z, Cint(2), Cint(2), ir[1], ir[2], ir[3], ir[4],
		  C_NULL, C_NULL, Cint(0), img, Cint(iw), Cint(ih), Cint(ibands), Cint(1), String(name))
	ok == 0 && @warn "drop: window is closed; image not added"
	ok != 0 && _remember_object!(scene, :image, name, I)  # Scene Objects "Save…" / File>Save can write it
	# Store the CRS + reveal the Geography menu if referenced (incl. guessed lon/lat -> WGS84).
	if ok != 0
		crs = crs_from(I; geographic=geog)
		hascrs(crs) && ccall(_fn(:gmtvtk_set_crs), Cvoid, (Ptr{Cvoid}, Cstring, Cstring, Cint),
		                     scene, crs.proj4, crs.wkt, Cint(crs.epsg))
	end
	# As for grids: re-register the promoted launcher so `fig` is the actual image figure.
	promote && ok != 0 && _register_fig!(QtImage(scene, I))
	return ok != 0
end

# Open a NEW window showing a bare image as an **ExtraObj** (so it carries the Scene Objects
# properties / drape / stack / delete menu) instead of as an imageOnly surface. Mirrors the proven
# Base Map empty-launcher pattern: open an empty launcher, PROMOTE a HIDDEN blank base over the
# image bbox (lays down the framed axes + coord grid + coord readout + centred 2-D view), then add
# the image on top as an ExtraObj. Use this for tool-generated images (e.g. the Tiles Tool mosaic)
# that must be manageable in the Scene Objects panel — `view_image`/`iview(I)` makes an imageOnly
# surface with NO properties row and leaves the bare backing plane visible (a red panel under the
# tile). Returns the QtImage handle.
function iview_image_obj(I::GMTimage, name::AbstractString; title::String="i'GMT")
	h = ccall(_fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), title)
	h == C_NULL && error("iview_image_obj: could not open the window")
	ir   = I.range
	geog = _isgeog(I)
	zblank = zeros(Float32, 2, 2)
	# Hidden scaffold: image_only=1 (last Cint) so rebuildSceneObjects skips a row for it; img=NULL.
	ccall(_fn(:gmtvtk_promote_surface_h), Cint,
	      (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
	       Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
	      h, zblank, Cint(2), Cint(2), ir[1], ir[2], ir[3], ir[4], Cint(geog),
	      C_NULL, C_NULL, Cint(0), C_NULL, Cint(0), Cint(0), Cint(0), Cint(1), "")
	ccall(_fn(:gmtvtk_hide_surface), Cvoid, (Ptr{Cvoid},), h)   # plane is scaffold only
	_add_image_to_scene(h, I, name; promote=false)              # ExtraObj image -> has properties menu
	# Referenced image -> store the CRS (reveals the Geography menu + geographic axes).
	pr = (isdefined(I, :proj4) && I.proj4 isa AbstractString) ? I.proj4 : ""
	ep = (I.epsg isa Integer && I.epsg > 0) ? Cint(I.epsg) : Cint(0)
	(pr != "" || ep != 0) && ccall(_fn(:gmtvtk_set_crs), Cvoid, (Ptr{Cvoid}, Cstring, Cstring, Cint),
	                               h, pr, "", ep)
	fig = _register_fig!(QtImage(h, I))
	_start_pump()
	return fig
end

# Add a referenced image INTO an existing window, framing it: an EMPTY launcher gets a hidden
# scaffold plane over the image bbox (real framed axes + coord readout + flat-2-D view), a POPULATED
# window grows its frame to include the image; then the image is added on top as a managed ExtraObj
# image (properties / drape / stack / save menu). The image CRS is pushed so the axes read lon/lat
# and the Geography menu reveals. Mirrors basemap.jl's in-place path; used by the Tiles Tool to drop
# its mosaic into the calling window. `geographic` toggles geographic vs linear (e.g. mercator) axes.
function _place_image_in_window(scene::Ptr{Cvoid}, I::GMTimage, name; geographic::Bool=true)
	ir = I.range
	if ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
		zblank = zeros(Float32, 2, 2)
		ccall(_fn(:gmtvtk_promote_surface_h), Cint,
		      (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		       Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
		      scene, zblank, Cint(2), Cint(2), Float64(ir[1]), Float64(ir[2]), Float64(ir[3]), Float64(ir[4]),
		      Cint(geographic ? 1 : 0), C_NULL, C_NULL, Cint(0), C_NULL, Cint(0), Cint(0), Cint(0), Cint(1), "")
		ccall(_fn(:gmtvtk_hide_surface), Cvoid, (Ptr{Cvoid},), scene)   # scaffold only
	else
		# Already framed (grid or earlier image): extend the frame + axes + hover domain to include the
		# image (no-op if it already fits); keeps xfac fixed so existing actors stay aligned.
		ccall(_fn(:gmtvtk_grow_frame_h), Cint,
		      (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble),
		      scene, Float64(ir[1]), Float64(ir[2]), Float64(ir[3]), Float64(ir[4]))
	end
	_add_image_to_scene(scene, I, name; promote=false)             # ExtraObj image -> properties menu
	crs = crs_from(I; geographic=geographic)
	hascrs(crs) && ccall(_fn(:gmtvtk_set_crs), Cvoid, (Ptr{Cvoid}, Cstring, Cstring, Cint),
	                     scene, crs.proj4, crs.wkt, Cint(crs.epsg))   # referenced -> reveals Geography menu
	return
end

# Best-effort geographic flag for a dropped grid/image (lon/lat axis titles + cos-lat aspect).
function _isgeog(G)
	try
		return GMT.isgeog(G) ? 1 : 0
	catch
		return 0
	end
end

# Add a dropped dataset as a line/point overlay in the window. z comes from column 3 if present,
# else 0 (there is no host grid to drape on for an arbitrary dropped-on window).
function _add_dataset_to_scene(scene::Ptr{Cvoid}, D, name)
	mode = _ds_kind(D) === :points ? :points : :lines
	xyz, segoff, nseg, npts = _pack_dataset_flat(D)
	modei = mode === :lines ? Cint(1) : Cint(0)
	cr, cg, cb = _ovl_color(nothing, mode)
	lw = mode === :lines ? 2.0 : 0.0
	ps = mode === :points ? 6.0 : 0.0
	ok = ccall(_fn(:gmtvtk_add_overlay_h), Cint,
		  (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cstring),
		  scene, xyz, Cint(npts), segoff, Cint(nseg), modei, cr, cg, cb, lw, ps, String(name === nothing ? "" : name))
	ok == 0 && @warn "drop: window is closed; dataset not added"
	return ok != 0
end

# Pack a GMTdataset (single or multi-segment) into the C overlay layout, taking z from column 3
# when present else 0 (no host grid to sample). Mirrors grid.jl `_pack_dataset` but grid-free.
function _pack_dataset_flat(D)
	segs = (D isa GMTdataset || D isa AbstractMatrix) ? (D,) : collect(D)
	xyz = Float64[]; segoff = Cint[0]; off = 0
	for seg in segs
		m = seg isa GMTdataset ? seg.data : seg
		n, ncol = size(m, 1), size(m, 2)
		for k in 1:n
			x = Float64(m[k, 1]); y = Float64(m[k, 2])
			z = ncol >= 3 ? Float64(m[k, 3]) : 0.0
			push!(xyz, x, y, z)
		end
		off += n; push!(segoff, Cint(off))
	end
	return xyz, segoff, length(segs), off
end

# Build the C-callable pointer and install it in the DLL. Called once from __init__.
function _register_drop_callback()
	fptr = @cfunction((s,c)->Base.invokelatest(_on_drop,s,c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_drop_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

"""
	iview() -> QtEmpty

Open an empty viewer window that acts as a drag-and-drop launcher: drop a grid, image, or table
file (anything `GMT.gmtread` understands) onto it — or onto any open viewer window — and it is
added to that window and listed in its "Scene Objects" panel. Returns a `QtEmpty` handle.
"""
function iview()
	h = ccall(_fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "i'GMT  —  drop a file")
	h == C_NULL && error("iview: could not open the empty window")
	fig = _register_fig!(QtEmpty(h))
	_start_pump()
	return fig
end

# C callback for File > New Window. `scene` = the window the menu was clicked in (unused for now;
# kept for a future "open near / inherit from" behaviour). Opens a fresh empty launcher, tracked in
# _FIGREG — the registry that the planned inter-window data exchange will address windows through.
function _on_new_window(::Ptr{Cvoid})::Cvoid
	try
		iview()
	catch e
		@warn "New Window: could not open a window" exception=(e,)
	end
	return
end

# Build the C-callable pointer + register it. Called once from __init__ (via _ensure_callbacks).
function _register_new_window()
	fptr = @cfunction((s)->Base.invokelatest(_on_new_window, s), Cvoid, (Ptr{Cvoid},))
	ccall(_fn(:gmtvtk_set_newwindow_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
