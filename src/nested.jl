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

# THE constructor for a nesting level's grid: a zero grid at the rectangle's limits/increments, named
# for the level. Used to CREATE the hollow layer ("Create blank grid", below) and to REBUILD it when a
# refill finds the rectangle has been resized since (transplant.jl `_on_nested_transplant`) — one
# function, so the two can never disagree on what "the grid for this level" is.
function _nested_blank(x0, x1, y0, y1, xi, yi, geog::Bool, nm::String)
	nx = round(Int, (x1 - x0) / xi) + 1
	ny = round(Int, (y1 - y0) / yi) + 1
	Z  = zeros(Float32, ny, nx)                       # row-major (y, x): GMT.jl matrix layout
	xv = collect(range(x0, x1; length = nx))
	yv = collect(range(y0, y1; length = ny))
	G  = mat2grid(Z; x = xv, y = yv)
	_grid_command!(G, "iGMT nested blank grid '$nm' ($(nx)x$(ny), inc $xi/$yi)")
	G.title = nm
	if geog
		G.proj4 = "+proj=longlat +datum=WGS84 +no_defs"
	end
	return G
end

function _nested_blank_grid(scene::Ptr{Cvoid}, x0, x1, y0, y1, xi, yi, geog::Bool, n::Integer)
	nm = "layer$(Int(n))"
	G  = _nested_blank(x0, x1, y0, y1, xi, yi, geog, nm)
	col = _NESTED_COLORS[((Int(n) - 1) % length(_NESTED_COLORS)) + 1]   # cycle a distinct solid colour
	_add_grid_to_scene(scene, G, nm; color = col)     # adds as an extra surface (Scene Objects row)
	ccall(_fn(:gmtvtk_set_object_visible), Cint, (Ptr{Cvoid}, Cstring, Cint), scene, nm, Cint(0))  # hidden
	return nothing
end

# ── ONE registration for the whole nesting chain ─────────────────────────────────────────────────
# The chain is GRIDLINE-registered end to end: the rectangle snap (85_polygon.cpp `nestBaseGrid`), the
# corner rule (`_nest_binning`) and nswing itself all read a grid's nodes as range[1]:inc:range[2]. A
# pixel-registered bathymetry breaks all three in ways that look like anything but a registration
# problem, so it is converted UNDER THE HOOD and the run carries on as if nothing had happened.

# What `grdedit -T` does to a pixel grid, in memory: the SAME nodes and values, the header moved onto
# them — region shrunk by half a cell (GMT.jl's own `grid2pix`), x/y the cell centres, registration 0.
# The z buffer is SHARED, not copied: no value moves, only the description of where they sit.
# A gridline grid comes back as itself.
function _as_gridline(G::GMTgrid)
	G.registration == 1 || return G
	nx, ny = _grid_dims(G)
	hdr = GMT.grid2pix(G; pix = false)
	Gg  = typeof(G)((getfield(G, f) for f in fieldnames(typeof(G)))...)
	Gg.range = copy(G.range);  Gg.range[1:4] = hdr[1:4]
	Gg.inc   = copy(G.inc)
	Gg.x = collect(range(hdr[1], hdr[2]; length = nx))
	Gg.y = collect(range(hdr[3], hdr[4]; length = ny))
	Gg.registration = 0
	return Gg
end

# Is the grid FILE at `path` pixel-registered? Header only (`grdinfo -C`, column 11 = registration),
# so a file the run will use as-is is never read twice. Unreadable -> false: the reader that follows
# reports it in its own words.
_grid_file_is_pixel(path::AbstractString) =
	try GMT.grdinfo(String(path), C = true).data[11] == 1 catch; false end

# Called (synchronously, via g_juliaEval) by the C++ nested-rectangle tool the moment the ROOT rectangle
# is drawn, before it snaps, and by a session load before it restores any nested rectangle. A pixel
# bathymetry gets its gridline header, and so does every grid already derived ON it — same pixel
# footprint, e.g. "Okada z", which `_okada_on_grid` gave the bathymetry's own header — so no two
# registrations ever meet in the chain. An Okada field computed AFTER this inherits gridline from the
# window grid by construction.
#
# NOTHING IS REBUILT. The values have not changed, so neither has the palette, the colour bar, the
# layer's row or its visibility: the viewer moves each layer's x/y frame IN PLACE
# (`gmtvtk_set_grid_frame_h`) and the registry swaps in the same z buffer under the new header.
# No-op on a gridline base.
function _nested_gridline_base!(scene::Ptr{Cvoid})
	B = _find_object(scene, :grid, "")
	(B isa GMTgrid && B.registration == 1) || return nothing
	v = get(_SCENE_OBJS, scene, nothing)
	v === nothing && return nothing
	moved = String[]
	for i in eachindex(v)
		k, n, d = v[i]
		(k === :grid && d isa GMTgrid && d.registration == 1) || continue
		(d === B || (d.range[1:4] == B.range[1:4] && d.inc == B.inc)) || continue
		Gg = _as_gridline(d)
		# the base answers to "" as well as to its own name; an extra only to its own
		nm = d === B ? "" : n
		ccall(_fn(:gmtvtk_set_grid_frame_h), Cint, (Ptr{Cvoid}, Cstring, Cdouble, Cdouble, Cdouble, Cdouble),
		      scene, nm, Gg.range[1], Gg.range[2], Gg.range[3], Gg.range[4]) == 0 &&
			error("the viewer has no grid '$(isempty(nm) ? "base" : nm)' to move")
		v[i] = (k, n, Gg)
		d === B || push!(moved, n)
		d === B && get(_FIGREG, scene, nothing) isa QtFigure && (_FIGREG[scene] = QtFigure(scene, Gg))
	end
	extra = isempty(moved) ? "" : " (and " * join(moved, ", ") * ")"
	_viewer_log_info(scene, "Nested grids: the bathymetry$extra is pixel-registered; converted to " *
		"gridline registration (grdedit -T) — the nesting chain and NSWING use gridline nodes.")
	return nothing
end
