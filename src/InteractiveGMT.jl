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
import GMT: movie
using Distributed: addprocs, workers, rmprocs, remotecall, remotecall_eval
using PrecompileTools: @setup_workload, @compile_workload

# --- C-API DLL loader (resolved at runtime in __init__; see libgmtvtk.jl) ----------------
include("errors.jl")   # THE failure sink + @tool_error: first, so every later file can report
include("libgmtvtk.jl")
include("selfupdate.jl") # update!() -- pull + rebuild in place, for a `] dev`-installed checkout

# --- handles, event loop, in-window Julia console ----------------------------------------
include("types.jl")
include("introspect.jl") # read-only scene-state snapshot for the test suite
include("crs.jl")        # centralized coordinate-reference-system store (proj4/wkt/epsg)
include("qsccube.jl")    # Cube view mode: PROJ's +proj=qsc face warp, sampled once and pushed to the viewer
include("eventloop.jl")
include("warmup.jl")     # JIT warm-up: compile a tool's code while its dialog is being filled in
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
include("importxy.jl")  # File > Open xy(z): Mirone point/arrow/scaled-symbol/text table imports
include("paste.jl")      # Ctrl+V: clipboard image / numeric table -> the SAME builders drop.jl uses
include("basemap.jl")    # World Topo Tiles picker (ported from Mirone bg_map.m)
include("tilestool.jl")  # Tools > Tiles Tool (ported from Mirone tiles_tool.m; mosaic via GMT.mosaic)
include("lidarpt.jl")    # Tools > LIDAR2011 PT (ported from Mirone cartas_militares.m 'nikles' mode)
include("gadm.jl")      # Geography > Administrative units (GADM): GMT.jl gadm boundaries
include("cartasmil.jl") # Tools > PT tools > Cartas Militares (Mirone cartas_militares.m, 1:25000 sheets)
include("dgtlidar.jl")   # Tools > DGT LIDAR (Portugal): GMT.jl dgt_lidar download + dgt_mosaic
include("dimfun.jl")     # grdsample Region box recompute (port of Mirone dim_funs.m)
include("grdsample.jl")  # GMT > Resample (grdsample)
include("bgregion.jl")   # File > Background region -> blank white 2-D map framed to W/E/S/N
include("savefile.jl")   # File > Save Grid / Save Image -> gmtwrite (netCDF/Surfer) / gdalwrite
include("session.jl")    # File > Save/Load Session (.igmtz): provenance registry + manifest + store-only zip
include("gmtscript.jl")  # reproduce a window as GMT.jl calls (live) or a standalone script
include("geography.jl")  # Geography menu -> GSHHG coastlines for the current view
include("solar.jl")      # Geography > Sun and terminators: day/night + twilights + night paint + sun position
include("plateboundaries.jl") # Geography > Plate boundaries -> 7 grouped boundary-type overlays
include("magneticisochrons.jl") # Geography > Magnetic isochrons > GPlates -> Sutton_isocs.sqlite
include("solids.jl")     # 3-D Bodies toolbar flyout -> GMT solids (cube/sphere/torus/cylinder/…) via view_fv
include("nswing.jl")     # Geophysics > NSWING tsunami (port of Mirone swan_options.m -> nswing exe)
include("aquamoto.jl")   # Geophysics > Tsunamis > Aquamoto viewer (port of Mirone aquamoto.m netCDF tab)
include("tsunamicatalog.jl") # Geophysics > Tsunamis > NOAA historical catalog (NCEI event database)
include("tsunamittt.jl") # Geophysics > Tsunamis > Tsunami travel times (GMT.jl ttt / wave_travel_time / tttimes)
include("igrf.jl")       # Geophysics > Magnetics > IGRF (port of Mirone igrf_options.m; GMT.magref)
include("seismicity.jl") # Geophysics > Seismology > Seismicity (port of Mirone earthquakes.m)
include("focal.jl")      # Geophysics > Seismology > Focal mechanisms (port of Mirone focal_meca.m)
include("deform.jl")     # Geophysics > Vertical elastic deformation: fault-trace endpoint (deform_mansinha.m)
include("nested.jl")     # "Nested grids" rectangle tool: host-side blank-grid builder (nesting_sizes.m)
include("transplant.jl") # Grid Tools > Transplant 2nd grid (port of Mirone utils/transplants.m)
include("movie.jl")      # InteractiveGMT frame scheduler + FFmpeg encoder (extends GMT.movie by dispatch)
include("movieanno.jl")  # movie frame labels (GMT movie -L) + progress indicators (-P) as scene elements
include("moviedlg.jl")   # Tools > Make movie: the dialog's block -> the ONE movie() call
include("measure.jl")    # line length/azimuth + polygon area for the vector context menu (CRS-aware)
include("info.jl")       # toolbar "i" button: grdinfo / gdalinfo report on the active grid/image
include("rtp3d.jl")      # Geophysics > Magnetics: reduce-to-pole via 2-D FFT (port of Mirone utils/rtp3d.m)
include("fftstuff.jl")   # Mag/Grav > FFT tool, Grid Tools > Spectrum, Image > FFT Spectrum (fft_stuff.m)
include("gravmag3d.jl")  # Geophysics > Magnetics > gmtgravmag3d: anomaly of a 3-D body (Okabe)
include("grdgravmag3d.jl") # Geophysics > Magnetics > grdgravmag3d: same anomaly from one or two grids
include("grdredpol.jl")  # Geophysics > Magnetics > grdredpol: continuous (differential) RTP
include("grdgradient.jl") # GMT menu > grdgradient: directional derivative / slope / aspect
include("hillshade.jl")  # View > Illumination (Hillshade): GMT illumination models (port of Mirone shading_params.m)
include("grdseamount.jl") # GMT menu > grdseamount: synthetic seamounts from a parameter table
include("manual.jl")     # the green "?" disk on every module dialog -> that module's GMTjl_doc page
include("mgd77tracks.jl") # Geophysics > Magnetics > Import *.gmt/*.nc file(s): cruise tracks (port of mirone.m GeophysicsImportGmtFile_CB)
include("clipgrid.jl")   # Grid Tools > Clip Grid: threshold/statistical grid clipping (port of Mirone ml_clip.m)
include("binarize.jl")   # Image > Binarize Image: threshold an image into a B&W mask (port of Mirone thresholdit.m)
include("imagehisto.jl") # Image > Show Histogram: histogram of the DISPLAYED image (port of Mirone image_histo.m)
include("imageenhance.jl") # Image > Image Enhance > 1 - Indexed and RGB (port of Mirone image_enhance.m)
include("imageresize.jl")  # Image > Image resize (port of Mirone imageresize.m; resampling via gdalwarp)
include("floodfill.jl")    # Image > Shape detector, the magic wand (port of Mirone floodfill.m)
include("classification.jl") # Image > K-means classification (port of Mirone classificationfig.m)
include("imageflip.jl")    # Image > Flip: up-down / left-right pixel flip, georef untouched (Mirone mirone.m)
include("rgbexplore.jl")   # Image > Explore RGB: the 13 colour components montage (Mirone mirone.m 'RGBexp')
include("empilhador.jl") # Tools > Empilhador: stack grids/L2 scenes into a 3-D file (port of Mirone empilhador.m)
include("oceancolor.jl") # Tools > Ocean Color Data Browser: OB.DAAC L3 catalogue + browse images
include("gridcalc.jl")   # Grid Tools > Grid calculator: expression over same-geometry grids (port of Mirone grid_calculator.m)
include("contours.jl")   # Grid Tools > Contours: GDAL-traced contour lines (port of Mirone contouring.m)
include("sdg.jl")        # Grid Tools > SDG: 2nd derivative along the gradient (port of Mirone GridToolsSDG_CB)
include("multiscale.jl") # Grid Tools > Terrain Modeling: moving-window terrain analysis (port of Mirone mirblock.c)
include("grdtrend.jl")   # GMT menu > grdtrend: polynomial trend surface / residuals / robust weights
include("grdlandmask.jl")# GMT menu > grdlandmask: wet/dry mask grid from the shoreline database
include("grdfilter.jl")  # GMT menu > grdfilter: space-domain filtering of the window's grid
include("grdfft.jl")     # GMT menu > grdfft: frequency-domain operations / power spectrum of the grid
include("grdhisteq.jl")  # GMT menu > grdhisteq: histogram equalization / equal-area levels
include("xyz2grd.jl")    # GMT menu > xyz2grd: an x,y,z table whose points sit on the nodes -> a grid
include("grdfill.jl")    # GMT menu > grdfill: fill the holes in the window's grid, or just find them
include("trend2d.jl")    # GMT menu > trend2d: polynomial z = f(x,y) fitted to an x,y,z table
include("cptbuild.jl")   # GMT menu > Make CPT: one dialog over makecpt and grd2cpt
include("gravfft.jl")    # GMT menu > gravfft: spectral geopotential, isostasy, admittance & coherence
include("grdrotater.jl") # GMT menu > grdrotater: reconstruct the window's grid by an Euler rotation
include("talwani2d.jl")  # GMT menu > talwani2d: anomalies over 2-D bodies given as cross-sections
include("talwani3d.jl")  # GMT menu > talwani3d: anomalies over 3-D bodies given as stacked contours
include("greenspline.jl")# GMT menu > greenspline: Green's-function splines, the whole module
include("gmtflexure.jl") # GMT menu > gmtflexure: flexure of a 2-D plate along a profile
include("grdflexure.jl") # GMT menu > grdflexure: flexure of a surface under a load (5 rheologies)
include("grdvolume.jl")  # GMT menu > grdvolume: area, volume and mean height against contour levels
include("gravprisms.jl") # GMT menu > gravprisms: the field of a body made of rectangular prisms
include("grdvector.jl")  # GMT menu > grdvector: the vector field of two grids, drawn as an overlay
include("earthregions.jl")# Tools > Earth regions: a named region's raster or boundaries (GMT.jl earthregions)
include("mbgrid.jl")     # MBGRID: Gaussian binning + zgrid/surface gap fill, via deps/src/mbgrid.c
include("interpolate.jl")# GMT menu > Interpolate: grid an x,y,z table (surface, nearneighbor, mbgrid, block*, …)
include("project.jl")    # Tools > Project: reproject the window's raster with gdalwarp (Mirone gdal_project.m)
include("lineops.jl")    # Tools > Vector Operations (port of Mirone src_figs/line_operations.m)
include("plates.jl")     # Plates > Euler rotations (port of Mirone euler_stuff.m; GMT spotter modules)
include("computeeuler.jl") # Plates > Compute Euler pole (port of Mirone compute_euler.m + mex/distmin.c)
include("shapenc.jl")    # write a SHAPENC netCDF file (port of Mirone utils/shapenc.m; GDAL MDArray API, no MEX)
include("gmtedit.jl")    # Geophysics > Magnetics > gmtedit: the MGD77 track editor (port of Mirone src_figs/gmtedit.m)
include("isocs.jl")      # parse Mirone data/isocs/*.dat isochron header -> write via shapenc
include("palettes.jl")   # Image > Color Palettes: the six palette families + CPT I/O (port of Mirone color_palettes.m)
include("bandslist.jl")  # Image > Load Bands: multiband/.vrt band picker (port of Mirone bands_list.m)

