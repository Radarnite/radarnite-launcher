#include "ui.h"
#include "port_ui.h"
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <algorithm>

namespace rn {
namespace {

enum : int {
    ID_MIN = 1, ID_MAX, ID_CLOSE, ID_ADD, ID_ROOT, ID_BUILDMENU, ID_EMPTY_ZIP, ID_EMPTY_FOLDER, ID_EMPTY_ROOT,
    ID_NOTICE_FW, ID_NOTICE_COS, ID_NAME, ID_STEP_DN, ID_BOTS, ID_STEP_UP, ID_RANGE, ID_TG0, ID_TG1, ID_TG2, ID_TG3,
    ID_GRIP, ID_F0, ID_F1, ID_F2, ID_SEARCH, ID_COPY, ID_OPENLOGS, ID_CLEAR, ID_LINES, ID_ACT, ID_PLAY,
    ID_SB_CONTENT, ID_SB_LOG, ID_POPUP, ID_TOAST, ID_META, ID_COSMETICS, ID_MODE0, ID_MODE1, ID_CELL0, ID_PREV = ID_CELL0 + 4, ID_NEXT, ID_SHEET, ID_SHEETX, ID_MENU0 = 200, ID_BUILD0 = 1000,
};
enum { F_FOCUS = 1, F_HAND = 2, F_EDIT = 4, F_DISABLED = 8, F_RING_IN = 16, F_CONTENT = 32 };
const UINT_PTR T_SPIN = 1, T_TOAST = 2, T_ANIM = 3;
// animation keys: kind | control id
const uint32_t K_HOVER = 1u << 16, K_COLOR = 2u << 16, K_PRESS = 3u << 16, K_SWITCH = 4u << 16, K_TOAST = 5u << 16,
               K_SHEEN = 6u << 16, K_GREET = 7u << 16, K_SHIMMER = 8u << 16, K_SHEET = 9u << 16;
const float TB = 40;
// lobby screen colors (the mockup's): glass fill, the opaque fields under the EDITs, the ready green, dark text
const uint32_t GLASS = 0x090D1C, FIELD = 0x0B0E19, GREEN = 0x3DFF6E, INK = 0x101114;
const uint32_t FROST = 0x0C1126;   // .frost: the dock's summary and settings sheet, rgba(12,17,38,.55)
const size_t MAXLOG = 3000;
const float LOG_LH = 19.2f;   // 12px * 1.6

struct MenuItem { Icon icon; const wchar_t* text; const wchar_t* small; Command cmd; bool sep; };
const MenuItem kAddMenu[] = {
    { Icon::Zip, L"Add from a downloaded zip…", nullptr, Command::AddZip, false },
    { Icon::Folder, L"Add from a folder…", nullptr, Command::AddFolder, false },
    { Icon::Zip, nullptr, nullptr, Command::Refresh, true },
    { Icon::Refresh, L"Rescan builds", nullptr, Command::Refresh, false },
    { Icon::Refresh, L"Check for updates", nullptr, Command::CheckUpdates, false },
    { Icon::Zip, nullptr, nullptr, Command::Refresh, true },
    { Icon::Folder, L"Change builds folder…", nullptr, Command::ChooseRoot, false },
};
const MenuItem kBuildMenu[] = {
    { Icon::Refresh, L"Regenerate cosmetics", nullptr, Command::Cosmetics, false },
    { Icon::Fw, L"Recreate firewall rules", L"admin", Command::Firewall, false },
    { Icon::Link, L"Desktop shortcut", nullptr, Command::Shortcut, false },
    { Icon::Folder, L"Open build folder", nullptr, Command::OpenLogs, false },
    { Icon::Zip, nullptr, nullptr, Command::Refresh, true },
    { Icon::Cog, L"Open config.json", nullptr, Command::OpenConfig, false },
};
// The Cosmetics button's menu (the cosmetics panel replaces it)
const MenuItem kCosMenu[] = {
    { Icon::Refresh, L"Regenerate cosmetics", nullptr, Command::Cosmetics, false },
};
const MenuItem* menuItems(int menu, int* n) {
    if (menu == 1) { *n = (int)(sizeof(kAddMenu) / sizeof(kAddMenu[0])); return kAddMenu; }
    if (menu == 3) { *n = (int)(sizeof(kCosMenu) / sizeof(kCosMenu[0])); return kCosMenu; }
    *n = (int)(sizeof(kBuildMenu) / sizeof(kBuildMenu[0])); return kBuildMenu;
}

std::wstring fmt(const wchar_t* f, ...) {
    wchar_t buf[1024];
    va_list a; va_start(a, f); vswprintf(buf, 1024, f, a); va_end(a);
    return buf;
}
std::wstring season(int major, int minor) {
    if (!major) return L"Unknown version";
    if (major <= 10) return fmt(L"Chapter 1 · Season %d", major == 1 ? (minor < 8 ? 1 : 2) : major);
    if (major <= 18) return fmt(L"Chapter 2 · Season %d", major - 10);
    if (major <= 22) return fmt(L"Chapter 3 · Season %d", major - 18);
    if (major <= 26) return fmt(L"Chapter 4 · Season %d", major - 22);
    if (major == 27) return L"Chapter 4 · Season OG";
    if (major <= 31) return fmt(L"Chapter 5 · Season %d", major - 27);
    return fmt(L"Fortnite %d.x", major);
}
// "C2 · S4" (the tabs)
std::wstring seasonShort(int major, int minor) {
    if (!major) return L"";
    if (major <= 10) return fmt(L"C1 · S%d", major == 1 ? (minor < 8 ? 1 : 2) : major);
    if (major <= 18) return fmt(L"C2 · S%d", major - 10);
    if (major <= 22) return fmt(L"C3 · S%d", major - 18);
    if (major <= 26) return fmt(L"C4 · S%d", major - 22);
    if (major == 27) return L"C4 · OG";
    if (major <= 31) return fmt(L"C5 · S%d", major - 27);
    return fmt(L"%d.x", major);
}
// The version a build is shown as: "3.6", never the raw build string or the folder ("3.6-CL-4019403")
std::wstring verLabel(const Build& b) {
    if (b.major && (b.version.find(L"-CL-") != std::wstring::npos || b.version.empty() || b.version == L"?")) return fmt(L"%d.%d", b.major, b.minor);
    return b.version;
}
std::wstring caps(const std::wstring& s) { std::wstring o = s; for (auto& c : o) c = (wchar_t)towupper(c); return o; }
// The button a menu belongs to (focus goes back to it)
int menuOwner(int menu) { return menu == 1 ? ID_ADD : menu == 2 ? ID_BUILDMENU : menu == 3 ? ID_COSMETICS : 0; }
std::wstring thousands(int n) {
    std::wstring s = std::to_wstring(n), o;
    int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; i--) { o.insert(o.begin(), s[i]); if (++c % 3 == 0 && i > 0) o.insert(o.begin(), L','); }
    return o;
}
std::wstring lower(const std::wstring& s) { std::wstring o = s; for (auto& c : o) c = (wchar_t)towlower(c); return o; }
bool has(const std::wstring& s, const wchar_t* t) { return s.find(t) != std::wstring::npos; }
int classify(const std::wstring& l) {
    for (auto t : { L"not starting", L"error", L"failed", L"could not", L"did not", L"cannot", L"stopped for safety", L"unsupported" }) if (has(l, t)) return 2;
    for (auto t : { L"warning", L"experimental", L"missing", L"was cancelled" }) if (has(l, t)) return 1;
    return 0;
}
// A support value as the page shows it: { css class (0 ok, 1 warn, 2 bad), label }
struct Sup { int cls; const wchar_t* label; };
Sup sup(Support s) { return s == Support::Yes ? Sup{ 0, L"Ready" } : s == Support::Experimental ? Sup{ 1, L"Experimental" } : Sup{ 2, L"Not supported" }; }
uint32_t clsColor(int c) { return c == 0 ? C::ok : c == 1 ? C::warn : c == 2 ? C::bad : C::dim; }
bool inside(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
bool overlaps(D2D1_RECT_F a, D2D1_RECT_F b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }
D2D1_RECT_F inter(D2D1_RECT_F a, D2D1_RECT_F b) { return { std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom) }; }
D2D1_RECT_F rc(float x, float y, float w, float h) { return { x, y, x + w, y + h }; }
D2D1_RECT_F grow(D2D1_RECT_F r, float d) { return r.right > r.left ? D2D1_RECT_F{ r.left - d, r.top - d, r.right + d, r.bottom + d } : r; }   // empty stays empty
bool systemMotion() { BOOL on = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0); return on != FALSE; }

// Greedy wrap of a log message for a monospace column count (white-space: pre-wrap; word-break: break-word).
template <class F> int wrap(const std::wstring& s, int cols, F&& line) {
    if (cols < 1) cols = 1;
    int n = 0;
    size_t i = 0, len = s.size();
    while (true) {
        size_t nl = s.find(L'\n', i);
        size_t end = nl == std::wstring::npos ? len : nl;
        size_t a = i;
        do {
            size_t rest = end - a;
            if (rest <= (size_t)cols) { line(a, rest); n++; a = end; break; }
            size_t brk = std::wstring::npos;
            size_t hy = std::wstring::npos;
            for (size_t k = a + cols; k > a; k--) {
                if (s[k] == L' ') { brk = k; break; }
                // break after a hyphen between letters/digits, not before a number ("rvn=-1"); after ? or ! before a
                // letter ("QueryProfile?|profileId", UAX #14 EX), like Chromium
                if (hy == std::wstring::npos && k <= a + cols && s[k - 1] == L'-' && k >= 2 && iswalnum(s[k - 2]) && !iswdigit(s[k])) hy = k;
                if (hy == std::wstring::npos && k <= a + cols && (s[k - 1] == L'?' || s[k - 1] == L'!') && iswalpha(s[k])) hy = k;
            }
            if (hy != std::wstring::npos && (brk == std::wstring::npos || hy > brk)) { line(a, hy - a); n++; a = hy; }
            else if (brk != std::wstring::npos) { line(a, brk - a); n++; a = brk + 1; }
            else { line(a, (size_t)cols); n++; a += cols; }
        } while (a < end);
        if (nl == std::wstring::npos) break;
        i = nl + 1;
    }
    return n ? n : 1;
}

LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_KEYDOWN) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (w == VK_TAB || w == VK_ESCAPE || w == VK_RETURN || (ctrl && w == 'L') || (id == ID_BOTS && (w == VK_UP || w == VK_DOWN))) {
            PostMessageW(GetParent(h), WM_APP_EDITKEY, w | (shift ? 0x10000 : 0) | (ctrl ? 0x20000 : 0), (LPARAM)id);
            return 0;
        }
    }
    if (m == WM_CHAR && (w == L'\t' || w == L'\r' || w == L'\n' || w == 27 || w == 12)) return 0;
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, editProc, id);
    return DefSubclassProc(h, m, w, l);
}

// An IDWriteTextRenderer that turns a layout's glyph runs into path geometries (the poster's outlined version)
class OutlineRenderer final : public IDWriteTextRenderer {
public:
    struct Run { ID2D1PathGeometry* geo; float x, y; };
    explicit OutlineRenderer(ID2D1Factory* f) : f_(f) {}
    ~OutlineRenderer() { for (auto& r : runs) release(r.geo); }
    std::vector<Run> runs;
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** p) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWritePixelSnapping) || id == __uuidof(IDWriteTextRenderer)) { *p = this; return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* v) override { *v = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override { *m = { 1, 0, 0, 1, 0, 0 }; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* v) override { *v = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* k = nullptr;
        if (SUCCEEDED(f_->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&k))) {
            k->SetFillMode(D2D1_FILL_MODE_WINDING);
            run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets, run->glyphCount, run->isSideways, run->bidiLevel % 2, k);
            k->Close();
            runs.push_back({ g, x, y }); g = nullptr;
        }
        release(k); release(g);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }
private:
    ID2D1Factory* f_;
};

} // namespace

// ================================================================================================ lifetime
App::App(Gfx& g, Core* core) : g_(g), core_(core) {
    InitializeCriticalSection(&cs_);
    anim_.motion = false;   // offscreen renders show the rest state (the page's tests run with reduced motion)
    edits_[0].id = ID_NAME; edits_[0].f = { Face::UI, 13, 600, false }; edits_[0].bg = FIELD;
    edits_[1].id = ID_BOTS; edits_[1].f = { Face::Display, 24, 900, true }; edits_[1].bg = FIELD; edits_[1].lazy = true;
    edits_[2].id = ID_SEARCH; edits_[2].f = { Face::UI, 12, 400, false };
    edits_[3].id = ID_P_SEARCH; edits_[3].f = { Face::UI, 13, 400, false }; edits_[3].bg = FIELD;   // the panel's search: a field on the season art, like the name
    float h = 0;
    std::wstring v = core_->uiPref(L"activityHeight");
    if (!v.empty() && (h = (float)_wtof(v.c_str())) >= 140) actH_ = h;
}

App::~App() {
    setTarget(nullptr, dpi_);
    for (auto& e : edits_) if (e.font) DeleteObject(e.font);
    if (editBrush_) DeleteObject(editBrush_);
    if (fieldBrush_) DeleteObject(fieldBrush_);
    DeleteCriticalSection(&cs_);
}

void App::attach(HWND hwnd) {
    hwnd_ = hwnd;
    anim_.motion = systemMotion();
    editBrush_ = CreateSolidBrush(RGB(0x18, 0x1A, 0x1F));
    fieldBrush_ = CreateSolidBrush(RGB((FIELD >> 16) & 255, (FIELD >> 8) & 255, FIELD & 255));
    art_.startAsync(hwnd, WM_APP_SINK);   // the season art arrives like a core message
    createEdits();
    img_.setNotify(hwnd, WM_APP_IMG);
}

void App::start() { core_->ready(this); }

void App::invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

void App::dirty(D2D1_RECT_F r) {
    if (!hwnd_ || r.right <= r.left || r.bottom <= r.top) return;
    RECT px = { (LONG)floorf(r.left * scale_) - 2, (LONG)floorf(r.top * scale_) - 2, (LONG)ceilf(r.right * scale_) + 2, (LONG)ceilf(r.bottom * scale_) + 2 };
    InvalidateRect(hwnd_, &px, FALSE);
}

void App::animate() {
    if (!hwnd_) return;
    bool run = anim_.running();
    if (run == animTimer_) return;
    animTimer_ = run;
    if (run) SetTimer(hwnd_, T_ANIM, USER_TIMER_MINIMUM, nullptr); else KillTimer(hwnd_, T_ANIM);
}

void App::setMotion(bool on) {
    if (anim_.motion == on) return;
    anim_.clear();
    anim_.motion = on;
    animate();
    invalidate();
}

D2D1_RECT_F App::actButtonRect() const { return rectOf(ID_ACT); }

float App::fx(uint32_t key, float target, D2D1_RECT_F r, float ms, Curve c) {
    if (measuring_) return target;
    r.top += oy_; r.bottom += oy_;
    return anim_.fx(key, target, r, ms, c);
}
uint32_t App::fxc(uint32_t key, uint32_t target, D2D1_RECT_F r, float ms, Curve c) {
    if (measuring_) return target;
    r.top += oy_; r.bottom += oy_;
    return anim_.fxColor(key, target, r, ms, c);
}
float App::tw(uint32_t key, float rest, D2D1_RECT_F r) {
    if (measuring_) return rest;
    r.top += oy_; r.bottom += oy_;
    return anim_.get(key, rest, r);
}

// .play.greet: the sheen crosses the Launch button twice, 0.3 s after the build list loads, a build is clicked or a
// session ends (nothing runs at rest)
void App::greet() {
    anim_.to(K_GREET, -1.2f, 1.2f, 1100, Curve::Ui, grow(rectOf(ID_PLAY), 1), 300, 2);
    animate();
}

void App::setTarget(ID2D1RenderTarget* rt, float dpi) {
    releaseArt();
    release(modeBr_[0]); release(modeBr_[1]); release(rangeBr_);
    release(shimmer_); release(sheen_); release(logoBrush_); release(logo_); release(brush2_); release(brush_);
    releasePortBrushes(); img_.setTarget(rt);
    rt_ = rt;
    dpi_ = dpi; scale_ = dpi / 96.f;
    if (!rt_) return;
    rt_->CreateSolidColorBrush(rgb(C::text), &brush_);
    rt_->CreateSolidColorBrush(rgb(C::text), &brush2_);
    // .play::after: linear-gradient(105deg, transparent 30%, rgba(16,17,20,.10) 46%, rgba(16,17,20,.16) 50%, .10 54%, transparent 70%)
    const D2D1_GRADIENT_STOP sheen[] = { { .30f, rgb(C::bg, 0) }, { .46f, rgb(C::bg, .10f) }, { .50f, rgb(C::bg, .16f) }, { .54f, rgb(C::bg, .10f) }, { .70f, rgb(C::bg, 0) } };
    // .sk: linear-gradient(90deg, bg2 25%, surface 50%, bg2 75%) on an 800px tile repeated across the element
    const D2D1_GRADIENT_STOP sk[] = { { 0, rgb(C::bg2) }, { .25f, rgb(C::bg2) }, { .5f, rgb(C::surface) }, { .75f, rgb(C::bg2) }, { 1, rgb(C::bg2) } };
    ID2D1GradientStopCollection* gs = nullptr;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(sheen, 5, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs))) rt_->CreateLinearGradientBrush({}, gs, &sheen_);
    release(gs);
    if (SUCCEEDED(rt_->CreateGradientStopCollection(sk, 5, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_WRAP, &gs))) rt_->CreateLinearGradientBrush({}, gs, &shimmer_);
    release(gs);
    // .sheet .m cards: linear-gradient(160deg, rgba(88,211,107,.45), rgba(31,138,58,.35)) and (160deg, #b26bff, #6a2bd0);
    // the bots slider: #8c6bff -> #b79bff
    const uint32_t grads[3][2] = { { 0x58D36B, 0x1F8A3A }, { 0xB26BFF, 0x6A2BD0 }, { 0x8C6BFF, 0xB79BFF } };
    const float galpha[3][2] = { { .45f, .35f }, { 1, 1 }, { 1, 1 } };
    for (int i = 0; i < 3; i++) {
        const D2D1_GRADIENT_STOP st[] = { { 0, rgb(grads[i][0], galpha[i][0]) }, { 1, rgb(grads[i][1], galpha[i][1]) } };
        if (SUCCEEDED(rt_->CreateGradientStopCollection(st, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs))) rt_->CreateLinearGradientBrush({}, gs, i < 2 ? &modeBr_[i] : &rangeBr_);
        release(gs);
    }
    if (g_.logo && g_.wic) {
        IWICBitmapScaler* sc = nullptr;
        UINT px = (UINT)lroundf(20 * scale_);
        if (SUCCEEDED(g_.wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(g_.logo, px, px, WICBitmapInterpolationModeFant))) {
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
            if (SUCCEEDED(rt_->CreateBitmapFromWicBitmap(sc, &bp, &logo_))) rt_->CreateBitmapBrush(logo_, &logoBrush_);
        }
        release(sc);
    }
}

void App::setDpi(float dpi) {
    dpi_ = dpi; scale_ = dpi / 96.f;
    if (!hwnd_) return;
    for (auto& e : edits_) {
        if (e.font) DeleteObject(e.font);
        e.font = CreateFontW(-(int)lroundf(e.f.size * scale_), 0, 0, 0, e.f.weight, e.f.italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, (e.f.face == Face::Display ? g_.gdiDisplayFamily : g_.gdiFamily).c_str());
        SendMessageW(e.hwnd, WM_SETFONT, (WPARAM)e.font, TRUE);
        HDC dc = GetDC(e.hwnd); HGDIOBJ old = SelectObject(dc, e.font);
        TEXTMETRICW tm; GetTextMetricsW(dc, &tm); e.textH = tm.tmHeight;
        SelectObject(dc, old); ReleaseDC(e.hwnd, dc);
        e.applied = {}; e.shown = false;
        ShowWindow(e.hwnd, SW_HIDE);
    }
    if (tip_) SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, MulDiv(400, (int)dpi, 96));
    invalidate();
}

