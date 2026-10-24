#include "video.h"
#include <mfapi.h>
#include <mfmediaengine.h>
#include <oleauto.h>

namespace rn {
namespace {

// The attribute GUIDs (mfmediaengine.h only declares them)
const GUID kCallback = { 0xc60381b8, 0x83a4, 0x41f8, { 0xa3, 0xd0, 0xde, 0x05, 0x07, 0x68, 0x49, 0xa9 } };
const GUID kPlaybackHwnd = { 0xd988879b, 0x67c9, 0x4d92, { 0xba, 0xa7, 0x6e, 0xad, 0xd4, 0x46, 0x03, 0x9d } };
const GUID kOutputFormat = { 0x5066893c, 0x8cf9, 0x42bc, { 0x8b, 0x8a, 0x47, 0x22, 0x12, 0xe5, 0x27, 0x26 } };
const CLSID kFactory = { 0xb44392da, 0x499b, 0x446b, { 0xa4, 0xcb, 0x00, 0x5f, 0xea, 0xd0, 0xe6, 0xd5 } };

// Media Foundation, loaded on the first video
typedef HRESULT(WINAPI* StartupFn)(ULONG, DWORD);
typedef HRESULT(WINAPI* AttrsFn)(IMFAttributes**, UINT32);
AttrsFn g_attrs = nullptr;
bool g_tried = false;
bool loadMf() {
    if (g_tried) return g_attrs != nullptr;
    g_tried = true;
    HMODULE m = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return false;
    auto startup = (StartupFn)(void*)GetProcAddress(m, "MFStartup");
    auto attrs = (AttrsFn)(void*)GetProcAddress(m, "MFCreateAttributes");
    if (!startup || !attrs || FAILED(startup(MF_VERSION, MFSTARTUP_FULL))) return false;
    g_attrs = attrs;
    return true;
}

// The video's window: the engine draws into it; the mouse goes through to the window under it
LRESULT CALLBACK videoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

// The engine's events (a Media Foundation thread): posted to the window, which applies them on its own thread.
struct Video::Notify final : IMFMediaEngineNotify {
    LONG refs = 1;
    HWND hwnd;
    LPARAM gen;
    Notify(HWND h, LPARAM g) : hwnd(h), gen(g) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** p) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEngineNotify)) { *p = (IMFMediaEngineNotify*)this; AddRef(); return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return (ULONG)r; }
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR, DWORD) override {
        if (event == MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY || event == MF_MEDIA_ENGINE_EVENT_PLAYING || event == MF_MEDIA_ENGINE_EVENT_ERROR)
            PostMessageW(hwnd, WM_APP_VIDEO, event, gen);
        return S_OK;
    }
};

bool Video::play(HWND parent, const std::string& url, RECT r, int radius, uint32_t border) {
    stop();
    url_ = url;
    if (url.compare(0, 8, "https://") || !loadMf()) { failed_ = true; return false; }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = videoProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"RnVideo";
        RegisterClassW(&wc);
        registered = true;
    }
    parent_ = parent; r_ = r; radius_ = radius; gen_++;
    hwnd_ = CreateWindowExW(0, L"RnVideo", L"", WS_CHILD | WS_CLIPSIBLINGS, r.left, r.top, r.right - r.left, r.bottom - r.top,
                            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    IMFMediaEngineClassFactory* f = nullptr;
    IMFAttributes* a = nullptr;
    notify_ = new Notify(parent, gen_);
    bool ok = hwnd_ && SUCCEEDED(CoCreateInstance(kFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFMediaEngineClassFactory), (void**)&f)) &&
              SUCCEEDED(g_attrs(&a, 3)) && SUCCEEDED(a->SetUnknown(kCallback, (IMFMediaEngineNotify*)notify_)) &&
              SUCCEEDED(a->SetUINT64(kPlaybackHwnd, (UINT64)(UINT_PTR)hwnd_)) && SUCCEEDED(a->SetUINT32(kOutputFormat, 87 /*DXGI_FORMAT_B8G8R8A8_UNORM*/)) &&
              SUCCEEDED(f->CreateInstance(0, a, &engine_));
    if (a) a->Release();
    if (f) f->Release();
    if (ok) {
        // no sound ever, loops like the page's <video muted loop autoplay>
        engine_->SetMuted(TRUE);
        engine_->SetLoop(TRUE);
        engine_->SetAutoPlay(TRUE);
        IMFMediaEngineEx* ex = nullptr;
        if (SUCCEEDED(engine_->QueryInterface(__uuidof(IMFMediaEngineEx), (void**)&ex))) {
            MFARGB b = { (BYTE)(border & 255), (BYTE)((border >> 8) & 255), (BYTE)((border >> 16) & 255), 255 };
            ex->UpdateVideoStream(nullptr, nullptr, &b);
            ex->Release();
        }
        std::wstring w(url.begin(), url.end());
        BSTR s = SysAllocString(w.c_str());
        ok = s && SUCCEEDED(engine_->SetSource(s));
        SysFreeString(s);
    }
    if (!ok) { stop(); url_ = url; failed_ = true; return false; }
    return true;
}

void Video::place(RECT r, bool visible) {
    if (!hwnd_) return;
    bool moved = memcmp(&r, &r_, sizeof r) != 0;
    if (moved) {
        r_ = r;
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (moved || visible != visible_ || (ready_ && !shown_ && visible)) {
        visible_ = visible;
        bool show = visible && ready_;
        if (show && radius_ > 0) {
            // the preview's left corners are rounded: so is the video
            int w = r.right - r.left, h = r.bottom - r.top;
            HRGN g = CreateRoundRectRgn(0, 0, w + 1 + radius_ * 2, h + 1, radius_ * 2, radius_ * 2);
            SetWindowRgn(hwnd_, g, FALSE);
        }
        ShowWindow(hwnd_, show ? SW_SHOWNA : SW_HIDE);
        shown_ = show;
        if (engine_) { if (visible) engine_->Play(); else engine_->Pause(); }
    }
}

void Video::onEvent(WPARAM event, LPARAM gen) {
    if (gen != gen_ || !engine_) return;
    if (event == MF_MEDIA_ENGINE_EVENT_ERROR) { RECT r = r_; std::string u = url_; stop(); url_ = u; r_ = r; failed_ = true; return; }
    if (!ready_) { ready_ = true; RECT r = r_; r_ = {}; place(r, visible_); }
}

void Video::stop() {
    if (engine_) { engine_->Shutdown(); engine_->Release(); engine_ = nullptr; }
    if (notify_) { notify_->Release(); notify_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    url_.clear();
    ready_ = shown_ = failed_ = false;
    visible_ = true;
}

} // namespace rn
