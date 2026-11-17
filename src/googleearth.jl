# googleearth.jl — 2D/3D flyout > "Send to Google Earth": the window as Google Earth sees it.
#
# Port of Mirone's utils/writekml.m (the toolbar "See in Google Earth" button), done better on the one
# point the user asked for: Mirone always wrote a loose .kml next to a loose image in its tmp dir. Here
# the output is ONE file the user picks, and it is a KMZ whenever the document references anything
# (the raster picture, a symbol's icon): doc.kml + its files, zipped (the store-only `_zip_bytes`,
# session.jl — no new dependency). Only a document with nothing to reference (lines, polygons, text)
# is written as a plain .kml.
#
# GOOGLE EARTH WANTS LON/LAT (WGS84). Everything sent is brought there first:
#   * the raster through `GMT.gdalwarp`, with THE gdalwarp option builder Tools > Project uses
#     (`_project_opts`, project.jl), so a warped picture keeps transparency the way every other warp does;
#   * every vector vertex through THE point transform (`_srs_xy`, crs.jl).
# A window that is already lon/lat is sent untouched. A window with no known CRS that is not lon/lat
# cannot be placed on the Earth at all, and says so.
#
# WHAT IS SENT is what is ON SCREEN, read through the same doors the GMT.jl script export and Save
# Session read it: the raster on display (`_active_element`), its displayed picture (`_grid_picture`:
# the viewer's own bake, CPT + shading — the pixels the user looks at, NaN nodes transparent), and the
# `gmtvtk_serialize_*` snapshots of every checked vector element.

const _GE_KML_NS = "http://www.opengis.net/kml/2.2"

_ge_esc(s) = replace(String(s), "&" => "&amp;", "<" => "&lt;", ">" => "&gt;", "\"" => "&quot;")
_ge_hex(v::Real) = string(clamp(round(Int, 255 * v), 0, 255); base = 16, pad = 2)
# KML colours are aabbggrr (alpha first, then BLUE): Mirone's writekml flips the triple for the same reason.
_ge_color(r, g, b, a = 1.0) = _ge_hex(a) * _ge_hex(b) * _ge_hex(g) * _ge_hex(r)
_ge_num(x::Real) = string(round(Float64(x); digits = 7))
_ge_lon(x::Float64) = x > 180.0 ? x - 360.0 : (x < -180.0 ? x + 360.0 : x)

# n×2 (or wider) window coordinates -> n×2 lon/lat, through THE transform. `src` = "" means the window
# is lon/lat already.
function _ge_lonlat(M::AbstractMatrix{<:Real}, src::String)::Matrix{Float64}
	XY = Float64.(M[:, 1:2])
	size(XY, 1) == 0 && return XY
	isempty(src) || (XY = _srs_xy(XY, src, _LONLAT))
	for i in 1:size(XY, 1)
		XY[i, 1] = _ge_lon(XY[i, 1])
	end
	return XY
end

_ge_coords(L::Matrix{Float64}) = join((string(_ge_num(L[i, 1]), ",", _ge_num(L[i, 2]), ",0") for i in 1:size(L, 1)), " ")

# A ring for KML must be explicitly closed.
function _ge_ring(L::Matrix{Float64})::Matrix{Float64}
	size(L, 1) < 3 && return L
	(L[1, 1] == L[end, 1] && L[1, 2] == L[end, 2]) && return L
	return vcat(L, L[1:1, :])
end

# ── the document being built ───────────────────────────────────────────────────────────────────
mutable struct _GEDoc
	styles::IOBuffer
	body::IOBuffer
	files::Vector{Tuple{String,Vector{UInt8}}}     # everything doc.kml references (KMZ members)
	nstyle::Int
	box::Vector{Float64}                           # lon/lat bbox of everything sent (W,E,S,N) for <LookAt>
	tag::String                                    # unique per send: GE caches members by path (see _ge_tmp_path)
end
_GEDoc() = _GEDoc(IOBuffer(), IOBuffer(), Tuple{String,Vector{UInt8}}[], 0, [Inf, -Inf, Inf, -Inf],
                   string(rand(UInt32); base = 36))

function _ge_grow!(d::_GEDoc, L::Matrix{Float64})
	size(L, 1) == 0 && return
	x0, x1 = extrema(view(L, :, 1)); y0, y1 = extrema(view(L, :, 2))
	d.box[1] = min(d.box[1], x0); d.box[2] = max(d.box[2], x1)
	d.box[3] = min(d.box[3], y0); d.box[4] = max(d.box[4], y1)
	return
