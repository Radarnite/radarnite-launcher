; Radarnite Launcher installer (NSIS 3, Unicode). Per-user: no administrator needed to install.
; Build (tools/release.sh does it): makensis -DVERSION=3.0.0-rc.1 -DVERSION_NUM=3.0.0 -DSRC=<staging> -DOUT=<setup.exe> radarnite.nsi
;   <staging>/app      the launcher exactly as it is installed (RadarniteLauncher.exe, node\, backend\, redirect\,
;                      gameserver\, patches\, tools\, THIRD-PARTY-NOTICES.txt, README-offline.md)
;   <staging>/radarnite.ico
; 3.x replaces a 2.x install in place: same folder, Start menu entry, registry keys, uninstall key and firewall group
; ("FN Offline Builds", managed by the launcher). config.json and the logs stay; the 2.x runtime files go
; (radarnite-2x-leftovers.nsh).

Unicode true
SetCompressor /SOLID lzma
SetCompressorDictSize 64
RequestExecutionLevel user
ManifestDPIAware true

!ifndef VERSION
  !define VERSION "3.0.0"
!endif
!ifndef VERSION_NUM
  !define VERSION_NUM "${VERSION}"   ; numeric X.Y.Z for the version resource (VERSION may carry -rc.N)
!endif
!define APP "Radarnite Launcher"
!define EXE "RadarniteLauncher.exe"
!define BACKEND "radarnite-backend.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\RadarniteLauncher"
!define APP_KEY "Software\Radarnite\Launcher"

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
!define MUI_PAGE_HEADER_SUBTEXT "Where Radarnite Launcher itself is installed (about 130 MB)."
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
!define MUI_PAGE_CUSTOMFUNCTION_SHOW FinishShow
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

; A silent update started by the launcher (its test channel: /S /RELAUNCH=fg|bg) opens it again once installed;
; bg = the window was not in use, so it comes back minimized without taking the focus.
Function .onInstSuccess
  IfSilent 0 done
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/RELAUNCH=" $1
  IfErrors done
  ${If} $1 != "bg"
    StrCpy $1 "fg"
  ${EndIf}
  Exec '"$INSTDIR\${EXE}" --updated $1'
  done:
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

; The desktop shortcut is the installer's only when it made it (DesktopShortcut=1): one the user already has (it may
; point somewhere else) is neither overwritten by default nor deleted by the uninstaller.
Function DesktopShortcut
  CreateShortCut "$DESKTOP\${APP}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
  WriteRegDWORD HKCU "${APP_KEY}" "DesktopShortcut" 1
FunctionEnd

Function FinishShow
  ReadRegDWORD $0 HKCU "${APP_KEY}" "DesktopShortcut"
  ${If} ${FileExists} "$DESKTOP\${APP}.lnk"
  ${AndIf} $0 != 1
    SendMessage $mui.FinishPage.ShowReadme ${BM_SETCHECK} ${BST_UNCHECKED} 0
  ${EndIf}
FunctionEnd

; A folder that cannot go (a file in it is in use) is reported and left for the next update; the install goes on.
!macro TolerantRMDir DIR
  RMDir /r "${DIR}"
  ${If} ${FileExists} "${DIR}\*.*"
    DetailPrint "Kept (in use, removed on the next update): ${DIR}"
  ${EndIf}
!macroend
!include "radarnite-2x-leftovers.nsh"

; The launcher's own processes (its exe, backend, tools, the 2.x node.exe and WebView2 window) and only those:
; tools/installer-own-processes.ps1 with RN_MODE=wait|kill; $0 = how many are still running, "error" if it could not run.
!macro OwnProcesses UN
Function ${UN}OwnProcesses
  Exch $R0
  InitPluginsDir
  File "/oname=$PLUGINSDIR\own-processes.ps1" "tools/installer-own-processes.ps1"
  System::Call 'kernel32::SetEnvironmentVariable(t "RN_INSTDIR", t "$INSTDIR")'
  System::Call 'kernel32::SetEnvironmentVariable(t "RN_MODE", t "$R0")'
  StrCpy $R0 "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"   ; 64-bit PowerShell from this 32-bit installer
  ${IfNot} ${FileExists} $R0
    StrCpy $R0 "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"
  ${EndIf}
  nsExec::Exec '"$R0" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\own-processes.ps1"'
  Pop $0
  Pop $R0
FunctionEnd

