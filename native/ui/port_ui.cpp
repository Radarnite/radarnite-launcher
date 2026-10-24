// The cosmetics panel of the window: launcher/ui/index.html's "port" section (radarnite.com's Browse all) drawn with
// Direct2D. Header, the filter bar (category, filters, "Not imported", search, sort), the live import box, a
// virtualized grid of cards (only the visible rows are drawn and only their images loaded), the Import dialog and the
// preview on click. The catalog is shared with the core (never copied); the filtered list is rebuilt only when a
// filter, the search or the catalog changes.
#include "ui.h"
#include "port_ui.h"
#include <shellapi.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <wctype.h>
#include <algorithm>

namespace rn {
namespace {

// ui.cpp's hit flags, animation kinds and chrome sizes (same values: the hits drawn here are read there). The panel's
// own animation kinds start at 9.
enum { F_FOCUS = 1, F_HAND = 2, F_EDIT = 4, F_DISABLED = 8, F_RING_IN = 16, F_CONTENT = 32 };
const uint32_t K_HOVER = 1u << 16, K_COLOR = 2u << 16, K_SHIMMER = 8u << 16, K_RING = 9u << 16, K_FADE = 10u << 16;
const float TB = 40;

std::wstring fmt(const wchar_t* f, ...) {
    wchar_t buf[1024];
    va_list a; va_start(a, f); vswprintf(buf, 1024, f, a); va_end(a);
    return buf;
}
std::wstring thousands(int n) {
    std::wstring s = std::to_wstring(n < 0 ? -n : n), o;
    int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; i--) { o.insert(o.begin(), s[i]); if (++c % 3 == 0 && i > 0) o.insert(o.begin(), L','); }
    return n < 0 ? L"-" + o : o;
}
D2D1_RECT_F inter(D2D1_RECT_F a, D2D1_RECT_F b) { return { std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom) }; }
D2D1_RECT_F rc(float x, float y, float w, float h) { return { x, y, x + w, y + h }; }
D2D1_RECT_F grow(D2D1_RECT_F r, float d) { return r.right > r.left ? D2D1_RECT_F{ r.left - d, r.top - d, r.right + d, r.bottom + d } : r; }

// the page's tokens for the panel (.port: --n-*)
namespace N {
const uint32_t ink = 0xF5F6F7, mut = 0xA1A1AA, line = 0x303238, surf = 0x202228, on = 0x111216, bg = 0x101114, panel = 0x181A1F,
               dim = 0x71717A, hov = 0x292C33, l2 = 0x3A3D44, l3b = 0x55575F, oncount = 0x5A5D66, l25b = 0x45484F,
               surf2 = 0x24262C, accent = 0xE5E7EB;
}

const wchar_t* kPT[] = { L"Outfits", L"Emotes", L"Back blings", L"Pickaxes", L"Gliders", L"Wraps", L"Loading screens", L"Sprays", L"Emoticons" };
const wchar_t* kPT1[] = { L"Outfit", L"Emote", L"Back Bling", L"Pickaxe", L"Glider", L"Wrap", L"Loading Screen", L"Spray", L"Emoticon" };
const Icon kPIC[] = { Icon::POutfit, Icon::PEmote, Icon::PBackpack, Icon::PPickaxe, Icon::PGlider, Icon::PWrap, Icon::PLoading, Icon::PSpray, Icon::PEmoji };
const int NT = (int)PortType::Count;
// average pak size per item (MB), measured on the first fn-port imports into 14.30, and seconds per item
const float kPMB[] = { 70, 3.7f, 7, 7, 9, 2, 2, 1, .5f };
const float kPSEC = 14;
const wchar_t* kSTS[] = { L"Not imported", L"Imported", L"In the build", L"Failed" };
const wchar_t* kSort[] = { L"Newest first", L"Oldest first", L"A to Z", L"Rarity" };

// rarities of the filter (radarnite.com's order) with the chips' gradients, and the cards' [dark, light] colours
struct Rar { const char* key; const wchar_t* label; uint32_t c1, c2; };
const Rar kRAR[] = {
    { "gaminglegends", L"Gaming", 0x4A2C8F, 0x8A42C8 }, { "icon", L"Icon", 0x0F9FB0, 0x2BC6CF }, { "dc", L"DC", 0x1F3F9C, 0x3462D9 },
    { "marvel", L"Marvel", 0x9E1C22, 0xD33A2C }, { "starwars", L"Star Wars", 0x111317, 0x2A2D35 }, { "frozen", L"Frozen", 0x6FA8DC, 0xA9D4F5 },
    { "lava", L"Lava", 0xB4421F, 0xEE8A2E }, { "dark", L"Dark", 0x4A1460, 0x9A2BB5 }, { "shadow", L"Shadow", 0x2B2C30, 0x55565C },
    { "slurp", L"Slurp", 0x1A7FD0, 0x36B4EE }, { "legendary", L"Legendary", 0xB4561D, 0xE07A2E }, { "epic", L"Epic", 0x7A3BC4, 0xA55BE5 },
    { "rare", L"Rare", 0x2165C2, 0x3B8CEB }, { "uncommon", L"Uncommon", 0x3F8A1F, 0x62B132 }, { "common", L"Common", 0x6E6F75, 0x9A9BA1 },
};
const int NR = (int)(sizeof kRAR / sizeof kRAR[0]);
struct RC { const char* key; uint32_t dark, light; };
const RC kRC[] = {
    { "legendary", 0x8a4a0e, 0xf39a3a }, { "epic", 0x4a1f80, 0xb06cff }, { "rare", 0x153d78, 0x5ab0ff }, { "uncommon", 0x1f5420, 0x6fd96f },
    { "common", 0x3a3f47, 0xaab2bd }, { "icon", 0x0f5560, 0x4fe3ef }, { "marvel", 0x5a1212, 0xff5c5c }, { "dc", 0x152a6e, 0x5d86ff },
    { "gaminglegends", 0x23267a, 0x7f8cff }, { "starwars", 0x0e1a3f, 0x3d5fd6 }, { "frozen", 0x1d4d6b, 0xa5e6ff }, { "lava", 0x6b2200, 0xff8a4a },
    { "shadow", 0x111317, 0x5f6675 }, { "slurp", 0x0f5a52, 0x5df0dc }, { "dark", 0x4a0f3f, 0xff5be8 }, { "columbus", 0x1c3150, 0x6ea6ff },
    { "cubeseries", 0x3e1360, 0xc36dff },
};
const uint32_t GREEN_ = 0x3DFF6E;   // the lobby's green (.badge.ok)
const RC& rcOf(const std::string& r) { for (auto& x : kRC) if (r == x.key) return x; return kRC[4]; }
int rarIndex(const std::string& r) { for (int i = 0; i < NR; i++) if (r == kRAR[i].key) return i; return -1; }

uint32_t scaleRgb(uint32_t c, float k) {
    return ((uint32_t)lroundf(((c >> 16) & 255) * k) << 16) | ((uint32_t)lroundf(((c >> 8) & 255) * k) << 8) | (uint32_t)lroundf((c & 255) * k);
}

// ASCII-and-beyond case-insensitive "contains" without allocating (needle already lower case)
bool icontains(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        size_t k = 0;
        while (k < needle.size() && (wchar_t)towlower(hay[i + k]) == needle[k]) k++;
        if (k == needle.size()) return true;
    }
    return false;
}

std::wstring fmtS(int64_t ms) {
    int64_t s = std::max<int64_t>(0, (ms + 500) / 1000);
    if (s < 60) return fmt(L"%lld s", (long long)s);
    if (s < 3600) return fmt(L"%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    return fmt(L"%lld h %lld min", (long long)(s / 3600), (long long)((s % 3600 + 30) / 60));
}
std::wstring fmtGB(double gb) {
    if (gb < 1) return fmt(L"%d MB", std::max(1, (int)lround(gb * 1024)));
    return gb < 10 ? fmt(L"%.1f GB", gb) : fmt(L"%d GB", (int)lround(gb));
}
std::wstring fmtH(double h) {
    if (h < 1) return fmt(L"%d min", std::max(1, (int)lround(h * 60)));
    return h < 10 ? fmt(L"%.1f h", h) : fmt(L"%d h", (int)lround(h));
}
std::wstring cap(const std::string& s) { std::wstring w(s.begin(), s.end()); if (!w.empty()) w[0] = (wchar_t)towupper(w[0]); return w; }
std::wstring tail(const std::wstring& path) { size_t d = path.rfind(L'.'); return d == std::wstring::npos ? path : path.substr(d + 1); }
// "C5 S1" -> "Chapter 5, Season 1" ("C1 SX" -> "Season X", "C4 SOG" -> "OG"), "" when it has another shape
std::wstring introduced(const std::wstring& k) {
    if (k.size() < 4 || k[0] != L'C') return L"";
    size_t sp = k.find(L" S");
    if (sp == std::wstring::npos || sp < 2) return L"";
    std::wstring c = k.substr(1, sp - 1), s = k.substr(sp + 2);
    if (c.empty() || s.empty()) return L"";
    return L"Chapter " + c + (s == L"OG" ? L", OG" : L", Season " + s);
}

} // namespace

// ================================================================================================ sink + state
void App::onPortCatalog(std::shared_ptr<const PortCatalog> c) { Event e{}; e.type = 4; e.cat = std::move(c); EnterCriticalSection(&cs_); queue_.push_back(std::move(e)); LeaveCriticalSection(&cs_); if (hwnd_) PostMessageW(hwnd_, WM_APP_SINK, 0, 0); }
void App::onPortLoad(const PortLoad& l) { Event e{}; e.type = 5; e.load = l; EnterCriticalSection(&cs_); queue_.push_back(std::move(e)); LeaveCriticalSection(&cs_); if (hwnd_) PostMessageW(hwnd_, WM_APP_SINK, 0, 0); }
void App::onPortJob(std::shared_ptr<const PortJob> j) { Event e{}; e.type = 6; e.job = std::move(j); EnterCriticalSection(&cs_); queue_.push_back(std::move(e)); LeaveCriticalSection(&cs_); if (hwnd_) PostMessageW(hwnd_, WM_APP_SINK, 0, 0); }
void App::onPortPreview(const PortPreview& p) { Event e{}; e.type = 7; e.pv = p; EnterCriticalSection(&cs_); queue_.push_back(std::move(e)); LeaveCriticalSection(&cs_); if (hwnd_) PostMessageW(hwnd_, WM_APP_SINK, 0, 0); }

void App::imagesArrived() {
    if (!pOpen_) return;
    if (pPv_ >= 0 || pj_) invalidate();
    else dirty({ pBody_.left, pBody_.top - 20, pBody_.right, pBody_.bottom });
}

void App::portEvent(Event& e) {
    switch (e.type) {
    case 4: {   // a catalog (a partial one first): keep the selection and the preview by id
        if (!e.cat || e.cat->build != pBuild_) break;
        std::vector<std::wstring> sel;
        std::wstring pv;
        if (pCat_) {
            for (size_t i = 0; i < pSel_.size() && i < pCat_->items.size(); i++) if (pSel_[i]) sel.push_back(pCat_->items[i].id);
            if (pPv_ >= 0 && pPv_ < (int)pCat_->items.size()) pv = pCat_->items[pPv_].id;
        }
        pCat_ = std::move(e.cat);
        pLoading_ = pCat_->partial;
        anim_.stop(K_SHIMMER | 10);
        pRi_.clear();
        std::unordered_map<std::wstring, int>().swap(pPath_);
        pSel_.assign(pCat_->items.size(), 0); pSelN_ = 0;
        pPv_ = -1;
        if (!sel.empty() || !pv.empty()) {
            std::unordered_map<std::wstring, int> ids;
            ids.reserve(pCat_->items.size());
            for (size_t i = 0; i < pCat_->items.size(); i++) ids[pCat_->items[i].id] = (int)i;
            for (auto& s : sel) { auto f = ids.find(s); if (f != ids.end() && importable(pCat_->items[f->second])) { pSel_[f->second] = 1; pSelN_++; } }
            auto f = ids.find(pv);
            if (f != ids.end()) pPv_ = f->second;
        }
        pDirty_ = true;
        break;
    }
    case 5: {   // a loading step
        if (e.load.build != pBuild_) break;
        PortRow* r = nullptr;
        for (auto& x : pRows_) if (x.stage == e.load.stage) r = &x;
        if (!r) { pRows_.push_back({ e.load.stage, e.load.ms, e.load.done, GetTickCount64() }); break; }
        r->ms = e.load.ms; r->done = e.load.done;
        break;
    }
    case 6: {   // the import: when it ends, the statuses are read again (the grid stays as it is meanwhile)
        // its last view (phase done / stopped / failed) stays as the finished card until dismissed or a new import
        if (e.job && (e.job->phase == L"done" || e.job->phase == L"stopped" || e.job->phase == L"failed")) { pjEnd_ = std::move(e.job); e.job.reset(); }
        else if (e.job) pjEnd_.reset();
        bool ended = pj_ && !e.job;
        pj_ = std::move(e.job);
        pjAt_ = GetTickCount64();
        if (ended && pCat_ && !pBuild_.empty()) core_->portCatalog(pBuild_, false);
        break;
    }
    case 7:     // the preview's details
        if (pPv_ >= 0 && pCat_ && pPv_ < (int)pCat_->items.size() && !_wcsicmp(pCat_->items[pPv_].id.c_str(), e.pv.id.c_str())) {
            pPvData_ = e.pv;
            pPvWait_ = false;
        }
        break;
    }
    updateSpinTimer();
}

bool App::portWheel(float x, float y, float delta) {
    if (!pOpen_) return false;
    if (pDlg_ || pPv_ >= 0 || pPop_) return true;   // nothing scrolls under an overlay
    D2D1_RECT_F r = pBody_;
    if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) return false;
    pScroll_ -= delta / 120.f * 120;
    invalidate();
    return true;
}

// The panel's scrollbar: a click on the track pages towards it, a press on the thumb drags it.
void App::portScrollbar(float y, bool down) {
    float view = pView_, total = std::max(pTotal_, view + 1);
    D2D1_RECT_F r = pBody_;
    float trackH = r.bottom - r.top, th = std::max(24.f, trackH * view / total), maxS = std::max(1.f, total - view);
    if (down) {
        float ty = r.top + (trackH - th) * pScroll_ / maxS;
        if (y < ty || y > ty + th) { pScroll_ += y < ty ? -view : view; invalidate(); return; }
        drag_ = ID_P_SB; dragY0_ = y; dragV0_ = pScroll_;
        if (hwnd_) SetCapture(hwnd_);
        return;
    }
    pScroll_ = dragV0_ + (y - dragY0_) * maxS / std::max(1.f, trackH - th);
    invalidate();
}

