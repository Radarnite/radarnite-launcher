# Parity 3: add a build from a zip or a folder through the test harness (coretest.exe addzip/addfolder): the build lands
# in the fake root, a folder on another drive is copied and its source kept. Needs the fake root of parity2.ps1.
# Scratch folders only; ref config.json restored.
param([string]$Dir = 'S:\rl-core-test')
$ErrorActionPreference = 'Stop'
$t = $Dir
$ref = Join-Path $t 'rlcoretest-ref'
$old = Join-Path $ref 'RadarniteLauncher.exe'
$new = Join-Path $t 'rn-core-cli.exe'
$harness = Join-Path $t 'coretest.exe'
$fake = Join-Path $t 'fakeroot'
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
function Same($a, $b) { [Convert]::ToBase64String([byte[]]$a) -eq [Convert]::ToBase64String([byte[]]$b) }
function Show([string]$label, [string]$text) { "  --- $label"; $text.TrimEnd() -split "`n" | ForEach-Object { '  ' + $_.TrimEnd() } }

try {
  '== add from zip / folder (root = fakeroot)'
  [void](Run $old "--set-builds-folder $fake")
  if (-not [IO.File]::ReadAllText($cfgPath).Contains('fakeroot')) { throw 'the builds folder is not the fake root: nothing added' }
  $z =Join-Path $t 'ziptest'; if (Test-Path $z) { Remove-Item -Recurse -Force $z }
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
