#include "art.h"
#include "../core/net.h"
#include "../core/paths.h"
#include "../core/util.h"
#include "gfx.h"
#include <wincodec.h>
#include <math.h>
#include <algorithm>
#include <set>

namespace rn {

// ------------------------------------------------------------------------------------------------ seasons
// Index = season ordinal (C1S1 = 1 ... C1SX = 10, then one per major: 11 = C2S1 ... 27 = C4 OG ... 31 = C5S4).
// Colors: the mockup's 14.30 blue, then a hue that matches each season's key art; skin = its battle-pass face.
static const SeasonArt kSeasons[] = {
    { L"", { 0x5B6476, 0x2A3040, 0x171B26, 0x0B0D12 }, nullptr },   // 0: unknown version
    { L"", { 0xFF7A59, 0xA8382A, 0x5C1C1A, 0x1A0A0C }, L"cid_028_athena_commando_f", L"Renegade Raider" },
    { L"Medieval", { 0x8FA6C9, 0x3E5276, 0x1F2B45, 0x0A0E18 }, L"cid_035_athena_commando_m_medieval", L"Black Knight" },
    { L"Space Explorers", { 0xA06BFF, 0x5A2FB0, 0x2C1866, 0x0E0A24 }, L"cid_084_athena_commando_m_assassin", L"The Reaper" },
    { L"Superheroes", { 0xFF4D5E, 0xA3202F, 0x561320, 0x16070B }, L"cid_116_athena_commando_m_carbideblack", L"Omega" },
    { L"Worlds Collide", { 0xFFB547, 0xA8641E, 0x5A3412, 0x170D06 }, L"cid_165_athena_commando_m_darkviking", L"Ragnarok" },
    { L"Darkness Rises", { 0xE46BFF, 0x8A2FB0, 0x461863, 0x140A1E }, L"cid_237_athena_commando_f_cowgirl", L"Calamity" },
    { L"You Better Watch Out", { 0x7FD8FF, 0x2F86C4, 0x174A78, 0x081627 }, L"cid_288_athena_commando_m_iceking", L"Ice King" },
    { L"X Marks the Spot", { 0x3FC6B8, 0x1B7A7E, 0x0E4250, 0x061A20 }, L"cid_347_athena_commando_m_pirateprogressive", L"Blackheart" },
    { L"The Future Is Yours", { 0x4AD9FF, 0x2A6FD6, 0x18307A, 0x090F2A }, L"cid_403_athena_commando_m_rooster", L"Sentinel" },
    { L"Out of Time", { 0x7B6BFF, 0x3A33B8, 0x1D1A66, 0x0A0A24 }, L"cid_484_athena_commando_m_knightremix", L"Ultima Knight" },
    { L"New World", { 0x3FD9A8, 0x1C8A78, 0x0F4A48, 0x061A1C }, L"cid_572_athena_commando_m_viper", L"Fusion" },
    { L"Top Secret", { 0xFFD36B, 0xB88A2A, 0x5E4415, 0x181006 }, L"cid_694_athena_commando_m_catburglar", L"Midas" },
    { L"Splash Down", { 0x4FB6FF, 0x1F6FB8, 0x123D6E, 0x06142A }, L"cid_772_athena_commando_m_sandcastle", L"Aquaman" },
    { L"Nexus War", { 0x5D86FF, 0x2A3F9E, 0x152A6E, 0x0A1028 }, L"cid_845_athena_commando_m_hightowertapas", L"Thor" },
    { L"Zero Point", { 0xE8C27A, 0x9A7A3E, 0x4E3D22, 0x14100A }, L"cid_960_athena_commando_m_cosmos", L"The Mandalorian" },
    { L"Primal", { 0x9BE15D, 0x4F8F2A, 0x284C18, 0x0B1608 }, L"cid_a_038_athena_commando_f_towersentinel", L"Spire Assassin" },
    { L"Invasion", { 0x3FE0D0, 0x178A8F, 0x0C4A55, 0x05181F }, L"cid_a_110_athena_commando_m_alientrooper", L"Kymera" },
    { L"Cubed", { 0xFF3B4F, 0xA0182E, 0x55101E, 0x170609 }, L"cid_a_197_athena_commando_m_clash", L"Carnage" },
    { L"Flipped", { 0xFF6363, 0xB8233A, 0x3A1F6E, 0x0B0B24 }, L"cid_a_293_athena_commando_m_parallelcomic", L"Spider-Man" },
    { L"Resistance", { 0xFFA24A, 0xB5521F, 0x5A2A1A, 0x170B08 }, L"cid_a_367_athena_commando_m_mystic", L"Doctor Strange" },
    { L"Vibin'", { 0xFF2D3D, 0x8F1020, 0x460A14, 0x100406 }, L"cid_a_422_athena_commando_m_realm", L"Darth Vader" },
    { L"Paradise", { 0xFF7AD1, 0xB03A92, 0x5A1F55, 0x170A1A }, L"character_rosedust", L"Spider-Gwen" },
    { L"Pillars", { 0x9FD9A0, 0x4F8A5A, 0x284A35, 0x0A1610 }, L"character_magicmeadow", L"Geralt" },
    { L"MEGA", { 0x7FB2D9, 0x3E6A8F, 0x1F3A52, 0x0A141C }, L"character_clearradius", L"Eren Jaeger" },
    { L"Wilds", { 0x6FE08A, 0x2E9450, 0x174D2C, 0x07180E }, L"character_loudphoenix", L"Optimus Prime" },
    { L"Last Resort", { 0xC39BFF, 0x6B47B8, 0x352466, 0x0E0A22 }, L"character_smarthyena", L"Ahsoka Tano" },
    { L"OG", { 0x5AB0FF, 0x2A62C4, 0x163470, 0x08112A }, L"character_rebelclaw_aviator", L"Renegade Lynx" },
    { L"Underground", { 0x4FD1FF, 0x1F7FBF, 0x114270, 0x061428 }, L"character_zebrascramble_bacon", L"Peter Griffin" },
    { L"Myths & Mortals", { 0xF5C96B, 0xA87A2A, 0x4F3A1A, 0x120D06 }, L"character_stunningmountain_armor", L"Zeus" },
    { L"Wrecked", { 0xFF8A3D, 0xB04A1A, 0x5A2512, 0x170906 }, L"character_megatoof_valve", L"Megalo Don" },
    { L"Absolute Doom", { 0x5DFF8A, 0x1F9A4A, 0x104D28, 0x06170C }, L"character_soilblend_haunt", L"Mysterio" },
};

const SeasonArt& seasonArt(int major, int minor) {
    int n = (int)(sizeof(kSeasons) / sizeof(kSeasons[0]));
    int i = major == 1 ? (minor < 8 ? 1 : 2) : major;
    return i >= 1 && i < n ? kSeasons[i] : kSeasons[0];
}

// ------------------------------------------------------------------------------------------------ loader
namespace {

Lock g_failLock;
std::set<std::wstring>& g_failed = *new std::set<std::wstring>;   // per process, never destroyed (a pool item may outlive exit): offline, no wait on every selection

bool safeId(const std::wstring& s) {
    if (s.empty() || s.size() > 80) return false;
    for (wchar_t c : s) if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_')) return false;
    return true;
}

// Separable box blur of an 8-bit plane, radius r, in place (tmp = scratch of the same size).
void boxBlur(std::vector<uint8_t>& p, std::vector<uint8_t>& tmp, int w, int h, int r) {
    if (r < 1) return;
    int d = 2 * r + 1;
    for (int y = 0; y < h; y++) {   // horizontal: p -> tmp
        const uint8_t* s = &p[(size_t)y * w]; uint8_t* o = &tmp[(size_t)y * w];
        int sum = 0;
        for (int x = -r; x <= r; x++) sum += (x >= 0 && x < w) ? s[x] : 0;
        for (int x = 0; x < w; x++) {
            o[x] = (uint8_t)(sum / d);
            int a = x + r + 1, b = x - r;
            sum += (a < w ? s[a] : 0) - (b >= 0 ? s[b] : 0);
        }
    }
    for (int x = 0; x < w; x++) {   // vertical: tmp -> p
        int sum = 0;
        for (int y = -r; y <= r; y++) sum += (y >= 0 && y < h) ? tmp[(size_t)y * w + x] : 0;
        for (int y = 0; y < h; y++) {
            p[(size_t)y * w + x] = (uint8_t)(sum / d);
            int a = y + r + 1, b = y - r;
            sum += (a < h ? tmp[(size_t)a * w + x] : 0) - (b >= 0 ? tmp[(size_t)b * w + x] : 0);
        }
    }
}

// The file decoded at px rows (never above its own size) into 32bpp premultiplied BGRA.
bool decode(const std::wstring& file, UINT px, ArtImage* out) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = false;
    UINT sw = 0, sh = 0;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&sw, &sh)) && sw && sh) {
        UINT h = std::max(1u, std::min(px, sh)), w = std::max(1u, (UINT)lround((double)sw * h / sh));
        IWICBitmapSource* src = fr;
        if (h != sh && SUCCEEDED(wic->CreateBitmapScaler(&sc))) {
            if (SUCCEEDED(sc->Initialize(fr, w, h, (WICBitmapInterpolationMode)4 /* HighQualityCubic */)) ||
                SUCCEEDED(sc->Initialize(fr, w, h, WICBitmapInterpolationModeFant))) src = sc;
            else { w = sw; h = sh; }
        }
        if (SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            out->px.resize((size_t)w * h * 4);
            if (SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)out->px.size(), out->px.data()))) { out->w = w; out->h = h; out->srcH = sh; ok = true; }
        }
    }
    release(cv); release(sc); release(fr); release(dec); release(wic);
    return ok;
}

