#!/usr/bin/env bash
# test-match-pc.sh - one background match on the PC with this checkout's fnbody.dll, match server DLL and backend
# (tools/test-match-bg.ps1 does the run there), results in dist/test-match/. Developer test, never in CI.
#   tools/test-match-pc.sh <pc work dir> [minutes] ["NAME=value;NAME=value" for the game processes]
# RN_PLAYMODE=legal|legal-vbucks|unlocked: the run's config.json play mode (profile "bgtest"; default none).
# RN_BOTS_AI=1: the bots use the game's own AI; RN_BOT_SKILL=1-10 their difficulty.
# RN_AUTOSTART=<s>: the bus leaves s seconds after joining (default 0: the match stays in the warmup).
# RN_BOTS=<n>: how many bots (default 6).
# Waits (RN_WAIT_MIN, default 10 minutes) while the PC is busy, another game process runs or the Paks just changed.
# The launcher folder of the run (rc\) is a copy
# of the installed launcher, refreshed from it on every run, with this checkout's files on top:
# pakpatch/out/fnbody.dll, gameserver-reboot/out/radarnite-server.dll and play/out/radarnite-play.exe when built,
# backend/*.mjs and backend/seasons always.
set -euo pipefail
DIR="${1:?usage: test-match-pc.sh <pc dir> [minutes] [env]}"
MIN="${2:-4}"
ENVS="${3:-}"
FWD="${DIR//\\//}"
ROOT="${RN_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$ROOT"

# up to RN_WAIT_MIN minutes (default 10) for no game running, the CPU at 70 % or less, no game process of any build
# (other sessions test on the same 14.30: a second match server cannot listen on 7777) and the build's Paks unchanged
# for 3 minutes (a pak rewritten while the server loads the map crashed it), else exit 75
until_ok=$(( $(date +%s) + ${RN_WAIT_MIN:-10} * 60 ))
paks='S:\FortniteBuilds\14.30\FortniteGame\Content\Paks'
while :; do
  load=$(machine-load get windows)
  game=$(jq -r .fortnite <<< "$load"); cpu=$(jq -r '[.cpu, .avg_30s] | max | floor' <<< "$load")
  busy=$(timeout 60 pc ps - <<PS 2>/dev/null | tr -d '\r' | tail -1
\$n = @(Get-Process FortniteClient-Win64-Shipping -ErrorAction SilentlyContinue).Count
\$w = (Get-ChildItem '$paks' -File -ErrorAction SilentlyContinue | Measure-Object LastWriteTime -Maximum).Maximum
"\$n \$([int]((Get-Date) - \$w).TotalSeconds)"
PS
)
  procs=${busy%% *}; pak_age=${busy##* }
  if [ "$game" = false ] && [ "$cpu" -le 70 ] && [ "${procs:-1}" = 0 ] && [ "${pak_age:-0}" -ge 180 ]; then break; fi
  echo "waiting: game $game, CPU ${cpu}%, game processes ${procs:-?}, Paks written ${pak_age:-?} s ago" >&2
  [ "$(date +%s)" -lt "$until_ok" ] || { echo "not starting" >&2; exit 75; }
  sleep 30
done

inst='%LOCALAPPDATA%\Programs\Radarnite Launcher'
pc sh "if not exist $DIR mkdir $DIR"
pc sh "robocopy \"$inst\" $DIR\\rc /MIR /XD cache port /XF config.json update.log \"Uninstall Radarnite Launcher.exe\" /NFL /NDL /NJH /NJS /NP >nul & if errorlevel 8 (exit 1) else (exit 0)"
[ -f pakpatch/out/fnbody.dll ] && scp -q pakpatch/out/fnbody.dll "windows:$FWD/rc/patches/fnbody.dll"
[ -f gameserver-reboot/out/radarnite-server.dll ] && scp -q gameserver-reboot/out/radarnite-server.dll "windows:$FWD/rc/gameserver/radarnite-server.dll"
[ -f play/out/radarnite-play.exe ] && scp -q play/out/radarnite-play.exe "windows:$FWD/rc/radarnite-play.exe"
scp -q backend/*.mjs "windows:$FWD/rc/backend/"
[ -d backend/seasons ] && scp -q -r backend/seasons "windows:$FWD/rc/backend/"
scp -q tools/test-match-bg.ps1 "windows:$FWD/test-match-bg.ps1"
pc sh "if exist $DIR\\out\\done.flag del $DIR\\out\\done.flag"

# a hidden scheduled task in the signed-in session (the game cannot create its swap chain from the ssh session)
pc ps - <<EOF
\$vbs = '$DIR\\hidden.vbs'
Set-Content -Encoding ascii \$vbs 'Set sh = CreateObject("WScript.Shell")
a = ""
For i = 0 To WScript.Arguments.Count - 1
  a = a & " """ & WScript.Arguments(i) & """"
Next
sh.Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File" & a, 0, True'
\$sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
\$arg = '"' + \$vbs + '" "$DIR\\test-match-bg.ps1" -Root "$DIR" -Minutes $MIN -Env "$ENVS" -PlayMode "${RN_PLAYMODE:-}" -AutoStart ${RN_AUTOSTART:-0} -Bots ${RN_BOTS:-6} -BotsAI ${RN_BOTS_AI:-0} -BotSkill ${RN_BOT_SKILL:-6}'
\$a = New-ScheduledTaskAction -Execute 'wscript.exe' -Argument \$arg
\$p = New-ScheduledTaskPrincipal -UserId \$sid -LogonType Interactive
Register-ScheduledTask -TaskName 'rl-test-match' -Action \$a -Principal \$p -Force | Out-Null
Start-ScheduledTask -TaskName 'rl-test-match'
Start-Sleep 3
Unregister-ScheduledTask -TaskName 'rl-test-match' -Confirm:\$false
'started'
EOF

deadline=$(( $(date +%s) + MIN * 60 + 240 ))
until pc sh "if exist $DIR\\out\\done.flag (exit 0) else (exit 1)" >/dev/null 2>&1; do
  [ "$(date +%s)" -lt "$deadline" ] || { echo "the run did not finish in time" >&2; break; }
  sleep 20
done
rm -rf dist/test-match; mkdir -p dist/test-match
# the shared ssh connection sometimes refuses new sessions: retry, the later tries on a connection of their own
fetch() {
  local i opts=()
  for i in 1 2 3 4 5; do
    rm -f dist/test-match.tar
    ssh -o BatchMode=yes "${opts[@]}" windows "tar -c -f - -C $DIR\\out $1" > dist/test-match.tar 2>/dev/null &&
      tar -x -C dist/test-match -f dist/test-match.tar 2>/dev/null && { rm -f dist/test-match.tar; return 0; }
    opts=(-o ControlPath=none -o ConnectTimeout=20); sleep 15
  done
  rm -f dist/test-match.tar; return 1
}
fetch "--exclude=shots ." || echo "could not fetch the results" >&2
fetch shots || true
# every run's results stay in dist/test-runs/<time> (the next run replaces dist/test-match)
mkdir -p dist/test-runs; cp -r dist/test-match "dist/test-runs/$(date +%m%d-%H%M%S)"
ls -la dist/test-match | head -30
cat dist/test-match/run.log
