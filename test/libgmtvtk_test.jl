# libgmtvtk_test.jl -- loads deps/build/gmtvtk_test.dll, the test-only twin of gmtvtk.dll (built
# from the SAME source with GMTVTK_TEST_API defined, see deps/CMakeLists.txt's gmtvtk_test
# target). Holds the gmtvtk_*_test hooks (headless fault/meca/symbol GUI-test harness) that
# production code and the shipped gmtvtk.dll never see or export.
#
# A TestItemRunner @testsetup so every @testitem that needs these hooks opts in with
# `setup=[GmtvtkTest]` -- isolated from InteractiveGMT itself, per "tests are a separate
# department".
#
# Scene* handles created by the PRODUCTION gmtvtk.dll (windows opened the normal way, through
# IG._fn) are safe to pass into gmtvtk_test.dll's functions: both DLLs compile the identical
# source with the identical compiler, so the Scene struct layout matches byte-for-byte and a
# Scene* is just an address. The one thing that does NOT cross the DLL boundary is file-static
# globals -- each DLL holds its OWN copy of e.g. `g_juliaFaultGeom` -- so a callback registered via
# IG._register_faultgeom() (which calls into gmtvtk.dll) is invisible to code running inside
# gmtvtk_test.dll. _register_faultgeom_test() below mirrors that one registration into the test
# dll's own global, since gmtvtk_fault_apply_test's geog=1 path depends on it.
@testmodule GmtvtkTest begin

using InteractiveGMT

# The test twin's file name is the platform's, not Windows'. Hardcoding "gmtvtk_test.dll" made every
# test-API item on Linux and macOS die as "not found" even when the library was right there beside it.
const _TEST_LIB_NAME = Sys.iswindows() ? "gmtvtk_test.dll" :
                       Sys.isapple()   ? "libgmtvtk_test.dylib" : "libgmtvtk_test.so"
# ...and it is looked for in BOTH places its production twin is (libgmtvtk.jl's _LOCAL_LIB /
# _SHARED_LIB): the package's own deps/build on a machine that built the repo, and the depot's
# gmtvtk_runtime where a downloaded rolling archive extracts. Looking only in the package is why
# this failed on every CI runner — nothing builds the library there, it is fetched, and a fetched
# one never lands in the checkout.
const _TEST_LOCAL      = joinpath(InteractiveGMT._PKGROOT, "deps", "build", _TEST_LIB_NAME)
const _TEST_SHARED     = joinpath(first(Base.DEPOT_PATH), "gmtvtk_runtime", "deps", "build", _TEST_LIB_NAME)
const _TEST_CANDIDATES = filter(isfile, unique([_TEST_LOCAL, _TEST_SHARED]))
const _TEST_LIB        = isempty(_TEST_CANDIDATES) ? _TEST_SHARED : first(_TEST_CANDIDATES)
const _TEST_DLL = Ref{Ptr{Cvoid}}(C_NULL)
const _TEST_FNS = Dict{Symbol,Ptr{Cvoid}}()

