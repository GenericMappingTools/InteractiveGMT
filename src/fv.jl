# GMTfv solids & arbitrary meshes (view_fv). A GMTfv is a faces-vertices solid (cube, sphere,
# country polygons, ...). We pack its SHARED vertices + per-face polygon cells (any corner count)
# and either its per-face colours (flat shading) or a CPT z-ramp, and hand them to gmtvtk_view_fv.
# Mirrors GMTF3D fv_to_mesh's sides/indices packing.
#
# DESIGN NOTE: the public `view_fv` is a thin STUB. It parses the loose kwargs — resolving every
# `nothing`/auto into a CONCRETE value (a neutral empty array stands in for an absent option,
# never `nothing`) — then calls the typed worker `_view_fv`, whose signature has ONLY concrete
# types (no `Union{...,Nothing}`, no `nothing` defaults).

# Flatten a GMTfv into shared vertices + per-face (sides, 0-based indices). Iterates the face
# groups in order (matches _fv_facecolors so cell colours line up 1:1 with cells).
function _fv_pack(fv::GMTfv)
	V  = fv.verts
	nv = size(V, 1)
	xyz = Vector{Float64}(undef, 3nv)
	@inbounds for i in 1:nv
		xyz[3i-2] = V[i, 1];  xyz[3i-1] = V[i, 2];  xyz[3i] = V[i, 3]
	end
	sides   = Cint[]
	indices = Cint[]
	for Fm in fv.faces
		isempty(Fm) && continue
		nf, npf = size(Fm)
		for r in 1:nf
			push!(sides, Cint(npf))
			for a in 1:npf;  push!(indices, Cint(Fm[r, a] - 1));  end
		end
	end
	return xyz, sides, indices
end

# Per-face RGB (3*nfaces UInt8) from the GMTfv's "-G" colour strings, in the SAME face order as
# _fv_pack. Empty Vector{UInt8} (the neutral element) when the solid carries no colours.
function _fv_facecolors(fv::GMTfv)
	(isempty(fv.color) || !any(!isempty, fv.color)) && return UInt8[]
	out = UInt8[]
	for (g, Fm) in enumerate(fv.faces)
		isempty(Fm) && continue
		nf = size(Fm, 1)
		cg = g <= length(fv.color) ? fv.color[g] : String[]
		for r in 1:nf
			rr, gg, bb = _parse_gmt_color(r <= length(cg) ? cg[r] : "")
			push!(out, rr, gg, bb)
		end
	end
	return out
end

# Per-face mean z (true z), in the SAME face order as _fv_pack -> drives the faceted CPT colouring
# that MATCHES the colorbar (each face = its mean-z colour through the same ramp).
function _fv_facez(fv::GMTfv)
	out = Float64[]
	V = fv.verts
	for Fm in fv.faces
		isempty(Fm) && continue
		nf, npf = size(Fm)
		for r in 1:nf
			zsum = 0.0
			@inbounds for c in 1:npf;  zsum += V[Fm[r, c], 3];  end
			push!(out, zsum / npf)
		end
	end
	return out
end

# bbox (xmin,xmax,ymin,ymax,zmin,zmax) — use fv.bbox when present, else from the vertices.
function _fv_bbox(fv::GMTfv)
	(length(fv.bbox) >= 6) && return Float64.(fv.bbox[1:6])
	V = fv.verts
	xmn, xmx = extrema(@view V[:, 1]);  ymn, ymx = extrema(@view V[:, 2]);  zmn, zmx = extrema(@view V[:, 3])
	return Float64[xmn, xmx, ymn, ymx, zmn, zmx]
end

