# view_grid: show a GMTgrid surface in the Qt + VTK viewer, plus the line/point overlay path
# (`add!` / the view_grid `data=` kwarg).

# ── "please wait" dialog for the FIRST grid of a session ──────────────────────────────────────────
# Opening the first grid takes a while — Julia compiles the read/view path and Qt+VTK build their
# first window — and until now that time passed with nothing at all on screen. One busy dialog covers
# it, put up by whichever path gets there first (view_grid, the file-open dispatch, a drop) and taken
# down when the window is up.
#
# ONLY the first one: later opens are fast, and a dialog that flashes for 200 ms on every open is
# worse than none. `_LOAD_DEPTH` makes the pair re-entrant — the outermost begin/end wins — so the
# nested calls (iview -> view_grid, drop -> view_grid) show one dialog, not two.
const _LOAD_DIALOG_DONE = Ref(false)
const _LOAD_DEPTH       = Ref(0)

function _load_dialog_begin(msg::AbstractString)
	_LOAD_DIALOG_DONE[] && return                      # not the first open: no dialog at all
	_LOAD_DEPTH[] += 1
	_LOAD_DEPTH[] == 1 || return                       # already showing (nested call)
	try
		ccall(_fn(:gmtvtk_progress_show_async), Cint, (Cint, Cstring), Cint(0), String(msg))
	catch
		_LOAD_DEPTH[] = 0                              # DLL not loaded / no display: carry on silently
	end
	return
end

function _load_dialog_end()
	_LOAD_DEPTH[] == 0 && return
	_LOAD_DEPTH[] -= 1
	_LOAD_DEPTH[] == 0 || return                       # an outer level is still running
	_LOAD_DIALOG_DONE[] = true                         # the slow first one is paid for; never again
	try
		ccall(_fn(:gmtvtk_progress_close), Cvoid, ())
	catch
	end
	return
end

# Flatten a GMTdataset (single or multi-segment Vector{GMTdataset}) into the C overlay layout:
# `xyz` = npts triples, `segoff` = nseg+1 segment offsets. z comes from column 3 if present, else
# is sampled off the surface so the line/points drape on it.
function _pack_dataset(D, G::GMTgrid)
	segs = (D isa GMTdataset || D isa AbstractMatrix) ? (D,) : collect(D)
	xyz = Float64[]
	segoff = Cint[0]
	off = 0
	for seg in segs
		m = seg isa GMTdataset ? seg.data : seg
		n, ncol = size(m, 1), size(m, 2)
		for k in 1:n
			x = Float64(m[k, 1]); y = Float64(m[k, 2])
			z = ncol >= 3 ? Float64(m[k, 3]) : Float64(_sample_grid(G, x, y))
			push!(xyz, x, y, z)
		end
		off += n
		push!(segoff, Cint(off))
	end
	return xyz, segoff, length(segs), off
end

