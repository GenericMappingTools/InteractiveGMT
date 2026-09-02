# :gui scenarios for the Plates menu (Euler rotations, Plate calculator). These open a REAL window
# through the built gmtvtk.dll,
# put lines in it, and drive the SAME callback the dialog drives, so the whole path runs: the line's
# vertices out of the scene (gmtvtk_serialize_vector_h), the GMT spotter call, and the rotated lines
# back in as new elements. The dialog itself is opened through its test hooks in gmtvtk_test.dll — a
# QUiLoader failure or a widget name that drifted out of euler_stuff.ui fails here, not on the user's
# first click. Opt in with INTERACTIVEGMT_TEST_GUI=1 (or `Pkg.test(test_args=["gui"])`).

@testmodule Plates begin

using InteractiveGMT
const IG = InteractiveGMT

# A small geographic grid (off Portugal) to hang the lines on.
function grid()
	IG.GMT.mat2grid(Float32[1000exp(-(((ix - 20) / 8)^2 + ((iy - 15) / 6)^2)) for iy in 0:30, ix in 0:40];
	                x = collect(range(-10.0, -6.0, length = 41)), y = collect(range(36.0, 39.0, length = 31)),
	                proj4 = "+proj=longlat +datum=WGS84")
end

const LINE = [-9.5 36.5; -9.0 37.0; -8.5 37.5; -8.0 38.0]

send(h, kv) = IG._on_euler(h, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n")))) |> (r -> IG._errored(r, "Euler rotations|Plate calculator|Compute Euler pole"))

end # @testmodule

@testitem "Euler: a line rotated by one finite pole is backtracker's own answer" tags=[:gui] setup=[Plates] begin
	IG = InteractiveGMT
	f = view_grid(Plates.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(Plates.LINE), "testline")
	@test IG._euler_segments(f.h, "testline")[1] ≈ Plates.LINE

	@test Plates.send(f.h, ["op=rotate", "target=testline", "usepole=1", "polelon=-40.8",
	                        "polelat=32.8", "poleang=-12.9", "revert=0", "geodetic=0",
	                        "polesfile=", "ages=", "showcmd=0"]) == 1
	got = IG._euler_segments(f.h, "testline rot -40.8/32.8/-12.9")
	@test length(got) == 1
	ref = IG.GMT.gmt("backtracker -E-40.8/32.8/-12.9 -Db -Q1", Plates.LINE)
	@test got[1] ≈ (isa(ref, Vector) ? ref[1].data : ref.data)[:, 1:2]
	# The SOURCE line stays in the window: a reconstruction is read against what it came from.
	@test IG._euler_segments(f.h, "testline")[1] ≈ Plates.LINE
end

@testitem "Euler: a rotation model + ages gives one line per age" tags=[:gui] setup=[Plates] begin
	IG = InteractiveGMT
	stg = joinpath(tempdir(), "igmt_stage_$(time_ns()).stg")
	# Cox's EUR-NAM stage poles (lon lat tstart tend angle) — GMT reads this 5-column form as stages.
	write(stg, """
	-23.54692\t80.33835\t90.0\t83.0\t-4.3588
	-22.68443\t80.43997\t83.0\t53.0\t-11.9737
	-25.56612\t 5.38147\t53.0\t48.0\t 2.5663
	157.29147\t 6.75318\t48.0\t37.0\t-3.4258
	129.90000\t68.00000\t37.0\t 0.0\t-7.8000
	""")
	try
		f = view_grid(Plates.grid())
		IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(Plates.LINE), "isoc")
		@test Plates.send(f.h, ["op=rotate", "target=isoc", "usepole=0", "polesfile=$stg",
		                        "ages=37,53", "agelabels=", "revert=0", "geodetic=0", "showcmd=0"]) == 1
		for a in (37, 53)
			got = IG._euler_segments(f.h, "isoc @ $(a).0 Ma")
			@test length(got) == 1
			ref = IG.GMT.gmt("backtracker -E$stg -Db -Q$a", Plates.LINE)
			@test got[1] ≈ (isa(ref, Vector) ? ref[1].data : ref.data)[:, 1:2]
		end
		# A ONE-POINT target is a flow line, not a rotated point (Mirone's own split).
		IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds([-9.0 37.0]), "seed")
		@test Plates.send(f.h, ["op=rotate", "target=seed", "usepole=0", "polesfile=$stg",
		                        "ages=90", "agelabels=", "revert=0", "geodetic=0", "showcmd=0"]) == 1
		fl = IG._euler_segments(f.h, "seed flow line")
		@test length(fl) == 1 && size(fl[1], 1) > 10
		@test fl[1][1, :] ≈ [-9.0, 37.0] atol = 1e-6         # the path starts at the seed point
	finally
		isfile(stg) && rm(stg, force = true)
	end
