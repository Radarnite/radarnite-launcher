#!/usr/bin/env bash
# ci-version.sh - the version, tag and release notes of the commit being built (.github/workflows/release.yml).
# Needs the full history and every tag (actions/checkout with fetch-depth: 0); runs anywhere with git and bash.
#   tools/ci-version.sh [--branch <name>] [--notes <file>]      branch default: $GITHUB_REF_NAME, else the checked out one
# Base X.Y.Z = the highest stable tag vX.Y.Z plus one patch, or the committed VERSION file when that is higher.
#   main         -> vX.Y.Z, a release (it becomes releases/latest: every installed launcher updates to it)
#   other branch -> vX.Y.Z-<branch>.N, a GitHub prerelease (never releases/latest), N = 1 + the highest N already tagged
#                   for that base and branch. <branch> = the name in lower case with every run of characters outside
#                   [0-9a-z-] turned into one "-" (feat/3.0 -> feat-3-0): valid semver identifiers, so the updater
#                   (native/core/update.cpp) orders them by semver precedence.
# No release (publish=false) when the head commit message contains "[skip release]", or when HEAD already carries
# this line's tag (a re-run). Prints key=value lines, also appended to $GITHUB_OUTPUT when it is set:
#   version, tag, prerelease (true|false), base, since (the tag the notes start from, "" = none), publish, reason
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "ci-version: $*" >&2; exit 1; }
cd "$ROOT"

branch="${GITHUB_REF_NAME:-}"; notes=""
while [ $# -gt 0 ]; do
  case "$1" in
    --branch) branch="$2"; shift 2 ;;
    --notes) notes="$2"; shift 2 ;;
    *) die "unknown option $1" ;;
  esac
done
[ -n "$branch" ] || branch="$(git rev-parse --abbrev-ref HEAD)"
[ "$branch" != HEAD ] || die "detached HEAD: pass --branch <name>"
main="${RN_RELEASE_BRANCH:-main}"
num='(0|[1-9][0-9]*)'
semsort() { sort -t. -k1,1n -k2,2n -k3,3n; }

# ---------------------------------------------------------------- base version
[ -f VERSION ] || die "no VERSION file at the repo root"
floor="$(tr -d ' \r\n' < VERSION)"
[[ "$floor" =~ ^$num\.$num\.$num$ ]] || die "VERSION must be X.Y.Z, got '$floor'"
last="$(git tag -l 'v*' | { grep -E "^v$num\.$num\.$num$" || true; } | sed 's/^v//' | semsort | tail -1)"
if [ -n "$last" ]; then
  IFS=. read -r ma mi pa <<<"$last"; next="$ma.$mi.$((pa + 1))"
else
  next="0.0.1"
fi
base="$(printf '%s\n%s\n' "$next" "$floor" | semsort | tail -1)"

# ---------------------------------------------------------------- version and tag
prev=""
if [ "$branch" = "$main" ]; then
  version="$base"; prerelease=false
  done_tags="$(git tag --points-at HEAD | { grep -E "^v$num\.$num\.$num$" || true; })"
else
  id="$(printf '%s' "$branch" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^0-9a-z-]+/-/g; s/-+/-/g; s/^-//; s/-$//' | cut -c1-40 | sed -E 's/-+$//')"
  [ -n "$id" ] || id="branch"
  [[ "$id" =~ ^[0-9]+$ ]] && id="b$id"   # an all-digit identifier would compare as a number (and may lead with 0)
  n="$(git tag -l "v$base-$id.*" | { grep -E "^v${base//./\\.}-$id\.$num$" || true; } | sed 's/.*\.//' | sort -n | tail -1)"
  [ -n "$n" ] && prev="v$base-$id.$n"
  version="$base-$id.$(( ${n:-0} + 1 ))"; prerelease=true
  done_tags="$(git tag --points-at HEAD | { grep -E "^v$num\.$num\.$num-$id\.$num$" || true; })"
fi
"$ROOT/native/core/version.sh" "$version" >/dev/null || die "'$version' is not a version the build accepts"
tag="v$version"
since="${prev:-${last:+v$last}}"

# ---------------------------------------------------------------- publish or not
publish=true; reason="new $([ "$prerelease" = true ] && echo prerelease || echo release) $tag"
msg="$(git log -1 --format=%B HEAD)"
if grep -qiF '[skip release]' <<<"$msg"; then
  publish=false; reason="the head commit message asks to skip the release"
elif [ -n "$done_tags" ]; then
  publish=false; reason="HEAD is already released as ${done_tags//$'\n'/ }"
fi

# ---------------------------------------------------------------- release notes (from the commits)
if [ -n "$notes" ]; then
  range="HEAD"; [ -n "$since" ] && range="$since..HEAD"
  total="$(git rev-list --no-merges --count "$range")"; max=60
  {
    echo "Radarnite Launcher $version"
    echo
    if [ "$prerelease" = true ]; then
      echo "Prerelease from branch \`$branch\` ($(git rev-parse --short=7 HEAD)). Installed release builds never get it: they follow the latest release only."
      echo
    fi
    echo "Changes since ${since:-the first commit}:"
    echo
    git log --no-merges --format='- %s (%h)' -n "$max" "$range"
    [ "$total" -le "$max" ] || echo "- ... and $((total - max)) more"
    echo
    echo "Install: run RadarniteLauncherSetup-$version.exe (per-user, no administrator).$([ "$prerelease" = true ] || echo ' Installed launchers update themselves.')"
  } > "$notes"
fi

out="$(printf '%s\n' "version=$version" "tag=$tag" "prerelease=$prerelease" "base=$base" "since=$since" "publish=$publish" "reason=$reason")"
echo "$out"
[ -z "${GITHUB_OUTPUT:-}" ] || echo "$out" >> "$GITHUB_OUTPUT"