"""
	view_fv(fv::GMTfv; cmap=:turbo, color=true, edges=false, geographic=nothing, title=...)

Show a `GMTfv` (faces-vertices solid or polygon mesh — cube, sphere, country polygons, …) in
the Qt + VTK viewer. Returns a `QtFV` handle immediately; the window stays live while you keep
using the REPL.

- `color` controls the face colouring and the colorbar:
  - `true` (default) — faceted per-face colour by each face's mean **z** through `cmap`; the
	colorbar shows that exact ramp (colours MATCH the bar).
  - `false` — smooth per-vertex z colouring through `cmap` (also matches the bar).
  - `:explicit` — honour the solid's own baked per-face colours (e.g. a hand-coloured cube);
	these are categorical, so NO colorbar is drawn.
  (`cmap` is any GMT name; `nothing` = the viewer's built-in ramp.)
- `edges` shows the mesh cell wires (toggle live with **`e`** in the viewer).
- `geographic` is auto-detected (override with `true`/`false`) — only affects the axis titles.

The vertical exaggeration comes from `fv.zscale` (set by `poly2fv`/`tri2fv`); 1 = true 1:1.
E.g. `view_fv(poly2fv(GMT.gmtread("countries.gmt")))`.
"""
# Resolve a GMTfv + colour options to the CONCRETE render arrays the C API takes — never `nothing`;
# an empty array is the neutral element (a C NULL = "absent"). Shared by the new-window path
# (`view_fv`/`_view_fv`) and the in-place 3-D Bodies path (`_promote_fv`, solids.jl):
#   facergb -> explicit per-face RGB (direct colours, no colorbar)
#   facez   -> per-face mean z       (faceted CPT colours that MATCH the colorbar)
#   cz/crgb -> CPT control nodes     (for the z-mapped modes)
# Returns (xyz, sides, indices, facergb, facez, cz, crgb, ncolor, bb, geog, zscale).
function _fv_resolve(fv::GMTfv; cmap=:turbo, color=true, geographic::Union{Bool,Nothing}=nothing)
	xyz, sides, indices = _fv_pack(fv)
	isempty(sides) && error("GMTfv has no faces to render")
	bb   = _fv_bbox(fv)
	geog = geographic === nothing ? GMT.isgeog(fv) : Bool(geographic)
	facergb = UInt8[];  facez = Float64[];  cz = Float64[];  crgb = Float64[];  ncolor = 0
	(color === :explicit) && (facergb = _fv_facecolors(fv))
	if isempty(facergb)                                        # z-mapped colouring (matches the bar)
		cz, crgb, ncolor = _cpt_nodes_range(bb[5], bb[6], cmap)
		(color === false) || (facez = _fv_facez(fv))          # true -> faceted per-face z; false -> smooth per-vertex
	end
	zscale = fv.zscale > 0 ? Float64(fv.zscale) : 1.0
	return (xyz, sides, indices, facergb, facez, cz, crgb, ncolor, bb, geog, zscale)
end

function view_fv(fv::GMTfv; cmap=:turbo, color=true, edges::Bool=false,
				 geographic::Union{Bool,Nothing}=nothing,
				 objname::AbstractString="",
				 title::AbstractString="GMT FV Viewer)")
	# --- STUB: resolve every kwarg to a CONCRETE value, then call the typed worker ----------
	xyz, sides, indices, facergb, facez, cz, crgb, ncolor, bb, geog, zscale =
		_fv_resolve(fv; cmap=cmap, color=color, geographic=geographic)
	return _view_fv(fv, xyz, sides, indices, facergb, facez, cz, crgb, ncolor,
					bb[1], bb[2], bb[3], bb[4], bb[5], bb[6], geog, zscale, edges, String(title), String(objname))
end

