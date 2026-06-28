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
#   8 depTop     depth to top  (km)          (unused here; okada derives the centroid itself)
#   9 rake       dislocation rake (deg)
#  10 slip       dislocation slip
#  11 hide       hide-fault-planes flag      (UI only)
#  12 scc        SCC flag                    (UI only — okada.jl has no SCC Green functions)
#  13 N  14 q    sub-fault discretisation    (UI only)
#  15 Mu         shear modulus (×10^10)      (UI only — used for Mw, not the elastic field)
#  16 R  17 I    output region / increment   (ignored for now: we compute on the window's own grid)
#  18 xStart 19 yStart   fault start vertex (UpperLeft corner of the fault plane)
#
# First iteration: pass the window's loaded GMTgrid (its region + spacing) to GMT.okada and add the
# resulting vertical-deformation grid back into the same window as "Okada z". R/I, SCC, N/q, Mu and the
# hide flag are not used yet.
function _on_elastic(scene::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		parts   = split(unsafe_string(cparams), ';')
		getp(i) = length(parts) >= i ? String(strip(parts[i])) : ""
		num(i)  = (v = tryparse(Float64, getp(i)); v === nothing ? NaN : v)

		L      = num(3);  W     = num(4);  strike = num(5);  dip   = num(6)
		depth  = num(7);  rake  = num(9);  slip   = num(10)
		x_start = num(18); y_start = num(19)

		(isnan(x_start) || isnan(y_start)) &&
			error("no fault trace found — draw a fault first, then Compute")
		for (nm, val) in (("Length", L), ("Width", W), ("Strike", strike), ("Dip", dip),
		                  ("Depth", depth), ("Rake", rake), ("Slip", slip))
			isnan(val) && error("missing/invalid '$nm'")
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
