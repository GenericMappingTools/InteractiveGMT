' ONE-TIME SETUP for a `] add`/`Pkg.update` install (no NSIS): creates a real Desktop .lnk
' shortcut, with the iGMT icon, that survives every future Pkg.update. Run this once (from
' wherever InteractiveGMT currently lives — find it with `pathof(InteractiveGMT)` in Julia if
' unsure). Safe to re-run anytime; it just overwrites the same two files + shortcut.
'
' Why a plain shortcut-to-the-package-tree doesn't work: a .lnk's ICON path is read once, when
' the shortcut is opened — unlike iview_app.vbs's own launch target, which it re-resolves live
' via Julia every run, a .lnk can't dynamically look up its icon file, so an icon path pointing
' into the Pkg-hash-changing tree would eventually show a broken icon.
'
' Fix: copy the two files that need a truly permanent, unchanging home — iview_app.vbs (which
' re-resolves InteractiveGMT's actual location at every launch, so ITS content never goes stale)
' and igmt.ico (a static asset that doesn't change between versions) — to the Desktop ONCE, then
' point the shortcut at those local copies. After this, the Desktop icon never needs touching
' again, no matter how many times InteractiveGMT's Pkg-hash directory changes underneath it.
Dim fso : Set fso = CreateObject("Scripting.FileSystemObject")
Dim sh  : Set sh  = CreateObject("WScript.Shell")

Dim tmp : tmp = sh.ExpandEnvironmentStrings("%TEMP%") & "\igmt_locate_" & CStr(Timer) & ".jl"
Dim f : Set f = fso.CreateTextFile(tmp, True)
f.WriteLine "p = Base.find_package(" & Chr(34) & "InteractiveGMT" & Chr(34) & ")"
f.WriteLine "print(p === nothing ? " & Chr(34) & Chr(34) & " : dirname(dirname(p)))"
f.Close

Dim ex : Set ex = sh.Exec("julia --startup-file=no " & Chr(34) & tmp & Chr(34))
Dim pkgRoot : pkgRoot = Trim(ex.StdOut.ReadAll())
fso.DeleteFile tmp, True

If pkgRoot = "" Then
    MsgBox "Could not locate InteractiveGMT." & vbCrLf & _
           "Is it added ('] add https://github.com/GenericMappingTools/InteractiveGMT') or dev'd in Julia's default environment?", _
           vbExclamation, "iGMT"
    WScript.Quit 1
End If

' NEVER sh.SpecialFolders("Desktop") -- if Windows/OneDrive has Desktop redirected into OneDrive
' ("Known Folder Move", common on managed machines), that call silently returns the OneDrive path
' and everything below would get copied straight into cloud sync. Force the real local Desktop.
Dim desktop : desktop = sh.ExpandEnvironmentStrings("%USERPROFILE%") & "\Desktop"
fso.CopyFile pkgRoot & "\iview_app.vbs", desktop & "\iview_app.vbs", True
fso.CopyFile pkgRoot & "\deps\assets\igmt.ico", desktop & "\igmt.ico", True

Dim link : Set link = sh.CreateShortcut(desktop & "\iGMT.lnk")
link.TargetPath = desktop & "\iview_app.vbs"
link.IconLocation = desktop & "\igmt.ico"
link.WindowStyle = 7
link.Save

MsgBox "iGMT Desktop shortcut created (or refreshed). You can run this setup script again anytime -- it's idempotent.", vbInformation, "iGMT"