void App::portShow(bool open) {
    if (open == pOpen_) return;
    pOpen_ = open;
    menu_ = 0;
    if (open) {
        if (pBuild_ != selected_ || !pCat_) portLoad(false);
        else { pDirty_ = true; }
        focus_ = 0;
    } else {
        pPop_ = 0; pDlg_ = false; pPv_ = -1;
        anim_.stop(K_SHIMMER | 10);
        if (hwnd_) SetWindowTextW(edits_[3].hwnd, L"");
        pQ_.clear(); pDirty_ = true;
        std::unordered_map<std::wstring, int>().swap(pPath_);
        std::vector<int>().swap(pList_);
        img_.setTarget(rt_);   // the decoded images go too (the disk cache stays)
        HeapCompact(GetProcessHeap(), 0);   // and the heap gives their pages back
        focus_ = ID_P_OPEN;
    }
    updateSpinTimer();
    invalidate();
}

void App::portLoad(bool refresh) {
    if (img_.dir().empty()) img_.setDir(core_->imageCacheDir());
    pBuild_ = selected_;
    pLoading_ = true;
    pRows_.clear();
    pLoadT0_ = GetTickCount64();
    pCat_.reset();
    pSel_.clear(); pSelN_ = 0;
    pRi_.clear();
    std::unordered_map<std::wstring, int>().swap(pPath_);
    pPv_ = -1; pDlg_ = false;
    pDirty_ = true; pScroll_ = 0; pCursor_ = 0;
    core_->portCatalog(pBuild_, refresh);
    updateSpinTimer();
    invalidate();
}

bool App::portMatch(const PortItem& it, int skip) const {
    if (skip != 1 && pTypes_ && !(pTypes_ & (1u << (int)it.type))) return false;
    if (skip != 2 && pSts_ && !(pSts_ & (1u << (int)it.status))) return false;
    if (skip != 3 && pRars_) {
        int ri = -1;
        size_t i = &it - &pCat_->items[0];
        if (i < pRi_.size()) ri = pRi_[i];
        if (ri < 0 || !(pRars_ & (1u << ri))) return false;
    }
    if (!pQ_.empty() && !icontains(it.name, pQ_) && !icontains(it.set, pQ_) && !icontains(it.id, pQ_) && !icontains(it.path, pQ_)) return false;
    return true;
}

void App::portRebuild() {
    pDirty_ = false;
    pList_.clear();
    memset(pCntT_, 0, sizeof pCntT_); memset(pCntS_, 0, sizeof pCntS_); memset(pCntR_, 0, sizeof pCntR_);
    pTodo_ = 0;
    pNewAll_ = -1;
    if (!pCat_) return;
    const auto& items = pCat_->items;
    if (!pCat_->partial && pCat_->error.empty()) { pNewAll_ = 0; for (auto& it : items) if (it.status == PortStatus::New) pNewAll_++; }
    if (pRi_.size() != items.size()) { pRi_.resize(items.size()); for (size_t i = 0; i < items.size(); i++) pRi_[i] = (int8_t)rarIndex(items[i].rarity); }
    // counts like the web: each group counted with the other filters applied
    for (size_t i = 0; i < items.size(); i++) {
        const PortItem& it = items[i];
        bool q = pQ_.empty() || icontains(it.name, pQ_) || icontains(it.set, pQ_) || icontains(it.id, pQ_) || icontains(it.path, pQ_);
        if (!q) continue;
        bool t = !pTypes_ || (pTypes_ & (1u << (int)it.type)), s = !pSts_ || (pSts_ & (1u << (int)it.status));
        int ri = pRi_[i];
        bool r = !pRars_ || (ri >= 0 && (pRars_ & (1u << ri)));
        if (s && r) pCntT_[(int)it.type]++;
        if (t && r) pCntS_[(int)it.status]++;
        if (t && s && ri >= 0) pCntR_[ri]++;
        if (t && s && r) { pList_.push_back((int)i); if (importable(it)) pTodo_++; }
    }
    auto name = [&](int i) -> const std::wstring& { return items[i].name.empty() ? items[i].id : items[i].name; };
    auto az = [&](int a, int b) { return _wcsicmp(name(a).c_str(), name(b).c_str()) < 0; };
    switch (pSort_) {
    case 0: std::stable_sort(pList_.begin(), pList_.end(), [&](int a, int b) { return items[a].added > items[b].added; }); break;
    case 1: std::stable_sort(pList_.begin(), pList_.end(), [&](int a, int b) {
                const std::string &x = items[a].added, &y = items[b].added;
                return x.empty() != y.empty() ? !x.empty() : x < y; }); break;
    case 2: std::stable_sort(pList_.begin(), pList_.end(), az); break;
    default: std::stable_sort(pList_.begin(), pList_.end(), [&](int a, int b) {
                int ra = pRi_[a] < 0 ? 99 : pRi_[a], rb = pRi_[b] < 0 ? 99 : pRi_[b];
                return ra != rb ? ra < rb : az(a, b); }); break;
    }
    pCursor_ = std::min(pCursor_, std::max(0, (int)pList_.size() - 1));
}

