// Radarnite Launcher 3.0 native window (Win32 + Direct2D/DirectWrite) over the real core (native/core, api.h).
//
//   RadarniteLauncher.exe                         the launcher window (real core)
//   RadarniteLauncher.exe --check|--scan|...      the 2.x console verbs (native/core/cli.h): no window, no mutex
//   RadarniteLauncher.exe --play <build>          launch that build; the window closes when the game closes
//                                                 (--automated, or started hidden: a script's launch, refused with exit
//                                                 code 6 while an anti-cheat or another game runs; core.h)
//   RadarniteLauncher.exe --scenario <id>         interactive, mock core, one scenario of tests/ui-scenarios.mjs
//   RadarniteLauncher.exe --selftest-ui [dir]     mock core, no visible window: first frame, RAM and CPU at rest, then
//                                                 every scenario rendered to <dir>\<id>.png (WIC) + <dir>\selftest.json;
//                                                 exit 0 when all ok
//   options: --render hw (Direct2D on the GPU instead of its software rasterizer), --exit-after <ms>,
//            --new-instance (developer: open the window even if one is already open),
//            --snapshot <png> (tests: the frame rendered offscreen to a PNG when the window is asked to close; the native
//            EDIT fields are not in it; for a window on a desktop DWM does not show, where no capture sees Direct2D)
//
// The window is single-instance per Windows session like 2.2.1 (launcher/Program.cs SingleInstance, same mutex and
// event names, so 2.x and 3.0 share them): starting it again brings the open window to the front and exits. The
// command-line modes (console verbs, --play, --selftest-ui) never take the mutex.
//
// Software rendering is the default: measured on the PC (125 %, 1500x1000 px) it draws a frame in ~8 ms and keeps the
// process at ~22 MB private / 111 ms to the first frame, against ~56 MB / 290 ms with a D3D device (driver memory).
//
// Custom frame like launcher/Web.cs: the window keeps WS_THICKFRAME | WS_CAPTION (Snap, animations, DWM shadow and
// rounded corners), WM_NCCALCSIZE gives it all to the client, WM_NCHITTEST answers the resize edges and HTCAPTION
// on the title bar (drag, double-click maximize, system menu are native).
#include "ui.h"
#include "../core/cli.h"
#include "../core/core.h"
#include "resource.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <string>

using namespace rn;

namespace {

Gfx g_gfx;
App* g_app = nullptr;
ID2D1HwndRenderTarget* g_rt = nullptr;
bool g_software = true, g_selftest = false;
int g_paints = 0, g_timers = 0;
double g_dirtyMax = 0;   // largest WM_PAINT update rect (px^2) since the selftest reset it
HRESULT g_lastHr = S_OK, g_createHr = S_FALSE;
RECT g_client = {};
double g_lastFrameMs = 0;
double g_firstFrameMs = -1;
double g_tGfx = 0, g_tWindow = 0, g_tTarget = 0;   // first-frame breakdown (ms since the process started)
const UINT WM_APP_SHOW = WM_APP + 16;   // another launch asked the open window to come to the front
int g_showRequests = 0;
std::wstring g_snapshot;   // --snapshot <png>
bool snapshot(HWND h, const std::wstring& path);

// ------------------------------------------------------------------------------------------------ single instance
// One main window per Windows session, as 2.2.1 does it: every copy of the launcher (installed or a dev tree) shares a
// session-local mutex; a new launch signals the named event, the open window restores itself and comes to the front,
// and the new process exits. `suffix` is only for the selftest (its own names, never the real launcher's).
namespace single {
const wchar_t kMutex[] = L"Local\\RadarniteLauncher.MainWindow", kShow[] = L"Local\\RadarniteLauncher.ShowMainWindow";
HANDLE g_mutex = nullptr, g_show = nullptr, g_wait = nullptr;

// True when this process owns the main window; otherwise the open one was asked to come to the front.
bool claim(const std::wstring& suffix) {
    HANDLE m = CreateMutexW(nullptr, TRUE, (kMutex + suffix).c_str());
    if (!m) return true;   // never block the launcher on a mutex problem
    if (GetLastError() != ERROR_ALREADY_EXISTS) { g_mutex = m; return true; }
    // an open main window listens on the event: ask it to come to the front and step aside (the core signals it)
    if (rn::core::ForwardToRunningInstance(suffix.c_str())) { CloseHandle(m); return false; }
    // no listener: the owner is exiting (e.g. closing for an update) or still starting; wait for it briefly
    DWORD w = WaitForSingleObject(m, 3000);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { g_mutex = m; return true; }   // abandoned: the owner died, it is ours
    CloseHandle(m);
    return false;
}

void CALLBACK signalled(PVOID hwnd, BOOLEAN) { PostMessageW((HWND)hwnd, WM_APP_SHOW, 0, 0); }

// Owner side: restore and focus the window when signalled (thread-pool wait, so it also works in modal loops).
void listen(HWND h, const std::wstring& suffix) {
    if (!g_mutex) return;
    g_show = CreateEventW(nullptr, FALSE, FALSE, (kShow + suffix).c_str());   // auto-reset
    if (g_show && !RegisterWaitForSingleObject(&g_wait, g_show, signalled, h, INFINITE, WT_EXECUTEDEFAULT)) g_wait = nullptr;
}

// When the window's message loop ends, so a new launcher can start right away.
void release() {
    if (g_wait) { UnregisterWaitEx(g_wait, INVALID_HANDLE_VALUE); g_wait = nullptr; }
    if (g_show) { CloseHandle(g_show); g_show = nullptr; }
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
}
} // namespace single
UINT g_dpi = 96;

double msSinceStart() {
    FILETIME c, e, k, u, now;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER a, b; a.LowPart = c.dwLowDateTime; a.HighPart = c.dwHighDateTime; b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return (double)(b.QuadPart - a.QuadPart) / 10000.0;
}
double cpuMs() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULARGE_INTEGER a, b; a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime; b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime;
    return (double)(a.QuadPart + b.QuadPart) / 10000.0;
}
size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX m = { sizeof m };
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m, sizeof m);
    return m.PrivateUsage;
}

