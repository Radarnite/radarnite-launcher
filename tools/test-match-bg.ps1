<#
test-match-bg.ps1 - one local match of a build, in the background, for the developer tests (`just test-match-pc`).
Run inside the signed-in session (a hidden scheduled task: the game needs the desktop for its swap chain), never by
hand while someone plays.

  -Root      work folder: rc\ (a launcher folder: node, backend, redirect, patches, gameserver, radarnite-play.exe),
             the run's output goes to out\
  -Build     the real build folder; the run uses <Root>\build, junctions to its Engine and FortniteGame and its own
             offline\ (copies of the build's cosmetics lists, never written back), so the owner's profile, logs and
             sandboxes are not touched
  -Minutes   how long the match runs before everything is closed
  -Env       extra environment for the game processes, "NAME=value;NAME=value" (FNRAGDOLL, FNOFFLINE_TEST_DEATHS, ...)
  -BotsAI    1 = the bots use the game's own AI
  -BotSkill  1-10, their difficulty (default 6)
  -AutoStart seconds after joining until the bus leaves (0 = it stays in the warmup)
  -PlayMode  config.json "playMode" of the run (legal, legal-vbucks, unlocked; empty = none), profile "bgtest"

Every window of the game is kept off the screen and non-activating (-WinX/-WinY, then WS_EX_NOACTIVATE and SetWindowPos
with SWP_NOACTIVATE), without sound (-nosound), at idle priority; if one ever takes the foreground anyway, the window
that had it gets it back (logged). Nothing here sends input: the
client joins the match through fnbody.dll's FNTEST_JOIN (a flag file this script creates once the server listens).
Every process the run started is closed at the end, whatever happens (finally): radarnite-play's tree, the backend and
any game process started from <Root>\build.
#>
param(
    [string]$Root = 'S:\rl-integ-test-143',
    [string]$Build = 'S:\FortniteBuilds\14.30',
    [int]$Minutes = 4,
    [int]$Bots = 6,
    [int]$Port = 3561,
    [string]$Env = '',
    [string]$PlayMode = '',
    [int]$AutoStart = 0,
    [int]$BotsAI = 0,
    [int]$BotSkill = 6
)
$ErrorActionPreference = 'Stop'
$out = Join-Path $Root 'out'
$b = Join-Path $Root 'build'
$rc = Join-Path $Root 'rc'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out 'run.log'
function Log($m) { "$(Get-Date -f HH:mm:ss.fff) $m" | Add-Content $log }

Add-Type @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class BgWin {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] static extern IntPtr GetLong(IntPtr h, int i);
  [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")] static extern IntPtr SetLong(IntPtr h, int i, IntPtr v);
  delegate bool EnumProc(IntPtr h, IntPtr l);
  // every top-level window of the process: WS_EX_NOACTIVATE (clicks and the system never activate it), off the
  // screen at the bottom of the z-order without activating it. Returns the windows newly marked.
  public static int Tame(int pid) {
    var hs = new List<IntPtr>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == (uint)pid) hs.Add(h); return true; }, IntPtr.Zero);
    int marked = 0;
    foreach (var h in hs) {
      long ex = GetLong(h, -20).ToInt64();
      if ((ex & 0x08000000) == 0) { SetLong(h, -20, new IntPtr(ex | 0x08000000)); marked++; }
      SetWindowPos(h, new IntPtr(1), -4000, -4000, 0, 0, 0x0010 | 0x0200 | 0x0001);
    }
    return marked;
  }
  public static int Owner(IntPtr h, int[] pids) { uint p; GetWindowThreadProcessId(h, out p); return Array.IndexOf(pids, (int)p) >= 0 ? (int)p : 0; }
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int cmd);
}
'@
[void][BgWin]::SetProcessDPIAware()

