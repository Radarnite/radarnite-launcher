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
- `tools/build-release.ps1` — builds `RadarniteLauncherSetup-<version>.exe` and its `.sha256`.

## Releasing

The version lives only in `launcher/FnOffline.csproj` (`<Version>`). Bump it, then run `tools/build-release.ps1`
on Windows (it reads that version, builds the launcher, assembles the installer and writes the `.sha256`). Publish
the resulting `RadarniteLauncherSetup-<version>.exe` and its `.sha256` as a GitHub release here, tagged `v<version>`:
every installed launcher checks this repo on start and offers the update.

## License

The launcher and tooling are the author's own work. `gameserver-reboot/` and the libraries under `*/vendor/` keep
their own licenses (see their `LICENSE` files).