"""
	view_grid(G::GMTgrid; cmap=:auto, drape=nothing, outside=:shademesh, outside_color=200,
			  title="i'GMT", geographic=nothing)

Show a GMT.jl grid in the Qt + VTK viewer. Returns a `QtFigure` handle immediately; the
window stays live while you keep using the REPL. Pass the handle to [`add!`](@ref) to add
elements (lines/points) to this window later. `cmap` is any GMT colormap name (e.g. `:geo`,
`:turbo`, `:rainbow`, `:roma`), applied LINEARLY over the grid's z range via
`makecpt`; pass `nothing` for the built-in ramp. The default `:auto` picks `:geo` for
topo/bathymetry grids (those GMT tags with `cpt == "geo"`) and `:turbo` for everything else. `drape` is an optional `GMTimage`
textured over the surface instead of the CPT colouring.

`outside` controls the grid area the `drape` image does NOT cover (mirrors GMTF3D):
- `:shademesh` (default) — flat `outside_color` fill (uncovered reads as the shaded surface).
- `:shade`               — flat `outside_color` fill.
- `:transparent`         — see-through; the CPT-coloured base surface shows through.
`outside_color` is a grey `0-255` int or an `(r,g,b)` tuple (0-255 ints or 0-1 floats).

The surface mesh (wire edges) is HIDDEN by default; press **`e`** in the viewer to
toggle it on/off (works on both the base surface and any drape).

`triangulate` (default `true`) builds the surface from 2 triangles per grid cell
(diagonals, GMTF3D-style); pass `false` for a single quad per cell.

`data` overlays a `GMTdataset` (single or multi-segment `Vector{GMTdataset}`) on the
surface, drawn as `mode=:lines` (default) or `mode=:points`. Its z comes from column 3
if present, else is sampled off the grid so it drapes on the relief. `data_color` is a
grey `0-255` int, an `(r,g,b)` (0-255 ints or 0-1 floats), or `nothing` (black lines /
red points). `data_size` sets the line width or point size in px (`0` = default).
**Right-click an overlay** for a context menu to change its colour, line style/width
(lines) or size and round/square (points).

`vcurtain` hangs a Fledermaus-style vertical image **curtain** (seismic / midwater profile)
along an XY track through the scene — one spec NamedTuple, or a vector of them:
`(; image, path, zrange, spacing=:distance, flipv=false, clip=false, clip_n=300)` (same
fields as [`add_curtain!`](@ref)). E.g.
`view_grid(G; vcurtain=(; image="sect.jpg", path=track, zrange=(-10000,0), clip=true))`.

`geographic` is auto-detected (override with `true`/`false`). For geographic grids
the vertical exaggeration is referenced to metres (1°lat ≈ 111111 m, 1°lon = that ×
cos(mid-lat)); z assumed metres.
"""
function view_grid(G::GMTgrid; cmap=:auto, drape=nothing, outside::Symbol=:shademesh, outside_color=200,
				   triangulate::Bool=true, data=nothing, mode::Symbol=:lines, data_color=nothing, data_size=0,
				   vcurtain=nothing, title::AbstractString="i'GMT",
				   geographic::Union{Bool,Nothing}=nothing)
	_load_dialog_begin("Opening the grid…")             # closed by _load_dialog_end just before we return
	# The viewer's grid ccall takes GMT's own memory layout ("BCB": column-major, z sized (ny,nx),
	# row 1 = south). A GDAL-produced grid (gdalwarp, gdaltranslate, gdaldem) is "TRB" — row-major,
	# north first, z sized (nx,ny) — and reading one as the other renders the transpose. This is the
	# SECOND door into the viewer (the other is `_add_grid_to_scene`); both normalise, so no caller
	# has to know which layout its grid happens to carry.
	G = _grid_to_bcb(G)
	cmap === :auto && (cmap = _default_cmap(G))         # geo only for topo/bathymetry grids, else turbo
	z = eltype(G.z) === Float32 ? G.z : Float32.(G.z)   # column-major; viewer reads z[ix*ny + iy]. Float32 = no copy (C stores float anyway)
	ny, nx = size(z)                  # GMT layout: dim1 = ny (y), dim2 = nx (x)
	r = G.range                       # [xmin xmax ymin ymax zmin zmax ...]
	geog = geographic === nothing ? _isgeographic(G) : geographic
	cz, crgb, ncolor = _cpt_nodes(G, cmap)   # linear CPT control nodes (z + rgb)
	# Georeferenced drape: the grid is kept WHOLE. Only the grid ∩ image area carries the
	# picture (RGBA canvas over the full grid bbox). `outside` decides the uncovered area:
	# opaque fill (+mesh for :shademesh) or transparent (CPT base shows through).
	edges = 0
	if drape === nothing
		img = C_NULL; iw = ih = ibands = 0
	else
		outside in (:transparent, :shade, :shademesh) ||
			error("`outside` must be :transparent, :shade or :shademesh (got :$outside)")
		ir = drape.range
		((min(r[2], ir[2]) > max(r[1], ir[1])) && (min(r[4], ir[4]) > max(r[3], ir[3]))) ||
			error("grid and image bounding boxes do not overlap")
		# outside_color: a grey 0-255 int, or an (r,g,b). 0-1 floats are scaled to 0-255.
		fcol = outside_color isa Number ? (outside_color, outside_color, outside_color) : outside_color
		fillu = map(c -> c <= 1 ? round(UInt8, 255c) : UInt8(round(c)), fcol)
		img, iw, ih, ibands = _drape_to_bbox(drape, r[1], r[2], r[3], r[4]; outside=outside, fill=fillu)
	end
	# mesh (wire edges) hidden by default; the 'e' hotkey toggles it live in the viewer.
	h = ccall(_fn(:gmtvtk_view_grid), Ptr{Cvoid},
		  (Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cint, Cint, Cstring),
		  z, nx, ny, r[1], r[2], r[3], r[4], Cint(geog), cz, crgb, Cint(ncolor),
		  img, Cint(iw), Cint(ih), Cint(ibands), Cint(edges), Cint(triangulate), Cint(0), title)
	fig = _register_fig!(QtFigure(h, G))
	_remember_object!(h, :grid, "", G)                # File>Save / Scene Objects "Save…" can write it
	_session_record!(h, :basegrid, :generated)        # Save Session: no source path here -> serialize the grid
	_apply_crs!(fig, crs_from(G; geographic=geog))    # store the CRS + reveal the Geography menu if referenced
	# Optional GMTdataset overlay (lines or points), added to the window just created.
	data !== nothing && _add_overlay!(fig, data, mode, data_color, data_size)
	# Optional vertical curtain(s) (Fledermaus seismic / midwater profile).
	_add_curtains!(fig, vcurtain)
	_start_pump()
	_load_dialog_end()
	return fig
