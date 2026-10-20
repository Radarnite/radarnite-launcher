# Radarnite Launcher native UI

Win32 + Direct2D/DirectWrite window of Radarnite Launcher 3.0, replacing the WebView2 page (`launcher/ui/index.html`).
Linked with the real core (`native/core`, `api.h`) into `RadarniteLauncher.exe`; a mock core stays for `--scenario`
and `--selftest-ui`.

- Custom title bar (drag, double-click maximize, Snap, resize edges) and dark DWM frame, per-monitor DPI v2
- Builds rail, build details with the four status cells, notices, modes card and settings form
- Activity panel: resizable, filters, search, copy/clear, virtualized list of up to 3,000 lines
- Launch bar with stages, spinner and Launch/Stop; menus, toasts and the page's tooltips
- Native EDIT fields, icons as Direct2D geometries from the page's SVG, keyboard navigation with a visible focus ring
- The page's transitions (`anim.h`: 120/180 ms, its easing curves): hover fades, switches, Launch press and sheen,
  toasts, loading shimmer; off with Windows "Show animations"
- Repaints only on events and only the rects that change; nothing runs at rest. Software Direct2D rasterizer by
  default (`--render hw` for the GPU)
- `--selftest-ui [dir]`: first frame, RAM and CPU at rest, transition checks in the live window, every scenario of
  `tests/ui-scenarios.mjs` (plus 4 native ones) to PNG, keyboard and mouse checks

Build and test from the dev box: `just build-mac` (the core, then this window; `just build` adds the backend, and falls
back to the PC when the Mac is busy), `just check-pc` (selftest on the PC, results in `selftest/`, then the same
scenarios rendered by Chromium and compared: `selftest/compare.json`, crops in `out/compare/`) and `just smoke-pc`
(the real core in the window on a hidden desktop, next to 2.2.1: `tools/window-smoke.ps1`).
