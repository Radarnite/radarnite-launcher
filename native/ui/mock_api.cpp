// Mock core: the scenarios of tests/ui-scenarios.mjs, plus an interactive mode where launch/stop and the menu
// commands are simulated with timers (callbacks on thread-pool threads, like the real core would).
#include "api.h"
#include "../core/sheet.h"
#include <windows.h>
#include <functional>
#include <map>
#include <memory>
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
// The lobby screen's mockup: 3.6, 14.30 and 17.50 with the match server, 14.30 selected
std::vector<Build> lobby() {
    Build a = mk(L"3.6", 3, 6, 4019403, L"4.20"); a.match = Support::Yes; a.matchWhy = kS3; a.cosmetics = 2210;
    Build b = mk(L"14.30", 14, 30, 14456520, L"4.26"); b.match = Support::Yes; b.matchWhy = kS3; b.cosmetics = 4025;
    std::vector<Build> v = { a, b, b1750() };
    for (auto& x : v) x.matchServer = true;
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
    // the lobby screen at the mockup's size, at 125 % and at the minimum window size
    { L"24-lobby-1280x800", 1280, 800, 1.0f, false },
    { L"25-lobby-1280x800-125", 1280, 800, 1.25f, false },
    { L"26-lobby-760x520", 760, 520, 1.0f, false },
    // the cosmetics panel (14.30): the lobby with its Cosmetics button, then the panel in its states
    { L"27-port-lobby-button", 1200, 800, 1.0f, false },
    { L"28-port-catalog-125", 1200, 800, 1.25f, false, 0, 0, 1 },
    { L"29-port-filters", 1200, 800, 1.0f, false, 0, 0, 2 },
    { L"30-port-import-all-dialog", 1200, 800, 1.0f, false, 0, 0, 3 },
    { L"31-port-importing-760x520", 760, 520, 1.0f, false, 0, 0, 1 },
    { L"32-port-selected-hover", 1200, 800, 1.0f, false, 0, 0, 5 },
    { L"33-port-preview", 1200, 800, 1.0f, false, 0, 0, 4 },
    { L"34-port-loading", 1200, 800, 1.0f, false, 0, 0, 1 },
    { L"35-port-importing-1920", 1920, 1040, 1.0f, false, 0, 0, 1 },
    { L"36-port-catalog-2560x1380-125", 2048, 1104, 1.25f, false, 0, 0, 1 },
    { L"37-port-catalog-1280x800", 1280, 800, 1.0f, false, 0, 0, 1 },
    // the import card: converting at 2560x1380 (125 %), signing, finished, failed
    { L"38-port-importing-2560x1380-125", 2048, 1104, 1.25f, false, 0, 0, 1 },
    { L"39-port-signing-1280x800", 1280, 800, 1.0f, false, 0, 0, 1 },
    { L"40-port-import-done", 1280, 800, 1.0f, false, 0, 0, 1 },
    { L"41-port-import-failed", 1280, 800, 1.0f, false, 0, 0, 1 },
    // the test channel: the launcher reopened by a silent update to a test build
    { L"42-test-build-updated", 1200, 800, 1.0f, false },
    // radarnite.com's item sheet in the preview: the whole shop history, its end (full history, the estimate's text),
    // the player under the clip
    { L"43-port-sheet-history", 1280, 800, 1.0f, false, 0, 0, 8 },
    { L"44-port-sheet-history-end", 1280, 800, 1.0f, false, 0, 0, 9 },
    { L"45-port-sheet-player", 1280, 800, 1.0f, false, 0, 0, 10 },
    { L"46-port-sheet-1920-125", 1536, 864, 1.25f, false, 0, 0, 8 },
    // the poster main screen at the mockup's size (2560x1380 at 125 %) and its always-open match settings at 1280x800
    // and at the minimum size (their content scrolls), with and without Activity
    { L"47-poster-2048x1104-125", 2048, 1104, 1.25f, false },
    { L"49-poster-settings-1280x800", 1280, 800, 1.0f, false },
    { L"50-poster-settings-760x520", 760, 520, 1.0f, false },
    { L"51-poster-settings-activity-760x520", 760, 520, 1.0f, true },
    { L"52-poster-activity-760x520", 760, 520, 1.0f, true },
};

