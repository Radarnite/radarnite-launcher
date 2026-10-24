// The preview's clip (radarnite.com's item card plays fortnite.gg's clip), muted and looped. It plays in a process of
// its own (RadarniteLauncher.exe --clip): Media Foundation and the graphics device it creates cost ~100 MB, and part
// of that is never given back while a process lives. The clip process puts a child window over the preview's media
// area of this window (a cross-process child: it never takes the focus, the mouse goes through it), lives in a job
// that dies with the launcher, and is killed when the preview closes. If it cannot play, the image stays.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>

namespace rn {

const UINT WM_APP_VIDEO = WM_APP + 4;   // from the clip process: its first frame is on screen (lParam = play generation)

class Video {
public:
    ~Video() { stop(); }
    // Starts the clip process for url (https only) over r (client pixels of parent; left corners rounded by radius px),
    // letterboxed on border (0xRRGGBB). Its window shows once the first frame is ready.
    bool play(HWND parent, const std::string& url, RECT r, int radius, uint32_t border);
    void place(RECT r, bool visible);   // every paint: follows the preview, hidden under a dialog
    void stop();                        // kills the clip process (its window goes with it)
    void onEvent(WPARAM event, LPARAM gen);   // WM_APP_VIDEO
    const std::string& url() const { return url_; }
    bool shown() const;                 // a frame is on screen
    bool failed() const { return failed_; }
    size_t processBytes() const;        // the clip process's private bytes (selftest)

private:
    HWND parent_ = nullptr, child_ = nullptr;
    HANDLE proc_ = nullptr;
    std::string url_;
    RECT r_{};
    bool visible_ = true, failed_ = false;
    LPARAM gen_ = 0;
};

// RadarniteLauncher.exe --clip <parent hwnd> <x> <y> <w> <h> <radius> <border rgb> <generation> <url>
int clipMain(int argc, wchar_t** argv);

} // namespace rn
