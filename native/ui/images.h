// Card images of the cosmetics panel: downloaded once (WinHTTP, https only) into a disk cache, decoded and scaled on a
// few worker threads straight to the size they are drawn at (WIC), and kept as Direct2D bitmaps in a small LRU bounded
// in bytes. Only what the current frame asks for is loaded: a request that scrolled out of view before a worker took
// it is dropped. Nothing runs at rest: the workers exit when the queue stays empty.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <stdint.h>
#include <deque>
#include <string>
#include <vector>

namespace rn {

class Images {
public:
    enum Fit : uint8_t {
        Cover,       // fills w x h, cropped around the horizontal centre, top aligned (object-fit: cover; center top)
        Featured,    // scaled to 1.07 h, centred, cropped to w wide, faded out from 66 % to 90 % of its height (the
                     // card's .art: top -7 %, its mask); drawn from 0.07 h above the card
        Contain,     // fits inside w x h (the preview's media)
    };
    Images();
    ~Images();
    void setDir(const std::wstring& dir);        // disk cache folder (created on first download)
    const std::wstring& dir() const { return dir_; }
    void setNotify(HWND h, UINT msg);            // posted when a decode is ready (offscreen: none, drain() polls)
    void setBudget(size_t bytes) { budget_ = bytes; }
    // Render target change: every bitmap belongs to the old one, so all are dropped (decoded again from disk).
    void setTarget(ID2D1RenderTarget* rt);
    void frame();                                 // once per paint, before the first get()
    // The bitmap of url fitted to w x h device pixels, or null while it loads (requested now). failed = it cannot load.
    ID2D1Bitmap* get(const std::string& url, UINT w, UINT h, Fit fit, bool* failed = nullptr);
    // Turns finished decodes into bitmaps (UI thread, with a target set). Returns how many arrived.
    int drain();
    bool idle();                                  // nothing queued, nothing being decoded
    size_t bytes() const { return bytes_; }       // decoded bitmaps held
    size_t count() const { return lru_.size(); }
    int threads();

    struct Job;
private:
    struct Entry { std::string key; ID2D1Bitmap* bmp; size_t bytes; uint32_t used; };
    static DWORD WINAPI worker(LPVOID self);
    void run();
    void evict();
    std::string keyOf(const std::string& url, UINT w, UINT h, Fit fit) const;

    ID2D1RenderTarget* rt_ = nullptr;
    std::wstring dir_;
    HWND notify_ = nullptr;
    UINT msg_ = 0;
    size_t budget_ = 12u << 20, bytes_ = 0;
    uint32_t frame_ = 0;
    std::vector<Entry> lru_;
    std::vector<std::string> failed_;             // keys that cannot load (not asked again this session)
    SRWLOCK lock_ = SRWLOCK_INIT;                 // the fields below
    std::deque<Job*> queue_;                      // newest request first
    std::vector<Job*> done_;
    std::vector<std::string> busy_;               // keys being decoded or queued
    int workers_ = 0, active_ = 0;
    HANDLE wake_ = nullptr;
    bool quit_ = false;
    uint32_t gen_ = 0;                            // bumped by setTarget: decodes for an older target are dropped
};

} // namespace rn