end

@testitem "Euler: Geodetic Lats changes the answer, and is GMT's own conversion" tags=[:gui] setup=[Plates] begin
	IG = InteractiveGMT
	f = view_grid(Plates.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(Plates.LINE), "testline")
	pole = ["polelon=-40.8", "polelat=32.8", "poleang=-12.9"]
	@test Plates.send(f.h, ["op=rotate", "target=testline", "usepole=1", pole...,
	                        "geodetic=0", "revert=0", "polesfile=", "ages=", "showcmd=0"]) == 1
	@test Plates.send(f.h, ["op=rotate", "target=testline", "usepole=1", pole...,
	                        "geodetic=1", "revert=0", "polesfile=", "ages=", "showcmd=0"]) == 1
	plain = IG._euler_segments(f.h, "testline rot -40.8/32.8/-12.9")[1]
	geod  = IG._euler_segments(f.h, "testline rot -40.8/32.8/-12.9, geodetic")[1]
	@test size(plain) == size(geod)
	@test !(plain ≈ geod)                                  # the option must actually bite
	# …and it bites by exactly mapproject's geodetic<->geocentric latitudes, nothing hand-rolled.
	inp = IG._euler_lat_convert(Plates.LINE, false)
	ref = IG.GMT.gmt("backtracker -E-40.8/32.8/-12.9 -Db -Q1", inp)
	D = (isa(ref, Vector) ? ref[1].data : ref.data)[:, 1:2]
	@test geod ≈ IG._euler_lat_convert(Matrix{Float64}(D), true)
end

@testitem "Euler: several lines rotate in one go" tags=[:gui] setup=[Plates] begin
	IG = InteractiveGMT
	f = view_grid(Plates.grid())
	other = [-9.8 36.2; -9.2 36.9; -8.6 37.4]
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(Plates.LINE), "lineA")
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(other), "lineB")
	@test Plates.send(f.h, ["op=rotate", "target1=lineA", "target2=lineB", "usepole=1",
	                        "polelon=-40.8", "polelat=32.8", "poleang=-12.9", "revert=0",
	                        "geodetic=0", "polesfile=", "ages=", "showcmd=0"]) == 1
	for (nm, src) in (("lineA", Plates.LINE), ("lineB", other))
		got = IG._euler_segments(f.h, "$nm rot -40.8/32.8/-12.9")
		@test length(got) == 1
		ref = IG.GMT.gmt("backtracker -E-40.8/32.8/-12.9 -Db -Q1", src)
		@test got[1] ≈ (isa(ref, Vector) ? ref[1].data : ref.data)[:, 1:2]
	end
	# One bad name among good ones must not sink the rest.
	@test Plates.send(f.h, ["op=rotate", "target1=lineA", "target2=no such line", "usepole=1",
	                        "polelon=10", "polelat=20", "poleang=5", "revert=0", "geodetic=0",
	                        "polesfile=", "ages=", "showcmd=0"]) == 1
	@test !isempty(IG._euler_segments(f.h, "lineA rot 10.0/20.0/5.0"))
end