// ================================================================================================ drawing helpers
// Wrapped text, at most maxLines (ellipsis on the last), uniform line height. Returns the height.
float App::para(const std::wstring& s, const Font& f, float x, float top, float w, float lh, int maxLines, uint32_t c, bool draw) {
    if (s.empty() || w <= 0) return 0;
    IDWriteTextLayout* l = nullptr;
    if (FAILED(g_.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), g_.format(f), w, maxLines > 0 ? lh * maxLines : 100000, &l))) return 0;
    l->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    float asc, h; g_.fontBox(f, &asc, &h);
    l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lh, (lh - h) / 2 + asc);
    if (maxLines > 0) {
        DWRITE_TRIMMING t = { DWRITE_TRIMMING_GRANULARITY_WORD, 0, 0 };
        IDWriteInlineObject* e = nullptr; g_.dw->CreateEllipsisTrimmingSign(g_.format(f), &e);
        l->SetTrimming(&t, e); release(e);
    }
    DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m);
    int lines = maxLines > 0 ? std::min((int)m.lineCount, maxLines) : (int)m.lineCount;
    if (draw && !measuring_) rt_->DrawTextLayout(D2D1::Point2F(x, top), l, br(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    l->Release();
    return lines * lh;
}

// .port .chip: 37 high, 12 px sides, 500 12.5 px, icon 13, count after the label (pill: the Filters count).
float App::chip(int id, float x, float y, float h, const std::wstring& label, int ic, const std::wstring& count, bool on, bool disabled, bool measure, bool pill) {
    Font f{ Face::UI, 12.5f, 500, false }, cf{ Face::UI, pill ? 11.f : 11.5f, 600, false, true };
    float lw = textW(label, f), cw = count.empty() ? 0 : pill ? std::max(18.f, textW(count, cf) + 10) : textW(count, cf);
    float w = 12 + (ic >= 0 ? 13 + 6 : 0) + lw + (count.empty() ? 0 : 6 + cw) + 12 + 2;
    if (measure) return w;
    D2D1_RECT_F r = rc(x, y, w, h);
    bool hov = hover_ == id && !disabled;
    float hv = fx(K_HOVER | id, hov ? 1.f : 0.f, r, 150);
    float a0 = alpha_;
    if (disabled) alpha_ *= .4f;
    uint32_t bgc = on ? (hov ? 0xFFFFFF : N::accent) : N::surf, bc = on ? N::accent : mix(N::line, N::l2, hv);
    if (on) box(r, 9, bgc, 1, bc);
    else if (glassChips_) glass(r, 9, .62f + .2f * hv, .12f + .16f * hv);
    else box(r, 9, bgc, hv, bc);
    uint32_t col = on ? N::on : glassChips_ ? C::white : mix(C::muted, C::text, hv);
    float cx = x + 1 + 12;
    if (ic >= 0) { icon((Icon)ic, cx, y + (h - 13) / 2, 13, col, .85f); cx += 13 + 6; }
    text(label, f, cx, y, lw + 1, h, col);
    cx += lw;
    if (!count.empty()) {
        cx += 6;
        if (pill) { fill(rc(cx, y + (h - 18) / 2, cw, 18), N::accent, 1, 9); text(count, cf, cx, y, cw, h, N::on, 1); }
        else text(count, cf, cx, y, cw + 1, h, on ? N::l3b : glassChips_ ? C::muted : C::dim);
    }
    alpha_ = a0;
    hit(id, r, F_FOCUS | F_HAND | (disabled ? F_DISABLED : 0), 9);
    return w;
}

App::RarBrush* App::rarBrush(const std::string& rarity) {
    const RC& c = rcOf(rarity);
    for (auto& b : rarBrushes_) if (b.key == c.key) return &b;
    if (!rt_) return nullptr;
    RarBrush b{ c.key, nullptr, nullptr, c.dark };
    ID2D1GradientStopCollection* gs = nullptr;
    // --bg: linear-gradient(180deg, light 0%, dark 100%)
    const D2D1_GRADIENT_STOP s1[] = { { 0, rgb(c.light) }, { 1, rgb(c.dark) } };
    if (SUCCEEDED(rt_->CreateGradientStopCollection(s1, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs))) rt_->CreateLinearGradientBrush({}, gs, &b.bg);
    release(gs);
    // .fsh: linear-gradient(0deg, ct 0%, ct 30%, transparent 100%), ct = color-mix(dark 55%, #000)
    uint32_t ct = scaleRgb(c.dark, .55f);
    const D2D1_GRADIENT_STOP s2[] = { { 0, rgb(ct) }, { .3f, rgb(ct) }, { 1, rgb(ct, 0) } };
    if (SUCCEEDED(rt_->CreateGradientStopCollection(s2, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs))) rt_->CreateLinearGradientBrush({}, gs, &b.shade);
    release(gs);
    rarBrushes_.push_back(b);
    return &rarBrushes_.back();
}
void App::releasePortBrushes() { for (auto& b : rarBrushes_) { release(b.bg); release(b.shade); } rarBrushes_.clear(); }

// ================================================================================================ the panel
void App::drawPort(float x0, float y0, float W, float H) {
    if (pDirty_) portRebuild();
    const Build* b = sel();
    // under it: the season without the skin, darkening towards the grid (baked in the stage: App::drawStage)
    float px = 24, x = x0 + px, w = W - 2 * px;
    glassChips_ = true;
    // ---- header: title, sub, Refresh, Close
    float y = y0 + 16;
    float cw = gbutton(ID_P_CLOSE, 0, 0, 36, L"Close", Icon::X, true, true);   // glass, like the lobby's top buttons
    float rw = gbutton(ID_P_REFRESH, 0, 0, 36, L"Refresh", Icon::Refresh, false, true);
    float hh = 28 + 4 + 17.4f;
    text(L"CURRENT FORTNITE COSMETICS", Font{ Face::Display, 28, 900, true }, x, y, w - cw - rw - 24, 28, C::text, 0, -0.56f);
    if (b) text(L"Imported into Fortnite " + b->version + L" as new items · nothing of the build is replaced", Font{ Face::UI, 12, 400, false }, x, y + 32, w - cw - rw - 24, 17.4f, C::muted);
    gbutton(ID_P_REFRESH, x + w - cw - 10 - rw, y + (hh - 36) / 2, 36, L"Refresh", Icon::Refresh, false);
    gbutton(ID_P_CLOSE, x + w - cw, y + (hh - 36) / 2, 36, L"Close", Icon::X, true);
    y += hh + 20;

    // ---- the filter bar: category, Filters, Not imported, search, sort
    const float bh = 37;
    Font sf{ Face::UI, 12.5f, 500, false };
    int nFil = __builtin_popcount(pTypes_) + __builtin_popcount(pSts_) + __builtin_popcount(pRars_);
    std::wstring catL = !pTypes_ ? L"All" : __builtin_popcount(pTypes_) == 1 ? kPT[__builtin_ctz(pTypes_)] : fmt(L"%d types", __builtin_popcount(pTypes_));
    bool newOn = pSts_ == 1u;
    std::wstring newN = pCntS_[0] ? thousands(pCntS_[0]) : L"";
    float catW = std::max(120.f, std::min(170.f, 12 + 14 + 6 + textW(catL, sf) + 28 + 2));
    float filW = chip(ID_P_FILTERS, 0, 0, bh, L"Filters", (int)Icon::PFilters, nFil ? std::to_wstring(nFil) : L"", false, false, true, true);
    float newW = chip(ID_P_NEW, 0, 0, bh, L"Not imported", (int)Icon::PNew, newN, newOn, false, true);
    bool sortIconOnly = false;
    float sortW = 12 + 14 + 6 + textW(L"Sort", sf) + 28 + 2;
    float fixed = catW + filW + newW + sortW + 4 * 8;
    float searchW = std::min(220.f, w - fixed);
    if (searchW < 140) { sortIconOnly = true; sortW = 12 + 14 + 28 + 2; searchW = std::max(110.f, std::min(220.f, w - (fixed - (12 + 6 + textW(L"Sort", sf))))); }
    bool showNew = catW + filW + newW + sortW + searchW + 4 * 8 <= w + 0.5f;
    if (!showNew) searchW = std::max(110.f, std::min(220.f, w - (catW + filW + sortW + 3 * 8)));
    float bx = x;
    auto selBtn = [&](int id, float sx, float sw, Icon ic, const std::wstring& label, bool open) {
        D2D1_RECT_F r = rc(sx, y, sw, bh);
        bool hov = hover_ == id;
        float hv = fx(K_HOVER | id, hov || open ? 1.f : 0.f, r, 150);
        glass(r, 9, .62f + .2f * std::max(hv, open ? 1.f : 0.f), .12f + .16f * hv);
        icon(ic, sx + 1 + 12, y + (bh - 14) / 2, 14, C::white);
        if (!label.empty()) text(label, sf, sx + 1 + 12 + 14 + 6, y, sw - 2 - 12 - 14 - 6 - 28, bh, C::text);
        // the ::after chevron: 5 px, border right/bottom, rotated 45 deg
        float cx = sx + sw - 1 - 9 - 2.5f, cy = y + bh / 2 - 1.5f;
        if (!measuring_) {
            rt_->DrawLine(D2D1::Point2F(cx - 3, cy - 1.5f), D2D1::Point2F(cx, cy + 1.5f), br(C::muted), 1.5f);
            rt_->DrawLine(D2D1::Point2F(cx, cy + 1.5f), D2D1::Point2F(cx + 3, cy - 1.5f), br(C::muted), 1.5f);
        }
        hit(id, r, F_FOCUS | F_HAND, 9);
    };
    selBtn(ID_P_CAT, bx, catW, Icon::PAll, catL, pPop_ == 1);
    if (pPop_ == 1) pPopAnchor_ = rc(bx, y, catW, bh);
    bx += catW + 8;
    chip(ID_P_FILTERS, bx, y, bh, L"Filters", (int)Icon::PFilters, nFil ? std::to_wstring(nFil) : L"", pPop_ == 2, false, false, true);
    if (pPop_ == 2) pPopAnchor_ = rc(bx, y, filW, bh);
    bx += filW + 8;
    if (showNew) { chip(ID_P_NEW, bx, y, bh, L"Not imported", (int)Icon::PNew, newN, newOn, false); bx += newW + 8; }
    float sortX = x + w - sortW, sx = sortX - 8 - searchW;
    {
        EditBox& e = edits_[3];
        bool foc = hwnd_ && GetFocus() == e.hwnd;
        D2D1_RECT_F r = rc(sx, y, searchW, bh);
        if (foc && !measuring_) rt_->DrawRoundedRectangle(D2D1::RoundedRect(grow(r, 1.5f), 10.5f, 10.5f), br(0xE5E7EB, .08f), 3);
        box(r, 9, 0x0B0E19, 1, C::white, fx(K_COLOR | ID_P_SEARCH, foc ? .34f : .12f, r, 150));   // FIELD: the EDIT paints the same
        icon(Icon::PSearch, sx + 10, y + (bh - 14) / 2, 14, C::muted);
        D2D1_RECT_F inner = { sx + 32, y + 1, sx + searchW - 12, y + bh - 1 };
        if (!measuring_) { e.r = inner; e.clip = clip_; e.show = hwnd_ != nullptr; }
        if (!e.show) text(pQ_.empty() ? L"Search name or ID (EID_…)" : pQ_, Font{ Face::UI, 13, 400, false }, inner.left, y, inner.right - inner.left, bh, pQ_.empty() ? C::muted : C::text);
        hit(ID_P_SEARCH, r, F_FOCUS | F_EDIT, 9);
    }
    selBtn(ID_P_SORT, sortX, sortW, Icon::PSort, sortIconOnly ? L"" : L"Sort", pPop_ == 3);
    if (pPop_ == 3) pPopAnchor_ = rc(sortX, y, sortW, bh);
    y += bh + 12;

    // ---- the body: scrolls on its own; only the visible rows of the grid are drawn
    float bodyTop = y, bodyH = y0 + H - y;
    pBody_ = rc(x0, bodyTop, W, bodyH);
    pView_ = bodyH;
    bool sb = pTotal_ > bodyH + 0.5f;
    pScroll_ = std::max(0.f, std::min(pScroll_, std::max(0.f, pTotal_ - bodyH)));
    pScroll_ = roundf(pScroll_ * scale_) / scale_;
    D2D1_RECT_F savedClip = clip_;
    clip_ = { x0, bodyTop, x0 + W - (sb ? 10 : 0), bodyTop + bodyH };
    rt_->PushAxisAlignedClip(clip_, D2D1_ANTIALIAS_MODE_ALIASED);
    float savedOy = oy_;
    D2D1_MATRIX_3X2_F savedBase = base_;
    oy_ = bodyTop - pScroll_;
    base_ = D2D1::Matrix3x2F::Translation(0, oy_);
    rt_->SetTransform(base_);
    float by = 12;   // body coordinates: 0 = top of the scrolled content
    float gw = w;
    // ---- the running import (scrolls with the grid: at the minimum size the grid stays reachable)
    if (pj_ || (pjEnd_ && (pjEndFor_.empty() || pjEndFor_ == pBuild_))) { float jh = drawPortJob(x, by, gw); pJobR_ = rc(x, by + oy_, gw, jh); by += jh + 20; }
    else pJobR_ = {};
    if (pCat_ && !pCat_->error.empty()) {
        std::wstring html = L"**The cosmetics of the current Fortnite cannot be listed.** " + pCat_->error + L".";
        Font nf{ Face::UI, 12.5f, 400, false };
        float th = rich(html, nf, 0, 0, gw - 32 - 16 - 12, 18.75f, C::warnText, C::text, 0, false);
        float nh = 24 + std::max(th, 18.f);
        fill(rc(x, by, gw, nh), C::warn, 0.12f, 12);
        icon(Icon::Warn, x + 16, by + 14, 16, C::warnText);
        rich(html, nf, x + 16 + 16 + 12, by + 12, gw - 32 - 16 - 12, 18.75f, C::warnText, C::text);
        by += nh + 16;
        button(ID_P_RETRY, x, by, L"Try again", Icon::Refresh, true, false, false);
        by += 28;
    } else {
        // section head: title, count, Clear, Import selected, Import all
        std::wstring h2 = __builtin_popcount(pTypes_) == 1 ? kPT[__builtin_ctz(pTypes_)] : L"All cosmetics";
        for (auto& c : h2) c = (wchar_t)towupper(c);
        Font h2f{ Face::Display, 22, 900, true };
        float h2w = textW(h2, h2f, -0.11f);
        float secH = 37;
        text(h2, h2f, x, by, h2w + 2, secH, C::white, 0, -0.11f);
        float rx = x + gw;
        if (pTodo_) {
            std::wstring l = L"Import all " + thousands(pTodo_) + L"…";
            float aw = chip(ID_P_IMPORTALL, 0, 0, secH, l, -1, L"", true, pj_ != nullptr, true);
            rx -= aw; chip(ID_P_IMPORTALL, rx, by, secH, l, -1, L"", true, pj_ != nullptr); rx -= 10;
        }
        std::wstring n = pSelN_ ? thousands(pSelN_) : L"";
        float iw = chip(ID_P_IMPORTSEL, 0, 0, secH, L"Import selected", -1, n, false, !pSelN_ || pj_, true);
        rx -= iw; chip(ID_P_IMPORTSEL, rx, by, secH, L"Import selected", -1, n, false, !pSelN_ || pj_);
        if (pSelN_) {
            Font cf{ Face::UI, 12.5f, 500, false };
            float clw = textW(L"Clear", cf) + 4;
            rx -= 10 + clw;
            text(L"Clear", cf, rx + 2, by, clw, secH, hover_ == ID_P_CLEARSEL ? N::ink : N::mut);
            hit(ID_P_CLEARSEL, rc(rx, by + 6, clw, secH - 12), F_FOCUS | F_HAND, 4);
        }
        // the count, in what the buttons leave
        float cw2 = rx - 10 - (x + h2w + 10);
        if (pCat_ && cw2 >= 60) text(thousands((int)pList_.size()) + L" items", Font{ Face::UI, 13, 500, false, true }, x + h2w + 10, by + 2, cw2, secH, N::mut);
        by += secH + 30;
        if (pLoading_) { float lh = drawPortLoad(x, by, gw, true); pLoadR_ = rc(x, by + oy_, gw, lh); by += lh + 26; }
        else pLoadR_ = {};
        // the grid: gap 30 px 10 px, cards 198:258 (radarnite.com: 6 of 198 at 1280). Cards grow with the window
        // (190 DIPs at 1280 to 240 from ~2100) instead of making a dozen small columns; narrow windows keep 168.
        float minW = gw < 2 * 190 + 10 ? 168.f : std::max(190.f, std::min(240.f, 190 + (gw - 1232) * .06f));
        int cols = std::max(1, (int)floorf((gw + 10) / (minW + 10)));
        float cardW = (gw - (cols - 1) * 10) / cols, cardH = cardW * 258 / 198, pitch = cardH + 30;
        pGrid_ = rc(x, by + oy_, gw, 0);
        pSlot_.clear();
        if (!pCat_) {
            // skeleton cards while the first list loads (one shimmer for all of them)
            float p = 0;
            if (anim_.motion) {
                uint32_t key = K_SHIMMER | 10;
                if (!anim_.running(key)) anim_.to(key, -400, 400, 1400, Curve::Linear, {}, 0, 0);
                p = tw(key, 0, rc(x, by, gw, std::min(bodyH, 3 * pitch)));
            }
            for (int i = 0; i < 18; i++) {
                D2D1_RECT_F r = rc(x + (i % cols) * (cardW + 10), by + (i / cols) * pitch, cardW, cardH);
                if (r.top + oy_ > bodyTop + bodyH) break;
                if (shimmer_ && !measuring_) {
                    shimmer_->SetStartPoint(D2D1::Point2F(r.left + p, 0)); shimmer_->SetEndPoint(D2D1::Point2F(r.left + p + 800, 0));
                    shimmer_->SetOpacity(alpha_);
                    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 14, 14), shimmer_);
                }
            }
            by += ((18 + cols - 1) / cols) * pitch;
        } else if (pList_.empty()) {
            const wchar_t* t = pSts_ == 2u ? L"Nothing imported yet." : L"Nothing matches.";
            D2D1_RECT_F r = rc(x + 8, by + 8, gw - 16, 20 + 18.125f + 20);
            if (!measuring_) {
                ID2D1StrokeStyle* dash = nullptr;
                float dashes[] = { 3, 3 };
                g_.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0), dashes, 2, &dash);
                rt_->DrawRoundedRectangle(D2D1::RoundedRect({ r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f }, 11.5f, 11.5f), br(C::line), 1, dash);
                release(dash);
            }
            text(t, Font{ Face::UI, 12.5f, 400, false }, r.left, r.top + 20, r.right - r.left, 18.125f, C::muted, 1);
            by += r.bottom - r.top + 16;
        } else {
            int rows = ((int)pList_.size() + cols - 1) / cols;
            float gridTop = by;
            // visible rows plus one more each side (their images load just before they scroll in)
            int r0 = std::max(0, (int)floorf((pScroll_ - gridTop) / pitch) - 1);
            int r1 = std::min(rows - 1, (int)floorf((pScroll_ + bodyH - gridTop) / pitch) + 1);
            for (int r = r0; r <= r1; r++)
                for (int c = 0; c < cols; c++) {
                    int li = r * cols + c;
                    if (li >= (int)pList_.size()) break;
                    int slot = (int)pSlot_.size();
                    if (slot >= P_SLOTS) break;
                    pSlot_.push_back(li);
                    drawPortCard(slot, pList_[li], rc(x + c * (cardW + 10), gridTop + r * pitch, cardW, cardH));
                }
            // one Tab stop for the whole grid (arrows move inside it)
            D2D1_RECT_F gv = inter(rc(x, gridTop, gw, rows * pitch - 30), rc(x, pScroll_, gw, bodyH));
            if (gv.bottom > gv.top) hit(ID_P_GRID, gv, F_FOCUS | F_RING_IN, 14);
            if (!hits_.empty() && hits_.back().id == ID_P_GRID) {   // behind the cards: they win the mouse
                Hit g = hits_.back(); hits_.pop_back();
                size_t first = hits_.size();
                while (first > 0 && hits_[first - 1].id >= ID_P_CARD0 && hits_[first - 1].id < ID_P_END) first--;
                hits_.insert(hits_.begin() + first, g);
            }
            by = gridTop + rows * pitch - 30;
            pGrid_.bottom = pGrid_.top + rows * pitch;
        }
    }
    by += 24;
    base_ = savedBase; oy_ = savedOy;
    rt_->SetTransform(base_);
    rt_->PopAxisAlignedClip();
    clip_ = savedClip;
    if (fabsf(by - pTotal_) > 0.5f) { pTotal_ = by; invalidate(); }
    if (sb) drawScrollbar(ID_P_SB, x0 + W - 10, bodyTop, bodyH, pScroll_, pTotal_, bodyH);
    glassChips_ = false;
}