export gmtscript, gmtreplay,
       view_grid, view_image, view_points, view_fv, view_demo, iview,
       add!, add_curtain!, add_symbols!, show_table, selection, isalive,
       poly2fv, colorize_by_z!, save_png, wait_windows, stereo!, movie, MovieFrame, orbit!, replace_grid!,
       set_layer!, nlayers, add_label!, add_progress!, remove_annotation!, movie_annotations,
       xyplot, clear!, profile_to_xyplot, xtime!, logscale!, stickplot, xyinfo!, xynowcross!,
       QtFigure, QtPoints, QtFV, QtImage, QtEmpty, QtXYPlot, rtp3d, shapenc, isoc2shapenc, shapenc2isoc,
       gmtedit, mbgrid

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
	_dbg("startup", "__init__ enter")
	# GDAL reads on ALL CORES. A tiled/compressed raster (every Landsat/Sentinel scene, every VRT over
	# them) is decompressed single-threaded by default; this is the one process-wide switch that fixes
	# it for every read in the package. Measured on a 4-band 60-Mpx Landsat VRT: 5.9 s -> 3.9 s.
	# Set here because it is a process setting, not a per-call one — never re-set at a call site.
	try
		GMT.Gdal.CPLSetConfigOption("GDAL_NUM_THREADS", "ALL_CPUS")
		GMT.Gdal.CPLSetConfigOption("GDAL_CACHEMAX", "1024")
	catch e
		@debug "InteractiveGMT: could not set the GDAL threading options (harmless)" exception=(e,)
	end
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
			@tool_error "InteractiveGMT: could not install basemap toolbar assets (rebuild deps/build.bat if the export is missing)." exception=(e,)
		end
		try
			_install_tiles_assets()
		catch e
			@tool_error "InteractiveGMT: could not install Tiles Tool world image (rebuild deps/build.bat if the export is missing)." exception=(e,)
		end
		try
			_install_lidar_assets()
		catch e
			@tool_error "InteractiveGMT: could not install the LIDAR2011 PT background image (rebuild deps/build.bat if the export is missing)." exception=(e,)
		end
	catch e
		@tool_error "InteractiveGMT: the Qt+VTK viewer DLL could not be loaded; build it with deps/build.bat (Windows only). Viewer calls will error until then." exception=(e,)
	end
	_dbg("startup", "__init__ exit")
