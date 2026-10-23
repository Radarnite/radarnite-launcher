# Core tests (PC)

Run by hand before a release; none of this runs in CI. Each check guards one risk that is expensive when it breaks.
The parity scripts compare with Radarnite Launcher 2.2.1, which is extracted from its release installer with 7-Zip and
never installed. Everything runs in one scratch folder (`-Dir`, default `S:\rl-core-test`). The real builds folder is
only read, and the real firewall group is only counted.

- `update-check.ps1` + `fake-github.mjs` (`just core-update-test-pc`): `--update-check` of builds as 3.0.0, 3.0.0-rc.1
  and 3.0.0-rc.2 (and of the window exe) against a fake GitHub API on 127.0.0.1 (`RADARNITE_UPDATE_API`): a release
  build never reads the release list and prints what 2.x printed; an rc is offered the next rc or the final, never an
  older one or a draft; then one check against the real API
- `parity2.ps1`: `config.json` must not be lost (a broken file is kept as `config.json.broken`, a BOM and other letter
  case are read, unknown keys are kept), `--stop` only stops processes started from inside the root, firewall rules in
  a temp group (a second run replaces them, the real group is untouched). It also builds the fake builds folder the
  other two scripts use, so it runs first
- `parity3.ps1`: add a build from a zip or a folder (a folder on another drive is copied and its source kept)
- `parity4.ps1`: the updater's download through the release redirect with its streamed SHA-256 and the `.sha256`
  token; one launch for a double click (a second Launch never starts a second session)
- `coretest.cpp`: harness for `rn::Core` with a printing `UiSink`, for `builddata::addZip`/`addFolder`, the updater's
  download and SHA-256 (`fetch`, `sha`), and `coretest semver`: the updater's tag parsing and semver precedence
  against a table

Layout the scripts expect in `-Dir`: `rlcoretest-ref\` (the extracted 2.2.1; `parity2.ps1` creates its config with
`--set-builds-folder S:\FortniteBuilds`), `rn-core-cli.exe`, `coretest.exe` (`RN_CORETEST=1 build.sh`) and, for
`parity4.ps1`, `RadarniteLauncherSetup-2.2.1.exe`. The ref folder name must not be the real install's
(`Radarnite Launcher`), and the fake builds are named `rlcoretest-*`: the firewall rule names are
`FN Offline - <folder>\...`, and creating a rule first removes any rule with the same name.
