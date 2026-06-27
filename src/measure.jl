# measure.jl — line length / azimuth and polygon area for the vector context menu.
#
# The C++ line menu (55_lineprops.cpp) gathers a line/polygon's vertices, writes them as a 2-D
# table, and calls back here through the EXISTING console-eval bridge (g_juliaEval) — so NO new
# @cfunction is registered: these ride the one console callback already installed at first window.
#
# CRS-aware. A geographic object uses GMT.mapproject (geodesic incremental distance in km + forward
# azimuth) and GMT.geodesicarea (m² on the ellipsoid); a cartesian one uses planar trig and
# GMT.geomarea (data-unit²). `scene` is the window handle: results for a polyline land in that
# window's Data Viewer (show_table); an area is printed and the C++ side pops it in a message box.

# Is this object geographic? Trust an explicit longlat proj4; otherwise fall back to GMT.guessgeog
# on the coordinates (the same crude range test the grids now use).
function _measure_isgeog(D, proj4::String)::Bool
	isempty(proj4) || return occursin("longlat", proj4) || occursin("latlong", proj4)
	try GMT.guessgeog(D) catch; false end
end

# Per-vertex incremental distance + azimuth for one segment's x,y vectors. BOTH are aligned to the
# ARRIVING vertex (GMT's mapproject convention): row i holds the distance/bearing of the segment
# (i-1)→i, so the FIRST vertex has inc = 0 and azimuth = NaN (no segment arrives at it).
#   geographic -> GMT.mapproject: -G+i+uk (incremental km), -Af (azimuth). One option per call so the
#                 appended column is always the LAST one (deterministic to read back).
#   cartesian  -> planar: hypot (data units) + atand(dx,dy) (bearing N(+Y)→CW), shifted to match.
function _seg_dist_azim(x::Vector{Float64}, y::Vector{Float64}, geog::Bool)
	n = length(x)
	n < 2 && return (zeros(n), fill(NaN, n))
	if geog
		seg = GMT.mat2ds(hcat(x, y); proj4 = GMT.prj4WGS84)          # tag geographic so -G is geodesic
		inc = Float64.(GMT.mapproject(seg, track_distances = "+i+uk").data[:, end])
		az  = Float64.(GMT.mapproject(seg, azim = "f").data[:, end])   # az[1] = NaN (no arriving seg)
		return (inc, az)
	end
	dx = diff(x);  dy = diff(y)
	inc    = [0.0; hypot.(dx, dy)]                              # planar distance, data units
	seg_az = mod.(atand.(dx, dy), 360.0)                        # bearing of each segment, length n-1
	return (inc, [NaN; seg_az])                                 # align to the arriving vertex (GMT-style)
end

# Read the temp table once: the per-segment GMTdataset list, the geographic flag, and the unit label.
function _measure_load(path::String, proj4::String)
	D    = GMT.gmtread(path)
	segs = isa(D, Vector) ? D : [D]
	geog = _measure_isgeog(segs[1], proj4)
	return (segs, geog, geog ? "km" : "units")
end

# Push a per-vertex table (Seg/#/X/Y + one value column) to the window's Data Viewer.
function _measure_table(scene::Ptr{Cvoid}, M::Matrix{Float64}, cols::Vector{String}, name::AbstractString)
	fig = get(_FIGREG, scene, nothing)
	(fig === nothing) && return
	show_table(fig, GMT.mat2ds(M; colnames = cols); name = name)
	return
end

