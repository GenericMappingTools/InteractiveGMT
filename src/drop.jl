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
# The String method is the ONE open-a-file-into-a-window path: window drop, File > Open,
# File > Recent Files (all via the Cstring wrapper) AND the desktop-icon drop (iview_app.jl,
# which opens an empty launcher and routes the file here) — never a second build path.
_on_drop(scene::Ptr{Cvoid}, cpath::Cstring)::Cvoid = _on_drop(scene, unsafe_string(cpath))
function _on_drop(scene::Ptr{Cvoid}, path::AbstractString)::Cvoid
	try
		# Already shown in a live window -> raise that window and ignore the duplicate drop.
		_open_window_for(path) != C_NULL && return
		empty = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
		# Cube files (3-D netCDF/grd) are NEVER read whole here -- a header-only probe decides
		# the layer count, then the slider dialog pulls one slice at a time (see _on_3d_cube_dropped).
		n_layers, zmin, zmax = _cube_probe(path)
		if n_layers > 1
			_on_3d_cube_dropped(scene, path, basename(path), empty, n_layers, zmin, zmax)
		else
			data = GMT.gmtread(path)
			_record_recent(path, data)                         # remember it in File > Recent Files
			_drop_into(scene, data, basename(path); promote=empty, source=path)
		end
		# Promoting the empty launcher makes this file the window's primary content -> retitle it.
		# A drop into an already-populated window just adds an extra layer, so its title is left alone.
		empty && ccall(_fn(:gmtvtk_set_title_h), Cvoid, (Ptr{Cvoid}, Cstring), scene, "i'GMT -- $(basename(path))")
		_mark_file_open(path, scene)                           # remember it so a re-drop is ignored
	catch e
		_viewer_log_error(scene, "Open '$(basename(path))' FAILED: $(sprint(showerror, e))")
		@warn "drop: could not read/open file" path exception=e
	end
	return
end

# Dispatch the dropped object by type into the window `scene`. `promote` reuses the empty launcher.
# `source` is the on-disk file path (threaded to Save Session so the layer is stored as a file ref,
# not serialized data); empty for non-file callers.
_drop_into(scene::Ptr{Cvoid}, G::GMTgrid,  name; promote=false, source="") = _add_grid_to_scene(scene, G, name; promote, source)

# ── 3D cube support: one layer on disk at a time, never the whole cube in memory ──────────────
# A dropped cube is NEVER read with a plain `GMT.gmtread(path)` (that would materialize every
# layer). Instead `_cube_probe` reads the file HEADER ONLY (GMT.grdinfo -Q, GMT's own native
# cube reader -- no data) for the layer count and the whole cube's z-range, and the slider dialog
# pulls exactly ONE 2-D slice per move straight off disk via `GMT.gmtread(path, layer=k)`.
# `_CUBE_INFO` remembers the probe (path/name/promote/source/z-range) per scene; `_CUBE_LOADED`
# tracks whether a slice is already showing so later slider moves REPLACE it in place
# (`_apply_host_grid!`, the same in-place swap transplant.jl uses) instead of stacking a new grid
# per step.
#
# `_cube_probe` used to detect cube-ness via a GDAL raster-band count (`Gdal.nraster`) instead of
# `grdinfo -Q`, specifically to avoid a try/catch on every dropped .nc/.grd file. PROVEN WRONG:
# GDAL opened on the bare filename (no `NETCDF:"file":varname` subdataset selector) only ever
# reports the file's FIRST 2-D slice as "Band 1" for a genuine multi-layer COARDS cube -- verified
# live (`Gdal.nraster` returned 1 on a real 6-layer test cube). `grdinfo -Q` uses GMT's own native
# cube reader, not GDAL, and is the mechanism GMT.jl's cube support is actually built on --
# correctness comes first; the try/catch this needs is a one-time cost per dropped file, not a
# hot loop, so it was never a real problem to begin with.
const _CUBE_INFO = Dict{Ptr{Cvoid}, @NamedTuple{path::String, name::String, promote::Bool, source::String, zmin::Float64, zmax::Float64, n_layers::Int}}()
const _CUBE_LOADED = Dict{Ptr{Cvoid}, Bool}()
# The slice currently shown per scene: (layer index, its grid, its cmap). Lets the "global min/max"
# checkbox rescale the COLOUR in place (no disk read, no surface rebuild) when the layer is unchanged.
const _CUBE_CUR = Dict{Ptr{Cvoid}, Any}()
# Optional whole-cube-in-RAM cache (the "Load all in RAM" button). When present, a layer switch
# slices this in memory (zero-copy `unsafe_wrap` view of the k-th slab) instead of a per-layer disk
# read -- ~1.4 s off disk vs ~instant from RAM for a large cube. Held for the scene's life; cleared
# when a new cube is dropped into the same window or the window dies.
const _CUBE_RAM = Dict{Ptr{Cvoid}, GMTgrid}()

