# Radarnite Launcher native UI

Win32 + Direct2D/DirectWrite window of Radarnite Launcher 3.0, replacing the WebView2 page (`launcher/ui/index.html`).
Linked with the real core (`native/core`, `api.h`) into `RadarniteLauncher.exe`; a mock core stays for `--scenario`
and `--selftest-ui`.

- Custom title bar (drag, double-click maximize, Snap, resize edges) and dark DWM frame, per-monitor DPI v2
- Lobby screen: the selected build's season art fills the window (per-season gradient, light rays, the battle-pass
  skin from the cosmetics API, cached in `cache\art` and decoded off the UI thread at the size it is drawn), build tabs
  with thumbnails, the version, status and Launch at the bottom left, a glass side panel with the status cells,
  modes and settings (`art.cpp`: the season table and the loader)
- Activity panel: resizable, filters, search, copy/clear, virtualized list of up to 3,000 lines
- Launch bar with stages, spinner and Launch/Stop; menus, toasts and the page's tooltips
- Native EDIT fields, icons as Direct2D geometries from the page's SVG, keyboard navigation with a visible focus ring
- The page's transitions (`anim.h`: 120/180 ms, its easing curves): hover fades, switches, Launch press and sheen,
  toasts, loading shimmer; off with Windows "Show animations"
- Repaints only on events and only the rects that change; nothing runs at rest. Software Direct2D rasterizer by
  default (`--render hw` for the GPU)
- `--selftest-ui [dir]`: first frame, RAM and CPU at rest, transition checks in the live window, every scenario of the
  mock core (`mock_api.cpp`: the 2.x page's scenarios plus 4 native ones) to PNG, keyboard and mouse checks

Build and test from the dev box: `just build-mac` (the core, then this window; `just build` adds the backend, and falls
back to the PC when the Mac is busy) and `just selftest-pc` (the selftest on the PC; `selftest/` keeps its
`selftest.json` and two of its PNGs).
