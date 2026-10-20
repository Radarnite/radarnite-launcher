// One play session (launcher/Session.cs): checks, the local backend, then the game client through launch.h.
// Everything it starts lives in the session's job object, so stop() (or the launcher exiting) ends all of it.
// Not in this core: the on-demand match server of 2.x (Session.MatchLoop/StartServerAsync/WatchMatch) starts a
// second game process, which belongs with the game launch.
#pragma once
#include "buildinfo.h"
#include "config.h"
#include "firewall.h"
#include "proc.h"
#include "util.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace rn {

// Session.Preflight: why the build cannot be played now ("" = OK). procs/rules let several builds share one process
// list and one firewall read (2.x read both again for every build).
std::wstring preflight(const BuildInfo& b, bool needMatch, const std::vector<proc::Info>& procs, const firewall::Rules& rules);
std::wstring preflight(const BuildInfo& b, bool needMatch);
// The backend the session would start: backend\radarnite-backend.exe, or node\node.exe + backend\server.mjs (2.x).
bool backendAvailable();
// Session.StopAll: kills only processes whose executable is inside the builds root (the offline builds, their stubs,
// a dev tree's node.exe), never the launcher itself. Returns how many.
int stopAll(const LogFn& log);

class PlaySession {
public:
    // (stage, state): stage backend|server|game, state run|ok|bad|skip.
    std::function<void(const wchar_t*, const wchar_t*)> stage;
    std::wstring failure;                 // why start() failed (shown in the launch bar)

    PlaySession(const BuildInfo& b, const LauncherConfig& c, LogFn log);
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    // StartAsync, blocking (call it on a worker thread). false = failure says why, everything started was stopped.
    bool start();
    bool running() const;                 // the game client is alive
    void waitClient();                    // returns when the client exits (at once without a client)
    std::wstring clientExitCode() const;  // "0x00000000", "?" without a client
    int64_t secondsSinceStart() const;
    void stop();                          // StopAll, ends the session's job, AuditWrites
    void auditWrites();

private:
    void fail(const std::wstring& why);
    void fitOwnPaks(const std::wstring& off);
    bool waitHttp(const std::wstring& url, int seconds);
    void setStage(const wchar_t* s, const wchar_t* v) { if (stage) stage(s, v); }

    BuildInfo b_;
    LauncherConfig c_;
    LogFn log_;
    FILETIME started_;                    // UTC
    ULONGLONG startedTick_;
    HANDLE job_ = nullptr;
    HANDLE backend_ = nullptr;
    HANDLE client_ = nullptr;
};

} // namespace rn
