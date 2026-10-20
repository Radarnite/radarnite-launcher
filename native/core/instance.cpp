// The second launch's side of the 2.2.1 single instance. The main window side (the mutex, listening, bringing the
// window to the front) belongs to the UI's WinMain; the core only signals an open window.
#include "core.h"
#include <windows.h>
#include <string>

namespace rn {
namespace core {

bool ForwardToRunningInstance(const wchar_t* suffix) {
    std::wstring name = L"Local\\RadarniteLauncher.ShowMainWindow";
    if (suffix) name += suffix;
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (!ev) return false;
    AllowSetForegroundWindow(ASFW_ANY);   // we were just started by the user: hand the foreground over
    BOOL ok = SetEvent(ev);
    CloseHandle(ev);
    return ok != FALSE;
}

} // namespace core
} // namespace rn
