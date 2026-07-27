# contours.jl — Grid Tools > "Contours" (port of Mirone's src_figs/contouring.m). Trace the window's
# grid at a user-built list of elevations and draw each level as ONE batched line overlay, all tagged
# with the same Scene Objects group so they fold under a single collapsible "Contours" row.
#
# The tracer is GDAL's, NOT grdcontour: `GDALContourGenerateEx` (marching squares, C, one pass over
# the raster for ALL levels at once) against the in-memory MEM raster `GMT.gmt2gd` makes from the
# grid — no file round-trip, no new dependency (GDAL already ships with GMT.jl, `GMT.libgdal`, the
# same library shapenc.jl ccalls). Measured on earth_relief_01m over (-60,20,10,60) = 3001x4801 with
# 91 levels: 4.2 s end-to-end here vs 35.6 s for `grdcontour(..., dump=true)`, ~8x.
#
# The generated OGR features are read back through the raw OGR C API straight into the flat
# xyz/segoff layout `gmtvtk_add_overlay_ex2_h` wants (see `_pack_dataset`, grid.jl) — going through
# `gd2gmt` -> Vector{GMTdataset} first cost an extra 2 s and 1.1 GiB on that same run for data we
# would immediately flatten again.
#
# The C++ dialog (ContourDialog, 70_window.cpp) loads deps/ui/contouring.ui at runtime and drives
# everything through `_contour_zrange` (Min/Max prefill) and `_on_contours` (Apply) via g_juliaEval.

const _CONTOUR_GROUP = "Contours"

# Levels currently drawn in each window, in the order their overlays were added. The group's
# "Color by grid colormap" needs to know which level each line stands for, and a level is a NUMBER —
# recovering it by parsing the row's name back out of a string would be a second, fragile source.
const _CONTOUR_DRAWN = Dict{Ptr{Cvoid}, Vector{Float64}}()

# A contour is annotated only if it is at least this many times longer than the label it would
# carry — an annotation as long as (or longer than) its own contour is noise, not information.
const _CONTOUR_LABEL_RATIO = 4.0
# The gap cut in the line under a label, as a multiple of that label's own width — a little wider
# than the glyphs so the line does not touch them. The label sits IN this gap: no background box,
# nothing painted over the line, the line simply is not there.
const _CONTOUR_LABEL_PAD = 1.35
# How far apart repeated labels sit on one contour, in label widths. A long contour carries several
# annotations, like any printed chart; one label on a contour crossing the whole map is useless.
const _CONTOUR_LABEL_SPACING = 22.0
# Clearance factor on a label's own box when testing it against labels already placed. 1.0 would let
# two annotations touch; a little over keeps white space between them.
const _CONTOUR_LABEL_CLEAR = 1.25
# Fallback gate for when the label's on-screen width cannot be measured (no live camera): a plain
# vertex count, the crude proxy the ratio rule replaces.
const _CONTOUR_LABEL_MINPTS = 20
# No single Apply may create more than this many billboards — a global contour set can hold 100k+
# segments and one text actor each would freeze the window.
const _CONTOUR_LABEL_MAX = 4000

# Font of the contour annotations (the numbers passed to gmtvtk_add_texts_h). Kept in ONE place so
# the width measured for the "is this contour long enough?" test is measured for the font actually
# drawn.
const _CONTOUR_LABEL_SIZE = 9
const _CONTOUR_LABEL_FONT = "Arial"

# On-screen length of one packed segment, in the SAME world units gmtvtk_label_width_world_h
# returns: x is multiplied by the scene's own x scale factor (gmtvtk_get_xfac = cos(midlat) for a
# geographic window, 1 otherwise), exactly as textApplyProps anchors a label at pos[0]*xfac.
#
# This is deliberately NOT measure.jl's `_line_length`: that answers "how long is this line on the
# Earth" (geodesic km, CRS- and Preferences-aware, one GMT call per segment). What is needed here is
# a different quantity — how much ROOM the line takes on the screen, to compare against a label's
# screen width — and it must be cheap enough to run on every one of 100k+ segments.
function _contour_screen_len(xyz::Vector{Float64}, i0::Int, i1::Int, xfac::Float64)
	len = 0.0
	@inbounds for i in (i0 + 1):(i1 - 1)
		dx = (xyz[3i + 1] - xyz[3i - 2]) * xfac
		dy = xyz[3i + 2] - xyz[3i - 1]
		len += hypot(dx, dy)
	end
	return len