; Before files are replaced: an updating launcher closes itself right after starting this installer, so wait for it;
; whatever of ours is still running is closed (after asking, unless silent). Files still locked after that (a game
; holding the launcher's DLLs, or no PowerShell) get the Retry/Cancel prompt; games are never closed from here.
Function ${UN}CheckRunning
  Push "wait"
  Call ${UN}OwnProcesses
  ${If} $0 != "error"
  ${AndIf} $0 != 0
    MessageBox MB_YESNO|MB_ICONEXCLAMATION "Radarnite Launcher is still running.$\r$\n$\r$\nClose it now and continue?" /SD IDYES IDYES +2
    Abort
    Push "kill"
    Call ${UN}OwnProcesses
    ${If} $0 != "error"
    ${AndIf} $0 != 0
      DetailPrint "$0 Radarnite Launcher process(es) did not close"
    ${EndIf}
  ${EndIf}
  retry:
  ClearErrors
  ${ForEach} $1 0 6 + 1
    ${Select} $1
      ${Case} 0
        StrCpy $2 "$INSTDIR\${EXE}"
      ${Case} 1
        StrCpy $2 "$INSTDIR\${BACKEND}"
      ${Case} 2
        StrCpy $2 "$INSTDIR\redirect\fnredirect.dll"
      ${Case} 3
        StrCpy $2 "$INSTDIR\gameserver\radarnite-server.dll"
      ${Case} 4
        StrCpy $2 "$INSTDIR\patches\fnpakpatch.dll"
      ${Case} 5
        StrCpy $2 "$INSTDIR\node\node.exe"
      ${Case} 6
        StrCpy $2 "$INSTDIR\patches\fnbody.dll"
    ${EndSelect}
    ${If} ${FileExists} $2
      FileOpen $0 $2 a
      ${If} ${Errors}
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "A Radarnite Launcher file is in use:$\r$\n$2$\r$\n$\r$\nClose Radarnite Launcher and the game, if it is running, and press Retry." /SD IDCANCEL IDRETRY retry
        Abort
      ${EndIf}
      FileClose $0
    ${EndIf}
  ${Next}
FunctionEnd
!macroend
!insertmacro OwnProcesses ""
!insertmacro OwnProcesses "un."

Section "Radarnite Launcher" SecMain
  SectionIn RO
  Call CheckRunning
  SetOutPath "$INSTDIR"
  ; Only over an existing Radarnite Launcher (2.x or 3.x): a folder picked by hand that holds other files is never
  ; cleaned. Upgrading from 2.x: its .NET runtime, WebView2 page and profile go; Node and server.mjs are installed again. Wildcards catch
  ; the runtime files of other 2.x versions; 3.x has no dll in the program folder root (tools/release.sh enforces it).
  ; Program folders are fully replaced on every install (so a file removed between versions cannot linger); only
  ; config.json (name, account, settings), the logs and the builds, which live elsewhere, stay.
  ${If} ${FileExists} "$INSTDIR\${EXE}"
    !insertmacro Remove2xLeftovers
    Delete "$INSTDIR\*.dll"
    Delete "$INSTDIR\*.deps.json"
    Delete "$INSTDIR\*.runtimeconfig.json"
    Delete "$INSTDIR\${BACKEND}"   ; 3.0 release candidates' backend: the Node one (node\, backend\) runs instead
    !insertmacro TolerantRMDir "$INSTDIR\tools"
    !insertmacro TolerantRMDir "$INSTDIR\gameserver"
    !insertmacro TolerantRMDir "$INSTDIR\redirect"
    !insertmacro TolerantRMDir "$INSTDIR\patches"
  ${EndIf}
  ClearErrors
  File /r "${SRC}/app/*.*"

  ExecWait '"$INSTDIR\${EXE}" --set-builds-folder "$BuildsDir"' $0
  ${If} $0 != 0
    MessageBox MB_ICONEXCLAMATION "Could not set the builds folder to $BuildsDir. Pick it later in the launcher (bottom left)." /SD IDOK
  ${EndIf}

  ; The pak tools (sigfit, game guard) just installed must be the ones this launcher was built with, and a dev tree's
  ; copy (<builds folder>\launcher\tools, used by scripts outside the launcher) is replaced by them, so no older sigfit
  ; survives an update. A mismatch is written to update.log (silent updates) and shown here; the launcher says it
  ; again before running a sigfit.
  ExecWait '"$INSTDIR\${EXE}" --sync-pak-tools' $0
  ${If} $0 != 0
    DetailPrint "Pak tools do not match this launcher (see update.log)"
    MessageBox MB_ICONEXCLAMATION "The pak tools (sigfit) in $INSTDIR\tools or $BuildsDir\launcher\tools do not match this launcher. Details in $INSTDIR\update.log. Install the launcher again; until then own skins may stop the game with $\"Corrupted data$\"." /SD IDOK
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
SectionEnd

Section "Uninstall"
  Call un.CheckRunning
  ; Firewall rules are only there if they were created from the launcher (that needed administrator too).
  IfSilent +3
  MessageBox MB_YESNO|MB_ICONQUESTION "Also remove the Windows Firewall rules Radarnite Launcher created?$\r$\n(Windows asks for administrator rights.)" IDNO +2
  ExecShellWait "runas" "$INSTDIR\${EXE}" "--firewall-remove" SW_HIDE

  Delete "$SMPROGRAMS\${APP}.lnk"
  ReadRegDWORD $0 HKCU "${APP_KEY}" "DesktopShortcut"
  ${If} $0 == 1
    Delete "$DESKTOP\${APP}.lnk"
    DeleteRegValue HKCU "${APP_KEY}" "DesktopShortcut"
  ${EndIf}
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegValue HKCU "${APP_KEY}" "InstallDir"
  ; BuildsFolder stays in the registry so a reinstall offers the same folder; the builds themselves are not removed.
  IfSilent +2
  MessageBox MB_ICONINFORMATION "Radarnite Launcher was removed. Your Fortnite builds were not deleted."
SectionEnd
