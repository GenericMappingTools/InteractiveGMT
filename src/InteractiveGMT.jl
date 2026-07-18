"""
InteractiveGMT — interactive 3-D viewing of GMT.jl data (grids, point clouds, GMTfv solids /
polygon meshes) in a self-contained **Qt6 + VTK** window. No dependency on F3D.

The viewer is a small C/C++ shared library (`deps/build/gmtvtk.dll`, built by `deps/build.bat`)
driven through a tiny C API; this package is the Julia bridge. Calls are NON-BLOCKING: they
return a live handle immediately and a Julia `Timer` pumps the Qt loop so the REPL stays usable.

Public API: [`view_grid`](@ref), [`view_points`](@ref), [`view_fv`](@ref), [`iview`](@ref),
[`add!`](@ref), [`add_curtain!`](@ref), [`show_table`](@ref), [`selection`](@ref),
[`poly2fv`](@ref), [`isalive`](@ref), [`save_png`](@ref), [`view_demo`](@ref).

Windows-only (the viewer DLL is a Windows binary).
"""
module InteractiveGMT

using GMT
using Distributed: addprocs, workers, rmprocs, remotecall, remotecall_eval
using PrecompileTools: @setup_workload, @compile_workload

# --- C-API DLL loader (resolved at runtime in __init__; see libgmtvtk.jl) ----------------
include("libgmtvtk.jl")
include("selfupdate.jl") # update!() -- pull + rebuild in place, for a `] dev`-installed checkout

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
include("dimfun.jl")     # grdsample Region box recompute (port of Mirone dim_funs.m)
include("grdsample.jl")  # GMT > Resample (grdsample)
include("bgregion.jl")   # File > Background region -> blank white 2-D map framed to W/E/S/N
include("savefile.jl")   # File > Save Grid / Save Image -> gmtwrite (netCDF/Surfer) / gdalwrite
include("session.jl")    # File > Save/Load Session (.igmtz): provenance registry + manifest + store-only zip
include("geography.jl")  # Geography menu -> GSHHG coastlines for the current view
include("solids.jl")     # 3-D Bodies toolbar flyout -> GMT solids (cube/sphere/torus/cylinder/…) via view_fv
include("nswing.jl")     # Geophysics > NSWING tsunami (port of Mirone swan_options.m -> nswing exe)
include("aquamoto.jl")   # Geophysics > Tsunamis > Aquamoto viewer (port of Mirone aquamoto.m netCDF tab)
include("igrf.jl")       # Geophysics > Magnetics > IGRF (port of Mirone igrf_options.m; GMT.magref)
include("seismicity.jl") # Geophysics > Seismology > Seismicity (port of Mirone earthquakes.m)
include("focal.jl")      # Geophysics > Seismology > Focal mechanisms (port of Mirone focal_meca.m)
include("deform.jl")     # Geophysics > Vertical elastic deformation: fault-trace endpoint (deform_mansinha.m)
include("nested.jl")     # "Nested grids" rectangle tool: host-side blank-grid builder (nesting_sizes.m)
include("transplant.jl") # Grid Tools > Transplant 2nd grid (port of Mirone utils/transplants.m)
include("measure.jl")    # line length/azimuth + polygon area for the vector context menu (CRS-aware)
include("info.jl")       # toolbar "i" button: grdinfo / gdalinfo report on the active grid/image
include("rtp3d.jl")      # Geophysics > Magnetics: reduce-to-pole via 2-D FFT (port of Mirone utils/rtp3d.m)

export view_grid, view_image, view_points, view_fv, view_demo, iview,
       add!, add_curtain!, add_symbols!, show_table, selection, isalive,
       poly2fv, colorize_by_z!, save_png, wait_windows, stereo!,
       xyplot, clear!, profile_to_xyplot, xtime!, logscale!, stickplot,
       QtFigure, QtPoints, QtFV, QtImage, QtEmpty, QtXYPlot, rtp3d

# --- precompile (ALL of it lives HERE, via PrecompileTools — never hidden in other files) ---
# Callbacks are thin invokelatest trampolines registered lazily on first window open
# (`_ensure_callbacks`, eventloop.jl), so the workload below never touches a @cfunction. It bakes
# the expensive pure-Julia work the first use of a menu would otherwise JIT-compile in front of
# the user — e.g. the first focal plot paid ~3.4 s of JIT vs 0.5 s of real work (2026-07-04,
# 133-event ISF; beachball geometry alone was 1.5 s). RUN what is GMT-free; ccall-bearing glue
# gets `precompile` directives only (compiled, never executed — the DLL is absent here).
#=
@setup_workload begin
	@compile_workload begin
		# Focal mechanisms: beachball geometry on two real mechanisms (one-plane Aki derivation
		# + two-plane general oblique) so every internal helper comes out compiled.
		for (s1, d1, r1, s2, d2, r2) in ((120.0, 45.0, -30.0, NaN, NaN, NaN),
		                                 (35.0, 60.0, 100.0, 190.0, 32.0, 73.0))
			comp, dilat, n1, n2 = _focal_patch_meca(s1, d1, r1, s2, d2, r2)
			_focal_sectors(s1, d1, r1, n1, n2)
		end
		precompile(_focal_filter, (Dict{String,String}, Vector{Float64}, Vector{Float64}, Vector{Float64}, Vector{Float64}))
		precompile(_focal_plot, (Ptr{Cvoid}, Dict{String,String}, Vector{Float64}, Vector{Float64},
		                         Vector{Float64}, Vector{Float64}, Vector{Float64}, Vector{Float64},
		                         Vector{Float64}, Vector{Float64}, Vector{Float64}, Vector{Float64},
		                         Vector{Float64}, Vector{Float64}, Vector{String}, Vector{Int}))
	end
end
=#

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

# make_desktop_shortcut.vbs writes the .lnk to the user's REAL desktop (whatever folder Windows
# resolves it to on this machine, via SpecialFolders) -- we don't try to guess that folder from
# Julia. Idempotent: overwrites the one iGMT.lnk, so re-running just refreshes it.
function _ensure_desktop_shortcut()
	Sys.iswindows() || return
	pkgroot = normpath(joinpath(@__DIR__, ".."))
	vbs = joinpath(pkgroot, "deps", "installer", "make_desktop_shortcut.vbs")
	isfile(vbs) || return
	cscript = joinpath(get(ENV, "SystemRoot", "C:\\Windows"), "System32", "cscript.exe")
	run(`$cscript //nologo $vbs $pkgroot`)
end

# Create the Desktop shortcut as part of COMPILING/installing the package, NOT lazily on first
# `using`: Pkg auto-precompiles right after `] dev` / `] add` / `] update`, and precompilation
# evaluates this top-level block, so the icon appears at install time. Guarded on
# jl_generating_output so it fires only while the precompile image is being generated. Non-fatal --
# a shortcut problem must never break precompilation.
if ccall(:jl_generating_output, Cint, ()) == 1
	try
		_ensure_desktop_shortcut()
	catch e
		@warn "InteractiveGMT: could not create the Desktop shortcut (non-fatal)." exception=(e,)
	end
end

end # module InteractiveGMT
