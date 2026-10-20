#!/usr/bin/env bash
# fetch-payload.sh - copy the prebuilt part of the program folder from the PC's dev tree, for tools/release.sh
# --payload. Same selection as the 2.x tools/build-release.ps1: redirect\, patches\ and tools\ whole, from
# gameserver\ only radarnite-server.dll, THIRD-PARTY-NOTICES.txt and README-offline.md; no .pdb. Nothing else of the
# dev tree (config.json, node\, backend\, ui\, src\) is read. release.sh scrubs and gates it.
#   tools/fetch-payload.sh <dest-dir | mac:<dir relative to ~>> [dev tree, default S:\FortniteBuilds\launcher]
# A mac: destination streams PC -> this box -> Mac without touching this box's disk. PC_HOST / MAC_HOST override
# the ssh aliases. Runs under the shared PC lock (/tmp/rn-launcher-pc.lock) when flock exists.
set -euo pipefail
DEST="${1:?usage: fetch-payload.sh <dest-dir | mac:dir> [dev tree]}"
SRC="${2:-S:\\FortniteBuilds\\launcher}"
PC="${PC_HOST:-windows}"; MAC="${MAC_HOST:-mac}"
# hold the shared PC lock for the whole copy (taken before the pipe opens, so neither ssh sits idle waiting for it)
if command -v flock >/dev/null; then exec 9>/tmp/rn-launcher-pc.lock; flock 9; fi

pc_tar() {
  ssh -o BatchMode=yes "$PC" "tar -c -f - -C \"$SRC\" --exclude=*.pdb redirect patches tools gameserver/radarnite-server.dll THIRD-PARTY-NOTICES.txt README-offline.md"
}

copy() {
  case "$DEST" in
    mac:*)
      local d="${DEST#mac:}"
      pc_tar | ssh -o BatchMode=yes "$MAC" "rm -rf ~/'$d' && mkdir -p ~/'$d' && tar -x -f - -C ~/'$d' && echo \"\$(du -sk ~/'$d' | cut -f1) KB, \$(find ~/'$d' -type f | wc -l | tr -d ' ') files in ~/$d (Mac)\""
      ;;
    *)
      rm -rf "$DEST"; mkdir -p "$DEST"
      pc_tar | tar -x -f - -C "$DEST"
      echo "$(du -sk "$DEST" | cut -f1) KB, $(find "$DEST" -type f | wc -l) files in $DEST"
      ;;
  esac
}
# ssh exits 255 when a shared connection drops mid-stream (seen twice in a row, then fine): retry the whole copy
for attempt in 1 2 3; do
  if copy; then exit 0; else rc=$?; fi
  echo "fetch-payload: attempt $attempt failed (exit $rc)" >&2
  [ "$rc" -eq 255 ] || exit "$rc"
  sleep 5
done
exit 255