void App::createEdits() {
    for (auto& e : edits_) {
        DWORD style = WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | (e.id == ID_BOTS ? ES_CENTER | ES_NUMBER : 0);
        e.hwnd = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)e.id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(e.hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SendMessageW(e.hwnd, EM_LIMITTEXT, e.id == ID_NAME ? 24 : e.id == ID_BOTS ? 2 : 200, 0);
        if (e.id == ID_SEARCH) SendMessageW(e.hwnd, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filter");
        if (e.id == ID_P_SEARCH) SendMessageW(e.hwnd, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search name or ID (EID_\u2026)");
        SetWindowSubclass(e.hwnd, editProc, (UINT_PTR)e.id, 0);
    }
    setDpi((float)GetDpiForWindow(hwnd_));
}

// ================================================================================================ core messages
void App::onState(const State& s) { Event e{}; e.type = 0; e.state = s; EnterCriticalSection(&cs_); queue_.push_back(std::move(e)); LeaveCriticalSection(&cs_); if (hwnd_) PostMessageW(hwnd_, WM_APP_SINK, 0, 0); }
void App::onSession(const Session& s) { Event e{}; e.type = 1; e.session = s; EnterCriticalSection(&cs_); queue_.push_back(std::move(e)); LeaveCriticalSection(&cs_); if (hwnd_) PostMessageW(hwnd_, WM_APP_SINK, 0, 0); }
void App::onLog(const LogLine& l) { Event e{}; e.type = 2; e.log = l; EnterCriticalSection(&cs_); queue_.push_back(std::move(e)); LeaveCriticalSection(&cs_); if (hwnd_) PostMessageW(hwnd_, WM_APP_SINK, 0, 0); }
void App::onToast(const std::wstring& t, Level l) { Event e{}; e.type = 3; e.text = t; e.level = l; EnterCriticalSection(&cs_); queue_.push_back(std::move(e)); LeaveCriticalSection(&cs_); if (hwnd_) PostMessageW(hwnd_, WM_APP_SINK, 0, 0); }

void App::drain() {
    std::vector<Event> q;
    EnterCriticalSection(&cs_); q.swap(queue_); LeaveCriticalSection(&cs_);
    for (auto& e : q) apply(e);
    takeArt();
}

void App::apply(Event& e) {
    switch (e.type) {
    case 0: {
        bool first = !loaded_;
        st_ = std::move(e.state);
        loaded_ = st_.loaded;
        if (loaded_) for (uint32_t i = 0; i < 8; i++) anim_.stop(K_SHIMMER | i);
        if (first && loaded_) greet();
        if (st_.hasSession) se_ = st_.session;
        auto have = [&](const std::wstring& n) { if (n.empty()) return false; for (auto& b : st_.builds) if (b.name == n) return true; return false; };
        if (!have(selected_)) {
            // the core's selection, else the build picked last time, else the newest build
            std::wstring last = core_->uiPref(L"lastBuild");
            selected_ = have(st_.selected) ? st_.selected : have(last) ? last : st_.builds.empty() ? L"" : st_.builds.back().name;
            for (size_t i = 0; i < st_.builds.size(); i++) if (st_.builds[i].name == selected_) tabEnsure_ = (int)i;
        }
        syncEdits();
        updateSpinTimer();
        break;
    }
    case 1: {
        SessionState was = se_.state;
        se_ = e.session;
        if (se_.state == SessionState::Error && was != SessionState::Error) { toast(se_.message.empty() ? L"Launch failed" : se_.message, Level::Bad); openActivity(true); }
        if (se_.state == SessionState::Idle && was != SessionState::Idle) greet();
        updateSpinTimer();
        break;
    }
    case 2: addLog(e.log); break;
    case 3: toast(e.text, e.level); break;
    default: portEvent(e); break;
    }
    invalidate();
}

void App::addLog(const LogLine& l) {
    LogEntry x;
    if (l.ts.empty()) { SYSTEMTIME t; GetLocalTime(&t); x.ts = fmt(L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond); }
    else x.ts = l.ts;
    x.text = l.text; x.lower = lower(l.text);
    x.lvl = l.level == Level::Auto ? classify(x.lower) : l.level == Level::Bad ? 2 : l.level == Level::Warn ? 1 : 0;
    const std::wstring& s = x.lower;
    x.key = !s.rfind(L"launching", 0) || !s.rfind(L"backend ready", 0) || !s.rfind(L"match server ready", 0) ||
            (!s.rfind(L"fortnite ", 0) && s.find(L" started", 9) != std::wstring::npos);
    logs_.push_back(std::move(x));
    if (logs_.size() > MAXLOG) logs_.pop_front();
    rowsDirty_ = true;
    if (!actOpen_) { unseen_++; int lv = logs_.back().lvl; if (lv == 2 || (lv == 1 && unseenLevel_ != 2)) unseenLevel_ = lv; }
    invalidate();
}

void App::toast(const std::wstring& text, Level level) {
    uint32_t seq = ++toastSeq_;
    toasts_.push_back({ text, level, GetTickCount64() + 4500, seq });
    anim_.to(K_TOAST | (seq & 0xFFFF), 0, 1, T2, Curve::Ui);   // @keyframes tin: opacity 0, translateY(6px)
    animate();
    setToastTimer();
    invalidate();
}

// one shot at the next expiry (toasts live 4.5 s)
void App::setToastTimer() {
    if (!hwnd_) return;
    if (toasts_.empty()) { KillTimer(hwnd_, T_TOAST); return; }
    ULONGLONG next = toasts_[0].until, now = GetTickCount64();
    for (auto& t : toasts_) next = std::min(next, t.until);
    SetTimer(hwnd_, T_TOAST, (UINT)std::max<ULONGLONG>(USER_TIMER_MINIMUM, next > now ? next - now : 0), nullptr);
}

void App::timer(UINT_PTR id) {
    if (id == T_SPIN) {
        if (spinRects_.empty() || !hwnd_) { invalidate(); return; }
        for (auto& r : spinRects_) {
            RECT px = { (LONG)floorf(r.left * scale_) - 2, (LONG)floorf(r.top * scale_) - 2, (LONG)ceilf(r.right * scale_) + 2, (LONG)ceilf(r.bottom * scale_) + 2 };
            InvalidateRect(hwnd_, &px, FALSE);
        }
        return;
    }
    if (id == T_TOAST) {
        ULONGLONG now = GetTickCount64();
        size_t n = toasts_.size();
        toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return t.until <= now; }), toasts_.end());
        if (n != toasts_.size()) for (auto& r : toastRects_) dirty(r);
        setToastTimer();
        return;
    }
    if (id == T_PORT) { invalidate(); return; }
    if (id == T_ANIM) {
        animTicks++;
        anim_.tick([&](D2D1_RECT_F r) { dirty(r); });
        animate();
    }
}

void App::updateSpinTimer() {
    bool run = se_.state == SessionState::Launching || se_.state == SessionState::Stopping ||
               (se_.state == SessionState::Running && se_.phase == MatchPhase::None &&
                (se_.backend == Stage::Run || se_.game == Stage::Run || se_.server == Stage::Run));
    // the cosmetics panel's spinners and elapsed times, only while it shows a load or an import
    bool port = pOpen_ && (pLoading_ || pj_ || pPvWait_);
    if (hwnd_) { if (port) SetTimer(hwnd_, T_PORT, 1000, nullptr); else KillTimer(hwnd_, T_PORT); }
    run = run || port;
    if (run == spinning_ || !hwnd_) { spinning_ = run && hwnd_; return; }
    spinning_ = run;
    if (run) SetTimer(hwnd_, T_SPIN, 33, nullptr); else KillTimer(hwnd_, T_SPIN);
}

bool App::busy() const { return se_.state == SessionState::Launching || se_.state == SessionState::Running || se_.state == SessionState::Stopping; }
bool App::sessionRunning() const { return se_.state == SessionState::Launching || se_.state == SessionState::Running; }

const Build* App::sel() const { for (auto& b : st_.builds) if (b.name == selected_) return &b; return nullptr; }
int App::level(const Build& b) const {
    if (b.lobby == Support::No) return 2;
    return b.firewall != L"OK" || b.lobby == Support::Experimental || !b.cosmetics ? 1 : 0;
}
bool App::matchOk(const Build* b) const { return b && b->match != Support::No && b->matchServer; }

void App::setConfig(uint32_t fields, const Config& v) {
    Config& c = st_.config;
    c.has = true;
    if (fields & ConfigPatch::DisplayName) c.displayName = v.displayName;
    if (fields & ConfigPatch::Mode) c.mode = v.mode;
    if (fields & ConfigPatch::Bots) c.bots = v.bots;
    if (fields & ConfigPatch::BotsAI) c.botsAI = v.botsAI;
    if (fields & ConfigPatch::InfiniteMaterials) c.infiniteMaterials = v.infiniteMaterials;
    if (fields & ConfigPatch::InfiniteAmmo) c.infiniteAmmo = v.infiniteAmmo;
    if (fields & ConfigPatch::LateGame) c.lateGame = v.lateGame;
    ConfigPatch p; p.fields = fields; p.values = c;
    core_->saveConfig(p);
    syncEdits();
    invalidate();
}

void App::select(const std::wstring& name, bool click) {
    if (busy()) return;
    if (click) greet();   // the page greets on every click on a build, also the selected one
    if (name == selected_) return;
    if (pOpen_) portShow(false);
    selected_ = name;
    for (size_t i = 0; i < st_.builds.size(); i++) if (st_.builds[i].name == name) tabEnsure_ = (int)i;
    core_->command(Command::Select, name);
    core_->setUiPref(L"lastBuild", name);
    contentScroll_ = 0;
    invalidate();
}

// The match settings sheet: fades in over the summary; closing it takes the focus out of it (the callers put it back
// on the Match settings button)
void App::openSheet(bool open) {
    if (sheetOpen_ == open) return;
    sheetOpen_ = open;
    contentScroll_ = 0;
    if (open) { anim_.to(K_SHEET, 0, 1, T2, Curve::Ui); animate(); }
    else anim_.stop(K_SHEET);
    invalidate();
}

void App::stepBuild(int d) {
    if (busy() || st_.builds.empty()) return;
    int i = 0;
    for (size_t k = 0; k < st_.builds.size(); k++) if (st_.builds[k].name == selected_) i = (int)k;
    int j = std::max(0, std::min((int)st_.builds.size() - 1, i + d));
    if (j != i) select(st_.builds[j].name, true);
}

void App::openActivity(bool open) {
    actOpen_ = open;
    if (open) { unseen_ = 0; unseenLevel_ = 0; logFollow_ = true; }
    else if (focus_ == ID_SEARCH || focus_ == ID_LINES || (focus_ >= ID_F0 && focus_ <= ID_CLEAR)) setFocus(ID_ACT, focusVisible_);
    invalidate();
}

// ================================================================================================ EDIT controls
void App::syncEdits() {
    if (!hwnd_) return;
    HWND f = GetFocus();
    wchar_t buf[64];
    auto set = [&](EditBox& e, const std::wstring& v) {
        if (f == e.hwnd) return;
        GetWindowTextW(e.hwnd, buf, 64);
        if (v != buf) SetWindowTextW(e.hwnd, v.c_str());
    };
    set(edits_[0], st_.config.displayName);
    set(edits_[1], std::to_wstring(botsDrag_ >= 0 ? botsDrag_ : st_.config.bots));
}

void App::commitEdit(int id) {
    wchar_t buf[64] = L"";
    if (id == ID_NAME) {
        GetWindowTextW(edits_[0].hwnd, buf, 64);
        std::wstring v = buf;
        while (!v.empty() && iswspace(v.back())) v.pop_back();
        while (!v.empty() && iswspace(v.front())) v.erase(v.begin());
        if (!v.empty() && v != st_.config.displayName) { Config c; c.displayName = v; setConfig(ConfigPatch::DisplayName, c); }
        else SetWindowTextW(edits_[0].hwnd, st_.config.displayName.c_str());
    } else if (id == ID_BOTS) {
        GetWindowTextW(edits_[1].hwnd, buf, 64);
        int v = std::max(0, std::min(99, _wtoi(buf)));
        if (v != st_.config.bots) { Config c; c.bots = v; setConfig(ConfigPatch::Bots, c); }
        SetWindowTextW(edits_[1].hwnd, std::to_wstring(st_.config.bots).c_str());
    }
}

void App::editCommand(int id, int code) {
    if (code == EN_SETFOCUS) { focus_ = id; invalidate(); }
    else if (code == EN_KILLFOCUS) { for (auto& e : edits_) if (e.id == id) e.want = false; commitEdit(id); invalidate(); }
    else if (code == EN_CHANGE && id == ID_P_SEARCH) {
        wchar_t buf[256] = L""; GetWindowTextW(edits_[3].hwnd, buf, 256);
        pQ_ = lower(buf); portRefilter();
    }
    else if (code == EN_CHANGE && id == ID_SEARCH) {
        wchar_t buf[256] = L""; GetWindowTextW(edits_[2].hwnd, buf, 256);
        q_ = lower(buf); rowsDirty_ = true; invalidate();
    }
}

void App::editKey(int id, UINT vk, bool ctrl, bool shift) {
    if (vk == VK_TAB) { moveFocus(shift ? -1 : 1); return; }
    if (ctrl && (vk == 'L' || vk == VK_RETURN)) { key(vk, true, shift); return; }
    if (vk == VK_RETURN) { commitEdit(id); return; }
    if (vk == VK_ESCAPE) {   // search: clear it; name/bots: back to the saved value
        if (id == ID_P_SEARCH) { if (pQ_.empty()) key(VK_ESCAPE, false, false); else SetWindowTextW(edits_[3].hwnd, L""); }
        else if (id == ID_SEARCH) SetWindowTextW(edits_[2].hwnd, L"");
        else {   // name / bots: back to the saved value; already there: Escape closes the sheet
            wchar_t buf[64] = L"";
            EditBox& e = edits_[id == ID_NAME ? 0 : 1];
            std::wstring saved = id == ID_NAME ? st_.config.displayName : std::to_wstring(st_.config.bots);
            GetWindowTextW(e.hwnd, buf, 64);
            if (saved == buf) key(VK_ESCAPE, false, false);
            else SetWindowTextW(e.hwnd, saved.c_str());
        }
        return;
    }
    if (id == ID_BOTS && (vk == VK_UP || vk == VK_DOWN)) {
        Config c; c.bots = std::max(0, std::min(99, st_.config.bots + (vk == VK_UP ? 1 : -1)));
        setConfig(ConfigPatch::Bots, c);
        SetWindowTextW(edits_[1].hwnd, std::to_wstring(c.bots).c_str());
        SendMessageW(edits_[1].hwnd, EM_SETSEL, 0, -1);
    }
}

void App::layoutEdits() {
    if (!hwnd_) return;
    for (auto& e : edits_) {
        bool show = e.show && (!e.lazy || e.want || GetFocus() == e.hwnd);
        D2D1_RECT_F vis = inter(e.r, e.clip);
        if (vis.right - vis.left < 4 || vis.bottom - vis.top < 4) show = false;
        for (auto& o : overlays_) if (overlaps(o, e.r)) show = false;
        if (!show) {
            if (e.shown) { if (GetFocus() == e.hwnd) SetFocus(hwnd_); ShowWindow(e.hwnd, SW_HIDE); e.shown = false; }
            continue;
        }
        // the EDIT is one text line tall, centred in the field (the field box itself is drawn by Direct2D)
        RECT r;
        r.left = lroundf(e.r.left * scale_); r.right = lroundf(e.r.right * scale_);
        int cy = lroundf((e.r.top + e.r.bottom) / 2 * scale_);
        r.top = cy - e.textH / 2; r.bottom = r.top + e.textH;
        bool clipped = vis.top > e.r.top + 0.5f || vis.bottom < e.r.bottom - 0.5f || vis.left > e.r.left + 0.5f || vis.right < e.r.right - 0.5f;
        if (memcmp(&r, &e.applied, sizeof r) || !e.shown || clipped != e.clipped) {
            SetWindowPos(e.hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (clipped) {
                HRGN g = CreateRectRgn(lroundf(vis.left * scale_) - r.left, lroundf(vis.top * scale_) - r.top, lroundf(vis.right * scale_) - r.left, lroundf(vis.bottom * scale_) - r.top);
                SetWindowRgn(e.hwnd, g, TRUE);
            } else if (e.clipped) SetWindowRgn(e.hwnd, nullptr, TRUE);
            e.applied = r; e.shown = true; e.clipped = clipped;
        }
    }
}

// WM_CTLCOLOREDIT: the EDITs are opaque; their background is the field's own color
HBRUSH App::editBrush(HWND edit, HDC dc) {
    uint32_t bg = C::bg2;
    for (auto& e : edits_) if (e.hwnd == edit) bg = e.bg;
    SetTextColor(dc, RGB(0xF5, 0xF6, 0xF7));
    SetBkColor(dc, RGB((bg >> 16) & 255, (bg >> 8) & 255, bg & 255));
    return bg == FIELD ? fieldBrush_ : editBrush_;
}

// ================================================================================================ primitives
ID2D1SolidColorBrush* App::br(uint32_t c, float a) { brush_->SetColor(rgb(c, a * alpha_)); return brush_; }

void App::fill(D2D1_RECT_F r, uint32_t c, float a, float radius) {
    if (measuring_) return;
    if (radius > 0) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), br(c, a));
    else rt_->FillRectangle(r, br(c, a));
}

// CSS box: background + 1px border inside the box, snapped to device pixels.
void App::box(D2D1_RECT_F r, float radius, uint32_t fillC, float fillA, uint32_t border, float borderA) {
    if (measuring_) return;
    auto snap = [&](float v) { return roundf(v * scale_) / scale_; };
    r = { snap(r.left), snap(r.top), snap(r.right), snap(r.bottom) };
    if (fillA > 0) fill(r, fillC, fillA, radius);
    if (borderA > 0) {
        float lw = std::max(1.f, floorf(scale_)) / scale_, h = lw / 2;
        D2D1_RECT_F b = { r.left + h, r.top + h, r.right - h, r.bottom - h };
        if (radius > 0) rt_->DrawRoundedRectangle(D2D1::RoundedRect(b, std::max(0.f, radius - h), std::max(0.f, radius - h)), br(border, borderA), lw);
        else rt_->DrawRectangle(b, br(border, borderA), lw);
    }
}

void App::hline(float x0, float x1, float y, uint32_t c) {
    if (measuring_) return;
    float lw = std::max(1.f, floorf(scale_)) / scale_;
    float yy = roundf(y * scale_) / scale_;
    rt_->FillRectangle({ x0, yy, x1, yy + lw }, br(c));
}
void App::vline(float x, float y0, float y1, uint32_t c) {
    if (measuring_) return;
    float lw = std::max(1.f, floorf(scale_)) / scale_;
    float xx = roundf(x * scale_) / scale_;
    rt_->FillRectangle({ xx, y0, xx + lw, y1 }, br(c));
}
void App::dot(float cx, float cy, float d, uint32_t c) {
    if (measuring_) return;
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d / 2, d / 2), br(c));
}

void App::icon(Icon i, float x, float y, float size, uint32_t c, float a) {
    if (measuring_) return;
    float vb;
    auto& parts = g_.icon(i, &vb);
    float s = size / vb;
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(x, y) * base_);
    auto* b = br(c, a);
    for (auto& p : parts) {
        if (p.fill) rt_->FillGeometry(p.geo, b);
        if (p.strokeWidth > 0) rt_->DrawGeometry(p.geo, b, p.strokeWidth, p.stroke);
    }
    rt_->SetTransform(base_);
}

void App::spinner(float x, float y, float size) {
    if (measuring_) return;
    spinRects_.push_back({ x, y + oy_, x + size, y + size + oy_ });
    float r = size / 2 - 1, cx = x + size / 2, cy = y + size / 2;
    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), br(C::l2), 2);
    float ang = (float)(GetTickCount64() % 800) / 800.f * 6.2831853f - 2.3561945f;   // starts at the top-left like border-top
    ID2D1PathGeometry* pg = nullptr; ID2D1GeometrySink* k = nullptr;
    if (SUCCEEDED(g_.d2d->CreatePathGeometry(&pg)) && SUCCEEDED(pg->Open(&k))) {
        k->BeginFigure(D2D1::Point2F(cx + r * cosf(ang), cy + r * sinf(ang)), D2D1_FIGURE_BEGIN_HOLLOW);
        float a2 = ang + 1.5707963f;
        k->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r * cosf(a2), cy + r * sinf(a2)), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        k->EndFigure(D2D1_FIGURE_END_OPEN); k->Close();
        rt_->DrawGeometry(pg, br(C::white), 2);
    }
    release(k); release(pg);
}

