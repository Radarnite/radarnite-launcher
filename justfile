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
# RN_MAC_PROJ: another Mac mirror than rl-launcher (two trees building at once must not share one).
# RN_VERSION: the version the core and the window are built as (native/core/version.sh), default version.h's;
# `just release <version>` sets it.

set shell := ["bash", "-euo", "pipefail", "-c"]

# backend-* recipes (backend-rs/): backend-build, backend-test, ... `just build-backend` runs
# backend-build in this tree's Mac mirror instead of the backend agent's own folder.
import 'backend-rs/justfile'

mac_proj := env_var_or_default("RN_MAC_PROJ", "rl-launcher")
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
    just build-fnbody

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
    tar -c native/core/*.cpp native/core/*.h native/core/build.cmd native/core/vendor native/ui/*.cpp native/ui/*.h native/ui/*.inc native/ui/app.rc \
      native/ui/app.manifest native/ui/build.rsp native/ui/build.cmd launcher/ui/fonts launcher/ui/radarnite.png launcher/radarnite.ico \
      "${vtar[@]}" | ssh -o BatchMode=yes windows 'tar -x -C {{pc_dir}}\src'
    pc sh "$vset"'set "ZIG=%LOCALAPPDATA%\fp-tools\zig\zig.exe" && {{pc_dir}}\src\native\core\build.cmd out && mkdir {{pc_dir}}\src\native\ui\out && {{pc_dir}}\src\native\ui\build.cmd {{pc_dir}}\src\native\ui\out\RadarniteLauncher.exe'
    mkdir -p native/core/out native/ui/out
    for f in core/out/librncore.a core/out/rn-core-cli.exe ui/out/RadarniteLauncher.exe; do scp -q "windows:{{pc_fwd}}/src/native/$f" "native/$f"; done
    ls -l native/core/out/librncore.a native/core/out/rn-core-cli.exe native/ui/out/RadarniteLauncher.exe

# Build radarnite-play.exe on the Mac (play/: the 2.x game start; dotnet publish, self-contained single file) -> play/out
build-play: (_wait-cpu "mac") _sync-mac
    mac sh 'set -o pipefail; cd ~/{{mac_dir}} && rm -rf play/out && export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 && {{dotnet}} publish play/RadarnitePlay.csproj -c Release -o play/out 2>&1 | { grep -E " error |warning " || true; }; test -f play/out/radarnite-play.exe'
    mkdir -p play/out
    mac get {{mac_dir}}/play/out/radarnite-play.exe play/out/radarnite-play.exe
    ls -l play/out/radarnite-play.exe

# Build patches/fnbody.dll (pakpatch/fnbody.c, the body sliders of fn-port outfits in 14.30) and its mock test on the
# Mac with zig cc, as release.yml does -> pakpatch/out
build-fnbody: (_wait-cpu "mac") _sync-mac
    mac sh 'set -o pipefail; cd ~/{{mac_dir}}/pakpatch && rm -rf out && mkdir out && {{zig}} cc -target x86_64-windows-gnu -O2 -shared -s -o out/fnbody.dll fnbody.c -luser32 && {{zig}} cc -target x86_64-windows-gnu -O1 -o out/fnbody-mock.exe test/fnbody-mock.c -luser32'
    mkdir -p pakpatch/out
    mac get {{mac_dir}}/pakpatch/out/fnbody.dll pakpatch/out/fnbody.dll
    mac get {{mac_dir}}/pakpatch/out/fnbody-mock.exe pakpatch/out/fnbody-mock.exe
    ls -l pakpatch/out/fnbody.dll pakpatch/out/fnbody-mock.exe

# The same build of fnbody.dll and its mock test on the PC (zig in %LOCALAPPDATA%\fp-tools\zig), when the Mac is busy
build-fnbody-pc:
    #!/usr/bin/env bash
    set -euo pipefail
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    pc sh '(if exist {{pc_dir}}\fnbody-src rmdir /s /q {{pc_dir}}\fnbody-src) & mkdir {{pc_dir}}\fnbody-src\test'
    scp -q pakpatch/*.c 'windows:{{pc_fwd}}/fnbody-src/'
    scp -q pakpatch/test/*.c 'windows:{{pc_fwd}}/fnbody-src/test/'
    pc sh 'cd /d {{pc_dir}}\fnbody-src && "%LOCALAPPDATA%\fp-tools\zig\zig.exe" cc -target x86_64-windows-gnu -O2 -shared -s -o fnbody.dll fnbody.c -luser32 && "%LOCALAPPDATA%\fp-tools\zig\zig.exe" cc -target x86_64-windows-gnu -O1 -o fnbody-mock.exe test\fnbody-mock.c -luser32'
    mkdir -p pakpatch/out
    scp -q 'windows:{{pc_fwd}}/fnbody-src/fnbody.dll' 'windows:{{pc_fwd}}/fnbody-src/fnbody-mock.exe' pakpatch/out/
    ls -l pakpatch/out/fnbody.dll pakpatch/out/fnbody-mock.exe

# fnbody.dll against the fake engine of pakpatch/test/fnbody-mock.c on the PC, names encoded as in 14.30 and plain
# (exit 0 = pass; never in CI)
test-fnbody:
    #!/usr/bin/env bash
    set -euo pipefail
    test -f pakpatch/out/fnbody-mock.exe || just build-fnbody
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    pc sh '(if exist {{pc_dir}}\fnbody rmdir /s /q {{pc_dir}}\fnbody) & mkdir {{pc_dir}}\fnbody'
    scp -q pakpatch/out/fnbody.dll pakpatch/out/fnbody-mock.exe 'windows:{{pc_fwd}}/fnbody/'
    pc sh '{{pc_dir}}\fnbody\fnbody-mock.exe {{pc_dir}}\fnbody'
    pc sh '{{pc_dir}}\fnbody\fnbody-mock.exe {{pc_dir}}\fnbody --plain'

# Build gameserver\radarnite-server.dll (gameserver-reboot/, the local match server) on the PC with its build.cmd and the
# portable MSVC -> gameserver-reboot/out. MinHook's sources and headers are not in the repo: they are copied from
# RN_MINHOOK_DIR (a folder with minhook\ and mhinc\, default the PC's Reboot build folder). Low priority, RN_JOBS
# compiler processes (default 4).
build-server-pc:
    #!/usr/bin/env bash
    set -euo pipefail
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    mh='{{env_var_or_default("RN_MINHOOK_DIR", '%LOCALAPPDATA%\fp-build\rb-pc')}}'
    pc sh 'if not exist {{pc_dir}}\rb mkdir {{pc_dir}}\rb'
    pc sh "if not exist {{pc_dir}}\rb\minhook (xcopy /e /i /q /y \"$mh\minhook\" {{pc_dir}}\rb\minhook && xcopy /e /i /q /y \"$mh\mhinc\" {{pc_dir}}\rb\mhinc) >nul"
    pc sh '(if exist "{{pc_dir}}\rb\Project Reboot 3.0" rmdir /s /q "{{pc_dir}}\rb\Project Reboot 3.0") & (if exist {{pc_dir}}\rb\vendor rmdir /s /q {{pc_dir}}\rb\vendor) & (if exist {{pc_dir}}\rb\obj rmdir /s /q {{pc_dir}}\rb\obj) & (if exist {{pc_dir}}\rb\out rmdir /s /q {{pc_dir}}\rb\out)'
    tar -c -C gameserver-reboot "Project Reboot 3.0" vendor build.cmd | ssh -o BatchMode=yes windows 'tar -x -C {{pc_dir}}\rb'
    pc sh 'cd /d {{pc_dir}}\rb && set JOBS={{env_var_or_default("RN_JOBS", "4")}} && start "" /low /wait /b cmd /c build.cmd > build.txt 2>&1 & type build.txt | findstr /v /c:"Note: including" & findstr /c:"BUILD OK" build.txt >nul'
    mkdir -p gameserver-reboot/out
    scp -q 'windows:{{pc_fwd}}/rb/out/radarnite-server.dll' gameserver-reboot/out/
    ls -l gameserver-reboot/out/radarnite-server.dll

# Battle pass and XP data of a season from a build's own Paks (cosmetics-gen --season, built on the PC with the .NET 10
# SDK in %LOCALAPPDATA%\fp-dotnet against the CUE4Parse checkout in RN_CUE4PARSE_DIR) -> backend/seasons/season<N>.json
# (tools/season-data.py). Developer step: the result is committed; nothing of this runs in CI or on players' machines.
season-data-pc season="14" build='S:\FortniteBuilds\14.30':
    #!/usr/bin/env bash
    set -euo pipefail
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    cue='{{env_var_or_default("RN_CUE4PARSE_DIR", '%LOCALAPPDATA%\fp-build\rl-cos\CUE4Parse')}}'
    pc sh '(if exist {{pc_dir}}\cosgen rmdir /s /q {{pc_dir}}\cosgen) & mkdir {{pc_dir}}\cosgen'
    scp -q cosmetics-gen/Program.cs cosmetics-gen/cosmetics-gen.csproj 'windows:{{pc_fwd}}/cosgen/'
    pc sh "cd /d {{pc_dir}}\cosgen && start \"\" /low /wait /b \"%LOCALAPPDATA%\\fp-dotnet\\dotnet.exe\" build -c Release -nologo -v q -p:CUE4ParseDir=\"$cue\" -o out > build.txt 2>&1 & findstr /c:\"error\" build.txt & if exist out\fn-cosmetics-gen.exe (exit 0) else (exit 1)"
    pc sh 'cd /d {{pc_dir}}\cosgen && set "DOTNET_ROOT=%LOCALAPPDATA%\fp-dotnet" && start "" /low /wait /b out\fn-cosmetics-gen.exe --build "{{build}}" --out x.json --engine 4.26 --aes "{{build}}\offline\aes.json" --season {{season}} season.json'
    mkdir -p dist
    scp -q 'windows:{{pc_fwd}}/cosgen/season.json' dist/season{{season}}-dump.json
    scp -q "windows:$(echo '{{build}}' | tr '\\' /)/offline/cosmetics.json" dist/season-cosmetics.json
    python3 tools/season-data.py dist/season{{season}}-dump.json dist/season-cosmetics.json backend/seasons/season{{season}}.json

# One local match of 14.30 in the background on the PC with this checkout's fnbody.dll, match server and backend
# (tools/test-match-pc.sh, tools/test-match-bg.ps1): off screen, muted, idle priority, no input; logs, screenshots and
# clip.mp4 in dist/test-match/. env: "NAME=value;..." for the game processes (FNRAGDOLL, FNOFFLINE_TEST_DEATHS, ...).
# Refuses while a game runs on the PC. Never in CI.
test-match-pc minutes="4" env="":
    #!/usr/bin/env bash
    set -euo pipefail
    exec 9>{{pc_lock}}; flock 9
    # a copy runs: bash reads a script while it runs, an edit to the checkout's copy during a run would break it
    t=$(mktemp); trap 'rm -f "$t"' EXIT; cp tools/test-match-pc.sh "$t"
    RN_ROOT="$PWD" bash "$t" '{{pc_dir}}' {{minutes}} '{{env}}'

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
    just build-play
    just _sync-mac   # the exes in out/ are the ones shipped, wherever they were built
    just _wait-cpu windows
    tools/fetch-payload.sh mac:{{mac_dir}}-payload
    # RN_GAMESERVER=<radarnite-server.dll>: that match server DLL instead of the payload's (test builds of server changes)
    if [ -n "${RN_GAMESERVER:-}" ]; then
      [ "$(head -c 2 "$RN_GAMESERVER")" = MZ ] || { echo "RN_GAMESERVER: not a DLL: $RN_GAMESERVER" >&2; exit 1; }
      scp -q "$RN_GAMESERVER" "mac:{{mac_dir}}-payload/gameserver/radarnite-server.dll"; echo "match server DLL: $RN_GAMESERVER"
    fi
    mac sh 'p=~/{{mac_dir}}-payload; for f in node/node.exe redirect/fnredirect.dll gameserver/radarnite-server.dll tools/cosmetics-gen/fn-cosmetics-gen.exe THIRD-PARTY-NOTICES.txt README-offline.md; do test -s "$p/$f" || { echo "payload: missing $f" >&2; exit 1; }; done; ls "$p"/patches/fnpakpatch.dll* >/dev/null || { echo "payload: no pak patch in patches/" >&2; exit 1; }'
    just _wait-cpu mac
    mac sh '{{mac_path}}; cd ~/{{mac_dir}} && tools/release.sh build --version {{version}} --launcher native/ui/out/RadarniteLauncher.exe --play play/out/radarnite-play.exe --fnbody pakpatch/out/fnbody.dll --payload ~/{{mac_dir}}-payload --out dist --makensis /opt/homebrew/bin/makensis'
    mkdir -p dist
    mac get {{mac_dir}}/dist/RadarniteLauncherSetup-{{version}}.exe dist/RadarniteLauncherSetup-{{version}}.exe
    mac get {{mac_dir}}/dist/RadarniteLauncherSetup-{{version}}.exe.sha256 dist/RadarniteLauncherSetup-{{version}}.exe.sha256
    [ "$(cat dist/RadarniteLauncherSetup-{{version}}.exe.sha256)" = "$(tools/release.sh sha256-line dist/RadarniteLauncherSetup-{{version}}.exe)" ]
    ls -l dist/RadarniteLauncherSetup-{{version}}.exe*

# Test build for the owner's installed launcher (test channel, native/core/update.h): `just publish-test t106` builds
# the installer as <version.h's version>-test.<N>.<tag> with `release` and publishes it to the feed (tools/test-feed.py;
# folder, address and port in ~/.config/rn-test-feed.env, served by the rn-test-feed user service). N counts the
# publishes, so the last one published is the newest whatever its task. Launchers with the channel on poll the feed
# every 2 minutes and install a newer build by themselves within a few minutes, never during a game.
publish-test tag notes="":
    #!/usr/bin/env bash
    set -euo pipefail
    env=~/.config/rn-test-feed.env
    [ -f "$env" ] || { echo "no $env (RN_FEED_DIR, RN_FEED_BIND, RN_FEED_PORT)" >&2; exit 1; }
    . "$env"
    base=$(sed -n 's/^#define RN_VERSION_STR "\([^"]*\)".*/\1/p' native/core/version.h)
    # above every release already out (an installed 3.0.4 never takes a 3.0.0-test.N): the highest stable tag plus one patch
    git fetch -q --tags origin 2>/dev/null || true
    last=$(git tag -l 'v*' | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sed 's/^v//' | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
    if [ -n "$last" ]; then
      next=$(awk -F. '{ print $1 "." $2 "." $3 + 1 }' <<< "$last")
      [ "$(printf '%s\n%s\n' "$base" "$next" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)" = "$next" ] && base=$next
    fi
    version=$(tools/test-feed.py next "$RN_FEED_DIR" "$base" {{quote(tag)}})
    cur=$(tools/test-feed.py current "$RN_FEED_DIR")
    if [ -n "$cur" ] && ! tools/test-feed.py newer "$version" "$cur"; then
      echo "$version is not newer than the feed's $cur: the launcher would never install it" >&2; exit 1
    fi
    echo "publishing $version"
    setup=dist/RadarniteLauncherSetup-$version.exe
    [ -f "$setup" ] || just release "$version"
    notes={{quote(notes)}}
    tools/test-feed.py publish "$RN_FEED_DIR" "$setup" "$version" "${notes:-$(git log -1 --format=%s)}"
    url="http://$RN_FEED_BIND:$RN_FEED_PORT"
    served=$(curl -sSf "$url/feed.json")
    [ "$(jq -r .version <<< "$served")" = "$version" ]
    [ "$(curl -sSf "$url/$(jq -r .installer <<< "$served")" | sha256sum | cut -d' ' -f1)" = "$(jq -r .sha256 <<< "$served")" ]
    echo "published $version: $url/feed.json"

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

# radarnite-play.exe's game-sound check (launcher/Audio.cs) in the signed-in user's session on the PC: a silent player
# muted at volume 0 like the test tooling does, run again, then Audio.Restore must unmute it (play/tools/selftest-audio.ps1)
selftest-audio-pc:
    #!/usr/bin/env bash
    set -euo pipefail
    test -f play/out/radarnite-play.exe || just build-play
    exec 9>{{pc_lock}}; flock 9
    just _wait-cpu windows
    pc sh 'if not exist {{pc_dir}}\audio mkdir {{pc_dir}}\audio'
    scp -q play/out/radarnite-play.exe play/tools/selftest-audio.ps1 'windows:{{pc_fwd}}/audio/'
    ssh -o BatchMode=yes windows 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File {{pc_dir}}\audio\selftest-audio.ps1 -Dir {{pc_dir}}\audio'

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
