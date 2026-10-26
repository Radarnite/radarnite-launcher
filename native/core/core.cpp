// rn::Core for the native window: launcher/Web.cs without the WebView. Same Activity lines, toasts, session states and
// order of operations as the 2.x host; the work runs on the core's own threads and comes back through UiSink.
#include "core.h"
#include "builddata.h"
#include "buildinfo.h"
#include "config.h"
#include "firewall.h"
#include "mods.h"
#include "paktools.h"
#include "paths.h"
#include "port.h"
#include "proc.h"
#include "session.h"
#include "update.h"
#include "util.h"
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace rn {
namespace {

std::wstring orOk(const std::wstring& s) { return s.empty() ? L"OK" : s; }
std::atomic<int> g_exitCode{ 0 };

// ---------------------------------------------------------------------------------------------- shell dialogs
template <typename T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool pickPath(HWND owner, const wchar_t* title, bool folder, const std::wstring& initial, std::wstring* out) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    DWORD o = 0;
    d->GetOptions(&o);
    o |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) o |= FOS_PICKFOLDERS;
    else {
        o |= FOS_FILEMUSTEXIST;
        COMDLG_FILTERSPEC f[] = { { L"Fortnite build (*.zip)", L"*.zip" } };
        d->SetFileTypes(1, f);
    }
    d->SetOptions(o);
    d->SetTitle(title);
    if (!initial.empty()) {
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&start)))) d->SetFolder(start);
        release(start);
    }
    bool ok = false;
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->Show(owner)) && SUCCEEDED(d->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { *out = p; ok = true; }
        CoTaskMemFree(p);
    }
    release(it);
    release(d);
    return ok;
}

// Program.CreateShortcut: desktop "Fortnite <build> Offline.lnk" that plays that build.
bool createShortcut(const BuildInfo& b, std::wstring* path, std::wstring* error) {
    *path = join(knownFolder(FOLDERID_Desktop), L"Fortnite " + b.name() + L" Offline.lnk");
    IShellLinkW* l = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l));
    if (SUCCEEDED(hr)) {
        std::wstring exe = selfExe();
        l->SetPath(exe.c_str());
        l->SetArguments((L"--play \"" + b.name() + L"\"").c_str());
        l->SetWorkingDirectory(paths::launcherDir().c_str());
        l->SetIconLocation(paths::shipping(b.dir).c_str(), 0);
        IPersistFile* f = nullptr;
        hr = l->QueryInterface(IID_PPV_ARGS(&f));
        if (SUCCEEDED(hr)) hr = f->Save(path->c_str(), TRUE);
        release(f);
    }
    release(l);
    if (FAILED(hr)) *error = winError((DWORD)hr);
    return SUCCEEDED(hr);
}

SessionState stateOf(const std::wstring& s) {
    if (s == L"launching") return SessionState::Launching;
    if (s == L"running") return SessionState::Running;
    if (s == L"stopping") return SessionState::Stopping;
    if (s == L"error") return SessionState::Error;
    return SessionState::Idle;
}
Stage stageOf(const std::wstring& s) {
    if (s == L"run") return Stage::Run;
    if (s == L"ok") return Stage::Ok;
    if (s == L"bad") return Stage::Bad;
    return Stage::None;
}
MatchPhase phaseOf(const std::wstring& s) {
    static const std::pair<const wchar_t*, MatchPhase> k[] = { { L"waiting", MatchPhase::Waiting }, { L"joined", MatchPhase::Joined },
        { L"warmup", MatchPhase::Warmup }, { L"bus", MatchPhase::Bus }, { L"storm", MatchPhase::Storm }, { L"ended", MatchPhase::Ended }, { L"down", MatchPhase::Down } };
    for (auto& x : k) if (s == x.first) return x.second;
    return MatchPhase::None;
}

