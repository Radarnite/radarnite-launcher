#!/usr/bin/env bash
# Build RadarniteUI.exe for x86_64 Windows with zig (Mac or Linux): native/ui/build.sh [out.exe]
# Compiler/linker flags live in build.rsp (shared with build.cmd).
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
OUT="${1:-RadarniteUI.exe}"
"$ZIG" rc /fo app.res app.rc
"$ZIG" c++ @build.rsp -o "$OUT"
rm -f app.res
ls -l "$OUT" | awk '{print $5, $9}'
