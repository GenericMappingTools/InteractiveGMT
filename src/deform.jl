# deform.jl — Vertical elastic deformation tool (Okada 1985), Geophysics menu. For now this file
# hosts only the fault-trace endpoint recompute used live by the dialog's Strike / Length boxes:
# a port of Mirone's deform_mansinha.m edit_FaultStrike_CB / edit_FaultLength_CB. Those callbacks
# keep the fault's start vertex fixed and move its end vertex by the DIRECT geodesic — Mirone uses
# vreckon, we use GMT.geod (its equivalent). The C++ dialog (ElasticDialog, 70_window.cpp) hands us
# the fixed start point + strike + length (km) and draws the returned endpoint. The Okada surface-
# deformation compute (Compute / Save fault) is a separate, not-yet-wired concern.

const _FAULTGEOM_BUF = Ref{Vector{UInt8}}(UInt8[0])

# C entry: fault start (lon1,lat1), strike (deg from north, CW) and length (km). Returns the direct-
# geodesic destination as "lon2/lat2" (empty on failure). Geographic faults only — the C++ side does
# the trivial cartesian trig itself, so this is never called for cartesian faults.
function _on_faultgeom(lon1::Cdouble, lat1::Cdouble, strike::Cdouble, len_km::Cdouble)::Cstring
	out = ""
	try
		dest, = GMT.geod([Float64(lon1), Float64(lat1)], Float64(strike), Float64(len_km); unit=:km)
		out = string(dest[1], '/', dest[2])
	catch e
		@warn "fault geodesic (geod) FAILED" exception=(e,)
	end
	_FAULTGEOM_BUF[] = Vector{UInt8}(codeunits(out * "\0"))
	return Cstring(pointer(_FAULTGEOM_BUF[]))
end

