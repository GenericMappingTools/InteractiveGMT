# output.jl — writers for a PotraceResult. New code, not a port of the C backends: they write the
# same geometry the C's svg/eps/geojson backends write (absolute coordinates, no unit quantization,
# no compression, no debug modes).
#
# A curve of n segments starts at c[n,3] (the end point of its last segment) and, for each segment
# i, goes CORNER: line to c[i,2] then to c[i,3]; CURVETO: cubic Bézier with controls c[i,1],c[i,2]
# ending at c[i,3]. Holes run opposite to their outer path, so a nonzero fill of a path together
# with its children leaves the holes open.

_fnum(v::Float64) = string(round(v; digits=3))

function _rgb01(color::String)
	h = lstrip(color, '#')
	length(h) == 6 || error("colour must be \"#rrggbb\", got \"$color\"")
	return (parse(Int, h[1:2]; base=16) / 255, parse(Int, h[3:4]; base=16) / 255, parse(Int, h[5:6]; base=16) / 255)
end

# ---------------------------------------------------------------------------------------------
# SVG

function _svg_curve(io::IO, cv::PrivCurve)
	m = cv.n
	p = cv.c[m, 3]
	print(io, "M", _fnum(p.x), " ", _fnum(p.y))
	for i = 1:m
		c1, c2, c3 = cv.c[i, 1], cv.c[i, 2], cv.c[i, 3]
		if cv.tag[i] == CORNER
			print(io, " L", _fnum(c2.x), " ", _fnum(c2.y), " ", _fnum(c3.x), " ", _fnum(c3.y))
		else
			print(io, " C", _fnum(c1.x), " ", _fnum(c1.y), " ", _fnum(c2.x), " ", _fnum(c2.y), " ",
			      _fnum(c3.x), " ", _fnum(c3.y))
		end
	end
	print(io, " Z")
	return nothing
end

# the C's write_paths_transparent_rec with grouping=1 (its default): one <path> per positive path
# together with its holes, then recurse into what lies inside the holes.
function _svg_tree(io::IO, tree::Union{PotracePath,Nothing})
	p = tree
	while p !== nothing
		print(io, "<path d=\"")
		_svg_curve(io, p.fcurve)
		q = p.childlist
		while q !== nothing
			print(io, " ")
			_svg_curve(io, q.fcurve)
			q = q.sibling
		end
		println(io, "\"/>")
		q = p.childlist
		while q !== nothing
			_svg_tree(io, q.childlist)
			q = q.sibling
		end
		p = p.sibling
	end
	return nothing
end

"""
    potrace_svg(fname::String, res::PotraceResult; color="#000000", scale=1.0)
    potrace_svg(io::IO, res::PotraceResult; ...)
    potrace_svg(fname_or_io, layers::Vector{Pair{String,PotraceResult}}; background=nothing, scale=1.0)

Write the traced curves as SVG. One pixel = `scale` pt. The second form writes several traces of
the same bitmap size, one `<g>` per `"#rrggbb" => result` pair, painted in the order given, over an
optional `background` colour filling the whole page.
"""
potrace_svg(io::IO, res::PotraceResult; color::String="#000000", scale::Real=1.0) =
	potrace_svg(io, [color => res]; scale=scale)

