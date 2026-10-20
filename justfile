# Radarnite Launcher 3.0 developer commands, run from the Linux dev box (never build here).
# Builds run on the Mac: `mac sync` mirrors this tree to ~/agent-work/mirror/rl-launcher and `mac sh` runs there (plain
# `ssh mac` always exits 0 and hides failures; `mac sh` keeps the remote exit code). zig c++ -> x86_64-windows-gnu for
# the core and the window, cargo zigbuild for the backend. The PC (ssh `windows`) runs the Windows tests, and builds the
# core and the window only when the Mac does not answer or stays above 70 % CPU.
# No machine starts anything above 70 % CPU: the recipes wait (up to 10 min) and then give up.
# PC jobs share one lock with the other agents (/tmp/rn-launcher-pc.lock): every PC recipe takes it itself and checks the
# CPU inside it, right before the job, so never wrap them in flock. Everything on the PC lives in pc_dir (just clean-remote).
# RN_WIN_LANE: optional queue wrapper for PC jobs (a script taking the cmd.exe line as its argument).

set shell := ["bash", "-euo", "pipefail", "-c"]

# backend-* recipes (backend-rs/): backend-build, backend-test, backend-smoke-linux, ... `just build-backend` runs
# backend-build in this tree's Mac mirror instead of the backend agent's own folder.
import 'backend-rs/justfile'

