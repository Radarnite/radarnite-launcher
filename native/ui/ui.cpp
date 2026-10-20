#include "ui.h"
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
    ID_SB_CONTENT, ID_SB_LOG, ID_POPUP, ID_TOAST, ID_MENU0 = 200, ID_BUILD0 = 1000,
};
enum { F_FOCUS = 1, F_HAND = 2, F_EDIT = 4, F_DISABLED = 8, F_RING_IN = 16, F_CONTENT = 32 };
const UINT_PTR T_SPIN = 1, T_TOAST = 2;
const float TB = 40, LB = 72;
const size_t MAXLOG = 3000;
const float LOG_LH = 19.2f;   // 12px * 1.6

struct MenuItem { Icon icon; const wchar_t* text; const wchar_t* small; Command cmd; bool sep; };
const MenuItem kAddMenu[] = {
    { Icon::Zip, L"Add from a downloaded zip…", nullptr, Command::AddZip, false },
    { Icon::Folder, L"Add from a folder…", nullptr, Command::AddFolder, false },
    { Icon::Zip, nullptr, nullptr, Command::Refresh, true },
    { Icon::Refresh, L"Rescan builds", nullptr, Command::Refresh, false },
    { Icon::Refresh, L"Check for updates", nullptr, Command::CheckUpdates, false },
};
const MenuItem kBuildMenu[] = {
    { Icon::Refresh, L"Regenerate cosmetics", nullptr, Command::Cosmetics, false },
    { Icon::Fw, L"Recreate firewall rules", L"admin", Command::Firewall, false },
    { Icon::Link, L"Desktop shortcut", nullptr, Command::Shortcut, false },
    { Icon::Folder, L"Open build folder", nullptr, Command::OpenLogs, false },
    { Icon::Zip, nullptr, nullptr, Command::Refresh, true },
    { Icon::Cog, L"Open config.json", nullptr, Command::OpenConfig, false },
};
const MenuItem* menuItems(int menu, int* n) {
    if (menu == 1) { *n = (int)(sizeof(kAddMenu) / sizeof(kAddMenu[0])); return kAddMenu; }
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
                // break after a hyphen between letters/digits, not before a number ("rvn=-1")
                if (hy == std::wstring::npos && k <= a + cols && s[k - 1] == L'-' && k >= 2 && iswalnum(s[k - 2]) && !iswdigit(s[k])) hy = k;
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

} // namespace

// ================================================================================================ lifetime
App::App(Gfx& g, Core* core) : g_(g), core_(core) {
    InitializeCriticalSection(&cs_);
    edits_[0].id = ID_NAME; edits_[0].f = { Face::UI, 13, 500, false };
    edits_[1].id = ID_BOTS; edits_[1].f = { Face::UI, 13, 600, false };
    edits_[2].id = ID_SEARCH; edits_[2].f = { Face::UI, 12, 400, false };
    float h = 0;
    std::wstring v = core_->uiPref(L"activityHeight");
    if (!v.empty() && (h = (float)_wtof(v.c_str())) >= 140) actH_ = h;
}

App::~App() {
    setTarget(nullptr, dpi_);
    for (auto& e : edits_) if (e.font) DeleteObject(e.font);
    if (editBrush_) DeleteObject(editBrush_);
    DeleteCriticalSection(&cs_);
}

void App::attach(HWND hwnd) {
    hwnd_ = hwnd;
    editBrush_ = CreateSolidBrush(RGB(0x18, 0x1A, 0x1F));
    createEdits();
}

void App::start() { core_->ready(this); }

void App::invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

void App::setTarget(ID2D1RenderTarget* rt, float dpi) {
    release(logoBrush_); release(logo_); release(brush2_); release(brush_);
    rt_ = rt;
    dpi_ = dpi; scale_ = dpi / 96.f;
    if (!rt_) return;
    rt_->CreateSolidColorBrush(rgb(C::text), &brush_);
    rt_->CreateSolidColorBrush(rgb(C::text), &brush2_);
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
        e.font = CreateFontW(-(int)lroundf(e.f.size * scale_), 0, 0, 0, e.f.weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, g_.gdiFamily.c_str());
        SendMessageW(e.hwnd, WM_SETFONT, (WPARAM)e.font, TRUE);
        HDC dc = GetDC(e.hwnd); HGDIOBJ old = SelectObject(dc, e.font);
        TEXTMETRICW tm; GetTextMetricsW(dc, &tm); e.textH = tm.tmHeight;
        SelectObject(dc, old); ReleaseDC(e.hwnd, dc);
        e.applied = {}; e.shown = false;
        ShowWindow(e.hwnd, SW_HIDE);
    }
    invalidate();
}

