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
#
# `datype` is the Preferences "Dist/Azim type"; `back` is the "Dir" (Backward → reverse azimuths):
#   geographic, "Ellipsoidal" -> GMT.invgeod (Karney geodesic on WGS84): dist m, forward az.
#   geographic, "Spherical"   -> GMT.invgeod on a sphere (+proj=longlat +ellps=sphere).
#   geographic, "Flat Earth"  -> GMT.mapproject -jf (flat-earth dist km + azimuth).
#   cartesian -> planar: hypot (data units) + atand(dx,dy) (bearing N(+Y)→CW), shifted to match.
function _seg_dist_azim(x::Vector{Float64}, y::Vector{Float64}, geog::Bool;
                        datype::AbstractString="Ellipsoidal", back::Bool=false)
	n = length(x)
	n < 2 && return (zeros(n), fill(NaN, n))
	if geog
		if datype == "Flat Earth"
			seg = GMT.mat2ds(hcat(x, y); proj4=GMT.prj4WGS84)
			inc = GMT.mapproject(seg, track_distances="+i+uk", j=:f).data[:, end]
			az  = GMT.mapproject(seg, azim=(back ? "b" : "f"), j=:f).data[:, end]   # az[1] = NaN
			return (inc, az)
		end
		# Ellipsoidal / Spherical via the inverse geodesic (geod). One pair per consecutive vertex.
		proj = (datype == "Spherical") ? "+proj=longlat +ellps=sphere" : ""
		p1 = hcat(x[1:end-1], y[1:end-1]);  p2 = hcat(x[2:end], y[2:end])
		d, a1, _ = GMT.invgeod(p1, p2; proj=proj, backward=back)
		dv = d  isa Real ? [d]  : d                                # n==2 returns scalars
		av = a1 isa Real ? [a1] : a1
		inc = [0.0; dv ./ 1000.0]                                  # m → km, aligned to arriving vertex
		az  = [NaN; mod.(av, 360.0)]                               # az ∈ [0,360) like mapproject -Af
		return (inc, az)
	end
	dx = diff(x);  dy = diff(y)
	inc    = [0.0; hypot.(dx, dy)]                              # planar distance, data units
	seg_az = mod.(atand.(dx, dy) .+ (back ? 180.0 : 0.0), 360.0)  # bearing of each segment, length n-1
	return (inc, [NaN; seg_az])                                 # align to the arriving vertex (GMT-style)
end

# Length unit scale + label from the Preferences "Measure units". Distances come out of
# _seg_dist_azim in km (geographic); this multiplies them into the chosen unit. Cartesian objects
# have no physical unit, so they always stay in data units regardless of the preference.
function _length_scale(units::AbstractString, geog::Bool)
	geog || return (1.0, "units")
	units == "meters"         && return (1000.0,      "m")
	units == "nautical miles" && return (1/1.852,     "NM")
	units == "miles"          && return (1/1.609344,  "mi")
	return (1.0, "km")                                         # kilometers (default)
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
function _line_length(scene::Ptr{Cvoid}, path::String, proj4::String, ispoly::Bool,
                      datype::AbstractString="Ellipsoidal", dir::AbstractString="Forward",
                      units::AbstractString="kilometers")
	segs, geog, _ = _measure_load(path, proj4)
	back = (dir == "Backward")
	scale, ulab = _length_scale(units, geog)                    # Preferences "Measure units"
	rows = Vector{NTuple{6,Float64}}()                          # Seg, #, X, Y, Δlen, cumlen
	total = 0.0
	for (si, seg) in enumerate(segs)
		x = Float64.(seg.data[:, 1]);  y = Float64.(seg.data[:, 2])
		isempty(x) && continue
		inc, _ = _seg_dist_azim(x, y, geog; datype=datype, back=back)
		inc = inc .* scale                                      # km → chosen unit (1.0 for cartesian)
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

