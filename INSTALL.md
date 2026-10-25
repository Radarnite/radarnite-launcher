# Radarnite Launcher - install, release and auto-update

## Repo and release

- Public source + releases: https://github.com/Radarnite/radarnite-launcher (org `Radarnite`, account `justdiegocom`).
- Latest release: https://github.com/Radarnite/radarnite-launcher/releases/tag/v2.2.0
  - `RadarniteLauncherSetup-2.2.0.exe` (sha256 `892c84d47c0891a8353118cb3e2b3c849f943520b5e7547bbaf3afaf5d7671ab`)
  - `RadarniteLauncherSetup-2.2.0.exe.sha256`

## Where it is installed

- Program: `%LOCALAPPDATA%\Programs\Radarnite Launcher` (per-user, no administrator).
- Builds folder: `S:\FortniteBuilds` (set by the installer; sees 3.6, 14.30, 17.50).
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

## Releases from GitHub Actions (3.x)

`.github/workflows/release.yml` builds and publishes every push to `main` on `ubuntu-latest`; nothing is released by hand.
A pull request (to any branch) runs the same version and installer and publishes nothing; a push to another
branch runs nothing (one trigger per job).

- Version: the last stable tag + 1 patch, or the committed `VERSION` file (X.Y.Z) when that is higher.
  `VERSION` is 3.0.0 while the last tag is v2.2.1; raise it for the next minor or major.
- `main` -> release `vX.Y.Z`, the latest one: every installed launcher updates. "Run workflow" on another branch only
  builds the installer; nothing but `main` is ever published (no prereleases).
- A head commit whose subject (first line) contains `[skip release]` is built but not published.
- Assets: `RadarniteLauncherSetup-<version>.exe`, its `.sha256` (2.x format) and `RadarniteLauncher-<version>.zip` (the
  program folder: no uninstaller, registry keys, shortcuts or 2.x cleanup). Notes: the commits since the last tag.
- The backend is `backend/server.mjs` from the checkout, run by Node. The payload (Node, tools, gameserver, redirect,
  patches, notices) comes from the v2.2.1 installer, pinned by sha256
  (`tools/payload-from-release.sh` says why it is not built in CI). A new payload = pin there the installer of a
  release built with `just release`, which takes it from the PC's dev tree.
- Optional repository secret `RN_PRIVACY_BAN`: more strings for the privacy gate to ban, one per line.

## How 2.x releases were cut (by hand; every PC then self-updates)

1. Bump `<Version>` in `launcher/FnOffline.csproj` (the single source of truth; the updater reads it from the assembly).
2. On Windows, run `tools/build-release.ps1` (reads that version, publishes the launcher self-contained + deterministic
   with no PDB, assembles the installer, writes the `.sha256`). It fails if any staged file still carries the builder
   name/home path or any `C:\Users\`.
3. Publish a GitHub release tagged `v<version>` here with `RadarniteLauncherSetup-<version>.exe` and its `.sha256`.
   Installed launchers pick it up on next start.

## Privacy of the installer

The shipped binaries carry no developer name or home path:
- the launcher is built deterministic with no PDB (no CodeView path);
- every `.pdb` is stripped from the staging;
- prebuilt tool/gameserver binaries (CUE4Parse, NAudio, OggVorbisEncoder, fn-modkit, radarnite-server) have their
  embedded build paths rewritten in place with same-length neutral tokens, so no real name and no `C:\Users\` survive;
- the build verifies this and aborts if anything leaks.
Verified on the installed tree: 0 occurrences of the developer name and 0 of `C:\Users\` across 753 files.

## What was cleaned in the dev tree (S:\FortniteBuilds\launcher)

Moved out (backups, in-use leftovers, stray reports): `backup-*`, `RadarniteLauncher.*.old-in-use-*`,
`INFORME-iostore-firma.md`, `UPDATE-PROMPT.md`. `src`, `tools`, `backend`, `gameserver`, `redirect`, `patches`,
`node`, `ui`, `runtimes`, `webview2-radarnite`, `config.json` were left untouched (still the dev tree the other
agents and the pipeline use; the hard-coded `tools\sigfit-installed.ps1` / `tools\fn-modkit-sigfit` paths still work).

## Not verified

- The full download-install auto-update path end to end (it only triggers when a strictly newer release exists; the
  version check, API reach, semver compare and "up to date" result were verified against the live v2.2.0).
- Cosmetics generation / sig-fit and in-game play were not re-run after the binary path scrub (the scrub only rewrites
  dead PDB path strings in the debug directory, not code).