void App::drawPortCard(int slot, int idx, D2D1_RECT_F r) {
    const PortItem& it = pCat_->items[idx];
    int id = ID_P_CARD0 + slot, qid = ID_P_QB0 + slot;
    bool can = importable(it), on = idx < (int)pSel_.size() && pSel_[idx];
    bool hov = hover_ == id || hover_ == qid;
    bool cur = focusVisible_ && focus_ == ID_P_GRID && pSlot_[slot] == pCursor_;
    float cw = r.right - r.left, ch = r.bottom - r.top;
    float a0 = alpha_;
    if (it.status == PortStatus::Build) alpha_ *= .45f;
    // outline: transparent at 6 px -> white at 2 px (hover and selected), 150 ms var(--ease)
    float ring = fx(K_RING | id, (hov && can) || on ? 1.f : 0.f, grow(r, 9), 150, Curve::Ui);
    if (ring > 0 && !measuring_) {
        float o = 6 - 4 * ring + 1;
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(grow(r, o), 14 + o, 14 + o), br(C::white, ring), 2);
    }
    RarBrush* rb = rarBrush(it.rarity);
    if (rb && rb->bg && !measuring_) {
        rb->bg->SetStartPoint(D2D1::Point2F(0, r.top)); rb->bg->SetEndPoint(D2D1::Point2F(0, r.bottom)); rb->bg->SetOpacity(alpha_);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 14, 14), rb->bg);
    }
    // art: featured (not for loading screens, sprays, emoticons, wraps) rises 7 % above the card; icons fill it
    bool useF = !it.featured.empty() && it.type != PortType::LoadingScreen && it.type != PortType::Spray && it.type != PortType::Emoji && it.type != PortType::Wrap;
    const std::string& url = useF ? it.featured : it.icon;
    float zoom = 1 + .03f * fx(K_HOVER | id, hov ? 1.f : 0.f, grow(r, 20), 300);
    bool failed = false;
    UINT pw = (UINT)lroundf(cw * scale_), ph = (UINT)lroundf(ch * scale_);
    ID2D1Bitmap* bmp = url.empty() || measuring_ ? nullptr : img_.get(url, pw, ph, useF ? Images::Featured : Images::Cover, &failed);
    if (bmp) {
        D2D1_SIZE_U s = bmp->GetPixelSize();
        float bw = s.width / scale_, bh2 = s.height / scale_;
        D2D1_RECT_F d = useF ? D2D1_RECT_F{ (r.left + r.right) / 2 - bw / 2, r.bottom - bh2, (r.left + r.right) / 2 + bw / 2, r.bottom }
                             : D2D1_RECT_F{ (r.left + r.right) / 2 - bw / 2, r.top, (r.left + r.right) / 2 + bw / 2, r.top + bh2 };
        D2D1_MATRIX_3X2_F saved = base_;
        if (zoom != 1) { base_ = D2D1::Matrix3x2F::Scale(zoom, zoom, D2D1::Point2F((r.left + r.right) / 2, r.bottom)) * base_; rt_->SetTransform(base_); }
        if (useF) rt_->DrawBitmap(bmp, d, alpha_, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else {
            // .art.flat: clipped to the card's rounded corners
            ID2D1BitmapBrush* bb = nullptr;
            if (SUCCEEDED(rt_->CreateBitmapBrush(bmp, &bb))) {
                bb->SetTransform(D2D1::Matrix3x2F::Scale(1 / scale_, 1 / scale_) * D2D1::Matrix3x2F::Translation(d.left, d.top));
                bb->SetOpacity(alpha_);
                bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(inter(r, d), 14, 14), bb);
                bb->Release();
            }
        }
        if (zoom != 1) { base_ = saved; rt_->SetTransform(base_); }
    } else if (url.empty() || failed) {
        para(it.id, Font{ Face::Mono, 11, 500, false }, r.left + 12, r.top + ch / 2 - 22, cw - 24, 14.3f, 3, C::white, true);
    }
    // .fsh: the bottom 58 % darkens towards the name
    if (rb && rb->shade && !measuring_) {
        float st = r.bottom - ch * .58f;
        // one fill of the whole card: the brush is transparent above st (two overlapping fills drew a hard band there)
        rb->shade->SetStartPoint(D2D1::Point2F(0, r.bottom)); rb->shade->SetEndPoint(D2D1::Point2F(0, st)); rb->shade->SetOpacity(alpha_);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 14, 14), rb->shade);
    }
    // name (2 lines) and "Type · Series"
    // .name: 800 14px/1.15, upright (the display face only ships its 900 italic: Inter at 800)
    Font nf{ Face::UI, 14, 800, false }, mf{ Face::UI, 11.5f, 400, false };
    std::wstring sub = kPT1[(int)it.type];
    std::wstring ser = !it.series.empty() ? it.series : cap(it.rarity);
    if (!ser.empty()) sub += L" · " + ser;
    const std::wstring& name = it.name.empty() ? it.id : it.name;
    float nh = para(name, nf, 0, 0, cw - 20, 16.1f, 2, C::white, false);
    float my = r.bottom - 12 - 15.525f;
    para(name, nf, r.left + 10, my - 3 - nh, cw - 20, 16.1f, 2, C::white);
    text(sub, mf, r.left + 10, my, cw - 20, 15.525f, C::white, 0, 0, .8f);
    // badge (top left), radarnite.com's "In the shop": a black pill skewed -12deg, a status dot, italic 900 white
    if (it.status != PortStatus::New) {
        const wchar_t* bt = it.status == PortStatus::Imported ? L"IMPORTED" : it.status == PortStatus::Build ? L"IN THE BUILD" : L"FAILED";
        uint32_t bc = it.status == PortStatus::Imported ? 0x3DFF6E : it.status == PortStatus::Failed ? C::bad : C::muted;
        Font bf{ Face::Display, 10, 900, true };
        float bh = 17, bw = 8 + 6 + 6 + textW(bt, bf, .1f) + 8, bx = r.left + 10, byy = r.top + 9;
        D2D1_MATRIX_3X2_F saved = base_;
        base_ = D2D1::Matrix3x2F::Skew(-12, 0, D2D1::Point2F(bx + bw / 2, byy + bh / 2)) * base_;
        rt_->SetTransform(base_);
        fill(rc(bx, byy, bw, bh), 0x000000, .92f, 6);
        base_ = saved; rt_->SetTransform(base_);
        if (!measuring_) rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bx + 8 + 3, byy + bh / 2), 3, 3), br(bc, alpha_));
        text(bt, bf, bx + 8 + 6 + 6, byy, bw, bh, C::white, 0, .1f);
    }
    alpha_ = a0;
    hit(id, r, F_HAND, 14);
    // select button (top right): shown on hover, or when selected
    if (can) {
        D2D1_RECT_F q = rc(r.right - 6 - 32, r.top + 6, 32, 32);
        float op = fx(K_FADE | qid, hov || on || cur ? 1.f : 0.f, grow(q, 2), 130);
        bool qh = hover_ == qid;
        if (op > 0 && !measuring_) {
            float a1 = alpha_; alpha_ = op;
            if (on) { fill(q, N::accent, 1, 16); icon(Icon::PChk, q.left + 8.5f, q.top + 8.5f, 15, N::on); }
            else {
                fill(q, qh ? N::ink : 0x0C0D10, qh ? 1 : .72f, 16);
                if (!qh) rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(q.left + 16, q.top + 16), 15.5f, 15.5f), br(C::white, .14f), 1);
                icon(Icon::PChk, q.left + 8.5f, q.top + 8.5f, 15, qh ? N::bg : C::text);
            }
            alpha_ = a1;
        }
        hit(qid, q, F_HAND, 16);
    }
    if (cur && !measuring_) rt_->DrawRoundedRectangle(D2D1::RoundedRect(grow(r, 3), 17, 17), br(C::white), 2);
}

// "Names and icons" / "Reading the cosmetics": one row per source with its own estimated bar.
float App::drawPortLoad(float x, float y, float w, bool draw) {
    ULONGLONG now = GetTickCount64();
    Font tf{ Face::UI, 12.5f, 500, false }, sf{ Face::UI, 12.5f, 400, false }, vf{ Face::UI, 12, 400, false, true };
    float yy = y + 1 + 14;
    struct R { const wchar_t* stage; const wchar_t* label; const wchar_t* src; };
    const R rows[] = { { L"names", L"Names and icons", L"fortnite-api.com" }, { L"list", L"Reading the cosmetics of the installed Fortnite", L"fn-port, the first time takes minutes" } };
    float h0 = yy;
    bool any = false;
    for (auto& rw : rows) {
        const PortRow* s = nullptr;
        for (auto& p : pRows_) if (p.stage == rw.stage) s = &p;
        if (!s) continue;
        if (any) yy += 10;
        any = true;
        int64_t el = (int64_t)(now - s->at);
        float pct = s->done ? 100.f : std::min(95.f, s->ms ? 100.f * el / s->ms : 50.f);
        std::wstring v = s->done ? L"done" : fmtS(el) + (s->ms ? L" of ~" + fmtS(s->ms) : L"");
        if (draw) {
            if (s->done) icon(Icon::Check, x + 1 + 16, yy + 1, 16, C::ok); else spinner(x + 1 + 16 + 1, yy + 2, 14);
            float vw = textW(v, vf) + 2;
            float lw = textW(rw.label, tf);
            text(rw.label, tf, x + 1 + 16 + 28, yy, w - 2 - 32 - 28 - vw - 10, 18.125f, s->done ? C::text : C::muted);
            text(std::wstring(L" · ") + rw.src, sf, x + 1 + 16 + 28 + lw, yy, std::max(0.f, w - 2 - 32 - 28 - vw - 10 - lw), 18.125f, C::muted);
            text(v, vf, x + w - 1 - 16 - vw, yy, vw, 18.125f, C::muted);
            fill(rc(x + 1 + 16 + 28, yy + 18.125f + 10, w - 2 - 32 - 28, 4), C::line, 1, 2);
            fill(rc(x + 1 + 16 + 28, yy + 18.125f + 10, (w - 2 - 32 - 28) * pct / 100, 4), C::text, 1, 2);
        }
        yy += 18.125f + 10 + 4;
    }
    if (!any) {
        if (draw) {
            spinner(x + 1 + 16 + 1, yy + 2, 14);
            text(L"Looking for the installed Fortnite…", tf, x + 1 + 16 + 28, yy, w - 80, 18.125f, C::muted);
        }
        yy += 18.125f;
    }
    if (pCat_ && pCat_->partial) {
        yy += 10;
        if (draw) text(L"These come from fortnite-api.com: they can be picked as soon as the game has been read.", Font{ Face::UI, 12, 400, false }, x + 1 + 16, yy, w - 34, 17.4f, C::muted);
        yy += 17.4f;
    }
    float h = yy - h0 + 14 + 1 + 14 + 1;
    if (draw) {
        D2D1_RECT_F r = rc(x, y, w, h);
        // the box goes under what was just drawn: draw it first next time is not possible in one pass, so a border only
        if (!measuring_) {
            float lw = std::max(1.f, floorf(scale_)) / scale_;
            rt_->DrawRoundedRectangle(D2D1::RoundedRect({ r.left + lw / 2, r.top + lw / 2, r.right - lw / 2, r.bottom - lw / 2 }, 11.5f, 11.5f), br(C::line), lw);
        }
    }
    return h;
}

