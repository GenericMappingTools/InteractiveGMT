# grdvector.jl — GMT menu > "grdvector": the vector field of two grids, drawn over the window's map.
#
# The C++ dialog is GrdVectorDialog (70_window.cpp, loads deps/ui/grdvector_dialog.ui).
#
# ONE THING TO BE CLEAR ABOUT, because it decides everything below: grdvector is a PLOTTING module.
# Its entire output is PostScript — there is no -D dump, no table, no grid, nothing an API can hand
# back as geometry (grdcontour has -D and that is why the contour tool can run the real module; this
# one has no such door). So this dialog speaks grdvector's OPTIONS and works out the arrows here, then
# hands them to the ONE vector-field layer (`_gv_add_solid` -> gmtvtk_add_arrows_h), the same one File >
# Open xy(z) > "Arrow field" draws with. The arrow itself is THE arrow every arrow in the app is
# (arrowParts, 10_geometry.cpp — SACRED_LAW), with the module's -Q heads set on it.
#
# Two of the module's switches are therefore NOT offered, rather than offered as dead knobs:
#   -N (no clip)  our overlay is never clipped at a border — an arrow is drawn whole wherever it
#                 lands, so the viewer behaves as if -N were always on. A checkbox would say nothing.
#   -T (sign scale)  it adjusts Cartesian azimuths for NEGATIVE axis scales. This window's x and y
#                 always increase, so there is no sign to adjust for.
# The line WIDTH is not in the dialog either: every overlay in this app takes its width from the
# right-click Line Properties menu (and Preferences' default), and a second place to set it would be
# a second answer to the same question.
#
# What the module's own options DO map onto:
#   -A / -Z   polar (r, theta) input, and theta as an azimuth instead of a direction
#   -I        which nodes carry an arrow (every n-th, or one every dx/dy)
#   -S        how long an arrow is per unit of magnitude — see `_gv_factor` for the four modes
#   -Q        the head: which end(s), how long (a fraction of each arrow) and how wide. Its +n is
#             read and written to the console's GMT command, but a head that is a fraction of its
#             arrow already shrinks with every shorter arrow, so it changes nothing on screen
#   -C        colour by magnitude: solid arrows through the colormap, line arrows as N equal-width classes
#   -R        the sub-region to draw over

# A grid box may name a layer already in this window OR a file on disk — the same two-sided reference
# NSWING's Source/Nest boxes take. The EXACT lookup is deliberate: a name that does not match a live
# scene grid must fall through to being read as a path, never be silently swapped for the window's
# primary grid (which is what the loose `_find_object` would do with a typo).
function _gv_resolve(scene::Ptr{Cvoid}, ref::AbstractString, what::AbstractString)
	s = String(strip(ref))
	isempty(s) && error("no $what grid")
	G = _find_object_exact(scene, :grid, s)
	G === nothing || return G
	isfile(s) || error("$what grid not found, and no layer in this window is called '$s': $s")
	G = _gmtread_trb(s)                                  # grids are READ in "TRB" — THE reader
	isa(G, GMTgrid) || error("$what: '$s' is a $(typeof(G)), not a grid")
	return G
end

# Node coordinates + z laid out (ny,nx) with row 1 = south, which is what `_zmat` guarantees for ANY
# layout the grid was read in. The coordinates come off the range and the increment (honouring pixel
# registration) rather than off `.x`/`.y`, so they cannot disagree with the rows `_zmat` just handed us.
function _gv_axes(G::GMTgrid)
	Z = _zmat(G)
	ny, nx = size(Z)
	r = G.range
	inc = (hasproperty(G, :inc) && G.inc !== nothing && length(G.inc) >= 2) ? Float64.(G.inc[1:2]) : Float64[0.0, 0.0]
	reg = (hasproperty(G, :registration) && G.registration !== nothing) ? Int(G.registration) : 0
	dx = inc[1] > 0 ? inc[1] : (nx > 1 ? (r[2] - r[1]) / (nx - (reg == 1 ? 0 : 1)) : 1.0)
	dy = inc[2] > 0 ? inc[2] : (ny > 1 ? (r[4] - r[3]) / (ny - (reg == 1 ? 0 : 1)) : 1.0)
	x0 = r[1] + (reg == 1 ? dx / 2 : 0.0)
	y0 = r[3] + (reg == 1 ? dy / 2 : 0.0)
	x = [x0 + (i - 1) * dx for i in 1:nx]
	y = [y0 + (j - 1) * dy for j in 1:ny]
	return x, y, Z, dx, dy
