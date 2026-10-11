# :gui scenario for Geophysics > MB-System > "MBnavadjust" (deps/src/mbnavadjust/: MB-System's mbnavadjust,
# ported). Opens the REAL tool, makes a project of copies of three crossing swath lines, imports them (the
# sections, the crossings, the project grid through MB-System's own mbdatalist and mbgrid), opens the first
# crossing in the analysis window and adds a tie, inverts, and shows the model plot and the visualization:
# the core, the engine, the Motif callbacks on their widget bridge, the canvases and the host's MB program
# runner and grid reader on one path.
#
# Needs MB-System's MBIO library (ENV["INTERACTIVEGMT_MBIO"], or on the PATH) and three crossing swath lines
# (ENV["INTERACTIVEGMT_MBNAVADJUST_TESTDATA"], a folder of *.mb59 files: the first three are used; by default
# the EM122 patch test of C:\j\OT). They are COPIED: nothing is written beside the originals. The project grid
# needs MB-System's GMT supplement (mbgrid); without it that one check is skipped, visibly.
# Opt in with INTERACTIVEGMT_TEST_GUI=1 (or `Pkg.test(test_args=["gui"])`).

@testitem "mbnavadjust: new project, import crossing lines, add a tie, invert, model plot, visualize" tags=[:gui] begin
	IG = InteractiveGMT
	Libdl = Base.Libc.Libdl
	srcdir = get(ENV, "INTERACTIVEGMT_MBNAVADJUST_TESTDATA", raw"C:\j\OT\PatchSurvezEM122PHINS")
	srcs = isdir(srcdir) ? sort(filter(f -> occursin(r"^\d{4}_.*\.mb59$", f), readdir(srcdir))) : String[]
	# The same places the tool looks, checked HERE first: with no library the tool would ask for one in a
	# modal dialog, and a test must never wait on a dialog.
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	isempty(mbio) && (mbio = Libdl.find_library(["mbio_w64", "mbio", "libmbio"]))
	# EXPERIMENTAL: the tool is switched in and out (IGMT_WITH_MBNAVADJUST in deps/CMakeLists.txt, the include
	# line in InteractiveGMT.jl); with either half off there is nothing to test.
	if !isdefined(IG, :mbnavadjust) || !haskey(IG._LIB_FNS, :gmtvtk_mbnavadjust_open)
		@test_skip "the experimental navigation adjustment tool is not built into this library / package"
	elseif isempty(mbio) || length(srcs) < 3
		@test_skip "MB-System MBIO library or three crossing swath lines not available"
	else
		pump(n = 20) = for _ in 1:n; sleep(0.05); end
		api = IG.GMT.G_API[]
		hasmbgrid = api != C_NULL && IG.GMT.GMT_Call_Module(api, "mbgrid", IG.GMT.GMT_MODULE_EXIST, C_NULL) == 0
		mktempdir() do d
			mkdir(joinpath(d, "data"))
			for f in srcs[1:3]
				cp(joinpath(srcdir, f), joinpath(d, "data", f))
			end
			dl = joinpath(d, "data", "datalist.mb-1")
			open(dl, "w") do io
				for f in srcs[1:3]
					println(io, joinpath(d, "data", f), " 59")
				end
			end
			try
				@test mbnavadjust()
				pump()
				s = IG._mbnavadjust_state()
				@test s.open && !s.project_open

				# File > New: the project file and its data folder
				nvh = joinpath(d, "test.nvh")
				@test IG._mbnavadjust_file(1, nvh)
				@test IG._mbnavadjust_state().project_open
				@test isfile(nvh) && isdir(joinpath(d, "test.dir"))

				# File > Import Swath Data (a datalist): files, sections, crossings
				@test IG._mbnavadjust_file(3, dl, -1)
				pump()
				s = IG._mbnavadjust_state()
				@test s.num_files == 3
				@test s.num_crossings > 0
				@test s.list_count == 3                                # List: the data files
				@test occursin(srcs[1], IG._mbnavadjust_list_item(0))
				@test isfile(joinpath(d, "test.dir", "nvs_0000_0000.mb71"))
				if hasmbgrid
					@test isfile(joinpath(d, "test.dir", "ProjectTopoAdj.grd"))   # mbgrid, run as a GMT module
				else
					@test_skip "MB-System's GMT supplement (mbgrid) not loaded: no project grid"
				end

				# List > Show All Crossings, then the first one: the analysis window, its three canvases
				@test IG._mbnavadjust_press("pushButton_showcrossings")
				@test IG._mbnavadjust_state().list_count == s.num_crossings
				@test IG._mbnavadjust_list_select(0)
				pump()
				s = IG._mbnavadjust_state()
				@test s.naverr_mode == 1 && s.crossing_select == 0      # MBNA_NAVERR_MODE_CROSSING
				for c in 0:2
					p = joinpath(d, "canvas$c.png")
					@test IG._mbnavadjust_save_png(c, p) && filesize(p) > 400
				end
				# drag section 2 over section 1 with the left button, then Add Tie
				@test IG._mbnavadjust_mouse(0, 1, 300, 300, 304, 302)
				@test IG._mbnavadjust_press("pushButton_naverr_addtie")
				pump()
				s = IG._mbnavadjust_state()
				@test s.num_ties >= 1
				@test IG._mbnavadjust_press("pushButton_naverr_dismiss")
				@test IG._mbnavadjust_state().naverr_mode == 0

				# Action > Invert Navigation: a solution from the tie
				@test IG._mbnavadjust_press("pushButton_invertnav")
				pump()
				@test IG._mbnavadjust_state().inversion_status > 0

				# View > Show Modelplot
				@test IG._mbnavadjust_press("pushButton_showmodelplot")
				pump()
				@test IG._mbnavadjust_state().modelplot
				p = joinpath(d, "modelplot.png")
				@test IG._mbnavadjust_save_png(3, p) && filesize(p) > 1000

				# View > Visualize Survey: the project grid in a viewer window, the nav and the ties on it
				if hasmbgrid
					@test IG._mbnavadjust_press("pushButton_updategrids")
					pump()
					@test IG._mbnavadjust_press("pushButton_visualize")
					pump(40)
					@test IG._mbnavadjust_state().visualization
				end

				@test occursin("New project initialized", IG._mbnavadjust_messages())
			finally
				IG._mbnavadjust_close()
				pump()
			end
			@test !IG._mbnavadjust_state().open
			@test isfile(joinpath(d, "test.nvh"))                     # saved at Quit
		end
	end
end