// .sk loading placeholder: an 800px gradient tile; while loading its shimmer moves it -400..400px in 1.4 s (linear)
void App::skeleton(int i, D2D1_RECT_F r) {
    if (measuring_) return;
    float p = 0;
    if (anim_.motion) {
        uint32_t key = K_SHIMMER | (uint32_t)i;
        if (!anim_.running(key)) anim_.to(key, -400, 400, 1400, Curve::Linear, {}, 0, 0);
        p = tw(key, 0, r);
    }
    if (!shimmer_) { fill(r, C::surface, 0.55f, 8); return; }
    shimmer_->SetStartPoint(D2D1::Point2F(r.left + p, 0));
    shimmer_->SetEndPoint(D2D1::Point2F(r.left + p + 800, 0));
    shimmer_->SetOpacity(alpha_);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 8, 8), shimmer_);
}

// box-shadow: 0 oy blur rgba(0,0,0,alpha). CSS blurs the offset box with a Gaussian of sigma = blur / 2, so at a
// distance d outside it the shadow is alpha * erfc(d / (sigma * sqrt 2)) / 2. Stacked rounded rects, outermost first,
// each with the alpha that brings the accumulated coverage to that profile at the middle of its band.
void App::shadow(D2D1_RECT_F r, float radius, float blur, float oy, float alpha, uint32_t color) {
    if (measuring_ || blur <= 0) return;
    float sigma = blur / 2, step = std::max(1.f, sigma / 6);
    auto target = [&](float d) { return alpha * 0.5f * erfcf(d / (sigma * 1.41421356f)); };
    float covered = 0;
    for (float e = 3 * sigma; e > -2 * sigma; e -= step) {
        float want = target(e - step / 2);
        float a = covered >= 1 ? 0 : 1 - (1 - want) / (1 - covered);
        covered = want;
        if (a <= 0.001f) continue;
        D2D1_RECT_F s = { r.left - e, r.top + oy - e, r.right + e, r.bottom + oy + e };
        if (s.right - s.left < 1 || s.bottom - s.top < 1) break;
        fill(s, color, a, std::max(0.f, radius + e));
    }
}

float App::textW(const std::wstring& s, const Font& f, float tracking) {
    if (s.empty()) return 0;
    IDWriteTextLayout* l = nullptr; float w = 0;
    if (SUCCEEDED(g_.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), g_.format(f), 10000, 1000, &l))) {
        if (f.tnum && g_.tnum) l->SetTypography(g_.tnum, { 0, (UINT32)s.size() });
        if (tracking != 0) { IDWriteTextLayout1* l1 = nullptr; if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) { l1->SetCharacterSpacing(0, tracking, 0, { 0, (UINT32)s.size() }); l1->Release(); } }
        DWRITE_TEXT_METRICS m; if (SUCCEEDED(l->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        l->Release();
    }
    return w;
}

// One line of text in a CSS line box (x, top, w, lh): half-leading like the browser, ellipsis when it does not fit.
// Returns true when the text had to be cut.
bool App::text(const std::wstring& s, const Font& f, float x, float top, float w, float lh, uint32_t c, int align, float tracking, float a) {
    if (s.empty() || w <= 0) return false;
    IDWriteTextFormat* tf = g_.format(f);
    IDWriteTextLayout* l = nullptr;
    if (!tf || FAILED(g_.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, w, lh * 4, &l))) return false;
    if (f.tnum && g_.tnum) l->SetTypography(g_.tnum, { 0, (UINT32)s.size() });
    if (tracking != 0) { IDWriteTextLayout1* l1 = nullptr; if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) { l1->SetCharacterSpacing(0, tracking, 0, { 0, (UINT32)s.size() }); l1->Release(); } }
    DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m);
    bool cut = m.widthIncludingTrailingWhitespace > w + 0.5f;
    if (!measuring_) {
        if (cut) {
            DWRITE_TRIMMING t = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            IDWriteInlineObject* e = nullptr; g_.dw->CreateEllipsisTrimmingSign(tf, &e);
            l->SetTrimming(&t, e); release(e);
        }
        l->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        float asc, h; g_.fontBox(f, &asc, &h);
        rt_->DrawTextLayout(D2D1::Point2F(x, top + (lh - h) / 2), l, br(c, a));
    }
    l->Release();
    return cut;
}

// Wrapped paragraph with **bold** runs (weight 600, boldC). Uniform line height lh. Returns the height.
float App::rich(const std::wstring& src, const Font& f, float x, float top, float w, float lh, uint32_t c, uint32_t boldC, int align, bool draw) {
    std::wstring s; std::vector<DWRITE_TEXT_RANGE> bold;
    bool on = false; UINT32 start = 0;
    for (size_t i = 0; i < src.size(); i++) {
        if (src[i] == L'*' && i + 1 < src.size() && src[i + 1] == L'*') {
            if (on) bold.push_back({ start, (UINT32)s.size() - start }); else start = (UINT32)s.size();
            on = !on; i++; continue;
        }
        s += src[i];
    }
    if (s.empty() || w <= 0) return 0;
    IDWriteTextLayout* l = nullptr;
    if (FAILED(g_.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), g_.format(f), w, 10000, &l))) return 0;
    l->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    float asc, h; g_.fontBox(f, &asc, &h);
    l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lh, (lh - h) / 2 + asc);
    l->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!measuring_ && draw) brush2_->SetColor(rgb(boldC, alpha_));
    for (auto& r : bold) { l->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r); if (!measuring_ && draw) l->SetDrawingEffect(brush2_, r); }
    DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m);
    if (!measuring_ && draw) rt_->DrawTextLayout(D2D1::Point2F(x, top), l, br(c));
    l->Release();
    return m.lineCount * lh;
}

void App::hit(int id, D2D1_RECT_F r, int flags, float radius) {
    if (measuring_ || noHit_) return;
    r.top += oy_; r.bottom += oy_;
    if (inContent_) flags |= F_CONTENT;
    hits_.push_back({ id, r, flags, radius, clip_, layer_ });
}
const App::Hit* App::findHit(float x, float y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) if (inside(it->r, x, y) && inside(it->clip, x, y)) return &*it;
    return nullptr;
}
const App::Hit* App::hitById(int id) const { for (auto& h : hits_) if (h.id == id) return &h; return nullptr; }
D2D1_RECT_F App::rectOf(int id) const { auto* h = hitById(id); return h ? h->r : D2D1_RECT_F{}; }

float App::button(int id, float x, float y, const wchar_t* label, Icon ic, bool sm, bool ghost, bool iconOnly, bool hasIcon, bool measure) {
    float h = sm ? 28.f : 34.f, pad = sm ? 10.f : 12.f;
    Font f{ Face::UI, sm ? 12.f : 13.f, 600, false };
    float tw = label ? textW(label, f) : 0;
    float w = iconOnly ? h : pad * 2 + 2 + (hasIcon ? 16 + 8 : 0) + tw;
    if (measure) return w;
    bool hov = hover_ == id;
    D2D1_RECT_F r = rc(x, y, w, h);
    uint32_t col;
    // .btn: background and border-color 120 ms (the ghost's text color has no transition)
    float hv = fx(K_HOVER | id, hov ? 1.f : 0.f, r);
    if (ghost) { box(r, 8, C::hover, hv, C::hover, 0); col = hov ? C::text : C::muted; }
    else { box(r, 8, mix(C::surface, C::hover, hv), 1, mix(C::line, C::l2, hv)); col = C::text; }
    if (hasIcon) icon(ic, iconOnly ? x + (w - 16) / 2 : x + 1 + pad, y + (h - 16) / 2, 16, col);
    if (label && !iconOnly && text(label, f, x + 1 + pad + (hasIcon ? 24 : 0), y, tw + 1, h, col)) issues.push_back(std::wstring(L"clipped btn: ") + label);
    hit(id, r, F_FOCUS | F_HAND, 8);
    return w;
}

void App::drawScrollbar(int id, float x, float y, float h, float scroll, float total, float view) {
    if (total <= view + 0.5f) return;
    float th = std::max(24.f, h * view / total);
    float ty = y + (h - th) * (scroll / (total - view));
    bool hov = hover_ == id || drag_ == id;
    fill({ x + 3, ty + 3, x + 7, ty + th - 3 }, hov ? C::l2 : C::line, 1, 2);
    hit(id, rc(x, y, 10, h), 0, 0);
}

// ================================================================================================ paint
// The main screen ("E · Poster"): the selected build's season art fills the window under the title bar, its version
// as a huge outlined poster number behind the skin; the build switcher and the Cosmetics / Activity / Build buttons on
// top, the season's name at the bottom left, one Launch at the bottom right with the match summary and the status
// line above it (the settings open as a sheet in the summary's place), Activity docked at the bottom when open.
void App::paint(float W, float H) {
    if (!rt_) return;
    W_ = W; H_ = H;
    hits_.clear(); issues.clear(); spinRects_.clear(); toastRects_.clear();
    overlays_.clear();
    anim_.frame();
    img_.drain(); img_.frame();
    for (auto& e : edits_) e.show = false;
    base_ = D2D1::Matrix3x2F::Identity(); oy_ = 0; alpha_ = 1;
    clip_ = { -1e9f, -1e9f, 1e9f, 1e9f };
    rt_->SetTransform(base_);
    rt_->Clear(rgb(C::bg));

    // Activity docks over the bottom (the art stays the same size under it: no new decode or stage bitmap when it
    // opens or its grip moves); the title and the dock keep at least 310 DIPs above it
    float actTop = H;
    if (actOpen_) actTop = H - std::max(140.f, std::min(actH_, H - TB - 310));
    drawStage(TB, H);
    drawTitle(W);

    // "E · Poster": the switcher and the icon buttons on top, the season's title at the bottom left, the dock (summary
    // or settings sheet, status line, Launch) at the bottom right; 48 DIPs from the edges on a large window
    float mx = std::max(24.f, std::min(48.f, W * .0234f)), my = std::max(24.f, std::min(48.f, (H - TB) * .045f));
    drawTop(TB + 24, W);
    contentClip_ = {};
    float dockTop = actTop - 24;
    playShown_ = false;
    if (pOpen_) { sheetRect_ = {}; drawPort(0, TB + 76, W, actTop - TB - 76); }   // the cosmetics panel under the switcher
    else {
        float dw = std::max(300.f, std::min(540.f, W * .264f));
        float dx = W - mx - dw;
        dockTop = drawDock(dx, dw, TB + 84, actTop - my);
        drawPoster(mx, dx - 24, TB + 84, actTop - my - 8);
    }
    if (actOpen_) drawActivity(0, actTop, W, H - actTop);
    if (playShown_ && !pOpen_) hit(ID_PLAY, playRect_, F_FOCUS | F_HAND | (playDisabled_ ? F_DISABLED : 0), playRadius_);   // the last control, like the page
    toastRight_ = pOpen_ ? W - 24 : W - mx;
    toastBottom_ = pOpen_ ? actTop - 24 : dockTop - 12;
    drawMenu();
    drawPortOverlays();
    drawToasts(W, H);

    // keyboard focus ring (focus-visible): 2px white, 2px outside the control (inside for the window buttons)
    if (focusVisible_ && focus_) {
        const Hit* f = hitById(focus_);
        if (f && !(f->flags & F_EDIT) && f->id != ID_P_GRID) {   // the grid draws its cursor's ring
            float o = (f->flags & F_RING_IN) ? -3.f : 3.f;
            D2D1_RECT_F r = { f->r.left - o, f->r.top - o, f->r.right + o, f->r.bottom + o };
            float rad = std::max(0.f, f->radius + o);
            rt_->PushAxisAlignedClip(f->clip, D2D1_ANTIALIAS_MODE_ALIASED);
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br(C::white), 2);
            rt_->PopAxisAlignedClip();
        }
    }
    layoutEdits();   // hides an EDIT under this frame's menu/toast; Windows then repaints the area it uncovers
    if (pendingEnsure_) { pendingEnsure_ = false; ensureVisible(focus_); }
    animate();
}

// ------------------------------------------------------------------------------------------------ lobby primitives
// .glass: rgba(9,13,28,.62) with a 1px rgba(255,255,255,.12) border (no backdrop blur)
void App::glass(D2D1_RECT_F r, float radius, float fillA, float borderA) { box(r, radius, GLASS, fillA, C::white, borderA); }

// .frost: the blurred stage under rgba(12,17,38,.55) with a 1px rgba(255,255,255,.14) border
void App::frost(D2D1_RECT_F r, float radius) {
    if (measuring_) return;
    if (blurBr_) {
        blurBr_->SetTransform(D2D1::Matrix3x2F::Translation(0, TB));
        blurBr_->SetOpacity(alpha_);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), blurBr_);
    }
    box(r, radius, FROST, .55f, C::white, .14f);
}

// .badge: a black pill skewed -12deg, italic 900 11px uppercase (+.02em); a level dot (.ok::before: 7px with a 2px
// ring) or a spinner before the text. Returns its width (21 DIPs tall).
float App::pill(float x, float y, const std::wstring& s, int lvl, bool measure) {
    Font f{ Face::Display, 11, 900, true };
    float tw = textW(s, f, 0.22f), dw = lvl == 3 ? 11 + 7 : lvl >= 0 ? 7 + 7 : 0;
    float w = 9 + dw + tw + 9, h = 21;
    if (measure || measuring_) return w;
    rt_->SetTransform(D2D1::Matrix3x2F::Skew(-12, 0, D2D1::Point2F(x + w / 2, y + h / 2)) * base_);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(rc(x, y, w, h), 6, 6), br(0x000000));
    rt_->SetTransform(base_);
    if (lvl == 3) spinner(x + 9, y + 5, 11);
    else if (lvl >= 0) {
        uint32_t c = lvl == 0 ? GREEN : lvl == 1 ? C::warn : C::bad;
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 9 + 3.5f, y + h / 2), 5.5f, 5.5f), br(c, .35f));
        dot(x + 9 + 3.5f, y + h / 2, 7, c);
    }
    text(s, f, x + 9 + dw, y, tw + 2, h, C::white, 0, 0.22f);
    return w;
}

// .glass.g: the top buttons (and the empty state's and notices'); ic = Icon::Count for a text-only one
float App::gbutton(int id, float x, float y, float h, const wchar_t* label, Icon ic, bool iconOnly, bool measure) {
    Font f{ Face::UI, 12.5f, 600, false };
    float pad = h < 34 ? 10.f : 14.f;
    bool hasIcon = ic != Icon::Count;
    float tw = label && !iconOnly ? textW(label, f) : 0;
    float w = iconOnly ? h : pad * 2 + (hasIcon ? 16 : 0) + (hasIcon && tw > 0 ? 8 : 0) + tw;
    if (measure) return w;
    D2D1_RECT_F r = rc(x, y, w, h);
    int owner = menu_ == 1 ? ID_ADD : menu_ == 2 ? ID_BUILDMENU : menu_ == 3 ? ID_COSMETICS : 0;
    float hv = fx(K_HOVER | id, hover_ == id || owner == id || (id == ID_ACT && actOpen_) || (id == ID_P_OPEN && pOpen_) ? 1.f : 0.f, r);
    glass(r, 12, .62f + .2f * hv, .12f + .16f * hv);
    if (hasIcon) icon(ic, iconOnly ? x + (w - 16) / 2 : x + pad, y + (h - 16) / 2, 16, C::white);
    if (tw > 0 && text(label, f, x + pad + (hasIcon ? 24 : 0), y, tw + 1, h, C::white)) issues.push_back(std::wstring(L"clipped btn: ") + label);
    hit(id, r, F_FOCUS | F_HAND, 12);
    return w;
}

// ------------------------------------------------------------------------------------------------ season art
void App::releaseArt() {
    release(blurBr_); release(blur_); release(bg_); release(bgRT_);
    for (auto& t : thumbs_) { release(t.brush); release(t.bmp); }
}

// Finished downloads/decodes: the selected skin replaces the art (the previous one is already gone), thumbnails
// land in their tab.
void App::takeArt() {
    bool changed = false;
    for (auto& im : art_.take()) {
        changed = true;
        if (im.thumb) {
            for (auto& t : thumbs_) if (t.skin == im.skin) {
                t.req = 0;
                if (!im.px.empty()) { release(t.brush); release(t.bmp); t.img = std::move(im); }
                break;
            }
        } else if (im.skin == artWant_) {
            artReqPx_ = 0;
            if (!im.px.empty()) { artImg_ = std::move(im); artGen_++; }
        }
    }
    if (changed) invalidate();
}

// The selected build's skin at px rows (the stage's height in device pixels; never above the file's own size).
// Another build drops the old pixels at once; a resize asks for a new decode only past 10 %.
void App::ensureArt(const wchar_t* skin, UINT px) {
    std::wstring s = skin ? skin : L"";
    if (s != artWant_) { artWant_ = s; artImg_ = ArtImage(); artReqPx_ = 0; artGen_++; }
    if (s.empty() || px < 16 || art_.failed(s)) return;
    UINT target = artImg_.srcH ? std::min(px, artImg_.srcH) : px;
    auto within = [&](UINT a) { return a && fabsf((float)a - (float)target) <= 0.1f * target; };
    UINT have = artImg_.px.empty() ? 0 : artImg_.h - artImg_.padT - artImg_.padB;
    if (within(have) || within(artReqPx_)) return;
    artReqPx_ = target;
    art_.request(s, false, target);
    if (!hwnd_) takeArt();   // offscreen: decoded right away
}

// A tab's thumbnail: .tab img { height: 150% } = 78 DIPs, its left part faded out (baked in the pixels).
ID2D1BitmapBrush* App::thumb(const wchar_t* skin) {
    if (!skin || !rt_) return nullptr;
    UINT need = (UINT)lroundf(78 * scale_);
    size_t i = 0;
    while (i < thumbs_.size() && thumbs_[i].skin != skin) i++;
    if (i == thumbs_.size()) { thumbs_.push_back(Thumb{}); thumbs_.back().skin = skin; }
    if (art_.failed(thumbs_[i].skin)) return nullptr;
    UINT target = thumbs_[i].img.srcH ? std::min(need, thumbs_[i].img.srcH) : need;
    UINT have = thumbs_[i].img.px.empty() ? 0 : thumbs_[i].img.h;
    if (have != target && thumbs_[i].req != target) {
        thumbs_[i].req = target;
        art_.request(thumbs_[i].skin, true, target);
        if (!hwnd_) takeArt();
    }
    Thumb& t = thumbs_[i];
    if (t.img.px.empty()) return nullptr;
    if (!t.brush) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_, dpi_);
        if (SUCCEEDED(rt_->CreateBitmap(D2D1::SizeU(t.img.w, t.img.h), t.img.px.data(), t.img.w * 4, bp, &t.bmp))) rt_->CreateBitmapBrush(t.bmp, &t.brush);
    }
    return t.brush;
}