end

# The desktop icon, on ALL THREE systems, made by the ONE launcher binary (deps/src/launcher.c ->
# deps/build/igmt[.exe]): a .lnk on Windows, a .desktop on Linux, an .app bundle on macOS. It also
# copies itself to ~/.gmt and records these two paths there, so the icon survives a `] add`
# reinstall into a differently-hashed package folder.
#
# Julia hands over both paths because it is the one place that knows them for certain -- the exact
# julia that is precompiling right now, and this package's own root. The launcher validates them
# and falls back to its own search if they ever go stale, so a stale hint costs nothing.
#
# (Until 2026-09-01 this ran cscript on deps/installer/make_desktop_shortcut.vbs, which is why
# Linux and macOS had no desktop icon at all. That script, iview_app.vbs, iview_launch.vbs and
# iview_splash.hta are still on disk — no longer on this path.)
function _ensure_desktop_shortcut()
	pkgroot = normpath(joinpath(@__DIR__, ".."))
	# _BIN_DIR (libgmtvtk.jl) is THE answer to "where did the binaries actually land" -- the
	# package's own deps/build on a dev checkout, SHARED_ROOT (<depot>/gmtvtk_runtime/deps/build)
	# on a plain install, where deps/build.jl extracts the release. igmt ships beside the library
	# in both layouts, so it is found by the same resolver rather than a second guess: deriving the
	# path from @__DIR__ found nothing on every non-dev machine and this function silently did
	# nothing there -- no desktop icon, no error.
	exe = joinpath(_BIN_DIR, Sys.iswindows() ? "igmt.exe" : "igmt")
	isfile(exe) || return
	julia = joinpath(Sys.BINDIR, Sys.iswindows() ? "julia.exe" : "julia")
	run(`$exe --install-shortcut --root=$pkgroot --julia=$julia`)
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
		@tool_error "InteractiveGMT: could not create the Desktop shortcut (non-fatal)." exception=(e,)
	end
end

end # module InteractiveGMT
