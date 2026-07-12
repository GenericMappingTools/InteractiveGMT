# focal.jl — Geophysics > Seismology > Focal mechanisms (port of Mirone's focal_meca.m /
# utils/patch_meca.m). The C++ dialog (FocalMechanismsDialog, 70_window.cpp) hands a
# newline-separated "key=value" block to `_on_focal` (same _nswing_parse format as
# NSWING/Seismicity). Catalog reading: ISF -> GMT.gmtisf(focal=true) (GMT already extracts the
# CMT-convention mechanism columns — no need to hand-port Mirone's read_isf.c), Aki & Richards /
# Harvard CMT plain-column files -> GMT.gmtread(table=true), Harvard CMT .ndk -> a small
# fixed-width text parser (port of readHarvardCMT.m; no GMT reader for that format). Each event's
# nodal-plane geometry becomes the "beachball" compressive/dilatational patch outlines via
# _focal_patch_meca (a straight port of patch_meca.m's equal-area Schmidt projection — its dead
# P/T principal-axis branch, unused anywhere in patch_meca.m, is dropped; only the null axis N is
# needed), scaled by magnitude and packed as PLAIN closed polygons via gmtvtk_add_meca_h (mirrors
# gmtvtk_add_slip_patches_h but with no isSlip/isFault wiring — clicking a beachball just gets the
# ordinary polygon Remove/fill menu). Beachballs are schematic SYMBOLS drawn ROUND ON SCREEN
# (Mirone/psmeca convention): the unit disk is placed in degrees around the event with its X
# offsets pre-divided by the window's xfac (see the placement comment in _focal_plot). Every
# reader also returns the optional lon0/lat0 "anchor"
# columns (Aki cols 8-9 / CMT cols 12-13 — the plotted symbol's true position when it differs from
# the epicenter; ISF/.ndk have none, Mirone plots those at the epicenter itself) and, for ISF/.ndk
# (the two formats that carry one), the event date string for the "Plot event date" checkbox —
# _focal_plot draws the anchor as a thin line (gmtvtk_add_overlay_h) when it differs from the
# epicenter, and the date as a text label (gmtvtk_add_text_h) above the beachball.
#
# The region crop (W/E/S/N appended by the C++ menu action) is applied uniformly to EVERY catalog
# format, matching this app's own seismicity.jl convention rather than Mirone's original (which
# only cropped the plain-column formats, not ISF/.ndk).

const _MECA_NP = 45   # angular subdivision per nodal-plane arc (patch_meca.m's `np`)

# ── nodal-plane geometry (patch_meca.m) ─────────────────────────────────────────────────────

_focal_zero360(a) = a >= 360 ? a - 360 : a < 0 ? a + 360 : a

function _focal_null_axis_strike(str1::Float64, dip1::Float64, str2::Float64, dip2::Float64)
	sd1, cd1 = sind(dip1), cosd(dip1)
	sd2, cd2 = sind(dip2), cosd(dip2)
	ss1, cs1 = sind(str1), cosd(str1)
	ss2, cs2 = sind(str2), cosd(str2)
	cosphn = sd1*cs1*cd2 - sd2*cs2*cd1
	sinphn = sd1*ss1*cd2 - sd2*ss2*cd1
	if sind(str1 - str2) < 0
		cosphn, sinphn = -cosphn, -sinphn
	end
	phn = atand(sinphn, cosphn)
	return phn < 0 ? phn + 360 : phn
end

_focal_null_axis_dip(str1::Float64, dip1::Float64, str2::Float64, dip2::Float64) =
	abs(asind(clamp(sind(dip1) * sind(dip2) * sind(str1 - str2), -1.0, 1.0)))

# Auxiliary (second) nodal plane — used when the catalog only gives the first nodal plane
# (Aki & Richards). NOT patch_meca.m's computed_strike1/computed_dip1/computed_rake1 anymore:
# that trig-identity port matched the .m source line-for-line (checked repeatedly) yet produced a
# WRONG beachball for general oblique rake — caught because the render didn't match GMT's own
# `psmeca` for the identical mechanism (str=120,dip=45,rake=-30: GMT shows two opposite lobes,
# the old code produced one connected blob). Root cause confirmed independently: the auxiliary
# plane it computed does not satisfy the required self-consistency invariant that "the auxiliary
# of the auxiliary must recover the original plane" (a fundamental property of the two-nodal-plane
# representation of a double couple) — str2/dip2 came out right, only rake2 was off (~20° in the
# test case). Replaced with the standard normal-vector/slip-vector construction (Aki & Richards
# 1980-style; North-East-Down): build the fault normal `n` and slip vector `u` from
# (strike,dip,rake), get the auxiliary plane's (n,u) by SWAPPING them (n̂↔û — the defining relation
# between a fault plane and its auxiliary), then read the auxiliary strike/dip/rake back off that
# swapped pair. Verified: recovers the identity (str1,dip1,rake1) from its own (n,u) exactly, and
# the auxiliary-of-the-auxiliary recovers the ORIGINAL plane exactly, for rake spanning the full
# -180..180 range (dip-slip, strike-slip, and general oblique) — patch_meca.m's version only had
# this checked for its handful of hand-verified special cases (rake = 0, ±90).
function _focal_sdr_to_nu(str::Float64, dip::Float64, rake::Float64)
	n = (-sind(dip)*sind(str), sind(dip)*cosd(str), -cosd(dip))
	u = (cosd(rake)*cosd(str) + cosd(dip)*sind(rake)*sind(str),
	     cosd(rake)*sind(str) - cosd(dip)*sind(rake)*cosd(str),
	     -sind(rake)*sind(dip))
	return n, u
end

function _focal_nu_to_sdr(n::NTuple{3,Float64}, u::NTuple{3,Float64})
	if n[3] > 0
		n = .-n; u = .-u                       # keep the normal pointing down (dip ∈ [0,90])
	end
	dip = acosd(clamp(-n[3], -1.0, 1.0))
	sd, cd = sind(dip), cosd(dip)
	str = mod(atand(-n[1], n[2]), 360)
	cphi, sphi = cosd(str), sind(str)
	sinrake = -u[3] / sd
	cosrake = abs(cphi) > 1e-9 ? (u[1] - cd*sinrake*sphi)/cphi : (u[2] + cd*sinrake*cphi)/sphi
	return str, dip, atand(sinrake, cosrake)
end

function _focal_second_plane(str1::Float64, dip1::Float64, rake1::Float64)
	n, u = _focal_sdr_to_nu(str1, dip1, rake1)
	return _focal_nu_to_sdr(u, n)                  # auxiliary plane: normal <-> slip
end

# MATLAB's "[a:step:b b]" idiom: the colon run plus a forced exact endpoint (patch_meca.m does
# this 4 times for the nodal-plane arcs + 2 times for the closing arcs).
_focal_seq(a, step, b) = vcat(collect(a:step:b), b)

