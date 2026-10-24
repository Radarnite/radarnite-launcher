#include "gfx.h"
#include "resource.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace rn {

// ------------------------------------------------------------------------------------------------ icons
// The <symbol> contents of ui/index.html, verbatim; parsed into path geometries once (on first use).
struct IconSrc { float box; const char* svg; };
static const IconSrc kIcons[(int)Icon::Count] = {
    { 16, R"(<path d="M4.5 2.6v10.8L13.2 8z" fill="currentColor"/>)" },
    { 16, R"(<path d="M8 3v10M3 8h10" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" fill="none"/>)" },
    { 16, R"(<circle cx="3.5" cy="8" r="1.3" fill="currentColor"/><circle cx="8" cy="8" r="1.3" fill="currentColor"/><circle cx="12.5" cy="8" r="1.3" fill="currentColor"/>)" },
    { 16, R"(<path d="M4 10l4-4 4 4" stroke="currentColor" stroke-width="1.7" fill="none" stroke-linecap="round" stroke-linejoin="round"/>)" },
    { 16, R"(<path d="M8 8.2a2.6 2.6 0 1 0 0-5.2 2.6 2.6 0 0 0 0 5.2zM3 13.5c.6-2.3 2.6-3.6 5-3.6s4.4 1.3 5 3.6" stroke="currentColor" stroke-width="1.5" fill="none" stroke-linecap="round"/>)" },
    { 16, R"(<path d="M2.5 13.5h11M4 13.5V8h8v5.5M4 8l4-4.5L12 8" stroke="currentColor" stroke-width="1.5" fill="none" stroke-linejoin="round" stroke-linecap="round"/>)" },
    { 16, R"(<rect x="3" y="5" width="10" height="8" rx="2" stroke="currentColor" stroke-width="1.5" fill="none"/><path d="M8 2.5V5M6 9h.01M10 9h.01" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/>)" },
    { 16, R"(<path d="M8 2l5 2v4c0 3-2.2 5.2-5 6-2.8-.8-5-3-5-6V4z" stroke="currentColor" stroke-width="1.5" fill="none" stroke-linejoin="round"/>)" },
    { 16, R"(<path d="M5.5 2.5L3 4l-1 3 2 .8V13.5h8V7.8l2-.8-1-3-2.5-1.5C10 3.8 9.1 4.5 8 4.5S6 3.8 5.5 2.5z" stroke="currentColor" stroke-width="1.4" fill="none" stroke-linejoin="round"/>)" },
    { 16, R"(<rect x="3" y="7" width="10" height="7" rx="1.6" stroke="currentColor" stroke-width="1.5" fill="none"/><path d="M5.5 7V5.2a2.5 2.5 0 0 1 5 0V7" stroke="currentColor" stroke-width="1.5" fill="none"/>)" },
    { 16, R"(<path d="M4 2h5.5L12.5 5v9H4z" stroke="currentColor" stroke-width="1.4" fill="none" stroke-linejoin="round"/><path d="M7 5h1M7 7h1M7 9h1" stroke="currentColor" stroke-width="1.4"/>)" },
    { 16, R"(<path d="M2 4.5c0-.8.6-1.5 1.5-1.5H6l1.5 1.7h5c.8 0 1.5.6 1.5 1.5v6c0 .8-.7 1.5-1.5 1.5h-9C2.6 13.7 2 13 2 12.2z" stroke="currentColor" stroke-width="1.4" fill="none"/>)" },
    { 16, R"(<path d="M13 8a5 5 0 1 1-1.5-3.6M13 2.5v3h-3" stroke="currentColor" stroke-width="1.5" fill="none" stroke-linecap="round" stroke-linejoin="round"/>)" },
    { 16, R"(<path d="M6.5 9.5l3-3M7 4.5l1-1a2.8 2.8 0 0 1 4 4l-1 1M9 11.5l-1 1a2.8 2.8 0 0 1-4-4l1-1" stroke="currentColor" stroke-width="1.5" fill="none" stroke-linecap="round"/>)" },
    { 16, R"(<circle cx="8" cy="8" r="2.2" stroke="currentColor" stroke-width="1.4" fill="none"/><path d="M8 1.8v1.6M8 12.6v1.6M1.8 8h1.6M12.6 8h1.6M3.6 3.6l1.1 1.1M11.3 11.3l1.1 1.1M3.6 12.4l1.1-1.1M11.3 4.7l1.1-1.1" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>)" },
    { 16, R"(<path d="M3 4h10M3 8h10M3 12h6" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>)" },
    { 16, R"(<rect x="5" y="5" width="8" height="8" rx="1.5" stroke="currentColor" stroke-width="1.4" fill="none"/><path d="M3 10.5V4.5C3 3.7 3.7 3 4.5 3h6" stroke="currentColor" stroke-width="1.4" fill="none" stroke-linecap="round"/>)" },
    { 16, R"(<path d="M3 4.5h10M6.5 4.5V3h3v1.5M4.5 4.5l.6 8.5h5.8l.6-8.5" stroke="currentColor" stroke-width="1.4" fill="none" stroke-linecap="round" stroke-linejoin="round"/>)" },
    { 16, R"(<path d="M8 2.5l6 10.5H2z" stroke="currentColor" stroke-width="1.4" fill="none" stroke-linejoin="round"/><path d="M8 7v2.6M8 11.3h.01" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>)" },
    { 16, R"(<path d="M3.5 8.5l3 3 6-7" stroke="currentColor" stroke-width="1.8" fill="none" stroke-linecap="round" stroke-linejoin="round"/>)" },
    { 16, R"(<path d="M4.5 4.5l7 7M11.5 4.5l-7 7" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>)" },
    { 10, R"(<path d="M0 5h10" stroke="currentColor" stroke-width="1"/>)" },
    { 10, R"(<rect x=".5" y=".5" width="9" height="9" stroke="currentColor" fill="none"/>)" },
    { 10, R"(<rect x=".5" y="2.5" width="7" height="7" stroke="currentColor" fill="none"/><path d="M2.5 2.5V.5h7v7h-2" stroke="currentColor" fill="none"/>)" },
    { 10, R"(<path d="M0 0l10 10M10 0L0 10" stroke="currentColor" stroke-width="1"/>)" },
};

