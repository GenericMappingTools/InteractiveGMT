' iGMT installed launcher (NSIS-installed tree only — NOT the dev iview_app.vbs).
' Self-locating: finds its own folder at runtime so the Desktop/Start Menu shortcut works no
' matter what $INSTDIR the user picked. Calls `julia` bare (from PATH) with --project pointed at
' this same installed folder, running the SAME iview_app.jl the dev launcher uses. Requires
' Julia + this package's deps (GMT.jl, ...) already installed/instantiated on the destination
' machine — the installer does not bundle or bootstrap Julia itself.
'
' Started MINIMIZED (style 7), not hidden: see iview_app.vbs for why (SW_HIDE would hide the Qt
' window too). Files dropped onto the shortcut are forwarded as arguments, same as the dev vbs.
Dim fso : Set fso = CreateObject("Scripting.FileSystemObject")
Dim here : here = fso.GetParentFolderName(WScript.ScriptFullName)
Dim sh : Set sh = CreateObject("WScript.Shell")
' Splash: launched IMMEDIATELY via mshta (no Julia/Qt startup cost) so the click gets instant
' feedback while Julia + package load/precompile happen in the background. Self-closing:
' iview_app.jl drops a ready-flag once its window is up; the HTA polls for it and closes
' itself (see iview_splash.hta). Clear any stale flag from a previous run first.
On Error Resume Next
Dim flagPath : flagPath = sh.ExpandEnvironmentStrings("%TEMP%") & "\igmt_ready.flag"
If fso.FileExists(flagPath) Then fso.DeleteFile flagPath, True
On Error Goto 0
If fso.FileExists(here & "\iview_splash.hta") Then
    sh.Run "mshta.exe """ & here & "\iview_splash.hta""", 1, False
End If

Dim extra : extra = ""
Dim i
For i = 0 To WScript.Arguments.Count - 1
    extra = extra & " """ & WScript.Arguments(i) & """"
Next
sh.Run "julia --project=""" & here & """ """ & here & "\iview_app.jl""" & extra, 7, False
