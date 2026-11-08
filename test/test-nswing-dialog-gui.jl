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
		@test dlg(g.h, "")["level"] == "1"
	finally
		GmtvtkTest._register_console_eval_test(false)
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
		g === nothing || ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), g.h)
	end
	for _ in 1:10; IG._pump_once(); sleep(0.02); end
end
