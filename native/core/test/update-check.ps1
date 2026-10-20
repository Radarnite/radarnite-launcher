# Updater end to end (just core-update-test-pc): the version table (coretest semver), then --update-check of
# rn-core-cli built as 3.0.0, 3.0.0-rc.1 and 3.0.0-rc.2, and of the window exe (-Window), against fake-github.mjs on
# 127.0.0.1 (RADARNITE_UPDATE_API), and once against the real GitHub API (-SkipReal to leave it out). stdout and exit
# code are the 2.x ones (exit 10 = update available); a release build must never read the release list. Each run's
# update.log goes to its own folder under -Dir (RADARNITE_LAUNCHER_DIR). Nothing is downloaded or installed.
# Layout in -Dir: rn-core-cli-<version>.exe, coretest.exe, fake-github.mjs, this script.
param([string]$Dir = 'S:\rl-rc-test\update', [string]$Window = '', [string]$Node = "$env:LOCALAPPDATA\fp-tools\node\node.exe", [switch]$SkipReal)
$ErrorActionPreference = 'Stop'
$repo = 'Radarnite/radarnite-launcher'
$script:pass = 0; $script:fail = 0
function Check([bool]$ok, [string]$what) { if ($ok) { $script:pass++; "PASS $what" } else { $script:fail++; "FAIL $what" } }

function Run([string]$exe, [string]$argline, [hashtable]$envs = @{}, [int]$timeoutMs = 120000) {
  $psi = New-Object Diagnostics.ProcessStartInfo $exe, $argline
  $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  [void]$psi.Environment.Remove('RADARNITE_UPDATE_API')
  foreach ($k in $envs.Keys) { $psi.Environment[$k] = $envs[$k] }
  $p = [Diagnostics.Process]::Start($psi)
  $ms = New-Object IO.MemoryStream
  $e = $p.StandardError.ReadToEndAsync()
  $copy = $p.StandardOutput.BaseStream.CopyToAsync($ms)
  $timedOut = -not $p.WaitForExit($timeoutMs)
  if ($timedOut) { $p.Kill() }
  [void]$copy.Wait(5000)
  [pscustomobject]@{ code = $(if ($timedOut) { 'TIMEOUT' } else { $p.ExitCode }); err = $e.Result; text = [Text.Encoding]::UTF8.GetString($ms.ToArray()).TrimEnd("`r", "`n") }
}

function Want([string]$cur, [string]$offer, [string]$asset) {
  if (-not $offer) { return "Up to date (installed $cur; repo $repo)." }
  if (-not $asset) { $asset = $offer }
  "Update available: $offer (installed $cur); installer https://github.com/$repo/releases/download/v$asset/RadarniteLauncherSetup-$asset.exe"
}

function LogLines([string]$lh) {
  $f = Join-Path $lh 'update.log'
  if (Test-Path $f) { @(Get-Content $f | ForEach-Object { $_.Substring($_.IndexOf(' ') + 1) }) } else { @() }
}

# One --update-check against the fake: case[~label] picks the fake's answers; offer = the version it must offer ("" =
# up to date), asset = the version in the installer name when it differs; listRead = whether the release list is asked.
function Case([string]$case, [string]$exe, [string]$cur, [string]$offer, [bool]$listRead, [string]$asset = '', [string]$logLine = '') {
  $lh = Join-Path $Dir ('home-' + $case.Replace('~', '-'))
  if (Test-Path $lh) { Remove-Item -Recurse -Force $lh }
  New-Item -ItemType Directory -Force $lh | Out-Null
  $r = Run $exe '--update-check' @{ RADARNITE_UPDATE_API = "$api/$case"; RADARNITE_LAUNCHER_DIR = $lh }
  $want = Want $cur $offer $asset; $code = $(if ($offer) { 10 } else { 0 })
  Check (($r.text -ceq $want) -and ($r.code -eq $code)) ("{0} ({1}): exit {2} | {3}" -f $case, (Split-Path $exe -Leaf), $r.code, $r.text)
  if (-not (($r.text -ceq $want) -and ($r.code -eq $code))) { "  want exit $code | $want" }
  if ($r.err) { '  stderr: ' + $r.err.Trim() }
  $hits = @(Get-Content $reqLog | Where-Object { $_.StartsWith("/$case/") } | ForEach-Object { $_.Substring($case.Length + 1) })
  $listed = @($hits | Where-Object { $_ -like '*releases?per_page=*' }).Count -gt 0
  Check (($listed -eq $listRead) -and ($hits.Count -ge 1)) ("{0}: release list {1} (requests: {2})" -f $case, $(if ($listRead) { 'read' } else { 'never read' }), ($hits -join ', '))
  $lines = LogLines $lh
  $lines | ForEach-Object { '  update.log: ' + $_ }
  if ($logLine) { Check ($lines -ccontains $logLine) "${case}: update.log has '$logLine'" }
}

$cli300 = Join-Path $Dir 'rn-core-cli-3.0.0.exe'
$cliRc1 = Join-Path $Dir 'rn-core-cli-3.0.0-rc.1.exe'
$cliRc2 = Join-Path $Dir 'rn-core-cli-3.0.0-rc.2.exe'

