# Radarnite Launcher 3.0 developer commands, run from the Linux dev box (never build here).
# Builds run on the Mac: `mac sync` mirrors this tree to ~/agent-work/mirror/rl-launcher and `mac sh` runs there (plain
# `ssh mac` always exits 0 and hides failures; `mac sh` keeps the remote exit code). zig c++ -> x86_64-windows-gnu for
# the core and the window, cargo zigbuild for the backend. The PC (ssh `windows`) runs the Windows tests, and builds the
# core and the window only when the Mac does not answer or stays above 70 % CPU.
# No machine starts anything above 70 % CPU: the recipes wait (up to 10 min) and then give up.
# PC jobs share one lock with the other agents (/tmp/rn-launcher-pc.lock): every PC recipe takes it itself and checks the
# CPU inside it, right before the job, so never wrap them in flock. Everything on the PC lives in pc_dir (just clean-remote).
# RN_WIN_LANE: optional queue wrapper for PC jobs (a script taking the cmd.exe line as its argument).
# RN_PC_DIR: another PC work folder than pc_dir's default (an environment variable, so the nested `just` calls get it).
# RN_VERSION: the version the core and the window are built as (native/core/version.sh), default version.h's;
# `just release <version>` sets it.

set shell := ["bash", "-euo", "pipefail", "-c"]

# backend-* recipes (backend-rs/): backend-build, backend-test, ... `just build-backend` runs
# backend-build in this tree's Mac mirror instead of the backend agent's own folder.
import 'backend-rs/justfile'

