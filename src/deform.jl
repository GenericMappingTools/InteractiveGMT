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
