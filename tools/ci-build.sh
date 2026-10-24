#!/usr/bin/env bash
# ci-build.sh - build the Radarnite Launcher 2.x installer, its .sha256 and the program-folder zip on Linux or macOS
# (.github/workflows/release.yml does it on ubuntu-latest; the same command works on any Unix box with the .NET 8 SDK,
# makensis, 7z and perl). tools/build-release.ps1 is the Windows equivalent from the dev tree.
#
#   tools/ci-build.sh --version <X.Y.Z[-pre.N]> [--payload <dir>] [--out <dir>] [--dotnet <path>] [--makensis <path>]
#       [--ban <text>]... [--keep-stage]
#
# version: stamped into the launcher (dotnet publish -p:Version, what the updater compares) and the installer
#          (-DVERSION, its version resource and DisplayVersion); the asset names carry it too.
# payload: the prebuilt part of the program folder (tools/payload-from-release.sh, default: downloaded by it).
#
# stage: dotnet publish (self-contained win-x64, deterministic, no pdb) -> payload (node\, redirect\, patches\,
# tools\, gameserver\radarnite-server.dll, notices, README-offline.md) + backend\server.mjs from this checkout -> strip
# every .pdb -> same-length scrub of builder paths in the payload (as tools/build-release.ps1 does) -> privacy gate ->
# makensis -> RadarniteLauncherSetup-<version>.exe, its .sha256 ("<sha256>  <name>", no newline: the format the updater reads with First64Hex) and RadarniteLauncher-<version>.zip (the staging as is;
# no .sha256 for it: the 2.x updater takes any .sha256 asset of the release as the installer's).
# config.json is never staged: the launcher creates it on first run and the installer must not overwrite it.
#
# Privacy gate (case-insensitive, UTF-8 and UTF-16LE) on every staged file: any "/Users/", "\Users\", "/home/",
# "agent-work"; this machine's account name (unless it is the GitHub runner's generic "runner", part of .NET type
# names), full name and home; the repo and payload folders; every --ban and every line of $RN_PRIVACY_BAN. Identities
# are read from the environment, never written here (this repo is public), and reported as "identity #n".
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "ci-build: $*" >&2; exit 1; }
sha256() { if command -v shasum >/dev/null; then shasum -a 256 "$1" | cut -d' ' -f1; else sha256sum "$1" | cut -d' ' -f1; fi; }

version="" payload="" out="$ROOT/dist" dotnet="" makensis="" keep=0
BANS=()
add_ban() { local s="$1"; s="${s%/}"; [ "${#s}" -ge 4 ] && BANS+=("$s"); return 0; }
while [ $# -gt 0 ]; do
  case "$1" in
    --version) version="$2"; shift 2 ;;
    --payload) payload="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --dotnet) dotnet="$2"; shift 2 ;;
    --makensis) makensis="$2"; shift 2 ;;
    --ban) add_ban "$2"; shift 2 ;;
    --keep-stage) keep=1; shift ;;
    *) die "unknown option $1" ;;
  esac
done
num='(0|[1-9][0-9]*)'
[[ "$version" =~ ^$num\.$num\.$num(-[0-9a-z-]+\.$num)?$ ]] || die "--version must be X.Y.Z or X.Y.Z-<id>.N, got '$version'"
version_num="${version%%-*}"
[ -n "$dotnet" ] || dotnet="$(command -v dotnet || true)"; [ -x "$dotnet" ] || die "dotnet not found (--dotnet)"
[ -n "$makensis" ] || makensis="$(command -v makensis || true)"; [ -x "$makensis" ] || die "makensis not found (--makensis)"
command -v 7z >/dev/null || die "7z not found"
command -v perl >/dev/null || die "perl not found"

stage="$(mktemp -d "${TMPDIR:-/tmp}/rl-stage.XXXXXX")"
cleanup() { [ "$keep" = 1 ] && { echo "stage kept: $stage"; return; }; rm -rf "$stage"; }
trap cleanup EXIT
app="$stage/app"; mkdir -p "$app"
if [ -z "$payload" ]; then payload="$stage/payload"; "$ROOT/tools/payload-from-release.sh" "$payload"; fi
[ -d "$payload" ] || die "no payload folder $payload"

