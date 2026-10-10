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
					@test ccall(IG._fn(:gmtvtk_mb_pane_scene), Ptr{Cvoid}, ()) != h                       # the full cloud's pane stepped aside
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

# Residues: hidden until the pane's Gridding made a surface; then the good soundings minus that surface open
# in the sub-cloud view, its z the residue. Flags made there reach the full cloud on Accept, and the next
# Gridding grids only what is left.
@testitem "3D Soundings Residues: shown after Gridding, its flags drop soundings from the next grid" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	src = get(ENV, "INTERACTIVEGMT_MBEDITVIZ_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb57\TN136HS.309.snipped.mb57")
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	if !haskey(IG._LIB_FNS, :gmtvtk_mb_residue_cloud_h)
		@test_skip "the experimental mbeditviz (and its cloud pane) is not built into this library"
	elseif isempty(mbio) || !isfile(mbio) || !isfile(src) || !isfile(src * ".inf")
		@test_skip "MB-System 5.8 MBIO library (INTERACTIVEGMT_MBIO) or the mb57 test file (+ .inf) not available"
	else
		pump(n = 20) = for _ in 1:n; sleep(0.05); end
		mktempdir() do d
			f = joinpath(d, basename(src))
			cp(src, f)
			cp(src * ".inf", f * ".inf")
			h = ccall(IG._fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "3D Soundings residues test")
			try
				IG._start_pump()
				IG._on_drop(h, f)
				pump()
				good() = ccall(IG._fn(:gmtvtk_mb_cloud_good_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Cint), h, C_NULL, 0)
				buttons() = ccall(IG._fn(:gmtvtk_mb_pane_buttons), Cint, ())
				g0 = good()
				@test g0 > 0
				@test buttons() & 8 == 0                         # no surface yet: no Residues
				D = IG._mb_cloud_dataset(h)
				m = split(IG._mb_grid_meta(D), '/')
				kv = "method=mbgrid\ninfile=t (point cloud)\ncloud=1\ncoords=geog\nregion=$(join(m[1:4], '/'))\ninc=$(m[5])/$(m[6])"
				grid() = GC.@preserve kv IG._on_interpolate(h, Base.unsafe_convert(Cstring, kv))
				@test grid() == 1
				pump()
				@test buttons() & 8 == 8                         # Gridding made a surface: Residues shows
				IG._mb_cloud_residues(h)
				pump()
				@test ccall(IG._fn(:gmtvtk_mb_pane_scene), Ptr{Cvoid}, ()) != h                               # the full cloud's pane stepped aside
				@test buttons() & 8 == 0                         # the residue view has no surface of its own
				# the view's soundings are the residues: small against the depths
				zr = zeros(Cint, 3); ccall(IG._fn(:gmtvtk_mb_soundings_counts), Cint, (Ptr{Cint},), zr)
				@test 0 < zr[2] <= g0
				# the residue window's OWN point selection (Shift+left-drag): the left half of the view
				rw = ccall(IG._fn(:gmtvtk_mb_pane_scene), Ptr{Cvoid}, ())
				@test rw != C_NULL && rw != h
				z1 = (IG._scene_state(rw)["zmin"], IG._scene_state(rw)["zmax"])   # round 1's vertical range
				nsel = ccall(GmtvtkTest._test_fn(:gmtvtk_cloud_select_box_test), Cint,
				             (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble), rw, 0.0, 0.0, 0.5, 1.0)
				@test nsel > 0
				@test IG._mbeditviz_editor_mode(1)               # and Pick, in the residue pane
				for i in 0:40
					IG._mbeditviz_editor_click(i)
				end
				pump()
				@test ccall(IG._fn(:gmtvtk_mb_area_finish_h), Cint, (Cint,), 1) == 1   # Accept
				pump(40)
				g1 = good()
				@test g1 <= g0 - nsel                            # every selected point is gone, the picks too
				@test buttons() & 8 == 8                         # its pane back, Residues still offered
				@test size(IG._mb_cloud_dataset(h).data, 1) == g1   # the next Gridding's input: the clean set
				@test grid() == 1
				# closing the residue window KEEPS what was selected there (it is not a Discard)
				IG._mb_cloud_residues(h)
				pump()
				rw = ccall(IG._fn(:gmtvtk_mb_pane_scene), Ptr{Cvoid}, ())
				# a later round is drawn on round 1's scale, so a smaller round looks smaller
				st = IG._scene_state(rw)
				@test st["zmin"] <= z1[1] && st["zmax"] >= z1[2]
				nsel = ccall(GmtvtkTest._test_fn(:gmtvtk_cloud_select_box_test), Cint,
				             (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble), rw, 0.5, 0.0, 1.0, 1.0)
				@test nsel > 0
				ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), rw)
				pump(40)
				@test good() == g1 - nsel
				@test grid() == 1
				# the residue pane's Auto flag slider: 1 flags none, smaller flags the farthest residues,
				# moving back restores them; what it holds at Accept leaves the full cloud
				g2 = good()
				@test ccall(IG._fn(:gmtvtk_mb_residue_autoflag), Cint, (Cdouble,), 0.5) == -1   # no residue view
				IG._mb_cloud_residues(h)
				pump()
				af(t) = ccall(IG._fn(:gmtvtk_mb_residue_autoflag), Cint, (Cdouble,), t)
				r1 = af(1.0)
				@test r1 > 0
				r5 = af(0.5); r1b = af(1.0); r3 = af(0.3); r2 = af(0.1)
				@test r5 < r1 && r1b == r1                       # flags, then restores exactly
				@test r2 <= r3 < r5                              # smaller fraction, more flagged
				@test af(0.3) == r3
				# Compute in the full cloud's Gridding dialog with the residue window STILL OPEN: its flags
				# and its selection are already out of the input
				rw = ccall(IG._fn(:gmtvtk_mb_pane_scene), Ptr{Cvoid}, ())
				nsel2 = ccall(GmtvtkTest._test_fn(:gmtvtk_cloud_select_box_test), Cint,
				              (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble), rw, 0.0, 0.0, 1.0, 0.5)
				@test nsel2 > 0
				nopen = size(IG._mb_cloud_dataset(h).data, 1)
				@test nopen < g2 - (r1 - r3)                     # the auto flags AND the selection are out
				@test grid() == 1
				ccall(GmtvtkTest._test_fn(:gmtvtk_cloud_select_box_test), Cint,      # the same box again: deselects
				      (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble), rw, 0.0, 0.0, 1.0, 0.5)
				@test size(IG._mb_cloud_dataset(h).data, 1) == g2 - (r1 - r3)
				@test ccall(IG._fn(:gmtvtk_mb_area_finish_h), Cint, (Cint,), 1) == 1   # Accept
				pump(40)
				@test good() == g2 - (r1 - r3)
				@test grid() == 1
				# back in the full cloud, View > Show flagged OFF hides what the residues flagged (it came back
				# as a cleaning): nothing but the good soundings is drawn
				@test ccall(IG._fn(:gmtvtk_mb_soundings_show_flagged), Cint, (Cint,), 0) == 1
				pump()
				cn = zeros(Cint, 3); ccall(IG._fn(:gmtvtk_mb_soundings_counts), Cint, (Ptr{Cint},), cn)
				@test cn[1] == cn[2] == good()
				@test ccall(IG._fn(:gmtvtk_mb_soundings_show_flagged), Cint, (Cint,), 1) == 1
				pump()
				ccall(IG._fn(:gmtvtk_mb_soundings_counts), Cint, (Ptr{Cint},), cn)
				@test cn[1] == cn[2] + cn[3]                     # Show flagged ON: they are all back
			finally
				ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
				pump()
			end
		end
	end
