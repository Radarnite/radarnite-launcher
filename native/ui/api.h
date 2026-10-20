// Radarnite Launcher 3.0: contract between the native UI (native/ui) and the launcher core (native/core).
//
// Frozen on day 1 of P6 (PLAN.md). It carries the same data as the 2.x host <-> page messages (launcher/Web.cs:
// PushState, PostSession, Log, toast; page -> host: select, launch, stop, saveConfig, the menu commands), so the core
// can fill it from the code it already ports from Web.cs without new concepts.
//
// Threading: the core may call UiSink from ANY thread. The UI copies the data and applies it on its own thread
// (posted window message), so the core never waits for the UI. Core methods are called from the UI thread only and
// must return quickly (long work goes to the core's own threads, results come back through UiSink).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace rn {

enum class Support { Yes, Experimental, No };

// One installed build, as BuildInfo + firewall + cosmetics give it (Web.cs PushState).
struct Build {
    std::wstring name;          // folder name, unique key (e.g. "17.50", "3.6-CL-4019403")
    std::wstring dir;           // full path of the build folder
    std::wstring version;       // "17.50"
    int major = 0, minor = 0;
    uint32_t cl = 0;            // changelist
    std::wstring engine;        // "4.26"
    Support lobby = Support::No;
    std::wstring lobbyWhy;
    Support match = Support::No;
    std::wstring matchWhy;
    bool matchServer = false;   // launcher\gameserver\radarnite-server.dll present
    int cosmetics = 0;          // items in offline\cosmetics.json (0 = missing)
    std::wstring firewall;      // L"OK" or the reason the rules are missing
};

// config.json fields the window shows/edits. has* = the key was present (a missing config shows the defaults).
struct Config {
    bool has = false;
    std::wstring displayName;
    std::wstring mode = L"practice";   // lobby | practice | bots (kept for config.json compatibility)
    int bots = 20;
    bool botsAI = false, infiniteMaterials = false, infiniteAmmo = false, lateGame = false;
};

// A partial config change made in the window (only the set fields are written by the core).
struct ConfigPatch {
    enum Field : uint32_t { DisplayName = 1, Mode = 2, Bots = 4, BotsAI = 8, InfiniteMaterials = 16, InfiniteAmmo = 32, LateGame = 64 };
    uint32_t fields = 0;
    Config values;
};

enum class Stage { None, Run, Ok, Bad };
enum class SessionState { Idle, Launching, Running, Stopping, Error };
enum class MatchPhase { None, Waiting, Joined, Warmup, Bus, Storm, Ended, Down };

struct Session {
    SessionState state = SessionState::Idle;
    std::wstring version;       // build version of the session
    std::wstring message;       // failure text when state == Error
    Stage backend = Stage::None, game = Stage::None, server = Stage::None;
    bool hasServer = false;     // the "Match server" stage exists (stages.server was set)
    MatchPhase phase = MatchPhase::None;
    std::wstring matchDetail;   // "Waiting for you: press PLAY in the game"
};

// Full state (Web.cs "state" message). loaded = false only before the first scan (skeleton).
struct State {
    bool loaded = false;
    std::vector<Build> builds;
    Config config;
    std::wstring selected;      // the core's selection ("" = none)
    std::wstring root;          // builds folder
    bool hasSession = false;
    Session session;
};

enum class Level { Auto, Info, Ok, Warn, Bad };   // Auto = the UI classifies the text like the 2.x page did

struct LogLine {
    std::wstring ts;            // "HH:MM:SS" ("" = the UI stamps it)
    std::wstring text;
    Level level = Level::Auto;
};

// Window -> core actions (the page's data-cmd values and play button). build = the selected build ("" = none).
enum class Command {
    Select, Launch, Stop, Refresh, CheckUpdates, AddZip, AddFolder, ChooseRoot,
    Cosmetics, Firewall, Shortcut, OpenLogs, OpenConfig,
};

// Implemented by the UI. Any thread.
struct UiSink {
    virtual void onState(const State& s) = 0;
    virtual void onSession(const Session& s) = 0;
    virtual void onLog(const LogLine& l) = 0;
    virtual void onToast(const std::wstring& text, Level level) = 0;
protected:
    ~UiSink() = default;
};

// Implemented by the core (or the mock). UI thread.
struct Core {
    virtual ~Core() = default;
    // The window is up: start pushing (state first). sink outlives the core.
    virtual void ready(UiSink* sink) = 0;
    virtual void command(Command c, const std::wstring& build) = 0;
    virtual void saveConfig(const ConfigPatch& p) = 0;
    // Per-user window preferences the 2.x page kept in localStorage ("lastBuild", "activityHeight").
    // The core decides where they live; "" = not set.
    virtual std::wstring uiPref(const wchar_t* key) = 0;
    virtual void setUiPref(const wchar_t* key, const std::wstring& value) = 0;
};

// Mock core with the scenarios of tests/ui-scenarios.mjs (ids "01-one-build-practice-100" ... ; null = interactive
// default: several builds, launch/stop simulated with timers).
Core* createMockCore(const wchar_t* scenario);

struct MockScenario {
    const wchar_t* id;
    int width, height;          // viewport in DIPs (title bar included, like the page)
    float scale;                // device pixel ratio (1.25 = 120 dpi)
    bool activityOpen;
    int menu = 0;               // menu open in the selftest render (0 none, 1 add, 2 build)
    int tabs = 0;               // Tab presses before the selftest render (keyboard focus ring)
};
// The scenario table (same order as ui-scenarios.mjs). count receives the number of entries.
const MockScenario* mockScenarios(int* count);

} // namespace rn