end

# A GMTimage is "referenced" (real-world coords -> show coordinate ANNOTATIONS) if it is
# geographic, carries a projection (proj4 / wkt / epsg), OR its range differs from the raw pixel
# dimensions. A plain (un-georeferenced) image has none of these -> shown with no axes at all.
function _is_referenced(I::GMTimage)
	GMT.isgeog(I) && return true
	(hasproperty(I, :proj4) && I.proj4 isa AbstractString && !isempty(I.proj4)) && return true
	(hasproperty(I, :wkt)   && I.wkt   isa AbstractString && !isempty(I.wkt))   && return true
	(hasproperty(I, :epsg)  && I.epsg  isa Integer        && I.epsg != 0)       && return true
	# Range vs raw pixel dimensions: a plain image spans pixel coords (extent ≈ ncols × nrows).
	ir = I.range
	ny = size(I.image, 1); nx = size(I.image, 2)
	xext = ir[2] - ir[1]; yext = ir[4] - ir[3]
	(isapprox(xext, nx; atol = 1.5) && isapprox(yext, ny; atol = 1.5)) || return true
	return false
end

"""
	view_image(I::GMTimage; title="i'GMT", geographic=nothing, axes=nothing)

Show a bare `GMTimage` (no elevation) in the viewer: a flat plane textured with the image,
opened maximized in a top-down orthographic map. Returns a `QtImage` handle immediately (the
window stays live while you use the REPL). The status-bar readout shows the pixel **colour**
(`rgb = R G B`) under the cursor instead of a z value. `geographic` is auto-detected
(`GMT.isgeog`); override with `true`/`false`. Also reachable as `iview(I)`.
"""
function view_image(I::GMTimage; title::String="i'GMT",
					geographic::Union{Bool,Nothing}=nothing, axes::Union{Bool,Nothing}=nothing)
	ir = I.range
	geog = geographic === nothing ? GMT.isgeog(I) : geographic
	# Referenced image -> show the lon/lat (X/Y) axes; plain (un-georeferenced) image -> no axes.
	# Auto: geographic, or carrying a projection (proj4/wkt/epsg). Override with `axes=true/false`.
	referenced = axes === nothing ? _is_referenced(I) : axes
	# Flat 2x2 zero grid spanning the image bbox; the image is draped over that whole bbox, so
	# the entire plane carries the picture (nothing is left uncovered -> outside is moot).
	nx = ny = 2
	z = zeros(Float32, ny, nx)                          # column-major ny x nx, all z = 0
	fillu = (UInt8(200), UInt8(200), UInt8(200))
	img, iw, ih, ibands = _drape_to_bbox(I, ir[1], ir[2], ir[3], ir[4]; outside=:transparent, fill=fillu)
	imode = referenced ? Cint(1) : Cint(2)             # 1 = axes + margin, 2 = no axes + edge-to-edge
	h = ccall(_fn(:gmtvtk_view_grid), Ptr{Cvoid},
		  (Ptr{Cfloat}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
		   Ptr{Cdouble}, Ptr{Cdouble}, Cint, Ptr{Cuchar}, Cint, Cint, Cint, Cint, Cint, Cint, Cstring),
		  z, Cint(nx), Cint(ny), ir[1], ir[2], ir[3], ir[4], Cint(geog),
		  C_NULL, C_NULL, Cint(0), img, Cint(iw), Cint(ih), Cint(ibands),
		  Cint(0), Cint(1), imode, title)               # edges=0, triangulate=1, image_only=imode
	h == C_NULL && error("view_image: the viewer could not open the window")
	fig = _register_fig!(QtImage(h, I))
	_remember_object!(h, :image, "", I)               # File>Save / Scene Objects "Save…" can write it
	_session_record!(h, :image, :generated)           # Save Session: no source path here -> serialize the image
	_apply_crs!(fig, crs_from(I; geographic=geog))    # store the CRS + reveal the Geography menu if referenced
	_start_pump()
	return fig
