# Scenario regression tests: open REAL Qt+VTK windows through the built gmtvtk.dll, drive the
# add-paths that keep breaking each other (empty launcher, grid promote, basemap on empty vs
# populated window, file drop, PNG output) and assert the scene state with gmtvtk_scene_state.
# These need a display (or QT_QPA_PLATFORM=offscreen) + the built DLL, so they are tagged :gui and
# only run when INTERACTIVEGMT_TEST_GUI=1. Each test closes its window in a finally block.

@testitem "empty launcher: no surface, no coordinate grid" tags=[:gui] begin
	IG = InteractiveGMT
	e = iview()
	try
		IG._pump_once()
		st = IG._scene_state(e.h)
		@test st["alive"] == 1
		@test st["has_surface"] == 0      # bare launcher, only a hidden placeholder
		@test st["emptyStart"] == 1
		@test st["axes"] == 0             # no coordinate grid yet
		@test st["n_extras"] == 0
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), e.h)
	end
end

@testitem "view_grid: full framed window with coordinate grid" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	f = view_grid(G)
	try
		IG._pump_once()
		st = IG._scene_state(f.h)
		@test st["has_surface"] == 1
		@test st["emptyStart"] == 0
		@test st["axes"] == 1             # coordinate grid present
		@test st["imageOnly"] == 0
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# THE regression this whole episode was about: a basemap tile dropped on an EMPTY launcher must come
# up as an ExtraObj IMAGE (so it carries the properties menu) AND keep the coordinate grid, the
# coordinate readout (referenced CRS) and a centred flat-2-D view.
@testitem "basemap on empty launcher: ExtraObj image + coords + flat-2D" tags=[:gui, :basemap] begin
	IG = InteractiveGMT
	if !isfile(IG._etopo4_path())
		@test_skip "data/etopo4.jpg not present"
	else
		e = iview()
		try
			s = "-180/180/-90/90/0/global"
			GC.@preserve s IG._on_basemap(e.h, Base.unsafe_convert(Cstring, s))
			IG._pump_once()
			st = IG._scene_state(e.h)
			@test st["has_surface"] == 1                 # blank flat base promoted -> framed
			@test st["axes"] == 1                        # coordinate grid present (the regression)
			@test st["imageOnly"] == 1
			@test st["flat2d"] == 1                      # centred top-down map
			@test st["crs"] == 1                         # referenced -> coord readout + Geography menu
			@test st["n_extras"] == 1
			@test st["extras"][1][1] == "image"          # the tile is an ExtraObj image (properties menu)
			@test occursin("Base image", st["extras"][1][2])
			@test st["x0"] ≈ -180 && st["x1"] ≈ 180
			@test st["y0"] ≈ -90  && st["y1"] ≈ 90
		finally
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), e.h)
		end
	end
end

@testitem "basemap on populated window: added on top, snaps to flat-2D" tags=[:gui, :basemap] begin
	IG = InteractiveGMT; GMT = IG.GMT
	if !isfile(IG._etopo4_path())
		@test_skip "data/etopo4.jpg not present"
	else
		G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[-10.0, 10.0], y=[-10.0, 10.0])
		f = view_grid(G)
		try
			before = IG._scene_state(f.h)
			s = "-180/180/-90/90/0/global"
			GC.@preserve s IG._on_basemap(f.h, Base.unsafe_convert(Cstring, s))
			IG._pump_once()
			st = IG._scene_state(f.h)
			@test st["has_surface"] == 1
			@test st["n_extras"] == before["n_extras"] + 1
			@test st["extras"][end][1] == "image"
			@test st["flat2d"] == 1                      # a basemap grows the frame -> snaps to the top-down flat-2D map
		finally
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
		end
	end
end

@testitem "drop a grid onto a launcher promotes it in place" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	path = joinpath(tempdir(), "interactivegmt_drop_$(getpid()).nc")
	GMT.gmtwrite(path, G)
	e = iview()
	try
		GC.@preserve path IG._on_drop(e.h, Base.unsafe_convert(Cstring, path))
		IG._pump_once()
		st = IG._scene_state(e.h)
		@test st["has_surface"] == 1      # the launcher was promoted, not a new window
		@test st["emptyStart"] == 0
		@test st["axes"] == 1
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), e.h)
		rm(path; force=true)
	end
end