# ---- the run's build folder: the real game files through junctions, its own offline\
New-Item -ItemType Directory -Force $b | Out-Null
foreach ($d in 'Engine', 'FortniteGame') {
    if (-not (Test-Path (Join-Path $b $d))) { cmd /c mklink /J "$(Join-Path $b $d)" "$(Join-Path $Build $d)" | Out-Null }
}
$off = Join-Path $b 'offline'
New-Item -ItemType Directory -Force $off | Out-Null
foreach ($f in 'build.json', 'aes.json', 'keychain.json', 'cosmetics.json', 'custom-cosmetics.json') {
    $src = Join-Path (Join-Path $Build 'offline') $f
    if (Test-Path $src) { Copy-Item -Force $src $off }
}
foreach ($d in 'sandbox-client', 'sandbox-server') { $p = Join-Path $off $d; if (Test-Path $p) { Remove-Item -Recurse -Force $p } }
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $off '*.log')
foreach ($d in 'results', 'profiles') { $p = Join-Path $off $d; if (Test-Path $p) { Remove-Item -Recurse -Force $p } }
$pm = if ($PlayMode) { ',"playMode":"' + $PlayMode + '","profile":"bgtest"' } else { '' }
'{"displayName":"bgtest","port":' + $Port + $pm + '}' | Set-Content -Encoding ascii (Join-Path $rc 'config.json')
$flag = Join-Path $out 'join.flag'

