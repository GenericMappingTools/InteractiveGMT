# crs.jl — the single place InteractiveGMT deals with coordinate reference systems (CRS).
#
# A figure's georeferencing is kept in ALL THREE interchangeable forms — PROJ4 string, WKT and
# (when derivable) EPSG code — so the viewer and any later GMT call can grab whichever it needs.
# `crs_from` pulls whatever a GMT object already carries and fills the missing forms with GMT.jl's
# proj/wkt/epsg converters (best-effort — not every direction is implemented yet). The resolved CRS
# is pushed down to the C viewer with `_apply_crs!`, which also drives the Geography menu: an empty
# (unreferenced) CRS keeps it hidden, since placing GSHHG features needs a reference frame.

"""
	CRS(proj4, wkt, epsg)

A coordinate reference system in its three interchangeable forms. An empty `proj4` and `wkt`
with `epsg == 0` means UNREFERENCED data. Build one from a GMT object with [`crs_from`](@ref).
"""
struct CRS
	proj4::String
	wkt::String
	epsg::Int
end

"The empty (unreferenced) CRS."
const NO_CRS = CRS("", "", 0)

"True if this CRS carries any referencing at all."
hascrs(c::CRS) = !isempty(c.proj4) || !isempty(c.wkt) || c.epsg != 0

# Read a string / integer property off a GMT object if it has a usable one (else "" / 0).
_prop_str(O, s::Symbol) = (hasproperty(O, s) && getfield(O, s) isa AbstractString) ? String(getfield(O, s)) : ""
_prop_int(O, s::Symbol) = (hasproperty(O, s) && getfield(O, s) isa Integer)        ? Int(getfield(O, s))    : 0

# Run a GMT converter that may throw or return junk; coerce to String / Int, "" / 0 on any failure.
_try_str(f)::String = try (r = f(); r isa AbstractString ? String(r) : "") catch; "" end
_try_int(f)::Int    = try (r = f(); r isa Integer ? Int(r) : 0)            catch; 0  end

"""
	crs_from(O; geographic=false) -> CRS

Build the [`CRS`](@ref) for a GMT object (`GMTgrid`, `GMTimage`, `GMTdataset`, `GMTfv`, …): read
whatever PROJ4 / WKT / EPSG it carries, then derive the missing forms with GMT.jl's converters,
using PROJ4 as the pivot. `geographic=true` resolves a plain lon/lat object with no explicit
projection to WGS84 (EPSG 4326). Returns [`NO_CRS`](@ref) for unreferenced data.
"""
function crs_from(O; geographic::Bool=false)
	p = _prop_str(O, :proj4)
	w = _prop_str(O, :wkt)
	e = _prop_int(O, :epsg)
	if isempty(p) && isempty(w) && e == 0          # nothing explicit on the object
		geographic || return NO_CRS                # ...and not geographic -> unreferenced
		return CRS("+proj=longlat +datum=WGS84 +no_defs", "", 4326)   # plain lon/lat == WGS84
	end
	# Establish PROJ4 as the pivot, then derive the other two forms from it.
	if isempty(p)
		!isempty(w)            && (p = _try_str(() -> GMT.wkt2proj(w)))
		isempty(p) && e != 0   && (p = _try_str(() -> GMT.epsg2proj(e)))
	end
	isempty(w) && !isempty(p)  && (w = _try_str(() -> GMT.proj2wkt(p)))
	isempty(w) && e != 0       && (w = _try_str(() -> GMT.epsg2wkt(e)))
	e == 0     && !isempty(p)  && (e = _try_int(() -> GMT.proj2epsg(p)))
	return CRS(p, w, e)
end

"""
	_apply_crs!(fig, crs::CRS) -> fig

Push a resolved [`CRS`](@ref) down to the C viewer window behind `fig`. The viewer stores all
three forms and shows / hides its Geography menu depending on whether the CRS is referenced.
"""
function _apply_crs!(fig, crs::CRS)
	h = _fig_handle(fig)
	h == C_NULL && return fig
	ccall(_fn(:gmtvtk_set_crs), Cvoid,
		  (Ptr{Cvoid}, Cstring, Cstring, Cint),
		  h, crs.proj4, crs.wkt, Cint(crs.epsg))
	return fig
end