end

# The module refuses two grids that are not on the same nodes ("the two grids must have identical
# dimensions"), and so does this — pairing (i,j) of one with (i,j) of the other is the whole premise.
function _gv_check_pair(G1::GMTgrid, G2::GMTgrid)
	x1, y1, Z1, dx1, dy1 = _gv_axes(G1)
	x2, y2, Z2, _, _ = _gv_axes(G2)
	size(Z1) == size(Z2) ||
		error("the two grids must have identical dimensions: $(size(Z1, 2))x$(size(Z1, 1)) against $(size(Z2, 2))x$(size(Z2, 1))")
	tol = max(abs(dx1), abs(dy1)) * 1e-6
	same = true
	isempty(x1) || (abs(x1[1] - x2[1]) <= tol) || (same = false)
	isempty(y1) || (abs(y1[1] - y2[1]) <= tol) || (same = false)
	same || error("the two grids cover different regions — $(G1.range[1:4]) against $(G2.range[1:4])")
	return x1, y1, Z1, Z2, dx1, dy1
end

# -I: how many nodes to step over. "auto" aims at ~`_GV_AUTO_ARROWS` arrows across each axis, which
# is the same idea GMT.jl's own grdvector wrapper uses when -I is absent (there it is the figure
# width over a maximum arrow length; here the window has no fixed width, so it is a node count).
const _GV_AUTO_ARROWS = 20

function _gv_step(d::Dict{String,String}, nx::Int, ny::Int, dx::Float64, dy::Float64)
	mode = _get(d, "incmode", "auto")
	mode == "auto" && return (max(1, round(Int, nx / _GV_AUTO_ARROWS)), max(1, round(Int, ny / _GV_AUTO_ARROWS)))
	sx = _get(d, "incx");  sy = _get(d, "incy")
	isempty(sx) && error("give the node step (or leave the spacing on automatic)")
	isempty(sy) && (sy = sx)
	vx = tryparse(Float64, sx);  vy = tryparse(Float64, sy)
	(vx === nothing || vy === nothing) && error("the node step must be numbers, not '$sx' / '$sy'")
	(vx > 0 && vy > 0) || error("the node step must be positive")
	mode == "x" && return (max(1, round(Int, vx)), max(1, round(Int, vy)))
	# mode == "inc": a spacing in data units, turned into whole nodes exactly as GMT.jl does.
	return (max(1, round(Int, vx / abs(dx))), max(1, round(Int, vy / abs(dy))))
end

# -A / -Z: the pair (c1, c2) read at a node -> the Cartesian (u, v). Without -A they already ARE
# (u, v); with -A they are (r, theta) with theta measured counter-clockwise from +x; -Z says that
# theta is instead an AZIMUTH, clockwise from north, and it implies -A (the module says so too).
function _gv_uv(c1::Float64, c2::Float64, polar::Bool, azim::Bool)
	polar || return (c1, c2)
	ang = azim ? (90.0 - c2) : c2
	s, c = sincosd(ang)
	return (c1 * c, c1 * s)
end

# -S, in the four shapes this window can actually mean:
#   auto     the largest arrow spans 0.9 of the decimated node spacing (Mirone's own autoscaling,
#            the same rule the Open xy(z) arrow import uses)
#   direct   `scale` is map units of length per unit of magnitude
#   inverse  GMT's own -Si: `scale` is magnitude per unit of length, so the factor is its reciprocal
#   fixed    GMT's own -Sl: every arrow is `scale` long and only its direction is data
# Returns (factor, fixedlen). `fixedlen > 0` means "ignore the factor, use this length".
function _gv_factor(d::Dict{String,String}, maxmag::Float64, spacing::Float64)
	mode = _get(d, "scalemode", "auto")
	if mode == "auto"
		maxmag > 0 || error("every vector has zero magnitude — nothing to draw")
		return (0.9 * spacing / maxmag, 0.0)
	end
	s = _get(d, "scale")
	isempty(s) && error("give the scale (or leave it on automatic)")
	v = tryparse(Float64, s)
	v === nothing && error("the scale must be a number, not '$s'")
	v > 0 || error("the scale must be positive")
	mode == "direct"  && return (v, 0.0)
	mode == "inverse" && return (1 / v, 0.0)
	mode == "fixed"   && return (0.0, v)
	error("unknown scale mode '$mode'")
