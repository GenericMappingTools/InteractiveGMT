# nested.jl — host side of the "Nested grids" (tsunami) rectangle tool. The C++ viewer owns the
# rectangles and their COMCOT/NSWING quantization (deps/src/85_polygon.cpp, port of Mirone's
# nesting_sizes.m). The only thing it can't do without GMT is materialise a grid, so its
# "Create blank grid" menu item calls back here via g_juliaEval.

# Build a zero grid spanning [x0,x1] × [y0,y1] at increments (xi,yi) and add it to the EXISTING
# window `scene` (the one holding the nested rectangles) as a HIDDEN extra surface. Called from
# nestCreateBlankGrid (55_lineprops.cpp), which passes the scene handle and `n` = this rect's 1-based
# position in the nesting chain. The grid is named "Nested grid n" so the names follow the grid stack
# order (base grid first, then 1, 2, 3 inward) and gets an (unchecked) row in Scene Objects; the user
# ticks it to show it. NO new window is ever opened. `geog` tags the grid as geographic.
function _nested_blank_grid(scene::Ptr{Cvoid}, x0, x1, y0, y1, xi, yi, geog::Bool, n::Integer)
	nm = "Nested grid $(Int(n))"
	nx = round(Int, (x1 - x0) / xi) + 1
	ny = round(Int, (y1 - y0) / yi) + 1
	Z  = zeros(Float32, ny, nx)                       # row-major (y, x): GMT.jl matrix layout
	xv = collect(range(x0, x1; length = nx))
	yv = collect(range(y0, y1; length = ny))
	G  = mat2grid(Z; x = xv, y = yv)
	G.title = nm
	if geog
		G.proj4 = "+proj=longlat +datum=WGS84 +no_defs"
	end
	_add_grid_to_scene(scene, G, nm)                  # adds as an extra surface (Scene Objects row)
	ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, nm, Cint(0))  # hidden
	return nothing
end