end

# A navigation track of the cloud offers the same "Show point-cloud" in its "MB-System" submenu (and so
# does its group's handle): the soundings of ITS file, in the same view an area gets, with the same
# Discard. Here the cloud is one file, so the track's view holds every good sounding of it.
@testitem "Navigation track: MB-System > Show point-cloud opens its file's soundings" tags=[:gui] begin
	IG = InteractiveGMT
	src = get(ENV, "INTERACTIVEGMT_MBEDITVIZ_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb57\TN136HS.309.snipped.mb57")
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	if !haskey(IG._LIB_FNS, :gmtvtk_mb_file_cloud_h)
		@test_skip "the experimental mbeditviz (and its cloud pane) is not built into this library"
	elseif isempty(mbio) || !isfile(mbio) || !isfile(src) || !isfile(src * ".inf")
		@test_skip "MB-System 5.8 MBIO library (INTERACTIVEGMT_MBIO) or the mb57 test file (+ .inf) not available"
	else
		pump(n = 20) = for _ in 1:n; sleep(0.05); end
		mktempdir() do d
			f = joinpath(d, basename(src))
			cp(src, f)
			cp(src * ".inf", f * ".inf")
			h = ccall(IG._fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "3D Soundings track test")
			try
				IG._start_pump()
				IG._on_drop(h, f)
				pump()
				good() = ccall(IG._fn(:gmtvtk_mb_cloud_good_h), Cint, (Ptr{Cvoid}, Ptr{Cdouble}, Cint), h, C_NULL, 0)
				g0 = good()
				@test g0 > 0
				function items(element, group)
					buf = zeros(UInt8, 2048)
					ccall(IG._fn(:gmtvtk_mbgrdviz_track_menu_test), Cint,
					      (Ptr{Cvoid}, Cstring, Cint, Ptr{UInt8}, Cint), h, element, Cint(group), buf, Cint(length(buf)))
					return unsafe_string(pointer(buf))
				end
				@test occursin("Show point-cloud", items(basename(f), 0))                       # the track
				@test occursin("Show point-cloud", items("Navigation - " * basename(f), 1))     # its group
				@test ccall(IG._fn(:gmtvtk_mb_file_cloud_h), Cint, (Ptr{Cvoid}, Cstring), h, f) == 1
				pump()
				@test ccall(IG._fn(:gmtvtk_mb_pane_scene), Ptr{Cvoid}, ()) != h                               # the full cloud's pane stepped aside
				# a FULL 3D Soundings pane: CUBE filter, Gridding -- and Discard, being a sub-cloud view
				@test ccall(IG._fn(:gmtvtk_mb_pane_buttons), Cint, ()) == 7
				@test ccall(IG._fn(:gmtvtk_mb_area_finish_h), Cint, (Cint,), 0) == 1   # Discard
				pump(40)
				@test good() == g0                               # nothing reached the full cloud
			finally
				ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
				pump()
			end
		end
	end
end

# View > Show flagged OFF hides EVERY flagged sounding: those that came in flagged and those an edit flags --
# Erase through the window's view flags soundings and they leave the view; ON brings them all back.
@testitem "3D Soundings: Show flagged off hides erased soundings too" tags=[:gui] begin
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
				@test b.drawn == b.good < a.drawn          # ...and they are hidden with the rest
				@test ccall(IG._fn(:gmtvtk_mb_soundings_show_flagged), Cint, (Cint,), 1) == 1
				pump()
				c = cnt()
				@test c.drawn == c.good + c.flagged       # Show flagged ON: every one drawn again
			finally
				ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
				pump()
			end
		end
	end
end