void discardTarget() {
    if (g_app) g_app->setTarget(nullptr, (float)g_dpi);
    release(g_rt);
}

void paint(HWND h) {
    PAINTSTRUCT ps;
    BeginPaint(h, &ps);
    RECT r; GetClientRect(h, &r);
    if (!g_rt) {
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(g_software ? D2D1_RENDER_TARGET_TYPE_SOFTWARE : D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), (float)g_dpi, (float)g_dpi);
        g_client = r;
        // GPU path: RETAIN_CONTENTS, so partial repaints (transitions, spinner) draw over the previous frame (the software
        // target keeps its bitmap anyway; the selftest checks both against a full repaint)
        g_createHr = g_gfx.d2d->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(h, D2D1::SizeU(r.right, r.bottom),
            g_software ? D2D1_PRESENT_OPTIONS_NONE : D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS), &g_rt);
        if (SUCCEEDED(g_createHr)) g_app->setTarget(g_rt, (float)g_dpi);
        if (!g_tTarget) g_tTarget = msSinceStart();
    }
    if (g_rt) {
        float s = g_dpi / 96.f;
        double t = msSinceStart();
        g_rt->BeginDraw();
        // partial updates (the spinner) only rasterize their own rect
        bool part = ps.rcPaint.right - ps.rcPaint.left < r.right || ps.rcPaint.bottom - ps.rcPaint.top < r.bottom;
        g_dirtyMax = std::max(g_dirtyMax, (double)(ps.rcPaint.right - ps.rcPaint.left) * (ps.rcPaint.bottom - ps.rcPaint.top));
        if (part) g_rt->PushAxisAlignedClip(D2D1::RectF(ps.rcPaint.left / s, ps.rcPaint.top / s, ps.rcPaint.right / s, ps.rcPaint.bottom / s), D2D1_ANTIALIAS_MODE_ALIASED);
        g_app->paint(r.right / s, r.bottom / s);
        if (part) g_rt->PopAxisAlignedClip();
        g_lastHr = g_rt->EndDraw();
        if (g_lastHr == D2DERR_RECREATE_TARGET) { discardTarget(); InvalidateRect(h, nullptr, FALSE); }
        else if (SUCCEEDED(g_lastHr)) { g_paints++; g_lastFrameMs = msSinceStart() - t; if (g_firstFrameMs < 0) g_firstFrameMs = msSinceStart(); }
    }
    EndPaint(h, &ps);
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    float s = g_dpi / 96.f;
    switch (m) {
    case WM_NCCALCSIZE:
        if (w) {
            // no system non-client area; when maximized Windows puts the window beyond the monitor by the frame size
            if (IsZoomed(h)) {
                auto* p = (NCCALCSIZE_PARAMS*)l;
                int fx = GetSystemMetricsForDpi(SM_CXFRAME, g_dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g_dpi);
                int fy = GetSystemMetricsForDpi(SM_CYFRAME, g_dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g_dpi);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx; p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
        ScreenToClient(h, &pt);
        RECT r; GetClientRect(h, &r);
        if (!IsZoomed(h)) {
            int b = MulDiv(5, g_dpi, 96), corner = b * 3;
            bool L = pt.x < b, R = pt.x >= r.right - b, T = pt.y < b, B = pt.y >= r.bottom - b;
            bool lc = pt.x < corner, rc = pt.x >= r.right - corner, tc = pt.y < corner, bc = pt.y >= r.bottom - corner;
            if ((T && lc) || (L && tc)) return HTTOPLEFT;
            if ((T && rc) || (R && tc)) return HTTOPRIGHT;
            if ((B && lc) || (L && bc)) return HTBOTTOMLEFT;
            if ((B && rc) || (R && bc)) return HTBOTTOMRIGHT;
            if (L) return HTLEFT; if (R) return HTRIGHT; if (T) return HTTOP; if (B) return HTBOTTOM;
        }
        return g_app->ncHit(pt.x / s, pt.y / s);
    }
    case WM_NCACTIVATE: return DefWindowProcW(h, m, w, -1);   // nothing to repaint in the (empty) frame
    case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: g_app->openMenu(0); break;   // a press on the title bar closes the menus
    case WM_ACTIVATE: g_app->setActive(LOWORD(w) != WA_INACTIVE); return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize.x = MulDiv(760, g_dpi, 96); mm->ptMinTrackSize.y = MulDiv(520, g_dpi, 96);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_rt) { g_rt->SetDpi((float)g_dpi, (float)g_dpi); g_app->setTarget(g_rt, (float)g_dpi); }
        g_app->setDpi((float)g_dpi);
        return 0;
    }
    case WM_SIZE:
        if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        g_app->setMaximized(w == SIZE_MAXIMIZED);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(h); return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        g_app->mouseMove((short)LOWORD(l) / s, (short)HIWORD(l) / s);
        return 0;
    }
    case WM_MOUSELEAVE: g_app->mouseLeave(); return 0;
    case WM_LBUTTONDOWN: g_app->mouseDown((short)LOWORD(l) / s, (short)HIWORD(l) / s); return 0;
    case WM_LBUTTONUP: g_app->mouseUp((short)LOWORD(l) / s, (short)HIWORD(l) / s); return 0;
    case WM_MOUSEWHEEL: {
        POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
        ScreenToClient(h, &pt);
        g_app->wheel(pt.x / s, pt.y / s, (float)GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && (HWND)w == h) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            if (g_app->cursor(pt.x / s, pt.y / s)) return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (g_app->key((UINT)w, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0)) return 0;
        break;
    case WM_CHAR: return 0;
    case WM_TIMER:
        g_timers++;
        if (w == 99) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
        g_app->timer(w);
        return 0;
    case WM_COMMAND:
        if (l) g_app->editCommand(LOWORD(w), HIWORD(w));
        return 0;
    case WM_CTLCOLOREDIT: return (LRESULT)g_app->editBrush((HWND)l, (HDC)w);
    case WM_SETTINGCHANGE:   // Windows "Show animations" (prefers-reduced-motion for the page)
        if (w == SPI_SETCLIENTAREAANIMATION && !g_selftest) { BOOL on = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0); g_app->setMotion(on != FALSE); }
        break;
    case WM_APP_SINK: g_app->drain(); return 0;
    case WM_APP_IMG: g_app->imagesArrived(); return 0;
    case WM_APP_VIDEO: g_app->videoEvent(w, l); return 0;
    case WM_APP_EDITKEY: g_app->editKey((int)l, (UINT)(w & 0xFFFF), (w & 0x20000) != 0, (w & 0x10000) != 0); return 0;
    case WM_APP_SHOW:
        g_showRequests++;
        if (g_selftest) return 0;   // the selftest window stays hidden and never takes the foreground
        if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOW);
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);   // back to normal or maximized, as it was
        SetForegroundWindow(h);
        return 0;
    case WM_CLOSE:
        if (!g_selftest && g_app->sessionRunning() &&
            MessageBoxW(h, L"Fortnite is running. Closing Radarnite Launcher stops the game and the local servers.", L"Radarnite Launcher",
                        MB_OKCANCEL | MB_ICONWARNING) != IDOK) return 0;
        if (!g_selftest && g_app->importing() &&
            MessageBoxW(h, L"Cosmetics are being imported. Closing stops the import (importing again continues where it stopped).", L"Radarnite Launcher",
                        MB_OKCANCEL | MB_ICONWARNING) != IDOK) return 0;
        if (!g_snapshot.empty()) snapshot(h, g_snapshot);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND createWindow(HINSTANCE inst, bool hidden, int wDip, int hDip) {
    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"RadarniteLauncher3";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);
    DWORD ex = hidden ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0;
    HWND h = CreateWindowExW(ex, wc.lpszClassName, L"Radarnite Launcher", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, inst, nullptr);
    if (!h) return nullptr;
    // selftest: a normal on-screen window (so Windows paints it for real) that DWM never shows (cloaked)
    if (hidden) { BOOL cloak = TRUE; DwmSetWindowAttribute(h, 13 /*DWMWA_CLOAK*/, &cloak, sizeof cloak); }
    g_dpi = GetDpiForWindow(h);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    MARGINS mg = { 0, 0, 1, 0 };
    DwmExtendFrameIntoClientArea(h, &mg);   // keeps the DWM shadow
    int w = MulDiv(wDip, g_dpi, 96), hh = MulDiv(hDip, g_dpi, 96);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    if (!hidden) { w = std::min(w, (int)(wa.right - wa.left - 40)); hh = std::min(hh, (int)(wa.bottom - wa.top - 40)); }
    int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - hh) / 2;
    SetWindowPos(h, nullptr, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return h;
}