function _register_faultgeom()
	fptr = @cfunction((a, b, c, d) -> Base.invokelatest(_on_faultgeom, a, b, c, d),
	                  Cstring, (Cdouble, Cdouble, Cdouble, Cdouble))
	ccall(_fn(:gmtvtk_set_faultgeom_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# ── Okada (1985) surface-deformation compute ──────────────────────────────────────────────────
# C callback (ElasticDialog Compute / Save fault). `scene` = receiving window's Scene*. `cparams` is
# a ';'-separated string assembled in 70_window.cpp's `assemble` lambda:
#   1 act        compute | save
#   2 coordMode  geog | cart                 (informational; okada guesses geog from the grid CRS)
#   3 L          fault length  (km)
#   4 W          fault width   (km)
#   5 fStrike    fault strike  (deg from N, CW)
#   6 dip        fault dip     (deg)
#   7 depth      fault top-centre depth (km)
#   8 depTop     depth to top  (km)          (compute: unused; save: top-edge depth)
#   9 rake       dislocation rake (deg)
#  10 slip       dislocation slip
#  11 hide       hide-fault-planes flag      (UI only)
#  12 scc        SCC flag                    (UI only — okada.jl has no SCC Green functions)
#  13 N  14 q    sub-fault discretisation    (UI only)
#  15 Mu         shear modulus (×10^10)      (UI only — used for Mw, not the elastic field)
#  16 R  17 I    output region / increment   (ignored for now: we compute on the window's own grid)
#  18 xStart 19 yStart   fault start vertex (UpperLeft corner of the fault plane)
#  20 savePath   output .dat for act=="save" (empty for compute)
#
# First iteration: pass the window's loaded GMTgrid (its region + spacing) to GMT.okada and add the
# resulting vertical-deformation grid back into the same window as "Okada z". R/I, SCC, N/q, Mu and the
# hide flag are not used yet.
function _on_elastic(scene::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		parts   = split(unsafe_string(cparams), ';')
		getp(i) = length(parts) >= i ? String(strip(parts[i])) : ""
		num(i)  = (v = tryparse(Float64, getp(i)); v === nothing ? NaN : v)

		act    = getp(1)
		L      = num(3);  W      = num(4);  strike = num(5);  dip   = num(6)
		depth  = num(7);  depTop = num(8);  rake   = num(9);  slip  = num(10)
		x_start = num(18); y_start = num(19)

		(isnan(x_start) || isnan(y_start)) &&
			error("no fault trace found — draw a fault first, then Compute")
		for (nm, val) in (("Length", L), ("Width", W), ("Strike", strike), ("Dip", dip),
		                  ("Depth", depth), ("Depth to Top", depTop), ("Rake", rake), ("Slip", slip))
			isnan(val) && error("missing/invalid '$nm'")
		end

		# Save fault — write the sub-fault-format .dat (port of deform_mansinha.m
		# push_save_subfault_CB) and return; no Okada compute.
		if act == "save"
			_save_subfault(scene, getp(20), x_start, y_start, L, W, strike, dip,
			               depth, isnan(depTop) ? 0.0 : depTop, rake, slip)
			return
		end

		fig = get(_FIGREG, scene, nothing)
		G   = fig isa QtFigure ? fig.G : error("no grid loaded in this window")

		# Echo the exact okada call to the console so the user can check the parameters.
		cmd = "GMT.okada(G; x_start=$x_start, y_start=$y_start, L=$L, W=$W, depth=$depTop, " *
		      "strike=$strike, dip=$dip, rake=$rake, slip=$slip)"
		_viewer_log_error(scene, "Okada: $cmd")

		Gdef = GMT.okada(G; x_start=x_start, y_start=y_start, L=L, W=W, depth=depTop,
		                 strike=strike, dip=dip, rake=rake, slip=slip)

		_add_grid_to_scene(scene, Gdef, "Okada z")
		_viewer_log_error(scene, "Okada: vertical deformation computed (z ∈ " *
			"[$(round(minimum(Gdef.z), digits=4)), $(round(maximum(Gdef.z), digits=4))]) → 'Okada z'")
	catch e
		_viewer_log_error(scene, "Okada deformation FAILED: $(sprint(showerror, e))")
		@warn "okada deformation FAILED" exception=(e,)
	end
	return
end

function _register_elastic()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_elastic, s, c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_elastic_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# ── Import Trace Fault ─────────────────────────────────────────────────────────────────────────
# Port of fault_models.m `subfault`. A sub-fault-format file is a multi-segment text file ('#' header
# lines) holding the slip model. Like Mirone's subfault we plot the dipping fault PLANE and its surface
# projection — not just the trace — so we read:
#   • the "#Fault_segment … nx(Along-strike)=N Dx=KM ny(downdip)=M Dy=KM" header → nx, Dx, ny, Dy
#   • the patch block (after "#Lat. Lon. depth slip rake strike dip") → cols Lat Lon depth slip rake strike dip
# The SURFACE trace is the up-dip edge of the shallowest downdip row, rebuilt exactly as Mirone does:
# shift each of the nx patch reference points BACK along strike by Dx/2 (azimuth strike+180), then
# append the last point shifted FORWARD by Dx/2 — an (nx+1)-vertex polyline. Mirone uses circ_geo
# (spherical); we use GMT.geod (the same direct-geodesic helper deform.jl already adopted as our
# circ_geo, so the import, the live trace edit and the Save-fault math all share one geodesic).
# The fault GEOMETRY (mean strike/dip of that row, total down-dip width ny·Dy, shallowest depth, plus
# slip cm→m and rake) is then handed to gmtvtk_add_fault_geom_h, which finalizes the trace through the
# SAME path as the interactive Draw Fault tool (isFault → Vertical elastic deformation dialog) AND
# immediately draws the dipping plane + its surface-projection rectangle, seeding the dialog from these
# file values — Mirone's subfault behaviour within this app's single-plane fault representation.

# Split a '#'-delimited multi-segment file into (header_lines, numeric_blocks, block_headers) — the
# text_read('#') equivalent fault_models.m relies on. Blank lines are skipped; each maximal run of
# numeric rows becomes one block (a Vector of Float64 rows), in file order. block_headers[i] is the '#'
# line IMMEDIATELY preceding block i — its column header (e.g. "#Lon. Lat. Depth" vs
# "#Lat. Lon. depth slip rake strike dip"), needed because the boundary and patch blocks list lon/lat
# in OPPOSITE order (deform_mansinha.m push_save_subfault).
function _read_multiseg(path::AbstractString)
	headers   = String[]
	blocks    = Vector{Vector{Vector{Float64}}}()
	blockhdrs = String[]
	cur       = Vector{Vector{Float64}}()
	lasthdr   = ""
	for raw in eachline(path)
		ln = strip(raw)
		isempty(ln) && continue
		if startswith(ln, '#')
			isempty(cur) || (push!(blocks, cur); cur = Vector{Vector{Float64}}())
			push!(headers, ln); lasthdr = ln
		else
			isempty(cur) && push!(blockhdrs, lasthdr)   # first data row of a new block -> record its header
			push!(cur, parse.(Float64, split(ln)))
		end
	end
	isempty(cur) || push!(blocks, cur)
	return headers, blocks, blockhdrs
end

# From a block's column header, return (lon_col, lat_col): the 1-based column indices of longitude and
# latitude. The boundary block heads "#Lon. Lat. Depth" (lon first) while the patch block heads
# "#Lat. Lon. depth …" (lat first); we key off whichever of "lon"/"lat" appears first in the header.
# Defaults to (1, 2) — lon,lat — when the header is missing/ambiguous.
function _loncols(hdr::AbstractString)
	h = lowercase(hdr)
	ilon = findfirst("lon", h); ilat = findfirst("lat", h)
	(ilon !== nothing && ilat !== nothing && first(ilat) < first(ilon)) ? (2, 1) : (1, 2)
end

# C callback: read `path`, build the surface fault trace and add it as a Draw-Fault line in the window
# `scene`, carrying the file's slip (m) + rake to seed the elastic dialog. Errors are logged to the
# in-window console (never thrown across the ccall).
function _on_importfault(scene::Ptr{Cvoid}, path::Cstring)::Cvoid
	try
		fname = unsafe_string(path)
		headers, blocks, blockhdrs = _read_multiseg(fname)
		isempty(blocks) && error("no numeric data found in $fname")

		# nx / Dx / ny / Dy from the "#Fault_segment … nx(Along-strike)=… Dx=…km ny(downdip)=… Dy=…km" header.
		hseg = nothing
		for h in headers
			if occursin("nx(Along-strike)", h); hseg = h; break; end
		end
		hseg === nothing && error("not a sub-fault file: no 'Fault_segment … nx(Along-strike)=' header")
		m = match(r"nx\(Along-strike\)=\s*([\d.]+).*?Dx=\s*([\d.]+).*?ny\(downdip\)=\s*([\d.]+).*?Dy=\s*([\d.]+)", hseg)
		m === nothing && error("could not parse nx/Dx/ny/Dy from header: $hseg")
		nx = round(Int, parse(Float64, m.captures[1]))
		Dx =            parse(Float64, m.captures[2])
		ny = round(Int, parse(Float64, m.captures[3]))
		Dy =            parse(Float64, m.captures[4])   # down-dip patch width (km); ny·Dy = total fault width

		# Identify the two blocks by width: the BOUNDARY rectangle has 3 columns (Lon Lat Depth), the
		# PATCH slip model has ≥7 (Lat Lon depth slip rake strike dip). The boundary block holds the
		# EXACT fault footprint the file was written from — its rows 1,2 are the trace up-dip endpoints
		# and rows 3,4 the down-dip corners — so the trace we display matches the file byte-for-byte
		# instead of being re-derived (and drifting) from the patch centres.
		bnd_i = findfirst(b -> !isempty(b) && length(b[1]) == 3, blocks)
		pat_i = findlast(b -> !isempty(b) && length(b[1]) >= 7, blocks)
		pat_i === nothing && error("no patch block (≥7 columns) found in $fname")
		P = blocks[pat_i]
		need = nx * ny
		length(P) < need && error("expected $need patch rows (nx*ny), found $(length(P))")
		plonc, platc = _loncols(blockhdrs[pat_i])      # patch block lon/lat column indices

		# Dislocation + plane geometry come from the PATCH block (the physics): slip (col 4, CENTIMETRES
		# → METRES), rake (col 5), strike (col 6), dip (col 7), depth-to-top (col 3, shallowest patch),
		# total down-dip width ny·Dy. Means are over the shallowest downdip row.
		rowdepth(k) = sum(P[(k-1)*nx + i][3] for i in 1:nx) / nx
		ksurf  = argmin([rowdepth(k) for k in 1:ny])
		base   = (ksurf - 1) * nx
		slip_m = P[base + 1][4] / 100.0
		rake   = P[base + 1][5]
		strike = sum(P[base + i][6] for i in 1:nx) / nx
		dip    = sum(P[base + i][7] for i in 1:nx) / nx
		width  = ny * Dy
		# col 3 is the depth to the BASE of the patch (Mirone writes FaultDepth, not FaultTopDepth —
		# deform_mansinha.m push_save_subfault). Depth-to-top of the whole fault = shallowest patch base
		# minus that patch's own vertical drop Dy·sin(dip); the dialog re-derives bottom Depth as
		# depthTop + W·sin(dip) = the deepest base. Clamp at 0 (no fault above the surface).
		depthTop = max(0.0, minimum(P[base + i][3] for i in 1:nx) - Dy * sind(dip))

		# Build the surface TRACE. Prefer the boundary block (exact file coordinates); fall back to
		# reconstructing the shallowest row's up-dip edge from the patch centres (Mirone subfault:
		# shift each by Dx/2 along strike+180, last point +Dx/2 forward) only when no boundary block.
		if bnd_i !== nothing && length(blocks[bnd_i]) >= 2
			B = blocks[bnd_i]
			blonc, blatc = _loncols(blockhdrs[bnd_i])
			xy = Float64[ B[1][blonc], B[1][blatc], B[2][blonc], B[2][blatc] ]   # trace endpoints, verbatim
			ntr = 2
		else
			lat = [P[base + i][platc] for i in 1:nx]
			lon = [P[base + i][plonc] for i in 1:nx]
			strk = [P[base + i][6] for i in 1:nx]
			rng = Dx / 2
			xy  = Float64[]
			for i in 1:nx
				d, = GMT.geod([lon[i], lat[i]], strk[i] + 180, rng; unit=:km)
				push!(xy, d[1], d[2])
			end
			d, = GMT.geod([lon[nx], lat[nx]], strk[1], rng; unit=:km)
			push!(xy, d[1], d[2])
			ntr = nx + 1
		end

		ok = ccall(_fn(:gmtvtk_add_fault_geom_h), Cint,
		           (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cdouble, Cint),
		           scene, xy, ntr, slip_m, rake, strike, dip, width, depthTop, Cint(1))
		ok == 1 || error("viewer rejected the fault trace (dead window handle?)")
		_viewer_log_error(scene, "Import Trace Fault: $(basename(fname)) — trace + dipping plane " *
			"(strike=$(round(strike, digits=1))°, dip=$(round(dip, digits=1))°, W=$(round(width, digits=1)) km, " *
			"slip=$(round(slip_m, digits=3)) m, rake=$(round(rake, digits=1))°)")
	catch e
		_viewer_log_error(scene, "Import Trace Fault FAILED: $(sprint(showerror, e))")
		@warn "Import Trace Fault FAILED" exception=(e,)
	end
	return
end

function _register_importfault()
	fptr = @cfunction((s, p) -> Base.invokelatest(_on_importfault, s, p), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_importfault_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# ── Import Model Slip ──────────────────────────────────────────────────────────────────────────
# Port of fault_models.m `subfault` (the FULL slip model, not just the trace). The same sub-fault
# file Import Trace Fault reads, but here every sub-fault patch is plotted as its own filled SURFACE-
# PROJECTION polygon coloured by slip — Mirone's loop of `patch('XData',x,'YData',y,'FaceColor',cor)`.
# We do NOT draw the dipping 3-D planes (only the surface projections), so the depth column + the
# hidden FaultTrace lines + the per-patch Okada/Mansinha context menus of the original are skipped.

# circ_geo (Mirone) with np=1 is a single direct-geodesic forward point: the destination at distance
# `dist_km` along azimuth `az` (deg from north, CW) from (lon,lat). GMT.jl's `circgeo` builds whole
# circles / regular polygons (a fixed azimuth set around one centre), so the per-patch single-azimuth
# step is `GMT.geod` — the direct-geodesic primitive `circgeo` is itself built on, and the one
# deform.jl already adopted as our circ_geo. Returns (lon, lat) in degrees.
function _geod_fwd(lon, lat, az, dist_km)
	d, = GMT.geod([Float64(lon), Float64(lat)], Float64(az), Float64(dist_km); unit=:km)
	return d[1], d[2]
end

# Port of MATLAB hot(86) reduced to rows 84:-1:23 — the exact 62-colour reversed-hot ramp subfault()
# builds (`cmap = hot(86); cmap = cmap(84:-1:23,:)`). Returns a 62×3 matrix of RGB in 0..1.
function _hot_subfault()
	m = 86;  n = 32                                  # n = fix(3/8*86)
	r = vcat((1:n) ./ n, ones(m - n))
	g = vcat(zeros(n), (1:n) ./ n, ones(m - 2n))
	b = vcat(zeros(2n), (1:(m - 2n)) ./ (m - 2n))
	return hcat(r, g, b)[84:-1:23, :]
end

# C callback: read `path`, build the surface-projection quad of every sub-fault patch (Mirone subfault
# geometry) coloured by slip, and add them as one filled-polygon group ("Slip model") to window `scene`.
# Errors are logged to the in-window console (never thrown across the ccall).
function _on_modelslip(scene::Ptr{Cvoid}, path::Cstring)::Cvoid
	try
		fname = unsafe_string(path)
		headers, blocks, blockhdrs = _read_multiseg(fname)
		isempty(blocks) && error("no numeric data found in $fname")

		# nx / Dx / ny / Dy from the "#Fault_segment … nx(Along-strike)=… Dx=…km ny(downdip)=… Dy=…km" header.
		hseg = nothing
		for h in headers
			if occursin("nx(Along-strike)", h); hseg = h; break; end
		end
		hseg === nothing && error("not a sub-fault file: no 'Fault_segment … nx(Along-strike)=' header")
		mm = match(r"nx\(Along-strike\)=\s*([\d.]+).*?Dx=\s*([\d.]+).*?ny\(downdip\)=\s*([\d.]+).*?Dy=\s*([\d.]+)", hseg)
		mm === nothing && error("could not parse nx/Dx/ny/Dy from header: $hseg")
		nx = round(Int, parse(Float64, mm.captures[1]))
		Dx =            parse(Float64, mm.captures[2])
		ny = round(Int, parse(Float64, mm.captures[3]))
		Dy =            parse(Float64, mm.captures[4])

		# Patch block: the ≥7-column slip model (Lat Lon depth slip rake strike dip).
		pat_i = findlast(b -> !isempty(b) && length(b[1]) >= 7, blocks)
		pat_i === nothing && error("no patch block (≥7 columns) found in $fname")
		P = blocks[pat_i]
		need = nx * ny
		length(P) < need && error("expected $need patch rows (nx*ny), found $(length(P))")
		# subfault HARD-CODES the patch columns: col1 = Lat, col2 = Lon, col3 = depth, col4 = slip(cm),
		# col5 = rake, col6 = strike, col7 = dip. The "#Lon. Lat. …" comment line is misleading (the data
		# is lat-first), so we ignore it exactly as Mirone's subfault() does — never trust that header.
		platc, plonc = 1, 2

		# Slip in METRES (col 4 is cm). Colour ramp keyed on the global slip range (Mirone subfault).
		slipAll = [P[i][4] * 1e-2 for i in 1:need]
		minSlip, maxSlip = extrema(slipAll)
		deltaSlip = maxSlip - minSlip
		deltaSlip == 0 && (deltaSlip = 1.0)            # single-patch model: colour is arbitrary
		cmap   = _hot_subfault()
		nCores = size(cmap, 1) - 1                     # 61

		xy      = Float64[]
		vcounts = Cint[]
		rgb     = Float64[]
		for k in 1:ny                                  # loop over downdip rows
			idx    = (k - 1) * nx + 1 : k * nx
			tmpy   = [P[i][platc] for i in idx]        # patch reference lat
			tmpx   = [P[i][plonc] for i in idx]        # patch reference lon
			strk   = [P[i][6]     for i in idx]
			dip    = [P[i][7]     for i in idx]
			slip_k = [P[i][4] * 1e-2 for i in idx]

			# Up-dip edge of the row (nx+1 vertices): shift each reference point back along strike+180 by
			# Dx/2, then append the last point shifted forward by Dx/2 along the first patch's strike.
			lon = Vector{Float64}(undef, nx + 1);  lat = similar(lon)
			for i in 1:nx
				lon[i], lat[i] = _geod_fwd(tmpx[i], tmpy[i], strk[i] + 180, Dx / 2)
			end
			lon[nx+1], lat[nx+1] = _geod_fwd(tmpx[nx], tmpy[nx], strk[1], Dx / 2)

			for i in 1:nx                              # one filled quad per patch
				# Down-dip corners: walk perpendicular to strike (strike+90) by Dy·cos(dip).
				rp = Dy * cosd(dip[i])
				c1lon, c1lat = _geod_fwd(lon[i],   lat[i],   strk[i] + 90, rp)
				c2lon, c2lat = _geod_fwd(lon[i+1], lat[i+1], strk[i] + 90, rp)
				push!(xy, lon[i],   lat[i],
				          lon[i+1], lat[i+1],
				          c2lon,    c2lat,
				          c1lon,    c1lat)
				push!(vcounts, Cint(4))
				ci = clamp(round(Int, (slip_k[i] - minSlip) / deltaSlip * nCores + 1), 1, size(cmap, 1))
				push!(rgb, cmap[ci, 1], cmap[ci, 2], cmap[ci, 3])
			end
		end

		npatch = length(vcounts)
		added = GC.@preserve xy vcounts rgb ccall(_fn(:gmtvtk_add_slip_patches_h), Cint,
			(Ptr{Cvoid}, Ptr{Cdouble}, Ptr{Cint}, Cint, Ptr{Cdouble}, Cstring),
			scene, xy, vcounts, Cint(npatch), rgb, "Slip model")
		added == 0 && error("viewer rejected the slip model (dead window handle?)")
		_viewer_log_error(scene, "Import Model Slip: $(basename(fname)) — $added patches " *
			"(nx=$nx, ny=$ny, slip ∈ [$(round(minSlip, digits=3)), $(round(maxSlip, digits=3))] m)")
	catch e
		_viewer_log_error(scene, "Import Model Slip FAILED: $(sprint(showerror, e))")
		@warn "Import Model Slip FAILED" exception=(e,)
	end
	return
end

function _register_modelslip()
	fptr = @cfunction((s, p) -> Base.invokelatest(_on_modelslip, s, p), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_modelslip_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# Fixed-decimal "%.Nf" formatter (Printf is not a package dependency). Mirrors the C printf width so
# the saved file matches Mirone's deform_mansinha output byte-for-byte.
function _ffmt(x::Real, n::Int)
	(isnan(x) || isinf(x)) && return "NaN"
	neg = x < 0
	scaled = round(BigInt, abs(float(x)) * BigInt(10)^n)
	s = lpad(string(scaled), n + 1, '0')
	int = s[1:end-n]; frac = s[end-n+1:end]
	return (neg ? "-" : "") * (n == 0 ? int : int * "." * frac)
end

# Port of deform_mansinha.m push_save_subfault_CB — write the current single fault / single segment in
# the sub-fault format: header, the 5-point fault-plane boundary (top edge from the trace + the two
# downdip corners offset perpendicular to strike by the width) and one patch line carrying slip/rake/
# strike/dip at the fault-trace mid-point. Geographic faults only; the offsets use the direct geodesic
# (GMT.geod, our circ_geo). x1/y1 = fault start vertex; the end vertex is re-derived from strike+length
# (the same geodesic the live trace uses), so it needs no extra param. Depths: depTop on the top edge,
# `depth` (bottom-edge depth) on the downdip corners + patch. Slip written in cm (slip·100).
function _save_subfault(scene, fname, x1, y1, L, W, strike, dip, depth, depTop, rake, slip)
	isempty(fname) && error("no output file selected")
	dest, = GMT.geod([Float64(x1), Float64(y1)], Float64(strike), Float64(L); unit=:km)  # end vertex
	x2, y2 = dest[1], dest[2]
	p2, = GMT.geod([x2, y2], strike + 90, W; unit=:km)        # downdip corner below end vertex
	p1, = GMT.geod([Float64(x1), Float64(y1)], strike + 90, W; unit=:km)   # below start vertex
	pm, = GMT.geod([Float64(x1), Float64(y1)], strike, L / 2; unit=:km)    # trace mid-point

	open(fname, "w") do io
		println(io, "#Total number of fault_segments=     1")
		println(io, "#Fault_segment =   1 nx(Along-strike)=   1 Dx= " * _ffmt(L, 2) *
		            "km ny(downdip)=   1 Dy= " * _ffmt(W, 2) * "km")
		println(io, "#Boundary of Fault_segment     1")
		println(io, "#Lon.  Lat.  Depth")
		bnd(x, y, d) = println(io, _ffmt(x, 5) * "\t" * _ffmt(y, 5) * "\t" * _ffmt(d, 5))
		bnd(x1, y1, depTop)
		bnd(x2, y2, depTop)
		bnd(p2[1], p2[2], depth)
		bnd(p1[1], p1[2], depth)
		bnd(x1, y1, depTop)
		println(io, "#Lat. Lon. depth slip rake strike dip")
		println(io, _ffmt(pm[2], 4) * "\t" * _ffmt(pm[1], 4) * "\t" * _ffmt(depth, 3) * "\t" *
		            _ffmt(slip * 100, 2) * "\t" * _ffmt(rake, 1) * "\t" * _ffmt(strike, 1) * "\t" *
		            _ffmt(dip, 1))
	end
	_viewer_log_error(scene, "Fault saved (sub-fault format) → $fname")
	return
end