// ---------------------------------------------------------------------------------------------- shared state
// Everything the worker threads touch lives here (held by shared_ptr), so a worker that outlives the window never
// reaches freed memory; the sink is cleared, under its own lock, when the core is destroyed.
struct Ctx : std::enable_shared_from_this<Ctx> {
    std::mutex sinkM;                  // held while calling the sink (the UI only queues and posts)
    UiSink* sink = nullptr;
    std::mutex m;                      // the fields below
    std::wstring selected, autoPlay;
    bool automated = false;            // autoPlay came from a script or an agent (core::AutomatedStart)
    std::wstring state = L"idle", sessionVersion, failure;
    std::map<std::wstring, std::wstring> stages;
    std::wstring phase, detail;
    std::shared_ptr<PlaySession> session;
    bool hasSessionBuild = false;
    BuildInfo sessionBuild;
    std::set<std::wstring> skippedSeen;
    std::shared_ptr<port::Job> portJob;   // a running cosmetics import (one at a time, never while a game runs)
    std::mutex previewM;               // held while catalog.json loads: a preview asked meanwhile waits for it
    port::Previews previews;
    std::vector<std::wstring> previewIds;   // lower-case ids of the last catalog (what the previews keep)
    std::mutex scanM;                  // one PushState at a time
    std::mutex launchM;                // one launch at a time, held until its game exits (2.x ran the prelude on the UI thread)
    std::atomic<bool> updating{ false };   // an update check or install is running
    std::atomic<int> busy{ 0 };        // long jobs running (Busy): adding a build, generating cosmetics

    void async(std::function<void(std::shared_ptr<Ctx>)> f) {
        auto self = shared_from_this();
        runAsync([self, f] { f(self); });
    }

    void log(const std::wstring& s, Level lv = Level::Auto) {
        LogLine l;
        l.ts = nowTime(L"HH:mm:ss");
        l.text = s;
        l.level = lv;
        {
            std::lock_guard<std::mutex> g(sinkM);
            if (sink) sink->onLog(l);
        }
        std::wstring dir;
        {
            std::lock_guard<std::mutex> g(m);
            if (session && hasSessionBuild) dir = sessionBuild.dir;
        }
        if (!dir.empty()) appendFile(join(paths::offline(dir), L"launcher.log"), narrow(nowIso() + L" " + s + L"\r\n"));
    }
    LogFn logger() {
        std::weak_ptr<Ctx> w = shared_from_this();
        return [w](const std::wstring& s) { if (auto c = w.lock()) c->log(s); };
    }
    void toast(const std::wstring& t, Level lv) {
        std::lock_guard<std::mutex> g(sinkM);
        if (sink) sink->onToast(t, lv);
    }

    Session sessionLocked() const {   // m held
        Session s;
        s.state = stateOf(state);
        s.version = sessionVersion;
        s.message = failure;
        for (auto& kv : stages) {
            Stage st = stageOf(kv.second);
            if (kv.first == L"backend") s.backend = st;
            else if (kv.first == L"game") s.game = st;
            else if (kv.first == L"server") { s.server = st; s.hasServer = true; }
        }
        s.phase = phaseOf(phase);
        s.matchDetail = detail;
        return s;
    }
    void postSession() {
        std::lock_guard<std::mutex> g(sinkM);
        Session s;
        {
            std::lock_guard<std::mutex> g2(m);
            s = sessionLocked();
        }
        if (sink) sink->onSession(s);
    }
    void setState(const wchar_t* st, bool clear = false) {
        {
            std::lock_guard<std::mutex> g(m);
            state = st;
            if (clear) { stages.clear(); phase.clear(); detail.clear(); }
        }
        postSession();
    }