end

# First vertex before the anchor `m`, and first vertex after it, that lie at least `half` (world
# units, same metric as _contour_screen_len) away ALONG the line. The vertices strictly between them
# are the ones the label covers — dropping them is what breaks the contour open under the
# annotation. Clamped to the segment's own ends, so a short side simply runs out.
function _contour_gap_ends(xyz::Vector{Float64}, i0::Int, i1::Int, m::Int, half::Float64, xfac::Float64)
	a = m;  d = 0.0
	@inbounds while a > i0 && d < half
		d += hypot((xyz[3a + 1] - xyz[3a - 2]) * xfac, xyz[3a + 2] - xyz[3a - 1])
		a -= 1
	end
	b = m;  d = 0.0
	@inbounds while b < i1 - 1 && d < half
		d += hypot((xyz[3b + 4] - xyz[3b + 1]) * xfac, xyz[3b + 5] - xyz[3b + 2])
		b += 1
	end
	return a, b
end

# Direction the label must read in, degrees CCW from +x, taken across the WHOLE gap (from the vertex
# where the line stops to the one where it starts again) rather than from one tiny inter-vertex step,
# so a wiggly contour does not throw the text off. Measured in the same x-scaled space the line is
# drawn in. Flipped by 180° when it would otherwise come out upside-down: a label reads along its
# contour, never backwards.
function _contour_label_angle(xyz::Vector{Float64}, ga::Int, gb::Int, xfac::Float64)
	dx = (xyz[3gb + 1] - xyz[3ga + 1]) * xfac
	dy = xyz[3gb + 2] - xyz[3ga + 2]
	(dx == 0.0 && dy == 0.0) && return 0.0
	a = atand(dy, dx)
	a >  90.0 && (a -= 180.0)
	a < -90.0 && (a += 180.0)
	return a
end

# Where to put the labels along ONE contour segment (vertices i0 … i1-1, 0-based), professional
# rules rather than "one in the middle":
#   * a contour shorter than _CONTOUR_LABEL_RATIO label widths gets none at all;
#   * otherwise labels repeat every _CONTOUR_LABEL_SPACING label widths, evenly spread so the first
#     and last sit half a spacing in from the ends — a long contour gets several, not one lonely one;
#   * each is then nudged to the STRAIGHTEST spot within half a spacing of its target: text reading
#     across a hairpin is what makes a contour map look amateur. Straightness = how close the arc
#     length over the label's own footprint is to the straight-line distance across it.
#   * a candidate is dropped if it cannot fit its own footprint clear of the segment's ends.
# Returns the anchor vertex indices, ascending.
function _contour_label_anchors(xyz::Vector{Float64}, i0::Int, i1::Int, xfac::Float64,
                                cum::Vector{Float64}, lw::Float64, half::Float64)
	out = Int[]
	n = i1 - i0
	(n < 3 || lw <= 0) && return out
	total = cum[n] - cum[1]
	total < _CONTOUR_LABEL_RATIO * lw && return out
	spacing = _CONTOUR_LABEL_SPACING * lw
	nlab = max(1, floor(Int, total / spacing))
	# Arc position of a vertex, measured from the segment's start.
	arcof(i) = cum[i - i0 + 1] - cum[1]
	# First vertex at or past arc position `t` (linear scan: targets are visited in order).
	j = i0
	for k in 1:nlab
		t = total * (k - 0.5) / nlab
		while j < i1 - 1 && arcof(j) < t
			j += 1
		end
		best = -1;  bestScore = Inf
		lo = t - 0.5 * spacing;  hi = t + 0.5 * spacing
		c = j
		while c > i0 && arcof(c) > lo
			c -= 1
		end
		while c < i1 - 1 && arcof(c) <= hi
			# must have its whole footprint inside this segment, or the label would hang off the end
			if arcof(c) >= half && arcof(c) <= total - half
				ga, gb = _contour_gap_ends(xyz, i0, i1, c, half, xfac)
				arc = arcof(gb) - arcof(ga)
				chord = hypot((xyz[3gb + 1] - xyz[3ga + 1]) * xfac, xyz[3gb + 2] - xyz[3ga + 2])
				score = chord > 0 ? arc / chord : Inf     # 1.0 = dead straight, grows with curvature
				if score < bestScore
					bestScore = score;  best = c
				end
			end
			c += 1
		end
		# Never let two labels' footprints touch (they would eat each other's line).
		if best >= 0 && (isempty(out) || arcof(best) - arcof(out[end]) > 2.2 * half)
			push!(out, best)
		end
	end
	return out