void App::createEdits() {
    for (auto& e : edits_) {
        DWORD style = WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | (e.id == ID_BOTS ? ES_CENTER | ES_NUMBER : 0);
        e.hwnd = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)e.id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(e.hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SendMessageW(e.hwnd, EM_LIMITTEXT, e.id == ID_NAME ? 24 : e.id == ID_BOTS ? 2 : 200, 0);
        if (e.id == ID_SEARCH) SendMessageW(e.hwnd, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filter");
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
}

void App::apply(Event& e) {
    switch (e.type) {
    case 0: {
        st_ = std::move(e.state);
        loaded_ = st_.loaded;
        if (st_.hasSession) se_ = st_.session;
        auto have = [&](const std::wstring& n) { if (n.empty()) return false; for (auto& b : st_.builds) if (b.name == n) return true; return false; };
        if (!have(selected_)) {
            // the core's selection, else the build picked last time, else the newest build
            std::wstring last = core_->uiPref(L"lastBuild");
            selected_ = have(st_.selected) ? st_.selected : have(last) ? last : st_.builds.empty() ? L"" : st_.builds.back().name;
        }
        syncEdits();
        updateSpinTimer();
        break;
    }
    case 1: {
        SessionState was = se_.state;
        se_ = e.session;
        if (se_.state == SessionState::Error && was != SessionState::Error) { toast(se_.message.empty() ? L"Launch failed" : se_.message, Level::Bad); openActivity(true); }
        updateSpinTimer();
        break;
    }
    case 2: addLog(e.log); break;
    case 3: toast(e.text, e.level); break;
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
    toasts_.push_back({ text, level, GetTickCount64() + 4500 });
    if (hwnd_) SetTimer(hwnd_, T_TOAST, 250, nullptr);
    invalidate();
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
        if (toasts_.empty()) KillTimer(hwnd_, T_TOAST);
        if (n != toasts_.size()) invalidate();
    }
}

void App::updateSpinTimer() {
    bool run = se_.state == SessionState::Launching || se_.state == SessionState::Stopping ||
               (se_.state == SessionState::Running && se_.phase == MatchPhase::None &&
                (se_.backend == Stage::Run || se_.game == Stage::Run || se_.server == Stage::Run));
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

void App::select(const std::wstring& name) {
    if (busy() || name == selected_) return;
    selected_ = name;
    core_->command(Command::Select, name);
    core_->setUiPref(L"lastBuild", name);
    contentScroll_ = 0;
    invalidate();
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
    else if (code == EN_KILLFOCUS) { commitEdit(id); invalidate(); }
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
        if (id == ID_SEARCH) SetWindowTextW(edits_[2].hwnd, L"");
        else if (id == ID_NAME) SetWindowTextW(edits_[0].hwnd, st_.config.displayName.c_str());
        else SetWindowTextW(edits_[1].hwnd, std::to_wstring(st_.config.bots).c_str());
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
        bool show = e.show;
        D2D1_RECT_F vis = inter(e.r, e.clip);
        if (vis.right - vis.left < 4 || vis.bottom - vis.top < 4) show = false;
        for (auto& o : overlaysPrev_) if (overlaps(o, e.r)) show = false;
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

// box-shadow approximation: stacked translucent rounded rects
void App::shadow(D2D1_RECT_F r, float radius, float blur, float oy, float alpha) {
    if (measuring_) return;
    const int n = 8;
    for (int i = n; i >= 1; i--) {
        float e = blur * i / n * 0.5f;
        D2D1_RECT_F s = { r.left - e, r.top + oy - e, r.right + e, r.bottom + oy + e };
        fill(s, 0x000000, alpha / n * 0.9f, radius + e);
    }
}

float App::textW(const std::wstring& s, const Font& f, float tracking) {
    if (s.empty()) return 0;
    IDWriteTextLayout* l = nullptr; float w = 0;
    if (SUCCEEDED(g_.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), g_.format(f), 10000, 1000, &l))) {
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
    hits_.push_back({ id, r, flags, radius, clip_ });
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
    bool hov = hover_ == id || (id == ID_ADD && menu_ == 1) || (id == ID_BUILDMENU && menu_ == 2);
    D2D1_RECT_F r = rc(x, y, w, h);
    uint32_t col;
    if (ghost) { box(r, 8, C::hover, hov ? 1.f : 0.f, C::hover, 0); col = hov ? C::text : C::muted; }
    else { box(r, 8, hov ? C::hover : C::surface, 1, hov ? C::l2 : C::line); col = C::text; }
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
void App::paint(float W, float H) {
    if (!rt_) return;
    W_ = W; H_ = H;
    hits_.clear(); issues.clear(); spinRects_.clear();
    overlaysPrev_ = overlays_; overlays_.clear();
    for (auto& e : edits_) e.show = false;
    base_ = D2D1::Matrix3x2F::Identity(); oy_ = 0; alpha_ = 1;
    clip_ = { -1e9f, -1e9f, 1e9f, 1e9f };
    rt_->SetTransform(base_);
    rt_->Clear(rgb(C::bg));

    drawTitle(W);
    float railW = W <= 720 ? 72.f : W <= 900 ? 200.f : 264.f;
    drawRail(0, TB, railW, H - TB);

    float wsX = railW, wsW = W - railW;
    fill({ wsX, TB, W, H }, C::bg2);
    float barY = H - LB, actTop = barY;
    if (actOpen_) actTop = barY - std::max(140.f, std::min(actH_, std::max(140.f, H * 0.7f)));
    float viewH = std::max(0.f, actTop - TB);
    contentView_ = viewH;
    bool sb = contentH_ > viewH + 0.5f;
    contentScroll_ = std::max(0.f, std::min(contentScroll_, std::max(0.f, contentH_ - viewH)));
    contentScroll_ = roundf(contentScroll_ * scale_) / scale_;
    contentClip_ = clip_ = { wsX, TB, W, actTop };
    rt_->PushAxisAlignedClip(clip_, D2D1_ANTIALIAS_MODE_ALIASED);
    oy_ = TB - contentScroll_;
    base_ = D2D1::Matrix3x2F::Translation(0, oy_);
    rt_->SetTransform(base_);
    inContent_ = true;
    float h = drawContent(wsX, 0, wsW - (sb ? 10 : 0), viewH);
    inContent_ = false;
    base_ = D2D1::Matrix3x2F::Identity(); oy_ = 0;
    rt_->SetTransform(base_);
    rt_->PopAxisAlignedClip();
    clip_ = { -1e9f, -1e9f, 1e9f, 1e9f };
    if (fabsf(h - contentH_) > 0.5f) { contentH_ = h; invalidate(); }
    if (sb) drawScrollbar(ID_SB_CONTENT, W - 10, TB, viewH, contentScroll_, contentH_, viewH);

    if (actOpen_) drawActivity(wsX, actTop, wsW, barY - actTop);
    drawBar(wsX, barY, wsW);
    drawMenu();
    drawToasts(W, H);

    // keyboard focus ring (focus-visible): 2px white, 2px outside the control (inside for the window buttons)
    if (focusVisible_ && focus_) {
        const Hit* f = hitById(focus_);
        if (f && !(f->flags & F_EDIT)) {
            float o = (f->flags & F_RING_IN) ? -3.f : 3.f;
            D2D1_RECT_F r = { f->r.left - o, f->r.top - o, f->r.right + o, f->r.bottom + o };
            float rad = std::max(0.f, f->radius + o);
            rt_->PushAxisAlignedClip(f->clip, D2D1_ANTIALIAS_MODE_ALIASED);
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br(C::white), 2);
            rt_->PopAxisAlignedClip();
        }
    }
    layoutEdits();
    if (overlays_.size() != overlaysPrev_.size() || (!overlays_.empty() && memcmp(overlays_.data(), overlaysPrev_.data(), overlays_.size() * sizeof(D2D1_RECT_F)))) invalidate();
    if (pendingEnsure_) { pendingEnsure_ = false; ensureVisible(focus_); }
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
    text(L"Launcher", f, x + w1, 0, 100, TB - 1, active_ ? C::muted : C::dim, 0, -0.13f);
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
        bool hov = hover_ == id;
        if (hov) fill(r, i == 2 ? C::closeRed : C::hover);
        uint32_t c = hov ? (i == 2 ? C::white : C::text) : active_ ? C::muted : C::dim;
        icon(ics[i], r.left + 18, (TB - 1 - 10) / 2, 10, c);
        hit(id, r, F_FOCUS | F_HAND | F_RING_IN, 0);
    }
}

void App::drawRail(float x, float y, float w, float h) {
    vline(x + w - 1, y, y + h, C::line2);
    bool tiny = w <= 72;
    float iw = w - 1;
    if (!tiny) text(L"BUILDS", Font{ Face::UI, 11, 600, false }, x + 16, y, 120, 48, C::dim, 0, 0.88f);
    float ax = tiny ? (iw - 28) / 2 : iw - 12 - 28;
    button(ID_ADD, ax, y + 10, nullptr, Icon::Plus, true, true, true);
    if (menu_ == 1) menuAnchor_ = rc(ax, y + 10, 28, 28);

    float footH = 12 + 17.4f + 12 + 1, ft = y + h - footH;
    // list
    float ly = y + 48, lx = x + 8, lw = iw - 16;
    D2D1_RECT_F saved = clip_;
    clip_ = { x, y + 48, x + iw, ft };
    rt_->PushAxisAlignedClip(clip_, D2D1_ANTIALIAS_MODE_ALIASED);
    if (!loaded_) {
        for (int i = 0; i < 3; i++) { fill(rc(lx, ly + 2, lw, 52), C::surface, 0.55f, 8); ly += 56; }
    } else if (st_.builds.empty()) {
        float th = rich(L"No builds yet.\nAdd a downloaded zip or a folder with **+**.", Font{ Face::UI, 12.5f, 400, false }, 0, 0, lw - 32 - 2, 18.125f, C::muted, C::text, 1, false);
        D2D1_RECT_F r = rc(lx, ly + 8, lw, 40 + th + 2);
        if (!measuring_) {
            ID2D1StrokeStyle* dash = nullptr;
            float dashes[] = { 3, 3 };
            g_.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0), dashes, 2, &dash);
            rt_->DrawRoundedRectangle(D2D1::RoundedRect({ r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f }, 11.5f, 11.5f), br(C::line), 1, dash);
            release(dash);
        }
        if (!tiny) rich(L"No builds yet.\nAdd a downloaded zip or a folder with **+**.", Font{ Face::UI, 12.5f, 400, false }, r.left + 17, r.top + 21, lw - 34, 18.125f, C::muted, C::text, 1);
    } else {
        bool b = busy();
        for (size_t i = 0; i < st_.builds.size(); i++) {
            const Build& bd = st_.builds[i];
            int id = ID_BUILD0 + (int)i;
            float ih = tiny ? 49.f : 10 + 20 + 2 + 17.4f + 10;
            D2D1_RECT_F r = rc(lx, ly, lw, ih);
            bool on = bd.name == selected_, hov = hover_ == id;
            if (on) fill(r, C::surface, 1, 8); else if (hov) fill(r, C::bg2, 1, 8);
            uint32_t dc = clsColor(level(bd));
            if (tiny) {
                text(bd.version, Font{ Face::Display, 15, 900, true }, lx + 4, ly + 10, lw - 8, 15, C::text, 1, -0.15f);
                dot(lx + lw / 2, ly + 10 + 15 + 6 + 4, 8, dc);
            } else {
                if (text(bd.version, Font{ Face::Display, 20, 900, true }, lx + 12, ly + 10, lw - 24 - 16, 20, C::text, 0, -0.2f)) issues.push_back(L"clipped bi.v: " + bd.version);
                text(season(bd.major, bd.minor), Font{ Face::UI, 12, 400, false }, lx + 12, ly + 10 + 20 + 2, lw - 24 - 16, 17.4f, C::muted);
                dot(lx + lw - 12 - 4, ly + ih / 2, 8, dc);
            }
            hit(id, r, F_FOCUS | F_HAND | (b ? 0 : 0), 8);
            ly += ih + 2;
        }
    }
    rt_->PopAxisAlignedClip();
    clip_ = saved;
    // footer: the builds folder
    hline(x, x + iw, ft, C::line2);
    bool hov = hover_ == ID_ROOT;
    uint32_t c = hov ? C::text : C::dim;
    icon(Icon::Folder, tiny ? x + (iw - 16) / 2 : x + 16, ft + 1 + 12 + (17.4f - 16) / 2, 16, c);
    if (!tiny) text(st_.root, Font{ Face::UI, 12, 400, false }, x + 16 + 16 + 8, ft + 1 + 12, iw - 16 - 16 - 8 - 16, 17.4f, c);
    hit(ID_ROOT, { x, ft, x + iw, y + h }, F_FOCUS | F_HAND | F_RING_IN, 0);
}