    // Web.cs PushState: builds + config for the window. A folder that is not a usable build is listed in Activity
    // with its reason (once) and never empties the list.
    void pushState() { async([](std::shared_ptr<Ctx> c) { c->pushStateNow(); }); }
    void pushStateNow() {
        std::lock_guard<std::mutex> scan(scanM);
        State st;
        st.loaded = true;
        LauncherConfig cfg = LauncherConfig::load();
        std::wstring root = paths::root();
        std::vector<std::pair<std::wstring, std::wstring>> skipped;
        auto all = BuildInfo::scan(root, &skipped);
        for (auto& s : skipped) {
            bool fresh;
            {
                std::lock_guard<std::mutex> g(m);
                fresh = skippedSeen.insert(upper(s.first + L"|" + s.second)).second;
            }
            if (fresh) log(L"Not a build, ignored: " + s.first + L" (" + s.second + L").", Level::Warn);
        }
        firewall::Rules rules;
        if (!all.empty()) rules = firewall::read();
        bool server = fileExists(paths::gameServerDll());
        for (auto& b : all) {
            Build x;
            x.name = b.name();
            x.dir = b.dir;
            x.version = b.version;
            x.major = b.major;
            x.minor = b.minor;
            x.cl = (uint32_t)b.cl;
            x.engine = b.engine;
            auto lob = b.lobby(), mat = b.match();
            x.lobby = lob.first; x.lobbyWhy = lob.second;
            x.match = mat.first; x.matchWhy = mat.second;
            x.matchServer = server;
            x.cosmetics = cosmeticsCount(b.dir);
            x.firewall = orOk(firewall::check(rules, b.dir));
            x.port = port::supports(b);
            x.imported = x.port ? port::importedCount(b.dir) : 0;
            st.builds.push_back(x);
        }
        st.config.has = true;
        st.config.displayName = cfg.displayName;
        st.config.mode = cfg.mode;
        st.config.bots = cfg.bots;
        st.config.botsAI = cfg.botsAI;
        st.config.infiniteMaterials = cfg.infiniteMaterials;
        st.config.infiniteAmmo = cfg.infiniteAmmo;
        st.config.lateGame = cfg.lateGame;
        st.config.playMode = cfg.playMode();
        st.config.profile = cfg.profile();
        st.config.botSkill = cfg.botSkill();
        st.mods = mods::load();
        st.root = root;
        std::lock_guard<std::mutex> g(sinkM);
        {
            std::lock_guard<std::mutex> g2(m);
            st.selected = selected;
            st.hasSession = true;
            st.session = sessionLocked();
        }
        if (sink) sink->onState(st);
    }

    // Web.cs Sel: the build named in the command, else the selected one.
    bool sel(const std::wstring& build, BuildInfo* out) {
        std::wstring name = build;
        if (name.empty()) { std::lock_guard<std::mutex> g(m); name = selected; }
        for (auto& b : BuildInfo::all()) if (b.name() == name) { *out = b; return true; }
        log(L"Pick a build first.", Level::Warn);
        return false;
    }

    void launch(const BuildInfo& b) {
        // a second Launch (double click, Enter held) before the state below turns "launching" must not start a second
        // session and backend on the same port
        std::unique_lock<std::mutex> one(launchM, std::try_to_lock);
        if (!one.owns_lock()) return;
        bool importing = false;
        {
            std::lock_guard<std::mutex> g(m);
            if (state == L"launching" || state == L"running") return;
            if (portJob) { importing = true; }
            else selected = b.name();
        }
        if (importing) { log(L"Wait for the cosmetics import to finish (or stop it) before launching.", Level::Warn); return; }
        {
            std::lock_guard<std::mutex> g(m);
            sessionVersion = b.version;
            sessionBuild = b;
            hasSessionBuild = true;
            failure.clear();
            stages.clear();
            phase.clear();
            detail.clear();
        }
        std::wstring fw = firewall::check(b.dir);
        if (!fw.empty()) {
            log(L"Firewall: " + fw, Level::Warn);
            if (!proc::runElevated(L"--firewall \"" + b.name() + L"\"")) {
                { std::lock_guard<std::mutex> g(m); state = L"error"; failure = L"Not starting without the firewall rules"; }
                postSession();
                return;
            }
            pushState();
        }
        LauncherConfig c = LauncherConfig::load();
        setState(L"launching");
        log(L"Launching Fortnite " + b.version + fmt(L" (CL %lld) - player \"", (long long)b.cl) + c.displayName + fmt(L"\", %d bots in Battle Royale", c.bots));
        auto s = std::make_shared<PlaySession>(b, c, logger());
        std::weak_ptr<Ctx> w = shared_from_this();
        s->stage = [w](const wchar_t* k, const wchar_t* v) {
            auto c = w.lock();
            if (!c) return;
            { std::lock_guard<std::mutex> g(c->m); c->stages[k] = v; }
            c->postSession();
        };
        s->match = [w](const wchar_t* k, const wchar_t* d) {
            auto c = w.lock();
            if (!c) return;
            { std::lock_guard<std::mutex> g(c->m); c->phase = k; c->detail = d; }
            c->postSession();
        };
        { std::lock_guard<std::mutex> g(m); session = s; }
        if (!s->start()) {
            {
                std::lock_guard<std::mutex> g(m);
                state = L"error";
                failure = s->failure.empty() ? L"Launch failed" : s->failure;
                session.reset();
            }
            postSession();
            return;
        }
        setState(L"running");
        if (HWND h = proc::mainWindow()) ShowWindowAsync(h, SW_MINIMIZE);
        s->waitClient();
        std::wstring code = s->clientExitCode();
        if (code == L"0x00000000" || code == L"0x00000001" || code == L"?") log(L"The game closed: stopping the local servers.");
        else log(L"Error: the game crashed (exit code " + code + fmt(L", after %lld s). ", (long long)s->secondsSinceStart()) +
                 L"Details: offline\\redirect-client.log and the game log in offline\\sandbox-client. Stopping the local servers.", Level::Bad);
        std::shared_ptr<PlaySession> cur;
        { std::lock_guard<std::mutex> g(m); cur = session; }
        if (cur) cur->stop();
        { std::lock_guard<std::mutex> g(m); session.reset(); }
        setState(L"idle", true);
        std::wstring autoPlayName;
        { std::lock_guard<std::mutex> g(m); autoPlayName = autoPlay; }
        HWND h = proc::mainWindow();
        if (!autoPlayName.empty()) { if (h) PostMessageW(h, WM_CLOSE, 0, 0); return; }   // shortcut launch: the window goes with the game
        if (h) { ShowWindowAsync(h, SW_SHOWNORMAL); SetForegroundWindow(h); }
    }

