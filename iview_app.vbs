' Launch the InteractiveGMT viewer. Started MINIMIZED (style 7) not hidden: a hidden launch sets
' STARTUPINFO SW_HIDE, which the Qt GUI window would inherit and stay invisible. iview_app.jl
' hides its own console on startup, so only the viewer window remains.
'
' Files dropped ONTO the desktop shortcut arrive here as WScript.Arguments; we forward each path
' to Julia, and iview_app.jl opens it (otherwise, with no args, it opens an empty launcher).
' sh.Run """C:\programs\Julia-1.10\bin\julia.exe"" --project=""C:\Users\j\.julia\dev\InteractiveGMT"" ""C:\Users\j\.julia\dev\InteractiveGMT\iview_app.jl""" & extra, 7, False
Dim sh : Set sh = CreateObject("WScript.Shell")
Dim extra : extra = ""
Dim i
For i = 0 To WScript.Arguments.Count - 1
    extra = extra & " """ & WScript.Arguments(i) & """"
Next
sh.Run """C:\programs\Julia-1.10\bin\julia.exe"" ""C:\Users\j\.julia\dev\InteractiveGMT\iview_app.jl""" & extra, 7, False
