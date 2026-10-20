// The real launcher core behind native/ui/api.h (the logic of launcher/Web.cs without the WebView), and the
// single-instance guard of the main window.
#pragma once
#include "../ui/api.h"

namespace rn {

// autoPlay = the build of "--play <build>" (desktop shortcut): selected and launched as soon as the window is ready;
// the window is closed when the game closes. nullptr = the normal window.
Core* createCore(const wchar_t* autoPlay = nullptr);

// One main window per Windows session (2.2.1 SingleInstance, same mutex/event names, so 2.x and 3.0 windows find
// each other). The UI calls claim() before creating its window (not for --play, the console verbs or
// --selftest-ui; --new-instance skips it), listen() once the window exists, release() after its message loop.
namespace instance {
bool claim();      // true = this process owns the main window; false = the open one was asked to come to the front: exit
void listen();     // restores and focuses this process's main window whenever another launch signals
void release();
} // namespace instance

} // namespace rn
