' Launch the InteractiveGMT viewer. Started MINIMIZED (style 7) not hidden: a hidden launch sets
' STARTUPINFO SW_HIDE, which the Qt GUI window would inherit and stay invisible. iview_app.jl
' hides its own console on startup, so only the viewer window remains.
'
' Path resolution, fast path first:
'   1. If iview_app.jl sits right next to this script (running in place -- a dev checkout, or
'      inside a live Pkg package folder), use this script's own folder. Zero overhead, exactly
'      one julia.exe launch, same speed as always.
'   2. Only if this is a DETACHED copy (e.g. sitting on your Desktop, so iview_app.jl is NOT next
'      to it) does it fall back to scanning ~/.julia/packages/InteractiveGMT/* for the
'      newest-modified folder -- a git-based Pkg install lives in a content-hashed folder that
'      gets a NEW hash on every Pkg.update, so a Desktop copy of this file needs SOME way to find
'      the current one. This is a plain filesystem scan, not a Julia subprocess -- still only one
'      julia.exe launch total, never two.
'
' (An earlier version asked Julia itself, via a second julia.exe subprocess, to resolve the path
' on every launch -- correct, but doubled startup time. Don't reintroduce that.)
'
' Files dropped ONTO the desktop shortcut arrive here as WScript.Arguments; we forward each path
' to Julia, and iview_app.jl opens it (otherwise, with no args, it opens an empty launcher).
Dim fso : Set fso = CreateObject("Scripting.FileSystemObject")
Dim sh  : Set sh  = CreateObject("WScript.Shell")

Dim here : here = fso.GetParentFolderName(WScript.ScriptFullName)

If Not fso.FileExists(here & "\iview_app.jl") Then
    ' Detached copy -- find the newest InteractiveGMT folder in the Pkg depot.
    Dim depot : depot = sh.ExpandEnvironmentStrings("%USERPROFILE%") & "\.julia\packages\InteractiveGMT"
    here = ""
    If fso.FolderExists(depot) Then
        Dim subFolder, best
        best = -1
        For Each subFolder In fso.GetFolder(depot).SubFolders
            If fso.FileExists(subFolder.Path & "\iview_app.jl") Then
                If CDbl(subFolder.DateLastModified) > best Then
                    best = CDbl(subFolder.DateLastModified)
                    here = subFolder.Path
                End If
            End If
        Next
    End If
End If

If here = "" Then
    MsgBox "Could not locate InteractiveGMT." & vbCrLf & _
           "Is it added ('] add https://github.com/GenericMappingTools/InteractiveGMT') in Julia's default environment?", _
           vbExclamation, "iGMT"
    WScript.Quit 1
End If

Dim extra : extra = ""
Dim i
For i = 0 To WScript.Arguments.Count - 1
    extra = extra & " " & Chr(34) & WScript.Arguments(i) & Chr(34)
Next
sh.Run "julia --project=" & Chr(34) & here & Chr(34) & " " & Chr(34) & here & "\iview_app.jl" & Chr(34) & extra, 7, False