end

# Do two label boxes overlap? Separating-axis test on two RECTANGLES, each given by its centre (in
# the drawn, x-scaled space), its rotation, and its half-width/half-height. Rectangles, not circles:
# a contour annotation is a long thin box, and two of them crossing at right angles in a steep area
# clear each other easily even though their centres are close.
function _contour_boxes_overlap(c1::NTuple{2,Float64}, a1::Float64, hw1::Float64, hh1::Float64,
                                c2::NTuple{2,Float64}, a2::Float64, hw2::Float64, hh2::Float64)
	dx = c2[1] - c1[1];  dy = c2[2] - c1[2]
	for (a, hw, hh, oa, ohw, ohh, sgn) in ((a1, hw1, hh1, a2, hw2, hh2, 1.0),
	                                       (a2, hw2, hh2, a1, hw1, hh1, -1.0))
		ca, sa = cosd(a), sind(a)
		for (ax, ay, halfSelf) in ((ca, sa, hw), (-sa, ca, hh))
			d = abs(sgn * (dx * ax + dy * ay))
			# how far the OTHER rectangle reaches along this axis
			oca, osa = cosd(oa), sind(oa)
			r = ohw * abs(ax * oca + ay * osa) + ohh * abs(-ax * osa + ay * oca)
			d > halfSelf + r && return false          # a gap on this axis: they cannot overlap
		end
	end
	return true
end

# Cumulative arc length along vertices i0 … i1-1, in the drawn (x-scaled) space. cum[1] = 0.
function _contour_cumlen(xyz::Vector{Float64}, i0::Int, i1::Int, xfac::Float64)
	n = i1 - i0
	cum = Vector{Float64}(undef, max(n, 1))
	cum[1] = 0.0
	@inbounds for i in (i0 + 1):(i1 - 1)
		cum[i - i0 + 1] = cum[i - i0] + hypot((xyz[3i + 1] - xyz[3i - 2]) * xfac,
		                                      xyz[3i + 2] - xyz[3i - 1])
	end
	return cum
end

# The grid the tool contours: the window's primary/base grid (same `_find_object` resolution every
# other Grid Tools entry uses). ONE resolver for both entry points below, so the Min/Max the dialog
# shows can never describe a different grid than the one Apply traces.
function _contour_grid(scene::Ptr{Cvoid})
	G = _find_object(scene, :grid, "")
	(G isa GMTgrid) || error("No grid loaded in this window")
	return G
end

# NaN-aware z range of the grid to be contoured. g_juliaEval round-trip for the dialog's read-only
# Min/Max boxes: prints "min/max".
function _contour_zrange(scene::Ptr{Cvoid})
	G = _contour_grid(scene)
	mn = Inf;  mx = -Inf
	@inbounds for v in G.z
		isnan(v) && continue
		v < mn && (mn = v)
		v > mx && (mx = v)
	end
	isfinite(mn) || error("Grid has no finite values to contour")
	print(mn, "/", mx)
	return nothing
end