mac_proj := "rl-launcher"
mac_dir := "agent-work/mirror/" + mac_proj
pc_dir := env_var_or_default("RN_PC_DIR", 'S:\rl-integ-test')
pc_fwd := replace(pc_dir, '\', '/')
pc_lock := "/tmp/rn-launcher-pc.lock"
zig := "/opt/homebrew/bin/zig"
dotnet := "~/agent-work/rn-tools/dotnet/dotnet"
mac_path := 'export PATH="$HOME/.cargo/bin:/opt/homebrew/bin:$PATH"'
rn_version := env_var_or_default("RN_VERSION", "")
quiet := '{ grep -v -e nullability -e _Nullable -e _Nonnull -e "note: insert" -e "In file included" -e "^ *|" -e "^ *[0-9]* |" -e "warnings generated" || true; }'

# the published 2.2.1 installer: the reference of the backend numbers and the migration test
ref_version := "2.2.1"
ref_sha256 := "54d59b107b80ad36053a7c724c5171fc763d05f0dcc3e5f8b73e87c2e7fd4060"
ref_setup := "dist/ref/RadarniteLauncherSetup-" + ref_version + ".exe"

# Build everything: the core, the window over it (native/ui/out/RadarniteLauncher.exe), then the backend
build:
    #!/usr/bin/env bash
    set -euo pipefail
    if just _cpu-ok mac; then just build-mac; else echo "Mac busy or away: building the core and the window on the PC" >&2; just build-pc; fi
    just build-backend
    just build-play

# Build the core on the Mac: native/core/out/librncore.a and rn-core-cli.exe (as RN_VERSION when set)
build-core: (_wait-cpu "mac") _sync-mac
    mac sh 'set -o pipefail; cd ~/{{mac_dir}}/native/core && RN_VERSION={{rn_version}} ZIG={{zig}} ./build.sh out 2>&1 | {{quiet}}'
    mkdir -p native/core/out
    mac get {{mac_dir}}/native/core/out/librncore.a native/core/out/librncore.a
    mac get {{mac_dir}}/native/core/out/rn-core-cli.exe native/core/out/rn-core-cli.exe

# Build the core, then the window linked with it, on the Mac
build-mac: build-core
    mac sh 'set -o pipefail; cd ~/{{mac_dir}}/native/ui && mkdir -p out && rm -f out/RadarniteLauncher.exe && ZIG={{zig}} ./build.sh out/RadarniteLauncher.exe 2>&1 | {{quiet}}'
    mkdir -p native/ui/out
    mac get {{mac_dir}}/native/ui/out/RadarniteLauncher.exe native/ui/out/RadarniteLauncher.exe
    ls -l native/core/out/librncore.a native/core/out/rn-core-cli.exe native/ui/out/RadarniteLauncher.exe

# Build the core and the window on the PC (zig in %LOCALAPPDATA%\fp-tools\zig, native/core/build.cmd + native/ui/build.cmd)
build-pc:
    #!/usr/bin/env bash
    set -euo pipefail
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    pc sh '(if exist {{pc_dir}}\src rmdir /s /q {{pc_dir}}\src) & mkdir {{pc_dir}}\src'
    # RN_VERSION: its version.gen.h made here (build.cmd has no semver parser) and shipped as src\version.gen.h
    gen=$(mktemp -d); trap 'rm -rf "$gen"' EXIT
    vset=""; vtar=()
    if [ -n "{{rn_version}}" ]; then
      native/core/version.sh "{{rn_version}}" > "$gen/version.gen.h"
      vset='set "RN_VERSION_H={{pc_dir}}\src\version.gen.h" && '; vtar=(-C "$gen" version.gen.h)
    fi
    tar -c native/core/*.cpp native/core/*.h native/core/build.cmd native/core/vendor native/ui/*.cpp native/ui/*.h native/ui/app.rc \
      native/ui/app.manifest native/ui/build.rsp native/ui/build.cmd launcher/ui/fonts launcher/ui/radarnite.png launcher/radarnite.ico \
      "${vtar[@]}" | ssh -o BatchMode=yes windows 'tar -x -C {{pc_dir}}\src'
    pc sh "$vset"'set "ZIG=%LOCALAPPDATA%\fp-tools\zig\zig.exe" && {{pc_dir}}\src\native\core\build.cmd out && {{pc_dir}}\src\native\ui\build.cmd {{pc_dir}}\src\native\ui\out\RadarniteLauncher.exe'
    mkdir -p native/core/out native/ui/out
    for f in core/out/librncore.a core/out/rn-core-cli.exe ui/out/RadarniteLauncher.exe; do scp -q "windows:{{pc_fwd}}/src/native/$f" "native/$f"; done
    ls -l native/core/out/librncore.a native/core/out/rn-core-cli.exe native/ui/out/RadarniteLauncher.exe

# Build radarnite-play.exe on the Mac (play/: the 2.x game start; dotnet publish, self-contained single file) -> play/out
build-play: (_wait-cpu "mac") _sync-mac
    mac sh 'set -o pipefail; cd ~/{{mac_dir}} && rm -rf play/out && export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 && {{dotnet}} publish play/RadarnitePlay.csproj -c Release -o play/out 2>&1 | { grep -E " error |warning " || true; }; test -f play/out/radarnite-play.exe'
    mkdir -p play/out
    mac get {{mac_dir}}/play/out/radarnite-play.exe play/out/radarnite-play.exe
    ls -l play/out/radarnite-play.exe

# Build the backend on the Mac (backend-build: radarnite-backend.exe + the Linux aarch64 binary for `just parity`)
build-backend: _sync-mac
    just backend_mac='{{mac_dir}}' backend_cargo='{{mac_path}}; cd ~/{{mac_dir}}/backend-rs' backend-build

# Backend parity on this box (light): Node vs the Linux build of the Rust backend (tests/parity/run.sh [options] [corpora])
parity *args:
    test -x backend-rs/out/radarnite-backend-linux-aarch64 || just build-backend
    tests/parity/run.sh {{args}}

# Local release candidate on the Mac: the exes built as <version>, the PC's payload, privacy gate, .sha256 -> dist/
# (never published). release.sh refuses a launcher whose ProductVersion is not <version>.
release version="3.0.0-rc.1":
    #!/usr/bin/env bash
    set -euo pipefail
    RN_VERSION='{{version}}' just build
    just _sync-mac   # the exes in out/ are the ones shipped, wherever they were built
    just _wait-cpu windows
    tools/fetch-payload.sh mac:{{mac_dir}}-payload
    mac sh 'p=~/{{mac_dir}}-payload; for f in redirect/fnredirect.dll gameserver/radarnite-server.dll tools/sigfit-installed.ps1 tools/cosmetics-gen/fn-cosmetics-gen.exe THIRD-PARTY-NOTICES.txt README-offline.md; do test -s "$p/$f" || { echo "payload: missing $f" >&2; exit 1; }; done; ls "$p"/patches/fnpakpatch.dll* >/dev/null || { echo "payload: no pak patch in patches/" >&2; exit 1; }'
    just _wait-cpu mac
    mac sh '{{mac_path}}; cd ~/{{mac_dir}} && tools/release.sh build --version {{version}} --launcher native/ui/out/RadarniteLauncher.exe --backend backend-rs/out/radarnite-backend.exe --play play/out/radarnite-play.exe --payload ~/{{mac_dir}}-payload --out dist --makensis /opt/homebrew/bin/makensis'
    mkdir -p dist
    mac get {{mac_dir}}/dist/RadarniteLauncherSetup-{{version}}.exe dist/RadarniteLauncherSetup-{{version}}.exe
    mac get {{mac_dir}}/dist/RadarniteLauncherSetup-{{version}}.exe.sha256 dist/RadarniteLauncherSetup-{{version}}.exe.sha256
    [ "$(cat dist/RadarniteLauncherSetup-{{version}}.exe.sha256)" = "$(tools/release.sh sha256-line dist/RadarniteLauncherSetup-{{version}}.exe)" ]
    ls -l dist/RadarniteLauncherSetup-{{version}}.exe*

# tools/release-selftest.sh on the Mac: placeholder exes, the privacy gate, the 2.x .sha256 format, the version check
release-selftest: (_wait-cpu "mac") _sync-mac
    mac sh '{{mac_path}}; cd ~/{{mac_dir}} && rm -rf ~/{{mac_dir}}-relselftest && ZIG={{zig}} MAKENSIS=/opt/homebrew/bin/makensis REF_VERSION={{ref_version}} tools/release-selftest.sh ~/{{mac_dir}}-relselftest'

# 2.2.1 -> <version> on the PC (tools/installer-migration-test.ps1): silent install in a scratch folder, then the upgrade
release-migration-test version="3.0.0-rc.1": _ref-setup
    #!/usr/bin/env bash
    set -euo pipefail
    new=dist/RadarniteLauncherSetup-{{version}}.exe
    [ -f "$new" ] || { echo "no $new: run just release {{version}} first" >&2; exit 1; }
    # the 3.x program folder = the installer's content, relative paths with backslashes
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    7z x -y -bso0 -bsp0 -o"$tmp/x" "$new" >/dev/null
    (cd "$tmp/x" && find . -type f ! -path './$PLUGINSDIR/*' | sed 's|^\./||; s|/|\\|g' | LC_ALL=C sort) > "$tmp/expected-3x.txt"
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    pc sh '(if exist {{pc_dir}}\migration rmdir /s /q {{pc_dir}}\migration) & mkdir {{pc_dir}}\migration'
    scp -q "$new" {{ref_setup}} tools/installer-migration-test.ps1 "$tmp/expected-3x.txt" 'windows:{{pc_fwd}}/migration/'
    mkdir -p dist/migration
    ssh -o BatchMode=yes windows 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File {{pc_dir}}\migration\installer-migration-test.ps1 -Root {{pc_dir}}\migration -Old {{pc_dir}}\migration\RadarniteLauncherSetup-{{ref_version}}.exe -New {{pc_dir}}\migration\RadarniteLauncherSetup-{{version}}.exe -Expected {{pc_dir}}\migration\expected-3x.txt' | tee dist/migration/migration.txt
    scp -q 'windows:{{pc_fwd}}/migration/out/migration.json' dist/migration/
    jq -e '.allPassed' dist/migration/migration.json >/dev/null

# The native tests, each its own PC job: the updater, then the window selftest
test-pc: core-update-test-pc selftest-pc

# The updater on the PC (native/core/test/update-check.ps1): the version table (coretest semver), then --update-check
# of rn-core-cli built on the Mac as 3.0.0, 3.0.0-rc.1 and 3.0.0-rc.2 (native/core/out/test) and of the window exe
# against a fake GitHub API on the PC's loopback (test/fake-github.mjs), and once against the real API
core-update-test-pc: (_wait-cpu "mac") _sync-mac
    #!/usr/bin/env bash
    set -euo pipefail
    vs="3.0.0 3.0.0-rc.1 3.0.0-rc.2"
    mac sh 'set -o pipefail; cd ~/{{mac_dir}}/native/core && for v in '"$vs"'; do RN_VERSION=$v RN_CORETEST=1 ZIG={{zig}} ./build.sh out/test/$v 2>&1 | {{quiet}}; done'
    mkdir -p native/core/out/test
    for v in $vs; do mac get {{mac_dir}}/native/core/out/test/$v/rn-core-cli.exe native/core/out/test/rn-core-cli-$v.exe; done
    mac get {{mac_dir}}/native/core/out/test/3.0.0/coretest.exe native/core/out/test/coretest.exe
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    pc sh '(if exist {{pc_dir}}\update rmdir /s /q {{pc_dir}}\update) & mkdir {{pc_dir}}\update'
    scp -q native/core/out/test/*.exe native/core/test/update-check.ps1 native/core/test/fake-github.mjs 'windows:{{pc_fwd}}/update/'
    win=""
    if [ -f native/ui/out/RadarniteLauncher.exe ]; then scp -q native/ui/out/RadarniteLauncher.exe 'windows:{{pc_fwd}}/update/'; win=' -Window {{pc_dir}}\update\RadarniteLauncher.exe'; fi
    mkdir -p dist
    ssh -o BatchMode=yes windows 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File {{pc_dir}}\update\update-check.ps1 -Dir {{pc_dir}}\update'"$win" | tee dist/update-check.txt
    grep -q '^update-check: [0-9]* passed, 0 failed' dist/update-check.txt

# Copy the window exe to the PC and run --selftest-ui in the signed-in session (cloaked, mock core): native/ui/selftest/
# keeps selftest.json and two of its PNGs (the live window and the busiest scenario)
selftest-pc *args: _exe
    #!/usr/bin/env bash
    set -euo pipefail
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    pc sh 'if not exist {{pc_dir}}\ui mkdir {{pc_dir}}\ui'
    scp -q native/ui/out/RadarniteLauncher.exe native/ui/tools/selftest-session.ps1 'windows:{{pc_fwd}}/ui/'
    ${RN_WIN_LANE:-ssh -o BatchMode=yes windows} 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File {{pc_dir}}\ui\selftest-session.ps1 -Dir {{pc_dir}}\ui -UiArgs "{{args}}"' >/dev/null
    mkdir -p native/ui/selftest
    scp -q 'windows:{{pc_fwd}}/ui/out/selftest.json' 'windows:{{pc_fwd}}/ui/out/00-live-window-01.png' \
      'windows:{{pc_fwd}}/ui/out/02-many-builds-bots-activity-125.png' native/ui/selftest/
    jq -c '{firstFrameMs, privateMB, idleCpu, idlePaints, idleTimers, renderer, exeBytes, liveCapture, failed}' native/ui/selftest/selftest.json
    jq -e '.failed == 0' native/ui/selftest/selftest.json >/dev/null
alias run-pc := selftest-pc

# Backend ready / RSS on a scratch copy of 14.30's offline data (baseline/measure-backend.ps1): Rust, then 2.2.1's Node
backend-measure-pc: _ref-setup
    #!/usr/bin/env bash
    set -euo pipefail
    test -f backend-rs/out/radarnite-backend.exe || just build-backend
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    just _ref-pc
    pc sh '(if exist {{pc_dir}}\backend rmdir /s /q {{pc_dir}}\backend) & mkdir {{pc_dir}}\backend'
    scp -q backend-rs/out/radarnite-backend.exe baseline/measure-backend.ps1 'windows:{{pc_fwd}}/backend/'
    mkdir -p dist
    ssh -o BatchMode=yes windows 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File {{pc_dir}}\backend\measure-backend.ps1 -Node {{pc_dir}}\backend\radarnite-backend.exe -Root {{pc_dir}}\backend' | tee dist/backend-rust.json
    ssh -o BatchMode=yes windows 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File {{pc_dir}}\backend\measure-backend.ps1 -Node {{pc_dir}}\core\rlcoretest-ref\node\node.exe -Server {{pc_dir}}\core\rlcoretest-ref\backend\server.mjs -Root {{pc_dir}}\backend' | tee dist/backend-node.json

# Remove this tree's work folders on the Mac and the PC
clean-remote:
    mac sh 'rm -rf ~/{{mac_dir}} ~/{{mac_dir}}-payload ~/{{mac_dir}}-relselftest'
    flock {{pc_lock}} pc sh 'if exist {{pc_dir}} rmdir /s /q {{pc_dir}}'

# The 2.2.1 installer in dist/ref, checked against the published sha256
_ref-setup:
    #!/usr/bin/env bash
    set -euo pipefail
    f={{ref_setup}}
    if [ ! -f "$f" ]; then
      mkdir -p "$(dirname "$f")"
      curl -sSfL -o "$f.part" "https://github.com/Radarnite/radarnite-launcher/releases/download/v{{ref_version}}/RadarniteLauncherSetup-{{ref_version}}.exe"
      mv "$f.part" "$f"
    fi
    echo "{{ref_sha256}}  $f" | sha256sum -c --quiet

# (inside the PC lock) 2.2.1 extracted, never installed, in pc_dir\core\rlcoretest-ref: the layout of native/core/test.
# The PC's 7z extracts it; without one, this box extracts it and streams the folder (about 370 MB).
_ref-pc:
    #!/usr/bin/env bash
    set -euo pipefail
    ref='{{pc_dir}}\core\rlcoretest-ref'
    if pc sh "if exist $ref\RadarniteLauncher.exe (exit 0) else (exit 1)" >/dev/null 2>&1; then exit 0; fi
    pc sh 'if not exist {{pc_dir}}\core mkdir {{pc_dir}}\core'
    scp -q {{ref_setup}} 'windows:{{pc_fwd}}/core/'
    if ! pc sh "7z x -y -bso0 -bsp0 -o$ref {{pc_dir}}\core\RadarniteLauncherSetup-{{ref_version}}.exe" >/dev/null; then
      echo "no 7z on the PC: extracting here" >&2
      tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
      7z x -y -bso0 -bsp0 -o"$tmp/x" {{ref_setup}} >/dev/null
      tar -c -C "$tmp/x" . | ssh -o BatchMode=yes windows "mkdir $ref & tar -x -C $ref"
    fi
    pc sh "if exist $ref\$PLUGINSDIR rmdir /s /q $ref\$PLUGINSDIR"
    pc sh "if exist $ref\RadarniteLauncher.exe (exit 0) else (exit 1)"

_exe:
    test -f native/ui/out/RadarniteLauncher.exe || just build

_sync-mac:
    mac sync . {{mac_proj}} >/dev/null

# exit 0 when the host answers and its CPU is at or below 70 % (1 = busy, 75 = no answer)
_cpu-ok host:
    #!/usr/bin/env bash
    set -uo pipefail
    m=$([ "{{host}}" = mac ] && echo mac || echo windows)
    cpu=$(machine-load get "$m" 2>/dev/null | jq -r 'if .up then (.cpu | floor) else empty end' 2>/dev/null) || cpu=""
    if [ -n "$cpu" ]; then :
    elif [ "{{host}}" = mac ]; then
      cpu=$(mac sh "top -l 2 -n 0 -s 1 | grep 'CPU usage' | tail -1" 2>/dev/null | sed -E 's/.* ([0-9.]+)% idle.*/\1/' | awk '{printf "%d", 100-$1}')
    else
      cpu=$(pc ps - 2>/dev/null <<< '(Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average' | tr -dc '0-9')
    fi
    [ -n "$cpu" ] || { echo "{{host}} does not answer" >&2; exit 75; }
    [ "$cpu" -le 70 ] || { echo "{{host}} CPU at ${cpu}%" >&2; exit 1; }

_wait-cpu host:
    #!/usr/bin/env bash
    for i in $(seq 40); do
      just _cpu-ok {{host}}; rc=$?
      [ $rc -eq 0 ] && exit 0
      [ $rc -eq 75 ] && exit 75
      echo "{{host}}: waiting for the CPU to drop to 70 %" >&2; sleep 15
    done
    echo "{{host}} stayed above 70 % CPU" >&2; exit 75
