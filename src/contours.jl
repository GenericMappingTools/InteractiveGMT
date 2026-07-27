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

# Segments shorter than this are never labelled (a 3-vertex crumb has no room for an annotation),
# and no single Apply may create more than _CONTOUR_LABEL_MAX billboards — a global contour set can
# hold 100k+ segments and one text actor each would freeze the window.
const _CONTOUR_LABEL_MINPTS = 20
const _CONTOUR_LABEL_MAX    = 4000

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
_contour_clear(scene::Ptr{Cvoid}) =
	ccall(_fn(:gmtvtk_remove_overlay_group_h), Cint, (Ptr{Cvoid}, Cstring), scene, _CONTOUR_GROUP)

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

	r, g, b = _ovl_color(nothing, :lines)          # black, the plain-line default
	txy = Float64[];  txts = String[]
	for (lev, xyz, segoff, nseg, npts) in sets
		name = "Contour " * _contour_fmt(lev)
		# _ex2_h: zIsPlaceholder=0 (z IS real data — the level), noConvertToPoints=1 (scattering a
		# contour to points is meaningless), same batched call plateboundaries.jl uses.
		GC.@preserve xyz segoff ccall(_fn(:gmtvtk_add_overlay_ex2_h), Cint,
			(Ptr{Cvoid}, Ptr{Cdouble}, Cint, Ptr{Cint}, Cint, Cint, Cdouble, Cdouble, Cdouble,
			 Cdouble, Cdouble, Cstring, Cstring, Cstring, Cint, Cint),
			scene, xyz, Cint(npts), segoff, Cint(nseg), Cint(1), r, g, b,
			1.0, 0.0, name, _CONTOUR_GROUP, "", Cint(1), Cint(0))
		labels || continue
		txt = _contour_fmt(lev)
		for k in 1:nseg                             # one label at each long-enough segment's middle
			length(txts) >= _CONTOUR_LABEL_MAX && break
			i0 = Int(segoff[k]);  i1 = Int(segoff[k + 1])
			(i1 - i0) < _CONTOUR_LABEL_MINPTS && continue
			m = i0 + (i1 - i0) ÷ 2                  # 0-based vertex index of the middle point
			push!(txy, xyz[3m + 1], xyz[3m + 2])
			push!(txts, txt)
		end
	end
	if !isempty(txts)
		blob = join(txts, '\x1e')
		GC.@preserve txy blob ccall(_fn(:gmtvtk_add_texts_h), Cint,
			(Ptr{Cvoid}, Ptr{Cdouble}, Cstring, Cint, Cdouble, Cdouble, Cdouble, Cint,
			 Cstring, Cint, Cint, Cstring, Ptr{Cint}),
			scene, txy, blob, Cint(length(txts)), r, g, b, Cint(9),
			"", Cint(0), Cint(0), _CONTOUR_GROUP, C_NULL)
	end
	return nothing
end
