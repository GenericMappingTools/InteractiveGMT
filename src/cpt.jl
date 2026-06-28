# CPT / colormap helpers: build vtkColorTransferFunction control nodes from GMT colormaps,
# plus the geographic detection and vertical-scale resolution shared by the viewers.

const _DEG2M     = 111194.9     # ~1 geographic degree in metres (GMT's value)
const _GEOG_VFRAC = 0.135       # geog auto: displayed z-range / horizontal extent (~vexag 20)

# Geographic? Prefer the grid's explicit projection; when it carries none, GMT.guessgeog falls
# back to a crude range test (limits inside [-180 360 -90 90]) so an UNREFERENCED lon/lat grid is
# still taken as geographic. Crudeness (a small cartesian grid in that box reads as geographic) is
# inherent and accepted — the user asked for this guess when no CRS is present.
_isgeographic(G::GMTgrid)::Bool = GMT.guessgeog(G)

# Default colormap for a grid. GMT tags topo/bathymetry remote grids (earth_relief / gebco /
# gebcosi / synbath) with cpt == "geo"; only those get the elevation `geo` ramp. Every other grid
# (computed fields, deformation, gravity, a plain user grid) defaults to `turbo`. Triggered by
# passing cmap=:auto (the viewer's default); an explicit cmap always wins.
_default_cmap(G::GMTgrid) = (G.cpt == "geo") ? :geo : :turbo

# Build a CPT (plain `makecpt`, LINEAR over the data range) and return its control nodes: z
# values `cz` and matching RGB `crgb` (0..1, row-major), for a faithful vtkColorTransferFunction
# on the C side. Returns (Float64[], Float64[], 0) on failure -> viewer falls back to its ramp.
function _cpt_nodes(G::GMTgrid, cmap)
	cmap === nothing && return (Float64[], Float64[], 0)
	fin = filter(isfinite, G.z)
	isempty(fin) && return (Float64[], Float64[], 0)
	zmn, zmx = extrema(fin)
	zmx <= zmn && return (Float64[], Float64[], 0)
	return _cpt_nodes_range(zmn, zmx, cmap)
end

# CPT control nodes (z + rgb) over an explicit [zmn,zmx] range. Returns (Float64[], Float64[], 0)
# on failure -> viewer falls back to its built-in ramp.
function _cpt_nodes_range(zmn, zmx, cmap)
	(cmap === nothing || !(zmx > zmn)) && return (Float64[], Float64[], 0)
	try
		# Resample to 256 discrete nodes so the viewer gets a full-resolution ramp (the master
		# CPT carries only ~dozens of control points; F3D uses 256).
		inc = (zmx - zmn) / 255
		C  = makecpt(cmap=cmap, range=(zmn, zmx, inc), continuous=true)
		cm = Float64.(C.colormap)            # colours; GMT APPENDS the CPT foreground row at the end
		rg = Float64.(C.range)               # nseg x 2 : [zlow zhigh] per slice (authoritative count)
		nseg = size(rg, 1)
		size(cm, 1) > nseg && (cm = cm[1:nseg, :])   # drop trailing FG colour — it made the top node GRAY
		cz  = vcat(rg[:, 1], rg[end, 2])             # nseg+1 boundary z values
		cmn = vcat(cm, cm[end:end, :])               # nseg+1 colours: reuse last real colour at top boundary
		length(cz) == size(cmn, 1) || (cz = collect(range(zmn, zmx, length=size(cmn, 1))))
		return (cz, vec(permutedims(cmn)), size(cmn, 1))
	catch e
		@warn "makecpt failed; using the viewer's built-in ramp" exception=e
		return (Float64[], Float64[], 0)
	end
end