// The import card, in the lobby's style (glass on the season): a big title with the count and the times, a thick
// progress bar with a glow, the phases as pills (the current one pulses), the item being converted shown large, the
// last items as mini cards that wrap, the importer's output behind a toggle. Once it ends (pjEnd_): done, stopped or
// failed, with what to do next.
float App::drawPortJob(float x, float y, float w) {
    const bool end = !pj_;
    const PortJob& j = end ? *pjEnd_ : *pj_;
    int64_t dt = end ? 0 : (int64_t)(GetTickCount64() - pjAt_);
    auto itemOf = [&](const std::wstring& p) -> const PortItem* {
        if (!pCat_) return nullptr;
        if (pPath_.empty() && !pCat_->items.empty()) { pPath_.reserve(pCat_->items.size()); for (size_t i = 0; i < pCat_->items.size(); i++) if (!pCat_->items[i].path.empty()) pPath_[pCat_->items[i].path] = (int)i; }
        auto f = pPath_.find(p);
        return f == pPath_.end() ? nullptr : &pCat_->items[f->second];
    };
    auto nm = [&](const std::wstring& p) { const PortItem* it = itemOf(p); return it && !it->name.empty() ? it->name : tail(p); };
    // an item's icon on its rarity's gradient, rounded
    auto tile = [&](const PortItem* it, D2D1_RECT_F r, float rad) {
        if (measuring_) return;
        RarBrush* rb = it ? rarBrush(it->rarity) : nullptr;
        if (rb && rb->bg) {
            rb->bg->SetStartPoint(D2D1::Point2F(0, r.top)); rb->bg->SetEndPoint(D2D1::Point2F(0, r.bottom)); rb->bg->SetOpacity(alpha_);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), rb->bg);
        } else fill(r, C::white, .08f, rad);
        ID2D1Bitmap* ib = it && !it->icon.empty() ? img_.get(it->icon, (UINT)lroundf((r.right - r.left) * scale_), (UINT)lroundf((r.bottom - r.top) * scale_), Images::Cover) : nullptr;
        if (!ib) return;
        ID2D1BitmapBrush* bb = nullptr;
        if (SUCCEEDED(rt_->CreateBitmapBrush(ib, &bb))) {
            bb->SetTransform(D2D1::Matrix3x2F::Scale(1 / scale_, 1 / scale_) * D2D1::Matrix3x2F::Translation(r.left, r.top));
            bb->SetOpacity(alpha_);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), bb);
            bb->Release();
        }
    };
    const bool done = end && j.phase == L"done", failed = end && j.phase == L"failed", stopped = end && !done && !failed;
    const float pad = w < 640 ? 16.f : 22.f, ix = x + pad, iw = w - 2 * pad;
    const bool narrow = iw < 560;
    auto body = [&]() -> float {
        float yy = y + pad;
        // ---- head: title + count/times on the left; the percentage and Stop (or the end's buttons) on the right
        Font tf{ Face::Display, narrow ? 20.f : 26.f, 900, true }, pf{ Face::Display, narrow ? 20.f : 26.f, 900, true };
        Font sf{ Face::UI, 13, 400, false, true }, sb{ Face::UI, 13, 700, false, true };
        float th = narrow ? 22.f : 28.f;
        std::wstring title = done ? L"IMPORTED INTO " + j.build : failed ? L"IMPORT FAILED" : stopped ? L"IMPORT STOPPED" : L"IMPORTING INTO " + j.build;
        int pct = j.total ? (int)floorf(100.f * j.done / j.total) : 0;
        std::wstring pctS = fmt(L"%d%%", pct);
        // the right side
        float rw = 0;
        if (!end) rw = chip(ID_P_STOP, 0, 0, 36, L"Stop", (int)Icon::X, L"", false, false, true);
        else {
            rw = gbutton(ID_P_JOBX, 0, 0, 36, L"Dismiss", Icon::X, true, true);
            if (done || failed) rw += 8 + chip(ID_P_JOBGO, 0, 0, 36, done ? L"Launch " + j.build : L"Show Activity", done ? (int)Icon::Play : -1, L"", true, false, true);
        }
        float pw = end ? 0 : textW(pctS, pf, -.3f) + 6;
        float rx = ix + iw - rw;
        float lw = iw - rw - (pw ? pw + 18 : 0) - 16;
        float ty = yy;
        float tx = ix;
        if (done || failed) {   // a round mark before the title
            float d = th;
            if (!measuring_) {
                fill(rc(tx, ty, d, d), done ? GREEN_ : C::bad, .18f, d / 2);
                icon(done ? Icon::Check : Icon::X, tx + d / 2 - 8, ty + d / 2 - 8, 16, done ? GREEN_ : C::bad);
            }
            tx += d + 10; lw -= d + 10;
        } else if (!end) { spinner(tx, ty + (th - 18) / 2, 18); tx += 18 + 12; lw -= 18 + 12; }
        text(title, tf, tx, ty, std::max(0.f, lw), th, failed ? C::badText : C::white, 0, -.3f);
        if (!end) {
            // the percentage: big, beside Stop
            text(pctS, pf, rx - 18 - pw, ty, pw, th, C::white, 2, -.3f);
            // Stop: a secondary danger button
            D2D1_RECT_F sr = rc(rx, ty + (th - 36) / 2, rw, 36);
            bool hov = hover_ == ID_P_STOP;
            float hv = fx(K_HOVER | ID_P_STOP, hov ? 1.f : 0.f, sr, 150);
            box(sr, 10, C::bad, .12f + .14f * hv, C::bad, .45f + .3f * hv);
            Font bf{ Face::UI, 12.5f, 600, false };
            icon(Icon::X, sr.left + 12, sr.top + 10, 16, C::badText);
            text(L"Stop", bf, sr.left + 12 + 16 + 8, sr.top, rw - 36, 36, C::badText);
            hit(ID_P_STOP, sr, F_FOCUS | F_HAND, 10);
        } else {
            float bx = ix + iw;
            bx -= 36; gbutton(ID_P_JOBX, bx, ty + (th - 36) / 2, 36, L"Dismiss", Icon::X, true);
            if (done || failed) {
                std::wstring l = done ? L"Launch " + j.build : L"Show Activity";
                float cw = chip(ID_P_JOBGO, 0, 0, 36, l, done ? (int)Icon::Play : -1, L"", true, false, true);
                bx -= 8 + cw; chip(ID_P_JOBGO, bx, ty + (th - 36) / 2, 36, l, done ? (int)Icon::Play : -1, L"", true, false);
            }
        }
        yy += th + 6;
        // the count and the times
        int64_t el = j.elapsedMs + dt;
        int64_t eta = j.etaMs ? std::max<int64_t>(0, j.etaMs - dt) : (j.done > 0 && j.done < j.total ? el * (j.total - j.done) / j.done : 0);
        float cx = ix;
        auto part = [&](const std::wstring& s, const Font& f, uint32_t c) { float tw = textW(s, f); text(s, f, cx, yy, std::max(0.f, ix + iw - cx), 18, c); cx += tw; };
        if (end) {
            part(thousands(j.ok) + L" imported", sb, C::white);
            if (j.failed) part(L" · " + thousands(j.failed) + L" not converted", sf, C::badText);
            part(L" · " + fmtS(el), sf, C::muted);
        } else {
            part(thousands(j.done) + L" / " + thousands(j.total), sb, C::white);
            part(L" items", sf, C::muted);
            if (j.failed) part(L" · " + thousands(j.failed) + L" not converted", sf, C::badText);
            part(L" · " + fmtS(el) + L" elapsed", sf, C::muted);
            if (eta) part(L" · about " + fmtS(eta) + L" left", sf, C::white);
        }
        yy += 18 + 14;
        // ---- the bar: 10 high, rounded, a glow under the filled part
        {
            D2D1_RECT_F tr = rc(ix, yy, iw, 10);
            fill(tr, C::white, .10f, 5);
            float k = j.total ? std::min(1.f, (float)j.done / j.total) : 0;
            if (done) k = 1;
            uint32_t bc = failed ? C::bad : stopped ? C::muted : GREEN_;
            if (k > 0) {
                D2D1_RECT_F fr = rc(ix, yy, std::max(10.f, iw * k), 10);
                if (!stopped && !measuring_) shadow(fr, 5, 18, 0, .55f, bc);
                fill(fr, bc, 1, 5);
                fill(rc(fr.left + 4, fr.top + 2, fr.right - fr.left - 8, 2), C::white, .35f, 1);   // a highlight along the top
            }
            yy += 10 + 18;
        }
        // ---- the phases as pills (while it runs)
        if (!end) {
            struct Ph { const wchar_t* k; const wchar_t* l; };
            const Ph ph[] = { { L"wait", L"CPU check" }, { L"load", L"Reading the game" }, { L"convert", L"Converting" }, { L"install", L"Installing" }, { L"sign", L"Signing paks" } };
            std::wstring curPh = j.phase == L"cpu" ? L"wait" : j.phase == L"start" ? L"" : j.phase;
            int ci = j.phase == L"start" ? 0 : -1;
            for (int i = 0; i < 5; i++) if (curPh == ph[i].k) ci = i;
            Font pfnt{ Face::UI, 12, 600, false };
            float px = ix, py = yy, ph_ = 30;
            float pulse = .5f + .5f * sinf((float)(GetTickCount64() % 1600) / 1600.f * 6.2831853f);
            for (int i = 0; i < 5; i++) {
                float lw2 = textW(ph[i].l, pfnt);
                float pw2 = 12 + 14 + 7 + lw2 + 14;
                float gap = i ? 22.f : 0.f;
                if (px + gap + pw2 > ix + iw && px > ix) { px = ix; py += ph_ + 8; gap = 0; }   // wraps
                if (gap) { fill(rc(px + 4, py + ph_ / 2 - .5f, 14, 1), C::white, i <= ci ? .45f : .15f); px += gap; }
                D2D1_RECT_F r = rc(px, py, pw2, ph_);
                if (i < ci) {
                    box(r, ph_ / 2, GREEN_, .14f, GREEN_, .35f);
                    icon(Icon::Check, px + 12, py + 8, 14, GREEN_);
                    text(ph[i].l, pfnt, px + 12 + 14 + 7, py, lw2 + 1, ph_, C::white, 0, 0, .85f);
                } else if (i == ci) {
                    if (!measuring_) {
                        rt_->DrawRoundedRectangle(D2D1::RoundedRect(grow(r, 2.5f), ph_ / 2 + 2.5f, ph_ / 2 + 2.5f), br(C::white, .10f + .22f * pulse), 2);
                        spinRects_.push_back({ r.left - 5, r.top - 5 + oy_, r.right + 5, r.bottom + 5 + oy_ });   // keeps the pulse moving
                    }
                    box(r, ph_ / 2, C::white, .16f, C::white, .55f);
                    spinner(px + 12, py + 8, 14);
                    text(ph[i].l, pfnt, px + 12 + 14 + 7, py, lw2 + 1, ph_, C::white);
                } else {
                    box(r, ph_ / 2, 0, 0, C::white, .14f);
                    dot(px + 12 + 7, py + ph_ / 2, 6, C::white);
                    text(ph[i].l, pfnt, px + 12 + 14 + 7, py, lw2 + 1, ph_, C::white, 0, 0, .45f);
                }
                px += pw2;
            }
            yy = py + ph_ + 16;
        }
        // ---- now: the item being converted, large; else what the importer does
        if (!end) {
            const PortItem* it = j.current.empty() ? nullptr : itemOf(j.current);
            std::wstring w0, w1;
            std::wstring cpu = j.cpu >= 0 ? fmt(L" · now %d %%", j.cpu) : L"";
            if (j.phase == L"start") { w0 = L"Starting…"; w1 = L"Preparing the list of items"; }
            else if (j.phase == L"wait") { w0 = L"Checking the CPU before the next batch"; w1 = L"Imports pause while the PC is above 70 %" + cpu; }
            else if (j.phase == L"cpu") { w0 = L"Waiting for the CPU to drop below 70 %" + cpu; w1 = L"Waiting " + fmtS(j.phaseMs + dt) + L" · it checks again every 20 s; closing heavy apps lets it continue sooner (gives up after 20 min)"; }
            else if (j.phase == L"load") { w0 = fmt(L"Batch %d of %d: reading the game's files", j.batch, j.batches); w1 = L"fn-port loads the installed game before converting · " + fmtS(j.phaseMs + dt); }
            else if (j.phase == L"convert") { w0 = j.current.empty() ? L"Converting" : nm(j.current); w1 = L"Meshes, textures, animations and sounds into a " + j.build + L" pak · " + fmtS(j.currentMs + dt); }
            else if (j.phase == L"install") { w0 = L"Installing into " + j.build; w1 = L"Copying the new paks and registering the items in custom-cosmetics.json"; }
            else if (j.phase == L"sign") { w0 = L"Signing the new paks"; w1 = L"Fitting the pak signatures so " + j.build + L" accepts them · " + fmtS(j.phaseMs + dt); }
            else w0 = j.line;
            bool big = j.phase == L"convert" && it;
            float nh = big ? (narrow ? 84.f : 104.f) : 72.f;
            D2D1_RECT_F nr = rc(ix, yy, iw, nh);
            fill(nr, 0x000000, .28f, 14);
            float ax = ix + 12, ay = yy + 12, as = nh - 24;
            float tx2 = ax + as + 16, tw2 = ix + iw - 16 - tx2;
            if (big) {
                const RC& rcl = rcOf(it->rarity);
                if (!measuring_) shadow(rc(ax, ay, as, as), 12, 24, 6, .5f, rcl.light);
                tile(it, rc(ax, ay, as, as), 12);
                Font kf{ Face::UI, 10.5f, 700, false };
                text(L"CONVERTING · " + fmtS(j.currentMs + dt), kf, tx2, ay + 2, tw2, 14, C::white, 0, .8f, .6f);
                Font nf{ Face::Display, narrow ? 18.f : 24.f, 900, true };
                text(w0, nf, tx2, ay + 18, tw2, narrow ? 22.f : 28.f, C::white, 0, -.2f);
                float ry = ay + 18 + (narrow ? 22.f : 28.f) + 6;
                // rarity pill (skewed, its color) + type
                std::wstring rl = !it->series.empty() ? it->series : cap(it->rarity);
                for (auto& c : rl) c = (wchar_t)towupper(c);
                Font rf{ Face::Display, 11, 900, true };
                float rpw = textW(rl, rf, .1f) + 16;
                D2D1_MATRIX_3X2_F saved = base_;
                base_ = D2D1::Matrix3x2F::Skew(-8, 0, D2D1::Point2F(tx2 + rpw / 2, ry + 10)) * base_;
                rt_->SetTransform(base_);
                fill(rc(tx2, ry, rpw, 20), rcl.light, 1, 5);
                base_ = saved; rt_->SetTransform(base_);
                text(rl, rf, tx2 + 8, ry, rpw, 20, C::white, 0, .1f);
                Font mf{ Face::UI, 12.5f, 400, false };
                text(std::wstring(kPT1[(int)it->type]) + L" · meshes, textures, animations and sounds into a " + j.build + L" pak", mf,
                     tx2 + rpw + 10, ry, std::max(0.f, ix + iw - 16 - (tx2 + rpw + 10)), 20, C::white, 0, 0, .7f);
            } else {
                fill(rc(ax, ay, as, as), C::white, .06f, as / 2);
                spinner(ax + as / 2 - 12, ay + as / 2 - 12, 24);
                Font af{ Face::UI, 15, 700, false }, mf{ Face::UI, 12.5f, 400, false };
                text(w0, af, tx2, ay + as / 2 - 21, tw2, 21, C::white);
                text(w1, mf, tx2, ay + as / 2 + 1, tw2, 18, C::white, 0, 0, .65f);
            }
            yy += nh + 18;
        } else {
            // what next
            Font mf{ Face::UI, 13, 400, false };
            std::wstring m = done ? L"Launch " + j.build + L" to see them in the locker."
                           : failed ? (j.line.empty() ? L"Nothing could be imported. Activity has the reason." : j.line)
                           : L"What was imported before the stop stays in " + j.build + L".";
            text(m, mf, ix, yy, iw, 18, failed ? C::badText : C::white, 0, 0, failed ? 1.f : .75f);
            yy += 18 + 18;
        }
        // ---- the last items: mini cards that wrap (newest first), two rows at most
        std::vector<const PortJobItem*> last;
        for (int i = (int)j.items.size() - 1; i >= 0; i--) if (j.items[i].st != L"queued") last.push_back(&j.items[i]);
        if (!last.empty()) {
            Font kf{ Face::UI, 10.5f, 700, false };
            text(!end ? L"LATEST" : j.ok ? L"IMPORTED" : L"TRIED", kf, ix, yy, 200, 14, C::white, 0, .8f, .55f);
            yy += 14 + 10;
            const float tw0 = 210, tg = 8, tH = 56;
            int cols = std::max(1, (int)floorf((iw + tg) / (tw0 + tg)));
            float tw3 = (iw - (cols - 1) * tg) / cols;
            int n = std::min((int)last.size(), cols * 2);
            if (n > cols) n -= n % cols;   // full rows only (the oldest left out)
            Font nf{ Face::UI, 12.5f, 700, false }, stf{ Face::UI, 11.5f, 500, false };
            for (int i = 0; i < n; i++) {
                const PortJobItem& e = *last[i];
                const PortItem* t = itemOf(e.path);
                D2D1_RECT_F r = rc(ix + (i % cols) * (tw3 + tg), yy + (i / cols) * (tH + tg), tw3, tH);
                bool okS = e.st == L"ok", bad = e.st == L"failed", conv = e.st == L"converting";
                fill(r, C::white, conv ? .10f : .05f, 12);
                if (conv) box(r, 12, 0, 0, C::white, .35f);
                tile(t, rc(r.left + 6, r.top + 6, tH - 12, tH - 12), 9);
                float nx = r.left + 6 + tH - 12 + 10, nw = r.right - 10 - nx;
                text(nm(e.path), nf, nx, r.top + 9, nw, 18, C::white);
                float sy = r.top + 9 + 18 + 2;
                if (okS) { icon(Icon::Check, nx, sy + 1.5f, 13, GREEN_); text(t ? std::wstring(kPT1[(int)t->type]) : L"Imported", stf, nx + 18, sy, nw - 18, 16, C::white, 0, 0, .6f); }
                else if (bad) { icon(Icon::X, nx, sy + 1.5f, 13, C::bad); text(L"Not converted", stf, nx + 18, sy, nw - 18, 16, C::badText); }
                else if (conv) { spinner(nx, sy + 2, 12); text(L"Converting…", stf, nx + 18, sy, nw - 18, 16, C::white, 0, 0, .8f); }
            }
            yy += ((n + cols - 1) / cols) * (tH + tg) - tg + 18;
        }
        // ---- the importer's output, behind a quiet toggle
        {
            Font lf{ Face::UI, 12, 500, false };
            std::wstring sum = std::wstring(pLogOpen_ ? L"Hide the importer's output" : L"Show the importer's output");
            float sumW = textW(sum, lf) + 2 + 16;
            D2D1_RECT_F tr = rc(ix, yy, sumW, 18);
            bool hov = hover_ == ID_P_LOG;
            if (!measuring_) {   // a chevron: right while closed, down while open
                ID2D1SolidColorBrush* cb = br(C::white, hov ? .9f : .5f);
                float cx = ix + 6, cy = yy + 9;
                if (pLogOpen_) { rt_->DrawLine(D2D1::Point2F(cx - 3.5f, cy - 1.75f), D2D1::Point2F(cx, cy + 1.75f), cb, 1.5f); rt_->DrawLine(D2D1::Point2F(cx, cy + 1.75f), D2D1::Point2F(cx + 3.5f, cy - 1.75f), cb, 1.5f); }
                else { rt_->DrawLine(D2D1::Point2F(cx - 1.75f, cy - 3.5f), D2D1::Point2F(cx + 1.75f, cy), cb, 1.5f); rt_->DrawLine(D2D1::Point2F(cx + 1.75f, cy), D2D1::Point2F(cx - 1.75f, cy + 3.5f), cb, 1.5f); }
            }
            text(sum, lf, ix + 16, yy, sumW - 16, 18, C::white, 0, 0, hov ? .9f : .5f);
            hit(ID_P_LOG, tr, F_FOCUS | F_HAND, 4);
            if (!end && j.batches > 1) {
                std::wstring bt = fmt(L"batch %d of %d", j.batch, j.batches) + (j.cpu >= 0 ? fmt(L" · CPU %d %%", j.cpu) : L"");
                float bw2 = textW(bt, lf) + 1;
                text(bt, lf, ix + iw - bw2, yy, bw2, 18, C::white, 0, 0, .45f);
            }
            yy += 18;
            if (pLogOpen_) {
                int recent = std::min<int>((int)j.recent.size(), 9);
                yy += 8;
                fill(rc(ix, yy, iw, 16 + recent * 17.25f), 0x000000, .35f, 10);
                Font mono{ Face::Mono, 11.5f, 400, false };
                float ly = yy + 8;
                for (int i = (int)j.recent.size() - recent; i < (int)j.recent.size(); i++) {
                    text(j.recent[i].first + L"  " + j.recent[i].second, mono, ix + 12, ly, iw - 24, 17.25f, C::ink2);
                    ly += 17.25f;
                }
                yy += 16 + recent * 17.25f;
            }
        }
        return yy + pad - y;
    };
    bool m0 = measuring_;
    measuring_ = true;
    float h = body();
    measuring_ = m0;
    D2D1_RECT_F card = rc(x, y, w, h);
    if (!measuring_) shadow(card, 18, 40, 14, .45f);
    glass(card, 18, .74f, .14f);
    body();
    return h;
}

