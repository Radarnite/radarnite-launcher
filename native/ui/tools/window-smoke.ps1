<#
  window-smoke.ps1 - the real window (RadarniteLauncher.exe = native/ui + native/core) on the PC, next to 2.2.1 in the
  same job. Usage over ssh (session 0); it re-runs itself in the signed-in session through a scheduled task and a hidden
  wscript launcher, like baseline/measure-2x.ps1:
    powershell -NoProfile -ExecutionPolicy Bypass -File window-smoke.ps1 -Dir S:\rl-integ-test\smoke
        [-Ref <extracted 2.2.1 folder>] [-Bench <baseline\session-bench.ps1>] [-Builds S:\FortniteBuilds]
  <Dir>\app\RadarniteLauncher.exe must be there. Steps:
  1. --set-builds-folder <Builds> gives <Dir>\app its config.json (the installer's way).
  2. In the signed-in session, on a separate Win32 desktop that is never shown: RadarniteLauncher.exe --new-instance
     (no single-instance mutex, so a launcher already open is never signalled). The core scans <Builds> and checks
     for updates; nothing is launched or stopped. Measured like the 2.2.1 baseline: process creation -> window
     visible, private bytes of the process tree at 10 s, CPU time of the tree from 10 s to 20 s. Then WM_CLOSE: the
     window renders its current frame to a PNG (--snapshot; no screen capture sees Direct2D on a desktop DWM does not
     show) and must exit by itself. The PNG's pixel spread and colour count tell a real frame from a blank one.
  3. With -Ref and -Bench: session-bench.ps1 on the extracted 2.2.1 (its --selftest-ui runs and its main window on
     its own hidden desktop), so both are measured under the same load.
  Every process started here is killed by exact path at the end (other RadarniteLauncher.exe processes, e.g. an
  installed or dev-tree launcher, are never touched); HKCU\Software\Radarnite is removed only if it was not there
  before. Results: <Dir>\out\smoke.json, <Dir>\out\window-3.0.png, <Dir>\out\window.json (2.2.1).
#>
param([string]$Dir = 'S:\rl-integ-test\smoke', [string]$Ref = '', [string]$Bench = '', [string]$Builds = 'S:\FortniteBuilds',
      [switch]$InSession)
$ErrorActionPreference = 'Stop'
$app = Join-Path $Dir 'app'
$exe = Join-Path $app 'RadarniteLauncher.exe'
$out = Join-Path $Dir 'out'

function Under([string]$path, [string]$root) { $path -and $path.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) }
function Ours { @(Get-CimInstance Win32_Process | Where-Object { (Under $_.ExecutablePath $Dir) -or ($Ref -and (Under $_.ExecutablePath $Ref)) -or
    ($Ref -and $_.Name -eq 'msedgewebview2.exe' -and $_.CommandLine -and $_.CommandLine.IndexOf((Join-Path $Ref 'webview2-radarnite'), [StringComparison]::OrdinalIgnoreCase) -ge 0) }) }

function LoadTypes {
  Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text; using System.Drawing; using System.Drawing.Imaging;
public static class RlSmoke {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct STARTUPINFO { public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
  [StructLayout(LayoutKind.Sequential)] public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr CreateDesktopW(string n, IntPtr d, IntPtr m, int f, uint a, IntPtr s);
  [DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr h);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcessW(string a, StringBuilder c, IntPtr p, IntPtr t, bool i, uint f, IntPtr e, string d, ref STARTUPINFO s, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr desk, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static int Start(string cmd, string cwd, string desktop) { var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si); si.lpDesktop = desktop; PROCESS_INFORMATION pi;
    if (!CreateProcessW(null, new StringBuilder(cmd), IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi)) throw new System.ComponentModel.Win32Exception();
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return pi.dwProcessId; }
  // the first visible top-level window of pid on that desktop
  public static IntPtr VisibleWindow(IntPtr desk, int pid) { IntPtr found = IntPtr.Zero;
    EnumDesktopWindows(desk, (h, l) => { int p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) { found = h; return false; } return true; }, IntPtr.Zero);
    return found; }
  public static string ClassName(IntPtr h) { var s = new StringBuilder(256); GetClassNameW(h, s, 256); return s.ToString(); }
  public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowTextW(h, s, 256); return s.ToString(); }
  // "WxH stddev distinctColours" of a PNG (stddev of luminance on a 4 px grid)
  public static string Stats(string png) {
    using (var bmp = new Bitmap(png)) {
      int w = bmp.Width, ht = bmp.Height; double sum = 0, sq = 0; int n = 0; var colours = new System.Collections.Generic.HashSet<int>();
      for (int y = 0; y < ht; y += 4) for (int x = 0; x < w; x += 4) { var c = bmp.GetPixel(x, y); double l = 0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B; sum += l; sq += l * l; n++; if (colours.Count < 5000) colours.Add(c.ToArgb()); }
      double mean = sum / n; return w + "x" + ht + " " + Math.Sqrt(Math.Max(0, sq / n - mean * mean)).ToString("F1", System.Globalization.CultureInfo.InvariantCulture) + " " + colours.Count;
    } }
}
'@
}

