<#
  build-release.ps1 - build the Radarnite Launcher installer (RadarniteLauncherSetup-<version>.exe) + its .sha256.

  The version is read from launcher\FnOffline.csproj <Version> (the single source of truth: the auto-updater reads the
  same value from the assembly). Bump it there for each release; nothing else carries the version.

  What it does:
    1. dotnet publish of the launcher (framework-dependent win-x64) into <stage>\app
    2. copy the runtime assets that are not part of the C# project (node, backend, redirect, gameserver, patches, tools)
       from the working launcher folder into <stage>\app   (NOT src, backups, webview2 cache or anything from Epic)
    3. ship a clean default config.json (empty identity: the launcher fills it in on first run)
    4. makensis radarnite.nsi -> RadarniteLauncherSetup-<version>.exe
    5. write RadarniteLauncherSetup-<version>.exe.sha256 (the auto-updater verifies against this)

  Nothing from Epic (builds, .paks, AES keys, device auth, .fp-epic) is ever staged: only our own launcher + tools.
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
if (-not $Version) {
  $Version = ([xml](Get-Content $csproj)).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1
}
if (-not $Version) { throw "could not read <Version> from $csproj" }
if (-not $Makensis) {
  $Makensis = (Get-ChildItem "$env:LOCALAPPDATA\fp-tools\nsis" -Recurse -Filter makensis.exe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
}
if (-not $Makensis -or -not (Test-Path $Makensis)) { throw "makensis.exe not found (set -Makensis)" }
if (-not (Test-Path $Dotnet)) { throw "dotnet not found at $Dotnet" }

Write-Host "Version   : $Version"
Write-Host "makensis  : $Makensis"
Write-Host "dotnet    : $Dotnet"

$stage = Join-Path $env:TEMP 'radarnite-stage'
$app   = Join-Path $stage 'app'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $app | Out-Null

Write-Host "`n== dotnet publish =="
& $Dotnet publish $csproj -c Release -o $app --nologo
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)" }
# No debug symbols or build leftovers in the shipped launcher root.
Get-ChildItem $app -Filter *.pdb -File | Remove-Item -Force -ErrorAction SilentlyContinue

Write-Host "`n== copy runtime assets =="
foreach ($d in 'node','backend','redirect','gameserver','patches','tools') {
  $s = Join-Path $Runtime $d
  if (-not (Test-Path $s)) { throw "missing runtime folder: $s" }
  Copy-Item $s (Join-Path $app $d) -Recurse -Force
  Write-Host "  + $d"
}
foreach ($f in 'README-offline.md','THIRD-PARTY-NOTICES.txt') {
  $s = Join-Path $Runtime $f
  if (Test-Path $s) { Copy-Item $s (Join-Path $app $f) -Force }
}

# Clean default config: empty identity (the launcher generates a random name + local account id on first run);
# builds folder is set by the installer via --set-builds-folder.
@'
{
  "displayName": "",
  "accountId": "",
  "port": 3551,
  "buildsFolder": ""
}
'@ | Set-Content -Path (Join-Path $app 'config.json') -Encoding UTF8

# The .nsi reads ${SRC}\radarnite.ico for the installer icon as well as from app\.
Copy-Item (Join-Path $app 'radarnite.ico') (Join-Path $stage 'radarnite.ico') -Force

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = Join-Path $OutDir "RadarniteLauncherSetup-$Version.exe"
if (Test-Path $out) { Remove-Item $out -Force }

Write-Host "`n== makensis =="
& $Makensis "/DVERSION=$Version" "/DSRC=$stage" "/DOUT=$out" (Join-Path $SrcRoot 'radarnite.nsi')
if ($LASTEXITCODE -ne 0) { throw "makensis failed ($LASTEXITCODE)" }
if (-not (Test-Path $out)) { throw "installer not produced" }

$hash = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
"$hash  RadarniteLauncherSetup-$Version.exe" | Set-Content -Path "$out.sha256" -Encoding ascii -NoNewline

$mb = [math]::Round((Get-Item $out).Length/1MB,1)
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "`n== done =="
Write-Host "installer : $out ($mb MB)"
Write-Host "sha256    : $hash"