// ------------------------------------------------------------------------------------------------ selftest
bool savePng(IWICBitmapSource* bmp, const std::wstring& path) {
    IWICStream* s = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    bool ok = SUCCEEDED(g_gfx.wic->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(g_gfx.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(s, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr));
    UINT w = 0, h = 0;
    if (ok) { bmp->GetSize(&w, &h); WICPixelFormatGUID f = GUID_WICPixelFormat32bppBGRA; ok = SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&f)); }
    ok = ok && SUCCEEDED(fr->WriteSource(bmp, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    release(fr); release(enc); release(s);
    return ok;
}

// The live window as Windows composes it (Direct2D surface + the native EDIT controls), BGRA rows.
bool grabWindow(HWND h, std::vector<BYTE>& px, int* w, int* hh) {
    DwmFlush();
    RECT r; GetClientRect(h, &r);
    *w = r.right; *hh = r.bottom;
    BITMAPINFO bi = {}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), *w, -*hh, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr), dc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    BOOL ok = PrintWindow(h, dc, PW_CLIENTONLY | 2 /*PW_RENDERFULLCONTENT*/);
    SelectObject(dc, old);
    if (ok) {
        px.assign((BYTE*)bits, (BYTE*)bits + (size_t)*w * *hh * 4);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    }
    DeleteObject(bmp); DeleteDC(dc); ReleaseDC(nullptr, sdc);
    return ok != FALSE;
}

