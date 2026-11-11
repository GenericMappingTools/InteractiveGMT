; igmt.nsi -- Windows installer for iGMT: Julia + GMT + GMT.jl + InteractiveGMT.
;
; Built by make_installer.ps1, which stages GMT and passes the paths:
;   makensis /DJULIA_SRC=C:\programs\Julia-1.14 /DSTAGE=<deps\build\installer_stage> /DBUILD=<deps\build> igmt.nsi
;
; Layout under $INSTDIR (per-user, no admin -- everything here is written at run time):
;   julia\              the bundled Julia, copied as is
;   gmt\bin, gmt\share  the staged GMT tree
;   depot\              private JULIA_DEPOT_PATH -- the user's own ~/.julia is never touched.
;                       InteractiveGMT is a git checkout in depot\dev\InteractiveGMT
;   env\                Project/Manifest made by make_env.jl, instantiated by setup.jl
;   igmt.exe            the launcher; igmt_bundle.ini beside it (written by setup.jl) gives it the
;                       julia, root and environment above
;
; The packages are NOT in the installer: setup.jl fetches GMT.jl + InteractiveGMT (the versions
; pinned in env\Manifest.toml) and precompiles them AT THEIR FINAL PATH, so the caches are valid on
; first launch.

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"

!ifndef JULIA_SRC
  !define JULIA_SRC "C:\programs\Julia-1.14"
!endif
!ifndef STAGE
  !error "STAGE not defined -- run make_installer.ps1"
!endif
!ifndef BUILD
  !error "BUILD not defined -- run make_installer.ps1"
!endif

!define APPNAME   "iGMT"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\iGMT"

Name "${APPNAME}"
OutFile "${BUILD}\iGMT-setup-win64.exe"
; A visible, plain folder (not the hidden %LOCALAPPDATA%\Programs). No InstallDirRegKey: an earlier
; install's folder must not replace this default on the directory page.
InstallDir "C:\Programs\iGMT"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

!define MUI_ICON   "${__FILEDIR__}\..\assets\igmt.ico"
!define MUI_UNICON "${__FILEDIR__}\..\assets\igmt.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\igmt.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start iGMT"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "iGMT" SecMain
  ; Installing over an earlier install: its folders go first, so nothing an earlier version or run
  ; left there (packages, compiled caches) survives. Only the installer's own subfolders.
  RMDir /r "$INSTDIR\julia"
  RMDir /r "$INSTDIR\gmt"
  RMDir /r "$INSTDIR\depot"
  RMDir /r "$INSTDIR\env"

  SetOutPath "$INSTDIR\julia"
  File /r "${JULIA_SRC}\*.*"

  SetOutPath "$INSTDIR\gmt"
  File /r "${STAGE}\gmt\*.*"

  SetOutPath "$INSTDIR"
  File "${BUILD}\igmt.exe"
  ; igmt.exe links the VC++ runtime; a clean machine may not have the redistributable.
  File "${STAGE}\gmt\bin\vcruntime140.dll"
  File "${STAGE}\gmt\bin\vcruntime140_1.dll"
  File "${__FILEDIR__}\..\assets\igmt.ico"
  File "${__FILEDIR__}\setup.jl"
  File "${__FILEDIR__}\..\..\src\gmtjl_strip.jl"
  File "${__FILEDIR__}\julia.bat"

  SetOutPath "$INSTDIR\env"
  File "${STAGE}\env\Project.toml"
  File "${STAGE}\env\Manifest.toml"
  File "${STAGE}\env\igmt_commit.txt"

  ; GMT.jl itself: a git clone, at the commit env\Manifest.toml was made with, Project.toml without
  ; the JLLs (ship_gmtjl.jl). The Manifest has it as a `dev` path, so Pkg never downloads it nor
  ; its JLLs. setup.jl builds and precompiles it into this depot; iGMT's update fast-forwards it.
  SetOutPath "$INSTDIR\depot\dev\GMT"
  File /r "${STAGE}\depot\dev\GMT\*.*"
  SetOutPath "$INSTDIR"

  ; The private depot, set before julia starts. The trailing ';' appends Julia's own bundled
  ; depots (stdlib caches) after it. setup.jl sets the rest of the environment itself.
  System::Call 'Kernel32::SetEnvironmentVariable(t "JULIA_DEPOT_PATH", t "$INSTDIR\depot;")i'

  DetailPrint "Installing GMT.jl and InteractiveGMT: download and precompile, this takes a while..."
  nsExec::ExecToLog '"$INSTDIR\julia\bin\julia.exe" --startup-file=no --history-file=no "$INSTDIR\setup.jl" "$INSTDIR"'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_ICONSTOP|MB_OK "Installing the Julia packages failed (exit code $0).$\n$\nThe details above show where it stopped." /SD IDOK
    Abort
  ${EndIf}

  ; Same name the launcher's --install-shortcut uses, so an older install's i'GMT icon is
  ; replaced by this one, not left beside it.
  Delete "$DESKTOP\iGMT.lnk"
  CreateShortcut "$DESKTOP\i'GMT.lnk" "$INSTDIR\igmt.exe" "" "$INSTDIR\igmt.ico"
  CreateDirectory "$SMPROGRAMS\iGMT"
  CreateShortcut "$SMPROGRAMS\iGMT\iGMT.lnk" "$INSTDIR\igmt.exe" "" "$INSTDIR\igmt.ico"
  CreateShortcut "$SMPROGRAMS\iGMT\Uninstall iGMT.lnk" "$INSTDIR\uninstall.exe"
  ; Julia on this install's depot (julia.bat sets JULIA_DEPOT_PATH), with Julia's own icon
  CreateShortcut "$DESKTOP\Julia (iGMT).lnk" "$INSTDIR\julia.bat" "" "$INSTDIR\julia\bin\julia.exe" 0
  CreateShortcut "$SMPROGRAMS\iGMT\Julia (iGMT).lnk" "$INSTDIR\julia.bat" "" "$INSTDIR\julia\bin\julia.exe" 0

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "Software\iGMT" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "iGMT"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\igmt.ico"
  WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "GenericMappingTools"
  WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
SectionEnd

; Removes only what the installer put there -- never RMDir /r $INSTDIR, which is whatever folder
; the user typed on the directory page.
Section "Uninstall"
  Delete "$DESKTOP\i'GMT.lnk"
  Delete "$DESKTOP\iGMT.lnk"
  Delete "$DESKTOP\Julia (iGMT).lnk"
  RMDir /r "$SMPROGRAMS\iGMT"
  RMDir /r "$INSTDIR\julia"
  RMDir /r "$INSTDIR\gmt"
  RMDir /r "$INSTDIR\depot"
  RMDir /r "$INSTDIR\env"
  Delete "$INSTDIR\igmt.exe"
  Delete "$INSTDIR\igmt_bundle.ini"
  Delete "$INSTDIR\setup.log"
  Delete "$INSTDIR\vcruntime140.dll"
  Delete "$INSTDIR\vcruntime140_1.dll"
  Delete "$INSTDIR\igmt.ico"
  Delete "$INSTDIR\setup.jl"
  Delete "$INSTDIR\gmtjl_strip.jl"
  Delete "$INSTDIR\julia.bat"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "Software\iGMT"
SectionEnd
