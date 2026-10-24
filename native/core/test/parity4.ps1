# Parity 4: the updater's download and SHA-256 path (nothing is run), and one launch for a double click. Needs the fake
# root of parity2.ps1 and RadarniteLauncherSetup-2.2.1.exe in -Dir.
param([string]$Dir = 'S:\rl-core-test')
$ErrorActionPreference = 'Stop'
$t = $Dir
$ref = Join-Path $t 'rlcoretest-ref'
$old = Join-Path $ref 'RadarniteLauncher.exe'
$new = Join-Path $t 'rn-core-cli.exe'
$harness = Join-Path $t 'coretest.exe'
$fake = Join-Path $t 'fakeroot'
$outDir = Join-Path $t 'out'
$fwGroup = 'rlcoretest-fw'
$cfgPath = Join-Path $ref 'config.json'
$utf8 = New-Object Text.UTF8Encoding $false
$backup = [IO.File]::ReadAllBytes($cfgPath)
function Run([string]$exe, [string]$argline, [hashtable]$envs = @{}, [int]$timeoutMs = 300000) {
  $psi = New-Object Diagnostics.ProcessStartInfo $exe, $argline
  $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  foreach ($k in $envs.Keys) { $psi.Environment[$k] = $envs[$k] }
  $p = [Diagnostics.Process]::Start($psi); $ms = New-Object IO.MemoryStream
  $e = $p.StandardError.ReadToEndAsync(); $copy = $p.StandardOutput.BaseStream.CopyToAsync($ms)
  if (-not $p.WaitForExit($timeoutMs)) { $p.Kill(); 'TIMEOUT ' + $argline }
  [void]$copy.Wait(5000)
  [pscustomobject]@{ bytes = $ms.ToArray(); code = $p.ExitCode; err = $e.Result; text = [Text.Encoding]::UTF8.GetString($ms.ToArray()) }
}
$envNew = @{ RADARNITE_LAUNCHER_DIR = $ref }
$envFw = @{ RADARNITE_LAUNCHER_DIR = $ref; RADARNITE_FW_GROUP = $fwGroup }
function Same($a, $b) { [Convert]::ToBase64String([byte[]]$a) -eq [Convert]::ToBase64String([byte[]]$b) }
function Show([string]$label, [string]$text) { "  --- $label"; $text.TrimEnd() -split "`n" | ForEach-Object { '  ' + $_.TrimEnd() } }
function GroupCount([string]$g) { $pol = New-Object -ComObject HNetCfg.FwPolicy2; @($pol.Rules | Where-Object { $_.Grouping -eq $g }).Count }

try {
  '== the updater path: download through the release redirect with streamed SHA-256, .sha256 token, file hash'
  $url = 'https://github.com/Radarnite/radarnite-launcher/releases/download/v2.2.1/RadarniteLauncherSetup-2.2.1.exe.sha256'
  $r = Run $harness "fetch $url `"$outDir\setup.sha256`""
  "  $($r.text.Trim())"
  "  Get-FileHash of the download: $((Get-FileHash "$outDir\setup.sha256").Hash.ToLower())"
  $r = Run $harness "sha `"$t\RadarniteLauncherSetup-2.2.1.exe`""
  "  installer: core $($r.text.Trim()) | Get-FileHash $((Get-FileHash "$t\RadarniteLauncherSetup-2.2.1.exe").Hash.ToLower())"

  '== one launch for a double click (coretest core sends Launch twice)'
  [void](Run $old "--set-builds-folder $fake")
  $cfgText = [IO.File]::ReadAllText($cfgPath)
  [IO.File]::WriteAllText($cfgPath, ($cfgText -replace '"port": 3551,', '"port": 39551,'), $utf8)
  $realBefore = GroupCount 'FN Offline Builds'
  [void](Run $new '--firewall rlcoretest-17.50' $envFw)
  $r = Run $harness 'core rlcoretest-17.50' $envFw
  $lines = $r.text -split "`n"
  "  exit $($r.code): 'Launching Fortnite' x$(@($lines | Where-Object { $_ -match 'Launching Fortnite' }).Count), 'Backend ready' x$(@($lines | Where-Object { $_ -match 'Backend ready' }).Count), last session: $(($lines | Where-Object { $_ -match '^\[session\]' } | Select-Object -Last 1).Trim())"
  [void](Run $new '--firewall-remove' $envFw)
  "  real group $realBefore -> $(GroupCount 'FN Offline Builds'), temp group $(GroupCount $fwGroup)"
  [IO.File]::WriteAllBytes($cfgPath, $backup)
} finally {
  [IO.File]::WriteAllBytes($cfgPath, $backup)
  "config restored: $(Same $backup ([IO.File]::ReadAllBytes($cfgPath)))"
  $left = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($t, [StringComparison]::OrdinalIgnoreCase) })
  "processes left from ${t}: $($left.Count)"
  $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "  killed own $($_.Name) $($_.ProcessId)" }
}
