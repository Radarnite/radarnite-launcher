<#
  session-bench.ps1 - window part of the 2.x baseline (see README.md). Runs in the signed-in user's session (started
  by measure-2x.ps1 through a scheduled task + a hidden wscript launcher), because over ssh everything is session 0:
  no desktop and no GPU.

  1. RadarniteLauncher.exe --selftest-ui <png> (the launcher's own developer mode: the real window at -32000,-32000,
     no taskbar button, no single-instance mutex; it closes itself about 2 s after the page reported "ready").
     windowVisibleMs = process creation -> first visible top-level window of the process (the blank WinForms frame).
     pageReadyMs     = process creation -> page "ready", taken as (PNG creation time - 2000 ms) because the launcher
                       creates the PNG exactly 2000 ms (Task.Delay) after "ready".
  2. The same with RADARNITE_SELFTEST_SLEEP=1 once: the launcher's own memory report (memAwake = launcher + WebView2
     processes, about 2 s after "ready").
  3. The normal main window (--new-instance: no mutex, no listener) on a separate, never-shown Win32 desktop, so it
     is fully awake (not minimized, so 2.2.1's dormant mode does not kick in) yet invisible: private bytes of the
     process tree at 10 s, idle CPU of the tree from 10 s to 20 s. One --selftest-ui run on that desktop as well, to
     compare against the normal desktop.
  Every process started here (and its descendants, and any msedgewebview2 using <Inst>\webview2-radarnite) is killed
  at the end. Nothing else is touched.
#>
param([string]$Inst, [string]$Out, [int]$SelfTests = 5)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Inst 'RadarniteLauncher.exe'
$udf = Join-Path $Inst 'webview2-radarnite'
$res = [ordered]@{ session = (Get-Process -Id $PID).SessionId }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class RlBench {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct STARTUPINFO { public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
  [StructLayout(LayoutKind.Sequential)]
  public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  public static extern IntPtr CreateDesktopW(string name, IntPtr dev, IntPtr dm, int flags, uint access, IntPtr sa);
  [DllImport("user32.dll", SetLastError = true)] public static extern bool CloseDesktop(IntPtr h);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
    IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr desk, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  public static bool HasVisibleWindow(IntPtr desk, int pid) {
    bool found = false;
    EnumProc f = (h, l) => { int p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) { found = true; return false; } return true; };
    if (desk == IntPtr.Zero) EnumWindows(f, IntPtr.Zero); else EnumDesktopWindows(desk, f, IntPtr.Zero);
    return found;
  }
  // desktop null = the caller's desktop
  public static int Start(string cmdLine, string cwd, string desktop) {
    var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si); si.lpDesktop = desktop;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(null, new StringBuilder(cmdLine), IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi))
      throw new System.ComponentModel.Win32Exception();
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return pi.dwProcessId;
  }
}
'@

function Tree([int]$root) {
  # root + descendants (by parent id, created after the root), + any msedgewebview2 on our user data folder
  $all = @(Get-CimInstance Win32_Process)
  $rootProc = $all | Where-Object { $_.ProcessId -eq $root }
  $since = if ($rootProc) { $rootProc.CreationDate } else { (Get-Date).AddMinutes(-10) }
  $ids = New-Object 'System.Collections.Generic.HashSet[int]'
  if ($rootProc) { [void]$ids.Add($root) }
  do {
    $added = 0
    foreach ($p in $all) {
      if (-not $ids.Contains([int]$p.ProcessId) -and $ids.Contains([int]$p.ParentProcessId) -and $p.CreationDate -ge $since) { [void]$ids.Add([int]$p.ProcessId); $added++ }
    }
  } while ($added -gt 0)
  foreach ($p in $all) {
    if ($p.Name -eq 'msedgewebview2.exe' -and $p.CommandLine -and $p.CommandLine.IndexOf($udf, [StringComparison]::OrdinalIgnoreCase) -ge 0) { [void]$ids.Add([int]$p.ProcessId) }
  }
  $all | Where-Object { $ids.Contains([int]$_.ProcessId) }
}

