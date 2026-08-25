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
	f = view_grid(GmtModules.grid())
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
	f = view_grid(G)
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
	f = view_grid(GmtModules.grid())
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

@testitem "grdfilter: every filter family the dialog can build" tags=[:gui] setup=[GmtModules] begin
	IG = InteractiveGMT
	f = view_grid(GmtModules.grid())
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
	f = view_grid(G)
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
	f = view_grid(GmtModules.grid())
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
		R = GmtModules.grid_named(f.h, expr)
		@test R !== nothing && size(R.z) == size(f.G.z)
		@test abs(sum(R.z) / length(R.z)) < abs(sum(f.G.z) / length(f.G.z))
		@test occursin("grid calculator", R.command)      # the recipe is stamped on the result
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "the GMT menu really carries the new module entries" tags=[:gui] setup=[GmtModules, GmtvtkTest] begin
	IG = InteractiveGMT
	_test_fn = GmtvtkTest._test_fn
	f = view_grid(GmtModules.grid())
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
	f = view_grid(G)
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
	f = view_grid(GmtModules.grid())
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
	f = view_grid(G)
	call(kv) = GmtModules.send(IG._on_grdvector, f.h, kv)
	try
		IG._SCENE_OBJS[f.h] = vcat(get(IG._SCENE_OBJS, f.h, Tuple{Symbol,String,Any}[]),
		                           Tuple{Symbol,String,Any}[(:grid, "u", U), (:grid, "v", V)])
		n0 = IG._scene_state(f.h)["n_overlays"]
		@test call(["usescene=0", "grid1=u", "grid2=v", "incmode=x", "incx=5",
		            "scalemode=auto", "heads=e", "color=black", "name=flow"]) == 1
		IG._pump_once()
		@test IG._scene_state(f.h)["n_overlays"] == n0 + 1        # ONE overlay for the whole field
		# By magnitude: one overlay per class, all under the one group name.
		n1 = IG._scene_state(f.h)["n_overlays"]
		@test call(["usescene=0", "grid1=u", "grid2=v", "incmode=x", "incx=5", "scalemode=auto",
		            "heads=be", "bymag=1", "nclass=4", "name=flow by |v|", "table=1"]) == 1
		IG._pump_once()
		@test IG._scene_state(f.h)["n_overlays"] > n1
		@test IG._scene_state(f.h)["n_table"] >= 1                # x y u v magnitude direction
		# Polar input describes the SAME field: r = hypot(u,v), theta = atand(v,u).
		R = GMT.mat2grid(Float32[hypot(x[ix] - xm, y[iy] - ym) for iy in 1:31, ix in 1:41]; x = x, y = y)
		T = GMT.mat2grid(Float32[atand(x[ix] - xm, -(y[iy] - ym)) for iy in 1:31, ix in 1:41]; x = x, y = y)
		IG._SCENE_OBJS[f.h] = vcat(IG._SCENE_OBJS[f.h],
		                           Tuple{Symbol,String,Any}[(:grid, "r", R), (:grid, "th", T)])
		n2 = IG._scene_state(f.h)["n_overlays"]
		@test call(["usescene=0", "grid1=r", "grid2=th", "polar=1", "incmode=x", "incx=5",
		            "scalemode=auto", "heads=e", "name=flow polar"]) == 1
		IG._pump_once()
		@test IG._scene_state(f.h)["n_overlays"] == n2 + 1
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "show_table pops the shared table, not a dock tab" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9];
	                 x = collect(range(0, 9, length = 10)), y = collect(range(0, 9, length = 10)))
	f = view_grid(G)
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
