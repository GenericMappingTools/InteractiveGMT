# :gui scenario for Tools > "Survey planning (mbgrdviz)" (deps/src/mbgrdviz/: MB-System's mbgrdviz, ported onto
# InteractiveGMT windows). Binds the REAL tool to a window showing a grid over MB-System's own test swath file, reads
# that file's navigation and swath bounds into the window, generates a survey route over an area drawn in the window
# (a two-point line), writes it in every route format plus a profile along it, round-trips the route and a site file
# through mbgrdviz's own readers, and opens a drawn region as a new window: the engine, libmbview's data half, the
# MBIO glue (the swath editor's run-time loader plus this tool's entry points) and the window's elements on one path.
#
# Needs MB-System's MBIO library (ENV["INTERACTIVEGMT_MBIO"], e.g. the take2 build's lib/mbio.dll) and the test swath
# file with its .inf (MB-System's test/utilities/testdata/mb57, or ENV["INTERACTIVEGMT_MBGRDVIZ_TESTFILE"]). Without
# either the item is skipped, visibly: CI has neither. Opt in with INTERACTIVEGMT_TEST_GUI=1.

@testitem "mbgrdviz: nav, survey route over a drawn area, every writer, site/route round trip, region view" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	using InteractiveGMT.GMT
	src = get(ENV, "INTERACTIVEGMT_MBGRDVIZ_TESTFILE",
	          raw"C:\progs_cygw\MB-System_take2\test\utilities\testdata\mb57\TN136HS.309.snipped.mb57")
	mbio = get(ENV, "INTERACTIVEGMT_MBIO", "")
	if !isdefined(IG, :mbgrdviz) || !haskey(IG._LIB_FNS, :gmtvtk_mbgrdviz_open)
		@test_skip "the experimental mbgrdviz is not built into this library / package"
	elseif isempty(mbio) || !isfile(mbio) || !isfile(src) || !isfile(src * ".inf")
		@test_skip "MB-System MBIO library (INTERACTIVEGMT_MBIO) or the mb57 test file (+ .inf) not available"
	else
		pump() = for _ in 1:10; sleep(0.05); end
		# a sloping sea floor over the swath file's area (-124.5058/-124.4974, 40.8363/40.8408)
		x = collect(range(-124.51, -124.49, length=101))
		y = collect(range(40.83, 40.85, length=101))
		z = Float32[-600.0 - 2000.0 * (xi + 124.51) - 1000.0 * (yj - 40.83) for yj in y, xi in x]
		G = mat2grid(z, x=x, y=y)
		addpoly(fig, xy::Matrix{Float64}, name::String; closed=false, rect=false) =
			ccall(IG._fn(:gmtvtk_add_poly_full), Cint,
			      (Ptr{Cvoid}, Ptr{Cdouble}, Cint, Cint, Cint, Cdouble, Cdouble, Cdouble, Cdouble, Cint,
			       Cdouble, Cdouble, Cdouble, Cdouble, Cstring, Cstring),
			      fig.h, vec(permutedims(hcat(xy, zeros(size(xy, 1))))), Cint(size(xy, 1)), Cint(closed ? 1 : 0),
			      Cint(rect ? 1 : 0), 1.0, 0.0, 0.0, 2.0, Cint(0), 0.0, 0.0, 0.0, 0.0, name, "")
		mktempdir() do d
			f = joinpath(d, basename(src))
			cp(src, f)
			cp(src * ".inf", f * ".inf")
			fig = IG.view_grid(G; geographic=true, title="mbgrdviz test grid")
			pump()
			try
				@test mbgrdviz(; fig)
				pump()
				s = IG._mbgrdviz_state()
				@test s.open && s.nviews == 1 && s.ready

				# Open Navigation, then Open Swath Data (mbgrdviz reads both from a datalist): the line and its
				# swath bounds land in the window
				dl = joinpath(d, "datalist.mb-1")
				write(dl, basename(f) * " 57\n")
				@test IG._mbgrdviz_open(:nav, dl)
				@test IG._mbgrdviz_state().nnav == 1
				@test IG._mbgrdviz_open(:swath, dl)
				@test IG._mbgrdviz_state().nnav == 2

				# the tracks' "MB-System" menu: the Action menu's four editors, on the group handle and on a
				# track (its swath bounds included); none on a line that is not a track
				menu(el, group) = (buf = zeros(UInt8, 1024);
				                   n = ccall(IG._fn(:gmtvtk_mbgrdviz_track_menu_test), Cint,
				                             (Ptr{Cvoid}, Cstring, Cint, Ptr{UInt8}, Cint), fig.h, el, group, buf, 1024);
				                   (n, unsafe_string(pointer(buf))))
				n, items = menu("Navigation", 1)
				@test n == 4 && split(items, '\n') == ["Open in MBedit", "Open in MBeditviz", "Open in MBnavedit",
				                                        "Open in MBvelocitytool"]
				track = basename(f)
				navidx(t) = ccall(IG._fn(:gmtvtk_mbgrdviz_nav_index), Cint, (Cstring,), t)
				@test navidx(track) >= 0
				@test navidx(track * " (swath bounds)") == navidx(track)
				@test navidx("no such line") == -1
				@test menu(track, 0)[1] == 4
				@test menu(track * " (swath bounds)", 0)[1] == 4
				@test menu("Navigation", 0)[1] == 0

				# "Pick in view": a click on a track (delivered as the scene answers one) checks it, a second
				# unchecks it; a click on any other line leaves the list alone; releasing the button disarms
				navsel(i) = ccall(IG._fn(:gmtvtk_mbgrdviz_nav_selected), Cint, (Cint,), i)
				deliver(nm) = ccall(_test_fn(:gmtvtk_euler_pick_deliver_test), Cint, (Ptr{Cvoid}, Cstring), fig.h, nm)
				i = navidx(track)
				@test navsel(i) == 0
				@test ccall(IG._fn(:gmtvtk_mbgrdviz_pick_nav), Cint, (Cint,), 1) == 1
				@test deliver(track) == 1
				@test navsel(i) == 1
				@test deliver(track * " (swath bounds)") == 1
				@test navsel(i) == 0
				@test deliver(track) == 1
				@test navsel(i) == 1
				@test ccall(IG._fn(:gmtvtk_mbgrdviz_pick_nav), Cint, (Cint,), 0) == 0
				@test deliver(track) == 0                # disarmed: the scene has no pick answer any more
				@test navsel(i) == 1
				@test IG._mbgrdviz_select_nav(Int(i), false)

				# the survey area: a two-point line drawn in the window, 600 m wide
				@test addpoly(fig, [-124.5050 40.8370; -124.4985 40.8400], "area line") >= 0
				pump()
				@test IG._mbgrdviz_set_area("area line", 600.0)
				name = IG._mbgrdviz_generate_survey(linespacing=100, crosslines=2, name="Survey")
				@test name == "Survey"
				s = IG._mbgrdviz_state()
				@test s.nroute == 1                     # the area line is the area, not a route
				@test s.working_route == 0
				# Generate Route again with the dialog still open: the route is REPLACED, as in mbgrdviz
				@test IG._mbgrdviz_generate_survey(linespacing=50, crosslines=0, name="Survey") == "Survey"
				@test IG._mbgrdviz_state().nroute == 1
				@test IG._mbgrdviz_survey_dismiss()

				# every route writer, on the selected route; and the profile along it
				@test IG._mbgrdviz_select_route("Survey")
				for (what, ext) in ((:route, "rte"), (:route_reversed, "rev.rte"), (:risi_heading, "risi1"),
				                    (:risi_noheading, "risi2"), (:risi2_heading, "risi3"), (:risi2_noheading, "risi4"),
				                    (:degdecmin, "ddm"), (:lnw, "lnw"), (:greensea_yml, "yml"), (:tecdis_lst, "lst"),
				                    (:kongsberg_dp, "kdp"), (:sis_asciiplan1, "sis1"), (:sis_asciiplan2, "sis2"),
				                    (:profile, "prf"))
					p = joinpath(d, "survey." * ext)
					@test IG._mbgrdviz_save(what, p)
					@test isfile(p) && filesize(p) > 100
				end
				rte = read(joinpath(d, "survey.rte"), String)
				@test occursin("## Route File Version", rte) && occursin("STARTROUTE", rte)

				# the route file back through mbgrdviz's reader: a second route in the window
				@test IG._mbgrdviz_open(:route, joinpath(d, "survey.rte"))
				@test IG._mbgrdviz_state().nroute == 2

				# a site file through the reader, then out through the writer
				ste = joinpath(d, "in.ste")
				write(ste, "## Site File Version 2.00\n-124.5010,40.8380,-650.0,2,1,Site A\n-124.5000,40.8390,-660.0,4,1,Site B\n")
				@test IG._mbgrdviz_open(:site, ste)
				@test IG._mbgrdviz_state().nsite == 2
				out = joinpath(d, "out.ste")
				@test IG._mbgrdviz_save(:site, out)
				txt = read(out, String)
				@test occursin("Site A", txt) && occursin("Site B", txt)

				# Open Region as New View: a rectangle drawn in the window becomes a new bound window
				@test addpoly(fig, [-124.506 40.835; -124.497 40.835; -124.497 40.842; -124.506 40.842], "region";
				              closed=true, rect=true) >= 0
				pump()
				@test IG._mbgrdviz_set_region("region")
				@test IG._mbgrdviz_open_region()
				pump()
				s = IG._mbgrdviz_state()
				@test s.nviews == 2 && s.ready
			finally
				IG._mbgrdviz_close()
				pump()
			end
			@test !IG._mbgrdviz_state().open
		end
	end
end
