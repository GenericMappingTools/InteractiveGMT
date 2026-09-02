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

# The colormap's NAME, for anything that has to say later which CPT a layer was coloured with —
# Save Session's recipe params and, through them, the GMT.jl script export (a script needs `cmap=`,
# and the C side keeps only the resolved LUT nodes: there is no cptName in the Scene, deliberately,
# because `makeGridCTF` is the one constructor and it takes nodes). A Symbol/String colormap has a
# name GMT can look up again; a GMTcpt OBJECT does not (it may be a palette read out of a data file),
# so it reports "" and the caller falls back to serializing the palette itself. `nothing` (the
# viewer's built-in ramp) is also "".
_cmap_tag(cmap)::String = (cmap isa Symbol || cmap isa AbstractString) ? String(cmap) : ""

# Build a CPT (plain `makecpt`, LINEAR over the data range) and return its control nodes: z
# values `cz` and matching RGB `crgb` (0..1, row-major), for a faithful vtkColorTransferFunction
# on the C side. Returns (Float64[], Float64[], 0) on failure -> viewer falls back to its ramp.
function _cpt_nodes(G::GMTgrid, cmap)
	cmap === nothing && return (Float64[], Float64[], 0)
	# ONE pass, NO allocation. `filter(isfinite, G.z)` materialised a second copy of the whole grid
	# just to take its extrema — a quarter of a gigabyte, and half a second, on a 60-Mpx satellite
	# band, on a path every single grid goes through.
	zmn, zmx = Inf, -Inf
	@inbounds for k in eachindex(G.z)
		v = Float64(G.z[k])
		isfinite(v) || continue
		(v < zmn) && (zmn = v)
		(v > zmx) && (zmx = v)
	end
	(isfinite(zmn) && zmx > zmn) || return (Float64[], Float64[], 0)
	return _cpt_nodes_range(zmn, zmx, cmap)
end

# CPT control nodes (z + rgb) over an explicit [zmn,zmx] range. Returns (Float64[], Float64[], 0)
# on failure -> viewer falls back to its built-in ramp.
function _cpt_nodes_range(zmn, zmx, cmap)
	(cmap === nothing || !(zmx > zmn)) && return (Float64[], Float64[], 0)
	# A CPT handed over as an OBJECT already carries the colours it means (a file's own embedded
	# palette, e.g. the `palette` variable of an OB.DAAC L3m grid). Its nodes are simply re-spread over
	# [zmn,zmx] — a round trip through `makecpt` could only resample, and would silently substitute the
	# viewer's ramp if it failed, i.e. throw the user's palette away.
	cmap isa GMTcpt && return _cpt_nodes_of(cmap, zmn, zmx)
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
		@tool_error "makecpt failed; using the viewer's built-in ramp" exception=e
		return (Float64[], Float64[], 0)
	end
end

# The control nodes of an EXPLICIT CPT, spread evenly over [zmn,zmx]. Colours are taken exactly as
# the CPT carries them (0-1 or 0-255, detected, never assumed), so a palette that came out of a data
# file reaches the screen unchanged. Same (cz, crgb, n) contract as `_cpt_nodes_range`.
function _cpt_nodes_of(C::GMTcpt, zmn, zmx)
	cm = Float64.(C.colormap)
	(isempty(cm) || size(cm, 2) < 3 || size(cm, 1) < 2) && return (Float64[], Float64[], 0)
	maximum(cm) > 1.0 && (cm = cm ./ 255)
	cm = cm[:, 1:3]
	n  = size(cm, 1)
	# The CPT's OWN slice boundaries, when it carries usable ones. A palette that came out of a data
	# file is not necessarily spread EVENLY in z — an OB.DAAC chlorophyll table is log-bunched, the way
	# NASA renders it — and re-spreading it evenly here would throw that away and hand back a ramp the
	# file never meant. Only when the CPT has no strictly increasing z of its own does the even spread
	# over [zmn,zmx] apply.
	rg = Float64.(C.range)
	if (size(rg, 1) == n && size(rg, 2) >= 2)
		cz = vcat(rg[:, 1], rg[end, 2])
		if (all(diff(cz) .> 0))
			return (cz, vec(permutedims(vcat(cm, cm[end:end, :]))), n + 1)
		end
	end
	return (collect(range(Float64(zmn), Float64(zmx), length = n)), vec(permutedims(cm)), n)
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
# Takes the WINDOW HANDLE, so EVERY grid layer's colormap can be changed — no window kind, no
# registration state, nothing else can make the chooser inert. It used to take the console's `fig` and
# be typed on QtFigure, with a stub for everything else; `fig` is bound only for a window a grid
# PROMOTED, so on a window whose grid arrived as an extra (a grid dropped onto an image, an Ocean
# Color subregion over its browse picture) there was no `fig` to bind and no method to reach —
# picking a colormap did nothing at all. Nothing here ever needed the figure: the target layer is
# `gridSel` and its scale is the [zmin,zmax] the C side resolved from the Color Bar row clicked.
function _recolor_grid(h::Ptr{Cvoid}, cmap, zmin, zmax, gridSel)
	cz, crgb, n = _cpt_nodes_range(zmin, zmax, cmap)
	n < 2 && return "colormap '$cmap' failed (makecpt)"
	ccall(_fn(:gmtvtk_set_cpt_grid), Cvoid,
	      (Ptr{Cvoid}, Cint, Ptr{Float64}, Ptr{Float64}, Cint),
	      h, Cint(gridSel), cz, crgb, Cint(n))
	return "colormap: $cmap"
end
# A window handle is what the viewer passes; keep the figure form working for anyone calling by hand.
_recolor_grid(fig, cmap, zmin, zmax, gridSel) = _recolor_grid(fig.h, cmap, zmin, zmax, gridSel)

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
