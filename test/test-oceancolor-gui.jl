# GUI-tier tests for Tools/Geophysics > Oceancolor (the OceanColorDialog in 70_window.cpp, loaded
# from deps/ui/oceancolor_browser.ui). No network: the test DLL never registers the Julia callback,
# so every request the dialog issues answers "not wired" and returns at once — which is exactly what
# these tests want to exercise, the WIDGETS and the parking, not the archive (that is the :net
# testitem in test-oceancolor-unit.jl).

@testitem "Ocean Color browser: the .ui loads and its combos drive it" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = IG.view_grid(IG.GMT.peaks(32))

	# A QUiLoader failure or a widget renamed out of the .ui shows up right here.
	@test ccall(_test_fn(:gmtvtk_oc_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test ccall(_test_fn(:gmtvtk_oc_parked_test), Cint, (Ptr{Cvoid},), f.h) == 0

	buf = Vector{UInt8}(undef, 1024)
	ccall(_test_fn(:gmtvtk_oc_state_test), Cint, (Ptr{UInt8}, Cint), buf, Cint(1024))
	st = split(unsafe_string(pointer(buf)), '\n')
	@test length(st) == 4
	@test st[1] == "1"				# the NEWER tile is the one Extract starts on
	# Instrument and Product open on the .ui's own defaults (Aqua-MODIS / SST). The PERIOD does NOT: it
	# is remembered across openings and sessions (prefs key oceancolor/period), so at this point it is
	# whatever was last picked on this machine — pinned down below, after a known pick.
	@test startswith(st[2], "1/1/")

	# Setting the combos runs the SAME handler a click runs.
	@test ccall(_test_fn(:gmtvtk_oc_select_test), Cint, (Cint, Cint, Cint),
	            Cint(3), Cint(2), Cint(2)) == 1
	ccall(_test_fn(:gmtvtk_oc_state_test), Cint, (Ptr{UInt8}, Cint), buf, Cint(1024))
	@test split(unsafe_string(pointer(buf)), '\n')[2] == "3/2/2"

	ccall(_test_fn(:gmtvtk_oc_delete_dialog_test), Cvoid, ())
	@test ccall(_test_fn(:gmtvtk_oc_parked_test), Cint, (Ptr{Cvoid},), f.h) == -1

	# A FRESH dialog comes back on the period just picked (2 = 8-day), while Instrument and Product
	# come back at the .ui defaults — the "remember the last Period" behaviour, end to end.
	@test ccall(_test_fn(:gmtvtk_oc_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	ccall(_test_fn(:gmtvtk_oc_state_test), Cint, (Ptr{UInt8}, Cint), buf, Cint(1024))
	@test split(unsafe_string(pointer(buf)), '\n')[2] == "1/1/2"
	ccall(_test_fn(:gmtvtk_oc_delete_dialog_test), Cvoid, ())
end

@testitem "Ocean Color browser: the X parks it, it does not kill it" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = IG.view_grid(IG.GMT.peaks(32))

	@test ccall(_test_fn(:gmtvtk_oc_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test ccall(_test_fn(:gmtvtk_oc_select_test), Cint, (Cint, Cint, Cint),
	            Cint(2), Cint(2), Cint(3)) == 1
	@test ccall(_test_fn(:gmtvtk_oc_parked_test), Cint, (Ptr{Cvoid},), f.h) == 0

	# The X hides and parks: still alive, now a row in Scene Objects.
	ccall(_test_fn(:gmtvtk_oc_close_dialog_test), Cvoid, ())
	@test ccall(_test_fn(:gmtvtk_oc_parked_test), Cint, (Ptr{Cvoid},), f.h) == 1
	rows = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	@test occursin("Ocean Color", rows)

	# Re-opening brings THAT dialog back — never a second one — still on the selection it was left on,
	# and its parked row is gone again.
	@test ccall(_test_fn(:gmtvtk_oc_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test ccall(_test_fn(:gmtvtk_oc_parked_test), Cint, (Ptr{Cvoid},), f.h) == 0
	buf = Vector{UInt8}(undef, 1024)
	ccall(_test_fn(:gmtvtk_oc_state_test), Cint, (Ptr{UInt8}, Cint), buf, Cint(1024))
	@test split(unsafe_string(pointer(buf)), '\n')[2] == "2/2/3"
	rows = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	@test !occursin("Ocean Color", rows)

	# Only the parked row's Delete really closes it.
	ccall(_test_fn(:gmtvtk_oc_delete_dialog_test), Cvoid, ())
	@test ccall(_test_fn(:gmtvtk_oc_parked_test), Cint, (Ptr{Cvoid},), f.h) == -1
end
