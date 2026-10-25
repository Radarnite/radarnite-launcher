// The preview's clip (radarnite.com's item sheet plays fortnite.gg's clip): looped, with sound at the saved volume like
// the site's player. It plays in a process of its own (RadarniteLauncher.exe --clip): Media Foundation and the
// graphics device it creates cost ~100 MB, and part of that is never given back while a process lives. The clip
// process puts a child window over the preview's media area of this window (a cross-process child: it never takes the
// focus; the mouse messages it gets are posted on to this window in its coordinates), lives in a job that dies with
// the launcher, and is killed when the preview closes. If it cannot play, the image stays.
// The window draws the player's controls under the clip and sends them as messages (play/pause, seek, volume, mute);
// the clip process answers with its position, length and paused state (4 a second while it plays, once when it stops).
// RN_CLIP_SILENT=1 in the environment (the selftest, test runs) keeps every clip muted whatever the controls say.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>

namespace rn {

const UINT WM_APP_VIDEO = WM_APP + 4;   // from the clip process: wParam 1 = its first frame is on screen, 2 = status (lParam packed)

class Video {
public:
    ~Video() { stop(); }
    // Starts the clip process for url (https only) over r (client pixels of parent; left corners rounded by radius px),
    // letterboxed on border (0xRRGGBB). Its window shows once the first frame is ready.
    // volume 0..1 and muted: the player's state when it starts.
    bool play(HWND parent, const std::string& url, RECT r, int radius, uint32_t border, float volume, bool muted);
    void place(RECT r, bool visible);   // every paint: follows the preview, hidden under a dialog
    void stop();                        // kills the clip process (its window goes with it)
    void onEvent(WPARAM event, LPARAM gen);   // WM_APP_VIDEO
    const std::string& url() const { return url_; }
    bool shown() const;                 // a frame is on screen
    bool failed() const { return failed_; }
    size_t processBytes() const;        // the clip process's private bytes (selftest)
    // The player's controls
    void setPaused(bool p);
    void seek(double seconds);
    void setVolume(float v, bool muted);
    bool paused() const { return paused_; }
    double position() const { return pos_; }
    double duration() const { return dur_; }
    bool status() const { return status_; }   // a status came since play()

private:
    HWND parent_ = nullptr, child_ = nullptr;
    HANDLE proc_ = nullptr;
    std::string url_;
    RECT r_{};
    bool visible_ = true, failed_ = false, paused_ = false, status_ = false;
    double pos_ = 0, dur_ = 0;
    LPARAM gen_ = 0;
    void send(WPARAM cmd, LPARAM v);
};

// RadarniteLauncher.exe --clip <parent hwnd> <x> <y> <w> <h> <radius> <border rgb> <generation> <volume per mille> <muted> <url>
int clipMain(int argc, wchar_t** argv);

} // namespace rn
