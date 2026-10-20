#!/usr/bin/env bash
# release-selftest.sh - end-to-end test of tools/release.sh + radarnite.nsi with placeholder exes built by zig
# (Mac or Linux with zig, makensis, 7z, perl, curl). Nothing built here is shipped.
#   tools/release-selftest.sh [workdir]        (ZIG=/opt/homebrew/bin/zig, MAKENSIS=..., REF_VERSION=2.2.1)
# Checks: a clean build passes; a builder path or identity planted in an exe (UTF-8 and UTF-16LE) fails the gate;
# user data in the payload is refused; the gate on the extracted installer catches what LZMA hides from a raw scan;
# the .sha256 is byte-identical in format to a published 2.x asset; the real 2.x payload passes the gate.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REL="$ROOT/tools/release.sh"
W="${1:-$(mktemp -d "${TMPDIR:-/tmp}/rl-release-selftest.XXXXXX")}"
ZIG="${ZIG:-zig}"; MAKENSIS="${MAKENSIS:-$(command -v makensis || echo /opt/homebrew/bin/makensis)}"
REF_VERSION="${REF_VERSION:-2.2.1}"
mkdir -p "$W"; cd "$W" || exit 1
pass=0; fail=0
ok() { echo "PASS $*"; pass=$((pass + 1)); }
ko() { echo "FAIL $*"; fail=$((fail + 1)); }
sha() { if command -v shasum >/dev/null; then shasum -a 256 "$1" | cut -d' ' -f1; else sha256sum "$1" | cut -d' ' -f1; fi; }

# ---- placeholder inputs ----
cat > stub.c <<'EOF'
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#ifndef PLANT
#define PLANT ""
#endif
#ifndef WPLANT
#define WPLANT L""
#endif
const char *volatile plant = PLANT;
const wchar_t *volatile wplant = WPLANT;
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--set-builds-folder")) return argc > 2 ? 0 : 2;
    printf("%s placeholder %s%ls\n", NAME, plant, wplant);
    return 0;
}
EOF
cc() { "$ZIG" cc -target x86_64-windows-gnu -O2 -s "$@" stub.c 2>&1 | grep -v '^$' || true; }
mkdir -p in
cc -DNAME='"RadarniteLauncher"' -o in/RadarniteLauncher.exe
cc -DNAME='"radarnite-backend"' -o in/radarnite-backend.exe
cc -DNAME='"RadarniteLauncher"' "-DPLANT=\"/Users/${USER:-builder}/agent-work/planted/main.c\"" -o in/launcher-planted.exe
cc -DNAME='"radarnite-backend"' '-DWPLANT=L"C:\\Users\\Planted\\src\\backend.pdb"' -o in/backend-planted.exe
cc -DNAME='"fn-cosmetics-gen"' -o in/tool.exe
[ -f in/RadarniteLauncher.exe ] && [ -f in/backend-planted.exe ] && ok "zig built the placeholder exes" || { ko "zig build"; exit 1; }

rm -rf payload; mkdir -p payload/redirect payload/gameserver payload/patches payload/tools/cosmetics-gen payload/tools/fn-modkit-sigfit
printf 'placeholder dll\n' > payload/redirect/fnredirect.dll
printf 'placeholder dll\n' > payload/gameserver/radarnite-server.dll
printf 'placeholder dll\n' > payload/patches/fnpakpatch.dll
printf '# placeholder\n' > payload/tools/sigfit-installed.ps1
cp in/tool.exe payload/tools/cosmetics-gen/fn-cosmetics-gen.exe
printf 'no extension\n' > payload/tools/cosmetics-gen/LICENSE
printf 'pdb\n' > payload/tools/cosmetics-gen/fn-cosmetics-gen.pdb
# a third-party dll as found in NuGet packages: its author's PDB path, UTF-8 and UTF-16LE -> scrubbed, then clean
perl -e 'binmode STDOUT; print "MZ..RSDS C:\\Users\\someone\\.nuget\\lib.pdb\0..", join("", map { "$_\0" } split //, "C:\\Users\\someone\\src"), "\0\0"' > payload/tools/fn-modkit-sigfit/ThirdParty.dll
printf 'notices\n' > payload/THIRD-PARTY-NOTICES.txt
printf 'readme\n' > payload/README-offline.md

build() { "$REL" build --makensis "$MAKENSIS" --payload "${PAYLOAD:-payload}" --out out "$@"; }

# ---- 1. clean build, prerelease version ----
V=3.0.0-rc.1
if build --version $V --launcher in/RadarniteLauncher.exe --backend in/radarnite-backend.exe > t1.log 2>&1; then
  ok "clean build $V"
else ko "clean build $V"; tail -30 t1.log; fi
EXE="out/RadarniteLauncherSetup-$V.exe"
if [ -f "$EXE.sha256" ]; then
  [ "$(cat "$EXE.sha256")" = "$(sha "$EXE")  RadarniteLauncherSetup-$V.exe" ] && ok ".sha256 matches the installer" || ko ".sha256 content"
  [ "$(tail -c 1 "$EXE.sha256" | od -An -c | tr -d ' ')" != '\n' ] && ok ".sha256 has no trailing newline" || ko ".sha256 newline"
fi
grep -q 'scrubbed .*ThirdParty.dll' t1.log && ok "third-party PDB paths scrubbed in the payload" || ko "payload scrub"
grep -q 'removed 1 pdb' t1.log && ok "payload pdb stripped" || ko "pdb strip"
if [ -f "$EXE" ]; then
  rm -rf x1 && mkdir x1 && 7z x -y -bso0 -bsp0 -ox1 "$EXE" >/dev/null
  [ -f x1/tools/cosmetics-gen/LICENSE ] && [ -f x1/radarnite-backend.exe ] && [ ! -e x1/tools/cosmetics-gen/fn-cosmetics-gen.pdb ] \
    && ok "installer holds the payload (incl. extension-less files), no pdb" || ko "installer content"
  [ -f "x1/\$PLUGINSDIR/own-processes.ps1" ] || ls x1 x1/\$PLUGINSDIR 2>/dev/null | head -5
