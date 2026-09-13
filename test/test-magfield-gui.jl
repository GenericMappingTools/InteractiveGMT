# :gui scenario for Geophysics > Magnetics > "Magnetic field lines (3-D)" (69_magfield.cpp).
# Opens the REAL window through the menu the user clicks, then reads the dialog's own state back
# through gmtvtk_magfield_test: a QUiLoader failure, a widget name that drifted out of
# magfield3d.ui, or a tracer that hands back nothing fails HERE, not on the user's first click.
# Opt in with INTERACTIVEGMT_TEST_GUI=1 (or `Pkg.test(test_args=["gui"])`).

@testitem "magfield: the menu opens a globe with traced field lines on it" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	e = iview()
	state(control = "", value = 0.0) = begin
		out = zeros(8)
		ok = ccall(_test_fn(:gmtvtk_magfield_test), Cint,
			(Ptr{Cvoid}, Cstring, Cdouble, Ptr{Cdouble}), e.h, control, value, out)
		@test ok == 1
		out
	end
	try
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring),
			e.h, "Magnetics/Magnetic field lines (3-D)") == 2
		s = state()
		# The .ui's own defaults: 12 longitudes x 4 rings x 2 hemispheres, date 2025.
		@test s[1] == 96
		@test s[2] > 96
		@test s[3] == 1.0          # the tubes are on screen
		@test s[4] == 1.0          # the Earth picture is on the sphere
		@test s[5] == 1.0          # ...and they are coloured by |B|
		@test s[6] == 2025.0
		@test s[8] > 0.0           # the camera was framed on the model
		# The two look boxes act at once (they repaint, they do not compute).
		@test state("earth", 0.0)[4] == 0.0
		@test state("earth", 1.0)[4] == 1.0
		@test state("color", 0.0)[5] == 0.0
		@test state("color", 1.0)[5] == 1.0
		# A date change alone changes NOTHING until Compute is pressed: only the action button runs.
		before = state()
		@test state("date", 1900.0)[1] == before[1]
		@test state("lon", 4.0)[1] == before[1]
		after = state("compute")
		@test after[1] == 32       # 4 longitudes x 4 rings x 2 hemispheres
		@test after[6] == 1900.0
		@test after[2] > 32
		# The tube radius is a drawn-thickness control, and it reaches the filter on the next Compute.
		state("tube", 0.02)
		@test isapprox(state("compute")[7], 0.02; atol = 1e-9)
		state("reset")
	finally
		state("close")
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), e.h)
	end
end