# ---------------------------------------------------------------- banned strings
add_ban "${HOME:-}"; add_ban "$ROOT"; add_ban "$(cd "$payload" && pwd)"
for u in "${USER:-}" "${LOGNAME:-}"; do [ "$u" = runner ] || add_ban "$u"; done
if [ "$(uname)" = Darwin ]; then add_ban "$(id -F 2>/dev/null || true)"
else add_ban "$(getent passwd "${USER:-}" 2>/dev/null | cut -d: -f5 | cut -d, -f1 || true)"; fi
if [ -n "${RN_PRIVACY_BAN:-}" ]; then while IFS= read -r l; do add_ban "$l"; done <<<"$RN_PRIVACY_BAN"; fi

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
# scrub_payload <file|dir> : same-length rewrite, in place, UTF-8 and UTF-16LE, of the banned identities and of any
# "<drive>:\Users\<name>", "/Users/<name>" or "/home/<name>" left in the prebuilt payload binaries (third-party PDB
# paths baked in by their authors' machines or CI). Only the payload: the launcher must come out of dotnet publish clean.
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

# ---------------------------------------------------------------- launcher: dotnet publish
echo "== dotnet publish $version (self-contained win-x64, deterministic, no pdb)"
rm -rf "$ROOT/launcher/bin" "$ROOT/launcher/obj"   # always a clean build (a dev tree may hold another configuration's output)
(cd "$ROOT" && "$dotnet" publish launcher/FnOffline.csproj -c Release -r win-x64 --self-contained -o "$app" --nologo -v q \
  -p:Version="$version" -p:DebugType=none -p:DebugSymbols=false -p:Deterministic=true -p:ContinuousIntegrationBuild=true \
  -p:TreatWarningsAsErrors=false)
[ -s "$app/RadarniteLauncher.exe" ] || die "dotnet publish left no RadarniteLauncher.exe"
[ -s "$app/radarnite.ico" ] || die "dotnet publish left no radarnite.ico"

# ---------------------------------------------------------------- the rest of the program folder
echo "== payload and backend"
cp -R "$payload"/. "$app/"
mkdir -p "$app/backend"; cp "$ROOT/backend/server.mjs" "$app/backend/"
rm -f "$app/config.json"
find "$app" -type f -iname '*.pdb' -delete
echo "== scrub builder paths out of the prebuilt payload (same length, in place)"
for e in "$payload"/*; do scrub_payload "$app/$(basename "$e")"; done
for f in RadarniteLauncher.exe RadarniteLauncher.dll ui/index.html node/node.exe backend/server.mjs redirect/fnredirect.dll \
    gameserver/radarnite-server.dll tools/sigfit-installed.ps1 THIRD-PARTY-NOTICES.txt README-offline.md; do
  [ -s "$app/$f" ] || die "staging lacks $f"
done
cp "$app/radarnite.ico" "$stage/radarnite.ico"
echo "   $(find "$app" -type f | wc -l | tr -d ' ') files, $(du -sk "$app" | cut -f1) KB"

echo "== privacy gate on the staging"
privacy_scan "$app" || die "the staging carries a builder identity or home path; nothing is packaged"

# ---------------------------------------------------------------- installer, .sha256, zip
mkdir -p "$out"; out="$(cd "$out" && pwd)"   # absolute: the zip is written from inside the staging
setup="$out/RadarniteLauncherSetup-$version.exe"
zip="$out/RadarniteLauncher-$version.zip"
rm -f "$setup" "$setup.sha256" "$zip"
echo "== makensis"
"$makensis" -V2 "-DVERSION=$version" "-DVERSION_NUM=$version_num" "-DSRC=$stage" "-DOUT=$setup" "$ROOT/radarnite.nsi"
[ -s "$setup" ] || die "makensis produced no installer"
# the payload is LZMA-compressed inside: this only catches a plaintext leak in the installer's own strings
privacy_scan "$setup" || die "the installer carries a banned string in plaintext"
printf '%s  %s' "$(sha256 "$setup")" "$(basename "$setup")" > "$setup.sha256"
echo "== zip"
(cd "$app" && 7z a -tzip -mx=9 -bso0 -bsp0 "$zip" '*') || die "7z could not write $zip"

echo "== done"
ls -l "$setup" "$setup.sha256" "$zip"
cat "$setup.sha256"; echo