# ONE leg of the interactive ruler (85_polygon.cpp's rulerExactLeg). The C++ side writes either the
# two 2-D end points or a grid-spacing densified 3-D terrain track, using the same lineWriteTable
# subdivision as the 3-D line export. Same engine as "Line length…" — _measure_load to read the
# table, _seg_dist_azim for the CRS-aware Preferences distance, _length_scale for the Preferences
# unit — so a ruler leg and a measured line length of the same two points are the same number.
# `terrain` adds the vertical rise between consecutive samples (grid z is metres when geographic).
# Prints "unit|value" for the caller to parse; `dir` has no meaning for a length.
function _ruler_length(path::String, proj4::String, datype::AbstractString="Ellipsoidal",
                       units::AbstractString="kilometers", terrain::Bool=false)
	segs, geog, _ = _measure_load(path, proj4)
	x = Float64.(segs[1].data[:, 1]);  y = Float64.(segs[1].data[:, 2])
	length(x) < 2 && (print("units|0"); return nothing)
	inc, _ = _seg_dist_azim(x, y, geog; datype=datype)
	if (terrain && size(segs[1].data, 2) >= 3)
		z = Float64.(segs[1].data[:, 3])
		inc = geog ? hypot.(inc, [0.0; diff(z)] ./ 1000.0) : hypot.(inc, [0.0; diff(z)])
	end
	scale, ulab = _length_scale(units, geog)
	print(ulab, '|', sum(inc) * scale)
	return nothing
end

# The Ctrl-drag profile's distance axis (60_profile.cpp `profileDistances`): the CUMULATIVE horizontal
# distance at every sample of the track, by the same engine and Preferences as the ruler above
# (`_seg_dist_azim`, `_length_scale`). Prints "<unit>|d1 d2 … dn".
function _profile_distances(path::String, proj4::String, datype::AbstractString="Ellipsoidal",
                            units::AbstractString="kilometers")
	segs, geog, _ = _measure_load(path, proj4)
	x = Float64.(segs[1].data[:, 1]);  y = Float64.(segs[1].data[:, 2])
	inc, _ = _seg_dist_azim(x, y, geog; datype=datype)
	scale, ulab = _length_scale(units, geog)
	print(ulab, '|', join((string(v) for v in cumsum(inc) .* scale), ' '))
	return nothing
end

# "Azimuth(s)…": for a single straight line the one forward azimuth; for a polyline a per-vertex
# #/X/Y/azimuth table (forward bearing to the next vertex; last vertex repeats the prior).
function _line_azimuth(scene::Ptr{Cvoid}, path::String, proj4::String, ispoly::Bool,
                       datype::AbstractString="Ellipsoidal", dir::AbstractString="Forward",
                       units::AbstractString="kilometers")
	segs, geog, _ = _measure_load(path, proj4)
	back = (dir == "Backward")
	rows = Vector{NTuple{5,Float64}}()                          # Seg, #, X, Y, azimuth
	for (si, seg) in enumerate(segs)
		x = Float64.(seg.data[:, 1]);  y = Float64.(seg.data[:, 2])
		isempty(x) && continue
		_, az = _seg_dist_azim(x, y, geog; datype=datype, back=back)
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
		# Fault strike = FLAT-EARTH azimuth of the trace's end points (first→last). The okada fault
		# plane is built on a flat-earth strike, so seed the dialog Strike from the same model rather
		# than the geodesic azimuth used elsewhere. Cartesian faults are already planar (atan).
		if geog
			a  = GMT.mapproject([x[1] y[1]; x[end] y[end]], A="f$(x[1])/$(y[1])", j=:f)
			az = a.data[end, end]
		else
			_, fa = _seg_dist_azim([x[1], x[end]], [y[1], y[end]], geog)
			az = length(fa) >= 2 ? fa[2] : NaN
		end
		print(total, '/', az, '/', geog ? 1 : 0)
	catch e
		@tool_error "fault length/azimuth FAILED" exception=(e,)
	end
	return nothing
end

