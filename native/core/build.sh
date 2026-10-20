#!/usr/bin/env bash
# Build the native core for x86_64 Windows with zig (Mac or Linux): native/core/build.sh [outdir]
#   <outdir>/librncore.a      every core module + yyjson, for native/ui to link instead of mock_api.cpp's core
#   <outdir>/rn-core-cli.exe  the command line (--check --scan ...) alone, console subsystem, for tests
#   <outdir>/coretest.exe     with RN_CORETEST=1: the test harness (test/coretest.cpp), never shipped
# The game launch comes from launch.cpp when it exists, otherwise from launch_stub.cpp (its own archive member, so a
# launch.cpp linked before the archive replaces it).
# RN_VERSION=<semver> (just release <version>): the version of this build instead of version.h's default, through
# <outdir>/obj/version.gen.h (version.sh), which native/ui also reads for the window's version resource.
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
OUT="${1:-out}"
OBJ="$OUT/obj"
rm -rf "$OBJ"
mkdir -p "$OBJ"

VERSION_FLAGS=()
if [ -n "${RN_VERSION:-}" ]; then
  ./version.sh "$RN_VERSION" > "$OBJ/version.gen.h"
  VERSION_FLAGS=(-DRN_VERSION_GEN "-I$OBJ")
fi
CXXFLAGS=(-target x86_64-windows-gnu -std=c++20 -Os -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections
  -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Wall -Wno-unused-function -Wno-missing-braces
  ${VERSION_FLAGS[@]+"${VERSION_FLAGS[@]}"} ${RN_DEFINES:-})
LIBS=(-lwinhttp -lbcrypt -lole32 -loleaut32 -luuid -lshell32 -ladvapi32 -luser32)

SRC=()
for f in *.cpp; do
  case "$f" in cli_main.cpp|launch.cpp|launch_stub.cpp) ;; *) SRC+=("${f%.cpp}") ;; esac
done
if [ -f launch.cpp ]; then SRC+=(launch); else SRC+=(launch_stub); fi

pids=()
"$ZIG" cc -target x86_64-windows-gnu -Os -ffunction-sections -fdata-sections -c vendor/yyjson/yyjson.c -o "$OBJ/yyjson.o" & pids+=($!)
for s in "${SRC[@]}" cli_main; do
  "$ZIG" c++ "${CXXFLAGS[@]}" -c "$s.cpp" -o "$OBJ/$s.o" & pids+=($!)
done
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ "$fail" = 0 ] || { echo "compile failed" >&2; exit 1; }

objs=()
for s in "${SRC[@]}"; do objs+=("$OBJ/$s.o"); done
rm -f "$OUT/librncore.a"
"$ZIG" ar rcs "$OUT/librncore.a" "${objs[@]}" "$OBJ/yyjson.o"
"$ZIG" c++ "${CXXFLAGS[@]}" -Wl,--subsystem,console -Wl,--gc-sections -s "$OBJ/cli_main.o" "$OUT/librncore.a" "${LIBS[@]}" -o "$OUT/rn-core-cli.exe"
if [ -n "${RN_CORETEST:-}" ]; then
  "$ZIG" c++ "${CXXFLAGS[@]}" -Wl,--subsystem,console -Wl,--gc-sections -s test/coretest.cpp "$OUT/librncore.a" "${LIBS[@]}" -o "$OUT/coretest.exe"
fi
ls -l "$OUT/librncore.a" "$OUT/rn-core-cli.exe" ${RN_CORETEST:+"$OUT/coretest.exe"} | awk '{print $5, $9}'
