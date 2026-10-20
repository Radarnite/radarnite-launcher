<#
  measure-backend.ps1 - backend start-up cost split by the build's offline data (see README.md).
    powershell -NoProfile -ExecutionPolicy Bypass -File measure-backend.ps1 -Node <node.exe> -Server <server.mjs>
  Runs `node server.mjs --build <scratch> --config <scratch config> --port <free>` against three scratch copies of
  <Builds>\<Build>\offline: full (cosmetics.json, custom-cosmetics.json, state.json), without custom-cosmetics.json,
  and empty. Ready = process creation -> "ready 127.0.0.1:PORT"; RSS / private = 300 ms after ready. Never touches
  the real build folder. Prints JSON.
#>
param([string]$Node, [string]$Server, [string]$Root = 'S:\rl-release-test', [string]$Builds = 'S:\FortniteBuilds',
      [string]$Build = '14.30', [int]$Runs = 7)
$ErrorActionPreference = 'Stop'
$src = Join-Path $Builds "$Build\offline"
$cfg = Join-Path $Root 'backend-config.json'
Set-Content $cfg '{ "displayName": "Baseline", "port": 3551 }' -Encoding ASCII
$variants = [ordered]@{ full = @('cosmetics.json', 'custom-cosmetics.json', 'state.json'); noCustomCosmetics = @('cosmetics.json', 'state.json'); empty = @() }
$l = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0); $l.Start(); $port = $l.LocalEndpoint.Port; $l.Stop()
$res = [ordered]@{ nodeVersion = (& $Node --version); server = (Get-FileHash $Server -Algorithm SHA256).Hash.ToLower() }
foreach ($v in $variants.Keys) {
  $b = Join-Path $Root "backend-$v\$Build"
  Remove-Item -Recurse -Force (Split-Path $b) -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force (Join-Path $b 'offline') | Out-Null
  $bytes = 0
  foreach ($f in $variants[$v]) { $s = Join-Path $src $f; if (Test-Path $s) { Copy-Item $s (Join-Path $b "offline\$f"); $bytes += (Get-Item $s).Length } }
  $ready = @(); $rss = @(); $priv = @()
  for ($i = 0; $i -lt $Runs + 2; $i++) {
    $psi = New-Object Diagnostics.ProcessStartInfo $Node, "`"$Server`" --build `"$b`" --config `"$cfg`" --port $port"
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $sw = [Diagnostics.Stopwatch]::StartNew(); $p = [Diagnostics.Process]::Start($psi)
    try {
      $line = $p.StandardOutput.ReadLine(); $t = $sw.Elapsed.TotalMilliseconds
      if ($line -notlike 'ready *') { throw "backend said: $line" }
      Start-Sleep -Milliseconds 300; $p.Refresh()
      if ($i -ge 2) { $ready += $t; $rss += $p.WorkingSet64 / 1MB; $priv += $p.PrivateMemorySize64 / 1MB }
    } finally { try { $p.Kill() } catch { }; $p.WaitForExit(5000) | Out-Null }
  }
  $s = $ready | Sort-Object
  $res[$v] = [ordered]@{ dataMB = [math]::Round($bytes / 1MB, 1); files = $variants[$v]; runs = $Runs
    readyMsMedian = [math]::Round($s[[math]::Floor($s.Count / 2)], 1); readyMsMin = [math]::Round($s[0], 1); readyMsMax = [math]::Round($s[-1], 1)
    rssMB = [math]::Round(($rss | Measure-Object -Average).Average, 1); privateMB = [math]::Round(($priv | Measure-Object -Average).Average, 1) }
  Remove-Item -Recurse -Force (Split-Path $b) -ErrorAction SilentlyContinue
}
Remove-Item $cfg -ErrorAction SilentlyContinue
$res | ConvertTo-Json -Depth 5
