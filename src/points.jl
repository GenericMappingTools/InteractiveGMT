# Point clouds with rubber-band selection. A coloured point cloud (Ctrl+right-drag a box to
# select points). Ported from GMTF3D `view_points` + its f3d_ext rubber-band selector, onto the
# self-contained Qt+VTK viewer.

"""
	view_points(D; cmap=:turbo, pointsize=4, pickcolor=(0.83,0.83,0.83),
				geographic=nothing, title=...)

Show a point cloud — a `GMTdataset` or an `N×≥3` matrix of `x y z [...]` rows — in the
Qt + VTK viewer, colouring each point by its **z** through the GMT colormap `cmap` (any
GMT name, e.g. `:turbo`, `:roma`, `:geo`; `nothing` = the built-in ramp). Returns a
`QtPoints` handle immediately; the window stays live while you keep using the REPL.

**Ctrl+right-drag** a box over the cloud to select points (TOGGLE — re-dragging the same
box deselects; **Ctrl+Z** undoes the last change). Plain right-drag stays the dolly. The
selected points are highlighted in `pickcolor` and kept for you — read them back with
[`selection`](@ref), which returns a copy of the picked `x y z [...]` rows (or `nothing`).

- `pointsize` — point size in px.
- `pickcolor` — highlight colour for selected points: an `(r,g,b)` (0-255 ints or 0-1
  floats) or a grey number.
- `geographic` — auto-detected for a `GMTdataset` (override with `true`/`false`); treats
  x,y as degrees and z as metres for a physically-true vertical scale.
"""
function view_points(D; cmap=:turbo, pointsize::Real=4, pickcolor=(0.83, 0.83, 0.83),
					 geographic::Union{Bool,Nothing}=nothing,
					 title::AbstractString="GMT 3-D Point Cloud  (Qt + VTK)")
	A = D isa GMTdataset ? Float64.(D.data) : Float64.(Matrix(D))
	size(A, 2) >= 3 || error("a point cloud needs at least 3 columns (x y z); got $(size(A,2))")
	N = size(A, 1);  N > 0 || error("dataset has no points")
	xyz = Vector{Float64}(undef, 3N)
	@inbounds for i in 1:N
		xyz[3i-2] = A[i, 1];  xyz[3i-1] = A[i, 2];  xyz[3i] = A[i, 3]
	end
	xmn, xmx = extrema(@view A[:, 1]);  ymn, ymx = extrema(@view A[:, 2])
	zmn, zmx = extrema(@view A[:, 3])
	geog = geographic === nothing ? (D isa GMTdataset ? GMT.isgeog(D) : false) : geographic
	cz, crgb, ncolor = _cpt_nodes_range(zmn, zmx, cmap)
	pr, pg, pb = _ovl_color(pickcolor, :points)   # name (:red), grey int, (r,g,b), or 0-1 floats
	h = ccall(_fn(:gmtvtk_view_points), Ptr{Cvoid},
		  (Ptr{Cdouble}, Cint, Ptr{Cdouble}, Ptr{Cdouble}, Cint,
		   Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cdouble,
		   Cdouble, Cdouble, Cdouble, Cstring),
		  xyz, Cint(N), cz, crgb, Cint(ncolor),
		  xmn, xmx, ymn, ymx, Cint(geog), Float64(pointsize),
		  pr, pg, pb, title)
	fig = _register_fig!(QtPoints(h, A))
	_apply_crs!(fig, crs_from(D; geographic=geog))    # store the CRS + reveal the Geography menu if referenced
	_start_pump()
	return fig
end

"""
	selection(fig::QtPoints) -> Matrix or nothing

Return a copy of the point-cloud rows currently selected with **Ctrl+right-drag** in the
viewer (`x y z [...]`), or `nothing` if none are selected (or the window is closed).
"""
function selection(fig::QtPoints)
	isalive(fig) || return nothing
	n = ccall(_fn(:gmtvtk_selection_count), Cint, (Ptr{Cvoid},), fig.h)
	n <= 0 && return nothing
	ids = Vector{Cint}(undef, n)
	got = ccall(_fn(:gmtvtk_get_selection), Cint, (Ptr{Cvoid}, Ptr{Cint}, Cint), fig.h, ids, Cint(n))
	got <= 0 && return nothing
	rows = Int.(ids[1:got]) .+ 1            # 0-based C ids -> 1-based Julia rows
	return fig.D[rows, :]
end
