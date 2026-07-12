# Vertical "curtains" (Fledermaus-style seismic / midwater profiles): an image hung on a
# vertical wall that follows an XY track THROUGH the scene, sharing the grid's coordinate space
# and vertical scale (mirrors GMTF3D's `vcurtain`). The wall is a quad strip (2 verts/column, top
# at zmax / bottom at zmin) textured with the image, drawn unlit so it shows true colour.

# XY track out of an N×2 matrix OR a GMTdataset (GMTdataset is AbstractArray, not Matrix).
_curtain_xy(P::AbstractMatrix) = (Float64.(P[:, 1]), Float64.(P[:, 2]))
_curtain_xy(D::GMTdataset)     = (Float64.(D.data[:, 1]), Float64.(D.data[:, 2]))

# Per-column horizontal texture coord u ∈ [0,1] along the track.
#   :distance — by cumulative chord length (a leg twice as long carries twice the image)
#   :simple   — even per point (first point = first column, last = last column)
function _curtain_u(px, py, spacing::Symbol)
	N = length(px)
	if spacing === :distance
		d = zeros(Float64, N)
		for i in 2:N;  d[i] = d[i-1] + hypot(px[i]-px[i-1], py[i]-py[i-1]);  end
		tot = d[end];  tot <= 0 && (tot = 1.0)
		return d ./ tot
	elseif spacing === :simple
		return N == 1 ? [0.0] : collect(0:N-1) ./ (N - 1)
	end
	error("unknown curtain spacing=:$spacing; choose :distance or :simple")
end

# Curtain texture as a FILE PATH that VTK decodes itself (correct orientation, no GMT layout
# ambiguity). A String is used as-is; a GMTimage is written to a temp PNG (gmtwrite handles its
# own layout). Returns the path the C side reads.
_curtain_texfile(path::AbstractString) =
	(isfile(path) || error("curtain image file not found: $path");  String(path))
function _curtain_texfile(I::GMTimage)
	p = tempname() * ".png"
	gmtwrite(p, I)
	return p
end

# Resample a polyline to `n` points evenly spaced by arc length (clip needs the density a
# 2-point input lacks). Mirrors GMTF3D's _densify_polyline.
function _densify_xy(px, py, n::Int)
	N = length(px)
	N == 1 && return (fill(Float64(px[1]), n), fill(Float64(py[1]), n))
	d = zeros(Float64, N)
	for i in 2:N;  d[i] = d[i-1] + hypot(px[i]-px[i-1], py[i]-py[i-1]);  end
	tot = d[end]
	tot <= 0 && return (fill(Float64(px[1]), n), fill(Float64(py[1]), n))
	xo = Vector{Float64}(undef, n);  yo = Vector{Float64}(undef, n);  j = 1
	for (k, sk) in enumerate(range(0.0, tot, length=n))
		while j < N && d[j+1] < sk;  j += 1;  end
		seg = d[j+1] - d[j];  t = seg <= 0 ? 0.0 : (sk - d[j]) / seg
		xo[k] = px[j] + t * (px[j+1] - px[j])
		yo[k] = py[j] + t * (py[j+1] - py[j])
	end
	return (xo, yo)
end

