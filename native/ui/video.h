// The preview's video (radarnite.com's item card plays fortnite.gg's clip): Media Foundation's Media Engine in
// rendering mode on a child window over the preview's media area, muted and looped. Media Foundation is loaded on the
// first video only (the window starts without it), and everything goes when the preview closes.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>

struct IMFMediaEngine;

namespace rn {

const UINT WM_APP_VIDEO = WM_APP + 4;   // an engine event (wParam = MF_MEDIA_ENGINE_EVENT, lParam = play generation)

class Video {
public:
    ~Video() { stop(); }
    // Plays url (https only) in a child window of parent at r (client pixels, left corners rounded by radius px),
    // letterboxed on border (0xRRGGBB). The window shows once the first frame is ready; false = it cannot play here.
    bool play(HWND parent, const std::string& url, RECT r, int radius, uint32_t border);
    void place(RECT r, bool visible);   // every paint: follows the preview, hidden under a dialog
    void stop();
    void onEvent(WPARAM event, LPARAM gen);   // WM_APP_VIDEO (UI thread)
    const std::string& url() const { return url_; }
    bool shown() const { return shown_; }     // a frame is on screen
    bool failed() const { return failed_; }   // the clip could not play: the image stays

private:
    struct Notify;
    HWND parent_ = nullptr, hwnd_ = nullptr;
    IMFMediaEngine* engine_ = nullptr;
    Notify* notify_ = nullptr;
    std::string url_;
    RECT r_{};
    int radius_ = 0;
    bool ready_ = false, shown_ = false, failed_ = false, visible_ = true;
    LPARAM gen_ = 0;
};

} // namespace rn
