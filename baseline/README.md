# Baseline: Radarnite Launcher 2.2.1

Numbers the 3.0 rewrite is measured against. `baseline.json` has every value, the targets from the plan and how each
value was taken. `installed-2.2.1-files.tsv` lists every installed file (relative path, bytes, sha256).

## Results

Measured on a Ryzen 5 3600 with an RTX 2060 and Windows 11, in normal use (`cpuLoadPercent` in the JSON).

| Metric | 2.2.1 | 3.0 target |
| --- | --- | --- |
| `--check 14.30`, median of 20 | 348 ms (an earlier run: 288 ms) | 100 ms |
| `--check` split: .NET start, scan, config, per-build checks | 96 + 40 + 33 + 179 ms | |
| First frame with content, warm median | 705 ms (an earlier run: 632 ms); cold ~3.2 s | 150 ms |
| Blank window visible | 224 ms | |
| Private RAM at 10 s: launcher plus 6 WebView2 processes | 204 MB, of which the launcher is 34 MB | 35 MB |
| Idle CPU, whole tree | 0.16 % of one core | 0 % |
| Installed | 373 MB, 752 files: .NET 160, tools 119, node 89, gameserver 2.5, WebView2 SDK 1.8 | about 125 MB |
| Backend ready / RSS, real 14.30 data | 961 ms / 223 MB; without the 92 MB `custom-cosmetics.json`: 299 ms / 53 MB | 30 ms / 15 MB |

## How it was measured

Run `baseline/measure-2x.ps1` on the Windows PC as one job over ssh. It writes to `S:\rl-release-test\out`.

- **Install.** Downloads the `v2.2.1` release asset and checks it against its `.sha256`. Installs it silently with
  `/S /BUILDS=S:\FortniteBuilds /D=S:\rl-release-test\Radarnite Launcher`. This uses the real installer, layout and
  `config.json`, but not the normal `%LOCALAPPDATA%\Programs` folder, because the PC had no installed launcher (the dev
  tree is run directly). The script refuses to run if any Radarnite Launcher registry key or shortcut already exists. At the
  end it removes exactly what the installer created.
- **Size.** Taken right after the install: payload, `config.json` and the uninstaller. Components are classified by
  folder and by root file name.
- **`--check 14.30`.** Times from process creation to exit with output drained, as the median of 20 runs after 3
  warm-ups. To split the time, it also times commands that stop earlier:
  - `--set-builds-folder` with no argument: the .NET start-up floor; it exits with code 2.
  - `--scan S:\FortniteBuilds`: start-up plus the build scan.
  - `--check` with an unknown build: adds the config.
  - The rest is the per-build checks. `Firewall.Check` runs twice, once directly and once from `Preflight`. Each time
    it enumerates every firewall rule through COM and walks the build for `*.exe`. `Preflight` also reads every
    process image path. Those three operations are timed separately from PowerShell, so those numbers are approximate.
- **Backend.** Runs `node\node.exe backend\server.mjs --build <copy> --config <config.json> --port <free>` on a scratch
  copy of `14.30\offline` (`cosmetics.json`, `custom-cosmetics.json`, `state.json`), never the real build folder. Ready
  time runs from process creation to the `ready 127.0.0.1:PORT` line; RSS is the working set 300 ms later. Node's own
  floor is measured the same way: `node -e` alone, and an idle `http` server. `measure-backend.ps1` repeats this with
  three versions of the data: full, without `custom-cosmetics.json` (92 MB for 14.30: it alone costs about 620 ms and
  170 MB), and empty.
