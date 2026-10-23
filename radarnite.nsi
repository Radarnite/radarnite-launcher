; Radarnite Launcher installer (NSIS 3, Unicode). Per-user: no administrator needed to install.
; Build (tools/ci-build.sh on Linux/macOS, tools/build-release.ps1 on Windows):
;   makensis -DVERSION=2.2.1 [-DVERSION_NUM=2.2.1] -DSRC=<staging folder> -DOUT=<setup.exe> radarnite.nsi
;   VERSION_NUM = numeric X.Y.Z for the version resource (VERSION may carry a prerelease suffix); forward slashes in
;   paths so the same script builds with the Windows and the Unix makensis.
;   <staging>\app      the launcher exactly as it is installed (RadarniteLauncher.exe, ui\, node\, backend\, redirect\,
;                      gameserver\, tools\, THIRD-PARTY-NOTICES.txt)
;   <staging>\radarnite.ico

Unicode true
SetCompressor /SOLID lzma
SetCompressorDictSize 64
RequestExecutionLevel user
ManifestDPIAware true

!ifndef VERSION
  !define VERSION "2.2.1"
!endif
!ifndef VERSION_NUM
  !define VERSION_NUM "${VERSION}"
!endif
!define APP "Radarnite Launcher"
!define EXE "RadarniteLauncher.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\RadarniteLauncher"
!define APP_KEY "Software\Radarnite\Launcher"
!define WEBVIEW2_GUID "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

Name "${APP}"
OutFile "${OUT}"
InstallDir "$LOCALAPPDATA\Programs\Radarnite Launcher"
InstallDirRegKey HKCU "${APP_KEY}" "InstallDir"
BrandingText "${APP} ${VERSION}"
VIProductVersion "${VERSION_NUM}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "CompanyName" "Radarnite"
VIAddVersionKey "FileDescription" "${APP} setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Radarnite"

Var BuildsDir

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define MUI_ICON "${SRC}/radarnite.ico"
!define MUI_UNICON "${SRC}/radarnite.ico"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "Install ${APP}"
!define MUI_WELCOMEPAGE_TEXT "Play old Fortnite builds on this PC, offline, with your own local server.$\r$\n$\r$\nRadarnite never talks to Epic and never uses your Epic account. It does not need administrator rights to install.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME

!define MUI_PAGE_HEADER_TEXT "Program folder"
!define MUI_PAGE_HEADER_SUBTEXT "Where Radarnite Launcher itself is installed (about 500 MB)."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Radarnite Launcher will be installed in this folder."
!insertmacro MUI_PAGE_DIRECTORY

!define MUI_PAGE_HEADER_TEXT "Builds folder"
!define MUI_PAGE_HEADER_SUBTEXT "Where your Fortnite builds will live."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Fortnite builds are added from the launcher (from a downloaded zip or a folder) and each one takes 20 to 90 GB. Pick a drive with room for them. If you already have builds, pick the folder that contains them (one folder per build).$\r$\n$\r$\nYou can change it later in the launcher."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "Builds folder"
!define MUI_DIRECTORYPAGE_VARIABLE $BuildsDir
!define MUI_DIRECTORYPAGE_VERIFYONLEAVE
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Open Radarnite Launcher"
!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Create a desktop shortcut"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION DesktopShortcut
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ; 64-bit Windows 10 or later only (the launcher and the builds are x64)
  System::Call "kernel32::GetCurrentProcess()p.r0"
  System::Call "kernel32::IsWow64Process(pr0, *i0r1)"
  ${If} $1 == 0
    MessageBox MB_ICONSTOP "Radarnite Launcher needs 64-bit Windows 10 or 11."
    Abort
  ${EndIf}
  ; Builds folder: the one picked last time, else <drive with the most free space>\RadarniteBuilds
  ReadRegStr $BuildsDir HKCU "${APP_KEY}" "BuildsFolder"
  ${If} $BuildsDir == ""
    StrCpy $BuildsDir "$PROFILE\Radarnite Builds"
    StrCpy $3 0
    ${GetDrives} "HDD" "PickDrive"
  ${EndIf}
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/BUILDS=" $1
  ${IfNot} ${Errors}
    StrCpy $BuildsDir $1
  ${EndIf}
