# smoke-pc.ps1: runs radarnite-backend.exe (and, for comparison, the launcher's node + server.mjs) on throwaway build
# folders under -Dir, hidden, and prints one JSON report: readiness, memory, idle CPU, sockets, and the normalized
# answers of both side by side (diffs listed). Everything it starts is stopped at the end; nothing outside -Dir is written.
# Nothing under S:\FortniteBuilds is read or run (the game may be open): Node is the portable one in fp-tools and
# server.mjs / cosmetics.json are copies placed in -Dir by the justfile recipe.
# -Cosmetics <cosmetics.json>: a big list for a realistic athena profile (default: one item).
# -BuildPc <build folder>: its offline\cosmetics.json and custom-cosmetics.json are copied (read only, and only while
# Fortnite is closed) instead, e.g. S:\FortniteBuilds\14.30 with its ~90 MB custom-cosmetics.json.
param(
  [string]$Dir = 'S:\rl-backend-test',
  [string]$Cosmetics = '',
  [string]$BuildPc = '',
  [string]$Node = "$env:LOCALAPPDATA\fp-tools\node\node.exe",
  [string]$ServerMjs = ''
)
if (-not $ServerMjs) { $ServerMjs = Join-Path $Dir 'server.mjs' }
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http
$acc = '0123456789abcdef0123456789abcdef'
$ua = 'Fortnite/++Fortnite+Release-17.50-CL-17388565 Windows/10.0.19045.1.256.64bit'
$exe = Join-Path $Dir 'radarnite-backend.exe'
$report = [ordered]@{ exeBytes = (Get-Item $exe).Length; buildPc = $BuildPc }
$started = @()

# processes left by an earlier run of this script (only ours: their command line points into -Dir)
function Stop-Leftovers {
  $mine = @(Get-CimInstance Win32_Process -Filter "Name='node.exe' OR Name='radarnite-backend.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($Dir) })
  foreach ($p in $mine) { try { Stop-Process -Id $p.ProcessId -Force } catch {} }
  $mine.Count
}
$report.leftoversStopped = Stop-Leftovers

function New-Build($name) {
  $root = Join-Path $Dir $name
  if (Test-Path $root) { Remove-Item -Recurse -Force $root }
  $off = Join-Path $root 'build\offline'
  New-Item -ItemType Directory -Force (Join-Path $off 'hotfix') | Out-Null
  [IO.File]::WriteAllText((Join-Path $root 'config.json'), "{`"displayName`":`"Smoke`",`"accountId`":`"$acc`",`"_help`":`"x`"}")
  if ($BuildPc) {
    if (Get-Process FortniteClient-Win64-Shipping -ErrorAction SilentlyContinue) { throw 'Fortnite is running: the build folder is not read' }
    foreach ($f in 'cosmetics.json', 'custom-cosmetics.json') { if (Test-Path "$BuildPc\offline\$f") { Copy-Item "$BuildPc\offline\$f" (Join-Path $off $f) } }
  }
  elseif ($Cosmetics -and (Test-Path $Cosmetics)) { Copy-Item $Cosmetics (Join-Path $off 'cosmetics.json') }
  else { [IO.File]::WriteAllText((Join-Path $off 'cosmetics.json'), '{"source":"smoke","items":[{"templateId":"AthenaCharacter:CID_A_085_Athena_Commando_F_Hardwood_K7ZZ1","variants":[{"channel":"Material","active":"Mat1","owned":["Mat1","Mat2"]}]}]}') }
  foreach ($f in 'b.ini', 'A.ini', 'DefaultEngine.ini') { [IO.File]::WriteAllText((Join-Path $off "hotfix\$f"), "[$f]") }
  return $root
}

function Get-FreePort { $l = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0); $l.Start(); $p = $l.LocalEndpoint.Port; $l.Stop(); $p }

