# libgmtvtk.jl -- load the self-contained Qt6 + VTK viewer DLL and resolve its C API.
#
# The viewer is a shared library (deps/build/gmtvtk.dll, built by deps/build.bat) with a tiny
# C API. Loading happens in the module `__init__` (NOT at top level / precompile time): the
# dlopen handle and the dlsym function pointers are RUNTIME values and cannot be baked into a
# precompiled image. Every ccall fetches its pointer with `_fn(:gmtvtk_...)`.
#
# NOTE: a dlopen'd DLL stays loaded for the life of the Julia session. After rebuilding
# gmtvtk.dll you must start a FRESH Julia session to pick up the changes (an old session also
# keeps the .dll file locked against the linker).

const _PKGROOT = normpath(joinpath(@__DIR__, ".."))

# Libdl without adding it to [deps]: the stdlib module is re-exposed inside Base.
const Libdl = Base.Libc.Libdl

# Where's gmtvtk.dll? Checked in order:
#   1. A LOCAL dev build (deps/build/gmtvtk.dll next to this checkout, from deps/build.bat) --
#      always wins first, so a developer actively rebuilding the DLL never picks up a stale
#      cached copy.
#   2. SHARED_LIB: the depot-wide runtime cache deps/build.jl fetches into
#      (~/.julia/gmtvtk_runtime/deps/build/gmtvtk.dll) -- this is the Pkg.add/Pkg.develop path.
#      It's keyed off the Julia DEPOT itself, not off this package's own (possibly
#      content-hashed, possibly-different-every-update) folder, so the SAME ~200 MB VTK/Qt/TBB
#      runtime is reused across every future update instead of being re-fetched into a new
#      folder each time.
#
# "Wins first" is a PREFERENCE, not a commitment. A local copy that cannot actually be loaded --
# stale (missing a newer export, so dlsym throws and no symbol resolves at all), or left behind by
# an older deps/build.jl that extracted releases straight into this folder, where none of the
# VTK/Qt DLLs sit beside it -- used to end the story: Pkg.build refreshed the SHARED cache that
# the loader then never looked at, so no amount of re-publishing could fix that machine. Both
# candidates are now tried in order and the first one that fully loads wins.
const _LIB_SUBDIR = "build"
const _LIB_NAME = Sys.iswindows() ? "gmtvtk.dll" : Sys.isapple() ? "libgmtvtk.dylib" : "libgmtvtk.so"
# The "what this library needs from the bundle beside it" manifest. ONE name per platform, and it
# must agree with deps/build.jl's REQUIRES_MANIFEST -- this file used to hardcode ".dll_requires",
# so on Linux the check below silently found no manifest and reported nothing, which is precisely
# the population the check exists for.
const _REQUIRES_MANIFEST = Sys.iswindows() ? ".dll_requires" : Sys.isapple() ? ".dylib_requires" : ".so_requires"
const _LOCAL_LIB  = joinpath(_PKGROOT, "deps", _LIB_SUBDIR, _LIB_NAME)
const _SHARED_LIB = joinpath(first(Base.DEPOT_PATH), "gmtvtk_runtime", "deps", _LIB_SUBDIR, _LIB_NAME)
const _LIB_CANDIDATES = filter(isfile, unique([_LOCAL_LIB, _SHARED_LIB]))
const _LIB      = isempty(_LIB_CANDIDATES) ? _SHARED_LIB : first(_LIB_CANDIDATES)
# Which one actually loaded -- set by _load_library, since that is only known at runtime.
const _LIB_USED = Ref{String}("")
const _BIN_DIR  = dirname(_LIB)
# The ordered-teardown atexit hook is registered once per session (see the end of _load_library).
const _ATEXIT_DONE = Ref{Bool}(false)