# ── EVERY PLOT KNOWS ITS DATA'S SYSTEM AND THE WINDOW'S, AND REPROJECTS WHEN THEY DIFFER ─────────
# Data has a referencing system (a coastline is lon/lat; a table may carry its own proj4/WKT), and so
# does the window it lands on (a LIDAR2011 mosaic is PT-TM06 metres; a UTM grid is UTM; a base map is
# lon/lat). Whenever the two differ the data is brought across before it is drawn — never drawn as if
# its numbers were the window's. One crossing per kind of geometry, used by EVERY plotting path:
#
#   _to_window        points          (symbols, labels, beachball centres)
#   _lines_to_window  lines, tracks   (coastlines, borders, rivers, plate boundaries, isochrons, …)
#   _polys_to_window  filled areas    (the night side of a terminator)
#   _dataset_to_window  a whole table (a dropped/opened file, a cruise track, a satellite track)
#
# Each keeps only what lies on the window's own footprint: a projection means something only near its
# own area (a Transverse Mercator asked for the far side of the Earth answers with garbage or not at
# all), and a line leaves the footprint exactly on its edge. Same system, or either side unreferenced:
# the identity, nothing touched.
#
# The other direction — a region of the window going OUT to a geographic source (GSHHG, a quake
# catalog, a tide model) — is `_box_to`, which the viewer reaches through `_on_lonlat_box`
# (sceneToLonLat, 30_app.cpp) before any such request leaves a window. Handing a projected window's
# metres to GMT as -R was "Geography FAILED … GMT error number = 74".

# THE geographic system of the data GMT and the bundled catalogs hand over (GSHHG, DCW, GADM, the
# quake and tsunami catalogs, solar, the plate files): plain lon/lat on WGS84.
const _LONLAT = "+proj=longlat +datum=WGS84 +no_defs"

# A CRS as ONE string GDAL takes; "" for unreferenced.
_crs_srs(c::CRS)::String = !isempty(c.proj4) ? c.proj4 : !isempty(c.wkt) ? c.wkt :
                          c.epsg != 0 ? "EPSG:$(c.epsg)" : ""

# The window's own system; "" when it carries none.
_window_srs(scene::Ptr{Cvoid})::String = _crs_srs(try _window_crs(scene) catch; NO_CRS end)

# Do two systems need a crossing at all? Same string, or both lon/lat (the datum shift between two
# geographic systems is below anything a screen shows), means no.
_same_srs(a::AbstractString, b::AbstractString)::Bool =
	a == b || (_project_is_geog(a) && _project_is_geog(b))

# Points from `from` to `to` — THE transform (`_vector_xy_project`, the call Projection uses).
_srs_xy(M::Matrix{Float64}, from::AbstractString, to::AbstractString) =
	_vector_xy_project(M, String(from), String(to))

# The box in system `to` that holds the rectangle W/E/S/N of system `from`. Its EDGES are walked, not
# only its corners: meridians, parallels and grid lines bend across systems, so the extreme value can
# lie between two corners.
function _box_to(from::AbstractString, to::AbstractString, W, E, S, N)::NTuple{4,Float64}
	t  = range(0.0, 1.0; length = 33)
	xs = vcat(W .+ (E - W) .* t, fill(Float64(E), 33), E .- (E - W) .* t, fill(Float64(W), 33))
	ys = vcat(fill(Float64(S), 33), S .+ (N - S) .* t, fill(Float64(N), 33), N .- (N - S) .* t)
	L  = _srs_xy(hcat(xs, ys), from, to)
	x0, x1 = extrema(view(L, :, 1));  y0, y1 = extrema(view(L, :, 2))
	_project_is_geog(to) && (y0 = max(y0, -90.0);  y1 = min(y1, 90.0))
	return (x0, x1, y0, y1)
end

# C callback (JuliaLonLatBoxFn): `box` = W,E,S,N of the window in its own units, overwritten with the
# lon/lat box that covers it. 1 = converted, 0 = the window is already lon/lat or unreferenced (box
# untouched). Never throws — an exception must not unwind through the viewer's event loop.
function _on_lonlat_box(scene::Ptr{Cvoid}, box::Ptr{Cdouble})::Cint
	try
		dst = _window_srs(scene)
		(isempty(dst) || _project_is_geog(dst)) && return Cint(0)
		b = unsafe_wrap(Array, box, 4)
		W, E, S, N = _box_to(dst, _LONLAT, b[1], b[2], b[3], b[4])
		b[1] = W;  b[2] = E;  b[3] = S;  b[4] = N
		return Cint(1)
	catch e
		_tool_failed(scene, "lon/lat region", e)
		return Cint(0)
	end
end