static bool attr(const char* el, const char* end, const char* name, std::string* out) {
    size_t n = strlen(name);
    for (const char* p = el + 1; p + n + 2 < end; p++) {
        if (p[-1] == ' ' && !strncmp(p, name, n) && p[n] == '=' && p[n + 1] == '"') {
            const char* v = p + n + 2; const char* q = (const char*)memchr(v, '"', end - v);
            if (!q) return false;
            out->assign(v, q - v); return true;
        }
    }
    return false;
}
static float fattr(const char* el, const char* end, const char* name, float def) {
    std::string s; return attr(el, end, name, &s) ? (float)atof(s.c_str()) : def;
}

// SVG path data -> geometry sink (M L H V C S A Z, absolute and relative, implicit repeats).
struct PathParser {
    const char* p;
    bool num(float* v) {
        while (*p == ' ' || *p == ',' || *p == '\n') p++;
        const char* s = p;
        if (*p == '-' || *p == '+') p++;
        bool dot = false, any = false;
        while ((*p >= '0' && *p <= '9') || (*p == '.' && !dot)) { if (*p == '.') dot = true; else any = true; p++; }
        if (*p == 'e' || *p == 'E') { p++; if (*p == '-' || *p == '+') p++; while (*p >= '0' && *p <= '9') p++; }
        if (!any) { p = s; return false; }
        *v = (float)strtod(std::string(s, p - s).c_str(), nullptr);
        return true;
    }
    void run(ID2D1GeometrySink* k, bool fill) {
        D2D1_POINT_2F cur = { 0, 0 }, start = { 0, 0 }, ctrl = { 0, 0 };
        bool open = false; char cmd = 0, last = 0;
        auto begin = [&] { if (!open) { k->BeginFigure(cur, fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW); open = true; } };
        while (true) {
            while (*p == ' ' || *p == ',' || *p == '\n') p++;
            if (!*p) break;
            if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) cmd = *p++;
            else if (cmd == 'M') cmd = 'L'; else if (cmd == 'm') cmd = 'l';   // implicit lineto after moveto
            bool rel = cmd >= 'a';
            float a[7];
            switch (cmd | 0x20) {
            case 'm':
                if (!num(&a[0]) || !num(&a[1])) return;
                if (open) { k->EndFigure(D2D1_FIGURE_END_OPEN); open = false; }
                cur = rel ? D2D1::Point2F(cur.x + a[0], cur.y + a[1]) : D2D1::Point2F(a[0], a[1]);
                start = cur; begin();
                break;
            case 'l':
                if (!num(&a[0]) || !num(&a[1])) return;
                begin(); cur = rel ? D2D1::Point2F(cur.x + a[0], cur.y + a[1]) : D2D1::Point2F(a[0], a[1]); k->AddLine(cur);
                break;
            case 'h':
                if (!num(&a[0])) return;
                begin(); cur.x = rel ? cur.x + a[0] : a[0]; k->AddLine(cur);
                break;
            case 'v':
                if (!num(&a[0])) return;
                begin(); cur.y = rel ? cur.y + a[0] : a[0]; k->AddLine(cur);
                break;
            case 'c': {
                for (int i = 0; i < 6; i++) if (!num(&a[i])) return;
                float ox = rel ? cur.x : 0, oy = rel ? cur.y : 0;
                D2D1_BEZIER_SEGMENT b = { { ox + a[0], oy + a[1] }, { ox + a[2], oy + a[3] }, { ox + a[4], oy + a[5] } };
                begin(); k->AddBezier(b); ctrl = b.point2; cur = b.point3;
                break;
            }
            case 's': {
                for (int i = 0; i < 4; i++) if (!num(&a[i])) return;
                float ox = rel ? cur.x : 0, oy = rel ? cur.y : 0;
                bool prevC = (last | 0x20) == 'c' || (last | 0x20) == 's';
                D2D1_POINT_2F c1 = prevC ? D2D1::Point2F(2 * cur.x - ctrl.x, 2 * cur.y - ctrl.y) : cur;
                D2D1_BEZIER_SEGMENT b = { c1, { ox + a[0], oy + a[1] }, { ox + a[2], oy + a[3] } };
                begin(); k->AddBezier(b); ctrl = b.point2; cur = b.point3;
                break;
            }
            case 'a': {
                for (int i = 0; i < 7; i++) if (!num(&a[i])) return;
                D2D1_POINT_2F to = rel ? D2D1::Point2F(cur.x + a[5], cur.y + a[6]) : D2D1::Point2F(a[5], a[6]);
                D2D1_ARC_SEGMENT s = { to, { a[0], a[1] }, a[2], a[4] != 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                       a[3] != 0 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL };
                begin(); k->AddArc(s); cur = to;
                break;
            }
            case 'z':
                if (open) { k->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; }
                cur = start;
                break;
            default: return;
            }
            last = cmd;
        }
        if (open) k->EndFigure(D2D1_FIGURE_END_OPEN);
    }
};

