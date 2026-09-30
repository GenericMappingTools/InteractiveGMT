# vectorwizard.jl — Tools > Vector Wizard: trace the picture a window shows into filled vector
# layers with the Potrace port (src/potrace). The C++ side (VectorWizardDialog, 70_window.cpp) only
# collects the settings and paints what this file pushes back; see JuliaVectorWizardFn, 30_app.cpp.
#
# The picture is the window's image, or — for a grid — the picture the grid is drawn as (its CPT over
# its data range, grdimage img_out). Three modes:
#   bw      one layer: the pixels darker than a threshold (lighter, with invert)
#   grey    N grey levels,  k-means on the luminance
#   colour  N colours,      k-means on RGB
# Grey/colour are traced STACKED: tones sorted light to dark, the lightest is the background, and
# layer k is "tone k or darker" — so the layers nest and leave no gaps between colours.
#
# Each traced layer lands in the window as an ordinary line overlay made FILLED (Overlay::filled,
# gmtvtk_overlay_set_filled_h), all under ONE group named for the source; Save writes SVG / EPS
# (potrace_svg / potrace_eps, layered) or a GMT multisegment file (" -G" per polygon, " -Ph" holes).

# Per-dialog state: the source picture, top row first, and where its pixel EDGES lie in the world.
mutable struct _VWState
	scene::Ptr{Cvoid}
	srcname::String
	A::Array{UInt8,3}                    # (h, w, nb), nb = 1 (grey) or 3 (RGB), row 1 = top
	region::NTuple{4,Float64}            # x0, x1, y0, y1 of the pixel edges
	proj4::String
	wkt::String
	epsg::Int
end

const _VWSTATE = Dict{Ptr{Cvoid}, _VWState}()

# ---------------------------------------------------------------------------------------------
# The source picture

# The CPT a grid is drawn with: its recorded colormap name (the session recipe carries it), else the
# default, over its data range — the same makecpt the viewer's LUT and the script export use.
function _vw_grid_picture(scene::Ptr{Cvoid}, name::String, G::GMTgrid)::GMTimage
	tag = ""
	for r in get(_SESSION_LOG, scene, ElementRecipe[])
		(r.name == name && haskey(r.params, "cmap")) && (tag = String(r.params["cmap"]))
	end
	isempty(tag) && (tag = String(_default_cmap(G)))
	zr = _script_zrange(G)
	zr === nothing && error("the grid has no finite values to draw")
	C = GMT.makecpt(cmap=Symbol(tag), range=zr, continuous=true)
	return GMT.grdimage(G, cmap=C, img_out=true)
end

# Does the window hold anything to trace? (An image or a grid; the same lookups _vw_source makes.)
_vw_has_source(scene::Ptr{Cvoid})::Bool =
	_find_object_named(scene, :image)[2] isa GMTimage || _find_object_named(scene, :grid)[2] isa GMTgrid

# The picture to trace: the image the caller named, else the window's image, else its grid. The
# window's PRIMARY raster is registered unnamed; its Scene Objects name ("surf_name") is what the
# user sees, so that is the name handed back.
function _vw_source(scene::Ptr{Cvoid}, wanted::String)
	if !isempty(wanted)
		I = _find_object(scene, :image, wanted)
		(I isa GMTimage) && return wanted, I, "image"
	end
	primary() = try String(get(_scene_state(scene), "surf_name", "")) catch; "" end
	nm, I = _find_object_named(scene, :image)
	(I isa GMTimage) && return (isempty(nm) ? primary() : nm), I, "image"
	nm, G = _find_object_named(scene, :grid)      # nm as registered: the recipe lookup keys on it
	(G isa GMTgrid) && return (isempty(nm) ? primary() : nm), _vw_grid_picture(scene, nm, G), "grid"
	error("this window has no image or grid to trace")
end

# (h, w, nb) UInt8 pixels, top row first, through the SAME accessor pair the drape/texture path uses
# (_pixaccess_img expands a palette, _north_first knows the row order), so what is traced is what
# the window shows. A 16-bit band is stretched to 8 bits first, as on display; alpha is dropped.
function _vw_pixels(I::GMTimage)::Array{UInt8,3}
	Iu = eltype(I.image) == UInt8 ? I : _stretch_to_u8(I)
	pix, nb, nlon, nlat, rowmajor = _pixaccess_img(Iu)
	nf = _north_first(Iu.layout, rowmajor)
	nc = nb >= 3 ? 3 : 1
	A = Array{UInt8}(undef, nlat, nlon, nc)
	@inbounds for b in 1:nc, c in 1:nlon, r in 1:nlat
		A[r, c, b] = pix(nf ? r : nlat - r + 1, c, b)
	end
	return A