// ---- the cosmetics (tests/ui-port-catalog.json, padded to the size of the real list with older made-up items)
struct PortSrc { const wchar_t *path, *id, *name, *set, *series; const char *rarity, *icon, *featured, *added; PortType type; PortStatus status; };
const PortSrc kPort[] = {
#include "mock_port.inc"
};
const int NPORT = (int)(sizeof kPort / sizeof kPort[0]), PORT_TOTAL = 12352;
Build b1430() {
    Build b = mk(L"14.30", 14, 30, 15341163, L"4.26");
    b.match = Support::Yes; b.matchWhy = kS3; b.cosmetics = 4100; b.port = true; b.imported = 21;
    return b;
}
std::shared_ptr<PortCatalog> portCatalogOf(const std::wstring& build) {
    auto c = std::make_shared<PortCatalog>();
    c->build = build; c->slotsFree = 7979;
    c->items.reserve(PORT_TOTAL);
    const char* rars[] = { "legendary", "epic", "rare", "uncommon", "common", "marvel", "dc", "icon", "gaminglegends", "starwars", "frozen", "lava", "dark", "shadow", "slurp" };
    char date[32];
    for (int i = 0; i < PORT_TOTAL; i++) {
        const PortSrc& s = kPort[i % NPORT];
        PortItem it;
        it.path = s.path; it.id = s.id; it.name = s.name; it.set = s.set; it.series = s.series;
        it.rarity = s.rarity; it.icon = s.icon; it.featured = s.featured; it.added = s.added; it.type = s.type; it.status = s.status;
        if (i >= NPORT) {
            std::wstring n = std::to_wstring(i);
            it.path += L"_" + n; it.id += L"_" + n; it.name += L" " + n;
            it.rarity = rars[i % 15];
            it.series.clear();
            snprintf(date, sizeof date, "20%02d-%02d-%02dT10:00:00Z", 18 + i % 8, 1 + i % 12, 1 + i % 28);
            it.added = date;
            it.status = i % 23 == 0 ? PortStatus::Build : i % 41 == 0 ? PortStatus::Failed : i % 9 == 0 ? PortStatus::Imported : PortStatus::New;
            if (i % 17 == 0) it.type = PortType::Glider;
            else if (i % 19 == 0) it.type = PortType::LoadingScreen;
        }
        c->items.push_back(std::move(it));
    }
    return c;
}
// An import half-way: batch 2 of 5, an item converting, the ones before it done (one failed).
std::shared_ptr<PortJob> portJobOf(int done) {
    auto j = std::make_shared<PortJob>();
    j->build = L"14.30"; j->phase = L"convert"; j->total = 120; j->done = done; j->failed = done > 30 ? 1 : 0; j->ok = done - j->failed;
    j->batch = 2; j->batches = 5; j->cpu = 41;
    j->elapsedMs = 9 * 60000 + 12000; j->currentMs = 6000; j->etaMs = (120 - done) * 14000LL;
    j->current = kPort[(done + 1) % NPORT].path;
    for (int i = std::max(0, done - 8); i <= done + 3; i++) {
        PortJobItem x; x.path = kPort[i % NPORT].path;
        x.st = i <= done ? (i == done - 3 ? L"failed" : L"ok") : i == done + 1 ? L"converting" : L"queued";
        j->items.push_back(x);
    }
    const wchar_t* lines[] = { L"batch 2/5: 25 items", L"loading the game files (2,048 paks)", L"loaded in 41 s", L"converting Character_CargoLying",
                               L"ok Character_CargoLying (12.4 s)", L"converting Backpack_CargoLying", L"ok Backpack_CargoLying (3.1 s)" };
    int k = 0;
    for (auto l : lines) { wchar_t ts[16]; swprintf(ts, 16, L"22:4%d:%02d", k / 4, (k * 13) % 60); j->recent.push_back({ ts, l }); k++; }
    return j;
}