end

# A shared <Style>; returns its id.
function _ge_style!(d::_GEDoc, inner::String)::String
	d.nstyle += 1
	id = "s$(d.nstyle)"
	print(d.styles, "\t<Style id=\"", id, "\">", inner, "</Style>\n")
	return id
end
_ge_line_style(r, g, b, w) = "<LineStyle><color>$(_ge_color(r, g, b))</color><width>$(_ge_num(max(w, 0.5)))</width></LineStyle>"
_ge_poly_style(r, g, b, a, outline::Bool) =
	"<PolyStyle><color>$(_ge_color(r, g, b, a))</color><outline>$(outline ? 1 : 0)</outline></PolyStyle>"

# ── raster bytes, in memory (GDAL /vsimem/ — no temporary file) ─────────────────────────────────
function _ge_image_bytes(I::GMTimage, ext::String)::Vector{UInt8}
	p = "/vsimem/igmt_ge_" * string(rand(UInt64); base = 16) * ext
	try
		_save_image(I, "", p)                       # THE image writer: true row order, any layout
		len = Ref{UInt64}(0)
		ptr = ccall((:VSIGetMemFileBuffer, GMT.Gdal.libgdal), Ptr{UInt8}, (Cstring, Ptr{UInt64}, Cint), p, len, 0)
		ptr == C_NULL && error("GDAL wrote nothing for the Google Earth picture")
		return copy(unsafe_wrap(Array, ptr, Int(len[])))
	finally
		GMT.Gdal.VSIUnlink(p)
	end
end

# The picture's outer edges W,E,S,N. A grid-registered picture's nodes are pixel CENTRES.
function _ge_edges(I::GMTimage)::NTuple{4,Float64}
	W, E, S, N = Float64.(I.range[1:4])
	if I.registration == 0
		hx, hy = I.inc[1] / 2, I.inc[2] / 2
		W -= hx; E += hx; S -= hy; N += hy
	end
	return (W, E, max(S, -90.0), min(N, 90.0))
end

# The raster on display as a GMTimage in ITS OWN system, plus its name; (nothing, "") when the window
# shows no raster. A grid is sent as the picture it displays, its NaN nodes made transparent.
function _ge_displayed_picture(scene::Ptr{Cvoid})
	k, nm = _active_element(scene)
	if k == 1
		G = _active_grid(scene, nm)
		G isa GMTgrid || return nothing, ""
		I = _grid_picture(scene, nm, G)              # (row, col, band), row 1 = north
		Z = _zmat(G)                                 # (ny, nx), row 1 = south
		if any(isnan, Z)
			ny, nx = size(I.image, 1), size(I.image, 2)
			A = Array{UInt8}(undef, ny, nx, 4)
			A[:, :, 1:3] .= I.image
			@inbounds for c in 1:nx, r in 1:ny
				A[r, c, 4] = isnan(Z[ny - r + 1, c]) ? 0x00 : 0xff
			end
			J = mat2img(A; x = G.x, y = G.y, proj4 = G.proj4, wkt = G.wkt)
			J.epsg = G.epsg
			I = J
		end
		name = isempty(nm) ? String(get(_scene_state(scene), "surf_name", "grid")) : String(nm)
		return I, name
	elseif k == 2
		n, I = _displayed_image(scene)
		I isa GMTimage || return nothing, ""
		return I, isempty(nm) ? (isempty(n) ? "image" : n) : String(nm)
	end
	return nothing, ""
end

# An opaque 8-bit RGB picture gains an opaque alpha band before a warp, so the corners a rotated or
# curved warp leaves empty come out TRANSPARENT over Google Earth instead of white.
function _ge_with_alpha(I::GMTimage)::GMTimage
	J = _to_band_planar(I)
	(eltype(J.image) == UInt8 && ndims(J.image) == 3 && size(J.image, 3) == 3) || return J
	K = mat2img(cat(J.image, fill(0xff, size(J.image, 1), size(J.image, 2)); dims = 3), J)
	K.layout = J.layout
	return K
end