    void stop() {
        setState(L"stopping");
        std::shared_ptr<PlaySession> s;
        { std::lock_guard<std::mutex> g(m); s = session; }
        if (s) s->stop();
        stopAll([this](const std::wstring& t) { log(t); });
        setState(L"idle", true);
        pushState();
    }

    // Web.cs RunUpdateAsync: offered, downloaded, verified and run; never while a game runs. periodic = the test
    // channel's poll (update.h): the feed alone, and not while the window is in use (active, input in the last 2 min).
    void runUpdate(bool manual, bool periodic = false) {
        bool expected = false;
        if (!updating.compare_exchange_strong(expected, true)) return;   // one check / install at a time
        bool foreground = false;
        bool canInstall = installAllowed(&foreground);
        if (periodic && foreground) {
            LASTINPUTINFO li = { sizeof li };
            if (GetLastInputInfo(&li) && GetTickCount() - li.dwTime < 2 * 60 * 1000) { updating = false; return; }
        }
        std::weak_ptr<Ctx> w = shared_from_this();
        auto recheck = [w](bool* fg) { auto c = w.lock(); return c && c->installAllowed(fg); };
        if (update::runInteractive(proc::mainWindow(), manual, canInstall, [w](const std::wstring& t, Level l) { if (auto c = w.lock()) c->toast(t, l); },
                                   periodic, recheck)) {
            std::shared_ptr<PlaySession> s;
            { std::lock_guard<std::mutex> g(m); s = session; }
            if (s) { s->stop(); stopAll([](const std::wstring&) {}); }
            if (HWND mw = proc::mainWindow()) PostMessageW(mw, WM_CLOSE, 0, 0);
            return;   // stays "updating": the window is closing
        }
        updating = false;
    }