# The Vertical-elastic-deformation fault plane: a fault + the dialog's plane build must create the
# buried 3-D dipping plane (hidden in flat-2D, shown in 3-D) AND a "<fault> — plane" Scene Objects
# handle row. This is the regression for the "no 3-D plane / no handle" episode.
@testitem "fault plane: buried 3-D plane + Scene Objects handle" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[100*sin(ix/3)+50*cos(iy/4) for iy in 0:40, ix in 0:40]; x=[0.0, 40.0], y=[0.0, 40.0])
	f = view_grid(G)
	try
		IG._pump_once()
		ccall(IG._fn(:gmtvtk_fault_add_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble),
		      f.h, 5.0, 8.0, 35.0, 30.0)
		out = zeros(Cdouble, 6)
		ex = ccall(IG._fn(:gmtvtk_fault_plane_test), Cint,
		           (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cint, Ptr{Cdouble}), f.h, 20.0, 30.0, 50.0, 0, out)
		@test ex == 1              # 3-D plane actor exists
		@test out[2] == 4          # quad: 4 corners
		@test out[4] > out[5]      # top above bottom (dips down)
		@test out[6] == 1          # gray surface patch visible

		# view_grid opens flat-2D -> 3-D plane hidden; switching to 3-D reveals it.
		@test out[3] == 0          # hidden in flat-2D
		ccall(IG._fn(:gmtvtk_set_flat2d_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, 0)
		IG._pump_once()
		ccall(IG._fn(:gmtvtk_fault_plane_test), Cint,
		      (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cint, Ptr{Cdouble}), f.h, 20.0, 30.0, 50.0, 0, out)
		@test out[3] == 1          # visible in 3-D

		rows = unsafe_string(ccall(IG._fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
		@test occursin("plane", rows)   # the "<fault> — plane" handle row exists

		# THE bug: the plane was a dialog-time preview, wiped when the dialog closed. Drive the REAL
		# dialog lifecycle and assert the plane + handle SURVIVE the close.
		@test ccall(IG._fn(:gmtvtk_fault_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
		IG._pump_once()
		ccall(IG._fn(:gmtvtk_fault_plane_test), Cint,
		      (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cint, Ptr{Cdouble}), f.h, 20.0, 30.0, 50.0, 0, out)
		ccall(IG._fn(:gmtvtk_fault_close_dialog_test), Cvoid, (Ptr{Cvoid},), f.h)
		IG._pump_once()
		@test ccall(IG._fn(:gmtvtk_fault_plane_test), Cint,
		            (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cint, Ptr{Cdouble}), f.h, 20.0, 30.0, 50.0, 0, out) == 1
		@test occursin("plane", unsafe_string(ccall(IG._fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h)))
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# Output correctness: a rendered window saves a real, non-empty PNG (valid 8-byte signature).
@testitem "save_png writes a valid PNG" tags=[:gui, :output] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	f = view_grid(G)
	path = joinpath(tempdir(), "interactivegmt_png_$(getpid()).png")
	try
		IG._pump_once()
		isfile(path) && rm(path; force=true)
		@test save_png(path) == true
		IG._pump_once()
		@test isfile(path)
		@test filesize(path) > 0
		sig = open(io -> read(io, 8), path, "r")
		@test sig == UInt8[0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
		rm(path; force=true)
	end
end

# Per-grid colorbar isolation: recolouring ONE grid's Color Bar must change ONLY that grid's colours,
# never another grid's (the regression the per-grid lut refactor is meant to guarantee).
@testitem "colorbar recolour is isolated per grid" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G1 = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	G2 = GMT.mat2grid(Float32[2(ix + iy) for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	f = view_grid(G1)                                  # base relief grid = tag -1
	try
		@test IG._add_grid_to_scene(f.h, G2, "g2")     # first dropped grid = tag 1
		IG._pump_once()
		st = IG._scene_state(f.h)
		@test st["n_extras"] == 1

		z = 9.0
		base0  = IG._grid_rgb_at(f.h, -1, z)           # base relief, tag -1
		extra0 = IG._grid_rgb_at(f.h,  1, z)           # dropped grid, tag 1
		@test base0  !== nothing
		@test extra0 !== nothing

		# Recolour ONLY the dropped grid (tag 1) to a clearly different map.
		IG._recolor_grid(f, "gray", G2.range[5], G2.range[6], 1)
		IG._pump_once()
		base1  = IG._grid_rgb_at(f.h, -1, z)
		extra1 = IG._grid_rgb_at(f.h,  1, z)
		@test base1  == base0        # base grid untouched
		@test extra1 != extra0       # tagged grid changed

		# Recolour ONLY the base grid (tag -1); the dropped grid must stay as just set.
		IG._recolor_grid(f, "jet", G1.range[5], G1.range[6], -1)
		IG._pump_once()
		@test IG._grid_rgb_at(f.h, -1, z) != base1   # base changed
		@test IG._grid_rgb_at(f.h,  1, z) == extra1  # tagged grid untouched
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# Fault trace = a 2-point OPEN line flagged isFault. The Scene Objects icon logic (50_scene.cpp) is
# `pg.isFault ? IC_Line` — ALWAYS the LINE icon, never the polygon icon. Lock the flags that drive it.
@testitem "fault trace is isFault + open (drives the LINE icon)" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	f = view_grid(G)
	try
		@test ccall(IG._fn(:gmtvtk_fault_add_test), Cint,
			(Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble), f.h, 2.0, 2.0, 7.0, 7.0) == 1
		IG._pump_once()
		buf = zeros(UInt8, 4096)
		n = ccall(IG._fn(:gmtvtk_scene_state), Cint, (Ptr{Cvoid}, Ptr{UInt8}, Cint), f.h, buf, length(buf))
		st = String(buf[1:n])
		poly0 = only(filter(kv -> startswith(kv, "poly0="), split(st, ';')))
		flags = split(split(poly0, ':')[1], '=')[2]      # "isFault,closed,nestKind"
		@test flags == "1,0,0"                            # isFault=1 (LINE icon), open, not a nest rect
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# ---- coverage for the previously-untested scene elements -----------------------------------------

@testitem "add! puts a line overlay on the window" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	f = view_grid(G)
	try
		add!(f, [1.0 1.0; 5.0 5.0; 8.0 2.0]; mode=:lines)
		IG._pump_once()
		st = IG._scene_state(f.h)
		@test st["n_overlays"] == 1
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "add_curtain! hangs a vertical curtain" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	if !isfile(IG._etopo4_logo())
		@test_skip "data/etopo4_logo.jpg not present"
	else
		G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
		f = view_grid(G)
		try
			add_curtain!(f, [0.0 0.0; 9.0 9.0]; image=IG._etopo4_logo(), zrange=(-100.0, 0.0))
			IG._pump_once()
			st = IG._scene_state(f.h)
			@test st["n_curtains"] == 1
		finally
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
		end
	end
end

@testitem "view_grid with a drape image carries a drape actor" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	if !isfile(IG._etopo4_path())
		@test_skip "data/etopo4.jpg not present"
	else
		G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[-10.0, 10.0], y=[-10.0, 10.0])
		I = IG._crop_etopo4(-10.0, 10.0, -10.0, 10.0)
		I.range = [-10.0, 10.0, -10.0, 10.0, 0.0, 255.0]   # tag the crop with the grid's geographic bbox
		f = view_grid(G; drape=I)
		try
			IG._pump_once()
			st = IG._scene_state(f.h)
			@test st["has_surface"] == 1
			@test st["drape"] == 1
		finally
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
		end
	end
end

@testitem "view_fv shows a solid/mesh surface" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	sq(x0, y0, z) = [x0 y0 z; x0+1 y0 z; x0+1 y0+1 z; x0 y0+1 z; x0 y0 z]
	fv = poly2fv([GMT.mat2ds(sq(0, 0, 0.0)), GMT.mat2ds(sq(2, 0, 1.0))])
	f = view_fv(fv)
	try
		IG._pump_once()
		st = IG._scene_state(f.h)
		@test st["alive"] == 1
		@test st["has_surface"] == 1
		@test st["imageOnly"] == 0
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "view_points opens a cloud; empty selection is nothing" tags=[:gui] begin
	IG = InteractiveGMT
	A = Float64[(j == 1 ? mod(i, 7) : j == 2 ? mod(2i, 5) : i / 3) for i in 1:60, j in 1:3]
	f = view_points(A)
	try
		IG._pump_once()
		st = IG._scene_state(f.h)
		@test st["alive"] == 1
		@test st["has_surface"] == 1       # the cloud actor counts as the surface
		@test selection(f) === nothing      # nothing picked yet
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

@testitem "show_table fills the Data Viewer rows" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	f = view_grid(G)
	try
		show_table(f, [1.0 2.0; 3.0 4.0; 5.0 6.0])
		IG._pump_once()
		st = IG._scene_state(f.h)
		@test st["n_table"] == 3
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# ---- X,Y plot tool (the standalone 2-D plotter) --------------------------------------------------

@testitem "xyplot opens, adds series, analysis grows it, saves a PNG" tags=[:gui, :xyplot] begin
	IG = InteractiveGMT
	t = collect(range(0, 2π; length=128))
	p = xyplot(t, sin.(t); name="sin", title="xy test", xlabel="x", ylabel="y")
	path = joinpath(tempdir(), "interactivegmt_xy_$(getpid()).png")
	try
		IG._pump_once()
		@test isalive(p)
		@test length(p.series) == 1
		add!(p, t, cos.(t); name="cos")
		@test length(p.series) == 2
		# a same-domain analysis op (compute + add, the path the Analysis menu drives)
		nx, ny, _ = IG._xy_compute("remove_mean", p.series[1][1], p.series[1][2])
		add!(p, nx, ny; name="sin − mean")
		@test length(p.series) == 3
		xtime!(p, :date); xtime!(p, :linear)          # mode switch must not throw / crash
		logscale!(p; y=true); logscale!(p; y=false)   # log toggle must not throw / crash
		IG._pump_once()
		isfile(path) && rm(path; force=true)
		@test save_png(path) == true
		IG._pump_once()
		@test isfile(path) && filesize(path) > 0
	finally
		ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), p.h)
		rm(path; force=true)
	end
end

@testitem "iview routes a 2-col table to the X,Y tool; 3-col stays a cloud" tags=[:gui, :xyplot] begin
	IG = InteractiveGMT; GMT = IG.GMT
	D2 = GMT.mat2ds([0.0 0.0; 1.0 1.0; 2.0 0.5; 3.0 2.0; 4.0 1.5])      # plain x,y table
	p = iview(D2)                                   # auto-route -> X,Y tool
	try
		@test p isa QtXYPlot
		@test isalive(p)
		@test length(p.series) == 1
	finally
		ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), p.h)
	end

	ii = collect(1:30)
	D3 = GMT.mat2ds(hcat(Float64.(mod.(ii, 7)), Float64.(mod.(2 .* ii, 5)), Float64.(ii) ./ 3))  # x y z -> cloud
	q = iview(D3)                                   # default 3-col -> 3-D cloud (unchanged behaviour)
	try
		@test q isa QtPoints
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), q.h)
	end

	r = iview(D3; xy=true)                          # force the X,Y tool: cols 2,3 become two series
	try
		@test r isa QtXYPlot
		@test length(r.series) == 2
	finally
		ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), r.h)
	end