// .tab img mask: linear-gradient(90deg, transparent, #000 60%) across the image.
void fadeLeft(ArtImage& im) {
    float span = 0.6f * im.w;
    for (UINT x = 0; x < im.w; x++) {
        float f = std::min(1.f, (x + 0.5f) / span);
        if (f >= 1) break;
        for (UINT y = 0; y < im.h; y++) {
            uint8_t* p = &im.px[((size_t)y * im.w + x) * 4];
            for (int k = 0; k < 4; k++) p[k] = (uint8_t)lroundf(p[k] * f);
        }
    }
}

// img.art filter: drop-shadow(0 30px 60px rgba(0,0,0,.55)) at an art height of 760px, baked under the skin once.
// The image grows by the shadow's reach; pad* say where the skin's own box is inside it.
void dropShadow(ArtImage& im, int* padL, int* padT, int* padR, int* padB) {
    float k = im.h / 760.f;
    float sigma = 30 * k;   // CSS blur radius 60px = 2 sigma
    int oy = (int)lroundf(30 * k), reach = (int)ceilf(3 * sigma);
    *padL = *padR = reach; *padT = std::max(0, reach - oy); *padB = reach + oy;
    int W = (int)im.w + *padL + *padR, H = (int)im.h + *padT + *padB;
    std::vector<uint8_t> a((size_t)W * H, 0), tmp((size_t)W * H);
    for (UINT y = 0; y < im.h; y++)
        for (UINT x = 0; x < im.w; x++) a[(size_t)(y + *padT + oy) * W + x + *padL] = im.px[((size_t)y * im.w + x) * 4 + 3];
    int r = std::max(1, (int)lroundf(sigma));
    for (int i = 0; i < 3; i++) boxBlur(a, tmp, W, H, r);   // three boxes ~ a Gaussian of sigma r
    std::vector<uint8_t> o((size_t)W * H * 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t* d = &o[((size_t)y * W + x) * 4];
            int s = (int)lroundf(a[(size_t)y * W + x] * 0.55f);
            int ix = x - *padL, iy = y - *padT;
            if (ix >= 0 && iy >= 0 && ix < (int)im.w && iy < (int)im.h) {
                const uint8_t* p = &im.px[((size_t)iy * im.w + ix) * 4];
                d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
                d[3] = (uint8_t)(p[3] + (255 - p[3]) * s / 255);   // premultiplied black under the skin
            } else { d[0] = d[1] = d[2] = 0; d[3] = (uint8_t)s; }
        }
    im.px.swap(o); im.w = W; im.h = H;
}

} // namespace

