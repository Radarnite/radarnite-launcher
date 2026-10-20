# Parity 2: fake build tree, config writer, --set-builds-folder, --stop, firewall (temp group), --play through the
# stub, --cosmetics, --update-check. Everything happens under S:\rl-core-test; the real builds folder is never the root
# of a write or a stop. The real firewall group is only read (rule count before/after).
param([string]$Dir = 'S:\rl-core-test', [switch]$SkipNetwork)
$ErrorActionPreference = 'Stop'
$t = $Dir
$ref = Join-Path $t 'rlcoretest-ref'
$old = Join-Path $ref 'RadarniteLauncher.exe'
$new = Join-Path $t 'rn-core-cli.exe'
$new220 = Join-Path $t 'rn-core-cli-220.exe'
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

$cfgPath = Join-Path $ref 'config.json'
$backup = [IO.File]::ReadAllBytes($cfgPath)

'== build.json cache writer (first --scan writes offline\build.json)'
$a = Run $old "--scan $fake"
$cacheA = @{}; Get-ChildItem $fake -Recurse -Filter build.json | ForEach-Object { $cacheA[$_.FullName] = [IO.File]::ReadAllBytes($_.FullName); Remove-Item $_.FullName }
$b = Run $new "--scan $fake" $envNew
$cacheB = @{}; Get-ChildItem $fake -Recurse -Filter build.json | ForEach-Object { $cacheB[$_.FullName] = [IO.File]::ReadAllBytes($_.FullName) }
CompareRun "--scan fakeroot (cold cache)" $a $b
foreach ($k in $cacheA.Keys) {
  $ok = $cacheB.ContainsKey($k) -and (Same $cacheA[$k] $cacheB[$k])
  "  build.json {0}: {1}" -f $k.Substring($fake.Length + 1), ($(if ($ok) { 'SAME' } else { 'DIFF' }))
  if (-not $ok) { '    2.2.1: ' + [Text.Encoding]::UTF8.GetString($cacheA[$k]); '    core : ' + $(if ($cacheB.ContainsKey($k)) { [Text.Encoding]::UTF8.GetString($cacheB[$k]) } else { '(missing)' }) }
}
if ($cacheB.Count -ne $cacheA.Count) { "  build.json count 2.2.1 $($cacheA.Count) / core $($cacheB.Count)" }
CompareRun "--scan fakeroot (warm cache)" (Run $old "--scan $fake") (Run $new "--scan $fake" $envNew)
Show 'scan output' (Run $new "--scan $fake" $envNew).text