function potrace_svg(io::IO, layers::Vector{Pair{String,PotraceResult}};
                     background::Union{Nothing,String}=nothing, scale::Real=1.0)
	isempty(layers) && error("potrace_svg: no layers")
	res = layers[1].second
	W, H = res.w * scale, res.h * scale
	println(io, "<?xml version=\"1.0\" standalone=\"no\"?>")
	println(io, "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\"")
	println(io, " width=\"$(_fnum(Float64(W)))pt\" height=\"$(_fnum(Float64(H)))pt\" viewBox=\"0 0 $(_fnum(Float64(W))) $(_fnum(Float64(H)))\"")
	println(io, " preserveAspectRatio=\"xMidYMid meet\">")
	println(io, "<metadata>")
	println(io, "Created by InteractiveGMT, Julia port of potrace 1.16 (Peter Selinger 2001-2019)")
	println(io, "</metadata>")
	background === nothing ||
		println(io, "<rect width=\"$(_fnum(Float64(W)))\" height=\"$(_fnum(Float64(H)))\" fill=\"$background\"/>")
	for (color, r) in layers
		(r.w == res.w && r.h == res.h) || error("potrace_svg: layers of different bitmap sizes")
		# potrace's own group transform: y up, pixel units
		println(io, "<g transform=\"translate(0,$(_fnum(Float64(H)))) scale($(_fnum(Float64(scale))),$(_fnum(-Float64(scale))))\"")
		println(io, "fill=\"$color\" stroke=\"none\">")
		_svg_tree(io, r.plist)
		println(io, "</g>")
	end
	println(io, "</svg>")
	return nothing
end
potrace_svg(fname::String, res::PotraceResult; kw...) = open(io -> potrace_svg(io, res; kw...), fname, "w")
potrace_svg(fname::String, layers::Vector{Pair{String,PotraceResult}}; kw...) =
	open(io -> potrace_svg(io, layers; kw...), fname, "w")

# ---------------------------------------------------------------------------------------------
# EPS

# One curve as PostScript path operators. PDF's content streams use the same path model with short
# names, so the PDF writer passes its own (m / l / c) through `ops`.
function _eps_curve(io::IO, cv::PrivCurve; ops::NTuple{3,String}=("moveto", "lineto", "curveto"))
	mv, ln, cu = ops
	m = cv.n
	p = cv.c[m, 3]
	println(io, _fnum(p.x), " ", _fnum(p.y), " ", mv)
	for i = 1:m
		c1, c2, c3 = cv.c[i, 1], cv.c[i, 2], cv.c[i, 3]
		if cv.tag[i] == CORNER
			println(io, _fnum(c2.x), " ", _fnum(c2.y), " ", ln, " ", _fnum(c3.x), " ", _fnum(c3.y), " ", ln)
		else
			println(io, _fnum(c1.x), " ", _fnum(c1.y), " ", _fnum(c2.x), " ", _fnum(c2.y), " ",
			        _fnum(c3.x), " ", _fnum(c3.y), " ", cu)
		end
	end
	return nothing
end

"""
    potrace_eps(fname::String, res::PotraceResult; color="#000000", scale=1.0)
    potrace_eps(io::IO, res::PotraceResult; ...)
    potrace_eps(fname_or_io, layers::Vector{Pair{String,PotraceResult}}; background=nothing, scale=1.0)

Write the traced curves as EPS (the C's render0 in long coding: a positive path and the holes
that follow it in the list are filled together). One pixel = `scale` pt. The layered form paints
each `"#rrggbb" => result` in turn over an optional page-filling `background`, as `potrace_svg`.
"""
potrace_eps(io::IO, res::PotraceResult; color::String="#000000", scale::Real=1.0) =
	potrace_eps(io, [color => res]; scale=scale)

