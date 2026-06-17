# CPT / colormap helpers: build vtkColorTransferFunction control nodes from GMT colormaps,
# plus the geographic detection and vertical-scale resolution shared by the viewers.

const _DEG2M     = 111194.9     # ~1 geographic degree in metres (GMT's value)
const _GEOG_VFRAC = 0.135       # geog auto: displayed z-range / horizontal extent (~vexag 20)

# GMT's own geographic flag (set from the grid's proj/CRS). peaks & plain mat2grid -> false;
# a grid/dataset with a geographic CRS -> true. No range heuristic (mis-flagged small grids).
_isgeographic(G::GMTgrid)::Bool = GMT.isgeog(G)

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
