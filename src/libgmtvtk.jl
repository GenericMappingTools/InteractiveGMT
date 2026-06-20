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

# Toolchain runtime DLL dirs (this machine). Dependent DLLs (Qt6*, vtk*) resolve from PATH at
# load time. Override any of these via the matching ENV var BEFORE `using InteractiveGMT`.
const _VTK_BIN = get(ENV, "INTERACTIVEGMT_VTK_BIN", raw"C:\programs\compa_libs\VTK-9.6.2\compileds\bin")
const _QT_BIN  = get(ENV, "INTERACTIVEGMT_QT_BIN",  raw"C:\programs\Qt6\6.11.1\msvc2022_64\bin")
const _QT_PLAT = get(ENV, "INTERACTIVEGMT_QT_PLAT", raw"C:\programs\Qt6\6.11.1\msvc2022_64\plugins\platforms")
const _LIB     = joinpath(_PKGROOT, "deps", "build", "gmtvtk.dll")

const _DLL     = Ref{Ptr{Cvoid}}(C_NULL)
const _LIB_FNS = Dict{Symbol,Ptr{Cvoid}}()

# Every exported C-API symbol resolved at load time.
const _LIB_SYMBOLS = (
	:gmtvtk_view_grid, :gmtvtk_view_demo, :gmtvtk_process_events,
	:gmtvtk_add_overlay, :gmtvtk_add_overlay_h, :gmtvtk_is_alive,
	:gmtvtk_add_curtain_h, :gmtvtk_add_curtain_file_h,
	:gmtvtk_view_points, :gmtvtk_selection_count, :gmtvtk_get_selection,
	:gmtvtk_view_fv, :gmtvtk_set_julia_eval, :gmtvtk_set_table,
	:gmtvtk_save_png, :gmtvtk_orbit, :gmtvtk_set_stereo,
	:gmtvtk_open_empty, :gmtvtk_set_drop_callback, :gmtvtk_add_surface_h,
	:gmtvtk_promote_surface_h,
	:gmtvtk_has_surface, :gmtvtk_close, :gmtvtk_add_recent,
	:gmtvtk_set_cpt, :gmtvtk_raise, :gmtvtk_set_crs,
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