function potrace_eps(io::IO, layers::Vector{Pair{String,PotraceResult}};
                     background::Union{Nothing,String}=nothing, scale::Real=1.0)
	isempty(layers) && error("potrace_eps: no layers")
	res = layers[1].second
	W, H = res.w * scale, res.h * scale
	println(io, "%!PS-Adobe-3.0 EPSF-3.0")
	println(io, "%%Creator: InteractiveGMT, Julia port of potrace 1.16 (Peter Selinger 2001-2019)")
	println(io, "%%LanguageLevel: 2")
	println(io, "%%BoundingBox: 0 0 ", ceil(Int, W), " ", ceil(Int, H))
	println(io, "%%HiResBoundingBox: 0 0 ", _fnum(Float64(W)), " ", _fnum(Float64(H)))
	println(io, "%%Pages: 1")
	println(io, "%%EndComments")
	println(io, "%%Page: 1 1")
	println(io, "save")
	println(io, _fnum(Float64(scale)), " ", _fnum(Float64(scale)), " scale")
	if background !== nothing
		r, g, b = _rgb01(background)
		println(io, _fnum(r), " ", _fnum(g), " ", _fnum(b), " setrgbcolor")
		println(io, "0 0 ", res.w, " ", res.h, " rectfill")
	end
	for (color, lr) in layers
		(lr.w == res.w && lr.h == res.h) || error("potrace_eps: layers of different bitmap sizes")
		r, g, b = _rgb01(color)
		println(io, _fnum(r), " ", _fnum(g), " ", _fnum(b), " setrgbcolor")
		p = lr.plist
		while p !== nothing
			_eps_curve(io, p.fcurve)
			println(io, "closepath")
			(p.next === nothing || p.next.sign == '+') && println(io, "fill")
			p = p.next
		end
	end
	println(io, "restore")
	println(io, "%%EOF")
	return nothing
end
potrace_eps(fname::String, res::PotraceResult; kw...) = open(io -> potrace_eps(io, res; kw...), fname, "w")
potrace_eps(fname::String, layers::Vector{Pair{String,PotraceResult}}; kw...) =
	open(io -> potrace_eps(io, layers; kw...), fname, "w")

# ---------------------------------------------------------------------------------------------
# PDF

"""
    potrace_pdf(fname::String, res::PotraceResult; color="#000000", scale=1.0)
    potrace_pdf(io::IO, res::PotraceResult; ...)
    potrace_pdf(fname_or_io, layers::Vector{Pair{String,PotraceResult}}; background=nothing, scale=1.0)

Write the traced curves as a one-page PDF, the page the size of the bitmap (one pixel = `scale` pt).
Same drawing as `potrace_eps`: each positive path is filled (nonzero) together with the holes that
follow it, the layers in the order given, over an optional page-filling `background`. The content
stream is left uncompressed (the C's pdf backend compresses it with zlib; the geometry is the same).
"""
potrace_pdf(io::IO, res::PotraceResult; color::String="#000000", scale::Real=1.0) =
	potrace_pdf(io, [color => res]; scale=scale)

function potrace_pdf(io::IO, layers::Vector{Pair{String,PotraceResult}};
                     background::Union{Nothing,String}=nothing, scale::Real=1.0)
	isempty(layers) && error("potrace_pdf: no layers")
	res = layers[1].second
	W, H = res.w * scale, res.h * scale
	# the page content: the same operators as the EPS, in PDF spelling
	cs = IOBuffer()
	println(cs, _fnum(Float64(scale)), " 0 0 ", _fnum(Float64(scale)), " 0 0 cm")
	if background !== nothing
		r, g, b = _rgb01(background)
		println(cs, _fnum(r), " ", _fnum(g), " ", _fnum(b), " rg")
		println(cs, "0 0 ", res.w, " ", res.h, " re f")
	end
	for (color, lr) in layers
		(lr.w == res.w && lr.h == res.h) || error("potrace_pdf: layers of different bitmap sizes")
		r, g, b = _rgb01(color)
		println(cs, _fnum(r), " ", _fnum(g), " ", _fnum(b), " rg")
		p = lr.plist
		while p !== nothing
			_eps_curve(cs, p.fcurve; ops=("m", "l", "c"))
			println(cs, "h")
			(p.next === nothing || p.next.sign == '+') && println(cs, "f")
			p = p.next
		end
	end
	content = take!(cs)
	# the file: five objects and a cross-reference table of their byte offsets
	out = IOBuffer()
	off = Int[]
	print(out, "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
	obj(s) = (push!(off, position(out)); print(out, length(off), " 0 obj\n", s, "\nendobj\n"))
	obj("<< /Type /Catalog /Pages 2 0 R >>")
	obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
	obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 $(_fnum(Float64(W))) $(_fnum(Float64(H)))] " *
	    "/Resources << >> /Contents 4 0 R >>")
	push!(off, position(out))
	print(out, "4 0 obj\n<< /Length ", length(content), " >>\nstream\n")
	write(out, content)
	print(out, "\nendstream\nendobj\n")
	obj("<< /Producer (InteractiveGMT, Julia port of potrace 1.16 \\(Peter Selinger 2001-2019\\)) >>")
	xref = position(out)
	print(out, "xref\n0 ", length(off) + 1, "\n0000000000 65535 f \n")
	for o in off
		print(out, lpad(o, 10, '0'), " 00000 n \n")
	end
	print(out, "trailer\n<< /Size ", length(off) + 1, " /Root 1 0 R /Info 5 0 R >>\nstartxref\n", xref, "\n%%EOF\n")
	write(io, take!(out))
	return nothing