// ================================================================================================ overlays
void App::drawPortOverlays() {
    if ((!pOpen_ || pPv_ < 0) && !video_.url().empty()) video_.stop();
    if (!pOpen_) return;
    if (pPop_) drawPortPop();
    if (pPv_ >= 0) drawPortPreview();
    if (pDlg_) drawPortDialog();
}

void App::drawPortPop() {
    layer_ = 2;
    hit(ID_P_SCRIM, { 0, TB, W_, H_ }, 0, 0);   // a click outside closes it
    Font cf{ Face::UI, 12.5f, 500, false }, nf{ Face::UI, 11.5f, 500, false, true }, hf{ Face::UI, 10.5f, 600, false };
    auto h4 = [&](const wchar_t* t, float x, float y) { text(t, hf, x, y, 300, 14, N::mut, 0, .735f); };
    if (pPop_ == 1) {
        // Category: All + the nine types, two columns
        float w = 336, colW = (w - 2 - 24 - 5) / 2, h = 1 + 12 + 14 + 8 + 5 * 35 + 4 * 5 + 12 + 1;
        float x = std::min(std::max(pBody_.left + 8, pPopAnchor_.left), pBody_.right - w - 8), y = pPopAnchor_.bottom + 8;
        D2D1_RECT_F r = rc(x, y, w, h);
        overlays_.push_back(r);
        shadow({ r.left + 16, r.top + 16, r.right - 16, r.bottom - 16 }, 12, 48, 18, .75f);
        box(r, 12, N::panel, 1, N::line);
        hit(ID_P_POP, r, 0, 12);
        h4(L"CATEGORY", x + 1 + 12 + 2, y + 1 + 12);
        float gy = y + 1 + 12 + 14 + 8;
        int total = 0; for (int i = 0; i < NT; i++) total += pCntT_[i];
        for (int i = 0; i <= NT; i++) {
            int id = ID_P_CHIP0 + i;
            bool on = i == 0 ? !pTypes_ : pTypes_ == (1u << (i - 1));
            int n = i == 0 ? total : pCntT_[i - 1];
            D2D1_RECT_F cr = rc(x + 1 + 12 + (i % 2) * (colW + 5), gy + (i / 2) * 40, colW, 35);
            bool hov = hover_ == id || (focusVisible_ && focus_ == id);
            fill(cr, on ? N::ink : hov ? N::hov : N::surf, 1, 8);
            uint32_t tc = on ? N::bg : n ? N::ink : N::dim;
            icon(i == 0 ? Icon::PAll : kPIC[i - 1], cr.left + 10, cr.top + 10, 15, on ? N::bg : N::mut);
            std::wstring ns = thousands(n);
            float nw = textW(ns, nf) + 2;
            text(i == 0 ? L"All" : kPT[i - 1], cf, cr.left + 10 + 15 + 8, cr.top, colW - 43 - nw - 8, 35, tc);
            text(ns, nf, cr.right - 10 - nw, cr.top, nw, 35, on ? N::oncount : N::mut);
            hit(id, cr, F_FOCUS | F_HAND, 8);
        }
    } else if (pPop_ == 2) {
        // Filters: Status, Type, Rarity (four columns), Clear all, Done
        float w = std::min(584.f, W_ - 32);
        float x = std::min(std::max(pBody_.left + 8, pPopAnchor_.left), pBody_.right - w - 8), y = pPopAnchor_.bottom + 8;
        int rars = 0; for (int i = 0; i < NR; i++) if (pCat_) { bool any = false; for (size_t k = 0; k < pRi_.size() && !any; k++) any = pRi_[k] == i; if (any) rars |= 1 << i; }
        int nr = __builtin_popcount(rars);
        float gw = w - 2 - 32, colW = (gw - 3 * 8) / 4;
        auto rowsH = [&](int n) { int r = (n + 3) / 4; return r * 33 + (r - 1) * 8.f; };
        float h = 1 + 12 + 17 + 12 + 1 + 14 + (14 + 7 + rowsH(4)) + 14 + (14 + 7 + rowsH(NT)) + (nr ? 14 + 14 + 7 + rowsH(nr) : 0) + 6 + 10 + 32 + 12 + 1;
        h = std::min(h, H_ - y - 16);
        D2D1_RECT_F r = rc(x, y, w, h);
        overlays_.push_back(r);
        shadow({ r.left + 16, r.top + 16, r.right - 16, r.bottom - 16 }, 12, 48, 18, .75f);
        box(r, 12, N::panel, 1, N::line);
        hit(ID_P_POP, r, 0, 12);
        int nFil = __builtin_popcount(pTypes_) + __builtin_popcount(pSts_) + __builtin_popcount(pRars_);
        Font hf2{ Face::UI, 13, 600, false };
        text(L"Filters", hf2, x + 1 + 16, y + 1 + 12, 100, 17, N::ink);
        if (nFil) text(fmt(L"%d on", nFil), Font{ Face::UI, 12, 500, false }, x + 1 + 16 + textW(L"Filters", hf2) + 8, y + 1 + 12, 100, 17, N::mut);
        float caw = textW(L"Clear all", cf) + 4;
        text(L"Clear all", cf, x + w - 1 - 16 - caw, y + 1 + 12, caw, 17, hover_ == ID_P_CLEARFILTERS ? N::ink : N::mut);
        hit(ID_P_CLEARFILTERS, rc(x + w - 1 - 16 - caw, y + 1 + 8, caw, 25), F_FOCUS | F_HAND, 4);
        float yy = y + 1 + 12 + 17 + 12;
        hline(x + 1, x + w - 1, yy, N::line);
        yy += 1 + 14;
        float gx = x + 1 + 16;
        auto grid = [&](const wchar_t* title, int n, auto&& label) {
            h4(title, gx, yy); yy += 14 + 7;
            for (int i = 0; i < n; i++) {
                int id; std::wstring l; int cnt; bool on; int rar;
                label(i, &id, &l, &cnt, &on, &rar);
                D2D1_RECT_F cr = rc(gx + (i % 4) * (colW + 8), yy + (i / 4) * 41, colW, 33);
                bool hov = hover_ == id || (focusVisible_ && focus_ == id);
                std::wstring cs = thousands(cnt);
                if (rar >= 0) {
                    // rarity chips: their gradient, white italic upper case, skewed -6 deg; on = a double ring
                    D2D1_MATRIX_3X2_F saved = base_;
                    base_ = D2D1::Matrix3x2F::Skew(-6, 0, D2D1::Point2F((cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2)) * base_;
                    rt_->SetTransform(base_);
                    if (on && !measuring_) {
                        rt_->DrawRoundedRectangle(D2D1::RoundedRect(grow(cr, 3), 11, 11), br(N::ink), 2);
                    }
                    if (!measuring_) {
                        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
                        const D2D1_GRADIENT_STOP st[] = { { 0, rgb(kRAR[rar].c1) }, { 1, rgb(kRAR[rar].c2) } };
                        if (SUCCEEDED(rt_->CreateGradientStopCollection(st, 2, &gs)) &&
                            SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(cr.left, cr.bottom), D2D1::Point2F(cr.right, cr.top)), gs, &lb))) {
                            lb->SetOpacity(cnt || on ? 1.f : .5f);
                            rt_->FillRoundedRectangle(D2D1::RoundedRect(cr, 8, 8), lb);
                        }
                        release(lb); release(gs);
                    }
                    Font rf{ Face::Display, 11.5f, 900, true };
                    float tw2 = textW(l, rf, .115f), nw = textW(cs, nf);
                    float sx = (cr.left + cr.right) / 2 - (tw2 + 6 + nw) / 2;
                    text(l, rf, sx, cr.top, tw2 + 1, 33, C::white, 0, .115f);
                    text(cs, Font{ Face::UI, 11, 500, false, true }, sx + tw2 + 6, cr.top, nw + 2, 33, C::white, 0, 0, on ? .85f : .7f);
                    base_ = saved; rt_->SetTransform(base_);
                } else {
                    box(cr, 8, on ? N::ink : hov ? N::surf2 : N::surf, 1, on ? N::ink : hov ? N::l25b : N::line);
                    // a long label with a big count ("Not imported 10,250") gets a smaller label rather than an ellipsis
                    Font lf = cf, nf2{ Face::UI, 11, 500, false, true };
                    float gap = 6, tw2 = textW(l, lf), nw = textW(cs, nf2);
                    if (tw2 + gap + nw > colW - 12) { lf.size = 11.5f; gap = 4; nf2.size = 10.5f; tw2 = textW(l, lf); nw = textW(cs, nf2); }
                    float sx = std::max(cr.left + 6, (cr.left + cr.right) / 2 - (tw2 + gap + nw) / 2);
                    text(l, lf, sx, cr.top, std::min(tw2 + 1, colW - 12 - nw - gap), 33, on ? N::on : cnt ? N::ink : N::dim);
                    text(cs, nf2, sx + std::min(tw2, colW - 12 - nw - gap) + gap, cr.top, nw + 2, 33, on ? N::oncount : N::mut);
                }
                hit(id, cr, F_FOCUS | F_HAND, 8);
            }
            yy += rowsH(n) + 14;
        };
        D2D1_RECT_F saved = clip_;
        clip_ = inter(clip_, { x, yy - 1, x + w, y + h - 1 - 12 - 32 - 10 });
        rt_->PushAxisAlignedClip(clip_, D2D1_ANTIALIAS_MODE_ALIASED);
        grid(L"STATUS", 4, [&](int i, int* id, std::wstring* l, int* c, bool* on, int* rar) { *id = ID_P_CHIP0 + 20 + i; *l = kSTS[i]; *c = pCntS_[i]; *on = (pSts_ >> i) & 1; *rar = -1; });
        grid(L"TYPE", NT, [&](int i, int* id, std::wstring* l, int* c, bool* on, int* rar) { *id = ID_P_CHIP0 + 40 + i; *l = kPT[i]; *c = pCntT_[i]; *on = (pTypes_ >> i) & 1; *rar = -1; });
        if (nr) {
            std::vector<int> ks; for (int i = 0; i < NR; i++) if (rars & (1 << i)) ks.push_back(i);
            grid(L"RARITY", nr, [&](int i, int* id, std::wstring* l, int* c, bool* on, int* rar) { int k = ks[i]; *id = ID_P_CHIP0 + 60 + k; *l = kRAR[k].label; for (auto& ch : *l) ch = (wchar_t)towupper(ch); *c = pCntR_[k]; *on = (pRars_ >> k) & 1; *rar = k; });
        }
        rt_->PopAxisAlignedClip();
        clip_ = saved;
        // Done
        float dw = chip(ID_P_DONE, 0, 0, 32, L"Done", -1, L"", true, false, true);
        chip(ID_P_DONE, x + w - 1 - 16 - dw, y + h - 1 - 12 - 32, 32, L"Done", -1, L"", true, false);
    } else if (pPop_ == 3) {
        float w = 250, h = 1 + 6 + 4 * 34 + 6 + 1;
        float x = std::max(pBody_.left + 8, pPopAnchor_.right - w), y = pPopAnchor_.bottom + 8;
        D2D1_RECT_F r = rc(x, y, w, h);
        overlays_.push_back(r);
        shadow({ r.left + 16, r.top + 16, r.right - 16, r.bottom - 16 }, 12, 48, 18, .75f);
        box(r, 12, C::bg2, 1, C::line);
        hit(ID_P_POP, r, 0, 12);
        for (int i = 0; i < 4; i++) {
            int id = ID_P_CHIP0 + 100 + i;
            D2D1_RECT_F o = rc(x + 1 + 6, y + 1 + 6 + i * 34, w - 14, 34);
            bool hov = hover_ == id || (focusVisible_ && focus_ == id);
            if (hov) fill(o, C::surface, 1, 7);
            if (pSort_ == i) icon(Icon::PChk, o.left + 10, o.top + 10, 14, C::text);
            text(kSort[i], Font{ Face::UI, 13, 500, false }, o.left + 10 + 14 + 8, o.top, w - 60, 34, pSort_ == i || hov ? C::text : C::muted);
            hit(id, o, F_FOCUS | F_HAND, 7);
        }
    }
    layer_ = 0;
}

