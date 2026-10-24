#include "images.h"
#include "gfx.h"
#include "../core/net.h"
#include <wincodec.h>
#include <math.h>
#include <stdio.h>
#include <algorithm>

namespace rn {

namespace {
const int kMaxWorkers = 4;

// FNV-1a 64 of the URL: the disk cache file name.
std::wstring fileFor(const std::wstring& dir, const std::string& url) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : url) { h ^= c; h *= 1099511628211ull; }
    wchar_t b[32];
    swprintf(b, 32, L"%016llx.img", (unsigned long long)h);
    return dir + L"\\" + b;
}

// Keeps the disk cache under ~256 MB: the oldest files go first (one pass when the window starts using it).
DWORD WINAPI pruneProc(LPVOID p) {
    std::wstring* dir = (std::wstring*)p;
    struct F { std::wstring name; uint64_t t, size; };
    std::vector<F> files;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((*dir + L"\\*.img").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            uint64_t s = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            files.push_back({ fd.cFileName, ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime, s });
            total += s;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (total > (256ull << 20)) {
        std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t < b.t; });
        for (auto& f : files) {
            if (total <= (192ull << 20)) break;
            if (DeleteFileW((*dir + L"\\" + f.name).c_str())) total -= f.size;
        }
    }
    delete dir;
    return 0;
}
} // namespace

struct Images::Job {
    std::string url, key;
    UINT w, h;
    Fit fit;
    uint32_t frame, gen;
    std::wstring dir;
    // result
    bool failed = false;
    UINT pw = 0, ph = 0;
    std::vector<uint8_t> px;
};

Images::Images() { wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

Images::~Images() {
    AcquireSRWLockExclusive(&lock_);
    quit_ = true;
    for (auto* j : queue_) delete j;
    queue_.clear();
    ReleaseSRWLockExclusive(&lock_);
    SetEvent(wake_);
    // a worker finishes the image it is on (a download times out after 15 s) and exits
    for (int i = 0; i < 400; i++) {
        AcquireSRWLockExclusive(&lock_);
        int w = workers_;
        ReleaseSRWLockExclusive(&lock_);
        if (!w) break;
        SetEvent(wake_);
        Sleep(50);
    }
    for (auto* j : done_) delete j;
    for (auto& e : lru_) e.bmp->Release();
    CloseHandle(wake_);
}

void Images::setDir(const std::wstring& dir) {
    if (dir == dir_) return;
    dir_ = dir;
    CreateDirectoryW(dir_.c_str(), nullptr);
    HANDLE t = CreateThread(nullptr, 0, pruneProc, new std::wstring(dir_), 0, nullptr);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_LOWEST); CloseHandle(t); }
}

void Images::setNotify(HWND h, UINT msg) { notify_ = h; msg_ = msg; }

void Images::setTarget(ID2D1RenderTarget* rt) {
    if (rt == rt_) return;
    rt_ = rt;
    for (auto& e : lru_) e.bmp->Release();
    lru_.clear();
    bytes_ = 0;
    AcquireSRWLockExclusive(&lock_);
    gen_++;
    for (auto* j : done_) delete j;
    done_.clear();
    for (auto* j : queue_) delete j;
    queue_.clear();
    busy_.clear();   // decodes in flight come back with the old generation and are dropped
    ReleaseSRWLockExclusive(&lock_);
    failed_.clear();
}

std::string Images::keyOf(const std::string& url, UINT w, UINT h, Fit fit) const {
    char b[48];
    snprintf(b, sizeof b, "|%u|%u|%d", w, h, (int)fit);
    return url + b;
}

// Bitmaps not drawn in the frame that just ended go first, least recently drawn first.
void Images::evict() {
    while (bytes_ > budget_) {
        size_t victim = (size_t)-1;
        for (size_t i = 0; i < lru_.size(); i++)
            if (lru_[i].used < frame_ && (victim == (size_t)-1 || lru_[i].used < lru_[victim].used)) victim = i;
        if (victim == (size_t)-1) break;
        bytes_ -= lru_[victim].bytes;
        lru_[victim].bmp->Release();
        lru_.erase(lru_.begin() + victim);
    }
}

