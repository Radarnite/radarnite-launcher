#!/usr/bin/env bash
# smoke-linux.sh <binary>: start the backend on a throwaway build folder, check the ready line and a few routes, stop it.
set -euo pipefail
bin=${1:?usage: smoke-linux.sh <radarnite-backend binary>}
dir=$(mktemp -d)
pid=
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; wait 2>/dev/null; rm -rf "$dir"' EXIT
mkdir -p "$dir/build/offline"
printf '{"displayName":"Smoke","accountId":"0123456789abcdef0123456789abcdef"}' > "$dir/config.json"
printf '{"source":"smoke","items":[{"templateId":"AthenaCharacter:CID_001","variants":[]}]}' > "$dir/build/offline/cosmetics.json"
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
start=$(date +%s%N)
"$bin" --build "$dir/build" --config "$dir/config.json" --port "$port" > "$dir/out" 2>&1 &
pid=$!
for i in $(seq 100); do [ -s "$dir/out" ] && break; sleep 0.01; done
ready_ms=$(( ($(date +%s%N) - start) / 1000000 ))
line=$(head -1 "$dir/out")
[ "$line" = "ready 127.0.0.1:$port" ] || { echo "unexpected first line: $line" >&2; exit 1; }
ua='Fortnite/++Fortnite+Release-17.50-CL-17388565 Windows/10'
curl -sf -A "$ua" "http://127.0.0.1:$port/offline/health"; echo
curl -sf -A "$ua" -X POST "http://127.0.0.1:$port/fortnite/api/game/v2/profile/x/client/QueryProfile?profileId=athena" -d '{}' | head -c 120; echo
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/Fortnite/API/Version"); [ "$code" = 200 ] || { echo "mixed case route: $code" >&2; exit 1; }
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/nope"); [ "$code" = 404 ] || { echo "unknown route: $code" >&2; exit 1; }
rss=$(awk '/VmRSS/ {print $2}' /proc/$pid/status)
ss -ltnp 2>/dev/null | rg -q "127.0.0.1:$port" || { echo "not listening on 127.0.0.1:$port" >&2; exit 1; }
echo "smoke ok: ready ${ready_ms} ms (incl. process start), RSS ${rss} kB, unknown-routes.log: $(cat "$dir/build/offline/unknown-routes.log")"