end

# -Q: the head. `heads` is which end(s) carry one ("" none / "e" tip / "b" tail / "be" both), `len`
# its length as a FRACTION of the arrow (an absolute plot size means nothing in a window with no
# fixed size), `ang` the half-angle of its arms, and `norm` the length below which the head shrinks
# in proportion — which is what the module's +n modifier is for.
function _gv_head(d::Dict{String,String})
	heads = _get(d, "heads", "e")
	heads in ("", "e", "b", "be") || error("unknown head placement '$heads'")
	sl = _get(d, "headlen", "0.33")
	len = tryparse(Float64, sl)
	len === nothing && error("the head length must be a number, not '$sl'")
	(0 < len <= 1) || error("the head length is a fraction of the arrow: it must lie in (0, 1], not $len")
	sa = _get(d, "headang", "18")
	ang = tryparse(Float64, sa)
	ang === nothing && error("the head half-angle must be a number, not '$sa'")
	(0 < ang < 90) || error("the head half-angle must lie between 0 and 90 degrees, not $ang")
	sn = _get(d, "norm")
	norm = 0.0
	if !isempty(sn)
		v = tryparse(Float64, sn)
		v === nothing && error("the shrink length (+n) must be a number, not '$sn'")
		v > 0 || error("the shrink length (+n) must be positive")
		norm = v
	end
	return heads, len, tand(ang), norm
end

# The equal-width magnitude classes -C stands in for, and the colour each one wears. The ramp is the
# app's own (the one the scaled-symbol import paints z with), so a magnitude field reads the same way
# here as everywhere else: blue low, red high.
function _gv_class_color(k::Int, n::Int)
	t = n <= 1 ? 0.5 : (k - 1) / (n - 1)
	return (clamp(1.5 - abs(4t - 3), 0, 1), clamp(1.5 - abs(4t - 2), 0, 1), clamp(1.5 - abs(4t - 1), 0, 1))
end

