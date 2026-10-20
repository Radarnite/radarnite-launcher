// Mock core: the scenarios of tests/ui-scenarios.mjs, plus an interactive mode where launch/stop and the menu
// commands are simulated with timers (callbacks on thread-pool threads, like the real core would).
#include "api.h"
#include <windows.h>
#include <functional>
#include <map>
#include <stdio.h>

namespace rn {
namespace {

Build b1750() {
    Build b;
    b.name = L"17.50"; b.dir = L"S:\\FortniteBuilds\\17.50"; b.version = L"17.50"; b.major = 17; b.minor = 50; b.cl = 17388565; b.engine = L"4.26";
    b.lobby = Support::Yes; b.lobbyWhy = L"UE4 (3.x-19.x): curl redirect + -noeac launch";
    b.match = Support::Experimental; b.matchWhy = L"Reboot: S16-S19 and S1-S2 'might not work perfectly'";
    // ui-scenarios.mjs never sets matchServer (the page then shows "match server missing"): kept identical here
    b.matchServer = false; b.cosmetics = 5338; b.firewall = L"OK";
    return b;
}
Build mk(const wchar_t* v, int maj, int min, uint32_t cl, const wchar_t* eng) {
    Build b = b1750();
    b.name = v; b.dir = std::wstring(L"S:\\FortniteBuilds\\") + v; b.version = v; b.major = maj; b.minor = min; b.cl = cl; b.engine = eng;
    return b;
}
const wchar_t* kS3 = L"Reboot: S3-S15 supported";
const wchar_t* kNoServer = L"no match server for this version (Reboot covers S1-S19 only)";

std::vector<Build> many() {
    std::vector<Build> v;
    Build a = mk(L"3.5", 3, 5, 4166199, L"4.20"); a.match = Support::Yes; a.matchWhy = kS3; a.cosmetics = 412; v.push_back(a);
    Build b = mk(L"7.40", 7, 40, 5046157, L"4.22"); b.match = Support::Yes; b.matchWhy = kS3; b.cosmetics = 1530; v.push_back(b);
    Build c = mk(L"12.41", 12, 41, 12905909, L"4.24"); c.match = Support::Yes; c.matchWhy = kS3; c.cosmetics = 0;
    c.firewall = L"firewall rules missing for 9 programs (e.g. FortniteLauncher.exe)"; v.push_back(c);
    v.push_back(b1750());
    Build d = mk(L"21.10", 21, 10, 21543261, L"5.1"); d.lobby = Support::Experimental; d.lobbyWhy = L"UE5 (20.x-23.x): curl redirect likely, untested";
    d.match = Support::No; d.matchWhy = kNoServer; d.cosmetics = 7702; v.push_back(d);
    Build e = mk(L"25.11", 25, 11, 26787151, L"5.3"); e.lobby = Support::No;
    e.lobbyWhy = L"uses EOS Easy Anti-Cheat and EOS login (24.x+): this method does not apply";
    e.match = Support::No; e.matchWhy = kNoServer; e.cosmetics = 0; v.push_back(e);
    return v;
}
Config cfg() {
    Config c; c.has = true; c.displayName = L"testingacc"; c.mode = L"practice"; c.bots = 20;
    c.botsAI = false; c.infiniteMaterials = true; c.infiniteAmmo = false; c.lateGame = false;
    return c;
}
std::vector<LogLine> logs(int n) {
    std::vector<LogLine> v;
    wchar_t buf[400];
    for (int i = 0; i < n; i++) {
        LogLine l;
        Level lv = i % 37 == 0 ? Level::Bad : i % 11 == 0 ? Level::Warn : Level::Auto;
        if (lv == Level::Bad) swprintf(buf, 400, L"Error: the redirect did not confirm (see offline\\redirect-client.log). Stopped for safety. (#%d)", i);
        else if (lv == Level::Warn) swprintf(buf, 400, L"Warning: the match server did not get ready (see offline\\userdir-server\\Saved\\Logs). The lobby still works. (#%d)", i);
        else if (i % 5 == 0) swprintf(buf, 400, L"redirect https://fortnite-public-service-prod11.ol.epicgames.com/fortnite/api/game/v2/profile/4e8efc51eb94432fb4b584c6ae052f83/client/QueryProfile?profileId=athena&rvn=-1 -> http://127.0.0.1:3551/... (#%d)", i);
        else swprintf(buf, 400, L"Backend ready on 127.0.0.1:3551 (local account \"testingacc\"). (#%d)", i);
        l.text = buf; l.level = lv;
        swprintf(buf, 400, L"22:%02d:%02d", 10 + (i / 60) % 50, i % 60);
        l.ts = buf;
        v.push_back(l);
    }
    return v;
}
Session running(MatchPhase ph = MatchPhase::None, const wchar_t* detail = L"") {
    Session s; s.state = SessionState::Running; s.version = L"17.50";
    s.backend = s.server = s.game = Stage::Ok; s.hasServer = true; s.phase = ph; s.matchDetail = detail;
    return s;
}

const MockScenario kScenarios[] = {
    { L"01-one-build-practice-100", 1200, 800, 1.0f, false },
    { L"02-many-builds-bots-activity-125", 960, 640, 1.25f, true },
    { L"03-launching-150", 800, 534, 1.5f, false },
    { L"04-notices-firewall-missing", 1200, 800, 1.0f, false },
    { L"05-unsupported", 1200, 800, 1.0f, false },
    { L"06-error-state", 1200, 800, 1.0f, false },
    { L"07-running", 1200, 800, 1.0f, false },
    { L"08-empty", 1200, 800, 1.0f, false },
    { L"09-minimum-760x520", 760, 520, 1.0f, false },
    { L"10-loading", 1200, 800, 1.0f, false },
    { L"12-bots-no-ai-125", 1180, 760, 1.25f, false },
    { L"13-bots-ai-on-warning", 1200, 800, 1.0f, false },
    { L"14-match-waiting", 1200, 800, 1.0f, false },
    { L"15-match-warmup-150", 880, 587, 1.5f, false },
    { L"16-match-storm", 1200, 800, 1.0f, false },
    { L"17-match-server-down", 1200, 800, 1.0f, true },
    { L"18-real-root-3.6-14.30-17.50", 1200, 800, 1.25f, true },
    { L"19-state-without-config", 1200, 800, 1.0f, false },
    { L"11-large-1920x1080", 1920, 1040, 1.0f, true },
    // native-only additions: the bots form only shows with the match server present
    { L"20-match-server-bots-form", 1200, 800, 1.0f, false },
    { L"21-match-server-ai-on-900-125", 900, 640, 1.25f, false },
    { L"22-build-menu-toast", 1200, 800, 1.0f, false, 2 },
    { L"23-keyboard-focus-switch", 1200, 800, 1.0f, false, 0, 18 },
};

// Timer-queue callback carrying a std::function (thread-pool thread).
struct Later { std::function<void()> f; HANDLE timer = nullptr; };
void CALLBACK laterCb(PVOID p, BOOLEAN) { auto* l = (Later*)p; l->f(); }

class MockCore final : public Core {
public:
    explicit MockCore(const wchar_t* scenario) : id_(scenario ? scenario : L"") { InitializeCriticalSection(&cs_); queue_ = CreateTimerQueue(); }
    ~MockCore() override {
        DeleteTimerQueueEx(queue_, INVALID_HANDLE_VALUE);   // waits for running callbacks
        for (auto* l : laters_) delete l;
        DeleteCriticalSection(&cs_);
    }

