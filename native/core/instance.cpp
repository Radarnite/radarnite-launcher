// Single main window per Windows session (launcher/Program.cs SingleInstance in 2.2.1).
#include "core.h"
#include "proc.h"
#include <windows.h>

namespace rn {
namespace instance {
namespace {

const wchar_t* kMutex = L"Local\\RadarniteLauncher.MainWindow";
const wchar_t* kShowEvent = L"Local\\RadarniteLauncher.ShowMainWindow";
HANDLE g_mutex = nullptr, g_show = nullptr;
volatile LONG g_released = 0;

DWORD WINAPI listenProc(LPVOID ev) {
    while (WaitForSingleObject((HANDLE)ev, INFINITE) == WAIT_OBJECT_0) {
        if (g_released) break;
        HWND h = proc::mainWindow();
        if (!h) continue;
        if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);   // back to normal or maximized, as it was
        else if (!IsWindowVisible(h)) ShowWindowAsync(h, SW_SHOW);
        SetForegroundWindow(h);
    }
    CloseHandle((HANDLE)ev);
    return 0;
}

} // namespace

bool claim() {
    HANDLE m = CreateMutexW(nullptr, TRUE, kMutex);
    if (!m) return true;   // never block the launcher on a mutex problem
    if (GetLastError() != ERROR_ALREADY_EXISTS) { g_mutex = m; return true; }
    // An open main window listens on the event: ask it to come to the front and step aside.
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kShowEvent);
    if (ev) {
        AllowSetForegroundWindow(ASFW_ANY);   // we were just started by the user: hand the foreground over
        SetEvent(ev);
        CloseHandle(ev);
        CloseHandle(m);
        return false;
    }
    // No listener: the owner is exiting (e.g. closing for an update) or still starting. Wait for it briefly.
    DWORD w = WaitForSingleObject(m, 3000);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { g_mutex = m; return true; }
    CloseHandle(m);
    return false;
}

void listen() {
    if (!g_mutex || g_show) return;
    g_show = CreateEventW(nullptr, FALSE, FALSE, kShowEvent);
    if (!g_show) return;
    HANDLE t = CreateThread(nullptr, 0, listenProc, g_show, 0, nullptr);
    if (t) CloseHandle(t);
}

void release() {
    // the listener wakes up, sees the flag and closes the event, so the next launcher finds no listener here
    if (g_show) { InterlockedExchange(&g_released, 1); SetEvent(g_show); g_show = nullptr; }
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
}

} // namespace instance
} // namespace rn