void App::drawPortDialog() {
    layer_ = 4;
    D2D1_RECT_F all = { 0, TB, W_, H_ };
    fill(all, 0x000000, .55f);
    overlays_.push_back(all);
    hit(ID_P_SCRIM, all, 0, 0);
    const Build* b = sel();
    int n = (int)pPending_.size();
    double mb = 0;
    for (int i : pPending_) mb += kPMB[(int)pCat_->items[i].type];
    double gb = mb / 1024, hours = n * kPSEC / 3600.0;
    bool over = n > (pCat_ ? pCat_->slotsFree : 0);
    float w = std::min(480.f, W_ - 32), pad = 20, iw = w - 2 - 2 * pad;
    std::wstring title = L"Import " + thousands(n) + (n == 1 ? L" cosmetic" : L" cosmetics") + L" into " + (b ? b->version : L"") + L"?";
    std::wstring p = std::wstring(pDlgAll_ ? L"Every cosmetic shown that is not imported yet." : L"The cosmetics you picked.") +
                     L" They are converted from the Fortnite installed on this PC and added as new items. It runs in the background at low priority; you can stop it and import again later to continue.";
    Font tf{ Face::UI, 15, 600, false }, pf{ Face::UI, 12.5f, 400, false };
    float th = para(title, tf, 0, 0, iw, 21.75f, 0, C::text, false);
    float ph = para(p, pf, 0, 0, iw, 18.125f, 0, C::ink2, false);
    std::wstring warn = L"**Only " + thousands(pCat_ ? pCat_->slotsFree : 0) + L" more items fit in this build** (one pak slot each). The import stops when they run out.";
    float wh = over ? 12 + rich(warn, pf, 0, 0, iw - 24, 18.125f, C::muted, C::ink2, 0, false) : 0;
    float numsH = 2 + 12 + 24.65f + 16.675f + 12;
    float h = 1 + pad + th + 8 + ph + 12 + numsH + 12 + wh + 16 + 34 + pad + 1;
    float x = (W_ - w) / 2, y = TB + std::max(16.f, (H_ - TB - h) / 2);
    D2D1_RECT_F r = rc(x, y, w, h);
    shadow(r, 12, 50, 20, .6f);
    box(r, 12, C::surface, 1, C::line);
    hit(ID_P_DLG, r, 0, 12);
    float ix = x + 1 + pad, yy = y + 1 + pad;
    para(title, tf, ix, yy, iw, 21.75f, 0, C::text); yy += th + 8;
    para(p, pf, ix, yy, iw, 18.125f, 0, C::ink2); yy += ph + 12;
    D2D1_RECT_F nr = rc(ix, yy, iw, numsH);
    box(nr, 8, C::bg, 1, C::line2);
    std::wstring nums[3] = { thousands(n), L"~" + fmtGB(gb), L"~" + fmtH(hours) };
    const wchar_t* labs[3] = { L"items", L"disk (estimate)", L"time (estimate)" };
    float cwid = (iw - 2) / 3;
    for (int i = 0; i < 3; i++) {
        float cx = ix + 1 + i * cwid;
        if (i) vline(cx, yy + 1, yy + numsH - 1, C::line2);
        text(nums[i], Font{ Face::UI, 17, 600, false, true }, cx + 12, yy + 1 + 12, cwid - 24, 24.65f, C::text);
        text(labs[i], Font{ Face::UI, 11.5f, 400, false }, cx + 12, yy + 1 + 12 + 24.65f, cwid - 24, 16.675f, C::muted);
    }
    yy += numsH + 12;
    if (over) {
        icon(Icon::Warn, ix, yy + 13, 16, C::warn);
        rich(warn, pf, ix + 24, yy + 12, iw - 24, 18.125f, C::muted, C::ink2);
        yy += wh;
    }
    yy += 16;
    // Cancel (ghost), Import (primary: white)
    float iwb = 12 + 2 + textW(L"Import", Font{ Face::UI, 13, 600, false }) + 12;
    float bxI = x + w - 1 - pad - iwb;
    D2D1_RECT_F ir = rc(bxI, yy, iwb, 34);
    bool ih = hover_ == ID_P_DLG_IMPORT;
    box(ir, 8, ih ? 0xE4E4E7 : C::white, 1, ih ? 0xE4E4E7 : C::white);
    text(L"Import", Font{ Face::UI, 13, 600, false }, ir.left, yy, iwb, 34, 0x101114, 1);
    float cwb = button(ID_P_DLG_CANCEL, 0, 0, L"Cancel", Icon::X, false, true, false, false, true);
    button(ID_P_DLG_CANCEL, bxI - 8 - cwb, yy, L"Cancel", Icon::X, false, true, false, false);
    hit(ID_P_DLG_IMPORT, ir, F_FOCUS | F_HAND, 8);
    layer_ = 0;
}

// The preview on click (radarnite.com's item card): media left, the item's details right.
void App::drawPortPreview() {
    if (!pCat_ || pPv_ < 0 || pPv_ >= (int)pCat_->items.size()) return;
    layer_ = 3;
    const PortItem& it = pCat_->items[pPv_];
    const Build* b = sel();
    D2D1_RECT_F all = { 0, TB, W_, H_ };
    fill(all, 0x000000, .55f);
    overlays_.push_back(all);
    hit(ID_P_SCRIM, all, 0, 0);
    float w = std::min(920.f, W_ - 48), h = std::min(560.f, H_ - TB - 48);
    float x = (W_ - w) / 2, y = TB + (H_ - TB - h) / 2;
    D2D1_RECT_F r = rc(x, y, w, h);
    shadow(r, 16, 50, 20, .6f);
    box(r, 16, N::panel, 1, C::line);
    hit(ID_P_PV, r, 0, 16);
    // media: the rarity's gradient, the featured image (else the icon) fitted inside
    float mw = std::min(w * .52f, h);
    D2D1_RECT_F m = rc(x + 1, y + 1, mw, h - 2);
    RarBrush* rb = rarBrush(it.rarity);
    if (rb && rb->bg && !measuring_) {
        rb->bg->SetStartPoint(D2D1::Point2F(0, m.top)); rb->bg->SetEndPoint(D2D1::Point2F(0, m.bottom)); rb->bg->SetOpacity(1);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(m, 15, 15);
        rt_->FillRoundedRectangle(rr, rb->bg);
        rt_->FillRectangle({ m.right - 16, m.top, m.right, m.bottom }, rb->bg);
    }
    const std::string& url = !it.featured.empty() ? it.featured : it.icon;
    float pad = 24;
    UINT pw = (UINT)lroundf((mw - 2 * pad) * scale_), ph = (UINT)lroundf((h - 2 - 2 * pad) * scale_);
    bool failed = false;
    ID2D1Bitmap* bmp = url.empty() || measuring_ ? nullptr : img_.get(url, pw, ph, Images::Contain, &failed);
    if (bmp) {
        D2D1_SIZE_U s = bmp->GetPixelSize();
        float bw = s.width / scale_, bh = s.height / scale_;
        float cx = (m.left + m.right) / 2, cy = (m.top + m.bottom) / 2;
        rt_->DrawBitmap(bmp, { cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2 }, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else if (url.empty() || failed) text(it.id, Font{ Face::Mono, 12, 500, false }, m.left, m.top, mw, h, C::white, 1, 0, .55f);
    else spinner((m.left + m.right) / 2 - 9, (m.top + m.bottom) / 2 - 9, 18);
    hit(ID_P_PV_MEDIA, m, 0, 15);
    // the clip over it (a child window: live only), once the details say there is one; hidden under the dialog
    if (hwnd_ && !pPvWait_ && !pPvData_.video.empty() && !measuring_) {
        RECT px = { lroundf(m.left * scale_), lroundf(m.top * scale_), lroundf(m.right * scale_), lroundf(m.bottom * scale_) };
        if (video_.url() != pPvData_.video) video_.play(hwnd_, pPvData_.video, px, lroundf(15 * scale_), rcOf(it.rarity).dark);
        video_.place(px, !pDlg_);
    }
    // close
    button(ID_P_PV_CLOSE, x + w - 1 - 12 - 28, y + 1 + 12, nullptr, Icon::X, true, true, true);
    // details
    float ix = m.right + 28, iw = x + w - 1 - 28 - ix, yy = y + 1 + 28;
    // rarity badge (skewed) + TYPE
    std::wstring lab = !it.series.empty() ? it.series : cap(it.rarity);
    for (auto& c : lab) c = (wchar_t)towupper(c);
    Font bf{ Face::Display, 13, 900, true }, tf{ Face::Display, 13.5f, 900, true };
    float bx = ix;
    if (!lab.empty()) {
        float bw = textW(lab, bf, .13f) + 18;
        int ri = rarIndex(it.rarity);
        uint32_t c1 = ri >= 0 ? kRAR[ri].c1 : 0x3A3D45, c2 = ri >= 0 ? kRAR[ri].c2 : 0x3A3D45;
        D2D1_RECT_F br2 = rc(bx, yy, bw, 24);
        D2D1_MATRIX_3X2_F saved = base_;
        base_ = D2D1::Matrix3x2F::Skew(-6, 0, D2D1::Point2F(bx + bw / 2, yy + 12)) * base_;
        rt_->SetTransform(base_);
        if (!measuring_) {
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            const D2D1_GRADIENT_STOP st[] = { { 0, rgb(c1) }, { 1, rgb(c2) } };
            if (SUCCEEDED(rt_->CreateGradientStopCollection(st, 2, &gs)) &&
                SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(br2.left, br2.bottom), D2D1::Point2F(br2.right, br2.top)), gs, &lb)))
                rt_->FillRoundedRectangle(D2D1::RoundedRect(br2, 6, 6), lb);
            release(lb); release(gs);
        }
        base_ = saved; rt_->SetTransform(base_);
        text(lab, bf, bx + 9, yy, bw, 24, C::white, 0, .13f);
        bx += bw + 8;
    }
    std::wstring ty = kPT1[(int)it.type];
    for (auto& c : ty) c = (wchar_t)towupper(c);
    text(ty, tf, bx, yy, ix + iw - bx, 24, N::mut, 0, .27f);
    yy += 24 + 8;
    // name: bold italic
    Font nf{ Face::Display, 32, 900, true };
    std::wstring name = it.name.empty() ? it.id : it.name;
    for (auto& c : name) c = (wchar_t)towupper(c);
    yy += para(name, nf, ix, yy, iw, 33, 2, C::white);
    // description, introduced in, set, id
    Font df{ Face::UI, 14.5f, 400, false }, inf{ Face::UI, 14, 400, false }, xf{ Face::UI, 12.5f, 400, false };
    if (pPvWait_) {
        yy += 10;
        for (int i = 0; i < 2; i++) { fill(rc(ix, yy, iw * (i ? .6f : .9f), 14), C::surface, 1, 4); yy += 22; }
    } else {
        if (!pPvData_.description.empty()) { yy += 8; yy += para(pPvData_.description, df, ix, yy, iw, 20.3f, 4, N::ink); }
        std::wstring when = introduced(pPvData_.introduced);
        if (!when.empty()) { yy += 6; yy += rich(L"Introduced in **" + when + L"**.", inf, ix, yy, iw, 19.6f, 0xD4D4D8, N::ink); }
    }
    if (!it.set.empty()) { yy += 6; yy += rich(L"Set **" + it.set + L"**", xf, ix, yy, iw, 17.5f, N::mut, 0xD4D4D8); }
    yy += 2;
    text(it.id, xf, ix, yy, iw, 17.5f, N::mut);
    yy += 17.5f;
    // status in this build
    yy += 16;
    std::wstring ver = b ? b->version : L"";
    std::wstring st; uint32_t sc;
    if (it.path.empty()) { st = L"Not importable yet: the game is still being read"; sc = C::dim; }
    else switch (it.status) {
    case PortStatus::New: st = L"Not imported into " + ver; sc = C::dim; break;
    case PortStatus::Imported: st = L"Imported into " + ver; sc = C::ok; break;
    case PortStatus::Build: st = L"Already in " + ver + L" (its own item)"; sc = C::muted; break;
    default: st = L"Not converted last time: importing tries again"; sc = C::bad; break;
    }
    dot(ix + 4, yy + 9, 8, sc);
    text(st, Font{ Face::UI, 13, 500, false }, ix + 16, yy, iw - 16, 18.85f, C::ink2);
    // buttons (bottom): Select, Import, fortnite.gg
    float by = y + h - 1 - 28 - 37;
    float bx2 = ix;
    if (importable(it)) {
        bool on = pPv_ < (int)pSel_.size() && pSel_[pPv_];
        Font f{ Face::UI, 13, 600, false };
        float iwb = 16 + textW(L"Import", f) + 16;
        D2D1_RECT_F ir = rc(bx2, by, iwb, 37);
        bool ih = hover_ == ID_P_PV_IMPORT;
        bool dis = pj_ != nullptr;
        float a0 = alpha_; if (dis) alpha_ *= .4f;
        box(ir, 9, ih && !dis ? 0xE4E4E7 : C::white, 1, ih && !dis ? 0xE4E4E7 : C::white);
        text(L"Import", f, ir.left, by, iwb, 37, 0x101114, 1);
        alpha_ = a0;
        hit(ID_P_PV_IMPORT, ir, F_FOCUS | F_HAND | (dis ? F_DISABLED : 0), 9);
        bx2 += iwb + 8;
        bx2 += chip(ID_P_PV_SELECT, bx2, by, 37, on ? L"Selected" : L"Select", (int)Icon::PChk, L"", on, false) + 8;
    }
    if (!pPvData_.page.empty()) {
        Font lf{ Face::UI, 13, 500, false };
        std::wstring l = L"fortnite.gg ↗";
        float lw = textW(l, lf) + 4;
        float lx = x + w - 1 - 28 - lw;
        text(l, lf, lx + 2, by, lw, 37, hover_ == ID_P_PV_LINK ? C::text : C::muted);
        hit(ID_P_PV_LINK, rc(lx, by + 6, lw, 25), F_FOCUS | F_HAND, 4);
    }
    layer_ = 0;
}

// ================================================================================================ input
std::wstring App::portTip(int id) const {
    switch (id) {
    case ID_P_OPEN: return pOpen_ ? L"Close the cosmetics" : L"Import cosmetics of the current Fortnite";
    case ID_P_JOBX: return L"Dismiss";
    case ID_P_CELL: return L"Read from the build's own data. Import more from the current Fortnite";
    case ID_P_REFRESH: return L"List again from the installed Fortnite";
    case ID_P_CLOSE: return L"Close (Esc)";
    case ID_P_NEW: return L"Only what is not imported yet";
    case ID_P_PV_CLOSE: return L"Close (Esc)";
    case ID_P_PV_LINK: return std::wstring(pPvData_.page.begin(), pPvData_.page.end());
    }
    if (id >= ID_P_QB0 && id < ID_P_QB0 + P_SLOTS) return L"Select";
    if (id >= ID_P_CARD0 && id < ID_P_CARD0 + P_SLOTS && pCat_) {
        int s = id - ID_P_CARD0;
        if (s < (int)pSlot_.size()) { const PortItem& it = pCat_->items[pList_[pSlot_[s]]]; return (it.name.empty() ? it.id : it.name) + L"\n" + it.id + (it.set.empty() ? L"" : L"\n" + it.set); }
    }
    return L"";
}

void App::portConfirm(const std::vector<int>& items, bool all) {
    if (items.empty() || pj_) return;
    pPending_ = items;
    pDlgAll_ = all;
    pDlg_ = true;
    pPop_ = 0;
    focus_ = ID_P_DLG_IMPORT;
    invalidate();
}

