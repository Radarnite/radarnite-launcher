// Test harness for the core without the window (not shipped). Build next to the core (see test/README.md):
//   coretest.exe core <build>         rn::Core through api.h with a printing UiSink: ready, select, save, refresh,
//                                      launch <build> (ends in the session Error of launch_stub.cpp), stop
//   coretest.exe addzip <zip>         builddata::addZip into the builds folder
//   coretest.exe addfolder <dir>      builddata::addFolder into the builds folder
//   coretest.exe fetch <url> <file>   net::download (redirects, streamed SHA-256) + update::sha256Token of the file
//   coretest.exe sha <file>           net::sha256 of a file's bytes
//   coretest.exe semver               update::parseTag / update::compare against a table (exit 1 on any failure)
// Like RadarniteLauncher.exe, it runs rn::cli::run() first (the core re-runs its own exe for "--firewall").
#include "../buildinfo.h"
#include "../builddata.h"
#include "../cli.h"
#include "../core.h"
#include "../net.h"
#include "../update.h"
#include "../util.h"
#include <objbase.h>
#include <shellapi.h>
#include <atomic>

using namespace rn;

namespace {

const wchar_t* lv(Level l) { return l == Level::Info ? L"info" : l == Level::Ok ? L"ok" : l == Level::Warn ? L"warn" : l == Level::Bad ? L"bad" : L"auto"; }
const wchar_t* st(SessionState s) {
    switch (s) {
    case SessionState::Idle: return L"idle";
    case SessionState::Launching: return L"launching";
    case SessionState::Running: return L"running";
    case SessionState::Stopping: return L"stopping";
    default: return L"error";
    }
}
const wchar_t* sg(Stage s) { return s == Stage::Run ? L"run" : s == Stage::Ok ? L"ok" : s == Stage::Bad ? L"bad" : L"-"; }

struct Sink : UiSink {
    std::atomic<int> states{ 0 };
    std::atomic<int> sessionState{ 0 };
    void onState(const State& s) override {
        out(fmt(L"[state] loaded=%d builds=%d selected='%ls' root=%ls config.has=%d name=%ls mode=%ls bots=%d", s.loaded, (int)s.builds.size(),
                s.selected.c_str(), s.root.c_str(), s.config.has, s.config.displayName.c_str(), s.config.mode.c_str(), s.config.bots));
        for (auto& b : s.builds)
            out(fmt(L"  build %ls v%ls %d.%d cl %u ue %ls lobby %ls match %ls server %d cosmetics %d fw %ls", b.name.c_str(), b.version.c_str(), b.major,
                    b.minor, b.cl, b.engine.c_str(), supportName(b.lobby), supportName(b.match), b.matchServer, b.cosmetics, b.firewall.c_str()));
        states++;
    }
    void onSession(const Session& s) override {
        out(fmt(L"[session] %ls version=%ls backend=%ls game=%ls server=%ls(%d) message='%ls'", st(s.state), s.version.c_str(), sg(s.backend), sg(s.game),
                sg(s.server), s.hasServer, s.message.c_str()));
        sessionState = (int)s.state;
    }
    void onLog(const LogLine& l) override { out(L"[log " + std::wstring(lv(l.level)) + L"] " + l.ts + L" " + l.text); }
    void onToast(const std::wstring& t, Level l) override { out(L"[toast " + std::wstring(lv(l)) + L"] " + t); }
};

void waitFor(const std::function<bool()>& f, int ms) {
    for (int i = 0; i < ms / 50 && !f(); i++) Sleep(50);
}

int coreFlow(const std::wstring& build) {
    Sink sink;
    Core* core = createCore(nullptr);
    core->ready(&sink);
    waitFor([&] { return sink.states >= 1; }, 15000);
    out(L"uiPref lastBuild='" + core->uiPref(L"lastBuild") + L"'");
    core->command(Command::Select, build);
    ConfigPatch p;
    p.fields = ConfigPatch::Bots | ConfigPatch::DisplayName;
    p.values.bots = 133;               // clamped to 99
    p.values.displayName = L"  CoreTest  ";
    core->saveConfig(p);
    int before = sink.states;
    core->command(Command::Refresh, L"");
    waitFor([&] { return sink.states > before; }, 15000);
    core->command(Command::Launch, L"");   // the selected build
    core->command(Command::Launch, L"");   // a double click: must not start a second session
    waitFor([&] { return sink.sessionState == (int)SessionState::Error || sink.sessionState == (int)SessionState::Running; }, 60000);
    core->command(Command::Stop, L"");
    waitFor([&] { return sink.sessionState == (int)SessionState::Idle; }, 30000);
    Sleep(500);
    delete core;
    return 0;
}

// The updater's version parsing and Semantic Versioning 2.0.0 precedence: one line per check, "FAIL" on a mismatch.
int semverTable() {
    int fails = 0, checks = 0;
    auto expect = [&](bool ok, const std::wstring& what, bool show = true) {
        checks++;
        if (!ok) fails++;
        if (show || !ok) out((ok ? L"ok   " : L"FAIL ") + what);
    };
    struct P { const wchar_t* tag; bool ok; int maj, min, pat; const wchar_t* pre; const wchar_t* str; };
    const P parses[] = {
        { L"v3.0.0-rc.1", true, 3, 0, 0, L"rc.1", L"3.0.0-rc.1" },
        { L"v2.2.1", true, 2, 2, 1, L"", L"2.2.1" },
        { L" V2.3 ", true, 2, 3, 0, L"", L"2.3.0" },                     // 2.x: two fields and white space accepted
        { L"v3.0.0+build.7", true, 3, 0, 0, L"", L"3.0.0" },              // build metadata dropped
        { L"3.0.0-rc.1+build.7", true, 3, 0, 0, L"rc.1", L"3.0.0-rc.1" },
        { L"v3.0.0-beta.x-y.07", true, 3, 0, 0, L"beta.x-y.07", L"3.0.0-beta.x-y.07" },
        { L"v3", false, 0, 0, 0, L"", L"" },
        { L"latest", false, 0, 0, 0, L"", L"" },
        { L"v-1.0.0", false, 0, 0, 0, L"", L"" },
        { L"v3.0.x-rc.1", false, 0, 0, 0, L"", L"" },
    };
    for (auto& p : parses) {
        update::Version v;
        bool ok = update::parseTag(p.tag, &v);
        expect(ok == p.ok && (!ok || (v.major == p.maj && v.minor == p.min && v.patch == p.pat && v.pre == p.pre && v.str() == p.str)),
               fmt(L"parseTag('%ls') -> %ls", p.tag, ok ? v.str().c_str() : L"(not a version)"));
    }
    auto ver = [](const wchar_t* t) { update::Version v; update::parseTag(t, &v); return v; };
    // each one sorts strictly before the next (the semver.org section 11 chain, then the release-candidate line)
    const wchar_t* chain[] = { L"1.0.0-alpha", L"1.0.0-alpha.1", L"1.0.0-alpha.beta", L"1.0.0-beta", L"1.0.0-beta.2", L"1.0.0-beta.11",
                               L"1.0.0-rc.1", L"1.0.0", L"2.2.1", L"3.0.0-rc.1", L"3.0.0-rc.2", L"3.0.0-rc.10", L"3.0.0",
                               L"3.0.1-rc.1", L"3.0.1", L"3.1.0-0", L"3.1.0-1", L"3.1.0-99999999999999999999", L"3.1.0-a", L"3.1.0", L"10.0.0" };
    const size_t n = sizeof chain / sizeof chain[0];
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < n; j++) {
            int want = i < j ? -1 : i > j ? 1 : 0, got = update::compare(ver(chain[i]), ver(chain[j]));
            expect(got == want, fmt(L"compare(%ls, %ls) = %d", chain[i], chain[j], got), j == i + 1);   // all pairs, neighbours shown
        }
    struct E { const wchar_t* a; const wchar_t* b; };
    const E equal[] = { { L"v3.0.0-rc.1", L"3.0.0-rc.1+meta" }, { L"2.3", L"v2.3.0" }, { L"3.0.0+a", L"3.0.0+b" }, { L"3.0.0-rc.01", L"3.0.0-rc.1" } };
    for (auto& e : equal) expect(update::compare(ver(e.a), ver(e.b)) == 0, fmt(L"compare(%ls, %ls) = 0", e.a, e.b));
    update::Version cur = update::current();
    update::Version again;
    expect(update::parseTag(cur.str(), &again) && update::compare(cur, again) == 0, L"current() = " + cur.str());
    out(fmt(L"semver: %d checks, %d failed", checks, fails));
    return fails ? 1 : 0;
}

} // namespace

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool handled = false;
    int rc = cli::run(argc, argv, &handled);
    if (handled) return rc;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring mode = argc > 1 ? argv[1] : L"", arg = argc > 2 ? argv[2] : L"";
    LogFn log = [](const std::wstring& s) { out(L"[log] " + s); };
    BuildInfo b;
    if (mode == L"core") rc = coreFlow(arg);
    else if (mode == L"addzip") { rc = builddata::addZip(arg, log, &b) ? 0 : 1; if (!rc) out(L"added " + b.name() + L" " + b.version + L" at " + b.dir); }
    else if (mode == L"addfolder") { rc = builddata::addFolder(arg, log, &b) ? 0 : 1; if (!rc) out(L"added " + b.name() + L" " + b.version + L" at " + b.dir); }
    else if (mode == L"fetch" && argc > 3) {
        std::wstring sha, error;
        rc = net::download(arg, L"radarnite-launcher", 60000, argv[3], &sha, &error) ? 0 : 1;
        std::string text;
        readFile(argv[3], &text);
        out(rc ? L"download failed: " + error : fmt(L"downloaded %d bytes, sha256 %ls, token %ls", (int)text.size(), sha.c_str(), update::sha256Token(widen(text)).c_str()));
    } else if (mode == L"semver") {
        rc = semverTable();
    } else if (mode == L"sha") {
        std::string data;
        rc = readFile(arg, &data) ? 0 : 1;
        out(L"sha256 " + net::sha256(data.data(), data.size()));
    } else { err(L"usage: coretest core <build> | addzip <zip> | addfolder <dir> | fetch <url> <file> | sha <file> | semver"); rc = 2; }
    LocalFree(argv);
    return rc;
}