end

# Where the pixel EDGES are: a pixel-registered raster's range already says so, a grid-registered
# one's is widened by half a cell.
function _vw_region(I::GMTimage)::NTuple{4,Float64}
	r = Float64.(I.range[1:4])
	I.registration == 1 && return (r[1], r[2], r[3], r[4])
	dx = length(I.inc) >= 1 ? Float64(I.inc[1]) / 2 : 0.5
	dy = length(I.inc) >= 2 ? Float64(I.inc[2]) / 2 : dx
	return (r[1] - dx, r[2] + dx, r[3] - dy, r[4] + dy)
end

# ---------------------------------------------------------------------------------------------
# Settings

struct _VWParams
	mode::Symbol          # :bw | :grey | :colour
	thr::Int
	inv::Bool
	n::Int
	pp::Potrace.PotraceParams
end

function _vw_params(kv::String)::_VWParams
	d = Dict{String,String}()
	for f in split(kv, ','; keepempty=false)
		p = split(f, '='; limit=2)
		length(p) == 2 && (d[strip(p[1])] = strip(p[2]))
	end
	num(k, def) = something(tryparse(Float64, get(d, k, "")), def)
	mode = Symbol(get(d, "mode", "bw"))
	mode in (:bw, :grey, :colour) || error("unknown mode '$mode'")
	pp = Potrace.PotraceParams(turdsize=round(Int, num("turd", 2)), turnpolicy=Symbol(get(d, "turn", "minority")),
	                           alphamax=num("alpha", 1.0), opticurve=num("opti", 1) != 0,
	                           opttolerance=num("tol", 0.2))
	return _VWParams(mode, clamp(round(Int, num("thr", 128)), 0, 255), num("inv", 0) != 0,
	                 clamp(round(Int, num("n", 8)), 2, 64), pp)
end

# ---------------------------------------------------------------------------------------------
# Tones

_vw_lum(r, g, b) = 0.299 * r + 0.587 * g + 0.114 * b

# Luminance of every pixel, (h, w).
function _vw_gray(A::Array{UInt8,3})::Matrix{Float64}
	size(A, 3) == 1 && return Float64.(A[:, :, 1])
	return _vw_lum.(Float64.(A[:, :, 1]), Float64.(A[:, :, 2]), Float64.(A[:, :, 3]))
end

# The centre (row of C) nearest to row i of M. A top-level function, not a closure inside the k-means:
# the closure over its reassigned centres was boxed and made the whole grouping ~50x slower.
function _vw_nearest(M::Matrix{Float64}, i::Int, C::Matrix{Float64}, K::Int, nb::Int)::Int
	bk = 1;  bd = Inf
	@inbounds for k in 1:K
		d = 0.0
		for b in 1:nb
			d += (M[i, b] - C[k, b])^2
		end
		d < bd && (bd = d; bk = k)
	end
	return bk
end

# k-means over the pixels of X (N x nb): centres fitted on a fixed subsample (deterministic: seeded
# at luminance quantiles, every stride-th pixel), then every pixel assigned to its nearest centre.
# Returns (centres K x nb, labels N).
function _vw_kmeans(X::Matrix{Float64}, K::Int; iters::Int=20, nsamp::Int=40000)
	N, nb = size(X)
	stride = max(1, N ÷ nsamp)
	S = X[1:stride:N, :]
	ns = size(S, 1)
	lum = nb == 3 ? _vw_lum.(S[:, 1], S[:, 2], S[:, 3]) : S[:, 1]
	o = sortperm(lum)
	C = S[o[round.(Int, range(1, ns; length=K))], :]
	nearest(M, i, C) = _vw_nearest(M, i, C, K, nb)
	lab = zeros(Int, ns)
	for _ in 1:iters
		for i in 1:ns
			lab[i] = nearest(S, i, C)
		end
		sums = zeros(K, nb);  cnt = zeros(Int, K)
		@inbounds for i in 1:ns
			k = lab[i];  cnt[k] += 1
			for b in 1:nb
				sums[k, b] += S[i, b]
			end
		end
		Cn = copy(C)
		for k in 1:K
			cnt[k] > 0 && (Cn[k, :] = sums[k, :] ./ cnt[k])      # an empty tone keeps its centre
		end
		Cn == C && break
		C = Cn
	end
	L = Vector{Int}(undef, N)
	for i in 1:N
		L[i] = nearest(X, i, C)
	end
	return C, L