"""
	add_curtain!(fig::QtFigure, path; image, zrange, spacing=:distance, flipv=false,
				 clip=false, clip_n=300)

Hang a Fledermaus-style vertical image **curtain** on the wall that follows the XY `path`
through `fig`'s scene (a seismic / midwater profile). Returns `fig`.

- `path`  — an `N×2` matrix (or `GMTdataset`) of the track in the grid's XY coords (`N=2` =
  a straight two-point curtain; more points weave).
- `image` — a `GMTimage` (in-memory) OR a file-path `String` (gmtread).
- `zrange=(zmin,zmax)` — the curtain's vertical extent in TRUE z units (same as the grid).
- `spacing` — `:distance` (image stretched by chord length, default) or `:simple` (even per point).
- `flipv`  — invert the image's vertical sense (default first scanline → top).
- `clip`   — cut the curtain's top edge to the grid surface so the wall hugs the relief
  (the image above the seafloor is dropped). Densifies the track to `clip_n` columns and
  samples `fig.G` along it.

The curtain shares the surface's vertical scale, so it rises/falls with the relief when the
vertical exaggeration changes, and it appears in the **Scene Objects** panel (hideable).
"""
function add_curtain!(fig::QtFigure, path; image, zrange, spacing::Symbol=:distance,
					  flipv::Bool=false, clip::Bool=false, clip_n::Int=300, record::Bool=true)
	isalive(fig) || (@warn "figure window is closed; curtain not added"; return fig)
	px, py = _curtain_xy(path)
	length(px) >= 2 || error("a curtain needs at least 2 track points; got $(length(px))")
	px0, py0 = px, py                                   # ORIGINAL track (clip densifies below) — saved for Session
	(length(zrange) >= 2 && zrange[2] > zrange[1]) ||
		error("zrange must be (zmin, zmax) with zmax > zmin; got $zrange")
	topz = C_NULL
	if clip                                            # cut the top edge to the bathymetry
		px, py = _densify_xy(px, py, clip_n)
		topz = Float64[_sample_grid(fig.G, px[i], py[i]) for i in eachindex(px)]
	end
	u = _curtain_u(px, py, spacing)
	imgpath = _curtain_texfile(image)
	ok = ccall(_fn(:gmtvtk_add_curtain_file_h), Cint,
		  (Ptr{Cvoid}, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble}, Cint,
		   Ptr{Cdouble}, Cstring, Cdouble, Cdouble, Cint),
		  fig.h, px, py, u, Cint(length(px)),
		  topz, imgpath, Float64(zrange[1]), Float64(zrange[2]), Cint(flipv))
	ok == 0 && @warn "curtain not added (window closed or image unreadable)"
	# Save Session: the curtain's source data lives here (not in the C++ Curtain), so capture a :curtain
	# recipe at add time. A file-path image is stored as a :file ref; an in-memory GMTimage is stashed as
	# PNG bytes for a sidecar. `record=false` is used on session replay (which re-records explicitly).
	if ok != 0 && record
		track = join(("$(px0[i]),$(py0[i])" for i in eachindex(px0)), "|")
		params = Dict{String,Any}("track" => track, "zmin" => string(zrange[1]), "zmax" => string(zrange[2]),
		                          "spacing" => String(spacing), "flipv" => string(flipv),
		                          "clip" => string(clip), "clip_n" => string(clip_n))
		if image isa AbstractString
			params["image"] = String(image); params["image_origin"] = "file"
		else
			id = _curtain_img_next_id(fig.h)
			_curtain_img_store!(fig.h, id, read(imgpath))
			params["image"] = id; params["image_origin"] = "generated"
		end
		_session_record!(fig.h, :curtain, :menu; params=params)
	end
	return fig
end

# Dispatch the `view_grid(; vcurtain=)` kwarg: nothing / one spec NamedTuple / a vector or tuple
# of specs. A spec is `(; image, path, zrange, spacing=:distance, flipv=false, clip=false, clip_n=300)`.
_add_curtains!(fig::QtFigure, ::Nothing) = nothing
_add_curtains!(fig::QtFigure, spec::NamedTuple) = _add_curtain_spec!(fig, spec)
function _add_curtains!(fig::QtFigure, specs)
	for s in specs;  _add_curtain_spec!(fig, s);  end
	return nothing
end

function _add_curtain_spec!(fig::QtFigure, spec::NamedTuple)
	hasproperty(spec, :image)  || error("vcurtain spec needs an `image` (GMTimage or file path)")
	hasproperty(spec, :path)   || error("vcurtain spec needs a `path` (N×2 matrix or GMTdataset)")
	hasproperty(spec, :zrange) || error("vcurtain spec needs `zrange=(zmin, zmax)`")
	add_curtain!(fig, spec.path; image=spec.image, zrange=spec.zrange,
				 spacing = get(spec, :spacing, :distance),
				 flipv   = get(spec, :flipv,   false),
				 clip    = get(spec, :clip,    false),
				 clip_n  = get(spec, :clip_n,  300))
	return fig
end
