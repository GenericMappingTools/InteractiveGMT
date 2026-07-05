# libgmtvtk_test.jl — loads deps/build/gmtvtk_test.dll, the test-only twin of gmtvtk.dll (built
# from the SAME source with GMTVTK_TEST_API defined, see deps/CMakeLists.txt's gmtvtk_test
# target). Holds the gmtvtk_*_test hooks (headless fault/meca/symbol GUI-test harness) that
# production code and the shipped gmtvtk.dll never see or export.
#
# A TestItemRunner @testsetup so every @testitem that needs these hooks opts in with
# `setup=[GmtvtkTest]` — isolated from InteractiveGMT itself, per "tests are a separate
# department".
#
# Scene* handles created by the PRODUCTION gmtvtk.dll (windows opened the normal way, through
# IG._fn) are safe to pass into gmtvtk_test.dll's functions: both DLLs compile the identical
# source with the identical compiler, so the Scene struct layout matches byte-for-byte and a
# Scene* is just an address. The one thing that does NOT cross the DLL boundary is file-static
# globals — each DLL holds its OWN copy of e.g. `g_juliaFaultGeom` — so a callback registered via
# IG._register_faultgeom() (which calls into gmtvtk.dll) is invisible to code running inside
# gmtvtk_test.dll. _register_faultgeom_test() below mirrors that one registration into the test
# dll's own global, since gmtvtk_fault_apply_test's geog=1 path depends on it.
@testmodule GmtvtkTest begin

using InteractiveGMT

const _TEST_LIB = joinpath(InteractiveGMT._PKGROOT, "deps", "build", "gmtvtk_test.dll")
const _TEST_DLL = Ref{Ptr{Cvoid}}(C_NULL)
const _TEST_FNS = Dict{Symbol,Ptr{Cvoid}}()

const _TEST_SYMBOLS = (
	:gmtvtk_fault_add_test, :gmtvtk_fault_apply_test, :gmtvtk_fault_plane_test,
	:gmtvtk_set_flat2d_test, :gmtvtk_objrows_test,
	:gmtvtk_fault_open_dialog_test, :gmtvtk_fault_close_dialog_test, :gmtvtk_trace_zbounds_test,
	:gmtvtk_meca_drag_test,
	:gmtvtk_symbol_add_test, :gmtvtk_symbol_drag_test,
	:gmtvtk_symbol_get_pos_test, :gmtvtk_symbol_ui_drag_test, :gmtvtk_sym_debug_test,
	:gmtvtk_send_ctrlc_test, :gmtvtk_clipboard_get_test,
	:gmtvtk_set_faultgeom_callback,   # NOT test-only — dlsym'd here too so we can mirror the
	                                  # callback registration into this dll's own global.
)

function _load_test_library()
	_TEST_DLL[] == C_NULL || return
	isfile(_TEST_LIB) || error("gmtvtk_test.dll not found at $_TEST_LIB — build with deps/build.bat")
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

function _register_faultgeom_test()
	fptr = @cfunction((a, b, c, d) -> Base.invokelatest(InteractiveGMT._on_faultgeom, a, b, c, d),
	                  Cstring, (Cdouble, Cdouble, Cdouble, Cdouble))
	ccall(_test_fn(:gmtvtk_set_faultgeom_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

export _test_fn, _register_faultgeom_test

end
