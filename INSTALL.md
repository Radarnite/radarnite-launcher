# Radarnite Launcher - install, release and auto-update

## Repo and release

- Public source + releases: https://github.com/Radarnite/radarnite-launcher (org `Radarnite`, account `justdiegocom`).
- Latest release: https://github.com/Radarnite/radarnite-launcher/releases/latest, published by `.github/workflows/release.yml`
  on every push to `main`: `RadarniteLauncherSetup-<version>.exe`, its `.sha256` and `RadarniteLauncher-<version>.zip`.

## Where it is installed

- Program: `%LOCALAPPDATA%\Programs\Radarnite Launcher` (per-user, no administrator).
- Builds folder: `S:\Fortnite\FortniteBuilds` (set by the installer; sees 3.6, 14.30, 17.50).
- The installer never ships or overwrites `config.json`: the launcher creates it on first run and keeps it on upgrades.

## Auto-update (how it works)

On start (and from the UI menu "Check for updates", and `RadarniteLauncher.exe --update-check` on the console), the
launcher calls `https://api.github.com/repos/Radarnite/radarnite-launcher/releases/latest` (no token; public), reads
the release tag, and compares it as semver with its own assembly version. If the release is newer it downloads the
attached `RadarniteLauncherSetup-<ver>.exe` to `%TEMP%`, verifies it against the release's `.sha256` asset (or, if
absent, the asset's own sha256 `digest` from the GitHub API), and only then runs it and closes the launcher so NSIS
can replace the files. Offline-first: any network/parse/verification error is written to `update.log` in the install
folder and otherwise ignored; a PC with no internet is never blocked and an unverified binary is never run. Code:
`launcher/Update.cs`, wired from `launcher/Web.cs` (start + menu) and `launcher/Program.cs` (`--update-check`, MainForm).

## How a release is cut (every PC then self-updates)

Nothing by hand: merging to `main` is the release. `release.yml` picks the version (`tools/ci-version.sh`: the last
`vX.Y.Z` tag plus one patch, or `launcher/FnOffline.csproj`'s `<Version>` when higher), builds the installer on
`ubuntu-latest` (`tools/ci-build.sh`: `dotnet publish` self-contained win-x64, deterministic, no PDB; the payload of
prebuilt tools from the pinned 2.2.1 installer, `tools/payload-from-release.sh`; `makensis`; the privacy gate below),
then publishes the GitHub release `v<version>` with `RadarniteLauncherSetup-<version>.exe`, its `.sha256` and the zip,
and checks that `releases/latest` now serves it. Pull requests build the same installer without publishing.
`[skip release]` in the merge commit skips the release; "Run workflow" on another branch publishes a prerelease
(`vX.Y.Z-<branch>.N`) that installed launchers never see. `tools/build-release.ps1` still builds the same installer on
Windows from the dev tree when a new payload (tools, Node, game DLLs) has to be published and pinned.

## Privacy of the installer

The shipped binaries carry no developer name or home path:
- the launcher is built deterministic with no PDB (no CodeView path);
- every `.pdb` is stripped from the staging;
- prebuilt tool/gameserver binaries (CUE4Parse, NAudio, OggVorbisEncoder, fn-modkit, radarnite-server) have their
  embedded build paths rewritten in place with same-length neutral tokens, so no real name and no `C:\Users\` survive;
- the build verifies this and aborts if anything leaks.
Verified on the installed tree: 0 occurrences of the developer name and 0 of `C:\Users\` across 753 files.

## What was cleaned in the dev tree (S:\Fortnite\FortniteBuilds\launcher)

Moved out (backups, in-use leftovers, stray reports): `backup-*`, `RadarniteLauncher.*.old-in-use-*`,
`INFORME-iostore-firma.md`, `UPDATE-PROMPT.md`. `src`, `tools`, `backend`, `gameserver`, `redirect`, `patches`,
`node`, `ui`, `runtimes`, `webview2-radarnite`, `config.json` were left untouched (still the dev tree the other
agents and the pipeline use; the hard-coded `tools\sigfit-installed.ps1` / `tools\fn-modkit-sigfit` paths still work).

## Not verified

- The full download-install auto-update path end to end (it only triggers when a strictly newer release exists; the
  version check, API reach, semver compare and "up to date" result were verified against the live v2.2.0).
- Cosmetics generation / sig-fit and in-game play were not re-run after the binary path scrub (the scrub only rewrites
  dead PDB path strings in the debug directory, not code).
