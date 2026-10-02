# :gui scenario for Tools > "Sound velocity editor (mbvelocitytool)" (deps/src/mbvelocitytool/:
# MB-System's mbvelocitytool, ported). Opens the REAL tool on a copy of MB-System's own test file,
# edits the profile with the mouse the way a user does, raytraces, writes the mbprocess files, quits,
# and reopens the saved profile: the engine, the MBIO glue (the swath editor's run-time loader plus
# this tool's own entry points, both the 5.7 and the 5.8 ABI), the Levitus port and the window on one path.
#
# Needs MB-System's MBIO library (GMT's mbio_w64.dll on the PATH, or ENV["INTERACTIVEGMT_MBIO"])
# and the test swath file (MB-System's test/utilities/testdata/mb11, or
# ENV["INTERACTIVEGMT_MBEDIT_TESTFILE"]). Without either the item is skipped, visibly: CI has neither.
# Opt in with INTERACTIVEGMT_TEST_GUI=1 (or `Pkg.test(test_args=["gui"])`).

@testitem "mbvelocitytool: raytrace a swath file, edit the profile, save for mbprocess, reopen" tags=[:gui] begin
	IG = InteractiveGMT
	Libdl = Base.Libc.Libdl
	# a format WITH travel times: without them mbvelocitytool raises its "travel times estimated"
	# warning dialog, and a test must never wait on a dialog
	src = get(ENV, "INTERACTIVEGMT_MBVELOCITY_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb57\TN136HS.309.snipped.mb57")
	# The same places the tool looks, checked HERE first: with no library the tool would ask for
	# one in a modal dialog, and a test must never wait on a dialog.
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	isempty(mbio) && (mbio = Libdl.find_library(["mbio_w64", "mbio", "libmbio"]))
	# EXPERIMENTAL: the tool is switched in and out (IGMT_WITH_MBVELOCITYTOOL in deps/CMakeLists.txt,
	# the include line in InteractiveGMT.jl); with either half off there is nothing to test.
	if !isdefined(IG, :mbvelocitytool) || !haskey(IG._LIB_FNS, :gmtvtk_mbvelocity_open)
		@test_skip "the experimental sound velocity tool is not built into this library / package"
	elseif isempty(mbio) || !isfile(src)
		@test_skip "MB-System MBIO library or the mb11 test file not available"
	else
		pump() = for _ in 1:10; sleep(0.05); end
		mktempdir() do d
			f = joinpath(d, basename(src))
			cp(src, f)
			try
				# open: the pings raytraced through the default editable profile (6 nodes)
				@test mbvelocitytool(f)
				pump()
				s = IG._mbvelocity_state()
				@test s.open && s.nbuffer > 0
				@test s.edit && s.nedit == 6
				@test s.nbeams > 0                      # residuals were computed
				@test s.ndisplay >= 1                   # the Levitus profile at the first fix
				png = joinpath(d, "view.png")
				@test IG._mbvelocity_save_png(png) && filesize(png) > 2000

				# middle button inside the profile box: a node is added there
				x0, x1, y0, y1 = IG._mbvelocity_profile_box()
				cx, cy = (x0 + x1) ÷ 2, (y0 + y1) ÷ 2
				@test IG._mbvelocity_mouse(2, cx, cy)
				@test IG._mbvelocity_state().nedit == 7
				# find it: the node whose depth lies between its neighbours' at the box middle
				nodes = [IG._mbvelocity_edit_node(i) for i in 0:6]
				@test all(!isnothing, nodes)
				@test issorted(first.(nodes))
				# left button drag 20 px to the right: that node is faster afterwards
				before = nodes
				@test IG._mbvelocity_mouse(1, cx, cy, cx + 20, cy)
				after = [IG._mbvelocity_edit_node(i) for i in 0:6]
				moved = findall(i -> after[i] != before[i], 1:7)
				@test length(moved) == 1 && last(after[moved[1]]) > last(before[moved[1]])
				# right button on it: deleted again
				@test IG._mbvelocity_mouse(3, cx + 20, cy)
				@test IG._mbvelocity_state().nedit == 6

				@test IG._mbvelocity_reprocess()
				@test IG._mbvelocity_state().nbeams > 0

				# the files mbprocess reads
				@test IG._mbvelocity_save_swath_svp()
				@test isfile(f * ".svp")
				@test IG._mbvelocity_save_residuals()
				@test isfile(f * ".sbo") && isfile(f * ".sbao")
				@test isfile(f * ".par")
				par = read(f * ".par", String)
				@test occursin("SVPFILE", par) && occursin("STATICFILE", par)

				@test IG._mbvelocity_close()
				pump()
				@test !IG._mbvelocity_state().open

				# reopen with the saved profile as the editable one (mbvelocitytool -W)
				@test mbvelocitytool(; edit_svp=f * ".svp")
				pump()
				s = IG._mbvelocity_state()
				@test s.open && s.edit && s.nedit == 6
			finally
				IG._mbvelocity_close()
				pump()
			end
		end
	end
end