# "Line length(s)…": total length always; for a polyline (>1 segment) also a per-vertex
# #/X/Y/Δlen/cumlen table in the Data Viewer. CRS-aware via _seg_dist_azim. `ispoly` unused
# (kept so all three measure entry points share one C-side signature).
function _line_length(scene::Ptr{Cvoid}, path::String, proj4::String, ispoly::Bool)
	segs, geog, ulab = _measure_load(path, proj4)
	rows = Vector{NTuple{6,Float64}}()                          # Seg, #, X, Y, Δlen, cumlen
	total = 0.0
	for (si, seg) in enumerate(segs)
		x = Float64.(seg.data[:, 1]);  y = Float64.(seg.data[:, 2])
		isempty(x) && continue
		inc, _ = _seg_dist_azim(x, y, geog)
		cum = cumsum(inc)
		total += isempty(cum) ? 0.0 : cum[end]
		for k in eachindex(x)
			push!(rows, (Float64(si), Float64(k), x[k], y[k], inc[k], cum[k]))
		end
	end
	isempty(rows) && (print("Nothing to measure."); return nothing)
	if length(rows) > 2                                         # polyline -> table
		M = reduce(vcat, (collect(r)' for r in rows))
		_measure_table(scene, M, ["Seg", "#", "X", "Y", "Δlen ($ulab)", "Cum ($ulab)"], "Line lengths")
	end
	print("Total length: ", round(total; digits = geog ? 4 : 6), " ", ulab)
	return nothing
end

# "Azimuth(s)…": for a single straight line the one forward azimuth; for a polyline a per-vertex
# #/X/Y/azimuth table (forward bearing to the next vertex; last vertex repeats the prior).
function _line_azimuth(scene::Ptr{Cvoid}, path::String, proj4::String, ispoly::Bool)
	segs, geog, _ = _measure_load(path, proj4)
	rows = Vector{NTuple{5,Float64}}()                          # Seg, #, X, Y, azimuth
	for (si, seg) in enumerate(segs)
		x = Float64.(seg.data[:, 1]);  y = Float64.(seg.data[:, 2])
		isempty(x) && continue
		_, az = _seg_dist_azim(x, y, geog)
		for k in eachindex(x)
			push!(rows, (Float64(si), Float64(k), x[k], y[k], az[k]))
		end
	end
	isempty(rows) && (print("Nothing to measure."); return nothing)
	if length(rows) > 2                                         # polyline -> table
		M = reduce(vcat, (collect(r)' for r in rows))
		_measure_table(scene, M, ["Seg", "#", "X", "Y", "Azimuth (°)"], "Azimuths")
	else                                                        # single straight line: the one bearing
		finite = filter(r -> !isnan(r[5]), rows)
		az = isempty(finite) ? NaN : finite[1][5]
		print("Azimuth = ", round(az; digits = 2), " °")
	end
	return nothing
end

# Total length + first→last forward azimuth of a fault trace, computed with the SAME geodesic path
# (_seg_dist_azim → GMT.mapproject) as the "Line length…"/"Azimuth…" measure menu — so the elastic-
# deformation dialog's seeded Length/Strike match the MEASURED numbers exactly (no haversine-vs-GMT
# divergence). Called synchronously from faultLineGeom (70_window.cpp) over the console-eval bridge.
# Prints "len/az/geog": len km (geographic) or data units (cartesian); az degrees from north CW; geog
# 1 geographic / 0 cartesian. Empty print on failure (C++ falls back to its local spherical formula).
function _fault_lenaz(path::String, proj4::String)
	try
		segs, geog, _ = _measure_load(path, proj4)
		seg = segs[1]
		x = Float64.(seg.data[:, 1]);  y = Float64.(seg.data[:, 2])
		length(x) < 2 && return nothing
		inc, _ = _seg_dist_azim(x, y, geog)                            # same per-segment geodesic as measure
		total  = sum(inc)
		_, fa  = _seg_dist_azim([x[1], x[end]], [y[1], y[end]], geog)  # first→last forward azimuth
		az     = length(fa) >= 2 ? fa[2] : NaN
		print(total, '/', az, '/', geog ? 1 : 0)
	catch e
		@warn "fault length/azimuth FAILED" exception=(e,)
	end
	return nothing
end

# Closed-ring polygon area for an x,y matrix. geographic -> GMT.geodesicarea (m² on the ellipsoid);
# cartesian -> GMT.geomarea (planar, data-unit²). Always positive (orientation-independent). The ring
# is closed here if the caller's first/last vertex differ.
function _polygon_area(M::Matrix{Float64}, geog::Bool)::Float64
	(M[1, :] != M[end, :]) && (M = vcat(M, M[1:1, :]))         # geometry must be a closed ring
	geog && return abs(GMT.geodesicarea(GMT.mat2ds(M; geom=GMT.wkbPolygon, proj4=GMT.prj4WGS84)))
	return abs(GMT.geomarea(GMT.mat2ds(M; geom=GMT.wkbPolygon)))
end

# "Area under polygon…": area via _polygon_area, CRS-aware. Prints the result (km²+m² when geographic,
# data-unit² when cartesian); the C++ side shows it in a message box.
function _poly_area(scene::Ptr{Cvoid}, path::String, proj4::String, ispoly::Bool)
	D   = GMT.gmtread(path)
	seg = isa(D, Vector) ? D[1] : D
	M   = Float64.(seg.data[:, 1:2])
	size(M, 1) < 3 && (print("Need at least 3 vertices for an area."); return nothing)
	if _measure_isgeog(seg, proj4)
		a_m2 = _polygon_area(M, true)
		print("Area = ", round(a_m2 / 1e6; digits = 4), " km²   (", round(a_m2; digits = 1), " m²)")
	else
		print("Area = ", _polygon_area(M, false), "  (data units²)")
	end
	return nothing
end