# Toolchain runtime DLL dirs (this machine). Dependent DLLs (Qt6*, vtk*) resolve from PATH at
# load time. Override any of these via the matching ENV var BEFORE `using InteractiveGMT`.
#
# A GMTVTK_PACKAGE=ON build (see deps/CMakeLists.txt) drops every VTK/Qt/TBB runtime DLL plus the
# Qt platform plugins (platforms/qwindows.dll, via windeployqt) into deps/build/ next to
# gmtvtk.dll itself -- that's the NSIS-installed / shared-runtime-cache layout, with no VTK/Qt
# toolchain on the destination machine at all. Detect that bundle and point straight at it;
# otherwise fall back to this dev machine's hard-coded toolchain paths (ENV overrides always
# win either way).
const _BUNDLED  = isdir(joinpath(_BIN_DIR, Sys.iswindows() ? "platforms" : joinpath("plugins", "platforms")))
const _VTK_BIN = get(ENV, "INTERACTIVEGMT_VTK_BIN", _BUNDLED ? _BIN_DIR : raw"C:\programs\compa_libs\VTK-9.6.2\compileds\bin")
const _QT_BIN  = get(ENV, "INTERACTIVEGMT_QT_BIN",  _BUNDLED ? _BIN_DIR : raw"C:\programs\Qt6\6.11.1\msvc2022_64\bin")
const _QT_PLAT = get(ENV, "INTERACTIVEGMT_QT_PLAT", _BUNDLED ? joinpath(_BIN_DIR, "platforms") : raw"C:\programs\Qt6\6.11.1\msvc2022_64\plugins\platforms")

const _DLL     = Ref{Ptr{Cvoid}}(C_NULL)
const _LIB_FNS = Dict{Symbol,Ptr{Cvoid}}()

# ABI generation this source requires from the library (see gmtvtk_abi_version, 90_c_api.cpp).
# BUMP BOTH TOGETHER whenever a host-facing export's signature changes. Generation 2 = every grid
# buffer is handed over with its layout code; a generation-1 library reads such a buffer transposed
# and shows vertical stripes, with no error anywhere — hence the check.
# Generation 4 = gmtvtk_serialize_texts takes an `includeGroups` flag and emits the group tag as its
# own field; a generation-3 library reads the flag as garbage and its lines are one field short.
# Generation 5 = the vector snapshots Save Session grew onto: overlays carry their group tag, symbols
# their oneShot/hasScale/hasRGB flags + per-point size/colour, and rulers have a serializer of their
# own. Field counts changed, so a generation-4 library's lines parse as garbage here.
# Generation 6 = gmtvtk_add_poly_full takes a trailing `groupName`, so a tool that paints several
# polygons in one action (Geography > Sun and terminators) can fold them under one Scene Objects row.
# A generation-5 library is handed one argument too many and reads the fill values shifted.
# Generation 7 = gmtvtk_set_shade_intensity_h takes a trailing `side`, because an Aquamoto tsunami
# layer has TWO surfaces (water on the live stage, land on the static bathymetry) and one reflectance
# cannot describe both. A generation-6 library reads that argument as garbage.
# Generation 8 = gmtvtk_set_cube_warp exists and gmtvtk_set_view_mode_h accepts mode 3 (the QSC cube
# body). A generation-7 library has no cube: the warp push finds no symbol and mode 3 falls through
# its view-mode switch, leaving the window in whatever mode it was already in.
# Generation 9 = gmtvtk_set_cube_axes_zrange takes the cube element's NAME as its second argument: the
# Z pin belongs to the axes set that cube's layers own, not to the window (it used to be honoured for
# the base surface only, so an extra-mounted cube's box jumped on every layer). A generation-8 library
# reads the name pointer as zmin and pins garbage.
const _ABI_REQUIRED = 9
# What the library that ACTUALLY loaded reports (1 = the export is absent, i.e. it predates the grid
# layout code). Read by `_grid_zbuf` (drop.jl): a library that cannot be told a buffer's layout is
# never handed a row-major one.
const _LIB_ABI = Ref{Int}(0)
_lib_abi() = _LIB_ABI[]