// The end of a 39-item import: signing, its finished view, or a failed one (3 tried, none converted).
std::shared_ptr<PortJob> portJobEnd(const wchar_t* phase) {
    auto j = portJobOf(37);
    std::wstring ph = phase;
    j->phase = ph; j->total = 39; j->batch = 2; j->batches = 2; j->current.clear(); j->etaMs = 0; j->cpu = -1;
    j->elapsedMs = 6 * 60000 + 17000; j->phaseMs = 43000;
    if (ph == L"failed") {
        j->done = 3; j->ok = 0; j->failed = 3; j->elapsedMs = 72000;
        j->line = L"fn-port stopped: the game's files could not be read (is the game updating?)";
        j->items.resize(3);
        for (auto& x : j->items) x.st = L"failed";
    } else {
        j->done = 39; j->failed = 1; j->ok = 38;
        std::vector<PortJobItem> keep;
        for (auto& x : j->items) { if (x.st == L"converting") x.st = L"ok"; if (x.st != L"queued") keep.push_back(x); }
        j->items = keep;
    }
    return j;
}

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
        if (id.empty()) { st.builds = withServer(many()); st.builds.push_back(b1430()); st.selected = L"17.50"; st.config.mode = L"bots"; post = logs(8); }
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
            Build a = mk(L"3.6-CL-4019403", 3, 6, 4019403, L"4.20"); a.version = L"3.6"; a.dir = L"S:\\FortniteBuilds\\3.6-CL-4019403\\Fortnite"; a.match = Support::Yes; a.matchWhy = kS3; a.cosmetics = 0; a.firewall = L"firewall rules missing for 6 programs";
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
        else if (id.rfind(L"24-", 0) == 0 || id.rfind(L"25-", 0) == 0 || id.rfind(L"26-", 0) == 0 || (_wtoi(id.c_str()) >= 43 && _wtoi(id.c_str()) <= 48)) {
            st.builds = lobby(); st.selected = L"14.30"; st.config.displayName = L"GoldenTiger86";
            LogLine l; l.ts = L"22:09:58"; l.text = L"Backend ready on 127.0.0.1:3551 (local account \"GoldenTiger86\")."; post.push_back(l);
            l.text = L"Warning: the match server did not get ready (see offline\\userdir-server\\Saved\\Logs). The lobby still works."; l.level = Level::Warn; post.push_back(l);
        }
        else if (_wtoi(id.c_str()) >= 49 && _wtoi(id.c_str()) <= 52) { st.builds = withServer(many()); st.config.mode = L"bots"; st.config.bots = 35; st.selected = L"17.50"; post = logs(40); }   // the match settings
        else if (id.rfind(L"42-", 0) == 0) { st.builds = lobby(); st.selected = L"14.30"; st.config.displayName = L"GoldenTiger86"; }
        else if (portScenario()) { st.builds = { b1750(), b1430() }; st.selected = L"14.30"; }
        lock(); state_ = st; unlock();
        sink_->onState(st);
        for (auto& l : pre) sink_->onLog(l);
        for (auto& l : post) sink_->onLog(l);
        if (hasSession) { lock(); session_ = se; unlock(); sink_->onSession(se); }
        if (id.rfind(L"22-", 0) == 0) sink_->onToast(L"Desktop shortcut created", Level::Ok);
        if (id.rfind(L"42-", 0) == 0) sink_->onToast(L"Updated to test build 3.0.0-t120.2", Level::Ok);
        if (id.rfind(L"31-", 0) == 0 || id.rfind(L"35-", 0) == 0 || id.rfind(L"38-", 0) == 0) { auto j = portJobOf(37); lock(); job_ = j; unlock(); sink_->onPortJob(j); }
        if (id.rfind(L"39-", 0) == 0) { auto j = portJobEnd(L"sign"); lock(); job_ = j; unlock(); sink_->onPortJob(j); }
        if (id.rfind(L"40-", 0) == 0) sink_->onPortJob(portJobEnd(L"done"));
        if (id.rfind(L"41-", 0) == 0) sink_->onPortJob(portJobEnd(L"failed"));
    }

    // ---- cosmetics: answered at once (the offscreen renders have no message loop); the import runs on timers
    void portCatalog(const std::wstring& build, bool) override {
        PortLoad l; l.build = build; l.stage = L"names"; l.ms = 1400; l.done = true;
        sink_->onPortLoad(l);
        l.stage = L"list"; l.ms = 190000; l.done = id_.rfind(L"34-", 0) != 0;
        sink_->onPortLoad(l);
        if (!l.done) return;   // loading: fn-port never finishes
        lock(); if (!cat_) cat_ = portCatalogOf(build); std::shared_ptr<const PortCatalog> c = cat_; unlock();
        sink_->onPortCatalog(c);
    }
    void portImport(const std::wstring& build, const std::vector<std::wstring>& paths) override {
        lock(); bool busy = job_ != nullptr; unlock();
        if (busy || paths.empty()) return;
        auto j = std::make_shared<PortJob>();
        j->build = build; j->phase = L"start"; j->total = (int)paths.size(); j->batches = ((int)paths.size() + 24) / 25;
        lock(); job_ = j; int seq = ++jobSeq_; unlock();
        sink_->onPortJob(j);
        after(700, [this, seq] {
            lock(); bool same = jobSeq_ == seq && job_; unlock();
            if (!same) return;
            auto x = portJobOf(3);
            lock(); job_ = x; unlock();
            sink_->onPortJob(x);
        });
    }
    void portCancel() override {
        after(150, [this] {
            lock(); auto last = job_; job_.reset(); jobSeq_++; unlock();
            if (last) { auto f = std::make_shared<PortJob>(*last); f->phase = L"stopped"; sink_->onPortJob(f); }   // like the core: its last view, then none
            sink_->onPortJob(nullptr);
            log(L"Import stopped: 3 imported");
        });
    }
    void portPreview(const std::wstring& id) override {
        // radarnite.com's catalog.json for some of the scenarios' items (the rest: made up, without a clip)
        struct Pv { const wchar_t* id; int gg; const wchar_t* desc; };
        static const Pv known[] = {
            { L"EID_LoneStar", 24945, L"It ain't your first rodeo." }, { L"Backpack_AsideDepot", 24930, L"It's showtime!" },
            { L"Character_AsideDepot", 24932, L"Say his name three times." }, { L"EID_AsideDepot", 24933, L"A dance for the recently deceased." },
            { L"Character_CargoLying", 24916, L"Be Careful What You Wish For" }, { L"Backpack_CargoLying", 24915, L"You only get one wish." },
            { L"EID_CargoLying", 24917, L"Don't do that!" }, { L"Character_AcidWhale", 24883, L"Also known as the Radio Demon." },
            { L"EID_AcidWhale", 24896, L"Alastor pulls all the strings." },
        };
        PortPreview p; p.id = id; p.found = true; p.introduced = L"C7 S4";
        p.description = L"A cosmetic of the current season, converted into a new item of the build.";
        for (auto& k : known)
            if (!_wcsicmp(id.c_str(), k.id)) {
                char u[96];
                p.description = k.desc;
                snprintf(u, sizeof u, "https://fnggcdn.com/items/%d/video-sd.mp4", k.gg); p.video = u;
                snprintf(u, sizeof u, "https://fnggcdn.com/items/%d/video.mp4", k.gg); p.videoHd = u;
                snprintf(u, sizeof u, "https://fortnite.gg/cosmetics?id=%d", k.gg); p.page = u;
            }
        // radarnite.com's shop data, on a fixed day so the renders never change: Texas Primadonna as the site had it on
        // 2026-10-24 (in the shop, first stay), Lord of the Ludes with Davy Jones' history of that day (six stays, a
        // return prediction), the rest never sold
        auto D = [](const char* s) { return rn::sheet::dayOf(s, 10); };
        p.today = D("2026-10-24");
        if (!_wcsicmp(id.c_str(), L"EID_LoneStar")) {
            p.inShop = true; p.first = p.last = p.today; p.occ = 1; p.price = p.regular = 500; p.p7 = 3; p.p30 = 31; p.p60 = 99;
            p.stays.push_back({ p.today, p.today });
        } else if (!_wcsicmp(id.c_str(), L"Character_CargoLying")) {
            p.set = L"Obsession";
            p.first = D("2024-07-17"); p.last = D("2026-09-29"); p.occ = 40; p.p7 = 4; p.p30 = 40; p.p60 = 55;
            const char* st[][2] = { { "2026-09-26", "2026-09-29" }, { "2026-01-26", "2026-01-29" }, { "2025-08-20", "2025-08-23" },
                                    { "2025-04-28", "2025-05-01" }, { "2025-01-29", "2025-02-01" }, { "2024-07-17", "2024-08-06" } };
            for (auto& x : st) p.stays.push_back({ D(x[0]), D(x[1]) });
        }
        sink_->onPortPreview(p);
    }
    std::wstring imageCacheDir() override {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        return std::wstring(t) + L"rn-ui-mock-img";
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
    std::shared_ptr<PortCatalog> cat_;
    std::shared_ptr<PortJob> job_;
    int jobSeq_ = 0;
    bool portScenario() const { int n = _wtoi(id_.c_str()); return n >= 27 && n <= 41; }
};

} // namespace

Core* createMockCore(const wchar_t* scenario) { return new MockCore(scenario); }
const MockScenario* mockScenarios(int* count) { *count = (int)(sizeof(kScenarios) / sizeof(kScenarios[0])); return kScenarios; }

} // namespace rn
