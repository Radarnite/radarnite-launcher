// Season art of the main screen: per-season colors and battle-pass skin, and the loader of the skin's featured.png
// (the cosmetics API's CDN, cached in <launcher>\cache\art, decoded with WIC at the size it is drawn).
// Async mode (a window): one thread-pool work item downloads/decodes and posts WM_APP_SINK; the UI thread takes the
// pixels and makes the Direct2D bitmaps. Sync mode (offscreen renders of the selftest): done inside request().
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace rn {

struct SeasonArt {
    const wchar_t* name;    // "Nexus War" ("" when it has none)
    uint32_t c[4];          // radial gradient: center, 30 %, 55 %, edge
    const wchar_t* skin;    // cosmetic id of the skin (nullptr: no art)
};
// The season of a build version (chapter 1 season 1 = 1, ..., 1:1 with the major after 2.x); the neutral entry when
// the version is unknown.
const SeasonArt& seasonArt(int major, int minor);

struct ArtImage {
    std::wstring skin;
    bool thumb = false;     // tab thumbnail (left part faded out), else the full art
    UINT w = 0, h = 0, srcH = 0;   // decoded size; srcH = the file's own height
    int padL = 0, padT = 0, padR = 0, padB = 0;   // full art: the baked drop shadow around the skin's own box (px)
    std::vector<uint8_t> px;       // 32bpp premultiplied BGRA, w*4 per row
};

class ArtLoader {
public:
    ArtLoader();
    ~ArtLoader();
    ArtLoader(const ArtLoader&) = delete;
    ArtLoader& operator=(const ArtLoader&) = delete;

    // Async from now on: results are announced with PostMessage(hwnd, msg).
    void startAsync(HWND hwnd, UINT msg);
    // Decode `skin` at `px` pixels tall (thumb: with the fade). A newer request for the same image replaces a queued one.
    void request(const std::wstring& skin, bool thumb, UINT px);
    bool pending() const;               // something queued or in flight
    std::vector<ArtImage> take();       // finished images (UI thread)
    bool failed(const std::wstring& skin) const;   // download or decode failed in this process (not retried)

    static std::wstring cacheDir();
    static std::wstring url(const std::wstring& skin);

private:
    struct Shared;
    Shared* s_;
};

} // namespace rn
