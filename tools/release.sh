#!/usr/bin/env bash
# release.sh - package a Radarnite Launcher 3.x release (runs on the Mac: makensis + 7z from Homebrew; any Unix box
# with makensis, 7z and perl works the same).
#
#   tools/release.sh build --version 3.0.0-rc.1 --launcher <RadarniteLauncher.exe> --play <radarnite-play.exe> \
#       --payload <dir> [--out <dir>] [--icon <ico>] [--nsi <radarnite.nsi>] [--makensis <path>] [--ban <text>]...
#       [--no-version-check]
#   tools/release.sh privacy-scan <file|dir>...   the privacy gate alone (exit 1 on any hit)
#   tools/release.sh scrub <dir>                  the payload's same-length path scrub alone, in place (vendor-pak-tools.sh)
#   tools/release.sh sha256-line <file>           the .sha256 asset content for <file> (no trailing newline)
#   tools/release.sh product-version <exe>        the ProductVersion of an exe's version resource ("" = none)
#
# build: stage -> scrub the prebuilt payload -> privacy gate on the staging -> makensis -> extract the installer with
# 7z, compare it file by file with the staging and run the gate again on what is really inside -> write
# RadarniteLauncherSetup-<version>.exe and its .sha256 ("<sha256>  <name>", no newline: the 2.x asset format, which
# the 2.x updater reads with First64Hex).
# --version is the one the launcher was built as (RN_VERSION, native/core/version.sh): its ProductVersion must be
# exactly --version, so the updater, the version resource, DisplayVersion and the installer name always agree
# (--no-version-check: only for placeholder exes without a version resource, tools/release-selftest.sh).
#
# The backend is Node: node\node.exe from the payload runs backend\server.mjs, copied from this checkout.
# --payload is the rest of the program folder exactly as installed: node\node.exe, redirect\fnredirect.dll,
# gameserver\radarnite-server.dll, patches\fnpakpatch.dll, tools\cosmetics-gen, THIRD-PARTY-NOTICES.txt,
# README-offline.md (tools/fetch-payload.sh copies it from the PC). The pak tools (tools\fn-modkit-sigfit,
# sigfit-installed.ps1, game-guard.ps1, pak-tools.manifest) come only from paktools/ (tools/vendor-pak-tools.sh): a
# payload that carries its own copy is refused, and the staged ones must match paktools/pak-tools.manifest.
# Those prebuilt binaries get the same-length path scrub of the 2.x build. The 3.0 exes
# are never scrubbed: they must come out of their builds clean (-s / strip, --remap-path-prefix), or the gate fails.
#
# Privacy gate (case-insensitive, UTF-8 and UTF-16LE): any "/Users/", "\Users\" (C:\Users\ or another drive),
# "/home/", "agent-work"; this machine's account name, full name and home; the repo and input folders; every --ban
# and every line of $RN_PRIVACY_BAN. Identities are read from the environment, never written here (this repo is
# public), and reported as "identity #n" so they never reach a log.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "release: $*" >&2; exit 1; }
sha256() { if command -v shasum >/dev/null; then shasum -a 256 "$1" | cut -d' ' -f1; else sha256sum "$1" | cut -d' ' -f1; fi; }

# ---------------------------------------------------------------- banned strings
BANS=()        # identities / builder paths (reported masked)
add_ban() { local s="$1"; s="${s%/}"; [ "${#s}" -ge 4 ] && BANS+=("$s"); return 0; }
collect_bans() {
  add_ban "${USER:-}"; add_ban "${LOGNAME:-}"; add_ban "${HOME:-}"
  local full=""
  if [ "$(uname)" = Darwin ]; then full="$(id -F 2>/dev/null || true)"
  else full="$(getent passwd "${USER:-}" 2>/dev/null | cut -d: -f5 | cut -d, -f1 || true)"; fi
  add_ban "$full"
  add_ban "$ROOT"
  if [ -n "${RN_PRIVACY_BAN:-}" ]; then while IFS= read -r l; do add_ban "$l"; done <<<"$RN_PRIVACY_BAN"; fi
}

