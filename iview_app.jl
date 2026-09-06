# Desktop-launcher script: open an empty InteractiveGMT viewer (drag-and-drop launcher) and keep
# the process alive while the window is open. Run by deps/build/igmt (deps/src/launcher.c), the
# desktop launcher behind the icon on all three systems.
#
#   julia --project=<this package dir> iview_app.jl
#
# Errors are appended to iview_app.log next to this file (the shortcut runs hidden, so without a
# log a failure would be invisible).
#
# Console hiding: we are launched with a VISIBLE console (so the GUI window is NOT force-hidden by
# the launcher's SW_HIDE), then hide our own console window here. Hiding the console does NOT touch
# the Qt GUI window. WINDOWS-ONLY, AND THE GUARD IS LOAD-BEARING: this block sits above the try
# below, so on a system with no kernel32 the ccall threw before anything was wrapped — nothing
# reached iview_app.log, no window opened, and the ready-flag the desktop splash polls was never
# written, so the splash sat there for its full 180 s timeout with the launcher's stderr going to
# /dev/null. That was the macOS/Linux desktop icon "doing nothing": not the bundle, not Finder, not
# the viewer — four lines of Windows API called unconditionally.
if Sys.iswindows()
    hwnd = ccall((:GetConsoleWindow, "kernel32"), stdcall, Ptr{Cvoid}, ())
    hwnd != C_NULL && ccall((:ShowWindow, "user32"), stdcall, Cint, (Ptr{Cvoid}, Cint), hwnd, 0)  # SW_HIDE
end

import Dates
let
    p = get(ENV, "INTERACTIVEGMT_DEBUG", "")
    path = isempty(p) ? "" : p == "1" ? joinpath(tempdir(), "igmt.log") : p
    isempty(path) || open(io -> println(io, round(time(); digits=3), "  startup  iview_app.jl script start"), path, "a")
end
const SPLASH_FLAG = joinpath(tempdir(), "igmt_ready.flag")

# The desktop-icon splash (the igmt launcher) polls this path and closes the instant it appears.
# The flag is NOT written here: the window becomes visible inside the viewer's own `win->show()`,
# more than a second before that call returns to Julia, so anything written on this side leaves
# the splash covering a window the user can already see. Hand the path to the viewer instead —
# `splashDropOnShow` (70_window.cpp) writes it in the same statement that shows the window.
ENV["IGMT_SPLASH_FLAG"] = SPLASH_FLAG

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
finally
    isfile(SPLASH_FLAG) && rm(SPLASH_FLAG; force=true)
end