@testitem "Euler: an isochron's own header poles reach the Poles selector" tags=[:gui] setup=[Plates] begin
	IG = InteractiveGMT
	answer() = IG._euler_last_result[]     # what the dialog reads back through the C answer channel
	# A Mirone isochron file: the "> …" header carries the line's own Euler poles.
	dat = joinpath(tempdir(), "igmt_isoc_$(time_ns()).dat")
	write(dat, """
	> 5c EURASIA/NORTH AMERICA FIN"136.53 63.63 -3.951 16.37" STG0"147.6332 51.4257 19.60 16.37 0.3362"
	-9.5 36.5
	-9.0 37.0
	-8.5 37.5
	""")
	try
		f = view_grid(Plates.grid())
		D = IG.GMT.gmtread(dat)
		IG._add_dataset_to_scene(f.h, D, "c5c")
		# The header survived the import as the overlay's own info…
		n = ccall(IG._fn(:gmtvtk_vector_info_h), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint),
		          f.h, "c5c", C_NULL, Cint(0))
		@test n > 0
		# …and comes back parsed as catalogue lines (FIN verbatim; STG as lon lat angle t_end).
		@test Plates.send(f.h, ["op=headerpoles", "target1=c5c", "showcmd=0"]) == 1
		lines = split(answer(), '\n'; keepempty = false)
		@test length(lines) == 2
		@test occursin("!FIN", lines[1]) && occursin("!STG0", lines[2])
		# Catalogue spelling: lon/lat to 2 decimals, angle to 4 — Mirone's own %.2f/%.3f line format.
		fin = parse.(Float64, split(split(lines[1], '!')[1]))
		@test fin ≈ [136.53, 63.63, -3.951, 16.37] atol = 1e-2      # FIN: lon lat ang age, verbatim
		stg = parse.(Float64, split(split(lines[2], '!')[1]))
		@test stg ≈ [147.6332, 51.4257, 0.3362, 16.37] atol = 1e-2  # STG: lon lat ANGLE t_end
		# A line with no header at all answers "no poles", not an error.
		IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(Plates.LINE), "plain")
		@test Plates.send(f.h, ["op=headerpoles", "target1=plain", "showcmd=0"]) == 1
		@test isempty(strip(answer()))
	finally
		isfile(dat) && rm(dat, force = true)
	end
end