# Every exported C-API symbol resolved at load time.
const _LIB_SYMBOLS = (
	:gmtvtk_view_grid, :gmtvtk_view_demo, :gmtvtk_process_events,
	:gmtvtk_add_overlay, :gmtvtk_add_overlay_h, :gmtvtk_add_overlay_ex_h, :gmtvtk_add_overlay_ex2_h, :gmtvtk_add_overlay_ex3_h, :gmtvtk_add_overlay_ex4_h, :gmtvtk_add_overlay_bounded_h, :gmtvtk_get_display_bounds_h,
	:gmtvtk_overlay_points_h, :gmtvtk_remove_overlay_group_h, :gmtvtk_remove_symbols_h,
	:gmtvtk_remove_polys_h, :gmtvtk_label_width_world_h, :gmtvtk_set_group_master_h,
	:gmtvtk_add_overlay_gapped_h, :gmtvtk_world_per_pixel_h, :gmtvtk_dblclick_test,
	:gmtvtk_add_symbols_h, :gmtvtk_add_symbols_ex_h, :gmtvtk_symbol_set_table_h, :gmtvtk_is_alive,
	:gmtvtk_add_curtain_h, :gmtvtk_add_curtain_file_h,
	:gmtvtk_view_points, :gmtvtk_promote_points_h, :gmtvtk_selection_count, :gmtvtk_get_selection,
	:gmtvtk_set_object_visible,
	:gmtvtk_view_fv, :gmtvtk_promote_fv_h, :gmtvtk_set_julia_eval, :gmtvtk_set_table, :gmtvtk_log_error,
	:gmtvtk_error_box, :gmtvtk_get_xfac,
	:gmtvtk_take_messages, :gmtvtk_shutdown,   # Qt's own warnings -> the failure sink; ordered teardown
	:gmtvtk_save_png, :gmtvtk_orbit, :gmtvtk_set_stereo,
	:gmtvtk_open_empty, :gmtvtk_set_drop_callback, :gmtvtk_set_paste_callback, :gmtvtk_add_surface_h,
	:gmtvtk_promote_surface_h, :gmtvtk_replace_base_grid_h, :gmtvtk_show_layer_image_h, :gmtvtk_show_layer_rgba_h,
	:gmtvtk_aqua_set_land_cpt_h, :gmtvtk_aqua_set_bathy_h, :gmtvtk_aqua_set_var_label_h,
	:gmtvtk_aqua_queue_open,
	:gmtvtk_remove_grid_h, :gmtvtk_remove_image_h, :gmtvtk_set_transplant_undo, :gmtvtk_unfold_scene_objects_h,
	:gmtvtk_open_vtk_h, :gmtvtk_add_mesh_h, :gmtvtk_show_new_element_h, :gmtvtk_reframe_z_h,
	:gmtvtk_grow_z_frame_h,
	:gmtvtk_reframe_named_h,
	:gmtvtk_swipe_select_mode_h,
	:gmtvtk_has_surface, :gmtvtk_has_element_h, :gmtvtk_close, :gmtvtk_add_recent,
	:gmtvtk_set_cpt, :gmtvtk_set_cpt_grid, :gmtvtk_grid_rgb_at, :gmtvtk_raise, :gmtvtk_set_crs,
	:gmtvtk_set_palette_callback, :gmtvtk_set_bands_callback,
	:gmtvtk_set_title_h, :gmtvtk_set_surface_name_h,
	:gmtvtk_set_basemap_callback, :gmtvtk_set_basemap_logo, :gmtvtk_set_basemap_icon,
	:gmtvtk_set_tiles_callback, :gmtvtk_set_tiles_world, :gmtvtk_tiles_set_bg, :gmtvtk_tiles_log,
	:gmtvtk_tiles_set_footprints,
	:gmtvtk_set_lidar_callback, :gmtvtk_set_lidar_image, :gmtvtk_lidar_set_tiles, :gmtvtk_lidar_status,
	:gmtvtk_set_oceancolor_callback, :gmtvtk_oc_set_tile, :gmtvtk_oc_status, :gmtvtk_oc_cache_info,
	:gmtvtk_oc_ask_login, :gmtvtk_oc_message, :gmtvtk_oc_set_products,
	:gmtvtk_oc_attach_grid_button_h,
	:gmtvtk_oc_progress_begin, :gmtvtk_oc_progress_set, :gmtvtk_oc_progress_end, :gmtvtk_oc_queue_place,
	:gmtvtk_set_bgregion_callback, :gmtvtk_set_newwindow_callback, :gmtvtk_set_save_callback,
	:gmtvtk_set_save_geotiff_callback, :gmtvtk_set_move_callback, :gmtvtk_set_img_stretch_callback,
	:gmtvtk_set_geography_callback, :gmtvtk_set_tides_callback, :gmtvtk_set_tidemodel_callback, :gmtvtk_set_earthtide_callback,
	:gmtvtk_set_solar_callback, :gmtvtk_solar_report,
	:gmtvtk_set_solid_callback, :gmtvtk_set_grdsample_callback, :gmtvtk_set_gridmeta_callback,
	:gmtvtk_set_dimfun_callback, :gmtvtk_set_nswing_callback,
	:gmtvtk_set_save_session_callback, :gmtvtk_set_load_session_callback, :gmtvtk_load_session_h,
	:gmtvtk_serialize_overlays, :gmtvtk_serialize_symbols, :gmtvtk_layer_display,
	:gmtvtk_window_screenshot,
	:gmtvtk_scene_state_full, :gmtvtk_apply_scene_state, :gmtvtk_serialize_texts,
	# View mode (0 = 3-D, 1 = flat 2-D map, 2 = globe / geographic orthographic). The globe is refused
	# for non-geographic data, which is why the setter answers with the mode it actually landed in.
	:gmtvtk_set_view_mode_h, :gmtvtk_get_view_mode_h, :gmtvtk_set_cube_warp,
	# Clamp a vector element (or a whole tagged group) onto the surface below it. The importer of an
	# x,y dataset calls this instead of draping the vertices itself — one clamp, and the source z survives.
	:gmtvtk_line_clamp_h,
	:gmtvtk_serialize_polys, :gmtvtk_add_poly_full, :gmtvtk_serialize_faults, :gmtvtk_add_nested_rect,
	:gmtvtk_serialize_rulers, :gmtvtk_add_ruler_h, :gmtvtk_set_vector_visible_h,
	:gmtvtk_serialize_vector_h, :gmtvtk_vector_info_h, :gmtvtk_set_euler_callback, :gmtvtk_euler_result,
	:gmtvtk_set_lineops_callback, :gmtvtk_lineops_result, :gmtvtk_vector_names_h, :gmtvtk_remove_vector_h,
	:gmtvtk_compute_euler_progress,
	:gmtvtk_refresh_fault_planes, :gmtvtk_overlay_style_h, :gmtvtk_set_overlay_style_h,
	:gmtvtk_set_igrf_point_callback, :gmtvtk_set_igrf_grid_callback, :gmtvtk_set_igrf_file_callback,
	:gmtvtk_set_rtp3d_callback,
	:gmtvtk_set_fftstuff_callback,
	:gmtvtk_set_gravmag3d_callback, :gmtvtk_set_grdgravmag3d_callback, :gmtvtk_set_grdredpol_callback, :gmtvtk_set_manual_callback, :gmtvtk_set_grdgradient_callback, :gmtvtk_set_grdseamount_callback,
	:gmtvtk_set_hillshade_callback, :gmtvtk_set_shade_intensity_h, :gmtvtk_set_warmup_callback,
	:gmtvtk_set_import_gmt_callback,
	:gmtvtk_set_binarize_callback, :gmtvtk_binarize_set_histogram, :gmtvtk_binarize_set_preview,
	:gmtvtk_set_image_histo_callback, :gmtvtk_histo_set_counts,
	:gmtvtk_set_image_enhance_callback, :gmtvtk_enhance_set_band, :gmtvtk_enhance_set_window,
	:gmtvtk_set_rgb_scatter_callback, :gmtvtk_set_forget_callback,
	:gmtvtk_set_image_resize_callback, :gmtvtk_resize_set_size, :gmtvtk_set_floodfill_callback,
	:gmtvtk_set_classify_callback, :gmtvtk_classify_set_classes,
	:gmtvtk_image_set_palette_h, :gmtvtk_image_set_has_orig_h,
	:gmtvtk_set_image_flip_callback, :gmtvtk_image_set_pixels_h,
	:gmtvtk_set_rgbexplore_callback, :gmtvtk_rgbexp_set_thumbs, :gmtvtk_image_set_rgb_h,
	:gmtvtk_set_clipgrid_callback, :gmtvtk_set_empilhador_callback, :gmtvtk_set_gridcalc_callback, :gmtvtk_set_grdtrend_callback, :gmtvtk_set_grdlandmask_callback, :gmtvtk_set_grdfilter_callback, :gmtvtk_set_grdfft_callback, :gmtvtk_set_grdhisteq_callback, :gmtvtk_set_xyz2grd_callback, :gmtvtk_set_grdfill_callback, :gmtvtk_set_trend2d_callback, :gmtvtk_set_cptbuild_callback,
	:gmtvtk_set_gravfft_callback, :gmtvtk_set_grdrotater_callback, :gmtvtk_set_talwani2d_callback,
	:gmtvtk_set_talwani3d_callback, :gmtvtk_set_greenspline_callback,
	:gmtvtk_set_gmtflexure_callback, :gmtvtk_set_grdflexure_callback,
	:gmtvtk_set_grdvolume_callback, :gmtvtk_set_gravprisms_callback,
	:gmtvtk_set_grdvector_callback, :gmtvtk_set_earthregions_callback,
	:gmtvtk_earthregions_set_listing, :gmtvtk_earthregions_set_region,
	:gmtvtk_set_dgt_callback, :gmtvtk_dgt_log, :gmtvtk_lidar_set_bg,
	:gmtvtk_set_gadm_callback, :gmtvtk_gadm_set_listing, :gmtvtk_gadm_set_countries, :gmtvtk_set_interpolate_callback, :gmtvtk_set_project_callback, :gmtvtk_set_ui_dir,
	:gmtvtk_overlay_set_table_h,
	:gmtvtk_vector_points_count_h, :gmtvtk_vector_points_get_h, :gmtvtk_vector_points_set_h,
	:gmtvtk_vector_unmapped_h,
	:gmtvtk_set_seismicity_callback,
	:gmtvtk_set_faultgeom_callback,
	:gmtvtk_set_elastic_callback, :gmtvtk_set_importfault_callback, :gmtvtk_add_fault_h,
	:gmtvtk_add_fault_geom_h, :gmtvtk_set_modelslip_callback, :gmtvtk_add_slip_patches_h,
	:gmtvtk_set_focal_callback, :gmtvtk_add_meca_h, :gmtvtk_set_meca_infos_h, :gmtvtk_add_text_h, :gmtvtk_add_texts_h, :gmtvtk_add_texts_ex_h,
	:gmtvtk_set_meca_props_callback, :gmtvtk_remove_meca_group_h,
	:gmtvtk_set_cube_layer_callback, :gmtvtk_set_cube_loadall_callback, :gmtvtk_set_cube_axes_zrange, :gmtvtk_show_cube_layer_dialog,
	:gmtvtk_cube_flat_mode, :gmtvtk_mark_cube, :gmtvtk_pick_netcdf_var,
	:gmtvtk_set_cube_slider_callback, :gmtvtk_mark_element_cube,
	:gmtvtk_scene_state,
	:gmtvtk_frame_for_image_h, :gmtvtk_fit2d, :gmtvtk_hide_surface,
	:gmtvtk_hide_other_grids, :gmtvtk_hide_other_images,
	:gmtvtk_capture_rect_rgb, :gmtvtk_capture_rect_databaked, :gmtvtk_free_rgb, :gmtvtk_get_crs, :gmtvtk_reframe_h,
	:gmtvtk_show_profile_xy,
	:gmtvtk_xyplot_open, :gmtvtk_xyplot_add_series, :gmtvtk_xyplot_clear,
	:gmtvtk_xyplot_is_alive, :gmtvtk_xyplot_close, :gmtvtk_xyplot_raise, :gmtvtk_xyplot_set_owner,
	:gmtvtk_xyplot_set_callback, :gmtvtk_xyplot_set_labels, :gmtvtk_xyplot_set_info,
	:gmtvtk_xyplot_add_now_cross, :gmtvtk_xyplot_add_bars,
	:gmtvtk_xyplot_set_analysis_callback, :gmtvtk_open_profile_in_xyplot,
	:gmtvtk_xyplot_set_seed_callback, :gmtvtk_xyplot_set_xtime, :gmtvtk_xyplot_set_logscale,
	:gmtvtk_xyplot_specgrant, :gmtvtk_xyplot_set_new_callback, :gmtvtk_open_xyplot_from_host,
	:gmtvtk_xyplot_log, :gmtvtk_xyplot_run_analysis,
	:gmtvtk_xyplot_add_page, :gmtvtk_xyplot_series_count, :gmtvtk_xyplot_series_npoints,
	:gmtvtk_xyplot_get_series, :gmtvtk_xyplot_series_name, :gmtvtk_xyplot_get_xtime,
	:gmtvtk_gmtedit_open, :gmtvtk_gmtedit_is_alive, :gmtvtk_gmtedit_close, :gmtvtk_gmtedit_raise,
	:gmtvtk_set_gmtedit_callback, :gmtvtk_gmtedit_set_title, :gmtvtk_gmtedit_set_varlist,
	:gmtvtk_gmtedit_set_parent,
	:gmtvtk_gmtedit_set_x, :gmtvtk_gmtedit_set_channel, :gmtvtk_gmtedit_set_flags,
	:gmtvtk_gmtedit_get_flags, :gmtvtk_gmtedit_get_channel, :gmtvtk_gmtedit_npoints,
	:gmtvtk_gmtedit_set_point, :gmtvtk_gmtedit_add_overlay, :gmtvtk_gmtedit_set_mark,
	:gmtvtk_gmtedit_set_autop, :gmtvtk_gmtedit_set_navfound, :gmtvtk_gmtedit_log,
	:gmtvtk_gmtedit_message,
	:gmtvtk_progress_show, :gmtvtk_progress_show_async, :gmtvtk_progress_update,
	:gmtvtk_progress_status, :gmtvtk_progress_close,
	# MBGRID (deps/src/mbgrid.c, a second C translation unit inside the SAME DLL — see
	# GMTVTK_SRC in deps/CMakeLists.txt). Resolved here with everything else: there is ONE
	# symbol resolver for this library, and a build too old to carry these is stale for the
	# same reason it would be stale missing any other export.
	:mbgrid_dims, :mbgrid_work_dims, :mbgrid_work_origin, :mbgrid_bin, :mbgrid_zgrid,
	:mbgrid_nodes, :mbgrid_fill, :mbgrid_extract, :mbgrid_run, :mbgrid_strerror,
)