    void ready(UiSink* sink) override {
        sink_ = sink;
        State st; st.loaded = true; st.root = L"S:\\FortniteBuilds"; st.config = cfg();
        std::vector<LogLine> pre, post;
        bool hasSession = false; Session se;
        auto one = [] { return std::vector<Build>{ b1750() }; };
        const std::wstring& id = id_;
        auto withServer = [](std::vector<Build> v) { for (auto& b : v) b.matchServer = true; return v; };
        if (id.empty()) { st.builds = withServer(many()); st.selected = L"17.50"; st.config.mode = L"bots"; post = logs(8); }
        else if (id.rfind(L"01-", 0) == 0) { st.builds = one(); st.selected = L"17.50"; }
        else if (id.rfind(L"02-", 0) == 0) { st.builds = many(); st.config.mode = L"bots"; st.config.bots = 35; st.selected = L"17.50"; post = logs(400); }
        else if (id.rfind(L"03-", 0) == 0) {
            st.builds = many(); st.config.mode = L"bots"; st.selected = L"17.50";
            hasSession = true; se.state = SessionState::Launching; se.version = L"17.50"; se.backend = Stage::Ok; se.server = Stage::Run; se.hasServer = true;
        }
        else if (id.rfind(L"04-", 0) == 0) { st.builds = many(); st.config.mode = L"lobby"; st.config.displayName = L"AVeryLongPlayerNameThatGoesOn"; st.selected = L"12.41"; }
        else if (id.rfind(L"05-", 0) == 0) { st.builds = many(); st.selected = L"25.11"; }
        else if (id.rfind(L"06-", 0) == 0) {
            st.builds = one(); st.selected = L"17.50"; post = logs(30);
            hasSession = true; se.state = SessionState::Error; se.version = L"17.50"; se.backend = Stage::Bad;
            se.message = L"Not starting: The Epic Games Launcher is open: quit it completely (also from the tray) and try again.";
        }
        else if (id.rfind(L"07-", 0) == 0) { st.builds = one(); st.config.mode = L"bots"; st.selected = L"17.50"; hasSession = true; se = running(); }
        else if (id.rfind(L"08-", 0) == 0) { st.selected = L""; }
        else if (id.rfind(L"09-", 0) == 0) { st.builds = many(); st.config.mode = L"bots"; st.selected = L"17.50"; }
        else if (id.rfind(L"10-", 0) == 0) { return; }   // loading: the host never answers
        else if (id.rfind(L"12-", 0) == 0) { st.builds = one(); st.config.mode = L"bots"; st.selected = L"17.50"; }
        else if (id.rfind(L"13-", 0) == 0) { st.builds = one(); st.config.mode = L"bots"; st.config.botsAI = true; st.selected = L"17.50"; }
        else if (id.rfind(L"14-", 0) == 0) { st.builds = one(); st.config.mode = L"bots"; st.selected = L"17.50"; hasSession = true; se = running(MatchPhase::Waiting, L"Waiting for you: press PLAY in the game"); }
        else if (id.rfind(L"15-", 0) == 0) { st.builds = one(); st.config.mode = L"bots"; st.selected = L"17.50"; hasSession = true; se = running(MatchPhase::Warmup, L"Warm-up island \u00b7 bus in 60 s"); }
        else if (id.rfind(L"16-", 0) == 0) { st.builds = one(); st.config.mode = L"bots"; st.selected = L"17.50"; hasSession = true; se = running(MatchPhase::Storm, L"Storm"); }
        else if (id.rfind(L"17-", 0) == 0) { st.builds = one(); st.config.mode = L"bots"; st.selected = L"17.50"; hasSession = true; se = running(MatchPhase::Down, L"The match server stopped"); post = logs(12); }
        else if (id.rfind(L"18-", 0) == 0) {
            Build a = mk(L"3.6-CL-4019403", 3, 6, 4019403, L"4.20"); a.dir = L"S:\\FortniteBuilds\\3.6-CL-4019403\\Fortnite"; a.match = Support::Yes; a.matchWhy = kS3; a.cosmetics = 0; a.firewall = L"firewall rules missing for 6 programs";
            Build b = mk(L"14.30", 14, 30, 15341163, L"4.26"); b.match = Support::Yes; b.matchWhy = kS3; b.cosmetics = 4100;
            st.builds = { a, b, b1750() }; st.selected = L"3.6-CL-4019403";
            LogLine l; l.level = Level::Warn; l.ts = L"22:09:58";
            l.text = L"Not a build, ignored: S:\\FortniteBuilds\\14.30-damaged (FortniteGame\\Binaries\\Win64\\FortniteClient-Win64-Shipping.exe is missing (incomplete extraction?)).";
            post.push_back(l);
        }
        else if (id.rfind(L"19-", 0) == 0) { st.builds = one(); st.selected = L"17.50"; st.config = Config(); }
        else if (id.rfind(L"11-", 0) == 0) { st.builds = many(); st.config.mode = L"lobby"; st.selected = L"7.40"; post = logs(60); }
        else if (id.rfind(L"20-", 0) == 0) { st.builds = withServer(many()); st.config.mode = L"bots"; st.config.bots = 35; st.selected = L"7.40"; }
        else if (id.rfind(L"23-", 0) == 0) { st.builds = withServer(many()); st.config.mode = L"bots"; st.selected = L"17.50"; }
        else if (id.rfind(L"21-", 0) == 0) { st.builds = withServer(many()); st.config.mode = L"bots"; st.config.botsAI = true; st.selected = L"17.50"; }
        else if (id.rfind(L"22-", 0) == 0) { st.builds = withServer(many()); st.selected = L"12.41"; post = logs(5); }
        lock(); state_ = st; unlock();
        sink_->onState(st);
        for (auto& l : pre) sink_->onLog(l);
        for (auto& l : post) sink_->onLog(l);
        if (hasSession) { lock(); session_ = se; unlock(); sink_->onSession(se); }
        if (id.rfind(L"22-", 0) == 0) sink_->onToast(L"Desktop shortcut created", Level::Ok);
    }

