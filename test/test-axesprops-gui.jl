# The Axes properties dialog (deps/ui/axes_props.ui, 56_axesprops.cpp): opened on a raster's own axes
# it loads with every tab and is on screen under its own title. An IMAGE has no Z, so on an image the
# 3-D and Z axis tabs and the vertical side are greyed out; on a grid everything is live.

@testitem "axes properties: the dialog opens on a grid's own axes with every tab live" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	fig = IG.view_grid(ve_grid(zspan = 1000.0)); ve_pump()
	h = getfield(fig, :h)
	png = tempname() * ".png"
	try
		@test ccall(_test_fn(:gmtvtk_axesprops_open_test), Cint, (Ptr{Cvoid}, Cint), h, Cint(4)) == 9
		ve_pump()
		@test ccall(_test_fn(:gmtvtk_toplevel_grab_test), Cint, (Cstring, Cstring), "Axes properties", png) == 1
		@test ccall(_test_fn(:gmtvtk_axesprops_tabmask_test), Cint, (Ptr{Cvoid},), h) == 0x1FF | (1 << 15)
	finally
		rm(png; force = true)
		ve_close(h)
	end
end

@testitem "axes properties: on an image every vertical property is greyed out" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	I = IG.GMT.mat2img(rand(UInt8, 32, 32))
	fig = IG.view_image(I); ve_pump()
	h = getfield(fig, :h)
	try
		@test ccall(_test_fn(:gmtvtk_axesprops_open_test), Cint, (Ptr{Cvoid}, Cint), h, Cint(0)) == 9
		ve_pump()
		m = ccall(_test_fn(:gmtvtk_axesprops_tabmask_test), Cint, (Ptr{Cvoid},), h)
		@test m & (1 << 1) == 0          # 3-D
		@test m & (1 << 4) == 0          # Z axis
		@test m & (1 << 15) == 0         # the Vertical side
		@test m & 0x1ED == 0x1ED         # every other tab live
	finally
		ve_close(h)
	end
end

@testitem "axes properties: Apply draws the fields on the raster's own axes" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	fig = IG.view_grid(ve_grid(zspan = 1000.0)); ve_pump()
	h = getfield(fig, :h)
	apply(k, v) = (o = zeros(3); r = ccall(_test_fn(:gmtvtk_axesprops_apply_test), Cint,
	              (Ptr{Cvoid}, Cstring, Cstring, Ptr{Cdouble}), h, k, v, o); (r, o))
	try
		@test ccall(_test_fn(:gmtvtk_axesprops_open_test), Cint, (Ptr{Cvoid}, Cint), h, Cint(0)) == 9
		ve_pump()
		r, o = apply("-Bpx.f", "auto")                       # fonts left at GMT's 12p black default ...
		@test r == 1 && o[1] == 10                           # ... keep the screen's own 10p white, never paper's
		r, o = apply("FONT_ANNOT_PRIMARY.size", "18p")
		@test r == 1 && o[1] == 18
		r, o = apply("MAP_TICK_PEN_PRIMARY.width", "3p")     # 3p = 4 px
		@test r == 1 && o[2] ≈ 4.0
		r, o = apply("MAP_FRAME_PEN.width", "3p")            # 3p = 4 px (1.5p is GMT's default: screen look kept)
		@test r == 1 && o[3] ≈ 4.0
	finally
		ve_close(h)
	end
end

@testitem "axes properties: Apply redraws the intervals, sides and grid of the axes on screen" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	fig = IG.view_grid(ve_grid(zspan = 1000.0)); ve_pump()      # x -15..-10, y 35..40
	h = getfield(fig, :h)
	ccall(_test_fn(:gmtvtk_set_flat2d_test), Cvoid, (Ptr{Cvoid}, Cint), h, 1); ve_pump()
	apply(k, v) = ccall(_test_fn(:gmtvtk_axesprops_apply_test), Cint,
	                    (Ptr{Cvoid}, Cstring, Cstring, Ptr{Cdouble}), h, k, v, zeros(3))
	counts() = (o = zeros(6); ccall(_test_fn(:gmtvtk_axesprops_counts_test), Cint, (Ptr{Cvoid}, Ptr{Cdouble}), h, o); o)
	try
		@test ccall(_test_fn(:gmtvtk_axesprops_open_test), Cint, (Ptr{Cvoid}, Cint), h, Cint(0)) == 9
		ve_pump()
		c0 = counts()
		@test c0[1] > 0 && c0[2] > 0                       # the map is annotated before any edit
		@test apply("-Bpx.a", "0.25") == 1                 # X annotated every 0.25 -> 21 numbers
		c1 = counts()
		@test c1[1] == 21
		@test c1[4] == 4                                   # the 4-side border, in the frame pen
		@test c1[3] == 0                                   # no grid yet
		@test apply("MAP_FRAME_AXES.Sg", "1") == 1         # grid from the South axis: one line per 0.25
		@test counts()[3] == 21
		@test apply("MAP_FRAME_AXES.Wg", "1") == 1         # ...and from the West
		c2 = counts()
		@test c2[3] > 21
		@test apply("MAP_FRAME_AXES.N", "Annot. + ticks") == 1
		@test counts()[1] == 42                            # North annotated too
		@test apply("MAP_FRAME_AXES.S", "Not drawn") == 1
		c3 = counts()
		@test c3[1] == 21 && c3[4] == 3                    # South gone: its numbers and its border line
		@test apply("-Bpy.a", "") == 1                     # empty a = Y not annotated
		@test counts()[2] == 0
	finally
		ve_close(h)
	end