# privacy_scan <file|dir>... : prints every hit, returns 1 if any
privacy_scan() {
  local list; list="$(mktemp)"
  find "$@" -type f -print0 > "$list"
  RN_BANS="$(printf '%s\n' "${BANS[@]+"${BANS[@]}"}")" perl -e '
    use strict; use warnings; use Encode qw(decode encode);
    my @generic = ("/Users/", "\\Users\\", "/home/", "agent-work");
    my @ids = grep { length } split /\n/, ($ENV{RN_BANS} // "");
    my @pats;   # [label, utf8 regex, utf16le regex]
    my $mk = sub { my ($label, $s) = @_; my $u = decode("UTF-8", $s);
      my $r8 = join "", map { quotemeta } split //, encode("UTF-8", $u);
      my $r16 = join "", map { quotemeta } split //, encode("UTF-16LE", $u);
      push @pats, [$label, qr/$r8/i, qr/$r16/i]; };
    $mk->("\x27$_\x27", $_) for @generic;
    my $n = 0; $mk->("identity #" . ++$n, $_) for @ids;
    local $/ = "\0"; my ($hits, $files) = (0, 0);
    open my $fl, "<", $ARGV[0] or die; my @files = map { chomp; $_ } <$fl>;
    for my $f (@files) {
      $files++; open my $fh, "<:raw", $f or do { warn "cannot read $f\n"; $hits++; next };
      local $/; my $d = <$fh>; close $fh; $d //= "";
      for my $p (@pats) {
        for my $enc ([q(utf8), $p->[1]], [q(utf16le), $p->[2]]) {
          my $c = () = $d =~ /$enc->[1]/g; next unless $c;
          $d =~ $enc->[1]; my $at = $-[0];
          printf "LEAK %s: %s x%d %s (first at 0x%x)\n", $f, $p->[0], $c, $enc->[0], $at; $hits++;
        }
      }
    }
    printf "privacy: %d file(s), %d pattern(s), %s\n", $files, scalar(@pats), $hits ? "$hits HIT(S)" : "clean";
    exit($hits ? 1 : 0);
  ' "$list"
  local rc=$?; rm -f "$list"; return $rc
}

# scrub_payload <dir> : same-length, in place, UTF-8 and UTF-16LE (only for prebuilt payload binaries)
scrub_payload() {
  local list; list="$(mktemp)"
  find "$1" -type f -print0 > "$list"
  RN_BANS="$(printf '%s\n' "${BANS[@]+"${BANS[@]}"}")" perl -e '
    use strict; use warnings; use Encode qw(decode encode);
    my @ids = sort { length($b) <=> length($a) } grep { length } split /\n/, ($ENV{RN_BANS} // "");
    my $tok = sub { my $len = shift; substr("buildagent" . ("0" x 256), 0, $len) };
    my $u16 = sub { encode("UTF-16LE", decode("UTF-8", shift)) };
    local $/ = "\0"; open my $fl, "<", $ARGV[0] or die; my @files = map { chomp; $_ } <$fl>;
    my $changed = 0;
    for my $f (@files) {
      open my $fh, "<:raw", $f or die "cannot read $f\n"; my $d = do { local $/; <$fh> } // ""; close $fh;
      my $o = $d;
      for my $id (@ids) {                                  # the identities, longest first
        my $b8 = encode("UTF-8", decode("UTF-8", $id)); my $r8 = $tok->(length $b8);
        $d =~ s/\Q$b8\E/$r8/gi;
        my $b16 = $u16->($id); my $r16 = $u16->($tok->(length(decode("UTF-8", $id))));
        $d =~ s/\Q$b16\E/$r16/gi;
      }
      # any "<drive>:\Users\<name>" / "/Users/<name>" / "/home/<name>" left (third-party PDB paths): same-length token
      my $pad = sub { my ($m, $base) = @_; length($m) > length($base) ? $base . ("0" x (length($m) - length($base))) : substr($base, 0, length $m) };
      $d =~ s{([A-Za-z]:\\Users\\[^\\\x00-\x1F]+)}{$pad->($1, "C:\\build")}gie;
      $d =~ s{(/(?:Users|home)/[^/\x00-\x1F]+)}{$pad->($1, "/build")}gie;
      $d =~ s{((?:[A-Za-z]\x00:\x00\\\x00|/\x00)(?:U\x00s\x00e\x00r\x00s\x00|h\x00o\x00m\x00e\x00)[\\/]\x00(?:[^\\/\x00]\x00)+)}{
        my $m = $1; my $base = encode("UTF-16LE", "C:\\build"); my $z = encode("UTF-16LE", "0");
        length($m) > length($base) ? $base . ($z x ((length($m) - length($base)) / 2)) : substr($base, 0, length $m) }gie;
      next if $d eq $o;
      die "scrub changed the size of $f\n" if length($d) != length($o);
      open my $wh, ">:raw", $f or die; print $wh $d; close $wh; $changed++;
      print "  scrubbed $f\n";
    }
    print "scrub: $changed file(s) rewritten\n";
  ' "$list"
  local rc=$?; rm -f "$list"; return $rc
}

# hash_tree <dir> [skip-top-dir] : "<sha256>  ./<path>" for every file, sorted
hash_tree() {
  (cd "$1" && find . -type f ${2:+! -path "./$2/*"} | LC_ALL=C sort | while IFS= read -r f; do echo "$(sha256 "$f")  $f"; done)
}

sha256_line() { local f="$1"; printf '%s  %s' "$(sha256 "$f")" "$(basename "$f")"; }

# product_version <exe> : the ProductVersion string of its VS_VERSIONINFO (UTF-16LE key, padding, value), "" if none
product_version() {
  perl -e 'use strict; use warnings; use Encode qw(decode encode);
    open my $f, "<:raw", $ARGV[0] or die "cannot read $ARGV[0]\n"; local $/; my $d = <$f> // ""; close $f;
    my $k = encode("UTF-16LE", "ProductVersion\0");
    while ($d =~ /\Q$k\E/g) {
      my $at = pos($d); next if (pos($d) - length $k) % 2;
      $at += 2 while substr($d, $at, 2) eq "\0\0" && $at % 4;   # padding to a 32-bit boundary
      my $v = ""; while ($at + 1 < length $d) { my $c = substr($d, $at, 2); last if $c eq "\0\0"; $v .= $c; $at += 2 }
      print decode("UTF-16LE", $v); exit 0;
    }' "$1"
}

# ---------------------------------------------------------------- build
cmd_build() {
  local version="" launcher="" play="" payload="" out="$ROOT/dist" icon="$ROOT/launcher/radarnite.ico"
  local nsi="$ROOT/radarnite.nsi" makensis="" keep=0 vcheck=1
  while [ $# -gt 0 ]; do
    case "$1" in
      --version) version="$2"; shift 2 ;;
      --launcher) launcher="$2"; shift 2 ;;
        --play) play="$2"; shift 2 ;;
      --payload) payload="$2"; shift 2 ;;
      --out) out="$2"; shift 2 ;;
      --icon) icon="$2"; shift 2 ;;
      --nsi) nsi="$2"; shift 2 ;;
      --makensis) makensis="$2"; shift 2 ;;
      --ban) add_ban "$2"; shift 2 ;;
      --keep-stage) keep=1; shift ;;
      --no-version-check) vcheck=0; shift ;;
      *) die "unknown option $1" ;;
    esac
  done
  [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] || die "--version must be semver (3.0.0, 3.0.0-rc.1 or 3.0.0-feat-3-0.1), got '$version'"
  local vnum="${version%%-*}"
  for f in "$launcher" "$play" "$icon" "$nsi" "$ROOT/backend/server.mjs"; do [ -f "$f" ] || die "missing file: '$f'"; done
  [ -d "$payload" ] || die "missing payload folder: '$payload'"
  for f in "$launcher" "$play"; do [ "$(head -c 2 "$f")" = MZ ] || die "not a Windows executable: $f"; done
  if [ "$vcheck" = 1 ]; then
    local pv; pv="$(product_version "$launcher")"
    [ "$pv" = "$version" ] || die "the launcher's ProductVersion is '$pv', not $version: build it as $version (RN_VERSION=$version, just release $version)"
  fi
  [ -n "$makensis" ] || makensis="$(command -v makensis || echo /opt/homebrew/bin/makensis)"
  [ -x "$makensis" ] || die "makensis not found (brew install makensis, or --makensis)"
  command -v 7z >/dev/null || die "7z not found (brew install p7zip / sevenzip): needed to verify the installer"
  add_ban "$(cd "$(dirname "$launcher")" && pwd)"; add_ban "$(cd "$(dirname "$play")" && pwd)"; add_ban "$(cd "$payload" && pwd)"

  STAGE="$(mktemp -d "${TMPDIR:-/tmp}/radarnite-stage.XXXXXX")"; KEEP=$keep
  trap '[ "$KEEP" = 1 ] && echo "staging kept: $STAGE" || rm -rf "$STAGE"' EXIT
  local stage="$STAGE" app="$STAGE/app"; mkdir -p "$app"
  echo "== stage $version (numeric $vnum)$([ "$vcheck" = 1 ] && echo ", launcher ProductVersion $version")"
  cp -R "$payload/." "$app/"
  # the 3.0 program folder: no .NET runtime next to the launcher, no WebView2 page, never user data or anything from Epic
  local bad=""
  for p in backend radarnite-backend.exe ui runtimes webview2-radarnite config.json RadarniteLauncher.dll RadarniteLauncher.deps.json RadarniteLauncher.runtimeconfig.json; do
    [ -e "$app/$p" ] && bad="$bad $p"
  done
  for f in "$app"/*.dll; do [ -e "$f" ] && bad="$bad $(basename "$f")"; done   # root dlls: the installer deletes 2.x root dlls
  while IFS= read -r f; do bad="$bad ${f#$app/}"; done < <(find "$app" -type f \( -iname 'state.json' -o -iname 'cosmetics.json' \
      -o -iname 'custom-cosmetics.json' -o -iname '*.pak' -o -iname '*.utoc' -o -iname '*.ucas' -o -iname '*.sig' -o -iname '.fp-epic' \
      -o -iname '*deviceauth*' -o -iname 'config.json' \))
  [ -z "$bad" ] || die "the payload must not contain:$bad"
  # the pak tools: never an older copy from a payload, only paktools/ (one copy, pinned by its manifest)
  for p in tools/fn-modkit-sigfit tools/sigfit-installed.ps1 tools/game-guard.ps1 tools/pak-tools.manifest; do
    [ -e "$app/$p" ] && bad="$bad $p"
  done
  [ -z "$bad" ] || die "the payload must not carry its own pak tools (they ship from paktools/):$bad"
  "$ROOT/tools/vendor-pak-tools.sh" check || die "paktools/ does not match its manifest"
  mkdir -p "$app/tools"; cp -R "$ROOT/paktools/." "$app/tools/"
  local npdb; npdb=$(find "$app" -type f -iname '*.pdb' | wc -l | tr -d ' ')
  find "$app" -type f -iname '*.pdb' -delete
  echo "  removed $npdb pdb"
  echo "== scrub prebuilt payload"
  scrub_payload "$app"
  local h f
  while read -r h f; do
    [ "$(sha256 "$app/tools/$f")" = "$h" ] || die "staged tools/$f is not the one in paktools/pak-tools.manifest (re-vendor with tools/vendor-pak-tools.sh)"
  done < <(grep -v '^#' "$ROOT/paktools/pak-tools.manifest")
  echo "  pak tools $(sed -n 's/^# version //p' "$ROOT/paktools/pak-tools.manifest") staged as in the manifest"
  cp "$launcher" "$app/RadarniteLauncher.exe"
  [ -s "$app/node/node.exe" ] || die "the payload has no node/node.exe (the backend runs on it)"
  mkdir -p "$app/backend"; cp "$ROOT/backend/server.mjs" "$app/backend/"
  cp "$play" "$app/radarnite-play.exe"
  cp "$icon" "$app/radarnite.ico"; cp "$icon" "$stage/radarnite.ico"
  echo "== privacy gate (staging)"
  privacy_scan "$app" "$stage/radarnite.ico" || die "privacy gate failed on the staging"

  mkdir -p "$out"; out="$(cd "$out" && pwd)"
  local name="RadarniteLauncherSetup-$version.exe" exe="$out/RadarniteLauncherSetup-$version.exe"
  rm -f "$exe" "$exe.sha256"
  echo "== makensis $("$makensis" -VERSION)"
  "$makensis" -V2 -WX "-DVERSION=$version" "-DVERSION_NUM=$vnum" "-DSRC=$stage" "-DOUT=$exe" "$nsi"
  [ -f "$exe" ] || die "makensis produced no installer"

  echo "== verify the installer content"
  local x="$stage/extracted"; mkdir -p "$x"
  7z x -y -bso0 -bsp0 -o"$x" "$exe" >/dev/null || die "7z could not extract $exe"
  # 7z puts NSIS internals in \$PLUGINSDIR; the payload is at the root. 7-Zip 23+ (not p7zip 16.02) also extracts the
  # uninstaller makensis wrote (radarnite.nsi's WriteUninstaller): not staged, so compared without it, gated with it
  local want got
  want="$(hash_tree "$app")"
  got="$(hash_tree "$x" '$PLUGINSDIR' | { grep -v '  \./Uninstall Radarnite Launcher\.exe$' || true; })"
  if [ "$want" != "$got" ]; then
    { diff <(echo "$want") <(echo "$got") || true; } | head -20 >&2
    die "the installer content differs from the staging"
  fi
  echo "  $(echo "$got" | wc -l | tr -d ' ') files identical to the staging"
  echo "== privacy gate (installer and its extracted content)"
  privacy_scan "$x" "$exe" || { rm -f "$exe"; die "privacy gate failed on the built installer (deleted)"; }

  sha256_line "$exe" > "$exe.sha256"
  [ "$(cat "$exe.sha256")" = "$(sha256 "$exe")  $name" ] || die ".sha256 does not match"
  [ "$(wc -c < "$exe.sha256" | tr -d ' ')" -eq $((64 + 2 + ${#name})) ] || die ".sha256 has an unexpected size"
  echo "== done"
  echo "installer: $exe ($(( $(wc -c < "$exe") / 1048576 )) MB, $(echo "$got" | wc -l | tr -d ' ') files, staged $(du -sk "$app" | cut -f1) KB)"
  echo "sha256   : $(cut -d' ' -f1 "$exe.sha256")"
}

collect_bans
case "${1:-}" in
  build) shift; cmd_build "$@" ;;
  scrub) [ -d "${2:-}" ] || die "usage: release.sh scrub <dir>"; scrub_payload "$2" ;;
  privacy-scan) shift; [ $# -gt 0 ] || die "usage: release.sh privacy-scan <file|dir>..."; privacy_scan "$@" ;;
  sha256-line) [ -f "${2:-}" ] || die "usage: release.sh sha256-line <file>"; sha256_line "$2" ;;
  product-version) [ -f "${2:-}" ] || die "usage: release.sh product-version <exe>"; product_version "$2"; echo ;;
  *) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