"""
	_focal_patch_meca(str1, dip1, rake1, str2=NaN, dip2=NaN, rake2=NaN) -> (comp, dilat, nodal1, nodal2)

Port of Mirone's `utils/patch_meca.m`: compute the compressive (`comp`) and dilatational
(`dilat`) quadrant outlines of a focal-mechanism "beachball" on the equal-area Schmidt
projection, PLUS the two nodal planes (`nodal1`, `nodal2`) as their own open rim-to-rim curves
(for stroking the beachball's internal boundary lines — see _focal_plot). Each is an Nx2 matrix
of (x,y) in the unit disk (radius 1, origin-centred). Give only the first nodal plane
(`str2=NaN`) to have the second one derived (Aki & Richards files); give both (ISF / Harvard
CMT / .ndk) to use them directly.
"""
function _focal_patch_meca(str1::Float64, dip1::Float64, rake1::Float64, str2::Float64=NaN, dip2::Float64=NaN, rake2::Float64=NaN)
	D2R = pi/180;  TWO_PI = 2pi
	abs(dip1 - 90) < 1e-5 && (dip1 = 90 - 1e-5)
	rake1 > 180 && (rake1 -= 360)
	rake1 -= sign(rake1) * 1e-5   # patch_meca.m's rake-singularity nudge (its |rake|<=180 guard is always true)
	if isnan(str2)
		str2, dip2, rake2 = _focal_second_plane(str1, dip1, rake1)
	else
		abs(dip2 - 90) < 1e-5 && (dip2 = 90 - 1e-5)
		rake2 > 180 && (rake2 -= 360)
		rake2 -= sign(rake2) * 1e-5
	end

	Nstr = _focal_null_axis_strike(str1, dip1, str2, dip2)

	# --- first nodal plane ---
	rot_s1 = [cosd(str1) -sind(str1); sind(str1) cosd(str1)]
	ang1 = (Nstr - str1) * D2R
	ang1 < 0 && (ang1 += TWO_PI)
	ang1 > pi && (ang1 -= pi)
	teta1 = _focal_seq(0.0, pi/_MECA_NP, ang1);  teta2 = _focal_seq(ang1, pi/_MECA_NP, pi)
	s1, s2 = sin.(teta1), sin.(teta2)
	radip = atan.(tand(dip1) .* s1);  rproj = sqrt(2) .* sin.((pi/2 .- radip) ./ 2)
	x1_P1, y1_P1 = rproj .* s1, rproj .* cos.(teta1)
	radip = atan.(tand(dip1) .* s2);  rproj = sqrt(2) .* sin.((pi/2 .- radip) ./ 2)
	x2_P1, y2_P1 = rproj .* s2, rproj .* cos.(teta2)
	plan1_a = hcat(x1_P1, y1_P1) * rot_s1
	plan1_b = hcat(x2_P1, y2_P1) * rot_s1

	# --- second nodal plane ---
	rot_s2 = [cosd(str2) -sind(str2); sind(str2) cosd(str2)]
	ang2 = (str2 - Nstr) * D2R
	ang2 < 0 && (ang2 += TWO_PI)
	ang2 > pi && (ang2 -= pi)
	teta1b = _focal_seq(0.0, pi/_MECA_NP, pi - ang2);  teta2b = _focal_seq(pi - ang2, pi/_MECA_NP, pi)
	s1b, s2b = sin.(teta1b), sin.(teta2b)
	radip = atan.(tand(dip2) .* s1b);  rproj = sqrt(2) .* sin.((pi/2 .- radip) ./ 2)
	x1_P2, y1_P2 = rproj .* s1b, rproj .* cos.(teta1b)
	radip = atan.(tand(dip2) .* s2b);  rproj = sqrt(2) .* sin.((pi/2 .- radip) ./ 2)
	x2_P2, y2_P2 = rproj .* s2b, rproj .* cos.(teta2b)
	plan2_a = hcat(x1_P2, y1_P2) * rot_s2
	plan2_b = hcat(x2_P2, y2_P2) * rot_s2

	# --- closing arcs (compressive/dilatational quadrant boundary) ---
	local ang_g, ang_l
	if rake1 >= 0
		a1 = str2 < str1 ? str2*D2R + pi : str2*D2R - pi
		ang_l, ang_g = minmax(a1, str1*D2R)
	else
		ang_g, ang_l = str1*D2R, str2*D2R
		ang_l > ang_g && (ang_g += TWO_PI)
	end
	teta = pi/2 .- _focal_seq(ang_l, pi/_MECA_NP, ang_g)
	arc1_x, arc1_y = cos.(reverse(teta)), sin.(reverse(teta))
	arc3_x, arc3_y = -reverse(arc1_x), -reverse(arc1_y)

	if rake1 >= 0
		if str2 > str1
			ang_g, ang_l = str1*D2R + pi, str2*D2R - pi
		else
			ang_g, ang_l = str1*D2R + pi, str2*D2R + pi
		end
	else
		ang_l, ang_g = str1*D2R, str2*D2R + pi
		(ang_g - ang_l) > TWO_PI && (ang_g -= TWO_PI)
		if ang_l > ang_g
			ang_l, ang_g = str2*D2R, str1*D2R - pi
			plan1_a, plan2_a = plan2_a, plan1_a
			plan1_b, plan2_b = plan2_b, plan1_b
		end
	end
	teta = pi/2 .- _focal_seq(ang_l, pi/_MECA_NP, ang_g)
	arc2_x, arc2_y = cos.(teta), sin.(teta)
	arc4_x, arc4_y = -reverse(arc2_x), -reverse(arc2_y)

	# --- assemble the two patches ---
	if rake1 >= 0
		pat1_x = vcat(plan1_a[:,1], plan1_b[:,1], arc3_x, plan2_a[:,1], plan2_b[:,1], arc1_x)
		pat1_y = vcat(plan1_a[:,2], plan1_b[:,2], arc3_y, plan2_a[:,2], plan2_b[:,2], arc1_y)
		pat2_x = vcat(plan2_b[:,1], arc2_x, reverse(plan1_b[:,1]), reverse(plan1_a[:,1]), arc4_x, plan2_a[:,1])
		pat2_y = vcat(plan2_b[:,2], arc2_y, reverse(plan1_b[:,2]), reverse(plan1_a[:,2]), arc4_y, plan2_a[:,2])
	else
		pat1_x = vcat(plan1_a[:,1], plan1_b[:,1], reverse(arc3_x), reverse(plan2_b[:,1]), reverse(plan2_a[:,1]), reverse(arc1_x))
		pat1_y = vcat(plan1_a[:,2], plan1_b[:,2], reverse(arc3_y), reverse(plan2_b[:,2]), reverse(plan2_a[:,2]), reverse(arc1_y))
		pat2_x = vcat(plan1_a[:,1], plan1_b[:,1], reverse(arc4_x), plan2_a[:,1], plan2_b[:,1], reverse(arc2_x))
		pat2_y = vcat(plan1_a[:,2], plan1_b[:,2], reverse(arc4_y), plan2_a[:,2], plan2_b[:,2], reverse(arc2_y))
	end
	# nodal1/nodal2: the two nodal planes as their own full rim-to-rim curves (plan*_a then plan*_b
	# is already one continuous curve, theta 0->pi — see above) — the CORRECT thing to stroke for the
	# beachball's internal boundary lines, straight from their own well-defined projection math,
	# rather than re-deriving them from comp/dilat's assembled fill outlines (comp and dilat are each
	# independently constructed and are NOT exact geometric complements of one another — they can
	# overlap in area at the shared boundary, so hunting for a "shared edge" between them there is
	# unreliable; tried it, it missed real seams and produced phantom lines through solid colour).
	nodal1 = vcat(plan1_a, plan1_b)
	nodal2 = vcat(plan2_a, plan2_b)
	return hcat(pat1_x, pat1_y), hcat(pat2_x, pat2_y), nodal1, nodal2   # comp, dilat, nodal1, nodal2 — unit-disk (x,y), origin-centred