if ($InSession) {
  # ------------------------------------------------------------------ signed-in session part
  $res = [ordered]@{ session = (Get-Process -Id $PID).SessionId }
  LoadTypes
  function Tree([int]$root) {
    $all = @(Get-CimInstance Win32_Process)
    $rootProc = $all | Where-Object { $_.ProcessId -eq $root }
    $since = if ($rootProc) { $rootProc.CreationDate } else { (Get-Date).AddMinutes(-10) }
    $ids = New-Object 'System.Collections.Generic.HashSet[int]'
    if ($rootProc) { [void]$ids.Add($root) }
    do { $added = 0
      foreach ($p in $all) { if (-not $ids.Contains([int]$p.ProcessId) -and $ids.Contains([int]$p.ParentProcessId) -and $p.CreationDate -ge $since) { [void]$ids.Add([int]$p.ProcessId); $added++ } }
    } while ($added -gt 0)
    $all | Where-Object { $ids.Contains([int]$_.ProcessId) }
  }
  function Sample([int]$root) {
    @(foreach ($c in (Tree $root)) { try { $p = [Diagnostics.Process]::GetProcessById([int]$c.ProcessId); $p.Refresh()
        [pscustomobject]@{ pid = [int]$c.ProcessId; name = $c.Name; priv = $p.PrivateMemorySize64; ws = $p.WorkingSet64; cpuMs = $p.TotalProcessorTime.TotalMilliseconds } } catch { } })
  }
  $desk = [IntPtr]::Zero; $id = 0
  try {
    $deskName = "rl-smoke-$PID"
    $desk = [RlSmoke]::CreateDesktopW($deskName, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)
    if ($desk -eq [IntPtr]::Zero) { throw "CreateDesktop failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    $png = Join-Path $out 'window-3.0.png'; Remove-Item $png -ErrorAction SilentlyContinue
    $id = [RlSmoke]::Start("`"$exe`" --new-instance --snapshot `"$png`"", $app, "WinSta0\$deskName")
    $start = [Diagnostics.Process]::GetProcessById($id).StartTime.ToUniversalTime()
    $h = [IntPtr]::Zero; $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt 30) {
      $h = [RlSmoke]::VisibleWindow($desk, $id)
      if ($h -ne [IntPtr]::Zero) { $res.windowVisibleMs = [math]::Round(([DateTime]::UtcNow - $start).TotalMilliseconds, 1); break }
      if (-not (Get-Process -Id $id -ErrorAction SilentlyContinue)) { $res.exitedEarly = $true; break }
      Start-Sleep -Milliseconds 2
    }
    if ($h -eq [IntPtr]::Zero) { throw 'no visible window' }
    $res.windowClass = [RlSmoke]::ClassName($h); $res.windowTitle = [RlSmoke]::Title($h)
    function Until([double]$sec) { $w = $sec * 1000 - ([DateTime]::UtcNow - $start).TotalMilliseconds; if ($w -gt 0) { Start-Sleep -Milliseconds ([int]$w) } }
    Until 10
    $s10 = Sample $id; $t10 = ([DateTime]::UtcNow - $start).TotalMilliseconds
    Until 20
    $s20 = Sample $id; $t20 = ([DateTime]::UtcNow - $start).TotalMilliseconds
    $cpu10 = @{}; foreach ($x in $s10) { $cpu10[$x.pid] = $x.cpuMs }
    $cpuDelta = 0.0; foreach ($x in $s20) { $base = 0.0; if ($cpu10.ContainsKey($x.pid)) { $base = $cpu10[$x.pid] }; $cpuDelta += [math]::Max(0, $x.cpuMs - $base) }
    $wall = $t20 - $t10
    $res.mainWindow = [ordered]@{ desktop = 'hidden'; processesAt10s = @($s10 | ForEach-Object { $_.name })
      treePrivateMBAt10s = [math]::Round((($s10 | Measure-Object priv -Sum).Sum) / 1MB, 1)
      treeWorkingSetMBAt10s = [math]::Round((($s10 | Measure-Object ws -Sum).Sum) / 1MB, 1)
      idleWindowMs = [math]::Round($wall); idleCpuMs = [math]::Round($cpuDelta, 1); idleCpuPctOfOneCore = [math]::Round($cpuDelta / $wall * 100, 3) }
    # close it like the user does: WM_CLOSE (the window first renders its frame to the PNG), then the core's destructor
    # and the message loop must end the process
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [void][RlSmoke]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    while ($sw.Elapsed.TotalSeconds -lt 10 -and (Get-Process -Id $id -ErrorAction SilentlyContinue)) { Start-Sleep -Milliseconds 50 }
    $res.closedByWmClose = -not (Get-Process -Id $id -ErrorAction SilentlyContinue)
    $res.closeMs = $sw.ElapsedMilliseconds
    $res.childrenLeft = @(Get-CimInstance Win32_Process | Where-Object { $_.ParentProcessId -eq $id } | ForEach-Object { $_.Name })
    $res.snapshot = if (Test-Path $png) { [RlSmoke]::Stats($png) } else { 'no PNG' }
  } catch { $res.error = "$($_.Exception.Message) @ $($_.InvocationInfo.ScriptLineNumber)" }
  finally {
    if ($id) { foreach ($c in (Tree $id)) { try { Stop-Process -Id $c.ProcessId -Force -ErrorAction Stop } catch { } } }
    if ($desk -ne [IntPtr]::Zero) { Start-Sleep -Milliseconds 500; [void][RlSmoke]::CloseDesktop($desk) }
  }
  # 2.2.1 under the same load: its selftest runs and main window (baseline/session-bench.ps1)
  if ($Ref -and $Bench -and (Test-Path $Bench)) {
    try { & $Bench -Inst $Ref -Out $out -SelfTests 3 } catch { $res.benchError = "$($_.Exception.Message)" }
  }
  $res | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'smoke-session.json') -Encoding UTF8
  exit 0
}

