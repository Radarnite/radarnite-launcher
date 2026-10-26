# Radarnite Launcher

Play old Fortnite builds on your own PC, offline. No Epic account, no internet, no servers: a local backend on
`127.0.0.1` stands in for Epic, and firewall rules keep each build off the network.

## Features

- Runs old Fortnite builds (tested on 3.6, 14.30, 17.50) fully offline.
- Local backend (Node) that emulates the Epic services the game expects: auth, profiles, friends, matchmaking, MCP.
- Local match server (gameserver) for Battle Royale with bots and a practice/creative mode.
- Cosmetics generated from the build's own data (CUE4Parse); custom skins via the IoStore sig-fit tooling.
- Play modes per profile: Legal (start with the defaults; levels, battle pass and V-bucks from matches; today's real
  item shop limited to what the build has), Legal with infinite V-bucks, or everything unlocked.
- Ragdoll deaths on 14.30: eliminated players fall with physics instead of the digitize effect.
- Mods for 14.30, on or off in the Mods tab: the engine console (F10), an in-game menu (F8), field of view, FPS cap,
  no post effects, slow motion and fly. Both keys can be changed; a key another program holds as a hotkey is marked.
- Match cheats for 14.30 (Mods tab and F8 menu): aim assist, teleport to the crosshair, the map pin or a random POI,
  spawn bots around you (enemies or teammates), god mode.
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
- `paktools/` — the one copy of the pak signature fitter (sigfit) and the game guard the launcher ships
  (`tools/vendor-pak-tools.sh`).
- `pakpatch/fnmods.c` — `fnmods.dll`, injected by the launcher into the 14.30 client: one DLL with modules (body
  sliders of fn-port outfits, ragdoll deaths, engine console, settings, match cheats, F8 menu), each on or off in `mods.json`
  (`pakpatch/test/fnbody-mock.c` runs the body sliders against a fake engine).
- `backend/progress.mjs` — play modes, profiles, levels, battle pass (`backend/seasons/`), V-bucks and the item shop.
- `radarnite.nsi` — the NSIS installer script.
- `tools/release.sh` — builds `RadarniteLauncherSetup-<version>.exe` and its `.sha256`.

## Releasing

GitHub Actions builds and publishes every push to `main` as the latest release (every installed launcher
updates) and builds every pull request. Versions and assets: see `INSTALL.md`.

## License

The launcher and tooling are the author's own work. `gameserver-reboot/` and the libraries under `*/vendor/` keep
their own licenses (see their `LICENSE` files).