@testitem "Euler dialog: opens, lists the lines, and takes picks" tags=[:gui] setup=[Plates, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = view_grid(Plates.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(Plates.LINE), "testline")
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds([-9.8 36.2; -9.2 36.9]), "otherline")
	@test ccall(_test_fn(:gmtvtk_euler_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	buf = Vector{UInt8}(undef, 256)
	sel() = (ccall(_test_fn(:gmtvtk_euler_targets_test), Cint, (Ptr{UInt8}, Cint), buf, Cint(256));
	         unsafe_string(pointer(buf)))
	n = ccall(_test_fn(:gmtvtk_euler_targets_test), Cint, (Ptr{UInt8}, Cint), buf, Cint(256))
	@test n == 2                                            # both lines offered as targets
	@test isempty(sel())                                    # nothing selected until asked
	# Click-pick stays armed and ACCUMULATES; a rect pick answers with several names at once.
	@test ccall(_test_fn(:gmtvtk_euler_arm_pick_test), Cint, (Cint,), Cint(1)) == 1
	@test ccall(_test_fn(:gmtvtk_euler_pick_deliver_test), Cint, (Ptr{Cvoid}, Cstring), f.h, "testline") == 1
	@test sel() == "testline"
	@test ccall(_test_fn(:gmtvtk_euler_pick_deliver_test), Cint, (Ptr{Cvoid}, Cstring), f.h, "otherline") == 1
	@test sort(split(sel(), '\n')) == ["otherline", "testline"]
	ccall(_test_fn(:gmtvtk_euler_delete_dialog_test), Cvoid, ())
end

@testitem "Plate calculator dialog: model, plates, pole and velocity" tags=[:gui] setup=[Plates, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	# The dialog lives in gmtvtk_test.dll, so THAT dll's Julia callback has to be registered (and its
	# answer channel mirrored) — otherwise the combos would come up empty.
	GmtvtkTest._register_euler_test()
	f = view_grid(Plates.grid())
	@test ccall(_test_fn(:gmtvtk_platecalc_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	buf = Vector{UInt8}(undef, 256)
	rd(what) = (ccall(_test_fn(:gmtvtk_platecalc_read_test), Cint, (Cstring, Ptr{UInt8}, Cint),
	                  what, buf, Cint(256)); unsafe_string(pointer(buf)))
	sel(model, fix, mov) = ccall(_test_fn(:gmtvtk_platecalc_select_test), Cint,
	                             (Cstring, Cstring, Cstring), model, fix, mov)
	# Nuvel-1A, Africa relative to Eurasia — the boxes must hold the published pole.
	@test sel("Nuvel1A", "EU", "AF") == length(IG._plate_poles("Nuvel1A"))
	@test parse(Float64, rd("polelon")) ≈ -20.61 atol = 0.02
	@test parse(Float64, rd("polelat")) ≈ 21.03 atol = 0.02
	@test parse(Float64, rd("polerate")) ≈ 0.1228 atol = 1e-3
	@test rd("fixenabled") == "1"
	@test rd("abs2rel") == "0"              # relative model: no "Make it relative" checkbox
	# Calculate reports the speed and azimuth of that pole at the point.
	@test ccall(_test_fn(:gmtvtk_platecalc_calc_test), Cint, (Cdouble, Cdouble), -9.0, 36.0) == 1
	@test occursin("mm/yr", rd("speed"))
	@test parse(Float64, split(rd("speed"))[3]) ≈ 4.2 atol = 0.05
	@test parse(Float64, split(rd("azim"))[3]) ≈ 307.6 atol = 0.2
	# The same plate on both sides does not move: the pole boxes clear, exactly as Mirone's do.
	@test sel("", "AF", "AF") > 0
	@test isempty(rd("polelon")) && isempty(rd("polerate"))
	# An absolute model offers "Make it relative" and locks the fixed-plate combo.
	@test sel("NNR", "", "AU") > 0
	@test rd("abs2rel") == "1"
	@test rd("fixenabled") == "0"
	nnr = IG._plate_poles("NNR")
	au = nnr[findfirst(p -> p.abbrev == "AU", nnr)]
	@test parse(Float64, rd("polerate")) ≈ au.rate atol = 1e-3
	ccall(_test_fn(:gmtvtk_platecalc_delete_dialog_test), Cvoid, ())
end

@testitem "Plate calculator map: the plates are there and a click drives the whole tool" tags=[:gui] setup=[Plates, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	GmtvtkTest._register_euler_test()
	f = view_grid(Plates.grid())
	@test ccall(_test_fn(:gmtvtk_platecalc_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	buf = Vector{UInt8}(undef, 256)
	rd(what) = (ccall(_test_fn(:gmtvtk_platecalc_read_test), Cint, (Cstring, Ptr{UInt8}, Cint),
	                  what, buf, Cint(256)); unsafe_string(pointer(buf)))
	# plate under a point + how many polygons the map holds for the current model
	at(lon, lat) = (n = ccall(_test_fn(:gmtvtk_platecalc_map_test), Cint, (Cdouble, Cdouble, Ptr{UInt8}, Cint),
	                          lon, lat, buf, Cint(256)); (n, unsafe_string(pointer(buf))))
	sel(model) = ccall(_test_fn(:gmtvtk_platecalc_select_test), Cint, (Cstring, Cstring, Cstring),
	                   model, "", "")
	@test sel("Nuvel1A") > 0
	n, tag = at(20.0, 0.0)
	@test n > 10                            # the Nuvel-1A polygon set really loaded
	@test tag == "AF"                       # central Africa
	@test at(-140.0, 0.0)[2] == "PA"        # central Pacific
	@test at(-60.0, -20.0)[2] == "SA"       # South America
	# A click IS the tool (Mirone's bdn_plate): point + moving plate + fixed neighbour + velocity.
	@test ccall(_test_fn(:gmtvtk_platecalc_map_click_test), Cint, (Cdouble, Cdouble), 20.0, 0.0) == 1
	@test parse(Float64, rd("lon")) ≈ 20.0 atol = 1e-3
	@test parse(Float64, rd("lat")) ≈ 0.0 atol = 1e-3
	@test !isempty(rd("polerate"))          # the pair's pole landed in the boxes
	@test occursin("mm/yr", rd("speed"))    # …and the velocity was computed, with no button pressed
	@test occursin("degree", rd("azim"))
	# The map follows the model: P. Bird's 52 plates are a different, bigger set.
	@test sel("PB") > 0
	@test at(20.0, 0.0)[1] > n
	@test at(20.0, 0.0)[2] == "AF"
	ccall(_test_fn(:gmtvtk_platecalc_delete_dialog_test), Cvoid, ())
end

@testitem "Plate calculator dialog: the X parks it, it does not kill it" tags=[:gui] setup=[Plates, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	GmtvtkTest._register_euler_test()
	f = view_grid(Plates.grid())
	@test ccall(_test_fn(:gmtvtk_platecalc_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test ccall(_test_fn(:gmtvtk_platecalc_select_test), Cint, (Cstring, Cstring, Cstring),
	            "PB", "EU", "AF") > 0
	@test ccall(_test_fn(:gmtvtk_platecalc_parked_test), Cint, (Ptr{Cvoid},), f.h) == 0
	ccall(_test_fn(:gmtvtk_platecalc_close_dialog_test), Cvoid, ())
	@test ccall(_test_fn(:gmtvtk_platecalc_parked_test), Cint, (Ptr{Cvoid},), f.h) == 1
	rows = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	@test occursin("Plate calculator", rows)
	# Re-opening brings THAT dialog back, still on the model and plates it was left on.
	@test ccall(_test_fn(:gmtvtk_platecalc_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test ccall(_test_fn(:gmtvtk_platecalc_parked_test), Cint, (Ptr{Cvoid},), f.h) == 0
	buf = Vector{UInt8}(undef, 256)
	ccall(_test_fn(:gmtvtk_platecalc_read_test), Cint, (Cstring, Ptr{UInt8}, Cint),
	      "polerate", buf, Cint(256))
	@test !isempty(unsafe_string(pointer(buf)))
	rows = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	@test !occursin("Plate calculator", rows)
	ccall(_test_fn(:gmtvtk_platecalc_delete_dialog_test), Cvoid, ())
	@test ccall(_test_fn(:gmtvtk_platecalc_parked_test), Cint, (Ptr{Cvoid},), f.h) == -1
end

@testitem "Euler dialog: the X parks it, it does not kill it" tags=[:gui] setup=[Plates, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = view_grid(Plates.grid())
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(Plates.LINE), "testline")
	@test ccall(_test_fn(:gmtvtk_euler_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test ccall(_test_fn(:gmtvtk_euler_arm_pick_test), Cint, (Cint,), Cint(1)) == 1
	@test ccall(_test_fn(:gmtvtk_euler_pick_deliver_test), Cint, (Ptr{Cvoid}, Cstring), f.h, "testline") == 1
	@test ccall(_test_fn(:gmtvtk_euler_parked_test), Cint, (Ptr{Cvoid},), f.h) == 0     # on screen
	# The X hides it and leaves a handle in Scene Objects — like a closed X,Y plot, not a destroyed one.
	ccall(_test_fn(:gmtvtk_euler_close_dialog_test), Cvoid, ())
	@test ccall(_test_fn(:gmtvtk_euler_parked_test), Cint, (Ptr{Cvoid},), f.h) == 1
	rows = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	@test occursin("Euler rotations", rows)
	# Re-opening brings THAT dialog back, with its selection still set — never a second, empty one.
	@test ccall(_test_fn(:gmtvtk_euler_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test ccall(_test_fn(:gmtvtk_euler_parked_test), Cint, (Ptr{Cvoid},), f.h) == 0
	buf = Vector{UInt8}(undef, 256)
	ccall(_test_fn(:gmtvtk_euler_targets_test), Cint, (Ptr{UInt8}, Cint), buf, Cint(256))
	@test unsafe_string(pointer(buf)) == "testline"
	rows = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	@test !occursin("Euler rotations", rows)                                            # row taken back
	# "Delete" on the parked row is the only real close.
	ccall(_test_fn(:gmtvtk_euler_delete_dialog_test), Cvoid, ())
	@test ccall(_test_fn(:gmtvtk_euler_parked_test), Cint, (Ptr{Cvoid},), f.h) == -1
end

# --- Compute Euler pole (compute_euler.m) ------------------------------------------------------
# The lines are a real isochron and ITS OWN rotation by a known pole, so the search has an exact
# answer to find and the test can say what "right" is.
@testitem "Compute Euler pole: the search recovers the pole the lines were made with" tags=[:gui] setup=[Plates] begin
	IG = InteractiveGMT
	f = view_grid(Plates.grid())
	lat  = collect(36.0:0.1:38.5)
	iso1 = [(-9.5 .+ 0.4 .* sin.((lat .- 36) .* 2)) lat]
	iso2 = IG._ce_rotated_line(iso1, 20.0, 60.0, 3.0)
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(iso1), "isoc1")
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(iso2), "isoc2")

	kv = ["op=ceuler", "line1=isoc1", "line2=isoc2", "polelon=19", "polelat=59", "poleang=2.9",
	      "lonrange=6", "latrange=6", "angrange=0.6", "nlon=13", "nlat=13", "nang=7",
	      "hellinger=0", "plotres=0", "loop=0", "residfile=", "residfmt=nc", "showcube=0"]
	@test Plates.send(f.h, kv) == 1                      # returns at once: the search is a task
	t0 = time()
	while IG._CE_RUNNING[] && time() - t0 < 120;  sleep(0.05);  end
	@test !IG._CE_RUNNING[]
	for _ in 1:40                                        # let the finishing Timer tick
		occursin("Fitted Line", IG._euler_last_result[]) || occursin("pole", IG._euler_last_result[]) ? break : nothing
		sleep(0.05)
	end
	sleep(0.5)
	lon, lat_, ang, res = IG._CE_BEST[]
	@test abs(lon - 20.0) < 1.5 && abs(lat_ - 60.0) < 1.5
	@test abs(ang - 3.0) < 0.1
	@test res < 3.0                                      # a few km of a perfect fit

	# The fitted line landed in the window as its own element, and the sources are still there.
	nm = "Fitted Line (" * IG._ffmt(lon, 2) * "/" * IG._ffmt(lat_, 2) * "/" * IG._ffmt(ang, 3) * ")"
	@test !isempty(IG._euler_segments(f.h, nm))
	@test !isempty(IG._euler_segments(f.h, "isoc1"))
	@test !isempty(IG._euler_segments(f.h, "isoc2"))
end

@testitem "Compute Euler pole dialog: lists lines, runs, and swaps to Hellinger" tags=[:gui] setup=[Plates, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	GmtvtkTest._register_ceuler_test()
	f = view_grid(Plates.grid())
	lat  = collect(36.0:0.1:38.5)
	iso1 = [(-9.5 .+ 0.4 .* sin.((lat .- 36) .* 2)) lat]
	iso2 = IG._ce_rotated_line(iso1, 20.0, 60.0, 3.0)
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(iso1), "isoc1")
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(iso2), "isoc2")

	@test ccall(_test_fn(:gmtvtk_ceuler_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	buf = Vector{UInt8}(undef, 512)
	rd(what) = (ccall(_test_fn(:gmtvtk_ceuler_read_test), Cint, (Cstring, Ptr{UInt8}, Cint),
	                  what, buf, Cint(512)); unsafe_string(pointer(buf)))
	set(what, v) = ccall(_test_fn(:gmtvtk_ceuler_set_test), Cint, (Cstring, Cstring), what, string(v))
	# Both window lines are on offer, and NOTHING is pre-selected — picking is the user's job.
	@test sort(split(rd("lines"), '\n')) == ["isoc1", "isoc2"]
	@test rd("line1") == "" && rd("line2") == ""

	for (k, v) in ("line1" => "isoc1", "line2" => "isoc2", "polelon" => "19", "polelat" => "59",
	               "poleang" => "2.9", "lonrange" => "6", "latrange" => "6", "angrange" => "0.6",
	               "nlon" => "13", "nlat" => "13", "nang" => "7")
		@test set(k, v) == 1
	end
	@test ccall(_test_fn(:gmtvtk_ceuler_compute_test), Cint, ()) == 1
	@test rd("running") == "1"                       # Compute is out, STOP is in while it searches
	t0 = time()
	while IG._CE_RUNNING[] && time() - t0 < 120;  sleep(0.05);  end
	sleep(0.8)                                       # the finishing push clears "running"
	@test rd("running") == "0"
	@test abs(parse(Float64, rd("poleang")) - 3.0) < 0.1
	@test abs(parse(Float64, rd("polelon")) - 20.0) < 1.5
	@test parse(Float64, rd("bfresidue")) < parse(Float64, rd("stresidue"))

	# "N*Delta" in an N-points box sets the RANGE too (Mirone's edit_nInt_CB): 100*0.1 is an even
	# number of intervals — 101 points — spanning 10 degrees.
	@test set("nlon", "100*0.1") == 1
	@test rd("nlon") == "101"
	@test parse(Float64, rd("lonrange")) ≈ 10.0 atol = 1e-6
	@test set("nlon", "12") == 1                     # a plain count is forced ODD
	@test rd("nlon") == "13"

	# The Hellinger switch relabels the N-points column into the DP tolerance, as the MATLAB one does.
	@test set("hellinger", "1") == 1
	@test rd("nintlabel") == "DP tolerance"
	@test set("hellinger", "0") == 1
	@test rd("nintlabel") == "N Points"
	ccall(_test_fn(:gmtvtk_ceuler_delete_dialog_test), Cvoid, ())
end

@testitem "Compute Euler pole: the Plates menu really carries the entry" tags=[:gui] setup=[Plates, GmtvtkTest] begin
	_test_fn = GmtvtkTest._test_fn
	f = view_grid(Plates.grid())
	# What the user picks: Geophysics > Plates, then Compute Euler pole…. Whether the dialog then
	# reaches the SCREEN cannot be asserted here — that hangs on the popup grab of a real, open menu,
	# which trigger() does not create — so this only pins that both entries exist and fire.
	trig(p) = ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), f.h, p)
	@test trig("Plates") == 1
	@test trig("Compute Euler pole") == 1
end

@testitem "Compute Euler pole dialog: visible on the FIRST open, and picks lines from the figure" tags=[:gui] setup=[Plates, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = view_grid(Plates.grid())
	lat  = collect(36.0:0.1:38.5)
	iso1 = [(-9.5 .+ 0.4 .* sin.((lat .- 36) .* 2)) lat]
	iso2 = IG._ce_rotated_line(iso1, 20.0, 60.0, 3.0)
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(iso1), "isoc1")
	IG._add_dataset_to_scene(f.h, IG.GMT.mat2ds(iso2), "isoc2")

	buf = Vector{UInt8}(undef, 512)
	rd(what) = (ccall(_test_fn(:gmtvtk_ceuler_read_test), Cint, (Cstring, Ptr{UInt8}, Cint),
	                  what, buf, Cint(512)); unsafe_string(pointer(buf)))
	set(what, v) = ccall(_test_fn(:gmtvtk_ceuler_set_test), Cint, (Cstring, Cstring), what, string(v))

	# The very FIRST open must put the dialog on screen (it used to take a second menu pick).
	@test ccall(_test_fn(:gmtvtk_ceuler_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
	@test rd("visible") == "1"
	@test rd("line1") == "" && rd("line2") == ""
	@test rd("pickdown") == "0"

	# "Pick lines from Figure" stays DOWN while picking; each double-click answer fills the next box
	# and the second one ends the pick by itself.
	@test set("pick", "1") == 1
	@test rd("pickdown") == "1"
	@test ccall(_test_fn(:gmtvtk_euler_pick_deliver_test), Cint, (Ptr{Cvoid}, Cstring), f.h, "isoc1") == 1
	@test rd("line1") == "isoc1" && rd("line2") == ""
	@test rd("pickdown") == "1"                      # still armed: the second line is missing
	@test ccall(_test_fn(:gmtvtk_euler_pick_deliver_test), Cint, (Ptr{Cvoid}, Cstring), f.h, "isoc1") == 1
	@test rd("line2") == ""                          # the same line cannot be both
	@test ccall(_test_fn(:gmtvtk_euler_pick_deliver_test), Cint, (Ptr{Cvoid}, Cstring), f.h, "isoc2") == 1
	@test rd("line2") == "isoc2"
	@test rd("pickdown") == "0"                      # both picked -> the button pops back up
	ccall(_test_fn(:gmtvtk_ceuler_delete_dialog_test), Cvoid, ())
end