    // Nothing an install would cut: no game (ours or not), no session, no import, no long job. *foreground = the
    // window (or one of its dialogs) is the active one.
    bool installAllowed(bool* foreground) {
        DWORD fgPid = 0;
        if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &fgPid);
        *foreground = fgPid == GetCurrentProcessId();
        {
            std::lock_guard<std::mutex> g(m);
            if ((session && session->running()) || state != L"idle" || portJob) return false;
        }
        if (busy > 0) return false;   // a build being added, cosmetics being generated
        for (auto& p : proc::list())
            if (ieq(p.exe, L"FortniteClient-Win64-Shipping.exe")) return false;
        return true;
    }

    // The test channel: the feed polled every 2 minutes while the window is open (update.h); off = no thread.
    void startTestPoll() {
        if (update::testFeed().empty()) return;
        std::weak_ptr<Ctx> w = shared_from_this();
        runAsync([w] {
            for (;;) {
                for (int i = 0; i < 120; i++) { Sleep(1000); if (w.expired()) return; }
                auto c = w.lock();
                if (!c) return;
                c->runUpdate(false, true);
            }
        });
    }

    // After a test build installed by the updater: say so once (or that it did not take).
    void reportTestInstall() {
        std::wstring want = update::takeInstalledTest();
        if (want.empty()) return;
        if (want == update::current().str()) toast(L"Updated to test build " + want, Level::Ok);
        else log(L"Test build " + want + L" did not install (still " + update::current().str() + L"; see update.log).", Level::Warn);
    }

    // RAII count of long jobs that the test channel's silent install must not cut (adding a build, cosmetics)
    struct Busy {
        std::atomic<int>& n;
        explicit Busy(std::atomic<int>& x) : n(x) { n++; }
        ~Busy() { n--; }
    };

    void afterAdd(const BuildInfo& b) {
        { std::lock_guard<std::mutex> g(m); selected = b.name(); }
        auto lob = b.lobby(), mat = b.match();
        log(L"Fortnite " + b.version + L": lobby " + supportName(lob.first) + L" (" + lob.second + L"); local match " + supportName(mat.first) + L" (" + mat.second + L").");
        pushState();
        if (lob.first == Support::No) return;
        log(L"Generating the cosmetics list from the build's own data...");
        std::wstring why;
        log(builddata::generateCosmetics(b, logger(), &why) ? L"Cosmetics ready." : L"Could not generate the cosmetics: " + why + L".");
        if (!firewall::check(b.dir).empty() &&
            MessageBoxW(proc::mainWindow(), (L"Create the firewall rules for " + b.version + L" now? (asks for administrator permission)").c_str(),
                        L"Radarnite Launcher", MB_YESNO) == IDYES)
            proc::runElevated(L"--firewall \"" + b.name() + L"\"");
        pushState();
    }

    // ---- cosmetics of the current game (Web.cs portCatalog / portImport / portCancel)
    void postPortJob() {
        std::shared_ptr<port::Job> j;
        { std::lock_guard<std::mutex> g(m); j = portJob; }
        std::shared_ptr<const PortJob> v = j ? j->view() : nullptr;
        std::lock_guard<std::mutex> g(sinkM);
        if (sink) sink->onPortJob(v);
    }
    void postCatalog(std::shared_ptr<const PortCatalog> c) {
        std::lock_guard<std::mutex> g(sinkM);
        if (sink) sink->onPortCatalog(c);
    }
    void rememberIds(const PortCatalog& c) {
        std::vector<std::wstring> ids;
        ids.reserve(c.items.size());
        for (auto& it : c.items) {
            std::wstring l = it.id;
            for (auto& ch : l) if (ch >= L'A' && ch <= L'Z') ch = (wchar_t)(ch - L'A' + L'a');
            ids.push_back(std::move(l));
        }
        std::lock_guard<std::mutex> g(m);
        previewIds.swap(ids);
    }
    std::vector<std::wstring> idsCopy() { std::lock_guard<std::mutex> g(m); return previewIds; }

    void portCatalog(const std::wstring& build, bool refresh) {
        BuildInfo b;
        if (!sel(build, &b)) return;
        std::wstring name = b.name();
        std::weak_ptr<Ctx> w = shared_from_this();
        auto cat = port::catalog(b, refresh, logger(), [w, name](const wchar_t* stage, int64_t ms, bool done) {
            auto c = w.lock();
            if (!c) return;
            PortLoad l; l.build = name; l.stage = stage; l.ms = ms; l.done = done;
            std::lock_guard<std::mutex> g(c->sinkM);
            if (c->sink) c->sink->onPortLoad(l);
        }, [w](std::shared_ptr<PortCatalog> early) {
            if (auto c = w.lock()) { c->rememberIds(*early); c->postCatalog(early); }
        });
        rememberIds(*cat);
        postCatalog(cat);
        postPortJob();
        // the previews (radarnite.com's catalog.json, a day), only once the grid is on screen
        if (!cat->error.empty()) return;
        std::lock_guard<std::mutex> g(previewM);
        previews.load(idsCopy(), logger());
    }
    void portPreview(const std::wstring& id) {
        std::lock_guard<std::mutex> g(previewM);
        if (!previews.loaded()) previews.load(idsCopy(), logger());
        PortPreview p = previews.get(id);
        std::lock_guard<std::mutex> g2(sinkM);
        if (sink) sink->onPortPreview(p);
    }

    void portImport(const std::wstring& build, const std::vector<std::wstring>& paths) {
        BuildInfo b;
        if (!sel(build, &b)) return;
        std::shared_ptr<port::Job> job;
        bool running = false, game = false;
        {
            std::lock_guard<std::mutex> g(m);
            running = portJob != nullptr;
            game = state == L"launching" || state == L"running" || state == L"stopping";
            if (!running && !game && !paths.empty()) portJob = job = std::make_shared<port::Job>();
        }
        // refused: the window already closed its dialog, so a toast says why nothing starts
        if (running) { log(L"A cosmetics import is already running.", Level::Warn); toast(L"A cosmetics import is already running", Level::Warn); return; }
        if (game) { log(L"Stop the game before importing cosmetics.", Level::Warn); toast(L"Stop the game before importing cosmetics", Level::Warn); return; }
        if (!job) return;
        job->build = b.version;   // the first view already names the build ("IMPORTING INTO 14.30")
        postPortJob();
        std::weak_ptr<Ctx> w = shared_from_this();
        auto last = std::make_shared<std::atomic<ULONGLONG>>(0);
        bool ok = port::importItems(b, paths, job, logger(), [w, last] {
            ULONGLONG now = GetTickCount64(), was = *last;
            if (now - was < 400 || !last->compare_exchange_strong(was, now)) return;
            if (auto c = w.lock()) c->postPortJob();
        });
        { std::lock_guard<std::mutex> g(m); portJob.reset(); }
        int done = job->okCount();
        toast(job->cancelled ? fmt(L"Import stopped: %d imported", done) : ok ? fmt(L"%d cosmetics imported into ", done) + b.version : L"The import failed (see Activity)",
              ok ? Level::Ok : job->cancelled ? Level::Info : Level::Bad);
        {   // its last view, marked done / stopped / failed (the window keeps it as the finished card), then no job
            auto fin = job->view();
            fin->phase = job->cancelled ? L"stopped" : ok ? L"done" : L"failed";
            std::lock_guard<std::mutex> g(sinkM);
            if (sink) sink->onPortJob(fin);
        }
        postPortJob();
        pushState();
    }
    void portCancel() {
        std::shared_ptr<port::Job> j;
        { std::lock_guard<std::mutex> g(m); j = portJob; }
        if (j) j->cancel();
    }

    void command(Command cmd, const std::wstring& build) {
        switch (cmd) {
        case Command::Select: { std::lock_guard<std::mutex> g(m); selected = build; } break;
        case Command::Refresh: pushState(); break;
        case Command::Launch:
            async([build](std::shared_ptr<Ctx> c) { BuildInfo b; if (c->sel(build, &b)) c->launch(b); });
            break;
        case Command::Stop: async([](std::shared_ptr<Ctx> c) { c->stop(); }); break;
        case Command::CheckUpdates: async([](std::shared_ptr<Ctx> c) { c->runUpdate(true); }); break;
        case Command::AddZip:
            async([](std::shared_ptr<Ctx> c) {
                std::wstring f;
                Busy b_(c->busy);
                if (!pickPath(proc::mainWindow(), L"Add a Fortnite build", false, L"", &f)) return;
                c->log(L"Extracting " + f + L" into " + paths::root() + L" (this can take a long time)...");
                BuildInfo b;
                if (builddata::addZip(f, c->logger(), &b)) c->afterAdd(b);
            });
            break;
        case Command::AddFolder:
            async([](std::shared_ptr<Ctx> c) {
                std::wstring d;
                Busy b_(c->busy);
                if (!pickPath(proc::mainWindow(), L"Build folder (the one that contains FortniteGame)", true, L"", &d)) return;
                BuildInfo b;
                if (builddata::addFolder(d, c->logger(), &b)) c->afterAdd(b);
            });
            break;
        case Command::ChooseRoot:
            async([](std::shared_ptr<Ctx> c) {
                bool busy;
                {
                    std::lock_guard<std::mutex> g(c->m);
                    busy = c->state == L"launching" || c->state == L"running" || c->state == L"stopping";
                }
                if (busy) { c->log(L"Stop the game before changing the builds folder."); return; }
                Busy b_(c->busy);
                std::wstring d;
                if (!pickPath(proc::mainWindow(), L"Folder for your Fortnite builds (one folder per build)", true, paths::root(), &d)) return;
                LauncherConfig cfg = LauncherConfig::load();
                cfg.buildsFolder = d;
                cfg.nulls &= ~(1u << 13);
                cfg.save();
                c->log(L"Builds folder: " + paths::root());
                { std::lock_guard<std::mutex> g(c->m); c->selected.clear(); }
                c->pushState();
            });
            break;
        case Command::Cosmetics:
            async([build](std::shared_ptr<Ctx> c) {
                BuildInfo b;
                if (!c->sel(build, &b)) return;
                Busy b_(c->busy);
                c->toast(L"Generating cosmetics for " + b.version + L"… (a few minutes)", Level::Info);
                c->log(L"Generating the cosmetics of " + b.version + L" from the build's own data...");
                std::wstring why;
                bool ok = builddata::generateCosmetics(b, c->logger(), &why);
                if (ok) c->log(L"Cosmetics ready.");
                else c->log(L"Could not generate the cosmetics of " + b.version + L": " + why + L" (log: " + join(paths::offline(b.dir), L"cosmetics-gen.log") + L").", Level::Bad);
                c->toast(ok ? L"Cosmetics ready" : L"Could not generate the cosmetics: " + why, ok ? Level::Ok : Level::Bad);
                c->pushState();
            });
            break;
        case Command::Firewall:
            async([build](std::shared_ptr<Ctx> c) {
                BuildInfo b;
                if (!c->sel(build, &b)) return;
                bool ok = proc::runElevated(L"--firewall \"" + b.name() + L"\"");
                if (ok) c->log(L"Firewall rules created.");
                else c->log(L"The rules were not created (was the administrator prompt cancelled?).", Level::Warn);
                c->pushState();
            });
            break;
        case Command::Shortcut:
            async([build](std::shared_ptr<Ctx> c) {
                BuildInfo b;
                if (!c->sel(build, &b)) return;
                std::wstring path, error;
                if (!createShortcut(b, &path, &error)) { c->log(L"Error: could not create the shortcut " + path + L": " + error, Level::Bad); return; }
                c->log(L"Shortcut created: " + path);
                c->toast(L"Desktop shortcut created", Level::Ok);
            });
            break;
        case Command::OpenLogs:
            async([build](std::shared_ptr<Ctx> c) {
                BuildInfo b;
                if (c->sel(build, &b)) proc::open(L"explorer.exe", L"\"" + paths::offline(b.dir) + L"\"");
            });
            break;
        case Command::OpenConfig: proc::open(L"notepad.exe", L"\"" + paths::config() + L"\""); break;
        }
    }
};

