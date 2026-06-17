# view_grid: show a GMTgrid surface in the Qt + VTK viewer, plus the line/point overlay path
# (`add!` / the view_grid `data=` kwarg).

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
	view_grid(G::GMTgrid; cmap=:geo, drape=nothing, outside=:shademesh, outside_color=200,
			  title="GMT 3-D Viewer  (Qt + VTK)", geographic=nothing)

Show a GMT.jl grid in the Qt + VTK viewer. Returns a `QtFigure` handle immediately; the
window stays live while you keep using the REPL. Pass the handle to [`add!`](@ref) to add
elements (lines/points) to this window later. `cmap` is any GMT colormap name (e.g. `:geo`,
`:turbo`, `:rainbow`, `:roma`), applied LINEARLY over the grid's z range via
`makecpt`; pass `nothing` for the built-in ramp. `drape` is an optional `GMTimage`
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
function view_grid(G::GMTgrid; cmap=:geo, drape=nothing, outside::Symbol=:shademesh, outside_color=200,
				   triangulate::Bool=true, data=nothing, mode::Symbol=:lines, data_color=nothing, data_size=0,
				   vcurtain=nothing, title::AbstractString="GMT 3-D Viewer  (Qt + VTK)",
				   geographic::Union{Bool,Nothing}=nothing)
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
	# Optional GMTdataset overlay (lines or points), added to the window just created.
	data !== nothing && _add_overlay!(fig, data, mode, data_color, data_size)
	# Optional vertical curtain(s) (Fledermaus seismic / midwater profile).
	_add_curtains!(fig, vcurtain)
	_start_pump()
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
	view_image(I::GMTimage; title="GMT 3-D Viewer  (Qt + VTK)", geographic=nothing, axes=nothing)

Show a bare `GMTimage` (no elevation) in the viewer: a flat plane textured with the image,
opened maximized in a top-down orthographic map. Returns a `QtImage` handle immediately (the
window stays live while you use the REPL). The status-bar readout shows the pixel **colour**
(`rgb = R G B`) under the cursor instead of a z value. `geographic` is auto-detected
(`GMT.isgeog`); override with `true`/`false`. Also reachable as `iview(I)`.
"""
function view_image(I::GMTimage; title::AbstractString="GMT 3-D Viewer  (Qt + VTK)",
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
		  (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble),
		  fig.h, xyz, Cint(npts), segoff, Cint(nseg), modei, cr, cg, cb, lw, ps)
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
