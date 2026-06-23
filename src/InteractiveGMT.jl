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
include("tilestool.jl")  # Tools > Tiles Tool (ported from Mirone tiles_tool.m; mosaic via GMT.mosaic)
include("bgregion.jl")   # File > Background region -> blank white 2-D map framed to W/E/S/N
include("savefile.jl")   # File > Save Grid / Save Image -> gmtwrite (netCDF/Surfer) / gdalwrite
include("geography.jl")  # Geography menu -> GSHHG coastlines for the current view

export view_grid, view_image, view_points, view_fv, view_demo, iview,
       add!, add_curtain!, add_symbols!, show_table, selection, isalive,
       poly2fv, colorize_by_z!, save_png, wait_windows, stereo!,
       xyplot, clear!, profile_to_xyplot, xtime!, logscale!, stickplot,
       QtFigure, QtPoints, QtFV, QtImage, QtEmpty, QtXYPlot

# NB: deliberately NO @compile_workload over the callbacks. `@cfunction` inference reaches into GMT,
# so precompiling the `_register_*` baked GMT specializations into our pkgimage — the cache ballooned
# by many MB. Callbacks are now thin invokelatest trampolines registered lazily on first window open
# (see `_ensure_callbacks` in eventloop.jl); their heavy bodies compile only when a menu is clicked.

# Load the viewer DLL + register the Julia-console callback. RUNTIME ONLY — a dlopen handle, the
# dlsym pointers and the @cfunction are all runtime values that cannot be baked into a precompiled
# image, so they must be created here, never at top level. Tolerant of a missing/unbuilt DLL (and
# of non-Windows) so `using InteractiveGMT` still succeeds; viewer calls then error on first use.
function __init__()
	# ONLY load the DLL here. The dlopen handle + dlsym pointers are runtime values that can't be
	# baked into a precompiled image, so they must resolve at load. Everything else (the 11 callback
	# registrations) is deferred to the first window open via `_ensure_callbacks` (eventloop.jl) — it
	# kept `using InteractiveGMT` from paying @cfunction inference for every GMT-touching callback.
	# Tolerant of a missing/unbuilt DLL (and non-Windows) so `using` still succeeds; viewer calls then
	# error on first use.
	try
		_load_library()
		# Global UI assets the viewer bakes into every window's toolbar at build time, so they must be
		# set BEFORE the first window opens — cheap static path pushes, no GMT inference (unlike the
		# callbacks, which stay lazy in _ensure_callbacks). Guarded: a DLL missing the export must not
		# block loading.
		try
			_install_basemap_assets()
		catch e
			@warn "InteractiveGMT: could not install basemap toolbar assets (rebuild deps/build.bat if the export is missing)." exception=(e,)
		end
		try
			_install_tiles_assets()
		catch e
			@warn "InteractiveGMT: could not install Tiles Tool world image (rebuild deps/build.bat if the export is missing)." exception=(e,)
		end
	catch e
		@warn "InteractiveGMT: the Qt+VTK viewer DLL could not be loaded; build it with deps/build.bat (Windows only). Viewer calls will error until then." exception=(e,)
	end
end

end # module InteractiveGMT
