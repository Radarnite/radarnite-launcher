# Parity 1: 2.2.1 (extracted, never installed) vs rn-core-cli.exe on the real builds folder, read-only commands only.
# Both use the same launcher folder (RADARNITE_LAUNCHER_DIR for the core). Also times --check like measure-2x.ps1.
# -Exe: another core exe instead of <Dir>\rn-core-cli.exe, e.g. the full RadarniteLauncher.exe (window + core).
param([string]$Dir = 'S:\rl-core-test', [int]$Runs = 20, [string]$Build = '14.30', [string]$Exe = '')
$ErrorActionPreference = 'Stop'
$t = $Dir
$ref = Join-Path $t 'rlcoretest-ref'
if (-not (Test-Path $ref)) { Move-Item (Join-Path $t 'ref\x') $ref; Remove-Item (Join-Path $t 'ref') -Recurse -Force -ErrorAction SilentlyContinue }
$old = Join-Path $ref 'RadarniteLauncher.exe'
$new = if ($Exe) { $Exe } else { Join-Path $t 'rn-core-cli.exe' }
$outDir = Join-Path $t 'out'
New-Item -ItemType Directory -Force $outDir | Out-Null

function Run([string]$exe, [string]$argline, [hashtable]$envs = @{}) {
  $psi = New-Object Diagnostics.ProcessStartInfo $exe, $argline
  $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  foreach ($k in $envs.Keys) { $psi.Environment[$k] = $envs[$k] }
  $p = [Diagnostics.Process]::Start($psi)
  $ms = New-Object IO.MemoryStream
  $e = $p.StandardError.ReadToEndAsync()
  $p.StandardOutput.BaseStream.CopyTo($ms)
  $p.WaitForExit()
  [pscustomobject]@{ bytes = $ms.ToArray(); code = $p.ExitCode; err = $e.Result }
}
$envNew = @{ RADARNITE_LAUNCHER_DIR = $ref }

# the ref tree gets its config.json the way the installer makes it
if (-not (Test-Path "$ref\config.json")) { $r = Run $old '--set-builds-folder S:\FortniteBuilds'; "setup: exit $($r.code) " + [Text.Encoding]::UTF8.GetString($r.bytes).Trim() }
$cfgHash = (Get-FileHash "$ref\config.json").Hash

$cases = @("--check $Build", '--check', '--check 17.50', '--check zz-no-such-build', '--scan', '--scan S:\FortniteBuilds\', '--firewall zz-no-such-build', '--cosmetics zz-no-such-build', '--set-builds-folder', '--set-builds-folder " "')
$i = 0
foreach ($c in $cases) {
  $i++
  $a = Run $old $c
  $b = Run $new $c $envNew
  [IO.File]::WriteAllBytes("$outDir\p1-$i-old.txt", $a.bytes); [IO.File]::WriteAllBytes("$outDir\p1-$i-new.txt", $b.bytes)
  $same = [Convert]::ToBase64String($a.bytes) -eq [Convert]::ToBase64String($b.bytes)
  "{0,-34} stdout {1}  exit {2}/{3}  ({4} bytes)" -f $c, ($(if ($same) { 'SAME' } else { 'DIFF' })), $a.code, $b.code, $a.bytes.Length
  if (-not $same) {
    '  --- 2.2.1'; [Text.Encoding]::UTF8.GetString($a.bytes).TrimEnd() -split "`n" | ForEach-Object { '  ' + $_.TrimEnd() }
    '  --- core'; [Text.Encoding]::UTF8.GetString($b.bytes).TrimEnd() -split "`n" | ForEach-Object { '  ' + $_.TrimEnd() }
  }
  if ($b.err) { '  core stderr: ' + $b.err.Trim() }
}
"config.json unchanged by the runs: $((Get-FileHash "$ref\config.json").Hash -eq $cfgHash)"
"--check $Build output (2.2.1):"
[Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes("$outDir\p1-1-old.txt")).TrimEnd() -split "`n" | ForEach-Object { '  ' + $_.TrimEnd() }

# ---- timing, same method as baseline/measure-2x.ps1 (process start -> exit, output drained, 3 warm-ups)
function Stats([double[]]$v) {
  $s = $v | Sort-Object; $n = $s.Count
  $med = if ($n % 2) { $s[($n - 1) / 2] } else { ($s[$n / 2 - 1] + $s[$n / 2]) / 2 }
  '{0} runs: median {1:N1} ms, min {2:N1}, max {3:N1}' -f $n, $med, $s[0], $s[$n - 1]
}
function TimeCmd([string]$file, [string]$argline, [int]$n, [hashtable]$envs = @{}, [int]$warm = 3) {
  $ms = New-Object System.Collections.Generic.List[double]
  for ($i = 0; $i -lt $warm + $n; $i++) {
    $psi = New-Object Diagnostics.ProcessStartInfo $file, $argline
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    foreach ($k in $envs.Keys) { $psi.Environment[$k] = $envs[$k] }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit(); $tm = $sw.Elapsed.TotalMilliseconds
    [void]$o.Result; [void]$e.Result
    if ($i -ge $warm) { $ms.Add($tm) }
  }
  Stats $ms.ToArray()
}
"timing --check $Build 2.2.1: " + (TimeCmd $old "--check $Build" $Runs)
"timing --check $Build core : " + (TimeCmd $new "--check $Build" $Runs $envNew)
"timing --check (all) 2.2.1: " + (TimeCmd $old '--check' 10)
"timing --check (all) core : " + (TimeCmd $new '--check' 10 $envNew)
"timing --scan core        : " + (TimeCmd $new '--scan' 10 $envNew)
"timing --set-builds-folder (usage) core: " + (TimeCmd $new '--set-builds-folder' 10 $envNew)
