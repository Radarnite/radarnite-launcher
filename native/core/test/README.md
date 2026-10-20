# Core tests (PC)

Parity against Radarnite Launcher 2.2.1, which is extracted from its release installer with 7-Zip and never installed.
Everything runs in one scratch folder (`-Dir`, default `S:\rl-core-test`). The real builds folder is only read, and
the real firewall group is only counted.

- `parity1.ps1`: read-only commands on the real builds folder (`--check`, `--scan`, usage errors), stdout bytes and exit
  codes, plus `--check` timing (median of 20 after 3 warm-ups, the `baseline/measure-2x.ps1` method)
- `parity2.ps1`: a fake builds folder (odd folders, ASCII-only and overflowing versions, nested and duplicate games),
  the `build.json` cache, `config.json` load/save cases, `--set-builds-folder`, `--stop` in the scratch root,
  firewall rules in a temp group, `--play` through the stub, `--cosmetics`, `--update-check`
- `parity3.ps1`: the same with the window-subsystem build (no console, like 2.x), config escaping over every BMP code
  point, `rn::Core` through `coretest.exe`, add from a zip or a folder
- `oracle.ps1` + `escapes.py`: give every BMP code point to 2.2.1's config writer; `escapes.py` turns its output into
  the escape table in `json.cpp`
- `coretest.cpp`: harness for `rn::Core` with a printing `UiSink`, for `builddata::addZip`/`addFolder`, and
  `coretest semver`: the updater's tag parsing and semver precedence against a table
- `update-check.ps1` + `fake-github.mjs` (`just core-update-test-pc`): `--update-check` of builds as 3.0.0, 3.0.0-rc.1
  and 3.0.0-rc.2 (and of the window exe) against a fake GitHub API on 127.0.0.1 (`RADARNITE_UPDATE_API`): a release
  build never reads the release list and prints what 2.x printed; an rc is offered the next rc or the final, never an
  older one or a draft; then one check against the real API

Layout the scripts expect in `-Dir`: `rlcoretest-ref\` (the extracted 2.2.1; its config is created with
`--set-builds-folder S:\FortniteBuilds`), `rn-core-cli.exe`, `rn-core-cli-220.exe` (`RN_VERSION=2.2.0 build.sh`, for
the exit 10 path), `coretest.exe` (`RN_CORETEST=1 build.sh`), `RadarniteLauncher-linktest.exe` (`ui-link.diff`
applied). The ref folder name must not
be the real install's (`Radarnite Launcher`), and the fake builds are named `rlcoretest-*`: the firewall rule names
are `FN Offline - <folder>\...`, and creating a rule first removes any rule with the same name.
