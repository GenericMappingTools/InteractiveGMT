# grdvector.jl — GMT menu > "grdvector": the vector field of two grids, drawn over the window's map.
#
# The C++ dialog is GrdVectorDialog (70_window.cpp, loads deps/ui/grdvector_dialog.ui).
#
# ONE THING TO BE CLEAR ABOUT, because it decides everything below: grdvector is a PLOTTING module.
# Its entire output is PostScript — there is no -D dump, no table, no grid, nothing an API can hand
# back as geometry (grdcontour has -D and that is why the contour tool can run the real module; this
# one has no such door). So this dialog speaks grdvector's OPTIONS and builds the arrows here, then
# hands them to the same overlay builder every imported/drawn line goes through. The arrow shape is
# not invented for the occasion either: it is the one File > Open xy(z) > "Arrow field" already draws
# (`_import_xy_arrows`, importxy.jl — Mirone's draw_funs.m loc_quiver), so the two agree.
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
#   -Q        the head: which end(s), how long, how wide, and the +n length below which it shrinks
#   -C        colour by magnitude (as N equal-width classes, since one overlay carries one colour)
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

# THE arrow shape, for every caller that draws one: this dialog and File > Open xy(z) > "Arrow field"
# (`_import_xy_arrows`, importxy.jl, Mirone's draw_funs.m loc_quiver) — one function, never a second
# spelling of the same geometry (SACRED_LAW.md). loc_quiver is the case heads="e", shrink=1.
# One arrow: the shaft, plus a 3-point polyline (arm, point, arm) for each end that carries a head.
# `dx`/`dy` are the FINISHED displacement, scale and all — including, on a geographic grid, the extra
# 1/cos(lat) the caller put on dx, because the window draws x compressed by cos(midlat) and an
# eastward vector would otherwise come out shorter on screen than an equal northward one.
# `shrink` is the +n taper: 1 for an arrow at or above the shrink length, len/norm below it.
function _gv_arrow!(segs::Vector{Matrix{Float64}}, x::Float64, y::Float64, dx::Float64, dy::Float64,
                    heads::AbstractString, alpha::Float64, beta::Float64, shrink::Float64)
	tx = x + dx;  ty = y + dy
	push!(segs, [x y; tx ty])
	a = alpha * shrink
	a > 0 || return
	if occursin('e', heads)
		push!(segs, [tx-a*(dx+beta*dy) ty-a*(dy-beta*dx); tx ty; tx-a*(dx-beta*dy) ty-a*(dy+beta*dx)])
	end
	if occursin('b', heads)                             # the same head at the tail, pointing back
		push!(segs, [x+a*(dx+beta*dy) y+a*(dy-beta*dx); x y; x+a*(dx-beta*dy) y+a*(dy+beta*dx)])
	end
	return
end

# The equal-width magnitude classes -C stands in for, and the colour each one wears. The ramp is the
# app's own (the one the scaled-symbol import paints z with), so a magnitude field reads the same way
# here as everywhere else: blue low, red high.
function _gv_class_color(k::Int, n::Int)
	t = n <= 1 ? 0.5 : (k - 1) / (n - 1)
	return (clamp(1.5 - abs(4t - 3), 0, 1), clamp(1.5 - abs(4t - 2), 0, 1), clamp(1.5 - abs(4t - 1), 0, 1))
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

		# Build the geometry. Each arrow's displacement is its (u, v) times the common factor — or,
		# with the fixed-length scale, its direction alone. The x half carries the extra 1/cos(lat)
		# on a geographic grid (see `_gv_arrow!`).
		classes = _on(d, "bymag") ? max(2, min(24, something(tryparse(Int, _get(d, "nclass", "7")), 7))) : 1
		mlo, mhi = extrema(mag)
		bag = [Matrix{Float64}[] for _ in 1:classes]
		maxlen = 0.0
		for k in eachindex(px)
			len = fixedlen > 0 ? fixedlen : mag[k] * fac
			ux = fixedlen > 0 ? pu[k] / mag[k] * len : pu[k] * fac
			uy = fixedlen > 0 ? pv[k] / mag[k] * len : pv[k] * fac
			geog && (ux /= max(cosd(py[k]), 0.01))
			shrink = (norm > 0 && len < norm) ? len / norm : 1.0
			len > maxlen && (maxlen = len)
			cls = 1
			if classes > 1 && mhi > mlo
				cls = clamp(floor(Int, (mag[k] - mlo) / (mhi - mlo) * classes) + 1, 1, classes)
			end
			_gv_arrow!(bag[cls], px[k], py[k], ux, uy, heads, alpha, beta, shrink)
		end

		# Drape: without it the arrows lie flat at z = 0 (a map overlay, like the coastlines); with it
		# each vertex is lifted onto the DISPLAYED surface so the field follows the relief.
		Gd = _on(d, "drape") ? _find_object(scene, :grid, _get(d, "grid")) : nothing
		lift(m::Matrix{Float64}) = Gd === nothing ? m :
			hcat(m, [Float64(_sample_grid(Gd, m[r, 1], m[r, 2])) for r in axes(m, 1)])

		name = _get(d, "name", "grdvector")
		isempty(name) && (name = "grdvector")
		if classes == 1
			D = GMTdataset[GMT.mat2ds(lift(m)) for m in bag[1]]
			_add_dataset_to_scene(scene, D, name; color = _get(d, "color", "black"),
			                      forceMode = :lines, noConvertToPoints = true)
		else
			# One overlay per magnitude class (an overlay carries ONE colour), all under a single
			# Scene Objects group so the whole field still folds under one row with one checkbox.
			step = (mhi - mlo) / classes
			for k in 1:classes
				isempty(bag[k]) && continue
				D = GMTdataset[GMT.mat2ds(lift(m)) for m in bag[k]]
				lab = string(name, " ", round(mlo + (k - 1) * step, sigdigits = 4), " – ",
				             round(mlo + k * step, sigdigits = 4))
				_add_dataset_to_scene(scene, D, lab; groupName = name, color = _gv_class_color(k, classes),
				                      forceMode = :lines, noConvertToPoints = true)
			end
		end

		# The field itself (the nodes that got an arrow), for the Data Viewer and for a file.
		if _on(d, "table") || !isempty(_get(d, "outfile"))
			T = GMT.mat2ds(hcat(px, py, pu, pv, mag, pt))
			T.colnames = _gv_colnames(polar)
			out = _get(d, "outfile")
			isempty(out) || GMT.gmtwrite(String(out), T)
			_on(d, "table") && show_table(scene, T; name = name)
		end

		g1name = _on(d, "usescene") ? (isempty(ref1) ? "u.grd" : ref1) : _get(d, "grid1")
		_viewer_log_error(scene, string(length(px), " vectors drawn. The same field in a GMT script: ",
		                  _gv_command(d, g1name, _get(d, "grid2"), multx, multy, fac, maxlen,
		                              heads, alpha, _get(d, "headang", "18"), norm)))
		return Cint(1)
	catch e
		_viewer_log_error(scene, "grdvector FAILED: $(sprint(showerror, e))")
		@warn "grdvector FAILED" exception=(e,)
		return Cint(0)
	end
end

function _register_grdvector()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdvector, s, c)::Cint, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdvector_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