end

# Pack `data` and push it onto figure `fig` as a line/point overlay (the C side renders
# immediately). Shared by view_grid's `data=` kwarg and add!.
function _add_overlay!(fig::QtFigure, data, mode::Symbol, data_color, data_size)
	mode in (:lines, :points) || error("`mode` must be :lines or :points (got :$mode)")
	xyz, segoff, nseg, npts = _pack_dataset(data, fig.G)
	modei = mode === :lines ? Cint(1) : Cint(0)
	cr, cg, cb = _ovl_color(data_color, mode)
	lw = mode === :lines  ? Float64(data_size) : 0.0
	ps = mode === :points ? Float64(data_size) : 0.0
	ok = ccall(_fn(:gmtvtk_add_overlay_h), Cint,
		  (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cstring),
		  fig.h, xyz, Cint(npts), segoff, Cint(nseg), modei, cr, cg, cb, lw, ps, "")
	ok == 0 && @warn "figure window is closed; overlay not added"
	return fig
end

"""
	add!(fig::QtFigure, data; mode=:lines, color=nothing, size=0)

Add elements to an existing viewer window `fig` (returned by `view_grid`). `data` is a
`GMTdataset` (single or multi-segment `Vector{GMTdataset}`), or an `N×2`/`N×3` matrix,
drawn as `mode=:lines` (default) or `mode=:points`. Elements with no z column are draped
on the figure's surface. `color` is a name (`:red`, `:black`, ...), a `0-255` grey, an
`(r,g,b)` (0-255 ints or 0-1 floats), or `nothing` (black lines / red points). `size` is
the line width or point size in px (`0` = default). Returns `fig`.

(Named `add!`, not `plot!`, to avoid clashing with GMT.jl's `plot!`.)
"""
add!(fig::QtFigure, data; mode::Symbol=:lines, color=nothing, size=0) =
	_add_overlay!(fig, data, mode, color, size)

# Pure crop math for "Roi Crop Tools" (port of Mirone's mirone.m ImageCrop_CB CropaGrid_pure /
# plain-image-crop cases) — no DLL, no window, so this is what the unit tests exercise directly.
# GMT.jl's in-memory `crop` (works on both GMTgrid and GMTimage) is the SAME function `GMT.grdcut`
# itself dispatches to for a plain region-only cut on an in-memory grid (grdcut.jl:81) — so cropping
# through here and through `grdcut(G; region=...)` must agree exactly; the unit test locks that down.
_roi_crop_grid(G::GMTgrid, w, e, s, n)   = GMT.crop(G; region=(w, e, s, n))[1]
# `_to_band_planar` FIRST (drape.jl): a disk-read RGB image is pixel-interleaved ("BRPa"), and a
# row-major image sends `crop` down its gdaltranslate branch, which reads the buffer as band-planar
# -> colour noise. De-interleaved, the same crop is exact.
_roi_crop_image(I::GMTimage, w, e, s, n) = GMT.crop(_to_band_planar(I); region=(w, e, s, n))[1]

