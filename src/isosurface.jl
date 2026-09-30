# Iso-surfaces of a 3-D cube (a tomography model, an earthquake-count volume). The C side extracts
# the surface (gmtvtk_add_isosurface_h -> VTK's contour filter) and hands it to the ONE mesh-layer
# door, gmtvtk_add_mesh_h, so an iso-surface is an ordinary mesh layer: Scene Objects row, properties,
# Remove, its own VE. The cube goes over WHERE IT LIES with its layout code — never transposed.

"""
	add_isosurface!(fig::QtFigure, C::GMTgrid; level, color=:orange, opacity=1.0, name="",
	                samezscale=true)

Add to the window `fig` the surface where the 3-D cube `C` crosses the value `level` — an
iso-surface, as a mesh layer with its own Scene Objects row. `C` is a GMT cube (`ndims(C.z) == 3`)
whose layers sit at the heights `C.v`, which may be irregular. Give `C.v` in the same vertical
units as `fig`'s own grid.

`color` is the surface colour (a name or an (r,g,b)), `opacity` its opacity from 0 to 1.
`samezscale=true` draws it on the vertical scale of `fig`'s own grid, so it stands at its true depth
under the topography (see [`add!`](@ref)). Like every added layer it becomes the layer on display;
tick the others back on with [`setvisible!`](@ref). Returns `fig`.
"""
function add_isosurface!(fig::QtFigure, C::GMTgrid; level::Real, color=:orange, opacity::Real=1.0,
                         name::String="", samezscale::Bool=true)
	ndims(C.z) == 3 || error("add_isosurface!: C must be a 3-D cube; its z has $(ndims(C.z)) dimensions")
	h   = getfield(fig, :h)
	reg = Int(C.registration)
	nx, ny, nz = length(C.x) - reg, length(C.y) - reg, size(C.z, 3)
	nx * ny * nz == length(C.z) ||
		error("add_isosurface!: cube dims $(nx)x$(ny)x$(nz) do not match its z buffer ($(length(C.z)) values)")
	length(C.v) == nz || error("add_isosurface!: the cube has $nz layers but $(length(C.v)) layer heights in C.v")
	# NODE coordinates: a pixel-registered cube keeps its cell EDGES in x/y, so its nodes are the centres.
	xc = reg == 1 ? Float64.(C.x[1:end-1]) .+ C.inc[1] / 2 : Float64.(C.x)
	yc = reg == 1 ? Float64.(C.y[1:end-1]) .+ C.inc[2] / 2 : Float64.(C.y)
	zc = Float64.(C.v)
	z  = eltype(C.z) === Float32 ? C.z : Float32.(C.z)   # no copy when it is already Float32
	c  = _ovl_color(color, :points)
	rgb = UInt8[round(UInt8, 255 * c[1]), round(UInt8, 255 * c[2]), round(UInt8, 255 * c[3])]
	nm = isempty(name) ? "Iso-surface $(level)" : name
	ok = GC.@preserve z xc yc zc rgb ccall(_fn(:gmtvtk_add_isosurface_h), Cint,
		(Ptr{Cvoid}, Ptr{Cfloat}, Cint, Cint, Cint, Cint, Ptr{Cdouble}, Ptr{Cdouble}, Ptr{Cdouble},
		 Cdouble, Ptr{Cuchar}, Cdouble, Cint, Cstring),
		h, z, Cint(nx), Cint(ny), Cint(nz), _grid_layout_code(C), xc, yc, zc,
		Float64(level), rgb, Float64(opacity), Cint(_isgeographic(C)), nm)
	ok == -1 && error("add_isosurface!: the cube never crosses level=$level")
	ok == 0  && error("add_isosurface!: surface not added (window closed?)")
	# The scale BEFORE the adopt, which frames the camera on the layer as it is drawn (see add!).
	samezscale && ccall(_fn(:gmtvtk_grid_share_zscale_h), Cint, (Ptr{Cvoid}, Cstring, Cint), h, nm, Cint(1))
	# The layer's own frame is the cube's node box.
	_adopt_new_element(h, nm, GMT.mat2ds([xc[1] yc[1] zc[1]; xc[end] yc[end] zc[end]]))
	return fig
end
