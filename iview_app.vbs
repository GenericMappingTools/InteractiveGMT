' Launch the InteractiveGMT viewer. Started MINIMIZED (style 7) not hidden: a hidden launch sets
' STARTUPINFO SW_HIDE, which the Qt GUI window would inherit and stay invisible. iview_app.jl
' hides its own console on startup, so only the viewer window remains.
'
' Resolves InteractiveGMT's install path by ASKING JULIA at every launch (Base.find_package —
' just reads the active project's Manifest, does not load/precompile the package) instead of
' hardcoding a location. This matters because a git-based Pkg install lives in a content-hashed
' folder (~/.julia/packages/InteractiveGMT/<hash>/) that gets a NEW <hash> on every
' `Pkg.update("InteractiveGMT")` — a script that instead finds its OWN folder (fso.GetParentFolderName)
' would go stale the moment you copy it to a Desktop shortcut and then update. Asking Julia fresh
' every time means the SAME copy of this file — wherever you put it, including a Desktop shortcut
' — keeps working across every future update, no re-copying, ever.
'
' Also works unmodified for a plain dev checkout (this file run in place): Base.find_package
' resolves `dev`ed packages from the Manifest too, same as `add`ed ones.
'
' The lookup expression is written to a temp .jl file rather than passed inline on the command
' line, to sidestep Windows/VBScript quote-escaping entirely.
'
' Files dropped ONTO the desktop shortcut arrive here as WScript.Arguments; we forward each path
' to Julia, and iview_app.jl opens it (otherwise, with no args, it opens an empty launcher).
Dim fso : Set fso = CreateObject("Scripting.FileSystemObject")
Dim sh  : Set sh  = CreateObject("WScript.Shell")

Dim tmp : tmp = sh.ExpandEnvironmentStrings("%TEMP%") & "\igmt_locate_" & CStr(Timer) & ".jl"
Dim f : Set f = fso.CreateTextFile(tmp, True)
f.WriteLine "p = Base.find_package(" & Chr(34) & "InteractiveGMT" & Chr(34) & ")"
f.WriteLine "print(p === nothing ? " & Chr(34) & Chr(34) & " : dirname(dirname(p)))"
f.Close

Dim ex : Set ex = sh.Exec("julia --startup-file=no " & Chr(34) & tmp & Chr(34))
Dim here : here = Trim(ex.StdOut.ReadAll())
fso.DeleteFile tmp, True

If here = "" Then
    MsgBox "Could not locate InteractiveGMT." & vbCrLf & _
           "Is it added ('] add https://github.com/GenericMappingTools/InteractiveGMT') or dev'd in Julia's default environment?", _
           vbExclamation, "iGMT"
    WScript.Quit 1
End If

Dim extra : extra = ""
Dim i
For i = 0 To WScript.Arguments.Count - 1
    extra = extra & " " & Chr(34) & WScript.Arguments(i) & Chr(34)
Next
sh.Run "julia --project=" & Chr(34) & here & Chr(34) & " " & Chr(34) & here & "\iview_app.jl" & Chr(34) & extra, 7, False