end

# The tones of the picture: `rank` (h, w) = 1 (lightest) .. K (darkest) and `rgb` (K x 3, 0-255).
# Black & white is the K = 2 case of the same thing: rank 2 = the traced pixels.
function _vw_tones(A::Array{UInt8,3}, p::_VWParams)
	h, w, nb = size(A)
	if p.mode === :bw
		G = _vw_gray(A)
		sel = p.inv ? (G .>= p.thr) : (G .< p.thr)
		rank = map(s -> s ? 2 : 1, sel)
		rgb = zeros(2, 3)
		for (k, m) in ((1, .!sel), (2, sel))              # each side's mean colour
			any(m) || continue
			for b in 1:3
				rgb[k, b] = sum(Float64.(A[:, :, min(b, nb)])[m]) / count(m)
			end
		end
		return rank, rgb
	end
	X = p.mode === :grey ? reshape(_vw_gray(A), :, 1) : Float64.(reshape(A, h * w, nb))
	C, L = _vw_kmeans(X, p.n)
	lum = size(C, 2) == 3 ? _vw_lum.(C[:, 1], C[:, 2], C[:, 3]) : C[:, 1]
	ord = sortperm(lum; rev=true)                         # lightest first
	pos = invperm(ord)
	rank = reshape(pos[L], h, w)
	rgb = size(C, 2) == 3 ? C[ord, :] : repeat(C[ord, 1], 1, 3)
	return rank, rgb
end

# ---------------------------------------------------------------------------------------------
# Preview and trace

# The preview: every pixel painted its tone's colour (the traced pixels black/the rest white in B&W),
# decimated to the preview box, RGB row-major top row first.
function _vw_push_preview(st::_VWState, dlg::Ptr{Cvoid}, rank::Matrix{Int}, rgb::Matrix{Float64}, bw::Bool)
	h, w = size(rank)
	step = max(1, ceil(Int, max(w / 880, h / 660)))
	rows = 1:step:h;  cols = 1:step:w
	buf = Vector{UInt8}(undef, 3 * length(rows) * length(cols))
	k = 1
	@inbounds for r in rows, c in cols
		t = rank[r, c]
		for b in 1:3
			buf[k] = bw ? (t == 2 ? 0x00 : 0xff) : round(UInt8, clamp(rgb[t, b], 0, 255));  k += 1
		end
	end
	ccall(_fn(:gmtvtk_vectorwizard_set_preview), Cvoid, (Ptr{Cvoid}, Cint, Cint, Ptr{UInt8}),
	      dlg, Cint(length(cols)), Cint(length(rows)), buf)
	return
end

_vw_text(dlg::Ptr{Cvoid}, which::Int, s::String) =
	ccall(_fn(:gmtvtk_vectorwizard_set_text), Cvoid, (Ptr{Cvoid}, Cint, Cstring), dlg, Cint(which), s)

_vw_hex(c) = "#" * join(string.(round.(Int, clamp.(c, 0, 255)); base=16, pad=2))

# Trace: one PotraceResult per tone above the background, "tone k or darker", in painting order.
# Returns (background "#rrggbb" or nothing, ["#rrggbb" => result, …]).
function _vw_trace(st::_VWState, p::_VWParams)
	rank, rgb = _vw_tones(st.A, p)
	K = size(rgb, 1)
	layers = Pair{String,Potrace.PotraceResult}[]
	for k in 2:K
		M = rank .>= k
		any(M) || continue
		push!(layers, _vw_hex(rgb[k, :]) => Potrace.potrace(Potrace.PotraceBitmap(M), p.pp))
	end
	bg = p.mode === :bw ? nothing : _vw_hex(rgb[1, :])       # B&W traces shapes, not a page
	return bg, layers
end

_vw_count(layers) = (sum(l -> length(Potrace.paths(l.second)), layers; init=0))

# The handle of this source's trace: "VW: <name>", the name without its file extension.
# A primary image with no name of its own is the row Scene Objects calls "Image" — so is the trace.
_vw_group(st::_VWState) = "VW: " * (isempty(st.srcname) ? "Image" : splitext(st.srcname)[1])

# The per-tone overlay names each window's traces use, by handle: the setters below find an overlay
# BY NAME (first match), so a tone of one trace may never share its name with a tone of another.
const _VW_NAMES = Dict{Ptr{Cvoid}, Dict{String,Vector{String}}}()

