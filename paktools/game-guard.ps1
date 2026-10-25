# Refuses a test launch of the offline build while the owner has a game open or an anti-cheat is running: the
# test hooks the build (redirect DLL with hardware breakpoints) and opens a game window, which must never happen
# next to an anti-cheat, nor while the owner plays. Prints "free" (exit 0) or "busy: <what>" (exit 6).
# What counts, all read from the system rather than from a list of games:
#   - anti-cheat processes and services: EasyAntiCheat*, BattlEye (BEService*, BEDaisy), Riot Vanguard (vgc, vgtray, vgk)
#   - the game Steam reports as running (HKCU\Software\Valve\Steam RunningAppID, and Apps\<id>\Running = 1)
#   - Windows' own game detection: GameBarPresenceWriter runs while Windows sees a game in the foreground session
#   - a process started from an Epic Games install folder (Epic launcher manifests), when Epic is installed
#   - a Fortnite client already running (any build)
$busy = @()
$ac = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^(EasyAntiCheat|BEService|BEDaisy|vgc$|vgtray$|vgk$)' }
if ($ac) { $busy += "anti-cheat process " + (($ac | ForEach-Object ProcessName | Sort-Object -Unique) -join ',') }
$acs = Get-Service -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Running' -and $_.Name -match '^(EasyAntiCheat|BEService|BEDaisy|vgc$|vgk$)' }
if ($acs) { $busy += "anti-cheat service " + (($acs | ForEach-Object Name | Sort-Object -Unique) -join ',') }
$steam = Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue
if ($steam -and $steam.RunningAppID -and [int64]$steam.RunningAppID -ne 0) {
  $name = (Get-ItemProperty "HKCU:\Software\Valve\Steam\Apps\$($steam.RunningAppID)" -ErrorAction SilentlyContinue).Name
  $busy += "Steam game running: $($steam.RunningAppID) $name"
} else {
  Get-ChildItem 'HKCU:\Software\Valve\Steam\Apps' -ErrorAction SilentlyContinue | ForEach-Object {
    $p = Get-ItemProperty $_.PSPath; if ($p.Running -eq 1) { $busy += "Steam game running: $($_.PSChildName) $($p.Name)" } }
}
if (Get-Process GameBarPresenceWriter -ErrorAction SilentlyContinue) { $busy += "Windows reports a game running (GameBarPresenceWriter)" }
$man = Get-ChildItem "$env:ProgramData\Epic\EpicGamesLauncher\Data\Manifests\*.item" -ErrorAction SilentlyContinue
if ($man) {
  $roots = @($man | ForEach-Object { try { (Get-Content $_.FullName -Raw | ConvertFrom-Json).InstallLocation } catch { } } | Where-Object { $_ })
  foreach ($p in Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path }) {
    foreach ($r in $roots) { if ($p.Path.StartsWith($r.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { $busy += "Epic game running: $($p.ProcessName)" } }
  }
}
if (Get-Process FortniteClient-Win64-Shipping -ErrorAction SilentlyContinue) { $busy += "a Fortnite client is running" }
if ($busy.Count -gt 0) { "busy: " + (($busy | Sort-Object -Unique) -join '; '); exit 6 }
"free"; exit 0