// .stage: .bg radial gradient, .rays, img.art and .fade, composed once into a bitmap of the stage's device pixels and
// rebuilt only when the size, the season, the art or the target change (a frame then draws one bitmap).
void App::drawStage(float top, float bottom) {
    if (bottom - top < 1) return;
    const Build* b = loaded_ ? sel() : nullptr;
    const SeasonArt& sa = b ? seasonArt(b->major, b->minor) : seasonArt(0, 0);
    float W = W_, sh = bottom - top;
    // img.skin: 1180 DIPs tall at a 1064 DIP stage, 30 below its top, centered at 46.4 % of the width
    const float SKIN_H = 1180.f / 1064, SKIN_T = 30.f / 1064, SKIN_X = .464f;
    ensureArt(sa.skin, (UINT)lroundf(sh * SKIN_H * scale_));
    // bare: the cosmetics panel shows the season without the skin and the version
    BgKey k = { W, top, bottom, scale_, &sa, artGen_, rt_, pOpen_, b && !pOpen_ ? verLabel(*b) : std::wstring() };
    bool same = bg_ && k.w == bgKey_.w && k.top == bgKey_.top && k.bottom == bgKey_.bottom && k.scale == bgKey_.scale && k.sa == bgKey_.sa &&
                k.artGen == bgKey_.artGen && k.rt == bgKey_.rt && k.bare == bgKey_.bare && k.ghost == bgKey_.ghost;
    if (!same) {
        release(blurBr_); release(blur_); release(bg_); release(bgRT_);
        D2D1_SIZE_U px = D2D1::SizeU((UINT32)ceilf(W * scale_ - 0.01f), (UINT32)ceilf(sh * scale_ - 0.01f));
        if (px.width && px.height && SUCCEEDED(rt_->CreateCompatibleRenderTarget(D2D1::SizeF(px.width / scale_, px.height / scale_), px, &bgRT_))) {
            ID2D1RenderTarget* t = bgRT_;
            t->BeginDraw();
            t->Clear(rgb(sa.c[3]));
            ID2D1GradientStopCollection* gs = nullptr;
            // .bg: radial-gradient(80% 110% at 42% 40%, c0 0%, c1 32%, c2 58%, c3 100%)
            const D2D1_GRADIENT_STOP bgs[] = { { 0, rgb(sa.c[0]) }, { .32f, rgb(sa.c[1]) }, { .58f, rgb(sa.c[2]) }, { 1, rgb(sa.c[3]) } };
            ID2D1RadialGradientBrush* rb = nullptr;
            if (SUCCEEDED(t->CreateGradientStopCollection(bgs, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs)) &&
                SUCCEEDED(t->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(.42f * W, .40f * sh), {}, .8f * W, 1.1f * sh), gs, &rb)))
                t->FillRectangle(D2D1::RectF(0, 0, W, sh), rb);
            release(rb); release(gs);
            // .rays: repeating-conic-gradient(from 190deg at 42% 44%, rgba(255,255,255,.055) 0 5deg, transparent 5deg 15deg),
            // masked by radial-gradient(55% 75% at 42% 44%, #000, transparent): 24 wedges under a fading radial brush
            float cx = .42f * W, cy = .44f * sh, R = 2 * (W + sh);
            ID2D1PathGeometry* pg = nullptr; ID2D1GeometrySink* gk = nullptr;
            if (SUCCEEDED(g_.d2d->CreatePathGeometry(&pg)) && SUCCEEDED(pg->Open(&gk))) {
                for (int i = 0; i < 24; i++) {
                    float a0 = (190 + 15.f * i) * 0.01745329f, a1 = a0 + 5 * 0.01745329f;
                    gk->BeginFigure(D2D1::Point2F(cx, cy), D2D1_FIGURE_BEGIN_FILLED);
                    gk->AddLine(D2D1::Point2F(cx + R * sinf(a0), cy - R * cosf(a0)));
                    gk->AddLine(D2D1::Point2F(cx + R * sinf(a1), cy - R * cosf(a1)));
                    gk->EndFigure(D2D1_FIGURE_END_CLOSED);
                }
                gk->Close();
                const D2D1_GRADIENT_STOP rs[] = { { 0, rgb(C::white, sa.skin ? .055f : .03f) }, { 1, rgb(C::white, 0) } };   // softer on the dark neutral one
                if (SUCCEEDED(t->CreateGradientStopCollection(rs, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs)) &&
                    SUCCEEDED(t->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), {}, .55f * W, .75f * sh), gs, &rb)))
                    t->FillGeometry(pg, rb);
                release(rb); release(gs);
            }
            release(gk); release(pg);
            // .ghost: the version, italic 900 at 600 DIPs on a 1064 DIP stage (narrower to fit the width), -.06em, 150 DIPs
            // from the left and the top (line-height .8); a .05 white fill and a 3px .16 white outline
            if (!k.ghost.empty()) {
                Font gf{ Face::Display, 100, 900, true };
                IDWriteTextLayout* l = nullptr;
                if (SUCCEEDED(g_.dw->CreateTextLayout(k.ghost.c_str(), (UINT32)k.ghost.size(), g_.format(gf), 1e5f, 1e4f, &l))) {
                    IDWriteTextLayout1* l1 = nullptr;
                    if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) { l1->SetCharacterSpacing(0, -6.f, 0, { 0, (UINT32)k.ghost.size() }); l1->Release(); }
                    DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m);
                    float size = std::min(600.f / 1064 * sh, m.width > 0 ? .92f * W / m.width * 100 : 1e9f), s = size / 100;
                    float asc, hh; g_.fontBox(gf, &asc, &hh);
                    float left = 150.f / 2048 * W, base = 150.f / 1064 * sh + ((80 - hh) / 2 + asc) * s;
                    OutlineRenderer o(g_.d2d);
                    l->Draw(nullptr, &o, 0, 0);
                    ID2D1SolidColorBrush* gb = nullptr;
                    if (SUCCEEDED(t->CreateSolidColorBrush(rgb(C::white, .05f), &gb))) {
                        for (auto& r : o.runs) {
                            t->SetTransform(D2D1::Matrix3x2F::Translation(r.x, 0) * D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(left, base));
                            gb->SetColor(rgb(C::white, .05f)); t->FillGeometry(r.geo, gb);
                            gb->SetColor(rgb(C::white, .16f)); t->DrawGeometry(r.geo, gb, 3 / s);
                        }
                        t->SetTransform(D2D1::Matrix3x2F::Identity());
                    }
                    release(gb);
                    l->Release();
                }
            }
            // img.skin: its drop shadow is in the pixels
            if (!artImg_.px.empty() && !k.bare) {
                ID2D1Bitmap* bm = nullptr;
                D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
                if (SUCCEEDED(t->CreateBitmap(D2D1::SizeU(artImg_.w, artImg_.h), artImg_.px.data(), artImg_.w * 4, bp, &bm))) {
                    float boxH = (float)(artImg_.h - artImg_.padT - artImg_.padB), kk = sh * SKIN_H / boxH;
                    float boxW = (artImg_.w - artImg_.padL - artImg_.padR) * kk;
                    float left = SKIN_X * W - .5f * boxW, tp = SKIN_T * sh, bot = tp + sh * SKIN_H;
                    t->DrawBitmap(bm, D2D1::RectF(left - artImg_.padL * kk, tp - artImg_.padT * kk, left + boxW + artImg_.padR * kk, bot + artImg_.padB * kk), 1,
                                  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                release(bm);
            }
            // .fade: linear-gradient(0deg, rgba(6,9,22,.85) 0%, rgba(6,9,22,.25) 30%, transparent 50%),
            //        linear-gradient(90deg, rgba(6,9,22,.5) 0%, transparent 25%, transparent 60%, rgba(6,9,22,.7) 100%)
            const uint32_t fc = 0x060916;
            const D2D1_GRADIENT_STOP f1[] = { { 0, rgb(fc, .85f) }, { .30f, rgb(fc, .25f) }, { .50f, rgb(fc, 0) }, { 1, rgb(fc, 0) } };
            const D2D1_GRADIENT_STOP f2[] = { { 0, rgb(fc, .5f) }, { .25f, rgb(fc, 0) }, { .60f, rgb(fc, 0) }, { 1, rgb(fc, .7f) } };
            ID2D1LinearGradientBrush* lb = nullptr;
            if (SUCCEEDED(t->CreateGradientStopCollection(f1, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs)) &&
                SUCCEEDED(t->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, sh), D2D1::Point2F(0, 0)), gs, &lb)))
                t->FillRectangle(D2D1::RectF(0, 0, W, sh), lb);
            release(lb); release(gs);
            if (SUCCEEDED(t->CreateGradientStopCollection(f2, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs)) &&
                SUCCEEDED(t->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(W, 0)), gs, &lb)))
                t->FillRectangle(D2D1::RectF(0, 0, W, sh), lb);
            release(lb); release(gs);
            if (k.bare) {
                // the cosmetics panel: from .2 behind the tabs to .55 at its title, .92 some 360 DIPs under it
                float L = 76 + std::max(360.f, std::min(sh - 76, 520.f));
                const D2D1_GRADIENT_STOP ps[] = { { 0, rgb(C::bg, .2f) }, { 76 / L, rgb(C::bg, .55f) }, { 256 / L, rgb(C::bg, .78f) }, { 436 / L, rgb(C::bg, .88f) }, { 1, rgb(C::bg, .92f) } };
                if (SUCCEEDED(t->CreateGradientStopCollection(ps, 5, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs)) &&
                    SUCCEEDED(t->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, L)), gs, &lb)))
                    t->FillRectangle(D2D1::RectF(0, 0, W, sh), lb);
                release(lb); release(gs);
            }
            if (SUCCEEDED(t->EndDraw())) bgRT_->GetBitmap(&bg_);
        }
        // .frost's backdrop-filter: blur(28px): the stage halved four times (each a 2x2 average), drawn back stretched
        // with linear filtering under the summary and the sheet
        if (bg_) {
            ID2D1Bitmap* src = bg_; src->AddRef();
            D2D1_SIZE_U px = bg_->GetPixelSize();
            for (int i = 0; i < 4 && src; i++) {
                px = D2D1::SizeU(std::max(1u, px.width / 2), std::max(1u, px.height / 2));
                ID2D1BitmapRenderTarget* h = nullptr; ID2D1Bitmap* out = nullptr;
                if (SUCCEEDED(rt_->CreateCompatibleRenderTarget(D2D1::SizeF(W, sh), px, &h))) {
                    h->BeginDraw();
                    h->DrawBitmap(src, D2D1::RectF(0, 0, W, sh), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    if (SUCCEEDED(h->EndDraw())) h->GetBitmap(&out);
                }
                release(h); release(src);
                src = out;
            }
            blur_ = src;
            if (blur_) rt_->CreateBitmapBrush(blur_, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &blurBr_);
        }
        bgKey_ = k;
    }
    if (bg_) {
        D2D1_SIZE_F s = bg_->GetSize();
        rt_->DrawBitmap(bg_, D2D1::RectF(0, top, s.width, top + s.height), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    } else fill({ 0, top, W, bottom }, sa.c[3]);
}

// ------------------------------------------------------------------------------------------------ top row
// .sw (top left): ‹, the builds as labels (version + "C2 S4"; the selected one white with a 3px underline), ›, + and
// the Q / E keycaps; the labels scroll (wheel, keyboard, selection) when they do not fit. .tr (top right): Cosmetics,
// Activity and Build as 44 DIP glass icon buttons with their badges.
void App::drawTop(float y, float W) {
    const Build* b = loaded_ ? sel() : nullptr;
    const float bs = 44, ty = y - 6, ah = 40;
    float right = W - 24;
    if (loaded_) {
        int nb = b ? 3 : 1;
        float x = W - 24 - nb * bs - (nb - 1) * 10;
        right = x;
        if (b) {
            int cosId = b->port ? (int)ID_P_OPEN : (int)ID_COSMETICS;   // where cosmetics can be imported: the cosmetics panel
            gbutton(cosId, x, ty, bs, L"Cosmetics", Icon::Cos, true);
            if (cosId == ID_P_OPEN && !pOpen_) {
                // cosmetics of the current game can be imported: a brighter ring and, once listed, how many are new
                box(rc(x, ty, bs, bs), 12, 0, 0, C::white, .42f);
                std::wstring n = pNewAll_ > 0 && pBuild_ == selected_ ? (pNewAll_ > 9999 ? fmt(L"%dk", pNewAll_ / 1000) : thousands(pNewAll_)) : L"";
                Font nf{ Face::UI, 10.5f, 700, false, true };
                float nw = std::max(18.f, textW(n, nf) + 12), nx = x + bs + 10 - nw, ny = ty - 8;
                if (!n.empty()) { fill(rc(nx, ny, nw, 18), C::white, 1, 9); text(n, nf, nx, ny, nw, 18, INK, 1); }
            }
            if (menu_ == 3) menuAnchor_ = rc(x, ty, bs, bs);
            x += bs + 10;
        }
        // Activity: the lines it got while closed, colored by their worst level
        gbutton(ID_ACT, x, ty, bs, L"Activity", Icon::Log, true);
        if (unseen_) {
            std::wstring n = unseen_ > 999 ? L"999+" : std::to_wstring(unseen_);
            Font nf{ Face::UI, 10.5f, 700, false, true };
            float nw = std::max(18.f, textW(n, nf) + 10), nx = x + bs + 7 - nw, ny = ty - 7;
            uint32_t bg = unseenLevel_ == 2 ? C::bad : unseenLevel_ == 1 ? C::warn : C::white;
            fill(rc(nx, ny, nw, 18), bg, 1, 9);
            text(n, nf, nx, ny, nw, 18, unseenLevel_ == 2 ? C::white : INK, 1);
        }
        x += bs + 10;
        if (b) {
            gbutton(ID_BUILDMENU, x, ty, bs, L"Build", Icon::More, true);
            if (menu_ == 2) menuAnchor_ = rc(x, ty, bs, bs);
        }
    }

    const float L = std::max(24.f, std::min(40.f, W * .0195f));
    int n = loaded_ ? (int)st_.builds.size() : 3, cur = -1;
    if (loaded_) for (int i = 0; i < n; i++) if (st_.builds[i].name == selected_) cur = i;
    Font vf{ Face::Display, 22, 900, true }, sf{ Face::UI, 11, 600, false };
    auto small = [&](const Build& bd) { std::wstring s = seasonShort(bd.major, bd.minor); size_t p = s.find(L" · "); if (p != std::wstring::npos) s.replace(p, 3, L" "); return s; };
    std::vector<float> lw((size_t)n, 100.f);
    float total = 0;
    for (int i = 0; i < n; i++) {
        if (loaded_) {
            const Build& bd = st_.builds[i];
            std::wstring s = small(bd);
            lw[i] = 16 + textW(verLabel(bd), vf) + (s.empty() ? 0 : 8 + textW(s, sf)) + 16;
        }
        total += lw[i] + (i ? 6 : 0);
    }
    bool keys = W >= 980;
    float fixedR = 6 + ah + 12 + ah + (keys ? 10 + 22 + 4 + 22 : 0);
    float lx0 = L + ah + 6;
    float avail = std::max(0.f, right - 24 - lx0 - fixedR);
    float viewW = std::min(total, avail);
    tabMax_ = std::max(0.f, total - avail);
    if (tabEnsure_ >= 0 && tabEnsure_ < n) {
        float a = 0;
        for (int i = 0; i < tabEnsure_; i++) a += lw[i] + 6;
        if (a < tabScroll_) tabScroll_ = a;
        else if (a + lw[tabEnsure_] > tabScroll_ + viewW) tabScroll_ = a + lw[tabEnsure_] - viewW;
    }
    tabEnsure_ = -1;
    tabScroll_ = std::max(0.f, std::min(tabScroll_, tabMax_));
    tabScroll_ = roundf(tabScroll_ * scale_) / scale_;

    // ‹ and ›: the previous / next build (Q / E); dim at the ends and while a session runs
    auto arrow = [&](int id, float ax, Icon ic, bool on) {
        D2D1_RECT_F r = rc(ax, y, ah, ah);
        float hv = fx(K_HOVER | id, on && hover_ == id ? 1.f : 0.f, r);
        glass(r, 12, .62f + .2f * hv, .12f + .16f * hv);
        icon(ic, ax + 12, y + 12, 16, C::white, on ? 1.f : .35f);
        hit(id, r, F_FOCUS | F_HAND | (on ? 0 : F_DISABLED), 12);
    };
    bool canPrev = loaded_ && cur > 0 && !busy(), canNext = loaded_ && cur >= 0 && cur + 1 < n && !busy();
    arrow(ID_PREV, L, Icon::ChevL, canPrev);

    D2D1_RECT_F saved = clip_;
    tabClip_ = clip_ = { lx0 - 4, y - 6, lx0 + viewW + 4, y + ah + 6 };
    rt_->PushAxisAlignedClip(clip_, D2D1_ANTIALIAS_MODE_ALIASED);
    float x = lx0 - tabScroll_;
    for (int i = 0; i < n; i++) {
        D2D1_RECT_F r = rc(x, y, lw[i], ah);
        x += lw[i] + 6;
        if (!loaded_) { skeleton(i, r); continue; }
        const Build& bd = st_.builds[i];
        int id = ID_BUILD0 + i;
        bool on = i == cur;
        float hv = fx(K_HOVER | id, hover_ == id ? 1.f : 0.f, r);   // color 120 ms (selection is instant)
        float a = on ? 1.f : .55f + .3f * hv;
        std::wstring v = verLabel(bd), s = small(bd);
        float vw = textW(v, vf);
        text(v, vf, r.left + 16, y, vw + 4, ah, C::white, 0, 0, a);
        if (!s.empty()) text(s, sf, r.left + 16 + vw + 8, y + 1, lw[i] - 32 - vw - 8 + 2, ah, C::white, 0, 0, a * .8f);
        if (on) fill(rc(r.left, r.bottom - 3, lw[i], 3), C::white);
        hit(id, r, F_FOCUS | F_HAND, 8);
    }
    rt_->PopAxisAlignedClip();
    clip_ = saved;

    float nx = lx0 + viewW + (n ? 6 : 0);
    arrow(ID_NEXT, nx, Icon::ChevR, canNext);
    // .tab.add
    D2D1_RECT_F ar = rc(nx + ah + 12, y, ah, ah);
    float hv = fx(K_HOVER | ID_ADD, hover_ == ID_ADD || menu_ == 1 ? 1.f : 0.f, ar);
    glass(ar, 12, .62f + .2f * hv, .12f + .16f * hv);
    icon(Icon::Plus, ar.left + 12, ar.top + 12, 16, C::white);
    hit(ID_ADD, ar, F_FOCUS | F_HAND, 12);
    if (menu_ == 1) menuAnchor_ = ar;
    if (keys && n > 1) {
        // .kbd: Q and E switch builds
        Font kf{ Face::UI, 11, 700, false };
        float kx = ar.right + 6 + 10;
        for (const wchar_t* k : { L"Q", L"E" }) {
            D2D1_RECT_F r = rc(kx, y + 9, 22, 22);
            box(r, 6, 0, 0, C::white, .28f);
            fill(rc(kx + 4, r.bottom - 2, 14, 1), C::white, .28f);   // border-bottom: 2px
            text(k, kf, kx, r.top, 22, 21, C::white, 1, 0, .8f);
            kx += 22 + 4;
        }
    }
}