# Header-only probe: (n_layers, zmin, zmax) for a netCDF/grd cube -- n_layers=0 if not a cube (or
# the header can't be read as one). ONE `grdinfo -Q` call gives both the layer count and the
# whole cube's z-range together (no data read either way).
function _cube_probe(path::String)::Tuple{Int,Float64,Float64}
	ext = lowercase(splitext(path)[2])
	(ext != ".nc" && ext != ".grd") && return (0, 0.0, 0.0)
	local info
	try
		info = GMT.grdinfo(path, C=true, Q=true)
	catch
		return (0, 0.0, 0.0)   # not a cube (or unreadable) -- falls through to the plain-grid path
	end
	inl = findfirst(==("n_layers"), info.colnames)
	iz1 = findfirst(==("z_min"), info.colnames)
	iz2 = findfirst(==("z_max"), info.colnames)
	(inl === nothing || iz1 === nothing || iz2 === nothing) && return (0, 0.0, 0.0)
	n = Int(info.data[inl])
	return n > 1 ? (n, Float64(info.data[iz1]), Float64(info.data[iz2])) : (0, 0.0, 0.0)
end

# Handle a dropped cube file: remember (path, n_layers, global z-range), show the non-modal
# slider dialog. No grid is read here -- the first slice is pulled lazily once the dialog fires
# its initial layer.
function _on_3d_cube_dropped(scene::Ptr{Cvoid}, path::String, name::AbstractString, promote::Bool, n_layers::Int, zmin::Float64, zmax::Float64)
	_CUBE_INFO[scene] = (path=path, name=String(name), promote=promote, source=path, zmin=zmin, zmax=zmax, n_layers=n_layers)
	_CUBE_LOADED[scene] = false
	delete!(_CUBE_RAM, scene)   # a fresh cube is not in RAM (the dock button re-enables to match)
	delete!(_CUBE_CUR, scene)
	# Pin the vertical axes to the WHOLE cube's z-range BEFORE the dialog builds layer 1, so the axis
	# box + Z labels stay put as the user switches layers (each layer's own min/max differs).
	ccall(_fn(:gmtvtk_set_cube_axes_zrange), Cvoid, (Ptr{Cvoid}, Cdouble, Cdouble), scene, zmin, zmax)
	ccall(_fn(:gmtvtk_show_cube_layer_dialog), Cvoid,
		(Ptr{Cvoid}, Cstring, Cint), scene, name, n_layers)
end