# Trace `G` at `levels` with GDAL and return one entry per level that produced anything:
# (level, xyz, segoff, nseg, npts) — exactly the argument set gmtvtk_add_overlay_ex2_h takes. z is
# the LEVEL itself for every vertex (a contour of value L sits at z = L on the 3-D surface, so it
# drapes exactly, with no grid resampling). Segments with fewer than `minpts` vertices are dropped
# (contouring.m's "Min pts").
function _contour_gdal(G::GMTgrid, levels::Vector{Float64}, minpts::Int)
	out = Tuple{Float64,Vector{Float64},Vector{Cint},Int,Int}[]
	isempty(levels) && return out
	gdl  = GMT.Gdal
	dsr  = GMT.gmt2gd(G)                    # MEM raster over the grid — no copy to disk
	band = ccall((:GDALGetRasterBand, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid}, Cint), dsr.ptr, Cint(1))
	(band == C_NULL) && error("Contours: could not wrap the grid as a GDAL raster")

	ds  = gdl.create(gdl.getdriver("MEM"))  # in-memory OGR sink for the traced lines
	lyr = gdl.createlayer(name = "contour", dataset = ds, geom = gdl.wkbLineString)
	gdl.addfielddefn!(lyr, "ID",   gdl.OFTInteger)
	gdl.addfielddefn!(lyr, "ELEV", gdl.OFTReal)

	# FIXED_LEVELS traces the exact list in ONE raster pass. NaN nodes are skipped by GDAL's own
	# marching squares (verified: a contour crossing a NaN block comes back split, no NODATA option
	# needed), so NaN-holed grids need no extra care here.
	opts = ["ID_FIELD=0", "ELEV_FIELD=1", "FIXED_LEVELS=" * join(levels, ',')]
	copts = C_NULL
	for o in opts
		copts = ccall((:CSLAddString, GMT.libgdal), Ptr{Ptr{UInt8}}, (Ptr{Ptr{UInt8}}, Cstring), copts, o)
	end
	err = GC.@preserve dsr ds ccall((:GDALContourGenerateEx, GMT.libgdal), Cint,
	          (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Cvoid}, Ptr{Cvoid}),
	          band, lyr.ptr, copts, C_NULL, C_NULL)
	ccall((:CSLDestroy, GMT.libgdal), Cvoid, (Ptr{Ptr{UInt8}},), copts)
	if err != 0
		msg = unsafe_string(ccall((:CPLGetLastErrorMsg, GMT.libgdal), Cstring, ()))
		error("Contours: GDALContourGenerateEx failed" * (isempty(msg) ? "" : " ($msg)"))
	end

	# One accumulator per requested level, in the order the user listed them.
	idx  = Dict{Float64,Int}(l => k for (k, l) in enumerate(levels))
	xyzs = [Float64[]  for _ in levels]
	offs = [Cint[0]    for _ in levels]
	tot  = zeros(Int, length(levels))

	ccall((:OGR_L_ResetReading, GMT.libgdal), Cvoid, (Ptr{Cvoid},), lyr.ptr)
	while true
		f = ccall((:OGR_L_GetNextFeature, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid},), lyr.ptr)
		f == C_NULL && break
		try
			elev = ccall((:OGR_F_GetFieldAsDouble, GMT.libgdal), Cdouble, (Ptr{Cvoid}, Cint), f, Cint(1))
			k = get(idx, elev, 0)
			k == 0 && continue                       # a level GDAL rounded differently — never happens with FIXED_LEVELS
			g = ccall((:OGR_F_GetGeometryRef, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid},), f)
			g == C_NULL && continue
			n = Int(ccall((:OGR_G_GetPointCount, GMT.libgdal), Cint, (Ptr{Cvoid},), g))
			(n < 2 || n < minpts) && continue
			xs = Vector{Float64}(undef, n);  ys = Vector{Float64}(undef, n)
			GC.@preserve xs ys ccall((:OGR_G_GetPoints, GMT.libgdal), Cint,
				(Ptr{Cvoid}, Ptr{Cvoid}, Cint, Ptr{Cvoid}, Cint, Ptr{Cvoid}, Cint),
				g, xs, Cint(8), ys, Cint(8), C_NULL, Cint(0))
			xyz = xyzs[k]
			@inbounds for i in 1:n
				push!(xyz, xs[i], ys[i], elev)       # z = the contour's own level: drapes on the surface
			end
			tot[k] += n
			push!(offs[k], Cint(tot[k]))
		finally
			ccall((:OGR_F_Destroy, GMT.libgdal), Cvoid, (Ptr{Cvoid},), f)
		end
	end
	GC.@preserve dsr ds nothing

	for (k, l) in enumerate(levels)
		tot[k] == 0 && continue
		push!(out, (l, xyzs[k], offs[k], length(offs[k]) - 1, tot[k]))
	end
	return out
