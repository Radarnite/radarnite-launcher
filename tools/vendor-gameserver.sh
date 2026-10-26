#!/usr/bin/env bash
# vendor-gameserver.sh - the match server DLL the launcher ships (gameserver\radarnite-server.dll), kept in
# gameserver-reboot/prebuilt/ as the one copy: CI cannot build it (an MSVC project whose MinHook and curl libraries are
# not in the repo), so it is built on Windows (just build-server-pc) and committed here, pinned by its manifest.
#
#   tools/vendor-gameserver.sh import <radarnite-server.dll>   scrub it, gate it, copy it in, write the manifest
#   tools/vendor-gameserver.sh check                          the DLL matches the manifest and the manifest was made
#                                                             from the committed sources (exit 1 otherwise; outside a
#                                                             git checkout, such as a synced mirror, only the DLL)
#   tools/vendor-gameserver.sh sources                        print the id of the committed sources
#
# The manifest (gameserver-reboot/prebuilt/radarnite-server.manifest) is "# sources <id>" and "<sha256>  <file>". The
# sources id hashes the committed tree of gameserver-reboot/ except prebuilt/ (git ls-tree HEAD), so `check` fails
# when the server's sources were changed and committed without importing a DLL built from them: a push to main then
# never ships a server older than its sources. import refuses uncommitted changes under gameserver-reboot/ (except
# prebuilt/), so the DLL it takes is the build of the committed tree.
# import applies release.sh's same-length scrub (release.sh's own scrub then changes nothing, and the shipped DLL is
# this one byte for byte) and its privacy gate. release.sh stages this DLL over the payload's and checks it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
P="$ROOT/gameserver-reboot/prebuilt"; DLL="$P/radarnite-server.dll"; M="$P/radarnite-server.manifest"
die() { echo "vendor-gameserver: $*" >&2; exit 1; }
sha256() { if command -v shasum >/dev/null; then shasum -a 256 "$1" | cut -d' ' -f1; else sha256sum "$1" | cut -d' ' -f1; fi; }

sources() {
  git -C "$ROOT" ls-tree HEAD -- gameserver-reboot/ | grep -v $'\tgameserver-reboot/prebuilt$' | sha256 /dev/stdin | cut -c1-16
}

check() {
  [ -f "$M" ] || die "no $M"
  [ -f "$DLL" ] || die "no $DLL"
  local want h f
  want="$(sed -n 's/^# sources \([0-9a-f]\{16\}\)$/\1/p' "$M")"; [ -n "$want" ] || die "$M has no '# sources <id>' line"
  read -r h f < <(grep -v '^#' "$M")
  [ "$f" = radarnite-server.dll ] || die "$M lists '$f', not radarnite-server.dll"
  [ "$(sha256 "$DLL")" = "$h" ] || die "$DLL is not the one in its manifest"
  if ! git -C "$ROOT" rev-parse --verify -q HEAD >/dev/null 2>&1; then
    echo "match server DLL $(echo "$h" | cut -c1-12) as in its manifest (sources not checked: no git checkout here)"; return 0
  fi
  [ "$(sources)" = "$want" ] || die "the match server's sources changed since its DLL was vendored: build it (just build-server-pc) and run tools/vendor-gameserver.sh import gameserver-reboot/out/radarnite-server.dll"
  echo "match server DLL $(echo "$h" | cut -c1-12) built from the committed sources $want"
}

import() {
  local src="${1:?usage: vendor-gameserver.sh import <radarnite-server.dll>}"
  [ "$(head -c 2 "$src")" = MZ ] || die "not a Windows DLL: $src"
  [ -z "$(git -C "$ROOT" status --porcelain -- gameserver-reboot/ ':!gameserver-reboot/prebuilt' ':!gameserver-reboot/out')" ] \
    || die "gameserver-reboot/ has uncommitted changes: commit them, rebuild, then import"
  local tmp; tmp="$(mktemp -d "${TMPDIR:-/tmp}/rl-gameserver.XXXXXX")"; trap 'rm -rf "$tmp"' RETURN
  cp "$src" "$tmp/radarnite-server.dll"
  "$ROOT/tools/release.sh" scrub "$tmp"
  "$ROOT/tools/release.sh" privacy-scan "$tmp" || die "privacy gate failed on $src"
  mkdir -p "$P"; cp "$tmp/radarnite-server.dll" "$DLL"
  { echo "# Radarnite match server (gameserver-reboot, just build-server-pc), vendored by tools/vendor-gameserver.sh"
    echo "# sources $(sources)"
    echo "$(sha256 "$DLL")  radarnite-server.dll"; } > "$M"
  check
}

case "${1:-}" in
  import) shift; import "$@" ;;
  check) check ;;
  sources) sources ;;
  *) die "usage: vendor-gameserver.sh import <dll> | check | sources" ;;
esac