end
potrace_pdf(fname::String, res::PotraceResult; kw...) = open(io -> potrace_pdf(io, res; kw...), fname, "w")
potrace_pdf(fname::String, layers::Vector{Pair{String,PotraceResult}}; kw...) =
	open(io -> potrace_pdf(io, layers; kw...), fname, "w")

# ---------------------------------------------------------------------------------------------
# GMTdataset

# A curve as a closed polyline: corners as they are, each Bézier sampled at `steps` equal steps of
# its parameter (the C's geojson backend uses 8). First point = last point.
function _flatten(cv::PrivCurve, steps::Int)
	m = cv.n
	pts = DPt[]
	cur = cv.c[m, 3]
	push!(pts, cur)
	for i = 1:m
		if cv.tag[i] == CORNER
			push!(pts, cv.c[i, 2], cv.c[i, 3])
		else
			for s = 1:steps
				push!(pts, bezier(s / steps, cur, cv.c[i, 1], cv.c[i, 2], cv.c[i, 3]))
			end
		end
		cur = cv.c[i, 3]
	end
	return pts
end

"""
    D = potrace_gmtds(res::PotraceResult; region=nothing, bezier_steps=8, proj4="", wkt="", epsg=0)

The traced curves as a `Vector{GMTdataset}` of closed polygons (geom = wkbPolygon), in the list
order of the trace: each outer polygon followed by its holes, the holes flagged with the `-Ph`
segment header. Béziers are sampled at `bezier_steps` points each.

`region = (xmin, xmax, ymin, ymax)` is the extent of the bitmap's pixel EDGES in world units —
for a grid-registered raster that is its range widened by half a cell on every side. Without it
the coordinates are pixels, y up, origin at the bottom-left corner.
"""
function potrace_gmtds(res::PotraceResult; region::Union{Nothing,NTuple{4,Real},Vector{<:Real}}=nothing,
                       bezier_steps::Int=8, proj4::String="", wkt::String="", epsg::Int=0)
	x0, x1, y0, y1 = region === nothing ? (0.0, Float64(res.w), 0.0, Float64(res.h)) : Float64.(Tuple(region))
	sx = (x1 - x0) / res.w
	sy = (y1 - y0) / res.h
	D = GMTdataset{Float64,2}[]
	for p in paths(res)
		pts = _flatten(p.fcurve, bezier_steps)
		xy = Matrix{Float64}(undef, length(pts), 2)
		for (k, q) in enumerate(pts)
			xy[k, 1] = x0 + sx * q.x
			xy[k, 2] = y0 + sy * q.y
		end
		push!(D, GMTdataset{Float64,2}(data=xy, colnames=["X", "Y"], header=(p.sign == '-' ? " -Ph" : ""),
		                               proj4=proj4, wkt=wkt, epsg=epsg, geom=3))
	end
	isempty(D) || GMT.set_dsBB!(D)
	return D
end