# Recolour a live grid window with a named GMT colormap (called from the viewer's colorbar chooser
# via the in-window console bridge, so `fig` is the QtFigure for that window). Recomputes CPT nodes
# over the grid's own data range and pushes them to the C side (gmtvtk_set_cpt), which mutates the
# shared colour transfer function so the surface, its LOD tiles and the colorbar all recolour.
function _recolor(fig::QtFigure, cmap)
	cz, crgb, n = _cpt_nodes(fig.G, cmap)
	n < 2 && return "colormap '$cmap' failed (makecpt)"
	ccall(_fn(:gmtvtk_set_cpt), Cvoid,
	      (Ptr{Cvoid}, Ptr{Float64}, Ptr{Float64}, Cint),
	      fig.h, cz, crgb, Cint(n))
	return "colormap: $cmap"
end
_recolor(fig, cmap) = "recolor: only grid windows have a colormap"

# Recolour ONE grid of a (possibly multi-grid) window. `gridSel` selects the target inside the C
# scene: -1 = the base relief surface, 0..N-1 = the Nth dropped/added grid. The C side supplies
# THAT grid's own [zmin,zmax] so the CPT spans the right grid — fixing the old behaviour where the
# colormap chooser on any group's Color Bar row always recoloured the first grid. Called from the
# viewer's colorbar chooser (applyColormap -> _console_eval, so `fig` is bound to this window).
function _recolor_grid(fig::QtFigure, cmap, zmin, zmax, gridSel)
	cz, crgb, n = _cpt_nodes_range(zmin, zmax, cmap)
	n < 2 && return "colormap '$cmap' failed (makecpt)"
	ccall(_fn(:gmtvtk_set_cpt_grid), Cvoid,
	      (Ptr{Cvoid}, Cint, Ptr{Float64}, Ptr{Float64}, Cint),
	      fig.h, Cint(gridSel), cz, crgb, Cint(n))
	return "colormap: $cmap"
end
_recolor_grid(fig, cmap, zmin, zmax, gridSel) = "recolor: only grid windows have a colormap"

# TEST PROBE: the RGB (each 0..1) that ONE grid's own lut maps `z` to. gridSel: -1 = base relief,
# 0..N-1 = the Nth extra grid. Returns nothing if the window/grid/lut is missing. Used by the suite to
# assert per-grid colorbar isolation (recolouring one grid must not change another's colours).
function _grid_rgb_at(h::Ptr{Cvoid}, gridSel::Integer, z::Real)
	out = zeros(Float64, 3)
	ok = ccall(_fn(:gmtvtk_grid_rgb_at), Cint,
	           (Ptr{Cvoid}, Cint, Cdouble, Ptr{Float64}), h, Cint(gridSel), Cdouble(z), out)
	return ok == 0 ? nothing : (out[1], out[2], out[3])
end

# z value -> "#rrggbb" via a GMTcpt colormap matrix (Mx3, stored 0-1 or 0-255). Mirrors GMTF3D z_to_hex.
function _z_to_hex(z, cmap::AbstractMatrix, zmin, zmax)
	N = size(cmap, 1)
	t = zmax > zmin ? (z - zmin) / (zmax - zmin) : 0.0
	i = clamp(round(Int, t * (N - 1)) + 1, 1, N)
	s = maximum(cmap) > 1.0 ? 1.0 : 255.0
	r = round(Int, clamp(cmap[i, 1] * s, 0, 255));  g = round(Int, clamp(cmap[i, 2] * s, 0, 255))
	b = round(Int, clamp(cmap[i, 3] * s, 0, 255))
	return string("#", lpad(string(r, base=16), 2, '0'), lpad(string(g, base=16), 2, '0'), lpad(string(b, base=16), 2, '0'))
end

# Resolve the factor that multiplies z (geog-aware :auto flat-slab). Mirrors GMTF3D _resolve_zscale.
function _resolve_zscale(zscale, dx, dy, dz, vfrac, isgeog, vexag)
	zscale === :auto || return float(zscale)
	(isgeog && vexag !== :auto) && return float(vexag) / _DEG2M
	horiz = max(dx, dy)
	(dz > 0 && horiz > 0) || return 1.0
	frac = isgeog ? _GEOG_VFRAC : vfrac
	return frac * horiz / dz
end
