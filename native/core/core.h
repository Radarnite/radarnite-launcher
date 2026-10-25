// The real launcher core behind native/ui/api.h (the logic of launcher/Web.cs without the WebView).
#pragma once
#include "../ui/api.h"

namespace rn {

// autoPlay = the build of "--play <build>" (desktop shortcut): selected and launched as soon as the window is ready;
// the window is closed when the game closes. nullptr = the normal window.
// automated = a launch by a script or an agent, not by the owner (core::AutomatedStart): it runs game-guard.ps1 first
// and does not start while an anti-cheat or another game is running (the window closes, exit code 6).
Core* createCore(const wchar_t* autoPlay = nullptr, bool automated = false);

namespace core {
// Single instance (2.2.1 names): the UI's WinMain owns the mutex Local\RadarniteLauncher.MainWindow and the event
// Local\RadarniteLauncher.ShowMainWindow. This only signals that event for a launch that lost the mutex, handing it
// the foreground. true = an open main window (2.2.1 or 3.0) was signalled. The console verbs never call it.
// suffix: appended to the event name, only for the UI selftest's own names (never the real launcher's).
bool ForwardToRunningInstance(const wchar_t* suffix = nullptr);
// A --play the owner did not start himself: RADARNITE_AUTOMATED=1, or started hidden (STARTF_USESHOWWINDOW with
// SW_HIDE: Start-Process -WindowStyle Hidden, how the test scripts start it). The desktop shortcut and the Play
// button are never automated. --automated on the command line says it explicitly.
bool AutomatedStart();
// The exit code the window's process should end with (0, or 6 when the game guard refused an automated launch).
int ExitCode();
} // namespace core

} // namespace rn