const _TEST_SYMBOLS = (
	:gmtvtk_fault_demo_test, :gmtvtk_set_fault_demo_callback,
	:gmtvtk_magfield_test, :gmtvtk_magfield_test_n, :gmtvtk_set_magfield_callback,
	:gmtvtk_fault_add_test, :gmtvtk_fault_apply_test, :gmtvtk_fault_plane_test, :gmtvtk_poly_edit_add_test,
	:gmtvtk_settings_format_test,
	:gmtvtk_set_flat2d_test, :gmtvtk_objrows_test,
	:gmtvtk_fft_dialog_test, :gmtvtk_fft_sizes_test, :gmtvtk_fft_park_test, :gmtvtk_scene_adopt_test,
	:gmtvtk_ecmwf_dialog_test,
	:gmtvtk_set_fftstuff_callback,
	:gmtvtk_fault_open_dialog_test, :gmtvtk_fault_close_dialog_test, :gmtvtk_trace_zbounds_test,
	:gmtvtk_meca_drag_test,
	:gmtvtk_symbol_add_test, :gmtvtk_symbol_drag_test,
	:gmtvtk_symbol_get_pos_test, :gmtvtk_symbol_ui_drag_test, :gmtvtk_sym_debug_test,
	:gmtvtk_symbol_click_jitter_test, :gmtvtk_symbol_layer_test, :gmtvtk_symbol_remove_test, :gmtvtk_seismicity_send_test, :gmtvtk_active_axes_test, :gmtvtk_symbol_table_test, :gmtvtk_symbol_toplayer_test, :gmtvtk_pixel_count_test, :gmtvtk_symbol_hover_test,
	:gmtvtk_send_ctrlc_test, :gmtvtk_clipboard_get_test, :gmtvtk_camera_get_test,
	:gmtvtk_vector_ground_gap_test,
	:gmtvtk_nswing_enter_test,
	:gmtvtk_visible_region_test,
	:gmtvtk_swipe_btn_enabled_test, :gmtvtk_swipe_set_mode_test,
	:gmtvtk_link_toggle_test, :gmtvtk_link_peek_test, :gmtvtk_link_state_test,
	:gmtvtk_swipe_click_test, :gmtvtk_right_click_test, :gmtvtk_right_button_test,
	:gmtvtk_euler_open_dialog_test, :gmtvtk_euler_close_dialog_test, :gmtvtk_euler_targets_test,
	:gmtvtk_euler_arm_pick_test, :gmtvtk_euler_pick_deliver_test,
	:gmtvtk_euler_parked_test, :gmtvtk_euler_delete_dialog_test,
	:gmtvtk_oc_open_dialog_test, :gmtvtk_oc_close_dialog_test, :gmtvtk_oc_delete_dialog_test,
	:gmtvtk_oc_parked_test, :gmtvtk_oc_state_test, :gmtvtk_oc_select_test,
	:gmtvtk_ceuler_open_dialog_test, :gmtvtk_ceuler_set_test, :gmtvtk_ceuler_read_test,
	:gmtvtk_ceuler_compute_test, :gmtvtk_ceuler_stop_test, :gmtvtk_ceuler_delete_dialog_test,
	:gmtvtk_ceuler_adopt_test, :gmtvtk_menu_trigger_test, :gmtvtk_menu_dump_test,
	:gmtvtk_window_menu_trigger_test, :gmtvtk_window_exists_test, :gmtvtk_pt_picker_shot_test,
	:gmtvtk_earthregions_list_test, :gmtvtk_earthregions_pick_test, :gmtvtk_earthregions_code_test,
	:gmtvtk_earthregions_region_test, :gmtvtk_earthregions_type_test,
	:gmtvtk_compute_euler_progress,
	:gmtvtk_aqua_side_rgb_test,       # per-side mean RGB off the live tsunami texture
	:gmtvtk_aqua_hold_arrow_test,     # hold the slice slider's < / > down for real, report the travel
	:gmtvtk_aqua_arrow_press_test,    # press / release an arrow, no pumping (the app's pump drives it)
	:gmtvtk_aqua_slider_value_test,   # the slice slider's value, no pumping
	:gmtvtk_render_count_test,        # renders that reached the screen since the last reset
	:gmtvtk_axes_sets_test,           # EVERY axes set: name|shown|onscreen|zlock|z0|z1 per line
	:gmtvtk_image_tex_hash_test,      # hash + dims of an image extra's texture bytes
	:gmtvtk_aqua_arrow_screen_test,   # screen pos of an arrow, for REAL OS mouse input
	:gmtvtk_view_fixed_size_test,     # pin the 3-D view widget size (comparable captures)
	:gmtvtk_objtree_checks_test,      # Scene Objects tree with each row's checkbox state
	:gmtvtk_objrow_click_test,        # click a row's checkbox by label path, as the user does
	:gmtvtk_aqua_edit_tip_test,       # type into a dialog line edit, finish the edit, read its hover text
	:gmtvtk_aqua_check_test,          # tick a dialog checkbox by name, its handler runs
	:gmtvtk_aqua_eta_curve_test,      # the eta(x) figure's live curve: point count + sum of y
	:gmtvtk_aqua_show_eta_test,       # put the eta(x) figure up through the Cinema tab's own box
	:gmtvtk_aqua_set_prof_range_test, # the Cinema profile x0/length boxes (the span the host is asked for)
	:gmtvtk_widget_enabled_test,      # is a named widget enabled? (the benchmark lock, on the widget)

	:gmtvtk_platecalc_open_dialog_test, :gmtvtk_platecalc_close_dialog_test,
	:gmtvtk_platecalc_delete_dialog_test, :gmtvtk_platecalc_parked_test,
	:gmtvtk_platecalc_select_test, :gmtvtk_platecalc_calc_test,
	# Make movie: opened through this dll's OWN hook, which inserts the production-made Scene* into
	# this dll's live-scene set first -- see the file-static note above.
	:gmtvtk_movie_open_dialog_test, :gmtvtk_movie_close_dialog_test,
	:gmtvtk_movie_parked_test, :gmtvtk_movie_delete_dialog_test,
	:gmtvtk_platecalc_read_test, :gmtvtk_platecalc_map_click_test, :gmtvtk_platecalc_map_test,
	:gmtvtk_set_faultgeom_callback,   # NOT test-only -- dlsym'd here too so we can mirror the
	                                  # callback registration into this dll's own global.
	:gmtvtk_set_euler_callback, :gmtvtk_euler_result,   # same, for the Plates dialogs.
	:gmtvtk_set_ui_dir,               # ditto: the .ui directory is a file-static override per dll.
	:gmtvtk_tile_mesh_test,           # the grid mesh's NaN contract, counted off makeGridTile's output
)