ID2D1StrokeStyle* Gfx::strokeStyle(int cap, int join) {
    auto& s = strokes_[cap][join];
    if (!s) {
        D2D1_CAP_STYLE cs = cap == 1 ? D2D1_CAP_STYLE_ROUND : cap == 2 ? D2D1_CAP_STYLE_SQUARE : D2D1_CAP_STYLE_FLAT;
        D2D1_LINE_JOIN js = join == 1 ? D2D1_LINE_JOIN_ROUND : join == 2 ? D2D1_LINE_JOIN_BEVEL : D2D1_LINE_JOIN_MITER;
        d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(cs, cs, cs, js), nullptr, 0, &s);
    }
    return s;
}

const std::vector<Gfx::IconPart>& Gfx::icon(Icon i, float* viewBox) {
    if (icons_.empty()) { icons_.resize((int)Icon::Count); iconBox_.resize((int)Icon::Count); }
    auto& parts = icons_[(int)i];
    const IconSrc& src = kIcons[(int)i];
    *viewBox = src.box;
    if (!parts.empty()) return parts;
    for (const char* e = src.svg; (e = strchr(e, '<')); ) {
        const char* end = strchr(e, '>'); if (!end) break;
        std::string v;
        bool fill = attr(e, end, "fill", &v) && v == "currentColor";
        bool stroke = attr(e, end, "stroke", &v) && v == "currentColor";
        float sw = stroke ? fattr(e, end, "stroke-width", 1.f) : 0.f;
        int cap = attr(e, end, "stroke-linecap", &v) ? (v == "round" ? 1 : v == "square" ? 2 : 0) : 0;
        int join = attr(e, end, "stroke-linejoin", &v) ? (v == "round" ? 1 : v == "bevel" ? 2 : 0) : 0;
        ID2D1Geometry* g = nullptr;
        if (!strncmp(e, "<path", 5) && attr(e, end, "d", &v)) {
            ID2D1PathGeometry* pg = nullptr; ID2D1GeometrySink* k = nullptr;
            if (SUCCEEDED(d2d->CreatePathGeometry(&pg)) && SUCCEEDED(pg->Open(&k))) {
                k->SetFillMode(D2D1_FILL_MODE_WINDING);
                PathParser pp{ v.c_str() }; pp.run(k, fill);
                k->Close(); k->Release();
                g = pg;
            }
        } else if (!strncmp(e, "<rect", 5)) {
            float x = fattr(e, end, "x", 0), y = fattr(e, end, "y", 0), w = fattr(e, end, "width", 0), h = fattr(e, end, "height", 0), rx = fattr(e, end, "rx", 0);
            ID2D1RoundedRectangleGeometry* rg = nullptr;
            d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rx, rx), &rg);
            g = rg;
        } else if (!strncmp(e, "<circle", 7)) {
            float cx = fattr(e, end, "cx", 0), cy = fattr(e, end, "cy", 0), r = fattr(e, end, "r", 0);
            ID2D1EllipseGeometry* eg = nullptr;
            d2d->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), &eg);
            g = eg;
        }
        if (g) parts.push_back({ g, fill, sw, stroke ? strokeStyle(cap, join) : nullptr });
        e = end;
    }
    return parts;
}