end

# Compact level text for the Scene Objects row / the label billboards: integral levels print without
# a decimal point ("-2000", not "-2000.0").
_contour_fmt(l::Float64) = (isfinite(l) && l == round(l) && abs(l) < 1e15) ? string(Int(round(l))) : string(l)

# Drop everything a previous Apply put in the window (overlays AND their labels, both tagged with
# _CONTOUR_GROUP). Apply always redraws the WHOLE list, so a re-Apply replaces rather than piles up.
function _contour_clear(scene::Ptr{Cvoid})
	delete!(_CONTOUR_DRAWN, scene)
	return ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, _CONTOUR_GROUP)
end

# Group properties (Scene Objects, the "Contours" handle): paint every contour with the colour its
# OWN level has in the grid's colormap, or put them all back to black. The colours come from the very
# same `_cpt_nodes(G, cmap)` the surface and the colour bar are built from — never a second palette —
# and are applied through gmtvtk_set_overlay_style_h, the same setter Load Session uses to restore an
# edited pen. Called from the group's menu via g_juliaEval.
function _contour_color_by_cpt(scene::Ptr{Cvoid}, on::Integer)
	levels = get(_CONTOUR_DRAWN, scene, Float64[])
	isempty(levels) && return nothing
	rgb = nothing
	if on != 0
		G = _contour_grid(scene)
		cz, crgb, n = _cpt_nodes(G, _default_cmap(G))
		n < 2 && error("Contours: no colormap to colour by")
		rgb = (cz, crgb, n)
	end
	cur = zeros(Float64, 6)                          # {r,g,b,width_px,style,opacity} of the live overlay
	for lev in levels
		nm = "Contour " * _contour_fmt(lev)
		# Read the pen the line HAS and change only its colour: width, dash style and opacity may have
		# been edited by hand through Line Properties, and a recolour must not quietly undo that.
		found = ccall(_fn(:gmtvtk_overlay_style_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{Cdouble}), scene, nm, cur)
		found == 0 && continue
		r, g, b = rgb === nothing ? (0.0, 0.0, 0.0) : _cpt_lookup(rgb[1], rgb[2], rgb[3], lev)
		ccall(_fn(:gmtvtk_set_overlay_style_h), Cint,
		      (Ptr{Cvoid}, Cstring, Cdouble, Cdouble, Cdouble, Cdouble, Cint, Cdouble),
		      scene, nm, r, g, b, cur[4], Cint(round(Int, cur[5])), cur[6])
	end
	return nothing
end

# Colour at `z` from the CPT control nodes (`cz` z values, `crgb` r,g,b triples, `n` nodes) —
# linearly interpolated between the two bracketing nodes, clamped at the ends. The nodes are exactly
# those handed to the viewer for the surface, so a contour and the terrain it sits on always agree.
function _cpt_lookup(cz::Vector{Float64}, crgb::Vector{Float64}, n::Int, z::Float64)
	z <= cz[1]  && return (crgb[1], crgb[2], crgb[3])
	z >= cz[n]  && return (crgb[3n - 2], crgb[3n - 1], crgb[3n])
	k = searchsortedlast(cz, z)
	k = clamp(k, 1, n - 1)
	d = cz[k + 1] - cz[k]
	t = d > 0 ? (z - cz[k]) / d : 0.0
	return (crgb[3k - 2] + t * (crgb[3k + 1] - crgb[3k - 2]),
	        crgb[3k - 1] + t * (crgb[3k + 2] - crgb[3k - 1]),
	        crgb[3k]     + t * (crgb[3k + 3] - crgb[3k]))
end