'== config.json load/save'
$cases = [ordered]@{
  'nohelp-unicode' = '{"displayName":"\u00d1and\u00fa-\u00e9 \u00fc \u4e2d\u6587 \u00a0\u00ad\u200b\ufeff\ue000\u2028\u0378\u0600\u3000 \ud83d\ude00 <>&''+`\" \\ \t /","accountId":"0123456789abcdef0123456789abcdef","port":3551,"mode":"practice","lastPlaylist":"playlist_defaultsolo","bots":20,"infiniteMaterials":true,"infiniteAmmo":false,"lateGame":false,"practicePlaylist":"\u007f\u0080\u009f\u00ff\ufffd\ufffe\uffff\u1680\u2066\u206f\u0085\u00a9\u20ac\u05d0\u0627","matchPlaylist":"x","botsAI":false,"autoStartSeconds":60,"buildsFolder":"S:\\FortniteBuilds"}'
  'broken' = '{ "displayName" : "Pepe Grillo", "accountId": "0123456789abcdef0123456789abcdef", "bots": 20,, }'
  'string-for-int' = '{"displayName":"A","accountId":"0123456789abcdef0123456789abcdef","bots":"20"}'
  'nulls' = '{"displayName":"A","accountId":"0123456789abcdef0123456789abcdef","mode":null,"lastPlaylist":null,"bots":5,"buildsFolder":"S:\\FortniteBuilds"}'
  'bom-and-case' = ([char]0xFEFF + '{"DisplayName":"Bee","ACCOUNTID":"0123456789abcdef0123456789abcdef","Bots":7,"buildsFolder":"S:\\FortniteBuilds"}')
  'float-port' = '{"displayName":"A","accountId":"0123456789abcdef0123456789abcdef","port":3551.0}'
  'big-port' = '{"displayName":"A","accountId":"0123456789abcdef0123456789abcdef","port":99999999999}'
  'negative-bots' = '{"displayName":"A","accountId":"0123456789abcdef0123456789abcdef","bots":-5,"buildsFolder":"S:\\FortniteBuilds"}'
  'duplicate-key' = '{"displayName":"A","displayName":"B","accountId":"0123456789abcdef0123456789abcdef","buildsFolder":"S:\\FortniteBuilds"}'
  'unknown-key (core keeps it)' = '{"displayName":"A","accountId":"0123456789abcdef0123456789abcdef","zzz":{"a":[1,2.50,"x"]},"buildsFolder":"S:\\FortniteBuilds"}'
  'bad-account (masked)' = '{"displayName":"A","accountId":"XYZ","buildsFolder":"S:\\FortniteBuilds"}'
  'empty-object (masked)' = '{}'
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

'== --selftest-inject (core)'
$b = Run $new '--selftest-inject' $envNew
"  exit $($b.code) | $($b.text.Trim())"

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

'== --play through the stub (port 39551, root = fakeroot, rules in the temp group)'
$cfgText = [IO.File]::ReadAllText($cfgPath)
[IO.File]::WriteAllText($cfgPath, ($cfgText -replace '"port": 3551,', '"port": 39551,'), $utf8)
$b = Run $new '--play rlcoretest-17.50' $envFw 60000
"  exit $($b.code)"; Show 'core --play' $b.text
if ($b.err) { '  stderr: ' + $b.err.Trim() }
$left = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($t, [StringComparison]::OrdinalIgnoreCase) })
"  processes left from $t after --play: $($left.Count) $(($left | ForEach-Object { $_.Name }) -join ', ')"
[IO.File]::WriteAllText($cfgPath, $cfgText, $utf8)

$b = Run $new '--firewall-remove' $envFw
"  --firewall-remove: exit $($b.code) | $($b.text.Trim())"
$realAfter = GroupCount $realGroup
"temp group after: $(GroupCount $fwGroup); real group after: $realAfter (before $realBefore) -> $(if ($realAfter -eq $realBefore) { 'UNCHANGED' } else { 'CHANGED!' })"

if (-not $SkipNetwork) {
  '== --cosmetics on a fake 3.6 build (AES from GitHub, generator fails: no paks)'
  $off = "$fake\rlcoretest-3.6\Fortnite\offline"
  $a = Run $old '--cosmetics rlcoretest-3.6' @{} 300000
  $logA = if (Test-Path "$off\cosmetics-gen.log") { (Get-Content "$off\cosmetics-gen.log").Count } else { 0 }
  Remove-Item "$off\aes.json", "$off\cosmetics-gen.log" -ErrorAction SilentlyContinue
  $b = Run $new '--cosmetics rlcoretest-3.6' $envNew 300000
  $logB = if (Test-Path "$off\cosmetics-gen.log") { (Get-Content "$off\cosmetics-gen.log").Count } else { 0 }
  CompareRun '--cosmetics rlcoretest-3.6' $a $b
  "  cosmetics-gen.log lines 2.2.1 $logA / core $logB"
  if (Same $a.bytes $b.bytes) { Show 'output' $b.text }

  '== --update-check'
  $a = Run $old '--update-check'; $b = Run $new '--update-check' $envNew; $c = Run $new220 '--update-check' $envNew
  "  2.2.1          exit $($a.code) | $($a.text.Trim())"
  "  core 3.0.0     exit $($b.code) | $($b.text.Trim())"
  "  core as 2.2.0  exit $($c.code) | $($c.text.Trim())"
  Get-Content (Join-Path $ref 'update.log') -Tail 3 | ForEach-Object { '  update.log: ' + $_ }
}

[IO.File]::WriteAllBytes($cfgPath, $backup)
"config restored: $(Same $backup ([IO.File]::ReadAllBytes($cfgPath)))"
$left = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($t, [StringComparison]::OrdinalIgnoreCase) })
"processes left from ${t}: $($left.Count)"
$left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "  killed own $($_.Name) $($_.ProcessId)" }