end

@testitem "Spector-Grant fit recovers depth on a live spectrum" tags=[:gui, :xyplot] begin
	IG = InteractiveGMT
	h = 500.0
	k = collect(0.0002:0.0002:0.02)
	P = 1000.0 .* exp.(-4π * h .* k)                 # synthetic mag power spectrum, depth 500 m
	p = xyplot(k, P; name="PSD")
	try
		IG._pump_once()
		# the C fit the interactive drag tool uses, over the whole band, unit 1/m
		d = ccall(IG._fn(:gmtvtk_xyplot_specgrant), Cdouble,
			(Ptr{Cvoid}, Cint, Cdouble, Cdouble, Cdouble), p.h, Cint(0), 0.0, 1.0, 1.0)
		@test isapprox(d, 500.0; rtol=1e-3)
		# bad series -> NaN
		@test isnan(ccall(IG._fn(:gmtvtk_xyplot_specgrant), Cdouble,
			(Ptr{Cvoid}, Cint, Cdouble, Cdouble, Cdouble), p.h, Cint(9), 0.0, 1.0, 1.0))
	finally
		ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), p.h)
	end
end

@testitem "Tools > X,Y plot opens a mirror-registered blank window" tags=[:gui, :xyplot] begin
	IG = InteractiveGMT
	h = ccall(IG._fn(:gmtvtk_open_xyplot_from_host), Ptr{Cvoid}, ())   # same path as the menu action
	try
		IG._pump_once()
		@test h != C_NULL
		p = get(IG._FIGREG, h, nothing)
		@test p isa QtXYPlot                              # the new-window callback registered a mirror
		@test isalive(p)
		@test length(p.series) == 0                       # blank
	finally
		ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), h)
	end
