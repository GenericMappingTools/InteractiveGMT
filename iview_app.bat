@echo off
REM Visible-console launcher for the InteractiveGMT empty viewer (for debugging; the desktop
REM shortcut uses iview_app.vbs which runs hidden). Shows Julia startup + any errors.
"C:\programs\Julia-1.10\bin\julia.exe" --project="C:\Users\j\.julia\dev\InteractiveGMT" "C:\Users\j\.julia\dev\InteractiveGMT\iview_app.jl"
pause