void Images::frame() {
    evict();
    frame_++;
    // requests the last frame did not repeat scrolled out of view: dropped before a worker spends time on them
    AcquireSRWLockExclusive(&lock_);
    for (auto it = queue_.begin(); it != queue_.end();) {
        if ((*it)->frame + 1 < frame_) {
            busy_.erase(std::find(busy_.begin(), busy_.end(), (*it)->key));
            delete *it;
            it = queue_.erase(it);
        } else ++it;
    }
    ReleaseSRWLockExclusive(&lock_);
}

ID2D1Bitmap* Images::get(const std::string& url, UINT w, UINT h, Fit fit, bool* failed) {
    if (failed) *failed = false;
    if (url.compare(0, 8, "https://") || !w || !h || !rt_) { if (failed) *failed = url.empty() || url.compare(0, 8, "https://"); return nullptr; }
    std::string key = keyOf(url, w, h, fit);
    for (auto& e : lru_) if (e.key == key) { e.used = frame_; return e.bmp; }
    for (auto& f : failed_) if (f == key) { if (failed) *failed = true; return nullptr; }
    AcquireSRWLockExclusive(&lock_);
    bool queued = false;
    if (std::find(busy_.begin(), busy_.end(), key) != busy_.end()) {
        for (auto* j : queue_) if (j->key == key) { j->frame = frame_; break; }
        queued = true;
    }
    if (!queued) {
        auto* j = new Job{ url, key, w, h, fit, frame_, gen_, dir_ };
        queue_.push_front(j);
        busy_.push_back(key);
        if (workers_ < kMaxWorkers && workers_ < (int)queue_.size() + active_) {
            HANDLE t = CreateThread(nullptr, 0, worker, this, 0, nullptr);
            if (t) { workers_++; SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
        }
    }
    ReleaseSRWLockExclusive(&lock_);
    SetEvent(wake_);
    return nullptr;
}

int Images::drain() {
    std::vector<Job*> got;
    AcquireSRWLockExclusive(&lock_);
    got.swap(done_);
    uint32_t gen = gen_;
    for (auto* j : got) if (j->gen == gen) { auto it = std::find(busy_.begin(), busy_.end(), j->key); if (it != busy_.end()) busy_.erase(it); }
    ReleaseSRWLockExclusive(&lock_);
    int n = 0;
    for (auto* j : got) {
        if (j->gen != gen || !rt_) { delete j; continue; }
        if (j->failed || j->px.empty()) failed_.push_back(j->key);
        else {
            ID2D1Bitmap* b = nullptr;
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            if (SUCCEEDED(rt_->CreateBitmap(D2D1::SizeU(j->pw, j->ph), j->px.data(), j->pw * 4, bp, &b))) {
                size_t bytes = (size_t)j->pw * j->ph * 4;
                lru_.push_back({ j->key, b, bytes, frame_ });
                bytes_ += bytes;
                n++;
            } else failed_.push_back(j->key);
        }
        delete j;
    }
    return n;
}

bool Images::idle() {
    AcquireSRWLockExclusive(&lock_);
    bool i = queue_.empty() && active_ == 0 && done_.empty();
    ReleaseSRWLockExclusive(&lock_);
    return i;
}

int Images::threads() {
    AcquireSRWLockExclusive(&lock_);
    int w = workers_;
    ReleaseSRWLockExclusive(&lock_);
    return w;
}

DWORD WINAPI Images::worker(LPVOID self) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ((Images*)self)->run();
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

void Images::run() {
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    for (;;) {
        Job* j = nullptr;
        AcquireSRWLockExclusive(&lock_);
        if (!quit_ && !queue_.empty()) { j = queue_.front(); queue_.pop_front(); active_++; }
        bool quit = quit_;
        ReleaseSRWLockExclusive(&lock_);
        if (!j) {
            // idle: wait a little for more, then exit (no thread stays at rest)
            DWORD w = quit ? WAIT_TIMEOUT : WaitForSingleObject(wake_, 3000);
            AcquireSRWLockExclusive(&lock_);
            bool stop = quit_ || (w != WAIT_OBJECT_0 && queue_.empty());
            if (stop) workers_--;
            ReleaseSRWLockExclusive(&lock_);
            if (stop) break;
            continue;
        }
        // 1) the disk cache, else the network
        std::wstring file = fileFor(j->dir, j->url);
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::wstring wurl(j->url.begin(), j->url.end());
            net::Response r = net::get(wurl, L"RadarniteLauncher", nullptr, 15000);
            if (r.ok() && !r.body.empty()) {
                std::wstring tmp = file + L".part";
                HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                DWORD wr = 0;
                bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, r.body.data(), (DWORD)r.body.size(), &wr, nullptr) && wr == r.body.size();
                if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
                if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
            }
        }
        // 2) decode, scale straight to the drawn size and crop to what shows
        j->failed = true;
        IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICBitmapScaler* sc = nullptr;
        IWICBitmapClipper* cl = nullptr; IWICFormatConverter* cv = nullptr;
        UINT iw = 0, ih = 0;
        if (wic && SUCCEEDED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&iw, &ih)) && iw && ih) {
            double s = j->fit == Cover ? std::max((double)j->w / iw, (double)j->h / ih)
                     : j->fit == Featured ? 1.07 * j->h / ih
                     : std::min((double)j->w / iw, (double)j->h / ih);
            UINT sw = std::max(1u, (UINT)lround(iw * s)), sh = std::max(1u, (UINT)lround(ih * s));
            WICRect clip = { 0, 0, (INT)sw, (INT)sh };
            if (j->fit != Contain) {
                clip.Width = (INT)std::min(sw, j->w);
                clip.Height = j->fit == Featured ? (INT)sh : (INT)std::min(sh, j->h);   // featured: the whole 1.07 h
                clip.X = ((INT)sw - clip.Width) / 2;
            }
            IWICBitmapSource* src = nullptr;
            if (SUCCEEDED(wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr, sw, sh, WICBitmapInterpolationModeFant))) {
                src = sc;
                if (clip.Width != (INT)sw || clip.Height != (INT)sh) {
                    if (SUCCEEDED(wic->CreateBitmapClipper(&cl)) && SUCCEEDED(cl->Initialize(sc, &clip))) src = cl; else src = nullptr;
                }
            }
            if (src && SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
                SUCCEEDED(cv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                j->pw = (UINT)clip.Width; j->ph = (UINT)clip.Height;
                j->px.resize((size_t)j->pw * j->ph * 4);
                if (SUCCEEDED(cv->CopyPixels(nullptr, j->pw * 4, (UINT)j->px.size(), j->px.data()))) j->failed = false;
                else std::vector<uint8_t>().swap(j->px);
                // .art's mask: opaque to 66 % of its height, transparent from 90 % (premultiplied: every channel)
                if (!j->failed && j->fit == Featured)
                    for (UINT y = 0; y < j->ph; y++) {
                        float t = (float)y / j->ph, a = t <= .66f ? 1.f : t >= .90f ? 0.f : 1.f - (t - .66f) / .24f;
                        if (a >= 1.f) continue;
                        uint8_t* row = &j->px[(size_t)y * j->pw * 4];
                        for (UINT k = 0; k < j->pw * 4; k++) row[k] = (uint8_t)(row[k] * a + .5f);
                    }
            }
        }
        release(cv); release(cl); release(sc); release(fr); release(dec);
        AcquireSRWLockExclusive(&lock_);
        active_--;
        bool post = done_.empty();
        if (quit_) delete j; else done_.push_back(j);
        HWND n = notify_;
        UINT msg = msg_;
        ReleaseSRWLockExclusive(&lock_);
        if (post && n) PostMessageW(n, msg, 0, 0);
    }
    release(wic);
}

} // namespace rn
