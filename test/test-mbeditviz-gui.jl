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

# :gui scenario for the 3D Soundings PANE of a swath point cloud: MB-System's own test file, opened like any file
# into an empty window (it comes up as a point cloud), gets the 3-D sounding editor as a narrow right-side pane of
# THAT window, its soundings read by MB-System's mbgetdata (no mbeditviz engine, no grid). An edit made through
# the pane (Pick, on the window's own view) takes a sounding off the good ones, and reaches the edit save file
# when the pane closes.
@testitem "3D Soundings pane: a swath cloud gets the editor docked in its window, edits reach the .esf" tags=[:gui] begin
	IG = InteractiveGMT
	src = get(ENV, "INTERACTIVEGMT_MBEDITVIZ_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb57\TN136HS.309.snipped.mb57")
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	if !haskey(IG._LIB_FNS, :gmtvtk_mb_cloud_pane_h)
		@test_skip "the experimental mbeditviz (and its cloud pane) is not built into this library"
	elseif isempty(mbio) || !isfile(mbio) || !isfile(src) || !isfile(src * ".inf")
		@test_skip "MB-System 5.8 MBIO library (INTERACTIVEGMT_MBIO) or the mb57 test file (+ .inf) not available"
	else
		pump() = for _ in 1:10; sleep(0.05); end
		mktempdir() do d
			f = joinpath(d, basename(src))
			cp(src, f)
			cp(src * ".inf", f * ".inf")
			h = ccall(IG._fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "3D Soundings pane test")
			try
				IG._start_pump()
				IG._on_drop(h, f)                    # the file door: a point cloud + its pane
				pump()
				# the pane's soundings (MB-System's mbgetdata) are there: its good-sounding count
				good() = ccall(IG._fn(:gmtvtk_mb_cloud_good_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Cint), h, C_NULL, 0)
				n0 = good()
				@test n0 > 0
				@test ccall(IG._fn(:gmtvtk_has_surface), Cint, (Ptr{Cvoid},), h) == 1
				# Pick (edit mode 1) on a good sounding, through the WINDOW's view: one fewer good sounding
				@test IG._mbeditviz_editor_mode(1)
				picked = false
				for i in 0:199
					IG._mbeditviz_editor_click(i)
					pump()
					good() < n0 && (picked = true; break)
				end
				@test picked
				@test !isfile(f * ".esf") || filesize(f * ".esf") == 0   # written at close, not before
				# closing the window closes its pane, and the pane writes its edits to the .esf
				ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
				h = C_NULL
				pump()
				@test isfile(f * ".esf") && filesize(f * ".esf") > 0
			finally
				h == C_NULL || ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
				pump()
			end
		end
	end
end

# A grid made from the pane's soundings, and a line area on it: "Show point-cloud" opens the soundings inside
# the area in a view of their own with their own 3D Soundings (the full cloud's pane steps aside meanwhile).
# Flags made there reach the full cloud on Accept, and only then; Discard drops them. Either way the full
# cloud gets its pane back.
@testitem "3D Soundings area view: Accept hands its flags to the full cloud, Discard drops them" tags=[:gui] begin
	IG = InteractiveGMT
	src = get(ENV, "INTERACTIVEGMT_MBEDITVIZ_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb57\TN136HS.309.snipped.mb57")
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	if !haskey(IG._LIB_FNS, :gmtvtk_mb_area_cloud_h)
		@test_skip "the experimental mbeditviz (and its cloud pane) is not built into this library"
	elseif isempty(mbio) || !isfile(mbio) || !isfile(src) || !isfile(src * ".inf")
		@test_skip "MB-System 5.8 MBIO library (INTERACTIVEGMT_MBIO) or the mb57 test file (+ .inf) not available"
	else
		pump(n = 20) = for _ in 1:n; sleep(0.05); end
		mktempdir() do d
			f = joinpath(d, basename(src))
			cp(src, f)
			cp(src * ".inf", f * ".inf")
			h = ccall(IG._fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "3D Soundings area test")
			try
				IG._start_pump()
				IG._on_drop(h, f)
				pump()
				good() = ccall(IG._fn(:gmtvtk_mb_cloud_good_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Cint), h, C_NULL, 0)
				g0 = good()
				@test g0 > 0
				# the area: the middle third of the soundings' extent
				D = IG._mb_cloud_dataset(h)
				x0, x1 = extrema(view(D.data, :, 1)); y0, y1 = extrema(view(D.data, :, 2))
				ax, bx = x0 + (x1 - x0) / 3, x1 - (x1 - x0) / 3
				ay, by = y0 + (y1 - y0) / 3, y1 - (y1 - y0) / 3
				poly = Float64[ax, ay, bx, ay, bx, by, ax, by]
				function area_round(accept::Bool)
					@test ccall(IG._fn(:gmtvtk_mb_area_cloud_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Cint), h, poly, Cint(4)) == 1
					pump()
					@test good() == -1                       # the full cloud's pane stepped aside
					@test IG._mbeditviz_editor_mode(1)      # Pick, in the area view
					for i in 0:40
						IG._mbeditviz_editor_click(i)
					end
					pump()
					@test ccall(IG._fn(:gmtvtk_mb_area_finish_h), Cint, (Cint,), accept ? 1 : 0) == 1
					pump(40)
					return good()
				end
				@test area_round(false) == g0                # Discard: nothing reached the full cloud
				@test area_round(true) < g0                  # Accept: the picks are the full cloud's now
			finally
				ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
				pump()
			end
		end
	end
end

# An edit marks a sounding, it never makes it vanish: with View > Show flagged OFF (which hides the soundings
# that came in flagged), Erase through the window's view flags soundings and every one of them is still drawn.
@testitem "3D Soundings: erased soundings stay drawn with Show flagged off" tags=[:gui] begin
	IG = InteractiveGMT
	src = get(ENV, "INTERACTIVEGMT_MBEDITVIZ_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb57\TN136HS.309.snipped.mb57")
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	if !haskey(IG._LIB_FNS, :gmtvtk_mb_soundings_counts)
		@test_skip "the experimental mbeditviz (and its cloud pane) is not built into this library"
	elseif isempty(mbio) || !isfile(mbio) || !isfile(src) || !isfile(src * ".inf")
		@test_skip "MB-System 5.8 MBIO library (INTERACTIVEGMT_MBIO) or the mb57 test file (+ .inf) not available"
	else
		pump(n = 20) = for _ in 1:n; sleep(0.05); end
		mktempdir() do d
			f = joinpath(d, basename(src))
			cp(src, f)
			cp(src * ".inf", f * ".inf")
			h = ccall(IG._fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "3D Soundings erase test")
			try
				IG._start_pump()
				IG._on_drop(h, f)
				pump()
				cnt() = (v = zeros(Cint, 3); ccall(IG._fn(:gmtvtk_mb_soundings_counts), Cint, (Ptr{Cint},), v);
				         (drawn = v[1], good = v[2], flagged = v[3]))
				@test ccall(IG._fn(:gmtvtk_mb_soundings_show_flagged), Cint, (Cint,), 0) == 1
				pump()
				a = cnt()
				@test a.drawn == a.good                    # flagged-at-load hidden: only the good are drawn
				@test IG._mbeditviz_editor_mode(2)         # Erase
				for i in 0:60
					IG._mbeditviz_editor_click(i)
				end
				pump()
				b = cnt()
				@test b.flagged > a.flagged                # Erase flagged soundings...
				@test b.drawn == a.drawn                   # ...and every one of them is still drawn
			finally
				ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
				pump()
			end
		end
	end
end
