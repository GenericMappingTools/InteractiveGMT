# :gui scenarios for the GMT-menu module dialogs (grdtrend, grdlandmask, grdfilter), the Grid
# calculator and show_table. These open a REAL window through the built gmtvtk.dll and drive each
# module's callback with exactly the "key=value" block its dialog sends, so the WHOLE path runs:
# option assembly -> live GMT -> the shared _gm3d_deliver tail that puts the result into Scene
# Objects. Opt in with INTERACTIVEGMT_TEST_GUI=1 (or `Pkg.test(test_args=["gui"])`).
#
# Results are checked through _SCENE_OBJS — what File>Save and every "selected" lookup see — so a
# module that computed something but failed to DELIVER it fails here too.

@testmodule GmtModules begin

using InteractiveGMT
const IG = InteractiveGMT

# The grid every item below fits/filters: a smooth bump plus short-wavelength noise, on a GEOGRAPHIC
# footprint (off Portugal) so the degrees-based distance flags are legal and there is land AND sea.
function grid()
	IG.GMT.mat2grid(
		Float32[1000exp(-(((ix - 20) / 8)^2 + ((iy - 15) / 6)^2)) + 60sin(ix * 0.9)cos(iy * 1.1)
		        for iy in 0:30, ix in 0:40];
		x = collect(range(-10.0, -6.0, length = 41)), y = collect(range(36.0, 39.0, length = 31)),
		proj4 = "+proj=longlat +datum=WGS84")
end

# Drive one module callback with the newline-separated block its dialog sends.
send(fn, h, kv) = fn(h, Base.unsafe_convert(Cstring, Base.cconvert(Cstring, join(kv, "\n"))))

# Scene Objects labels of the grids the window holds, in add order.
names(h) = [t[2] for t in get(IG._SCENE_OBJS, h, Tuple{Symbol,String,Any}[]) if t[1] === :grid]
# The grid delivered under `name` (nothing when the module never delivered it).
grid_named(h, name) = IG._find_object(h, :grid, name)
# Mean |dz/dx|: a low-pass filter must lower it.
roughness(A) = sum(abs, diff(Float64.(A), dims = 2)) / length(A)

end # @testmodule