// ------------------------------------------------------------------------------------------------ poster title (bottom left)
// Bottom up: the meta line (Fortnite version · CL · UE · the season's skin), the season's name (italic 900, 92 DIPs
// at most, smaller to fit), the season pill, and the notices above it.
void App::drawPoster(float x, float right, float top, float bottom) {
    float w = std::max(0.f, right - x);
    if (!loaded_) {   // skeleton while the host loads
        skeleton(3, rc(x, bottom - 19 - 10 - 83, std::min(w, 420.f), 83));
        skeleton(4, rc(x, bottom - 19, std::min(w, 300.f), 19));
        return;
    }
    const Build* b = sel();
    if (!b) {
        // empty: how to add a build
        const int ids[3] = { ID_EMPTY_ZIP, ID_EMPTY_FOLDER, ID_EMPTY_ROOT };
        const wchar_t* lb[3] = { L"Add from zip…", L"Add from folder…", L"Change builds folder…" };
        const Icon ic[3] = { Icon::Zip, Icon::Folder, Icon::Folder };
        float bwid[3]; int row[3], rows = 1; float cx = 0;
        for (int i = 0; i < 3; i++) {
            bwid[i] = gbutton(ids[i], 0, 0, 40, lb[i], ic[i], false, true);
            if (cx > 0 && cx + bwid[i] > w) { rows++; cx = 0; }
            row[i] = rows - 1; cx += bwid[i] + 8;
        }
        float by = bottom - rows * 40 - (rows - 1) * 8;
        cx = x; int r0 = 0;
        for (int i = 0; i < 3; i++) {
            if (row[i] != r0) { r0 = row[i]; cx = x; }
            gbutton(ids[i], cx, by + row[i] * 48, 40, lb[i], ic[i], false);
            cx += bwid[i] + 8;
        }
        Font hf{ Face::UI, 12.5f, 400, false };
        std::wstring help = L"Builds live in **" + st_.root + L"**. A downloaded zip is extracted there; a folder is moved or copied.";
        float hw = std::min(w, 520.f), th = rich(help, hf, 0, 0, hw, 18.75f, C::ink2, C::white, 0, false);
        float y = by - 20 - th;
        rich(help, hf, x, y, hw, 18.75f, C::ink2, C::white);
        Font v100{ Face::Display, 100, 900, true };
        float vw = textW(L"NO BUILD", v100, -3.f);
        float size = std::max(36.f, std::min(92.f, vw > 0 ? (w - 8) / vw * 100 : 92.f));
        float vy = y - 14 - .9f * size;
        text(L"NO BUILD", Font{ Face::Display, size, 900, true }, x, vy, w + size, .9f * size, C::white, 0, -.03f * size);
        if (vy - 12 - 21 > top) pill(x, vy - 12 - 21, L"GET STARTED", -1);
        return;
    }

    const SeasonArt& sa = seasonArt(b->major, b->minor);
    std::wstring ver = verLabel(*b);
    // notices (.notice of the page): why it cannot launch, or what is missing, with their buttons
    struct Notice { int cls; std::wstring html; int btn; const wchar_t* label; };
    std::vector<Notice> notices;
    if (b->lobby == Support::No) notices.push_back({ 2, L"**Fortnite " + ver + L" cannot be launched.** " + b->lobbyWhy + L".", 0, nullptr });
    else {
        if (b->firewall != L"OK") notices.push_back({ 1, L"**Firewall rules missing.** " + b->firewall + L". They are created before launch (administrator prompt).", ID_NOTICE_FW, L"Create now" });
        if (!b->cosmetics) notices.push_back({ 1, L"**No cosmetics list yet.** Only the default items will show in the locker.", ID_NOTICE_COS, L"Generate" });
    }
    float nw = std::min(w, 520.f);
    Font nf{ Face::UI, 12.5f, 400, false };
    auto noticeH = [&](const Notice& n, float* tw, float* nbw) {
        *nbw = n.btn ? gbutton(n.btn, 0, 0, 28, n.label, Icon::Count, false, true) : 0;
        *tw = nw - 14 - 16 - 10 - (n.btn ? *nbw + 12 : 0) - 14;
        return 24 + std::max(rich(n.html, nf, 0, 0, *tw, 18.75f, C::warnText, C::white, 0, false), n.btn ? 28.f : 18.f);
    };
    float notesH = 0;
    for (auto& n : notices) { float a, c; notesH += noticeH(n, &a, &c) + 10; }

    // .m: "Fortnite 14.30 · CL 14456520 · UE 4.26 · Thor" (the folder too when it is not the version)
    Font mf{ Face::UI, 13, 400, false };
    const float mlh = 18.85f;
    std::wstring meta = L"Fortnite " + ver;
    if (b->cl) meta += fmt(L" · CL %u", b->cl);
    if (!b->engine.empty()) meta += L" · UE " + b->engine;
    if (sa.hero && b->major) meta += std::wstring(L" · ") + sa.hero;
    if (b->name.rfind(ver, 0) != 0) meta += L" · " + b->name;
    float my = bottom - mlh;
    text(meta, mf, x, my, w, mlh, C::white, 0, 0, .7f);
    hit(ID_META, rc(x, my, std::min(w, textW(meta, mf)), mlh), 0, 0);   // tooltip only: the build's folder

    // .n: the season's name, as big as fits the width and what is left of the height (92 at most)
    std::wstring name = sa.name && *sa.name && b->major ? caps(sa.name) : L"FORTNITE " + ver;
    Font n100{ Face::Display, 100, 900, true };
    float nw100 = textW(name, n100, -3.f);
    float byW = nw100 > 0 ? (w - 8) / nw100 * 100 : 92.f;   // the italic leans out: a little room on the right
    float room = my - 10 - top - notesH;
    bool showPill = b->major != 0;
    float size = std::min({ 92.f, byW, (room - (showPill ? 12 + 21 : 0)) / .9f });
    if (size < 40 && showPill) { showPill = false; size = std::min({ 92.f, byW, room / .9f }); }
    if (size < 24) return;
    float ny = my - 10 - .9f * size;
    text(name, Font{ Face::Display, size, 900, true }, x, ny, w + size, .9f * size, C::white, 0, -.03f * size);
    if (showPill) { ny -= 12 + 21; pill(x, ny, caps(season(b->major, b->minor)), -1); }
    ny -= 12;
    for (auto it = notices.rbegin(); it != notices.rend(); ++it) {
        float tw, nbw, h = noticeH(*it, &tw, &nbw);
        ny -= h;
        if (ny < top) break;
        D2D1_RECT_F r = rc(x, ny, nw, h);
        uint32_t tc = it->cls == 2 ? C::badText : C::warnText;
        glass(r, 12, .72f);
        fill(r, it->cls == 2 ? C::bad : C::warn, .14f, 12);
        icon(Icon::Warn, x + 14, ny + 12 + 1.4f, 16, tc);
        rich(it->html, nf, x + 14 + 16 + 10, ny + 12, tw, 18.75f, tc, C::white);
        if (it->btn) gbutton(it->btn, r.right - 14 - nbw, ny + (h - 28) / 2, 28, it->label, Icon::Count, false);
        ny -= 10;
    }
}

// ------------------------------------------------------------------------------------------------ dock (bottom right)
// .dock: the match summary (or the settings sheet in its place), the status line and Launch, from the bottom up.
// Returns its top (the toasts go above it).
float App::drawDock(float x, float w, float top, float bottom) {
    playShown_ = false;
    sheetRect_ = {};
    float lh = std::max(68.f, std::min(112.f, (H_ - TB) * .1053f));
    D2D1_RECT_F pr = rc(x, bottom - lh, w, lh);
    if (!loaded_) {
        skeleton(5, pr);
        float sy = pr.top - 12 - 19 - 14 - 108;
        if (sy >= top) { skeleton(6, rc(x, sy, w, 108)); return sy; }
        return pr.top;
    }
    const Build* b = sel();
    if (!b) return bottom;
    const SeasonArt& sa = seasonArt(b->major, b->minor);
    // the summary as it fits: with its chips, without them, or only a Match settings button on the status line
    measuring_ = true;
    float oh = okline(x + 4, 0, w - 4), full = summary(x, 0, w, true), compact = summary(x, 0, w, false);
    int mode = sheetOpen_ ? 0 : pr.top - 12 - oh - 14 - full >= top ? 2 : pr.top - 12 - oh - 14 - compact >= top ? 1 : 3;
    const float cog = 28;
    if (mode == 3) oh = std::max(cog, okline(x + 4, 0, w - 4 - cog - 10));
    measuring_ = false;
    float okTop = pr.top - 12 - oh;
    okline(x + 4, okTop, w - 4 - (mode == 3 ? cog + 10 : 0));
    float dockTop = okTop;
    if (sheetOpen_) dockTop = drawSheet(x, w, top, okTop + 1);   // .sheet{bottom:190px}: on the status line's top
    else if (mode == 3) {
        D2D1_RECT_F cr = rc(x + w - cog, okTop + (oh - cog) / 2, cog, cog);
        float hv = fx(K_HOVER | ID_SHEET, hover_ == ID_SHEET ? 1.f : 0.f, cr);
        fill(cr, C::white, .12f + .08f * hv, 8);
        icon(Icon::Cog, cr.left + 6, cr.top + 6, 16, C::white);
        hit(ID_SHEET, cr, F_FOCUS | F_HAND | (b->lobby == Support::No ? F_DISABLED : 0), 8);
    } else {
        float ch = mode == 2 ? full : compact;
        summary(x, okTop - 14 - ch, w, mode == 2);
        dockTop = okTop - 14 - ch;
    }
    drawLaunch(pr, sa.c[0]);
    return dockTop;
}

// .okline: a dot by level, "Ready" (or the session's state) and what the build has: lobby · local match · cosmetics ·
// firewall, each with its reason as the tooltip (the old status cells). Wraps under itself. Returns the height.
float App::okline(float x, float y, float w) {
    const Build* b = sel();
    Font f{ Face::UI, 13, 400, false }, fb{ Face::UI, 13, 600, false };
    const float lh = 18.85f;
    auto statusDot = [&](float cy, uint32_t c) {
        if (measuring_) return;
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 3.5f, cy + lh / 2), 5.5f, 5.5f), br(c, .35f));
        dot(x + 3.5f, cy + lh / 2, 7, c);
    };
    const float x0 = x + 7 + 10;
    if (se_.state == SessionState::Error) {
        statusDot(y, C::bad);
        std::wstring m = L"**Failed** " + (se_.message.empty() ? L"Launch failed" : se_.message) + L" **Details in Activity.**";
        return std::max(lh, rich(m, f, x0, y, std::max(40.f, x + w - x0), lh, C::badText, C::white, 0, !measuring_));
    }
    // the flow: pieces of text (and the launch stages' glyphs) separated by " · " or a dash, wrapping under the label
    struct Seg { std::wstring s; uint32_t c; float a; int id; int flags; int glyph; int sep; bool bold; };   // glyph: 1 ok, 2 bad, 3 spinner, 4 waiting; sep: 1 " · ", 2 dash, 3 gap
    std::vector<Seg> segs;
    const float mut = .62f;
    if (busy()) {
        static const struct { int cls; const wchar_t* label; } MP[] = {
            { -1, L"" }, { -1, L"Match ready" }, { 0, L"In the match" }, { 0, L"In the match" }, { 0, L"In the match" },
            { 0, L"In the match" }, { -1, L"Match over" }, { 2, L"Server stopped" } };
        bool ph = se_.state == SessionState::Running && se_.phase != MatchPhase::None;
        int pcls = ph ? MP[(int)se_.phase].cls : 0;
        std::wstring label = se_.state == SessionState::Stopping ? L"Stopping" : se_.state == SessionState::Launching ? L"Launching" : ph ? MP[(int)se_.phase].label : L"Running";
        if (se_.state == SessionState::Running) statusDot(y, pcls == 2 ? C::bad : GREEN);
        else if (!measuring_) spinner(x - 2, y + (lh - 11) / 2, 11);
        segs.push_back({ label, C::white, 1, 0, 0, 0, 0, true });
        if (se_.state == SessionState::Stopping) segs.push_back({ L"Stopping the game and the local servers…", C::white, mut, 0, 0, 0, 3, false });
        else if (ph) segs.push_back({ se_.matchDetail, pcls == 0 ? C::white : pcls == 2 ? C::badText : C::white, pcls == 0 ? 1.f : pcls == 2 ? 1.f : mut, 0, 0, 0, 3, false });
        else {
            struct St { Stage s; const wchar_t* l; } steps[3] = { { se_.backend, L"Backend" }, { se_.game, L"Game" }, { se_.server, L"Match server" } };
            int nst = se_.hasServer ? 3 : 2;
            for (int i = 0; i < nst; i++) {
                Stage s = steps[i].s;
                int g = s == Stage::Ok ? 1 : s == Stage::Bad ? 2 : s == Stage::Run ? 3 : 4;
                segs.push_back({ steps[i].l, C::white, s == Stage::Ok || s == Stage::Run ? 1.f : mut, 0, 0, g, i ? 2 : 3, false });
            }
        }
    } else if (b && b->lobby == Support::No) {
        statusDot(y, C::bad);
        segs.push_back({ L"Not supported", C::white, 1, 0, 0, 0, 0, true });
        segs.push_back({ b->lobbyWhy, C::badText, 1, ID_CELL0, 0, 0, 3, false });
    } else if (b) {
        int lv = level(*b);
        statusDot(y, lv == 0 ? GREEN : C::warn);
        segs.push_back({ L"Ready", C::white, 1, 0, 0, 0, 0, true });
        bool exp = b->lobby == Support::Experimental;
        segs.push_back({ exp ? L"Lobby (experimental)" : L"Lobby", exp ? C::warnText : C::white, exp ? 1.f : mut, ID_CELL0, 0, 0, 3, false });
        bool mexp = b->match == Support::Experimental;
        segs.push_back({ !matchOk(b) ? L"no local match" : mexp ? L"local match (experimental)" : L"local match", mexp && matchOk(b) ? C::warnText : C::white,
                         mexp && matchOk(b) ? 1.f : mut, ID_CELL0 + 1, 0, 0, 1, false });
        segs.push_back({ b->cosmetics ? thousands(b->cosmetics) + L" cosmetics" : L"no cosmetics list", b->cosmetics ? C::white : C::warnText, b->cosmetics ? mut : 1.f, ID_CELL0 + 2, 0, 0, 1, false });
        if (b->port) segs.push_back({ L"Import ›", C::white, 1, ID_P_CELL, F_FOCUS | F_HAND, 0, 3, false });
        bool fw = b->firewall == L"OK";
        segs.push_back({ fw ? L"firewall local only" : L"firewall rules missing", fw ? C::white : C::badText, fw ? mut : 1.f, ID_CELL0 + 3, 0, 0, 1, false });
    }
    float sepW = textW(L" · ", f);
    float cx = x0, cy = y;
    for (auto& s : segs) {
        const Font& ft = s.bold ? fb : f;
        float sw = s.sep == 1 ? sepW : s.sep == 2 ? 26.f : s.sep == 3 ? 10.f : 0.f;
        float gw = s.glyph ? (s.glyph == 4 ? 8.f : s.glyph == 3 ? 14.f : 16.f) + (s.s.empty() ? 0 : 6) : 0;
        float tw = s.s.empty() ? 0 : textW(s.s, ft);
        if (cx > x0 && cx + sw + gw + tw > x + w + 0.5f) { cx = x0; cy += lh; sw = 0; }
        if (s.sep == 1 && sw > 0) text(L" · ", f, cx, cy, sw + 1, lh, C::white, 0, 0, mut);
        if (s.sep == 2 && sw > 0) fill(rc(cx + 6, cy + lh / 2 - .5f, 14, 1), C::white, .3f);
        cx += sw;
        float sx = cx;
        if (s.glyph == 1) icon(Icon::Check, cx, cy + (lh - 16) / 2, 16, GREEN);
        else if (s.glyph == 2) icon(Icon::X, cx, cy + (lh - 16) / 2, 16, C::bad);
        else if (s.glyph == 3 && !measuring_) spinner(cx, cy + (lh - 14) / 2, 14);
        else if (s.glyph == 4) dot(cx + 4, cy + lh / 2, 8, C::dim);
        cx += gw;
        float vis = std::min(tw + 1, x + w - cx);
        if (tw > 0) text(s.s, ft, cx, cy, vis, lh, s.c, 0, 0, s.a);
        if (s.id == ID_P_CELL) {
            float hv = fx(K_HOVER | ID_P_CELL, hover_ == ID_P_CELL ? 1.f : 0.f, rc(cx, cy, tw, lh));
            fill(rc(cx, cy + lh - 2, std::max(0.f, vis - 8), 1), C::white, .2f + .5f * hv);
        }
        if (s.id) hit(s.id, rc(sx, cy, cx - sx + std::max(0.f, vis), lh), s.flags, 4);
        cx += tw;
    }
    return cy - y + lh;
}

// .sum: what Launch starts (title, the switches as chips) and the Match settings button. Returns the height.
float App::summary(float x, float y, float w, bool withChips) {
    const Build* b = sel();
    const Config& c = st_.config;
    bool dis = b->lobby == Support::No, hasMatch = matchOk(b);
    const float px = 18, iw = w - 2 * px - 2;
    struct Chip { Icon ic; std::wstring s; uint32_t c; };
    std::vector<Chip> chips;
    if (hasMatch) {
        chips.push_back({ Icon::Bots, c.bots == 1 ? std::wstring(L"1 bot") : fmt(L"%d bots", c.bots), C::white });
        if (c.infiniteMaterials) chips.push_back({ Icon::Count, L"Infinite materials", C::white });
        if (c.infiniteAmmo) chips.push_back({ Icon::Count, L"Infinite ammo", C::white });
        chips.push_back({ Icon::Count, c.lateGame ? L"Late game" : L"Late game off", C::white });
        if (c.botsAI) chips.push_back({ Icon::Warn, L"Bot AI on", C::warnText });
    } else chips.push_back({ Icon::Count, L"Lobby, locker and styles", C::white });
    if (!c.displayName.empty()) chips.push_back({ Icon::Lobby, c.displayName, C::white });
    Font cf{ Face::UI, 12, 600, false };
    std::vector<D2D1_RECT_F> cr;
    float cx = 0, cy = 0;
    for (auto& ch : chips) {
        float cw = std::min(iw, 10 + (ch.ic != Icon::Count ? 16 + 6 : 0) + textW(ch.s, cf) + 10);
        if (cx > 0 && cx + cw > iw) { cx = 0; cy += 28 + 6; }
        cr.push_back(rc(cx, cy, cw, 28));
        cx += cw + 6;
    }
    if (!withChips) chips.clear();
    float h = 1 + 16 + 34 + (chips.empty() ? 0 : 12 + cy + 28) + 16 + 1;
    if (measuring_) return h;
    D2D1_RECT_F r = rc(x, y, w, h);
    if (dis) { alpha_ = .5f; noHit_ = true; }
    frost(r, 18);
    float ty = y + 17, lx = x + 1 + px;
    Font bf{ Face::UI, 13, 600, false };
    float bw = 12 + 16 + 8 + textW(L"Match settings", bf) + 12;
    D2D1_RECT_F brr = rc(x + w - 1 - px - bw, ty, bw, 34);
    float hv = fx(K_HOVER | ID_SHEET, hover_ == ID_SHEET ? 1.f : 0.f, brr);
    fill(brr, C::white, .12f + .08f * hv, 10);
    icon(Icon::Cog, brr.left + 12, ty + 9, 16, C::white);
    text(L"Match settings", bf, brr.left + 36, ty, bw - 48 + 1, 34, C::white);
    hit(ID_SHEET, brr, F_FOCUS | F_HAND, 10);
    // .md: what the match is (modes are picked in the game: Practice or Battle Royale)
    std::wstring t = hasMatch ? L"LOCAL MATCH" : L"LOBBY ONLY";
    float tw30 = textW(t, Font{ Face::Display, 30, 900, true }), avail = brr.left - 12 - lx;
    float ts = std::max(14.f, std::min(30.f, tw30 > 0 ? (avail - 8) / tw30 * 30 : 30.f));
    text(t, Font{ Face::Display, ts, 900, true }, lx, ty, avail + 4, 34, C::white);
    float chy = ty + 34 + 12;
    for (size_t i = 0; i < chips.size(); i++) {
        D2D1_RECT_F q = cr[i];
        q = { lx + q.left, chy + q.top, lx + q.right, chy + q.bottom };
        fill(q, C::white, .1f, 14);
        float tx = q.left + 10;
        if (chips[i].ic != Icon::Count) { icon(chips[i].ic, tx, q.top + 6, 16, chips[i].c); tx += 16 + 6; }
        text(chips[i].s, cf, tx, q.top, q.right - 10 - tx, 28, chips[i].c);
    }
    alpha_ = 1; noHit_ = false;
    return h;
}

// .sheet: the match settings in place of the summary, above the status line (Esc or a click outside closes it); its
// content scrolls when the window is short. Returns its top.
float App::drawSheet(float x, float w, float top, float bottom) {
    measuring_ = true;
    float ch = sheetBody(x + 23, 20, w - 46) + 20;   // padding 20 22 (+ the 1px border)
    measuring_ = false;
    float h = std::max(0.f, std::min(ch + 2, bottom - top));   // never over the top row: the content scrolls
    D2D1_RECT_F r = rc(x, bottom - h, w, h);
    sheetRect_ = r;
    float a = tw(K_SHEET, 1, grow(r, 2));   // fades in when it opens (180 ms)
    alpha_ = a;
    frost(r, 20);
    float viewTop = r.top + 1, viewBottom = r.bottom - 1, viewH = std::max(0.f, viewBottom - viewTop);
    contentView_ = viewH;
    contentH_ = ch;
    bool sb = contentH_ > viewH + 0.5f;
    contentScroll_ = std::max(0.f, std::min(contentScroll_, std::max(0.f, contentH_ - viewH)));
    contentScroll_ = roundf(contentScroll_ * scale_) / scale_;
    contentClip_ = clip_ = { x + 1, viewTop, x + w - 1, viewBottom };
    rt_->PushAxisAlignedClip(clip_, D2D1_ANTIALIAS_MODE_ALIASED);
    oy_ = viewTop - contentScroll_;
    base_ = D2D1::Matrix3x2F::Translation(0, oy_);
    rt_->SetTransform(base_);
    inContent_ = true;
    sheetBody(x + 23, 20, w - 46);
    inContent_ = false;
    base_ = D2D1::Matrix3x2F::Identity(); oy_ = 0;
    rt_->SetTransform(base_);
    rt_->PopAxisAlignedClip();
    clip_ = { -1e9f, -1e9f, 1e9f, 1e9f };
    if (sb) drawScrollbar(ID_SB_CONTENT, x + w - 12, viewTop + 10, viewH - 20, contentScroll_, contentH_, viewH);
    alpha_ = 1;
    return r.top;
}

