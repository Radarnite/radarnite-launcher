# Radarnite Launcher 3.0 developer commands, run from the Linux dev box.
# Builds run on the Mac (ssh `mac`, zig c++ -> x86_64-windows-gnu); the PC (ssh `windows`) only runs the test binaries.
# No machine starts anything above 70 % CPU: the recipes wait (up to 10 min) and then give up.
# RN_WIN_LANE: optional queue wrapper for PC jobs (a script taking the cmd.exe line as its argument, e.g. tools/win.sh).

set shell := ["bash", "-euo", "pipefail", "-c"]

mac_dir := "agent-work/native-ui"
pc_dir := 'S:\native-ui-test'

# Build on the Mac, or on the PC when the Mac does not answer or is above 70 % CPU
build:
    #!/usr/bin/env bash
    set -euo pipefail
    if just _cpu-ok mac; then just build-mac; else echo "Mac busy or away: building on the PC" >&2; just build-pc; fi

# Build native/ui/out/RadarniteUI.exe on the Mac
build-mac: (_wait-cpu "mac")
    rsync -a --relative native/ui/ launcher/ui/fonts/ launcher/ui/radarnite.png launcher/radarnite.ico mac:{{mac_dir}}/repo/
    ssh -o BatchMode=yes mac 'cd ~/{{mac_dir}}/repo/native/ui && mkdir -p out && rm -f out/RadarniteUI.exe && ZIG=/opt/homebrew/bin/zig ./build.sh out/RadarniteUI.exe 2>&1 | grep -v -e nullability -e _Nullable -e _Nonnull -e "note: insert" -e "In file included" -e "^ *|" -e "^ *[0-9]* |" -e "warnings generated"'
    mkdir -p native/ui/out
    scp -q mac:{{mac_dir}}/repo/native/ui/out/RadarniteUI.exe native/ui/out/RadarniteUI.exe
    ls -l native/ui/out/RadarniteUI.exe

# Build native/ui/out/RadarniteUI.exe on the PC (zig in %LOCALAPPDATA%\fp-tools\zig)
build-pc: (_wait-cpu "windows")
    ssh -o BatchMode=yes windows 'if not exist {{pc_dir}}\src mkdir {{pc_dir}}\src'
    tar -c native/ui/*.cpp native/ui/*.h native/ui/app.rc native/ui/app.manifest native/ui/build.rsp native/ui/build.cmd \
      launcher/ui/fonts launcher/ui/radarnite.png launcher/radarnite.ico | ssh -o BatchMode=yes windows 'tar -x -C {{pc_dir}}\src'
    ssh -o BatchMode=yes windows 'cd /d {{pc_dir}}\src\native\ui && set "ZIG=%LOCALAPPDATA%\fp-tools\zig\zig.exe" && build.cmd {{pc_dir}}\RadarniteUI.exe'
    mkdir -p native/ui/out
    scp -q 'windows:S:/native-ui-test/RadarniteUI.exe' native/ui/out/RadarniteUI.exe
    ls -l native/ui/out/RadarniteUI.exe

# Copy the exe to the PC, run --selftest-ui in the signed-in session (cloaked window) and bring PNGs + JSON to native/ui/selftest
run-pc *args: (_wait-cpu "windows")
    test -f native/ui/out/RadarniteUI.exe || just build
    ssh -o BatchMode=yes windows 'if not exist {{pc_dir}} mkdir {{pc_dir}}'
    scp -q native/ui/out/RadarniteUI.exe 'windows:{{pc_dir}}\RadarniteUI.exe'
    (printf '$env:RN_UI_DIR=%s; $env:RN_UI_ARGS=%s\n' "'{{pc_dir}}'" "'{{args}}'"; cat native/ui/tools/selftest-session.ps1) | \
      ${RN_WIN_LANE:-ssh -o BatchMode=yes windows} 'powershell -NoProfile -NonInteractive -Command -' | grep -E 'firstFrame|private|idle|renderer|exeBytes|stopped'
    mkdir -p native/ui/selftest
    scp -q 'windows:S:/native-ui-test/out/*' native/ui/selftest/

# Run the selftest over plain ssh (session 0: no desktop, so no GPU and a forced first frame); PNGs are the same
run-pc-ssh *args: (_wait-cpu "windows")
    test -f native/ui/out/RadarniteUI.exe || just build
    ssh -o BatchMode=yes windows 'if not exist {{pc_dir}} mkdir {{pc_dir}}'
    scp -q native/ui/out/RadarniteUI.exe 'windows:{{pc_dir}}\RadarniteUI.exe'
    ${RN_WIN_LANE:-ssh -o BatchMode=yes windows} 'cd /d {{pc_dir}} && start "" /wait RadarniteUI.exe --selftest-ui {{pc_dir}}\out-ssh {{args}} & type out-ssh\selftest.json'

# Remove the temporary folders on the Mac and the PC
clean-remote:
    ssh -o BatchMode=yes mac 'rm -rf ~/{{mac_dir}}'
    ssh -o BatchMode=yes windows 'if exist {{pc_dir}} rmdir /s /q {{pc_dir}}'

# exit 0 when the host answers and its CPU is at or below 70 % (1 = busy, 75 = no answer)
_cpu-ok host:
    #!/usr/bin/env bash
    set -uo pipefail
    if [ "{{host}}" = mac ]; then
      cpu=$(ssh -o BatchMode=yes -o ConnectTimeout=5 mac "top -l 2 -n 0 -s 1 | grep 'CPU usage' | tail -1" 2>/dev/null | sed -E 's/.* ([0-9.]+)% idle.*/\1/' | awk '{printf "%d", 100-$1}')
    else
      cpu=$(ssh -o BatchMode=yes -o ConnectTimeout=5 windows 'powershell -NoProfile -Command "(Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average"' 2>/dev/null | tr -dc '0-9')
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
