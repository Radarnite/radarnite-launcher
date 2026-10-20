<#
  measure-2x.ps1 - P1 baseline of Radarnite Launcher 2.x as installed by its own installer (see README.md).

  One job, run over ssh on the Windows PC:
    powershell -NoProfile -ExecutionPolicy Bypass -File S:\rl-release-test\measure-2x.ps1 [-Version 2.2.1]
  1. Downloads RadarniteLauncherSetup-<Version>.exe from the GitHub release and checks it against its .sha256.
  2. Installs it silently into <Root>\Radarnite Launcher (/S /BUILDS=<Builds> /D=...): the real installer, the real
     layout and config.json (the installer runs --set-builds-folder), but not in the user's program folder.
     Refuses to run when HKCU\Software\Radarnite, the Uninstall key or the Start menu shortcut already exist.
  3. Full file listing (relative path, bytes, sha256) and sizes per component.
  4. --check <Build>: median of <Runs> (after 3 warm-ups) plus the commands that isolate its parts.
  5. Backend: node server.mjs against a scratch copy of <Builds>\<Build>\offline (never the real build folder).
  6. Window (session-bench.ps1) in the signed-in session through a scheduled task and a hidden wscript launcher.
  7. Cleanup: kills only what it started, removes the install folder and exactly the HKCU entries the installer
     created. Results stay in <Root>\out (baseline-raw.json, installed-files.tsv, window.json, ...).
  Output paths are normalized (%USERPROFILE%, %LOCALAPPDATA%, %TEMP%): nothing personal is written.
#>
param(
  [string]$Root = 'S:\rl-release-test',
  [string]$Version = '2.2.1',
  [string]$Builds = 'S:\FortniteBuilds',
  [string]$Build = '14.30',
  [string]$DevTree = 'S:\FortniteBuilds\launcher',
  [int]$Runs = 20,
  [switch]$KeepInstall
)
$ErrorActionPreference = 'Stop'
$inst  = Join-Path $Root 'Radarnite Launcher'      # same folder name as a real install (the node firewall rule keys on it)
$out   = Join-Path $Root 'out'
$exe   = Join-Path $inst 'RadarniteLauncher.exe'
$setup = Join-Path $Root "RadarniteLauncherSetup-$Version.exe"
$appKey    = 'HKCU:\Software\Radarnite'
$uninstKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RadarniteLauncher'
$smLnk     = Join-Path ([Environment]::GetFolderPath('Programs')) 'Radarnite Launcher.lnk'
$task      = 'rl-release-baseline'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$R = [ordered]@{ version = $Version; measuredAt = (Get-Date).ToUniversalTime().ToString('o') }