# -------------------------------------------------------------------- orchestration (ssh, session 0)
$appKey = 'HKCU:\Software\Radarnite'
$task = 'rl-integ-window-smoke'
$R = [ordered]@{ exe = $exe; exeBytes = (Get-Item $exe).Length; ref = $Ref }
$R.preState = [ordered]@{ appKey = (Test-Path $appKey); ours = @(Ours).Count }
New-Item -ItemType Directory -Force -Path $out | Out-Null
try {
  if ($R.preState.ours) { throw "processes from $Dir or the ref are already running" }
  # the installer's way of making config.json
  $psi = New-Object Diagnostics.ProcessStartInfo $exe, "--set-builds-folder `"$Builds`""
  $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.WorkingDirectory = $app
  $p = [Diagnostics.Process]::Start($psi); $o = $p.StandardOutput.ReadToEnd(); $p.WaitForExit()
  $R.setBuildsFolder = [ordered]@{ exit = $p.ExitCode; out = $o.Trim(); config = Test-Path (Join-Path $app 'config.json') }
  if ($p.ExitCode -ne 0) { throw "--set-builds-folder failed: $o" }
  if ($Ref -and -not (Test-Path (Join-Path $Ref 'config.json'))) { throw "the ref has no config.json (run core parity first)" }

  Remove-Item (Join-Path $out 'smoke-session.json') -ErrorAction SilentlyContinue
  $vbs = Join-Path $Dir 'run-smoke.vbs'
  $cmd = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ""$PSCommandPath"" -InSession -Dir ""$Dir"" -Builds ""$Builds"""
  if ($Ref) { $cmd += " -Ref ""$Ref"" -Bench ""$Bench""" }
  Set-Content -Path $vbs -Encoding ASCII -Value ('CreateObject("WScript.Shell").Run "' + $cmd.Replace('"', '""') + '", 0, True')
  $sid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
  Register-ScheduledTask -TaskName $task -Force -Action (New-ScheduledTaskAction -Execute 'wscript.exe' -Argument "//B //NoLogo `"$vbs`"" -WorkingDirectory $Dir) `
    -Principal (New-ScheduledTaskPrincipal -UserId $sid -LogonType Interactive) -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 6)) | Out-Null
  Start-ScheduledTask -TaskName $task
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (-not (Test-Path (Join-Path $out 'smoke-session.json')) -and $sw.Elapsed.TotalSeconds -lt 330) { Start-Sleep -Seconds 1 }
  Start-Sleep -Seconds 1
  if (Test-Path (Join-Path $out 'smoke-session.json')) { $R.session = Get-Content (Join-Path $out 'smoke-session.json') -Raw | ConvertFrom-Json } else { $R.error = 'the session part did not finish' }
  if (Test-Path (Join-Path $out 'window.json')) { $R.ref221 = Get-Content (Join-Path $out 'window.json') -Raw | ConvertFrom-Json }
} catch {
  $R.error = "$($_.Exception.Message) @ line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
  try { Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction Stop } catch { }
  $left = Ours
  $R.killedAtEnd = @($left | ForEach-Object { "$($_.Name) $($_.ProcessId)" })
  $left | ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch { } }
  if (-not $R.preState.appKey -and (Test-Path $appKey)) { $R.appKeyCreated = @(Get-ChildItem -Recurse $appKey | ForEach-Object { $_.Name -replace '^HKEY_CURRENT_USER', 'HKCU' }); Remove-Item -Recurse -Force $appKey -ErrorAction SilentlyContinue }
  $R.postState = [ordered]@{ appKey = (Test-Path $appKey); ours = @(Ours).Count }
  $json = $R | ConvertTo-Json -Depth 8
  $u = Split-Path $env:USERPROFILE -Leaf
  $json = $json -replace [regex]::Escape($env:USERPROFILE.Replace('\', '\\')), '%USERPROFILE%' -replace [regex]::Escape($u), '<user>'
  [IO.File]::WriteAllText((Join-Path $out 'smoke.json'), $json, (New-Object Text.UTF8Encoding $false))
  $json
}