function _load_test_library()
	_TEST_DLL[] == C_NULL || return
	isfile(_TEST_LIB) || error("$_TEST_LIB_NAME not found at $_TEST_LIB - build it " *
	                           "(Windows: deps/build.bat, Linux: deps/build.sh)")
	InteractiveGMT._load_library()             # ensures VTK/Qt toolchain dirs are already on PATH
	_TEST_DLL[] = Base.Libc.Libdl.dlopen(_TEST_LIB)
	for s in _TEST_SYMBOLS
		_TEST_FNS[s] = Base.Libc.Libdl.dlsym(_TEST_DLL[], s)
	end
	# Mirror the .ui directory into THIS dll, for the same reason the callbacks above are mirrored:
	# the override is a file-static, so the one InteractiveGMT._load_library() pushed reached the
	# production library only. Left unset, this dll falls back to the path compiled into it — the
	# BUILD machine's deps/ui, which does not exist anywhere else — and every dialog it opens through
	# QUiLoader silently fails to load, which reads as "the hook returned 0".
	let uidir = joinpath(InteractiveGMT._PKGROOT, "deps", "ui")
		isdir(uidir) && ccall(_TEST_FNS[:gmtvtk_set_ui_dir], Cvoid, (Cstring,), uidir)
	end
	return
end

function _test_fn(sym::Symbol)::Ptr{Cvoid}
	_load_test_library()
	p = get(_TEST_FNS, sym, C_NULL)
	p == C_NULL && error("$_TEST_LIB_NAME missing symbol :$sym")
	return p
end

# Mesh a grid with the real makeGridTile (one tile, whole grid) and count what it built. `Z` is
# indexed Z[i, j] with i along x and j along y (south first), i.e. exactly the BCB memory the builder
# reads. Returns a NamedTuple of the counters gmtvtk_tile_mesh_test documents.
function tile_mesh_stats(Z::Matrix{Float32}; step::Int = 1, holeRim::Bool = false, pixelCells::Bool = false)
	nx, ny = size(Z)
	zb = vec(permutedims(Z))                    # z[i*ny + j]: j fastest
	out = zeros(Float64, 6)
	ok = ccall(_test_fn(:gmtvtk_tile_mesh_test), Cint,
	           (Ptr{Cfloat}, Cint, Cint, Cint, Cint, Cint, Ptr{Cdouble}),
	           zb, nx, ny, step, holeRim, pixelCells, out)
	ok == 1 || error("gmtvtk_tile_mesh_test refused the input")
	return (cells = Int(out[1]), points = Int(out[2]), nan_cells = Int(out[3]),
	        on_nan = Int(out[4]), invented = Int(out[5]), missed = Int(out[6]))
end

