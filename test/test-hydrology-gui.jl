# Grid Tools > Drainage analysis… (deps/src/76_hydrology.cpp, src/hydrology.jl). Driven the way a user
# drives it: the menu entry opens the dialog, boxes are ticked and Run is CLICKED; the dialog's own
# timer then follows the run and adds the results to the window.
@testitem "Drainage analysis: menu, Run, results in the window" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	pump(n) = for _ in 1:n  IG._pump_once()  end
	# A tilted valley with two side valleys: one basin draining south, plus a pit to fill.
	nx, ny = 120, 90
	x = collect(range(-8.5, -8.0, length = nx));  y = collect(range(40.0, 40.4, length = ny))
	Z = Float32[200 + 300 * (yy - 40.0) + 400 * abs(xx + 8.25) + 20 * sin(15 * xx) for yy in y, xx in x]
	Z[45, 60] -= 50
	G = IG.GMT.mat2grid(Z; x = x, y = y, proj4 = "+proj=longlat +datum=WGS84")
	fig = IG.iview(G)
	h = getfield(fig, :h)
	W(obj, what, txt = "") = ccall(_test_fn(:gmtvtk_toplevel_widget_test), Cint, (Cstring, Cstring, Cint, Cstring),
	                               "Drainage analysis", obj, Cint(what), txt)
	try
		pump(20)
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), h, "Drainage analysis") >= 1
		pump(10)
		@test W("edStreamArea", 0, "50") == 1
		@test W("edBasinArea", 0, "50") == 1
		@test W("chkTwi", 2) == 1
		@test W("btnRun", 1) == 1
		# Wait for the dialog to collect the run (its timer adds the results), at most 60 s.
		t0 = time()
		while time() - t0 < 60
			pump(5);  sleep(0.05)
			isempty(IG._HYDRO_JOBS) && any(n -> occursin("wetness index", n), [n for (k, n, o) in get(IG._SCENE_OBJS, h, [])]) && break
		end
		names = [n for (k, n, o) in get(IG._SCENE_OBJS, h, [])]
		@test any(n -> occursin("drainage basins", n), names)
		@test any(n -> occursin("wetness index", n), names)
		B = only(o for (k, n, o) in IG._SCENE_OBJS[h] if occursin("drainage basins", n))
		# Same geometry as the source grid (derived-from-a-grid law), and every land cell labelled.
		@test B.x == G.x && B.y == G.y && B.inc == G.inc && B.registration == G.registration
		@test count(!isnan, B.z) > 0.9 * length(B.z)
		# The basin limits are vectorized and plotted, like the rivers: a lines group with its own row.
		@test any(n -> occursin("basin outlines", n), [n for (k, n, o) in get(IG._SCENE_OBJS, h, [])]) ||
		      occursin("basin outlines", unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), h)))
		# PURE 2-D data: the basin outlines are x,y only, so Save offers x,y and nothing else — even with
		# a grid under them (it used to default to x,y,z sampled from that grid). The rivers carry a REAL z
		# (streamnet: x, y, z, order), so they keep the x,y,z choice.
		src = replace(only(n for n in names if occursin("drainage basins", n)), " drainage basins" => "")
		offers3d(nm) = ccall(_test_fn(:gmtvtk_line_save_offers3d_test), Cint, (Ptr{Cvoid}, Cstring), h, nm)
		@test offers3d("$src basin outlines") == 0
		@test offers3d("$src streams") >= 1
		# Paint each river system in its own colour: one colour per reach, every reach the colour of the
		# reach it flows into; off = back to the line colour.
		nm = "$src streams"
		S = IG._HYDRO_RIVERS[h][nm]
		segcol(k) = (c = zeros(UInt8, 3); ccall(_test_fn(:gmtvtk_overlay_seg_colors_test), Cint, (Ptr{Cvoid}, Cstring, Cint, Ptr{UInt8}), h, nm, Cint(k - 1), c); c)
		nsegcol() = ccall(_test_fn(:gmtvtk_overlay_seg_colors_test), Cint, (Ptr{Cvoid}, Cstring, Cint, Ptr{UInt8}), h, nm, Cint(-1), C_NULL)
		@test nsegcol() == 0
		@test IG._hydro_paint_systems(h, nm, true) == ""
		@test nsegcol() == length(S)
		starts = Dict((d.data[1, 1], d.data[1, 2]) => k for (k, d) in enumerate(S))
		for k in eachindex(S)
			j = get(starts, (S[k].data[end, 1], S[k].data[end, 2]), 0)
			j == 0 || @test segcol(k) == segcol(j)
		end
		@test IG._hydro_paint_systems(h, nm, false) == ""
		@test nsegcol() == 0
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
		pump(5)
	end
end

# The Julia doors alone: a run can be stopped, and a bad request is refused with a message.
@testitem "Drainage analysis: Stop and refusals" tags=[:gui] begin
	IG = InteractiveGMT
	pump(n) = for _ in 1:n  IG._pump_once()  end
	G = IG.GMT.mat2grid(rand(Float32, 1500, 1500))
	fig = IG.iview(G)
	h = getfield(fig, :h)
	try
		pump(10)
		@test IG._hydro_run(h, "basins=0;streams=0") == "Tick at least one result."
		waitjob() = (r = ""; for _ in 1:1200; r = IG._hydro_poll(h); startswith(r, "run") || break; sleep(0.05); end; r)
		# km² on a grid with no georeferencing: the run starts, and fails saying why.
		@test IG._hydro_run(h, "basins=1;unit=km2;basin_area=1") == ""
		r = waitjob()
		@test startswith(r, "fail") && occursin("km²", r)
		@test any(e -> occursin("km²", e), IG._take_tool_errors!())    # the refusal is reported, and claimed here
		# Stop: the run ends as cancelled, nothing to adopt.
		@test IG._hydro_run(h, "basins=1") == ""
		IG._hydro_stop(h)
		@test startswith(waitjob(), "cancel")
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
		pump(5)
	end
end

# The dialog PARKS like every tool window: the X, minimise and Esc hide it into this window's Scene
# Objects, the menu brings the same one back, and only the parked row's Delete ends it.
@testitem "Drainage analysis dialog: X, minimise and Esc park it, Delete ends it" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = IG.iview(IG.GMT.mat2grid(rand(Float32, 60, 50)))
	h = getfield(f, :h)
	parked() = ccall(_test_fn(:gmtvtk_hydro_parked_test), Cint, (Ptr{Cvoid},), h)
	opened() = ccall(_test_fn(:gmtvtk_hydro_open_dialog_test), Cint, (Ptr{Cvoid},), h)
	leave(how) = ccall(_test_fn(:gmtvtk_hydro_leave_dialog_test), Cvoid, (Ptr{Cvoid}, Cint), h, how)
	rows() = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), h))
	try
		@test opened() == 1 && parked() == 0
		@test opened() == 1 && parked() == 0          # ONE dialog: the second open brings back the first
		for how in (0, 1, 2)                          # the X, minimise, Esc
			leave(how)
			@test parked() == 1
			@test occursin("Drainage analysis", rows())
			@test opened() == 1 && parked() == 0
		end
		leave(0)
		@test ccall(_test_fn(:gmtvtk_hydro_delete_dialog_test), Cint, (Ptr{Cvoid},), h) == 1
		@test parked() == -1
		@test !occursin("Drainage analysis", rows())
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
	end
	for _ in 1:10; IG._pump_once(); sleep(0.02); end
end

# The DEM as iGMT reads it — "TRB", row-major with (ny,nx) dims — must give the SAME flow network as
# the same file read column-major. GMT.jl's hydro reader takes (ny,nx) as column-major, so a
# non-square TRB DEM used to be computed on a scrambled grid ("pure trash" on algarve50.grd).
@testitem "Drainage analysis: a TRB-read DEM gives the same network as a BCB one" tags=[:unit] begin
	IG = InteractiveGMT
	nx, ny = 130, 70                              # NOT square: a square grid hid the bug
	x = collect(range(-8.5, -8.0, length = nx));  y = collect(range(40.0, 40.3, length = ny))
	Z = Float32[300 + 80 * sin(30 * xx) * cos(25 * yy) + 50 * sin(70 * xx + 3yy) + 200 * (yy - 40) for yy in y, xx in x]
	f = tempname() * ".grd"
	try
		IG.GMT.gmtwrite(f, IG.GMT.mat2grid(Z; x = x, y = y))
		G = IG._gmtread_trb(f)
		@test G.layout[1:2] == "TR"
		H = IG.GMT.gmtread(f)
		F = IG.GMT.flowdir(IG._hydro_dem(G))
		F2 = IG.GMT.flowdir(H)
		@test isequal(F.dem, F2.dem)
		@test F.rcv == F2.rcv
	finally
		rm(f; force = true)
	end