fi

# ---- 2/3. planted builder path / identity in the 3.0 exes: the gate must fail ----
if build --version 3.0.0 --launcher in/launcher-planted.exe --backend in/radarnite-backend.exe > t2.log 2>&1; then
  ko "planted /Users/<user>/agent-work (UTF-8) was NOT caught"
else
  grep -q "LEAK .*RadarniteLauncher.exe: '/Users/'" t2.log && grep -q 'identity #' t2.log && grep -q "'agent-work'" t2.log \
    && ok "planted UTF-8 path caught (/Users/, identity, agent-work)" || { ko "UTF-8 plant: wrong report"; grep LEAK t2.log; }
fi
grep -q "${USER:-no-such-user}" t2.log && ko "the gate printed the identity into the log" || ok "identities are masked in the log"
if build --version 3.0.0 --launcher in/RadarniteLauncher.exe --backend in/backend-planted.exe > t3.log 2>&1; then
  ko "planted C:\\Users\\ (UTF-16LE) was NOT caught"
else
  grep -q "LEAK .*radarnite-backend.exe: '\\\\Users\\\\' x1 utf16le" t3.log && ok "planted UTF-16LE path caught" || { ko "UTF-16 plant: wrong report"; grep LEAK t3.log; }
fi

# ---- 4. user data in the payload is refused ----
rm -rf payload-bad && cp -R payload payload-bad && printf '{}' > payload-bad/config.json
if PAYLOAD=payload-bad build --version 3.0.0 --launcher in/RadarniteLauncher.exe --backend in/radarnite-backend.exe > t4.log 2>&1; then
  ko "config.json in the payload was accepted"
else grep -q 'must not contain: config.json' t4.log && ok "config.json in the payload refused" || ko "t4 wrong reason"; fi

# ---- 5. LZMA hides a leak from a raw scan; the extracted scan catches it ----
rm -rf stage5 && mkdir -p stage5/app && cp -R payload/. stage5/app/ && rm -f stage5/app/tools/cosmetics-gen/*.pdb stage5/app/tools/fn-modkit-sigfit/ThirdParty.dll
cp in/launcher-planted.exe stage5/app/RadarniteLauncher.exe; cp in/backend-planted.exe stage5/app/radarnite-backend.exe
cp "$ROOT/launcher/radarnite.ico" stage5/app/; cp "$ROOT/launcher/radarnite.ico" stage5/
"$MAKENSIS" -V1 -DVERSION=3.0.0 -DSRC="$W/stage5" -DOUT="$W/planted-setup.exe" "$ROOT/radarnite.nsi" > t5.log 2>&1
if "$REL" privacy-scan planted-setup.exe > t5raw.log 2>&1; then ok "raw scan of the LZMA installer sees nothing (why the gate extracts it)"; else echo "note: raw scan hit:"; cat t5raw.log; fi
rm -rf x5 && mkdir x5 && 7z x -y -bso0 -bsp0 -ox5 planted-setup.exe >/dev/null
if "$REL" privacy-scan x5 > t5x.log 2>&1; then ko "extracted planted installer passed the gate"
else grep -q 'utf8' t5x.log && grep -q 'utf16le' t5x.log && ok "extracted installer: both encodings caught" || { ko "t5 report"; cat t5x.log; }; fi

# ---- 6. .sha256 format == the published 2.x asset, byte for byte ----
base="https://github.com/Radarnite/radarnite-launcher/releases/download/v$REF_VERSION"
ref="RadarniteLauncherSetup-$REF_VERSION.exe"
[ -f "$ref" ] || curl -sSfL -o "$ref" "$base/$ref" || ko "download $ref"
curl -sSfL -o "$ref.sha256.asset" "$base/$ref.sha256" || ko "download $ref.sha256"
"$REL" sha256-line "$ref" > "$ref.sha256.ours"
cmp -s "$ref.sha256.ours" "$ref.sha256.asset" && ok ".sha256 byte-identical to the $REF_VERSION release asset" || { ko ".sha256 format differs from $REF_VERSION"; od -c "$ref.sha256.ours" | tail -2; }

# ---- 7. a release with the real 2.x payload (tools, gameserver, redirect, patches) passes after the scrub ----
# raw, those third-party dlls carry their CI builders' PDB paths (/home/runner/work/...): informational only
rm -rf ref-x && mkdir ref-x && 7z x -y -bso0 -bsp0 -oref-x "$ref" >/dev/null
rm -rf payload-real && mkdir payload-real
for p in tools gameserver redirect patches THIRD-PARTY-NOTICES.txt README-offline.md; do cp -R "ref-x/$p" payload-real/; done
"$REL" privacy-scan payload-real > t7raw.log 2>&1; echo "info: raw $REF_VERSION payload: $(tail -1 t7raw.log) ($(grep -c "'/home/'" t7raw.log) of them '/home/')"
if PAYLOAD=payload-real build --version 3.0.0-rc.2 --launcher in/RadarniteLauncher.exe --backend in/radarnite-backend.exe > t7.log 2>&1; then
  ok "release with the real $REF_VERSION payload: $(grep '^installer:' t7.log | sed 's|.*/||')"
else ko "release with the real $REF_VERSION payload"; grep -E 'LEAK|release:' t7.log | head -20; fi

echo "== $pass passed, $fail failed (work dir $W)"
[ "$fail" -eq 0 ]