# "Crop Image" (plain, un-referenced): `GMT.crop` preserves the source image's own CRS, which is
# correct for "Crop Image (with coords)" but wrong for the plain variant — Mirone's real distinction
# between the two buttons. Strips proj4/wkt/epsg off a copy so the result carries no georeferencing
# (still sits at its true world x/y in the shared-axes window; just not GeoTIFF-referenceable).
function _strip_crs(I::GMTimage)
	I2 = deepcopy(I)
	I2.proj4 = ""; I2.wkt = ""; I2.epsg = 0
	return I2
end

# Read the window's OWN CRS back (gmtvtk_get_crs, the getter counterpart to _apply_crs!'s
# gmtvtk_set_crs — nothing previously exposed the actual proj4/wkt strings). Returns NO_CRS if the
# window carries none.
function _window_crs(scene::Ptr{Cvoid})
	proj4buf = zeros(UInt8, 512); wktbuf = zeros(UInt8, 8192)
	epsg = ccall(_fn(:gmtvtk_get_crs), Cint,
		(Ptr{Cvoid}, Ptr{UInt8}, Cint, Ptr{UInt8}, Cint),
		scene, proj4buf, Cint(length(proj4buf)), wktbuf, Cint(length(wktbuf)))
	z = findfirst(==(0x00), proj4buf); proj4 = String(proj4buf[1:(z === nothing ? 0 : z - 1)])
	z = findfirst(==(0x00), wktbuf);   wkt   = String(wktbuf[1:(z === nothing ? 0 : z - 1)])
	return CRS(proj4, wkt, Int(epsg))
end

# Fallback for "Crop Image"/"Crop Image (with coords)" when the window has no separate bitmap image
# to crop: the rendered GRID itself IS the picture in that case (Mirone's own architecture always
# has one — a grid there is displayed AS a pseudocolour image; this app's true-3D grids don't, until
# captured). Reuses the SAME proven NDC-projection capture technique as File>Save Screenshot GeoTIFF
# (`gmtvtk_capture_rect_rgb`, 90_c_api.cpp), applied to the rectangle's own bbox instead of the whole
# data bbox, at the window's CURRENT camera angle (no flat2d forcing).
#
# Both variants place the picture at its real world x/y (so it sits correctly in the SHARED-axes
# window — see SACRED_LAW.md, no exception for "plain"). `coords`: Mirone's real distinction between
# the two Crop Image buttons was never about axes (this app has none per-image, the WINDOW'S are
# shared) — it's whether the picture carries real georeferencing at all. `coords=true` attaches the
# window's own CRS (`_window_crs`), so the result round-trips through File>Save Image as a genuine
# GeoTIFF; `coords=false` leaves it a bare picture with no proj4/wkt, matching Mirone's un-referenced
# plain crop.
function _capture_rect_image(scene::Ptr{Cvoid}, w, e, s, n; coords::Bool=true)
	pRgb = Ref{Ptr{UInt8}}(C_NULL)
	pW = Ref{Cint}(0); pH = Ref{Cint}(0)
	# Prefer the data-space bake (gmtvtk_capture_rect_databaked): native grid resolution, independent
	# of the current render-window size / zoom -- the screen-space grab below is only a fallback for
	# when there's no baked grid to read from (see the export's own doc, 90_c_api.cpp).
	ok = ccall(_fn(:gmtvtk_capture_rect_databaked), Cint,
		(Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble, Ptr{Ptr{UInt8}}, Ptr{Cint}, Ptr{Cint}),
		scene, Float64(w), Float64(e), Float64(s), Float64(n), pRgb, pW, pH)
	if ok == 0
		ok = ccall(_fn(:gmtvtk_capture_rect_rgb), Cint,
			(Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble, Ptr{Ptr{UInt8}}, Ptr{Cint}, Ptr{Cint}),
			scene, Float64(w), Float64(e), Float64(s), Float64(n), pRgb, pW, pH)
	end
	ok == 0 && error("Roi Crop: rectangle isn't visible on screen, nothing to capture")
	try
		view = unsafe_wrap(Array, pRgb[], (3, Int(pW[]), Int(pH[])))   # (band, col, row), C memory, borrowed
		mat = permutedims(view, (3, 2, 1))                              # (row, col, band), owned copy
		if coords
			crs = _window_crs(scene)
			return GMT.mat2img(mat; x=[Float64(w), Float64(e)], y=[Float64(s), Float64(n)],
			                   proj4=crs.proj4, wkt=crs.wkt)
		else
			return GMT.mat2img(mat; x=[Float64(w), Float64(e)], y=[Float64(s), Float64(n)])
		end
	finally
		ccall(_fn(:gmtvtk_free_rgb), Cvoid, (Ptr{UInt8},), pRgb[])
	end
