' Launch the InteractiveGMT viewer. Started MINIMIZED (style 7) not hidden: a hidden launch sets
' STARTUPINFO SW_HIDE, which the Qt GUI window would inherit and stay invisible. iview_app.jl
' hides its own console on startup, so only the viewer window remains.
'
' Self-locating (same trick as deps/installer/iview_launch.vbs): finds its own folder at runtime
' instead of hardcoding a julia.exe path + dev checkout path, so this works on any machine/checkout
' without editing. Calls `julia` bare (from PATH) with --project pointed at this same folder.
'
' Files dropped ONTO the desktop shortcut arrive here as WScript.Arguments; we forward each path
' to Julia, and iview_app.jl opens it (otherwise, with no args, it opens an empty launcher).
Dim fso : Set fso = CreateObject("Scripting.FileSystemObject")
Dim here : here = fso.GetParentFolderName(WScript.ScriptFullName)
Dim sh : Set sh = CreateObject("WScript.Shell")
Dim extra : extra = ""
Dim i
For i = 0 To WScript.Arguments.Count - 1
    extra = extra & " """ & WScript.Arguments(i) & """"
Next
sh.Run "julia --project=""" & here & """ """ & here & "\iview_app.jl""" & extra, 7, False