# OPTIONAL exports: resolved when the library has them, SKIPPED without complaint when it does not.
#
# `_LIB_SYMBOLS` above is a hard contract — a library missing any of it is refused as stale, which is
# right for the exports the app cannot run without. It is the WRONG rule for an export a feature
# branch has just added: the DLL is a build artefact shared by every branch in the working tree, so
# putting a brand-new symbol in the hard list means checking that branch out refuses to load the DLL
# built from any other one, and every branch switch costs a full rebuild. That is the trade this
# tier exists to avoid: the branch's own feature is unavailable until its DLL is built, and NOTHING
# ELSE is affected.
#
# A symbol graduates from here into `_LIB_SYMBOLS` once its C side is on master — at that point every
# build really does have it, and a library without it really is stale.
const _LIB_OPTIONAL = (
	:gmtvtk_save_png_h,        # movie tool: render ONE window to PNG (gmtvtk_save_png is the app-wide one)
	:gmtvtk_render_size_h,     # movie tool: force the render size, so every frame comes out identical
	:gmtvtk_anno_add_h,        # movie tool: create a frame label (-L) / progress indicator (-P)
	:gmtvtk_anno_set_h,        # movie tool: push one frame's text + progress fraction
	:gmtvtk_anno_remove_h,     # movie tool: drop one annotation and its actors
	:gmtvtk_anno_count_h,      # movie tool: how many the window carries (-1 = window gone)
	:gmtvtk_set_movie_callback,# movie tool: Tools > Make movie -> _on_movie
	:gmtvtk_open_movie_dialog_h,# movie tool: open that dialog on one window
)