# The GroundOverlay: the displayed picture, warped to lon/lat when its system is another.
function _ge_raster!(d::_GEDoc, scene::Ptr{Cvoid}, wsrc::String, wgeog::Bool)
	I, name = _ge_displayed_picture(scene)
	I === nothing && return false
	rsrc = _crs_srs(crs_from(I))
	isempty(rsrc) && (rsrc = wsrc)
	geog = isempty(rsrc) ? wgeog : _project_is_geog(rsrc)
	(geog || !isempty(rsrc)) ||
		error("the displayed raster has no known coordinate system, so it cannot be placed on the Earth")
	if !geog
		J = _ge_with_alpha(I)
		opts = _project_opts(Dict("t_srs" => _LONLAT, "s_srs" => rsrc), true; alpha = _has_alpha_band(J))
		R = GMT.gdalwarp(J, opts)
		R isa GMTimage || error("gdalwarp returned a $(typeof(R)), not an image")
		I = R
	end
	W, E, S, N = _ge_edges(I)
	W >= 180.0 && (W -= 360.0; E -= 360.0)
	# PNG where there is transparency, a palette or more than 8 bits; JPEG (far smaller) otherwise.
	png = _has_alpha_band(I) || I.n_colors > 0 || eltype(I.image) != UInt8 ||
	      size(I.alpha) == (size(I.image, 1), size(I.image, 2))
	href = "files/overlay_" * d.tag * (png ? ".png" : ".jpg")
	push!(d.files, (href, _ge_image_bytes(I, png ? ".png" : ".jpg")))
	print(d.body, "\t<GroundOverlay><name>", _ge_esc(name), "</name><Icon><href>", href, "</href></Icon>",
	      "<altitudeMode>clampToGround</altitudeMode><LatLonBox><north>", _ge_num(N), "</north><south>",
	      _ge_num(S), "</south><east>", _ge_num(E), "</east><west>", _ge_num(W),
	      "</west></LatLonBox></GroundOverlay>\n")
	_ge_grow!(d, [W S; E N])
	return true
end

# ── symbol icons (packed in the KMZ, drawn here: no web icon, works offline) ────────────────────
# The viewer's own glyph shapes (makeSymbolGlyph, 50_scene.cpp): n-gon at a rotation, star, line glyphs.
function _ge_shape(sym::AbstractString)
	poly(n, rot) = [(cosd(rot + 360k / n), sind(rot + 360k / n)) for k in 0:n-1]
	sym == "s" && return (:poly, poly(4, 45.0))
	sym == "t" && return (:poly, poly(3, 90.0))
	sym == "i" && return (:poly, poly(3, -90.0))
	sym == "d" && return (:poly, poly(4, 90.0))
	sym == "h" && return (:poly, poly(6, 0.0))
	sym == "n" && return (:poly, poly(5, 90.0))
	sym == "g" && return (:poly, poly(8, 22.5))
	sym == "a" && return (:poly, [(r * cosd(90 + 36k), r * sind(90 + 36k)) for k in 0:9 for r in (iseven(k) ? 1.0 : 0.382,)])
	sym == "x" && return (:lines, [((-0.8, -0.8), (0.8, 0.8)), ((-0.8, 0.8), (0.8, -0.8))])
	sym == "+" && return (:lines, [((-1.0, 0.0), (1.0, 0.0)), ((0.0, -1.0), (0.0, 1.0))])
	sym == "-" && return (:lines, [((-1.0, 0.0), (1.0, 0.0))])
	sym == "y" && return (:lines, [((0.0, -1.0), (0.0, 1.0))])
	return (:poly, poly(32, 0.0))                   # circle (and "p", the point)
end

