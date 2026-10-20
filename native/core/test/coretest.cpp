// Test harness for the core without the window (not shipped). Build next to the core (see test/README.md):
//   coretest.exe core <build>         rn::Core through api.h with a printing UiSink: ready, select, save, refresh,
//                                      launch <build> (ends in the session Error of launch_stub.cpp), stop
//   coretest.exe addzip <zip>         builddata::addZip into the builds folder
//   coretest.exe addfolder <dir>      builddata::addFolder into the builds folder
// Like RadarniteLauncher.exe, it runs rn::cli::run() first (the core re-runs its own exe for "--firewall").
#include "../buildinfo.h"
#include "../builddata.h"
#include "../cli.h"
#include "../core.h"
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
    waitFor([&] { return sink.sessionState == (int)SessionState::Error || sink.sessionState == (int)SessionState::Running; }, 60000);
    core->command(Command::Stop, L"");
    waitFor([&] { return sink.sessionState == (int)SessionState::Idle; }, 30000);
    Sleep(500);
    delete core;
    return 0;
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
    else { err(L"usage: coretest core <build> | addzip <zip> | addfolder <dir>"); rc = 2; }
    LocalFree(argv);
    return rc;
}