# Callback: the slider/spinbox moved to `layer_index` (0-based), or the "global min/max" checkbox
# was toggled (`use_global` != 0). Two very different costs:
#   * checkbox toggle (SAME layer already shown) -> rescale the COLOUR in place via gmtvtk_set_cpt:
#     no disk read, no surface rebuild, one Render. This is what made the checkbox feel instant
#     instead of the old full re-read + geometry rebuild, and it actually moves the colorbar (the
#     old path let gmtvtk_replace_base_grid recompute the colorbar range from the slice's own data,
#     so "global" silently did nothing).
#   * layer change (or first load) -> read that ONE slice off disk, add/replace the surface, cache
#     it, then apply the colour scaling.
function _on_load_cube_layer(scene::Ptr{Cvoid}, layer_index::Cint, use_global::Cint)::Cvoid
	try
		info = get(_CUBE_INFO, scene, nothing)
		info === nothing && (@warn "No cube info stored for scene"; return)

		layer_i = Int(layer_index) + 1                     # 0-based (C/UI) -> 1-based (Julia)
		layer_name = info.name                             # stable name so the base grid updates in place
		cur = get(_CUBE_CUR, scene, nothing)
		loaded = get(_CUBE_LOADED, scene, false)
		# The Shading dock picks the algorithm: flat shaded image (fast) OR a real 3-D surface with a
		# surface look (Cast shadows / Hillshade). Read the selection and render accordingly.
		flat = ccall(_fn(:gmtvtk_cube_flat_mode), Cint, (Ptr{Cvoid},), scene) != 0

		# Same layer already resident -> reuse the cached slice (no re-read) for a colour toggle OR an
		# algorithm switch (flat image <-> surface); only the render path differs.
		if cur !== nothing && cur.layer == layer_i && loaded
			chosen = use_global != 0 ? cur.glob : cur.loc
			zr     = use_global != 0 ? (info.zmin, info.zmax) : nothing
			if flat
				_show_cube_layer_image!(scene, cur.G, layer_name, chosen)   # cheap texture repaint
			else
				_apply_host_grid!(scene, cur.G, layer_name; zrange=zr)      # real 3-D surface
			end
			_CUBE_CUR[scene] = (layer=layer_i, G=cur.G, cmap=cur.cmap, loc=cur.loc, glob=cur.glob, flat=flat)
			_cube_push_cpt(scene, chosen)
			_mark_cube(scene, layer_index, use_global)
			return
		end

		# Fetch the slice: a zero-copy RAM slab ("Load all in RAM") or a lazy one-layer disk read.
		ram = get(_CUBE_RAM, scene, nothing)
		Gk  = ram !== nothing ? _cube_layer_view(ram, layer_i) : GMT.gmtread(info.path, layer=layer_i)
		Gk === nothing && (@warn "Failed to read cube layer $layer_i from $(info.path)"; return)
		cmap = _default_cmap(Gk)
		# Precompute BOTH CPTs for this slice (the only makecpt cost, paid once per layer read): its own
		# [zmin,zmax] and the whole cube's global range. Toggling the checkbox then swaps between them.
		loc    = _cpt_nodes(Gk, cmap)
		glob   = _cpt_nodes_range(info.zmin, info.zmax, cmap)
		chosen = use_global != 0 ? glob : loc
		first  = !loaded
		zr     = use_global != 0 ? (info.zmin, info.zmax) : nothing
		if flat
			# Fast illuminated image (flat textured plane), NOT a warped 3-D surface.
			_show_cube_layer_image!(scene, Gk, layer_name, chosen; first=first, source=info.source)
		elseif first
			_add_grid_to_scene(scene, Gk, layer_name; promote=info.promote, source=info.source, zrange=zr)
		else
			_apply_host_grid!(scene, Gk, layer_name; zrange=zr)   # real 3-D surface, in-place swap
		end
		if first
			_CUBE_LOADED[scene] = true
			ram === nothing && _record_recent(info.path, Gk)
		end
		_CUBE_CUR[scene] = (layer=layer_i, G=Gk, cmap=cmap, loc=loc, glob=glob, flat=flat)
		_cube_push_cpt(scene, chosen)
		_mark_cube(scene, layer_index, use_global)
	catch e
		@error "Failed to load cube layer" exception=(e, catch_backtrace())
	end
	return
end

# Tell the viewer this window is showing cube layer `layer_index` (0-based) with the given colour-range
# choice, so the Shading dock can re-render THIS layer when the user switches shading algorithm.
_mark_cube(scene::Ptr{Cvoid}, layer_index::Cint, use_global::Cint) =
	ccall(_fn(:gmtvtk_mark_cube), Cvoid, (Ptr{Cvoid}, Cint, Cint), scene, layer_index, use_global)

