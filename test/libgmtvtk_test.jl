# libgmtvtk_test.jl â€” loads deps/build/gmtvtk_test.dll, the test-only twin of gmtvtk.dll (built
# from the SAME source with GMTVTK_TEST_API defined, see deps/CMakeLists.txt's gmtvtk_test
# target). Holds the gmtvtk_*_test hooks (headless fault/meca/symbol GUI-test harness) that
# production code and the shipped gmtvtk.dll never see or export.
#
# A TestItemRunner @testsetup so every @testitem that needs these hooks opts in with
# `setup=[GmtvtkTest]` â€” isolated from InteractiveGMT itself, per "tests are a separate
# department".
#
# Scene* handles created by the PRODUCTION gmtvtk.dll (windows opened the normal way, through
# IG._fn) are safe to pass into gmtvtk_test.dll's functions: both DLLs compile the identical
# source with the identical compiler, so the Scene struct layout matches byte-for-byte and a
# Scene* is just an address. The one thing that does NOT cross the DLL boundary is file-static
# globals â€” each DLL holds its OWN copy of e.g. `g_juliaFaultGeom` â€” so a callback registered via
# IG._register_faultgeom() (which calls into gmtvtk.dll) is invisible to code running inside
# gmtvtk_test.dll. _register_faultgeom_test() below mirrors that one registration into the test
# dll's own global, since gmtvtk_fault_apply_test's geog=1 path depends on it.
@testmodule GmtvtkTest begin

using InteractiveGMT

const _TEST_LIB = joinpath(InteractiveGMT._PKGROOT, "deps", "build", "gmtvtk_test.dll")
const _TEST_DLL = Ref{Ptr{Cvoid}}(C_NULL)
const _TEST_FNS = Dict{Symbol,Ptr{Cvoid}}()

const _TEST_SYMBOLS = (
	:gmtvtk_fault_add_test, :gmtvtk_fault_apply_test, :gmtvtk_fault_plane_test, :gmtvtk_poly_edit_add_test,
	:gmtvtk_settings_format_test,
	:gmtvtk_set_flat2d_test, :gmtvtk_objrows_test,
	:gmtvtk_fft_dialog_test, :gmtvtk_fft_sizes_test, :gmtvtk_fft_park_test, :gmtvtk_scene_adopt_test,
	:gmtvtk_set_fftstuff_callback,
	:gmtvtk_fault_open_dialog_test, :gmtvtk_fault_close_dialog_test, :gmtvtk_trace_zbounds_test,
	:gmtvtk_meca_drag_test,
	:gmtvtk_symbol_add_test, :gmtvtk_symbol_drag_test,
	:gmtvtk_symbol_get_pos_test, :gmtvtk_symbol_ui_drag_test, :gmtvtk_sym_debug_test,
	:gmtvtk_symbol_click_jitter_test, :gmtvtk_symbol_layer_test, :gmtvtk_symbol_remove_test, :gmtvtk_seismicity_send_test, :gmtvtk_active_axes_test, :gmtvtk_symbol_table_test, :gmtvtk_symbol_toplayer_test, :gmtvtk_pixel_count_test, :gmtvtk_symbol_hover_test,
	:gmtvtk_send_ctrlc_test, :gmtvtk_clipboard_get_test, :gmtvtk_camera_get_test,
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
	:gmtvtk_platecalc_open_dialog_test, :gmtvtk_platecalc_close_dialog_test,
	:gmtvtk_platecalc_delete_dialog_test, :gmtvtk_platecalc_parked_test,
	:gmtvtk_platecalc_select_test, :gmtvtk_platecalc_calc_test,
	# Make movie: opened through this dll's OWN hook, which inserts the production-made Scene* into
	# this dll's live-scene set first -- see the file-static note above.
	:gmtvtk_movie_open_dialog_test, :gmtvtk_movie_close_dialog_test,
	:gmtvtk_movie_parked_test, :gmtvtk_movie_delete_dialog_test,
	:gmtvtk_platecalc_read_test, :gmtvtk_platecalc_map_click_test, :gmtvtk_platecalc_map_test,
	:gmtvtk_set_faultgeom_callback,   # NOT test-only â€” dlsym'd here too so we can mirror the
	                                  # callback registration into this dll's own global.
	:gmtvtk_set_euler_callback, :gmtvtk_euler_result,   # same, for the Plates dialogs.
)

function _load_test_library()
	_TEST_DLL[] == C_NULL || return
	isfile(_TEST_LIB) || error("gmtvtk_test.dll not found at $_TEST_LIB â€” build with deps/build.bat")
	InteractiveGMT._load_library()             # ensures VTK/Qt toolchain dirs are already on PATH
	_TEST_DLL[] = Base.Libc.Libdl.dlopen(_TEST_LIB)
	for s in _TEST_SYMBOLS
		_TEST_FNS[s] = Base.Libc.Libdl.dlsym(_TEST_DLL[], s)
	end
	return
end

function _test_fn(sym::Symbol)::Ptr{Cvoid}
	_load_test_library()
	p = get(_TEST_FNS, sym, C_NULL)
	p == C_NULL && error("gmtvtk_test.dll missing symbol :$sym")
	return p
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

# CLICK one of its buttons by objectName â€” the real handler runs, exactly as a user click would.
fft_click(scene::Ptr{Cvoid}, button::AbstractString) =
	ccall(_test_fn(:gmtvtk_fft_dialog_test), Cint, (Ptr{Cvoid}, Cstring, Cstring), scene, String(button), "")

# What the two padding boxes ended up holding: (rows, cols).
function fft_sizes(scene::Ptr{Cvoid})
	out = zeros(Cint, 2)
	ccall(_test_fn(:gmtvtk_fft_sizes_test), Cint, (Ptr{Cvoid}, Ptr{Cint}), scene, out)
	return (Int(out[1]), Int(out[2]))
end

# Let THIS dll consider `scene` alive â€” `sceneAlive`'s set is a file-static per dll, so anything
# gated on it (parkTool, and every other refuse-to-touch-a-dead-scene path) would otherwise no-op
# on a window the production dll opened. Same mirroring the callback registrations above do.
scene_adopt(scene::Ptr{Cvoid}) =
	ccall(_test_fn(:gmtvtk_scene_adopt_test), Cint, (Ptr{Cvoid},), scene)

# Minimise the FFT dialog (parking it in Scene Objects) or bring it back: 1 = parked, 0 = showing.
fft_park(scene::Ptr{Cvoid}, park::Bool) =
	ccall(_test_fn(:gmtvtk_fft_park_test), Cint, (Ptr{Cvoid}, Cint), scene, park ? Cint(1) : Cint(0))

# The Scene Objects panel as text â€” the only proof a result was really PUT IN THE WINDOW.
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
# this dll cannot see â€” so the wrapper mirrors Julia's own record of the answer
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
# gmtvtk_compute_euler_progress on a Julia Timer, and again that lands in the production dll â€” so the
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
