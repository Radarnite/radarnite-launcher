#!/usr/bin/env bash
# payload-from-release.sh - the prebuilt part of the program folder, for tools/release.sh --payload, taken from a
# published installer pinned by sha256: what CI ships.
#   tools/payload-from-release.sh <dest-dir> [<version> <sha256>]     default: 2.2.1
# The installer is downloaded from this repo's release v<version> (or read from $RN_PAYLOAD_SETUP), checked against
# <sha256>, extracted with 7z and never run. Same selection as tools/fetch-payload.sh copies from the dev tree:
# node\, redirect\, patches\ and tools\ whole, from gameserver\ only radarnite-server.dll, THIRD-PARTY-NOTICES.txt and
# README-offline.md; no .pdb. The installer's old pak tools (tools\fn-modkit-sigfit, sigfit-installed.ps1) are
# dropped: they ship only from paktools/ (tools/vendor-pak-tools.sh), and release.sh refuses a payload carrying them.
# The launcher itself and backend\server.mjs come from this checkout.
# Not built in CI: node.exe is the Node.js runtime; the notices and LargePakPatch have no source here; fn-modkit (the
# pak tools) builds from fn-viewer, a separate repo, and is vendored in paktools/; cosmetics-gen builds against a CUE4Parse checkout ($(CUE4ParseDir)) that is not in
# the repo; the gameserver is an MSVC project whose MinHook and curl libraries are not in the repo; and the DLLs loaded
# into the game (redirect, the pak patches) ship as the binaries that were played with, not rebuilt by another
# toolchain. A new payload ships by publishing a release built with `just release` and pinning it here.
# patches\fnmods.dll (the 14.30 mods) is not in 2.2.1: release.yml builds it from pakpatch/fnmods.c, release.sh --fnmods.
# gameserver\radarnite-server.dll is taken here for the payload's shape only: release.sh replaces it with the vendored
# build of the committed server sources, gameserver-reboot/prebuilt (tools/vendor-gameserver.sh).
set -euo pipefail
DEST="${1:?usage: payload-from-release.sh <dest-dir> [<version> <sha256>]}"
VERSION="${2:-2.2.1}"
SHA256="${3:-54d59b107b80ad36053a7c724c5171fc763d05f0dcc3e5f8b73e87c2e7fd4060}"
REPO="${RN_PAYLOAD_REPO:-Radarnite/radarnite-launcher}"
die() { echo "payload: $*" >&2; exit 1; }
sha256() { if command -v shasum >/dev/null; then shasum -a 256 "$1" | cut -d' ' -f1; else sha256sum "$1" | cut -d' ' -f1; fi; }
command -v 7z >/dev/null || die "7z not found (p7zip-full / sevenzip)"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/rl-payload.XXXXXX")"; trap 'rm -rf "$tmp"' EXIT
setup="$tmp/RadarniteLauncherSetup-$VERSION.exe"
if [ -n "${RN_PAYLOAD_SETUP:-}" ]; then
  cp "$RN_PAYLOAD_SETUP" "$setup"
else
  curl -sSfL --retry 3 -o "$setup" "https://github.com/$REPO/releases/download/v$VERSION/RadarniteLauncherSetup-$VERSION.exe"
fi
got="$(sha256 "$setup")"
[ "$got" = "$SHA256" ] || die "RadarniteLauncherSetup-$VERSION.exe has sha256 $got, not the pinned $SHA256"

7z x -y -bso0 -bsp0 -o"$tmp/x" "$setup" >/dev/null || die "7z could not extract $setup"
rm -rf "$DEST"; mkdir -p "$DEST/gameserver"
cp -R "$tmp/x/node" "$tmp/x/redirect" "$tmp/x/patches" "$tmp/x/tools" "$tmp/x/THIRD-PARTY-NOTICES.txt" "$tmp/x/README-offline.md" "$DEST/"
cp "$tmp/x/gameserver/radarnite-server.dll" "$DEST/gameserver/"
find "$DEST" -type f -iname '*.pdb' -delete
rm -rf "${DEST:?}/tools/fn-modkit-sigfit" "${DEST:?}/tools/sigfit-installed.ps1" "${DEST:?}/tools/game-guard.ps1" "${DEST:?}/tools/pak-tools.manifest"

# what the installer needs of a payload
for f in node/node.exe redirect/fnredirect.dll gameserver/radarnite-server.dll \
    tools/cosmetics-gen/fn-cosmetics-gen.exe THIRD-PARTY-NOTICES.txt README-offline.md; do
  [ -s "$DEST/$f" ] || die "missing $f"
done
ls "$DEST"/patches/fnpakpatch.dll* >/dev/null 2>&1 || die "no pak patch in patches/"
echo "payload from v$VERSION (sha256 ${SHA256:0:12}...): $(du -sk "$DEST" | cut -f1) KB, $(find "$DEST" -type f | wc -l | tr -d ' ') files in $DEST"
