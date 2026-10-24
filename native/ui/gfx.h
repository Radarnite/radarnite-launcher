// Direct2D / DirectWrite helpers for the native UI: factories, the embedded Inter fonts, text drawing with CSS-like
// line boxes, and the icons of ui/index.html as path geometries.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <string>
#include <vector>

namespace rn {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

inline D2D1_COLOR_F rgb(uint32_t hex, float a = 1.f) {
    return D2D1::ColorF(((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a);
}

// Design tokens (ui/index.html :root).
namespace C {
const uint32_t bg = 0x101114, bg2 = 0x181A1F, surface = 0x202228, surface2 = 0x24262C, hover = 0x292C33, line = 0x303238,
               line2 = 0x25272C, l2 = 0x3A3D44, text = 0xF5F6F7, ink2 = 0xD4D4D8, muted = 0xA1A1AA, dim = 0x71717A,
               ok = 0x6CCB8F, warn = 0xE3B55B, bad = 0xE5646E, warnText = 0xF1D9A6, badText = 0xF4C0C5, closeRed = 0xC42B1C,
               white = 0xFFFFFF;
}

enum class Face { UI, Display, Mono };
struct Font {
    Face face = Face::UI;
    float size = 13;
    int weight = 400;
    bool italic = false;
    bool tnum = false;   // font-variant-numeric: tabular-nums
};

enum class Icon {
    Play, Plus, More, Chev, Lobby, Build, Bots, Match, Cos, Fw, Zip, Folder, Refresh, Link, Cog, Log, Copy, Trash, Warn,
    Check, X, WMin, WMax, WRes, WClose, Count
};

class Gfx {
public:
    bool init(HINSTANCE inst);
    void shutdown();

    ID2D1Factory* d2d = nullptr;
    IDWriteFactory5* dw = nullptr;
    IWICImagingFactory* wic = nullptr;
    IDWriteTypography* tnum = nullptr;   // OpenType tnum, for Font::tnum

    IDWriteTextFormat* format(const Font& f);
    // Line box metrics of a font (CSS "normal" content area): ascent and ascent+descent in DIPs.
    void fontBox(const Font& f, float* ascent, float* height);
    // Advance of one character of the mono font (for the virtualized log wrapping).
    float monoAdvance(float size);

    // Icon geometry (viewBox units) and its viewBox size (16, or 10 for the window controls).
    struct IconPart { ID2D1Geometry* geo; bool fill; float strokeWidth; ID2D1StrokeStyle* stroke; };
    const std::vector<IconPart>& icon(Icon i, float* viewBox);

    // Raw TTF bytes of Inter (for GDI: the native EDIT controls) and the GDI family name to use.
    std::wstring gdiFamily = L"Segoe UI", gdiDisplayFamily = L"Segoe UI";   // Inter, Inter Tight (the bots number EDIT)
    // Logo (radarnite.png), decoded once.
    IWICBitmapSource* logo = nullptr;
    std::wstring families;   // the embedded families found (selftest report)

private:
    bool loadFonts(HINSTANCE inst);
    ID2D1StrokeStyle* strokeStyle(int cap, int join);
    IDWriteFontCollection1* fonts_ = nullptr;
    std::wstring uiFamily_ = L"Segoe UI", displayFamily_ = L"Segoe UI", monoFamily_ = L"Consolas";
    struct Fmt { Face face; int sizeX100; int weight; bool italic; IDWriteTextFormat* f; float asc, h; };
    std::vector<Fmt> formats_;
    float monoAdv_ = 0, monoAdvSize_ = 0;
    Fmt& fmt(const Font& f);
    std::vector<std::vector<IconPart>> icons_;
    std::vector<float> iconBox_;
    ID2D1StrokeStyle* strokes_[3][3] = {};
    std::vector<std::vector<uint8_t>> fontData_;
    HANDLE gdiFont_ = nullptr, gdiFont2_ = nullptr;
};

} // namespace rn