function Read-Shared($path) {
  if (-not (Test-Path $path)) { return '' }
  $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
  try { (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
}

function Start-Backend($kind, $root) {
  $port = Get-FreePort
  $a = @('--build', "`"$root\build`"", '--config', "`"$root\config.json`"", '--port', $port)
  $out = Join-Path $root 'stdout.txt'
  $sw = [Diagnostics.Stopwatch]::StartNew()
  if ($kind -eq 'rust') { $p = Start-Process -FilePath $exe -ArgumentList $a -RedirectStandardOutput $out -RedirectStandardError "$root\stderr.txt" -WindowStyle Hidden -PassThru }
  else { $p = Start-Process -FilePath $Node -ArgumentList (@("`"$ServerMjs`"") + $a) -RedirectStandardOutput $out -RedirectStandardError "$root\stderr.txt" -WindowStyle Hidden -PassThru }
  $script:started += $p
  while ($sw.ElapsedMilliseconds -lt 15000 -and -not (Read-Shared $out).Contains("`n")) { Start-Sleep -Milliseconds 2 }
  $readyMs = $sw.ElapsedMilliseconds
  # the backend's own "listening on" log time minus the process start: readiness without PowerShell's overhead
  $first = (Read-Shared "$root\build\offline\backend.log") -split "`n" | Where-Object { $_ -match ' listening on ' } | Select-Object -First 1
  $fromStart = if ($first) { [math]::Round(([DateTime]::Parse($first.Substring(0, 24)).ToUniversalTime() - $p.StartTime.ToUniversalTime()).TotalMilliseconds, 1) } else { $null }
  [pscustomobject]@{ p = $p; port = $port; root = $root; line = (Read-Shared $out).Trim(); readyMs = $readyMs; readyFromStartMs = $fromStart }
}

$http = [Net.Http.HttpClient]::new()
$null = $http.DefaultRequestHeaders.TryAddWithoutValidation('User-Agent', $ua)
function Call($b, $method, $path, $body = $null) {
  $req = [Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::new($method), "http://127.0.0.1:$($b.port)$path")
  if ($null -ne $body) { $req.Content = [Net.Http.StringContent]::new($body) }
  $r = $http.SendAsync($req).GetAwaiter().GetResult()
  "$([int]$r.StatusCode) " + $r.Content.ReadAsStringAsync().GetAwaiter().GetResult()
}

function Norm($s) {
  $s = $s -replace '\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z', '<T>'
  $s = $s -replace 'eg1~offline-[0-9a-f]{32}', '<TOK>' -replace 'offline-refresh-[0-9a-f]{16}', '<RT>'
  $s = $s -replace '"(id|ticketId|matchId|sessionId)":"[0-9a-f]{32}"', '"$1":"<ID>"' -replace ' id="[0-9a-f]{16}"', ' id="<XID>"'
  $s = $s -replace '"(readyAt|at)":\d{13}', '"$1":<MS>' -replace '"estimatedWaitSec":\d+', '"estimatedWaitSec":<N>'
  $s = $s -replace '127\.0\.0\.1:\d+', '127.0.0.1:<PORT>' -replace 'ServerPort=\d+', 'ServerPort=<PORT>'
  $s -replace '"hash":"[0-9a-f]{40}","hash256":"[0-9a-f]{64}"', '"hash":"<H>","hash256":"<H>"'
}

function Ws-Open($b, $path, $proto) {
  $ws = [Net.WebSockets.ClientWebSocket]::new()
  if ($proto) { $ws.Options.AddSubProtocol($proto) }
  $null = $ws.ConnectAsync([Uri]"ws://127.0.0.1:$($b.port)$path", [Threading.CancellationToken]::None).GetAwaiter().GetResult()
  $ws
}
function Ws-Send($ws, $text) {
  $bytes = [Text.Encoding]::UTF8.GetBytes($text)
  $null = $ws.SendAsync([ArraySegment[byte]]::new($bytes), 'Text', $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
}
function Ws-Recv($ws) {
  $buf = New-Object byte[] 65536
  $cts = [Threading.CancellationTokenSource]::new(6000)
  try { $r = $ws.ReceiveAsync([ArraySegment[byte]]::new($buf), $cts.Token).GetAwaiter().GetResult() } catch { return '<timeout>' }
  if ($r.MessageType -eq 'Close') { return "close:$([int]$r.CloseStatus)" }
  [Text.Encoding]::UTF8.GetString($buf, 0, $r.Count)
}

function Run-All($b) {
  $o = [ordered]@{}
  $P = "/fortnite/api/game/v2/profile/$acc/client"
  $o.health = Call $b GET '/offline/health'
  $o.token = Norm (Call $b POST '/account/api/oauth/token' 'grant_type=password&username=Smoke%40offline.local&password=x')
  $times = @()
  foreach ($i in 1..3) { $sw = [Diagnostics.Stopwatch]::StartNew(); $q = Call $b POST "$P/QueryProfile?profileId=athena&rvn=-1" '{}'; $times += $sw.ElapsedMilliseconds }
  $o.athenaMs = ($times | Measure-Object -Minimum).Minimum; $o.athenaBytes = $q.Length
  $o.athenaHash = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes((Norm $q)))).Replace('-', '').Substring(0, 16)
  $o.equip = (Norm (Call $b POST "$P/SetCosmeticLockerSlot?profileId=athena" '{"category":"Character","itemToSlot":"AthenaCharacter:CID_A_085_Athena_Commando_F_Hardwood_K7ZZ1","slotIndex":0,"variantUpdates":[{"channel":"Material","active":"Mat2","owned":[]}]}')).Length
  $o.keychainCount = ((Call $b GET '/fortnite/api/storefront/v2/keychain') -split '","').Count
  $o.timeline = (Norm (Call $b GET '/fortnite/api/calendar/v1/timeline')).Substring(0, 160)
  $o.mixedCase = (Call $b GET '/Fortnite/API/Version').Substring(0, 3)
  $o.unknown = (Call $b GET '/nope/x').Substring(0, 3)
  $o.hotfixList = Norm (Call $b GET '/fortnite/api/cloudstorage/system')
  $o.hotfixEngine = Norm (Call $b GET '/fortnite/api/cloudstorage/system/DefaultEngine.ini')
  $o.cloudPut = (Call $b PUT "/fortnite/api/cloudstorage/user/$acc/Client%20Settings.Sav" 'save').Substring(0, 3)
  $o.cloudList = Norm (Call $b GET "/fortnite/api/cloudstorage/user/$acc")
  $o.badUri = Call $b GET '/fortnite/api/cloudstorage/system/%E0%A4%A'
  # matchmaker: queued until a match server for the wanted playlist is ready
  $null = Call $b GET '/fortnite/api/game/v2/matchmakingservice/ticket/player/x?bucketId=17388565%3A1%3AEU%3Aplaylist_defaultsolo'
  $ws = Ws-Open $b '/matchmaking' $null
  $mm = @(Ws-Recv $ws; Ws-Recv $ws; Ws-Recv $ws)
  $null = Call $b POST '/offline/server-ready' '{"address":"127.0.0.1","port":7777,"playlist":"playlist_defaultsolo"}'
  $mm += @(Ws-Recv $ws; Ws-Recv $ws; Ws-Recv $ws)
  $ws.Dispose()
  $o.matchmaker = Norm ($mm -join ' | ')
  $o.session = Norm (Call $b GET '/fortnite/api/matchmaking/session/S1')
  # XMPP
  $x = Ws-Open $b '/' 'xmpp'
  $xs = @()
  Ws-Send $x '<open xmlns="urn:ietf:params:xml:ns:xmpp-framing" to="prod.ol.epicgames.com" version="1.0"/>'; $xs += Ws-Recv $x; $xs += Ws-Recv $x
  Ws-Send $x ('<auth mechanism="PLAIN" xmlns="urn:ietf:params:xml:ns:xmpp-sasl">' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("`0$acc`0token")) + '</auth>'); $xs += Ws-Recv $x
  Ws-Send $x '<iq type="set" id="b1"><bind xmlns="urn:ietf:params:xml:ns:xmpp-bind"><resource>V2:Fortnite:WIN::ABC</resource></bind></iq>'; $xs += Ws-Recv $x
  Ws-Send $x '<presence><status>{}</status></presence>'; $xs += Ws-Recv $x
  Ws-Send $x '<close xmlns="urn:ietf:params:xml:ns:xmpp-framing"/>'; $xs += Ws-Recv $x; $xs += Ws-Recv $x
  $x.Dispose()
  $o.xmpp = Norm ($xs -join ' | ')
  $o.state = Read-Shared (Join-Path $b.root 'build\offline\state.json')
  $o.unknownLog = Norm (Read-Shared (Join-Path $b.root 'build\offline\unknown-routes.log'))
  $o
}

function Measure-Proc($b) {
  $p = Get-Process -Id $b.p.Id
  $cpu0 = $p.TotalProcessorTime.TotalMilliseconds; Start-Sleep -Seconds 2; $p.Refresh()
  $listen = @(Get-NetTCPConnection -OwningProcess $b.p.Id -State Listen -ErrorAction SilentlyContinue | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" })
  $remote = @(Get-NetTCPConnection -OwningProcess $b.p.Id -ErrorAction SilentlyContinue | Where-Object { $_.State -ne 'Listen' -and $_.RemoteAddress -notin '127.0.0.1', '0.0.0.0', '::' })
  [ordered]@{ workingSetMB = [math]::Round($p.WorkingSet64 / 1MB, 1); privateMB = [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
    idleCpuMs2s = [math]::Round($p.TotalProcessorTime.TotalMilliseconds - $cpu0, 1); listen = $listen -join ','; remoteConnections = $remote.Count }
}

try {
  $r = Start-Backend 'rust' (New-Build 'rust')
  $report.rust = [ordered]@{ line = $r.line; readyMs = $r.readyMs; readyFromStartMs = $r.readyFromStartMs }
  $rr = Run-All $r
  $report.rust.mem = Measure-Proc $r
  $report.rust.athenaMs = $rr.athenaMs; $report.rust.athenaBytes = $rr.athenaBytes
  $report.rust.stderr = Read-Shared "$($r.root)\stderr.txt"
  $report.nodeUsed = (Test-Path $Node) -and (Test-Path $ServerMjs)
  if ($report.nodeUsed) {
    $n = Start-Backend 'node' (New-Build 'node')
    $report.node = [ordered]@{ line = $n.line; readyMs = $n.readyMs; readyFromStartMs = $n.readyFromStartMs }
    $nn = Run-All $n
    $report.node.mem = Measure-Proc $n
    $report.node.athenaMs = $nn.athenaMs; $report.node.athenaBytes = $nn.athenaBytes
    $diffs = @()
    foreach ($k in $rr.Keys) { if ($k -notin 'athenaMs' -and "$($rr[$k])" -ne "$($nn[$k])") { $diffs += [ordered]@{ key = $k; rust = "$($rr[$k])"; node = "$($nn[$k])" } } }
    $report.diffs = $diffs
    $report.sameAnswers = $diffs.Count -eq 0
  }
  $report.sample = [ordered]@{ hotfixList = $rr.hotfixList.Substring(0, [math]::Min(220, $rr.hotfixList.Length)); matchmaker = $rr.matchmaker; xmpp = $rr.xmpp.Substring(0, 200); state = $rr.state }
} finally {
  foreach ($p in $started) { try { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } catch {} }
  Start-Sleep -Milliseconds 300
  $report.leftRunning = @($started | Where-Object { -not $_.HasExited }).Count + (Stop-Leftovers)
  $http.Dispose()
}
$report | ConvertTo-Json -Depth 6
