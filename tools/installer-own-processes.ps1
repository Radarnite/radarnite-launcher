# installer-own-processes.ps1 - used by radarnite.nsi (copied to $PLUGINSDIR at install time). Finds the processes
# that belong to one Radarnite Launcher install and nothing else: every process whose image is under the install
# folder (launcher, backend, tools, the 2.x node.exe) and the WebView2 processes of the 2.x window (msedgewebview2.exe
# with that folder's webview2-radarnite profile). Games run from the builds folder and are never touched.
#   env RN_INSTDIR = install folder; RN_MODE = wait (wait up to 15 s for them to exit) | kill (end them, wait up to 5 s)
#   exit code = how many are still running (0 = none)
$ErrorActionPreference = 'Stop'
$dir = $env:RN_INSTDIR.TrimEnd('\') + '\'
$profileDir = $dir + 'webview2-radarnite'
# never this script nor the installer/uninstaller that started it (an uninstaller run with _?= lives in the folder)
$self = @($PID, (Get-CimInstance Win32_Process -Filter "ProcessId=$PID").ParentProcessId)
function Own {
    @(Get-CimInstance Win32_Process | Where-Object {
        $self -notcontains $_.ProcessId -and (
        ($_.ExecutablePath -and $_.ExecutablePath.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase)) -or
        ($_.Name -eq 'msedgewebview2.exe' -and $_.CommandLine -and $_.CommandLine.IndexOf($profileDir, [StringComparison]::OrdinalIgnoreCase) -ge 0)) })
}
$seconds = 15
if ($env:RN_MODE -eq 'kill') {
    $seconds = 5
    foreach ($p in Own) { try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop } catch { } }
}
$until = (Get-Date).AddSeconds($seconds)
do {
    $left = (Own).Count
    if ($left -eq 0) { break }
    Start-Sleep -Milliseconds 400
} while ((Get-Date) -lt $until)
exit $left
