# :gui scenario for Tools > "Swath editor (mbedit)" (deps/src/mbedit/: MB-System's mbedit, ported).
# Opens the REAL editor on a copy of MB-System's own test file, edits it through the same key
# macros a user types, quits, and reopens it from the edit save file: the engine, the MBIO glue
# (run-time loaded, both the 5.7 and the 5.8 ABI), the ESF round trip and the window all on one path.
#
# Needs MB-System's MBIO library (GMT's mbio_w64.dll on the PATH, or ENV["INTERACTIVEGMT_MBIO"])
# and the test swath file (MB-System's test/utilities/testdata/mb11, or
# ENV["INTERACTIVEGMT_MBEDIT_TESTFILE"]). Without either the item is skipped, visibly: CI has neither.
# Opt in with INTERACTIVEGMT_TEST_GUI=1 (or `Pkg.test(test_args=["gui"])`).

@testitem "mbedit: open a swath file, flag the view, quit, reopen from the edit save file" tags=[:gui] begin
	IG = InteractiveGMT
	Libdl = Base.Libc.Libdl
	src = get(ENV, "INTERACTIVEGMT_MBEDIT_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb11\TN136HS.309.snipped.mb11")
	# The same places the editor looks, checked HERE first: with no library the editor would ask
	# for one in a modal dialog, and a test must never wait on a dialog.
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	isempty(mbio) && (mbio = Libdl.find_library(["mbio_w64", "mbio", "libmbio"]))
	# EXPERIMENTAL: the editor is switched in and out (IGMT_WITH_MBEDIT in deps/CMakeLists.txt, the
	# include line in InteractiveGMT.jl); with either half off there is nothing to test.
	if !isdefined(IG, :mbedit) || !haskey(IG._LIB_FNS, :gmtvtk_mbedit_open)
		@test_skip "the experimental swath editor is not built into this library / package"
	elseif isempty(mbio) || !isfile(src)
		@test_skip "MB-System MBIO library or the mb11 test file not available"
	else
		pump() = for _ in 1:10; sleep(0.05); end
		mktempdir() do d
			f = joinpath(d, basename(src))
			cp(src, f)
			try
				# open: the file's 2 pings x 16 beams, nothing flagged
				@test mbedit(f; format=11, esf=false)
				pump()
				s = IG._mbedit_state()
				@test s.open && s.numfiles == 1 && s.currentfile == 0
				@test s.nbuffer == 2 && s.nplot == 2
				@test s.nflagged == 0 && s.nunflagged == 32
				# the canvas really was drawn (not left white)
				png = joinpath(d, "view.png")
				@test IG._mbedit_save_png(png) && filesize(png) > 2000

				# 'x' = Flag View (mbedit's key macro): every sounding on view flagged
				@test IG._mbedit_key('x')
				s = IG._mbedit_state()
				@test s.nflagged == 32 && s.nunflagged == 0
				# 'f' = Forward by "Pings to step"
				@test IG._mbedit_key('f')
				@test IG._mbedit_state().icurrent == 1

				# Quit saves: the edit save file and the mbprocess parameter file are written
				@test IG._mbedit_close()
				pump()
				@test !IG._mbedit_state().open
				@test isfile(f * ".esf")
				@test isfile(f * ".par")

				# reopen applying the saved edits (mbedit -S): the flags are back
				@test mbedit(f; format=11, esf=true)
				pump()
				s = IG._mbedit_state()
				@test s.nflagged == 32 && s.nunflagged == 0
				# 'c' = Unflag View
				@test IG._mbedit_key('c')
				@test IG._mbedit_state().nflagged == 0
			finally
				IG._mbedit_close()
				pump()
			end
		end
	end
end
