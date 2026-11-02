# Data Viewer (spreadsheet tab). Display tabular data in a window's "Data Viewer" tab. Accepts a
# GMTdataset (single or multi-segment), a plain matrix, or a vector.

# Reduce an input to (column-major Float64 matrix, column names, default tab name).
_table_matrix(M::AbstractMatrix) = (Float64.(M), String[], "matrix")
_table_matrix(v::AbstractVector) = (reshape(Float64.(v), :, 1), String[], "vector")
function _table_matrix(D::GMTdataset)
	cn = (isdefined(D, :colnames) && D.colnames !== nothing) ? String.(D.colnames) : String[]
	nm = (isdefined(D, :header) && !isempty(D.header)) ? String(D.header) : "GMTdataset"
	return Float64.(D.data), cn, nm
end
function _table_matrix(D::AbstractVector{<:GMTdataset})
	isempty(D) && return (Matrix{Float64}(undef, 0, 0), String[], "GMTdataset")
	M  = vcat((Float64.(d.data) for d in D)...)
	cn = (D[1].colnames !== nothing) ? String.(D[1].colnames) : String[]
	return M, cn, "GMTdataset"
end

# The n node/pixel-centre coordinates of a grid or image axis: `v` as it is when it holds n values, the
# cell centres when it holds the n+1 cell edges of a pixel-registered one, else just 1..n.
function _table_nodes(v, n::Int)::Vector{Float64}
	length(v) == n     && return Float64.(v)
	length(v) == n + 1 && return [(Float64(v[k]) + Float64(v[k+1])) / 2 for k in 1:n]
	return collect(1.0:n)
end
# The x heads of a matrix table, short: just enough significant digits to tell neighbours apart.
function _table_heads(xs::Vector{Float64})::Vector{String}
	length(xs) < 2 && return [string(round(x; sigdigits=6)) for x in xs]
	dx = minimum(abs, diff(xs))
	mx = maximum(abs, xs)
	sd = (dx > 0 && mx > 0) ? clamp(ceil(Int, log10(mx / dx)) + 1, 3, 10) : 6
	return [(s = string(round(x; sigdigits=sd)); endswith(s, ".0") ? s[1:end-2] : s) for x in xs]
end

# A GRID as a table laid out the way it is drawn: one row per y node, NORTH ON TOP, one column per x
# node headed by its x; the first column is the row's y. Read through `_zmat` (any layout, no copy).
function _table_matrix(G::GMTgrid)
	ndims(G.z) == 2 || error("a cube has no single table; show one of its layers")
	Z = _zmat(G)                                   # z[iy, ix], row 1 = south
	ny, nx = size(Z)
	xs, ys = _table_nodes(G.x, nx), _table_nodes(G.y, ny)
	M = Matrix{Float64}(undef, ny, nx + 1)
	@inbounds for r in 1:ny
		iy = ny - r + 1
		M[r, 1] = ys[iy]
		for ix in 1:nx
			M[r, ix + 1] = Z[iy, ix]
		end
	end
	return M, ["y \\ x"; _table_heads(xs)], "grid"
end

# ONE BAND of an image, as a table laid out like the grid's above (north on top, x across, y in the
# first column), its raw pixel values (an indexed image's indices, not their colours). The pixels are
# read with `_pixaccess`, the drape's own layout-aware accessor.
struct _ImageBand
	I::GMTimage
	band::Int
end
_table_matrix(I::GMTimage) = _table_matrix(_ImageBand(I, 1))
function _table_matrix(B::_ImageBand)
	I, S, lay = B.I, B.I.image, B.I.layout
	d3 = ndims(S) == 3
	nb = d3 ? size(S, 3) : 1
	1 <= B.band <= nb || error("the image has $nb band(s), not a band $(B.band)")
	rowmajor = length(lay) >= 2 && lay[2] == 'R'
	nlon, nlat = rowmajor ? (size(S, 1), size(S, 2)) : (size(S, 2), size(S, 1))
	pix = _pixaccess(S, lay, d3, nb, rowmajor, nlon, nlat)
	nf  = _north_first(lay, rowmajor)
	xs, ys = _table_nodes(I.x, nlon), _table_nodes(I.y, nlat)
	M = Matrix{Float64}(undef, nlat, nlon + 1)
	@inbounds for r in 1:nlat
		lat = nf ? r : nlat - r + 1                # the array row holding the r-th row from the north
		M[r, 1] = ys[nlat - r + 1]
		for c in 1:nlon
			M[r, c + 1] = Float64(pix(lat, c, B.band))
		end
	end
	return M, ["y \\ x"; _table_heads(xs)], nb > 1 ? "image (band $(B.band) of $nb)" : "image"
end

# The TEXT column of the input, one string per row joined by '\n' — or "" when there is none, so
# the table gets no text column at all. Only a GMTdataset carries text; a multi-segment one has it
# only when EVERY segment does (rows from a text-less segment would have nothing to show).
_table_text(::Any) = ""
_has_text(D::GMTdataset) = !isempty(D.text) && length(D.text) == size(D.data, 1)
_table_text(D::GMTdataset) = _has_text(D) ? join(replace.(D.text, '\n' => ' '), '\n') : ""
function _table_text(D::AbstractVector{<:GMTdataset})
	(isempty(D) || !all(_has_text, D)) && return ""
	return join((replace(t, '\n' => ' ') for d in D for t in d.text), '\n')
end

"""
	show_table(fig, data; name="")

Show `data` in the window's **Data Viewer** spreadsheet tab and bring that tab forward.
`fig` is any viewer handle (`QtFigure`/`QtPoints`/`QtFV`); `data` is a `GMTdataset` (single
or multi-segment `Vector{GMTdataset}`), a plain matrix, a vector, a `GMTgrid` or a `GMTimage` (its
first band; north on top, the x of each column in its header and the y of each row in the first
column). `name` labels the tab (defaults to the dataset's header / "matrix" / "vector" / "grid" /
"image"). Returns `fig`.
"""
function show_table(fig, data; name::AbstractString="")
	M, headers, nm = _table_matrix(data)
	Mc = Matrix{Float64}(M)                 # contiguous column-major Float64 for the ccall
	nrows, ncols = size(Mc)
	isempty(name) && (name = nm)
	hdr = isempty(headers) ? "" : join(headers, '\t')
	# a grid or an image band is a MATRIX: MATLAB's variable-editor look (uniform narrow cells, 5 digits)
	ok = if data isa Union{GMTgrid,GMTimage,_ImageBand}
		GC.@preserve Mc ccall(_fn(:gmtvtk_set_matrix_table), Cint,
			(Ptr{Cvoid}, Cstring, Ptr{Cdouble}, Cint, Cint, Cstring),
			_fig_handle(fig), String(name), Mc, Cint(nrows), Cint(ncols), hdr)
	else
		GC.@preserve Mc ccall(_fn(:gmtvtk_set_table), Cint,
			(Ptr{Cvoid}, Cstring, Ptr{Cdouble}, Cint, Cint, Cstring, Cstring),
			_fig_handle(fig), String(name), Mc, Cint(nrows), Cint(ncols), hdr, _table_text(data))
	end
	ok == 0 && @warn "figure window is closed; table not shown"
	return fig
end
