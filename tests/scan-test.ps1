# Build scan test: odd folders never break the list. Usage: scan-test.ps1 <RadarniteLauncher.exe>
param($exe = "S:\FortniteBuilds\launcher\RadarniteLauncher.exe")
$root = Join-Path $env:TEMP ("rl-scantest-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
function FakeBuild($dir, $version) {
  $w = Join-Path $dir "FortniteGame\Binaries\Win64"; New-Item -ItemType Directory -Force $w | Out-Null
  [IO.File]::WriteAllBytes((Join-Path $w "FortniteClient-Win64-Shipping.exe"), [Text.Encoding]::Unicode.GetBytes("xx++Fortnite+Release-$version" + [char]0))
}
try {
  FakeBuild "$root\17.50" "17.50-CL-17388565"                                   # normal
  FakeBuild "$root\3.6-CL-4019403\Fortnite" "3.6-CL-4019403"                   # archive with its own top folder
  New-Item -ItemType Directory -Force "$root\14.30-damaged" | Out-Null            # only a pak (partial extraction)
  Set-Content "$root\14.30-damaged\pakchunk11-WindowsClient.pak" "x"
  New-Item -ItemType Directory -Force "$root\half\FortniteGame\Content" | Out-Null # FortniteGame without the exe
  New-Item -ItemType Directory -Force "$root\empty" | Out-Null                    # empty folder
  New-Item -ItemType Directory -Force "$root\launcher" | Out-Null                 # the launcher itself: skipped silently
  Set-Content "$root\3.6-CL-4019403.7z" "x"                                       # a file, not a folder
  $out = & $exe --scan $root | Out-String
  $out
  $ok = $true
  foreach ($must in @('build 17.50 | 17.50 | CL 17388565', 'build 3.6-CL-4019403 | 3.6 | CL 4019403', 'lobby Yes | match Yes',
                      'skip 14.30-damaged | no FortniteGame folder', 'skip half | FortniteGame\Binaries\Win64\FortniteClient-Win64-Shipping.exe is missing', 'skip empty |')) {
    if (-not $out.Contains($must)) { "MISSING: $must"; $ok = $false } }
  if ($out -match 'launcher') { "UNEXPECTED: launcher listed"; $ok = $false }
  if ($ok) { "SCAN TEST PASSED" } else { "SCAN TEST FAILED"; exit 1 }
} finally { Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue }
