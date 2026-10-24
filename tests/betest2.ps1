"CPU " + (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average
$p = Start-Process S:\Fortnite\FortniteBuilds\launcher\node\node.exe -ArgumentList 'S:\Fortnite\FortniteBuilds\launcher\backend\server.mjs','--build','S:\Fortnite\FortniteBuilds\17.50','--config','S:\Fortnite\FortniteBuilds\launcher\config.json','--port','3551' -PassThru -WindowStyle Hidden
Start-Sleep 2
$h = @{ 'User-Agent' = 'Fortnite/++Fortnite+Release-17.50-CL-17388565 Windows/10.0.19045.1.256.64bit' }
$b = 'http://127.0.0.1:3551'
(Invoke-WebRequest -UseBasicParsing "$b/offline/health" -Headers $h).Content
$id = (Invoke-RestMethod -Method Post "$b/account/api/oauth/token" -Headers $h -Body 'grant_type=password&username=x@offline.local&password=offline').account_id
"account " + $id
$sw = [Diagnostics.Stopwatch]::StartNew()
$r = Invoke-RestMethod -Method Post "$b/fortnite/api/game/v2/profile/$id/client/QueryProfile?profileId=athena&rvn=-1" -Headers $h -Body '{}' -ContentType 'application/json'
"athena QueryProfile: " + $sw.ElapsedMilliseconds + " ms, items " + ($r.profileChanges[0].profile.items.PSObject.Properties | Measure-Object).Count + ", season " + $r.profileChanges[0].profile.stats.attributes.season_num
$it = $r.profileChanges[0].profile.items.'athenacharacter:cid_a_085_athena_commando_f_hardwood_k7zz1'
"sample item: " + ($it | ConvertTo-Json -Depth 6 -Compress).Substring(0,250)
$v = ($r.profileChanges[0].profile.items.PSObject.Properties | ? { $_.Value.attributes.variants.Count -gt 0 } | Measure-Object).Count
"items with styles: $v"
$eq = Invoke-RestMethod -Method Post "$b/fortnite/api/game/v2/profile/$id/client/SetCosmeticLockerSlot?profileId=athena&rvn=-1" -Headers $h -Body '{"lockerItem":"sandbox_loadout","category":"Character","itemToSlot":"AthenaCharacter:cid_a_085_athena_commando_f_hardwood_k7zz1","slotIndex":0,"variantUpdates":[]}' -ContentType 'application/json'
"after equip rvn " + $eq.profileRevision + " slot: " + ($eq.profileChanges[0].profile.items.sandbox_loadout.attributes.locker_slots_data.slots.Character.items -join ',')
"keychain entries: " + (Invoke-RestMethod "$b/fortnite/api/storefront/v2/keychain" -Headers $h).Count
"timeline season: " + (Invoke-RestMethod "$b/fortnite/api/calendar/v1/timeline" -Headers $h).channels.'client-events'.states[0].state.seasonNumber
"--- TCP of backend:"
Get-NetTCPConnection -OwningProcess $p.Id -ErrorAction SilentlyContinue | Select LocalAddress,LocalPort,RemoteAddress,RemotePort,State | ft -auto | Out-String
"--- UDP of backend:"; Get-NetUDPEndpoint -OwningProcess $p.Id -ErrorAction SilentlyContinue | ft -auto | Out-String
"backend RAM MB: " + [int]((Get-Process -Id $p.Id).WorkingSet64/1MB)
Stop-Process -Id $p.Id -Force; Start-Sleep 1
"left: " + @(Get-Process node -ErrorAction SilentlyContinue | ? { $_.Path -like 'S:\Fortnite\FortniteBuilds*' }).Count
# reset the test equip so the owner starts clean
Remove-Item S:\Fortnite\FortniteBuilds\17.50\offline\state.json -ErrorAction SilentlyContinue