    void command(Command c, const std::wstring& build) override {
        switch (c) {
        case Command::Select: lock(); state_.selected = build; unlock(); break;
        case Command::Launch: launch(build); break;
        case Command::Stop: stop(); break;
        case Command::Refresh: { lock(); State s = state_; unlock(); sink_->onState(s); log(L"Rescanned the builds folder."); break; }
        case Command::CheckUpdates: toast(L"Radarnite Launcher is up to date (mock)", Level::Ok); break;
        case Command::AddZip: log(L"Mock: the zip picker would open here."); break;
        case Command::AddFolder: log(L"Mock: the folder picker would open here."); break;
        case Command::ChooseRoot: log(L"Mock: the builds folder picker would open here."); break;
        case Command::Cosmetics:
            toast(L"Generating cosmetics for " + build + L"\u2026 (a few minutes)", Level::Info);
            log(L"Generating the cosmetics of " + build + L" from the build's own data...");
            after(1500, [this, build] {
                lock(); for (auto& b : state_.builds) if (b.name == build && !b.cosmetics) b.cosmetics = 1234; State s = state_; unlock();
                log(L"Cosmetics ready."); toast(L"Cosmetics ready", Level::Ok); sink_->onState(s);
            });
            break;
        case Command::Firewall:
            after(400, [this, build] {
                lock(); for (auto& b : state_.builds) if (b.name == build) b.firewall = L"OK"; State s = state_; unlock();
                log(L"Firewall rules created."); sink_->onState(s);
            });
            break;
        case Command::Shortcut: log(L"Shortcut created on the desktop: Fortnite " + build + L" Offline.lnk (mock)"); toast(L"Desktop shortcut created", Level::Ok); break;
        case Command::OpenLogs: log(L"Mock: Explorer would open the build's offline folder."); break;
        case Command::OpenConfig: log(L"Mock: Notepad would open config.json."); break;
        }
    }