- **Window** (`session-bench.ps1`). Runs in the signed-in session through a scheduled task and a hidden wscript
  launcher, because over ssh everything runs in session 0, with no desktop and no GPU.
  - `--selftest-ui` is the launcher's own developer mode: the real window at -32000,-32000, no taskbar button, no
    single-instance mutex.
  - `windowVisibleMs`: process creation until the WinForms frame is first visible, still blank.
  - `pageReadyMs`: process creation until the page sends `ready`, taken as PNG creation time minus 2000 ms (the
    launcher writes the PNG exactly 2 s after `ready`). This is the first frame with content.
  - The first run builds a fresh WebView2 profile, so it is reported apart as cold.
  - RAM at 10 s and idle CPU come from the normal main window (`--new-instance`, no mutex) started on a separate Win32
    desktop that is never shown. It is fully awake, not minimized, so the 2.2.1 dormant mode does not start, and it is
    invisible. Measures: private bytes of the whole process tree at 10 s (launcher and its WebView2 processes), and
    CPU time of the tree from 10 s to 20 s.
  - A `--selftest-ui` run on that desktop gives the same `pageReadyMs` as on the normal desktop, so the hidden desktop
    does not change the result. Its GPU process is present.
  - Every process started is killed by PID tree and by its WebView2 profile path.

Timer resolution limits window times to about ±16 ms. The PC was in normal use while measuring (CPU load is recorded
in `machine`).

## What 3.0 must keep, because other things use it

- **The program folder.** These keep their names and places:
  - `RadarniteLauncher.exe`: shortcuts, the uninstall key, `--play` desktop shortcuts and the firewall rule key.
  - `tools\sigfit-installed.ps1`, `tools\fn-modkit-sigfit\fn-modkit.dll` and `tools\cosmetics-gen\fn-cosmetics-gen.exe`.
    `sigfit-installed.ps1` is launched by the launcher before each classic-pak launch as
    `powershell -File tools\sigfit-installed.ps1 -Build <dir>`. It finds the tool next to itself and runs it with
    `%LOCALAPPDATA%\fp-dotnet\dotnet.exe` (framework-dependent), not with the launcher's own .NET runtime, so removing
    that runtime does not break it.
  - `redirect\fnredirect.dll`, `gameserver\radarnite-server.dll` and `patches\fnpakpatch.dll`: they are injected into
    the game.
- **The command line.** `RadarniteLauncher.exe --play "<version>"` must keep working when started hidden, from the
  launcher folder, with the process staying alive as `RadarniteLauncher.exe` while the game runs, with `--play` in its
  command line. Several scripts start it this way and then find it by name and command line:
  `S:\skin-launcher\ps\{start-launcher,test-skin,play-skin}.ps1`, `S:\port-cosmeticos\ps\{start-launcher,test-port}.ps1`,
  `S:\cosmeticos-1430\ps\{play.ps1,play1430.cmd}` and `S:\fn-modkit\scripts\play-test.ps1`. `--firewall-remove` is
  documented in `README-offline.md` and used by the uninstaller.
- **The dev tree.** `S:\FortniteBuilds\launcher` keeps `tools\`, `node\` and `backend\` for the importers and scripts:
  - `S:\skin-launcher\ps\import-skin.ps1` exits with code 6 unless `<launcher>\backend\server.mjs` contains
    `custom-cosmetics.json`, then calls `<launcher>\tools\sigfit-installed.ps1 -Build`.
  - `install-skin.ps1` looks for `backend\server.mjs` or `src\backend\server.mjs`.
  - `S:\port-cosmeticos\ps\port.ps1` hard-codes `S:\FortniteBuilds\launcher\backend\server.mjs` and
    `...\tools\sigfit-installed.ps1`.
  - The Rust backend must read `<build>\offline\custom-cosmetics.json` the same way.
- **Unchanged identifiers.** The firewall group `FN Offline Builds`, `HKCU\Software\Radarnite\Launcher` (`InstallDir`,
  `BuildsFolder`), the uninstall key `RadarniteLauncher`, `config.json` in the program folder, and `update.log` there.
- **Release assets.** Named `RadarniteLauncherSetup-<version>.exe` and `.exe.sha256`, in the format
  `<sha256>  <name>` with no newline. The 2.x updater matches those names and reads the first 64 hex characters.
