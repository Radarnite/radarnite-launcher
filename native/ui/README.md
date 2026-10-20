# Radarnite Launcher native UI

Win32 + Direct2D/DirectWrite window of Radarnite Launcher 3.0, replacing the WebView2 page (`launcher/ui/index.html`).
Runs against a mock core until `native/core` implements `api.h`.

- Custom title bar (drag, double-click maximize, Snap, resize edges) and dark DWM frame, per-monitor DPI v2
- Builds rail, build details with the four status cells, notices, modes card and settings form
- Activity panel: resizable, filters, search, copy/clear, virtualized list of up to 3,000 lines
- Launch bar with stages, spinner and Launch/Stop; menus and toasts
- Native EDIT fields, icons as Direct2D geometries from the page's SVG, keyboard navigation with a visible focus ring
- Repaints only on events; software Direct2D rasterizer by default (`--render hw` for the GPU)
- `--selftest-ui [dir]`: first frame, RAM and CPU at rest, every scenario of `tests/ui-scenarios.mjs` to PNG, interaction checks

Build and test from the dev box: `just build` (Mac, or the PC when the Mac is busy) and `just run-pc` (selftest on the
PC, results in `selftest/`).