# Render a cube layer as a fast ILLUMINATED IMAGE (flat quad + hillshade texture) instead of a
# warped 3-D surface (gmtvtk_show_layer_image_h). Keeps the full-res z in the viewer for the
# coordinate readout and keeps the colorbar. `nodes` = (cz, crgb, ncolor) is the CPT to bake with
# (its range drives the colorbar too). On the first layer it also does the Save / Session / CRS
# bookkeeping that `_add_grid_to_scene` would. Returns true on success.
function _show_cube_layer_image!(scene::Ptr{Cvoid}, G::GMTgrid, name::String, nodes;
                                 first::Bool=false, source::String="")
	cz, crgb, ncolor = nodes
	z = eltype(G.z) === Float32 ? G.z : Float32.(G.z)
	ny, nx = size(z); r = G.range
	geog = _isgeographic(G)
	ok = ccall(_fn(:gmtvtk_show_layer_image_h), Cint,
		(Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		 Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cstring),
		scene, z, Cint(nx), Cint(ny), r[1], r[2], r[3], r[4], Cint(geog),
		cz, crgb, Cint(ncolor), name)
	ok == 0 && (@warn "cube layer image: the viewer rejected the update (window closed?)"; return false)
	if first
		_remember_object!(scene, :grid, name, G)                       # File>Save / Scene Objects "Save…"
		_session_record!(scene, :basegrid, isempty(source) ? :generated : :file, source; name=name)
		crs = crs_from(G; geographic=geog)
		hascrs(crs) && ccall(_fn(:gmtvtk_set_crs), Cvoid, (Ptr{Cvoid}, Cstring, Cstring, Cint),
		                     scene, crs.proj4, crs.wkt, Cint(crs.epsg))   # reveal the Geography menu if referenced
		_register_fig!(QtFigure(scene, G))                             # re-register the promoted launcher as a grid fig
	end
	return true
end

# Push a precomputed CPT (cz, crgb, n) IN PLACE onto the shared colour transfer function the
# surface/tiles/colorbar all share -- one ccall + Render, no geometry touched. gmtvtk_set_cpt also
# retargets the colorbar legend to this CPT's range (the whole point of the global-min/max option).
function _cube_push_cpt(scene::Ptr{Cvoid}, nodes)
	cz, crgb, n = nodes
	n < 2 && return
	ccall(_fn(:gmtvtk_set_cpt), Cvoid, (Ptr{Cvoid}, Ptr{Float64}, Ptr{Float64}, Cint),
	      scene, cz, crgb, Cint(n))
	return
end

# Zero-copy 2-D slice of a whole-cube GMTgrid held in RAM: the k-th slab is contiguous (last
# dimension), so `unsafe_wrap` hands back a real `Matrix{Float32}` that ALIASES the cube's memory
# -- no data copy. The parent cube stays rooted in `_CUBE_RAM`, so the aliased buffer is valid for
# as long as this scene keeps the cube. The z-range is left [0,0] here; `_cpt_nodes` rescans the
# slice for the colour range, and `_apply_host_grid!` only uses the x/y range.
function _cube_layer_view(C::GMTgrid, k::Int)::GMTgrid
	nrows, ncols = size(C.z, 1), size(C.z, 2)
	off = (k - 1) * nrows * ncols + 1
	zk  = unsafe_wrap(Array, pointer(C.z, off), (nrows, ncols))   # aliases cube memory, no copy
	inc2 = length(C.inc) >= 2 ? C.inc[1:2] : copy(C.inc)
	return GMT.GMTgrid(; proj4=C.proj4, wkt=C.wkt, epsg=C.epsg, geog=C.geog,
		range=Float64[C.range[1], C.range[2], C.range[3], C.range[4], 0.0, 0.0],
		inc=inc2, registration=C.registration, nodata=C.nodata,
		x=C.x, y=C.y, z=zk, layout=C.layout, cpt=C.cpt)
end