# The FFT tool (Mag/Grav > FFT tool, Image > FFT Spectrum) asks Julia for everything it does, so a
# dialog built inside gmtvtk_test.dll needs THIS dll's own g_juliaFFTStuff set.
function register_fftstuff_test()
	fptr = @cfunction((s, c, t, n) -> Base.invokelatest(InteractiveGMT._on_fftstuff, s, c, t, n)::Cint,
	                  Cint, (Ptr{Cvoid}, Cstring, Ptr{UInt8}, Cint))
	ccall(_test_fn(:gmtvtk_set_fftstuff_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# Open the FFT dialog on `scene` (1 = it is up). `png` non-empty also grabs it, for a layout check.
fft_open(scene::Ptr{Cvoid}, png::AbstractString = "") =
	ccall(_test_fn(:gmtvtk_fft_dialog_test), Cint, (Ptr{Cvoid}, Cstring, Cstring), scene, "", String(png))

# CLICK one of its buttons by objectName -- the real handler runs, exactly as a user click would.
fft_click(scene::Ptr{Cvoid}, button::AbstractString) =
	ccall(_test_fn(:gmtvtk_fft_dialog_test), Cint, (Ptr{Cvoid}, Cstring, Cstring), scene, String(button), "")

# What the two padding boxes ended up holding: (rows, cols).
function fft_sizes(scene::Ptr{Cvoid})
	out = zeros(Cint, 2)
	ccall(_test_fn(:gmtvtk_fft_sizes_test), Cint, (Ptr{Cvoid}, Ptr{Cint}), scene, out)
	return (Int(out[1]), Int(out[2]))
end

# Let THIS dll consider `scene` alive -- `sceneAlive`'s set is a file-static per dll, so anything
# gated on it (parkTool, and every other refuse-to-touch-a-dead-scene path) would otherwise no-op
# on a window the production dll opened. Same mirroring the callback registrations above do.
scene_adopt(scene::Ptr{Cvoid}) =
	ccall(_test_fn(:gmtvtk_scene_adopt_test), Cint, (Ptr{Cvoid},), scene)

# Minimise the FFT dialog (parking it in Scene Objects) or bring it back: 1 = parked, 0 = showing.
fft_park(scene::Ptr{Cvoid}, park::Bool) =
	ccall(_test_fn(:gmtvtk_fft_park_test), Cint, (Ptr{Cvoid}, Cint), scene, park ? Cint(1) : Cint(0))

# The Scene Objects panel as text -- the only proof a result was really PUT IN THE WINDOW.
objrows(scene::Ptr{Cvoid}) =
	unsafe_string(ccall(_test_fn(:gmtvtk_objrows_test), Cstring, (Ptr{Cvoid},), scene))

function _register_faultgeom_test()
	fptr = @cfunction((a, b, c, d) -> Base.invokelatest(InteractiveGMT._on_faultgeom, a, b, c, d),
	                  Cstring, (Cdouble, Cdouble, Cdouble, Cdouble))
	ccall(_test_fn(:gmtvtk_set_faultgeom_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# The Plates dialogs (Euler rotations, Plate calculator) ASK Julia for their content, so a dialog
# built inside gmtvtk_test.dll needs that dll's own g_juliaEuler set. Julia's answer travels the
# other way through gmtvtk.dll's gmtvtk_euler_result (IG._fn resolves in the production dll), which
# this dll cannot see -- so the wrapper mirrors Julia's own record of the answer
# (InteractiveGMT._euler_last_result) into THIS dll's copy right after the call returns.
function _euler_test_cb(scene::Ptr{Cvoid}, params::Cstring)::Cint
	r = Base.invokelatest(InteractiveGMT._on_euler, scene, params)
	ccall(_test_fn(:gmtvtk_euler_result), Cvoid, (Cstring,), InteractiveGMT._euler_last_result[])
	return r
end

function _register_euler_test()
	fptr = @cfunction(_euler_test_cb, Cint, (Ptr{Cvoid}, Cstring))
	ccall(_test_fn(:gmtvtk_set_euler_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# Compute Euler pole: same story as above, plus the run's PROGRESS. Its search reports back through
# gmtvtk_compute_euler_progress on a Julia Timer, and again that lands in the production dll -- so the
# extra sink InteractiveGMT keeps for exactly this is pointed at this dll's copy of the export.
function _register_ceuler_test()
	_register_euler_test()
	InteractiveGMT._CE_EXTRA_PUSH[] = (cur, mx, txt) ->
		ccall(_test_fn(:gmtvtk_compute_euler_progress), Cvoid, (Cint, Cint, Cstring),
		      Cint(cur), Cint(mx), txt)
	return
end

export _test_fn, register_fftstuff_test, fft_open, fft_click, fft_sizes, fft_park, scene_adopt, objrows,
	_register_faultgeom_test, _register_euler_test, _register_ceuler_test

end
