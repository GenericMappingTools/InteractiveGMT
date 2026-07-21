# libgmtvtk.jl — load the self-contained Qt6 + VTK viewer DLL and resolve its C API.
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
const _LOCAL_LIB  = joinpath(_PKGROOT, "deps", "build", "gmtvtk.dll")
const _SHARED_LIB = joinpath(first(Base.DEPOT_PATH), "gmtvtk_runtime", "deps", "build", "gmtvtk.dll")
const _LIB      = isfile(_LOCAL_LIB) ? _LOCAL_LIB : _SHARED_LIB
const _BIN_DIR  = dirname(_LIB)

# Toolchain runtime DLL dirs (this machine). Dependent DLLs (Qt6*, vtk*) resolve from PATH at
# load time. Override any of these via the matching ENV var BEFORE `using InteractiveGMT`.
#
# A GMTVTK_PACKAGE=ON build (see deps/CMakeLists.txt) drops every VTK/Qt/TBB runtime DLL plus the
# Qt platform plugins (platforms/qwindows.dll, via windeployqt) into deps/build/ next to
# gmtvtk.dll itself — that's the NSIS-installed / shared-runtime-cache layout, with no VTK/Qt
# toolchain on the destination machine at all. Detect that bundle and point straight at it;
# otherwise fall back to this dev machine's hard-coded toolchain paths (ENV overrides always
# win either way).
const _BUNDLED  = isdir(joinpath(_BIN_DIR, "platforms"))
const _VTK_BIN = get(ENV, "INTERACTIVEGMT_VTK_BIN", _BUNDLED ? _BIN_DIR : raw"C:\programs\compa_libs\VTK-9.6.2\compileds\bin")
const _QT_BIN  = get(ENV, "INTERACTIVEGMT_QT_BIN",  _BUNDLED ? _BIN_DIR : raw"C:\programs\Qt6\6.11.1\msvc2022_64\bin")
const _QT_PLAT = get(ENV, "INTERACTIVEGMT_QT_PLAT", _BUNDLED ? joinpath(_BIN_DIR, "platforms") : raw"C:\programs\Qt6\6.11.1\msvc2022_64\plugins\platforms")

const _DLL     = Ref{Ptr{Cvoid}}(C_NULL)
const _LIB_FNS = Dict{Symbol,Ptr{Cvoid}}()