function Norm([string]$s) {
  if (-not $s) { return $s }
  foreach ($p in @(@($env:TEMP, '%TEMP%'), @($env:LOCALAPPDATA, '%LOCALAPPDATA%'), @($env:APPDATA, '%APPDATA%'), @($env:USERPROFILE, '%USERPROFILE%'))) {
    # raw and JSON-escaped (\\) forms
    if ($p[0]) { $s = $s -replace [regex]::Escape($p[0]), $p[1] -replace [regex]::Escape($p[0].Replace('\', '\\')), $p[1] }
  }
  # the bare account name, wherever it is left (e.g. "C:\Users\<name>" spelled differently)
  $u = Split-Path $env:USERPROFILE -Leaf
  if ($u) { $s = $s -replace [regex]::Escape($u), '<user>' }
  return $s
}
function Stats([double[]]$v) {
  $s = $v | Sort-Object; $n = $s.Count
  $med = if ($n % 2) { $s[($n - 1) / 2] } else { ($s[$n / 2 - 1] + $s[$n / 2]) / 2 }
  [ordered]@{ runs = $n; medianMs = [math]::Round($med, 1); minMs = [math]::Round($s[0], 1); maxMs = [math]::Round($s[$n - 1], 1)
    p10Ms = [math]::Round($s[[math]::Floor(($n - 1) * 0.1)], 1); p90Ms = [math]::Round($s[[math]::Ceiling(($n - 1) * 0.9)], 1)
    meanMs = [math]::Round(($s | Measure-Object -Average).Average, 1) }
}
# process creation -> exit, stdout/stderr drained; 3 warm-ups are discarded
function TimeCmd([string]$file, [string]$argline, [int]$n, [int]$warm = 3) {
  $ms = New-Object System.Collections.Generic.List[double]; $codes = @{}
  for ($i = 0; $i -lt $warm + $n; $i++) {
    $psi = New-Object Diagnostics.ProcessStartInfo $file, $argline
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit(); $t = $sw.Elapsed.TotalMilliseconds
    [void]$o.Result; [void]$e.Result
    if ($i -ge $warm) { $ms.Add($t); $codes["$($p.ExitCode)"] = 1 }
  }
  $r = Stats $ms.ToArray(); $r.exitCodes = @($codes.Keys); $r
}
function TimeBlock([scriptblock]$b, [int]$n = 7) {
  & $b | Out-Null
  $ms = for ($i = 0; $i -lt $n; $i++) { $sw = [Diagnostics.Stopwatch]::StartNew(); & $b | Out-Null; $sw.Elapsed.TotalMilliseconds }
  Stats $ms
}
function DirBytes([string]$d) { if (Test-Path $d) { [long](Get-ChildItem $d -Recurse -File -Force | Measure-Object Length -Sum).Sum } else { 0 } }
function MB([long]$b) { [math]::Round($b / 1MB, 1) }
function KillOwn {
  # only processes running from <Root>, and WebView2 processes on our user data folder
  $udf = Join-Path $inst 'webview2-radarnite'
  Get-CimInstance Win32_Process | Where-Object {
    ($_.ExecutablePath -and $_.ExecutablePath.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) -or
    ($_.Name -eq 'msedgewebview2.exe' -and $_.CommandLine -and $_.CommandLine.IndexOf($udf, [StringComparison]::OrdinalIgnoreCase) -ge 0)
  } | ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop; "killed $($_.Name) $($_.ProcessId)" } catch { } }
}

# ---- pre-state: never touch an existing install or its registry ----
$pre = [ordered]@{ appKey = (Test-Path $appKey); uninstallKey = (Test-Path $uninstKey); startMenuLnk = (Test-Path $smLnk)
  programsInstall = (Test-Path (Join-Path $env:LOCALAPPDATA 'Programs\Radarnite Launcher')) }
$R.preState = $pre
if ($pre.appKey -or $pre.uninstallKey -or $pre.startMenuLnk) { throw "refusing to install: Radarnite Launcher registry/shortcut already present ($($pre | ConvertTo-Json -Compress))" }

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$os = Get-CimInstance Win32_OperatingSystem
$R.machine = [ordered]@{ cpu = $cpu.Name.Trim(); logicalCores = [Environment]::ProcessorCount
  ramGB = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1); os = "$($os.Caption) $($os.Version)"
  gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name })
  buildsDrive = (Get-Volume -DriveLetter $Builds.Substring(0, 1) | ForEach-Object { "$($_.FileSystem) $([math]::Round($_.Size / 1GB)) GB" })
  cpuLoadBefore = (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average }
$wv = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}' -ErrorAction SilentlyContinue
$R.machine.webview2Runtime = $wv.pv