# The real worker — CONCRETE types only (no Union/Nothing). An empty array passes a C NULL, which
# the C side reads as "absent": no explicit colours / no per-face z / built-in ramp.
function _view_fv(fv::GMTfv, xyz::Vector{Float64}, sides::Vector{Cint}, indices::Vector{Cint},
				  facergb::Vector{UInt8}, facez::Vector{Float64}, cz::Vector{Float64},
				  crgb::Vector{Float64}, ncolor::Int,
				  x0::Float64, x1::Float64, y0::Float64, y1::Float64, z0::Float64, z1::Float64,
				  geographic::Bool, zscale::Float64, edges::Bool, title::String, objname::String)
	nv     = length(xyz) ÷ 3
	nfaces = length(sides)
	fc  = isempty(facergb) ? Ptr{Cuchar}(C_NULL)  : pointer(facergb)
	fzp = isempty(facez)   ? Ptr{Cdouble}(C_NULL) : pointer(facez)
	czp = isempty(cz)      ? Ptr{Cdouble}(C_NULL) : pointer(cz)
	cgp = isempty(crgb)    ? Ptr{Cdouble}(C_NULL) : pointer(crgb)
	# gmtvtk_view_fv copies all arrays into VTK during the call -> no keep-alive needed afterwards.
	h = GC.@preserve xyz sides indices facergb facez cz crgb ccall(_fn(:gmtvtk_view_fv), Ptr{Cvoid},
		(Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Ptr{Cint}, Ptr{Cuchar}, Ptr{Cdouble},
		 Ptr{Cdouble}, Ptr{Cdouble}, Cint,
		 Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble,
		 Cint, Cdouble, Cint, Cstring, Cstring),
		xyz, Cint(nv), sides, Cint(nfaces), indices, fc, fzp,
		czp, cgp, Cint(ncolor),
		x0, x1, y0, y1, z0, z1,
		Cint(geographic), zscale, Cint(edges), title, objname)
	fig = _register_fig!(QtFV(h, fv))
	_apply_crs!(fig, crs_from(fv; geographic=geographic))   # store the CRS + reveal the Geography menu if referenced
	_start_pump()
	return fig
end

# ─── named solids catalogue (view_fv("cube"), …) ─────────────────────────────
# The generators (cube, sphere, torus, icosahedron, …) are GMT.jl functions; each SOLIDS entry
# forwards every kwarg untouched so the generator's OWN defaults stand. An optional positional (a
# primitive's circumradius `r`) uses a `missing` sentinel: omit it -> the generator default; give
# it -> passed positionally. Generators with a REQUIRED positional (cylinder r/h, revolve curve,
# loft C1/C2, extrude shape/h) carry DEMO sample data only.

# Default demo profiles for the parametric generators (used when the caller gives none).
_demo_revolve_curve() = (x = collect(range(0, 2pi, length=15)) .+ 1; [x zeros(length(x)) -cos.(x)])
function _demo_loft_curves()                       # circle base -> 6-lobe star top
	t = range(0, 2pi, 75);  r = 5.0
	C1 = [r .* cos.(t) r .* sin.(t) zeros(length(t))]
	f  = tt -> r + 2.0 * sin(6tt)
	C2 = stack([(f(tt) * cos(tt), f(tt) * sin(tt), 3.0) for tt in t])'
	return C1, C2
end
function _demo_extrude_shape()                     # 5-point star outline (Mx2)
	a  = range(pi/2, 2pi + pi/2, 11)[1:10]
	rr = [isodd(k) ? 2.0 : 0.8 for k in 1:10]
	return [rr .* cos.(a) rr .* sin.(a)]
end

const SOLIDS = Dict{String,Function}(
	# closed primitives — `r` (optional) is the circumradius (centre→vertex), not the side
	"icosahedron" => (; r=missing, kw...) -> ismissing(r) ? icosahedron(; kw...) : icosahedron(r; kw...),
	"octahedron"  => (; r=missing, kw...) -> ismissing(r) ? octahedron(; kw...)  : octahedron(r; kw...),
	"dodecahedron"=> (; r=missing, kw...) -> ismissing(r) ? dodecahedron(; kw...) : dodecahedron(r; kw...),
	"tetrahedron" => (; r=missing, kw...) -> ismissing(r) ? tetrahedron(; kw...) : tetrahedron(r; kw...),
	"cube"        => (; r=missing, kw...) -> ismissing(r) ? cube(; kw...)        : cube(r; kw...),
	"sphere"      => (; r=missing, kw...) -> ismissing(r) ? sphere(; kw...)      : sphere(r; kw...),
	"torus"       => (; kw...)            -> torus(; kw...),                       # all-keyword
	# generators / required positionals — demo SAMPLE data only; optional kwargs pass through
	"cylinder"    => (; r=1.0, h=3.0, kw...)                -> cylinder(r, h; kw...),
	"revolve"     => (; curve=_demo_revolve_curve(), kw...) -> revolve(curve; kw...),
	"loft"        => (; C1=_demo_loft_curves()[1], C2=_demo_loft_curves()[2], kw...) -> loft(C1, C2; kw...),
	"extrude"     => (; shape=_demo_extrude_shape(), h=1.0, kw...) -> extrude(shape, h; kw...),
)

