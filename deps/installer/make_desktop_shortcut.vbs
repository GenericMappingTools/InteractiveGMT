' Creates a real Desktop .lnk shortcut, with the iGMT icon. Two ways to run:
'   1. From deps/build.jl, automatically, on every `Pkg.build("InteractiveGMT")` -- passes the
'      package root as WScript.Arguments(0), so no Julia lookup subprocess needed and no
'      interactive MsgBox popups (a build should never block on a dialog).
'   2. Standalone, by hand, with no arguments -- looks up the current InteractiveGMT install via
'      Julia (Base.find_package) and reports success/failure with a MsgBox.
'
' EXACTLY ONE icon, always. It points STRAIGHT at pkgRoot's own iview_app.vbs and igmt.ico -- we
' NEVER copy those files to the Desktop and NEVER point the shortcut at a Desktop copy. (An earlier
' version had a second "copy to Desktop for `] add` installs" branch; it littered the Desktop with
' iview_app.vbs + igmt.ico and pointed the .lnk at desktop\iview_app.vbs -- forbidden. Removed.)
' iview_app.vbs is already self-locating, so even for a hash-changing `] add` install the direct
' target stays valid enough; the icon path is the only thing that could go stale, and that is an
' acceptable trade against ever cluttering the Desktop.
Dim fso : Set fso = CreateObject("Scripting.FileSystemObject")
Dim sh  : Set sh  = CreateObject("WScript.Shell")

Dim calledFromBuild : calledFromBuild = (WScript.Arguments.Count > 0)
Dim pkgRoot

If calledFromBuild Then
    pkgRoot = WScript.Arguments(0)
Else
    Dim tmp : tmp = sh.ExpandEnvironmentStrings("%TEMP%") & "\igmt_locate_" & CStr(Timer) & ".jl"
    Dim f : Set f = fso.CreateTextFile(tmp, True)
    f.WriteLine "p = Base.find_package(" & Chr(34) & "InteractiveGMT" & Chr(34) & ")"
    f.WriteLine "print(p === nothing ? " & Chr(34) & Chr(34) & " : dirname(dirname(p)))"
    f.Close

    Dim ex : Set ex = sh.Exec("julia --startup-file=no " & Chr(34) & tmp & Chr(34))
    pkgRoot = Trim(ex.StdOut.ReadAll())
    fso.DeleteFile tmp, True

    If pkgRoot = "" Then
        MsgBox "Could not locate InteractiveGMT." & vbCrLf & _
               "Is it added ('] add https://github.com/GenericMappingTools/InteractiveGMT') or dev'd in Julia's default environment?", _
               vbExclamation, "iGMT"
        WScript.Quit 1
    End If
End If

' Use sh.SpecialFolders("Desktop") -- the ACTUAL desktop as Windows resolves it for this user,
' whatever folder that is on this particular machine. A hardcoded "%USERPROFILE%\Desktop" is wrong:
' on a machine where the shell's Desktop is set to a different folder, %USERPROFILE%\Desktop is an
' empty, NON-displayed folder, so the .lnk lands there and the user sees no icon at all -- looks
' exactly like "no shortcut was created". SpecialFolders always returns the one folder whose
' contents actually appear on screen, so the icon shows up next to the user's other shortcuts.
Dim desktop : desktop = sh.SpecialFolders("Desktop")

' EXACTLY ONE icon, and it points STRAIGHT at the real source files in pkgRoot -- never a copy on
' the Desktop, never a shortcut whose target is desktop\iview_app.vbs. (An earlier version copied
' the .vbs+.ico to the Desktop for `add` installs and pointed the .lnk at those copies; that is
' forbidden -- it littered the Desktop with extra files and a wrong-target shortcut.) So: delete
' any stray copies a previous run may have dropped, then create the single .lnk -> source.
If fso.FileExists(desktop & "\iview_app.vbs") Then fso.DeleteFile desktop & "\iview_app.vbs", True
If fso.FileExists(desktop & "\igmt.ico") Then fso.DeleteFile desktop & "\igmt.ico", True

' Normalize any forward slashes -- callers sometimes pass a "/"-separated pkgRoot, and VBScript's
' GetParentFolderName/GetFileName only ever split on "\". A "/"-path would otherwise build broken
' targets like ".../InteractiveGMT/iview_app.vbs" that the rest of this script mishandles.
pkgRoot = Replace(pkgRoot, "/", "\")
If Right(pkgRoot, 1) = "\" Then pkgRoot = Left(pkgRoot, Len(pkgRoot) - 1)

Dim link : Set link = sh.CreateShortcut(desktop & "\iGMT.lnk")
link.TargetPath   = pkgRoot & "\iview_app.vbs"
link.IconLocation = pkgRoot & "\igmt.ico"
link.WindowStyle  = 7
link.Save

If Not calledFromBuild Then
    MsgBox "iGMT Desktop shortcut created (or refreshed). You can run this setup script again anytime -- it's idempotent.", vbInformation, "iGMT"
End If
