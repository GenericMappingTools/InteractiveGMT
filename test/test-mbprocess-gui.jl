# :gui scenario for Geophysics > MB-System > "Process swath data (mbprocess)" (deps/src/72_mbprocess.cpp,
# src/mbprocess.jl). Opens the REAL dialog on a copy of MB-System's own test file and presses its
# buttons: the .par is made (mbset -L), mbprocess writes the processed file, the edits switch is
# written into the .par, and a datalist is expanded by MBIO -- MB-System's mbset / mbprocess run in
# this process through GMT's MB-System supplement, their report in the dialog's log.
#
# Needs GMT with the MB-System supplement (mbprocess / mbset modules) and the mb11 test file
# (ENV["INTERACTIVEGMT_MBEDIT_TESTFILE"]); without either the item is skipped, visibly.
# Opt in with INTERACTIVEGMT_TEST_GUI=1 (or `Pkg.test(test_args=["gui"])`).

@testitem "mbprocess: the dialog parks in its window and comes back" tags=[:gui] begin
	IG = InteractiveGMT
	if !haskey(IG._LIB_FNS, :gmtvtk_mbprocess_open_h)
		@test_skip "this library was built without the mbprocess dialog"
	else
		pump() = for _ in 1:10; sleep(0.05); end
		G = IG.GMT.mat2grid(Float32[i + j for j in 1:20, i in 1:20], x = collect(1.0:20.0), y = collect(1.0:20.0))
		fig = IG.view_grid(G; title = "mbprocess parking")
		pump()
		try
			@test mbprocess_dialog(; fig)
			pump()
			@test IG._mbprocess_parked(fig) == 0
			IG._mbprocess_do(6)                    # the X: parks, not closes
			pump()
			@test IG._mbprocess_parked(fig) == 1
			@test mbprocess_dialog(; fig)          # the menu again: back, and its row gone
			pump()
			@test IG._mbprocess_parked(fig) == 0
			IG._mbprocess_do(6)
			pump()
			@test IG._mbprocess_parked(fig) == 1
		finally
			IG._mbprocess_do(5)                    # Delete: gone for good, row and all
			pump()
		end
		@test IG._mbprocess_parked(fig) == -1
	end
end

@testitem "mbprocess: make the .par, process a file, switch the edits, expand a datalist" tags=[:gui] begin
	IG = InteractiveGMT
	src = get(ENV, "INTERACTIVEGMT_MBEDIT_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb11\TN136HS.309.snipped.mb11")
	api = InteractiveGMT.GMT.G_API[]
	hasmb = api != C_NULL &&
	        InteractiveGMT.GMT.GMT_Call_Module(api, "mbprocess", InteractiveGMT.GMT.GMT_MODULE_EXIST, C_NULL) == 0
	if !haskey(IG._LIB_FNS, :gmtvtk_mbprocess_open)
		@test_skip "this library was built without the mbprocess dialog"
	elseif !hasmb || !isfile(src)
		@test_skip "GMT has no MB-System supplement, or the mb11 test file is not available"
	else
		pump() = for _ in 1:10; sleep(0.05); end
		mktempdir() do d
			f = joinpath(d, basename(src))
			cp(src, f)
			fp = joinpath(d, "TN136HS.309.snippedp.mb11")         # mbprocess's default output name
			try
				@test mbprocess_dialog(f)
				pump()
				@test IG._mbprocess_do(-1) == 1                      # one file listed
				@test !isfile(f * ".par")

				# Process: the .par is made first (mbset -L), then the file is processed
				@test IG._mbprocess_do(1) == 1
				@test isfile(f * ".par")
				@test isfile(fp)
				log = IG._mbprocess_log()
				@test occursin("Data processed", log)
				@test occursin("1 of 1 file(s) done", log)

				# the edits switch goes into the .par (Save .par = mbset only)
				@test IG._mbprocess_do(3) == 1
				@test IG._mbprocess_do(2) == 1
				par = IG._mbp_read_par(f * ".par")
				@test par["EDITSAVEMODE"] == "1"
				@test endswith(par["EDITSAVEFILE"], basename(f) * ".esf")
				@test IG._mbprocess_do(4) == 1
				@test IG._mbprocess_do(2) == 1
				@test IG._mbp_read_par(f * ".par")["EDITSAVEMODE"] == "0"

				# the other tabs: only the keys changed are written, by their mbset names
				# (keys that still let the file be processed below: no tide / SVP / nav file is needed)
				@test IG._mbprocess_set_par("TIDEFORMAT", "2")         # a combo whose values start at 1
				@test IG._mbprocess_set_par("ROLLBIASMODE", "1")
				@test IG._mbprocess_set_par("ROLLBIAS", "0.5")
				@test IG._mbprocess_set_par("HEADINGMODE", "2")
				@test !IG._mbprocess_set_par("NOSUCHKEY", "1")
				@test IG._mbprocess_do(2) == 1
				par = IG._mbp_read_par(f * ".par")
				@test par["TIDEFORMAT"] == "2" && par["HEADINGMODE"] == "2"
				@test par["ROLLBIASMODE"] == "1" && parse(Float64, par["ROLLBIAS"]) == 0.5
				@test par["EDITSAVEMODE"] == "0" && par["DRAFTMODE"] == "0"   # untouched keys stay
				@test IG._mbprocess_do(5) == 0

				# a datalist: MBIO lists its file, and the dialog processes it
				dl = joinpath(d, "datalist.mb-1")
				write(dl, basename(f) * " 11\n")
				@test mbprocess_dialog(dl)
				pump()
				@test IG._mbprocess_do(-1) == 1
				@test IG._mbprocess_do(1) == 1
				@test occursin("1 of 1 file(s) done", IG._mbprocess_log())

				# a request that fails says why
				@test_throws "No such file" IG._mbp_request(Dict("what" => "files",
				                                                 "input" => joinpath(d, "nope.mb11")))
			finally
				IG._mbprocess_do(5)
				pump()
			end
		end
	end
end
