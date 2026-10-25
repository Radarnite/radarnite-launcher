# Parity 2: fake build tree, config.json that must not be lost (broken file kept as config.json.broken, BOM, unknown
# keys), --stop limited to the root, firewall rules (temp group). Everything happens under S:\rl-core-test; the real
# builds folder is never the root of a write or a stop. The real firewall group is only read (rule count before/after).
param([string]$Dir = 'S:\rl-core-test')
$ErrorActionPreference = 'Stop'
$t = $Dir
$ref = Join-Path $t 'rlcoretest-ref'
$old = Join-Path $ref 'RadarniteLauncher.exe'
$new = Join-Path $t 'rn-core-cli.exe'
$fake = Join-Path $t 'fakeroot'
$outDir = Join-Path $t 'out'
$fwGroup = 'rlcoretest-fw'
$realGroup = 'FN Offline Builds'
New-Item -ItemType Directory -Force $outDir | Out-Null

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
  "{0,-44} stdout {1}  exit {2}/{3}" -f $label, ($(if ($s) { 'SAME' } else { 'DIFF' })), $a.code, $b.code
  if (-not $s) { Show '2.2.1' $a.text; Show 'core' $b.text }
  if ($b.err) { '  core stderr: ' + $b.err.Trim() }
}
function GroupCount([string]$g) { $pol = New-Object -ComObject HNetCfg.FwPolicy2; @($pol.Rules | Where-Object { $_.Grouping -eq $g }).Count }

# ------------------------------------------------------------------ fake builds folder
function FakeBuild([string]$dir, [string]$version, [switch]$Ascii) {
  $w = Join-Path $dir 'FortniteGame\Binaries\Win64'; New-Item -ItemType Directory -Force $w | Out-Null
  $enc = if ($Ascii) { [Text.Encoding]::ASCII } else { [Text.Encoding]::Unicode }
  $bytes = if ($version) { $enc.GetBytes("xx++Fortnite+Release-$version" + [char]0) } else { New-Object byte[] 0 }
  [IO.File]::WriteAllBytes((Join-Path $w 'FortniteClient-Win64-Shipping.exe'), $bytes)
}
if (Test-Path $fake) { Remove-Item -Recurse -Force $fake }
FakeBuild "$fake\rlcoretest-17.50" '17.50-CL-17388565'
New-Item -ItemType Directory -Force "$fake\rlcoretest-17.50\Engine\Binaries\Win64" | Out-Null
[IO.File]::WriteAllBytes("$fake\rlcoretest-17.50\Engine\Binaries\Win64\CrashReportClient.exe", [byte[]]@(77, 90))
[IO.File]::WriteAllBytes("$fake\rlcoretest-17.50\FortniteGame\Binaries\Win64\FortniteLauncher.exe", [byte[]]@(77, 90))
FakeBuild "$fake\rlcoretest-3.6\Fortnite" '3.6-CL-4019403'
FakeBuild "$fake\rlcoretest-12.41-ascii" '12.41-CL-12905909' -Ascii
FakeBuild "$fake\rlcoretest-noversion" ''
FakeBuild "$fake\rlcoretest-25.11-eos" '25.11-CL-26787151'
[IO.File]::WriteAllBytes("$fake\rlcoretest-25.11-eos\FortniteGame\Binaries\Win64\FortniteClient-Win64-Shipping_EAC_EOS.exe", [byte[]]@(77, 90))
FakeBuild "$fake\rlcoretest-overflow" '99999999999.1-CL-5'
FakeBuild "$fake\rlcoretest-two\a" '7.40-CL-5046157'
FakeBuild "$fake\rlcoretest-two\b" '7.40-CL-5046157'
FakeBuild "$fake\rlcoretest-21.10" '21.10-CL-21543261'
FakeBuild "$fake\rlcoretest-2.5" '2.5.0-CL-3847564'
New-Item -ItemType Directory -Force "$fake\rlcoretest-14.30-damaged" | Out-Null
Set-Content "$fake\rlcoretest-14.30-damaged\pakchunk11-WindowsClient.pak" 'x'
New-Item -ItemType Directory -Force "$fake\rlcoretest-half\FortniteGame\Content" | Out-Null
New-Item -ItemType Directory -Force "$fake\rlcoretest-empty" | Out-Null
New-Item -ItemType Directory -Force "$fake\launcher" | Out-Null
Set-Content "$fake\rlcoretest-3.6.7z" 'x'