# Every exported C-API symbol resolved at load time.
const _LIB_SYMBOLS = (
	:gmtvtk_view_grid, :gmtvtk_view_demo, :gmtvtk_process_events,
	:gmtvtk_add_overlay, :gmtvtk_add_overlay_h, :gmtvtk_add_symbols_h, :gmtvtk_is_alive,
	:gmtvtk_add_curtain_h, :gmtvtk_add_curtain_file_h,
	:gmtvtk_view_points, :gmtvtk_selection_count, :gmtvtk_get_selection,
	:gmtvtk_set_object_visible,
	:gmtvtk_view_fv, :gmtvtk_promote_fv_h, :gmtvtk_set_julia_eval, :gmtvtk_set_table, :gmtvtk_log_error,
	:gmtvtk_error_box, :gmtvtk_get_xfac,
	:gmtvtk_save_png, :gmtvtk_orbit, :gmtvtk_set_stereo,
	:gmtvtk_open_empty, :gmtvtk_set_drop_callback, :gmtvtk_add_surface_h,
	:gmtvtk_promote_surface_h, :gmtvtk_replace_base_grid_h, :gmtvtk_show_layer_image_h, :gmtvtk_show_layer_rgba_h,
	:gmtvtk_aqua_set_land_cpt_h, :gmtvtk_aqua_set_bathy_h, :gmtvtk_aqua_set_var_label_h,
	:gmtvtk_remove_grid_h, :gmtvtk_set_transplant_undo, :gmtvtk_unfold_scene_objects_h,
	:gmtvtk_has_surface, :gmtvtk_close, :gmtvtk_add_recent,
	:gmtvtk_set_cpt, :gmtvtk_set_cpt_grid, :gmtvtk_grid_rgb_at, :gmtvtk_raise, :gmtvtk_set_crs,
	:gmtvtk_set_title_h, :gmtvtk_set_surface_name_h,
	:gmtvtk_set_basemap_callback, :gmtvtk_set_basemap_logo, :gmtvtk_set_basemap_icon,
	:gmtvtk_set_tiles_callback, :gmtvtk_set_tiles_world, :gmtvtk_tiles_set_bg, :gmtvtk_tiles_log,
	:gmtvtk_set_bgregion_callback, :gmtvtk_set_newwindow_callback, :gmtvtk_set_save_callback,
	:gmtvtk_set_save_geotiff_callback, :gmtvtk_set_move_callback, :gmtvtk_set_img_stretch_callback,
	:gmtvtk_set_geography_callback, :gmtvtk_set_tides_callback, :gmtvtk_set_earthtide_callback,
	:gmtvtk_set_solid_callback, :gmtvtk_set_grdsample_callback, :gmtvtk_set_gridmeta_callback,
	:gmtvtk_set_dimfun_callback, :gmtvtk_set_nswing_callback,
	:gmtvtk_set_save_session_callback, :gmtvtk_set_load_session_callback, :gmtvtk_load_session_h,
	:gmtvtk_window_screenshot,
	:gmtvtk_scene_state_full, :gmtvtk_apply_scene_state, :gmtvtk_serialize_texts,
	:gmtvtk_serialize_polys, :gmtvtk_add_poly_full, :gmtvtk_serialize_faults, :gmtvtk_add_nested_rect,
	:gmtvtk_refresh_fault_planes, :gmtvtk_overlay_style_h, :gmtvtk_set_overlay_style_h,
	:gmtvtk_set_igrf_point_callback, :gmtvtk_set_igrf_grid_callback, :gmtvtk_set_igrf_file_callback,
	:gmtvtk_set_rtp3d_callback,
	:gmtvtk_set_clipgrid_callback,
	:gmtvtk_set_seismicity_callback,
	:gmtvtk_set_faultgeom_callback,
	:gmtvtk_set_elastic_callback, :gmtvtk_set_importfault_callback, :gmtvtk_add_fault_h,
	:gmtvtk_add_fault_geom_h, :gmtvtk_set_modelslip_callback, :gmtvtk_add_slip_patches_h,
	:gmtvtk_set_focal_callback, :gmtvtk_add_meca_h, :gmtvtk_set_meca_infos_h, :gmtvtk_add_text_h, :gmtvtk_add_texts_h,
	:gmtvtk_set_meca_props_callback, :gmtvtk_remove_meca_group_h,
	:gmtvtk_set_cube_layer_callback, :gmtvtk_set_cube_loadall_callback, :gmtvtk_set_cube_axes_zrange, :gmtvtk_show_cube_layer_dialog,
	:gmtvtk_cube_flat_mode, :gmtvtk_mark_cube, :gmtvtk_pick_netcdf_var,
	:gmtvtk_set_cube_slider_callback, :gmtvtk_mark_element_cube,
	:gmtvtk_scene_state,
	:gmtvtk_frame_for_image_h, :gmtvtk_fit2d, :gmtvtk_grow_frame_h, :gmtvtk_hide_surface,
	:gmtvtk_hide_other_grids, :gmtvtk_hide_other_images,
	:gmtvtk_capture_rect_rgb, :gmtvtk_free_rgb, :gmtvtk_get_crs, :gmtvtk_reframe_h,
	:gmtvtk_show_profile_xy,
	:gmtvtk_xyplot_open, :gmtvtk_xyplot_add_series, :gmtvtk_xyplot_clear,
	:gmtvtk_xyplot_is_alive, :gmtvtk_xyplot_close, :gmtvtk_xyplot_raise,
	:gmtvtk_xyplot_set_callback, :gmtvtk_xyplot_set_labels,
	:gmtvtk_xyplot_set_analysis_callback, :gmtvtk_open_profile_in_xyplot,
	:gmtvtk_xyplot_set_seed_callback, :gmtvtk_xyplot_set_xtime, :gmtvtk_xyplot_set_logscale,
	:gmtvtk_xyplot_specgrant, :gmtvtk_xyplot_set_new_callback, :gmtvtk_open_xyplot_from_host,
	:gmtvtk_xyplot_log, :gmtvtk_xyplot_run_analysis,
	:gmtvtk_xyplot_add_page, :gmtvtk_xyplot_series_count, :gmtvtk_xyplot_series_npoints,
	:gmtvtk_xyplot_get_series, :gmtvtk_xyplot_series_name, :gmtvtk_xyplot_get_xtime,
	:gmtvtk_progress_show, :gmtvtk_progress_show_async, :gmtvtk_progress_update,
	:gmtvtk_progress_status, :gmtvtk_progress_close,
)

# Resolve a loaded C-API function pointer. Errors clearly if the library never loaded.
@inline function _fn(sym::Symbol)::Ptr{Cvoid}
	p = get(_LIB_FNS, sym, C_NULL)
	p == C_NULL && error("InteractiveGMT viewer library not loaded (symbol :$sym). Build deps/build.bat and restart Julia.")
	return p
end

# Put the toolchain DLLs on PATH, dlopen the viewer, resolve every symbol. Idempotent.
function _load_library()
	_DLL[] == C_NULL || return                       # already loaded
	isfile(_LIB) || error("gmtvtk.dll not found at $_LIB — build it with deps/build.bat")
	ENV["PATH"] = _VTK_BIN * ";" * _QT_BIN * ";" * get(ENV, "PATH", "")
	ENV["QT_QPA_PLATFORM_PLUGIN_PATH"] = _QT_PLAT
	_DLL[] = Libdl.dlopen(_LIB)
	for s in _LIB_SYMBOLS
		_LIB_FNS[s] = Libdl.dlsym(_DLL[], s)
	end
	return
end
