# fnkeys-test.ps1 - fnkeys-test.exe twice in the signed-in user's session: "self", then "held" against an AutoHotkey v2
# script made like the one that took F8, F9 and F10 from the game (an InputHook running, a "~" hotkey, a plain hotkey
# outside any #HotIf, which AutoHotkey registers with RegisterHotKey), on F14 instead: no usual keyboard has it, so the
# user loses no key. The script has no tray icon and is stopped in the end. Developer test, run by hand (just test-fnkeys).
#   powershell -File fnkeys-test.ps1 -Exe <fnkeys-test.exe> [-Ahk <AutoHotkey64.exe>]
# Hotkeys need the signed-in session's window station (RegisterHotKey fails with 1459 in an ssh session's): run from
# session 0, it starts itself there hidden (a scheduled task with no window: nothing takes the focus) and prints what that
# run wrote; exit 0 = passed.
param([Parameter(Mandatory)][string]$Exe, [string]$Out = '',
      [string]$Ahk = "$env:ProgramFiles\AutoHotkey\v2\AutoHotkey64.exe")
$ErrorActionPreference = 'Stop'

if ((Get-Process -Id $PID).SessionId -eq 0) {
    $res = Join-Path (Split-Path $Exe) 'fnkeys-result.txt'
    Remove-Item $res -ErrorAction SilentlyContinue
    $vbs = Join-Path (Split-Path $Exe) 'fnkeys-hidden.vbs'
    Set-Content -Encoding ascii $vbs ('CreateObject("WScript.Shell").Run "powershell.exe -NoProfile -ExecutionPolicy Bypass ' +
        '-WindowStyle Hidden -File """ & WScript.Arguments(0) & """ -Exe """ & WScript.Arguments(1) & """ -Out """ & ' +
        'WScript.Arguments(2) & """ -Ahk """ & WScript.Arguments(3) & """", 0, True')
    $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $a = New-ScheduledTaskAction -Execute 'wscript.exe' -Argument "`"$vbs`" `"$PSCommandPath`" `"$Exe`" `"$res`" `"$Ahk`""
    $p = New-ScheduledTaskPrincipal -UserId $sid -LogonType Interactive
    Register-ScheduledTask -TaskName 'rl-test-fnkeys' -Action $a -Principal $p -Force | Out-Null
    try {
        Start-ScheduledTask -TaskName 'rl-test-fnkeys'
        for ($i = 0; $i -lt 60 -and !(Test-Path $res); $i++) { Start-Sleep 1 }
    } finally {
        Unregister-ScheduledTask -TaskName 'rl-test-fnkeys' -Confirm:$false
    }
    if (!(Test-Path $res)) { 'no result after 60 s'; exit 1 }
    $r = Get-Content $res
    $r
    if ($r[-1] -ne 'PASSED') { exit 1 }
    exit 0
}

$log = @()
try {
    $log += & $Exe self; $self = $LASTEXITCODE
    $ahkFile = Join-Path ([IO.Path]::GetTempPath()) "fnkeys-test-$PID.ahk"
    Set-Content -Encoding utf8 $ahkFile @'
#Requires AutoHotkey v2.0
#SingleInstance Off
#NoTrayIcon
Persistent
global ih := InputHook("V I1")
ih.KeyOpt("{All}", "N")
ih.Start()
~F15::return
F14::return
'@
    $p = Start-Process $Ahk -ArgumentList "`"$ahkFile`"" -PassThru
    try {
        Start-Sleep -Milliseconds 1500
        $log += & $Exe held; $held = $LASTEXITCODE
    } finally {
        if (!$p.HasExited) { Stop-Process -Id $p.Id -Force }
        Remove-Item $ahkFile -ErrorAction SilentlyContinue
    }
    $log += "self: $self, held: $held"
    $log += if ($self -or $held) { 'FAILED' } else { 'PASSED' }
} catch {
    $log += "ERROR: $_"
}
Set-Content -Encoding utf8 $Out $log
