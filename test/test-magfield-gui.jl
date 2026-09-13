# :gui scenario for Geophysics > Magnetics > "Magnetic field lines (3-D)" (69_magfield.cpp).
# Opens the REAL window through the menu the user clicks, then reads the dialog's own state back
# through gmtvtk_magfield_test: a QUiLoader failure, a widget name that drifted out of
# magfield3d.ui, or a tracer that hands back nothing fails HERE, not on the user's first click.
# Opt in with INTERACTIVEGMT_TEST_GUI=1 (or `Pkg.test(test_args=["gui"])`).

@testitem "magfield: the menu opens a globe with traced field lines on it" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	e = iview()
	state(control = "", value = 0.0) = begin
		out = zeros(14)
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
		@test s[9] == 0.0          # the sphere opens wearing the Earth image
		@test s[10] == 1.0         # ...and the dip-pole markers are up
		# The dip poles: north in arctic Canada / Siberia, south off Adélie Land. Both well inside
		# their own polar cap, and NOT antipodal -- which is the whole point of computing both.
		@test s[12] > 60.0 && s[14] < -60.0
		@test abs(abs(s[11] - s[13]) - 180.0) > 5.0
		# The look controls act at once (they repaint, they do not re-trace).
		@test state("poles", 0.0)[10] == 0.0
		@test state("poles", 1.0)[10] == 1.0
		@test state("color", 0.0)[5] == 0.0
		@test state("color", 1.0)[5] == 1.0
		# Every sphere skin: the Earth image, the IGRF total field built from the date box, and none.
		@test state("skin", 2.0)[4] == 0.0          # plain: no texture on the globe
		igrf = state("skin", 1.0)
		@test igrf[9] == 1.0 && igrf[4] == 1.0      # the IGRF field image really got built and hung
		@test state("skin", 0.0)[4] == 1.0
		# A date change alone changes NOTHING until Compute is pressed: only the action button runs.
		before = state()
		@test state("date", 1900.0)[1] == before[1]
		@test state("lon", 4.0)[1] == before[1]
		after = state("compute")
		@test after[1] == 32       # 4 longitudes x 4 rings x 2 hemispheres
		@test after[6] == 1900.0
		@test after[2] > 32
		# The poles ARE the date: 125 years of secular variation moved them, and Compute re-derived
		# them rather than leaving 2025's markers on a 1900 field.
		@test (after[11], after[12]) != (before[11], before[12])
		@test (after[13], after[14]) != (before[13], before[14])
		@test after[12] > 60.0 && after[14] < -60.0
		# The tube radius is a drawn-thickness control, and it reaches the filter on the next Compute.
		state("tube", 0.02)
		@test isapprox(state("compute")[7], 0.02; atol = 1e-9)
		state("reset")
	finally
		state("close")
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), e.h)
	end
end
