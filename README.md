# Radarnite Launcher

Play old Fortnite builds on your own PC, offline. No Epic account, no internet, no servers: a local backend on
`127.0.0.1` stands in for Epic, and firewall rules keep each build off the network.

## Features

- Runs old Fortnite builds (tested on 3.6, 14.30, 17.50) fully offline.
- Local backend (Node) that emulates the Epic services the game expects: auth, profiles, friends, matchmaking, MCP.
- Local match server (gameserver) for Battle Royale with bots and a practice/creative mode.
- Cosmetics generated from the build's own data (CUE4Parse); custom skins via the IoStore sig-fit tooling.
- One-click firewall rules so a build can never reach the internet.
- WebView2 desktop UI (with a plain WinForms fallback).
- Per-user install under `%LOCALAPPDATA%\Programs\Radarnite Launcher`; no administrator needed.
- Automatic updates from this repo's public GitHub releases (verified by SHA-256 before anything runs).

## Layout

- `launcher/` — the desktop app (.NET 8, WinForms + WebView2).
- `backend/` — the local Epic-emulation backend (`server.mjs`).
- `redirect/` — the DLL that redirects the game's traffic to the local backend.
- `gameserver-reboot/` — the local match server.
- `cosmetics-gen/`, `pakpatch/` — cosmetics and pak tooling.
- `radarnite.nsi` — the NSIS installer script.
- `tools/ci-build.sh` — builds `RadarniteLauncherSetup-<version>.exe`, its `.sha256` and `RadarniteLauncher-<version>.zip` on
  Linux or macOS (what `.github/workflows/release.yml` runs); `tools/build-release.ps1` is the Windows equivalent.

## Releasing

Releases come from GitHub Actions (`.github/workflows/release.yml`), never by hand. Every push to `main` publishes
`vX.Y.Z` = the last release plus one patch (or `launcher/FnOffline.csproj`'s `<Version>` when that is higher: bump it
there for a minor or major), with `RadarniteLauncherSetup-<version>.exe`, its `.sha256` and the program-folder zip,
built on `ubuntu-latest` (`dotnet publish` for the launcher, `makensis` for the installer; the prebuilt tools, Node and
game DLLs come from the pinned 2.2.1 installer, see `tools/payload-from-release.sh`). Pull requests build the same
installer without publishing; `[skip release]` in the merge commit skips the release. Every installed launcher checks
`releases/latest` on start and offers the update.

## License

The launcher and tooling are the author's own work. `gameserver-reboot/` and the libraries under `*/vendor/` keep
their own licenses (see their `LICENSE` files).