# Colour a solid's faces with a hue ramp keyed on face-centroid z, filling fv.color with GMT
# "-G#rrggbb" strings so a colourless generated solid renders FACETED per-face colour (the
# viewer's faceted direct-colour path) instead of a smooth CPT ramp. Mirrors GMTF3D colorize_by_z!.
function colorize_by_z!(fv::GMTfv)
	V = fv.verts
	zmin, zmax = extrema(@view V[:, 3])
	span = (zmax > zmin) ? (zmax - zmin) : 1.0
	fv.color = Vector{Vector{String}}(undef, length(fv.faces))
	for (g, Fm) in enumerate(fv.faces)
		if isempty(Fm)
			fv.color[g] = String[];  continue
		end
		nf, npf = size(Fm)
		cols = Vector{String}(undef, nf)
		for r in 1:nf
			zc = sum(V[Fm[r, c], 3] for c in 1:npf) / npf
			t  = (zc - zmin) / span
			rr = round(Int, 255 * t);  bb = round(Int, 255 * (1 - t));  gg = round(Int, 80 + 100 * (1 - abs(2t - 1)))
			cols[r] = string("-G#", lpad(string(rr, base=16), 2, '0'), lpad(string(gg, base=16), 2, '0'),
									lpad(string(bb, base=16), 2, '0'))
		end
		fv.color[g] = cols
	end
	return fv
end

"""
	view_fv(name::AbstractString; color=true, cmap=:turbo, edges=false, geographic=nothing,
			title="", solid_kwargs...)

Show a NAMED GMT solid from the [`SOLIDS`](@ref) catalogue (`"cube"`, `"sphere"`, `"torus"`,
`"icosahedron"`, `"octahedron"`, `"dodecahedron"`, `"tetrahedron"`, `"cylinder"`, `"revolve"`,
`"loft"`, `"extrude"`). The solid takes its OWN parameters — any kwarg that is NOT a viewer
keyword is forwarded untouched to the GMT generator:

	view_fv("cube"; r=3)                 # r = circumradius (centre→vertex)
	view_fv("sphere"; n=4)               # sphere's own subdivision level
	view_fv("torus"; R=8, nx=200, edges=true)
	view_fv("revolve"; curve=mycurve)    # your own profile (else a demo profile)

`color` behaves as in [`view_fv(::GMTfv)`](@ref): `true` (default) = FACETED per-face colour by
mean z through `cmap` with a MATCHING colorbar; `false` = smooth per-vertex z; `:explicit` =
the solid's own baked colours (no colorbar). The remaining viewer kwargs (`cmap`, `edges`,
`geographic`, `title`) behave the same.
"""
function view_fv(name::AbstractString; color=true, cmap=:turbo, edges::Bool=false,
				 geographic::Union{Bool,Nothing}=nothing, title::AbstractString="", kwargs...)
	lname   = lowercase(String(name))
	builder = get(SOLIDS, lname, nothing)
	builder === nothing &&
		error("unknown solid '$name'. Choose one of: $(join(sort(collect(keys(SOLIDS))), ", "))")
	fv = builder(; kwargs...)                       # every non-viewer kwarg -> the GMT generator
	ttl = isempty(title) ? "GMT $lname  (Qt + VTK)" : String(title)
	return view_fv(fv; color=color, cmap=cmap, edges=edges, geographic=geographic,
				   objname=uppercasefirst(lname), title=ttl)   # Scene Objects checkbox = the solid name
end

