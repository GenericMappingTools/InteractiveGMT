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
		# The drop path's netCDF subdataset probe (_netcdf_subdatasets, drop.jl) runs GMT.gdalinfo on
		# every .nc/.grd it sees — and GDAL's dataset cache keeps the file handle open afterward with
		# no Julia-visible reference to release (confirmed live: a bare `GMT.gdalinfo(path)` alone, no
		# window/drop involved, leaves the SAME file EBUSY to unlink; GC.gc(), a sleep, and GMT.jl's
		# own session teardown all failed to clear it). Windows enforces the lock strictly (no
		# delete-while-open); a few short retries is the standard, safe way to ride out a transient
		# GDAL/OS file lock without reaching into GDAL's global driver-manager cache from a test.
		for _ in 1:20
			try
				rm(path; force=true)
				break
			catch e2
				e2 isa Base.IOError || rethrow()
				sleep(0.1)
			end
		end
	end
end

# The Vertical-elastic-deformation fault plane: a fault + the dialog's plane build must create the
# buried 3-D dipping plane (hidden in flat-2D, shown in 3-D) AND a "<fault> — plane" Scene Objects
# handle row. This is the regression for the "no 3-D plane / no handle" episode.
@testitem "fault plane: buried 3-D plane + Scene Objects handle" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[100*sin(ix/3)+50*cos(iy/4) for iy in 0:40, ix in 0:40]; x=[0.0, 40.0], y=[0.0, 40.0])
	f = view_grid(G)
	try
		IG._pump_once()
		ccall(_test_fn(:gmtvtk_fault_add_test), Cint, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble),
		      f.h, 5.0, 8.0, 35.0, 30.0)
		out = zeros(Cdouble, 6)
		ex = ccall(_test_fn(:gmtvtk_fault_plane_test), Cint,
		           (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cint, Ptr{Cdouble}), f.h, 20.0, 30.0, 50.0, 0, out)
		@test ex == 1              # 3-D plane actor exists
		@test out[2] == 4          # quad: 4 corners
		@test out[4] > out[5]      # top above bottom (dips down)
		@test out[6] == 1          # gray surface patch visible

		# view_grid opens flat-2D -> 3-D plane hidden; switching to 3-D reveals it.
		@test out[3] == 0          # hidden in flat-2D
		ccall(_test_fn(:gmtvtk_set_flat2d_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, 0)
		IG._pump_once()
		ccall(_test_fn(:gmtvtk_fault_plane_test), Cint,
		      (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cint, Ptr{Cdouble}), f.h, 20.0, 30.0, 50.0, 0, out)
		@test out[3] == 1          # visible in 3-D

		rows = unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h))
		@test occursin("plane", rows)   # the "<fault> — plane" handle row exists

		# THE bug: the plane was a dialog-time preview, wiped when the dialog closed. Drive the REAL
		# dialog lifecycle and assert the plane + handle SURVIVE the close.
		@test ccall(_test_fn(:gmtvtk_fault_open_dialog_test), Cint, (Ptr{Cvoid},), f.h) == 1
		IG._pump_once()
		ccall(_test_fn(:gmtvtk_fault_plane_test), Cint,
		      (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cint, Ptr{Cdouble}), f.h, 20.0, 30.0, 50.0, 0, out)
		ccall(_test_fn(:gmtvtk_fault_close_dialog_test), Cvoid, (Ptr{Cvoid},), f.h)
		IG._pump_once()
		@test ccall(_test_fn(:gmtvtk_fault_plane_test), Cint,
		            (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cint, Ptr{Cdouble}), f.h, 20.0, 30.0, 50.0, 0, out) == 1
		@test occursin("plane", unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), f.h)))
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# A symbol layer that asked for spheres (a seismicity catalog) must draw CIRCLES on a flat-2-D map —
# a sphere is a 3-D body and a top-down map has no third dimension to show it in — and must honour its
# PER-POINT sizes in BOTH pipelines. Both halves were broken: the layer stayed a sphere in 2-D, and the
# GPU-instanced sphere mapper ignored the size factors, drawing every event at the layer's base size.
@testitem "symbol layer: sphere -> circle in flat-2D, per-point sizes in both" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	f = view_grid(G)                      # view_grid opens flat-2D
	try
		IG._pump_once()
		IG.add_symbols!(f.h, [2.0, 4.0, 6.0], [2.0, 4.0, 6.0]; z=[-1.0, -2.0, -3.0],
		                symbol=:sphere, size=[4.0, 8.0, 12.0], fill=:red, name="Seismicity")
		IG._pump_once()
		out = zeros(Cdouble, 5)
		@test ccall(_test_fn(:gmtvtk_symbol_layer_test), Cint,
		            (Ptr{Cvoid}, Cint, Ptr{Cdouble}), f.h, 0, out) == 1
		@test out[2] == 1                 # wantSolid: it asked for spheres …
		@test out[1] == 0                 # … but on a flat-2-D map it draws the flat counterpart
		@test out[3] == 1                 # and it scales by the per-point factors
		@test out[4] == 3                 # 3 points

		# … and on a flat map it draws OVER the raster: a marker seen straight down must show
		# whatever its depth (the depth-cleared overlay layer).
		toplayer(i) = ccall(_test_fn(:gmtvtk_symbol_toplayer_test), Cint, (Ptr{Cvoid}, Cint), f.h, i)
		@test toplayer(0) == 1

		ccall(_test_fn(:gmtvtk_set_flat2d_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, 0)   # tilt to 3-D
		IG._pump_once()
		ccall(_test_fn(:gmtvtk_symbol_layer_test), Cint, (Ptr{Cvoid}, Cint, Ptr{Cdouble}), f.h, 0, out)
		@test out[1] == 1                 # real spheres again …
		@test out[3] == 1                 # … still scaled per point (the vtkGlyph3DMapper bug)
		# THE 3-D rule: a buried hypocentre is a real body — it goes back to the MAIN renderer and
		# loses the depth test to the surface above it. Left on the overlay layer (the kind switch
		# used not to restack) the events were drawn straight THROUGH the grid.
		@test toplayer(0) == 0

		ccall(_test_fn(:gmtvtk_set_flat2d_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, 1)   # and back
		IG._pump_once()
		ccall(_test_fn(:gmtvtk_symbol_layer_test), Cint, (Ptr{Cvoid}, Cint, Ptr{Cdouble}), f.h, 0, out)
		@test out[1] == 0 && out[3] == 1
		@test toplayer(0) == 1

		# A layer that asked for a FLAT glyph is left alone by the mode switch.
		IG.add_symbols!(f.h, [1.0], [1.0]; symbol=:triangle, size=6.0, name="Flat")
		IG._pump_once()
		ccall(_test_fn(:gmtvtk_symbol_layer_test), Cint, (Ptr{Cvoid}, Cint, Ptr{Cdouble}), f.h, 1, out)
		@test out[1] == 0 && out[2] == 0 && out[3] == 0
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# Hover asks "is the terrain BETWEEN the camera and this event", never "is this event below the
# ground". The two differ the moment the camera is not looking straight down, and answering the
# second one killed the tooltip for every event with a real depth: only depth-0 events (which sit
# ABOVE the surface) ever answered. Depth by itself must never disable hover.
@testitem "hover: a buried hypocentre still answers; the terrain only hides its pixels" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[0.0 for iy in 0:20, ix in 0:20]; x=[0.0, 20.0], y=[0.0, 20.0])  # lid at z=0
	f = view_grid(G)
	buf = Vector{UInt8}(undef, 1024)
	hover(x, y, z) = (n = ccall(_test_fn(:gmtvtk_symbol_hover_test), Cint,
	                            (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Ptr{UInt8}, Cint),
	                            f.h, x, y, z, buf, length(buf));
	                  n <= 0 ? "" : String(buf[1:n]))
	flat2d(on) = (ccall(_test_fn(:gmtvtk_set_flat2d_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, on);
	              IG._pump_once())
	try
		IG._pump_once()
		# under the lid · above the lid · deep but OUTSIDE the grid, so nothing is in the way
		IG.add_symbols!(f.h, [10.0, 5.0, 30.0], [5.0, 15.0, 10.0];
		                z=[-100000.0, 100000.0, -100000.0], symbol=:sphere, size=40.0,
		                fill=(1.0, 0.0, 1.0), name="evts", info=["BURIED", "ABOVE", "OUTSIDE"])
		IG._pump_once()
		@test hover(10.0, 5.0, -100000.0) == "BURIED"      # flat map: depth is not in play at all

		flat2d(0)                                           # tilt into 3-D
		# PICKING IS NOT RENDERING (10_geometry.cpp, above pickSymbolAt). Every one of these still
		# answers: burial is a fact about the ground, visibility a fact about the camera, and a
		# tooltip is information, not paint. Two gates that made buried events go silent were
		# deliberately removed — they left a 3-D view where only events shallow enough to stick out
		# above the ground could be interrogated at all — and the source says in as many words not to
		# add a third. What the terrain DOES hide is the pixels, and that law has its own test below
		# ("a surface hides what is buried under it"), asserted on the framebuffer.
		@test hover(10.0, 5.0, -100000.0) == "BURIED"      # under the lid, and still interrogable
		@test hover(5.0, 15.0, 100000.0)  == "ABOVE"
		@test hover(30.0, 10.0, -100000.0) == "OUTSIDE"

		# What DOES change between the two views is where the layer is parked: in 3-D it sits in the
		# main renderer so the real depth test can hide it, and in flat-2-D it is promoted to the
		# depth-cleared overlay layer so a map marker shows whatever its depth.
		toplayer() = ccall(_test_fn(:gmtvtk_symbol_toplayer_test), Cint, (Ptr{Cvoid}, Cint), f.h, Cint(0))
		@test toplayer() == 0
		flat2d(1)
		@test toplayer() == 1
		@test hover(10.0, 5.0, -100000.0) == "BURIED"
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# THE LAW, asserted on the RENDERED PIXELS, not on the scene graph: in 3-D, anything below a surface
# is not visible through that surface. Every earlier test of this checked a proxy — which glyph the
# layer drew, which renderer held it — and a proxy is exactly what let "events drawn through the
# grid" ship twice. This one reads the framebuffer: a magenta sphere 100 km under an opaque grid must
# contribute ZERO pixels. The two other legs stop it passing for the wrong reason: the same sphere
# IS counted in the flat-2-D map view (a marker shows whatever its depth), and a symbol ABOVE the
# surface IS counted in the very same 3-D view (so "0" can never mean "symbols never render here").
@testitem "3-D: a surface hides what is buried under it (pixels, not proxies)" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT
	# REAL RELIEF, and depths of the same order as it. A flat z=0 grid has no Z span at all, so a
	# symbol at ±100 km lands far outside the picture and counts 0 pixels for the wrong reason —
	# "off-frame" would read exactly like "occluded". The buried one also sits well INSIDE the
	# footprint (not near the front edge): a surface has no skirt, so from a low oblique angle you
	# genuinely can see under its edge, and that is not a bug. The two symbols are at different
	# (x,y) — on one spot the later-drawn covers the other and this would measure paint order.
	G = GMT.mat2grid(Float32[500 + 300*sin(ix/3.0) for iy in 0:20, ix in 0:20]; x=[0.0, 20.0], y=[0.0, 20.0])
	f = view_grid(G)
	px(r, g, b) = ccall(_test_fn(:gmtvtk_pixel_count_test), Cint,
	                    (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble, Cdouble), f.h, r, g, b, 0.25)
	flat2d(on) = (ccall(_test_fn(:gmtvtk_set_flat2d_test), Cvoid, (Ptr{Cvoid}, Cint), f.h, on);
	              IG._pump_once())
	try
		IG._pump_once()
		magenta0 = px(1.0, 0.0, 1.0)
		yellow0  = px(1.0, 1.0, 0.0)              # baselines: whatever the grid itself paints

		# A hypocentre UNDER the surface, well inside the grid's footprint.
		IG.add_symbols!(f.h, [10.0], [14.0]; z=[-500.0], symbol=:sphere, size=30.0,
		                fill=(1.0, 0.0, 1.0), name="Deep")
		IG._pump_once()
		@test px(1.0, 0.0, 1.0) > magenta0 + 100   # flat-2-D map: the marker shows

		flat2d(0)                                  # tilt into 3-D
		@test px(1.0, 0.0, 1.0) == magenta0        # …and the surface above it hides it completely

		# Same view, same layer kind, a symbol ABOVE the surface: still drawn. Without this leg a
		# regression that stopped drawing symbols in 3-D altogether would pass the test above.
		IG.add_symbols!(f.h, [6.0], [6.0]; z=[2500.0], symbol=:sphere, size=30.0,
		                fill=(1.0, 1.0, 0.0), name="High")
		IG._pump_once()
		@test px(1.0, 1.0, 0.0) > yellow0 + 100

		flat2d(1)                                  # back to the map: depth stops mattering again
		@test px(1.0, 0.0, 1.0) > magenta0 + 100
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# "Show data table" shows THE DATA — a catalog's own lon/lat/depth/magnitude/date, handed over by
# whoever plotted it. Never a graphical property: how big a circle is drawn is a fact about the
# picture, not about the earthquake, and has no business in a data table.
@testitem "symbol layer data table carries the catalog, not graphics" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	f = view_grid(G)
	buf = Vector{UInt8}(undef, 4096)
	table(h, i) = (n = ccall(_test_fn(:gmtvtk_symbol_table_test), Cint,
	                         (Ptr{Cvoid}, Cint, Ptr{UInt8}, Cint), h, i, buf, length(buf));
	               n <= 0 ? "" : String(buf[1:n]))
	try
		IG._pump_once()
		IG.add_symbols!(f.h, [2.0, 4.0], [3.0, 5.0]; symbol=:sphere, size=[6.0, 12.0], name="Cat",
		                datanames=["Lon", "Lat", "Mag"],
		                datarows=[["2.0", "3.0", "5.1"], ["4.0", "5.0", "6.2"]])
		IG._pump_once()
		t = split(table(f.h, 0), ';')
		@test t[1] == "3" && t[2] == "2"          # 3 columns, one row per point
		@test t[3] == "Lon,Lat,Mag"               # the catalog's own columns …
		@test t[4] == "2.0,3.0,5.1"
		@test !occursin("Size", t[3])             # … and nothing about how it is drawn

		# A layer with no data of its own carries no table (the viewer then shows its coordinates).
		IG.add_symbols!(f.h, [1.0], [1.0]; symbol=:circle, size=6.0, name="Plain")
		IG._pump_once()
		@test startswith(table(f.h, 1), "0;0;")
		# A table that does not align 1:1 with the points is refused, never shown half-filled.
		IG.add_symbols!(f.h, [1.0, 2.0], [1.0, 2.0]; symbol=:circle, size=6.0, name="Bad",
		                datanames=["A"], datarows=[["1"]])
		IG._pump_once()
		@test startswith(table(f.h, 2), "0;0;")
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# A depth-bearing overlay (a seismicity catalog) may grow the Z FRAME of a GRID — that is what puts
# its hypocentres inside the axes cube. It may NOT grow the frame of an IMAGE window: an image has no
# Z, and the blank scaffold plane such a window carries has a synthetic 0..1 Z that is not data.
# Stretching that to -600 km rebuilt the window's bounds hundreds of km tall and everything in the
# main renderer fell outside the camera's clipping range — a blank canvas with the axes still drawn
# and every row still checked in Scene Objects ("the second plot deletes the basemap and shows no
# circles"). One rule: no grid on display -> nothing to grow.
@testitem "Z frame grows for a grid, never for an image window" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT
	ax = zeros(Cdouble, 7)
	frame(h) = (ccall(_test_fn(:gmtvtk_active_axes_test), Cint, (Ptr{Cvoid}, Ptr{Cdouble}), h, ax); copy(ax))
	G = GMT.mat2grid(Float32[100*sin(ix/3) for iy in 0:20, ix in 0:20]; x=[-10.0, 10.0], y=[-10.0, 10.0])
	f = view_grid(G)
	try
		IG._pump_once()
		before = frame(f.h)
		@test before[1] == 1                           # the grid's own set owns the display
		ccall(IG._fn(:gmtvtk_grow_z_frame_h), Cvoid, (Ptr{Cvoid}, Cdouble, Cdouble), f.h, -600000.0, 0.0)
		IG._pump_once()
		after = frame(f.h)
		@test after[6] < before[6]                     # a grid's Z frame DOES follow the catalog down
		@test after[2] == before[2] && after[3] == before[3]   # …and X/Y are never touched by an overlay
		@test after[4] == before[4] && after[5] == before[5]
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
	if !isfile(IG._etopo4_path())
		@test_skip "data/etopo4.jpg not present"
	else
		e = iview()
		try
			s = "-110/-55/-5/33/0/region"
			GC.@preserve s IG._on_basemap(e.h, Base.unsafe_convert(Cstring, s))
			IG._pump_once()
			before = frame(e.h)
			ccall(IG._fn(:gmtvtk_grow_z_frame_h), Cvoid, (Ptr{Cvoid}, Cdouble, Cdouble), e.h, -600000.0, 0.0)
			IG._pump_once()
			@test frame(e.h) == before                  # an image window's frame is untouched, Z included
			@test IG._scene_state(e.h)["extravis0"] == 1   # …and the picture is still on screen
		finally
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), e.h)
		end
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
@testitem "fault trace is isFault + open (drives the LINE icon)" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[ix + iy for iy in 0:9, ix in 0:9]; x=[0.0, 9.0], y=[0.0, 9.0])
	f = view_grid(G)
	try
		@test ccall(_test_fn(:gmtvtk_fault_add_test), Cint,
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
		# _crop_etopo4 hands back the crop AND the extent its pixels really cover (the request is
		# rounded and clamped to the image): georeference it with THAT, never with what was asked for.
		I, W, E, S, N = IG._crop_etopo4(-10.0, 10.0, -10.0, 10.0)
		I.range = [W, E, S, N, 0.0, 255.0]
		f = view_grid(G; drape=I)
		try
			IG._pump_once()
			st = IG._scene_state(f.h)
			@test st["has_surface"] == 1
			@test st["drape"] == 1
		finally
			# gmtvtk_close is async: pump so this window is really gone before the next test item runs,
			# instead of leaving its destruction (and the host callbacks it fires) to land in the middle
			# of an unrelated one — the same ordering hazard the X,Y items below guard against.
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
			IG._pump_once()
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
		@test size(selection(f), 1) == 0    # nothing picked yet -- selection() always returns a Matrix (points.jl docstring)
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
	# "X,Y tool: one window, new page" (xyplot.jl _XY_CURRENT) is deliberate: a live X,Y window means
	# the NEXT xyplot() call adds a page to it rather than opening a fresh one. A sibling test item's
	# window may still be closing (gmtvtk_xyplot_close is async, same as gmtvtk_close) and so still
	# reads as alive here -- force a genuinely FRESH window for this test regardless of ordering/timing.
	IG._XY_CURRENT[] = C_NULL
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

	IG._XY_CURRENT[] = C_NULL   # p's window close (line 394) is async and unpumped -- same reuse risk as above
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

# The FOURTH view mode: the QSC cube. It is the globe's twin — same flags, same engine, only the
# transform inside the scene's one mapping differs — so what is asserted here is that the four-state
# really is a four-state: every transition lands where it was told, including globe <-> cube, which
# swaps the body INSIDE a mapping object that every already-attached transform filter is holding a
# pointer to. (What the cube's geometry then IS gets checked against PROJ in the unit item below;
# the visible-region hook that would show it end-to-end returns nothing under the test runner's
# offscreen window — for the globe just as much as for the cube — so it is no use here.)
@testitem "view modes: 3-D / flat-2D / globe / QSC cube all round-trip" tags=[:gui] begin
	IG = InteractiveGMT; GMT = IG.GMT
	G = GMT.mat2grid(Float32[100sind(2lat) + 50cosd(3lon) for lat in -60.0:2.0:60.0, lon in -180.0:2.0:178.0];
	                 x=collect(-180.0:2.0:178.0), y=collect(-60.0:2.0:60.0))
	G.proj4 = "+proj=longlat +datum=WGS84 +no_defs"
	f = view_grid(G)
	try
		setmode(m) = ccall(IG._fn(:gmtvtk_set_view_mode_h), Cint, (Ptr{Cvoid}, Cint), f.h, Cint(m))
		getmode()  = ccall(IG._fn(:gmtvtk_get_view_mode_h), Cint, (Ptr{Cvoid},), f.h)
		@test IG._QSC_DONE[]                          # the +proj=qsc table reached the library
		for m in (3, 2, 3, 1, 3, 0, 3)                # every transition, cube <-> each of the others
			@test setmode(m) == 1
			IG._pump_once()
			@test getmode() == m
		end
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
	end
end

# The Cube mode's PROJECTION, checked against PROJ itself rather than against a stored expectation:
# the viewer's cube is only as good as the table qsccube.jl samples out of `+proj=qsc`, so what has
# to hold is that a fold-and-interpolate lookup of that table reproduces GDAL/PROJ over the whole
# face. This is also the guard on the OCTANT FOLD: QSC is C0 but not C1 across the face diagonals,
# and a table sampled on a plain (a,b) square instead of on (a, b/a) is ~80x worse right there —
# well outside the tolerance below, so a regression to the naive layout fails this outright.
@testitem "QSC cube: the sampled warp reproduces PROJ over a whole face" tags=[:unit] begin
	IG = InteractiveGMT; GMT = IG.GMT
	n = IG._QSC_N
	fwd, back = IG._qsc_warp_tables()
	# The same fold + bilinear lookup the viewer does (cubeWarpApply, 10_geometry.cpp).
	function look(tab::Vector{Float64}, p::Float64, q::Float64)
		sp = p < 0 ? -1.0 : 1.0;  sq = q < 0 ? -1.0 : 1.0
		A = abs(p);  B = abs(q);  swapped = false
		if B > A;  A, B = B, A;  swapped = true;  end
		s  = A > 0 ? B / A : 0.0
		gx = clamp(A, 0, 1) * (n - 1);  gy = clamp(s, 0, 1) * (n - 1)
		i0 = min(floor(Int, gx), n - 2);  j0 = min(floor(Int, gy), n - 2)
		fx = gx - i0;  fy = gy - j0
		at(i, j, c) = tab[(j * n + i) * 2 + c]
		o = ntuple(2) do c
			(1-fx)*(1-fy)*at(i0,j0,c) + fx*(1-fy)*at(i0+1,j0,c) +
			(1-fx)*fy*at(i0,j0+1,c)   + fx*fy*at(i0+1,j0+1,c)
		end
		u, v = swapped ? (o[2], o[1]) : (o[1], o[2])
		return (sp * u, sq * v)
	end
	# A deterministic sweep of the whole front face, deliberately OFF the table's own nodes (the
	# irrational offsets) so every sample is interpolated, and dense enough that plenty of them land
	# in the cells the two face diagonals cut through — which is the only place this can go wrong.
	m  = 41
	ab = reduce(vcat, [[(-1 + 2*(i - 0.5 + 0.113)/m)  (-1 + 2*(j - 0.5 + 0.371)/m)]
	                   for j in 1:m for i in 1:m])
	np = size(ab, 1)
	ll = reduce(vcat, [begin a = ab[k,1]; b = ab[k,2]
	                         [atand(a, 1.0)  asind(b / sqrt(1 + a*a + b*b))]
	                   end for k in 1:np])
	ref = GMT.lonlat2xy(ll, t_srs=IG._QSC_SRS) ./ IG._QSC_R
	ef  = maximum(abs(look(fwd, ab[k,1], ab[k,2])[c] - ref[k,c]) for k in 1:np, c in 1:2)
	@test ef < 1e-5                                   # measured 2.8e-6 = 1.6e-4 deg of arc, ~18 m
	# …and the inverse table lands back on the point the forward one came from.
	rt = maximum(begin u, v = look(fwd, ab[k,1], ab[k,2])
	                   a2, b2 = look(back, u, v)
	                   max(abs(a2 - ab[k,1]), abs(b2 - ab[k,2]))
	             end for k in 1:np)
	@test rt < 1e-4
	# The three symmetries the fold rests on, straight from PROJ: they are what lets ONE octant of one
	# face carry the whole cube. Broken here = the fold above silently projects to the wrong place.
	W(a, b) = vec(GMT.lonlat2xy([atand(a, 1.0)  asind(b / sqrt(1 + a*a + b*b))],
	                            t_srs=IG._QSC_SRS) ./ IG._QSC_R)
	for (a, b) in ((0.4, 0.7), (0.9, 0.15), (0.25, 0.25))
		u, v = W(a, b)
		@test isapprox(W(-a,  b), [-u,  v]; atol=1e-12)
		@test isapprox(W( a, -b), [ u, -v]; atol=1e-12)
		@test isapprox(W( b,  a), [ v,  u]; atol=1e-12)
	end
end
