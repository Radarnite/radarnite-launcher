<#
  build-release.ps1 - build the Radarnite Launcher installer (RadarniteLauncherSetup-<version>.exe) + its .sha256.

  Version: read from launcher\FnOffline.csproj <Version> (single source of truth; the auto-updater reads the same
  value from the assembly). Bump it there for each release; nothing else carries the version.

  Privacy: the shipped binaries must not leak the developer's name or home path. The launcher is built deterministic
  with no PDB (no CodeView path); every PDB is stripped; the prebuilt tool/gameserver binaries have the developer
  name rewritten in place (same byte length) to an anonymous token. The build FAILS if any staged file still carries
  the name. Nothing from Epic (builds, .paks, AES keys, device auth, .fp-epic) is ever staged.

  The installer must never overwrite the user's config.json: it is NOT staged (the launcher creates it on first run
  and keeps it on upgrades).
#>
param(
  [string]$Version,
  [string]$SrcRoot  = 'S:\FortniteBuilds\launcher\src',
  [string]$Runtime  = 'S:\FortniteBuilds\launcher',
  [string]$OutDir   = 'S:\FortniteBuilds\installer',
  [string]$Dotnet   = "$env:LOCALAPPDATA\fp-dotnet\dotnet.exe",
  [string]$Makensis
)
$ErrorActionPreference = 'Stop'