end

# What drainagebasins drops (basins under the min. area — along a coast, thousands of tiny catchments
# draining straight to the sea) is ONE class, 0, with its own outline — never left blank.
@testitem "Drainage analysis: dropped catchments form the class 0 with an outline" tags=[:unit] begin
	IG = InteractiveGMT
	nx, ny = 200, 150
	x = collect(range(-8.5, -8.0, length = nx));  y = collect(range(40.0, 40.4, length = ny))
	Z = Float32[300 + 80 * sin(30 * xx) * cos(25 * yy) + 50 * sin(70 * xx + 3yy) + 200 * (yy - 40) for yy in y, xx in x]
	Z[1:10, 1:20] .= NaN32                         # no-data cells must stay NaN
	G = IG.GMT.mat2grid(Z; x = x, y = y)
	F = IG.GMT.flowdir(IG._hydro_dem(G))
	B = IG.GMT.drainagebasins(F; minarea = 2000)
	nb = count(isnan, B.z)
	@test nb > 200                                 # the filter really dropped land
	IG._hydro_residual_class!(B, F)
	@test count(isnan, B.z) == count(isnan, F.dem) # only no-data is left blank
	@test count(==(0.0f0), B.z) == nb - count(isnan, F.dem)
	O = IG._hydro_basin_outlines(B)
	@test any(d -> d.attrib["Px"] == "0", O)
	@test all(d -> parse(Float64, d.attrib["Px"]) >= 0, O)
	@test IG._hydro_basin_color(0) == (0.82, 0.82, 0.82)
	# One flat colour per class: two nodes per class, both that class's colour, the step half way.
	cz, crgb, n = IG._class_nodes(B, IG._hydro_basin_color)
	nb = round(Int, maximum(filter(!isnan, B.z)))
	@test n == 2 * (nb + 1) && cz[1] == -0.5 && cz[2] < 0.5 && cz[3] == 0.5
	@test crgb[1:3] == crgb[4:6] == [0.82, 0.82, 0.82]
	@test all(k -> crgb[6k+1:6k+3] == crgb[6k+4:6k+6] == collect(IG._hydro_class_color(k)), 1:nb)
	@test IG._hydro_class_color(300) == IG._hydro_class_color(300 - 256)   # any number of classes
end

# The rivers go through the line simplification: no vertex per cell along straight runs. What stays
# are rows of the reach itself (Douglas-Peucker never moves a vertex), its ends included, with z and
# Strahler order — so the network stays connected and keeps its columns.
@testitem "Drainage analysis: rivers are simplified, ends and columns kept" tags=[:unit] begin
	IG = InteractiveGMT
	nx, ny = 200, 150
	x = collect(range(-8.5, -8.0, length = nx));  y = collect(range(40.0, 40.4, length = ny))
	Z = Float32[300 + 80 * sin(30 * xx) * cos(25 * yy) + 50 * sin(70 * xx + 3yy) + 200 * (yy - 40) for yy in y, xx in x]
	G = IG.GMT.mat2grid(Z; x = x, y = y)
	F = IG.GMT.flowdir(IG._hydro_dem(G))
	S0 = IG.GMT.streamnet(F; threshold = 50)
	S = deepcopy(S0)
	IG._hydro_simplify_lines!(S, 0.5 * G.inc[1])
	@test sum(d -> size(d.data, 1), S) < sum(d -> size(d.data, 1), S0)
	for (a, b) in zip(S0, S)
		@test b.data[1, :] == a.data[1, :] && b.data[end, :] == a.data[end, :]
		@test size(b.data, 2) == 4 && b.colnames == ["x", "y", "z", "order"]
		@test all(r -> any(k -> a.data[k, :] == b.data[r, :], 1:size(a.data, 1)), 1:size(b.data, 1))
	end
end