# C++ dialog entry point (Apply, and the two Delete buttons, which re-draw the pruned list — ONE
# function draws contours, never a second path). `params` = "minpts;labels;lev1,lev2,…"; an EMPTY
# level list just clears the window's contours. Prints nothing; errors go to the Errors tab.
function _on_contours(scene::Ptr{Cvoid}, params::AbstractString)
	p = split(params, ';')
	minpts = length(p) >= 1 ? something(tryparse(Int, strip(p[1])), 0) : 0
	labels = length(p) >= 2 && strip(p[2]) == "1"
	levels = Float64[]
	if length(p) >= 3
		for t in split(p[3], ',')
			v = tryparse(Float64, strip(t))
			v === nothing || push!(levels, v)
		end
	end
	unique!(levels)

	_contour_clear(scene)
	isempty(levels) && return nothing
	G = _contour_grid(scene)
	sets = _contour_gdal(G, levels, max(minpts, 0))
	isempty(sets) && error("No contour crossed the grid at any of those elevations")

	cr, cg, cb = _ovl_color(nothing, :lines)       # black — contour lines AND their annotations
	xfac = ccall(_fn(:gmtvtk_get_xfac), Cdouble, (Ptr{Cvoid},), scene)
	(isfinite(xfac) && xfac > 0) || (xfac = 1.0)
	# World units per screen pixel RIGHT NOW: the label widths below come back in world units, and the
	# hole half-width has to travel to the viewer in pixels (it re-cuts the holes on every zoom).
	wpp = ccall(_fn(:gmtvtk_world_per_pixel_h), Cdouble, (Ptr{Cvoid},), scene)
	(isfinite(wpp) && wpp > 0) || (wpp = 0.0)
	txy = Float64[];  tz = Float64[];  tang = Float64[];  txts = String[]
	# Boxes of the labels already placed — centre (x-scaled space), rotation, half-width, half-height.
	# Shared across ALL levels of this run: that is what stops two different contours from writing
	# over each other where the surface is steep and the levels crowd together.
	lboxc = NTuple{2,Float64}[];  lboxa = Float64[];  lboxw = Float64[];  lboxh = Float64[]
	drawn = Float64[]                               # levels that actually produced a line, for the recolour
	for (lev, xyz, segoff, nseg, npts) in sets
		name = "Contour " * _contour_fmt(lev)
		txt  = _contour_fmt(lev)
		# How wide THIS level's annotation is on screen, in world units, measured in the very text
		# engine that will draw it. A contour must be _CONTOUR_LABEL_RATIO times longer than that to
		# earn one; 0 back means the width could not be measured, so fall back to the vertex count.
		lhref = Ref{Cdouble}(0.0)
		lw = labels ? ccall(_fn(:gmtvtk_label_width_world_h), Cdouble,
		                    (Ptr{Cvoid}, Cstring, Cint, Cstring, Cint, Cint, Ptr{Cdouble}),
		                    scene, txt, Cint(_CONTOUR_LABEL_SIZE), _CONTOUR_LABEL_FONT, Cint(0), Cint(0), lhref) : 0.0
		lh = lhref[]
		half = 0.5 * _CONTOUR_LABEL_PAD * lw         # how far the hole reaches each side of the anchor
		# The box this level's annotations occupy for the overlap test, with a little clearance so two
		# labels never end up merely touching.
		hwPad = 0.5 * _CONTOUR_LABEL_CLEAR * lw
		hhPad = 0.5 * _CONTOUR_LABEL_CLEAR * (lh > 0 ? lh : 0.6 * lw)

		# The contour geometry is NEVER cut: the whole polyline goes to the viewer exactly as GDAL
		# traced it, and the label holes are handed over separately as vertex-index pairs that the
		# renderer skips when it builds the line cells. So a contour stays ONE object — double-click
		# to edit gets the whole contour, the data table and Line length see all of it — while the
		# drawn line still opens up under each annotation, with no box and nothing painted over it.
		anchors = Cint[]
		if labels && lw > 0
			for k in 1:nseg
				length(txts) >= _CONTOUR_LABEL_MAX && break
				i0 = Int(segoff[k]);  i1 = Int(segoff[k + 1])
				i1 - i0 < 3 && continue
				cum = _contour_cumlen(xyz, i0, i1, xfac)
				for m in _contour_label_anchors(xyz, i0, i1, xfac, cum, lw, half)
					length(txts) >= _CONTOUR_LABEL_MAX && break
					ga, gb = _contour_gap_ends(xyz, i0, i1, m, half, xfac)
					gb - ga < 2 && continue               # nothing to open up: skip rather than draw over
					ang = _contour_label_angle(xyz, ga, gb, xfac)
					# Would this annotation land on one already placed? Where the surface is steep the
					# levels crowd together, and each contour spacing its OWN labels says nothing about
					# its neighbours' — this is the check that keeps two different levels from writing
					# over each other. Tested against every label of this whole run, not just this level.
					ctr = (xyz[3m + 1] * xfac, xyz[3m + 2])
					clash = false
					@inbounds for q in eachindex(lboxc)
						if _contour_boxes_overlap(ctr, ang, hwPad, hhPad, lboxc[q], lboxa[q], lboxw[q], lboxh[q])
							clash = true;  break
						end
					end
					clash && continue
					push!(lboxc, ctr);  push!(lboxa, ang);  push!(lboxw, hwPad);  push!(lboxh, hhPad)
					push!(anchors, Cint(m))
					push!(txy, xyz[3m + 1], xyz[3m + 2])
					push!(tz,  lev)                       # the label rides at its contour's own height
					push!(tang, ang)
					push!(txts, txt)
				end
			end
		end
		# The holes are handed over as ANCHORS plus a half-width in SCREEN PIXELS, not as fixed vertex
		# ranges: the labels are screen-constant, so the viewer re-cuts the holes from these anchors
		# whenever the zoom changes and they stay exactly as wide as the text in them.
		# noConvertToPoints=1 (scattering a contour to points is meaningless), zIsPlaceholder=0 (z IS
		# real data — the level). The points and segment offsets go over whole.
		halfpx = (wpp > 0) ? half / wpp : 0.0
		GC.@preserve xyz segoff anchors ccall(_fn(:gmtvtk_add_overlay_gapped_h), Cint,
			(Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble,
			 Cdouble, Cdouble, Cstring, Cstring, Cstring, Cint, Cint, Ptr{Cint}, Cint, Cdouble, Cint),
			scene, xyz, Cint(npts), segoff, Cint(nseg), Cint(1), cr, cg, cb,
			1.0, 0.0, name, _CONTOUR_GROUP, "", Cint(1), Cint(0),
			isempty(anchors) ? C_NULL : anchors, Cint(length(anchors)), halfpx, Cint(1))
		push!(drawn, lev)
	end
	_CONTOUR_DRAWN[scene] = drawn
	if !isempty(txts)
		blob = join(txts, '\x1e')
		# vcenter=1: batch-owned labels are bottom-justified by default (a focal-mechanism date must
		# grow upward off its ball), but a contour annotation has to STRADDLE its line — that is what
		# puts it inside the gap cut for it instead of above the contour.
		# flat=1 + tz + tang: the label lies in the XY plane, turned to the line's own direction and
		# anchored at the contour's height, so it READS ALONG the contour and stays in its gap from any
		# view angle. A billboard cannot do either (always upright, always at z=0 -> the gap and the
		# text pulled apart as soon as the view was not straight down).
		GC.@preserve txy tz tang blob ccall(_fn(:gmtvtk_add_texts_ex_h), Cint,
			(Ptr{Cvoid}, Ptr{Cdouble}, Cstring, Cint, Cdouble, Cdouble, Cdouble, Cint,
			 Cstring, Cint, Cint, Cstring, Ptr{Cint}, Cint, Ptr{Cdouble}, Ptr{Cdouble}, Cint),
			scene, txy, blob, Cint(length(txts)), cr, cg, cb, Cint(_CONTOUR_LABEL_SIZE),
			_CONTOUR_LABEL_FONT, Cint(0), Cint(0), _CONTOUR_GROUP, C_NULL, Cint(1),
			tz, tang, Cint(1))
	end
	return nothing
end