// ------------------------------------------------------------------------------------------------ fonts
static bool resourceBytes(HINSTANCE inst, int id, const void** data, DWORD* size) {
    HRSRC r = FindResourceW(inst, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10 /*RT_RCDATA*/));
    if (!r) return false;
    HGLOBAL g = LoadResource(inst, r);
    *data = LockResource(g); *size = SizeofResource(inst, r);
    return *data && *size;
}

bool Gfx::loadFonts(HINSTANCE inst) {
    IDWriteInMemoryFontFileLoader* loader = nullptr;
    if (FAILED(dw->CreateInMemoryFontFileLoader(&loader))) return false;
    dw->RegisterFontFileLoader(loader);
    IDWriteFontSetBuilder1* builder = nullptr;
    if (FAILED(dw->CreateFontSetBuilder(&builder))) { loader->Release(); return false; }
    for (int id : { RES_FONT_INTER, RES_FONT_INTER_TIGHT }) {
        const void* data; DWORD size;
        if (!resourceBytes(inst, id, &data, &size)) continue;
        IDWriteFontFileStream* st = nullptr;
        if (FAILED(dw->UnpackFontFile(DWRITE_CONTAINER_TYPE_WOFF2, data, size, &st))) continue;
        UINT64 n = 0; st->GetFileSize(&n);
        const void* frag; void* ctx;
        std::vector<uint8_t> ttf;
        if (SUCCEEDED(st->ReadFileFragment(&frag, 0, n, &ctx))) { ttf.assign((const uint8_t*)frag, (const uint8_t*)frag + n); st->ReleaseFileFragment(ctx); }
        st->Release();
        if (ttf.empty()) continue;
        fontData_.push_back(std::move(ttf));
        auto& keep = fontData_.back();
        IDWriteFontFile* file = nullptr;
        if (SUCCEEDED(loader->CreateInMemoryFontFileReference(dw, keep.data(), (UINT32)keep.size(), nullptr, &file))) { builder->AddFontFile(file); file->Release(); }
        DWORD count = 0;   // GDI copies for the EDIT controls
        if (id == RES_FONT_INTER) gdiFont_ = AddFontMemResourceEx(keep.data(), (DWORD)keep.size(), nullptr, &count);
        else gdiFont2_ = AddFontMemResourceEx(keep.data(), (DWORD)keep.size(), nullptr, &count);
    }
    IDWriteFontSet* set = nullptr;
    bool ok = SUCCEEDED(builder->CreateFontSet(&set)) && SUCCEEDED(dw->CreateFontCollectionFromFontSet(set, &fonts_));
    release(set); builder->Release(); loader->Release();   // the loader stays registered with the factory
    if (!ok) return false;
    for (UINT32 i = 0, n = fonts_->GetFontFamilyCount(); i < n; i++) {
        IDWriteFontFamily1* fam = nullptr; IDWriteLocalizedStrings* names = nullptr;
        if (SUCCEEDED(fonts_->GetFontFamily(i, &fam)) && SUCCEEDED(fam->GetFamilyNames(&names))) {
            wchar_t name[128] = L""; names->GetString(0, name, 128);
            if (!families.empty()) families += L", ";
            families += name;
            if (wcsstr(name, L"Tight")) displayFamily_ = name; else if (!wcscmp(name, L"Inter")) uiFamily_ = name;
        }
        release(names); release(fam);
    }
    if (displayFamily_ == L"Segoe UI") displayFamily_ = uiFamily_;
    // the mono stack of the page: Cascadia Mono, then Consolas
    IDWriteFontCollection* sys = nullptr;
    if (SUCCEEDED(static_cast<IDWriteFactory*>(dw)->GetSystemFontCollection(&sys))) {
        UINT32 idx; BOOL exists = FALSE;
        if (SUCCEEDED(sys->FindFamilyName(L"Cascadia Mono", &idx, &exists)) && exists) monoFamily_ = L"Cascadia Mono";
        sys->Release();
    }
    if (gdiFont_) gdiFamily = L"Inter";
    gdiDisplayFamily = gdiFont2_ ? displayFamily_ : gdiFamily;
    return true;
}