end

# Even-odd point-in-polygon membership test (standard ray-cast). Kept as the reference
# membership definition for _focal_patch_meca's boundaries (used by tests/audits) — NOT a
# general utility, local to this file on purpose.
function _focal_pip(P::Matrix{Float64}, x::Float64, y::Float64)
	n = size(P, 1); inside = false; j = n
	@inbounds for i in 1:n
		xi, yi = P[i,1], P[i,2];  xj, yj = P[j,1], P[j,2]
		if ((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)
			inside = !inside
		end
		j = i
	end
	return inside
end

# ── sector construction (replaces patch_meca's comp/dilat fill rings) ───────────────────────
# patch_meca.m's comp/dilat boundaries are "there and back" walks spliced from arc pieces that
# self-touch AND self-overlap for many mechanisms; every triangulation strategy tried on them
# (ear-clipping, vtkContourTriangulator on the raw ring, split-into-sub-loops as a union, split
# as an even-odd multi-contour set) failed for SOME real mechanism, because the sub-loops can
# genuinely CROSS each other (even-odd cancellation area) — ill-posed input for any polygon
# renderer. So the fills don't use those rings at all anymore. A beachball is, by definition,
# the unit disk cut by the two nodal-plane curves into at most 4 SECTORS: the two curves
# (nodal1/nodal2 — each a clean rim-to-rim polyline straight from its own projection formula)
# cross exactly once inside the disk (at the null-axis projection P), so each sector is
# rim-arc + one half of one curve (rim -> P) + one half of the other (P -> rim) — a SIMPLE
# polygon by construction, trivially triangulatable. Each sector is coloured by the P-wave
# first-motion sign (the definition of compressive/dilatational) evaluated at an interior
# point via the standard double-couple radiation pattern sign((n·d)(u·d)) — the same formula
# _focal_patch_meca's output was verified against.

# P-wave first-motion sign at unit-disk point (x,y) (equal-area lower hemisphere, y = North,
# x = East): true -> compressive quadrant, false -> dilatational.
function _focal_comp_at(str::Float64, dip::Float64, rake::Float64, x::Float64, y::Float64)
	n, u = _focal_sdr_to_nu(str, dip, rake)
	th = 2 * asin(clamp(hypot(x, y) / sqrt(2), 0.0, 1.0))       # take-off angle from vertical(down)
	az = atan(x, y)
	d = (sin(th)*cos(az), sin(th)*sin(az), cos(th))
	return (n[1]*d[1] + n[2]*d[2] + n[3]*d[3]) * (u[1]*d[1] + u[2]*d[2] + u[3]*d[3]) > 0
end

# Crossing of the two nodal polylines: the (i, j, point) minimising segment-segment distance.
# The curves cross exactly once (at the null-axis projection); closest-approach of the sampled
# polylines is a robust stand-in for the exact intersection at _MECA_NP resolution.
function _focal_curve_cross(c1::Matrix{Float64}, c2::Matrix{Float64})
	best = Inf;  bi = 1;  bj = 1
	for i in axes(c1, 1), j in axes(c2, 1)
		d2 = (c1[i,1]-c2[j,1])^2 + (c1[i,2]-c2[j,2])^2
		d2 < best && ((best, bi, bj) = (d2, i, j))
	end
	P = [(c1[bi,1] + c2[bj,1])/2  (c1[bi,2] + c2[bj,2])/2]
	return bi, bj, P
end

"""
	_focal_sectors(str1, dip1, rake1, nodal1, nodal2) -> Vector{(poly::Matrix, iscomp::Bool)}

Cut the unit disk into its (≤4) beachball sectors along the two nodal curves and classify each
as compressive/dilatational by the radiation-pattern sign at an interior sample. Every returned
polygon is SIMPLE (rim arc + two curve halves meeting at the crossing point).
"""
function _focal_sectors(str1::Float64, dip1::Float64, rake1::Float64, nodal1::Matrix{Float64}, nodal2::Matrix{Float64})
	i1, i2, P = _focal_curve_cross(nodal1, nodal2)
	# halves, each stored rim -> P (crossing point appended/prepended so both halves meet at P)
	halves = Dict{Tuple{Int,Int},Matrix{Float64}}(       # (curve id, which end) -> rim->P polyline
		(1, 1) => vcat(nodal1[1:i1, :], P), (1, 2) => vcat(reverse(nodal1[i1:end, :], dims=1), P),
		(2, 1) => vcat(nodal2[1:i2, :], P), (2, 2) => vcat(reverse(nodal2[i2:end, :], dims=1), P))
	ends = [(atan(nodal1[1,2], nodal1[1,1]), 1, 1), (atan(nodal1[end,2], nodal1[end,1]), 1, 2),
	        (atan(nodal2[1,2], nodal2[1,1]), 2, 1), (atan(nodal2[end,2], nodal2[end,1]), 2, 2)]
	sort!(ends, by = first)
	out = Tuple{Matrix{Float64},Bool}[]
	for k in 1:4
		a0, c0, e0 = ends[k]
		a1, c1, e1 = ends[mod1(k + 1, 4)]
		a1 <= a0 && (a1 += 2pi)
		arc = [ (cos(t), sin(t)) for t in range(a0, a1, length = max(3, ceil(Int, (a1 - a0) / (pi/_MECA_NP)))) ]
		h_out = reverse(halves[(c0, e0)], dims=1)         # P -> rim endpoint e0 … then arc … then
		h_in  = halves[(c1, e1)]                          # rim endpoint e1 -> P
		poly = vcat(h_out, [first.(arc) last.(arc)], h_in)
		size(poly, 1) < 3 && continue
		# interior sample: walk from P a little toward the arc midpoint (stays inside the sector)
		amid = (a0 + a1) / 2
		iscomp = false;  found = false
		for t in (0.5, 0.3, 0.7, 0.15, 0.9)
			q1 = (1-t)*P[1] + t*cos(amid);  q2 = (1-t)*P[2] + t*sin(amid)
			if _focal_pip(poly, q1, q2)
				iscomp = _focal_comp_at(str1, dip1, rake1, q1, q2);  found = true
				break
			end
		end
		found || (iscomp = _focal_comp_at(str1, dip1, rake1, (P[1]+cos(amid))/2, (P[2]+sin(amid))/2))
		push!(out, (poly, iscomp))
	end
	return out
end

# Called from C++ (BeachballWidget, via g_juliaEval) by the Focal Meca Studio demo dialog on every
# Strike/Dip/Rake change — serializes _focal_sectors' output (fills) + the two nodal curves (the
# ALWAYS-black boundary lines) so the widget draws the EXACT SAME geometry as the real catalog
# beachballs (_focal_plot below), never its own re-approximation — the "three laws" (see
# .wolf/cerebrum.md "focal-beachball-three-laws") were only hard-won once, here, and must not be
# re-derived a second time in C++.
# Output: "<iscomp 0|1>:x,y;x,y;...|<iscomp>:...|#N1:x,y;...;#N2:x,y;...;" (unit-disk, origin-centred,
# 6 significant digits). Prints nothing on any failure — the C++ side keeps its schematic fallback.
function _focal_demo_sectors(str1::Float64, dip1::Float64, rake1::Float64)
	try
		_comp, _dilat, nodal1, nodal2 = _focal_patch_meca(str1, dip1, rake1)
		for (poly, iscomp) in _focal_sectors(str1, dip1, rake1, nodal1, nodal2)
			print(iscomp ? '1' : '0', ':')
			for i in axes(poly, 1)
				print(round(poly[i,1], sigdigits=6), ',', round(poly[i,2], sigdigits=6), ';')
			end
			print('|')
		end
		print("#N1:")
		for i in axes(nodal1, 1)
			print(round(nodal1[i,1], sigdigits=6), ',', round(nodal1[i,2], sigdigits=6), ';')
		end
		print("#N2:")
		for i in axes(nodal2, 1)
			print(round(nodal2[i,1], sigdigits=6), ',', round(nodal2[i,2], sigdigits=6), ';')
		end
	catch
	end
	return nothing
end

# ── catalog readers ─────────────────────────────────────────────────────────────────────────
# All return (lon, lat, dep, mag, str1, dip1, rake1, str2, dip2, rake2, plon, plat, date).
# str2/dip2/rake2 are NaN-filled for Aki & Richards files (only one nodal plane given —
# _focal_patch_meca derives the second, same as Mirone's define_second_plane). plon/plat is the
# ANCHOR position the beachball is actually drawn at (defaults to lon/lat when the catalog carries
# no separate anchor columns) — Mirone draws a thin line from (lon,lat) to (plon,plat) when they
# differ. date is a per-event "" (unknown) or a display string for the "Plot event date" option.

_focal_none() = (Float64[], Float64[], Float64[], Float64[], Float64[], Float64[], Float64[],
                 Float64[], Float64[], Float64[], Float64[], Float64[], String[])

function _focal_matrix(file)::Matrix{Float64}
	D = GMT.gmtread(file; table=true)
	return D isa GMTdataset ? D.data : reduce(vcat, (seg.data for seg in D))
end

# Aki & Richards: lon,lat,depth,strike,dip,rake,mag[,lon0,lat0[,extra]] (Mirone n_column 7/9/10 —
# columns 8-9, when present, are the anchor position the beachball is actually plotted at).
function _focal_aki(file::String)
	m = _focal_matrix(file)
	nc = size(m, 2)
	nc >= 7 || error("Aki & Richards file: expected >=7 columns (lon,lat,depth,strike,dip,rake,mag), got $nc")
	n = size(m, 1);  nan = fill(NaN, n)
	plon, plat = nc >= 9 ? (m[:,8], m[:,9]) : (m[:,1], m[:,2])
	return m[:,1], m[:,2], m[:,3], m[:,7], m[:,4], m[:,5], m[:,6], nan, nan, nan, plon, plat, fill("", n)
end

# Harvard CMT convention (plain columns): lon,lat,depth,str1,dip1,rake1,str2,dip2,rake2,mantissa,
# exponent[,lon0,lat0[,extra]] (Mirone n_column 11/13/14 — columns 12-13 are the anchor).
function _focal_cmt(file::String)
	m = _focal_matrix(file)
	nc = size(m, 2)
	nc >= 11 || error("Harvard CMT file: expected >=11 columns (lon,lat,depth,str1,dip1,rake1,str2,dip2,rake2,mantissa,exponent), got $nc")
	n = size(m, 1)
	mag = @. (log10(m[:,10]) + m[:,11] - 9.1) * 2/3
	plon, plat = nc >= 13 ? (m[:,12], m[:,13]) : (m[:,1], m[:,2])
	return m[:,1], m[:,2], m[:,3], mag, m[:,4], m[:,5], m[:,6], m[:,7], m[:,8], m[:,9], plon, plat, fill("", n)
end

# ISF catalog, cropped to the visible region + restricted to events carrying a focal mechanism by
# gmtisf itself (`focal=true` = Global CMT convention: str1,dip1,rake1,str2,dip2,rake2,mantissa,
# exponent). GMT already parses the ISF moment-tensor blocks — no need to hand-port read_isf.c. No
# `abstime`: the discrete year/month/day columns are read directly for the "day/month/year" event
# date label (Mirone's `sprintf('%d/%d/%d', day, month, year)`) — an anchor is not a thing ISF
# carries, so plon/plat = lon/lat (matches Mirone: `handles.plot_pos = [out_d(1,:)' out_d(2,:)']`).
function _focal_isf(file::String, W::Float64, E::Float64, S::Float64, N::Float64)
	D = GMT.gmtisf(file; R=(W, E, S, N), focal=true)
	size(D, 1) == 0 && return _focal_none()
	m = D.data
	ci(name) = findfirst(==(name), D.colnames)
	mantissa = m[:, ci("mantissa")];  expo = m[:, ci("exponent")]
	# ISF carries 9999999-style sentinels for a missing scalar moment (seen in real catalogs:
	# exponent = 9.999999e6 -> "Mw 6666660" poisoning the dialog's Max-magnitude prefill and, if
	# kept, a planet-sized beachball). Any physically impossible moment -> NaN magnitude; the
	# plot loop and the filter both already skip NaN-mag events.
	mag = @. ifelse((mantissa > 0) & (expo < 40), (log10(mantissa) + expo - 9.1) * 2/3, NaN)
	yy, mo, dd = m[:,ci("year")], m[:,ci("month")], m[:,ci("day")]
	date = [string(round(Int, dd[i]), "/", round(Int, mo[i]), "/", round(Int, yy[i])) for i in axes(m,1)]
	lon, lat = m[:,ci("lon")], m[:,ci("lat")]
	return lon, lat, m[:,ci("depth")], mag,
	       m[:,ci("strike1")], m[:,ci("dip1")], m[:,ci("rake1")],
	       m[:,ci("strike2")], m[:,ci("dip2")], m[:,ci("rake2")], lon, lat, date
end

# Harvard CMT .ndk catalog: fixed 5-lines-per-event text format (port of readHarvardCMT.m). Line 1
# (hypocenter) carries lat/lon/depth at chars 28:47 and the event date at chars 6:9 (year), 10:13
# ("/MM/"), 15 (2nd day digit) — Mirone's own `[todos{n}(15:15) todos{n}(10:13) todos{n}(6:9)]`
# slice, ported literally (including its odd field order — not our place to silently "fix" a
# historical Mirone quirk); line 4 (moment-tensor header) carries the exponent at chars 1:2; line 5
# (principal-axes/nodal-planes line) carries str1,dip1,rake1,str2,dip2,rake2 at chars 58:80 and the
# scalar-moment mantissa at chars 52:56. plon/plat = lon/lat (Mirone: `handles.plot_pos =
# handles.data(:,1:2)`). A malformed record stops reading there and keeps every event parsed so far
# (matches Mirone's catch behaviour).
function _focal_ndk(file::String)
	lines = readlines(file)
	nev = length(lines) ÷ 5
	nev == 0 && return _focal_none()
	lon = Vector{Float64}(undef, nev);  lat = similar(lon);  dep = similar(lon);  mag = similar(lon)
	str1 = similar(lon);  dip1 = similar(lon);  rake1 = similar(lon)
	str2 = similar(lon);  dip2 = similar(lon);  rake2 = similar(lon)
	date = fill("", nev)
	k = 0
	try
		for kk in 1:nev
			k = kk
			base = (kk - 1) * 5
			l1 = lines[base+1];  l4 = lines[base+4];  l5 = lines[base+5]
			la, lo, dp = parse.(Float64, split(strip(l1[28:47])))
			lon[kk] = lo;  lat[kk] = la;  dep[kk] = dp
			date[kk] = string(l1[15]) * l1[10:13] * l1[6:9]
			mexp = parse(Float64, strip(l4[1:2])) - 7           # -7: dyn-cm exponent -> N-m (SI)
			vals = parse.(Float64, split(strip(l5[58:80])))
			str1[kk], dip1[kk], rake1[kk], str2[kk], dip2[kk], rake2[kk] = vals
			mman = parse(Float64, strip(l5[52:56]))
			M0 = mman * 10.0^mexp
			mag[kk] = 2/3 * (log10(M0) - 9.1)                   # Mw
		end
	catch e
		k <= 1 && rethrow()
		lon=lon[1:k-1]; lat=lat[1:k-1]; dep=dep[1:k-1]; mag=mag[1:k-1]
		str1=str1[1:k-1]; dip1=dip1[1:k-1]; rake1=rake1[1:k-1]
		str2=str2[1:k-1]; dip2=dip2[1:k-1]; rake2=rake2[1:k-1]
		date=date[1:k-1]
	end
	return lon, lat, dep, mag, str1, dip1, rake1, str2, dip2, rake2, lon, lat, date
end

# ── filter + plot ───────────────────────────────────────────────────────────────────────────

# Visible region ∩ magnitude ∩ depth, applied uniformly to EVERY catalog format (see the file
# header note — this app's own convention, reusing seismicity.jl's "region" dialog field parser).
function _focal_filter(d, lon, lat, dep, mag)
	W, E, S, N = _seis_region(d)
	m0 = something(tryparse(Float64, _get(d, "magmin")), 1.0)
	m1 = something(tryparse(Float64, _get(d, "magmax")), 10.0)
	z0 = something(tryparse(Float64, _get(d, "depmin")), 0.0)
	z1 = something(tryparse(Float64, _get(d, "depmax")), 900.0)
	n = length(lon)
	keep = BitVector(undef, n)
	@inbounds for i in 1:n
		keep[i] = (W <= lon[i] <= E) && (S <= lat[i] <= N) &&
		          (m0 <= mag[i] <= m1) && (z0 <= dep[i] <= z1)
	end
	return keep
end

# N-S radius (km) that a Mw-5 beachball gets when the dialog's "Magnitude 5 size" field holds
# the Mirone-default 0.8 cm — 2% of the W/E/S/N region's width, so 0.8 cm always reads as a
# sensibly-sized ball at any zoom. `_focal_cm_to_km` (below) rescales any OTHER cm value against
# this same reference, so the two can never drift apart (same-quantity-same-function).
_focal_mag5_default(W, E, S, N) = max(1.0, 0.02 * (E - W) * 111.32 * cosd((S + N) / 2))

# Mirone's own historical default: the "Magnitude 5 size" field is a PRINTED-cm symbol size
# (focal_meca.m / psmeca -S convention), same as the .ui's shipped "0.8". There is no fixed page
# here to give cm a literal ground meaning, so a cm value is a MULTIPLIER on `_focal_mag5_default`:
# entering the reference 0.8 cm reproduces that 2%-of-region size exactly; 1.6 cm doubles it, 0.4 cm
# halves it, etc. Restores cm as the user-facing unit (never km — that was a 2026-07-04 workaround
# that also broke because the .ui's own "0.8" default was then read as 0.8 KILOMETRES = sub-pixel).
const _FOCAL_MAG5_REF_CM = 0.8
_focal_cm_to_km(cm, W, E, S, N) = (cm / _FOCAL_MAG5_REF_CM) * _focal_mag5_default(W, E, S, N)

# Called from C++ (FocalMechanismsDialog, via g_juliaEval) the MOMENT a catalog file is picked or
# the format selection changes — i.e. from the DIALOG itself, before OK is ever clicked. Does two
# things with the one read: (1) on an EMPTY launcher, frames the basemap to the file's own extent
# RIGHT THERE (reusing the same _on_basemap crop-and-promote path _on_focal uses at plot time —
# so the user sees the map while still picking filters, not only after clicking OK); (2) prefills
# every data-derived filter box from the catalog's own values — Min/Max magnitude, Min/Max depth —
# plus resets "Magnitude 5 size" back to Mirone's fixed 0.8 cm reference (_FOCAL_MAG5_REF_CM; NOT
# data-derived — cm is a multiplier on the region-relative default, same value for every catalog)
# so the dialog never sits there with a leftover value from a previously-picked file.
# `print`s a single "mag5/minmag/maxmag/mindepth/maxdepth" line (same g_juliaEval convention as
# _fault_lenaz: plain stdout, no `show`-quoting) and returns `nothing`; the C++ side splits on '/'.
# Prints nothing on any read failure (wrong format for this file yet, bad/partial path): the C++
# side leaves the map and every field alone in that case.
function _focal_peek_and_frame(scene::Ptr{Cvoid}, file::String, fmt::Int)
	try
		isfile(file) || return nothing
		lon, lat, dep, mag = fmt == 1 ? _focal_isf(file, -180.0, 180.0, -90.0, 90.0)[1:4] :
		           fmt == 2 ? _focal_aki(file)[1:4] :
		           fmt == 3 ? _focal_cmt(file)[1:4] :
		           fmt == 4 ? _focal_ndk(file)[1:4] : (return nothing)
		isempty(lon) && return nothing
		W, E = extrema(lon); S, N = extrema(lat)
		if ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
			pad(lo, hi) = (hi - lo) < 1e-6 ? (lo - 1.0, hi + 1.0) : (lo - 0.05 * (hi - lo), hi + 0.05 * (hi - lo))
			Wd, Ed = pad(W, E); Sd, Nd = pad(S, N)
			Wd = max(Wd, -180.0); Ed = min(Ed, 180.0); Sd = max(Sd, -90.0); Nd = min(Nd, 90.0)
			_on_basemap(scene, "$Wd/$Ed/$Sd/$Nd/0/region")
			ccall(_fn(:gmtvtk_process_events), Cint, ())
		end
		finmag = filter(isfinite, mag)               # sentinel moments are NaN-mag (see _focal_isf)
		m0, m1 = isempty(finmag) ? (NaN, NaN) : extrema(finmag)
		z0, z1 = extrema(dep)
		print(_FOCAL_MAG5_REF_CM, '/', m0, '/', m1, '/', z0, '/', z1)
	catch
	end
	return nothing
end

# Pack every kept event's compressive+dilatational patches and hand the whole catalog to
# gmtvtk_add_meca_h in ONE call — each patch already carries its own explicit fill colour, so
# (unlike seismicity's per-bucket symbol layers) there is no need to split into separate objects.
#
# PLACEMENT: beachballs are schematic SYMBOLS and must be ROUND ON SCREEN at any latitude —
# the Mirone (printed patch) / psmeca (paper symbol) convention. Geodesically-true footprints
# were tried (GMT.geod destination per disk point) and rejected: physically correct but visually
# WRONG — an ellipse stretched 1/cos(lat) on the degree axes ("deformed" bug report) — and the
# per-event geod batches dominated plot time on real catalogs. Instead the unit disk goes out in
# plain degrees with its X offset pre-divided by the window's own X actor scale
# (gmtvtk_get_xfac = cos(midlat)); the actor scale then multiplies it back, making screen X/Y
# offsets exactly equal — a perfect circle on ANY window, no geodesy needed.
# `mag5size` is the dialog's "Magnitude 5 size" field in **centimetres** — Mirone's own
# printed-symbol convention (focal_meca.m / psmeca -S), default 0.8. Converted to an N-S radius in
# KM via `_focal_cm_to_km` (a multiplier on the region-relative default), never taken as a literal
# ground distance — see that function's comment.
function _focal_plot(scene::Ptr{Cvoid}, d, lon, lat, dep, mag, str1, dip1, rake1, str2, dip2, rake2,
                      plon, plat, date, idx)
	# mag5size = cm size of a Mw-5 beachball. Empty/zero/garbage falls back to the reference 0.8 cm
	# (_FOCAL_MAG5_REF_CM) — i.e. the same 2%-of-region default as leaving the dialog untouched.
	cm5 = something(tryparse(Float64, _get(d, "mag5size")), 0.0)
	cm5 <= 0 && (cm5 = _FOCAL_MAG5_REF_CM)
	W, E, S, N = _seis_region(d)
	mag5 = _focal_cm_to_km(cm5, W, E, S, N)      # N-S radius in KM for this event's Mw-5 reference
	# X actor scale of this window (cos(midlat) geographic, 1 cartesian) — needed for
	# screen-round symbol placement, see the comment at the vertex loop below.
	xfac = ccall(_fn(:gmtvtk_get_xfac), Cdouble, (Ptr{Cvoid},), scene)
	(isfinite(xfac) && xfac > 0) || (xfac = 1.0)
	depcolors = _on(d, "depcolors")
	plotdate = _on(d, "plotdate")
	# Date-label styling — overridable from the group's properties dialog (mecaGroupPropsDialog,
	# 50_scene.cpp): "datefont"/"datefontsize"/"datecolor" ("r/g/b" 0-255, same convention as
	# compcolor/dilatcolor/rimcolor)/"datebold"/"dateitalic". Default Arial 7 BLACK — NOT TextLabel's
	# own yellow default, which washes out illegibly over light relief/basemap backgrounds; small size
	# since a catalog packs one date per event right above each ball.
	datefont = _get(d, "datefont", "Arial")
	datefontsize = something(tryparse(Int, _get(d, "datefontsize")), 7)
	datebold = _on(d, "datebold")
	dateitalic = _on(d, "dateitalic")
	# Outline (rim + nodal lines) width — dialog/props field "rimwidth", 0..1-ish; ×100 becomes
	# the stroke's PIXEL width in C++ (0.010 → 1 px). The black lines are a REQUIRED part of a
	# beachball (they ALWAYS separate the two quadrant colours) — a zero/negative width falls
	# back to the default, never "no outline".
	rimfrac = something(tryparse(Float64, _get(d, "rimwidth")), 0.010)
	rimfrac <= 0 && (rimfrac = 0.010)
	compnames = ntuple(k -> (c = _get(d, "c$k"); isempty(c) ? _SEIS_DEF_COLOR[k] : c), 5)
	# Solid compression/dilatation/rim colours — the group's properties dialog (Scene Objects,
	# 50_scene.cpp mecaGroupPropsDialog) edits these via "compcolor"/"dilatcolor"/"rimcolor"
	# ("r/g/b" 0-255, _parse_gmt_color's convention). depcolors (per-depth-bucket) OVERRIDES compcol
	# per-event below regardless of this default — the two colour mechanisms are independent.
	_d2f(hexrgb, fallback) = isempty(hexrgb) ? fallback : ((c = _parse_gmt_color(hexrgb)); (c[1]/255, c[2]/255, c[3]/255))
	compcolor0  = _d2f(_get(d, "compcolor"),  (0.0, 0.0, 0.0))
	dilatcolor0 = _d2f(_get(d, "dilatcolor"), (1.0, 1.0, 1.0))
	rimcolor0   = _d2f(_get(d, "rimcolor"),   (0.0, 0.0, 0.0))
	datecolor   = _d2f(_get(d, "datecolor"),  (0.0, 0.0, 0.0))   # black — yellow washed out over light relief
	white = dilatcolor0
	xy = Float64[];  vcounts = Cint[];  rgb = Float64[];  evid = Cint[]
	# Beachballs are OPAQUE symbols: one ball is an indivisible unit in the depth order — its
	# fills and lines stack together, and NOTHING from another event may bleed through it. Rank =
	# ei*3+role (event index dominates; role — dilat=0 < comp=1 < lines=2 — only orders WITHIN one
	# ball). Draw order is magnitude DESCENDING, so a smaller ball always plots ON TOP of any
	# bigger overlapping one and stays fully visible — the reverse order would bury it whole
	# (the old "impossible all-black beachball"), and the role-band scheme tried before instead
	# let every event's comp fill punch through every other event's dilat fill (not opaque).
	order = sort(collect(idx); by = i -> isnan(mag[i]) ? Inf : mag[i], rev=true)
	ei = -1     # 0-based EVENT index — bumped once per KEPT event, shared by every patch it contributes
	# Anchor lines + date labels are ACCUMULATED across the event loop and sent in ONE C call each
	# after it — the per-event gmtvtk_add_text_h/gmtvtk_add_overlay_h calls each did a full Scene
	# Objects rebuild + window render, which is what made "Plot event date" take ~90 s for a
	# 133-event catalog while the beachballs themselves took 0.5 s (2026-07-04).
	axyz = Float64[];  asegoff = Cint[]
	txy = Float64[];  txts = String[];  tevid = Cint[]   # tevid[k] = the ei owning txts[k] (drag-follow link)
	infos = String[]        # per-kept-event hover text (date/mag/depth), index == ei (0-based ei+1)
	for i in order
		isnan(mag[i]) && continue
		dim = mag5 / 5 * max(mag[i], 0.0)      # radius in KM for this event's magnitude
		dim <= 0 && continue
		comp, dilat, nodal1, nodal2 = _focal_patch_meca(str1[i], dip1[i], rake1[i], str2[i], dip2[i], rake2[i])
		compcol = depcolors ? _ovl_color(compnames[_seis_bucket(_SEIS_DEP_EDGES, dep[i])], :fill) : compcolor0
		# Fills = the disk's (≤4) sectors cut along the two nodal curves — every one a SIMPLE
		# polygon coloured by the first-motion sign (see _focal_sectors; patch_meca's comp/dilat
		# rings are NOT used for filling, they are ill-posed triangulation input).
		loops = Vector{Matrix{Float64}}();  cols = NTuple{3,Float64}[];  roles = Int[]
		for (poly, iscomp) in _focal_sectors(str1[i], dip1[i], rake1[i], nodal1, nodal2)
			push!(loops, poly)
			push!(cols, iscomp ? compcol : white)
			push!(roles, iscomp ? 1 : 0)
		end
		isempty(loops) && continue
		# Stroke set = outer circle + the two nodal planes, straight from their own well-defined
		# curves (nodal1/nodal2, _focal_patch_meca) — the standard GMT psmeca convention. Sent as
		# OPEN POLYLINES (negative vcount) — the C++ side strokes them as ONE constant-pixel-width
		# line actor per event, always visible at any zoom (world-space ribbons went sub-pixel).
		circle = let th = range(0.0, 2pi, length=2 * _MECA_NP + 1)
			hcat(cos.(th), sin.(th))   # closed: first point == last
		end
		for curve in (circle, nodal1, nodal2)
			push!(loops, curve);  push!(cols, rimcolor0);  push!(roles, 2)
		end
		ei += 1
		# Hover metadata for this ball (tooltip via gmtvtk_set_meca_infos_h below). Same order as
		# ei, so infos[ei+1] is event ei. Date line only for catalogs that carry one (CMT/ISF).
		push!(infos, string(isempty(date[i]) ? "" : "Date: $(date[i])\n",
		                    "Magnitude: ", round(mag[i], digits=1),
		                    "\nDepth: ", round(dep[i], digits=1), " km"))
		# SCREEN-ROUND placement, in degrees. Beachballs are schematic SYMBOLS (Mirone's printed
		# patches, psmeca's paper symbols) — they must render as perfect circles on screen at any
		# latitude, NOT as the geodesically-true ground footprint (tried: GMT.geod destinations —
		# physically correct and visually WRONG, an ellipse stretched 1/cos(lat) on a degree axis;
		# also one geod batch per event dominated plot time on real catalogs). The window scales
		# actor X by xfac (= cos(midlat)); pre-dividing the X offset by xfac makes screen X/Y
		# offsets equal — exactly round — on any window. mag5size (km) maps via 1° lat = 111.32 km.
		rdeg = dim / 111.32
		for (loop, col, role) in zip(loops, cols, roles)
			np = size(loop, 1)
			for r in 1:np
				push!(xy, plon[i] + rdeg * loop[r,1] / xfac, plat[i] + rdeg * loop[r,2])
			end
			push!(vcounts, Cint(role == 2 ? -np : np))   # negative = open polyline to stroke
			push!(rgb, col[1], col[2], col[3])
			push!(evid, Cint(ei * 3 + role))   # event-dominant rank — see the OPAQUE comment above
		end
		if lon[i] != plon[i] || lat[i] != plat[i]           # anchor line, epicenter -> plotted position
			push!(asegoff, Cint(length(axyz) ÷ 3))
			push!(axyz, lon[i], lat[i], 0.0, plon[i], plat[i], 0.0)
		end
		if plotdate && !isempty(date[i])
			push!(txy, plon[i], plat[i] + rdeg * 1.4)      # clear of the rim (centred justification means
			                                                # half the glyph height sits BELOW the anchor)
			push!(txts, date[i])
			push!(tevid, Cint(ei))
		end
	end
	if !isempty(asegoff)     # all anchor lines, ONE overlay call (one rebuild+render)
		push!(asegoff, Cint(length(axyz) ÷ 3))   # terminal offset — addOverlay reads segoff[k+1]
		GC.@preserve axyz asegoff ccall(_fn(:gmtvtk_add_overlay_h), Cint,
			(Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble,
			 Cdouble, Cdouble, Cstring),
			scene, axyz, Cint(length(axyz) ÷ 3), asegoff, Cint(length(asegoff) - 1), Cint(1),
			0.0, 0.0, 0.0, 1.0, -1.0, "Focal mechanisms")
	end
	if !isempty(txts)        # all date labels, ONE batch call (one rebuild+render)
		blob = join(txts, '\x1e')
		# tevid tags each label with its owning event (0-based ei) — gmtvtk_add_meca_h (below) reads
		# TextLabel::mecaEvent to wire MecaBall::dateLabel, so dragging a ball carries its date along.
		GC.@preserve txy blob tevid ccall(_fn(:gmtvtk_add_texts_h), Cint,
			(Ptr{Cvoid}, Ptr{Cdouble}, Cstring, Cint, Cdouble, Cdouble, Cdouble, Cint,
			 Cstring, Cint, Cint, Cstring, Ptr{Cint}),
			scene, txy, blob, Cint(length(txts)), datecolor[1], datecolor[2], datecolor[3], datefontsize,
			datefont, Cint(datebold ? 1 : 0), Cint(dateitalic ? 1 : 0), "Focal mechanisms", tevid)
	end
	isempty(vcounts) && return 0
	# plotdate/datefont/datefontsize/datecolor/datebold/dateitalic (computed above) are threaded
	# through so the C++ side can cache the date-label settings into MecaGroupProps ALONGSIDE the
	# colours/rim-width — the properties dialog (mecaGroupPropsDialog, 50_scene.cpp) must know this
	# batch's CURRENT "Plot event date" state, or touching any other control (e.g. outline colour)
	# silently resets it to OFF and deletes the labels (2026-07-05 bug).
	n = GC.@preserve xy vcounts rgb evid ccall(_fn(:gmtvtk_add_meca_h), Cint,
		(Ptr{Cvoid}, Ptr{Cdouble}, Ptr{Cint}, Cint, Ptr{Cdouble}, Ptr{Cint},
		 Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cstring,
		 Cint, Cstring, Cint, Cdouble, Cdouble, Cdouble, Cint, Cint),
		scene, xy, vcounts, Cint(length(vcounts)), rgb, evid,
		compcolor0[1], compcolor0[2], compcolor0[3], dilatcolor0[1], dilatcolor0[2], dilatcolor0[3],
		rimcolor0[1], rimcolor0[2], rimcolor0[3], rimfrac*100.0, "Focal mechanisms",
		Cint(plotdate ? 1 : 0), datefont, Cint(datefontsize), datecolor[1], datecolor[2], datecolor[3],
		Cint(datebold ? 1 : 0), Cint(dateitalic ? 1 : 0))
	if n > 0 && !isempty(infos)      # attach the hover tooltips to the balls just plotted
		GC.@preserve infos ccall(_fn(:gmtvtk_set_meca_infos_h), Cint,
			(Ptr{Cvoid}, Cstring, Ptr{Cstring}, Cint),
			scene, "Focal mechanisms", infos, Cint(length(infos)))
	end
	return Int(n)
end

# ── C callback ──────────────────────────────────────────────────────────────────────────────

# Stashes the LAST successfully-plotted catalog's parsed params + per-event arrays, keyed by scene
# handle — lets the Scene Objects group's properties dialog (mecaGroupPropsDialog, 50_scene.cpp)
# re-plot with new colours/rim-width WITHOUT re-reading the catalog file (the file may have moved,
# or filters may have been applied interactively) via _on_meca_props below. One entry per WINDOW,
# since a window realistically holds one focal-mechanism batch at a time (matches the group's own
# fixed "Focal mechanisms" name — see _focal_plot's ccall).
const _FOCAL_LAST = Dict{Ptr{Cvoid}, NamedTuple}()

# Focal failures MUST be visible: the Errors-tab log (sceneLogError) is silently dropped on a
# window without the console dock, and even with one the user may never look there — three
# straight "plotted nothing" bug reports were failures whose reason went to an invisible log.
_focal_fail(scene, msg) = (_viewer_log_error(scene, msg);
	ccall(_fn(:gmtvtk_error_box), Cvoid, (Ptr{Cvoid}, Cstring, Cstring), scene, "Focal mechanisms", String(msg)))

# cparams = "key=value\n…" (the same block format NSWING/Seismicity use -> same parser).
#
# BASEMAP ORDER, empty launcher: the basemap is placed IMMEDIATELY after the catalog is READ —
# from the FULL read extent, before the mag/depth filter narrows it to the kept subset. Placing it
# only once "keep"/idx are known (the earlier version of this fix) meant a filter that excluded
# every event returned with NO basemap ever shown — the user staring at a blank/frozen window even
# though the file was read fine and its extent was already known. There is no meaningful "visible
# region" on an empty launcher either way, so cropping the read (or filter) against the C++ menu's
# placeholder viewport silently drops every real event ("no events" LIE, 2026-07-04) — read the
# WHOLE catalog (world crop for the region-cropped ISF reader) regardless. A populated window is
# unchanged: crop/filter against the current on-screen region, exactly as before.
_on_focal(scene::Ptr{Cvoid}, cparams::Cstring)::Cvoid = _on_focal(scene, unsafe_string(cparams))

# The catalog request block (file + format + region + mag/depth filter + colours) reproduces the whole
# beachball layer, so Save Session stores it verbatim as a :focal recipe (no data) and Load re-dispatches
# here. On an empty launcher this also frames a basemap (recorded as its own earlier recipe), so :focal
# is always replayed as an extra on top of that.
function _on_focal(scene::Ptr{Cvoid}, cparams::String)::Cvoid
	try
		d = _nswing_parse(cparams)
		fmt = something(tryparse(Int, _get(d, "format", "1")), 1)
		file = _get(d, "file")
		isempty(file) && error("Focal mechanisms: no catalog file selected")
		empty = ccall(_fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), scene) == 0
		W, E, S, N = empty ? (-180.0, 180.0, -90.0, 90.0) : _seis_region(d)
		lon, lat, dep, mag, str1, dip1, rake1, str2, dip2, rake2, plon, plat, date =
			fmt == 1 ? _focal_isf(file, W, E, S, N) :
			fmt == 2 ? _focal_aki(file) :
			fmt == 3 ? _focal_cmt(file) :
			fmt == 4 ? _focal_ndk(file) : error("unknown catalog format $fmt")
		if isempty(lon)
			_focal_fail(scene, "The catalog returned no events.\n\nfile: $file\nformat: $fmt (1=ISF 2=Aki 3=CMT 4=.ndk)\nregion: $W/$E/$S/$N")
			return
		end
		if empty
			# The file is READ — plot the basemap RIGHT NOW, framed to the WHOLE catalog's own extent
			# (5% pad; a single-point/degenerate catalog gets a +-1 deg fallback pad), before the
			# mag/depth filter runs. A filter that excludes everything below still leaves the user
			# looking at a real map of where the catalog is, not a blank window.
			pad(lo, hi) = (hi - lo) < 1e-6 ? (lo - 1.0, hi + 1.0) : (lo - 0.05 * (hi - lo), hi + 0.05 * (hi - lo))
			Wd, Ed = pad(extrema(lon)...)
			Sd, Nd = pad(extrema(lat)...)
			Wd = max(Wd, -180.0); Ed = min(Ed, 180.0); Sd = max(Sd, -90.0); Nd = min(Nd, 90.0)
			_on_basemap(scene, "$Wd/$Ed/$Sd/$Nd/0/region")
			d["region"] = "$Wd/$Ed/$Sd/$Nd"          # superset of every point read -> filter below is mag/depth-only
			# Force it to actually PAINT now, before the per-event beachball geometry build — otherwise
			# the whole read+frame+plot happens inside one blocking call under an indeterminate busy
			# spinner and the map only appears at the very end, reading as "frozen".
			ccall(_fn(:gmtvtk_process_events), Cint, ())
		end
		keep = _focal_filter(d, lon, lat, dep, mag)
		idx = findall(keep)
		if isempty(idx)
			_focal_fail(scene, "No events match the selected filters.\n\ncatalog events: $(length(lon))\nregion: $W/$E/$S/$N\nmag: $(_get(d,"magmin"))-$(_get(d,"magmax"))  depth: $(_get(d,"depmin"))-$(_get(d,"depmax"))")
			return
		end
		n = _focal_plot(scene, d, lon, lat, dep, mag, str1, dip1, rake1, str2, dip2, rake2, plon, plat, date, idx)
		if n == 0
			_focal_fail(scene, "All $(length(idx)) selected events produced no drawable patches (degenerate magnitudes/sizes?).")
			return
		end
		_FOCAL_LAST[scene] = (d=d, lon=lon, lat=lat, dep=dep, mag=mag, str1=str1, dip1=dip1, rake1=rake1,
		                      str2=str2, dip2=dip2, rake2=rake2, plon=plon, plat=plat, date=date, idx=idx)
		# Save Session: remember the request (newlines escaped to keep it a single manifest value).
		_session_record!(scene, :focal, :menu; params=Dict{String,Any}("cparams" => replace(cparams, '\n' => '\x1e')))
		_viewer_log_error(scene, "Focal mechanisms: plotted $(length(idx)) of $(length(lon)) events ($n patches)")
	catch e
		_focal_fail(scene, "Focal mechanisms FAILED: $(sprint(showerror, e))")
		@warn "focal: failed" exception=(e, catch_backtrace())
	end
	return