void App::portPreviewOpen(int item) {
    if (!pCat_ || item < 0 || item >= (int)pCat_->items.size()) return;
    pPv_ = item;
    pPvData_ = PortPreview();
    pPvWait_ = true;
    pPop_ = 0;
    core_->portPreview(pCat_->items[item].id);
    updateSpinTimer();
    invalidate();
}

void App::portCursor(int idx, bool keyboard) {
    if (pList_.empty()) return;
    pCursor_ = std::max(0, std::min((int)pList_.size() - 1, idx));
    // keep it in view: the grid's geometry of the last frame
    float gw = pGrid_.right - pGrid_.left;
    int cols = std::max(1, (int)floorf((gw + 10) / (168 + 10)));
    float cardW = (gw - (cols - 1) * 10) / cols, pitch = cardW * 258 / 198 + 30;
    float top = pGrid_.top - (pBody_.top - pScroll_) + (pCursor_ / cols) * pitch;   // content coordinates
    if (pCursor_ < cols) pScroll_ = 0;   // the first row: the top of the panel with it
    else if (top - 12 < pScroll_) pScroll_ = top - 12;
    else if (top + pitch - 30 + 12 > pScroll_ + pView_) pScroll_ = top + pitch - 30 + 12 - pView_;
    focusVisible_ = keyboard;
    invalidate();
}

void App::portActivate(int id) {
    auto toggle = [&](int item) {
        if (!pCat_ || item < 0 || item >= (int)pCat_->items.size() || !importable(pCat_->items[item])) return;
        if (pSel_.size() != pCat_->items.size()) pSel_.assign(pCat_->items.size(), 0);
        pSel_[item] ^= 1;
        pSelN_ += pSel_[item] ? 1 : -1;
        invalidate();
    };
    if (id >= ID_P_QB0 && id < ID_P_QB0 + P_SLOTS) { int s = id - ID_P_QB0; if (s < (int)pSlot_.size()) toggle(pList_[pSlot_[s]]); return; }
    if (id >= ID_P_CARD0 && id < ID_P_CARD0 + P_SLOTS) {
        int s = id - ID_P_CARD0;
        if (s < (int)pSlot_.size()) { pCursor_ = pSlot_[s]; portPreviewOpen(pList_[pSlot_[s]]); }
        return;
    }
    if (id >= ID_P_CHIP0 && id < ID_P_CHIP0 + 120) {
        int k = id - ID_P_CHIP0;
        if (k <= NT) { pTypes_ = k == 0 ? 0 : 1u << (k - 1); pPop_ = 0; setFocus(ID_P_CAT, focusVisible_); }
        else if (k >= 20 && k < 24) pSts_ ^= 1u << (k - 20);
        else if (k >= 40 && k < 40 + NT) pTypes_ ^= 1u << (k - 40);
        else if (k >= 60 && k < 60 + NR) pRars_ ^= 1u << (k - 60);
        else if (k >= 100 && k < 104) { pSort_ = k - 100; pPop_ = 0; setFocus(ID_P_SORT, focusVisible_); }
        portRefilter();
        return;
    }
    switch (id) {
    case ID_P_OPEN: portShow(!pOpen_); break;   // the lobby's Cosmetics button
    case ID_P_CELL: portShow(true); break;      // the lobby's COSMETICS status cell
    case ID_P_CLOSE: portShow(false); break;
    case ID_P_REFRESH: case ID_P_RETRY: portLoad(true); break;
    case ID_P_CAT: case ID_P_FILTERS: case ID_P_SORT: {
        int p = id == ID_P_CAT ? 1 : id == ID_P_FILTERS ? 2 : 3;
        pPop_ = pPop_ == p ? 0 : p;
        if (pPop_ && focusVisible_) focus_ = p == 1 ? ID_P_CHIP0 : p == 2 ? ID_P_CHIP0 + 20 : ID_P_CHIP0 + 100 + pSort_;
        invalidate();
        break;
    }
    case ID_P_NEW: pSts_ = pSts_ == 1u ? 0 : 1u; portRefilter(); break;
    case ID_P_CLEARFILTERS: pTypes_ = pSts_ = pRars_ = 0; portRefilter(); break;
    case ID_P_DONE: pPop_ = 0; setFocus(ID_P_FILTERS, focusVisible_); break;
    case ID_P_CLEARSEL: std::fill(pSel_.begin(), pSel_.end(), 0); pSelN_ = 0; invalidate(); break;
    case ID_P_IMPORTSEL: {
        std::vector<int> v;
        for (size_t i = 0; i < pSel_.size(); i++) if (pSel_[i]) v.push_back((int)i);
        portConfirm(v, false);
        break;
    }
    case ID_P_IMPORTALL: {
        std::vector<int> v;
        for (int i : pList_) if (importable(pCat_->items[i])) v.push_back(i);
        portConfirm(v, true);
        break;
    }
    case ID_P_DLG_CANCEL: pDlg_ = false; focus_ = 0; invalidate(); break;
    case ID_P_DLG_IMPORT: {
        std::vector<std::wstring> paths;
        for (int i : pPending_) paths.push_back(pCat_->items[i].path);
        pDlg_ = false; pPv_ = -1;
        std::fill(pSel_.begin(), pSel_.end(), 0); pSelN_ = 0;
        pjEndFor_ = pBuild_;   // the build it runs on, kept for its finished card
        core_->portImport(pBuild_, paths);
        focus_ = 0;
        invalidate();
        break;
    }
    case ID_P_STOP: core_->portCancel(); break;
    case ID_P_JOBX: pjEnd_.reset(); pLogOpen_ = false; break;
    case ID_P_JOBGO:
        if (pjEnd_ && pjEnd_->phase == L"done") { std::wstring bn = pjEndFor_.empty() ? pBuild_ : pjEndFor_; pjEnd_.reset(); portShow(false); core_->command(Command::Launch, bn); }   // to the lobby, launching
        else openActivity(true);
        break;
    case ID_P_LOG: pLogOpen_ = !pLogOpen_; invalidate(); break;
    case ID_P_PV_CLOSE: pPv_ = -1; focus_ = 0; invalidate(); break;
    case ID_P_PV_SELECT: toggle(pPv_); break;
    case ID_P_PV_IMPORT: portConfirm({ pPv_ }, false); break;
    case ID_P_PV_LINK:
        // only a fortnite.gg page (the URL comes from a downloaded file)
        if (!pPvData_.page.compare(0, 20, "https://fortnite.gg/") && hwnd_) {
            std::wstring u(pPvData_.page.begin(), pPvData_.page.end());
            ShellExecuteW(hwnd_, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    case ID_P_SCRIM:
        if (pDlg_) break;   // the dialog is modal
        if (pPop_) { pPop_ = 0; invalidate(); break; }
        if (pPv_ >= 0) { pPv_ = -1; invalidate(); }
        break;
    }
}

bool App::portKey(UINT vk, bool ctrl, bool shift) {
    (void)ctrl; (void)shift;
    if (vk == VK_ESCAPE) {   // the dialog, a popup, the preview, then the panel
        if (pDlg_) { pDlg_ = false; focus_ = 0; }
        else if (pPop_) { int t = pPop_ == 1 ? ID_P_CAT : pPop_ == 2 ? ID_P_FILTERS : ID_P_SORT; pPop_ = 0; setFocus(t, focusVisible_); }
        else if (pPv_ >= 0) { pPv_ = -1; focus_ = ID_P_GRID; }
        else portShow(false);
        invalidate();
        return true;
    }
    if (focus_ == ID_P_GRID && !pDlg_ && pPv_ < 0 && !pPop_ && !pList_.empty()) {
        float gw = pGrid_.right - pGrid_.left;
        int cols = std::max(1, (int)floorf((gw + 10) / (168 + 10)));
        float cardH = (gw - (cols - 1) * 10) / cols * 258 / 198;
        int page = std::max(1, (int)floorf(pView_ / (cardH + 30))) * cols;
        int c = pCursor_;
        switch (vk) {
        case VK_LEFT: c--; break;
        case VK_RIGHT: c++; break;
        case VK_UP: c -= cols; break;
        case VK_DOWN: c += cols; break;
        case VK_PRIOR: c -= page; break;
        case VK_NEXT: c += page; break;
        case VK_HOME: c = 0; break;
        case VK_END: c = (int)pList_.size() - 1; break;
        case VK_RETURN: portPreviewOpen(pList_[pCursor_]); return true;
        case VK_SPACE: {
            int item = pList_[pCursor_];
            if (importable(pCat_->items[item])) {
                if (pSel_.size() != pCat_->items.size()) pSel_.assign(pCat_->items.size(), 0);
                pSel_[item] ^= 1; pSelN_ += pSel_[item] ? 1 : -1;
            }
            focusVisible_ = true; invalidate();
            return true;
        }
        default: return false;
        }
        portCursor(c, true);
        return true;
    }
    if (pOpen_ && !pDlg_ && pPv_ < 0 && !pPop_ && (vk == VK_PRIOR || vk == VK_NEXT)) { pScroll_ += (vk == VK_NEXT ? 1 : -1) * pView_ * 0.9f; invalidate(); return true; }
    if (pPop_ && (vk == VK_DOWN || vk == VK_UP || vk == VK_LEFT || vk == VK_RIGHT)) { moveFocus(vk == VK_DOWN || vk == VK_RIGHT ? 1 : -1); return true; }
    return false;
}

// ================================================================================================ selftest
void App::portScene(int what) {
    portShow(true);
    drain();
    if (what == 2) pPop_ = 2;
    if (what == 3) { std::vector<int> v; for (int i : pList_.empty() ? (portRebuild(), pList_) : pList_) if (importable(pCat_->items[i])) v.push_back(i); portConfirm(v, true); }
    if (what == 4 || what == 5 || what == 7) {
        if (pDirty_) portRebuild();
        if (what == 4 && !pList_.empty()) { portPreviewOpen(pList_[1 < pList_.size() ? 1 : 0]); drain(); }
        if (what == 7 && !pList_.empty()) { portPreviewOpen(pList_[0]); drain(); }
        if (what == 5 && pCat_) {
            pSel_.assign(pCat_->items.size(), 0);
            int n = 0;
            for (int i : pList_) if (importable(pCat_->items[i]) && n < 3) { pSel_[i] = 1; n++; }
            pSelN_ = n;
            hover_ = ID_P_CARD0 + 1;
        }
    }
    if (what == 6) pScroll_ = 1e9f;
    invalidate();
}

std::vector<std::pair<std::wstring, bool>> App::portCheck(float W, float H) {
    std::vector<std::pair<std::wstring, bool>> out;
    auto frame = [&] { rt_->BeginDraw(); paint(W, H); rt_->EndDraw(); };
    auto center = [&](int id, float* x, float* y) { D2D1_RECT_F r = rectOf(id); *x = (r.left + r.right) / 2; *y = (r.top + r.bottom) / 2; };
    auto click = [&](int id) { frame(); float x, y; center(id, &x, &y); mouseDown(x, y); mouseUp(x, y); drain(); frame(); };
    auto check = [&](const wchar_t* name, bool ok) { out.push_back({ name, ok }); };
    frame(); frame();
    click(ID_P_OPEN);
    check(L"Browse and import opens the panel with the build's catalog", pOpen_ && pCat_ && !pCat_->items.empty());
    size_t all = pList_.size();
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    frame();
    QueryPerformanceCounter(&t1);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    check(fmt(L"frame with %d cards in the list under 50 ms (%d ms, %d drawn)", (int)all, (int)ms, (int)pSlot_.size()).c_str(), ms < 50 && pSlot_.size() < 120);
    click(ID_P_NEW);
    bool onlyNew = !pList_.empty();
    for (int i : pList_) if (pCat_->items[i].status != PortStatus::New) onlyNew = false;
    check(L"Not imported shows only new items", pSts_ == 1u && onlyNew);
    click(ID_P_NEW);
    click(ID_P_CAT);
    check(L"the category button opens its popup", pPop_ == 1);
    click(ID_P_CHIP0 + 1 + (int)PortType::Emote);
    bool emotes = !pList_.empty();
    for (int i : pList_) if (pCat_->items[i].type != PortType::Emote) emotes = false;
    check(L"picking Emotes closes it and filters", pPop_ == 0 && emotes);
    click(ID_P_CAT); click(ID_P_CHIP0);
    check(L"All brings every item back", pList_.size() == all);
    pQ_ = L"lonestar"; pDirty_ = true; frame();
    check(L"search finds an id regardless of case", pList_.size() >= 1 && pList_.size() < all);
    pQ_.clear(); pDirty_ = true; frame();
    click(ID_P_QB0);
    check(L"the select button selects a card", pSelN_ == 1);
    click(ID_P_IMPORTSEL);
    check(L"Import selected opens the dialog with it", pDlg_ && pPending_.size() == 1);
    key(VK_ESCAPE, false, false); frame();
    check(L"Escape closes the dialog first, the panel stays", !pDlg_ && pOpen_);
    click(ID_P_CARD0);
    check(L"a click on a card opens its preview", pPv_ >= 0);
    drain(); frame();
    check(L"the preview gets its details", !pPvWait_);
    key(VK_ESCAPE, false, false); frame();
    check(L"Escape closes the preview", pPv_ < 0 && pOpen_);
    setFocus(ID_P_GRID, true); pCursor_ = 0; key(VK_RIGHT, false, false); key(VK_DOWN, false, false); frame();
    float gw = pGrid_.right - pGrid_.left;
    int cols = std::max(1, (int)floorf((gw + 10) / (168 + 10)));
    check(L"arrows move the grid's cursor (right, down a row)", pCursor_ == 1 + cols);
    key(VK_END, false, false); frame();
    check(L"End scrolls the last card into view", pCursor_ == (int)pList_.size() - 1 && pScroll_ > 0);
    key(VK_HOME, false, false); frame();
    check(L"Home goes back to the top", pCursor_ == 0 && pScroll_ < 1);
    click(ID_P_IMPORTALL);
    bool dlgAll = pDlg_ && (int)pPending_.size() == pTodo_;
    click(ID_P_DLG_IMPORT);
    drain(); frame();
    check(L"Import all asks, then the import starts and shows its progress", dlgAll && !pDlg_ && pj_ != nullptr);
    click(ID_P_STOP);
    for (int i = 0; i < 40 && pj_; i++) { Sleep(50); drain(); }
    frame();
    check(L"Stop ends the import", pj_ == nullptr);
    key(VK_ESCAPE, false, false); frame();
    check(L"Escape closes the panel", !pOpen_);
    return out;
}

} // namespace rn