float App::drawContent(float x0, float y0, float avail, float viewH) {
    (void)viewH;
    bool n1040 = W_ <= 1040, n1000 = W_ <= 1000, n900 = W_ <= 900, shortH = H_ <= 560;
    float boxW = std::min(880.f, avail), px = n1040 ? 24.f : 32.f;
    float pt = shortH ? 16.f : n1040 ? 20.f : 24.f, pb = n1040 ? 20.f : 24.f, gap = shortH ? 16.f : 24.f;
    float x = x0 + (avail - boxW) / 2 + px, cw = boxW - 2 * px, y = y0 + pt;
    const Config& c = st_.config;

    if (!loaded_) {   // skeleton while the host loads
        fill(rc(x, y, cw, 110), C::surface, 0.55f, 8); y += 110 + gap;
        fill(rc(x, y, cw, 220), C::surface, 0.55f, 8); y += 220;
        return y + pb;
    }
    const Build* b = sel();
    if (!b) {
        text(L"No build", Font{ Face::Display, 40, 900, true }, x, y, cw, 36, C::text, 0, -1.0f); y += 36 + 8;
        text(L"Add a Fortnite build to start", Font{ Face::UI, 15, 600, false }, x, y, cw, 21.75f, C::text); y += 21.75f + 2;
        text(L"Builds live in " + st_.root + L". A downloaded zip is extracted there; a folder is moved or copied.", Font{ Face::UI, 12, 400, false }, x, y, cw, 17.4f, C::dim);
        y += 17.4f + gap;
        float bx = x;
        bx += button(ID_EMPTY_ZIP, bx, y, L"Add from zip…", Icon::Zip, false, false, false) + 8;
        bx += button(ID_EMPTY_FOLDER, bx, y, L"Add from folder…", Icon::Folder, false, false, false) + 8;
        button(ID_EMPTY_ROOT, bx, y, L"Change builds folder…", Icon::Folder, false, false, false);
        return y + 34 + pb;
    }

    // ---- hero
    float verSize = shortH ? 40.f : n1040 ? 48.f : 56.f;
    float bw = button(ID_BUILDMENU, 0, 0, L"Build", Icon::More, true, false, false, true, true);
    float leftW = cw - bw - 16;
    float vlh = verSize * 0.9f;
    text(b->version, Font{ Face::Display, verSize, 900, true }, x, y, leftW, vlh, C::text, 0, -0.025f * verSize);
    float yy = y + vlh + 8;
    text(season(b->major, b->minor), Font{ Face::UI, 15, 600, false }, x, yy, leftW, 21.75f, C::text); yy += 21.75f + 2;
    text(fmt(L"CL %u · Unreal Engine %ls · %ls", b->cl, b->engine.c_str(), (b->dir.empty() ? b->name : b->dir).c_str()),
         Font{ Face::UI, 12, 400, false }, x, yy, leftW, 17.4f, C::dim);
    yy += 17.4f;
    button(ID_BUILDMENU, x + cw - bw, y, L"Build", Icon::More, true, false, false);
    if (menu_ == 2) menuAnchor_ = rc(x + cw - bw, y + oy_, bw, 28);
    y = std::max(yy, y + 28) + 16;

    // ---- the four status cells
    Sup lob = sup(b->lobby), mat = b->matchServer ? sup(b->match) : Sup{ 2, L"match server missing" };
    bool fwOk = b->firewall == L"OK";
    struct Cell { Icon ic; const wchar_t* k; std::wstring v; int cls; } cells[4] = {
        { Icon::Lobby, L"Lobby", lob.label, lob.cls },
        { Icon::Match, L"Local match", mat.label, mat.cls },
        { Icon::Cos, L"Cosmetics", b->cosmetics ? thousands(b->cosmetics) : L"Missing", b->cosmetics ? 0 : 1 },
        { Icon::Fw, L"Firewall", fwOk ? L"127.0.0.1 only" : L"Missing", fwOk ? 0 : 2 },
    };
    int cols = n900 ? 2 : 4, rows = 4 / cols;
    float cellH = 12 + 17.4f + 2 + 18.85f + 12, cellW = (cw - 2) / cols;
    float stripH = rows * cellH + (rows - 1) + 2;
    box(rc(x, y, cw, stripH), 12, C::bg, 1, C::line2);
    for (int i = 0; i < 4; i++) {
        int col = i % cols, row = i / cols;
        float cx = x + 1 + col * cellW, cy = y + 1 + row * (cellH + 1);
        if (col > 0) vline(cx, cy, cy + cellH, C::line2);
        if (row > 0 && col == 0) hline(x + 1, x + cw - 1, cy - 1, C::line2);
        icon(cells[i].ic, cx + 16, cy + 12 + (17.4f - 16) / 2, 16, C::muted);
        text(cells[i].k, Font{ Face::UI, 12, 400, false }, cx + 16 + 22, cy + 12, cellW - 32 - 22, 17.4f, C::muted);
        text(cells[i].v, Font{ Face::UI, 13, 600, false }, cx + 16, cy + 12 + 17.4f + 2, cellW - 32, 18.85f, clsColor(cells[i].cls));
    }
    y += stripH;

    // ---- notices
    struct Notice { int cls; std::wstring html; int btn; const wchar_t* label; };
    std::vector<Notice> notices;
    if (b->lobby == Support::No) notices.push_back({ 2, L"**Fortnite " + b->version + L" cannot be launched.** " + b->lobbyWhy + L".", 0, nullptr });
    else {
        if (!fwOk) notices.push_back({ 1, L"**Firewall rules missing.** " + b->firewall + L". They are created before launch (administrator prompt).", ID_NOTICE_FW, L"Create now" });
        if (!b->cosmetics) notices.push_back({ 1, L"**No cosmetics list yet.** Only the default items will show in the locker.", ID_NOTICE_COS, L"Generate" });
    }
    for (auto& n : notices) {
        y += gap;
        float nbw = n.btn ? button(n.btn, 0, 0, n.label, Icon::Plus, true, false, false, false, true) : 0;
        float tw = cw - 32 - 16 - 12 - (n.btn ? nbw + 12 : 0);
        uint32_t tc = n.cls == 2 ? C::badText : C::warnText;
        Font nf{ Face::UI, 12.5f, 400, false };
        float th = rich(n.html, nf, 0, 0, tw, 18.75f, tc, C::text, 0, false);
        float h = 24 + std::max(th, n.btn ? 28.f : 18.f);
        fill(rc(x, y, cw, h), n.cls == 2 ? C::bad : C::warn, 0.12f, 12);
        icon(Icon::Warn, x + 16, y + 12 + 2, 16, tc);
        rich(n.html, nf, x + 16 + 16 + 12, y + 12, tw, 18.75f, tc, C::text);
        if (n.btn) button(n.btn, x + cw - 16 - nbw, y + 12, n.label, Icon::Plus, true, false, false, false);
        y += h;
    }

    // ---- settings card (measured first, then drawn on its background)
    y += gap;
    bool dis = b->lobby == Support::No, hasMatch = matchOk(b);
    float cardY = y;
    auto body = [&]() -> float {
        float sp = shortH ? 16.f : 20.f, ix = x + 1 + sp, iw = cw - 2 - 2 * sp, yy = cardY + 1 + sp;
        text(L"Modes", Font{ Face::UI, 12, 600, false }, ix, yy, iw, 17.4f, C::muted); yy += 17.4f + 8;
        if (hasMatch) {
            float mw = (iw - 8) / 2, mpv = n1000 ? 10.f : 12.f;
            Font df{ Face::UI, 12, 400, false };
            std::wstring d[2] = { L"Pick **Creative / Playground** in the game: alone, infinite materials and ammo, 0 ping.",
                                  fmt(L"Pick **Solo, Duos, Trios or Squads**: bus, storm and loot, with %d bots.", c.bots) };
            float dh = std::max(rich(d[0], df, 0, 0, mw - 26, 16.8f, C::muted, C::ink2, 0, false), rich(d[1], df, 0, 0, mw - 26, 16.8f, C::muted, C::ink2, 0, false));
            float mh = 2 + 2 * mpv + 18.85f + 4 + dh;
            const Icon mi[2] = { Icon::Build, Icon::Bots };
            const wchar_t* mt[2] = { L"Practice", L"Battle Royale" };
            for (int i = 0; i < 2; i++) {
                float mx = ix + i * (mw + 8);
                box(rc(mx, yy, mw, mh), 8, C::bg2, 1, C::line2);
                icon(mi[i], mx + 13, yy + 1 + mpv + (18.85f - 16) / 2, 16, C::text);
                if (text(mt[i], Font{ Face::UI, 13, 600, false }, mx + 13 + 24, yy + 1 + mpv, mw - 26 - 24, 18.85f, C::text)) issues.push_back(std::wstring(L"clipped mode.t: ") + mt[i]);
                rich(d[i], df, mx + 13, yy + 1 + mpv + 18.85f + 4, mw - 26, 16.8f, C::muted, C::ink2);
            }
            yy += mh;
            if (n1000) {
                yy += 8;
                yy += rich(L"In the game's lobby press **CHANGE**, pick a mode, then **PLAY**. The match server for it starts by itself.", Font{ Face::UI, 12, 400, false }, ix, yy, iw, 17.4f, C::muted, C::ink2);
            }
        } else {
            Font nf{ Face::UI, 12.5f, 400, false };
            float th = rich(L"This build has no local match: lobby, locker and styles only.", nf, 0, 0, iw - 24, 18.125f, C::muted, C::ink2, 0, false);
            icon(Icon::Lobby, ix, yy + (th - 16) / 2, 16, C::muted);
            rich(L"This build has no local match: lobby, locker and styles only.", nf, ix + 24, yy, iw - 24, 18.125f, C::muted, C::ink2);
            yy += th;
        }
        yy += sp;
        hline(x + 1, x + cw - 1, yy, C::line2);
        yy += 1 + sp;

        // player name + bots
        int cols2 = n900 ? 1 : 2;
        float colW = cols2 == 2 ? (iw - 24) / 2 : iw, fieldH = 17.4f + 8 + 34 + 6 + 17.4f;
        auto field = [&](float fx, float fy, const wchar_t* label, const wchar_t* help) {
            text(label, Font{ Face::UI, 12, 600, false }, fx, fy, colW, 17.4f, C::muted);
            text(help, Font{ Face::UI, 12, 400, false }, fx, fy + 17.4f + 8 + 34 + 6, colW, 17.4f, C::dim);
            return fy + 17.4f + 8;
        };
        {
            float fy = field(ix, yy, L"Player name", L"Local account only, never an Epic account.");
            EditBox& e = edits_[0];
            bool foc = hwnd_ && GetFocus() == e.hwnd;
            bool hov = hover_ == ID_NAME;
            box(rc(ix, fy, colW, 34), 8, C::bg2, 1, foc ? C::muted : hov ? C::l2 : C::line);
            D2D1_RECT_F inner = { ix + 13, fy + 1, ix + colW - 13, fy + 33 };
            if (!measuring_) { e.r = { inner.left, inner.top + oy_, inner.right, inner.bottom + oy_ }; e.clip = clip_; e.show = hwnd_ && !dis; }
            if (!e.show || measuring_) text(c.displayName, Font{ Face::UI, 13, 500, false }, inner.left, fy, inner.right - inner.left, 34, C::text);
            hit(ID_NAME, rc(ix, fy, colW, 34), F_FOCUS | F_EDIT, 8);
        }
        if (hasMatch) {
            float bx = cols2 == 2 ? ix + colW + 24 : ix, by = cols2 == 2 ? yy : yy + fieldH + 16;
            float fy = field(bx, by, L"Bots", L"0–99. 20 suits this PC.");
            int bots = botsDrag_ >= 0 ? botsDrag_ : c.bots;
            box(rc(bx, fy, 98, 34), 8, C::bg2, 1, C::line);
            for (int k = 0; k < 2; k++) {
                int id = k ? ID_STEP_UP : ID_STEP_DN;
                float sx = k ? bx + 1 + 30 + 36 : bx + 1;
                text(k ? L"+" : L"−", Font{ Face::UI, 13, 400, false }, sx, fy, 30, 34, hover_ == id ? C::text : C::muted, 1);
                hit(id, rc(sx, fy + 1, 30, 32), F_FOCUS | F_HAND, 6);
            }
            EditBox& e = edits_[1];
            D2D1_RECT_F inner = { bx + 31, fy + 1, bx + 31 + 36, fy + 33 };
            if (!measuring_) { e.r = { inner.left, inner.top + oy_, inner.right, inner.bottom + oy_ }; e.clip = clip_; e.show = hwnd_ && !dis; }
            if (!e.show || measuring_) text(std::to_wstring(bots), Font{ Face::UI, 13, 600, false }, inner.left, fy, 36, 34, C::text, 1);
            hit(ID_BOTS, rc(inner.left, fy, 36, 34), F_FOCUS | F_EDIT, 4);
            float rx = bx + 98 + 12, rw = std::max(60.f, colW - 98 - 12);
            float cy = fy + 17;
            fill({ rx, cy - 2, rx + rw, cy + 2 }, C::line, 1, 2);
            float tx = rx + 8 + (rw - 16) * bots / 99.f;
            if (!measuring_) {
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx, cy), 12, 12), br(C::white, 0.08f));
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx, cy), 8, 8), br(C::white));
            }
            hit(ID_RANGE, rc(rx, fy, rw, 34), F_FOCUS | F_HAND, 6);
        }
        yy += cols2 == 2 || !hasMatch ? fieldH : 2 * fieldH + 16;

        if (hasMatch) {
            // four switches, two per row
            yy += 12;
            struct Tg { const wchar_t* label; const wchar_t* sub; bool on; } tg[4] = {
                { L"Bots use the game's AI", L"Experimental: crashes the match on 17.50", c.botsAI },
                { L"Late game", L"Small storm, shields", c.lateGame },
                { L"Infinite materials", L"Battle Royale (practice always has them)", c.infiniteMaterials },
                { L"Infinite ammo", L"Battle Royale (practice always has it)", c.infiniteAmmo },
            };
            float tcw = (iw - 24) / 2;
            Font sf{ Face::UI, 11.5f, 400, false };
            for (int r = 0; r < 2; r++) {
                float lh[2], rowH = 34;
                for (int k = 0; k < 2; k++) { lh[k] = 18.85f + rich(tg[r * 2 + k].sub, sf, 0, 0, tcw - 44, 16.675f, C::dim, C::dim, 0, false); rowH = std::max(rowH, lh[k]); }
                for (int k = 0; k < 2; k++) {
                    const Tg& t = tg[r * 2 + k];
                    int id = ID_TG0 + r * 2 + k;
                    float tx = ix + k * (tcw + 24), ty = yy + (rowH - lh[k]) / 2;
                    text(t.label, Font{ Face::UI, 13, 400, false }, tx, ty, tcw - 44, 18.85f, C::ink2);
                    rich(t.sub, sf, tx, ty + 18.85f, tcw - 44, 16.675f, C::dim, C::dim);
                    float sx = tx + tcw - 32, sy = yy + (rowH - 18) / 2;
                    fill(rc(sx, sy, 32, 18), t.on ? C::ok : C::l2, 1, 9);
                    dot(sx + 3 + 6 + (t.on ? 14 : 0), sy + 9, 12, C::white);
                    hit(id, rc(tx, yy, tcw, rowH), F_FOCUS | F_HAND, 4);
                }
                yy += rowH;
            }
            // note about the bots' AI
            yy += 12;
            Font nf{ Face::UI, 12.5f, 400, false };
            std::wstring note = c.botsAI ? L"**Bot AI is on.** On " + b->version + L" the game's bot AI crashes the match server a few seconds after it starts. Turn it off to play."
                                         : L"**Bots have no AI.** They drop from the bus and land, but they do not loot or fight.";
            float th = rich(note, nf, 0, 0, iw - 24 - 24, 18.125f, C::muted, C::ink2, 0, false);
            float nh = 16 + std::max(17.f, th);
            fill(rc(ix, yy, iw, nh), c.botsAI ? C::warn : C::surface, c.botsAI ? 0.12f : 1.f, 8);
            icon(c.botsAI ? Icon::Warn : Icon::Bots, ix + 12, yy + 8 + 1, 16, c.botsAI ? C::warn : C::muted);
            rich(note, nf, ix + 12 + 16 + 8, yy + 8, iw - 24 - 24, 18.125f, c.botsAI ? C::ink2 : C::muted, C::ink2);
            yy += nh;
        }
        yy += sp + 1;
        return yy - cardY;
    };
    measuring_ = true;
    float cardH = body();
    measuring_ = false;
    if (dis) { alpha_ = 0.5f; noHit_ = true; }
    box(rc(x, cardY, cw, cardH), 12, C::bg, 1, C::line2);
    body();
    alpha_ = 1; noHit_ = false;
    y = cardY + cardH;
    return y + pb;
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
    Font ff{ Face::UI, 12, 500, false };
    float segW = 2 + 2 + 2 * 2;
    float bwid[3];
    for (int i = 0; i < 3; i++) { bwid[i] = 20 + textW(fl[i], ff) + 6 + textW(std::to_wstring(n[i]), ff); segW += bwid[i]; }
    box(rc(hx, hy, segW, 30), 8, C::bg2, 1, C::line2);
    float fx = hx + 1 + 2;
    for (int i = 0; i < 3; i++) {
        int id = ID_F0 + i;
        bool on = filter_ == i;
        if (on) fill(rc(fx, hy + 3, bwid[i], 24), C::hover, 1, 6);
        uint32_t col = on || hover_ == id ? C::text : C::muted;
        text(fl[i], ff, fx + 10, hy + 3, bwid[i], 24, col);
        text(std::to_wstring(n[i]), ff, fx + 10 + textW(fl[i], ff) + 6, hy + 3, 60, 24, C::dim);
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
        box(rc(sx, hy + 1, sw, 28), 8, C::bg2, 1, foc ? C::muted : hover_ == ID_SEARCH ? C::l2 : C::line);
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

void App::drawBar(float x, float y, float w) {
    fill({ x, y, x + w, y + LB }, C::bg);
    hline(x, x + w, y, C::line2);
    const Build* b = sel();
    const Config& c = st_.config;
    bool isBusy = busy();
    // play / stop button
    bool stopStyle = isBusy;
    bool disabled = isBusy ? se_.state == SessionState::Stopping : (!b || b->lobby == Support::No);
    Font pf = stopStyle ? Font{ Face::UI, 14, 600, false } : Font{ Face::Display, 24, 900, true };
    const wchar_t* pl = stopStyle ? L"Stop" : L"LAUNCH";
    float ptw = textW(pl, pf, stopStyle ? 0 : -0.24f);
    float minW = stopStyle ? 150.f : (W_ <= 720 ? 140.f : 196.f);
    float pw = std::max(minW, 24 + 18 + 10 + ptw + 24 + (stopStyle ? 2 : 0));
    D2D1_RECT_F pr = rc(x + w - 24 - pw, y + 10, pw, 52);
    bool phov = hover_ == ID_PLAY && !disabled;
    uint32_t pc;
    if (stopStyle) { box(pr, 12, phov ? C::hover : C::surface, 1, C::line, disabled ? 0.f : 1.f); pc = disabled ? C::dim : C::text; }
    else if (disabled) { fill(pr, C::surface, 1, 12); pc = C::dim; }
    else {
        if (phov && !measuring_) rt_->DrawRoundedRectangle(D2D1::RoundedRect({ pr.left - 1.5f, pr.top - 1.5f, pr.right + 1.5f, pr.bottom + 1.5f }, 13.5f, 13.5f), br(C::white, 0.16f), 3);
        fill(pr, C::white, 1, 12); pc = C::bg;
    }
    float cx = pr.left + (pw - (18 + 10 + ptw)) / 2;
    icon(stopStyle ? Icon::X : Icon::Play, cx, y + 10 + 17, 18, pc);
    if (text(pl, pf, cx + 28, y + 10, ptw + 2, 52, pc, 0, stopStyle ? 0 : -0.24f)) issues.push_back(L"clipped play");
    hit(ID_PLAY, pr, F_FOCUS | F_HAND | (disabled ? F_DISABLED : 0), 12);

    // activity toggle
    Font af{ Face::UI, 12.5f, 600, false };
    int badgeN = unseen_ ? unseen_ : (int)logs_.size();
    std::wstring bn = std::to_wstring(badgeN);
    float bw = std::max(18.f, 10 + textW(bn, Font{ Face::UI, 11, 400, false }));
    float aw = 12 + 16 + 8 + textW(L"Activity", af) + 8 + bw + 12 + 2;
    D2D1_RECT_F ar = rc(pr.left - 16 - aw, y + 19, aw, 34);
    bool ahov = hover_ == ID_ACT || actOpen_;
    box(ar, 8, C::surface, ahov ? 1.f : 0.f, C::line);
    uint32_t ac = ahov ? C::text : C::muted;
    icon(Icon::Log, ar.left + 13, y + 19 + 9, 16, ac);
    text(L"Activity", af, ar.left + 13 + 24, y + 19, 80, 34, ac);
    float bx = ar.right - 13 - bw;
    uint32_t bbg = unseen_ && unseenLevel_ == 2 ? C::bad : unseen_ && unseenLevel_ == 1 ? C::warn : C::surface2;
    uint32_t bfg = unseen_ && unseenLevel_ == 2 ? C::white : unseen_ && unseenLevel_ == 1 ? 0x111111 : C::ink2;
    fill(rc(bx, y + 27, bw, 18), bbg, 1, 9);
    text(bn, Font{ Face::UI, 11, 400, false }, bx, y + 27, bw, 18, bfg, 1);
    hit(ID_ACT, ar, F_FOCUS | F_HAND, 8);

    // status (left)
    float sx = x + 24, sw = ar.left - 16 - sx;
    float ty = y + (LB - (18.85f + 17.4f)) / 2;
    Font fa{ Face::UI, 13, 600, false }, fb{ Face::UI, 12, 400, false };
    if (isBusy) {
        static const struct { const wchar_t* cls; const wchar_t* label; } MP[] = {
            { L"", L"" }, { L"", L"Match ready" }, { L"ok", L"In the match" }, { L"ok", L"In the match" }, { L"ok", L"In the match" },
            { L"ok", L"In the match" }, { L"", L"Match over" }, { L"bad", L"Match server stopped" } };
        bool ph = se_.state == SessionState::Running && se_.phase != MatchPhase::None;
        int pcls = ph ? (!wcscmp(MP[(int)se_.phase].cls, L"ok") ? 0 : !wcscmp(MP[(int)se_.phase].cls, L"bad") ? 2 : -1) : 0;
        float tx = sx;
        if (se_.state == SessionState::Running) { dot(sx + 4, y + LB / 2, 8, ph ? (pcls == -1 ? C::ok : clsColor(pcls)) : C::ok); tx += 8 + 12; }
        else { spinner(sx, y + LB / 2 - 7, 14); tx += 14 + 12; }
        std::wstring head = se_.state == SessionState::Stopping ? L"Stopping…"
                          : se_.state != SessionState::Running ? L"Launching Fortnite " + se_.version + L"…"
                          : ph ? std::wstring(MP[(int)se_.phase].label) + L" · Fortnite " + se_.version
                          : L"Fortnite " + se_.version + L" is running";
        float tw = sx + sw - tx;
        text(head, fa, tx, ty, tw, 18.85f, C::text);
        float by = ty + 18.85f;
        if (ph) text(se_.matchDetail, fb, tx, by, tw, 17.4f, pcls == 0 ? C::ink2 : pcls == 2 ? C::badText : C::muted);
        else {
            struct St { Stage s; const wchar_t* l; } steps[3] = { { se_.backend, L"Backend" }, { se_.game, L"Game" }, { se_.server, L"Match server" } };
            int nst = se_.hasServer ? 3 : 2;
            float zx = tx;
            for (int i = 0; i < nst; i++) {
                if (i) { fill(rc(zx + 6, by + 8.2f, 16, 1), C::line); zx += 6 + 16 + 6; }
                Stage s = steps[i].s;
                if (s == Stage::Ok) { icon(Icon::Check, zx, by + 0.7f, 16, C::ok); zx += 16; }
                else if (s == Stage::Bad) { icon(Icon::X, zx, by + 0.7f, 16, C::bad); zx += 16; }
                else if (s == Stage::Run) { spinner(zx, by + 1.7f, 14); zx += 14; }
                else { dot(zx + 4, by + 8.7f, 8, C::dim); zx += 8; }
                if (W_ > 900) { zx += 6; text(steps[i].l, fb, zx, by, 200, 17.4f, C::muted); zx += textW(steps[i].l, fb); }
            }
        }
    } else if (se_.state == SessionState::Error) {
        icon(Icon::Warn, sx, y + LB / 2 - 8, 16, C::bad);
        text(se_.message.empty() ? L"Launch failed" : se_.message, fa, sx + 28, ty, sw - 28, 18.85f, C::badText);
        text(L"Details in Activity", fb, sx + 28, ty + 18.85f, sw - 28, 17.4f, C::muted);
    } else if (!b) {
        text(L"No build selected", fa, sx, ty, sw, 18.85f, C::text);
        text(L"Add or pick a build on the left", fb, sx, ty + 18.85f, sw, 17.4f, C::muted);
    } else {
        dot(sx + 4, y + LB / 2, 8, clsColor(level(*b)));
        std::wstring extra = matchOk(b) ? fmt(L"Mode picked in the game · %d bots in Battle Royale", c.bots) : L"Lobby and locker only";
        text(b->lobby == Support::No ? L"Not supported" : L"Ready · Fortnite " + b->version, fa, sx + 20, ty, sw - 20, 18.85f, C::text);
        text(extra + L" · " + c.displayName, fb, sx + 20, ty + 18.85f, sw - 20, 17.4f, C::muted);
    }
}

void App::drawMenu() {
    if (!menu_) return;
    int n; const MenuItem* it = menuItems(menu_, &n);
    Font f{ Face::UI, 13, 400, false }, sf{ Face::UI, 11.5f, 400, false };
    float cw = 0, h = 2 + 8;
    for (int i = 0; i < n; i++) {
        if (it[i].sep) { h += 9; continue; }
        h += 34;
        cw = std::max(cw, 10 + 16 + 10 + textW(it[i].text, f) + (it[i].small ? 16 + textW(it[i].small, sf) : 0) + 10);
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
    float bottom = H - LB - 16;
    Font f{ Face::UI, 12.5f, 400, false };
    for (int i = (int)toasts_.size() - 1; i >= 0; i--) {
        const Toast& t = toasts_[i];
        float maxText = 420 - 32 - 2 - 8 - 12;
        float tw = std::min(maxText, textW(t.text, f) + 1);
        float th = rich(t.text, f, 0, 0, tw, 18.125f, C::text, C::text, 0, false);
        float w = 32 + 2 + 8 + 12 + tw, h = 20 + 2 + std::max(th, 8.f);
        D2D1_RECT_F r = rc(W - 24 - w, bottom - h, w, h);
        overlays_.push_back(r);
        shadow(r, 12, 30, 12, 0.45f);
        box(r, 12, C::surface, 1, C::line);
        uint32_t dc = t.level == Level::Ok ? C::ok : t.level == Level::Warn ? C::warn : t.level == Level::Bad ? C::bad : C::dim;
        dot(r.left + 17 + 4, r.top + h / 2, 8, dc);
        rich(t.text, f, r.left + 17 + 8 + 12, r.top + 11, tw, 18.125f, C::text, C::text);
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
    if (id != hover_) { hover_ = id; invalidate(); }
}

void App::mouseLeave() { if (hover_) { hover_ = 0; invalidate(); } }

void App::mouseDown(float x, float y) {
    focusVisible_ = false;
    const Hit* h = findHit(x, y);
    int id = h ? h->id : 0;
    int toggle = menu_ == 1 ? ID_ADD : menu_ == 2 ? ID_BUILDMENU : 0;
    if (menu_ && id != ID_POPUP && !(id >= ID_MENU0 && id < ID_MENU0 + 20) && id != toggle) { menu_ = 0; invalidate(); }
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
    const Hit* l = hitById(ID_LINES);
    if (actOpen_ && l && inside(l->r, x, y)) {
        logScroll_ = std::max(0.f, logScroll_ - delta / 120.f * LOG_LH * 3);
        logFollow_ = logScroll_ >= logMax_ - 8;
    } else if (inside(contentClip_, x, y)) {
        contentScroll_ -= delta / 120.f * 100;
    } else return;
    invalidate();
}

void App::activate(int id) {
    const std::wstring& b = selected_;
    if (id >= ID_BUILD0) { int i = id - ID_BUILD0; if (i < (int)st_.builds.size()) select(st_.builds[i].name); return; }
    if (id >= ID_MENU0) {
        int n; const MenuItem* it = menuItems(menu_, &n);
        int i = id - ID_MENU0;
        int m = menu_;
        menu_ = 0;
        if (i < n && !it[i].sep) core_->command(it[i].cmd, b);
        setFocus(m == 1 ? ID_ADD : ID_BUILDMENU, focusVisible_);
        invalidate();
        return;
    }
    Config c;
    switch (id) {
    case ID_MIN: ShowWindow(hwnd_, SW_MINIMIZE); break;
    case ID_MAX: ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); break;
    case ID_CLOSE: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case ID_ADD: case ID_BUILDMENU: {
        int m = id == ID_ADD ? 1 : 2;
        menu_ = menu_ == m ? 0 : m;
        if (menu_ && focusVisible_) focus_ = ID_MENU0;   // keyboard: the first item
        invalidate();
        break;
    }
    case ID_ROOT: case ID_EMPTY_ROOT: core_->command(Command::ChooseRoot, b); break;
    case ID_EMPTY_ZIP: core_->command(Command::AddZip, b); break;
    case ID_EMPTY_FOLDER: core_->command(Command::AddFolder, b); break;
    case ID_NOTICE_FW: core_->command(Command::Firewall, b); break;
    case ID_NOTICE_COS: core_->command(Command::Cosmetics, b); break;
    case ID_STEP_DN: case ID_STEP_UP:
        c.bots = std::max(0, std::min(99, st_.config.bots + (id == ID_STEP_UP ? 1 : -1)));
        setConfig(ConfigPatch::Bots, c);
        break;
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
        for (auto& e : edits_) if (e.id == id && e.shown) target = e.hwnd;
        if (GetFocus() != target) SetFocus(target);
        if (target != hwnd_ && keyboard) SendMessageW(target, EM_SETSEL, 0, -1);   // like tabbing into an input
    }
    ensureVisible(id);
    invalidate();
}

void App::ensureVisible(int id) {
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
    for (auto& h : hits_) {
        if (!(h.flags & F_FOCUS) || (h.flags & F_DISABLED)) continue;
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
    if (vk == VK_ESCAPE) {
        if (menu_) { int t = menu_ == 1 ? ID_ADD : ID_BUILDMENU; menu_ = 0; setFocus(t, focusVisible_); return true; }
        return false;
    }
    if (ctrl && vk == VK_RETURN) { const Hit* p = hitById(ID_PLAY); if (p && !(p->flags & F_DISABLED)) activate(ID_PLAY); return true; }
    if (ctrl && vk == 'L') { openActivity(!actOpen_); return true; }
    if (vk == VK_TAB) { if (menu_) { int t = menu_ == 1 ? ID_ADD : ID_BUILDMENU; menu_ = 0; focus_ = t; } moveFocus(shift ? -1 : 1); return true; }
    if (menu_ && (vk == VK_DOWN || vk == VK_UP)) { moveFocus(vk == VK_DOWN ? 1 : -1); return true; }
    int f = focus_;
    if (f >= ID_BUILD0 && (vk == VK_DOWN || vk == VK_UP)) {
        if (busy() || st_.builds.empty()) return true;
        int i = -1;
        for (size_t k = 0; k < st_.builds.size(); k++) if (st_.builds[k].name == selected_) i = (int)k;
        int j = std::max(0, std::min((int)st_.builds.size() - 1, i + (vk == VK_DOWN ? 1 : -1)));
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
    if (vk == VK_RETURN || vk == VK_SPACE) {
        const Hit* h = hitById(f);
        if (h && !(h->flags & F_DISABLED) && f != ID_LINES) { focusVisible_ = true; activate(f); return true; }
    }
    if (vk == VK_PRIOR || vk == VK_NEXT) { contentScroll_ += (vk == VK_NEXT ? 1 : -1) * contentView_ * 0.9f; invalidate(); return true; }
    return false;
}

} // namespace rn
