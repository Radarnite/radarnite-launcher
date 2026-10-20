// Processes (launcher/Core.cs Native + Session.Start/StopAll): the process list, image paths, job objects, starting
// helper processes inside a job, killing process trees, and the launcher's own window.
#pragma once
#include "util.h"
#include <windows.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace rn {
namespace proc {

struct Info {
    DWORD pid = 0, ppid = 0;
    std::wstring exe;                     // image file name ("FortniteClient-Win64-Shipping.exe")
};
// Every process, in the order Process.GetProcesses() returns them.
std::vector<Info> list();
// Process.ProcessName: the image name without ".exe".
std::wstring shortName(const std::wstring& exe);
// Full path of the process image ("" = gone or no access): Native.ImagePath.
std::wstring imagePath(DWORD pid);

// A job object whose processes die when its last handle closes (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE).
HANDLE newJob();
// The process-wide job for helper tools (cosmetics generator, sig-fit, tar): dies with the launcher.
HANDLE toolsJob();

struct Start {
    std::wstring exe;                     // full path; the command line is "exe" args, like ProcessStartInfo
    std::wstring args;
    std::wstring cwd;                     // "" = the launcher's current directory
    std::vector<std::pair<std::wstring, std::wstring>> env;   // variables set (or replaced) for the child
    bool hidden = true;                   // CreateNoWindow + WindowStyle.Hidden
    HANDLE job = nullptr;                 // created inside it (PROC_THREAD_ATTRIBUTE_JOB_LIST)
    HANDLE out = nullptr, err = nullptr;  // inheritable write ends for stdout/stderr (nullptr = none)
    DWORD priority = 0;                   // e.g. BELOW_NORMAL_PRIORITY_CLASS (0 = normal)
};
// CreateProcessW. Returns the process handle (the caller closes it) or nullptr with *error.
HANDLE start(const Start& s, DWORD* pid, std::wstring* error);
// Starts s with stdout and stderr read line by line (UTF-8, CR/LF trimmed) until both close; onLine runs on reader
// threads. Returns the exit code, or -1 when it could not start (*error).
int run(Start s, const std::function<void(const std::wstring& line, bool isErr)>& onLine, std::wstring* error);

// Process.Kill(entireProcessTree: true) on pid using a list() snapshot. true = killed or already gone.
bool killTree(DWORD pid, const std::vector<Info>& snapshot);

// The launcher's main window in this process (class RadarniteLauncher3, else any visible unowned top-level window).
HWND mainWindow();
// Runs this exe again elevated (UAC prompt, hidden) and waits: Program.RunElevated. false = cancelled or exit code != 0.
bool runElevated(const std::wstring& args);
// ShellExecute "open" (explorer.exe "<dir>", notepad.exe "<file>"...). false when it could not start.
bool open(const std::wstring& exe, const std::wstring& args);

} // namespace proc
} // namespace rn