    void saveConfig(const ConfigPatch& p) override {
        lock();
        Config& c = state_.config;
        c.has = true;
        if (p.fields & ConfigPatch::DisplayName) c.displayName = p.values.displayName;
        if (p.fields & ConfigPatch::Mode) c.mode = p.values.mode;
        if (p.fields & ConfigPatch::Bots) c.bots = p.values.bots;
        if (p.fields & ConfigPatch::BotsAI) c.botsAI = p.values.botsAI;
        if (p.fields & ConfigPatch::InfiniteMaterials) c.infiniteMaterials = p.values.infiniteMaterials;
        if (p.fields & ConfigPatch::InfiniteAmmo) c.infiniteAmmo = p.values.infiniteAmmo;
        if (p.fields & ConfigPatch::LateGame) c.lateGame = p.values.lateGame;
        unlock();
    }

    std::wstring uiPref(const wchar_t* key) override { lock(); auto it = prefs_.find(key); std::wstring v = it == prefs_.end() ? L"" : it->second; unlock(); return v; }
    void setUiPref(const wchar_t* key, const std::wstring& v) override { lock(); prefs_[key] = v; unlock(); }

private:
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
    void log(const std::wstring& t, Level l = Level::Auto) { LogLine x; x.text = t; x.level = l; sink_->onLog(x); }
    void toast(const std::wstring& t, Level l) { sink_->onToast(t, l); }
    void after(DWORD ms, std::function<void()> f) {
        auto* l = new Later{ std::move(f) };
        lock(); laters_.push_back(l); unlock();
        CreateTimerQueueTimer(&l->timer, queue_, laterCb, l, ms, 0, WT_EXECUTEONLYONCE);
    }
    void setSession(const std::function<void(Session&)>& f) { lock(); f(session_); Session s = session_; unlock(); sink_->onSession(s); }

