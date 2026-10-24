// LaunchGame (launch.h) through radarnite-play.exe (play/): the 2.x launch code (launcher/Session.cs) as a windowless
// helper next to the launcher exe. It runs hidden inside the session's job, reports on stdout (one "kind: text" line
// per event, UTF-8) and prints "pid: <n>" once the client runs with the redirect confirmed. Builds with a local match
// keep it alive until the client exits (2.x's on-demand match server); the others let it exit right away. Its lines
// keep being read on a worker thread after LaunchGame returns.
#include "launch.h"
#include "paths.h"
#include "proc.h"
#include <memory>

namespace rn {

namespace {

// One argument as CommandLineToArgvW and the .NET runtime split it: quoted, with the backslashes before a quote (or
// before the closing quote) doubled.
std::wstring quoteArg(const std::wstring& a) {
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        r.append(c == L'"' ? bs * 2 + 1 : bs, L'\\');
        bs = 0;
        r += c;
    }
    r.append(bs * 2, L'\\');
    return r + L"\"";
}

std::wstring helperExe() {
    std::wstring self = join(parentDir(selfExe()), L"radarnite-play.exe");   // where the installer puts it
    if (fileExists(self)) return self;
    std::wstring dir = join(paths::launcherDir(), L"radarnite-play.exe");    // RADARNITE_LAUNCHER_DIR test trees
    return fileExists(dir) ? dir : self;
}

std::wstring helperArgs(const LaunchRequest& r) {
    const LauncherConfig& c = *r.config;
    auto b = [](bool v) { return v ? L"1" : L"0"; };
    return L"--build " + quoteArg(r.buildDir) + L" --launcher-dir " + quoteArg(paths::launcherDir()) +
           fmt(L" --port %d --name ", c.port) + quoteArg(c.displayName) + L" --last-playlist " + quoteArg(c.lastPlaylist) +
           L" --practice-playlist " + quoteArg(c.practicePlaylist) + fmt(L" --bots %d --infinite-materials %ls --infinite-ammo %ls",
           c.bots, b(c.infiniteMaterials), b(c.infiniteAmmo)) + fmt(L" --late-game %ls --bots-ai %ls --autostart %d", b(c.lateGame),
           b(c.botsAI), c.autoStartSeconds);
}

// What LaunchGame waits for: the client's pid, or the end of the helper's output.
struct Waiter {
    Lock l;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD pid = 0;
    std::wstring error, lastLog;
    ~Waiter() { if (ready) CloseHandle(ready); }
};

// "kind: text" -> kind, text (a line without a known kind is a log line: a runtime error on stderr, for one)
bool splitLine(const std::wstring& line, std::wstring* kind, std::wstring* text) {
    size_t c = line.find(L": ");
    if (c == std::wstring::npos || c == 0 || c > 8) return false;
    *kind = line.substr(0, c);
    *text = line.substr(c + 2);
    return true;
}

void splitWord(const std::wstring& s, std::wstring* first, std::wstring* rest) {
    size_t sp = s.find(L' ');
    *first = s.substr(0, sp);
    *rest = sp == std::wstring::npos ? L"" : trim(s.substr(sp + 1));
}

// The lastPlaylist the game asked for, saved like 2.x's MatchLoop did (the helper never writes config.json).
void savePlaylist(const std::wstring& code) {
    if (blank(code)) return;
    paths::ConfigLock lock;
    LauncherConfig c = LauncherConfig::load();
    if (c.lastPlaylist == code) return;
    c.lastPlaylist = code;
    c.save();
}

struct Reader {
    HANDLE pipe;                        // owned
    std::shared_ptr<Waiter> w;
    LogFn log;
    std::function<void(const wchar_t*, const wchar_t*)> stage, match;
    HANDLE client = nullptr;            // owned, set once "pid:" arrived (stage/match lines after the game ended are dropped)