function Sample([int]$root) {
  $rows = foreach ($c in (Tree $root)) {
    try {
      $p = [Diagnostics.Process]::GetProcessById([int]$c.ProcessId); $p.Refresh()
      $type = 'launcher'
      if ($c.Name -eq 'msedgewebview2.exe') { $type = 'webview2'; if ($c.CommandLine -match '--type=([a-z-]+)') { $type = 'webview2-' + $Matches[1] } }
      elseif ($c.ProcessId -ne $root) { $type = 'other-' + $c.Name }
      [pscustomobject]@{ pid = [int]$c.ProcessId; type = $type; priv = $p.PrivateMemorySize64; ws = $p.WorkingSet64; cpuMs = $p.TotalProcessorTime.TotalMilliseconds }
    } catch { }
  }
  @($rows)
}

function KillTree([int]$root) {
  foreach ($c in (Tree $root)) { try { Stop-Process -Id $c.ProcessId -Force -ErrorAction Stop } catch { } }
}

function WaitExit([int]$id, [int]$seconds) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $seconds) {
    if (-not (Get-Process -Id $id -ErrorAction SilentlyContinue)) { return $true }
    Start-Sleep -Milliseconds 200
  }
  return $false
}

function SelfTest([string]$tag, [string]$desktopName, [IntPtr]$desk) {
  $png = Join-Path $Out "selftest-$tag.png"
  Remove-Item $png, ([IO.Path]::ChangeExtension($png, '.json')) -ErrorAction SilentlyContinue
  $id = [RlBench]::Start("`"$exe`" --selftest-ui `"$png`"", $Inst, $desktopName)
  $start = [Diagnostics.Process]::GetProcessById($id).StartTime.ToUniversalTime()
  $visibleMs = $null
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt 30 -and $null -eq $visibleMs) {
    if ([RlBench]::HasVisibleWindow($desk, $id)) { $visibleMs = ([DateTime]::UtcNow - $start).TotalMilliseconds; break }
    if (-not (Get-Process -Id $id -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Milliseconds 2
  }
  $memPeak = $null
  # sample the tree once while the page is up (between "ready" and the capture)
  $sw2 = [Diagnostics.Stopwatch]::StartNew()
  while (-not (Test-Path $png) -and $sw2.Elapsed.TotalSeconds -lt 60 -and (Get-Process -Id $id -ErrorAction SilentlyContinue)) { Start-Sleep -Milliseconds 50 }
  $s = Sample $id
  if ($s.Count) { $memPeak = [math]::Round((($s | Measure-Object priv -Sum).Sum) / 1MB, 1) }
  $exited = WaitExit $id 120
  if (-not $exited) { KillTree $id }
  Start-Sleep -Milliseconds 500
  KillTree $id   # WebView2 children that outlived the host, if any
  $r = [ordered]@{ tag = $tag; desktop = $(if ($desktopName) { 'hidden' } else { 'default' }); exitedByItself = $exited
    windowVisibleMs = $(if ($null -ne $visibleMs) { [math]::Round($visibleMs, 1) } else { $null }) }
  if (Test-Path $png) {
    $r.pageReadyMs = [math]::Round(((Get-Item $png).CreationTimeUtc - $start).TotalMilliseconds - 2000, 1)
    $r.treePrivateMBAtCapture = $memPeak
    $r.processesAtCapture = $s.Count
  } else { $r.pageReadyMs = $null; $r.note = 'no PNG produced' }
  $j = [IO.Path]::ChangeExtension($png, '.json')
  if (Test-Path $j) { $r.selftestReport = Get-Content $j -Raw | ConvertFrom-Json }
  [pscustomobject]$r
}

$started = New-Object System.Collections.Generic.List[int]
$desk = [IntPtr]::Zero
try {
  # --- 1. selftest-ui on the normal desktop (off-screen window) ---
  $runs = @()
  for ($i = 1; $i -le $SelfTests; $i++) { $runs += SelfTest "default-$i" $null ([IntPtr]::Zero) }
  $res.selftests = $runs

  # --- 2. the launcher's own memory report (memAwake), once ---
  $env:RADARNITE_SELFTEST_SLEEP = '1'
  try { $res.selftestSleep = SelfTest 'sleep' $null ([IntPtr]::Zero) } finally { Remove-Item Env:\RADARNITE_SELFTEST_SLEEP -ErrorAction SilentlyContinue }

  # --- 3. main window on a separate, never-shown desktop ---
  $deskName = "rl-bench-$PID"
  $desk = [RlBench]::CreateDesktopW($deskName, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)
  if ($desk -eq [IntPtr]::Zero) { throw "CreateDesktop failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
  $full = "WinSta0\$deskName"
  $res.selftestHiddenDesktop = SelfTest 'hidden' $full $desk

  $errLog = Join-Path $Inst 'ui-error.log'
  $errBefore = if (Test-Path $errLog) { (Get-Item $errLog).Length } else { 0 }
  $id = [RlBench]::Start("`"$exe`" --new-instance", $Inst, $full)
  $started.Add($id)
  $start = [Diagnostics.Process]::GetProcessById($id).StartTime.ToUniversalTime()
  $visibleMs = $null
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt 30) {
    if ([RlBench]::HasVisibleWindow($desk, $id)) { $visibleMs = ([DateTime]::UtcNow - $start).TotalMilliseconds; break }
    Start-Sleep -Milliseconds 2
  }
  function Until([double]$sec) { $w = $sec * 1000 - ([DateTime]::UtcNow - $start).TotalMilliseconds; if ($w -gt 0) { Start-Sleep -Milliseconds ([int]$w) } }
  Until 10
  $s10 = Sample $id; $t10 = ([DateTime]::UtcNow - $start).TotalMilliseconds
  Until 20
  $s20 = Sample $id; $t20 = ([DateTime]::UtcNow - $start).TotalMilliseconds
  $byType = $s10 | Group-Object type | ForEach-Object { [ordered]@{ type = $_.Name; processes = $_.Count
      privateMB = [math]::Round((($_.Group | Measure-Object priv -Sum).Sum) / 1MB, 1)
      workingSetMB = [math]::Round((($_.Group | Measure-Object ws -Sum).Sum) / 1MB, 1) } }
  # idle CPU: CPU time of the tree between the two samples (processes present at 20 s; new ones count in full)
  $cpu10 = @{}; foreach ($x in $s10) { $cpu10[$x.pid] = $x.cpuMs }
  $cpuDelta = 0.0; foreach ($x in $s20) { $base = 0.0; if ($cpu10.ContainsKey($x.pid)) { $base = $cpu10[$x.pid] }; $cpuDelta += [math]::Max(0, $x.cpuMs - $base) }
  $wall = $t20 - $t10
  $cores = [Environment]::ProcessorCount
  $res.mainWindow = [ordered]@{
    desktop = 'hidden'; windowVisibleMs = $(if ($null -ne $visibleMs) { [math]::Round($visibleMs, 1) } else { $null })
    sampleAtMs = [math]::Round($t10); processesAt10s = $s10.Count
    treePrivateMBAt10s = [math]::Round((($s10 | Measure-Object priv -Sum).Sum) / 1MB, 1)
    treeWorkingSetMBAt10s = [math]::Round((($s10 | Measure-Object ws -Sum).Sum) / 1MB, 1)
    launcherPrivateMBAt10s = [math]::Round((($s10 | Where-Object type -eq 'launcher' | Measure-Object priv -Sum).Sum) / 1MB, 1)
    byType = @($byType)
    idleWindowMs = [math]::Round($wall); idleCpuMs = [math]::Round($cpuDelta, 1)
    idleCpuPctOfOneCore = [math]::Round($cpuDelta / $wall * 100, 2)
    idleCpuPctOfMachine = [math]::Round($cpuDelta / $wall * 100 / $cores, 3); logicalCores = $cores
    webview2FailedOver = $(if (Test-Path $errLog) { (Get-Item $errLog).Length -gt $errBefore } else { $false })
  }
  KillTree $id
} catch {
  $res.error = "$($_.Exception.Message) @ $($_.InvocationInfo.ScriptLineNumber)"
} finally {
  foreach ($id in $started) { KillTree $id }
  # stray WebView2 processes of our user data folder only
  Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.IndexOf($udf, [StringComparison]::OrdinalIgnoreCase) -ge 0 } |
    ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch { } }
  Get-CimInstance Win32_Process -Filter "Name='RadarniteLauncher.exe'" | Where-Object { $_.ExecutablePath -eq $exe } |
    ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch { } }
  if ($desk -ne [IntPtr]::Zero) { Start-Sleep -Milliseconds 500; [void][RlBench]::CloseDesktop($desk) }
  $res | ConvertTo-Json -Depth 8 | Set-Content -Path (Join-Path $Out 'window.json') -Encoding UTF8
}