end

# Scene Objects group properties dialog (mecaGroupPropsDialog, 50_scene.cpp) Apply -> here:
# cparams = "compcolor=r/g/b\ndilatcolor=r/g/b\nrimcolor=r/g/b\nrimwidth=<0..1 fraction>". Merges
# these overrides into the ORIGINAL catalog params (stashed by _on_focal above), removes the old
# batch, and re-plots — a new rim width needs fresh geodesic geometry, so this can never be a cheap
# in-place recolour.
function _on_meca_props(scene::Ptr{Cvoid}, groupname::Cstring, cparams::Cstring)::Cvoid
	try
		gname = unsafe_string(groupname)
		overrides = _nswing_parse(unsafe_string(cparams))
		orig = get(_FOCAL_LAST, scene, nothing)
		if orig === nothing
			_viewer_log_error(scene, "Focal mechanisms: no stored catalog to re-plot (re-open the dialog after plotting once)")
			return
		end
		d2 = merge(orig.d, overrides)
		ccall(_fn(:gmtvtk_remove_meca_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, gname)
		n = _focal_plot(scene, d2, orig.lon, orig.lat, orig.dep, orig.mag, orig.str1, orig.dip1, orig.rake1,
		                 orig.str2, orig.dip2, orig.rake2, orig.plon, orig.plat, orig.date, orig.idx)
		_FOCAL_LAST[scene] = merge(orig, (d=d2,))
		_viewer_log_error(scene, "Focal mechanisms: re-plotted with new properties ($n patches)")
	catch e
		_viewer_log_error(scene, "Focal mechanisms properties FAILED: $(sprint(showerror, e))")
		@warn "focal props: failed" exception=(e, catch_backtrace())
	end
	return
end

# Build the C-callable pointers + register them. Lazy (first window) via _ensure_callbacks — the
# @cfunction is a thin invokelatest trampoline so it drags no GMT into compile.
function _register_focal()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_focal, s, c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_focal_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

function _register_meca_props()
	fptr = @cfunction((s, g, c) -> Base.invokelatest(_on_meca_props, s, g, c), Cvoid, (Ptr{Cvoid}, Cstring, Cstring))
	ccall(_fn(:gmtvtk_set_meca_props_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