    void line(std::string raw) {
        if (raw.size() >= 3 && (uint8_t)raw[0] == 0xEF && (uint8_t)raw[1] == 0xBB && (uint8_t)raw[2] == 0xBF) raw.erase(0, 3);
        std::wstring l = widen(raw), kind, text;
        if (blank(l)) return;
        if (!splitLine(l, &kind, &text)) { kind = L"log"; text = l; }
        bool gameOver = client && WaitForSingleObject(client, 0) == WAIT_OBJECT_0;
        if (kind == L"log") {
            { Guard g(w->l); w->lastLog = text; }
            if (log) log(text);
        } else if (kind == L"error") {
            Guard g(w->l);
            w->error = text;
        } else if (kind == L"pid") {
            DWORD pid = (DWORD)wcstoul(text.c_str(), nullptr, 10);
            if (pid && !client) client = OpenProcess(SYNCHRONIZE, FALSE, pid);
            { Guard g(w->l); w->pid = pid; }
            SetEvent(w->ready);
        } else if (kind == L"stage" || kind == L"match") {
            std::wstring a, b;
            splitWord(text, &a, &b);
            auto& f = kind == L"stage" ? stage : match;
            if (f && !gameOver && !a.empty()) f(a.c_str(), b.c_str());
        } else if (kind == L"playlist") {
            savePlaylist(trim(text));
        } else if (log) {
            log(l);
        }
    }

    void run() {
        std::string cur;
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(pipe, buf, sizeof buf, &got, nullptr) && got) {
            for (DWORD i = 0; i < got; i++) {
                char c = buf[i];
                if (c == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); line(cur); cur.clear(); }
                else cur += c;
            }
        }
        if (!cur.empty()) line(cur);
        CloseHandle(pipe);
        if (client) CloseHandle(client);
        SetEvent(w->ready);   // the helper exited (or closed its output): LaunchGame stops waiting
    }
};

} // namespace

HANDLE LaunchGame(const LaunchRequest& r, std::wstring* error) {
    auto fail = [&](const std::wstring& e) -> HANDLE { if (error) *error = e; return nullptr; };
    if (!r.config) return fail(L"The game did not start: no launcher configuration.");
    std::wstring exe = helperExe();
    if (!fileExists(exe)) return fail(L"A launcher file is missing (radarnite-play.exe).");

    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return fail(L"Could not start radarnite-play.exe: " + winError(GetLastError()));
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    proc::Start s;
    s.exe = exe;
    s.args = helperArgs(r);
    s.cwd = parentDir(exe);
    s.job = r.job;
    s.hidden = true;
    s.out = wr;
    s.err = wr;
    std::wstring err;
    Handle helper(proc::start(s, nullptr, &err));
    CloseHandle(wr);   // the helper holds the only write end now: its exit ends the reader
    if (!helper.ok()) { CloseHandle(rd); return fail(L"Could not start radarnite-play.exe: " + err); }

    auto w = std::make_shared<Waiter>();
    auto reader = std::make_shared<Reader>();
    reader->pipe = rd;
    reader->w = w;
    reader->log = r.log;
    reader->stage = r.stage;
    reader->match = r.match;
    runAsync([reader] { reader->run(); });

    // Bounded by the helper: it gives up on the redirect after ~130 s, and the session's stop() ends its job.
    WaitForSingleObject(w->ready, INFINITE);
    DWORD pid;
    std::wstring helperError, lastLog;
    {
        Guard g(w->l);
        pid = w->pid;
        helperError = w->error;
        lastLog = w->lastLog;
    }
    if (pid) {
        HANDLE client = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (client) return client;
        return fail(L"The game process exited at start.");
    }
    if (!helperError.empty()) return fail(helperError);
    DWORD code = 0;
    if (WaitForSingleObject(helper, 5000) == WAIT_OBJECT_0 && GetExitCodeProcess(helper, &code) && code != 0)
        return fail(fmt(L"The game did not start (radarnite-play.exe exit code 0x%08lX).", (unsigned long)code));
    return fail(lastLog.empty() ? L"The game did not start." : L"The game did not start: " + lastLog);
}

} // namespace rn
