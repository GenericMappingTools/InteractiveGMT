# Desktop-launcher script: open an empty InteractiveGMT viewer (drag-and-drop launcher) and keep
# the process alive while the window is open. Run by deps/build/igmt (deps/src/launcher.c), the
# desktop launcher behind the icon on all three systems.
#
#   julia --project=<this package dir> iview_app.jl
#
# Errors are appended to iview_app.log next to this file (the shortcut runs hidden, so without a
# log a failure would be invisible) AND written to the failure flag the launcher polls, which
# closes the splash at once and puts the text on screen in a message box.
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

# ------------------------------------------------------------------ splash status + failure
#
# Two more files in the same tempdir, both polled by the splash (launcher.c: status_read /
# launch_failed). They are the ONLY way the user learns what the wait is: the console is hidden,
# and the two slow cases — precompiling GMT.jl, precompiling InteractiveGMT — take minutes on a
# first run with nothing on screen but an animated bar.
const STATUS_FILE = joinpath(tempdir(), "igmt_status.txt")
const FAIL_FLAG   = joinpath(tempdir(), "igmt_fail.flag")
const APP_LOG     = joinpath(@__DIR__, "iview_app.log")
const T0 = time()

status(msg::String) = (try write(STATUS_FILE, msg) catch; end; nothing)
#   ASCII ONLY: the X11 splash draws this through a core font, which is Latin-1.

function elapsed()
    s = round(Int, time() - T0)
    s < 60 ? string(s, "s") : string(div(s, 60), "m", lpad(rem(s, 60), 2, '0'), "s")
end

# Is `name` still to be precompiled? Resolved through InteractiveGMT's OWN dependency context, not
# the active project: an `] add`ed copy has no Manifest of its own and GMT is not necessarily a
# name the default environment can see — identify_package(pkgid, name) asks the right question in
# both layouts. Any failure (older Julia with no isprecompiled, unresolvable name) answers "no",
# which costs nothing but a less specific caption.
const IGMT_ID = try Base.identify_package("InteractiveGMT") catch; nothing end

function stale(name::String)
    try
        id = name == "InteractiveGMT" ? IGMT_ID :
             (IGMT_ID === nothing ? nothing : Base.identify_package(IGMT_ID, name))
        id === nothing ? false : !Base.isprecompiled(id)
    catch
        false
    end
end

# `using InteractiveGMT` below may precompile GMT.jl first and InteractiveGMT after it, inside one
# blocking call. This task re-asks which of the two is still stale every couple of seconds and
# rewrites the caption, so the splash tracks the real phase (and shows the clock, which is what
# makes a five-minute wait tolerable). It runs because precompilation waits on subprocesses and
# yields; if it never gets scheduled the caption simply stays at what was written before the call.
const WATCH = Ref(true)

function watch_compile()
    while WATCH[]
        g, i = stale("GMT"), stale("InteractiveGMT")
        (g || i) || return
        status(string(g ? "Compiling GMT.jl - first run, this can take several minutes" :
                          "Compiling InteractiveGMT - this can take a few minutes",
                      "   (", elapsed(), ")"))
        sleep(2)
    end
end

let g = stale("GMT"), i = stale("InteractiveGMT")
    status(g ? "Compiling GMT.jl - first run, this can take several minutes" :
           i ? "Compiling InteractiveGMT - this can take a few minutes" :
               "Loading InteractiveGMT...")
    (g || i) && (@async watch_compile())
end

try
    using InteractiveGMT
    WATCH[] = false
    status("Opening the viewer window...")
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
    WATCH[] = false
    txt = sprint(showerror, e, catch_backtrace())
    try
        open(APP_LOG, "a") do io
            println(io, "[", Dates.now(), "] ", txt)
        end
    catch
    end
    # The launcher polls this file: it kills the splash the moment it appears and shows the text.
    # First line is the log path so the box can point at the full history.
    try write(FAIL_FLAG, string("LOG: ", APP_LOG, "\n", txt)) catch; end
    rethrow()
finally
    WATCH[] = false
    isfile(SPLASH_FLAG) && rm(SPLASH_FLAG; force=true)
    isfile(STATUS_FILE) && rm(STATUS_FILE; force=true)
end
