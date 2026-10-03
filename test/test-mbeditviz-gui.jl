# :gui scenario for Tools > "Bathymetry editor and patch test (mbeditviz)" (deps/src/mbeditviz/: MB-System's
# mbeditviz, ported). Opens the REAL tool on a copy of MB-System's own test file, loads and grids it onto the survey
# map, picks a region into the 3-D sounding editor, flags the view with the key macro, closes the map (which writes
# the edit save file, as mbeditviz does) and reopens the file to find the edits applied: the engine, the MBIO glue
# (the swath editor's run-time loader plus this tool's 5.8 entry points), the survey map and the editor on one path.
#
# Needs MB-System 5.8.x's MBIO library (ENV["INTERACTIVEGMT_MBIO"], e.g. the take2 build's lib/mbio.dll) and the test
# swath file with its .inf (MB-System's test/utilities/testdata/mb57, or ENV["INTERACTIVEGMT_MBEDITVIZ_TESTFILE"]).
# Without either the item is skipped, visibly: CI has neither. Opt in with INTERACTIVEGMT_TEST_GUI=1.

@testitem "mbeditviz: grid a swath file, edit a region in 3-D, the edits reach the edit save file" tags=[:gui] begin
	IG = InteractiveGMT
	src = get(ENV, "INTERACTIVEGMT_MBEDITVIZ_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb57\TN136HS.309.snipped.mb57")
	# mbeditviz needs 5.8.x: the MBIO named explicitly (a 5.7 library on the PATH would be refused in a dialog,
	# and a test must never wait on a dialog)
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	if !isdefined(IG, :mbeditviz) || !haskey(IG._LIB_FNS, :gmtvtk_mbeditviz_open)
		@test_skip "the experimental mbeditviz is not built into this library / package"
	elseif isempty(mbio) || !isfile(mbio) || !isfile(src) || !isfile(src * ".inf")
		@test_skip "MB-System 5.8 MBIO library (INTERACTIVEGMT_MBIO) or the mb57 test file (+ .inf) not available"
	else
		pump() = for _ in 1:10; sleep(0.05); end
		mktempdir() do d
			f = joinpath(d, basename(src))
			cp(src, f)
			cp(src * ".inf", f * ".inf")
			try
				@test mbeditviz(f; format=57)
				pump()
				s = IG._mbeditviz_state()
				@test s.open && s.numfiles == 1 && s.gridstatus == 0

				# View All + Apply (the suggested cell size): the file is loaded and gridded onto the survey map
				@test IG._mbeditviz_view_all()
				pump()
				s = IG._mbeditviz_state()
				@test s.numloaded == 1 && s.gridstatus == 2
				@test s.nx > 1 && s.ny > 1

				# a region covering everything: every usable sounding into the 3-D editor
				@test IG._mbeditviz_select_box(-1e9, 1e9, -1e9, 1e9)
				pump()
				s = IG._mbeditviz_state()
				@test s.editor && s.nselected > 0
				n = s.nselected
				png = joinpath(d, "soundings.png")
				@test IG._mbeditviz_editor_save_png(png) && filesize(png) > 2000

				# ',' = Flag View (the key macro; its 'x' is the gizmo's, as in every iGMT view): every good
				# sounding on view flagged
				@test IG._mbeditviz_editor_key(',')
				s = IG._mbeditviz_state()
				@test s.nflagged == n

				# closing the survey map ends the grid and writes the edits (mbeditviz_destroy_grid)
				@test IG._mbeditviz_close_map()
				pump()
				s = IG._mbeditviz_state()
				@test s.gridstatus == 0 && !s.editor
				@test isfile(f * ".esf")
				@test isfile(f * ".par")

				# Quit, reopen, grid again: the saved edits are applied on load
				@test IG._mbeditviz_close()
				pump()
				@test !IG._mbeditviz_state().open
				@test mbeditviz(f; format=57)
				pump()
				@test IG._mbeditviz_view_all()
				pump()
				@test IG._mbeditviz_select_box(-1e9, 1e9, -1e9, 1e9)
				s = IG._mbeditviz_state()
				@test s.nselected == n && s.nflagged == n
			finally
				IG._mbeditviz_close()
				pump()
			end
		end
	end
end
