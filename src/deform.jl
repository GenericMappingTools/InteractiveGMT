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
		                  ("Depth", depth), ("Rake", rake), ("Slip", slip))
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
		cmd = "GMT.okada(G; x_start=$x_start, y_start=$y_start, L=$L, W=$W, depth=$depth, " *
		      "strike=$strike, dip=$dip, rake=$rake, slip=$slip)"
		_viewer_log_error(scene, "Okada: $cmd")

		Gdef = GMT.okada(G; x_start=x_start, y_start=y_start, L=L, W=W, depth=depth,
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
# lines) holding the slip model. We only need the surface fault TRACE, so we read just two things:
#   • the "#Fault_segment … nx(Along-strike)=N Dx=KM ny(downdip)=M Dy=KM" header → nx, Dx, ny
#   • the patch block (after "#Lat. Lon. depth slip rake strike dip") → columns Lat Lon … strike
# For each downdip row k we rebuild that row's up-dip edge exactly as Mirone does: shift each of the
# nx patch reference points BACK along strike by Dx/2 (azimuth strike+180), then append the last point
# shifted FORWARD by Dx/2 — an (nx+1)-vertex polyline. Mirone uses circ_geo (spherical); we use
# GMT.geod (the same direct-geodesic helper deform.jl already adopted as our circ_geo, so the import,
# the live trace edit and the Save-fault math all share one geodesic). Each trace is then handed to
# gmtvtk_add_fault_h, which finalizes it through the SAME path as the interactive Draw Fault tool, so
# the imported line carries identical properties (isFault → the Vertical elastic deformation dialog).

# Split a '#'-delimited multi-segment file into (header_lines, numeric_blocks) — the text_read('#')
# equivalent fault_models.m relies on. Blank lines are skipped; each maximal run of numeric rows
# becomes one block (a Vector of Float64 rows), in file order.
function _read_multiseg(path::AbstractString)
	headers = String[]
	blocks  = Vector{Vector{Vector{Float64}}}()
	cur     = Vector{Vector{Float64}}()
	for raw in eachline(path)
		ln = strip(raw)
		isempty(ln) && continue
		if startswith(ln, '#')
			isempty(cur) || (push!(blocks, cur); cur = Vector{Vector{Float64}}())
			push!(headers, ln)
		else
			push!(cur, parse.(Float64, split(ln)))
		end
	end
	isempty(cur) || push!(blocks, cur)
	return headers, blocks
end

# C callback: read `path`, build the surface fault trace and add it as a Draw-Fault line in the window
# `scene`, carrying the file's slip (m) + rake to seed the elastic dialog. Errors are logged to the
# in-window console (never thrown across the ccall).
function _on_importfault(scene::Ptr{Cvoid}, path::Cstring)::Cvoid
	try
		fname = unsafe_string(path)
		headers, blocks = _read_multiseg(fname)

		# nx / Dx from the "#Fault_segment … nx(Along-strike)=… Dx=…km ny(downdip)=… Dy=…km" header.
		hseg = nothing
		for h in headers
			if occursin("nx(Along-strike)", h); hseg = h; break; end
		end
		hseg === nothing && error("not a sub-fault file: no 'Fault_segment … nx(Along-strike)=' header")
		m = match(r"nx\(Along-strike\)=\s*([\d.]+).*?Dx=\s*([\d.]+).*?ny\(downdip\)=\s*([\d.]+)", hseg)
		m === nothing && error("could not parse nx/Dx/ny from header: $hseg")
		nx = round(Int, parse(Float64, m.captures[1]))
		Dx =            parse(Float64, m.captures[2])
		ny = round(Int, parse(Float64, m.captures[3]))

		# The patch block is the last numeric block (after "#Lat. Lon. depth slip rake strike dip");
		# the earlier block is the segment boundary rectangle, which the trace does not need.
		isempty(blocks) && error("no numeric data found in $fname")
		P = blocks[end]
		need = nx * ny
		length(P) < need && error("expected $need patch rows (nx*ny), found $(length(P))")

		# The SURFACE trace is the up-dip edge of the SHALLOWEST downdip row only. Mirone's subfault
		# builds one (hidden) FaultTrace line PER downdip row — the deeper rows are progressively offset
		# perpendicular to strike, so plotting them all would lay down ny parallel lines marching
		# downdip, NOT a fault trace. Keep just the shallowest row (min mean depth, col 3); robust to
		# whether the file lists rows top-to-bottom or bottom-to-top.
		rowdepth(k) = sum(P[(k-1)*nx + i][3] for i in 1:nx) / nx
		ksurf = argmin([rowdepth(k) for k in 1:ny])
		lat  = [P[(ksurf-1)*nx + i][1] for i in 1:nx]   # col 1 = Lat
		lon  = [P[(ksurf-1)*nx + i][2] for i in 1:nx]   # col 2 = Lon
		strk = [P[(ksurf-1)*nx + i][6] for i in 1:nx]   # col 6 = strike

		# Rebuild that row's up-dip edge: shift each patch point BACK along strike by Dx/2 (azimuth
		# strike+180), then append the last point shifted FORWARD by Dx/2 — an (nx+1)-vertex polyline.
		rng = Dx / 2                                    # half the along-strike spacing, in km
		xy  = Float64[]                                 # interleaved lon,lat for gmtvtk_add_fault_h
		for i in 1:nx
			d, = GMT.geod([lon[i], lat[i]], strk[i] + 180, rng; unit=:km)
			push!(xy, d[1], d[2])
		end
		d, = GMT.geod([lon[nx], lat[nx]], strk[1], rng; unit=:km)   # last point forward (row strike)
		push!(xy, d[1], d[2])

		# Dislocation values for the elastic dialog, from the surface row's first patch: slip (col 4) is
		# in CENTIMETRES in the sub-fault format → convert to METRES; rake = col 5 (degrees).
		slip_m = P[(ksurf-1)*nx + 1][4] / 100.0
		rake   = P[(ksurf-1)*nx + 1][5]

		ok = ccall(_fn(:gmtvtk_add_fault_h), Cint,
		           (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cdouble, Cdouble),
		           scene, xy, nx + 1, slip_m, rake)
		ok == 1 || error("viewer rejected the fault trace (dead window handle?)")
		_viewer_log_error(scene, "Import Trace Fault: surface trace added from $(basename(fname)) " *
			"(slip=$(round(slip_m, digits=3)) m, rake=$(round(rake, digits=1))°)")
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