float App::sheetBody(float x, float y0, float w) {
    float y = y0;
    const Build* b = sel();
    const Config& c = st_.config;
    bool dis = b->lobby == Support::No, hasMatch = matchOk(b);
    Font lf{ Face::UI, 10.5f, 600, false }, mf{ Face::UI, 12, 400, false }, rf{ Face::UI, 13.5f, 400, false };
    // header: MATCH SETTINGS · 14.30, and "Esc to close" (a button too)
    const float hh = 17.4f;
    float ew = textW(L"Esc to close", mf);
    text(L"MATCH SETTINGS · " + verLabel(*b), lf, x, y, w - ew - 12, hh, C::white, 0, .84f, .62f);
    D2D1_RECT_F er = rc(x + w - ew, y, ew, hh);
    float ev = fx(K_HOVER | ID_SHEETX, hover_ == ID_SHEETX ? 1.f : 0.f, er);
    text(L"Esc to close", mf, er.left, y, ew + 1, hh, C::white, 0, 0, .62f + .38f * ev);
    hit(ID_SHEETX, grow(er, 3), F_FOCUS | F_HAND, 6);
    y += hh + 10;
    float a0 = alpha_;
    if (dis) { alpha_ = .5f * a0; noHit_ = true; }

    // .modes: Practice and Battle Royale (both picked in the game), or lobby only
    Font df{ Face::UI, 11.5f, 400, false };
    if (hasMatch) {
        float mw = (w - 8) / 2;
        const wchar_t* mt[2] = { L"PRACTICE", L"BATTLE ROYALE" };
        const wchar_t* md[2] = { L"Creative · Playground", L"Solo to Squads" };
        float t20 = std::max(textW(mt[0], Font{ Face::Display, 20, 900, true }), textW(mt[1], Font{ Face::Display, 20, 900, true }));
        float ts = std::max(12.f, std::min(20.f, t20 > 0 ? (mw - 28 - 4) / t20 * 20 : 20.f));
        for (int i = 0; i < 2; i++) {
            D2D1_RECT_F r = rc(x + i * (mw + 8), y, mw, 74);
            // linear-gradient(160deg, ...): the line through the center along (sin 160, -cos 160)
            if (modeBr_[i] && !measuring_) {
                const float dx = 0.3420201f, dy = 0.9396926f;
                float len = mw * dx + 74 * dy, gx = (r.left + r.right) / 2, gy = (r.top + r.bottom) / 2;
                modeBr_[i]->SetStartPoint(D2D1::Point2F(gx - dx * len / 2, gy - dy * len / 2));
                modeBr_[i]->SetEndPoint(D2D1::Point2F(gx + dx * len / 2, gy + dy * len / 2));
                modeBr_[i]->SetOpacity(alpha_);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), modeBr_[i]);
            }
            text(mt[i], Font{ Face::Display, ts, 900, true }, r.left + 14, r.bottom - 12 - 16.7f - 24.2f, mw - 28 + ts, 24.2f, C::white);
            text(md[i], df, r.left + 14, r.bottom - 12 - 16.7f, mw - 28, 16.7f, C::white, 0, 0, .85f);
            hit(ID_MODE0 + i, r, 0, 0);   // tooltip only
        }
    } else {
        D2D1_RECT_F r = rc(x, y, w, 74);
        fill(r, C::white, .06f, 12);
        text(L"LOBBY ONLY", Font{ Face::Display, 20, 900, true }, r.left + 14, r.bottom - 12 - 16.7f - 24.2f, w - 28, 24.2f, C::white);
        text(L"No local match on this build: lobby, locker and styles", df, r.left + 14, r.bottom - 12 - 16.7f, w - 28, 16.7f, C::white, 0, 0, .75f);
        hit(ID_MODE0, r, 0, 0);
    }
    y += 74 + 14;

    auto row = [&](const wchar_t* label, float rightW) { fill(rc(x, y, w, 1), C::white, .1f); text(label, rf, x, y, w - rightW - 12, 44, C::white); };
    if (hasMatch) {
        // bots: the slider, then the number (an EDIT while it has the focus)
        row(L"Bots", std::min(280.f, w - textW(L"Bots", rf) - 24));
        int bots = botsDrag_ >= 0 ? botsDrag_ : c.bots;
        float gw = std::min(280.f, w - textW(L"Bots", rf) - 24), gx = x + w - gw;
        const float numW = 34;
        float cy = y + 22, tx0 = gx, tw = std::max(30.f, gw - 12 - numW);
        float kx = tx0 + tw * bots / 99.f;
        fill({ tx0, cy - 2, tx0 + tw, cy + 2 }, C::white, .16f, 2);
        if (!measuring_) {
            if (rangeBr_) {   // linear-gradient(90deg, #8c6bff, #b79bff) up to the knob
                rangeBr_->SetStartPoint(D2D1::Point2F(tx0, 0)); rangeBr_->SetEndPoint(D2D1::Point2F(std::max(tx0 + 1, kx), 0));
                rangeBr_->SetOpacity(alpha_);
                rt_->FillRoundedRectangle(D2D1::RoundedRect({ tx0, cy - 2, std::max(tx0 + 4, kx), cy + 2 }, 2, 2), rangeBr_);
            }
            if (hover_ == ID_RANGE || drag_ == ID_RANGE) rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), 12, 12), br(C::white, 0.12f));
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy + 1), 8, 8), br(0x000000, .18f));   // 0 1px 4px rgba(0,0,0,.4)
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), 7, 7), br(C::white));
        }
        hit(ID_RANGE, rc(tx0 - 8, y + 8, tw + 16, 28), F_FOCUS | F_HAND, 6);
        EditBox& e = edits_[1];
        D2D1_RECT_F nr = rc(x + w - numW, y + 6, numW, 32);
        bool editing = hwnd_ && (e.want || GetFocus() == e.hwnd);
        if (!measuring_) { e.r = { nr.left + 2, nr.top + 1 + oy_, nr.right - 2, nr.bottom - 1 + oy_ }; e.clip = clip_; e.show = hwnd_ && !dis; }
        if (editing && !dis) box(nr, 8, FIELD, 1, C::white, .42f);
        else text(std::to_wstring(bots), Font{ Face::Display, 24, 900, true }, nr.left - 6, y, numW + 6, 44, C::white, 2);
        hit(ID_BOTS, nr, F_FOCUS | F_EDIT, 8);
        y += 44;

        // .tg: the four switches (their details are the tooltips)
        struct Tg { const wchar_t* label; bool on; } tg[4] = {
            { L"Bots use the game's AI", c.botsAI }, { L"Late game", c.lateGame }, { L"Infinite materials", c.infiniteMaterials }, { L"Infinite ammo", c.infiniteAmmo },
        };
        for (int i = 0; i < 4; i++) {
            int id = ID_TG0 + i;
            row(tg[i].label, 30);
            float sx = x + w - 30, sy = y + (44 - 17) / 2;
            // background and the knob 180 ms var(--ease)
            float v = fx(K_SWITCH | id, tg[i].on ? 1.f : 0.f, rc(sx, sy, 30, 17), T2, Curve::Ui);
            if (v < 1) fill(rc(sx, sy, 30, 17), C::white, .18f * (1 - v), 8.5f);
            if (v > 0) fill(rc(sx, sy, 30, 17), 0x3FC46A, v, 8.5f);
            if (!measuring_) rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx + 2 + 6.5f + 13 * v, sy + 8.5f), 6.5f, 6.5f), br(C::white, .85f + .15f * v));
            hit(id, rc(x - 6, y + 1, w + 12, 43), F_FOCUS | F_HAND, 6);
            y += 44;
            if (i == 0 && c.botsAI) {   // the page's warning when the bots' AI is on
                Font wf{ Face::UI, 11.5f, 400, false };
                std::wstring note = L"**Bot AI is on.** On " + verLabel(*b) + L" it crashes the match server a few seconds after it starts.";
                float th = rich(note, wf, 0, 0, w - 24, 16.5f, C::warnText, C::white, 0, false);
                icon(Icon::Warn, x, y + 0.5f, 16, C::warn);
                rich(note, wf, x + 24, y, w - 24, 16.5f, C::warnText, C::white);
                y += th + 10;
            }
        }
    }

    // player name (a native EDIT on an opaque field)
    row(L"Player name", std::min(220.f, w - textW(L"Player name", rf) - 24));
    {
        EditBox& e = edits_[0];
        bool foc = hwnd_ && GetFocus() == e.hwnd;
        float fw = std::min(220.f, w - textW(L"Player name", rf) - 24);
        D2D1_RECT_F fr = rc(x + w - fw, y + 6, fw, 32);
        float fv = fx(K_COLOR | ID_NAME, foc ? 1.f : hover_ == ID_NAME ? .45f : 0.f, fr);   // border 120 ms on hover and focus
        box(fr, 9, FIELD, 1, C::white, .1f + .3f * fv);
        D2D1_RECT_F inner = { fr.left + 12, fr.top + 1, fr.right - 12, fr.bottom - 1 };
        if (!measuring_) { e.r = { inner.left, inner.top + oy_, inner.right, inner.bottom + oy_ }; e.clip = clip_; e.show = hwnd_ && !dis; }
        if (!e.show || measuring_) text(c.displayName, Font{ Face::UI, 13, 600, false }, inner.left, fr.top, inner.right - inner.left, 32, C::white);
        hit(ID_NAME, fr, F_FOCUS | F_EDIT, 9);
        y += 44;
    }
    alpha_ = a0; noHit_ = false;
    return y;
}

// .launch: white, as tall as the window allows (68 to 112 DIPs), a glow in the season's color; Stop while a session
// runs. Its hit is added at the end of the frame (the last control in the focus order, like the page).
void App::drawLaunch(D2D1_RECT_F pr, uint32_t glow) {
    const Build* b = sel();
    bool stopStyle = busy();
    bool disabled = stopStyle ? se_.state == SessionState::Stopping : (!b || b->lobby == Support::No);
    playRect_ = pr; playShown_ = true; playDisabled_ = disabled;
    float bw = pr.right - pr.left, bh = pr.bottom - pr.top, k = bh / 112;
    float rr = std::max(16.f, std::min(22.f, bh * .196f));
    playRadius_ = rr;
    D2D1_RECT_F area = grow(pr, 4);
    bool phov = hover_ == ID_PLAY && !disabled;
    // background 120 ms; the hover ring 120 ms; :active scale(.985) 120 ms var(--ease). Text, style and width change at once.
    uint32_t bg = fxc(K_COLOR | ID_PLAY, disabled ? 0x1A1E2B : stopStyle ? (phov ? 0x1C2338 : 0x0E1222) : C::white, area);
    float ring = fx(K_HOVER | ID_PLAY, phov ? 1.f : 0.f, area);
    float press = fx(K_PRESS | ID_PLAY, press_ == ID_PLAY && !disabled ? 1.f : 0.f, area, T1, Curve::Ui);
    // box-shadow: 0 12px 40px <season color> .45
    if (!stopStyle && !disabled) shadow(grow(pr, -4), rr, 40, 12, .6f, glow);
    D2D1_MATRIX_3X2_F saved = base_;
    if (press > 0) {
        float s = 1 - 0.015f * press;
        base_ = D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F((pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2)) * base_;
        rt_->SetTransform(base_);
    }
    if (ring > 0 && !measuring_) rt_->DrawRoundedRectangle(D2D1::RoundedRect(grow(pr, 1.5f), rr + 1.5f, rr + 1.5f), br(C::white, 0.16f * ring), 3);
    if (stopStyle || disabled) box(pr, rr, bg, .9f, C::white, .18f);
    else {
        fill(pr, bg, 1, rr);
        if (!measuring_) rt_->DrawRoundedRectangle(D2D1::RoundedRect(grow(pr, .5f), rr + .5f, rr + .5f), br(C::white, .4f), 1);   // 0 0 0 1px rgba(255,255,255,.4)
    }
    // .play::after sheen (greet wins over hover, like the later CSS rule); out of the button at rest
    float sh = anim_.running(K_GREET) ? tw(K_GREET, -1.2f, area) : anim_.running(K_SHEEN) ? tw(K_SHEEN, -1.2f, area) : -1.2f;
    if (sh > -1.2f && sh < 1.2f && sheen_ && !measuring_) {
        const float dx = 0.9659258f, dy = 0.2588190f;   // 105deg: (sin a, -cos a)
        float len = bw * dx + bh * dy, gx = (pr.left + pr.right) / 2 + sh * bw, gy = (pr.top + pr.bottom) / 2;
        sheen_->SetStartPoint(D2D1::Point2F(gx - dx * len / 2, gy - dy * len / 2));
        sheen_->SetEndPoint(D2D1::Point2F(gx + dx * len / 2, gy + dy * len / 2));
        sheen_->SetOpacity(alpha_);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(pr, rr, rr), sheen_);
    }
    // ▶ LAUNCH <small>version</small>: italic 900 64px at 112 tall (scaled with the button), gap 14px; the version 20px at .6
    float ps = std::max(34.f, 64 * k), ss = std::max(13.f, 20 * k);
    Font pf{ Face::Display, ps, 900, true }, sf{ Face::Display, ss, 900, true };
    const wchar_t* pl = stopStyle ? L"STOP" : L"LAUNCH";
    std::wstring ver = stopStyle || !b ? L"" : verLabel(*b);
    float iconS = std::max(40.f, 53.3f * k) * (stopStyle ? .75f : 1.f), gap = std::max(10.f, 14 * k);
    float g0 = 4.5f / 16 * iconS, gw = (stopStyle ? 7.f : 8.7f) / 16 * iconS;   // the glyph inside the icon box
    float ptw = textW(pl, pf), stw = ver.empty() ? 0 : textW(ver, sf);
    float cw = gw + gap + ptw + (stw > 0 ? gap + stw : 0);
    if (cw > bw - 32) { stw = 0; cw = gw + gap + ptw; }
    if (cw > bw - 32) { ps = std::max(20.f, ps * (bw - 32) / cw); pf.size = ps; ptw = textW(pl, pf); cw = gw + gap + ptw; }
    uint32_t pc = disabled ? 0x6B7080 : stopStyle ? C::white : INK;
    float cx = pr.left + (bw - cw) / 2, cy = (pr.top + pr.bottom) / 2;
    icon(stopStyle ? Icon::X : Icon::Play, cx - g0, cy - iconS / 2, iconS, pc);
    if (text(pl, pf, cx + gw + gap, pr.top, ptw + 4, bh, pc)) issues.push_back(L"clipped play");
    if (stw > 0) text(ver, sf, cx + gw + gap + ptw + gap, pr.top, stw + 2, bh, disabled ? 0x6B7080 : INK, 0, 0, disabled ? 1.f : .6f);
    base_ = saved;
    rt_->SetTransform(base_);
}

void App::drawTitle(float W) {
    fill({ 0, 0, W, TB }, C::bg);
    hline(0, W, TB - 1, C::line2);
    uint32_t tc = active_ ? C::text : C::dim;
    if (logoBrush_) {
        logoBrush_->SetTransform(D2D1::Matrix3x2F::Translation(16, 10));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(rc(16, 10, 20, 20), 5, 5), logoBrush_);
    }
    Font f{ Face::UI, 13, 600, false };
    float x = 16 + 20 + 8;
    float w1 = textW(L"Radarnite ", f, -0.13f);
    text(L"Radarnite", f, x, 0, w1, TB - 1, tc, 0, -0.13f);
    text(L"Launcher", f, x + w1, 0, 100, TB - 1, C::muted, 0, -0.13f);   // .mark span b keeps its colour when inactive
    x += w1 + textW(L"Launcher", f, -0.13f) + 12;
    if (W > 900 && loaded_ && !st_.builds.empty()) {
        bool fw = true; for (auto& b : st_.builds) if (b.firewall != L"OK") fw = false;
        dot(x + 8 + 4, (TB - 1) / 2, 8, fw ? C::ok : C::warn);
        text(fw ? L"Offline · 127.0.0.1 only" : L"Firewall rules missing", Font{ Face::UI, 12, 400, false }, x + 8 + 8 + 6, 0, std::max(0.f, W - 138 - x - 30), TB - 1, C::dim);
    }
    float bx = W - 138;
    const Icon ics[3] = { Icon::WMin, maximized_ ? Icon::WRes : Icon::WMax, Icon::WClose };
    for (int i = 0; i < 3; i++) {
        int id = ID_MIN + i;
        D2D1_RECT_F r = rc(bx + 46 * i, 0, 46, TB - 1);
        bool hov = hover_ == id, x = i == 2;
        // .wc button: background and color 120 ms; inactive window: dim, except the close button's hover
        float hv = fx(K_HOVER | id, hov ? 1.f : 0.f, r);
        if (hv > 0) fill(r, x ? C::closeRed : C::hover, hv);
        uint32_t c = fxc(K_COLOR | id, hov && x ? C::white : !active_ ? C::dim : hov ? C::text : C::muted, r);
        icon(ics[i], r.left + 18, (TB - 1 - 10) / 2, 10, c);
        hit(id, r, F_FOCUS | F_HAND | F_RING_IN, 0);
    }
}

void App::drawActivity(float x, float y, float w, float h) {
    fill({ x, y, x + w, y + h }, C::bg);
    hline(x, x + w, y, C::line2);
    hit(ID_GRIP, { x, y - 3, x + w, y + 3 }, 0, 0);
    float hy = y + 1 + 3 + 8, hh = 30;   // the grip (6px, margin-top -3px) takes 3px of the panel
    float hx = x + 24;
    Font tf{ Face::UI, 13, 600, false };
    text(L"Activity", tf, hx, hy, 100, hh, C::text);
    hx += textW(L"Activity", tf) + 8 + 8;
    // filters
    int n[3] = { (int)logs_.size(), 0, 0 };
    for (auto& l : logs_) { if (l.lvl == 1) n[1]++; else if (l.lvl == 2) n[2]++; }
    const wchar_t* fl[3] = { L"All", L"Warnings", L"Errors" };
    Font ff{ Face::UI, 12, 500, false }, fn{ Face::UI, 12, 500, false, true };   // .fseg .n: tabular-nums
    float segW = 2 + 2 + 2 * 2;
    float bwid[3];
    for (int i = 0; i < 3; i++) { bwid[i] = 20 + textW(fl[i], ff) + 6 + textW(std::to_wstring(n[i]), fn); segW += bwid[i]; }
    box(rc(hx, hy, segW, 30), 8, C::bg2, 1, C::line2);
    float fx = hx + 1 + 2;
    for (int i = 0; i < 3; i++) {
        int id = ID_F0 + i;
        bool on = filter_ == i;
        if (on) fill(rc(fx, hy + 3, bwid[i], 24), C::hover, 1, 6);
        uint32_t col = on || hover_ == id ? C::text : C::muted;
        text(fl[i], ff, fx + 10, hy + 3, bwid[i], 24, col);
        text(std::to_wstring(n[i]), fn, fx + 10 + textW(fl[i], ff) + 6, hy + 3, 60, 24, C::dim);
        hit(id, rc(fx, hy + 3, bwid[i], 24), F_FOCUS | F_HAND, 6);
        fx += bwid[i] + 2;
    }
    // right side: search, copy, folder, clear
    float rx = x + w - 24 - 28;
    const Icon ri[3] = { Icon::Trash, Icon::Folder, Icon::Copy };
    const int rid[3] = { ID_CLEAR, ID_OPENLOGS, ID_COPY };
    for (int i = 0; i < 3; i++) { button(rid[i], rx, hy + 1, nullptr, ri[i], true, true, true); rx -= 28 + 8; }
    float sw = W_ <= 720 ? 120.f : 180.f, sx = rx + 28 - sw;
    if (sx > fx + 8) {
        EditBox& e = edits_[2];
        bool foc = hwnd_ && GetFocus() == e.hwnd;
        box(rc(sx, hy + 1, sw, 28), 8, C::bg2, 1, foc ? C::muted : C::line);   // .search: no hover style, no transition
        D2D1_RECT_F inner = { sx + 11, hy + 2, sx + sw - 11, hy + 28 };
        e.r = inner; e.clip = clip_; e.show = hwnd_ != nullptr;
        if (!e.show) {
            if (q_.empty()) text(L"Filter", Font{ Face::UI, 12, 400, false }, inner.left, hy + 1, inner.right - inner.left, 28, C::dim);
            else text(q_, Font{ Face::UI, 12, 400, false }, inner.left, hy + 1, inner.right - inner.left, 28, C::text);
        }
        hit(ID_SEARCH, rc(sx, hy + 1, sw, 28), F_FOCUS | F_EDIT, 8);
    }

    // virtualized lines
    float ly = hy + hh + 8, lh = y + h - ly;
    logView_ = lh;
    if (rowsDirty_) {
        rows_.clear();
        for (size_t i = 0; i < logs_.size(); i++) {
            const LogEntry& l = logs_[i];
            if ((filter_ == 0 || l.lvl == filter_) && (q_.empty() || l.lower.find(q_) != std::wstring::npos)) rows_.push_back((int)i);
        }
        rowsDirty_ = false;
    }
    Font mf{ Face::Mono, 12, 400, false }, mk{ Face::Mono, 12, 600, false };
    float adv = g_.monoAdvance(12);
    bool sb = logMax_ > 0;
    float textR = x + w - 24 - (sb ? 10 : 0);
    float msgX = x + 24 + 96, msgW = textR - msgX;
    int cols = std::max(1, (int)floorf(msgW / adv));
    rowY_.resize(rows_.size() + 1);
    rowY_[0] = 0;
    for (size_t i = 0; i < rows_.size(); i++) {
        LogEntry& l = logs_[rows_[i]];
        if (l.wrapCols != cols) { l.lines = wrap(l.text, cols, [](size_t, size_t) {}); l.wrapCols = cols; }
        rowY_[i + 1] = rowY_[i] + l.lines * LOG_LH + 2;
    }
    float total = rowY_.back() + 12;
    float maxS = std::max(0.f, total - lh);
    bool sbNow = maxS > 0;
    if (sbNow != sb) { logMax_ = maxS; invalidate(); }
    logMax_ = maxS;
    if (logFollow_) logScroll_ = maxS;
    logScroll_ = std::max(0.f, std::min(logScroll_, maxS));
    logScroll_ = roundf(logScroll_ * scale_) / scale_;
    D2D1_RECT_F lclip = { x, ly, x + w, y + h };
    rt_->PushAxisAlignedClip(lclip, D2D1_ANTIALIAS_MODE_ALIASED);
    if (rows_.empty()) {
        text(logs_.empty() ? L"No activity yet. Launch messages, warnings and errors appear here." : L"Nothing matches the filter.",
             Font{ Face::UI, 12, 400, false }, x + 24, ly + 16, textR - x - 24, LOG_LH, C::dim);
    } else {
        size_t i = std::upper_bound(rowY_.begin(), rowY_.end(), logScroll_) - rowY_.begin();
        i = i ? i - 1 : 0;
        for (; i < rows_.size(); i++) {
            float ry = ly + rowY_[i] - logScroll_;
            if (ry > y + h) break;
            const LogEntry& l = logs_[rows_[i]];
            float top = ry + 1;
            text(l.ts, mf, x + 24, top, 62, LOG_LH, C::dim);
            if (l.lvl) dot(x + 24 + 62 + 8 + 9, top + 9.5f, 6, clsColor(l.lvl));
            else dot(x + 24 + 62 + 8 + 9, top + 9.5f, 6, C::dim);
            uint32_t col = l.key ? C::text : l.lvl == 2 ? C::badText : l.lvl == 1 ? C::warnText : C::ink2;
            const Font& f = l.key ? mk : mf;
            float yy = top;
            wrap(l.text, cols, [&](size_t a, size_t n) {
                if (yy + LOG_LH >= ly && yy <= y + h) text(l.text.substr(a, n), f, msgX, yy, msgW + adv, LOG_LH, col);
                yy += LOG_LH;
            });
        }
    }
    rt_->PopAxisAlignedClip();
    hit(ID_LINES, lclip, F_FOCUS | F_RING_IN, 0);
    if (sbNow) drawScrollbar(ID_SB_LOG, x + w - 10, ly, lh, logScroll_, total, lh);
}