# Why the library failed to load, kept so the FIRST viewer call can repeat it. __init__ is
# deliberately tolerant (a missing DLL must not break `using`), so its @warn scrolls away long
# before the user calls a viewer function -- and the error they then hit named a missing SYMBOL,
# which is a lie whenever the real cause was a dependency Windows could not resolve.
const _LOAD_ERROR = Ref{String}("")

# Which of gmtvtk.dll's dependencies are absent (B, see deps/build.jl). `.dll_requires` is written
# beside gmtvtk.dll at package time (deps/CMakeLists.txt) and lists every non-system DLL the
# viewer imports, transitively. A dependency missing HERE is the one failure mode dlopen reports
# with the useless "The specified module could not be found" -- naming no module. Returns the
# missing names, or empty when there is no manifest to check against (a dev build).
function _missing_runtime_modules(bin::String = _BIN_DIR)::Vector{String}
	man = joinpath(bin, _REQUIRES_MANIFEST)
	isfile(man) || return String[]
	miss = String[]
	for ln in eachline(man)
		n = strip(ln)
		(isempty(n) || startswith(n, '#')) && continue
		isfile(joinpath(bin, n)) || push!(miss, n)
	end
	return miss
end

# Resolve a loaded C-API function pointer. Errors clearly if the library never loaded.
@inline function _fn(sym::Symbol)::Ptr{Cvoid}
	p = get(_LIB_FNS, sym, C_NULL)
	if p == C_NULL
		# An OPTIONAL export that this library does not carry is not a load failure — the library
		# loaded fine, it is simply older than the feature asking for it. Say that, or the message
		# sends the reader after a viewer that is plainly working.
		if sym in _LIB_OPTIONAL && _DLL[] != C_NULL
			error("InteractiveGMT: this feature needs the viewer export :$sym, which " *
			      "$(_LIB_USED[]) does not have — it was built before the feature existed. " *
			      "Rebuild with deps/build.bat and restart Julia. Everything else works meanwhile.")
		end
		why = _LOAD_ERROR[]
		error("InteractiveGMT viewer library not loaded (symbol :$sym)." *
		      (isempty(why) ? " Build deps/build.bat and restart Julia." : "\n" * why))
	end
	return p