bool Gfx::init(HINSTANCE inst) {
    D2D1_FACTORY_OPTIONS fo = {};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &fo, (void**)&d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), (IUnknown**)&dw))) return false;
    loadFonts(inst);   // without the embedded fonts the system UI font is used
    if (SUCCEEDED(dw->CreateTypography(&tnum))) tnum->AddFontFeature({ DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 });
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        const void* data; DWORD size;
        IWICStream* s = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
        if (resourceBytes(inst, RES_LOGO_PNG, &data, &size) && SUCCEEDED(wic->CreateStream(&s)) && SUCCEEDED(s->InitializeFromMemory((BYTE*)data, size)) &&
            SUCCEEDED(wic->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
            SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            logo = cv; cv = nullptr;
        }
        release(cv); release(fr); release(dec); release(s);
    }
    return true;
}

void Gfx::shutdown() {
    for (auto& f : formats_) release(f.f);
    formats_.clear();
    for (auto& v : icons_) for (auto& p : v) release(p.geo);
    icons_.clear();
    for (auto& r : strokes_) for (auto& s : r) release(s);
    release(tnum); release(logo); release(fonts_); release(wic); release(dw); release(d2d);
    if (gdiFont_) RemoveFontMemResourceEx(gdiFont_);
    if (gdiFont2_) RemoveFontMemResourceEx(gdiFont2_);
}

Gfx::Fmt& Gfx::fmt(const Font& f) {
    int sx = (int)lroundf(f.size * 100);
    for (auto& x : formats_) if (x.face == f.face && x.sizeX100 == sx && x.weight == f.weight && x.italic == f.italic) return x;
    const std::wstring& fam = f.face == Face::Display ? displayFamily_ : f.face == Face::Mono ? monoFamily_ : uiFamily_;
    IDWriteTextFormat* tf = nullptr;
    dw->CreateTextFormat(fam.c_str(), f.face == Face::Mono ? nullptr : fonts_, (DWRITE_FONT_WEIGHT)f.weight,
                         f.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, f.size, L"en-us", &tf);
    float asc = f.size * 0.97f, h = f.size * 1.21f;
    if (tf) {
        tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        IDWriteTextLayout* l = nullptr;
        if (SUCCEEDED(dw->CreateTextLayout(L"Ag", 2, tf, 1000, 1000, &l))) {
            DWRITE_LINE_METRICS lm; UINT32 n = 0;
            if (SUCCEEDED(l->GetLineMetrics(&lm, 1, &n)) && n) { asc = lm.baseline; h = lm.height; }
            l->Release();
        }
    }
    formats_.push_back({ f.face, sx, f.weight, f.italic, tf, asc, h });
    return formats_.back();
}

IDWriteTextFormat* Gfx::format(const Font& f) { return fmt(f).f; }
void Gfx::fontBox(const Font& f, float* ascent, float* height) { auto& x = fmt(f); *ascent = x.asc; *height = x.h; }

float Gfx::monoAdvance(float size) {
    if (size == monoAdvSize_) return monoAdv_;
    Font f{ Face::Mono, size, 400, false };
    IDWriteTextLayout* l = nullptr; float w = size * 0.55f;
    if (SUCCEEDED(dw->CreateTextLayout(L"0000000000", 10, format(f), 1000, 100, &l))) {
        DWRITE_TEXT_METRICS m; if (SUCCEEDED(l->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace / 10;
        l->Release();
    }
    monoAdvSize_ = size; monoAdv_ = w;
    return w;
}

} // namespace rn
