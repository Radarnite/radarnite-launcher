// Game launch: the one entry point the core calls to start the game client. launch.cpp runs radarnite-play.exe (the
// 2.x launch code, play/) for it; launch_stub.cpp only reports "not implemented" (build.sh picks launch.cpp when it
// exists; for the UI, link launch.cpp's object before librncore.a).
#pragma once
#include "buildinfo.h"
#include "config.h"
#include "util.h"
#include <windows.h>
#include <string>

namespace rn {

struct LaunchRequest {
    std::wstring buildDir;              // BuildInfo::dir: the folder that holds FortniteGame
    std::wstring exePath;               // paths::shipping(buildDir)
    std::wstring dllPath;               // paths::redirectDll()
    std::wstring offlineDir;            // paths::offline(buildDir): per-build logs and data
    const BuildInfo* build = nullptr;   // version, CL, engine, support
    const LauncherConfig* config = nullptr;   // config.json as loaded for this session (port, displayName, accountId...)
    HANDLE job = nullptr;               // the session's job object (KILL_ON_JOB_CLOSE): every process started goes in it
    LogFn log;                          // Activity lines (any thread)
    // Optional, any thread, also after LaunchGame returned (while the client runs): (stage, state) like
    // PlaySession::stage (game|server, run|ok|bad|""), and the local match's (phase, detail) (Session.Match).
    std::function<void(const wchar_t*, const wchar_t*)> stage, match;
};

// Starts the game client for an already-running backend. Returns the client's process handle or nullptr with *error set.
HANDLE LaunchGame(const LaunchRequest& r, std::wstring* error);

} // namespace rn
