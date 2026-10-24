#!/usr/bin/env bash
# Build RadarniteLauncher.exe (the window over the real core) for x86_64 Windows with zig (Mac or Linux):
# native/ui/build.sh [out.exe]. Build the core first (native/core/build.sh -> ../core/out/librncore.a).
# Compiler/linker flags live in build.rsp (shared with build.cmd). The version resource (app.rc) takes the version of
# the core it links: ../core/out/obj/version.gen.h when the core was built with RN_VERSION, else version.h's default.
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
OUT="${1:-RadarniteLauncher.exe}"
[ -f ../core/out/librncore.a ] || { echo "missing ../core/out/librncore.a: run native/core/build.sh first" >&2; exit 1; }
RCFLAGS=()
if [ -f ../core/out/obj/version.gen.h ]; then RCFLAGS=(/i ../core/out/obj /d RN_VERSION_GEN); fi
"$ZIG" rc ${RCFLAGS[@]+"${RCFLAGS[@]}"} /fo app.res app.rc
"$ZIG" c++ @build.rsp -o "$OUT"
rm -f app.res
ls -l "$OUT" | awk '{print $5, $9}'