@testitem "grdtrend delivers trend / residuals / weights" tags=[:gui] setup=[GmtModules] begin
	IG = InteractiveGMT
	f = viz(GmtModules.grid())
	call(kv) = GmtModules.send(IG._on_grdtrend, f.h, kv)
	try
		@test call(["what=trend", "model=3", "robust=0", "protectnans=0", "axis=", "grid="]) == 1
		IG._pump_once()
		R = GmtModules.grid_named(f.h, "Trend (n=3)")
		@test R !== nothing && size(R.z) == size(f.G.z)

		@test call(["what=diff", "model=3", "robust=0", "protectnans=0", "axis=", "grid="]) == 1
		D = GmtModules.grid_named(f.h, "Residuals (n=3)")
		@test D !== nothing
		@test abs(sum(D.z) / length(D.z)) < abs(sum(f.G.z) / length(f.G.z))   # residuals are centred

		@test call(["what=weights", "model=3", "robust=1", "protectnans=0", "axis=", "grid="]) == 1
		W = GmtModules.grid_named(f.h, "Weights (n=3)")
		@test W !== nothing
		w = filter(!isnan, W.z)
		@test maximum(w) <= 1.001 && minimum(w) > 0        # robust weights live in (0, 1]

		# a 1-D (+x) fit really is constant along the other axis
		@test call(["what=trend", "model=2", "robust=0", "protectnans=0", "axis=x", "grid="]) == 1
		X = GmtModules.grid_named(f.h, "Trend (n=2)")
		@test X !== nothing
		@test maximum(X.z[:, 1]) - minimum(X.z[:, 1]) < 1e-4
		@test maximum(X.z[1, :]) - minimum(X.z[1, :]) > 1e-2
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "grdtrend Protect NaNs puts the input's holes back" tags=[:gui] setup=[GmtModules] begin
	IG = InteractiveGMT
	G = GmtModules.grid();  G.z[5, 5] = NaN32;  G.z[6, 6] = NaN32;  G.hasnans = 2
	f = viz(G)
	call(kv) = GmtModules.send(IG._on_grdtrend, f.h, kv)
	try
		# The trend is a polynomial: it covers the input's holes unless we ask for them back.
		@test call(["what=trend", "model=3", "robust=0", "protectnans=0", "axis=", "grid="]) == 1
		@test count(isnan, GmtModules.grid_named(f.h, "Trend (n=3)").z) == 0
		@test call(["what=trend", "model=6", "robust=0", "protectnans=1", "axis=", "grid="]) == 1
		@test count(isnan, GmtModules.grid_named(f.h, "Trend (n=6)").z) == 2
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "grdlandmask builds a mask and masks the window's grid" tags=[:gui] setup=[GmtModules] begin
	IG = InteractiveGMT
	f = viz(GmtModules.grid())
	call(kv) = GmtModules.send(IG._on_grdlandmask, f.h, kv)
	try
		@test call(["clip=0", "region=-10/-6/36/39", "inc=0.1", "res=l",
		            "maskvalues=0/1", "pixel=0", "verbose=0", "grid="]) == 1
		IG._pump_once()
		M = GmtModules.grid_named(f.h, "Land mask")
		@test M !== nothing && sort(unique(M.z)) == Float32[0, 1]   # this box holds land AND sea

		# Second method: the mask is applied to the window's own grid, so it keeps that grid's shape
		# and punches NaN holes where the first node value says so.
		@test call(["clip=1", "res=l", "maskvalues=NaN/1", "pixel=0", "verbose=0", "grid="]) == 1
		C = GmtModules.grid_named(f.h, "Masked grid")
		@test C !== nothing && size(C.z) == size(f.G.z)
		@test count(isnan, C.z) > 0 && count(!isnan, C.z) > 0
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "grdlandmask dialog: X, minimise and Esc park it, Delete ends it" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = viz(GmtModules.grid())
	parked() = ccall(_test_fn(:gmtvtk_landmask_parked_test), Cint, (Ptr{Cvoid},), f.h)
	opened() = ccall(_test_fn(:gmtvtk_landmask_open_dialog_test), Cint, (Ptr{Cvoid},), f.h)
	leave(how) = ccall(_test_fn(:gmtvtk_landmask_leave_dialog_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, how)
	rows() = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	try
		# Opening twice yields ONE dialog: the second call brings back the first.
		@test opened() == 1 && parked() == 0
		@test opened() == 1 && parked() == 0
		# Every way out parks it, with a row in this window's Scene Objects; the menu brings it back.
		for how in (0, 1, 2)                     # the X, minimise, Esc
			leave(how)
			@test parked() == 1
			@test occursin("grdlandmask", rows())
			@test opened() == 1 && parked() == 0
		end
		# Only the parked row's own "Delete" ends it, and the row goes with it.
		leave(0)
		@test ccall(_test_fn(:gmtvtk_landmask_delete_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
		@test parked() == -1
		@test !occursin("grdlandmask", rows())
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
	# closed: the test dll must have given the borrowed window back (see test-dgt-gui.jl)
	for _ in 1:10; IG._pump_once(); sleep(0.02); end
	@test ccall(_test_fn(:gmtvtk_scene_borrowed_test), Cint, (Ptr{Cvoid},), f.h) == 0
end

@testitem "grdblend blends a list of grids and a blend file" tags=[:gui] setup=[GmtModules] begin
	IG = InteractiveGMT
	d = mktempdir()
	a = joinpath(d, "a.grd");  b = joinpath(d, "b.grd")
	IG.GMT.gmtwrite(a, IG.GMT.grdmath("-R0/5/0/5 -I0.5 X Y ADD"))
	IG.GMT.gmtwrite(b, IG.GMT.grdmath("-R4/9/1/6 -I0.5 X Y MUL"))
	f = viz(GmtModules.grid())
	call(kv) = GmtModules.send(IG._on_grdblend, f.h, kv)
	try
		# the map's header read: one line per path, empty for a non-grid
		@test IG._grdblend_headers([a, b, joinpath(d, "nope.grd")])[1:2] ==
		      ["0.0 5.0 0.0 5.0 0.5 0.5 11 11 0 0", "4.0 9.0 1.0 6.0 0.5 0.5 11 11 0 0"]
		@test IG._grdblend_headers([joinpath(d, "nope.grd")]) == [""]
		@test call(["files=$a\t$b", "region=0/9/0/6", "inc=0.5", "pixel=0", "mode=", "weights=",
		            "geog=0", "verbose=0"]) == 1
		B = GmtModules.grid_named(f.h, "Blended grid")
		@test B !== nothing && IG.GMT.getsize(B) == (19, 13)
		@test count(isnan, B.z) > 0 && count(!isnan, B.z) > 0       # (0..4, 5..6) is nobody's
		# clobber "uppermost", no-data filled, and the weights output under its own name
		@test call(["files=$a\t$b", "region=0/9/0/6", "inc=0.5", "mode=u", "sign=p", "nodata=-1",
		            "weights="]) == 1
		@test count(isnan, GmtModules.grid_named(f.h, "Blended grid").z) == 0
		@test call(["files=$a\t$b", "region=0/9/0/6", "inc=0.5", "mode=", "weights=w"]) == 1
		@test GmtModules.grid_named(f.h, "Blend weights") !== nothing
		# a blend file in place of the list
		job = joinpath(d, "blend.job")
		write(job, "$a - 1\n$b - 2\n")
		@test call(["files=", "blendfile=$job", "region=0/9/0/6", "inc=0.5"]) == 1
		# the inputs' referencing system survives (GMT's grdblend drops it), and two systems are refused
		utm = "+proj=utm +zone=29 +datum=WGS84 +units=m +no_defs"
		ra = joinpath(d, "ra.grd");  rb = joinpath(d, "rb.grd");  rc = joinpath(d, "rc.grd")
		for (p, w, e, srs) in ((ra, 0, 5, utm), (rb, 4, 9, utm), (rc, 4, 9, "+proj=utm +zone=30 +datum=WGS84 +units=m +no_defs"))
			G = IG.GMT.grdmath("-R$w/$e/0/5 -I0.5 X Y ADD");  G.proj4 = srs
			IG.GMT.gmtwrite(p, G)
		end
		@test call(["files=$ra\t$rb", "region=0/9/0/5", "inc=0.5"]) == 1
		@test occursin("zone=29", GmtModules.grid_named(f.h, "Blended grid").proj4)
		refused(kv) = IG._errored(call(kv), "grdblend") == 0
		@test refused(["files=$ra\t$rc", "region=0/9/0/5", "inc=0.5"])
		# no referencing next to lon/lat: taken along when its limits look like lon/lat, else refused;
		# next to projected grids it is always refused (nothing tells its system)
		geo = "+proj=longlat +datum=WGS84 +no_defs"
		ga = joinpath(d, "ga.grd");  gb = joinpath(d, "gb.grd");  gx = joinpath(d, "gx.grd")
		G = IG.GMT.grdmath("-R0/5/0/5 -I0.5 X Y ADD");  G.proj4 = geo;  IG.GMT.gmtwrite(ga, G)
		IG.GMT.gmtwrite(gb, IG.GMT.grdmath("-R4/9/0/5 -I0.5 X Y MUL"))            # bare, lon/lat-ish
		IG.GMT.gmtwrite(gx, IG.GMT.grdmath("-R400/900/0/500 -I50 X Y MUL"))       # bare, not lon/lat
		@test call(["files=$ga\t$gb", "region=0/9/0/5", "inc=0.5"]) == 1
		@test occursin("longlat", GmtModules.grid_named(f.h, "Blended grid").proj4)
		@test refused(["files=$ga\t$gx", "region=0/900/0/500", "inc=50"])
		@test refused(["files=$ra\t$gb", "region=0/9/0/5", "inc=0.5"])
		# two GeoTIFF tiles (pixel registered): GMT's own reformat/resample of such inputs garbles the
		# blend on Windows, so they go over in memory — every node of both tiles lands, values intact
		t1 = joinpath(d, "t1.tif");  t2 = joinpath(d, "t2.tif")
		G1 = IG.GMT.grdmath("-R0/10/0/10 -I1 -r X Y ADD");  G1.proj4 = utm;  IG.GMT.gmtwrite(t1, G1)
		G2 = IG.GMT.grdmath("-R0/10/10/20 -I1 -r X Y MUL"); G2.proj4 = utm;  IG.GMT.gmtwrite(t2, G2)
		@test call(["files=$t1\t$t2", "region=0/10/0/20", "inc=1", "pixel=1"]) == 1
		T = GmtModules.grid_named(f.h, "Blended grid")
		@test count(!isnan, T.z) == 200 && occursin("zone=29", T.proj4)
		@test IG.GMT.grdtrack(T, [2.5 3.5; 7.5 15.5]).data[:, 3] ≈ [2.5 + 3.5, 7.5 * 15.5]
		@test call(["files=$t1\t$t2", "region=0/10/0/20", "inc=1", "pixel=0"]) == 1   # gridline out
		T = GmtModules.grid_named(f.h, "Blended grid")
		@test T.registration == 0 && count(!isnan, T.z) >= 200
		@test IG.GMT.grdtrack(T, [3.0 4.0; 7.0 15.0]).data[:, 3] ≈ [3.0 + 4.0, 7.0 * 15.0] atol = 1e-3
		# refusals, each claimed: one grid only, a missing grid, no region
		@test refused(["files=$a", "region=0/9/0/6", "inc=0.5"])
		@test refused(["files=$a\t$(joinpath(d, "nope.grd"))", "region=0/9/0/6", "inc=0.5"])
		@test refused(["files=$a\t$b", "region=///", "inc=0.5"])
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
		rm(d; recursive = true, force = true)
	end
end

@testitem "grdblend dialog: map picks fill the list, park, Delete" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	GmtvtkTest._register_grdblend_test()
	d = mktempdir()
	a = joinpath(d, "a.grd");  b = joinpath(d, "b.grd")
	IG.GMT.gmtwrite(a, IG.GMT.grdmath("-R0/5/0/5 -I0.5 X Y ADD"))
	IG.GMT.gmtwrite(b, IG.GMT.grdmath("-R4/9/1/6 -I0.25 X Y MUL"))
	f = viz(GmtModules.grid())
	parked() = ccall(_test_fn(:gmtvtk_grdblend_parked_test), Cint, (Ptr{Cvoid},), f.h)
	opened() = ccall(_test_fn(:gmtvtk_grdblend_open_dialog_test), Cint, (Ptr{Cvoid},), f.h)
	leave(how) = ccall(_test_fn(:gmtvtk_grdblend_leave_dialog_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, how)
	pick(i, mode) = ccall(_test_fn(:gmtvtk_grdblend_pick_test), Cint, (Ptr{Cvoid}, Cint, Cint), f.h, i, mode)
	region() = unsafe_string(ccall(_test_fn(:gmtvtk_grdblend_region_test), Cstring, (Ptr{Cvoid},), f.h))
	rows() = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
	try
		@test opened() == 1 && parked() == 0
		# entering the directory scans it: no button to press
		@test ccall(_test_fn(:gmtvtk_grdblend_scan_test), Cint, (Ptr{Cvoid}, Cstring), f.h, d) == 2
		@test ccall(_test_fn(:gmtvtk_grdblend_feed_test), Cint, (Ptr{Cvoid}, Cstring), f.h,
		            "$a\n$b\n$(joinpath(d, "nope.grd"))") == 2          # the non-grid gets no box
		@test pick(-1, 1) == 2                                         # a band over both
		@test region() == "0/9/0/6|0.25"                               # union, finest spacing
		@test pick(0, 0) == 1                                          # a click takes one off...
		@test pick(0, 0) == 2                                          # ...and puts it back
		@test pick(0, 1) == 2                                          # adding twice lists it once
		@test pick(-1, 2) == 0                                         # Ctrl+band takes all off
		# arrow keys pan the map: the view moves toward the arrow, Shift by more
		key(k, sh) = (xy = zeros(2); ccall(_test_fn(:gmtvtk_grdblend_key_test), Cint,
		              (Ptr{Cvoid}, Cint, Cint, Ptr{Cdouble}), f.h, k, sh, xy); xy)
		c0 = key(-1, 0)
		c1 = key(1, 0);  @test c1[1] > c0[1] && c1[2] == c0[2]          # Right: east
		c2 = key(2, 0);  @test c2[2] > c1[2] && c2[1] == c1[1]          # Up: north
		c3 = key(0, 1);  @test c1[1] - c0[1] < c2[1] - c3[1]            # Shift+Left: a bigger step west
		c4 = key(3, 0);  @test c4[2] < c3[2]                            # Down: south
		for how in (0, 1, 2)                                           # the X, minimise, Esc
			leave(how)
			@test parked() == 1
			@test occursin("grdblend", rows())
			@test opened() == 1 && parked() == 0
		end
		leave(0)
		@test ccall(_test_fn(:gmtvtk_grdblend_delete_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
		@test parked() == -1
		@test !occursin("grdblend", rows())
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
		rm(d; recursive = true, force = true)
	end
	for _ in 1:10; IG._pump_once(); sleep(0.02); end
	@test ccall(_test_fn(:gmtvtk_scene_borrowed_test), Cint, (Ptr{Cvoid},), f.h) == 0
end

@testitem "grdfilter: every filter family the dialog can build" tags=[:gui] setup=[GmtModules] begin
	IG = InteractiveGMT
	f = viz(GmtModules.grid())
	call(kv) = GmtModules.send(IG._on_grdfilter, f.h, kv)
	try
		r0 = GmtModules.roughness(f.G.z)
		for F in ("b0.4", "c0.4", "g0.4", "m0.4", "p0.4", "h0.4/10", "l0.4", "L0.4", "u0.4", "U0.4")
			@test call(["filter=$F", "distance=0", "grid="]) == 1
			@test GmtModules.grid_named(f.h, "Filtered ($F)") !== nothing
		end
		@test GmtModules.roughness(GmtModules.grid_named(f.h, "Filtered (g0.4)").z) < r0

		# The high-pass is the complement: low + high reconstructs the input.
		@test call(["filter=g0.4+h", "distance=0", "grid="]) == 1
		hi = GmtModules.grid_named(f.h, "Filtered (g0.4+h)")
		lo = GmtModules.grid_named(f.h, "Filtered (g0.4)")
		@test hi !== nothing && maximum(abs.(hi.z .+ lo.z .- f.G.z)) < 1e-2

		for F in ("m0.4+q0.25", "h0.4/10+c+u", "p0.4+l", "b0.4/0.2")   # the modifiers the dialog assembles
			@test call(["filter=$F", "distance=0", "grid="]) == 1
		end
		# Distance flags 1-5 say "the grid is in degrees". An in-memory grid does not carry that to the
		# module, so the wrapper must add -fg or GMT refuses: "Input grid is Cartesian but your
		# distance mode is set for geographic distances".
		@test call(["filter=m40", "distance=4", "grid="]) == 1
		@test call(["filter=m40", "distance=1", "grid="]) == 1

		# -R / -I narrow and resample the output; -T flips the registration.
		@test call(["filter=g0.4", "distance=0", "region=-9/-7/37/38", "inc=0.1", "grid="]) == 1
		S = GmtModules.grid_named(f.h, "Filtered (g0.4)")
		@test S !== nothing && S.range[1] >= -9.001 && S.range[2] <= -6.999
		@test call(["filter=g0.5", "distance=0", "toggle=1", "grid="]) == 1
		T = GmtModules.grid_named(f.h, "Filtered (g0.5)")
		@test T !== nothing && T.registration != f.G.registration
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "grdfilter NaN policy" tags=[:gui] setup=[GmtModules] begin
	IG = InteractiveGMT
	G = GmtModules.grid();  G.z[10, 10] = NaN32;  G.hasnans = 2
	f = viz(G)
	call(kv) = GmtModules.send(IG._on_grdfilter, f.h, kv)
	try
		@test call(["filter=g0.4", "distance=0", "nans=p", "grid="]) == 1
		@test count(isnan, GmtModules.grid_named(f.h, "Filtered (g0.4)").z) > 1   # NaN spreads
		@test call(["filter=g0.5", "distance=0", "nans=r", "grid="]) == 1
		@test count(isnan, GmtModules.grid_named(f.h, "Filtered (g0.5)").z) == 1  # only the input hole
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "grid calculator delivers a derived grid into the window" tags=[:gui] setup=[GmtModules] begin
	IG = InteractiveGMT
	f = viz(GmtModules.grid())
	try
		# Two same-geometry grids in one window: the base plus a trend of it.
		@test GmtModules.send(IG._on_grdtrend, f.h,
			["what=trend", "model=3", "robust=0", "protectnans=0", "axis=", "grid="]) == 1
		IG._pump_once()
		nms = GmtModules.names(f.h)
		@test "Trend (n=3)" in nms
		# The base grid is registered in _SCENE_OBJS under the EMPTY name ("" = the primary, see
		# savefile.jl's _remember_object!) -- nms[1] is literally "" here, not a usable calculator
		# token. The real GridCalculatorDialog never sends that raw bookkeeping key either: it sends
		# its OWN displayed label (baseName(), 70_window.cpp:5550) -- "Surface" whenever the window's
		# surfName is empty, exactly the view_grid case here. Drive the callback with that SAME label,
		# not nms[1].
		base = "Surface"
		# …and the calculator's list offers exactly those two (same limits and increments).
		out = mktemp() do path, io                       # _gridcalc_names prints its list
			redirect_stdout(() -> IG._gridcalc_names(f.h, base), io)
			flush(io);  read(path, String)
		end
		@test "Trend (n=3)" in split(strip(out), '\n')
		# Subtracting them is the residual field; it must land as its own named grid.
		expr = "$base - &{Trend (n=3)}"
		@test GmtModules.send(IG._on_gridcalc, f.h, ["expr=$expr", "base=$base"]) == 1
		IG._pump_once()
		# Named by the operands' ALIASES (A, B, … in order of appearance), never by their full names
		R = GmtModules.grid_named(f.h, "A - B")
		@test R !== nothing && size(R.z) == size(f.G.z)
		@test abs(sum(R.z) / length(R.z)) < abs(sum(f.G.z) / length(f.G.z))
		@test occursin("grid calculator", R.command)      # the recipe is stamped on the result
		@test occursin("A = $base, B = Trend (n=3)", R.command)   # …with who is who
		# The same aliases again: a NEW name, never a replace of the first result
		@test GmtModules.send(IG._on_gridcalc, f.h, ["expr=$expr", "base=$base"]) == 1
		IG._pump_once()
		@test GmtModules.grid_named(f.h, "A - B (2)") !== nothing && GmtModules.grid_named(f.h, "A - B") !== nothing
		# A LOGICAL expression is a mask: a Bool image on the base grid's own geometry, not a 0/1 grid.
		mexpr = "($base > 0) & ($base < 1e9)"
		mname = "(A > 0) & (A < 1e9)"
		@test GmtModules.send(IG._on_gridcalc, f.h, ["expr=$mexpr", "base=$base"]) == 1
		IG._pump_once()
		Mi = IG._find_object_exact(f.h, :image, mname)              # the ROW'S object: the mask itself
		@test Mi isa IG.GMTimage{Bool,2}
		@test !haskey(get(IG._IMG_ORIG, f.h, Dict()), mname)        # not an 8-bit copy of a "wider" source
		@test Mi.x == f.G.x && Mi.y == f.G.y && Mi.inc == f.G.inc && Mi.registration == f.G.registration
		@test count(Mi.image) == count(z -> z > 0, f.G.z)
		@test !(mname in GmtModules.names(f.h))                         # never ALSO a grid
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "grid calculator dialog: results join its list (masks too); no Close button" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = viz(GmtModules.grid())
	GmtvtkTest._register_gridcalc_test()
	try
		@test ccall(_test_fn(:gmtvtk_gridcalc_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
		@test ccall(_test_fn(:gmtvtk_gridcalc_close_button_test), Cint, (Ptr{Cvoid},), f.h) == 0
		compute(expr) = (buf = zeros(UInt8, 8192);
			n = ccall(_test_fn(:gmtvtk_gridcalc_compute_test), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint),
			          f.h, expr, buf, Cint(length(buf)));
			(n, split(GC.@preserve(buf, unsafe_string(pointer(buf))), '\n')))
		n, rows = compute("Surface * 2")
		@test "A * 2" in rows                                   # a grid result: offered to the next expression
		n, rows = compute("(Surface > 0) & (Surface < 1e9)")
		@test "(A > 0) & (A < 1e9)" in rows                     # a MASK result too: it is an ifelse operand
		n, rows = compute("ifelse((A > 0) & (A < 1e9), Surface, 0)")
		@test "ifelse(A, B, 0)" in rows                         # …and it really works as one

		# ifelse writes its own commas: each argument, once complete, is followed by ", " when the next
		# one starts (a grid name from the list, a number); a number's own digits are never split.
		typed(steps...) = (buf = zeros(UInt8, 4096);
			ccall(_test_fn(:gmtvtk_gridcalc_type_test), Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint),
			      f.h, join(steps, '\n'), buf, Cint(length(buf)));
			GC.@preserve(buf, unsafe_string(pointer(buf))))
		s = typed("btn:push_fun_ifelse", "list:Surface", "btn:push_op_ge", "btn:push_digit_0", "btn:push_digit_dot",
		          "btn:push_digit_4", "list:A * 2", "btn:push_digit_2", "btn:push_digit_5", "btn:push_digit_5", "btn:push_par_r")
		@test replace(s, r"\s+" => " ") == " ifelse( Surface >= 0.4, A * 2, 255)"
		s = typed("btn:push_fun_ifelse", "btn:push_par_l", "list:Surface", "btn:push_op_gt", "btn:push_digit_1",
		          "btn:push_par_r", "btn:push_digit_7")
		@test endswith(replace(s, r"\s+" => ""), "(Surface>1),7")
		@test !occursin(",", typed("btn:push_fun_sqrt", "list:Surface"))     # only ifelse gets commas
		@test isempty(typed("list:Surface", "btn:push_clear"))               # Clear empties the box
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "grid calculator dialog parks: X, minimise, Esc; Delete" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = viz(GmtModules.grid())
	parked() = ccall(_test_fn(:gmtvtk_gridcalc_parked_test), Cint, (Ptr{Cvoid},), f.h)
	opened() = ccall(_test_fn(:gmtvtk_gridcalc_open_dialog_test), Cint, (Ptr{Cvoid},), f.h)
	leave(how) = ccall(_test_fn(:gmtvtk_gridcalc_leave_dialog_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, how)
	try
		@test opened() == 1 && parked() == 0
		for how in (0, 1, 2)                           # the X, minimise, Esc (there is no Close button)
			leave(how)
			@test parked() == 1
			@test opened() == 1 && parked() == 0       # the menu brings the SAME one back
		end
		leave(0)
		@test ccall(_test_fn(:gmtvtk_gridcalc_delete_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
		@test parked() == -1                           # Delete really destroys it
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "the GMT menu really carries the new module entries" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = viz(GmtModules.grid())
	try
		trig(p) = ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring), f.h, p)
		# Firing the entry BUILDS its dialog, so this also proves each .ui still loads through QUiLoader
		# — a renamed widget or a broken .ui shows up here, not on the user's screen. ("Make CPT" is
		# passed without its "(makecpt / grd2cpt)" tail: that '/' is the hook's own path separator.)
		for entry in ("grdfft", "grdhisteq", "grdfill", "xyz2grd", "trend2d", "Make CPT",
		              "grdrotater", "greenspline", "grdvolume", "grdvector")
			@test trig(entry) == 1
			IG._pump_once()
		end
		# Tools > Earth regions is not a GMT-menu entry, but it is the same kind of runtime-.ui dialog
		# and fails the same way when its .ui stops loading.
		@test trig("Earth regions") == 1
		IG._pump_once()
		# Listing a collection opens ITS OWN read-only window — not the Errors console. 255 regions is
		# a lot of text to lose behind a failure message, and vice versa. The listing goes back to the
		# DIALOG that asked (that is what makes double-click-to-pick possible), so this drives the
		# callback the way the dialog does: through the live EarthRegionsDialog the menu just opened.
		@test ccall(_test_fn(:gmtvtk_earthregions_list_test), Cint, (Cstring, Cstring),
		            "Earth regions", "IHO") == 1
		IG._pump_once()
		@test ccall(_test_fn(:gmtvtk_window_exists_test), Cint, (Cstring,), "the IHO collection") >= 1
		# …and double-clicking a row puts that region's code in the dialog's box.
		# Line 0 is the column header; line 1 is the first region, IHO1 (Baltic Sea).
		@test ccall(_test_fn(:gmtvtk_earthregions_pick_test), Cint, (Cstring, Cint), "IHO collection", Cint(1)) == 1
		IG._pump_once()
		@test unsafe_string(ccall(_test_fn(:gmtvtk_earthregions_code_test), Cstring, (Cstring,),
		                          "Earth regions")) == "IHO1"
		# …and the row's own boundaries land in the Region boxes, read straight off the row.
		reg(t) = unsafe_string(ccall(_test_fn(:gmtvtk_earthregions_region_test), Cstring, (Cstring,), t))
		@test reg("Earth regions") == "9.8408/30.3471/53.6016/65.9071"
		# A code TYPED by hand gets the same readout — resolved through GMT.jl, not off any listing.
		@test ccall(_test_fn(:gmtvtk_earthregions_type_test), Cint, (Cstring, Cstring),
		            "Earth regions", "IHO31") == 1
		IG._pump_once()
		@test reg("Earth regions") == "34.4654/39.3056/45.1026/47.2896"
		# The GRAVITY tools are not in the GMT menu: they live in Geophysics > Gravity, which is one of
		# the rotating discipline pages — so the path is two steps, exactly like "Plates/…".
		for entry in ("gravfft", "talwani2d", "talwani3d", "gravprisms", "gmtflexure", "grdflexure")
			@test trig("Gravity/" * entry) == 2
			IG._pump_once()
		end
		exists(t) = ccall(_test_fn(:gmtvtk_window_exists_test), Cint, (Cstring,), t)
		@test exists("Make CPT") >= 1                  # …and the dialog really is on screen
		# Each of the new ones too: a .ui that failed to load leaves `dlg` null and NOTHING on
		# screen, which the trigger alone (it returns 1 for "the action fired") cannot tell apart.
		for w in ("gravfft", "grdrotater", "talwani2d", "talwani3d", "greenspline",
		          "gmtflexure", "grdflexure", "grdvolume", "gravprisms", "grdvector",
		          "Earth regions")
			@test exists(w) >= 1
		end
		# (No "is it gone from the GMT menu" assertion: menuFindDeep searches the WHOLE menu bar, so
		# with the Gravity page showing it would find those entries wherever they live. The removal is
		# a source fact — one addAction per tool — not something this hook can tell apart.)

		# The Color Palettes window gets that SAME dialog from its OWN menu bar, and hands the palette
		# it builds back into its own list — the user's "Make CPT in Color Palettes" road.
		@test trig("Color Palettes") == 1
		IG._pump_once()
		@test exists("Color Palettes") == 1
		wtrig(t, p) = ccall(_test_fn(:gmtvtk_window_menu_trigger_test), Cint, (Cstring, Cstring), t, p)
		@test wtrig("Color Palettes", "Make CPT") == 1
		IG._pump_once()
		@test exists("Make CPT") >= 2                  # the GMT menu's one, plus this window's own
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "grdfill fills the holes and reports where they were" tags=[:gui] setup=[GmtModules] begin
	IG = InteractiveGMT
	G = GmtModules.grid()
	G.z[10:14, 12:18] .= NaN32                        # one rectangular hole
	f = viz(G)
	call(kv) = GmtModules.send(IG._on_grdfill, f.h, kv)
	try
		# -L: the hole's bounding box, as a table (no grid delivered).
		@test call(["mode=list", "polygons=0", "grid=" * GmtModules.names(f.h)[1]]) == 1
		IG._pump_once()
		@test IG._scene_state(f.h)["n_table"] >= 1     # one row per hole (West East South North)
		# The fill itself: a new grid, and not a NaN left in it.
		@test call(["mode=fill", "algo=n", "grid=" * GmtModules.names(f.h)[1]]) == 1
		IG._pump_once()
		R = GmtModules.grid_named(f.h, "Filled (nearest neighbour)")
		@test R !== nothing && !any(isnan, R.z)
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "grdhisteq equalizes, and grdfft transforms" tags=[:gui] setup=[GmtModules] begin
	IG = InteractiveGMT
	f = viz(GmtModules.grid())
	base = GmtModules.names(f.h)[1]
	try
		# Equal-area cell indices: the result IS the cell number, so it spans 0..ncells-1.
		@test GmtModules.send(IG._on_grdhisteq, f.h,
		                      ["mode=grid", "flavour=cells", "ncells=8", "grid=$base"]) == 1
		IG._pump_once()
		R = GmtModules.grid_named(f.h, "Equalized (8 cells)")
		@test R !== nothing && maximum(R.z) <= 8 && minimum(R.z) >= 0
		# The levels table: one row per cell.
		@test GmtModules.send(IG._on_grdhisteq, f.h,
		                      ["mode=table", "ncells=8", "grid=$base"]) == 1
		IG._pump_once()
		@test IG._scene_state(f.h)["n_table"] >= 1     # one row per cell (Start Stop Cell)
		# grdfft upward continuation: a low-pass, so the field gets smoother.
		@test GmtModules.send(IG._on_grdfft, f.h, ["mode=grid", "upward=5000", "grid=$base"]) == 1
		IG._pump_once()
		U = GmtModules.grid_named(f.h, "grdfft (up 5000 m)")
		@test U !== nothing
		@test GmtModules.roughness(U.z) < GmtModules.roughness(f.G.z)
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# grdvector's arrows are geometry this app builds, not PostScript it asks GMT for — so the proof it
# works is that the overlay really lands in the window. One field per magnitude-class overlay, and
# the same run's table in the Data Viewer.
@testitem "grdvector draws its arrows into the window" tags=[:gui] setup=[GmtModules] begin
	IG = InteractiveGMT; GMT = IG.GMT
	# A rotational field on the window's own footprint: u = -(y-ym), v = (x-xm). Never zero except at
	# the centre, so almost every node gets an arrow.
	G = GmtModules.grid()
	x = collect(range(-10.0, -6.0, length = 41));  y = collect(range(36.0, 39.0, length = 31))
	xm = (x[1] + x[end]) / 2;  ym = (y[1] + y[end]) / 2
	U = GMT.mat2grid(Float32[-(y[iy] - ym) for iy in 1:31, ix in 1:41]; x = x, y = y)
	V = GMT.mat2grid(Float32[ (x[ix] - xm) for iy in 1:31, ix in 1:41]; x = x, y = y)
	f = viz(G)
	call(kv) = GmtModules.send(IG._on_grdvector, f.h, kv)
	try
		IG._SCENE_OBJS[f.h] = vcat(get(IG._SCENE_OBJS, f.h, Tuple{Symbol,String,Any}[]),
		                           Tuple{Symbol,String,Any}[(:grid, "u", U), (:grid, "v", V)])
		# SOLID arrows (the default): ONE arrow layer for the whole field, one vector per arrow node,
		# coloured by magnitude when asked.
		arrows() = filter(sl -> !isempty(sl.uv), IG._parse_symbols_blob(IG._serialize_symbols_raw(f.h)))
		@test call(["usescene=0", "grid1=u", "grid2=v", "incmode=x", "incx=5",
		            "scalemode=auto", "bymag=1", "name=flow solid"]) == 1
		IG._pump_once()
		al = arrows()
		@test length(al) == 1 && al[1].name == "flow solid" && al[1].sym == "arrow"
		@test size(al[1].uv, 1) > 0 && size(al[1].rgb, 1) == size(al[1].uv, 1) && al[1].worldSize > 0
		# LINE arrows (the dialog's "Solid 3-D arrows" unticked): THE arrow too — the same one field layer,
		# with open heads, never an overlay of hand-drawn V's.
		n0 = IG._scene_state(f.h)["n_overlays"]
		@test call(["usescene=0", "grid1=u", "grid2=v", "incmode=x", "incx=5", "solid=0",
		            "scalemode=auto", "heads=e", "color=black", "name=flow"]) == 1
		IG._pump_once()
		@test IG._scene_state(f.h)["n_overlays"] == n0            # no overlay
		al = arrows()
		@test length(al) == 2 && al[2].name == "flow" && size(al[2].uv, 1) == size(al[1].uv, 1)
		# By magnitude: N equal-width classes, one colour per arrow, still ONE layer.
		@test call(["usescene=0", "grid1=u", "grid2=v", "incmode=x", "incx=5", "scalemode=auto", "solid=0",
		            "heads=be", "bymag=1", "nclass=4", "name=flow by |v|", "table=1"]) == 1
		IG._pump_once()
		al = arrows()
		@test length(al) == 3 && size(al[3].rgb, 1) == size(al[3].uv, 1)
		@test length(unique(eachrow(round.(al[3].rgb; digits = 6)))) <= 4   # at most nclass colours
		@test IG._scene_state(f.h)["n_table"] >= 1                # x y u v magnitude direction
		# Polar input describes the SAME field: r = hypot(u,v), theta = atand(v,u).
		R = GMT.mat2grid(Float32[hypot(x[ix] - xm, y[iy] - ym) for iy in 1:31, ix in 1:41]; x = x, y = y)
		T = GMT.mat2grid(Float32[atand(x[ix] - xm, -(y[iy] - ym)) for iy in 1:31, ix in 1:41]; x = x, y = y)
		IG._SCENE_OBJS[f.h] = vcat(IG._SCENE_OBJS[f.h],
		                           Tuple{Symbol,String,Any}[(:grid, "r", R), (:grid, "th", T)])
		@test call(["usescene=0", "grid1=r", "grid2=th", "polar=1", "incmode=x", "incx=5", "solid=0",
		            "scalemode=auto", "heads=e", "name=flow polar"]) == 1
		IG._pump_once()
		al = arrows()
		@test length(al) == 4 && size(al[4].uv, 1) == size(al[2].uv, 1)   # the same field, the same arrows
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "show_table pops the shared table, not a dock tab" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9];
	                 x = collect(range(0, 9, length = 10)), y = collect(range(0, 9, length = 10)))
	f = viz(G)
	try
		show_table(f, [1.0 2.0; 3.0 4.0; 5.0 6.0])
		IG._pump_once()
		@test IG._scene_state(f.h)["n_table"] == 3
		# a named-column dataset (what measure.jl / gravmag3d send) goes the same way
		show_table(f, GMT.mat2ds([1.0 10.0 100.0; 2.0 20.0 200.0]; colnames = ["lon", "lat", "anom"]);
		           name = "Track anomaly")
		IG._pump_once()
		@test IG._scene_state(f.h)["n_table"] == 2
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# Interpolate remembers each method's Options: a value accepted in one dialog (saved as Options > OK saves
# it) is what the NEXT dialog opens with. The iGMT.ini entry it travels through is restored by the hook.
@testitem "Interpolate: the Options of a method are remembered by the next dialog" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	h = ccall(IG._fn(:gmtvtk_open_empty), Ptr{Cvoid}, (Cstring,), "Interpolate options test")
	try
		buf = zeros(UInt8, 256)
		@test ccall(GmtvtkTest._test_fn(:gmtvtk_interp_options_roundtrip_test), Cint,
		            (Ptr{Cvoid}, Cstring, Cstring, Cstring, Ptr{UInt8}, Cint), h, "mbgrid", "scale", "3", buf, Cint(256)) == 1
		@test unsafe_string(pointer(buf)) == "3"
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
	end
end

# ifelse with TRANSPARENCY (user's order, 2026-11-17): `255@50` paints the picked pixels white at 50 %
# transparency, `@100` makes them fully see-through whatever the number; the result carries an alpha
# band, opaque everywhere else. Both an in-memory (band-planar) and a disk (pixel-interleaved) RGB image.
@testitem "grid calculator: ifelse paints transparency (255@50, 0@100)" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	# pixel-registered, so the picture keeps the grid's exact geometry through a GeoTIFF round trip
	G = IG.GMT.mat2grid(Float32[ix for iy in 1:10, ix in 1:20], x = [0.0, 20.0], y = [0.0, 10.0], reg = 1)
	f = viz(G; title = "ZZtransp")
	try
		P = IG.GMT.mat2img(cat(fill(0x14, 10, 20), fill(0xc8, 10, 20), fill(0x1e, 10, 20); dims = 3), G)
		tif = joinpath(mktempdir(), "pic.tif");  IG.GMT.gmtwrite(tif, P)
		for (nm, I) in (("Pmem", P), ("Pdisk", IG.GMT.gmtread(tif)))
			@test IG._add_image_to_scene(f.h, I, nm; promote = false)
			for (e, a) in (("255@50", 0x80), ("0@100", 0x00), ("(255, 0, 0)@25", 0xbf))
				expr = "ifelse(Surface >= 11, $nm, $e)"
				@test GmtModules.send(IG._on_gridcalc, f.h, ["expr=$expr", "base=Surface"]) == 1
				R = last([d for (k, n, d) in IG._SCENE_OBJS[f.h] if k === :image && startswith(n, "ifelse(")])
				@test size(R.image, 3) == 4
				idx, npl, _ = IG._gc_rgb_index(R)
				px(ix, iy) = [R.image[idx(ix, iy) + b * npl] for b in 0:3]
				@test px(15, 5) == [0x14, 0xc8, 0x1e, 0xff]               # kept: the picture, opaque
				# @100: the pixel keeps the picture's OWN colour (only alpha changes) — a painted colour under
				# alpha 0 bleeds into the opaque edge when the texture is smoothed (a whitish ring)
				col = e == "255@50" ? [0xff, 0xff, 0xff] : e == "0@100" ? [0x14, 0xc8, 0x1e] : [0xff, 0x00, 0x00]
				@test px(3, 5) == vcat(col, a)                             # painted, at its transparency
			end
		end
		# Crop Image (with coords) of a north-east rectangle takes the NORTH-EAST pixels, right way up — the
		# picture itself and every ifelse result made from it, in memory and from disk (the disk one used to
		# come back as the wrong rows, flipped).
		IG.GMT.gmtwrite(tif, IG.GMT.mat2img(cat(UInt8[iy * 20 for iy in 1:10, ix in 1:20],
		                                        UInt8[ix * 10 for iy in 1:10, ix in 1:20], fill(0x40, 10, 20); dims = 3), G))
		@test IG._add_image_to_scene(f.h, IG.GMT.gmtread(tif), "Grad"; promote = false)
		for e in ("", "0", "0@50")
			if !isempty(e)
				@test GmtModules.send(IG._on_gridcalc, f.h, ["expr=ifelse(Surface >= 11, Grad, $e)", "base=Surface"]) == 1
			end
			X = isempty(e) ? IG._find_object_exact(f.h, :image, "Grad") :
			    last([d for (k, n, d) in IG._SCENE_OBJS[f.h] if k === :image && startswith(n, "ifelse(")])
			C = IG._roi_crop_image(X, 10.0, 20.0, 6.0, 10.0)
			cidx, cnpl, _ = IG._gc_rgb_index(C)
			ncx, ncy = C.layout[2] == 'R' ? (size(C.image, 1), size(C.image, 2)) : (size(C.image, 2), size(C.image, 1))
			@test (C.image[cidx(1, 1)], C.image[cidx(1, ncy)]) == (140, 200)                 # red: rows 7 (S) .. 10 (N)
			@test (C.image[cidx(1, 1) + cnpl], C.image[cidx(ncx, 1) + cnpl]) == (110, 200)    # green: cols 11 (W) .. 20 (E)
		end
		# `@` paints an RGB image only
		@test GmtModules.send(IG._on_gridcalc, f.h, ["expr=ifelse(Surface >= 11, Surface, 0@50)", "base=Surface"]) == 2
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# REPROJECT TAKES WHAT IS ON DISPLAY (user's order, 2026-11-17): a cropped IMAGE on display, its grid
# still in the window — the CRS selector / Tools > Project used to warp the grid. Both the check and the
# warp now ask `_displayed_raster`.
@testitem "reproject warps the raster on display (an image over a grid)" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	G = IG.GMT.mat2grid(Float32[ix + iy for iy in 1:30, ix in 1:40], x = [-10.0, -6.0], y = [38.0, 41.0],
	                    reg = 1, proj4 = "+proj=longlat +datum=WGS84 +no_defs")
	f = viz(G; title = "ZZreproj")
	try
		P = IG.GMT.mat2img(cat(fill(0x20, 30, 40), fill(0x90, 30, 40), fill(0x30, 30, 40); dims = 3), G)
		@test IG._add_image_to_scene(f.h, P, "ZZpic"; promote = false)
		IG._on_roi_crop(f.h, "image_coords", "-9/-7/39/40")          # the crop is now what is displayed
		IG._pump_once()
		O = IG._displayed_raster(f.h)
		@test O isa IG.GMTimage && O === IG._find_object_exact(f.h, :image, "ZZpic (cropped)")
		@test startswith(IG._crs_check(f.h, "", "EPSG:3763"), "OK\t")
		before = Set(keys(IG._FIGREG))
		@test GmtModules.send(IG._on_project, f.h, ["t_srs=EPSG:3763", "s_srs=+proj=longlat +datum=WGS84 +no_defs",
		                                            "resample=bilinear", "projname=PT-TM06", "grid="]) == 1
		IG._pump_once()
		new = [IG._FIGREG[h] for h in keys(IG._FIGREG) if !(h in before)]
		@test length(new) == 1 && !(new[1] isa IG.QtFigure)            # an IMAGE window, never a grid one
		newobjs = get(IG._SCENE_OBJS, getfield(new[1], :h), [])
		@test any(t -> t[3] isa IG.GMTimage, newobjs) && !any(t -> t[3] isa IG.GMTgrid, newobjs)
		# An image WITH TRANSPARENCY is warped as transparency (-srcalpha/-dstalpha): the white hidden under
		# its transparent pixels never reaches the visible edge (the ring came back after reprojecting).
		n = 120
		A = Array{UInt8}(undef, n, n, 4)
		for iy in 1:n, ix in 1:n
			inside = hypot(ix - 60.5, iy - 60.5) <= 40
			A[iy, ix, 1:3] .= inside ? (0x20, 0x80, 0x20) : (0xff, 0xff, 0xff)
			A[iy, ix, 4] = inside ? 0xff : 0x00
		end
		Gd = IG.GMT.mat2grid(zeros(Float32, n, n), x = [-9.0, -8.0], y = [39.0, 40.0], reg = 1,
		                     proj4 = "+proj=longlat +datum=WGS84 +no_defs")
		W = IG.GMT.gdalwarp(IG.GMT.mat2img(A, Gd),
		                    IG._project_opts(Dict("t_srs" => "EPSG:3763", "resample" => "bilinear"), true; alpha = true))
		S = IG._to_band_planar(W).image
		@test size(S, 3) == 4
		@test !any(S[i, j, 4] > 0 && S[i, j, 2] > 0xa0 for i in axes(S, 1), j in axes(S, 2))   # no whitish visible pixel
	finally
		for h in collect(keys(IG._FIGREG))
			h == f.h || ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
		end
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# A MASK WEARS ITS OWN ICON in Scene Objects (user's order, 2026-11-17): black and white only, unlike a
# picture's. A Grid calculator logical result gets it; a plain picture keeps the image icon.
@testitem "grid calculator: a mask's Scene Objects row wears the mask icon" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	icon(h, l) = ccall(_test_fn(:gmtvtk_objicon_test), Cint, (Ptr{Cvoid}, Cstring), h, l)
	IC_IMAGE, IC_MASK = 1, 19                                       # ObjIcon (50_scene.cpp)
	G = IG.GMT.mat2grid(Float32[ix for iy in 1:10, ix in 1:20], x = [0.0, 20.0], y = [0.0, 10.0], reg = 1)
	f = viz(G; title = "ZZmaskicon")
	try
		@test IG._add_image_to_scene(f.h, IG.GMT.mat2img(rand(UInt8, 10, 20, 3), G), "ZZphoto"; promote = false)
		@test GmtModules.send(IG._on_gridcalc, f.h, ["expr=ZZmaskicon >= 11", "base=ZZmaskicon"]) == 1
		for _ in 1:10; IG._pump_once(); sleep(0.03); end
		@test icon(f.h, "A >= 11") == IC_MASK
		@test icon(f.h, "ZZphoto") == IC_IMAGE
		d = mktempdir()
		@test ccall(_test_fn(:gmtvtk_objicon_png_test), Cint, (Cint, Cstring), IC_MASK, joinpath(d, "mask.png")) == 1
		P = IG.GMT.gdalread(joinpath(d, "mask.png"))                  # black and white only (plus transparency)
		px = [(P[i, j, 1], P[i, j, 2], P[i, j, 3]) for i in axes(P, 1), j in axes(P, 2) if size(P, 3) < 4 || P[i, j, 4] == 0xff]
		@test all(c -> c[1] == c[2] == c[3], px)                       # no colour anywhere
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# "Delete this polygon" on a traced mask outline takes ONLY the polygon clicked (user's order, 2026-11-17):
# the blob's ring alone; a hole's click removes the field around it together with that hole.
@testitem "digitize whites: Delete this polygon removes only the clicked polygon" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	GmtvtkTest._register_console_eval_test(true)
	nseg(h, nm) = ccall(_test_fn(:gmtvtk_overlay_nseg_test), Cint, (Ptr{Cvoid}, Cstring), h, nm)
	setseg(h, k) = ccall(_test_fn(:gmtvtk_set_menu_seg_test), Cvoid, (Ptr{Cvoid}, Cint), h, k)
	click(h, nm, item) = ccall(_test_fn(:gmtvtk_line_menu_click_test), Cint, (Ptr{Cvoid}, Cstring, Cstring), h, nm, item)
	n = 120
	G = IG.GMT.mat2grid(zeros(Float32, n, n), x = [0.0, 3600.0], y = [0.0, 3600.0], reg = 1)
	m = [(12 <= hypot(ix - 40.5, iy - 40.5) <= 28) || (hypot(ix - 95.5, iy - 90.5) <= 15) for iy in 1:n, ix in 1:n]
	f = viz(G; title = "ZZdigitize")
	try
		M = IG._gc_mask_image(m, G)
		@test IG._commit_mask!(f.h, M, "ZZmask") == 1
		IG._digitize_whites(f.h, "ZZmask")
		IG._pump_once()
		ol = "ZZmask boundary"
		@test nseg(f.h, ol) == 3
		# which ring is the blob, and which is the field's hole (the viewer's overlay keeps _digitize_rings' order)
		xyz, so, ns, _ = IG._digitize_rings(M, 1)
		rings = [Matrix{Float64}(reshape(xyz, 3, :)'[so[s]+1:so[s+1], 1:2]) for s in 1:ns]
		cont = IG._ring_containers(rings)
		kblob = findfirst(k -> isempty(cont[k]) && !any(c -> k in c, cont), 1:ns)
		khole = findfirst(c -> !isempty(c), cont)
		setseg(f.h, kblob - 1)
		@test click(f.h, ol, "Delete this polygon") == 1
		@test nseg(f.h, ol) == 2                                  # the blob only
		setseg(f.h, (khole > kblob ? khole - 1 : khole) - 1)      # the hole, indexed in what is left
		@test click(f.h, ol, "Delete this polygon") == 1
		@test nseg(f.h, ol) == -1                                 # the field and its hole: nothing left
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# A MASK SURVIVES SAVE / LOAD SESSION (user's report, 2026-11-17: a session's logical arrays came back as
# 0/1 pictures, so ifelse refused them): saved with `mask=1` in the manifest, loaded as the Bool mask.
# A session written before that flag (sess2.igmtz) is read the same way: a generated one-band 0/1 picture.
@testitem "grid calculator: masks survive Save/Load Session as masks" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	G = IG.GMT.mat2grid(Float32[ix for iy in 1:10, ix in 1:20], x = [0.0, 20.0], y = [0.0, 10.0], reg = 1)
	f = viz(G; title = "ZZsessmask")
	path = joinpath(mktempdir(), "masks.igmtz");  q = nothing
	try
		@test GmtModules.send(IG._on_gridcalc, f.h, ["expr=Surface >= 11", "base=Surface"]) == 1
		IG._on_save_session(f.h, path)
		man = IG._zip_read(path)["session.manifest"]
		@test occursin("param.mask=1", String(copy(man)))
		q = IG._on_load_session(C_NULL, path);  IG._pump_once()
		h = getfield(q, :h)
		M = IG._find_object_exact(h, :image, "A >= 11")
		@test M isa IG.GMTimage{Bool,2} && count(M.image) == 10 * 10
		# …and is a mask to the calculator again
		base = unsafe_string(ccall(IG._fn(:gmtvtk_base_name_h), Cstring, (Ptr{Cvoid},), h))
		@test GmtModules.send(IG._on_gridcalc, h, ["expr=ifelse(A >= 11, $base, 0)", "base=$base"]) == 1
	finally
		q === nothing || ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), getfield(q, :h))
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# WHAT IS DISPLAYED ANSWERS (user's order, 2026-11-17): with a grid AND an image on top of it both
# showing, the readout over the image prints the image's COLOUR, never the z of the grid under it;
# beside the image it is the grid's z again. Roi Crop's "Crop Image" and Info ask the same resolver.
@testitem "readout, crop and Info speak for the image on top of a grid" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	pump(n = 10) = for _ in 1:n; IG._pump_once(); sleep(0.03); end
	hover(h, x, y) = (buf = zeros(UInt8, 1024);
		ccall(_test_fn(:gmtvtk_hover_readout_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Ptr{UInt8}, Cint),
		      h, x, y, buf, Cint(length(buf)));
		GC.@preserve buf unsafe_string(pointer(buf)))
	G = IG.GMT.mat2grid(Float32[100iy + ix for iy in 1:10, ix in 1:20], x = collect(1.0:20.0), y = collect(1.0:10.0))
	f = viz(G; title = "ZZseen")
	pump()
	try
		# a plain green picture over the WEST half of the grid; the grid stays checked beside it
		P = IG.GMT.mat2img(cat(fill(0x14, 10, 10), fill(0xc8, 10, 10), fill(0x1e, 10, 10); dims = 3),
		                   x = [1.0, 10.0], y = [1.0, 10.0])
		@test IG._add_image_to_scene(f.h, P, "ZZpic"; promote = false)
		pump()
		over = hover(f.h, 5.0, 5.0)
		@test occursin("rgb", over) && !occursin("z =", over)           # the image answers, with its colour
		zs = match(r"z = (-?[0-9.]+)", hover(f.h, 15.0, 5.0))            # beside it, the grid's own z (≈ 515)
		@test zs !== nothing && abs(parse(Float64, zs[1]) - 515) < 2
		@test IG._displayed_image_at(f.h, 5.0, 5.0)[1] == "ZZpic"
		@test IG._displayed_image_at(f.h, 15.0, 5.0)[2] === nothing      # a grid is displayed there
		# Crop Image takes the picture on top (a hidden image of the window would have been taken before)
		@test IG._on_roi_crop(f.h, "image_coords", "3/7/3/7") === nothing
		pump()
		Ic = IG._find_object_exact(f.h, :image, "ZZpic (cropped)")
		@test Ic isa IG.GMTimage && Ic.image[1, 1, 2] == 0xc8
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# A MASK IS A BOOLEAN IMAGE (0/1), whichever tool made it, and what the window shows is what Info and the
# readout describe: a mask on screen is reported as the Bool image it is, and an image wider than 8 bits
# reads out its own values (DN), never the colour of the 8-bit copy it is drawn from.
@testitem "masks are Bool images; Info and the readout describe the image on screen" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	pump(n = 10) = for _ in 1:n; IG._pump_once(); sleep(0.03); end
	hover(h, x, y) = (buf = zeros(UInt8, 1024);
		ccall(_test_fn(:gmtvtk_hover_readout_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Ptr{UInt8}, Cint),
		      h, x, y, buf, Cint(length(buf)));
		GC.@preserve buf unsafe_string(pointer(buf)))
	info(h) = mktemp() do path, io
		redirect_stdout(() -> IG._info_text(IG._FIGREG[h], "grdinfo"), io)
		flush(io);  read(path, String)
	end
	# z = 100*row + col (row 1 = south), so every node says where it is
	G = IG.GMT.mat2grid(Float32[100iy + ix for iy in 1:10, ix in 1:20], x = collect(1.0:20.0), y = collect(1.0:10.0))
	f = viz(G; title = "ZZmasklaw")
	pump()
	try
		# the Grid calculator: a logical expression -> a Bool mask, which is then what the window shows
		expr = "(Surface > 500) & (Surface < 900)"
		@test GmtModules.send(IG._on_gridcalc, f.h, ["expr=$expr", "base=Surface"]) == 1
		pump()
		@test IG._active_element(f.h) == (2, "(A > 500) & (A < 900)")  # the viewer: the mask is on screen (named by alias)
		r = info(f.h)
		@test occursin("GMTimage", r) && occursin("Bool", r)           # Info describes THE MASK...
		@test !occursin("grid", lowercase(first(split(r, '\n'))))      # ...never the hidden grid under it
		@test occursin("DN = 1", hover(f.h, 7.0, 6.0))                 # node 607: inside the condition
		@test occursin("DN = 0", hover(f.h, 7.0, 2.0))                 # node 207: outside it

		# Binarize / Flood fill / Classification hand over a 0/255 picture: it lands as a Bool mask
		M8 = IG.GMT.mat2img(map(v -> v > 1000 ? 0xff : 0x00, IG._zmat(G)), G)
		@test IG._commit_mask!(f.h, M8, "ZZbytes mask") == 1
		pump()
		Mb = IG._find_object_exact(f.h, :image, "ZZbytes mask")
		@test Mb isa IG.GMTimage{Bool,2} && count(Mb.image) == count(>(1000), G.z)
		@test !haskey(get(IG._IMG_ORIG, f.h, Dict()), "ZZbytes mask")   # not a stretch source
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end

	# a 16-bit band (a Landsat DN raster): read as a UInt16 GMTimage, its readout is the DN under the cursor
	d = mktempdir()
	tif = joinpath(d, "band16.tif")
	IG.GMT.gdaltranslate(G .+ 1000f0, ["-ot", "UInt16", "-of", "GTiff"], dest = tif)
	w = IG.iview()
	h = getfield(w, :h)
	pump()
	try
		IG._open_spec_into(h, tif, basename(tif), true)
		pump()
		B = IG._info_image(h, IG._FIGREG[h])
		@test B isa IG.GMTimage{UInt16}                               # the band, not its 8-bit copy
		@test occursin("UInt16", info(h))
		@test occursin("DN = 1407", hover(h, 7.0, 4.0))              # node (col 7, row 4) = 1000 + 407
		@test occursin("DN = 1912", hover(h, 12.0, 9.0))
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), h)
	end
end
