# Renders the page scenarios with Chromium (tests/ui-scenarios.mjs unchanged + page-extra.mjs) and compares them with
# the native selftest PNGs in <Dir>\out (from `just run-pc`). Node + Playwright portable from %LOCALAPPDATA%\fp-tools,
# one browser at a time, closed in finally; every node this script starts is killed with its process tree at the end
# or after the timeout. Results in <Dir>\cmp (PNGs <= 1280x720 + compare.json).
# Usage on the PC: powershell -NoProfile -ExecutionPolicy Bypass -File compare-pc.ps1 [-Dir S:\native-ui-test] [-Fresh] [-Crops "id:x,y,w,h,zoom;..."]
param([string]$Dir = 'S:\native-ui-test', [switch]$Fresh, [int]$TimeoutSec = 300, [string]$Crops = '')
$ErrorActionPreference = 'Continue'   # taskkill writes to stderr when there is nothing left to kill
$tools = "$env:LOCALAPPDATA\fp-tools"
$env:PW_DIR = "$tools\playwright\"   # trailing backslash: createRequire resolves node_modules from inside this folder
$env:PLAYWRIGHT_BROWSERS_PATH = "$tools\pw-browsers"
$env:CMP_CROPS = $Crops   # extra zoomed crops: "id:x,y,w,h,zoom;..." (see compare.mjs)
$node = "$tools\node\node.exe"
$src = "$Dir\page"; $pageOut = "$Dir\page-out"; $cmp = "$Dir\cmp"
if (-not (Test-Path "$Dir\out\selftest.json")) { Write-Output 'no native selftest in out\: run `just run-pc` first'; exit 1 }

function Run-Node([string]$name, [string[]]$argv) {
    $log = "$Dir\$name.log"
    $p = Start-Process -FilePath $node -ArgumentList $argv -WorkingDirectory $src -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle   # keeps the handle so ExitCode is available after the exit
    try {
        if (-not $p.WaitForExit($TimeoutSec * 1000)) { [Console]::WriteLine("$name timed out after $TimeoutSec s") }
    } finally {
        if (-not $p.HasExited) { & taskkill.exe /T /F /PID $p.Id 2>&1 | Out-Null }
    }
    Get-Content $log, "$log.err" -ErrorAction SilentlyContinue | ForEach-Object { [Console]::WriteLine($_) }
    return $p.ExitCode
}

$t0 = Get-Date
if ($Fresh -or -not (Test-Path "$pageOut\report.json")) {
    Remove-Item -Recurse -Force $pageOut -ErrorAction SilentlyContinue
    # with the scrollbars WebView2 shows (Playwright hides them by default)
    $pre = @('--require', "$src\native\ui\tools\pw-scrollbars.cjs")
    $null = Run-Node 'page-suite' ($pre + @("$src\tests\ui-scenarios.mjs", "$src\launcher\ui", $pageOut))
    $null = Run-Node 'page-extra' ($pre + @("$src\native\ui\tools\page-extra.mjs", "$src\launcher\ui", $pageOut))
}
Remove-Item -Recurse -Force $cmp -ErrorAction SilentlyContinue
$rc = Run-Node 'compare' @("$src\native\ui\tools\compare.mjs", "$Dir\out", $cmp, $pageOut)
Write-Output ("total {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds)
$left = Get-CimInstance Win32_Process -Filter "Name='node.exe'" | Where-Object { $_.CommandLine -like "*$src*" }
if ($left) { $left | ForEach-Object { & taskkill.exe /T /F /PID $_.ProcessId 2>&1 | Out-Null; Write-Output "killed leftover node $($_.ProcessId)" } }
exit $rc