class CoreImpl : public Core {
public:
    CoreImpl(const wchar_t* autoPlay, bool automated) : c_(std::make_shared<Ctx>()) {
        if (autoPlay && *autoPlay) { c_->autoPlay = autoPlay; c_->selected = autoPlay; c_->automated = automated; }
    }

    ~CoreImpl() override {
        { std::lock_guard<std::mutex> g(c_->sinkM); c_->sink = nullptr; }
        // closing the window stops the game and the local servers (Web.cs FormClosing)
        std::shared_ptr<PlaySession> s;
        { std::lock_guard<std::mutex> g(c_->m); s = c_->session; }
        if (s) { s->stop(); stopAll([](const std::wstring&) {}); }
        // a cosmetics import stops after the batch being installed (importing again continues), like 2.x's close
        std::shared_ptr<port::Job> j;
        { std::lock_guard<std::mutex> g(c_->m); j = c_->portJob; }
        if (j) j->cancelAndWait(120000);
    }

    void ready(UiSink* sink) override {
        { std::lock_guard<std::mutex> g(c_->sinkM); c_->sink = sink; }
        c_->pushState();
        std::wstring play = c_->autoPlay;
        if (!play.empty()) {
            c_->async([play](std::shared_ptr<Ctx> c) {
                // a script's or an agent's launch: never next to an anti-cheat or a game the owner has open, checked
                // before anything else (game-guard.ps1, paktools.h); the owner's own Play is never held back
                if (c->automated) {
                    std::wstring busy = paktools::gameGuard();
                    if (!busy.empty()) {
                        c->log(L"Not starting this automated launch: " + busy + L".", Level::Bad);
                        g_exitCode = 6;
                        if (HWND h = proc::mainWindow()) PostMessageW(h, WM_CLOSE, 0, 0);
                        return;
                    }
                    c->log(L"Game guard: no anti-cheat or other game running.");
                }
                // Program.Find: the folder name (any case) or the exact version
                for (auto& b : BuildInfo::all()) {
                    if (ieq(b.name(), play) || b.version == play) { c->launch(b); return; }
                }
                HWND h = proc::mainWindow();
                MessageBoxW(h, (L"Build " + play + L" not found in " + paths::root() + L".").c_str(), L"Radarnite Launcher", MB_OK);
                if (h) PostMessageW(h, WM_CLOSE, 0, 0);
            });
        } else {
            c_->async([](std::shared_ptr<Ctx> c) {
                paktools::syncDevTree([c](const std::wstring& t) { c->log(t); });   // a dev tree's sigfit stays current
                c->reportTestInstall();
                c->runUpdate(false);   // offline-first, silent without network
                c->startTestPoll();
            });
        }
    }