# the ref tree gets its config.json the way the installer makes it
$cfgPath = Join-Path $ref 'config.json'
if (-not (Test-Path $cfgPath)) { $r = Run $old '--set-builds-folder S:\FortniteBuilds'; "setup: exit $($r.code) " + $r.text.Trim() }
$backup = [IO.File]::ReadAllBytes($cfgPath)

'== config.json load/save'
$cases = [ordered]@{
  'broken' = '{ "displayName" : "Pepe Grillo", "accountId": "0123456789abcdef0123456789abcdef", "bots": 20,, }'
  'bom-and-case' = ([char]0xFEFF + '{"DisplayName":"Bee","ACCOUNTID":"0123456789abcdef0123456789abcdef","Bots":7,"buildsFolder":"S:\\FortniteBuilds"}')
  'unknown-key (core keeps it)' = '{"displayName":"A","accountId":"0123456789abcdef0123456789abcdef","zzz":{"a":[1,2.50,"x"]},"buildsFolder":"S:\\FortniteBuilds"}'
}
$utf8 = New-Object Text.UTF8Encoding $false
function Mask([string]$s) { $s -replace '"accountId": "[0-9a-f]{32}"', '"accountId": "<random>"' -replace '"displayName": "[A-Za-z]+[0-9]{2}"', '"displayName": "<random>"' -replace 'config: [A-Za-z]+[0-9]{2} ', 'config: <random> ' }
foreach ($name in $cases.Keys) {
  $res = @{}
  foreach ($who in 'old', 'new') {
    [IO.File]::WriteAllBytes($cfgPath, $utf8.GetBytes($cases[$name])); Remove-Item "$cfgPath.broken" -ErrorAction SilentlyContinue
    $r = if ($who -eq 'old') { Run $old '--check zz-no-such-build' } else { Run $new '--check zz-no-such-build' $envNew }
    $res[$who] = [pscustomobject]@{ out = $r.text; code = $r.code; cfg = [IO.File]::ReadAllText($cfgPath); broken = (Test-Path "$cfgPath.broken"); err = $r.err }
  }
  $o = $res['old']; $n = $res['new']
  $mask = $name -like '*masked*'
  $co = if ($mask) { Mask $o.cfg } else { $o.cfg }; $cn = if ($mask) { Mask $n.cfg } else { $n.cfg }
  $so = if ($mask) { Mask $o.out } else { $o.out }; $sn = if ($mask) { Mask $n.out } else { $n.out }
  "{0,-30} config {1}  stdout {2}  .broken {3}/{4}  exit {5}/{6}" -f $name, ($(if ($co -ceq $cn) { 'SAME' } else { 'DIFF' })), ($(if ($so -ceq $sn) { 'SAME' } else { 'DIFF' })), $o.broken, $n.broken, $o.code, $n.code
  if ($co -cne $cn) { Show '2.2.1 config' $co; Show 'core config' $cn }
  if ($so -cne $sn) { Show '2.2.1 stdout' $so; Show 'core stdout' $sn }
  if ($n.err) { '  core stderr: ' + $n.err.Trim() }
}
[IO.File]::WriteAllBytes($cfgPath, $backup); Remove-Item "$cfgPath.broken" -ErrorAction SilentlyContinue
$a = Run $old '--check zz-no-such-build'; $b = Run $new '--check zz-no-such-build' $envNew
"current config untouched by both: $(Same $backup ([IO.File]::ReadAllBytes($cfgPath)))"

'== --set-builds-folder (trailing backslash kept, like DirectoryInfo.FullName)'
$a = Run $old "--set-builds-folder $fake\"; $ca = [IO.File]::ReadAllBytes($cfgPath)
[IO.File]::WriteAllBytes($cfgPath, $backup)
$b = Run $new "--set-builds-folder $fake\" $envNew; $cb = [IO.File]::ReadAllBytes($cfgPath)
CompareRun "--set-builds-folder $fake\" $a $b
"  config bytes: $(if (Same $ca $cb) { 'SAME' } else { 'DIFF' })"