# Closed-ring polygon area for an x,y matrix. Returns m² (geographic) or data-unit² (cartesian),
# always positive (orientation-independent). The ring is closed here if first/last vertex differ.
#   geographic + "Ellipsoidal" -> GMT.geodesicarea (exact area on the WGS84 ellipsoid).
#   geographic, anything else  -> GMT.gmtspatial -Q (e.g. spherical geographic area), km² → m².
#   cartesian                  -> GMT.gmtspatial -Q (planar area, data units²).
_polygon_area(M::Matrix{Float64}, geog::Bool, datype::AbstractString="Ellipsoidal")::Float64 =
	_rings_areas([M], geog, datype)[1]

# THE area of polygon rings, one number per ring (m² when geographic, data units² otherwise) — the
# engines the Preferences Dist/Azim type names, as always. ONE gmtspatial call for every ring when the
# engine is gmtspatial (one row per ring), so a traced outline of thousands of rings is not thousands
# of module calls; the ellipsoidal geodesic area is asked ring by ring (GMT.geodesicarea sums a vector).
function _rings_areas(rings::Vector{Matrix{Float64}}, geog::Bool, datype::AbstractString="Ellipsoidal")::Vector{Float64}
	closed = [(M[1, :] != M[end, :]) ? vcat(M, M[1:1, :]) : M for M in rings]   # geometry must be closed rings
	if geog && datype == "Ellipsoidal"
		return [abs(Float64(GMT.geodesicarea(GMT.mat2ds(M; geom=GMT.wkbPolygon, proj4=GMT.prj4WGS84)))) for M in closed]
	end
	D = geog ? [GMT.mat2ds(M; geom=GMT.wkbPolygon, proj4=GMT.prj4WGS84) for M in closed] :
	           [GMT.mat2ds(M; geom=GMT.wkbPolygon) for M in closed]
	R = geog ? GMT.gmtspatial(D; area="k") : GMT.gmtspatial(D; area=true)
	a = abs.(Float64.(R.data[:, end]))
	size(R.data, 1) == length(closed) || error("area: got $(size(R.data, 1)) areas for $(length(closed)) rings")
	return geog ? a .* 1e6 : a                                    # gmtspatial -Qk reports km² → m²
end

# Is each ring a HOLE? A ring lying inside an odd number of the other rings is one (even-odd rule), so
# the answer needs no ring orientation. Tested with the midpoint of the ring's first edge — on a traced
# pixel outline that point is never on another ring — against the rings whose box holds it.
_ring_holes(rings::Vector{Matrix{Float64}})::Vector{Bool} = [isodd(length(c)) for c in _ring_containers(rings)]

# For every ring, the rings it lies INSIDE (indices) — the nesting both the hole rule and the polygon a
# ring belongs to are read from. Each ring is tested with the midpoint of its first edge (on a traced
# pixel outline never on another ring) against the rings whose box holds that point.
function _ring_containers(rings::Vector{Matrix{Float64}})::Vector{Vector{Int}}
	box = [(minimum(@view M[:, 1]), maximum(@view M[:, 1]), minimum(@view M[:, 2]), maximum(@view M[:, 2])) for M in rings]
	inside(px, py, M) = begin
		c = false;  n = size(M, 1)
		@inbounds for i in 1:n
			j = i == 1 ? n : i - 1
			yi, yj = M[i, 2], M[j, 2]
			if (yi > py) != (yj > py) && px < (M[j, 1] - M[i, 1]) * (py - yi) / (yj - yi) + M[i, 1]
				c = !c
			end
		end
		c
	end
	cont = [Int[] for _ in rings]
	Threads.@threads for k in eachindex(rings)
		M = rings[k]
		px = (M[1, 1] + M[2, 1]) / 2;  py = (M[1, 2] + M[2, 2]) / 2
		for j in eachindex(rings)
			j == k && continue
			b = box[j]
			(b[1] <= px <= b[2] && b[3] <= py <= b[4]) || continue
			inside(px, py, rings[j]) && push!(cont[k], j)
		end
	end
	return cont
end

