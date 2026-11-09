# :gui scenario for Geophysics > NSWING's dialog, built for real through gmtvtk_test.dll.
#
# A window with no grid of its own has no bathymetry: the dialog must start by asking for LEVEL 0 (the
# bathymetry file) — not level 1 — keep level 1 closed until it has one, and once that file is given
# fill "Time step (sec)" with the CFL estimate exactly as it does from a window's own grid (Mirone's
# tintol.m: min(dx,dy) in metres / sqrt(|zmin| g) / 2). A window that has a grid still starts on
# level 1. Opt in with INTERACTIVEGMT_TEST_GUI=1.

@testitem "NSWING dialog: an empty window asks for the level-0 bathymetry first, and it sets the time step" tags=[:gui, :nswing] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT
	_test_fn = GmtvtkTest._test_fn
	GmtvtkTest._register_console_eval_test(true)
	dlg(h, level0) = begin
		buf = zeros(UInt8, 2048)
		ccall(_test_fn(:gmtvtk_nswing_level0_test), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint), h, level0, buf, 2048)
		Dict(split(kv, '='; limit = 2)[1] => split(kv, '='; limit = 2)[2] for kv in split(unsafe_string(pointer(buf)), '|'))
	end

	# A geographic bathymetry, 1/60 degree cells, 4000 m at its deepest
	x = collect(-10.0:1/60:-9.0);  y = collect(36.0:1/60:37.0)
	G = GMT.mat2grid(Float32[-4000.0 + 3000.0 * (xi + 10.0) for yj in y, xi in x], x = x, y = y)
	G.proj4 = "+proj=longlat +datum=WGS84 +no_defs"
	tmp = mktempdir()
	bathy = joinpath(tmp, "layer0.nc")
	GMT.gmtwrite(bathy, deepcopy(G))

	f = IG.iview()                                       # the empty launcher: no grid of its own
	g = nothing
	try
		r = dlg(f.h, "")
		@test r["level"] == "0"
		@test r["enabled"] == "1"
		@test r["prompt"] == "Enter the bathymetry grid (level 0)"

		r = dlg(f.h, bathy)                              # typed into the Nest box, the real textChanged path
		dt = 1/60 * 111000.0 / sqrt(4000.0 * 9.8) / 2.0
		@test r["dt"] == string(round(dt, digits = 3))
		@test r["geog"] == "1"
		@test r["next"] == "1"                           # level 1 opens once level 0 is there

		# ...and RUN takes that file as the bathymetry of the run (the in-memory run, not a file run)
		src = joinpath(tmp, "source.nc")
		GMT.gmtwrite(src, GMT.mat2grid(zeros(Float32, length(y), length(x)), x = x, y = y))
		d = IG._nswing_parse("source=$src\nnestL0=$bathy\nlevel=0\noutmode=grids\nname=$(joinpath(tmp, "tsu"))\ngrn=10\nncycles=10\ndt=4\n")
		opts, msgs, nests, base, s = IG._nswing_validate(f.h, d)
		@test base isa GMT.GMTgrid && size(IG._zmat(base)) == (length(y), length(x))
		@test isempty(nests)                             # level 0 is never a nest

		g = IG.viz(GMT.peaks())                          # a window WITH a grid: level 0 is that grid
		r = dlg(g.h, "")
		@test r["level"] == "1"
		# ...and level 0 NAMES it: the window's base grid, before "In memory grid"
		nm = IG._scene_state(g)["surf_name"]
		@test !isempty(nm) && r["level0text"] == "$nm — In memory grid"
	finally
		GmtvtkTest._register_console_eval_test(false)
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
		g === nothing || ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), g.h)
	end
	for _ in 1:10; IG._pump_once(); sleep(0.02); end
end

# A nest that breaks the rule is not only TOLD, it is DRAWN: the RUN pre-flight's error carries the
# geometry line, the C++ popup parses it into one panel per offending corner. Here the lower-left
# corner obeys and the upper-right one does not (ratio 5, X_MAX and Y_MAX half-misplaced).
@testitem "NSWING dialog: a nesting violation is drawn, corner as it is vs as the rule requires" tags=[:gui, :nswing] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT
	P = GMT.mat2grid(zeros(Float32, 21, 21), x = collect(0.0:1.0:20.0), y = collect(0.0:1.0:20.0))
	x = collect(5.6:0.2:15.0);  y = collect(5.6:0.2:14.6)
	C = GMT.mat2grid(zeros(Float32, length(y), length(x)), x = x, y = y)
	txt = try IG._nswing_dialog_door(() -> IG._nswing_check_nest_fits(P, C, "parent", "child")); "" catch e; sprint(showerror, e) end
	@test occursin("X_MAX should be", txt) && !occursin("X_MIN should be", txt)
	g = split(txt[findfirst("@@NESTGEOM", txt)[end]+1:end])
	@test parse(Int, g[end]) == 4 | 8                     # X_MAX + Y_MAX, nothing at the obeying corner
	@test parse.(Float64, g[11:14]) ≈ [5.6, 5.6, 15.4, 14.4]
	@test sprint(showerror, IG.NestViolation("m", "g")) == "m"   # a run's log reads as before
	# a daughter NOT finer than its parent: said as such, no corner verdicts, nothing to draw
	S = GMT.mat2grid(zeros(Float32, 11, 11), x = collect(5.0:1.0:15.0), y = collect(5.0:1.0:15.0))
	same = try IG._nswing_dialog_door(() -> IG._nswing_check_nest_fits(P, S, "parent", "child")); "" catch e; sprint(showerror, e) end
	@test occursin("not finer than its parent", same) && !occursin("should be", same) && !occursin("@@NESTGEOM", same)
	f = IG.iview()
	try
		@test ccall(GmtvtkTest._test_fn(:gmtvtk_nest_rules_view_test), Cint, (Cstring, Cstring), txt, "") == 1
		@test ccall(GmtvtkTest._test_fn(:gmtvtk_nest_rules_view_test), Cint, (Cstring, Cstring), same, "") == 0
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
	for _ in 1:10; IG._pump_once(); sleep(0.02); end
end