# "Load all in RAM" button callback. Read the WHOLE cube into memory so later layer switches slice
# it in place (see `_cube_layer_view`) instead of reading each slab off disk. Refuses (return 1) if
# it would not fit in free RAM. Returns: 0 = loaded, 1 = not enough RAM, 2 = error.
function _on_cube_load_all(scene::Ptr{Cvoid})::Cint
	try
		info = get(_CUBE_INFO, scene, nothing)
		info === nothing && return Cint(2)
		haskey(_CUBE_RAM, scene) && return Cint(0)          # already resident

		# Bytes per layer from the currently shown slice (already in hand) or a one-layer probe.
		cur = get(_CUBE_CUR, scene, nothing)
		nbytes_layer = if cur !== nothing && cur.G isa GMTgrid
			length(cur.G.z) * sizeof(eltype(cur.G.z))
		else
			g1 = GMT.gmtread(info.path, layer=1)
			g1 === nothing && return Cint(2)
			length(g1.z) * sizeof(eltype(g1.z))
		end
		need = Float64(nbytes_layer) * info.n_layers
		# Keep a 20% headroom over the raw cube size (transient read buffers + working set).
		(need * 1.2 > Float64(Sys.free_memory())) && return Cint(1)

		C = GMT.gmtread(info.path, layers=:all)
		(C isa GMTgrid && ndims(C.z) == 3) || return Cint(2)
		_CUBE_RAM[scene] = C
		return Cint(0)
	catch e
		@error "Cube load-all failed" exception=(e, catch_backtrace())
		return Cint(2)
	end
end

# Register the cube layer callbacks (lazy registration): the per-layer slider callback and the
# "Load all in RAM" button callback.
function _register_cube_callback()
	ccall(_fn(:gmtvtk_set_cube_layer_callback), Cvoid,
		(Ptr{Cvoid},), @cfunction(_on_load_cube_layer, Cvoid, (Ptr{Cvoid}, Cint, Cint)))
	ccall(_fn(:gmtvtk_set_cube_loadall_callback), Cvoid,
		(Ptr{Cvoid},), @cfunction(_on_cube_load_all, Cint, (Ptr{Cvoid},)))
end
_drop_into(scene::Ptr{Cvoid}, I::GMTimage, name; promote=false, source="") = _add_image_to_scene(scene, I, name; promote, source)
function _drop_into(scene::Ptr{Cvoid}, D::GMTdataset, name; promote=false, source="")
	promote ? _promote_dataset(scene, D, name) : _add_dataset_to_scene(scene, D, name)
end
function _drop_into(scene::Ptr{Cvoid}, D::Vector{<:GMTdataset}, name; promote=false, source="")
	promote ? _promote_dataset(scene, D, name) : _add_dataset_to_scene(scene, D, name)
end
_drop_into(scene::Ptr{Cvoid}, x, name; promote=false, source="") = @warn "drop: unsupported data type" type=typeof(x)

# Promote the bare launcher IN PLACE into a framed map over the dataset's extent, then add the
# vector as an overlay — the imported line / polyline / points / polygon lands in THIS iGMT window,
# never spawns the standalone X,Y plot tool or a second window. Same proven empty-launcher pattern
# basemap.jl / iview_image_obj use: promote a HIDDEN imageOnly scaffold (real framed axes + coord
# readout + flat-2-D view, no Scene Objects row, no colorbar), then hang the real object on top.
function _promote_dataset(scene::Ptr{Cvoid}, D, name)
	W, E, S, N = _dataset_bbox(D)
	dx = E - W; dy = N - S
	px = dx == 0 ? 1.0 : 0.05dx; py = dy == 0 ? 1.0 : 0.05dy   # 5 % pad (1.0 for a degenerate axis)
	W -= px; E += px; S -= py; N += py
	d1   = D isa AbstractVector ? D[1] : D
	geog = _isgeog(d1) == 1
	zblank = zeros(Float32, 2, 2)
	ccall(_fn(:gmtvtk_promote_surface_h), Cint,
	      (Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
	       Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cstring),
	      scene, zblank, Cint(2), Cint(2), W, E, S, N, Cint(geog ? 1 : 0),
	      C_NULL, C_NULL, Cint(0), C_NULL, Cint(0), Cint(0), Cint(0), Cint(1), "")
	ccall(_fn(:gmtvtk_hide_surface), Cvoid, (Ptr{Cvoid},), scene)   # scaffold plane only
	if geog
		crs = crs_from(d1; geographic=true)
		hascrs(crs) && ccall(_fn(:gmtvtk_set_crs), Cvoid, (Ptr{Cvoid}, Cstring, Cstring, Cint),
		                     scene, crs.proj4, crs.wkt, Cint(crs.epsg))   # reveals Geography menu
	end
	_add_dataset_to_scene(scene, D, name)                          # overlay -> stays in this window
	return true