$csproj = Join-Path $SrcRoot 'launcher\FnOffline.csproj'
if (-not $Version) { $Version = ([xml](Get-Content $csproj)).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1 }
if (-not $Version) { throw "could not read <Version> from $csproj" }
if (-not $Makensis) { $Makensis = (Get-ChildItem "$env:LOCALAPPDATA\fp-tools\nsis" -Recurse -Filter makensis.exe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }
if (-not $Makensis -or -not (Test-Path $Makensis)) { throw "makensis.exe not found (set -Makensis)" }
if (-not (Test-Path $Dotnet)) { throw "dotnet not found at $Dotnet" }
Write-Host "Version=$Version  makensis=$Makensis  dotnet=$Dotnet"

# ---- personal-path scrub (same-length, in-place; UTF-8 and UTF-16LE), via native Latin1 string search (fast) ----
# ISO-8859-1 maps each byte 0..255 to the same code point, so a file's raw bytes round-trip through a string and
# String.IndexOf/Replace (ordinal) run natively. UTF-16LE needles are the chars interleaved with NUL. Each pair is
# the SAME length so byte offsets never shift (safe to patch a PE/.NET image in place). Longest first.
$L1 = [Text.Encoding]::GetEncoding(28591)
function U16([string]$s){ -join ($s.ToCharArray() | ForEach-Object { "$_`0" }) }
# The identity to scrub is read from the build environment, never hard-coded here (this script is public). Prebuilt
# binaries bake in the builder's home path (CodeView/PDB records): we rewrite the full home path and the bare user
# name with same-length neutral tokens so no real name and no C:\Users\<name> survive.
function SameLen([string]$s){ $seed = 'buildagent' + ('0' * 64); $seed.Substring(0, $s.Length) }
$homeDir  = $env:USERPROFILE                # e.g. C:\Users\<name>
$userName = Split-Path $homeDir -Leaf       # e.g. <name>
$PAIRS = @()                                # longest first so the home path goes before the bare name
if ($homeDir)  { $PAIRS += @{ from = $homeDir;  to = (SameLen $homeDir)  } }
if ($userName) { $PAIRS += @{ from = $userName; to = (SameLen $userName) } }
# strings that must be ZERO in every staged file after scrubbing (the verify gate)
$BANNED = @($homeDir, $userName) | Where-Object { $_ }
function Count-Banned([string]$path){
  $s = $L1.GetString([IO.File]::ReadAllBytes($path)); $c = 0
  foreach ($b in $BANNED) { foreach ($ndl in @($b, (U16 $b))) { $i = 0; while (($i = $s.IndexOf($ndl, $i, [StringComparison]::Ordinal)) -ge 0) { $c++; $i += $ndl.Length } } }
  return $c
}
function Scrub-Name([string]$path){
  $s = $L1.GetString([IO.File]::ReadAllBytes($path)); $orig = $s
  foreach ($p in $PAIRS) { $s = $s.Replace($p.from, $p.to).Replace((U16 $p.from), (U16 $p.to)) }
  # Generic: any remaining "C:\Users\<user>" (UTF-8, the form embedded in prebuilt third-party PDB paths, e.g. NuGet
  # libraries built on their authors' machines or CI). Replace the "C:\Users\<user>" segment (up to the next
  # backslash) with a same-length neutral token, so no "C:\Users\" survives in any shipped binary.
  $s = [regex]::Replace($s, 'C:\\Users\\[^\\\x00-\x1F]+', {
    param($m); $b = 'C:\build'; if ($m.Value.Length -gt $b.Length) { $b + ('0' * ($m.Value.Length - $b.Length)) } else { $b.Substring(0, $m.Value.Length) }
  })
  if ($s -eq $orig) { return $false }
  [IO.File]::WriteAllBytes($path, $L1.GetBytes($s))
  return $true
}

# ---- staging ----
$stage = Join-Path $env:TEMP 'radarnite-stage'
$app   = Join-Path $stage 'app'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $app | Out-Null

Write-Host "`n== dotnet publish (self-contained, deterministic, no pdb) =="
& $Dotnet publish $csproj -c Release -o $app --nologo `
  -p:SelfContained=true --self-contained `
  -p:DebugType=none -p:DebugSymbols=false `
  -p:Deterministic=true -p:ContinuousIntegrationBuild=true
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)" }

Write-Host "`n== copy runtime assets =="
foreach ($d in 'node','backend','redirect','patches','tools') {
  $s = Join-Path $Runtime $d
  if (-not (Test-Path $s)) { throw "missing runtime folder: $s" }
  Copy-Item $s (Join-Path $app $d) -Recurse -Force
  Write-Host "  + $d"
}
# gameserver: ONLY the server DLL (the folder also holds dev junk: alternate builds, diffs, pdb, backups)
$gsDst = Join-Path $app 'gameserver'
New-Item -ItemType Directory -Force -Path $gsDst | Out-Null
Copy-Item (Join-Path $Runtime 'gameserver\radarnite-server.dll') (Join-Path $gsDst 'radarnite-server.dll') -Force
Write-Host "  + gameserver\radarnite-server.dll (only)"
foreach ($f in 'README-offline.md','THIRD-PARTY-NOTICES.txt') {
  $s = Join-Path $Runtime $f
  if (Test-Path $s) { Copy-Item $s (Join-Path $app $f) -Force }
}
# NOTE: config.json is intentionally NOT staged (the installer must not overwrite the user's settings).

Write-Host "`n== strip every pdb =="
$pdbs = Get-ChildItem $app -Recurse -File -Filter *.pdb
$pdbs | Remove-Item -Force
Write-Host "  removed $($pdbs.Count) pdb"

Write-Host "`n== scrub developer name / home path in all staged files =="
$scrubbed = 0
Get-ChildItem $app -Recurse -File | ForEach-Object { if (Scrub-Name $_.FullName) { $scrubbed++; Write-Host "  scrubbed $($_.FullName.Substring($app.Length+1))" } }
Write-Host "  files scrubbed: $scrubbed"

Write-Host "`n== verify: no staged file carries the builder name or home path =="
$leaks = @()
Get-ChildItem $app -Recurse -File | ForEach-Object { $c = Count-Banned $_.FullName; if ($c -gt 0) { $leaks += "$($_.FullName) ($c)" } }
if ($leaks.Count -gt 0) { $leaks | ForEach-Object { Write-Host "  LEAK $_" }; throw "builder identity still present in $($leaks.Count) staged file(s); aborting" }
$nFiles = (Get-ChildItem $app -Recurse -File).Count
Write-Host "  OK: 0 occurrences of the builder name/home path (UTF-8 and UTF-16) in $nFiles staged files"
# Hard gate: no "C:\Users\" of any kind may remain in a shipped file (not the builder's, not a third party's).
$cusers = @()
Get-ChildItem $app -Recurse -File | ForEach-Object { $s = $L1.GetString([IO.File]::ReadAllBytes($_.FullName)); if ($s.IndexOf('C:\Users\', [StringComparison]::OrdinalIgnoreCase) -ge 0 -or $s.IndexOf((U16 'C:\Users\'), [StringComparison]::OrdinalIgnoreCase) -ge 0) { $cusers += $_.FullName.Substring($app.Length+1) } }
if ($cusers.Count -gt 0) { $cusers | ForEach-Object { Write-Host "  C:\Users LEFT: $_" }; throw "'C:\Users\' still present in $($cusers.Count) staged file(s); aborting" }
Write-Host "  OK: no 'C:\Users\' in any staged file"

# The .nsi reads ${SRC}\radarnite.ico for the installer icon as well as from app\.
Copy-Item (Join-Path $app 'radarnite.ico') (Join-Path $stage 'radarnite.ico') -Force

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = Join-Path $OutDir "RadarniteLauncherSetup-$Version.exe"
if (Test-Path $out) { Remove-Item $out -Force }

Write-Host "`n== makensis =="
& $Makensis "/DVERSION=$Version" "/DSRC=$stage" "/DOUT=$out" (Join-Path $SrcRoot 'radarnite.nsi')
if ($LASTEXITCODE -ne 0) { throw "makensis failed ($LASTEXITCODE)" }
if (-not (Test-Path $out)) { throw "installer not produced" }

# The installer payload is LZMA-compressed, so a raw scan of the .exe cannot confirm the payload; the pre-compression
# staging scan above is the real gate. This only catches an accidental plaintext leak in the installer's own strings.
if ((Count-Banned $out) -gt 0) { throw "the installer binary carries a banned string in plaintext" }

$hash = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
"$hash  RadarniteLauncherSetup-$Version.exe" | Set-Content -Path "$out.sha256" -Encoding ascii -NoNewline
$mb = [math]::Round((Get-Item $out).Length/1MB,1)
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "`n== done =="
Write-Host "installer : $out ($mb MB)"
Write-Host "sha256    : $hash"