end

# Turn the two Linux dlopen failures that are NOT our bug into instructions. Both report a library
# by name and nothing else, which sends people looking inside the bundle where the answer never is:
#
#  * "version `CXXABI_1.3.15' not found (required by .../libvtkCommonCore-9.6.so.1)" -- Julia's own
#    private libstdc++ (lib/julia) is older than the viewer's Qt/VTK need, and it is already loaded
#    under the SONAME libstdc++.so.6 before we get here, so the newer copy beside libgmtvtk.so can
#    never win. deps/build.jl installs the newer one into Julia's lib dir; that has either not run
#    yet or was undone by a Julia upgrade (juliaup installs a fresh, unpatched lib/julia).
#  * "libEGL.so.1: cannot open shared object file" -- a GLVND library, deliberately NOT bundled
#    (it is the entry point to the host GPU driver, see deps/cmake/linux_bundle.cmake.in). The
#    .host_requires manifest shipped with the bundle lists them and the package to install.
function _linux_load_hint(bin::String, msg::String)::String
	if occursin(r"CXXABI_|GLIBCXX_", msg)
		return "\n  Julia's private libstdc++ ($(joinpath(Sys.BINDIR, Base.PRIVATE_LIBDIR))) is older" *
		       " than the viewer needs.\n  Fix with:  using Pkg; Pkg.build(\"InteractiveGMT\")   then restart Julia."
	end
	man = joinpath(bin, ".host_requires")
	isfile(man) || return ""
	want = [strip(l) for l in eachline(man) if !isempty(strip(l)) && !startswith(strip(l), '#')]
	miss = filter(n -> occursin(n, msg), want)
	isempty(miss) && return ""
	return "\n  Missing from the HOST system (not shipped on purpose -- they load your GPU driver): " *
	       join(miss, ", ") * "\n  Fix with:  sudo apt install libgl1 libglx0 libegl1 libopengl0"
end

# One candidate: put ITS OWN toolchain dirs on PATH, dlopen it, resolve EVERY symbol. Returns the
# failure reason, or nothing on success. A partial success is treated as failure and unloaded: a
# stale dll that opens fine but lacks one newer export would otherwise leave _LIB_FNS half-filled,
# and the first viewer call blames a random symbol for a problem that is really "wrong file".
function _try_load(lib::String)::Union{Nothing,String}
	bin  = dirname(lib)
	if !Sys.iswindows()
		ENV["QT_PLUGIN_PATH"] = joinpath(bin, "plugins")
		ENV["QT_QPA_PLATFORM_PLUGIN_PATH"] = joinpath(bin, "plugins", "platforms")
	end
	bund = isdir(joinpath(bin, "platforms"))
	vtk  = get(ENV, "INTERACTIVEGMT_VTK_BIN", bund ? bin : raw"C:\programs\compa_libs\VTK-9.6.2\compileds\bin")
	qt   = get(ENV, "INTERACTIVEGMT_QT_BIN",  bund ? bin : raw"C:\programs\Qt6\6.11.1\msvc2022_64\bin")
	plat = get(ENV, "INTERACTIVEGMT_QT_PLAT", bund ? joinpath(bin, "platforms") : raw"C:\programs\Qt6\6.11.1\msvc2022_64\plugins\platforms")
	if Sys.iswindows()
		ENV["PATH"] = vtk * ";" * qt * ";" * get(ENV, "PATH", "")
		ENV["QT_QPA_PLATFORM_PLUGIN_PATH"] = plat
	end
	h = C_NULL
	try
		h = Libdl.dlopen(lib)
	catch e
		# dlopen names only gmtvtk.dll, never the dependency Windows could not find, so say it.
		miss = _missing_runtime_modules(bin)
		isempty(miss) || return "$lib: missing VTK/Qt modules beside it -- $(join(miss, ", "))"
		msg = sprint(showerror, e)
		return "$lib: could not be loaded ($msg)" *
		       (Sys.islinux() ? _linux_load_hint(bin, msg) : "")   # both hints are Linux-only conditions
	end
	for s in _LIB_OPTIONAL          # present -> usable; absent -> only that feature is, see _LIB_OPTIONAL
		p = Libdl.dlsym(h, s; throw_error=false)
		p === nothing || (_LIB_FNS[s] = p)
	end
	for s in _LIB_SYMBOLS
		p = Libdl.dlsym(h, s; throw_error=false)
		if p === nothing
			Libdl.dlclose(h)
			empty!(_LIB_FNS)
			return "$lib: stale build -- export :$s is missing"
		end
		_LIB_FNS[s] = p
	end
	# ABI GENERATION. A missing export is caught above; a CHANGED SIGNATURE is not — the call still
	# links and still returns, it just reads the data wrong. That is how a library built before the
	# grid layout code (`zlayout`) existed painted vertical stripes: it took a row-major "TRB" buffer
	# and read it column-major, silently. So the generation is checked here, and a library older than
	# this source expects is REFUSED like any other stale build (the loader then falls through to the
	# next candidate, which is what makes a dev build win over an outdated shared runtime).
	abi = let p = Libdl.dlsym(h, :gmtvtk_abi_version; throw_error=false)
		p === nothing ? 1 : ccall(p, Cint, ())
	end
	_LIB_ABI[] = Int(abi)
	if abi < _ABI_REQUIRED
		Libdl.dlclose(h); empty!(_LIB_FNS)
		return "$lib: stale build -- ABI generation $abi, this version needs $(_ABI_REQUIRED) " *
		       "(grid buffers carry a layout code; an older library reads them transposed)"
	end
	_DLL[] = h
	_LIB_USED[] = lib
	return nothing