    void launch(const std::wstring& build) {
        lock(); bool busy = session_.state == SessionState::Launching || session_.state == SessionState::Running; std::wstring ver = build;
        for (auto& b : state_.builds) if (b.name == build) ver = b.version;
        unlock();
        if (busy) return;
        log(L"Launching Fortnite " + ver + L" - player \"testingacc\" (mock)");
        setSession([&](Session& s) { s = Session(); s.state = SessionState::Launching; s.version = ver; s.backend = Stage::Run; });
        after(600, [this] { log(L"Backend ready on 127.0.0.1:3551 (local account \"testingacc\")."); setSession([](Session& s) { s.backend = Stage::Ok; s.server = Stage::Run; s.hasServer = true; }); });
        after(1400, [this] { log(L"Match server ready."); setSession([](Session& s) { s.server = Stage::Ok; s.game = Stage::Run; }); });
        after(2200, [this, ver] {
            log(L"Fortnite " + ver + L" started.");
            setSession([](Session& s) { s.game = Stage::Ok; s.state = SessionState::Running; s.phase = MatchPhase::Waiting; s.matchDetail = L"Waiting for you: press PLAY in the game"; });
        });
    }
    void stop() {
        setSession([](Session& s) { s.state = SessionState::Stopping; });
        after(700, [this] { log(L"The game closed: stopping the local servers."); setSession([](Session& s) { s = Session(); }); });
    }

    std::wstring id_;
    UiSink* sink_ = nullptr;
    CRITICAL_SECTION cs_;
    HANDLE queue_ = nullptr;
    std::vector<Later*> laters_;
    State state_;
    Session session_;
    std::map<std::wstring, std::wstring> prefs_;
};

} // namespace

Core* createMockCore(const wchar_t* scenario) { return new MockCore(scenario); }
const MockScenario* mockScenarios(int* count) { *count = (int)(sizeof(kScenarios) / sizeof(kScenarios[0])); return kScenarios; }

} // namespace rn