    void command(Command cmd, const std::wstring& build) override { c_->command(cmd, build); }

    void saveConfig(const ConfigPatch& p) override {
        LauncherConfig c = LauncherConfig::load();
        const Config& v = p.values;
        if (p.fields & ConfigPatch::DisplayName) { c.displayName = trim(v.displayName); c.nulls &= ~1u; }
        if (p.fields & ConfigPatch::Mode) { c.mode = v.mode; c.nulls &= ~(1u << 3); }
        if (p.fields & ConfigPatch::Bots) c.bots = v.bots < 0 ? 0 : v.bots > 99 ? 99 : v.bots;
        if (p.fields & ConfigPatch::BotsAI) c.botsAI = v.botsAI;
        if (p.fields & ConfigPatch::InfiniteMaterials) c.infiniteMaterials = v.infiniteMaterials;
        if (p.fields & ConfigPatch::InfiniteAmmo) c.infiniteAmmo = v.infiniteAmmo;
        if (p.fields & ConfigPatch::LateGame) c.lateGame = v.lateGame;
        if (p.fields & ConfigPatch::PlayMode) c.setExtraStr("playMode", v.playMode);
        if (p.fields & ConfigPatch::BotSkill) c.setExtraInt("botSkill", v.botSkill < 1 ? 1 : v.botSkill > 10 ? 10 : v.botSkill);
        if (p.fields & ConfigPatch::Profile) { std::wstring n = trim(v.profile).substr(0, 24); c.setExtraStr("profile", n.empty() ? L"Main" : n); }
        c.save();
    }