struct ArtLoader::Shared {
    struct Req { std::wstring skin; bool thumb; UINT px; };
    CRITICAL_SECTION cs;
    std::vector<Req> queue;
    std::vector<ArtImage> done;
    HWND hwnd = nullptr;
    UINT msg = 0;
    PTP_WORK work = nullptr;
    bool running = false, stop = false;
    int inflight = 0;
    LONG refs = 1;
    Shared() { InitializeCriticalSection(&cs); }
    ~Shared() { DeleteCriticalSection(&cs); }
    void unref() { if (InterlockedDecrement(&refs) == 0) delete this; }

    // Download (when not cached) and decode one image. Any thread.
    static bool process(const Req& r, ArtImage* out) {
        out->skin = r.skin; out->thumb = r.thumb;
        if (!safeId(r.skin)) return false;
        { Guard g(g_failLock); if (g_failed.count(r.skin)) return false; }
        std::wstring dir = cacheDir(), file = dir + L"\\" + r.skin + L".png";
        bool ok = true;
        if (!fileExists(file)) {
            createDirs(dir);
            std::wstring tmp = file + L".tmp", error;
            ok = net::download(url(r.skin), L"RadarniteLauncher/3.0", 15000, tmp, nullptr, &error) &&
                 MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
            if (!ok) DeleteFileW(tmp.c_str());
        }
        if (ok) {
            HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ok = decode(file, r.px, out);
            if (SUCCEEDED(co)) CoUninitialize();
            if (!ok) DeleteFileW(file.c_str());   // a broken file is fetched again next time
        }
        if (!ok) { Guard g(g_failLock); g_failed.insert(r.skin); return false; }
        if (r.thumb) fadeLeft(*out);
        else dropShadow(*out, &out->padL, &out->padT, &out->padR, &out->padB);
        return true;
    }

