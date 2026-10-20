# Radarnite Launcher native core

C++ core of Radarnite Launcher 3.0: the logic of `launcher/*.cs` without the WebView, behind `native/ui/api.h`.
Same command line, stdout, exit codes, files (`config.json`, `offline\build.json`) and firewall rules as 2.2.x.

- `paths`, `config`, `buildinfo`: launcher and builds folders, `config.json` (unknown keys kept), memory-mapped
  version scan with the `offline\build.json` cache, lobby/match support
- `firewall`: `INetFwPolicy2` through vtables, one rule enumeration per read, create or remove
- `proc`, `session`: process list, job objects (`PROC_THREAD_ATTRIBUTE_JOB_LIST`), preflight, StopAll, the backend
  (`backend\radarnite-backend.exe`, else `node\node.exe backend\server.mjs`), health wait, write audit
- `launch.h`: the one game-launch entry point, `LaunchGame`; `launch_stub.cpp` answers
  "game launch not implemented in this core"
- `net`, `update`: WinHTTP + BCrypt SHA-256 self-update, same assets and `.sha256` check as `Update.cs`
- `builddata`: AES keys and mappings download, `fn-cosmetics-gen`, add a build from a zip (Windows `tar.exe`) or a folder
- `cli`: `--check --scan --set-builds-folder --firewall --firewall-remove --update-check --cosmetics --stop --selftest-inject`
- `core`: `rn::createCore()` implements `api.h`; `rn::core::ForwardToRunningInstance()` signals an open window

## Build

`ZIG=/opt/homebrew/bin/zig native/core/build.sh [outdir]` (Mac or PC, never the dev box):

- `out/librncore.a`: every module plus yyjson
- `out/rn-core-cli.exe`: the command line alone (console subsystem), for tests

Version: `version.h` (3.0.0), or `RN_DEFINES='-DRN_VERSION_MAJOR=3 -DRN_VERSION_MINOR=0 -DRN_VERSION_PATCH=1'`.
Test switches (environment): `RADARNITE_LAUNCHER_DIR` (another launcher folder: its `config.json`, `node`, `tools`),
`RADARNITE_FW_GROUP` (firewall group instead of `FN Offline Builds`). Neither crosses the UAC prompt of the elevated
`--firewall` re-run.

## Linking the window to the real core

`ui-link.diff` is the change to `native/ui/main.cpp`, checked by linking it (649 KB):

1. `rn::cli::run(argc, argv, &handled)` right after `CommandLineToArgvW`, before any window or mutex; return its exit
   code when `handled`. The installer's `--set-builds-folder` and the elevated `--firewall` go through it.
2. `--play <build>` → `rn::createCore(build)`; otherwise `rn::createCore(nullptr)` instead of `createMockCore(nullptr)`.
   The mock stays for `--scenario` and `--selftest-ui`.
3. Single instance stays in WinMain (2.2.1 mutex and event); a launch that loses the mutex may call
   `rn::core::ForwardToRunningInstance()`.

In `build.rsp`: `../core/out/librncore.a` after `app.res`, and `-lwinhttp -lbcrypt -loleaut32 -ladvapi32`. Build the
core first for the same target. A `native/core/launch.cpp` providing `LaunchGame` replaces the stub: `build.sh` uses it
when present; for the window, link its object before `librncore.a`.

## Not in this core

- The game launch (`launch.h`).
- The 2.x on-demand match server (`Session.MatchLoop`, `StartServerAsync`, `WatchMatch`): it starts a second game
  process, so it belongs with the game launch. The session's "server" stage and match phases stay empty.

Tests: `test/README.md`.