'== --stop (root = fakeroot): only processes started from inside it'
$pingDir = Join-Path $fake 'rlcoretest-17.50\stoptest'; New-Item -ItemType Directory -Force $pingDir | Out-Null
Copy-Item "$env:SystemRoot\System32\PING.EXE" "$pingDir\rlping.exe"
foreach ($who in 'old', 'new') {
  $p = Start-Process "$pingDir\rlping.exe" '-n 120 127.0.0.1' -WindowStyle Hidden -PassThru
  Start-Sleep -Milliseconds 300
  $r = if ($who -eq 'old') { Run $old '--stop' } else { Run $new '--stop' $envNew }
  Start-Sleep -Milliseconds 300
  "{0}: exit {1} | {2} | ping still running: {3}" -f $who, $r.code, $r.text.Trim(), (-not $p.HasExited)
  if (-not $p.HasExited) { $p.Kill() }
}
$a = Run $old '--stop'; $b = Run $new '--stop' $envNew
CompareRun '--stop (nothing running)' $a $b

'== firewall with a temp group (core only; 2.2.1 cannot use another group)'
$realBefore = GroupCount $realGroup
"real group '$realGroup' rules before: $realBefore; temp group '$fwGroup' before: $(GroupCount $fwGroup)"
$b = Run $new '--firewall rlcoretest-17.50' $envFw
"  --firewall rlcoretest-17.50: exit $($b.code) | $($b.text.Trim()) $($b.err)"
$b = Run $new '--firewall rlcoretest-17.50' $envFw
"  again (stale rules replaced, no duplicates): exit $($b.code) | $($b.text.Trim()) | temp group rules: $(GroupCount $fwGroup)"
$pol = New-Object -ComObject HNetCfg.FwPolicy2
$mine = @($pol.Rules | Where-Object { $_.Grouping -eq $fwGroup } | Sort-Object Name)
$mine | ForEach-Object { '  rule: {0} | dir {1} act {2} prof 0x{3:X} proto {4} en {5} | {6}' -f $_.Name, $_.Direction, $_.Action, $_.Profiles, $_.Protocol, $_.Enabled, $_.ApplicationName }
$m = $mine | Where-Object { $_.Name -like '*node.exe (outbound)' } | Select-Object -First 1
$real = @($pol.Rules | Where-Object { $_.Grouping -eq $realGroup -and $_.ApplicationName -like '*node.exe' -and $_.Direction -eq 2 }) | Select-Object -First 1
if ($m -and $real) {
  foreach ($prop in 'Direction', 'Action', 'Profiles', 'Protocol', 'RemoteAddresses', 'Enabled', 'InterfaceTypes', 'EdgeTraversal', 'LocalAddresses') {
    "  vs real 2.x node rule: {0,-16} {1}" -f $prop, ($(if ("$($m.$prop)" -eq "$($real.$prop)") { 'SAME' } else { "DIFF core '$($m.$prop)' real '$($real.$prop)'" }))
  }
  "  description core: $($m.Description)"
  "  description real: $($real.Description)"
}
$b = Run $new '--check rlcoretest-17.50' $envFw
Show 'core --check rlcoretest-17.50 with the temp group' $b.text

$b = Run $new '--firewall-remove' $envFw
"  --firewall-remove: exit $($b.code) | $($b.text.Trim())"
$realAfter = GroupCount $realGroup
"temp group after: $(GroupCount $fwGroup); real group after: $realAfter (before $realBefore) -> $(if ($realAfter -eq $realBefore) { 'UNCHANGED' } else { 'CHANGED!' })"

[IO.File]::WriteAllBytes($cfgPath, $backup)
"config restored: $(Same $backup ([IO.File]::ReadAllBytes($cfgPath)))"
$left = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($t, [StringComparison]::OrdinalIgnoreCase) })
"processes left from ${t}: $($left.Count)"
$left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "  killed own $($_.Name) $($_.ProcessId)" }