mac_proj := "rl-launcher"
mac_dir := "agent-work/mirror/" + mac_proj
pc_dir := 'S:\rl-integ-test'
pc_fwd := replace(pc_dir, '\', '/')
pc_lock := "/tmp/rn-launcher-pc.lock"
zig := "/opt/homebrew/bin/zig"
mac_path := 'export PATH="$HOME/.cargo/bin:/opt/homebrew/bin:$PATH"'
quiet := '{ grep -v -e nullability -e _Nullable -e _Nonnull -e "note: insert" -e "In file included" -e "^ *|" -e "^ *[0-9]* |" -e "warnings generated" || true; }'

# the published 2.2.1 installer: the reference of the core parity, the window numbers and the migration test
ref_version := "2.2.1"
ref_sha256 := "54d59b107b80ad36053a7c724c5171fc763d05f0dcc3e5f8b73e87c2e7fd4060"
ref_setup := "dist/ref/RadarniteLauncherSetup-" + ref_version + ".exe"

# Build everything: the core, the window over it (native/ui/out/RadarniteLauncher.exe), then the backend
build:
    #!/usr/bin/env bash
    set -euo pipefail
    if just _cpu-ok mac; then just build-mac; else echo "Mac busy or away: building the core and the window on the PC" >&2; just build-pc; fi
    just build-backend

# Build the core on the Mac: native/core/out/librncore.a and rn-core-cli.exe
build-core: (_wait-cpu "mac") _sync-mac
    mac sh 'set -o pipefail; cd ~/{{mac_dir}}/native/core && ZIG={{zig}} ./build.sh out 2>&1 | {{quiet}}'
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
    tar -c native/core/*.cpp native/core/*.h native/core/build.cmd native/core/vendor native/ui/*.cpp native/ui/*.h native/ui/app.rc \
      native/ui/app.manifest native/ui/build.rsp native/ui/build.cmd launcher/ui/fonts launcher/ui/radarnite.png launcher/radarnite.ico \
      | ssh -o BatchMode=yes windows 'tar -x -C {{pc_dir}}\src'
    pc sh 'set "ZIG=%LOCALAPPDATA%\fp-tools\zig\zig.exe" && {{pc_dir}}\src\native\core\build.cmd out && {{pc_dir}}\src\native\ui\build.cmd {{pc_dir}}\src\native\ui\out\RadarniteLauncher.exe'
    mkdir -p native/core/out native/ui/out
    for f in core/out/librncore.a core/out/rn-core-cli.exe ui/out/RadarniteLauncher.exe; do scp -q "windows:{{pc_fwd}}/src/native/$f" "native/$f"; done
    ls -l native/core/out/librncore.a native/core/out/rn-core-cli.exe native/ui/out/RadarniteLauncher.exe

# Build the backend on the Mac (backend-build: radarnite-backend.exe + the Linux aarch64 binary for `just parity`)
build-backend: _sync-mac
    just backend_mac='{{mac_dir}}' backend_cargo='{{mac_path}}; cd ~/{{mac_dir}}/backend-rs' backend-build

# Backend parity on this box (light): Node vs the Linux build of the Rust backend (tests/parity/run.sh [options] [corpora])
parity *args:
    test -x backend-rs/out/radarnite-backend-linux-aarch64 || just build-backend
    tests/parity/run.sh {{args}}

# Local release candidate on the Mac: the real exes, the PC's payload, privacy gate, .sha256 -> dist/ (never published)
release version="3.0.0-rc.1": build
    #!/usr/bin/env bash
    set -euo pipefail
    just _sync-mac   # the exes in out/ are the ones shipped, wherever they were built
    just _wait-cpu windows
    tools/fetch-payload.sh mac:{{mac_dir}}-payload
    mac sh 'p=~/{{mac_dir}}-payload; for f in redirect/fnredirect.dll gameserver/radarnite-server.dll patches/fnpakpatch.dll tools/sigfit-installed.ps1 tools/cosmetics-gen/fn-cosmetics-gen.exe THIRD-PARTY-NOTICES.txt README-offline.md; do test -s "$p/$f" || { echo "payload: missing $f" >&2; exit 1; }; done'
    just _wait-cpu mac
    mac sh '{{mac_path}}; cd ~/{{mac_dir}} && tools/release.sh build --version {{version}} --launcher native/ui/out/RadarniteLauncher.exe --backend backend-rs/out/radarnite-backend.exe --payload ~/{{mac_dir}}-payload --out dist --makensis /opt/homebrew/bin/makensis'
    mkdir -p dist
    mac get {{mac_dir}}/dist/RadarniteLauncherSetup-{{version}}.exe dist/RadarniteLauncherSetup-{{version}}.exe
    mac get {{mac_dir}}/dist/RadarniteLauncherSetup-{{version}}.exe.sha256 dist/RadarniteLauncherSetup-{{version}}.exe.sha256
    [ "$(cat dist/RadarniteLauncherSetup-{{version}}.exe.sha256)" = "$(tools/release.sh sha256-line dist/RadarniteLauncherSetup-{{version}}.exe)" ]
    ls -l dist/RadarniteLauncherSetup-{{version}}.exe*

# tools/release-selftest.sh on the Mac: placeholder exes, the privacy gate, the 2.x .sha256 format, the real 2.2.1 payload
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

# Every Windows check of the window exe, each its own PC job: core parity, selftest, window smoke, backend numbers
test-pc: core-parity-pc selftest-pc smoke-pc backend-measure-pc

# The full exe's console verbs against 2.2.1 extracted from its installer (native/core/test): stdout + exit, --check timing
core-parity-pc: _ref-setup _exe
    #!/usr/bin/env bash
    set -euo pipefail
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    just _ref-pc
    scp -q native/ui/out/RadarniteLauncher.exe 'windows:{{pc_fwd}}/core/RadarniteLauncher-linktest.exe'
    scp -q native/core/out/rn-core-cli.exe native/core/test/parity1.ps1 native/core/test/oracle.ps1 native/core/test/parity3.ps1 'windows:{{pc_fwd}}/core/'
    mkdir -p dist
    pc ps - <<'EOF' | tee dist/core-parity.txt
    $d = '{{pc_dir}}\core'; Set-Location $d
    $pre = Test-Path 'HKCU:\Software\Radarnite'
    & .\parity1.ps1 -Dir $d -Exe "$d\RadarniteLauncher-linktest.exe"
    & .\oracle.ps1 -Dir $d
    & .\parity3.ps1 -Dir $d -GuiOnly
    if (-not $pre -and (Test-Path 'HKCU:\Software\Radarnite')) { Remove-Item -Recurse -Force 'HKCU:\Software\Radarnite'; 'HKCU\Software\Radarnite was created by the runs: removed' }
    EOF
    ! grep -E ' DIFF|TIMEOUT|config restored: False|untouched: False|unchanged by the runs: False' dist/core-parity.txt

# Copy the window exe to the PC and run --selftest-ui in the signed-in session (cloaked, mock core): native/ui/selftest/
selftest-pc *args: _exe
    #!/usr/bin/env bash
    set -euo pipefail
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    pc sh 'if not exist {{pc_dir}}\ui mkdir {{pc_dir}}\ui'
    scp -q native/ui/out/RadarniteLauncher.exe native/ui/tools/selftest-session.ps1 'windows:{{pc_fwd}}/ui/'
    ${RN_WIN_LANE:-ssh -o BatchMode=yes windows} 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File {{pc_dir}}\ui\selftest-session.ps1 -Dir {{pc_dir}}\ui -UiArgs "{{args}}"' >/dev/null
    mkdir -p native/ui/selftest
    scp -q 'windows:{{pc_fwd}}/ui/out/*' native/ui/selftest/
    jq -c '{firstFrameMs, privateMB, idleCpu, idlePaints, idleTimers, renderer, exeBytes, liveCapture, failed}' native/ui/selftest/selftest.json
    jq -e '.failed == 0' native/ui/selftest/selftest.json >/dev/null
alias run-pc := selftest-pc

# The real core in the window on the PC: hidden desktop, scans the builds folder, PNG, WM_CLOSE; 2.2.1 measured alongside
smoke-pc: _ref-setup _exe
    #!/usr/bin/env bash
    set -euo pipefail
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    just _ref-pc
    pc sh '(if exist {{pc_dir}}\smoke rmdir /s /q {{pc_dir}}\smoke) & mkdir {{pc_dir}}\smoke\app'
    scp -q native/ui/out/RadarniteLauncher.exe 'windows:{{pc_fwd}}/smoke/app/'
    scp -q native/ui/tools/window-smoke.ps1 baseline/session-bench.ps1 'windows:{{pc_fwd}}/smoke/'
    ssh -o BatchMode=yes windows 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File {{pc_dir}}\smoke\window-smoke.ps1 -Dir {{pc_dir}}\smoke -Ref {{pc_dir}}\core\rlcoretest-ref -Bench {{pc_dir}}\smoke\session-bench.ps1' > /dev/null
    mkdir -p dist/smoke
    scp -q 'windows:{{pc_fwd}}/smoke/out/smoke.json' 'windows:{{pc_fwd}}/smoke/out/window-3.0*.png' dist/smoke/
    jq '{error, windowVisibleMs: .session.windowVisibleMs, windowClass: .session.windowClass, capture: .session.capture, captureRetry: .session.captureRetry, closedByWmClose: .session.closedByWmClose, v30: .session.mainWindow, v221: .ref221.mainWindow, v221selftests: [.ref221.selftests[]?.pageReadyMs], postState}' dist/smoke/smoke.json
    jq -e '(.error == null) and (.session.error == null) and .session.closedByWmClose and (.session.windowClass == "RadarniteLauncher3")' dist/smoke/smoke.json >/dev/null

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

# The page scenarios rendered by Chromium on the PC vs the last native selftest (-Fresh renders the page again)
compare-pc *args:
    #!/usr/bin/env bash
    set -euo pipefail
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    pc sh 'if not exist {{pc_dir}}\ui\page mkdir {{pc_dir}}\ui\page'
    tar -c launcher/ui tests/ui-scenarios.mjs native/ui/tools/page-extra.mjs native/ui/tools/compare.mjs native/ui/tools/pw-scrollbars.cjs | ssh -o BatchMode=yes windows 'tar -x -C {{pc_dir}}\ui\page'
    scp -q native/ui/tools/compare-pc.ps1 'windows:{{pc_fwd}}/ui/compare-pc.ps1'
    ${RN_WIN_LANE:-ssh -o BatchMode=yes windows} 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File {{pc_dir}}\ui\compare-pc.ps1 -Dir {{pc_dir}}\ui {{args}}'
    rm -rf native/ui/out/compare
    mkdir -p native/ui/out/compare
    scp -q 'windows:{{pc_fwd}}/ui/cmp/*' native/ui/out/compare/

# Native selftest, then the comparison with the page
check-pc:
    just selftest-pc
    just compare-pc

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