# The trace behind every product in a window, by key (Overlay::vwKey), so its handle can save it as
# SVG / EPS / PDF later. The key is the handle name it was born with: a rename does not orphan it.
const _VW_PRODUCTS = Dict{Ptr{Cvoid}, Dict{String,Tuple{_VWState,Union{Nothing,String},Vector{Pair{String,Potrace.PotraceResult}}}}}()

# The traced layers into the window: one FILLED line overlay per tone (the background as the frame
# rectangle) — a single layer IS the handle; several hang under one group. Replaced, never piled up,
# when the same source is traced again.
function _vw_add_to_window(st::_VWState, bg, layers)
	grp = _vw_group(st)
	ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), st.scene, grp)
	ccall(_fn(:gmtvtk_remove_overlay_named_h), Cint, (Ptr{Cvoid}, Cstring), st.scene, grp)
	names = get!(() -> Dict{String,Vector{String}}(), _VW_NAMES, st.scene)
	delete!(names, grp)
	taken = Set{String}(Iterators.flatten(values(names)))
	x0, x1, y0, y1 = st.region
	sets = Tuple{String,Vector{GMTdataset{Float64,2}}}[]
	if bg !== nothing
		frame = GMTdataset{Float64,2}(data=[x0 y0; x1 y0; x1 y1; x0 y1; x0 y0], colnames=["X", "Y"],
		                              proj4=st.proj4, wkt=st.wkt, epsg=st.epsg, geom=3)
		push!(sets, (bg, [frame]))
	end
	for (col, res) in layers
		D = Potrace.potrace_gmtds(res; region=st.region, proj4=st.proj4, wkt=st.wkt, epsg=st.epsg)
		# the " -Ph" hole flags mean nothing in the window (the fill finds holes by nesting), and a
		# segment header becomes that segment's HOVER text there
		foreach(d -> d.header = "", D)
		isempty(D) || push!(sets, (col, D))
	end
	single = length(sets) == 1
	mine = String[]
	for (k, (col, D)) in enumerate(sets)
		c = Potrace._rgb01(col)
		nm = single ? grp : "tone $k ($col)"
		n = 2
		while !single && nm in taken                        # another trace's tone already has it
			nm = "tone $k ($col) #$n";  n += 1
		end
		push!(mine, nm)
		_add_dataset_to_scene(st.scene, D, nm; groupName=single ? "" : grp, color=c, noConvertToPoints=true,
		                      noDataTable=true, forceMode=:lines)
		# a hairline outline in the fill colour: the area is the fill, the pen only seals the seams
		ccall(_fn(:gmtvtk_set_overlay_style_h), Cint, (Ptr{Cvoid}, Cstring, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cdouble),
		      st.scene, nm, c[1], c[2], c[3], 1.0, Cint(0), 1.0)
		ccall(_fn(:gmtvtk_overlay_set_filled_h), Cint, (Ptr{Cvoid}, Cstring, Cint), st.scene, nm, Cint(1))
		ccall(_fn(:gmtvtk_overlay_set_vwkey_h), Cint, (Ptr{Cvoid}, Cstring, Cstring), st.scene, nm, grp)
	end
	names[grp] = mine
	get!(() -> valtype(_VW_PRODUCTS)(), _VW_PRODUCTS, st.scene)[grp] = (st, bg, layers)
	return length(sets)
end

# Save by extension: .svg / .eps / .pdf (pixel units, the page is the picture) or a GMT multisegment table
# .txt / .dat (world coordinates, one " -G" polygon per segment, holes " -Ph", the background first).
function _vw_save(st::_VWState, path::String, bg, layers)
	ext = lowercase(splitext(path)[2])
	if ext == ".svg"
		Potrace.potrace_svg(path, layers; background=bg)
	elseif ext == ".eps" || ext == ".ps"
		Potrace.potrace_eps(path, layers; background=bg)
	elseif ext == ".pdf"
		Potrace.potrace_pdf(path, layers; background=bg)
	elseif ext == ".txt" || ext == ".dat"
		out = GMTdataset{Float64,2}[]
		x0, x1, y0, y1 = st.region
		gcol(col) = join(round.(Int, 255 .* collect(Potrace._rgb01(col))), '/')
		bg === nothing || push!(out, GMTdataset{Float64,2}(data=[x0 y0; x1 y0; x1 y1; x0 y1; x0 y0],
		                         colnames=["X", "Y"], header=" -G$(gcol(bg))", geom=3))
		for (col, res) in layers
			for D in Potrace.potrace_gmtds(res; region=st.region)
				D.header = D.header == " -Ph" ? " -Ph" : " -G$(gcol(col))"
				push!(out, D)
			end
		end
		isempty(out) && error("nothing was traced")
		for D in out
			D.geom = 0                                    # a plain table: the headers carry the polygons
		end
		out[1].proj4 = st.proj4;  out[1].wkt = st.wkt;  out[1].epsg = st.epsg
		GMT.set_dsBB!(out)
		GMT.gmtwrite(path, out)
	else
		# not ".gmt": that is the OGR/GMT format, which has no place for the -G / -Ph headers
		error("unknown file type '$ext' (use .svg, .eps, .pdf, .txt or .dat)")
	end
	return
