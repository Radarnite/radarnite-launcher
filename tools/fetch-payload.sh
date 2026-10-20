#!/usr/bin/env bash
# fetch-payload.sh - copy the prebuilt part of the program folder from the PC's dev tree, for tools/release.sh
# --payload. Same selection as the 2.x tools/build-release.ps1: redirect\, patches\ and tools\ whole, from
# gameserver\ only radarnite-server.dll, THIRD-PARTY-NOTICES.txt and README-offline.md; no .pdb. Nothing else of the
# dev tree (config.json, node\, backend\, ui\, src\) is read. release.sh scrubs and gates it.
#   tools/fetch-payload.sh <dest-dir | mac:<dir relative to ~>> [dev tree, default S:\FortniteBuilds\launcher]
# A mac: destination streams PC -> this box -> Mac without touching this box's disk. PC_HOST / MAC_HOST override
# the ssh aliases; the PC side runs under the shared PC lock (/tmp/rn-launcher-pc.lock) when flock exists.
set -euo pipefail
DEST="${1:?usage: fetch-payload.sh <dest-dir | mac:dir> [dev tree]}"
SRC="${2:-S:\\FortniteBuilds\\launcher}"
PC="${PC_HOST:-windows}"; MAC="${MAC_HOST:-mac}"
LOCK=(); command -v flock >/dev/null && LOCK=(flock /tmp/rn-launcher-pc.lock)

pc_tar() {
  "${LOCK[@]}" ssh -o BatchMode=yes "$PC" "tar -c -f - -C \"$SRC\" --exclude=*.pdb redirect patches tools gameserver/radarnite-server.dll THIRD-PARTY-NOTICES.txt README-offline.md"
}

case "$DEST" in
  mac:*)
    d="${DEST#mac:}"
    pc_tar | ssh -o BatchMode=yes "$MAC" "rm -rf ~/'$d' && mkdir -p ~/'$d' && tar -x -f - -C ~/'$d' && du -sk ~/'$d' | cut -f1 && find ~/'$d' -type f | wc -l"
    ;;
  *)
    rm -rf "$DEST"; mkdir -p "$DEST"
    pc_tar | tar -x -f - -C "$DEST"
    echo "$(du -sk "$DEST" | cut -f1) KB, $(find "$DEST" -type f | wc -l) files in $DEST"
    ;;
esac