end

# (W, E, S, N) extent of a GMTdataset (single or multi-segment) from its x/y columns.
function _dataset_bbox(D)
	segs = (D isa GMTdataset || D isa AbstractMatrix) ? (D,) : collect(D)
	W = Inf; E = -Inf; S = Inf; N = -Inf
	for seg in segs
		m = seg isa GMTdataset ? seg.data : seg
		x = @view m[:, 1]; y = @view m[:, 2]
		W = min(W, minimum(x)); E = max(E, maximum(x))
		S = min(S, minimum(y)); N = max(N, maximum(y))
	end
	return Float64(W), Float64(E), Float64(S), Float64(N)
end

# Add a dropped grid as a CPT-coloured surface in the window. On the empty launcher `promote`
# reconfigures THAT window in place (gmtvtk_promote_surface_h); otherwise it is added as an extra.
function _add_grid_to_scene(scene::Ptr{Cvoid}, G::GMTgrid, name; cmap=:auto, color=nothing, promote=false, source="", record=true, zrange=nothing)
	# `ny, nx = size(z)` on a 3-D array does NOT error in Julia -- it silently drops the third
	# dimension and proceeds with truncated data, no error anywhere. A cube must never reach here
	# (the cube-detection probe in _on_drop is supposed to intercept it first); fail loudly if it
	# ever does, instead of silently rendering wrong/truncated data with no visible error.
	ndims(G.z) == 3 && error("_add_grid_to_scene got a 3-D cube grid ($(size(G.z))) -- cube detection missed it upstream")
	cmap === :auto && (cmap = _default_cmap(G))   # geo only for topo/bathymetry grids, else turbo
	z = eltype(G.z) === Float32 ? G.z : Float32.(G.z); ny, nx = size(z); r = G.range
	# `color` (r,g,b in 0..1) forces a SOLID-colour 2-node CPT (used by the flat zero nested grids, whose
	# all-equal z would otherwise collapse _cpt_nodes to the viewer's blue ramp). `zrange` (zmn,zmx)
	# overrides the CPT's own autoscale (used by the cube layer slider's "global min/max" checkbox
	# to scale every slice off the whole cube's range instead of each slice's own narrower one).
	cz, crgb, ncolor = color !== nothing ? ([-1.0, 1.0], Float64[color[1], color[2], color[3], color[1], color[2], color[3]], 2) :
		zrange !== nothing ? _cpt_nodes_range(zrange[1], zrange[2], cmap) : _cpt_nodes(G, cmap)
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
	# Save Session: known file path -> store a file ref (:file); no path -> serialize the grid (:generated).
	# `record=false` suppresses this when a higher-level tool logs its own (menu) recipe (e.g. basemap).
	(ok != 0 && record) && _session_record!(scene, promote ? :basegrid : :dropgrid,
	                            isempty(source) ? :generated : :file, source; name=String(name))
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
function _add_image_to_scene(scene::Ptr{Cvoid}, I::GMTimage, name; promote=false, source="", record=true)
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
	# Save Session: known file path -> file ref (:file); no path -> serialize the image (:generated).
	# `record=false` suppresses this when a higher-level tool logs its own (menu) recipe (e.g. basemap).
	(ok != 0 && record) && _session_record!(scene, promote ? :image : :dropimage,
	                            isempty(source) ? :generated : :file, source; name=String(name))
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
#
# A dropped x,y table draws as a LINE / polyline by default — only data whose stored geometry is
# EXPLICITLY point (WKB point code) starts as points. A plain table (`gmtread` leaves geom unknown)
# is a line; the user flips it to a scatter via the overlay's "Convert to points" menu item. This is
# deliberately NOT `_ds_kind` (whose unknown-geometry heuristic guesses :points) — that heuristic
# still drives the front-door `iview(D)` routing, just not the drop-overlay default.
function _add_dataset_to_scene(scene::Ptr{Cvoid}, D, name)
	d1   = D isa AbstractVector ? first(D) : D
	mode = _geom_kind(Int(d1.geom)) === :points ? :points : :lines
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
