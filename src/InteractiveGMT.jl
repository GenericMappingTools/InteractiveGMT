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
include("eventloop.jl")
include("console.jl")

# --- shared helpers ----------------------------------------------------------------------
include("colors.jl")
include("cpt.jl")
include("drape.jl")

# --- viewers + scene elements ------------------------------------------------------------
include("grid.jl")
include("table.jl")
include("curtain.jl")
include("points.jl")
include("fv.jl")
include("dispatch.jl")
include("drop.jl")

export view_grid, view_image, view_points, view_fv, view_demo, iview,
       add!, add_curtain!, show_table, selection, isalive,
       poly2fv, colorize_by_z!, save_png, wait_windows, stereo!,
       QtFigure, QtPoints, QtFV, QtImage, QtEmpty

# Load the viewer DLL + register the Julia-console callback. RUNTIME ONLY — a dlopen handle, the
# dlsym pointers and the @cfunction are all runtime values that cannot be baked into a precompiled
# image, so they must be created here, never at top level. Tolerant of a missing/unbuilt DLL (and
# of non-Windows) so `using InteractiveGMT` still succeeds; viewer calls then error on first use.
function __init__()
	try
		_load_library()
		_register_console_eval()
		_register_drop_callback()
	catch e
		@warn "InteractiveGMT: the Qt+VTK viewer DLL could not be loaded; build it with deps/build.bat (Windows only). Viewer calls will error until then." exception=(e,)
	end
end

end # module InteractiveGMT