end

# SACRED_LAW.md, "same operation = same function". A DERIVED variable becoming what the window shows
# is the SAME transition a newly opened FILE performs, so it is the SAME function: the new row
# CHECKED, EVERY other data layer UNCHECKED (grid, image, mesh, base surface — the KIND is
# irrelevant), the axes + camera re-framed to the new thing's OWN X/Y/Z, Scene Objects unfolded.
# `gmtvtk_show_new_element_h`, reached through `_adopt_new_element` (drop.jl), is that one function.
#
# Every derive tool (crop, RTP, IGRF, Okada, clip, grdsample, illumination, binarize, tides, the
# derived-image commit) used to hand-roll the same three steps instead —
# `_show_object!` + `_hide_other_objects!(kind)` + unfold — a parallel implementation that drifted in
# exactly the two ways the law predicts:
#   * it hid only the SAME KIND, so anything of the other kind stayed on screen WITH ITS OWN AXES
#     (raster-own-axes law: every raster owns a set now). The user's repro: crop a rectangle out of an
#     Ocean Color grid and the global browse IMAGE's axes were still standing around it — the crop
#     looked like it had "kept the parent's axes".
#   * most of those sites never re-framed at all, so the derived grid genuinely wore the parent's frame.
# Do NOT reintroduce the trio, and do NOT add a per-kind hide helper back. A tool that needs "my
# result is now what this window shows" calls THIS, with the object it just produced.
_adopt_derived!(scene::Ptr{Cvoid}, name::AbstractString, data) = _adopt_new_element(scene, name, data)

# DELETED, do not bring back: `_hide_other_objects!(scene, kind, keepname)`, `_hide_all_grids!` and
# `_show_object!`. Together they WERE the parallel implementation `_adopt_derived!` above replaces —
# per-kind hiding is precisely the drift that left an image's axes standing around a cropped grid.
# Their C exports (`gmtvtk_hide_other_grids` / `_images`) stay: `gmtvtk_show_new_element_h` is built
# on the same ground-truth walk of the live actors, which is the half of them that was right.
#
# (kept for the record, since it is the reasoning that produced them)
# Earlier version hid a single guessed "srcname" (`_find_object_named`'s first-match-of-kind). That
# guess is only right when the window holds exactly one grid/image; the moment it holds more than
# one (e.g. a base grid PLUS a named extra like "layer0.grd" — the user's own live repro, confirmed
# by their Scene Objects screenshot showing BOTH still checked after a crop), `_find_object_named`
# has no way to know which one a freehand rectangle was actually drawn over, so it can guess wrong
# and hide the WRONG (or no) object, leaving the real source checked. Fix: don't guess — hide ALL
# other known objects of this kind. This still gives exactly the law's OBSERVABLE guarantee (only
# the new result ends up checked), and matches this app's own existing one-visible-at-a-time
# convention for grids (`gmtvtk_add_surface_h`'s own comment: "two grids visible at once is never
# wanted"), so it's not overreach — dead-in-fact, this is the same guarantee that convention already
# assumed but never enforced ITSELF for the crop case.
#
# For :grid, this goes straight to `gmtvtk_hide_other_grids` (90_c_api.cpp) — GROUND TRUTH,
# operating directly on the live `s->extras`/`s->surf`, not a Julia-side bookkeeping dict. A dict
# can only hide what IT remembers being added; if a grid reached the scene through any path the
# registry doesn't track (or the two ever drift apart), a Julia-only fix is blind to it — reading
# straight off the actual actors/names removes that class of gap entirely: whatever the Scene
# Objects panel shows a checkbox for is exactly what the C++ loop can see and hide.
#
# :image went through `gmtvtk_hide_other_images`, the same ground-truth C export as :grid. Both are
# now reached only through `gmtvtk_show_new_element_h`, which does the whole transition instead of
# just the hide half.