void App::drawMenu() {
    if (!menu_) return;
    int n; const MenuItem* it = menuItems(menu_, &n);
    Font f{ Face::UI, 13, 400, false }, sf{ Face::UI, 11.5f, 400, false };
    float cw = 0, h = 2 + 8;
    for (int i = 0; i < n; i++) {
        if (it[i].sep) { h += 9; continue; }
        h += 34;
        cw = std::max(cw, 10 + 16 + 10 + textW(it[i].text, f) + (it[i].small ? 10 + 16 + textW(it[i].small, sf) : 0) + 10);   // the 10px gap is also before <small>
    }
    float w = std::max(240.f, cw + 8 + 2);
    float right = menuAnchor_.right, left = std::max(4.f, right - w);
    D2D1_RECT_F r = rc(left, menuAnchor_.bottom + 6, w, h);
    overlays_.push_back(r);
    shadow(r, 12, 40, 16, 0.5f);
    box(r, 12, C::surface, 1, C::line);
    hit(ID_POPUP, r, 0, 12);
    float y = r.top + 1 + 4;
    for (int i = 0; i < n; i++) {
        if (it[i].sep) { hline(r.left + 1 + 4 + 6, r.right - 1 - 4 - 6, y + 4, C::line); y += 9; continue; }
        int id = ID_MENU0 + i;
        D2D1_RECT_F ir = rc(r.left + 5, y, w - 10, 34);
        if (hover_ == id || focus_ == id) fill(ir, C::hover, 1, 6);
        icon(it[i].icon, ir.left + 10, y + 9, 16, C::text);
        text(it[i].text, f, ir.left + 36, y, ir.right - ir.left - 46, 34, C::text);
        if (it[i].small) text(it[i].small, sf, ir.left, y, ir.right - ir.left - 10, 34, C::dim, 2);
        hit(id, ir, F_FOCUS | F_HAND, 6);
        y += 34;
    }
}

void App::drawToasts(float W, float H) {
    (void)H;
    float bottom = toastBottom_;
    Font f{ Face::UI, 12.5f, 400, false };
    for (int i = (int)toasts_.size() - 1; i >= 0; i--) {
        const Toast& t = toasts_[i];
        float maxText = 420 - 32 - 2 - 8 - 12;
        float tw = std::min(maxText, textW(t.text, f) + 1);
        float th = rich(t.text, f, 0, 0, tw, 18.125f, C::text, C::text, 0, false);
        float w = 32 + 2 + 8 + 12 + tw, h = 20 + 2 + std::max(th, 8.f);
        (void)W;
        D2D1_RECT_F r = rc(toastRight_ - w, bottom - h, w, h);
        overlays_.push_back({ r.left, r.top, r.right, r.bottom + 6 });   // the whole slide, so it stays put while it moves
        D2D1_RECT_F area = { r.left - 46, r.top - 34, r.right + 46, r.bottom + 6 + 12 + 46 };   // + shadow (3 sigma)
        toastRects_.push_back(area);
        // @keyframes tin: from opacity 0, translateY(6px); 180 ms var(--ease)
        float a = this->tw(K_TOAST | (t.seq & 0xFFFF), 1, area);
        r.top += 6 * (1 - a); r.bottom += 6 * (1 - a);
        alpha_ = a;
        shadow(r, 12, 30, 12, 0.45f);
        box(r, 12, C::surface, 1, C::line);
        uint32_t dc = t.level == Level::Ok ? C::ok : t.level == Level::Warn ? C::warn : t.level == Level::Bad ? C::bad : C::dim;
        dot(r.left + 17 + 4, r.top + h / 2, 8, dc);
        rich(t.text, f, r.left + 17 + 8 + 12, r.top + 11, tw, 18.125f, C::text, C::text);
        alpha_ = 1;
        hit(ID_TOAST, r, 0, 12);
        bottom -= h + 8;
    }
}

// ================================================================================================ input
int App::ncHit(float x, float y) const {
    if (y < TB - 1 && x < W_ - 138) return HTCAPTION;
    return HTCLIENT;
}

bool App::cursor(float x, float y) {
    const Hit* h = findHit(x, y);
    if (drag_ == ID_GRIP || (h && h->id == ID_GRIP)) { SetCursor(LoadCursor(nullptr, IDC_SIZENS)); return true; }
    if (h && (h->flags & F_HAND) && !(h->flags & F_DISABLED)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return true; }
    if (h && (h->flags & F_EDIT)) { SetCursor(LoadCursor(nullptr, IDC_IBEAM)); return true; }
    return false;
}

void App::mouseMove(float x, float y) {
    if (drag_ == ID_RANGE) {
        D2D1_RECT_F r = rectOf(ID_RANGE);
        int v = (int)lroundf((x - r.left - 8) / std::max(1.f, r.right - r.left - 16) * 99);
        v = std::max(0, std::min(99, v));
        if (v != botsDrag_) { botsDrag_ = v; syncEdits(); invalidate(); }
        return;
    }
    if (drag_ == ID_GRIP) {
        actH_ = std::max(140.f, std::min(H_ * 0.7f, dragV0_ + dragY0_ - y));
        invalidate();
        return;
    }
    if (drag_ == ID_P_SB) { portScrollbar(y, false); return; }
    if (drag_ == ID_SB_CONTENT || drag_ == ID_SB_LOG) {
        bool content = drag_ == ID_SB_CONTENT;
        D2D1_RECT_F r = rectOf(drag_);
        float total = content ? contentH_ : logMax_ + logView_, view = content ? contentView_ : logView_;
        float trackH = r.bottom - r.top, th = std::max(24.f, trackH * view / std::max(1.f, total));
        float v = dragV0_ + (y - dragY0_) * (total - view) / std::max(1.f, trackH - th);
        if (content) contentScroll_ = v; else { logScroll_ = v; logFollow_ = v >= logMax_ - 8; }
        invalidate();
        return;
    }
    const Hit* h = findHit(x, y);
    int id = h ? h->id : 0;
    if (id != hover_) setHover(id);
}

void App::mouseLeave() { if (hover_) setHover(0); }

// Only the two controls change: repaint their rects (4 DIPs more for the Launch button's ring); their transitions
// start when they are drawn (fx) and the animation timer repaints the same rects until they settle.
void App::setHover(int id) {
    int old = hover_;
    hover_ = id;
    dirty(grow(rectOf(old), 4));
    dirty(grow(rectOf(id), 4));
    const Hit* p = hitById(ID_PLAY);
    if (id == ID_PLAY && p && !(p->flags & F_DISABLED)) { anim_.to(K_SHEEN, -1.2f, 1.2f, 700, Curve::Ui, grow(p->r, 4)); animate(); }   // .play:hover::after
    else if (old == ID_PLAY) anim_.stop(K_SHEEN);
    updateTip();
}

std::wstring App::tooltip(int id) const {
    if (id >= ID_PORT && id < ID_P_END) return portTip(id);
    const Build* b = sel();
    switch (id) {
    case ID_MIN: return L"Minimize";
    case ID_MAX: return maximized_ ? L"Restore down" : L"Maximize";
    case ID_CLOSE: return L"Close";
    case ID_ADD: return L"Add build";
    case ID_COSMETICS: return L"Cosmetics of this build";
    case ID_MODE0: return matchOk(b) ? L"Pick Creative / Playground in the game: alone, infinite materials and ammo, 0 ping." : L"This build has no local match: lobby, locker and styles only.";
    case ID_MODE1: return fmt(L"Pick Solo, Duos, Trios or Squads: bus, storm and loot, with %d bots.", st_.config.bots);
    case ID_TG0: return L"Experimental: crashes the match on 17.50";
    case ID_TG1: return L"Small storm, shields";
    case ID_TG2: return L"Battle Royale (practice always has them)";
    case ID_TG3: return L"Battle Royale (practice always has it)";
    case ID_BUILDMENU: return L"Build actions";
    case ID_ACT: return L"Activity (Ctrl+L)";
    case ID_PREV: return L"Previous build (Q)";
    case ID_NEXT: return L"Next build (E)";
    case ID_SHEET: return L"Bots, switches and player name";
    case ID_SHEETX: return L"Close (Esc)";
    case ID_GRIP: return L"Drag to resize";
    case ID_COPY: return L"Copy activity";
    case ID_OPENLOGS: return L"Open the build's log folder";
    case ID_CLEAR: return L"Clear";
    case ID_META: return b ? b->dir : L"";
    case ID_CELL0: return b ? b->lobbyWhy : L"";
    case ID_CELL0 + 1: return b ? (b->matchServer ? b->matchWhy : L"launcher\\gameserver\\radarnite-server.dll is missing") : L"";
    case ID_CELL0 + 2: return L"Read from the build's own data";
    case ID_CELL0 + 3: return b ? (b->firewall == L"OK" ? L"Every exe of the build is blocked from the internet" : b->firewall) : L"";
    }
    if (id >= ID_BUILD0 && id - ID_BUILD0 < (int)st_.builds.size()) { const Build& x = st_.builds[id - ID_BUILD0]; std::wstring v = verLabel(x); return v + (x.name != v ? L" (" + x.name + L")" : L"") + L" · " + season(x.major, x.minor); }
    return L"";
}

// Tooltips like the page's title attributes, with the control Chromium uses on Windows: the tool is the hovered
// control's rect with its text; comctl32 shows it after the hover delay (TTF_SUBCLASS)
void App::updateTip() {
    if (!tip_ && hwnd_ && hover_) {   // created on the first hover, after the first frame
        tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (tip_) {
            TTTOOLINFOW ti = { sizeof ti };
            ti.uFlags = TTF_SUBCLASS; ti.hwnd = hwnd_; ti.uId = 1; ti.lpszText = (LPWSTR)L"";
            SendMessageW(tip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, MulDiv(400, (int)dpi_, 96));
        }
    }
    if (!tip_) return;
    std::wstring t = tooltip(hover_);
    const Hit* h = hitById(hover_);
    TTTOOLINFOW ti = { sizeof ti };
    ti.hwnd = hwnd_; ti.uId = 1;
    if (h && !t.empty()) {
        D2D1_RECT_F r = inter(h->r, h->clip);
        ti.rect = { lroundf(r.left * scale_), lroundf(r.top * scale_), lroundf(r.right * scale_), lroundf(r.bottom * scale_) };
    }
    SendMessageW(tip_, TTM_POP, 0, 0);
    SendMessageW(tip_, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    ti.lpszText = (LPWSTR)t.c_str();
    SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

void App::mouseDown(float x, float y) {
    focusVisible_ = false;
    const Hit* h = findHit(x, y);
    int id = h ? h->id : 0;
    int toggle = menuOwner(menu_);
    if (menu_ && id != ID_POPUP && !(id >= ID_MENU0 && id < ID_MENU0 + 20) && id != toggle) { menu_ = 0; invalidate(); }
    else if (sheetOpen_ && !inside(sheetRect_, x, y) && id != ID_SHEET && id != ID_TOAST && !(id >= ID_MENU0 && id < ID_BUILD0) && id != ID_POPUP) openSheet(false);
    if (!h || (h->flags & F_DISABLED) || id == ID_POPUP || id == ID_TOAST) {
        if (hwnd_ && GetFocus() != hwnd_) SetFocus(hwnd_);
        invalidate();
        return;
    }
    if (h->flags & F_EDIT) { setFocus(id, false); return; }
    if (hwnd_ && GetFocus() != hwnd_) SetFocus(hwnd_);
    if (id == ID_RANGE) {
        drag_ = ID_RANGE; focus_ = ID_RANGE;
        if (hwnd_) SetCapture(hwnd_);
        mouseMove(x, y);
        return;
    }
    if (id == ID_GRIP) { drag_ = id; dragY0_ = y; dragV0_ = actH_; if (hwnd_) SetCapture(hwnd_); return; }
    if (id == ID_P_SB) { portScrollbar(y, true); return; }
    if (id == ID_SB_CONTENT || id == ID_SB_LOG) {
        bool content = id == ID_SB_CONTENT;
        float total = content ? contentH_ : logMax_ + logView_, view = content ? contentView_ : logView_;
        float trackH = h->r.bottom - h->r.top, th = std::max(24.f, trackH * view / std::max(1.f, total));
        float scroll = content ? contentScroll_ : logScroll_, maxS = std::max(1.f, total - view);
        float ty = h->r.top + (trackH - th) * scroll / maxS;
        if (y < ty || y > ty + th) {   // click on the track: page towards it
            float v = scroll + (y < ty ? -view : view);
            if (content) contentScroll_ = v; else { logScroll_ = std::max(0.f, v); logFollow_ = logScroll_ >= logMax_ - 8; }
            invalidate();
            return;
        }
        drag_ = id; dragY0_ = y; dragV0_ = scroll;
        if (hwnd_) SetCapture(hwnd_);
        return;
    }
    if (h->flags & F_FOCUS) focus_ = id;
    press_ = id;
    if (hwnd_) SetCapture(hwnd_);
    invalidate();
}

void App::mouseUp(float x, float y) {
    if (drag_) {
        if (drag_ == ID_RANGE && botsDrag_ >= 0) { Config c; c.bots = botsDrag_; botsDrag_ = -1; setConfig(ConfigPatch::Bots, c); }
        if (drag_ == ID_GRIP) core_->setUiPref(L"activityHeight", std::to_wstring((int)actH_));
        drag_ = 0;
        if (hwnd_) ReleaseCapture();
        invalidate();
        return;
    }
    int p = press_; press_ = 0;
    if (hwnd_ && GetCapture() == hwnd_) ReleaseCapture();
    const Hit* h = findHit(x, y);
    if (p && h && h->id == p && !(h->flags & F_DISABLED)) activate(p);
    invalidate();
}

void App::wheel(float x, float y, float delta) {
    if (portWheel(x, y, delta)) return;
    const Hit* l = hitById(ID_LINES);
    if (actOpen_ && l && inside(l->r, x, y)) {
        logScroll_ = std::max(0.f, logScroll_ - delta / 120.f * LOG_LH * 3);
        logFollow_ = logScroll_ >= logMax_ - 8;
    } else if (tabMax_ > 0 && inside(tabClip_, x, y)) {
        tabScroll_ -= delta / 120.f * 80;
    } else if (inside(contentClip_, x, y)) {
        contentScroll_ -= delta / 120.f * 100;
    } else return;
    invalidate();
}

void App::activate(int id) {
    const std::wstring& b = selected_;
    if (id >= ID_PORT && id < ID_P_END) { portActivate(id); return; }
    if (id >= ID_BUILD0) { int i = id - ID_BUILD0; if (i < (int)st_.builds.size()) select(st_.builds[i].name, true); return; }
    if (id >= ID_MENU0) {
        int n; const MenuItem* it = menuItems(menu_, &n);
        int i = id - ID_MENU0;
        int m = menu_;
        menu_ = 0;
        if (i < n && !it[i].sep) core_->command(it[i].cmd, b);
        setFocus(menuOwner(m), focusVisible_);
        invalidate();
        return;
    }
    Config c;
    switch (id) {
    case ID_MIN: ShowWindow(hwnd_, SW_MINIMIZE); break;
    case ID_MAX: ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); break;
    case ID_CLOSE: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case ID_ADD: case ID_BUILDMENU: case ID_COSMETICS: {   // the cosmetics panel takes ID_COSMETICS over
        int m = id == ID_ADD ? 1 : id == ID_BUILDMENU ? 2 : 3;
        menu_ = menu_ == m ? 0 : m;
        if (menu_ && focusVisible_) focus_ = ID_MENU0;   // keyboard: the first item
        invalidate();
        break;
    }
    case ID_EMPTY_ROOT: core_->command(Command::ChooseRoot, b); break;
    case ID_EMPTY_ZIP: core_->command(Command::AddZip, b); break;
    case ID_EMPTY_FOLDER: core_->command(Command::AddFolder, b); break;
    case ID_NOTICE_FW: core_->command(Command::Firewall, b); break;
    case ID_NOTICE_COS: core_->command(Command::Cosmetics, b); break;
    case ID_TG0: c.botsAI = !st_.config.botsAI; setConfig(ConfigPatch::BotsAI, c); break;
    case ID_TG1: c.lateGame = !st_.config.lateGame; setConfig(ConfigPatch::LateGame, c); break;
    case ID_TG2: c.infiniteMaterials = !st_.config.infiniteMaterials; setConfig(ConfigPatch::InfiniteMaterials, c); break;
    case ID_TG3: c.infiniteAmmo = !st_.config.infiniteAmmo; setConfig(ConfigPatch::InfiniteAmmo, c); break;
    case ID_F0: case ID_F1: case ID_F2: filter_ = id - ID_F0; rowsDirty_ = true; invalidate(); break;
    case ID_COPY: {
        std::wstring all;
        for (auto& l : logs_) { all += l.ts; all += L' '; all += l.text; all += L"\r\n"; }
        if (hwnd_ && OpenClipboard(hwnd_)) {
            EmptyClipboard();
            HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, (all.size() + 1) * sizeof(wchar_t));
            if (m) { memcpy(GlobalLock(m), all.c_str(), (all.size() + 1) * sizeof(wchar_t)); GlobalUnlock(m); SetClipboardData(CF_UNICODETEXT, m); }
            CloseClipboard();
        }
        toast(L"Activity copied", Level::Info);
        break;
    }
    case ID_OPENLOGS: core_->command(Command::OpenLogs, b); break;
    case ID_CLEAR: logs_.clear(); rows_.clear(); rowsDirty_ = true; invalidate(); break;
    case ID_ACT: openActivity(!actOpen_); break;
    case ID_PREV: case ID_NEXT: stepBuild(id == ID_NEXT ? 1 : -1); break;
    case ID_SHEET: {
        bool kb = focusVisible_;
        openSheet(!sheetOpen_);
        if (sheetOpen_ && kb) focus_ = matchOk(sel()) && sel()->lobby != Support::No ? (int)ID_RANGE : (int)ID_SHEETX;   // keyboard: into the sheet
        break;
    }
    case ID_SHEETX: openSheet(false); setFocus(ID_SHEET, focusVisible_); break;
    case ID_PLAY:
        if (se_.state == SessionState::Launching || se_.state == SessionState::Running) core_->command(Command::Stop, b);
        else if (!b.empty()) core_->command(Command::Launch, b);
        break;
    }
}