# The POLYGON ring `k` (0-based) belongs to, printed as 0-based ring indices: its outer ring first, then
# that ring's own holes. A ring at even nesting depth is an outer ring; a hole (odd depth) answers with
# the ring directly around it. The holes of an outer ring are the rings exactly one level inside it.
# Called by the viewer (overlayRingRegion, 55_lineprops.cpp) for "Area under polygon…" / "Delete this
# polygon" on the polygon clicked out of a many-polygon outline. `path` is the outline's ring table.
function _ring_region(path::AbstractString, k::Integer)
	D = GMT.gmtread(String(path))
	segs = D isa Vector ? D : [D]
	rings = [Float64.(d.data[:, 1:2]) for d in segs]
	cont = _ring_containers(rings)
	depth = length.(cont)
	i = k + 1
	if isodd(depth[i])                                 # a hole: the ring right around it is the polygon
		i = cont[i][argmax(depth[cont[i]])]
	end
	holes = [j for j in eachindex(rings) if depth[j] == depth[i] + 1 && i in cont[j]]
	print(join(string.([i; holes] .- 1), ' '))
	return nothing
end

# "Area under polygon…": area via _polygon_area, CRS-aware + Preferences Dist/Azim type. Prints the
# result (km²+m² when geographic, data-unit² when cartesian); the C++ side shows it in a message box.
# `dir` is accepted (shared C-side signature) but unused — direction has no meaning for an area.
function _poly_area(scene::Ptr{Cvoid}, path::String, proj4::String, ispoly::Bool,
                    datype::AbstractString="Ellipsoidal", dir::AbstractString="Forward",
                    units::AbstractString="kilometers")
	D   = GMT.gmtread(path)
	seg = isa(D, Vector) ? D[1] : D
	# SEVERAL rings (a "Digitize whites" outline: every white region, holes included): each ring's area,
	# the holes taken off (_ring_holes). Used to measure only the first ring and ignore the rest.
	if D isa Vector && length(D) > 1
		rings = [Float64.(d.data[:, 1:2]) for d in D if size(d.data, 1) >= 3]
		isempty(rings) && (print("Need at least 3 vertices for an area."); return nothing)
		geog = _measure_isgeog(seg, proj4)
		a = _rings_areas(rings, geog, datype)
		h = _ring_holes(rings)
		tot = sum(a[.!h]) - sum(a[h]; init = 0.0)
		what = "$(count(.!h)) polygon(s)" * (any(h) ? ", $(count(h)) hole(s) subtracted" : "")
		if geog
			print("Area = ", round(tot / 1e6; digits = 4), " km²   (", round(tot; digits = 1), " m²)   — ", what)
		else
			print("Area = ", tot, "  (data units²)   — ", what)
		end
		return nothing
	end
	M   = Float64.(seg.data[:, 1:2])
	size(M, 1) < 3 && (print("Need at least 3 vertices for an area."); return nothing)
	if _measure_isgeog(seg, proj4)
		a_m2 = _polygon_area(M, true, datype)
		print("Area = ", round(a_m2 / 1e6; digits = 4), " km²   (", round(a_m2; digits = 1), " m²)")
	else
		print("Area = ", _polygon_area(M, false, datype), "  (data units²)")
	end
	return nothing
end