# ---- environment of everything started below
$env:FNOFFLINE_EXTRA_ARGS = '-windowed -ResX=640 -ResY=360 -WinX=-4000 -WinY=-4000 -nosound'
$env:FNTEST_JOIN = $flag
foreach ($kv in ($Env -split ';')) { if ($kv -match '^\s*([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1].Trim())" -Value $Matches[2] } }
Log "env: $Env"

$started = @()
$t0 = Get-Date
$prevFg = [BgWin]::GetForegroundWindow()
try {
    $node = Start-Process -FilePath (Join-Path $rc 'node\node.exe') -ArgumentList "`"$(Join-Path $rc 'backend\server.mjs')`" --build `"$b`" --config `"$(Join-Path $rc 'config.json')`" --port $Port" `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out 'backend-stdout.txt') -RedirectStandardError (Join-Path $out 'backend-stderr.txt')
    $started += $node.Id
    try { $node.PriorityClass = 'BelowNormal' } catch {}
    Start-Sleep 3
    $play = Start-Process -FilePath (Join-Path $rc 'radarnite-play.exe') -WindowStyle Hidden -PassThru `
        -ArgumentList "--build `"$b`" --launcher-dir `"$rc`" --port $Port --name bgtest --bots $Bots --bots-ai $BotsAI --bot-skill $BotSkill --autostart $AutoStart --last-playlist playlist_defaultsolo --infinite-materials 1" `
        -RedirectStandardOutput (Join-Path $out 'play.txt') -RedirectStandardError (Join-Path $out 'play-err.txt')
    $started += $play.Id
    Log "backend $($node.Id), radarnite-play $($play.Id)"
    $seen = @{}
    $ready = $false
    while (((Get-Date) - $t0).TotalMinutes -lt $Minutes -and -not $play.HasExited) {
        foreach ($p in (Get-Process FortniteClient-Win64-Shipping -ErrorAction SilentlyContinue)) {
            $path = $null; try { $path = $p.Path } catch {}
            if (-not $path -or -not $path.StartsWith($b, [StringComparison]::OrdinalIgnoreCase)) { continue }
            if (-not $seen.ContainsKey($p.Id)) {
                # the match server (-nullrhi) keeps its windows hidden itself (radarnite-play); restyling or moving them
                # made it quit or crash while its DLL started
                $cl = (Get-CimInstance Win32_Process -Filter "ProcessId = $($p.Id)" -ErrorAction SilentlyContinue).CommandLine
                $seen[$p.Id] = -not ($cl -and $cl.Contains('-nullrhi'))
                Log "game process $($p.Id) ($(if ($seen[$p.Id]) { 'client' } else { 'match server' }))"
            }
            # the match server keeps the launcher's priority until it is ready: at idle priority its DLL took over 20 s
            # to start, the map load ran into it and the server quit or crashed
            if ($seen[$p.Id] -or $ready) { try { if ($p.PriorityClass -ne 'Idle') { $p.PriorityClass = 'Idle' } } catch {} }
            if (-not $seen[$p.Id]) { continue }
            $m = [BgWin]::Tame($p.Id)
            if ($m) { Log "game process $($p.Id): $m window(s) made non-activating" }
        }
        $fg = [BgWin]::GetForegroundWindow()
        $owner = [BgWin]::Owner($fg, [int[]]@($seen.Keys))
        if ($owner) {
            # giving it back can be refused (foreground lock); minimizing the window activates the one under it
            $ok = $prevFg -ne [IntPtr]::Zero -and [BgWin]::SetForegroundWindow($prevFg)
            if (-not $ok) { [void][BgWin]::ShowWindowAsync($fg, 6) }
            Log "game process $owner had the foreground: $(if ($ok) { 'given back' } else { 'window minimized' })"
        } else { $prevFg = $fg }
        if (-not $ready -and (Test-Path (Join-Path $out 'play.txt')) -and (Select-String -Quiet -Path (Join-Path $out 'play.txt') -Pattern 'Match server ready')) {
            $ready = $true; Set-Content $flag 'go'; Log 'match server ready: join flag set'
        }
        Start-Sleep -Milliseconds 200
    }
    Log "run over (radarnite-play exited: $($play.HasExited))"
}
catch { Log "ERROR $_" }
finally {
    foreach ($id in $started) { cmd /c "taskkill /T /F /PID $id >nul 2>&1" }
    Start-Sleep 2
    # anything still alive that this run started from its build folder or its launcher folder
    Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and ($_.ExecutablePath.StartsWith($b, [StringComparison]::OrdinalIgnoreCase) -or $_.ExecutablePath.StartsWith($rc, [StringComparison]::OrdinalIgnoreCase)) } |
        ForEach-Object { Log "killing leftover $($_.ProcessId) $($_.Name)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    # the suspended stubs (FortniteLauncher, *_EAC) have no readable path: those whose parent was one of ours
    $ids = @($started) + @($seen.Keys)
    Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'Fortnite*' -and $ids -contains [int]$_.ParentProcessId } |
        ForEach-Object { Log "killing stub $($_.ProcessId) $($_.Name)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:FNTEST_JOIN, Env:FNOFFLINE_EXTRA_ARGS -ErrorAction SilentlyContinue

    # ---- results
    $sc = Join-Path $off 'sandbox-client'
    Copy-Item -ErrorAction SilentlyContinue (Join-Path $sc 'fnbody.log') $out
    Copy-Item -ErrorAction SilentlyContinue (Join-Path $off 'backend.log') $out
    Copy-Item -ErrorAction SilentlyContinue (Join-Path $off 'unknown-routes.log') $out
    foreach ($d in 'results', 'profiles') { if (Test-Path (Join-Path $off $d)) { Copy-Item -Recurse -Force (Join-Path $off $d) $out } }
    Copy-Item -ErrorAction SilentlyContinue (Join-Path $b 'FortniteGame\Binaries\Win64\radarnite-server.log') $out
    Copy-Item -ErrorAction SilentlyContinue (Join-Path $sc 'LocalAppData\FortniteGame\Saved\Logs\FortniteGame.log') (Join-Path $out 'client.log')
    Copy-Item -ErrorAction SilentlyContinue (Join-Path $off 'sandbox-server\LocalAppData\FortniteGame\Saved\Logs\FortniteGame.log') (Join-Path $out 'server.log')
    $shots = Get-ChildItem -ErrorAction SilentlyContinue -Recurse (Join-Path $sc 'LocalAppData\FortniteGame\Saved\Screenshots') -Filter *.png | Sort-Object LastWriteTime
    $i = 0
    New-Item -ItemType Directory -Force (Join-Path $out 'shots') | Out-Null
    foreach ($s in $shots) { $i++; Copy-Item $s.FullName (Join-Path $out ('shots\{0:d4}.png' -f $i)) }
    Log "$i screenshots"
    $ff = Join-Path $env:LOCALAPPDATA 'fp-tools\ffmpeg\bin\ffmpeg.exe'
    if (-not (Test-Path $ff)) { $ff = (Get-ChildItem -Recurse (Join-Path $env:LOCALAPPDATA 'fp-tools\ffmpeg') -Filter ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }
    if ($i -ge 4 -and $ff) {
        & $ff -hide_banner -loglevel error -y -framerate 2.5 -i (Join-Path $out 'shots\%04d.png') -vf 'scale=640:-2' -c:v libx264 -pix_fmt yuv420p -crf 30 (Join-Path $out 'clip.mp4') 2>&1 | ForEach-Object { Log "ffmpeg: $_" }
    }
    Log 'done'
    Set-Content (Join-Path $out 'done.flag') 'done'
}