# ---- a vector field as ONE symbol layer of arrows (gmtvtk_add_arrows_h) ---------------------------
# THE drawing of a vector field — the grdvector dialog (both its looks) and File > Open xy(z) > Import
# Arrow field come here, so a field can never be drawn two ways. Every arrow is THE arrow (arrowGlyphPD
# -> arrowParts), one instanced layer with ONE Scene Objects row, its own properties (single colour /
# colour by magnitude / length / heads and shape) and Remove.
#   x, y     tail positions; z = NaN stands the arrow on the displayed relief (the C side takes the
#            highest ground under its whole length, so a rigid arrow is not buried in a slope)
#   u, v     the vector (east, north); zero vectors must already be gone
#   len      WORLD length of the longest arrow (the map's y unit; x is cos-lat compressed by the window)
#   mags     what "magnitude" means for colour, hover and table — normally hypot(u, v); a fixed-length
#            field passes unit u, v and its real magnitudes here
#   cmap     colour by magnitude through this GMT CPT, or `nothing` for one flat `color`
#   rgb      one RGB (0..1) per arrow, 3n values, given outright (grdvector's -C classes); wins over cmap
function _gv_add_solid(scene::Ptr{Cvoid}, x::Vector{Float64}, y::Vector{Float64}, z::Vector{Float64},
                       u::Vector{Float64}, v::Vector{Float64}, name::String, len::Float64;
                       mags::Vector{Float64}=hypot.(u, v), cmap=:jet, color=:black,
                       rgb::Union{Nothing,Vector{Float64}}=nothing)::Nothing
	n = length(x)
	n > 0 || error("no vector to draw")
	(len > 0 && isfinite(len)) || error("arrow length must be a positive number")
	xyz = Vector{Float64}(undef, 3n); uv = Vector{Float64}(undef, 2n)
	for k in 1:n
		xyz[3k-2] = x[k]; xyz[3k-1] = y[k]; xyz[3k] = z[k]
		uv[2k-1] = u[k];  uv[2k] = v[k]
	end
	rgb === nothing && cmap !== nothing && (rgb = _gv_mag_rgb(mags, cmap))
	fr, fg, fb = _ovl_color(color, :lines)
	sc = mags ./ max.(hypot.(u, v), eps())            # back to the real components (fixed-length fields)
	ur = u .* sc;  vr = v .* sc
	info = join((string("u = ", round(ur[k], sigdigits=5), "\nv = ", round(vr[k], sigdigits=5),
	                    "\n|v| = ", round(mags[k], sigdigits=5)) for k in 1:n), '\x1e')
	ok = GC.@preserve xyz uv rgb ccall(_fn(:gmtvtk_add_arrows_h), Cint,
		(Ptr{Cvoid}, Ptr{Cdouble}, Ptr{Cdouble}, Cint, Cdouble, Ptr{Cdouble}, Cdouble, Cdouble, Cdouble, Cstring, Cstring),
		scene, xyz, uv, Cint(n), len, rgb === nothing ? C_NULL : pointer(rgb), fr, fg, fb, name, info)
	ok == 0 && error("the window refused the arrow field (closed, or nothing drawable)")
	# The layer's OWN data for "Show data table": the field itself, never a graphical property.
	fmt(a) = string(round(a, sigdigits=8))
	rows = join((join((fmt(x[k]), fmt(y[k]), fmt(ur[k]), fmt(vr[k]), fmt(mags[k])), '\x1f') for k in 1:n), '\x1e')
	ccall(_fn(:gmtvtk_symbol_set_table_h), Cint, (Ptr{Cvoid}, Cstring, Cstring, Cstring),
	      scene, name, join(("x", "y", "u", "v", "magnitude"), '\x1f'), rows)
	return nothing
end

# One RGB (0..1) per arrow: its magnitude through `cmap` spread over [0, max]. The nodes come from the
# app's ONE CPT builder (`_cpt_nodes_range`) and are read by its ONE lookup (`_cpt_lookup`), so an arrow
# field is coloured exactly as a grid with the same colormap would be.
function _gv_mag_rgb(mags::Vector{Float64}, cmap)::Union{Nothing,Vector{Float64}}
	mmax = maximum(mags)
	mmax > 0 || return nothing
	cz, crgb, nn = _cpt_nodes_range(0.0, mmax, cmap)
	nn == 0 && return nothing
	out = Vector{Float64}(undef, 3 * length(mags))
	for (k, m) in enumerate(mags)
		r, g, b = _cpt_lookup(cz, crgb, nn, m)
		out[3k-2] = r; out[3k-1] = g; out[3k] = b
	end
	return out
end

# "Colour by magnitude…" on an arrow layer's properties (50_scene.cpp -> console eval): read the
# layer's vectors back, colour them through `cmap`, hand the colours in. Errors go to the Errors tab.
function _recolor_arrows(scene::Ptr{Cvoid}, name::String, cmap::String)::Nothing
	n = ccall(_fn(:gmtvtk_arrows_get_uv_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}, Cint), scene, name, C_NULL, 0)
	n > 0 || error("no arrow layer called '$name' in this window")
	uv = Vector{Float64}(undef, 2n)
	ccall(_fn(:gmtvtk_arrows_get_uv_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}, Cint), scene, name, uv, n)
	rgb = _gv_mag_rgb([hypot(uv[2k-1], uv[2k]) for k in 1:n], cmap)
	rgb === nothing && error("colormap '$cmap' could not be built")
	ok = ccall(_fn(:gmtvtk_arrows_set_rgb_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}, Cint, Cdouble, Cdouble, Cdouble),
	           scene, name, rgb, Cint(n), 0.0, 0.0, 0.0)
	ok == 0 && error("could not recolour '$name'")
	return nothing
end