function _register_lonlat_box()
	fptr = @cfunction((s, b) -> Base.invokelatest(_on_lonlat_box, s, b), Cint, (Ptr{Cvoid}, Ptr{Cdouble}))
	ccall(_fn(:gmtvtk_set_lonlat_box_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# Everything a crossing from `src` into this window needs, worked out once per call: both systems,
# the window's footprint in ITS units (`frame`) and in the DATA's (`box`). `nothing` = no crossing
# (same system, or either side unreferenced). A window with no raster yet has no footprint: then
# nothing is cut, only transformed.
function _xform_ctx(scene::Ptr{Cvoid}, src::AbstractString)
	dst = _window_srs(scene)
	(isempty(src) || isempty(dst) || _same_srs(src, dst)) && return nothing
	f = _seis_display_frame(scene)
	f === nothing && return (src = String(src), dst = dst, frame = nothing, box = nothing)
	return (src = String(src), dst = dst, frame = f, box = _box_to(dst, src, f...))
end

# The window's footprint as a box in system `src` (the frame itself when no crossing is needed), or
# nothing when it shows no raster. What a catalog in `src` is CROPPED with.
function _frame_in(scene::Ptr{Cvoid}, src::AbstractString)
	ctx = _xform_ctx(scene, src)
	ctx === nothing && return _seis_display_frame(scene)
	return ctx.box
end

# Longitude `x` brought into the turn that starts at `W` (box and data may use -180..180 or 0..360).
_lon_into(x::Float64, W::Float64)::Float64 = mod(x - W, 360.0) + W

_inbox(x, y, b) = (b[1] <= x <= b[2]) && (b[3] <= y <= b[4])

# Bring the first column of `M` (data in system `srs`) into the longitude turn starting at `W` — only
# when `srs` is geographic; a projected x is a distance and has no turns.
function _wrap_lon!(M::Matrix{Float64}, srs::AbstractString, W::Float64)
	_project_is_geog(srs) || return M
	for r in 1:size(M, 1)
		isnan(M[r, 1]) || (M[r, 1] = _lon_into(M[r, 1], W))
	end
	return M
end

"""
    _to_window(scene, src, x, y) -> (x′, y′, keep)

POINTS in system `src` into the window's coordinates. When a crossing is needed only the points on the
window's footprint survive and `keep` lists their indices, so whatever else the caller carries per
point (sizes, colours, tooltips, table rows) follows them. `keep === nothing`: nothing moved.
"""
function _to_window(scene::Ptr{Cvoid}, src::AbstractString, x, y)
	ctx = _xform_ctx(scene, src)
	ctx === nothing && return (x, y, nothing)
	n = length(x)
	M = hcat(Float64.(collect(x)), Float64.(collect(y)))
	ctx.box === nothing || _wrap_lon!(M, ctx.src, ctx.box[1])
	cand = ctx.box === nothing ? collect(1:n) : [i for i in 1:n if _inbox(M[i, 1], M[i, 2], ctx.box)]
	isempty(cand) && return (Float64[], Float64[], Int[])
	Q = _srs_xy(M[cand, :], ctx.src, ctx.dst)
	ctx.frame === nothing || _wrap_lon!(Q, ctx.dst, ctx.frame[1])
	keep = ctx.frame === nothing ? collect(1:length(cand)) :
	       [k for k in 1:length(cand) if _inbox(Q[k, 1], Q[k, 2], ctx.frame)]
	return Q[keep, 1], Q[keep, 2], cand[keep]
end

"""
    _lines_to_window(scene, src, mats) -> (pieces, owner, rowsrc)

LINES in system `src` into the window's coordinates. `mats` are segment matrices with x, y in the
first two columns; every further column rides along (interpolated where a line is cut). Each segment
is cut to the footprint, transformed, and cut again to the footprint in the window's own units, so a
line ends exactly on the frame. `owner[k]` is the segment `pieces[k]` came from (per-segment info
follows it); `rowsrc[k][r]` the source row each vertex stands for (per-vertex text follows it).
No crossing needed: `mats` unchanged.
"""
function _lines_to_window(scene::Ptr{Cvoid}, src::AbstractString, mats::AbstractVector)
	ctx = _xform_ctx(scene, src)
	ctx === nothing && return (mats, collect(1:length(mats)), [collect(1:size(m, 1)) for m in mats])
	pieces = Matrix{Float64}[];  owner = Int[];  rowsrc = Vector{Int}[]
	for (i, m) in enumerate(mats)
		size(m, 1) == 0 && continue
		M = Matrix{Float64}(m)
		if ctx.box === nothing
			push!(pieces, M);  push!(owner, i);  push!(rowsrc, collect(1:size(M, 1)))
			continue
		end
		_wrap_lon!(M, ctx.src, ctx.box[1])
		ps, rs = _clip_runs(M, ctx.box...)
		append!(pieces, ps);  append!(owner, fill(i, length(ps)));  append!(rowsrc, rs)
	end
	isempty(pieces) && return (pieces, owner, rowsrc)
	# ONE transform call for every vertex of every piece.
	Q = _srs_xy(reduce(vcat, [p[:, 1:2] for p in pieces]), ctx.src, ctx.dst)
	ctx.frame === nothing || _wrap_lon!(Q, ctx.dst, ctx.frame[1])
	k = 0
	for p in pieces
		n = size(p, 1)
		p[:, 1:2] .= view(Q, k+1:k+n, :)
		k += n
	end
	ctx.frame === nothing && return (pieces, owner, rowsrc)
	out = Matrix{Float64}[];  own2 = Int[];  rs2 = Vector{Int}[]
	for (j, p) in enumerate(pieces)
		ps, rs = _clip_runs(p, ctx.frame...)
		append!(out, ps);  append!(own2, fill(owner[j], length(ps)))
		append!(rs2, [rowsrc[j][r] for r in rs])
	end
	return out, own2, rs2
end

# Cut a polyline (x, y in columns 1:2, any further columns carried along) to the rectangle W/E/S/N.
# Liang–Barsky on every edge: a line leaving the box ends ON the boundary, at an interpolated vertex,
# and a line re-entering starts a new piece. Returns the pieces (two vertices or more) and, per piece,
# the source row each vertex stands for (an interpolated one takes the nearer end), so per-vertex
# attributes that cannot be interpolated — a text label — still follow their point.
function _clip_runs(M::Matrix{Float64}, W, E, S, N)
	pieces = Matrix{Float64}[];  rows = Vector{Int}[]
	n = size(M, 1)
	cur = Vector{Float64}[];  cr = Int[]
	function flush!()
		if length(cur) >= 2
			push!(pieces, permutedims(reduce(hcat, cur)));  push!(rows, copy(cr))
		end
		empty!(cur);  empty!(cr)
	end
	if n == 1
		_inbox(M[1, 1], M[1, 2], (W, E, S, N)) && (push!(pieces, copy(M)); push!(rows, [1]))
		return pieces, rows
	end
	for i in 1:n-1
		a = M[i, :];  b = M[i+1, :]
		if isnan(a[1]) || isnan(a[2]) || isnan(b[1]) || isnan(b[2])
			flush!();  continue
		end
		dx = b[1] - a[1];  dy = b[2] - a[2]
		t0, t1 = 0.0, 1.0;  vis = true
		for (p, q) in ((-dx, a[1] - W), (dx, E - a[1]), (-dy, a[2] - S), (dy, N - a[2]))
			if p == 0.0
				q < 0.0 && (vis = false; break)
			else
				r = q / p
				if p < 0.0
					r > t1 && (vis = false; break)
					r > t0 && (t0 = r)
				else
					r < t0 && (vis = false; break)
					r < t1 && (t1 = r)
				end
			end
		end
		if !vis
			flush!();  continue
		end
		t0 > 0.0 && flush!()                                     # entering: a new piece starts here
		if isempty(cur)
			push!(cur, t0 == 0.0 ? a : a .+ t0 .* (b .- a));  push!(cr, t0 <= 0.5 ? i : i + 1)
		end
		push!(cur, t1 == 1.0 ? b : a .+ t1 .* (b .- a));  push!(cr, t1 >= 0.5 ? i + 1 : i)
		t1 < 1.0 && flush!()                                     # leaving: this piece ends on the edge
	end
	flush!()
	return pieces, rows
end

"""
    _polys_to_window(scene, src, rings) -> rings′

FILLED AREAS (closed rings, x, y in system `src`) into the window's coordinates, cut to its footprint
as AREAS — a polygon clip, not a line cut, so a night side that covers the whole window comes back as
the whole window. No crossing needed: `rings` unchanged.
"""
function _polys_to_window(scene::Ptr{Cvoid}, src::AbstractString, rings::AbstractVector)
	ctx = _xform_ctx(scene, src)
	ctx === nothing && return rings
	out = Matrix{Float64}[]
	for R in rings
		size(R, 1) >= 4 || continue
		M = Matrix{Float64}(R[:, 1:2])
		ctx.box === nothing || _wrap_lon!(M, ctx.src, ctx.box[1])
		parts = ctx.box === nothing ? [M] : _clip_poly_rect(M, ctx.box)
		for P in parts
			Q = _srs_xy(P, ctx.src, ctx.dst)
			ctx.frame === nothing || _wrap_lon!(Q, ctx.dst, ctx.frame[1])
			append!(out, ctx.frame === nothing ? [Q] : _clip_poly_rect(Q, ctx.frame))
		end
	end
	return out
end

# A ring cut to a rectangle as an AREA, through GMT.clipbyrect (GDAL) on a polygon geometry.
function _clip_poly_rect(M::Matrix{Float64}, b)::Vector{Matrix{Float64}}
	D = GMT.mat2ds(M; geom = GMT.wkbPolygon)
	C = GMT.clipbyrect([D], Float64[b[1], b[2], b[3], b[4]])
	(C === nothing || isempty(C)) && return Matrix{Float64}[]
	segs = C isa GMTdataset ? [C] : C
	return [Matrix{Float64}(s.data[:, 1:2]) for s in segs if size(s.data, 1) >= 4]
end

# How many of the window's own units make one kilometre — for a symbol whose size is a ground
# distance (a focal mechanism's radius). Read off the system's own unit; metres when it names none.
function _srs_units_per_km(srs::AbstractString)::Float64
	m = match(r"\+to_meter=([0-9.eE+-]+)", srs)
	m !== nothing && return 1000.0 / parse(Float64, m.captures[1])
	u = match(r"\+units=(\S+)", srs)
	u === nothing && return 1000.0
	return u.captures[1] == "km" ? 1.0 : u.captures[1] == "ft" ? 3280.839895 :
	       u.captures[1] == "us-ft" ? 3280.833333 : 1000.0
end

# A table's OWN referencing system: its proj4/WKT/EPSG when it carries one; with none, lon/lat when
# the same range test every other "is this table geographic?" question uses (`_measure_isgeog`) says
# so; otherwise "" — a plain table in nobody's system, drawn in the window's units as it always was.
function _dataset_srs(D)::String
	d1 = D isa GMTdataset ? D : first(D)
	s = !isempty(d1.proj4) ? d1.proj4 : !isempty(d1.wkt) ? d1.wkt : ""
	!isempty(s) && return s
	return _measure_isgeog(d1, "") ? _LONLAT : ""
end

"""
    _dataset_to_window(scene, D; kind=nothing) -> D′ | nothing

A TABLE (one GMTdataset or a vector of them) into the window's coordinates, from its own system
(`_dataset_srs`). Points keep the rows on the window's footprint; lines are cut to it; every column
and per-row text rides along. The result is stamped with the window's system, so passing it through
here again is a no-op. No crossing needed: `D` as it is. `nothing` when nothing lands on the window.
"""
function _dataset_to_window(scene::Ptr{Cvoid}, D; kind::Union{Nothing,Symbol} = nothing)
	segs = D isa GMTdataset ? [D] : collect(D)
	isempty(segs) && return D
	src = _dataset_srs(D)
	ctx = _xform_ctx(scene, src)
	ctx === nothing && return D
	mats = [Matrix{Float64}(s.data) for s in segs]
	out = GMTdataset[]
	# `kind` = how the caller is going to draw the rows; by default the same classifier the overlay
	# add uses. Points are crossed one by one, lines are cut and crossed as lines.
	if something(kind, _drop_overlay_kind(D)) === :points
		for (s, m) in zip(segs, mats)
			x, y, keep = _to_window(scene, src, view(m, :, 1), view(m, :, 2))
			keep === nothing && (keep = collect(1:size(m, 1)))
			isempty(keep) && continue
			mm = m[keep, :];  mm[:, 1] .= x;  mm[:, 2] .= y
			push!(out, _ds_like(s, mm, isempty(s.text) ? String[] : s.text[keep], ctx.dst))
		end
	else
		pieces, owner, rowsrc = _lines_to_window(scene, src, mats)
		for k in eachindex(pieces)
			s = segs[owner[k]]
			push!(out, _ds_like(s, pieces[k], isempty(s.text) ? String[] : s.text[rowsrc[k]], ctx.dst))
		end
	end
	isempty(out) && return nothing
	return (D isa GMTdataset && length(out) == 1) ? out[1] : out
end

# A new segment carrying `s`'s own description (column names, header, attributes, geometry) over the
# data `m`, stamped with the system `srs` it is now in.
function _ds_like(s::GMTdataset, m::Matrix{Float64}, text, srs::AbstractString)
	d = GMT.mat2ds(m; geom = s.geom)
	d.colnames = s.colnames;  d.header = s.header;  d.attrib = s.attrib
	d.text = isempty(text) ? String[] : Vector{String}(text)
	startswith(srs, "+") || startswith(srs, "EPSG") ? (d.proj4 = String(srs)) : (d.wkt = String(srs))
	return d
end
