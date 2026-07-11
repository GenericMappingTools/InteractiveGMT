# nested.jl — host side of the "Nested grids" (tsunami) rectangle tool. The C++ viewer owns the
# rectangles and their COMCOT/NSWING quantization (deps/src/85_polygon.cpp, port of Mirone's
# nesting_sizes.m). The only thing it can't do without GMT is materialise a grid, so its
# "Create blank grid" menu item calls back here via g_juliaEval.

# Distinct solid colours cycled per nested blank grid (tab10-ish), so successive grids are visually
# told apart instead of all rendering in the viewer's flat-z blue ramp. RGB in 0..1.
const _NESTED_COLORS = [
	(0.89, 0.10, 0.11), (0.22, 0.49, 0.72), (0.30, 0.69, 0.29), (0.60, 0.31, 0.64),
	(1.00, 0.50, 0.00), (0.65, 0.34, 0.16), (0.97, 0.51, 0.75), (0.45, 0.45, 0.45),
]

# Build a zero grid spanning [x0,x1] × [y0,y1] at increments (xi,yi) and add it to the EXISTING
# window `scene` (the one holding the nested rectangles) as a HIDDEN extra surface. Called from
# nestCreateBlankGrid (55_lineprops.cpp), which passes the scene handle and `n` = this rect's 1-based
# position in the nesting chain. The grid is named "layerN" so the names follow the grid stack
# order (base grid first, then 1, 2, 3 inward) and gets an (unchecked) row in Scene Objects; the user
# ticks it to show it. NO new window is ever opened. `geog` tags the grid as geographic.
function _nested_blank_grid(scene::Ptr{Cvoid}, x0, x1, y0, y1, xi, yi, geog::Bool, n::Integer)
	nm = "layer$(Int(n))"
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
	col = _NESTED_COLORS[((Int(n) - 1) % length(_NESTED_COLORS)) + 1]   # cycle a distinct solid colour
	_add_grid_to_scene(scene, G, nm; color = col)     # adds as an extra surface (Scene Objects row)
	ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, nm, Cint(0))  # hidden
	return nothing
end