# "Extract profile…": the grdtrack twin of the Ctrl+left-drag profiler. Sample the window's DISPLAYED grid
# along a drawn line/polyline and open the (distance, elevation) graph in the standalone X,Y plot
# tool (Object Manager + Analysis + save). The line is DENSIFIED here by the grid's node spacing (a
# two-vertex straight line becomes a full-resolution track), grdtrack interpolates z on the FULL-res
# grid, and the distance axis is the same CRS-aware geodesic (_seg_dist_azim) the measure menu and
# Preferences use — so distances match "Line length…". `dir` has no meaning for a profile.
function _extract_profile(scene::Ptr{Cvoid}, path::String, proj4::String,
                          datype::AbstractString="Ellipsoidal", units::AbstractString="kilometers",
                          gridname::AbstractString="", elemname::AbstractString="")
	# `gridname` is the Scene Objects label of the grid ON DISPLAY (the C++ side's activeGridName): with
	# two grids loaded, grdtrack must sample the VISIBLE one, not the base grid buried under it. Empty
	# label = the base surface is the visible one, which is also _find_object's first-of-kind fallback.
	G = _find_object(scene, :grid, String(gridname))
	G === nothing && (print("No grid to sample."); return nothing)
	D   = GMT.gmtread(path)
	seg = isa(D, Vector) ? D[1] : D                             # a drawn line/polyline is one segment
	x = Float64.(seg.data[:, 1]);  y = Float64.(seg.data[:, 2])
	length(x) < 2 && (print("Need at least two vertices for a profile."); return nothing)

	# Densify each segment by the grid node spacing (min of |dx|,|dy|) so grdtrack samples the grid at
	# ~one point per node — mirrors the 3-D save-with-distance subdivision in lineWriteTable.
	dx = abs(G.inc[1]);  dy = abs(G.inc[2])
	spacing = (dx > 0 && dy > 0) ? min(dx, dy) : max(dx, dy)
	xd = Float64[];  yd = Float64[]
	for i in 1:length(x)-1
		seglen = hypot(x[i+1] - x[i], y[i+1] - y[i])
		nsub   = (spacing > 0 && seglen > 0) ? clamp(ceil(Int, seglen / spacing), 1, 20000) : 1
		for k in 0:nsub-1                                       # a..(b exclusive); next segment writes b
			t = k / nsub
			push!(xd, x[i] + t * (x[i+1] - x[i]));  push!(yd, y[i] + t * (y[i+1] - y[i]))
		end
	end
	push!(xd, x[end]);  push!(yd, y[end])

	z    = Float64.(GMT.grdtrack(G, hcat(xd, yd)).data[:, end])   # bilinear on the full-res grid
	# A line that Vector Operations' `thicken` widened carries N+1 parallel tracks; its profile is
	# their STACK (Mirone's StackTrack). The tracks are offsets of THIS line, so each is sampled at
	# the same densified positions, shifted — one grdtrack per track, averaged ignoring the samples
	# that fell off the grid. An ordinary line finds nothing here and the value above stands.
	tracks = get(_LOP_THICK, (scene, String(elemname)), nothing)
	if tracks !== nothing && length(tracks) > 1
		acc = zeros(Float64, length(xd));  cnt = zeros(Int, length(xd))
		for T in tracks
			# Each track is the line shifted by a constant, so its offset is read off vertex 1.
			ox = T[1, 1] - x[1]
			oy = T[1, 2] - y[1]
			zt = Float64.(GMT.grdtrack(G, hcat(xd .+ ox, yd .+ oy)).data[:, end])
			for i in eachindex(zt)
				isnan(zt[i]) && continue
				acc[i] += zt[i];  cnt[i] += 1
			end
		end
		z = [cnt[i] > 0 ? acc[i] / cnt[i] : NaN for i in eachindex(acc)]
	end
	geog = _measure_isgeog(seg, proj4)
	inc, _        = _seg_dist_azim(xd, yd, geog; datype=datype)   # CRS-aware incremental distance
	scale, ulab   = _length_scale(units, geog)
	dist          = cumsum(inc .* scale)

	keep = .!isnan.(z)                                           # drop samples that fell off the grid
	dist = dist[keep];  z = z[keep]
	length(z) < 2 && (print("Profile is entirely off the grid."); return nothing)

	xlab = geog ? "Distance ($ulab)" : "Distance"
	# Window title = "Profile <grid name>" and nothing longer (same wording the bottom-dock Profile
	# panel's own "Open in X,Y plot tool" uses, ProfilePanel::setProfile).
	ttl = isempty(gridname) ? "Profile" : "Profile " * String(gridname)
	(tracks !== nothing && length(tracks) > 1) && (ttl *= " (stacked, $(length(tracks)) tracks)")
	_xyplot(dist, z; name=ttl, title=ttl, xlabel=xlab, ylabel="Elevation")
	return nothing
end