# "Roi Crop Tools" rectangle context-menu entries (55_lineprops.cpp rectRoiCrop) — port of Mirone's
# draw_funs.m rectangle item_tools submenu / mirone.m ImageCrop_CB. Crops the window's PRIMARY grid
# or image (found via savefile.jl's `_find_object_named`, so we also learn what to hide) to the
# rectangle's bounding box.
#
# SACRED_LAW.md derived-variable display law, NO EXCEPTIONS: the crop is a NEW derived variable, so
# it ALWAYS goes into the SAME window as a new named Scene Objects row
# (`_add_grid_to_scene`/`_add_image_to_scene` — the SAME shared add-to-scene functions every other
# drop/derive path uses, never a raw ccall or a new window), starts CHECKED, everything it was
# derived from is UNCHECKED, and Scene Objects UNFOLDS to reveal it — all of that through the ONE
# shared transition `_adopt_derived!`.
#
# `kind`: "grid" | "image" | "image_coords" — an earlier version of this function opened "image"
# (Mirone's PLAIN, un-georeferenced crop) in a NEW standalone window instead, reasoning that a
# pixel-space image can't join an already-georeferenced window's shared axes. That reasoning may be
# true, but carving out an exception to the law is not this function's call to make — the law says
# ALWAYS, so "image" now follows the exact same same-window path as "image_coords". Both currently
# behave identically once in-window (Mirone's plain-vs-coords distinction was ONLY meaningful for a
# standalone window's own axes, which no longer applies) — that's a consequence of the law, not a
# bug. Only these three basic crops are ported for now; Mirone's fuller ROI toolset
# (stats/clip/fill-gaps/spectral…) is not.
function _on_roi_crop(scene::Ptr{Cvoid}, kind::String, rectstr::String)::Cvoid
	k = ""
	try
		k = kind
		parts = split(rectstr, '/')
		length(parts) == 4 || error("Roi Crop: malformed rect '$rectstr'")
		w, e, s, n = parse.(Float64, parts)
		if k == "grid"
			srcname, G = _find_object_named(scene, :grid)
			G === nothing && error("No grid to crop in this window")
			Gc = _roi_crop_grid(G, w, e, s, n)
			newname = isempty(srcname) ? "Cropped grid" : "$srcname (cropped)"
			_add_grid_to_scene(scene, Gc, newname; promote=false)
			# ONE transition (`_adopt_derived!`): checked, everything else unchecked WHATEVER ITS KIND,
			# axes+camera re-framed to the crop's OWN X/Y/Z (`Gc.range`), Scene Objects unfolded.
			_adopt_derived!(scene, newname, Gc)
		elseif k == "image" || k == "image_coords"
			srcname, I = _find_object_named(scene, :image)
			wantCoords = (k == "image_coords")
			# No separate bitmap image in this window (e.g. a pure grid, no draped/dropped image) ->
			# capture the rendered grid itself instead of erroring. See `_capture_rect_image`.
			capturedFromGrid = I === nothing
			Ic = capturedFromGrid ? _capture_rect_image(scene, w, e, s, n; coords=wantCoords) :
			     wantCoords       ? _roi_crop_image(I, w, e, s, n) :   # GMT.crop preserves I's own CRS as-is
			                        _strip_crs(_roi_crop_image(I, w, e, s, n))   # plain: no georeferencing
			newname = (capturedFromGrid || isempty(srcname)) ? "Cropped image" : "$srcname (cropped)"
			_add_image_to_scene(scene, Ic, newname; promote=false)
			# Same ONE transition as the grid branch. It hides every other layer of EVERY kind, so the
			# old `capturedFromGrid && _hide_all_grids!` special case (the picture's real source was a
			# grid) is not a case at all any more — which is the point of there being one function.
			# The margin images keep so their tick labels stay on screen comes from the data type too.
			_adopt_derived!(scene, newname, Ic)
		else
			error("Roi Crop: unknown kind '$k'")
		end
		_viewer_log_error(scene, "Cropped $k")
	catch e
		_viewer_log_error(scene, "Roi Crop FAILED: $(sprint(showerror, e))")
		@warn "roi_crop: could not crop" exception=(e,)
	end
	return
end
