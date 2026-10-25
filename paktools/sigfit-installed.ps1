# Fits every pak of offline\custom-cosmetics.json to a signature file of the build (fn-modkit sigfit, SigFit.cs), so
# the game's pak signature check accepts them ("Corrupted data was found in game files" otherwise). Paks that
# already pass are skipped. The originals go to offline\backup-sigfit-<date>; the game must be closed.
#   powershell -File sigfit-installed.ps1 [-Build S:\FortniteBuilds\14.30] [-Tool <fn-modkit.dll>]
param([string]$Build = 'S:\FortniteBuilds\14.30', [string]$Tool = '')
$ErrorActionPreference = 'Stop'
# the tool lives next to this script in the launcher (launcher\tools\fn-modkit-sigfit), never in a synced build mirror
if (-not $Tool) { $Tool = @((Join-Path $PSScriptRoot 'fn-modkit-sigfit\fn-modkit.dll'), "$env:LOCALAPPDATA\fp-tools\fn-modkit-sigfit\fn-modkit.dll") | ? { Test-Path $_ } | Select -First 1 }
if (-not $Tool -or -not (Test-Path $Tool)) { throw "fn-modkit sigfit not found (launcher\tools\fn-modkit-sigfit): own paks NOT checked" }
if (Get-Process FortniteClient-Win64-Shipping -ErrorAction SilentlyContinue) { throw 'Fortnite is running: close it first (its paks are open).' }
$paks = Join-Path $Build 'FortniteGame\Content\Paks'; $off = Join-Path $Build 'offline'; $cc = Join-Path $off 'custom-cosmetics.json'
$list = @((Get-Content $cc -Raw -Encoding UTF8 | ConvertFrom-Json).items | ForEach-Object { $_.paks } | Where-Object { $_ -like '*.pak' } | Sort-Object -Unique |
          ForEach-Object { Join-Path $paks $_ } | Where-Object { Test-Path $_ })
if (-not $list) { 'no custom paks'; exit 0 }
$tmp = Join-Path $off 'sigfit-tmp'; New-Item -ItemType Directory $tmp -Force | Out-Null
$env:DOTNET_ROOT = "$env:LOCALAPPDATA\fp-dotnet"
$ErrorActionPreference = 'Continue'   # its progress notes on stderr are not errors (Windows PowerShell 5)
$out = & "$env:LOCALAPPDATA\fp-dotnet\dotnet.exe" $Tool sigfit --pak ($list -join ',') --paks $paks --out $tmp --custom $cc 2>$null
$ErrorActionPreference = 'Stop'
# every pak must come back with a result line: no output (tool crashed, runtime missing) is a failure, never "fine"
$results = @($out | ? { $_ -match '^\{' })
if ($LASTEXITCODE -gt 1 -or $results.Count -ne $list.Count) { throw "fn-modkit sigfit gave $($results.Count) results for $($list.Count) paks (exit $LASTEXITCODE): own paks NOT checked" }
$bak = Join-Path $off ('backup-sigfit-' + (Get-Date -Format 'yyyyMMdd-HHmmss')); $done = 0; $bad = 0
foreach ($line in $results) {
    $r = $line | ConvertFrom-Json
    if (-not $r.ok) { $bad++; "FAILED $($r.pak): $($r.error)"; continue }
    if ($r.already) { continue }
    New-Item -ItemType Directory $bak -Force | Out-Null
    foreach ($ext in '.pak', '.sig') {
        $name = [IO.Path]::ChangeExtension($r.pak, $ext)
        Copy-Item (Join-Path $paks $name) $bak -Force
        Move-Item (Join-Path $tmp $name) (Join-Path $paks $name) -Force
    }
    $done++; "fitted $($r.pak) <- $($r.Donor).sig ($($r.Chunks) chunks, $($r.Files) files)"
}
Remove-Item $tmp -Recurse -Force
"$done fitted, $($list.Count - $done - $bad) already fine, $bad failed" + $(if ($done) { "; originals in $bak" } else { '' })
if ($bad) { exit 1 }