# ─── poly2fv (GMTdataset polygons -> GMTfv) ──────────────────────────────────
# Folds closed 3-D polygons into ONE GMTfv (one face per polygon, any corner count); faces
# coloured by mean true-z through `cmap`. Vertical scale via the same geog-aware :auto logic as
# view_grid (see _resolve_zscale in cpt.jl).

"""
	poly2fv(D::Vector{<:GMTdataset}; cmap=:turbo, zscale=:auto, vfrac=0.2, vexag=:auto,
			isgeog=false, ncolor=256, triangulate=false) -> GMTfv

Fold a vector of **closed 3-D polygons** into a single coloured `GMTfv` ready for [`view_fv`](@ref)
— one mesh face per polygon, any corner count. Each polygon needs `x y z` columns; a repeated
closing vertex is dropped. Faces are coloured by their mean *z* through the GMT colormap `cmap`.
Pass `triangulate=true` to fan-split every polygon into triangles (concave / non-planar polys).
"""
function poly2fv(D::Vector{<:GMTdataset}; cmap=:turbo, zscale=:auto, vfrac=0.2, vexag=:auto,
				 isgeog::Bool=false, ncolor::Int=256, triangulate::Bool=false)
	isempty(D) && error("no polygons to render")
	faces_xyz = Matrix{Float64}[]
	for d in D
		P = Matrix{Float64}(d.data)
		size(P, 2) >= 3 || error("polygon needs 3-D vertices (x y z); got $(size(P,2)) columns")
		(size(P, 1) >= 2 && @views P[1, 1:3] == P[end, 1:3]) && (P = P[1:end-1, :])
		size(P, 1) >= 3 || continue
		if triangulate
			for t in 2:size(P, 1)-1;  push!(faces_xyz, P[[1, t, t + 1], :]);  end
		else
			push!(faces_xyz, P)
		end
	end

	nf = length(faces_xyz)
	nf == 0 && error("no usable polygon faces (need >= 3 vertices each)")
	ncorners = sum(size(P, 1) for P in faces_xyz)
	V  = Matrix{Float64}(undef, ncorners, 3)
	zc = Vector{Float64}(undef, nf)
	buckets = Dict{Int,Vector{Tuple{Int,Vector{Int}}}}()
	vi = 0

	for (k, P) in enumerate(faces_xyz)
		np  = size(P, 1)
		row = Vector{Int}(undef, np);  zsum = 0.0
		for c in 1:np
			vi += 1
			V[vi, 1] = P[c, 1];  V[vi, 2] = P[c, 2];  V[vi, 3] = P[c, 3]
			row[c] = vi;  zsum += P[c, 3]
		end
		zc[k] = zsum / np
		push!(get!(buckets, np, Tuple{Int,Vector{Int}}[]), (k, row))
	end

	xmin, xmax = extrema(@view V[:, 1]);  ymin, ymax = extrema(@view V[:, 2]);  zmin, zmax = extrema(@view V[:, 3])
	s = _resolve_zscale(zscale, xmax - xmin, ymax - ymin, zmax - zmin, vfrac, isgeog, vexag)
	czmin, czmax = extrema(zc)
	(czmax > czmin) || (czmin -= 0.5;  czmax += 0.5)
	step = (czmax - czmin) / ncolor
	cm = GMT.makecpt(cmap=string(cmap), range=(czmin, czmax, step)).colormap
	faces = Matrix{Int}[];  colors = Vector{Vector{String}}()

	for npf in sort(collect(keys(buckets)))
		rows = buckets[npf];  m = length(rows)
		Fm = Matrix{Int}(undef, m, npf);  col = Vector{String}(undef, m)
		for (j, (gk, row)) in enumerate(rows)
			@inbounds for c in 1:npf;  Fm[j, c] = row[c];  end
			col[j] = string("-G", _z_to_hex(zc[gk], cm, czmin, czmax))
		end
		push!(faces, Fm);  push!(colors, col)
	end
	bb = Float64[xmin, xmax, ymin, ymax, zmin, zmax]
	return GMT.GMTfv(verts=V, faces=faces, color=colors, bbox=bb, isflat=fill(false, length(faces)), zscale=s)
end
