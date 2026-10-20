<#
  installer-migration-test.ps1 - 2.x -> 3.x upgrade test of radarnite.nsi on a real Windows PC, in a scratch folder.
    powershell -NoProfile -ExecutionPolicy Bypass -File installer-migration-test.ps1 -Old <2.x setup> -New <3.x setup>
        -Expected <3.x file list> [-Root S:\rl-release-test]
  1. Installs <Old> silently into <Root>\Radarnite Launcher and plants user data (a key in config.json, update.log).
  2. In the signed-in session (scheduled task + hidden wscript): starts that 2.x window on a never-shown desktop, so its
     RadarniteLauncher.exe and WebView2 processes run and hold files; locks one file in its WebView2 profile; runs
     <New> silently over it (as an update would) and records which processes survived.
  3. Checks: only that install's processes were closed; the folder holds exactly the 3.x files (<Expected>, relative
     paths) plus config.json, the logs, the uninstaller and the locked file; every key and value of config.json kept
     (the installer's --set-builds-folder rewrites it in the launcher's own format) and update.log unchanged;
     InstallDir, BuildsFolder and the uninstall key kept, DisplayVersion updated; the silent uninstaller removes the
     uninstall key in place (_?=) without waiting on itself, and leaves a desktop shortcut it did not create.
  Refuses to run when a Radarnite Launcher install, its registry keys or shortcuts exist; removes exactly what it
  created. Results: <Root>\out\migration.json.
#>
param([string]$Root = 'S:\rl-release-test', [string]$Old, [string]$New, [string]$Expected,
      [string]$Builds = 'S:\FortniteBuilds', [switch]$InSession)