end

# Load the viewer: try each candidate in preference order, first one that FULLY loads wins.
# Idempotent.
function _load_library()
	_DLL[] == C_NULL || return                       # already loaded
	isempty(_LIB_CANDIDATES) && error("$_LIB_NAME not found (looked in $(dirname(_LOCAL_LIB)) and $(dirname(_SHARED_LIB))) -- build it with " *
	                                  (Sys.iswindows() ? "deps/build.bat" : Sys.isapple() ? "cmake -S deps -B deps/build" : "deps/build.sh"))
	why = String[]
	for lib in _LIB_CANDIDATES
		r = _try_load(lib)
		r === nothing && break
		push!(why, r)
	end
	if _DLL[] == C_NULL
		_LOAD_ERROR[] = join(why, "\n") *
			"\nFix with:  using Pkg; Pkg.build(\"InteractiveGMT\")   then restart Julia."
		@error "InteractiveGMT: could not load the viewer library.\n" * _LOAD_ERROR[]
		error(_LOAD_ERROR[])
	end
	isempty(why) || @warn "InteractiveGMT: fell back to $(_LIB_USED[])" skipped=join(why, "; ")
	# Tell the viewer where OUR .ui files are. It cannot work this out on its own: it derives the
	# path from where the DLL itself sits, and for a non-dev install that is the depot runtime cache
	# (~/.julia/gmtvtk_runtime/deps/build), whose sibling deps/ui may not exist or may hold an older
	# set than this package -- every dialog whose .ui is missing there then silently fails to open.
	# The .ui ship with the package (they are in git), so _PKGROOT/deps/ui is always the right answer.
	let uidir = joinpath(_PKGROOT, "deps", "ui")
		isdir(uidir) && ccall(_fn(:gmtvtk_set_ui_dir), Cvoid, (Cstring,), uidir)
	end
	# Tear Qt down IN ORDER when this process ends. Without it the QApplication (deliberately never
	# deleted while running) was still registered as owning the main thread when the C runtime ran
	# Qt's static destructors, and every session ended with three
	#     QThreadStorage: entry N destroyed before end of thread 0x...
	# lines — Qt reporting a real teardown-order defect, on stderr, where nothing looked. Registered
	# once, at load, and it no-ops if no window was ever opened.
	if !_ATEXIT_DONE[]
		_ATEXIT_DONE[] = true
		atexit() do
			try
				ccall(_fn(:gmtvtk_shutdown), Cvoid, ())
			catch
			end
		end
	end
	return
end
