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

"""
	show_table(fig, data; name="")

Show `data` in the window's **Data Viewer** spreadsheet tab and bring that tab forward.
`fig` is any viewer handle (`QtFigure`/`QtPoints`/`QtFV`); `data` is a `GMTdataset` (single
or multi-segment `Vector{GMTdataset}`), a plain matrix, or a vector. `name` labels the tab
(defaults to the dataset's header / "matrix" / "vector"). Returns `fig`.
"""
function show_table(fig, data; name::AbstractString="")
	M, headers, nm = _table_matrix(data)
	Mc = Matrix{Float64}(M)                 # contiguous column-major Float64 for the ccall
	nrows, ncols = size(Mc)
	isempty(name) && (name = nm)
	hdr = isempty(headers) ? "" : join(headers, '\t')
	ok = GC.@preserve Mc ccall(_fn(:gmtvtk_set_table), Cint,
		(Ptr{Cvoid}, Cstring, Ptr{Cdouble}, Cint, Cint, Cstring),
		_fig_handle(fig), String(name), Mc, Cint(nrows), Cint(ncols), hdr)
	ok == 0 && @warn "figure window is closed; table not shown"
	return fig
end