$ErrorActionPreference = 'Stop'
$inst = Join-Path $Root 'Radarnite Launcher'
$out = Join-Path $Root 'out'
$udf = Join-Path $inst 'webview2-radarnite'
function Own {
  @(Get-CimInstance Win32_Process | Where-Object {
    ($_.ExecutablePath -and $_.ExecutablePath.StartsWith($inst + '\', [StringComparison]::OrdinalIgnoreCase)) -or
    ($_.Name -eq 'msedgewebview2.exe' -and $_.CommandLine -and $_.CommandLine.IndexOf($udf, [StringComparison]::OrdinalIgnoreCase) -ge 0) })
}

if ($InSession) {
  # ---------------------------------------------------------------- signed-in session part
  $res = [ordered]@{ session = (Get-Process -Id $PID).SessionId }
  Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class RlDesk {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct STARTUPINFO { public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
  [StructLayout(LayoutKind.Sequential)] public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr CreateDesktopW(string n, IntPtr d, IntPtr m, int f, uint a, IntPtr s);
  [DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr h);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcessW(string a, StringBuilder c, IntPtr p, IntPtr t, bool i, uint f, IntPtr e, string d, ref STARTUPINFO s, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  public static int Start(string cmd, string cwd, string desktop) { var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si); si.lpDesktop = desktop; PROCESS_INFORMATION pi;
    if (!CreateProcessW(null, new StringBuilder(cmd), IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi)) throw new System.ComponentModel.Win32Exception();
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return pi.dwProcessId; }
}
'@
  $desk = [IntPtr]::Zero; $fs = $null
  try {
    $name = "rl-migrate-$PID"
    $desk = [RlDesk]::CreateDesktopW($name, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)
    if ($desk -eq [IntPtr]::Zero) { throw 'CreateDesktop failed' }
    [void][RlDesk]::Start("`"$inst\RadarniteLauncher.exe`" --new-instance", $inst, "WinSta0\$name")
    $sw = [Diagnostics.Stopwatch]::StartNew()
    do { Start-Sleep -Milliseconds 500; $own = Own } while ((@($own | Where-Object Name -eq 'msedgewebview2.exe').Count -lt 3) -and $sw.Elapsed.TotalSeconds -lt 40)
    Start-Sleep -Seconds 3
    $own = Own
    $res.ownBefore = @($own | ForEach-Object { $_.Name } | Group-Object | ForEach-Object { "$($_.Name) x$($_.Count)" })
    $ownIds = @($own | ForEach-Object { [int]$_.ProcessId })
    # everyone else's launcher, node and WebView2 main (browser) processes: they must all survive (WebView2 helper
    # processes of other apps come and go by themselves, so they are only counted)
    $others = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -in 'RadarniteLauncher.exe', 'msedgewebview2.exe', 'node.exe' -and $ownIds -notcontains [int]$_.ProcessId })
    $mains = @($others | Where-Object { -not ($_.CommandLine -match '--type=') })
    $res.othersBefore = $others.Count; $res.otherMainsBefore = $mains.Count
    # user data as it is right before the upgrade (the running 2.x appends to update.log on its update check)
    $snap = @{ config = [IO.File]::ReadAllBytes((Join-Path $inst 'config.json')); log = [IO.File]::ReadAllBytes((Join-Path $inst 'update.log')) }
    $fs = [IO.File]::Open((Join-Path $udf 'rl-migration-lock.tmp'), 'Create', 'ReadWrite', 'None')
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $New -ArgumentList "/S /BUILDS=$Builds /D=$inst" -Wait -PassThru
    $res.installMs = $sw.ElapsedMilliseconds; $res.installExit = $p.ExitCode
    $fs.Close(); $fs = $null
    $res.ownAfter = @(Own | ForEach-Object { "$($_.Name) $($_.ProcessId)" })
    $alive = @{}; foreach ($pp in [Diagnostics.Process]::GetProcesses()) { $alive[$pp.Id] = 1 }
    $res.otherMainsGone = @($mains | Where-Object { -not $alive.ContainsKey([int]$_.ProcessId) } | ForEach-Object { "$($_.Name) $($_.ProcessId)" })
    $res.otherHelpersGone = @($others | Where-Object { $mains -notcontains $_ -and -not $alive.ContainsKey([int]$_.ProcessId) }).Count
    $cfgAfter = [IO.File]::ReadAllBytes((Join-Path $inst 'config.json')); $logAfter = [IO.File]::ReadAllBytes((Join-Path $inst 'update.log'))
    $res.configJsonUnchanged = [Convert]::ToBase64String($cfgAfter) -eq [Convert]::ToBase64String($snap.config)
    # every key of the 2.x config, with the same value (unknown keys included); bytes and key order may change
    $u8 = New-Object Text.UTF8Encoding $false
    $cb = $u8.GetString($snap.config).TrimStart([char]0xFEFF) | ConvertFrom-Json; $ca = $u8.GetString($cfgAfter).TrimStart([char]0xFEFF) | ConvertFrom-Json
    $res.configJsonChangedKeys = @($cb.PSObject.Properties | Where-Object { -not ($ca.PSObject.Properties.Name -contains $_.Name) -or
      (($_.Value | ConvertTo-Json -Compress -Depth 10) -cne ($ca.($_.Name) | ConvertTo-Json -Compress -Depth 10)) } | ForEach-Object { $_.Name })
    $res.configJsonValuesKept = $res.configJsonChangedKeys.Count -eq 0
    $res.updateLogKept = ($logAfter.Length -ge $snap.log.Length) -and ([Convert]::ToBase64String($logAfter, 0, $snap.log.Length) -eq [Convert]::ToBase64String($snap.log))
  } catch { $res.error = "$($_.Exception.Message) @ $($_.InvocationInfo.ScriptLineNumber)" }
  finally {
    if ($fs) { $fs.Close() }
    Own | ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch { } }
    if ($desk -ne [IntPtr]::Zero) { Start-Sleep -Milliseconds 500; [void][RlDesk]::CloseDesktop($desk) }
    $res | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'migration-session.json') -Encoding UTF8
  }
  exit 0
}

# -------------------------------------------------------------------- orchestration (ssh, session 0)
$appKey = 'HKCU:\Software\Radarnite'; $appSub = 'HKCU:\Software\Radarnite\Launcher'
$uninstKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RadarniteLauncher'
$smLnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'Radarnite Launcher.lnk'
$deskLnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Radarnite Launcher.lnk'
$task = 'rl-release-migration'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$R = [ordered]@{ old = (Split-Path $Old -Leaf); new = (Split-Path $New -Leaf) }
$pre = [ordered]@{ appKey = (Test-Path $appKey); uninstallKey = (Test-Path $uninstKey); startMenuLnk = (Test-Path $smLnk); desktopLnk = (Test-Path $deskLnk) }
$R.preState = $pre
if ($pre.appKey -or $pre.uninstallKey -or $pre.startMenuLnk) { throw "refusing: Radarnite Launcher registry/shortcut already present" }
$checks = [ordered]@{}
try {
  foreach ($f in $Old, $New, $Expected) { if (-not (Test-Path $f)) { throw "missing $f" } }
  Own | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
  if (Test-Path $inst) { Remove-Item -Recurse -Force $inst }
  $p = Start-Process -FilePath $Old -ArgumentList "/S /BUILDS=$Builds /D=$inst" -Wait -PassThru
  if ($p.ExitCode -ne 0 -or -not (Test-Path "$inst\RadarniteLauncher.exe")) { throw "2.x install failed ($($p.ExitCode))" }
  $R.oldVersion = (Get-Item "$inst\RadarniteLauncher.exe").VersionInfo.ProductVersion
  # user data that must survive byte for byte
  $cfgPath = Join-Path $inst 'config.json'
  $cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
  $cfg | Add-Member -NotePropertyName '_migrationMarker' -NotePropertyValue 'keep-me' -Force
  [IO.File]::WriteAllText($cfgPath, ($cfg | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding $false))
  Add-Content (Join-Path $inst 'update.log') "$((Get-Date).ToString('o')) migration-test marker"
  $before = @{ installDir = (Get-ItemProperty $appSub).InstallDir; buildsFolder = (Get-ItemProperty $appSub).BuildsFolder; uninstall = (Get-ItemProperty $uninstKey).UninstallString }

  # the upgrade itself runs in the signed-in session
  Remove-Item (Join-Path $out 'migration-session.json') -ErrorAction SilentlyContinue
  $self = $MyInvocation.MyCommand.Path
  $vbs = Join-Path $Root 'run-migration.vbs'
  $cmd = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ""$self"" -InSession -Root ""$Root"" -New ""$New"" -Builds ""$Builds"""
  Set-Content -Path $vbs -Encoding ASCII -Value ('CreateObject("WScript.Shell").Run "' + $cmd.Replace('"', '""') + '", 0, True')
  $sid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
  Register-ScheduledTask -TaskName $task -Force -Action (New-ScheduledTaskAction -Execute 'wscript.exe' -Argument "//B //NoLogo `"$vbs`"" -WorkingDirectory $Root) `
    -Principal (New-ScheduledTaskPrincipal -UserId $sid -LogonType Interactive) -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 5)) | Out-Null
  Start-ScheduledTask -TaskName $task
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (-not (Test-Path (Join-Path $out 'migration-session.json')) -and $sw.Elapsed.TotalSeconds -lt 240) { Start-Sleep -Seconds 1 }
  Start-Sleep -Seconds 1
  $S = Get-Content (Join-Path $out 'migration-session.json') -Raw | ConvertFrom-Json
  $R.session = $S

  # ---- checks ----
  $checks.ownProcessesWereRunning = @($S.ownBefore | Where-Object { $_ -like 'msedgewebview2.exe*' -or $_ -like 'RadarniteLauncher.exe*' }).Count -ge 2
  $checks.installerExit0 = $S.installExit -eq 0
  $checks.ownProcessesClosed = @($S.ownAfter).Count -eq 0
  $checks.otherLaunchersAndBrowsersUntouched = ($S.otherMainsBefore -gt 0) -and (@($S.otherMainsGone).Count -eq 0)
  $want = @(Get-Content $Expected | Where-Object { $_ } | ForEach-Object { $_.Replace('/', '\') })
  $keep = @('config.json', 'update.log', 'ui-error.log', 'Uninstall Radarnite Launcher.exe', 'webview2-radarnite\rl-migration-lock.tmp')
  $have = @(Get-ChildItem $inst -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($inst.Length + 1) })
  $R.missing3x = @($want | Where-Object { $have -notcontains $_ })
  $R.unexpected = @($have | Where-Object { $want -notcontains $_ -and $keep -notcontains $_ })
  $checks.exactly3xFilesPlusUserData = ($R.missing3x.Count -eq 0) -and ($R.unexpected.Count -eq 0)
  $checks.lockedFileToleratedAndKept = Test-Path (Join-Path $udf 'rl-migration-lock.tmp')
  $checks.configJsonValuesKept = [bool]$S.configJsonValuesKept
  $R.configJsonBytesUnchanged = [bool]$S.configJsonUnchanged   # info: false once the real exe's --set-builds-folder runs
  $checks.updateLogKept = [bool]$S.updateLogKept
  # info: whether an unknown config key survived the 2.x window itself (3.x must keep unknown keys)
  $R.unknownConfigKeyAfter2xAndUpgrade = (Get-Content $cfgPath -Raw | ConvertFrom-Json)._migrationMarker -eq 'keep-me'
  $after = Get-ItemProperty $appSub; $un = Get-ItemProperty $uninstKey
  $checks.registryKept = ($after.InstallDir -eq $before.installDir) -and ($after.BuildsFolder -eq $before.buildsFolder) -and ($un.UninstallString -eq $before.uninstall)
  $R.displayVersion = $un.DisplayVersion
  $checks.displayVersionUpdated = $un.DisplayVersion -ne $R.oldVersion
  $checks.startMenuLnk = Test-Path $smLnk
  # the 3.x uninstaller, run in place (_?=) like a Sandbox test: it must not close itself, must keep BuildsFolder and
  # must leave a desktop shortcut it did not create (backed up first and put back if it went anyway)
  $lnkBak = Join-Path $Root 'desktop-lnk.bak'
  if ($pre.desktopLnk) { Copy-Item $deskLnk $lnkBak -Force; $lnkHash = (Get-FileHash $deskLnk -Algorithm SHA256).Hash }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $u = Start-Process -FilePath (Join-Path $inst 'Uninstall Radarnite Launcher.exe') -ArgumentList "/S _?=$inst" -Wait -PassThru
  $left = @(Get-ChildItem $inst -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($inst.Length + 1) })
  $R.uninstall = [ordered]@{ exit = $u.ExitCode; ms = $sw.ElapsedMilliseconds; filesLeft = $left }
  $checks.uninstallRemovesFilesAndKey = ($u.ExitCode -eq 0) -and -not (Test-Path $uninstKey) -and (@($left | Where-Object { $_ -ne 'Uninstall Radarnite Launcher.exe' }).Count -eq 0)
  $checks.uninstallDidNotWaitOnItself = $sw.ElapsedMilliseconds -lt 12000
  $checks.uninstallKeepsBuildsFolder = [bool](Get-ItemProperty $appSub -ErrorAction SilentlyContinue).BuildsFolder
  if ($pre.desktopLnk) {
    $checks.userDesktopShortcutKept = (Test-Path $deskLnk) -and ((Get-FileHash $deskLnk -Algorithm SHA256).Hash -eq $lnkHash)
    if (-not (Test-Path $deskLnk)) { Copy-Item $lnkBak $deskLnk -Force; $R.desktopLnkRestored = $true }
    Remove-Item $lnkBak -Force -ErrorAction SilentlyContinue
  }
} catch {
  $R.error = "$($_.Exception.Message) @ line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
  try { Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction Stop } catch { }
  Own | ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch { } }
  Start-Sleep -Seconds 1
  Remove-Item -Recurse -Force $inst -ErrorAction SilentlyContinue
  if (-not $pre.appKey) { Remove-Item -Recurse -Force $appKey -ErrorAction SilentlyContinue }
  if (-not $pre.uninstallKey) { Remove-Item -Recurse -Force $uninstKey -ErrorAction SilentlyContinue }
  if (-not $pre.startMenuLnk) { Remove-Item -Force $smLnk -ErrorAction SilentlyContinue }
  if (-not $pre.desktopLnk) { Remove-Item -Force $deskLnk -ErrorAction SilentlyContinue }
  $R.checks = $checks
  $R.allPassed = ($checks.Count -gt 0) -and -not ($checks.Values -contains $false) -and -not $R.error
  $R.postState = [ordered]@{ appKey = (Test-Path $appKey); uninstallKey = (Test-Path $uninstKey); startMenuLnk = (Test-Path $smLnk); desktopLnk = (Test-Path $deskLnk); installDir = (Test-Path $inst) }
  $json = $R | ConvertTo-Json -Depth 8
  $u = Split-Path $env:USERPROFILE -Leaf
  $json = $json -replace [regex]::Escape($env:USERPROFILE.Replace('\', '\\')), '%USERPROFILE%' -replace [regex]::Escape($u), '<user>'
  [IO.File]::WriteAllText((Join-Path $out 'migration.json'), $json, (New-Object Text.UTF8Encoding $false))
  "allPassed=$($R.allPassed) error=$($R.error)"
  $checks.GetEnumerator() | ForEach-Object { "  $($_.Key): $($_.Value)" }
  "postState: $($R.postState | ConvertTo-Json -Compress)"
}
