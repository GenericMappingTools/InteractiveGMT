# Desktop-launcher script: open an empty InteractiveGMT viewer (drag-and-drop launcher) and keep
# the process alive while the window is open. Run by iview_app.vbs / the desktop shortcut.
#
#   julia --project=<this package dir> iview_app.jl
#
# Errors are appended to iview_app.log next to this file (the shortcut runs hidden, so without a
# log a failure would be invisible).
#
# Console hiding: we are launched with a VISIBLE console (so the GUI window is NOT force-hidden by
# the launcher's SW_HIDE), then hide our own console window here. Hiding the console does NOT touch
# the Qt GUI window. Windows-only.
let
    hwnd = ccall((:GetConsoleWindow, "kernel32"), stdcall, Ptr{Cvoid}, ())
    hwnd != C_NULL && ccall((:ShowWindow, "user32"), stdcall, Cint, (Ptr{Cvoid}, Cint), hwnd, 0)  # SW_HIDE
end

import Dates
try
    using InteractiveGMT
    if isempty(ARGS)
        iview()                       # no files: empty launcher window (drop files onto it)
    else
        # Files dropped on the desktop icon: open each through the SAME path as a drop onto a
        # window / File > Open / File > Recent Files — an empty launcher promoted in place by
        # _on_drop. One shared open-file path; iview(f) (gmtvtk_view_grid) would be a second one.
        for f in ARGS
            try
                fig = iview()         # empty launcher window
                InteractiveGMT._on_drop(InteractiveGMT._fig_handle(fig), abspath(f))
            catch e
                @warn "could not open dropped file" file=f exception=e
            end
        end
    end
    wait_windows()    # block (yielding, so the Qt pump runs) until the window(s) close
catch e
    open(joinpath(@__DIR__, "iview_app.log"), "a") do io
        println(io, "[", Dates.now(), "] ", sprint(showerror, e, catch_backtrace()))
    end
    rethrow()
end
