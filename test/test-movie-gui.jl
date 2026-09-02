# :gui scenario for Tools > Make movie. Opens the REAL dialog through the built gmtvtk_test.dll, so a
# QUiLoader failure or a widget name that drifted out of deps/ui/movie_dialog.ui fails here rather
# than on the user's first click. Opt in with INTERACTIVEGMT_TEST_GUI=1 (or Pkg.test(test_args=["gui"])).

@testitem "Make movie dialog: the X parks it, it does not kill it" tags=[:gui, :movie] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = view_grid(IG.GMT.peaks())

	# Opening it twice must yield ONE dialog: the second call unparks/raises the first.
	@test ccall(_test_fn(:gmtvtk_movie_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test ccall(_test_fn(:gmtvtk_movie_parked_test), Cint, (Ptr{Cvoid},), f.h) == 0
	@test ccall(_test_fn(:gmtvtk_movie_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test ccall(_test_fn(:gmtvtk_movie_parked_test), Cint, (Ptr{Cvoid},), f.h) == 0

	# The X PARKS it: hidden, but carrying a row in this window's Scene Objects dock.
	ccall(_test_fn(:gmtvtk_movie_close_dialog_test), Cvoid, (Ptr{Cvoid},), f.h)
	@test ccall(_test_fn(:gmtvtk_movie_parked_test), Cint, (Ptr{Cvoid},), f.h) == 1
	rows = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	@test occursin("Make movie", rows)

	# Re-opening brings THAT dialog back rather than building a second one.
	@test ccall(_test_fn(:gmtvtk_movie_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test ccall(_test_fn(:gmtvtk_movie_parked_test), Cint, (Ptr{Cvoid},), f.h) == 0

	# Only the parked row's own "Delete" ends it -- and then the row goes with it.
	@test ccall(_test_fn(:gmtvtk_movie_delete_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test ccall(_test_fn(:gmtvtk_movie_parked_test), Cint, (Ptr{Cvoid},), f.h) == -1
	rows2 = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	@test !occursin("Make movie", rows2)
end