// The window's current frame drawn by App::paint into a WIC bitmap (client size, window DPI) and saved as PNG.
bool snapshot(HWND h, const std::wstring& path) {
    RECT r; GetClientRect(h, &r);
    IWICBitmap* bmp = nullptr; ID2D1RenderTarget* rt = nullptr;
    bool ok = r.right > 0 && r.bottom > 0 &&
              SUCCEEDED(g_gfx.wic->CreateBitmap(r.right, r.bottom, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
              SUCCEEDED(g_gfx.d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)g_dpi, (float)g_dpi), &rt));
    if (ok) {
        float s = g_dpi / 96.f;
        g_app->setTarget(rt, (float)g_dpi);
        rt->BeginDraw(); g_app->paint(r.right / s, r.bottom / s); ok = SUCCEEDED(rt->EndDraw());
        g_app->setTarget(g_rt, (float)g_dpi);   // back to the window's own target
        ok = ok && savePng(bmp, path);
    }
    release(rt); release(bmp);
    return ok;
}

bool captureWindow(HWND h, const std::wstring& path) {
    std::vector<BYTE> px; int w, hh;
    if (!grabWindow(h, px, &w, &hh)) return false;
    IWICBitmap* wb = nullptr; bool saved = false;
    if (SUCCEEDED(g_gfx.wic->CreateBitmapFromMemory(w, hh, GUID_WICPixelFormat32bppBGRA, w * 4, (UINT)px.size(), px.data(), &wb))) { saved = savePng(wb, path); wb->Release(); }
    return saved;
}

std::string json(const std::wstring& s) {
    std::string o;
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; }
        else if (c < 0x80) o += (char)c;
        else { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; }
    }
    return o;
}