try {
  # ---- 1. download + verify ----
  $base = "https://github.com/Radarnite/radarnite-launcher/releases/download/v$Version"
  if (-not (Test-Path $setup)) { & curl.exe -sSfL -o $setup "$base/RadarniteLauncherSetup-$Version.exe"; if ($LASTEXITCODE) { throw "download failed" } }
  & curl.exe -sSfL -o "$setup.sha256" "$base/RadarniteLauncherSetup-$Version.exe.sha256"; if ($LASTEXITCODE) { throw "sha256 download failed" }
  $want = ((Get-Content "$setup.sha256" -Raw).Trim() -split '\s+')[0].ToLower()
  $got = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
  if ($got -ne $want) { throw "sha256 mismatch: $got != $want" }
  $R.installer = [ordered]@{ sha256 = $got; bytes = (Get-Item $setup).Length }

  # ---- 2. silent install into our folder ----
  KillOwn | Out-Null
  if (Test-Path $inst) { Remove-Item -Recurse -Force $inst }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $setup -ArgumentList "/S /BUILDS=$Builds /D=$inst" -Wait -PassThru
  $R.installer.silentInstallMs = $sw.ElapsedMilliseconds; $R.installer.exitCode = $p.ExitCode
  if (-not (Test-Path $exe)) { throw "install produced no $exe" }
  $R.installer.configBuildsFolder = (Get-Content (Join-Path $inst 'config.json') -Raw | ConvertFrom-Json).buildsFolder
  $R.installer.registryWritten = [ordered]@{ appKey = (Test-Path $appKey); uninstallKey = (Test-Path $uninstKey); startMenuLnk = (Test-Path $smLnk) }
  $R.installer.fileVersion = (Get-Item $exe).VersionInfo.ProductVersion

  # ---- 3. listing + sizes (right after install: payload + config.json + uninstaller) ----
  $files = Get-ChildItem $inst -Recurse -File -Force
  $rows = foreach ($f in $files) { "{0}`t{1}`t{2}" -f $f.FullName.Substring($inst.Length + 1), $f.Length, (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower() }
  $rows = $rows | Sort-Object
  [IO.File]::WriteAllLines((Join-Path $out 'installed-files.tsv'), [string[]](@("path`tbytes`tsha256") + $rows), (New-Object Text.UTF8Encoding $false))
  $total = ($files | Measure-Object Length -Sum).Sum
  $known = 'node', 'backend', 'tools', 'gameserver', 'redirect', 'patches', 'ui', 'runtimes', 'webview2-radarnite'
  $rootFiles = Get-ChildItem $inst -File -Force
  $own = $rootFiles | Where-Object { $_.Name -like 'RadarniteLauncher.*' }
  $wvRoot = $rootFiles | Where-Object { $_.Name -like 'Microsoft.Web.WebView2.*' -or $_.Name -eq 'WebView2Loader.dll' }
  $misc = $rootFiles | Where-Object { $_.Name -in 'radarnite.ico', 'THIRD-PARTY-NOTICES.txt', 'README-offline.md', 'config.json' -or $_.Name -like 'Uninstall*.exe' -or $_.Name -like '*.log' }
  $dotnetFiles = $rootFiles | Where-Object { $_ -notin $own -and $_ -notin $wvRoot -and $_ -notin $misc }
  $satDirs = Get-ChildItem $inst -Directory -Force | Where-Object { $_.Name -notin $known }
  $satBytes = [long]0; foreach ($d in $satDirs) { $satBytes += DirBytes $d.FullName }
  $tools = [ordered]@{}
  foreach ($d in Get-ChildItem (Join-Path $inst 'tools') -Directory -ErrorAction SilentlyContinue) {
    $rc = Get-ChildItem $d.FullName -Filter *.runtimeconfig.json -File | Select-Object -First 1
    $kind = 'native'
    if ($rc) { $j = Get-Content $rc.FullName -Raw; $kind = if ($j -match 'includedFrameworks') { 'dotnet-self-contained' } else { 'dotnet-framework-dependent' } }
    $tools[$d.Name] = [ordered]@{ MB = MB (DirBytes $d.FullName); files = @(Get-ChildItem $d.FullName -Recurse -File).Count; kind = $kind
      exes = @(Get-ChildItem $d.FullName -Filter *.exe -File | ForEach-Object { $_.Name }) }
  }
  $looseTools = @(Get-ChildItem (Join-Path $inst 'tools') -File -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name) ($($_.Length))" })
  $rcMain = Get-Content (Join-Path $inst 'RadarniteLauncher.runtimeconfig.json') -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json
  $R.installed = [ordered]@{
    files = $files.Count; totalMB = MB $total
    components = [ordered]@{
      dotnetRuntime = [ordered]@{ MB = MB ((($dotnetFiles | Measure-Object Length -Sum).Sum) + $satBytes); rootFiles = @($dotnetFiles).Count
        satelliteDirs = @($satDirs | ForEach-Object { $_.Name }); frameworks = @($rcMain.runtimeOptions.includedFrameworks | ForEach-Object { "$($_.name) $($_.version)" }) }
      launcherAssembly = [ordered]@{ MB = MB (($own | Measure-Object Length -Sum).Sum); files = @($own | ForEach-Object { $_.Name }) }
      webview2 = [ordered]@{ MB = MB ((($wvRoot | Measure-Object Length -Sum).Sum) + (DirBytes (Join-Path $inst 'runtimes'))); files = @($wvRoot | ForEach-Object { $_.Name }); note = 'WebView2 SDK dlls + runtimes\; the runtime itself is the system Evergreen install' }
      ui = [ordered]@{ MB = MB (DirBytes (Join-Path $inst 'ui')) }
      node = [ordered]@{ MB = MB (DirBytes (Join-Path $inst 'node')); version = (& (Join-Path $inst 'node\node.exe') --version) }
      backend = [ordered]@{ MB = MB (DirBytes (Join-Path $inst 'backend')); files = @(Get-ChildItem (Join-Path $inst 'backend') -Recurse -File | ForEach-Object { $_.FullName.Substring($inst.Length + 1) }) }
      tools = [ordered]@{ MB = MB (DirBytes (Join-Path $inst 'tools')); dirs = $tools; looseFiles = $looseTools }
      gameserver = [ordered]@{ MB = MB (DirBytes (Join-Path $inst 'gameserver')) }
      redirect = [ordered]@{ MB = MB (DirBytes (Join-Path $inst 'redirect')) }
      patches = [ordered]@{ MB = MB (DirBytes (Join-Path $inst 'patches')) }
      other = [ordered]@{ MB = MB (($misc | Measure-Object Length -Sum).Sum); files = @($misc | ForEach-Object { $_.Name }) }
    }
  }

  # ---- 4. --check and its parts ----
  $buildDir = Join-Path $Builds $Build
  $R.check = [ordered]@{
    check         = TimeCmd $exe "--check $Build" $Runs
    checkNoBuild  = TimeCmd $exe '--check zz-no-such-build' 10
    scan          = TimeCmd $exe "--scan $Builds" 10
    runtimeFloor  = TimeCmd $exe '--set-builds-folder' 10
  }
  $psi = New-Object Diagnostics.ProcessStartInfo $exe, "--check $Build"; $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true
  $p = [Diagnostics.Process]::Start($psi); $txt = $p.StandardOutput.ReadToEnd(); $p.WaitForExit()
  [IO.File]::WriteAllText((Join-Path $out 'check-output.txt'), (Norm $txt), (New-Object Text.UTF8Encoding $false))
  $devExe = Join-Path $DevTree 'RadarniteLauncher.exe'
  if (Test-Path $devExe) { $R.check.devTree = TimeCmd $devExe "--check $Build" 10; $R.check.devTree.version = (Get-Item $devExe).VersionInfo.ProductVersion }
  # the parts of --check measured from PowerShell (approximate: same OS work, different caller)
  Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class RlImg {
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(int a, bool i, int pid);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool QueryFullProcessImageNameW(IntPtr h, int f, StringBuilder sb, ref int n);
  public static int All() { int n = 0; foreach (var p in System.Diagnostics.Process.GetProcesses()) { var h = OpenProcess(0x1000, false, p.Id); if (h == IntPtr.Zero) continue;
    var sb = new StringBuilder(1024); int c = sb.Capacity; if (QueryFullProcessImageNameW(h, 0, sb, ref c)) n++; CloseHandle(h); } return n; }
}
'@
  $fwRules = 0
  $R.check.parts = [ordered]@{
    firewallRulesEnum = TimeBlock { $pol = New-Object -ComObject HNetCfg.FwPolicy2; $n = 0; foreach ($r in $pol.Rules) { if ($r.Grouping -eq 'FN Offline Builds') { $n++ } }; $n }
    buildExeWalk      = TimeBlock { [IO.Directory]::EnumerateFiles($buildDir, '*.exe', [IO.SearchOption]::AllDirectories) | Measure-Object | Out-Null }
    processImagePaths = TimeBlock { [RlImg]::All() }
    note = 'Firewall.Check runs twice per --check (direct + Preflight): each enumerates every firewall rule through COM (dynamic binder) and walks the build folder for *.exe; Preflight also reads every process image path.'
  }
  $R.check.parts.firewallRuleCount = (New-Object -ComObject HNetCfg.FwPolicy2).Rules.Count
  $R.check.parts.buildExeCount = @([IO.Directory]::EnumerateFiles($buildDir, '*.exe', [IO.SearchOption]::AllDirectories)).Count
  $R.check.parts.processCount = @([Diagnostics.Process]::GetProcesses()).Count

  # ---- 5. backend: node server.mjs on a scratch copy of the build's offline data ----
  $fake = Join-Path $Root "builds\$Build"
  New-Item -ItemType Directory -Force -Path (Join-Path $fake 'offline') | Out-Null
  foreach ($f in 'cosmetics.json', 'custom-cosmetics.json', 'state.json') {
    $src = Join-Path $buildDir "offline\$f"; if (Test-Path $src) { Copy-Item $src (Join-Path $fake "offline\$f") -Force }
  }
  $node = Join-Path $inst 'node\node.exe'; $mjs = Join-Path $inst 'backend\server.mjs'; $cfg = Join-Path $inst 'config.json'
  $l = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0); $l.Start(); $port = $l.LocalEndpoint.Port; $l.Stop()
  $ready = New-Object System.Collections.Generic.List[double]; $ws = New-Object System.Collections.Generic.List[double]; $pv = New-Object System.Collections.Generic.List[double]
  $ws3 = New-Object System.Collections.Generic.List[double]
  for ($i = 0; $i -lt 12; $i++) {
    $psi = New-Object Diagnostics.ProcessStartInfo $node, "`"$mjs`" --build `"$fake`" --config `"$cfg`" --port $port"
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    try {
      $line = $p.StandardOutput.ReadLine(); $t = $sw.Elapsed.TotalMilliseconds
      if ($line -notlike 'ready *') { throw "backend said: $line / $($p.StandardError.ReadToEnd())" }
      Start-Sleep -Milliseconds 300; $p.Refresh()
      if ($i -ge 2) { $ready.Add($t); $ws.Add($p.WorkingSet64 / 1MB); $pv.Add($p.PrivateMemorySize64 / 1MB) }
      if ($i -eq 11) { Start-Sleep -Seconds 3; $p.Refresh(); $ws3.Add($p.WorkingSet64 / 1MB) }
    } finally { try { $p.Kill() } catch { }; $p.WaitForExit(5000) | Out-Null }
  }
  $floor = TimeCmd $node '-e "console.log(1)"' 10
  $R.backend = [ordered]@{ readyMs = (Stats $ready.ToArray()); rssMBAfterReady = [math]::Round(($ws | Measure-Object -Average).Average, 1)
    privateMBAfterReady = [math]::Round(($pv | Measure-Object -Average).Average, 1); rssMBAt3s = [math]::Round($ws3[0], 1)
    nodeStartupFloor = $floor; cosmeticsJsonMB = MB ((Get-Item (Join-Path $fake 'offline\cosmetics.json') -ErrorAction SilentlyContinue).Length)
    note = 'node.exe server.mjs --build <scratch copy> --config <install config.json> --port <free>; ready = process creation -> "ready 127.0.0.1:PORT" line; RSS = working set 300 ms after ready' }

  # ---- 6. window, in the signed-in session ----
  Remove-Item (Join-Path $out 'window.json') -ErrorAction SilentlyContinue
  $vbs = Join-Path $Root 'run-hidden.vbs'
  $cmd = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ""$Root\session-bench.ps1"" -Inst ""$inst"" -Out ""$out"""
  Set-Content -Path $vbs -Encoding ASCII -Value ('CreateObject("WScript.Shell").Run "' + $cmd.Replace('"', '""') + '", 0, True')
  $sid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
  $a = New-ScheduledTaskAction -Execute 'wscript.exe' -Argument "//B //NoLogo `"$vbs`"" -WorkingDirectory $Root
  $pr = New-ScheduledTaskPrincipal -UserId $sid -LogonType Interactive
  $st = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 10) -AllowStartIfOnBatteries
  Register-ScheduledTask -TaskName $task -Action $a -Principal $pr -Settings $st -Force | Out-Null
  Start-ScheduledTask -TaskName $task
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (-not (Test-Path (Join-Path $out 'window.json')) -and $sw.Elapsed.TotalSeconds -lt 420) { Start-Sleep -Seconds 1 }
  Start-Sleep -Seconds 1
  $R.window = if (Test-Path (Join-Path $out 'window.json')) { Get-Content (Join-Path $out 'window.json') -Raw | ConvertFrom-Json } else { 'no window.json (timeout)' }
  $R.webview2UserDataMB = MB (DirBytes (Join-Path $inst 'webview2-radarnite'))
  $ul = Join-Path $inst 'update.log'
  $R.updateLogOfTestInstall = if (Test-Path $ul) { @(Get-Content $ul | ForEach-Object { Norm $_ }) } else { @() }
  $R.machine.cpuLoadAfter = (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average

  # ---- 7. what other scripts expect from the launcher folder ----
  $sig = Join-Path $DevTree 'tools\sigfit-installed.ps1'
  if (Test-Path $sig) {
    Copy-Item $sig (Join-Path $out 'sigfit-installed.dev.ps1') -Force
    $R.sigfitInstalled = [ordered]@{ devSha256 = (Get-FileHash $sig -Algorithm SHA256).Hash.ToLower()
      installedSha256 = $(if (Test-Path (Join-Path $inst 'tools\sigfit-installed.ps1')) { (Get-FileHash (Join-Path $inst 'tools\sigfit-installed.ps1') -Algorithm SHA256).Hash.ToLower() } else { $null }) }
    Copy-Item (Join-Path $inst 'tools\sigfit-installed.ps1') (Join-Path $out 'sigfit-installed.installed.ps1') -Force -ErrorAction SilentlyContinue
  }
  $rg = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\rg.exe'
  if (Test-Path $rg) {
    $pat = '(launcher[\\/]+(node|backend|tools|gameserver|redirect|patches|ui)\b|server\.mjs|RadarniteLauncher\.exe|sigfit-installed|fn-cosmetics-gen|fn-modkit-sigfit|radarnite-server\.dll|fnredirect\.dll|fnpakpatch\.dll)'
    # scripts outside the launcher's own source that name launcher files (the launcher's src\ is left out)
    $where = @($DevTree, 'S:\fn-modkit', 'S:\FN', 'S:\skin-launcher', 'S:\port-cosmeticos', 'S:\cosmeticos-1430', 'S:\Mods') | Where-Object { Test-Path $_ }
    $where += @(Get-ChildItem (Join-Path $Builds '*') -File -Include *.ps1, *.cmd, *.bat, *.md -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    $hits = & $rg -n -i --no-messages -g '*.ps1' -g '*.psm1' -g '*.cmd' -g '*.bat' -g '*.mjs' -g '*.js' -g '*.py' -g '*.md' -g '*.cs' -g '*.json' -g '!**/node_modules/**' -g '!**/webview2-radarnite/**' -g '!**/bin/**' -g '!**/obj/**' --max-columns 220 --max-filesize 2M -e $pat -- @($where) 2>$null
    $hits = @($hits | Where-Object { -not $_.StartsWith((Join-Path $DevTree 'src') + '\', [StringComparison]::OrdinalIgnoreCase) })
    $c01 = & $rg --files --no-messages --max-depth 5 -g 'c01-launch.ps1' -- 'S:\' 2>$null
    [IO.File]::WriteAllLines((Join-Path $out 'refs.txt'), [string[]]@(@($hits) + @($c01 | ForEach-Object { "FILE $_" }) | ForEach-Object { Norm $_ }), (New-Object Text.UTF8Encoding $false))
  }
} catch {
  $R.error = "$($_.Exception.Message) @ line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
  try { Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction Stop } catch { }
  $R.cleanup = @(KillOwn)
  if (-not $KeepInstall) {
    Start-Sleep -Seconds 1
    Remove-Item -Recurse -Force $inst -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force (Join-Path $Root 'builds') -ErrorAction SilentlyContinue
    if (-not $pre.appKey) { Remove-Item -Recurse -Force $appKey -ErrorAction SilentlyContinue }
    if (-not $pre.uninstallKey) { Remove-Item -Recurse -Force $uninstKey -ErrorAction SilentlyContinue }
    if (-not $pre.startMenuLnk) { Remove-Item -Force $smLnk -ErrorAction SilentlyContinue }
  }
  $R.postState = [ordered]@{ appKey = (Test-Path $appKey); uninstallKey = (Test-Path $uninstKey); startMenuLnk = (Test-Path $smLnk); installDir = (Test-Path $inst) }
  $json = $R | ConvertTo-Json -Depth 10
  [IO.File]::WriteAllText((Join-Path $out 'baseline-raw.json'), (Norm $json), (New-Object Text.UTF8Encoding $false))
  "done; error=$($R.error)"
  "postState: $($R.postState | ConvertTo-Json -Compress)"
}
