#include "session.h"
#include "launch.h"
#include "net.h"
#include "paktools.h"
#include "paths.h"
#include <knownfolders.h>
#include <deque>
#include <mutex>
#include <thread>

namespace rn {

namespace {
std::mutex g_stopLock;

// The item shop of the progression (backend/progress.mjs): today's real shop, radarnite.com's data.json, into
// offline\shop.json once a day, in the background: the launch never waits for it, and without a network the backend
// uses the last copy or a rotation of the build's own items. Only in the legal play modes.
// Nothing is logged from the download thread (it can outlive the session); the backend logs the shop it uses.
void refreshShop(const std::wstring& off, const std::wstring& mode) {
    if (mode != L"legal" && mode != L"legal-vbucks") return;
    std::wstring file = join(off, L"shop.json");
    SYSTEMTIME t; GetSystemTime(&t);
    char today[32]; snprintf(today, sizeof today, "\"date\": \"%04u-%02u-%02u\"", t.wYear, t.wMonth, t.wDay);
    std::string have;
    if (readFile(file, &have) && have.substr(0, 400).find(today) != std::string::npos) return;
    std::thread([file] {
        std::wstring err;
        net::fetchFile(L"https://radarnite.com/data.json", L"RadarniteLauncher", 60000, file, &err);
    }).detach();
}
}

bool backendAvailable() {
    return fileExists(paths::backend()) || (fileExists(paths::node()) && fileExists(paths::backendScript()));
}

namespace {

template <typename F>
std::wstring preflightImpl(const BuildInfo& b, const std::vector<proc::Info>& procs, F firewallCheck) {
    for (auto& p : procs) {
        std::wstring name = proc::shortName(p.exe);
        if (istarts(name, L"EpicGamesLauncher") || ieq(name, L"EpicWebHelper"))
            return L"The Epic Games Launcher is open: quit it completely (also from the tray) and try again.";
        if (istarts(name, L"FortniteClient") || istarts(name, L"FortniteLauncher")) {
            std::wstring path = proc::imagePath(p.pid);
            if (path.empty() || !isUnder(path, paths::root())) return L"The real Fortnite is running: close it before playing offline.";
            return L"An offline build is already running: use Stop first.";
        }
    }
    auto lobby = b.lobby();
    if (lobby.first == Support::No) return L"Fortnite " + b.version + L" is not supported: " + lobby.second + L".";
    if (!backendAvailable() || !fileExists(paths::redirectDll())) return L"A launcher file is missing (node, backend or redirect).";
    std::wstring fw = firewallCheck();
    if (!fw.empty()) return L"Firewall: " + fw + L". Press Create firewall rules (asks for administrator permission).";
    return L"";
}

} // namespace

std::wstring preflight(const BuildInfo& b, bool, const std::vector<proc::Info>& procs, const firewall::Rules& rules) {
    return preflightImpl(b, procs, [&] { return firewall::check(rules, b.dir); });
}

std::wstring preflightChecked(const BuildInfo& b, const std::vector<proc::Info>& procs, const std::wstring& firewallCheck) {
    return preflightImpl(b, procs, [&] { return firewallCheck; });
}

std::wstring preflight(const BuildInfo& b, bool needMatch) { return preflight(b, needMatch, proc::list(), firewall::read()); }

int stopAll(const LogFn& log) {
    int n = 0;
    DWORD self = GetCurrentProcessId();
    std::wstring root = paths::root();
    auto snap = proc::list();
    for (auto& p : snap) {
        if (p.pid == self || p.pid == 0) continue;
        std::wstring path = proc::imagePath(p.pid);
        if (path.empty() || !isUnder(path, root)) continue;
        std::wstring fn = fileName(path);
        if (ieq(fn, L"FnOffline.exe") || ieq(fn, L"RadarniteLauncher.exe")) continue;
        if (proc::killTree(p.pid, snap)) n++;
    }
    log(n > 0 ? fmt(L"Stopped %d offline build processes.", n) : L"Nothing was running.");
    return n;
}

PlaySession::PlaySession(const BuildInfo& b, const LauncherConfig& c, LogFn log) : b_(b), c_(c), log_(std::move(log)) {
    GetSystemTimePreciseAsFileTime(&started_);
    startedTick_ = GetTickCount64();
}

PlaySession::~PlaySession() {
    if (job_) CloseHandle(job_);   // KILL_ON_JOB_CLOSE: whatever the session started goes with it
    if (backend_) CloseHandle(backend_);
    if (client_) CloseHandle(client_);
}

void PlaySession::fail(const std::wstring& why) {
    failure = why;
    log_(why);
}

bool PlaySession::running() const { return client_ && WaitForSingleObject(client_, 0) == WAIT_TIMEOUT; }

void PlaySession::waitClient() { if (client_) WaitForSingleObject(client_, INFINITE); }

std::wstring PlaySession::clientExitCode() const {
    DWORD code = 0;
    if (!client_ || !GetExitCodeProcess(client_, &code)) return L"?";
    return fmt(L"0x%08lX", (unsigned long)code);
}

int64_t PlaySession::secondsSinceStart() const { return (int64_t)((GetTickCount64() - startedTick_) / 1000); }

bool PlaySession::start() {
    bool matchOk = b_.match().first != Support::No && fileExists(paths::gameServerDll());
    std::wstring why = preflight(b_, matchOk);
    if (!why.empty()) { fail(L"Not starting: " + why); return false; }
    std::wstring off = paths::offline(b_.dir);
    if (!fileExists(join(off, L"cosmetics.json")))
        log_(L"Warning: this build has no cosmetics list yet (press Generate cosmetics): only the defaults will show.");
    fitOwnPaks(off);
    refreshShop(off, c_.playMode());

    // 1. backend (127.0.0.1 only)
    setStage(L"backend", L"run");
    job_ = proc::newJob();
    proc::Start s;
    std::wstring common = L"--build \"" + b_.dir + L"\" --config \"" + paths::config() + L"\" --port " + std::to_wstring(c_.port) + (matchOk ? L"" : L" --no-match");
    if (fileExists(paths::backend())) { s.exe = paths::backend(); s.args = common; }
    else { s.exe = paths::node(); s.args = L"\"" + paths::backendScript() + L"\" " + common; }
    s.cwd = parentDir(s.exe);
    s.job = job_;
    std::wstring err;
    backend_ = proc::start(s, nullptr, &err);
    if (!backend_) log_(L"Error: could not start " + s.exe + L": " + err);
    if (!backend_ || !waitHttp(fmt(L"http://127.0.0.1:%d/offline/health", c_.port), 15)) {
        setStage(L"backend", L"bad");
        fail(L"The backend did not start (see offline\\backend.log).");
        stop();
        return false;
    }
    log_(fmt(L"Backend ready on 127.0.0.1:%d (local account \"%ls\").", c_.port, c_.displayName.c_str()));
    setStage(L"backend", L"ok");
    auto match = b_.match();
    if (match.first == Support::No) log_(L"No local match for " + b_.version + L": " + match.second + L". Lobby only.");
    else if (!matchOk) log_(L"Local match: launcher\\gameserver\\radarnite-server.dll is missing. Lobby only.");
    else if (match.first == Support::Experimental) log_(L"Local match is EXPERIMENTAL on " + b_.version + L": " + match.second + L".");

    // 2. client (launch.h)
    setStage(L"game", L"run");
    LaunchRequest r;
    r.buildDir = b_.dir;
    r.exePath = paths::shipping(b_.dir);
    r.dllPath = paths::redirectDll();
    r.offlineDir = off;
    r.build = &b_;
    r.config = &c_;
    r.job = job_;
    r.log = log_;
    r.stage = stage;
    r.match = this->match;   // the member, not the build's match support above
    err.clear();
    client_ = LaunchGame(r, &err);
    if (!client_) {
        setStage(L"game", L"bad");
        fail(err.empty() ? L"The game did not start." : err);
        stop();
        return false;
    }
    setStage(L"game", L"ok");
    log_(L"Fortnite " + b_.version + L" started. Pick a mode in the game (CHANGE in the lobby) and press PLAY. Closing the game stops everything.");
    return true;
}

// Our own cosmetic paks (offline\custom-cosmetics.json) must match a signature file of the build, or 14.30 shows
// "Corrupted data was found in game files" and quits: every launch of a classic-pak build checks them first and fits
// any that do not match (tools\sigfit-installed.ps1 with the tool next to it, paktools.h); a second or two when nothing
// changed. A sigfit that is not the one this launcher was built with is never run.
void PlaySession::fitOwnPaks(const std::wstring& off) {
    if (b_.usesIoStore() || !fileExists(join(off, L"custom-cosmetics.json"))) return;
    std::wstring bad = paktools::verify(paktools::dir());
    if (!bad.empty()) {
        log_(L"Error: own paks NOT checked: the launcher's sigfit is not the expected one (" + bad + L"). Reinstall the launcher; the game may stop with \"Corrupted data\".");
        return;
    }
    paktools::syncDevTree(log_);
    std::wstring script = paktools::sigfitScript();
    proc::Start s;
    s.exe = L"powershell.exe";
    // -Tool: always the launcher's own tool, never a fallback copy elsewhere on the PC
    s.args = L"-NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -Build \"" + b_.dir + L"\" -Tool \"" + paktools::sigfitTool() + L"\"";
    s.job = job_ ? job_ : proc::toolsJob();
    std::vector<std::wstring> lines;
    std::wstring firstErr, err;
    std::mutex m;
    int code = proc::run(s, [&](const std::wstring& line, bool isErr) {
        std::wstring t = trim(line);
        std::lock_guard<std::mutex> g(m);
        if (isErr) { if (firstErr.empty() && !t.empty()) firstErr = t; return; }
        if (!t.empty()) lines.push_back(t);
    }, &err);
    if (code == -1 && !err.empty()) { log_(L"Warning: could not check the own paks' signatures: " + err); return; }
    for (auto& l : lines) if (starts(l, L"fitted ") || starts(l, L"FAILED ")) log_(L"Own paks: " + l);
    if (!lines.empty()) log_((code == 0 ? L"Own paks: " : L"Warning: own paks: ") + lines.back());
    if (code != 0 && !firstErr.empty()) log_(L"Warning: own paks: " + firstErr);
}

bool PlaySession::waitHttp(const std::wstring& url, int seconds) {
    for (int i = 0; i < seconds * 4; i++) {
        if (net::get(url, L"radarnite-launcher", nullptr, 3000).ok()) return true;
        // the backend already exited: no point in waiting the full 15 s
        if (backend_ && WaitForSingleObject(backend_, 0) == WAIT_OBJECT_0) return false;
        Sleep(250);
    }
    return false;
}

void PlaySession::stop() {
    std::lock_guard<std::mutex> g(g_stopLock);
    stopAll(log_);
    // what the session started outside the builds root (the backend of an installed launcher) ends with its job
    if (job_) { TerminateJobObject(job_, 1); CloseHandle(job_); job_ = nullptr; }
    auditWrites();
}

// Files changed during the session in the real Fortnite/Epic per-user folders (there should be none: the build
// writes inside its own folder), listed in offline\writes-outside.log.
void PlaySession::auditWrites() {
    std::wstring local = knownFolder(FOLDERID_LocalAppData), roaming = knownFolder(FOLDERID_RoamingAppData);
    std::wstring dirs[] = { join(local, L"FortniteGame"), join(local, L"CrashReportClient"), join(local, L"EpicGamesLauncher"),
                            join(local, L"UnrealEngine"), join(local, L"EpicOnlineServicesUIHelper"), join(roaming, L"Epic") };
    ULARGE_INTEGER since;
    since.LowPart = started_.dwLowDateTime;
    since.HighPart = started_.dwHighDateTime;
    since.QuadPart -= 2 * 10000000ull;
    std::vector<std::wstring> hits;
    for (auto& d : dirs) {
        if (!dirExists(d)) continue;
        std::deque<std::wstring> pending{ d };
        while (!pending.empty()) {
            std::wstring cur = pending.front();
            pending.pop_front();
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW((cur + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                std::wstring p = cur + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) pending.push_back(p); continue; }
                ULARGE_INTEGER w;
                w.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                w.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                if (w.QuadPart >= since.QuadPart) hits.push_back(isoLocal(fd.ftLastWriteTime) + L" " + p);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::wstring report = join(paths::offline(b_.dir), L"writes-outside.log");
    std::wstring text = L"session " + isoLocal(started_) + L" - " + nowIso() + fmt(L": %d files changed outside the build\r\n", (int)hits.size());
    for (size_t i = 0; i < hits.size() && i < 500; i++) text += hits[i] + L"\r\n";
    writeFile(report, narrow(text));
    log_(hits.empty() ? L"Write check: nothing written outside the build folder."
                      : fmt(L"Write check: %d files changed in real Fortnite/Epic folders during the session (see offline\\writes-outside.log). They may come from the real game if it was used meanwhile.", (int)hits.size()));
}

} // namespace rn
