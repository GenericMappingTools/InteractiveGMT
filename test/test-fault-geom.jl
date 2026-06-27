# Regression test for the fault-trace endpoint recompute (Vertical elastic deformation dialog's
# Strike/Length edits, port of Mirone deform_mansinha.m edit_Fault*_CB). Opens a REAL Qt+VTK window,
# injects a fault line, then drives the SAME core the dialog calls (faultApplyGeom, via the
# gmtvtk_fault_*_test hooks) and asserts the trace's end vertex lands on the direct geodesic (geog,
# GMT.geod) or the trig endpoint (cart). Tagged :gui — needs the DLL + a display, runs only with
# INTERACTIVEGMT_TEST_GUI=1.

@testitem "fault endpoint: Strike/Length move the trace to the geodesic endpoint" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	e = iview()
	try
		IG._register_faultgeom()                          # geog path solves the endpoint in Julia (geod)
		IG._pump_once()

		# Inject a 2-vertex fault starting at (lon1,lat1).
		lon1, lat1 = -10.0, 38.0
		n = ccall(IG._fn(:gmtvtk_fault_add_test), Cint,
		          (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble), e.h, lon1, lat1, -9.0, 38.0)
		@test n == 1

		# Geographic: end vertex must be the direct-geodesic destination for (strike, len km).
		out = zeros(Float64, 2); strike, len = 35.0, 80.0
		nv = ccall(IG._fn(:gmtvtk_fault_apply_test), Cint,
		           (Ptr{Cvoid}, Cdouble, Cdouble, Cint, Ptr{Cdouble}), e.h, strike, len, 1, out)
		@test nv == 2                                      # collapses to a clean 2-vertex segment
		dest, = GMT.geod([lon1, lat1], strike, len; unit=:km)
		@test abs(out[1] - dest[1]) < 1e-9
		@test abs(out[2] - dest[2]) < 1e-9
		d, az, = GMT.invgeod([lon1, lat1], [out[1], out[2]])   # round-trips back to the inputs
		@test abs(d - len*1000) < 1e-3
		@test abs(az - strike)  < 1e-6

		# Cartesian: endpoint = start + len*[sin,cos](azimuth-from-north). az=90 -> due "east".
		nc = ccall(IG._fn(:gmtvtk_fault_apply_test), Cint,
		           (Ptr{Cvoid}, Cdouble, Cdouble, Cint, Ptr{Cdouble}), e.h, 90.0, 5.0, 0, out)
		@test nc == 2
		@test abs(out[1] - (lon1 + 5.0)) < 1e-9
		@test abs(out[2] - lat1)         < 1e-9
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), e.h)
	end
end