FunctionEnd

; GetDrives callback: remember the fixed drive with the most free space (in MB, $3).
Function PickDrive
  ${DriveSpace} $9 "/D=F /S=M" $0
  ${If} $0 > $3
    StrCpy $3 $0
    StrCpy $BuildsDir "$9RadarniteBuilds"
  ${EndIf}
  Push $0
FunctionEnd

Function DesktopShortcut
  CreateShortCut "$DESKTOP\${APP}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
FunctionEnd

; The launcher (or a game it started) is running from this folder: files would be locked.
!macro CheckRunning UN
Function ${UN}CheckRunning
  retry:
  ClearErrors
  ${If} ${FileExists} "$INSTDIR\${EXE}"
    FileOpen $0 "$INSTDIR\${EXE}" a
    ${If} ${Errors}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Radarnite Launcher is open. Close it (and the game, if it is running) and press Retry." IDRETRY retry
      Abort
    ${EndIf}
    FileClose $0
  ${EndIf}
FunctionEnd
!macroend
!insertmacro CheckRunning ""
!insertmacro CheckRunning "un."

Section "Radarnite Launcher" SecMain
  SectionIn RO
  Call CheckRunning
  SetOutPath "$INSTDIR"
  ; Program folders are fully replaced on every install (so a file removed between versions cannot linger); only
  ; config.json (name, account, settings) and the builds, which live elsewhere, stay.
  RMDir /r "$INSTDIR\ui"
  RMDir /r "$INSTDIR\tools"
  RMDir /r "$INSTDIR\gameserver"
  RMDir /r "$INSTDIR\backend"
  RMDir /r "$INSTDIR\redirect"
  RMDir /r "$INSTDIR\patches"
  File /r "${SRC}/app/*.*"
  File "${SRC}/radarnite.ico"

  ExecWait '"$INSTDIR\${EXE}" --set-builds-folder "$BuildsDir"' $0
  ${If} $0 != 0
    MessageBox MB_ICONEXCLAMATION "Could not set the builds folder to $BuildsDir. Pick it later in the launcher (bottom left)."
  ${EndIf}

  CreateDirectory "$SMPROGRAMS"
  CreateShortCut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0

  WriteUninstaller "$INSTDIR\Uninstall Radarnite Launcher.exe"
  WriteRegStr HKCU "${APP_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${APP_KEY}" "BuildsFolder" "$BuildsDir"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "Radarnite"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall Radarnite Launcher.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall Radarnite Launcher.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"

  ; The window is drawn by Microsoft Edge WebView2 (built into Windows 11 and up-to-date Windows 10).
  ReadRegStr $0 HKLM "SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_GUID}" "pv"
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    ReadRegStr $0 HKCU "Software\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_GUID}" "pv"
  ${EndIf}
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    IfSilent +3
    MessageBox MB_YESNO|MB_ICONINFORMATION "Radarnite Launcher needs Microsoft Edge WebView2 Runtime, which is not on this PC.$\r$\n$\r$\nOpen Microsoft's download page now?" IDNO +2
    ExecShell "open" "https://go.microsoft.com/fwlink/p/?LinkId=2124703"
  ${EndIf}
SectionEnd

Section "Uninstall"
  Call un.CheckRunning
  ; Firewall rules are only there if they were created from the launcher (that needed administrator too).
  IfSilent +3
  MessageBox MB_YESNO|MB_ICONQUESTION "Also remove the Windows Firewall rules Radarnite Launcher created?$\r$\n(Windows asks for administrator rights.)" IDNO +2
  ExecShellWait "runas" "$INSTDIR\${EXE}" "--firewall-remove" SW_HIDE

  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$DESKTOP\${APP}.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegValue HKCU "${APP_KEY}" "InstallDir"
  ; BuildsFolder stays in the registry so a reinstall offers the same folder; the builds themselves are not removed.
  IfSilent +2
  MessageBox MB_ICONINFORMATION "Radarnite Launcher was removed. Your Fortnite builds were not deleted."
SectionEnd
