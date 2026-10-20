# Parity 3: the GUI-subsystem build (RadarniteLauncher-linktest.exe = native/ui + librncore.a, no console like 2.x)
# against 2.2.1; config escaping over every BMP code point; rn::Core through the test harness; add from zip/folder.
# Scratch folders only; ref config.json and firewall restored; real group only counted.
param([string]$Dir = 'S:\rl-core-test')
$ErrorActionPreference = 'Stop'
$t = $Dir
$ref = Join-Path $t 'rlcoretest-ref'
$old = Join-Path $ref 'RadarniteLauncher.exe'
$new = Join-Path $t 'rn-core-cli.exe'
$gui = Join-Path $t 'RadarniteLauncher-linktest.exe'
$harness = Join-Path $t 'coretest.exe'
$fake = Join-Path $t 'fakeroot'
$outDir = Join-Path $t 'out'
$fwGroup = 'rlcoretest-fw'
$cfgPath = Join-Path $ref 'config.json'
$utf8 = New-Object Text.UTF8Encoding $false
$backup = [IO.File]::ReadAllBytes($cfgPath)

function Run([string]$exe, [string]$argline, [hashtable]$envs = @{}, [int]$timeoutMs = 120000) {
  $psi = New-Object Diagnostics.ProcessStartInfo $exe, $argline
  $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  foreach ($k in $envs.Keys) { $psi.Environment[$k] = $envs[$k] }
  $p = [Diagnostics.Process]::Start($psi)
  $ms = New-Object IO.MemoryStream
  $e = $p.StandardError.ReadToEndAsync()
  $copy = $p.StandardOutput.BaseStream.CopyToAsync($ms)
  if (-not $p.WaitForExit($timeoutMs)) { $p.Kill(); 'TIMEOUT ' + $argline }
  [void]$copy.Wait(5000)
  [pscustomobject]@{ bytes = $ms.ToArray(); code = $p.ExitCode; err = $e.Result; text = [Text.Encoding]::UTF8.GetString($ms.ToArray()) }
}
$envNew = @{ RADARNITE_LAUNCHER_DIR = $ref }
$envFw = @{ RADARNITE_LAUNCHER_DIR = $ref; RADARNITE_FW_GROUP = $fwGroup }
function Same($a, $b) { [Convert]::ToBase64String([byte[]]$a) -eq [Convert]::ToBase64String([byte[]]$b) }
function Show([string]$label, [string]$text) { "  --- $label"; $text.TrimEnd() -split "`n" | ForEach-Object { '  ' + $_.TrimEnd() } }
function CompareRun([string]$label, $a, $b) {
  $s = Same $a.bytes $b.bytes
  "{0,-40} stdout {1}  exit {2}/{3}" -f $label, ($(if ($s) { 'SAME' } else { 'DIFF' })), $a.code, $b.code
  if (-not $s) { Show '2.2.1' $a.text; Show 'core' $b.text }
  if ($b.err) { '  core stderr: ' + $b.err.Trim() }
}
function GroupCount([string]$g) { $pol = New-Object -ComObject HNetCfg.FwPolicy2; @($pol.Rules | Where-Object { $_.Grouping -eq $g }).Count }
function Stats([double[]]$v) { $s = $v | Sort-Object; $n = $s.Count; $med = if ($n % 2) { $s[($n - 1) / 2] } else { ($s[$n / 2 - 1] + $s[$n / 2]) / 2 }; '{0} runs: median {1:N1} ms, min {2:N1}, max {3:N1}' -f $n, $med, $s[0], $s[$n - 1] }
function TimeCmd([string]$file, [string]$argline, [int]$n, [hashtable]$envs = @{}) {
  $ms = New-Object System.Collections.Generic.List[double]
  for ($i = 0; $i -lt $n + 3; $i++) {
    $psi = New-Object Diagnostics.ProcessStartInfo $file, $argline
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    foreach ($k in $envs.Keys) { $psi.Environment[$k] = $envs[$k] }
    $sw = [Diagnostics.Stopwatch]::StartNew(); $p = [Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync(); $p.WaitForExit(); $tm = $sw.Elapsed.TotalMilliseconds
    [void]$o.Result; [void]$e.Result
    if ($i -ge 3) { $ms.Add($tm) }
  }
  Stats $ms.ToArray()
}

try {
  '== GUI-subsystem core vs 2.2.1 on the real builds folder (read-only commands)'
  foreach ($c in '--check 14.30', '--check', '--check 17.50', '--check zz-no-such-build', '--scan', "--scan $fake", '--firewall zz-no-such-build', '--set-builds-folder') {
    CompareRun $c (Run $old $c) (Run $gui $c $envNew)
  }
  "config.json untouched: $(Same $backup ([IO.File]::ReadAllBytes($cfgPath)))"
  'timing --check 14.30 2.2.1      : ' + (TimeCmd $old '--check 14.30' 20)
  'timing --check 14.30 core (GUI) : ' + (TimeCmd $gui '--check 14.30' 20 $envNew)
  'timing --check 14.30 core (CLI) : ' + (TimeCmd $new '--check 14.30' 20 $envNew)

  '== config writer over every BMP code point (vs 2.2.1 output saved by oracle.ps1)'
  $sb = New-Object Text.StringBuilder
  for ($c = 0; $c -lt 0x10000; $c++) { if ($c -ge 0xD800 -and $c -le 0xDFFF) { continue }; [void]$sb.AppendFormat('\u{0:x4}', $c) }
  [IO.File]::WriteAllText($cfgPath, '{"displayName":"A","accountId":"0123456789abcdef0123456789abcdef","practicePlaylist":"' + $sb.ToString() + '","buildsFolder":"S:\\FortniteBuilds"}', $utf8)
  $r = Run $gui '--check zz-no-such-build' $envNew
  $want = [IO.File]::ReadAllBytes("$outDir\oracle-config.json"); $got = [IO.File]::ReadAllBytes($cfgPath)
  "all-BMP config.json: $(if (Same $want $got) { 'SAME' } else { 'DIFF' }) ($($got.Length) bytes vs $($want.Length))"

  '== non-ASCII name: config.json and stdout (ANSI code page, best-fit, like .NET without a console)'
  $json = '{"displayName":"\u00d1and\u00fa \u00a0\u3000\ufeff\u4e2d \u0101 \ud83d\ude00","accountId":"0123456789abcdef0123456789abcdef","buildsFolder":"S:\\FortniteBuilds"}'
  $res = @{}
  foreach ($who in 'old', 'gui') {
    [IO.File]::WriteAllText($cfgPath, $json, $utf8)
    $r = if ($who -eq 'old') { Run $old '--check zz-no-such-build' } else { Run $gui '--check zz-no-such-build' $envNew }
    $res[$who] = @{ out = $r.bytes; cfg = [IO.File]::ReadAllBytes($cfgPath) }
  }
  "  config $(if (Same $res.old.cfg $res.gui.cfg) { 'SAME' } else { 'DIFF' }); stdout $(if (Same $res.old.out $res.gui.out) { 'SAME' } else { 'DIFF' }): " + (($res.gui.out | ForEach-Object { '{0:x2}' -f $_ }) -join ' ')
  [IO.File]::WriteAllBytes($cfgPath, $backup)

  '== rn::Core through api.h (coretest.exe core): root = fakeroot, temp firewall group, port 39551'
  $realBefore = GroupCount 'FN Offline Builds'
  [void](Run $old "--set-builds-folder $fake")
  $cfgText = [IO.File]::ReadAllText($cfgPath)
  [IO.File]::WriteAllText($cfgPath, ($cfgText -replace '"port": 3551,', '"port": 39551,'), $utf8)
  $r = Run $new '--firewall rlcoretest-17.50' $envFw
  "  rules: $($r.text.Trim())"
  $r = Run $harness 'core rlcoretest-17.50' $envFw 120000
  "  exit $($r.code)"; Show 'coretest core' $r.text
  if ($r.err) { '  stderr: ' + $r.err.Trim() }
  $cfgAfter = [IO.File]::ReadAllText($cfgPath)
  "  saveConfig wrote: " + (($cfgAfter -split "`r`n" | Where-Object { $_ -match '"(displayName|bots)"' }) -join ' ')
  "  UI prefs key: " + ((Get-ItemProperty 'HKCU:\Software\Radarnite\Launcher\UI' -ErrorAction SilentlyContinue | Out-String).Trim())
  $r = Run $new '--firewall-remove' $envFw
  "  $($r.text.Trim()) | real group $realBefore -> $(GroupCount 'FN Offline Builds')"

  '== add from zip / folder (root = fakeroot)'
  $z = Join-Path $t 'ziptest'; if (Test-Path $z) { Remove-Item -Recurse -Force $z }
  function FakeBuild([string]$dir, [string]$version) {
    $w = Join-Path $dir 'FortniteGame\Binaries\Win64'; New-Item -ItemType Directory -Force $w | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $w 'FortniteClient-Win64-Shipping.exe'), [Text.Encoding]::Unicode.GetBytes("xx++Fortnite+Release-$version" + [char]0))
    New-Item -ItemType Directory -Force (Join-Path $dir 'FortniteGame\Content\Paks') | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $dir 'FortniteGame\Content\Paks\pakchunk0-WindowsClient.pak'), (New-Object byte[] 300000))
  }
  FakeBuild "$z\zipsrc\Top\rlzip" '9.10-CL-6639283'
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [IO.Compression.ZipFile]::CreateFromDirectory("$z\zipsrc", "$z\rlzip.zip")
  $r = Run $harness "addzip `"$z\rlzip.zip`"" $envNew 300000
  "  addzip exit $($r.code)"; Show 'addzip' $r.text
  FakeBuild "$z\movesrc\rlmove" '7.30-CL-4834550'
  $r = Run $harness "addfolder `"$z\movesrc`"" $envNew
  "  addfolder (same drive: moved) exit $($r.code)"; Show 'addfolder' $r.text
  $cdir = Join-Path $env:TEMP 'rlcoretest-copysrc'; if (Test-Path $cdir) { Remove-Item -Recurse -Force $cdir }
  FakeBuild "$cdir\rlcopy" '8.51-CL-6165369'
  $r = Run $harness "addfolder `"$cdir\rlcopy`"" $envNew
  "  addfolder (other drive: copied) exit $($r.code), source kept: $(Test-Path "$cdir\rlcopy\FortniteGame")"; Show 'addfolder' $r.text
  Remove-Item -Recurse -Force $cdir
  Show 'scan after adding' (Run $new "--scan $fake" $envNew).text
  "  leftovers in fakeroot: " + ((Get-ChildItem $fake -Directory | Where-Object { $_.Name -like '_*' } | ForEach-Object Name) -join ', ')
} finally {
  [IO.File]::WriteAllBytes($cfgPath, $backup)
  "config restored: $(Same $backup ([IO.File]::ReadAllBytes($cfgPath)))"
  $left = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($t, [StringComparison]::OrdinalIgnoreCase) })
  "processes left from ${t}: $($left.Count)"
  $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "  killed own $($_.Name) $($_.ProcessId)" }
}
