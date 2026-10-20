// The real launcher core behind native/ui/api.h (the logic of launcher/Web.cs without the WebView).
#pragma once
#include "../ui/api.h"

namespace rn {

// autoPlay = the build of "--play <build>" (desktop shortcut): selected and launched as soon as the window is ready;
// the window is closed when the game closes. nullptr = the normal window.
Core* createCore(const wchar_t* autoPlay = nullptr);

namespace core {
// Single instance (2.2.1 names): the UI's WinMain owns the mutex Local\RadarniteLauncher.MainWindow and the event
// Local\RadarniteLauncher.ShowMainWindow. This only signals that event for a launch that lost the mutex, handing it
// the foreground. true = an open main window (2.2.1 or 3.0) was signalled. The console verbs never call it.
bool ForwardToRunningInstance();
} // namespace core

} // namespace rn
