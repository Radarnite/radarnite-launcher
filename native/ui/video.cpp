#include "video.h"
#include <mfapi.h>
#include <mfmediaengine.h>
#include <oleauto.h>
#include <psapi.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>

namespace rn {
namespace {

const UINT WM_CLIP_EVENT = WM_APP + 1;     // clip process: an engine event (its own window)
const UINT WM_CLIP_VISIBLE = WM_APP + 2;   // launcher -> clip window: wParam = show it (else hidden and paused)
const UINT WM_CLIP_CMD = WM_APP + 3;       // launcher -> clip window: wParam = command, lParam = its value
enum : WPARAM { CMD_PAUSE = 1, CMD_SEEK = 2, CMD_VOLUME = 3 };   // lParam: 1 paused / ms / volume per mille | muted << 16
// status (WM_APP_VIDEO wParam 2): bits 0-7 generation, 8 paused, 9-32 position ms, 33-56 length ms
LPARAM packStatus(LPARAM gen, bool paused, double pos, double dur) {
    auto ms = [](double s) -> uint64_t { return s > 0 && s < 16000 ? (uint64_t)(s * 1000) : 0; };
    return (LPARAM)(((uint64_t)gen & 255) | ((uint64_t)paused << 8) | (ms(pos) << 9) | (ms(dur) << 33));
}
const wchar_t* kClass = L"RnClip";

// ================================================================================================ launcher side
// One job for every clip process: closing the launcher (its last handle) kills them.
HANDLE clipJob() {
    static HANDLE job = nullptr;
    if (!job) {
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    return job;
}

} // namespace

bool Video::play(HWND parent, const std::string& url, RECT r, int radius, uint32_t border, float volume, bool muted) {
    stop();
    url_ = url;
    gen_++;
    if (url.compare(0, 8, "https://") || url.find_first_of("\" ") != std::string::npos) { failed_ = true; return false; }
    parent_ = parent; r_ = r;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    paused_ = false; status_ = false; pos_ = dur_ = 0;
    wchar_t args[200];
    swprintf(args, 200, L" --clip %llu %ld %ld %ld %ld %d %u %lld %d %d ", (unsigned long long)(UINT_PTR)parent, r.left, r.top,
             r.right - r.left, r.bottom - r.top, radius, border, (long long)gen_, (int)lroundf(std::max(0.f, std::min(1.f, volume)) * 1000), muted ? 1 : 0);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"" + args + std::wstring(url.begin(), url.end());
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
        failed_ = true;
        return false;
    }
    HANDLE job = clipJob();
    if (!job || !AssignProcessToJobObject(job, pi.hProcess)) {   // never one that could outlive the launcher
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        failed_ = true;
        return false;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    return true;
}

void Video::place(RECT r, bool visible) {
    if (!proc_) return;
    if (WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) {   // it could not play (or died): the image stays
        CloseHandle(proc_); proc_ = nullptr; child_ = nullptr; failed_ = true;
        return;
    }
    if (!child_) { r_ = r; visible_ = visible; return; }   // applied when its window is up (onEvent)
    if (memcmp(&r, &r_, sizeof r)) {
        r_ = r;
        SetWindowPos(child_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    if (visible != visible_) { visible_ = visible; PostMessageW(child_, WM_CLIP_VISIBLE, visible, 0); }
}

void Video::onEvent(WPARAM ev, LPARAM gen) {
    if (ev == 2) {   // status
        uint64_t v = (uint64_t)gen;
        if ((v & 255) != ((uint64_t)gen_ & 255) || !proc_) return;
        paused_ = (v >> 8) & 1;
        pos_ = ((v >> 9) & 0xFFFFFF) / 1000.0;
        dur_ = ((v >> 33) & 0xFFFFFF) / 1000.0;
        status_ = true;
        return;
    }
    if (gen != gen_ || !proc_ || child_) return;
    child_ = FindWindowExW(parent_, nullptr, kClass, nullptr);
    if (!child_) return;
    RECT r = r_; r_ = {};
    bool v = visible_; visible_ = true;
    place(r, v);
}

bool Video::shown() const { return child_ && IsWindowVisible(child_); }

void Video::send(WPARAM cmd, LPARAM v) {
    HWND c = child_ ? child_ : (proc_ && parent_ ? FindWindowExW(parent_, nullptr, kClass, nullptr) : nullptr);
    if (c) PostMessageW(c, WM_CLIP_CMD, cmd, v);
}
void Video::setPaused(bool p) { paused_ = p; send(CMD_PAUSE, p ? 1 : 0); }
void Video::seek(double s) { pos_ = std::max(0.0, s); send(CMD_SEEK, (LPARAM)(pos_ * 1000)); }
void Video::setVolume(float v, bool muted) { send(CMD_VOLUME, (LPARAM)lroundf(std::max(0.f, std::min(1.f, v)) * 1000) | (muted ? 1 << 16 : 0)); }

size_t Video::processBytes() const {
    PROCESS_MEMORY_COUNTERS_EX m = { sizeof m };
    return proc_ && GetProcessMemoryInfo(proc_, (PROCESS_MEMORY_COUNTERS*)&m, sizeof m) ? m.PrivateUsage : 0;
}

void Video::stop() {
    if (proc_) {
        TerminateProcess(proc_, 0);
        WaitForSingleObject(proc_, 3000);   // its window goes with it
        CloseHandle(proc_);
        proc_ = nullptr;
    }
    child_ = nullptr;
    url_.clear();
    failed_ = false;
    visible_ = true;
    paused_ = false; status_ = false; pos_ = dur_ = 0;
    gen_++;
}

// ================================================================================================ clip process
namespace {

// The attribute GUIDs (mfmediaengine.h only declares them)
const GUID kCallback = { 0xc60381b8, 0x83a4, 0x41f8, { 0xa3, 0xd0, 0xde, 0x05, 0x07, 0x68, 0x49, 0xa9 } };
const GUID kPlaybackHwnd = { 0xd988879b, 0x67c9, 0x4d92, { 0xba, 0xa7, 0x6e, 0xad, 0xd4, 0x46, 0x03, 0x9d } };
const GUID kOutputFormat = { 0x5066893c, 0x8cf9, 0x42bc, { 0x8b, 0x8a, 0x47, 0x22, 0x12, 0xe5, 0x27, 0x26 } };
const CLSID kFactory = { 0xb44392da, 0x499b, 0x446b, { 0xa4, 0xcb, 0x00, 0x5f, 0xea, 0xd0, 0xe6, 0xd5 } };

typedef HRESULT(WINAPI* StartupFn)(ULONG, DWORD);
typedef HRESULT(WINAPI* AttrsFn)(IMFAttributes**, UINT32);

// The engine's events (a Media Foundation thread), posted to the clip window.
struct Notify final : IMFMediaEngineNotify {
    LONG refs = 1;
    HWND hwnd = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** p) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEngineNotify)) { *p = (IMFMediaEngineNotify*)this; AddRef(); return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return (ULONG)r; }
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR, DWORD) override {
        if (event == MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY || event == MF_MEDIA_ENGINE_EVENT_PLAYING || event == MF_MEDIA_ENGINE_EVENT_ERROR ||
            event == MF_MEDIA_ENGINE_EVENT_PAUSE || event == MF_MEDIA_ENGINE_EVENT_SEEKED || event == MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE)
            PostMessageW(hwnd, WM_CLIP_EVENT, event, 0);
        return S_OK;
    }
};

struct Clip {
    HWND parent = nullptr, hwnd = nullptr;
    IMFMediaEngine* engine = nullptr;
    int radius = 0;
    MFARGB border = {};
    LPARAM gen = 0;
    bool ready = false, visible = true, paused = false, silent = false;
    void apply() {
        bool show = ready && visible;
        if (show && !paused && engine) engine->Play(); else if (engine) engine->Pause();
        ShowWindow(hwnd, show ? SW_SHOWNA : SW_HIDE);
        // the position 4 times a second while it plays; once more when it stops
        if (show && !paused) SetTimer(hwnd, 2, 250, nullptr); else KillTimer(hwnd, 2);
        status();
    }
    void status() {
        if (!engine || !ready) return;
        double d = engine->GetDuration();
        PostMessageW(parent, WM_APP_VIDEO, 2, packStatus(gen, paused || !visible, engine->GetCurrentTime(), d == d ? d : 0));
    }
    void volume(LPARAM v) {   // per mille in the low 16 bits, bit 16 = muted
        if (!engine) return;
        engine->SetMuted(silent || (v >> 16) ? TRUE : FALSE);
        engine->SetVolume(silent ? 0 : std::min<LPARAM>(1000, v & 0xFFFF) / 1000.0);
    }
    // object-fit: cover when it only trims a little (no bands above and below), else contain on the border color
    void fit() {
        IMFMediaEngineEx* ex = nullptr;
        if (!engine || FAILED(engine->QueryInterface(__uuidof(IMFMediaEngineEx), (void**)&ex))) return;
        RECT c; GetClientRect(hwnd, &c);
        DWORD vw = 0, vh = 0;
        MFVideoNormalizedRect src = { 0, 0, 1, 1 };
        if (SUCCEEDED(engine->GetNativeVideoSize(&vw, &vh)) && vw && vh && c.right > 0 && c.bottom > 0) {
            float k = ((float)c.right / c.bottom) / ((float)vw / vh);   // < 1: the clip is wider than the area
            if (k < 1 && k >= .8f) { src.left = (1 - k) / 2; src.right = 1 - src.left; }
            else if (k > 1 && 1 / k >= .8f) { src.top = (1 - 1 / k) / 2; src.bottom = 1 - src.top; }
        }
        ex->UpdateVideoStream(&src, nullptr, &border);
        ex->Release();
    }
    void region() {
        // the preview's left corners are rounded: so is the clip
        RECT c; GetClientRect(hwnd, &c);
        if (radius > 0) SetWindowRgn(hwnd, CreateRoundRectRgn(0, 0, c.right + 1 + radius * 2, c.bottom + 1, radius * 2, radius * 2), TRUE);
    }
};
Clip g_clip;

// The mouse over the clip belongs to the launcher's window (a click toggles pause, the wheel scrolls): HTTRANSPARENT
// only reaches windows of the same thread, so the messages are posted on, in the parent's client coordinates.
LRESULT CALLBACK clipProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        MapWindowPoints(h, g_clip.parent, &p, 1);
        PostMessageW(g_clip.parent, m == WM_LBUTTONDBLCLK ? WM_LBUTTONDOWN : m, w, MAKELPARAM(p.x, p.y));
        return 0;
    }
    case WM_MOUSEWHEEL: PostMessageW(g_clip.parent, m, w, l); return 0;   // screen coordinates already
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_SIZE: g_clip.region(); if (g_clip.ready) g_clip.fit(); return 0;
    case WM_TIMER:
        if (w == 2) { g_clip.status(); return 0; }
        if (!IsWindow(g_clip.parent)) DestroyWindow(h);   // the launcher's window is gone
        return 0;
    case WM_CLIP_VISIBLE: g_clip.visible = w != 0; g_clip.apply(); return 0;
    case WM_CLIP_CMD:
        if (w == CMD_PAUSE) { g_clip.paused = l != 0; g_clip.apply(); }
        else if (w == CMD_SEEK && g_clip.engine) { g_clip.engine->SetCurrentTime(l / 1000.0); g_clip.status(); }
        else if (w == CMD_VOLUME) g_clip.volume(l);
        return 0;
    case WM_CLIP_EVENT:
        if (w == MF_MEDIA_ENGINE_EVENT_ERROR) { DestroyWindow(h); return 0; }   // the launcher sees the exit: image stays
        if (!g_clip.ready) {
            g_clip.ready = true;
            g_clip.fit();
            g_clip.apply();
            PostMessageW(g_clip.parent, WM_APP_VIDEO, 1, g_clip.gen);
        } else g_clip.status();
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int clipMain(int argc, wchar_t** argv) {
    if (argc < 13) return 2;
    Clip& c = g_clip;
    c.parent = (HWND)(UINT_PTR)_wcstoui64(argv[2], nullptr, 10);
    int x = _wtoi(argv[3]), y = _wtoi(argv[4]), w = _wtoi(argv[5]), h = _wtoi(argv[6]);
    c.radius = _wtoi(argv[7]);
    uint32_t border = (uint32_t)wcstoul(argv[8], nullptr, 10);
    c.border = { (BYTE)(border & 255), (BYTE)((border >> 8) & 255), (BYTE)((border >> 16) & 255), 255 };
    c.gen = (LPARAM)_wtoi64(argv[9]);
    int vol = _wtoi(argv[10]);
    bool muted = _wtoi(argv[11]) != 0;
    wchar_t sil[8] = {};
    c.silent = GetEnvironmentVariableW(L"RN_CLIP_SILENT", sil, 8) && sil[0] == L'1';
    std::wstring url = argv[12];
    if (!IsWindow(c.parent) || url.compare(0, 8, L"https://")) return 3;

    HMODULE mf = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto startup = mf ? (StartupFn)(void*)GetProcAddress(mf, "MFStartup") : nullptr;
    auto attrsFn = mf ? (AttrsFn)(void*)GetProcAddress(mf, "MFCreateAttributes") : nullptr;
    if (!startup || !attrsFn || FAILED(startup(MF_VERSION, MFSTARTUP_FULL))) return 4;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = clipProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    // a child of the launcher's window (hidden until the first frame): never activated, never in the taskbar
    c.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, x, y, w, h, c.parent, nullptr, wc.hInstance, nullptr);
    if (!c.hwnd) return 5;
    c.region();
    SetTimer(c.hwnd, 1, 1000, nullptr);

    Notify* n = new Notify();
    n->hwnd = c.hwnd;
    IMFMediaEngineClassFactory* f = nullptr;
    IMFAttributes* a = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(kFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFMediaEngineClassFactory), (void**)&f)) &&
              SUCCEEDED(attrsFn(&a, 3)) && SUCCEEDED(a->SetUnknown(kCallback, (IMFMediaEngineNotify*)n)) &&
              SUCCEEDED(a->SetUINT64(kPlaybackHwnd, (UINT64)(UINT_PTR)c.hwnd)) && SUCCEEDED(a->SetUINT32(kOutputFormat, 87 /*DXGI_FORMAT_B8G8R8A8_UNORM*/)) &&
              SUCCEEDED(f->CreateInstance(0, a, &c.engine));
    if (a) a->Release();
    if (f) f->Release();
    if (ok) {
        // like the site's <video autoplay loop>: sound at the saved volume (RN_CLIP_SILENT: never)
        c.volume(vol | (muted ? 1 << 16 : 0));
        c.engine->SetLoop(TRUE);
        c.engine->SetAutoPlay(TRUE);
        c.fit();
        BSTR s = SysAllocString(url.c_str());
        ok = s && SUCCEEDED(c.engine->SetSource(s));
        SysFreeString(s);
    }
    if (ok) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    } else if (IsWindow(c.hwnd)) DestroyWindow(c.hwnd);
    if (c.engine) { c.engine->Shutdown(); c.engine->Release(); }
    n->Release();
    return ok ? 0 : 6;
}

} // namespace rn