'== coretest semver'
$r = Run (Join-Path $Dir 'coretest.exe') 'semver'
$r.text -split "`n" | ForEach-Object { '  ' + $_.TrimEnd() }
Check ($r.code -eq 0) "semver table (exit $($r.code))"

'== --update-check against the fake GitHub API'
$reqLog = Join-Path $Dir 'requests.log'
Remove-Item $reqLog -ErrorAction SilentlyContinue
$psi = New-Object Diagnostics.ProcessStartInfo $Node, "`"$Dir\fake-github.mjs`" `"$reqLog`""
$psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true
$fake = [Diagnostics.Process]::Start($psi)
try {
  $port = $fake.StandardOutput.ReadLine()
  if ($port -notmatch '^\d+$') { throw "the fake API did not start: '$port'" }
  $api = "http://127.0.0.1:$port"
  # release build: releases/latest only, the 2.x lines
  Case 'stable-vs-221' $cli300 '3.0.0' '' $false -logLine 'up to date (installed 3.0.0, latest 2.2.1)'
  Case 'stable-vs-301' $cli300 '3.0.0' '3.0.1' $false -logLine 'update available: 3.0.1 > 3.0.0'
  Case 'stable-vs-mislabelled' $cli300 '3.0.0' '3.0.1' $false -asset '3.0.1-hotfix.1'
  # pre-release builds: the newer of releases/latest and the newest of the list
  Case 'rc1-vs-rc2' $cliRc1 '3.0.0-rc.1' '3.0.0-rc.2' $true -logLine 'update available: 3.0.0-rc.2 > 3.0.0-rc.1'
  Case 'rc2-vs-final' $cliRc2 '3.0.0-rc.2' '3.0.0' $true -logLine 'update available: 3.0.0 > 3.0.0-rc.2'
  Case 'rc2-draft-rc3' $cliRc2 '3.0.0-rc.2' '' $true -logLine 'up to date (installed 3.0.0-rc.2, latest 2.2.1, newest 3.0.0-rc.2)'
  Case 'rc2-vs-older-rc1' $cliRc2 '3.0.0-rc.2' '' $true
  Case 'rc2-rc3-no-installer' $cliRc2 '3.0.0-rc.2' '' $true -logLine 'release v3.0.0-rc.3 has no RadarniteLauncherSetup-*.exe asset'
  Case 'rc1-list-500' $cliRc1 '3.0.0-rc.1' '' $true -logLine 'up to date (installed 3.0.0-rc.1, latest 2.2.1, newest -)'
  Case 'rc1-no-latest' $cliRc1 '3.0.0-rc.1' '3.0.0-rc.2' $true
  Case 'rc1-newer-rc-than-final' $cliRc1 '3.0.0-rc.1' '3.0.1-rc.1' $true
  if ($Window) {
    '== window exe ' + (Split-Path $Window -Leaf)
    $vi = (Get-Item $Window).VersionInfo
    "  ProductVersion $($vi.ProductVersion), FileVersion $($vi.FileVersion), FileVersionRaw $($vi.FileMajorPart).$($vi.FileMinorPart).$($vi.FileBuildPart).$($vi.FilePrivatePart)"
    switch ($vi.ProductVersion) {
      '3.0.0-rc.1' { Case 'rc1-vs-rc2~window' $Window '3.0.0-rc.1' '3.0.0-rc.2' $true }
      '3.0.0-rc.2' { Case 'rc2-vs-final~window' $Window '3.0.0-rc.2' '3.0.0' $true }
      '3.0.0' { Case 'stable-vs-221~window' $Window '3.0.0' '' $false }
      default { "  no fake case for $($vi.ProductVersion)" }
    }
  }
} finally {
  if (-not $fake.HasExited) { $fake.Kill(); [void]$fake.WaitForExit(5000) }
}

if (-not $SkipReal) {
  '== --update-check against the real GitHub API (nothing newer is published for these builds)'
  $real = @(@{ exe = $cli300; cur = '3.0.0' })
  if ($Window) { $real += @{ exe = $Window; cur = (Get-Item $Window).VersionInfo.ProductVersion } }
  foreach ($x in $real) {
    $lh = Join-Path $Dir ('home-real-' + $x.cur); if (Test-Path $lh) { Remove-Item -Recurse -Force $lh }; New-Item -ItemType Directory -Force $lh | Out-Null
    $r = Run $x.exe '--update-check' @{ RADARNITE_LAUNCHER_DIR = $lh }
    $lines = LogLines $lh
    $ok = ($r.code -eq 0) -and ($r.text -ceq (Want $x.cur '')) -and (@($lines | Where-Object { $_ -match "^up to date \(installed $([regex]::Escape($x.cur)), latest \d+\.\d+\.\d+" }).Count -eq 1)
    Check $ok ("real ({0}): exit {1} | {2}" -f (Split-Path $x.exe -Leaf), $r.code, $r.text)
    $lines | ForEach-Object { '  update.log: ' + $_ }
  }
}

$left = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Dir, [StringComparison]::OrdinalIgnoreCase) })
$left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "  killed own $($_.Name) $($_.ProcessId)" }
"update-check: $script:pass passed, $script:fail failed"
