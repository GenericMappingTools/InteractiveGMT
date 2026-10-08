# :gui scenario for Geophysics > MB-System > "MBnavedit" (deps/src/mbnavedit/: MB-System's mbnavedit,
# ported). Opens the REAL editor on a copy of MB-System's own test file, picks a fix with the mouse the
# way a user does, switches mode with a key, interpolates, quits (the .nve and the mbprocess parameter
# file are written), and reopens on the edited navigation: the engine, the MBIO glue (the swath editor's
# run-time loader plus this tool's own entry points) and the window on one path.
#
# Needs MB-System's MBIO library (ENV["INTERACTIVEGMT_MBIO"], or on the PATH) exporting mb_pr_update_nav,
# and the test swath file (MB-System's test/utilities/testdata/mb57, or ENV["INTERACTIVEGMT_MBNAVEDIT_TESTFILE"]).
# Without either the item is skipped, visibly: CI has neither.
# Opt in with INTERACTIVEGMT_TEST_GUI=1 (or `Pkg.test(test_args=["gui"])`).

@testitem "mbnavedit: open a swath file, pick and interpolate a fix, write the .nve, reopen" tags=[:gui] begin
	IG = InteractiveGMT
	Libdl = Base.Libc.Libdl
	src = get(ENV, "INTERACTIVEGMT_MBNAVEDIT_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb57\TN136HS.309.snipped.mb57")
	# The same places the tool looks, checked HERE first: with no library the tool would ask for
	# one in a modal dialog, and a test must never wait on a dialog.
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	isempty(mbio) && (mbio = Libdl.find_library(["mbio_w64", "mbio", "libmbio"]))
	# EXPERIMENTAL: the tool is switched in and out (IGMT_WITH_MBNAVEDIT in deps/CMakeLists.txt, the
	# include line in InteractiveGMT.jl); with either half off there is nothing to test.
	if !isdefined(IG, :mbnavedit) || !haskey(IG._LIB_FNS, :gmtvtk_mbnavedit_open)
		@test_skip "the experimental navigation editor is not built into this library / package"
	elseif isempty(mbio) || !isfile(src)
		@test_skip "MB-System MBIO library or the mb57 test file not available"
	else
		pump() = for _ in 1:10; sleep(0.05); end
		# How many records the file really holds: MB-System's own mbinfo summary beside it says so (the
		# mb57 test file is "snipped" to 2). The navigation read must load every one of them — never a
		# hard-coded count written for some other, bigger file.
		inf  = src * ".inf"
		m    = isfile(inf) ? match(r"Number of Records:\s+(\d+)", read(inf, String)) : nothing
		nrec = m === nothing ? 0 : parse(Int, m[1])
		allread(n) = n >= 1 && (nrec == 0 || n == nrec)
		mktempdir() do d
			f = joinpath(d, basename(src))
			cp(src, f)
			try
				# open: the navigation read, the six default plots laid out
				@test mbnavedit(f; use_previous=0)
				pump()
				s = IG._mbnavedit_state()
				@test s.open && s.file_open && s.numfiles == 1 && s.currentfile == 0
				@test allread(s.nbuff) && s.nplot > 0
				@test s.number_plots == 6
				png = joinpath(d, "view.png")
				@test IG._mbnavedit_save_png(png) && filesize(png) > 2000

				# Pick mode (the default): the left button toggles the fix nearest the pointer, in the
				# plot it is pressed in -- the longitude plot is the second one
				box = IG._mbnavedit_plot_box(1)
				@test box !== nothing && box.type == 1
				i = s.current_id + s.nplot ÷ 2
				xy = IG._mbnavedit_record_xy(1, i)
				@test xy !== nothing
				@test IG._mbnavedit_mouse(1, xy[1], xy[2])
				r = IG._mbnavedit_record(i)
				@test r !== nothing && (r.selected & 2) != 0          # selected in the longitude plot
				# Interpolate: the selected longitude is replaced from its neighbours, and stays finite
				@test IG._mbnavedit_press("pushButton_interpolate")
				r2 = IG._mbnavedit_record(i)
				@test r2 !== nothing && isfinite(r2.lon)
				# Revert puts the original value back
				@test IG._mbnavedit_press("pushButton_revert")
				@test IG._mbnavedit_record(i).lon == r.lon

				# the mode keys: U = Select, Q = Pick
				@test IG._mbnavedit_key('u')
				@test IG._mbnavedit_state().mode_pick == 1
				@test IG._mbnavedit_key('q')
				@test IG._mbnavedit_state().mode_pick == 0

				# stepping through the buffer and back
				@test IG._mbnavedit_press("pushButton_end")
				@test IG._mbnavedit_press("pushButton_start")
				@test IG._mbnavedit_state().current_id == 0

				# Quit: the edited navigation and the mbprocess parameters are written
				@test IG._mbnavedit_close()
				pump()
				@test !IG._mbnavedit_state().open
				@test isfile(f * ".nve") && filesize(f * ".nve") > 0
				@test isfile(f * ".par")
				@test occursin("NAVFILE", read(f * ".par", String))

				# reopen on the edited navigation (the "use previously edited navigation" answer: yes)
				@test mbnavedit(f; use_previous=1)
				pump()
				s = IG._mbnavedit_state()
				@test s.open && s.file_open && allread(s.nbuff)
			finally
				IG._mbnavedit_close()
				pump()
			end
		end
	end
end