# The grdvector command line that would draw this same field in a GMT script. It is written to the
# window's console, not run: the module makes PostScript, and this dialog makes scene geometry.
function _gv_command(d::Dict{String,String}, g1::AbstractString, g2::AbstractString,
                     multx::Int, multy::Int, fac::Float64, maxlen::Float64,
                     heads::AbstractString, alpha::Float64, ang::AbstractString, norm::Float64)
	o = String[g1, g2]
	_on(d, "polar")   && push!(o, "-A")
	_on(d, "azimuth") && push!(o, "-Z")
	push!(o, "-Ix$(multx)/$(multy)")
	# -Si is GMT's "plot length per data unit", which is exactly what `fac` is here; the bare -S is
	# its reciprocal. The LENGTH UNIT is this window's map units, not the inches a real figure would
	# use, so the number is right in shape and wants rescaling to whatever -J the script sets up.
	push!(o, fac > 0 ? "-Si" * string(round(fac, sigdigits = 6)) : "-Sl" * string(round(maxlen, sigdigits = 6)))
	if !isempty(heads)
		# The dialog's head length is a FRACTION of the arrow; -Q wants an absolute size, so what
		# goes out is that fraction of the LONGEST arrow this run drew.
		q = "-Q" * string(round(alpha * maxlen, sigdigits = 4)) * "+a" * ang
		occursin('b', heads) && (q *= "+b")
		occursin('e', heads) && (q *= "+e")
		norm > 0 && (q *= "+n" * string(norm))
		push!(o, q)
	end
	r = (_get(d, "xmin"), _get(d, "xmax"), _get(d, "ymin"), _get(d, "ymax"))
	all(!isempty, r) && push!(o, "-R" * join(r, '/'))
	_on(d, "bymag") ? push!(o, "-C<cpt>") : push!(o, "-W" * _get(d, "color", "black"))
	return "grdvector " * join(o, ' ')
end

# Column names for the Data Viewer, so the field reads as physics instead of "col 5".
_gv_colnames(polar::Bool) = ["x", "y", "u", "v", "magnitude", polar ? "theta" : "direction"]

