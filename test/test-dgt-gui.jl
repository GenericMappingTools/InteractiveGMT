# :gui scenario for PT Tools > DGT LIDAR. Opens the REAL dialog (and its map picker) through the built
# gmtvtk_test.dll. The account check (begin()) is not part of it: it talks to the portal over the
# network, and parking has nothing to do with that. Opt in with INTERACTIVEGMT_TEST_GUI=1.

@testitem "DGT LIDAR: X, minimise, Esc and Use this region park; Delete ends dialog and map" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = view_grid(IG.GMT.peaks())
	opened() = ccall(_test_fn(:gmtvtk_dgt_open_dialog_test), Cint, (Ptr{Cvoid},), f.h)
	parked() = ccall(_test_fn(:gmtvtk_dgt_parked_test), Cint, (Ptr{Cvoid},), f.h)
	leave(how) = ccall(_test_fn(:gmtvtk_dgt_leave_dialog_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, how)
	rows() = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	try
		@test opened() == 1 && parked() == 0
		@test opened() == 1 && parked() == 0            # the same dialog, not a second one
		for how in (0, 1, 2)                            # the X, minimise, Esc
			leave(how)
			@test parked() == 1
			@test occursin("DGT LIDAR", rows())
			@test opened() == 1 && parked() == 0
		end
		# "Use this region" PARKS the map (it used to destroy it) and brings the dialog back.
		@test ccall(_test_fn(:gmtvtk_dgt_use_region_test), Cint, (Ptr{Cvoid},), f.h) == 1
		@test occursin("Pick region", rows())
		# The dialog's own Delete ends it, and takes its parked map with it.
		leave(0)
		@test ccall(_test_fn(:gmtvtk_dgt_delete_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
		@test parked() == -1
		@test !occursin("DGT LIDAR", rows()) && !occursin("Pick region", rows())
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end
