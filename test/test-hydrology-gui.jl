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