end

@testitem "clear! empties an X,Y plot" tags=[:gui, :xyplot] begin
	IG = InteractiveGMT
	t = collect(range(0, 1; length=32))
	p = xyplot(t, t; name="a")
	try
		add!(p, t, t .^ 2; name="b")
		@test length(p.series) == 2
		clear!(p)
		@test length(p.series) == 0
		@test isalive(p)
	finally
		ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), p.h)
	end
end

# The 3-D Profile -> X,Y tool bridge: a populated Profile panel handed to the X,Y tool must spawn a
# window whose Julia mirror carries the series (so Save / Analysis work on it — the seed callback).
@testitem "profile_to_xyplot seeds a mirror with the panel series" tags=[:gui, :xyplot] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	f = view_grid(G)
	q = nothing
	try
		x = collect(0.0:0.5:10.0); y = sin.(x)
		ccall(IG._fn(:gmtvtk_show_profile_xy), Cint,
			(Ptr{Cvoid}, Ptr{Float64}, Ptr{Float64}, Cint, Cstring, Cstring, Cstring, Cint),
			f.h, x, y, length(x), "", "Distance", "Elevation", 0)
		IG._pump_once()
		q = profile_to_xyplot(f)
		@test isalive(q)
		@test length(q.series) == 1                   # seed callback populated the Julia mirror
	finally
		q === nothing || ccall(IG._fn(:gmtvtk_xyplot_close), Cvoid, (Ptr{Cvoid},), q.h)
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end