    static void CALLBACK run(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WORK) {
        auto* s = (Shared*)ctx;
        for (;;) {
            EnterCriticalSection(&s->cs);
            if (s->stop || s->queue.empty()) { s->running = false; LeaveCriticalSection(&s->cs); break; }
            Req r = s->queue.front();
            s->queue.erase(s->queue.begin());
            s->inflight++;
            LeaveCriticalSection(&s->cs);
            ArtImage im;
            bool ok = process(r, &im);
            EnterCriticalSection(&s->cs);
            s->inflight--;
            if (!s->stop) s->done.push_back(std::move(im));   // also a failure (empty pixels): the UI stops waiting
            HWND h = s->hwnd; UINT m = s->msg;
            LeaveCriticalSection(&s->cs);
            (void)ok;
            if (h) PostMessageW(h, m, 0, 0);
        }
        s->unref();
    }
};

ArtLoader::ArtLoader() : s_(new Shared) {}

ArtLoader::~ArtLoader() {
    EnterCriticalSection(&s_->cs);
    s_->stop = true; s_->hwnd = nullptr; s_->queue.clear();
    LeaveCriticalSection(&s_->cs);
    if (s_->work) CloseThreadpoolWork(s_->work);   // freed once a running callback returns; it holds its own reference
    s_->unref();
}

void ArtLoader::startAsync(HWND hwnd, UINT msg) {
    EnterCriticalSection(&s_->cs);
    s_->hwnd = hwnd; s_->msg = msg;
    if (!s_->work) s_->work = CreateThreadpoolWork(Shared::run, s_, nullptr);
    LeaveCriticalSection(&s_->cs);
}

void ArtLoader::request(const std::wstring& skin, bool thumb, UINT px) {
    EnterCriticalSection(&s_->cs);
    bool async = s_->work != nullptr;
    if (!async) {   // offscreen: right here
        LeaveCriticalSection(&s_->cs);
        ArtImage im;
        Shared::process({ skin, thumb, px }, &im);
        EnterCriticalSection(&s_->cs);
        s_->done.push_back(std::move(im));
        LeaveCriticalSection(&s_->cs);
        return;
    }
    bool queued = false;
    for (auto& q : s_->queue) if (q.skin == skin && q.thumb == thumb) { q.px = px; queued = true; }
    if (!queued) s_->queue.push_back({ skin, thumb, px });
    if (!s_->running) {
        s_->running = true;
        InterlockedIncrement(&s_->refs);
        SubmitThreadpoolWork(s_->work);
    }
    LeaveCriticalSection(&s_->cs);
}

bool ArtLoader::pending() const {
    EnterCriticalSection(&s_->cs);
    bool p = !s_->queue.empty() || s_->inflight > 0 || s_->running || !s_->done.empty();
    LeaveCriticalSection(&s_->cs);
    return p;
}

std::vector<ArtImage> ArtLoader::take() {
    std::vector<ArtImage> v;
    EnterCriticalSection(&s_->cs);
    v.swap(s_->done);
    LeaveCriticalSection(&s_->cs);
    return v;
}

bool ArtLoader::failed(const std::wstring& skin) const { Guard g(g_failLock); return g_failed.count(skin) != 0; }

std::wstring ArtLoader::cacheDir() { return paths::launcherDir() + L"\\cache\\art"; }
std::wstring ArtLoader::url(const std::wstring& skin) { return L"https://fortnite-api.com/images/cosmetics/br/" + skin + L"/featured.png"; }

} // namespace rn