int selftest(HINSTANCE inst, std::wstring dir) {
    g_selftest = true;
    CreateDirectoryW(dir.c_str(), nullptr);
    // 1) the real window, invisible (cloaked tool window, never activated), scenario 01: first frame, then rest until 10 s
    Core* core = createMockCore(L"01-one-build-practice-100");
    g_app = new App(g_gfx, core);
    HWND h = createWindow(inst, true, 1200, 800);
    g_app->attach(h);
    bool systemMotion = g_app->motion();
    g_app->setMotion(true);   // the transitions run whatever "Show animations" says, so the checks below measure them
    g_app->start();
    g_app->drain();
    g_tWindow = msSinceStart();
    ShowWindow(h, SW_SHOWNOACTIVATE);
    UpdateWindow(h);
    // no desktop (ssh runs in session 0): Windows sends no WM_PAINT, so draw the first frame through the same path
    bool forced = g_firstFrameMs < 0;
    if (forced) paint(h);
    double first = g_firstFrameMs;
    DWORD session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &session);
    // rest: from first frame + 1 s to 10 s after the start, nothing should run (no timers, no repaints)
    MSG msg;
    auto pump = [&](double untilMs) {
        while (msSinceStart() < untilMs) {
            DWORD left = (DWORD)std::max(1.0, untilMs - msSinceStart());
            MsgWaitForMultipleObjects(0, nullptr, FALSE, left, QS_ALLINPUT);
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
    };
    pump(std::max(first, 0.0) + 1000);
    // the Launch button greets ~2.5 s after the first state and the season art arrives (downloaded once, then from the
    // cache): the rest starts when nothing moves any more
    double settleLimit = msSinceStart() + 8000;
    while ((g_app->animating() || g_app->artPending()) && msSinceStart() < settleLimit) pump(msSinceStart() + 20);
    double settled = msSinceStart();
    bool settledOk = !g_app->animating() && !g_app->artPending();
    double cpu0 = cpuMs(), t0 = msSinceStart(); int paints0 = g_paints, timers0 = g_timers;
    pump(10000);
    double cpu1 = cpuMs(), t1 = msSinceStart(); int idlePaints = g_paints - paints0, idleTimers = g_timers - timers0;
    size_t priv = privateBytes();
    PROCESS_MEMORY_COUNTERS_EX pm = { sizeof pm };
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);

    // transitions in the live window (needs a desktop: session 0 gets no WM_PAINT)
    std::string checks;
    int failed = 0;
    auto addCheck = [&](const std::string& name, bool ok) {
        char line[400];
        snprintf(line, sizeof line, "%s\n    {\"check\": \"%s\", \"ok\": %s}", checks.empty() ? "" : ",", name.c_str(), ok ? "true" : "false");
        checks += line;
        if (!ok) failed++;
    };
    int hoverIn = 0, hoverOut = 0, afterPaints = 0, afterTimers = 0; double dirtyPct = 0; long long partialDiff = -1;
    if (!forced) {
        char buf[200];
        snprintf(buf, sizeof buf, "the greet sheen ends and the animation timer stops (%.0f ms after the start)", settled);
        addCheck(buf, settledOk);
        D2D1_RECT_F ar = g_app->actButtonRect();
        float hx = (ar.left + ar.right) / 2, hy = (ar.top + ar.bottom) / 2;
        RECT cr; GetClientRect(h, &cr);
        int p0 = g_paints; g_dirtyMax = 0;
        g_app->mouseMove(hx, hy); pump(msSinceStart() + 300);
        hoverIn = g_paints - p0; p0 = g_paints;
        g_app->mouseLeave(); pump(msSinceStart() + 300);
        hoverOut = g_paints - p0;
        dirtyPct = g_dirtyMax * 100.0 / std::max(1.0, (double)cr.right * cr.bottom);
        int p1 = g_paints, k1 = g_timers;
        pump(msSinceStart() + 500);
        afterPaints = g_paints - p1; afterTimers = g_timers - k1;
        snprintf(buf, sizeof buf, "hover fades the Activity button in and out (%d + %d frames)", hoverIn, hoverOut);
        addCheck(buf, hoverIn >= 3 && hoverOut >= 3);
        snprintf(buf, sizeof buf, "transition frames repaint only the control (largest %.2f %% of the window)", dirtyPct);
        addCheck(buf, dirtyPct > 0 && dirtyPct < 2);
        snprintf(buf, sizeof buf, "nothing runs once it settles (%d paints, %d timers in 500 ms)", afterPaints, afterTimers);
        addCheck(buf, afterPaints == 0 && afterTimers == 0);
        // what the partial repaints left on screen = a full repaint of the same state
        std::vector<BYTE> a, b; int aw = 0, ah = 0, bw = 0, bh = 0;
        bool ga = grabWindow(h, a, &aw, &ah);
        InvalidateRect(h, nullptr, FALSE); UpdateWindow(h);
        bool gb = grabWindow(h, b, &bw, &bh);
        if (ga && gb && aw == bw && ah == bh) { partialDiff = 0; for (size_t i = 0; i < a.size(); i += 4) if (memcmp(&a[i], &b[i], 3)) partialDiff++; }
        snprintf(buf, sizeof buf, "partial repaints leave the same pixels as a full repaint (%lld px differ)", partialDiff);
        addCheck(buf, partialDiff == 0);
        for (auto& r : g_app->liveCheck()) addCheck(json(r.first), r.second);
    } else addCheck("transition checks skipped: no desktop (session 0)", true);
    // single instance across processes, on names of its own: a second launch signals this window and steps aside;
    // once the window lets go, a new launch owns it
    {
        std::wstring suffix = L".selftest-" + std::to_wstring(GetCurrentProcessId());
        auto probe = [&]() -> long {
            wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --selftest-single-instance " + suffix;
            STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi = {};
            if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
            DWORD code = (DWORD)-2;
            if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
            else TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            return (long)code;
        };
        bool owner = single::claim(suffix);
        single::listen(h, suffix);
        int shows = g_showRequests;
        long second = probe();
        pump(msSinceStart() + 300);
        bool signalledOk = g_showRequests == shows + 1;
        single::release();
        long after = probe();
        char buf[200];
        snprintf(buf, sizeof buf, "single instance: a second launch signals the open window and exits (%ld), then a new one owns it (%ld)", second, after);
        addCheck(buf, owner && second == 10 && signalledOk && after == 0);
    }
    BOOL dark = FALSE;
    HRESULT darkHr = DwmGetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    bool live = captureWindow(h, dir + L"\\00-live-window-01.png");
    DestroyWindow(h);
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    delete g_app; g_app = nullptr; delete core;
    discardTarget();

    // 1b) the cosmetics panel in a live window: memory with the panel open and its images loaded, after scrolling
    //     through ~1,000 cards, and after closing it (before the offscreen renders: their memory would count too)
    double portOpenMs = -1, portImagesMs = -1;
    double portVideoMs = -1;
    size_t portClipProc = 0, portPrivPreClip = 0;
    size_t portPrivVideo = 0, portPrivVideoClosed = 0;
    size_t portPriv0 = 0, portPrivOpen = 0, portPrivScrolled = 0, portPrivClosed = 0, portImgBytes = 0, portImgMax = 0, portImgCount = 0;
    {
        Core* c = createMockCore(L"28-port-catalog-125");
        g_app = new App(g_gfx, c);
        HWND ph = createWindow(inst, true, 1200, 800);
        g_app->attach(ph);
        g_app->start(); g_app->drain();
        ShowWindow(ph, SW_SHOWNOACTIVATE);
        UpdateWindow(ph);
        if (!g_rt) paint(ph);
        pump(msSinceStart() + 300);
        portPriv0 = privateBytes();
        double t = msSinceStart();
        g_app->portScene(1);
        if (forced) paint(ph); else UpdateWindow(ph);
        portOpenMs = msSinceStart() - t;
        for (double until = msSinceStart() + 15000; msSinceStart() < until; ) {
            pump(msSinceStart() + 50);
            if (forced) paint(ph);
            if (g_app->images().idle()) break;
        }
        portImagesMs = msSinceStart() - t;
        portPrivOpen = privateBytes();
        captureWindow(ph, dir + L"\\00-live-port-panel.png");
        portImgBytes = g_app->images().bytes(); portImgCount = g_app->images().count();
        RECT cr; GetClientRect(ph, &cr);
        float s = g_dpi / 96.f;
        for (int k = 0; k < 40; k++) {
            g_app->wheel(cr.right / s / 2, cr.bottom / s / 2, -120 * 10);
            if (forced) paint(ph); else UpdateWindow(ph);
            pump(msSinceStart() + 120);
            portImgMax = std::max(portImgMax, g_app->images().bytes());
        }
        for (double until = msSinceStart() + 5000; msSinceStart() < until && !g_app->images().idle(); ) pump(msSinceStart() + 50);
        portPrivScrolled = privateBytes();
        // closed: the decoded images go back (the image workers exit after 3 s idle)
        g_app->key(VK_ESCAPE, false, false);
        if (forced) paint(ph); else UpdateWindow(ph);
        pump(msSinceStart() + 4000);
        portPrivClosed = privateBytes();
        int workers = g_app->images().threads();
        // opened again, then the first card's preview plays its clip in a process of its own: this process before,
        // with and after the clip, the panel open all along
        g_app->portScene(1);
        if (forced) paint(ph); else UpdateWindow(ph);
        for (double until = msSinceStart() + 5000; msSinceStart() < until; ) { pump(msSinceStart() + 50); if (forced) paint(ph); if (g_app->images().idle()) break; }
        pump(msSinceStart() + 500);
        portPrivPreClip = privateBytes();
        g_app->portScene(7);
        if (forced) paint(ph); else UpdateWindow(ph);
        double tv = msSinceStart();
        while (msSinceStart() < tv + 15000 && !g_app->videoShown()) { pump(msSinceStart() + 50); if (forced) paint(ph); }
        portVideoMs = g_app->videoShown() ? msSinceStart() - tv : -1;
        pump(msSinceStart() + 1500);
        portPrivVideo = privateBytes();
        portClipProc = g_app->clipBytes();
        captureWindow(ph, dir + L"\\00-live-port-preview-video.png");
        // the player's messages: Space pauses the clip and its own status says so
        g_app->key(VK_SPACE, false, false);
        for (double until = msSinceStart() + 3000; msSinceStart() < until && !(g_app->clipStatus() && g_app->clipPaused()); ) pump(msSinceStart() + 50);
        bool clipPaused = g_app->clipStatus() && g_app->clipPaused();
        g_app->key(VK_ESCAPE, false, false);
        if (forced) paint(ph); else UpdateWindow(ph);
        bool videoGone = !g_app->videoShown() && !FindWindowExW(ph, nullptr, L"RnClip", nullptr);
        pump(msSinceStart() + 1500);
        portPrivVideoClosed = privateBytes();
        g_app->key(VK_ESCAPE, false, false);
        if (forced) paint(ph); else UpdateWindow(ph);
        pump(msSinceStart() + 4000);
        char buf[300];
        snprintf(buf, sizeof buf, "cosmetics panel: images within their budget while scrolling (%.1f MB at most, %zu held when opened)", portImgMax / 1048576.0, portImgCount);
        addCheck(buf, portImgMax <= (12u << 20) + (2u << 20));
        snprintf(buf, sizeof buf, "the preview plays the item's clip, looped (silent in tests), first frame after %.0f ms, and closing it removes it", portVideoMs);
        addCheck(buf, (forced || portVideoMs >= 0) && videoGone);
        addCheck("Space pauses the clip: its process answers paused", forced || portVideoMs < 0 || clipPaused);
        snprintf(buf, sizeof buf, "cosmetics panel: the image workers exit once nothing loads (%d running)", workers);
        addCheck(buf, workers == 0);
        DestroyWindow(ph);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        delete c; delete g_app; g_app = nullptr;
        discardTarget();
    }

    // 2) every scenario rendered offscreen (WIC bitmap, same drawing code, texts of the EDIT fields drawn by Direct2D)
    int n; const MockScenario* sc = mockScenarios(&n);
    std::string items;
    for (int i = 0; i < n; i++) {
        double ts = msSinceStart();
        Core* c = createMockCore(sc[i].id);
        App* app = new App(g_gfx, c);
        app->start(); app->drain();
        if (sc[i].activityOpen) app->openActivity(true);
        if (sc[i].menu) app->openMenu(sc[i].menu);
        if (sc[i].port) app->portScene(sc[i].port);
        UINT pw = (UINT)lroundf(sc[i].width * sc[i].scale), ph = (UINT)lroundf(sc[i].height * sc[i].scale);
        IWICBitmap* bmp = nullptr; ID2D1RenderTarget* rt = nullptr;
        bool ok = SUCCEEDED(g_gfx.wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
                  SUCCEEDED(g_gfx.d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * sc[i].scale, 96.f * sc[i].scale), &rt));
        if (ok) {
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            app->setTarget(rt, 96.f * sc[i].scale);
            for (int pass = 0; pass < 3; pass++) {
                rt->BeginDraw(); app->paint((float)sc[i].width, (float)sc[i].height); rt->EndDraw();
                if (pass == 0) for (int t = 0; t < sc[i].tabs; t++) app->key(VK_TAB, false, false);   // keyboard focus scenarios
                if (pass == 0 && sc[i].port) app->portSceneFramed(sc[i].port);   // what needs the laid-out panel
                if (pass == 0 && sc[i].clickActClose) {
                    D2D1_RECT_F xr = app->actCloseRect();
                    float cx = (xr.left + xr.right) / 2, cy = (xr.top + xr.bottom) / 2;
                    app->mouseDown(cx, cy); app->mouseUp(cx, cy); app->mouseLeave();
                    if (app->activityOpen()) app->issues.push_back(L"the click on Activity's X left the panel open");
                }
            }
            // the panel's images: drawn again until the visible ones are decoded (downloaded the first time), 15 s at most
            for (double until = msSinceStart() + 15000; sc[i].port && msSinceStart() < until; ) {
                Sleep(60);
                rt->BeginDraw(); app->paint((float)sc[i].width, (float)sc[i].height); rt->EndDraw();
                if (app->images().idle()) { rt->BeginDraw(); app->paint((float)sc[i].width, (float)sc[i].height); rt->EndDraw(); break; }
            }
            app->setTarget(nullptr, 96.f);
            ok = savePng(bmp, dir + L"\\" + sc[i].id + L".png");
        }
        release(rt); release(bmp);
        std::string iss;
        for (auto& s : app->issues) { if (!iss.empty()) iss += ","; iss += "\"" + json(s) + "\""; }
        char line[512];
        snprintf(line, sizeof line, "%s\n    {\"id\": \"%s\", \"viewport\": \"%dx%d@%gx\", \"ok\": %s, \"ms\": %.1f, \"issues\": [%s]}",
                 items.empty() ? "" : ",", json(sc[i].id).c_str(), sc[i].width, sc[i].height, sc[i].scale, ok ? "true" : "false", msSinceStart() - ts, iss.c_str());
        items += line;
        if (!ok) failed++;
        delete app; delete c;
    }

    // 3) interactions on an offscreen window state (scenario 20: the bots form)
    {
        Core* c = createMockCore(L"20-match-server-bots-form");
        App* app = new App(g_gfx, c);
        app->start(); app->drain();
        IWICBitmap* bmp = nullptr; ID2D1RenderTarget* rt = nullptr;
        if (SUCCEEDED(g_gfx.wic->CreateBitmap(1200, 800, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
            SUCCEEDED(g_gfx.d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(), &rt))) {
            app->setTarget(rt, 96.f);
            for (auto& r : app->selfCheck(1200, 800)) addCheck(json(r.first), r.second);
            app->setTarget(nullptr, 96.f);
        }
        release(rt); release(bmp);
        delete c; delete app;   // the core first: its timers (launch) never reach a deleted window state
    }
    {
        Core* c = createMockCore(L"28-port-catalog-125");
        App* app = new App(g_gfx, c);
        app->start(); app->drain();
        IWICBitmap* bmp = nullptr; ID2D1RenderTarget* rt = nullptr;
        if (SUCCEEDED(g_gfx.wic->CreateBitmap(1200, 800, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
            SUCCEEDED(g_gfx.d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(), &rt))) {
            app->setTarget(rt, 96.f);
            for (auto& r : app->portCheck(1200, 800)) addCheck(json(r.first), r.second);
            app->setTarget(nullptr, 96.f);
        }
        release(rt); release(bmp);
        delete c; delete app;
    }

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa = {}; GetFileAttributesExW(exe, GetFileExInfoStandard, &fa);
    DPI_AWARENESS aw = GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
    double idleMs = t1 - t0, idleCpu = cpu1 - cpu0;
    FILE* f = _wfopen((dir + L"\\selftest.json").c_str(), L"wb");
    if (!f) return 1;
    fprintf(f, "{\n  \"firstFrameMs\": %.1f,\n  \"firstFrameSteps\": {\"gfx\": %.1f, \"window\": %.1f, \"target\": %.1f},\n  \"privateBytes\": %zu,\n  \"privateMB\": %.1f,\n  \"workingSetMB\": %.1f,\n"
               "  \"idleCpu\": %.3f,\n  \"idleCpuMs\": %.1f,\n  \"idleWindowMs\": %.0f,\n  \"idlePaints\": %d,\n  \"idleTimers\": %d,\n  \"exeBytes\": %lu,\n"
               "  \"dwmDarkFrame\": %s,\n  \"systemMotion\": %s,\n  \"animSettledMs\": %.0f,\n  \"hoverFrames\": [%d, %d],\n  \"hoverDirtyMaxPct\": %.2f,\n  \"afterTransition\": {\"paints\": %d, \"timers\": %d},\n  \"partialVsFullDiffPx\": %lld,\n"
               "  \"port\": {\"openFrameMs\": %.1f, \"imagesMs\": %.0f, \"privateMB\": {\"before\": %.1f, \"open\": %.1f, \"scrolled\": %.1f, \"closed\": %.1f, \"preClip\": %.1f, \"clip\": %.1f, \"afterClip\": %.1f, \"clipProcess\": %.1f}, \"videoFirstFrameMs\": %.0f, \"imagesMB\": %.1f, \"imagesMaxMB\": %.1f, \"images\": %zu},\n"
               "  \"renderer\": \"%s\",\n  \"dpi\": %u,\n  \"dpiAwareness\": %d,\n  \"fonts\": \"%s\",\n  \"gdiFont\": \"%s\",\n  \"liveCapture\": %s,\n  \"firstFrameForced\": %s,\n  \"endDrawHr\": \"0x%08lx\",\n  \"createHr\": \"0x%08lx\",\n  \"client\": \"%ldx%ld\",\n  \"frameMs\": %.1f,\n  \"session\": %lu,\n  \"failed\": %d,\n  \"scenarios\": [%s\n  ],\n  \"interactions\": [%s\n  ]\n}\n",
            first, g_tGfx, g_tWindow, g_tTarget, priv, priv / 1048576.0, pm.WorkingSetSize / 1048576.0, idleMs > 0 ? idleCpu / idleMs * 100.0 : -1.0, idleCpu, idleMs, idlePaints, idleTimers,
            fa.nFileSizeLow, SUCCEEDED(darkHr) ? (dark ? "true" : "false") : "null", systemMotion ? "true" : "false", settled, hoverIn, hoverOut, dirtyPct, afterPaints, afterTimers, partialDiff,
            portOpenMs, portImagesMs, portPriv0 / 1048576.0, portPrivOpen / 1048576.0, portPrivScrolled / 1048576.0, portPrivClosed / 1048576.0, portPrivPreClip / 1048576.0, portPrivVideo / 1048576.0, portPrivVideoClosed / 1048576.0, portClipProc / 1048576.0, portVideoMs, portImgBytes / 1048576.0, portImgMax / 1048576.0, portImgCount, g_software ? "d2d-software" : "d2d-hardware", g_dpi, (int)aw, json(g_gfx.families).c_str(), json(g_gfx.gdiFamily).c_str(),
            live ? "true" : "false", forced ? "true" : "false", (unsigned long)g_lastHr, (unsigned long)g_createHr, g_client.right, g_client.bottom, g_lastFrameMs, session, failed, items.c_str(), checks.c_str());
    fclose(f);
    return failed ? 1 : 0;
}

} // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // no-op when the manifest set it
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // the preview's clip, in a process of its own (video.h)
    if (argc >= 2 && !wcscmp(argv[1], L"--clip")) { int rc = rn::clipMain(argc, argv); LocalFree(argv); CoUninitialize(); return rc; }
    {
        // console verbs of 2.x (--check, --scan, --firewall, --set-builds-folder, --update-check ...): no window, no mutex
        bool handled = false;
        int rc = rn::cli::run(argc, argv, &handled);
        if (handled) { LocalFree(argv); CoUninitialize(); return rc; }
    }
    std::wstring scenario, selftestDir, probeSuffix, autoPlay;
    bool selftestMode = false, newInstance = false, probe = false, quietStart = false, automated = false;
    int exitAfter = 0;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--play" && i + 1 < argc) autoPlay = argv[++i];
        else if (a == L"--selftest-ui") { selftestMode = true; if (i + 1 < argc && argv[i + 1][0] != L'-') selftestDir = argv[++i]; }
        else if (a == L"--scenario" && i + 1 < argc) scenario = argv[++i];
        else if (a == L"--render" && i + 1 < argc) g_software = std::wstring(argv[++i]) != L"hw";
        else if (a == L"--exit-after" && i + 1 < argc) exitAfter = _wtoi(argv[++i]);
        else if (a == L"--snapshot" && i + 1 < argc) g_snapshot = argv[++i];
        else if (a == L"--new-instance") newInstance = true;
        else if (a == L"--automated") automated = true;   // a script's or an agent's --play: game guard first (core.h)
        else if (a == L"--updated" && i + 1 < argc) quietStart = std::wstring(argv[++i]) == L"bg";   // relaunched by a silent update
        else if (a == L"--selftest-single-instance" && i + 1 < argc) { probe = true; probeSuffix = argv[++i]; }   // selftest only
    }
    LocalFree(argv);
    // the selftest and the mock scenarios never make a sound: their clip processes inherit this (video.h)
    if (selftestMode || !scenario.empty()) SetEnvironmentVariableW(L"RN_CLIP_SILENT", L"1");
    // selftest probe: a second launch on the selftest's names; 10 = it signalled the open window and stepped aside
    if (probe) { int code = single::claim(probeSuffix) ? 0 : 10; single::release(); CoUninitialize(); return code; }
    // the window: one per session (2.2.1 behaviour), unless --new-instance; --play and --selftest-ui never take the mutex
    if (!selftestMode && !newInstance && autoPlay.empty() && !single::claim(L"")) { CoUninitialize(); return 0; }
    if (!g_gfx.init(inst)) return 3;
    g_tGfx = msSinceStart();

    int rc = 0;
    if (selftestMode) {
        if (selftestDir.empty()) { wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); selftestDir = exe; selftestDir = selftestDir.substr(0, selftestDir.find_last_of(L'\\')) + L"\\selftest-ui"; }
        rc = selftest(inst, selftestDir);
    } else {
        // the real core; the mock only for a --scenario (it, and --selftest-ui, show made-up builds)
        Core* core = !scenario.empty() ? createMockCore(scenario.c_str()) : rn::createCore(autoPlay.empty() ? nullptr : autoPlay.c_str(),
                                                                                          automated || rn::core::AutomatedStart());
        g_app = new App(g_gfx, core);
        HWND h = createWindow(inst, false, 1180, 740);
        single::listen(h, L"");
        g_app->attach(h);
        g_app->start();
        // after a test build's silent update made while the window was not in use: back in the taskbar, no focus taken
        ShowWindow(h, quietStart ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL);
        UpdateWindow(h);
        if (exitAfter > 0) SetTimer(h, 99, exitAfter, nullptr);
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
        delete core;   // stops the core's session (or the mock's timers) before the window state goes away
        if (scenario.empty() && rn::core::ExitCode()) rc = rn::core::ExitCode();
        delete g_app; g_app = nullptr;
        discardTarget();
        single::release();
    }
    g_gfx.shutdown();
    CoUninitialize();
    return rc;
}