end

@testitem "axes properties: Plot with GMT.jl draws the frame the dialog set" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	fig = IG.view_grid(ve_grid(zspan = 1000.0)); ve_pump()
	h = getfield(fig, :h)
	ccall(_test_fn(:gmtvtk_set_flat2d_test), Cvoid, (Ptr{Cvoid}, Cint), h, 1); ve_pump()
	apply(k, v) = ccall(_test_fn(:gmtvtk_axesprops_apply_test), Cint,
	                    (Ptr{Cvoid}, Cstring, Cstring, Ptr{Cdouble}), h, k, v, zeros(3))
	# The GMT command the script's first (framed) call runs, asked of GMT.jl itself (Vd=2: no plotting).
	function framecmd()
		steps, _ = IG._script_emit(h)
		s = steps[1]
		kw = Pair{Symbol,Any}[k => IG._script_resolve(v, Dict{Symbol,Any}()) for (k, v) in s.kw if k !== :cmap]
		pos = IG._script_resolve(s.pos, Dict{Symbol,Any}())
		return string(getfield(IG.GMT, s.mod)(pos; kw..., Vd = 2))
	end
	try
		@test occursin("-BWSen", framecmd())                  # never edited: the app's convention
		@test ccall(_test_fn(:gmtvtk_axesprops_open_test), Cint, (Ptr{Cvoid}, Cint), h, Cint(0)) == 9
		ve_pump()
		@test apply("-Bpx.a", "0.25") == 1
		@test apply("MAP_FRAME_AXES.Sg", "1") == 1
		@test apply("MAP_FRAME_AXES.N", "Annot. + ticks") == 1
		@test apply("MAP_FRAME_PEN.color", "#ff0000") == 1
		c = framecmd()
		@test occursin("-BWeSN", c)
		@test occursin(r"-Bp?xa0\.25f?g", c)                  # the 0.25 annotations + the South grid
		@test occursin("MAP_FRAME_PEN", c) && occursin("#ff0000", c)
		@test !occursin("MAP_TICK_LENGTH_PRIMARY", c)            # 5p/2.5p is GMT's own: left out
		@test !occursin("FONT_", c) && !occursin("MAP_TICK_PEN", c) && !occursin("MAP_GRID_PEN", c)   # untouched = GMT defaults
		@test apply("-Bpy.a", "0.25") == 1                       # Y spaced like X ...
		@test apply("MAP_FRAME_AXES.Wg", "1") == 1               # ... gridded like X
		c = framecmd()
		@test occursin(r"-Bp?a0.25f?g", c) && !occursin(r"-Bp?[xy]a", c)   # one frame, no xaxis/yaxis
	finally
		ve_close(h)
	end
end

@testitem "axes properties: a geographic raster's axes carry no lon/lat names" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	include(joinpath(@__DIR__, "ve_helpers.jl"))
	fig = IG.view_grid(ve_grid(zspan = 1000.0)); ve_pump()      # lon/lat grid, opened in 3-D
	h = getfield(fig, :h)
	counts() = (o = zeros(6); ccall(_test_fn(:gmtvtk_axesprops_counts_test), Cint, (Ptr{Cvoid}, Ptr{Cdouble}), h, o); o)
	try
		@test counts()[1] > 0                              # the axes are up (numbers drawn)
		@test counts()[6] == 0                             # but no "lon" / "lat" titles
	finally
		ve_close(h)
	end
end
