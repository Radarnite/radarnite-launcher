# Runs RadarniteUI.exe --selftest-ui in the signed-in user's session (the window is cloaked: never visible), waits for
# selftest.json, removes the scheduled task and any leftover selftest process (only this exe path), prints the JSON.
# Over ssh everything runs in session 0 (no desktop: no WM_PAINT, no GPU), so the real first-frame/RAM/CPU numbers come
# from here. Usage on the PC: powershell -NoProfile -ExecutionPolicy Bypass -File selftest-session.ps1 [-Dir S:\native-ui-test] [-UiArgs '--render hw']
param([string]$Dir = 'S:\native-ui-test', [string]$UiArgs = '')
$ErrorActionPreference = 'Stop'
$dir = $Dir
$out = "$dir\out"; $exe = "$dir\RadarniteUI.exe"; $name = 'rn-native-ui-selftest'
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
$sid = ([System.Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
$a = New-ScheduledTaskAction -Execute $exe -Argument "--selftest-ui $out $UiArgs" -WorkingDirectory $dir
$p = New-ScheduledTaskPrincipal -UserId $sid -LogonType Interactive
Register-ScheduledTask -TaskName $name -Action $a -Principal $p -Force | Out-Null
try {
    Start-ScheduledTask -TaskName $name
    $t0 = Get-Date
    do { Start-Sleep -Milliseconds 500 } while (-not (Test-Path "$out\selftest.json") -and ((Get-Date) - $t0).TotalSeconds -lt 90)
    Start-Sleep -Seconds 1
} finally {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false
    Get-CimInstance Win32_Process -Filter "Name='RadarniteUI.exe'" | Where-Object { $_.ExecutablePath -eq $exe } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force; Write-Output "stopped leftover selftest process $($_.ProcessId)" }
}
if (Test-Path "$out\selftest.json") { Get-Content "$out\selftest.json" } else { Write-Output 'no selftest.json'; exit 1 }