# C callback (Draw button): `cparams` is the newline-separated "key=value" block described in
# 30_app.cpp's JuliaGrdVectorFn. Returns Cint 1 on success, 0 on failure.
function _on_grdvector(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		d = _nswing_parse(unsafe_string(cparams))

		# The first component is the DISPLAYED layer unless a grid was named for it; the second is
		# always named, because a vector field is two grids and a window holds one.
		ref1 = _on(d, "usescene") ? _get(d, "grid") : _get(d, "grid1")
		G1 = _on(d, "usescene") ? _find_object(scene, :grid, ref1) : _gv_resolve(scene, ref1, "first component")
		G1 === nothing && error("no grid in this window — name a file for the first component")
		G2 = _gv_resolve(scene, _get(d, "grid2"), "second component")

		x, y, Z1, Z2, gdx, gdy = _gv_check_pair(G1, G2)
		ny, nx = size(Z1)
		multx, multy = _gv_step(d, nx, ny, gdx, gdy)

		azim  = _on(d, "azimuth")
		polar = _on(d, "polar") || azim                  # -Z implies -A, the module's own rule
		geog  = _on(d, "geog")

		# -R: the sub-region the arrows are drawn over. All four boxes or none.
		rb = (_get(d, "xmin"), _get(d, "xmax"), _get(d, "ymin"), _get(d, "ymax"))
		haveR = all(!isempty, rb)
		if haveR
			rv = tryparse.(Float64, rb)
			any(v -> v === nothing, rv) && error("the region takes four numbers")
			w, e, s, n = rv
			(w < e && s < n) || error("the region must have xmin < xmax and ymin < ymax")
		else
			w = e = s = n = 0.0
		end

		# Walk the decimated nodes once, keeping only what is finite, inside the region, and moving.
		px = Float64[]; py = Float64[]; pu = Float64[]; pv = Float64[]; pt = Float64[]
		for j in 1:multy:ny, i in 1:multx:nx
			xi = x[i];  yj = y[j]
			haveR && !(w <= xi <= e && s <= yj <= n) && continue
			c1 = Float64(Z1[j, i]);  c2 = Float64(Z2[j, i])
			(isfinite(c1) && isfinite(c2)) || continue
			u, v = _gv_uv(c1, c2, polar, azim)
			(isfinite(u) && isfinite(v)) || continue
			(u == 0 && v == 0) && continue               # a zero vector has no direction to draw
			push!(px, xi); push!(py, yj); push!(pu, u); push!(pv, v)
			push!(pt, polar ? c2 : atand(v, u))
		end
		isempty(px) && error("no vector survived — check the region, the spacing, and the grids' NaNs")

		mag = sqrt.(pu .^ 2 .+ pv .^ 2)
		cosmid = geog ? max(cosd((minimum(y) + maximum(y)) / 2), 0.01) : 1.0
		spacing = min(multx * abs(gdx) * cosmid, multy * abs(gdy))
		fac, fixedlen = _gv_factor(d, maximum(mag), spacing)
		heads, alpha, beta, norm = _gv_head(d)
		name = _get(d, "name", "grdvector")
		isempty(name) && (name = "grdvector")

		# THE arrow layer for both looks the dialog offers (SACRED_LAW: every arrow is THE arrow,
		# arrowParts): one layer through the ONE field builder, then the module's -Q heads on it. Its
		# length is the longest arrow's; a fixed-length scale hands unit vectors in and keeps the real
		# magnitudes for colour, hover and table. "Solid 3-D arrows" ticked: filled heads, a body in 3-D;
		# unticked: open heads, the field stays a flat drawing on the map in every view.
		solid = _get(d, "solid", "1") == "1"
		maxlen = fixedlen > 0 ? fixedlen : maximum(mag) * fac
		uu = fixedlen > 0 ? pu ./ mag : pu
		vv = fixedlen > 0 ? pv ./ mag : pv
		zz = _on(d, "drape") ? fill(NaN, length(px)) : zeros(length(px))
		# -C: solid arrows colour by magnitude through the colormap; line arrows by the dialog's N
		# equal-width classes on the app's blue-to-red ramp (`_gv_class_color`).
		classrgb = nothing
		if !solid && _on(d, "bymag")
			classes = max(2, min(24, something(tryparse(Int, _get(d, "nclass", "7")), 7)))
			mlo, mhi = extrema(mag)
			classrgb = Float64[]
			for m in mag
				k = mhi > mlo ? clamp(floor(Int, (m - mlo) / (mhi - mlo) * classes) + 1, 1, classes) : 1
				append!(classrgb, collect(_gv_class_color(k, classes)))
			end
		end
		_gv_add_solid(scene, px, py, zz, uu, vv, name, maxlen; mags = mag, rgb = classrgb,
		              cmap = _on(d, "bymag") ? :jet : nothing, color = _get(d, "color", "black"))
		kind = solid ? 1 : 2                             # ArrowHeadKind: AH_Arrow (filled) / AH_Open
		ccall(_fn(:gmtvtk_arrows_set_style_h), Cint,
		      (Ptr{Cvoid}, Cstring, Cint, Cint, Cdouble, Cdouble, Cint, Cint), scene, name,
		      occursin('b', heads) ? kind : 0, occursin('e', heads) ? kind : 0, alpha, beta, solid ? 1 : 0, solid ? 1 : 0)

		# The field itself (the nodes that got an arrow), for the Data Viewer and for a file.
		if _on(d, "table") || !isempty(_get(d, "outfile"))
			T = GMT.mat2ds(hcat(px, py, pu, pv, mag, pt))
			T.colnames = _gv_colnames(polar)
			out = _get(d, "outfile")
			isempty(out) || GMT.gmtwrite(String(out), T)
			_on(d, "table") && show_table(scene, T; name = name)
		end

		g1name = _on(d, "usescene") ? (isempty(ref1) ? "u.grd" : ref1) : _get(d, "grid1")
		_viewer_log_info(scene, string(length(px), " vectors drawn. The same field in a GMT script: ",
		                  _gv_command(d, g1name, _get(d, "grid2"), multx, multy, fac, maxlen,
		                              heads, alpha, _get(d, "headang", "18"), norm)))
		return Cint(1)
	catch e
		_tool_failed(scene, "grdvector", e)
		return Cint(0)
	end
end

function _register_grdvector()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdvector, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdvector_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
