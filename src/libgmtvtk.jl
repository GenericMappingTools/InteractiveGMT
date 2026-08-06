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
const _LIB_NAME = Sys.iswindows() ? "gmtvtk.dll" : "libgmtvtk.so"
const _LOCAL_LIB  = joinpath(_PKGROOT, "deps", _LIB_SUBDIR, _LIB_NAME)
const _SHARED_LIB = joinpath(first(Base.DEPOT_PATH), "gmtvtk_runtime", "deps", _LIB_SUBDIR, _LIB_NAME)
const _LIB_CANDIDATES = filter(isfile, unique([_LOCAL_LIB, _SHARED_LIB]))
const _LIB      = isempty(_LIB_CANDIDATES) ? _SHARED_LIB : first(_LIB_CANDIDATES)
# Which one actually loaded -- set by _load_library, since that is only known at runtime.
const _LIB_USED = Ref{String}("")
const _BIN_DIR  = dirname(_LIB)

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

# Every exported C-API symbol resolved at load time.
const _LIB_SYMBOLS = (
	:gmtvtk_view_grid, :gmtvtk_view_demo, :gmtvtk_process_events,
	:gmtvtk_add_overlay, :gmtvtk_add_overlay_h, :gmtvtk_add_overlay_ex_h, :gmtvtk_add_overlay_ex2_h, :gmtvtk_add_overlay_ex3_h, :gmtvtk_add_overlay_ex4_h, :gmtvtk_add_overlay_bounded_h, :gmtvtk_get_display_bounds_h,
	:gmtvtk_overlay_points_h, :gmtvtk_remove_overlay_group_h, :gmtvtk_label_width_world_h,
	:gmtvtk_add_overlay_gapped_h, :gmtvtk_world_per_pixel_h, :gmtvtk_dblclick_test,
	:gmtvtk_add_symbols_h, :gmtvtk_add_symbols_ex_h, :gmtvtk_is_alive,
	:gmtvtk_add_curtain_h, :gmtvtk_add_curtain_file_h,
	:gmtvtk_view_points, :gmtvtk_promote_points_h, :gmtvtk_selection_count, :gmtvtk_get_selection,
	:gmtvtk_set_object_visible,
	:gmtvtk_view_fv, :gmtvtk_promote_fv_h, :gmtvtk_set_julia_eval, :gmtvtk_set_table, :gmtvtk_log_error,
	:gmtvtk_error_box, :gmtvtk_get_xfac,
	:gmtvtk_save_png, :gmtvtk_orbit, :gmtvtk_set_stereo,
	:gmtvtk_open_empty, :gmtvtk_set_drop_callback, :gmtvtk_set_paste_callback, :gmtvtk_add_surface_h,
	:gmtvtk_promote_surface_h, :gmtvtk_replace_base_grid_h, :gmtvtk_show_layer_image_h, :gmtvtk_show_layer_rgba_h,
	:gmtvtk_aqua_set_land_cpt_h, :gmtvtk_aqua_set_bathy_h, :gmtvtk_aqua_set_var_label_h,
	:gmtvtk_remove_grid_h, :gmtvtk_remove_image_h, :gmtvtk_set_transplant_undo, :gmtvtk_unfold_scene_objects_h,
	:gmtvtk_open_vtk_h, :gmtvtk_add_mesh_h, :gmtvtk_show_new_element_h, :gmtvtk_reframe_z_h,
	:gmtvtk_reframe_named_h,
	:gmtvtk_swipe_select_mode_h,
	:gmtvtk_has_surface, :gmtvtk_close, :gmtvtk_add_recent,
	:gmtvtk_set_cpt, :gmtvtk_set_cpt_grid, :gmtvtk_grid_rgb_at, :gmtvtk_raise, :gmtvtk_set_crs,
	:gmtvtk_set_title_h, :gmtvtk_set_surface_name_h,
	:gmtvtk_set_basemap_callback, :gmtvtk_set_basemap_logo, :gmtvtk_set_basemap_icon,
	:gmtvtk_set_tiles_callback, :gmtvtk_set_tiles_world, :gmtvtk_tiles_set_bg, :gmtvtk_tiles_log,
	:gmtvtk_set_lidar_callback, :gmtvtk_set_lidar_image, :gmtvtk_lidar_set_tiles, :gmtvtk_lidar_status,
	:gmtvtk_set_oceancolor_callback, :gmtvtk_oc_set_tile, :gmtvtk_oc_status, :gmtvtk_oc_cache_info,
	:gmtvtk_oc_ask_login, :gmtvtk_oc_message, :gmtvtk_oc_set_products,
	:gmtvtk_oc_attach_grid_button_h,
	:gmtvtk_oc_progress_begin, :gmtvtk_oc_progress_set, :gmtvtk_oc_progress_end, :gmtvtk_oc_queue_place,
	:gmtvtk_set_bgregion_callback, :gmtvtk_set_newwindow_callback, :gmtvtk_set_save_callback,
	:gmtvtk_set_save_geotiff_callback, :gmtvtk_set_move_callback, :gmtvtk_set_img_stretch_callback,
	:gmtvtk_set_geography_callback, :gmtvtk_set_tides_callback, :gmtvtk_set_tidemodel_callback, :gmtvtk_set_earthtide_callback,
	:gmtvtk_set_solid_callback, :gmtvtk_set_grdsample_callback, :gmtvtk_set_gridmeta_callback,
	:gmtvtk_set_dimfun_callback, :gmtvtk_set_nswing_callback,
	:gmtvtk_set_save_session_callback, :gmtvtk_set_load_session_callback, :gmtvtk_load_session_h,
	:gmtvtk_window_screenshot,
	:gmtvtk_scene_state_full, :gmtvtk_apply_scene_state, :gmtvtk_serialize_texts,
	:gmtvtk_serialize_polys, :gmtvtk_add_poly_full, :gmtvtk_serialize_faults, :gmtvtk_add_nested_rect,
	:gmtvtk_serialize_vector_h, :gmtvtk_vector_info_h, :gmtvtk_set_euler_callback, :gmtvtk_euler_result,
	:gmtvtk_compute_euler_progress,
	:gmtvtk_refresh_fault_planes, :gmtvtk_overlay_style_h, :gmtvtk_set_overlay_style_h,
	:gmtvtk_set_igrf_point_callback, :gmtvtk_set_igrf_grid_callback, :gmtvtk_set_igrf_file_callback,
	:gmtvtk_set_rtp3d_callback,
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
	:gmtvtk_set_clipgrid_callback, :gmtvtk_set_empilhador_callback, :gmtvtk_set_gridcalc_callback, :gmtvtk_set_grdtrend_callback, :gmtvtk_set_grdlandmask_callback, :gmtvtk_set_grdfilter_callback, :gmtvtk_set_interpolate_callback, :gmtvtk_set_ui_dir,
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
	:gmtvtk_xyplot_add_now_cross,
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
	man = joinpath(bin, ".dll_requires")
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
		       (Sys.iswindows() ? "" : _linux_load_hint(bin, msg))
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
	_DLL[] = h
	_LIB_USED[] = lib
	return nothing
end

# Load the viewer: try each candidate in preference order, first one that FULLY loads wins.
# Idempotent.
function _load_library()
	_DLL[] == C_NULL || return                       # already loaded
	isempty(_LIB_CANDIDATES) && error("gmtvtk.dll not found (looked in $(dirname(_LOCAL_LIB)) and $(dirname(_SHARED_LIB))) -- build it with deps/build.bat")
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
	return
end