end

# ---------------------------------------------------------------------------------------------
# The C callback: params = "op \n arg \n key=value,…" (JuliaVectorWizardFn, 30_app.cpp).
function _on_vectorwizard(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cint
	op = ""
	try
		ln = split(unsafe_string(cparams), '\n')
		op = String(strip(ln[1]))
		arg = length(ln) >= 2 ? String(ln[2]) : ""
		kv = length(ln) >= 3 ? String(ln[3]) : ""
		if op == "close"
			delete!(_VWSTATE, dlg)
			return Cint(1)
		end
		if op == "saveproduct"                # a product's own "Save as SVG / EPS / PDF…": arg = path, line 3 = key
			pr = get(get(_VW_PRODUCTS, scene, valtype(_VW_PRODUCTS)()), strip(kv), nothing)
			pr === nothing && error("this trace is no longer available (traced in an earlier session?)")
			path = String(strip(arg))
			lowercase(splitext(path)[2]) in (".svg", ".eps", ".pdf") || error("save as .svg, .eps or .pdf")
			_vw_save(pr[1], path, pr[2], pr[3])
			return Cint(1)
		end
		p = _vw_params(kv)
		if op == "init"
			# Nothing in the window to trace is not a failure: 2 tells the dialog to ASK for a file.
			_vw_has_source(scene) || return Cint(2)
			nm, I, kind = _vw_source(scene, String(strip(arg)))
			A = _vw_pixels(I)
			crs_p4 = String(something(I.proj4, ""));  crs_wkt = String(something(I.wkt, ""))
			st = _VWState(scene, String(nm), A, _vw_region(I), crs_p4, crs_wkt, Int(I.epsg))
			_VWSTATE[dlg] = st
			h, w, nb = size(A)
			_vw_text(dlg, 0, "$(kind == "grid" ? "Grid picture" : "Image") \"$(isempty(nm) ? "(primary)" : nm)\" — " *
			                 "$(w) x $(h) px, $(nb == 3 ? "RGB" : "grey")")
			op = "preview"
		end
		st = get(_VWSTATE, dlg, nothing)
		(st isa _VWState) || error("this dialog has no picture loaded")
		if op == "preview"
			rank, rgb = _vw_tones(st.A, p)
			_vw_push_preview(st, dlg, rank, rgb, p.mode === :bw)
			_vw_text(dlg, 1, p.mode === :bw ? "Black: the pixels that will be traced." :
			                 "$(size(rgb, 1)) tones, lightest = background.")
		elseif op == "trace"
			t0 = time()
			bg, layers = _vw_trace(st, p)
			n = _vw_add_to_window(st, bg, layers)
			t = time() - t0
			_vw_text(dlg, 1, "$n filled layer(s), $(_vw_count(layers)) curves ($(round(t; digits=1)) s) " *
			                 "under \"$(_vw_group(st))\" in Scene Objects.")
		elseif op == "save"
			isempty(strip(arg)) && error("no file name")
			bg, layers = _vw_trace(st, p)
			_vw_save(st, String(strip(arg)), bg, layers)
			_vw_text(dlg, 1, "Saved $(length(layers)) layer(s), $(_vw_count(layers)) curves to $(strip(arg))")
		else
			error("unknown op '$op'")
		end
		return Cint(1)
	catch e
		_tool_failed(scene, "Vector Wizard ($op)", e)
		return Cint(0)
	end
end

function _register_vectorwizard()
	fptr = @cfunction((s, d, c) -> Base.invokelatest(_on_vectorwizard, s, d, c)::Cint, Cint,
	                  (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_vectorwizard_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
