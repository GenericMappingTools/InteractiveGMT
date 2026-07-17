' Launch the InteractiveGMT viewer. Started MINIMIZED (style 7) not hidden: a hidden launch sets
' STARTUPINFO SW_HIDE, which the Qt GUI window would inherit and stay invisible. iview_app.jl
' hides its own console on startup, so only the viewer window remains.
'
' Path resolution, fast path first:
'   1. If iview_app.jl sits right next to this script (running in place -- a dev checkout, or
'      inside a live Pkg package folder), use this script's own folder. Zero overhead, exactly
'      one julia.exe launch, same speed as always.
'   2. Only if this is a DETACHED copy (e.g. sitting on your Desktop, so iview_app.jl is NOT next
'      to it) does it look for a live install, checking BOTH possible locations:
'        a. `] dev`-installed: a FIXED directory (<depot>\dev\InteractiveGMT), no scanning needed.
'        b. `] add`-installed: a content-hashed folder (<depot>\packages\InteractiveGMT\<hash>)
'           that gets a NEW hash on every Pkg.update -- scan for the newest-modified one.
'      This is a plain filesystem check, not a Julia subprocess -- still only one julia.exe
'      launch total, never two.
'
' (An earlier version asked Julia itself, via a second julia.exe subprocess, to resolve the path
' on every launch -- correct, but doubled startup time. Don't reintroduce that.)
'
' Files dropped ONTO the desktop shortcut arrive here as WScript.Arguments; we forward each path
' to Julia, and iview_app.jl opens it (otherwise, with no args, it opens an empty launcher).
Dim fso : Set fso = CreateObject("Scripting.FileSystemObject")
Dim sh  : Set sh  = CreateObject("WScript.Shell")

Dim here : here = fso.GetParentFolderName(WScript.ScriptFullName)

Dim devPath, depot, diag, subFoldersScanned
devPath = "" : depot = "" : diag = ""
subFoldersScanned = 0

If Not fso.FileExists(here & "\iview_app.jl") Then
    ' Detached copy -- find the live InteractiveGMT install.
    '
    ' Depot root: respect JULIA_DEPOT_PATH if set (Julia checks this env var itself; a
    ' customized depot location is real, not hypothetical -- this same machine already turned
    ' out to have Desktop silently redirected by OneDrive). ExpandEnvironmentStrings returns the
    ' literal "%JULIA_DEPOT_PATH%" string unexpanded when the var isn't set, which is how we
    ' detect "not set" here. Windows uses ";" to separate multiple depot paths -- take the first.
    Dim depotRoot : depotRoot = sh.ExpandEnvironmentStrings("%JULIA_DEPOT_PATH%")
    If depotRoot = "%JULIA_DEPOT_PATH%" Then
        depotRoot = sh.ExpandEnvironmentStrings("%USERPROFILE%") & "\.julia"
    ElseIf InStr(depotRoot, ";") > 0 Then
        depotRoot = Left(depotRoot, InStr(depotRoot, ";") - 1)
    End If
    here = ""

    ' 1. `] dev` install -- fixed location, check directly first, no scanning.
    devPath = depotRoot & "\dev\InteractiveGMT"
    If fso.FileExists(devPath & "\iview_app.jl") Then
        here = devPath
    Else
        ' 2. `] add` install -- content-hashed depot, scan for the newest match.
        depot = depotRoot & "\packages\InteractiveGMT"
        If fso.FolderExists(depot) Then
            Dim subFolder, best
            best = -1
            For Each subFolder In fso.GetFolder(depot).SubFolders
                subFoldersScanned = subFoldersScanned + 1
                If fso.FileExists(subFolder.Path & "\iview_app.jl") Then
                    If CDbl(subFolder.DateLastModified) > best Then
                        best = CDbl(subFolder.DateLastModified)
                        here = subFolder.Path
                    End If
                End If
            Next
        End If
    End If
End If

If here = "" Then
    ' Report exactly what was checked instead of a bare "not found" -- no more guessing/back
    ' and forth needed to diagnose why.
    diag = "Checked dev install: " & devPath & " (not found)" & vbCrLf
    If Not fso.FolderExists(depot) Then
        diag = diag & "Checked add install: depot folder does not exist: " & depot
    Else
        diag = diag & "Checked add install: depot folder exists (" & depot & ")," & vbCrLf & _
               "scanned " & subFoldersScanned & " subfolder(s), none contained iview_app.jl."
    End If
    MsgBox "Could not locate InteractiveGMT." & vbCrLf & vbCrLf & diag & vbCrLf & vbCrLf & _
           "Is it added ('] add https://github.com/GenericMappingTools/InteractiveGMT') or dev'd in Julia's default environment?", _
           vbExclamation, "iGMT"
    WScript.Quit 1
End If

' --project=<here> ONLY if <here> has its OWN Manifest.toml (a `] dev`-instantiated checkout --
' this dev checkout has one). A plain `Pkg.add`-installed copy in the depot has NO Manifest of
' its own -- the resolved dependency graph lives only in your DEFAULT environment
' (~/.julia/environments/v1.10/), where `] add` put it. Pointing --project straight at a bare,
' Manifest-less package folder forces Julia into an empty/unresolved environment and it tries to
' re-resolve + precompile the entire dependency tree from scratch, which fails ("Failed to
' precompile InteractiveGMT..."). Launch bare (no --project) in that case instead, so Julia uses
' your normal default environment where `Pkg.add` already resolved everything correctly -- same
' as what a plain interactive `using InteractiveGMT` does.
Dim projectArg : projectArg = ""
If fso.FileExists(here & "\Manifest.toml") Then
    projectArg = "--project=" & Chr(34) & here & Chr(34) & " "
End If

Dim extra : extra = ""
Dim i
For i = 0 To WScript.Arguments.Count - 1
    extra = extra & " " & Chr(34) & WScript.Arguments(i) & Chr(34)
Next
sh.Run "julia " & projectArg & Chr(34) & here & "\iview_app.jl" & Chr(34) & extra, 7, False
