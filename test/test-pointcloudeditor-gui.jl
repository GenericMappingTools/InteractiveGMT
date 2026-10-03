# :gui scenario for Tools > "Point cloud editor (pointCloudEditor)" (deps/src/pointcloudeditor/: MB-System's
# pointCloudEditor, ported). Opens the REAL editor on the grid of an InteractiveGMT window (geographic, so shown in
# UTM through MBIO's projection), ERASEs and RESTOREs points with the rubber band, does it again at another vertical
# exaggeration, draws an elevation profile (-elev), and grids MB-System's own test swath file through the ported
# mbeditviz engine: the scene, the selection, the profile cutter and both readers on one path.
#
# Needs MB-System 5.8's MBIO library (ENV["INTERACTIVEGMT_MBIO"]) and, for the swath half, the mb57 test file with
# its .inf (ENV["INTERACTIVEGMT_PCE_TESTFILE"]). Without the library the item is skipped, visibly: CI has none.
# Opt in with INTERACTIVEGMT_TEST_GUI=1.

@testitem "pointCloudEditor: grid in UTM, rubber-band erase/restore, exaggeration, elevation profile, swath" tags=[:gui] begin
	IG = InteractiveGMT
	using InteractiveGMT.GMT
	src = get(ENV, "INTERACTIVEGMT_PCE_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb57\TN136HS.309.snipped.mb57")
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	if !isdefined(IG, :pointcloudeditor) || !haskey(IG._LIB_FNS, :gmtvtk_pce_open)
		@test_skip "the experimental pointCloudEditor is not built into this library / package"
	elseif isempty(mbio) || !isfile(mbio)
		@test_skip "MB-System MBIO library (INTERACTIVEGMT_MBIO) not available"
	else
		pump() = for _ in 1:10; sleep(0.05); end
		x = collect(range(-124.51, -124.49, length=41))
		y = collect(range(40.83, 40.85, length=31))
		z = Float32[-600.0 - 2000.0 * (xi + 124.51) - 1000.0 * (yj - 40.83) for yj in y, xi in x]
		G = mat2grid(z, x=x, y=y)
		fig = IG.view_grid(G; geographic=true, title="pointCloudEditor test grid")
		pump()
		try
			@test pointcloudeditor(; fig)
			pump()
			s = IG._pce_state()
			@test s.open && s.npoints == 41 * 31 && s.ncells == 2 * 40 * 30
			@test s.nbad == 0 && s.editmode == 0
			w, h = IG._pce_canvas_size()
			@test w > 100 && h > 100

			# ERASE: a band over the middle of the view marks part of the points bad
			@test IG._pce_rubber_band(w ÷ 3, h ÷ 3, 2w ÷ 3, 2h ÷ 3)
			pump()
			s = IG._pce_state()
			@test s.selecting
			@test 0 < s.nbad < s.npoints
			nbad = s.nbad

			# RESTORE over the same band: every one of them good again
			@test IG._pce_set_edit_mode(1)
			@test IG._pce_rubber_band(w ÷ 3, h ÷ 3, 2w ÷ 3, 2h ÷ 3)
			@test IG._pce_state().nbad == 0

			# at 5x exaggeration the band still selects the points it covers
			@test IG._pce_set_edit_mode(0)
			@test IG._pce_set_vertical_exagg(5.0)
			@test IG._pce_rubber_band(w ÷ 3, h ÷ 3, 2w ÷ 3, 2h ÷ 3)
			@test 0 < IG._pce_state().nbad < IG._pce_state().npoints
			@test IG._pce_set_vertical_exagg(1.0)

			# -elev: the band is a profile line across the data
			@test IG._pce_set_elev_profile(true)
			pump()
			w, h = IG._pce_canvas_size()
			@test IG._pce_rubber_band(w ÷ 8, h ÷ 2, 3w ÷ 8, h ÷ 2)
			s = IG._pce_state()
			@test s.elevprofile && s.nprofile > 2
			mktempdir() do d
				png = joinpath(d, "pce.png")
				@test IG._pce_save_png(png) && filesize(png) > 2000
			end
			@test IG._pce_set_elev_profile(false)

			# a swath file, gridded by the mbeditviz engine
			if isfile(src) && isfile(src * ".inf")
				mktempdir() do d
					f = joinpath(d, basename(src))
					cp(src, f)
					cp(src * ".inf", f * ".inf")
					@test pointcloudeditor(f)
					pump()
					s = IG._pce_state()
					@test s.npoints > 100 && s.ncells > 0 && s.nbad == 0
					@test !isfile(f * ".esf")      # browse mode: nothing written beside the data
				end
			else
				@test_skip "the mb57 test file (+ .inf) is not available"
			end
		finally
			IG._pce_close()
			pump()
		end
		@test !IG._pce_state().open
	end
end