function _ge_inpoly(x, y, P)
	c = false; j = length(P)
	for i in eachindex(P)
		xi, yi = P[i]; xj, yj = P[j]
		((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi) && (c = !c)
		j = i
	end
	return c
end
function _ge_segdist(x, y, a, b)
	dx, dy = b[1] - a[1], b[2] - a[2]
	t = clamp(((x - a[1]) * dx + (y - a[2]) * dy) / max(dx * dx + dy * dy, eps()), 0.0, 1.0)
	return hypot(x - a[1] - t * dx, y - a[2] - t * dy)
end

# A 32x32 RGBA icon of `sym`: `fill` inside (nothing = hollow), `edge` on the outline (nothing = none).
# 4x4 supersampling for a clean edge. Encoded as PNG by THE image writer.
function _ge_icon(sym::AbstractString, fill, edge, ewpx::Real)::Vector{UInt8}
	N = 32; ss = 4
	kind, geom = _ge_shape(sym)
	sc = 0.85                                       # leave room for the outline inside the 32 px
	P = [(sc * p[1], sc * p[2]) for p in (kind === :poly ? geom : Tuple{Float64,Float64}[])]
	segs = kind === :poly ? [(P[i], P[mod1(i + 1, length(P))]) for i in eachindex(P)] :
	       [((sc * a[1], sc * a[2]), (sc * b[1], sc * b[2])) for (a, b) in geom]
	hw = max(Float64(ewpx), kind === :lines ? 2.5 : 1.0) / N         # half width in unit coords (radius 1 = N/2 px)
	linecol = kind === :lines ? something(fill, edge, (1.0, 1.0, 1.0)) : edge
	A = zeros(UInt8, N, N, 4)
	for r in 1:N, c in 1:N
		sr = 0.0; sg = 0.0; sb = 0.0; n = 0
		for a in 1:ss, b in 1:ss
			x = -1 + 2 * (c - 1 + (b - 0.5) / ss) / N
			y = 1 - 2 * (r - 1 + (a - 0.5) / ss) / N
			col = nothing
			if linecol !== nothing && minimum(_ge_segdist(x, y, s[1], s[2]) for s in segs) <= hw
				col = linecol
			elseif kind === :poly && fill !== nothing && _ge_inpoly(x, y, P)
				col = fill
			end
			col === nothing && continue
			sr += col[1]; sg += col[2]; sb += col[3]; n += 1
		end
		n == 0 && continue
		A[r, c, 1] = round(UInt8, 255 * sr / n); A[r, c, 2] = round(UInt8, 255 * sg / n)
		A[r, c, 3] = round(UInt8, 255 * sb / n); A[r, c, 4] = round(UInt8, 255 * n / (ss * ss))
	end
	return _ge_image_bytes(mat2img(A; x = [0.0, Float64(N)], y = [0.0, Float64(N)]), ".png")
end

function _ge_icon_style!(d::_GEDoc, png::Vector{UInt8}, scale::Real; tinted::Bool = false)::String
	href = "files/icon_$(d.tag)_$(length(d.files) + 1).png"
	push!(d.files, (href, png))
	return _ge_style!(d, "<IconStyle><scale>$(_ge_num(scale))</scale><Icon><href>$href</href></Icon></IconStyle>" *
	                     "<LabelStyle><scale>0</scale></LabelStyle>")
end

# Points as Placemarks of one shared style; `colors`/`scales` (optional, per point) override it inline.
function _ge_points!(d::_GEDoc, name::String, L::Matrix{Float64}, style::String;
                     colors::Union{Nothing,Matrix{Float64}} = nothing, scales = nothing, base_scale::Real = 1.0)
	size(L, 1) == 0 && return
	print(d.body, "\t<Folder><name>", _ge_esc(name), "</name>\n")
	for i in 1:size(L, 1)
		print(d.body, "\t\t<Placemark><styleUrl>#", style, "</styleUrl>")
		if colors !== nothing || scales !== nothing
			print(d.body, "<Style><IconStyle>")
			colors === nothing || print(d.body, "<color>", _ge_color(colors[i, 1] / 255, colors[i, 2] / 255, colors[i, 3] / 255), "</color>")
			scales === nothing || print(d.body, "<scale>", _ge_num(base_scale * scales[i]), "</scale>")
			print(d.body, "</IconStyle></Style>")
		end
		print(d.body, "<Point><coordinates>", _ge_num(L[i, 1]), ",", _ge_num(L[i, 2]), ",0</coordinates></Point></Placemark>\n")
	end
	print(d.body, "\t</Folder>\n")
	_ge_grow!(d, L)
	return
end

# Lines: ONE Placemark per element, its segments a MultiGeometry.
function _ge_lines!(d::_GEDoc, name::String, segs::Vector{Matrix{Float64}}, style::String)
	segs = filter(L -> size(L, 1) >= 2, segs)
	isempty(segs) && return
	print(d.body, "\t<Placemark><name>", _ge_esc(name), "</name><styleUrl>#", style, "</styleUrl><MultiGeometry>")
	for L in segs
		print(d.body, "<LineString><tessellate>1</tessellate><coordinates>", _ge_coords(L), "</coordinates></LineString>")
		_ge_grow!(d, L)
	end
	print(d.body, "</MultiGeometry></Placemark>\n")
	return
end

# Polygons: `rings` = [(outer, [holes…]), …].
function _ge_polys!(d::_GEDoc, name::String, rings::Vector{Tuple{Matrix{Float64},Vector{Matrix{Float64}}}}, style::String)
	isempty(rings) && return
	print(d.body, "\t<Placemark><name>", _ge_esc(name), "</name><styleUrl>#", style, "</styleUrl><MultiGeometry>")
	for (outer, holes) in rings
		print(d.body, "<Polygon><tessellate>1</tessellate><outerBoundaryIs><LinearRing><coordinates>",
		      _ge_coords(_ge_ring(outer)), "</coordinates></LinearRing></outerBoundaryIs>")
		for H in holes
			print(d.body, "<innerBoundaryIs><LinearRing><coordinates>", _ge_coords(_ge_ring(H)),
			      "</coordinates></LinearRing></innerBoundaryIs>")
		end
		print(d.body, "</Polygon>")
		_ge_grow!(d, outer)
	end
	print(d.body, "</MultiGeometry></Placemark>\n")
	return
end

# ── every vector element on screen ──────────────────────────────────────────────────────────────
function _ge_vectors!(d::_GEDoc, h::Ptr{Cvoid}, src::String)
	# Line / point overlays (tables, coastlines, contours, tracks, …)
	for ov in _parse_overlays_blob(_script_blob(:gmtvtk_serialize_overlays, h))
		ov.visible || continue
		nm = isempty(ov.name) ? "overlay" : ov.name
		if ov.mode == 1
			if ov.filled                                # holes as the viewer paints them (even-odd)
				ds = _script_fill_rings(_script_dataset(ov.segs, false))
				rings = Tuple{Matrix{Float64},Vector{Matrix{Float64}}}[]
				for D in ds
					L = _ge_lonlat(D.data, src)
					startswith(D.header, " -Ph") && !isempty(rings) ? push!(rings[end][2], L) :
					                                               push!(rings, (L, Matrix{Float64}[]))
				end
				st = _ge_style!(d, _ge_line_style(ov.r, ov.g, ov.b, ov.lw) * _ge_poly_style(ov.r, ov.g, ov.b, 1.0, true))
				_ge_polys!(d, nm, rings, st)
			else
				st = _ge_style!(d, _ge_line_style(ov.r, ov.g, ov.b, ov.lw))
				_ge_lines!(d, nm, [_ge_lonlat(M, src) for M in ov.segs], st)
			end
		else
			st = _ge_icon_style!(d, _ge_icon("c", (ov.r, ov.g, ov.b), nothing, 0), max(ov.ps, 2.0) / 32)
			_ge_points!(d, nm, _ge_lonlat(reduce(vcat, ov.segs), src), st)
		end
	end
	# Symbol layers and arrow fields
	for sl in _parse_symbols_blob(_script_blob(:gmtvtk_serialize_symbols, h))
		sl.visible || continue
		nm = isempty(sl.name) ? "symbols" : sl.name
		D = _symbol_layer(h, sl.name, false)        # THE reader: x un-baked to true coordinates
		isempty(D) && continue
		XY = D[1].data
		if !isempty(sl.uv)                          # an arrow field: each arrow a segment, tail to tip
			xf = ccall(_fn(:gmtvtk_get_xfac), Cdouble, (Ptr{Cvoid},), h)
			(isfinite(xf) && xf != 0.0) || (xf = 1.0)
			mmax = maximum(hypot(sl.uv[i, 1], sl.uv[i, 2]) for i in 1:size(sl.uv, 1))
			mmax > 0 || continue
			segs = [_ge_lonlat([XY[i, 1] XY[i, 2]; XY[i, 1] + sl.worldSize * sl.uv[i, 1] / mmax / xf  XY[i, 2] + sl.worldSize * sl.uv[i, 2] / mmax], src)
			        for i in 1:size(XY, 1)]
			_ge_lines!(d, nm, segs, _ge_style!(d, _ge_line_style(sl.r, sl.g, sl.b, 1.5)))
			continue
		end
		percolor = !isempty(sl.rgb)
		# A layer coloured point by point gets a WHITE icon that each Placemark tints (Google Earth
		# multiplies the icon by its colour; a black outline stays black).
		fill = sl.filled ? (percolor ? (1.0, 1.0, 1.0) : (sl.r, sl.g, sl.b)) : nothing
		edge = sl.evis ? (percolor ? (0.0, 0.0, 0.0) : (sl.er, sl.eg, sl.eb)) :
		       (sl.filled ? nothing : (percolor ? (1.0, 1.0, 1.0) : (sl.r, sl.g, sl.b)))
		scale = max(sl.sizePx, 4.0) / 32
		st = _ge_icon_style!(d, _ge_icon(sl.sym, fill, edge, sl.evis ? sl.ew : 1.5), scale)
		_ge_points!(d, nm, _ge_lonlat(XY, src), st; colors = percolor ? sl.rgb : nothing,
		            scales = isempty(sl.scale) ? nothing : sl.scale, base_scale = scale)
	end
	# Drawn polygons / polylines / rectangles / circles
	for line in split(_serialize_polys_raw(h), '\n'; keepempty = false)
		p = split(line, ';'; limit = 13)
		length(p) < 13 && continue
		name = String(p[12])
		_script_shown(h, name) || continue
		M = _script_verts(String(p[13]))
		size(M, 1) < 2 && continue
		closed = parse(Int, p[1]) != 0
		lr, lg, lb, lw = parse(Float64, p[3]), parse(Float64, p[4]), parse(Float64, p[5]), parse(Float64, p[6])
		fr, fg, fb, fop = parse(Float64, p[8]), parse(Float64, p[9]), parse(Float64, p[10]), parse(Float64, p[11])
		nm = isempty(name) ? "polygon" : name
		L = _ge_lonlat(M, src)
		if closed
			st = _ge_style!(d, _ge_line_style(lr, lg, lb, lw) * _ge_poly_style(fr, fg, fb, fop, true))
			_ge_polys!(d, nm, [(L, Matrix{Float64}[])], st)
		else
			_ge_lines!(d, nm, [L], _ge_style!(d, _ge_line_style(lr, lg, lb, lw)))
		end
	end
	# Fault traces
	for line in split(_serialize_faults_raw(h), '\n'; keepempty = false)
		startswith(line, "F;") || continue
		p = split(line, ';'; limit = 10)
		length(p) < 10 && continue
		name = String(p[9])
		_script_shown(h, name) || continue
		M = _script_verts2(String(p[10]))
		size(M, 1) < 2 && continue
		_ge_lines!(d, isempty(name) ? "fault" : name, [_ge_lonlat(M, src)], _ge_style!(d, _ge_line_style(1.0, 0.0, 0.0, 2.0)))
	end
	# Ruler tracks, with the pen they wear on screen
	for r in _parse_rulers_blob(_serialize_rulers_raw(h))
		name = "Ruler $(r.id)"
		o = zeros(5)
		ccall(_fn(:gmtvtk_line_pen_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), h, name, o) == 1 || continue
		o[5] != 0 || continue
		_ge_lines!(d, name, [_ge_lonlat(r.verts, src)], _ge_style!(d, _ge_line_style(o[1], o[2], o[3], o[4])))
	end
	# Text labels, every one on screen (a batch's too): a label with no pin, in its own colour
	texts = _parse_texts_blob(_serialize_texts_raw(h; groups = true))
	if !isempty(texts)
		print(d.body, "\t<Folder><name>Texts</name>\n")
		for t in texts
			L = _ge_lonlat([t.x t.y], src)
			print(d.body, "\t\t<Placemark><name>", _ge_esc(t.text), "</name><Style><IconStyle><scale>0</scale></IconStyle>",
			      "<LabelStyle><color>", _ge_color(t.r, t.g, t.b), "</color><scale>", _ge_num(max(t.size, 6) / 14),
			      "</scale></LabelStyle></Style><Point><coordinates>", _ge_num(L[1, 1]), ",", _ge_num(L[1, 2]),
			      ",0</coordinates></Point></Placemark>\n")
			_ge_grow!(d, L)
		end
		print(d.body, "\t</Folder>\n")
	end
	return
end

# Where Google Earth flies to on open: the middle of everything sent, from high enough to see all of it.
function _ge_lookat(box::Vector{Float64})::String
	all(isfinite, box) || return ""
	lon, lat = (box[1] + box[2]) / 2, (box[3] + box[4]) / 2
	ext = max((box[2] - box[1]) * cosd(clamp(lat, -89.0, 89.0)), box[4] - box[3], 1e-4)
	rng = clamp(ext * 111_195.0 * 1.6, 500.0, 2.0e7)
	return "\t<LookAt><longitude>$(_ge_num(lon))</longitude><latitude>$(_ge_num(lat))</latitude>" *
	       "<altitude>0</altitude><heading>0</heading><tilt>0</tilt><range>$(_ge_num(rng))</range>" *
	       "<altitudeMode>clampToGround</altitudeMode></LookAt>\n"
end

# Hand the file to whatever the system opens .kmz/.kml with (Google Earth Pro, once installed).
function _ge_launch(path::String)
	if Sys.iswindows()
		run(Cmd(["cmd", "/c", "start", "", path]); wait = false)
	elseif Sys.isapple()
		run(`open $path`; wait = false)
	else
		run(`xdg-open $path`; wait = false)
	end
	return
end

# The file the menu entry writes: the system's tmp folder (Mirone wrote its MironeTec.kml to its own
# tmp dir the same way — no question asked), named after the window AND the moment of the send.
# NEVER the same name twice: Google Earth caches a document's pictures by their full path
# (<file>.kmz/files/overlay.png), so a second send written over the first one's name was drawn with
# the cached picture of the first and the box of the second — the image landed in the wrong place
# (user's report, 2026-11-17). Sends older than a day are swept from the folder on the way.
function _ge_tmp_path(h::Ptr{Cvoid})::String
	nm = try String(get(_scene_state(h), "surf_name", "")) catch; "" end
	nm = replace(splitext(nm)[1], r"[^A-Za-z0-9_.-]+" => "_")
	isempty(nm) && (nm = "GoogleEarth")
	for f in readdir(tempdir(); join = true)
		occursin(r"^iGMT_GE_.*\.km[lz]$", basename(f)) || continue
		try (time() - mtime(f) > 86400) && rm(f; force = true) catch end   # in use by GE: left alone
	end
	stamp = string(round(Int, time() * 1000); base = 36)
	return joinpath(tempdir(), "iGMT_GE_" * nm * "_" * stamp * ".kmz")
end

"""
	_google_earth(fig, path=<tmp>/iGMT_<window>.kmz; launch=true) -> String

Write what the window `fig` shows — the raster on display and every checked vector element — as a
Google Earth document in lon/lat, and open it there. The file is a KMZ whenever the document carries
files (the raster picture, symbol icons); a plain KML otherwise. `path`'s extension is set to match.
Returns the path written.
"""
function _google_earth(fig, path::AbstractString = ""; launch::Bool = true)::String
	h = _fig_handle(fig)
	try
		isempty(path) && (path = _ge_tmp_path(h))
		h == C_NULL && error("no window")
		crs  = _window_crs(h)
		wsrc = _crs_srs(crs)
		geog = isempty(wsrc) ? get(_scene_state(h), "crs", 0) == 1 : _project_is_geog(wsrc)
		(geog || !isempty(wsrc)) ||
			error("this window has no known coordinate system and is not longitude/latitude, so it cannot be placed on the Earth")
		vsrc = geog ? "" : wsrc                      # vectors live in the window's system
		d = _GEDoc()
		_ge_raster!(d, h, wsrc, geog)
		_ge_vectors!(d, h, vsrc)
		(position(d.body) == 0) && error("nothing on display to send to Google Earth")
		title = String(get(_scene_state(h), "surf_name", "iGMT"))
		isempty(title) && (title = "iGMT")
		kml = IOBuffer()
		print(kml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"", _GE_KML_NS, "\">\n<Document>\n",
		      "\t<name>", _ge_esc(title), "</name>\n", _ge_lookat(d.box), String(take!(d.styles)), String(take!(d.body)),
		      "</Document>\n</kml>\n")
		doc = take!(kml)
		out = splitext(String(path))[1] * (isempty(d.files) ? ".kml" : ".kmz")
		if isempty(d.files)
			open(f -> write(f, doc), out, "w")
		else
			_zip_write_safe(out, vcat([("doc.kml", doc)], d.files))   # doc.kml FIRST: what Google Earth opens
		end
		_viewer_log_info(h, "Google Earth -> $out")
		launch && _ge_launch(out)
		print(out)
		return out
	catch e
		_tool_failed(h, "Send to Google Earth", e)
		rethrow()
	end
end