void App::setFocus(int id, bool keyboard) {
    focus_ = id; focusVisible_ = keyboard;
    if (hwnd_) {
        HWND target = hwnd_;
        for (auto& e : edits_) if (e.id == id) {
            if (e.lazy && !e.shown && e.show) { e.want = true; layoutEdits(); }   // the bots number: its EDIT appears to take the focus
            if (e.shown) target = e.hwnd;
        }
        if (GetFocus() != target) SetFocus(target);
        if (target != hwnd_ && keyboard) SendMessageW(target, EM_SETSEL, 0, -1);   // like tabbing into an input
    }
    ensureVisible(id);
    invalidate();
}

void App::ensureVisible(int id) {
    if (id >= ID_BUILD0 && id - ID_BUILD0 < (int)st_.builds.size()) { tabEnsure_ = id - ID_BUILD0; invalidate(); return; }
    const Hit* h = hitById(id);
    if (!h || !(h->flags & F_CONTENT)) return;
    float before = contentScroll_;
    if (h->r.top < h->clip.top) contentScroll_ -= h->clip.top - h->r.top + 12;
    else if (h->r.bottom > h->clip.bottom) contentScroll_ += h->r.bottom - h->clip.bottom + 12;
    if (contentScroll_ != before) {
        // move the recorded rects too, so the EDIT can take the focus before the next paint
        for (auto& x : hits_) if (x.flags & F_CONTENT) { x.r.top -= contentScroll_ - before; x.r.bottom -= contentScroll_ - before; }
        for (auto& e : edits_) if (e.clip.top == contentClip_.top) { e.r.top -= contentScroll_ - before; e.r.bottom -= contentScroll_ - before; }
        layoutEdits();
        invalidate();
    }
}

void App::moveFocus(int dir) {
    std::vector<int> order;
    int top = 0;
    for (auto& h : hits_) top = std::max(top, h.layer);
    for (auto& h : hits_) {
        if (!(h.flags & F_FOCUS) || (h.flags & F_DISABLED) || h.layer != top) continue;
        bool inMenu = h.id >= ID_MENU0 && h.id < ID_BUILD0;
        if (menu_ ? !inMenu : inMenu) continue;
        order.push_back(h.id);
    }
    if (order.empty()) return;
    int i = -1;
    for (size_t k = 0; k < order.size(); k++) if (order[k] == focus_) i = (int)k;
    int n = (int)order.size();
    int j = i < 0 ? (dir > 0 ? 0 : n - 1) : ((i + dir) % n + n) % n;
    setFocus(order[j], true);
}

bool App::key(UINT vk, bool ctrl, bool shift) {
    if (pOpen_ && !menu_ && portKey(vk, ctrl, shift)) return true;
    if (vk == VK_ESCAPE) {
        if (menu_) { int t = menuOwner(menu_); menu_ = 0; setFocus(t, focusVisible_); return true; }
        if (sheetOpen_) { openSheet(false); setFocus(ID_SHEET, focusVisible_); return true; }
        return false;
    }
    if (ctrl && vk == VK_RETURN) { const Hit* p = hitById(ID_PLAY); if (p && !(p->flags & F_DISABLED)) activate(ID_PLAY); return true; }
    if (ctrl && vk == 'L') { openActivity(!actOpen_); return true; }
    if (vk == VK_TAB) { if (menu_) { int t = menuOwner(menu_); menu_ = 0; focus_ = t; } moveFocus(shift ? -1 : 1); return true; }
    if (menu_ && (vk == VK_DOWN || vk == VK_UP)) { moveFocus(vk == VK_DOWN ? 1 : -1); return true; }
    int f = focus_;
    if (!ctrl && !menu_ && !pOpen_ && (vk == 'Q' || vk == 'E')) { stepBuild(vk == 'E' ? 1 : -1); return true; }   // like the game's tabs
    if (f >= ID_BUILD0 && (vk == VK_DOWN || vk == VK_UP || vk == VK_LEFT || vk == VK_RIGHT)) {   // the tabs
        if (busy() || st_.builds.empty()) return true;
        int i = -1;
        for (size_t k = 0; k < st_.builds.size(); k++) if (st_.builds[k].name == selected_) i = (int)k;
        int j = std::max(0, std::min((int)st_.builds.size() - 1, i + (vk == VK_DOWN || vk == VK_RIGHT ? 1 : -1)));
        select(st_.builds[j].name);
        setFocus(ID_BUILD0 + j, true);
        return true;
    }
    if (f == ID_RANGE) {
        int v = st_.config.bots;
        switch (vk) {
        case VK_LEFT: case VK_DOWN: v--; break;
        case VK_RIGHT: case VK_UP: v++; break;
        case VK_PRIOR: v += 10; break;
        case VK_NEXT: v -= 10; break;
        case VK_HOME: v = 0; break;
        case VK_END: v = 99; break;
        default: v = -1000;
        }
        if (v != -1000) { Config c; c.bots = std::max(0, std::min(99, v)); setConfig(ConfigPatch::Bots, c); focusVisible_ = true; return true; }
    }
    if (f == ID_LINES) {
        float d = 0;
        switch (vk) {
        case VK_UP: d = -LOG_LH; break;
        case VK_DOWN: d = LOG_LH; break;
        case VK_PRIOR: d = -logView_ * 0.9f; break;
        case VK_NEXT: d = logView_ * 0.9f; break;
        case VK_HOME: d = -1e9f; break;
        case VK_END: d = 1e9f; break;
        }
        if (d) { logScroll_ = std::max(0.f, std::min(logMax_, logScroll_ + d)); logFollow_ = logScroll_ >= logMax_ - 8; invalidate(); return true; }
    }
    // Left / Right switch builds where they move nothing else (the tabs, the slider and the log took them above)
    if ((vk == VK_LEFT || vk == VK_RIGHT) && !menu_ && !pOpen_ && !sheetOpen_ && f != ID_LINES && f != ID_SEARCH && !(f >= ID_F0 && f <= ID_CLEAR)) {
        stepBuild(vk == VK_RIGHT ? 1 : -1);
        return true;
    }
    if (vk == VK_RETURN || vk == VK_SPACE) {
        const Hit* h = hitById(f);
        // Enter launches unless a control was reached with the keyboard (never stops: that stays on the button and
        // Ctrl+Enter)
        if (vk == VK_RETURN && (!h || !focusVisible_) && !menu_ && !pOpen_ && !busy()) {
            const Hit* p = hitById(ID_PLAY);
            if (p && !(p->flags & F_DISABLED)) activate(ID_PLAY);
            return true;
        }
        if (h && !(h->flags & F_DISABLED) && f != ID_LINES) { focusVisible_ = true; activate(f); return true; }
    }
    if (vk == VK_PRIOR || vk == VK_NEXT) { contentScroll_ += (vk == VK_NEXT ? 1 : -1) * contentView_ * 0.9f; invalidate(); return true; }
    return false;
}

// ================================================================================================ selftest
std::vector<std::pair<std::wstring, bool>> App::selfCheck(float W, float H) {
    std::vector<std::pair<std::wstring, bool>> out;
    auto frame = [&] { rt_->BeginDraw(); paint(W, H); rt_->EndDraw(); };
    auto center = [&](int id, float* x, float* y) { D2D1_RECT_F r = rectOf(id); *x = (r.left + r.right) / 2; *y = (r.top + r.bottom) / 2; };
    auto click = [&](int id) { frame(); float x, y; center(id, &x, &y); mouseDown(x, y); mouseUp(x, y); frame(); };
    auto check = [&](const wchar_t* name, bool ok) { out.push_back({ name, ok }); };
    frame(); frame();

    click(ID_SHEET);
    check(L"Match settings opens the sheet", sheetOpen_ && hitById(ID_TG1) && !hitById(ID_SHEET));
    bool late = st_.config.lateGame;
    click(ID_TG1);
    check(L"click switch toggles lateGame", st_.config.lateGame == !late);
    int bots = st_.config.bots;
    setFocus(ID_RANGE, true); key(VK_RIGHT, false, false); frame();
    check(L"Right on the slider adds a bot", st_.config.bots == bots + 1);
    key(VK_LEFT, false, false); frame();
    check(L"Left on the slider removes it", st_.config.bots == bots);
    frame();
    { float x, y; center(ID_RANGE, &x, &y); D2D1_RECT_F r = rectOf(ID_RANGE); mouseDown(r.right - 2, y); mouseUp(r.right - 2, y); frame(); }
    check(L"slider drag to the end = 99 bots", st_.config.bots == 99);
    setFocus(ID_RANGE, true); key(VK_HOME, false, false); frame();
    check(L"slider Home = 0 bots", st_.config.bots == 0);
    key(VK_RIGHT, false, false); key(VK_PRIOR, false, false); frame();
    check(L"slider Right + PageUp = 11 bots", st_.config.bots == 11);
    key(VK_ESCAPE, false, false); frame();
    check(L"Escape closes the sheet and focuses Match settings", !sheetOpen_ && focus_ == ID_SHEET && hitById(ID_SHEET));
    click(ID_SHEET);
    { float ox = 5, oy = TB + 200; mouseDown(ox, oy); mouseUp(ox, oy); frame(); }
    check(L"a click outside the sheet closes it", !sheetOpen_);

    // focus order: Tab from nothing reaches every control once and wraps
    focus_ = 0; frame();
    int n = 0; for (auto& h : hits_) if ((h.flags & F_FOCUS) && !(h.flags & F_DISABLED) && !(h.id >= ID_MENU0 && h.id < ID_BUILD0)) n++;
    key(VK_TAB, false, false); int first = focus_;
    for (int i = 1; i < n; i++) key(VK_TAB, false, false);
    key(VK_TAB, false, false);
    check(L"Tab cycles through every control and wraps", first == ID_MIN && focus_ == first);
    key(VK_TAB, true, true);   // Shift+Tab (ctrl ignored for Tab)
    check(L"Shift+Tab goes back to the last control", focus_ == ID_PLAY);

    // build list keyboard selection
    size_t cur = 0; for (size_t i = 0; i < st_.builds.size(); i++) if (st_.builds[i].name == selected_) cur = i;
    setFocus(ID_BUILD0 + (int)cur, true); frame();
    key(VK_DOWN, false, false); frame();
    check(L"Down in the build list selects the next build", cur + 1 < st_.builds.size() ? selected_ == st_.builds[cur + 1].name : true);
    key(VK_UP, false, false); frame();
    check(L"Up goes back", selected_ == st_.builds[cur].name);
    key(VK_RIGHT, false, false); key(VK_LEFT, false, false); frame();
    check(L"Right and Left on the tabs move the selection and back", selected_ == st_.builds[cur].name && focus_ == ID_BUILD0 + (int)cur);
    focus_ = 0; key('E', false, false); frame();
    bool eNext = cur + 1 < st_.builds.size() ? selected_ == st_.builds[cur + 1].name : true;
    key('Q', false, false); frame();
    check(L"E selects the next build and Q goes back", eNext && selected_ == st_.builds[cur].name);
    click(ID_NEXT);
    bool nNext = cur + 1 < st_.builds.size() ? selected_ == st_.builds[cur + 1].name : true;
    click(ID_PREV);
    check(L"the arrow buttons select the next build and back", nNext && selected_ == st_.builds[cur].name);

    // build menu: click opens, Escape closes and returns the focus
    click(ID_BUILDMENU);
    check(L"Build menu opens on click", menu_ == 2);
    key(VK_ESCAPE, false, false); frame();
    check(L"Escape closes the menu and focuses its button", menu_ == 0 && focus_ == ID_BUILDMENU);
    setFocus(ID_ADD, true); key(VK_RETURN, false, false); frame();
    check(L"Enter on + opens the add menu on its first item", menu_ == 1 && focus_ == ID_MENU0);
    key(VK_DOWN, false, false); frame();
    check(L"Down moves to the next menu item", focus_ == ID_MENU0 + 1);
    float ox = 5, oy = H - 5; mouseDown(ox, oy); mouseUp(ox, oy); frame();
    check(L"Click outside closes the menu", menu_ == 0);

    // activity: Ctrl+L, filters, search, virtualized 3000 lines
    key('L', true, false); frame();
    check(L"Ctrl+L opens Activity", actOpen_);
    for (int i = 0; i < 5000; i++) { LogLine l; l.ts = L"10:00:00"; l.text = i % 7 ? L"Backend ready on 127.0.0.1:3551 (#" + std::to_wstring(i) + L")" : L"Error: could not start (#" + std::to_wstring(i) + L")"; addLog(l); }
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    frame();
    QueryPerformanceCounter(&t1);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    check(L"log keeps the last 3000 lines", logs_.size() == MAXLOG);
    check((L"frame with 3000 lines under 50 ms (" + std::to_wstring((int)ms) + L" ms)").c_str(), ms < 50);
    check(L"log follows the end", logFollow_ && logScroll_ >= logMax_ - 1);
    click(ID_F2);
    bool onlyBad = !rows_.empty(); for (int r : rows_) if (logs_[r].lvl != 2) onlyBad = false;
    check(L"Errors filter shows only errors", filter_ == 2 && onlyBad);
    q_ = lower(L"#4998"); rowsDirty_ = true; filter_ = 0; frame();
    check(L"search filters the lines", rows_.size() == 1);
    q_.clear(); rowsDirty_ = true; frame();
    float before = actH_;
    { D2D1_RECT_F g = rectOf(ID_GRIP); float y = (g.top + g.bottom) / 2; mouseDown(100 + g.left, y); mouseMove(100 + g.left, y - 60); mouseUp(100 + g.left, y - 60); frame(); }
    check(L"dragging the grip makes Activity taller", actH_ > before + 50);
    setFocus(ID_LINES, true); key(VK_HOME, false, false); frame();
    check(L"Home in the log goes to the top and stops following", logScroll_ == 0 && !logFollow_);
    click(ID_CLEAR);
    check(L"Clear empties the log", logs_.empty());
    key('L', true, false); frame();
    check(L"Ctrl+L closes Activity", !actOpen_);

    // launch bar: Ctrl+Enter only when a build can launch
    const Hit* p = hitById(ID_PLAY);
    check(L"Launch enabled for a supported build", p && !(p->flags & F_DISABLED));

    // keyboard on the other widgets
    bool mats = st_.config.infiniteMaterials;
    click(ID_SHEET);
    setFocus(ID_TG2, true); key(VK_SPACE, false, false); frame();
    check(L"Space toggles the focused switch", st_.config.infiniteMaterials == !mats);
    openActivity(true); frame();
    focus_ = 0; for (int i = 0; i < 200 && focus_ != ID_F1; i++) key(VK_TAB, false, false);
    bool reached = focus_ == ID_F1;
    key(VK_RETURN, false, false); frame();
    check(L"Tab reaches the Activity filters and Enter picks one", reached && filter_ == 1);
    filter_ = 0; rowsDirty_ = true; openActivity(false); frame();
    setFocus(ID_ADD, true); key(VK_RETURN, false, false); frame();
    key(VK_DOWN, false, false); key(VK_DOWN, false, false); frame();   // over the separator: "Rescan builds"
    bool onRescan = focus_ == ID_MENU0 + 3;
    key(VK_RETURN, false, false); drain(); frame();
    check(L"Enter on a menu item runs it and gives the focus back to its button",
          onRescan && menu_ == 0 && focus_ == ID_ADD && !logs_.empty() && logs_.back().lower.find(L"rescanned") != std::wstring::npos);

    // frame: the title bar drags (HTCAPTION) except over the window buttons
    check(L"title bar is a caption except the window buttons, the rest is client",
          ncHit(300, 20) == HTCAPTION && ncHit(W - 23, 20) == HTCLIENT && ncHit(W - 23 - 46 * 2, 20) == HTCLIENT && ncHit(300, 60) == HTCLIENT);
    // tooltips: the page's title attributes
    bool tips = true;
    const int tipIds[] = { ID_MIN, ID_MAX, ID_CLOSE, ID_ADD, ID_COSMETICS, ID_BUILDMENU, ID_ACT, ID_PREV, ID_NEXT, ID_SHEET, ID_GRIP, ID_COPY, ID_OPENLOGS, ID_CLEAR, ID_META, ID_BUILD0, ID_MODE0, ID_TG0 };
    for (int id : tipIds) tips = tips && !tooltip(id).empty();
    const Build* sb = sel();
    check(L"icon buttons, build items and status cells have the page's tooltips",
          tips && sb && tooltip(ID_CELL0) == sb->lobbyWhy && tooltip(ID_CELL0 + 1) == (sb->matchServer ? sb->matchWhy : L"launcher\\gameserver\\radarnite-server.dll is missing"));

    openSheet(false); frame();
    size_t ci = 0; for (size_t i = 0; i < st_.builds.size(); i++) if (st_.builds[i].name == selected_) ci = i;
    click(ID_BUILD0 + (int)ci);
    key(VK_RETURN, false, false); drain(); frame();
    check(L"Enter after a click on the build launches it", se_.state == SessionState::Launching);
    return out;
}

std::vector<std::pair<std::wstring, bool>> App::liveCheck() {
    std::vector<std::pair<std::wstring, bool>> out;
    if (!hwnd_) return out;
    auto check = [&](const wchar_t* name, bool ok) { out.push_back({ name, ok }); };
    RECT wr, cr; GetWindowRect(hwnd_, &wr); GetClientRect(hwnd_, &cr);
    check(L"no system frame: the client area is the whole window (WM_NCCALCSIZE)", wr.right - wr.left == cr.right && wr.bottom - wr.top == cr.bottom);
    auto nc = [&](float x, float y) {
        POINT pt = { lroundf(x * scale_), lroundf(y * scale_) }; ClientToScreen(hwnd_, &pt);
        return (int)SendMessageW(hwnd_, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y));
    };
    float W = cr.right / scale_, H = cr.bottom / scale_;
    check(L"WM_NCHITTEST: resize edges and corners, caption, window buttons, client",
          nc(1, 300) == HTLEFT && nc(W - 1, 300) == HTRIGHT && nc(300, 0.5f) == HTTOP && nc(300, H - 1) == HTBOTTOM &&
          nc(1, 1) == HTTOPLEFT && nc(W - 1, H - 1) == HTBOTTOMRIGHT && nc(300, 20) == HTCAPTION && nc(W - 23, 20) == HTCLIENT && nc(400, 300) == HTCLIENT);
    // the search box is a native EDIT: typing filters, Escape clears it
    openActivity(true); UpdateWindow(hwnd_);
    EditBox& e = edits_[2];
    SetWindowTextW(e.hwnd, L"Error");
    bool typed = q_ == L"error" && e.shown;
    editKey(ID_SEARCH, VK_ESCAPE, false, false);
    check(L"typing in the search EDIT filters, Escape clears it", typed && q_.empty() && GetWindowTextLengthW(e.hwnd) == 0);
    openActivity(false); UpdateWindow(hwnd_);
    // the tooltip control itself: one tool, moved onto the hovered status cell with its text
    D2D1_RECT_F cell = rectOf(ID_CELL0 + 1);
    mouseMove((cell.left + cell.right) / 2, (cell.top + cell.bottom) / 2);
    bool tipOk = false;
    if (tip_) {
        wchar_t buf[1024] = L"";
        TTTOOLINFOW ti = { sizeof ti };
        ti.hwnd = hwnd_; ti.uId = 1; ti.lpszText = buf;
        int tools = (int)SendMessageW(tip_, TTM_GETTOOLCOUNT, 0, 0);
        bool info = SendMessageW(tip_, TTM_GETTOOLINFOW, 0, (LPARAM)&ti) != 0;
        RECT got = ti.rect;
        buf[0] = 0; ti.lpszText = buf;
        SendMessageW(tip_, TTM_GETTEXTW, 1024, (LPARAM)&ti);
        D2D1_RECT_F v = inter(cell, hitById(ID_CELL0 + 1)->clip);
        RECT want = { lroundf(v.left * scale_), lroundf(v.top * scale_), lroundf(v.right * scale_), lroundf(v.bottom * scale_) };
        tipOk = tools == 1 && info && !memcmp(&got, &want, sizeof got) && got.right > got.left && tooltip(ID_CELL0 + 1) == buf && buf[0];
    }
    check(L"hovering a status cell moves the tooltip onto it with the cell's reason", tipOk);
    // the season art: downloaded or read from the cache off the UI thread, then the stage bitmap is rebuilt with it
    check(L"the season art arrives off the UI thread and the stage is drawn with it",
          !art_.pending() && !artImg_.px.empty() && bg_ && bgKey_.artGen == artGen_);
    mouseLeave();
    return out;
}

} // namespace rn