    void saveMods(const Mods& m) override {
        if (!mods::save(m)) c_->log(L"Mods: could not save " + mods::path() + L".", Level::Warn);
    }

    void portCatalog(const std::wstring& build, bool refresh) override {
        c_->async([build, refresh](std::shared_ptr<Ctx> c) { c->portCatalog(build, refresh); });
    }
    void portImport(const std::wstring& build, const std::vector<std::wstring>& paths) override {
        c_->async([build, paths](std::shared_ptr<Ctx> c) { c->portImport(build, paths); });
    }
    void portCancel() override { c_->async([](std::shared_ptr<Ctx> c) { c->portCancel(); }); }
    void portPreview(const std::wstring& id) override { c_->async([id](std::shared_ptr<Ctx> c) { c->portPreview(id); }); }
    std::wstring imageCacheDir() override { return join(port::work(), L"img"); }

    std::wstring uiPref(const wchar_t* key) override { return ::rn::uiPref(key); }
    void setUiPref(const wchar_t* key, const std::wstring& value) override { ::rn::setUiPref(key, value); }

private:
    std::shared_ptr<Ctx> c_;
};

} // namespace

Core* createCore(const wchar_t* autoPlay, bool automated) { return new CoreImpl(autoPlay, automated); }

namespace core {
bool AutomatedStart() {
    if (getEnv(L"RADARNITE_AUTOMATED") == L"1") return true;
    STARTUPINFOW si = { sizeof si };
    GetStartupInfoW(&si);
    return (si.dwFlags & STARTF_USESHOWWINDOW) && si.wShowWindow == SW_HIDE;
}
int ExitCode() { return g_exitCode; }
} // namespace core

} // namespace rn
