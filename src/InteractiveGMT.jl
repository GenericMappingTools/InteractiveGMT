"""
InteractiveGMT — interactive 3-D viewing of GMT.jl data (grids, point clouds, GMTfv solids /
polygon meshes) in a self-contained **Qt6 + VTK** window. No dependency on F3D.

The viewer is a small C/C++ shared library (`deps/build/gmtvtk.dll`, built by `deps/build.bat`)
driven through a tiny C API; this package is the Julia bridge. Calls are NON-BLOCKING: they
return a live handle immediately and a Julia `Timer` pumps the Qt loop so the REPL stays usable.

Public API: [`view_grid`](@ref), [`view_points`](@ref), [`view_fv`](@ref), [`f3dview`](@ref),
[`add!`](@ref), [`add_curtain!`](@ref), [`show_table`](@ref), [`selection`](@ref),
[`poly2fv`](@ref), [`isalive`](@ref), [`save_png`](@ref), [`view_demo`](@ref).

Windows-only (the viewer DLL is a Windows binary).
"""
module InteractiveGMT

using GMT
using PrecompileTools

# --- C-API DLL loader (resolved at runtime in __init__; see libgmtvtk.jl) ----------------
include("libgmtvtk.jl")

# --- handles, event loop, in-window Julia console ----------------------------------------
include("types.jl")
include("introspect.jl") # read-only scene-state snapshot for the test suite
include("crs.jl")        # centralized coordinate-reference-system store (proj4/wkt/epsg)
include("eventloop.jl")
include("console.jl")

# --- shared helpers ----------------------------------------------------------------------
include("colors.jl")
include("symbols.jl")    # generic screen-constant symbol layers (volcanoes, seismicity, …)
include("cpt.jl")
include("drape.jl")

# --- viewers + scene elements ------------------------------------------------------------
include("grid.jl")
include("table.jl")
include("curtain.jl")
include("points.jl")
include("fv.jl")
include("dispatch.jl")
include("xyplot.jl")     # standalone X,Y plot tool (vtkChartXY); evolution of the Profile
include("xyanalysis.jl") # X,Y Analysis menu (remove mean/trend, derivatives, FFT, autocorr)
include("xystick.jl")    # stick (vector) diagrams for the X,Y tool (ecran 'stick')
include("drop.jl")
include("basemap.jl")    # World Topo Tiles picker (ported from Mirone bg_map.m)
include("bgregion.jl")   # File > Background region -> blank white 2-D map framed to W/E/S/N
include("geography.jl")  # Geography menu -> GSHHG coastlines for the current view

export view_grid, view_image, view_points, view_fv, view_demo, iview,
       add!, add_curtain!, add_symbols!, show_table, selection, isalive,
       poly2fv, colorize_by_z!, save_png, wait_windows, stereo!,
       xyplot, clear!, profile_to_xyplot, xtime!, logscale!, stickplot,
       QtFigure, QtPoints, QtFV, QtImage, QtEmpty, QtXYPlot

@compile_workload begin
	_load_library(),
	_register_console_eval(), _register_drop_callback(), _register_xy_callback(), _register_xy_analysis(), _register_xy_seed(),
	_register_xy_new(), _register_basemap(), _register_bgregion(), _register_geography(), _register_tides(), _register_earthtide()
end
	
# Load the viewer DLL + register the Julia-console callback. RUNTIME ONLY — a dlopen handle, the
# dlsym pointers and the @cfunction are all runtime values that cannot be baked into a precompiled
# image, so they must be created here, never at top level. Tolerant of a missing/unbuilt DLL (and
# of non-Windows) so `using InteractiveGMT` still succeeds; viewer calls then error on first use.
function __init__()
	# Loading the DLL is fatal: every registration below dlsyms into it, so if it can't load there
	# is nothing to wire — warn once and bail (viewer calls then error on first use).
	try
		_load_library()
	catch e
		@warn "InteractiveGMT: the Qt+VTK viewer DLL could not be loaded; build it with deps/build.bat (Windows only). Viewer calls will error until then." exception=(e,)
		return
	end
	# Each callback registration is guarded on its own: one failing dlsym (e.g. a DLL built before a
	# given export existed) must NOT silently skip the others. A skipped registration shows up as
	# "<feature>: not wired" in the live window, so name the failure here to make that traceable.
	for (name, fn) in (("console",   _register_console_eval),
	                    ("drop",      _register_drop_callback),
	                    ("xy",        _register_xy_callback),
	                    ("xy-analysis", _register_xy_analysis),
	                    ("xy-seed",   _register_xy_seed),
	                    ("xy-new",    _register_xy_new),
	                    ("basemap",   _register_basemap),
	                    ("bgregion",  _register_bgregion),
	                    ("geography", _register_geography),
	                    ("tides",     _register_tides),
	                    ("earthtide", _register_earthtide))
		try
			fn()
		catch e
			@warn "InteractiveGMT: registration '$name' failed; that feature will be \"not wired\" in the viewer. Rebuild the DLL (deps/build.bat) and restart Julia if the export is missing." exception=(e,)
		end
	end
end

end # module InteractiveGMT
