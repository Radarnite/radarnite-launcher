# Runs radarnite-play.exe --selftest-audio (play/Program.cs SelfTestAudio) in the signed-in user's session, where the
# audio endpoints are (over ssh everything runs in session 0), waits for its result, removes the scheduled task and any
# leftover process of this exe path only, prints the output. Nothing audible: the test player plays digital silence.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File selftest-audio.ps1 -Dir S:\rl-integ-test\audio
param([string]$Dir = 'S:\rl-integ-test\audio')
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Dir 'radarnite-play.exe'; $txt = "$Dir\audio.txt"; $done = "$Dir\audio.done"; $cmd = "$Dir\selftest-audio.cmd"
$name = 'rn-play-audio-selftest'
Remove-Item -Force $txt, $done -ErrorAction SilentlyContinue
# a batch file waits for the (GUI subsystem) exe and keeps its exit code
Set-Content -Encoding ascii $cmd "@`"$exe`" --selftest-audio > `"$txt`" 2>&1`r`n@> `"$done`" echo %ERRORLEVEL%"
$sid = ([System.Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
$a = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"$cmd`"" -WorkingDirectory $Dir
$p = New-ScheduledTaskPrincipal -UserId $sid -LogonType Interactive
Register-ScheduledTask -TaskName $name -Action $a -Principal $p -Force | Out-Null
try {
    Start-ScheduledTask -TaskName $name
    $t0 = Get-Date
    do { Start-Sleep -Milliseconds 500 } while (-not (Test-Path $done) -and ((Get-Date) - $t0).TotalSeconds -lt 150)
} finally {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false
    Get-CimInstance Win32_Process -Filter "Name='radarnite-play.exe'" | Where-Object { $_.ExecutablePath -eq $exe } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force; Write-Output "stopped leftover selftest process $($_.ProcessId)" }
}
if (Test-Path $txt) { Get-Content $txt }
if (-not (Test-Path $done)) { Write-Output 'no result (timed out)'; exit 1 }
Start-Sleep -Milliseconds 500
$code = "$(Get-Content -Raw $done)".Trim()
Write-Output "exit code $code"
if ($code -ne '0') { exit 1 }