# "Save this river and its upstream network…": the reach clicked plus EVERY reach that drains into it,
# and nothing else — checked against the downstream walk of every reach — written as a GMT multisegment
# table and as a GeoPackage, one segment per reach, read back whole.
@testitem "Drainage analysis: a river saved with its whole upstream network" tags=[:unit] begin
	IG = InteractiveGMT
	nx, ny = 200, 150
	x = collect(range(-8.5, -8.0, length = nx));  y = collect(range(40.0, 40.4, length = ny))
	Z = Float32[300 + 80 * sin(30 * xx) * cos(25 * yy) + 50 * sin(70 * xx + 3yy) + 200 * (yy - 40) for yy in y, xx in x]
	G = IG.GMT.mat2grid(Z; x = x, y = y, proj4 = "+proj=longlat +datum=WGS84")
	F = IG.GMT.flowdir(IG._hydro_dem(G))
	S = IG._hydro_simplify_lines!(IG.GMT.streamnet(F; threshold = 30), 0.5 * G.inc[1])
	n = length(S)
	# downstream walk: the reach starting where reach k ends
	starts = Dict((d.data[1, 1], d.data[1, 2]) => k for (k, d) in enumerate(S))
	down(k) = get(starts, (S[k].data[end, 1], S[k].data[end, 2]), 0)
	drains_into(k, i) = (while k != 0; k == i && return true; k = down(k); end; false)
	for i in 1:n
		up = IG._hydro_upstream(S, i)
		@test up[1] == i
		@test Set(up) == Set(k for k in 1:n if drains_into(k, i))
	end
	i = argmax([length(IG._hydro_upstream(S, k)) for k in 1:n])       # the reach with the biggest network
	nup = length(IG._hydro_upstream(S, i))
	@test nup > 3
	h = Ptr{Cvoid}(UInt(0x1234))                                       # a window key; nothing is drawn
	IG._HYDRO_RIVERS[h] = Dict("DEM streams" => S)
	d = S[i].data
	try
		for ext in ("txt", "gpkg", "shp")
			f = tempname() * "." * ext
			try
				r = IG._hydro_save_upstream(h, "DEM streams", i - 1, d[1, 1], d[1, 2], d[end, 1], d[end, 2], f)
				@test startswith(r, "ok|")
				R = IG._gmtread(f)
				@test (R isa Vector ? length(R) : 1) == nup
			finally
				rm(f; force = true)
			end
		end
		# a wrong index still finds the river by its ends
		r = IG._hydro_save_upstream(h, "DEM streams", n + 5, d[1, 1], d[1, 2], d[end, 1], d[end, 2], tempname() * ".txt")
		@test startswith(r, "ok|")
	finally
		delete!(IG._HYDRO_RIVERS, h)
	end
end

# The river SYSTEMS: every reach is in the system of the reach it flows into, each outlet reach heads a
# system of its own, and the systems are numbered by size (1 = most reaches).
@testitem "Drainage analysis: river systems" tags=[:unit] begin
	IG = InteractiveGMT
	nx, ny = 200, 150
	x = collect(range(-8.5, -8.0, length = nx));  y = collect(range(40.0, 40.4, length = ny))
	Z = Float32[300 + 80 * sin(30 * xx) * cos(25 * yy) + 50 * sin(70 * xx + 3yy) + 200 * (yy - 40) for yy in y, xx in x]
	G = IG.GMT.mat2grid(Z; x = x, y = y)
	S = IG._hydro_simplify_lines!(IG.GMT.streamnet(IG.GMT.flowdir(IG._hydro_dem(G)); threshold = 30), 0.5 * G.inc[1])
	sys = IG._hydro_systems(S)
	starts = Dict((d.data[1, 1], d.data[1, 2]) => k for (k, d) in enumerate(S))
	down(k) = get(starts, (S[k].data[end, 1], S[k].data[end, 2]), 0)
	outlets = [k for k in eachindex(S) if down(k) == 0]
	@test all(k -> down(k) == 0 || sys[k] == sys[down(k)], eachindex(S))
	@test sort(sys[outlets]) == collect(1:length(outlets))
	sizes = [count(==(v), sys) for v in 1:length(outlets)]
	@test issorted(sizes, rev = true)
	@test all(o -> count(==(sys[o]), sys) == length(IG._hydro_upstream(S, o)), outlets)
end
