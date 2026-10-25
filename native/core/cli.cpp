#include "cli.h"
#include "builddata.h"
#include "buildinfo.h"
#include "config.h"
#include "firewall.h"
#include "paths.h"
#include "proc.h"
#include "session.h"
#include "update.h"
#include "util.h"

namespace rn {
namespace cli {
namespace {

// Program.Find: the build whose folder name matches (any case) or whose version is exactly v.
bool find(const wchar_t* v, BuildInfo* out) {
    if (!v) return false;
    for (auto& b : BuildInfo::all()) {
        if (ieq(b.name(), v) || b.version == v) { *out = b; return true; }
    }
    return false;
}

std::wstring orOk(const std::wstring& s) { return s.empty() ? L"OK" : s; }

int check(const wchar_t* arg) {
    LauncherConfig c = LauncherConfig::load();
    // The process list, the firewall rules and the build's exe walk are independent: they run at the same time, once
    // (2.x read the rules and walked the build twice per build, one after the other).
    bool loaded = false;
    std::vector<proc::Info> procs;
    firewall::Rules rules;
    for (auto& b : BuildInfo::all()) {
        if (arg && b.name() != arg && b.version != arg) continue;
        std::vector<std::wstring> programs;
        if (!loaded) {
            parallel({ [&] { programs = firewall::programsFor(b.dir); }, [&] { rules = firewall::read(); }, [&] { procs = proc::list(); } });
            loaded = true;
        } else programs = firewall::programsFor(b.dir);
        std::wstring fw = firewall::check(rules, programs);
        auto lobby = b.lobby(), match = b.match();
        out(b.name() + L": Fortnite " + b.version + fmt(L" CL %lld, UE ", (long long)b.cl) + b.engine);
        out(L"  lobby: " + std::wstring(supportName(lobby.first)) + L" (" + lobby.second + L"); local match: " + supportName(match.first) + L" (" + match.second + L")");
        out(std::wstring(L"  cosmetics: ") + (fileExists(join(b.dir, L"offline\\cosmetics.json")) ? L"yes" : L"no") + L"; firewall: " + orOk(fw));
        out(L"  preflight: " + orOk(preflightChecked(b, procs, fw)));
    }
    out(L"config: " + c.displayName + L" mode=" + c.mode + fmt(L" bots=%d port=%d", c.bots, c.port));
    return 0;
}

int scan(const wchar_t* arg) {
    std::vector<std::pair<std::wstring, std::wstring>> skipped;
    auto list = BuildInfo::scan(arg ? std::wstring(arg) : paths::root(), &skipped);
    for (auto& b : list)
        out(L"build " + b.name() + L" | " + b.version + fmt(L" | CL %lld | dir ", (long long)b.cl) + b.dir + L" | lobby " +
            supportName(b.lobby().first) + L" | match " + supportName(b.match().first));
    for (auto& s : skipped) out(L"skip " + fileName(s.first) + L" | " + s.second);
    return 0;
}

int setBuildsFolder(const wchar_t* arg) {
    if (!arg || blank(arg)) { out(L"usage: --set-builds-folder <folder>"); return 2; }
    LauncherConfig c = LauncherConfig::load();
    std::wstring dir = fullPath(expandEnv(arg));
    if (!createDirs(dir)) { err(L"Error: " + ioError(GetLastError(), dir)); return 1; }
    c.buildsFolder = dir;
    c.nulls &= ~(1u << 13);
    if (!c.save()) { err(L"Error: " + ioError(GetLastError(), paths::config())); return 1; }
    out(L"Builds folder: " + c.buildsFolder);
    return 0;
}

int createFirewall(const wchar_t* arg) {
    BuildInfo b;
    if (!find(arg, &b)) { out(L"No build named " + std::wstring(arg ? arg : L"")); return 2; }
    std::wstring error;
    int n = firewall::create(b.dir, &error);
    if (!error.empty()) { err(L"Error: " + error); return 1; }
    std::wstring check = firewall::check(b.dir);
    out(fmt(L"%d firewall rules created for ", n) + b.version + L". Check: " + orOk(check));
    return check.empty() ? 0 : 1;
}

int removeFirewall() {
    std::wstring error;
    int n = firewall::removeAll(&error);
    if (n == 0 && !error.empty()) { err(L"Error: " + error); return 1; }
    out(fmt(L"%d rules of the group \"", n) + firewall::group() + L"\" removed.");
    return 0;
}

int updateCheck() {
    update::Release r = update::check();
    std::wstring cur = update::current().str();
    std::wstring feed = update::testFeed();
    std::wstring where = std::wstring(L"repo ") + update::kRepo + (feed.empty() ? L"" : L", test feed " + feed);
    if (!r.found) { out(L"Up to date (installed " + cur + L"; " + where + L")."); return 0; }
    out(std::wstring(r.test ? L"Test build available: " : L"Update available: ") + r.version.str() + L" (installed " + cur + L"); installer " + r.installerUrl);
    return 10;
}

// --test-channel [<feed.json URL>|off]: the owner's test builds (update.h); no argument = show it.
int testChannel(const wchar_t* arg) {
    if (arg && !update::setTestFeed(arg)) { err(L"Error: the test feed must be an http(s) URL, or off."); return 2; }
    std::wstring feed = update::testFeed();
    out(feed.empty() ? L"Test channel: off." : L"Test channel: " + feed);
    return 0;
}

int cosmetics(const wchar_t* arg) {
    BuildInfo b;
    if (!find(arg, &b)) { out(L"No build named " + std::wstring(arg ? arg : L"")); return 2; }
    std::wstring why;
    return builddata::generateCosmetics(b, [](const std::wstring& s) { out(s); }, &why) ? 0 : 1;
}

} // namespace

int run(int argc, wchar_t** argv, bool* handled) {
    *handled = true;
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    const wchar_t* arg = argc > 2 ? argv[2] : nullptr;
    if (cmd == L"--stop") { stopAll([](const std::wstring& s) { out(s); }); return 0; }
    if (cmd == L"--firewall") return createFirewall(arg);
    if (cmd == L"--set-builds-folder") return setBuildsFolder(arg);
    if (cmd == L"--firewall-remove") return removeFirewall();
    if (cmd == L"--update-check") return updateCheck();
    if (cmd == L"--test-channel") return testChannel(arg);
    if (cmd == L"--scan") return scan(arg);
    if (cmd == L"--check") return check(arg);
    if (cmd == L"--selftest-inject") { out(L"game launch not implemented in this core"); return 1; }
    if (cmd == L"--cosmetics") return cosmetics(arg);
    *handled = false;
    return 0;
}

int playConsole(const wchar_t* name) {
    BuildInfo b;
    if (!find(name, &b)) { out(L"Build " + std::wstring(name ? name : L"") + L" not found in " + paths::root() + L"."); return 2; }
    LauncherConfig c = LauncherConfig::load();
    std::wstring logFile = join(paths::offline(b.dir), L"launcher.log");
    LogFn log = [logFile](const std::wstring& s) {
        out(nowTime(L"HH:mm:ss") + L" " + s);
        appendFile(logFile, narrow(nowIso() + L" " + s + L"\r\n"));
    };
    log(L"Launching Fortnite " + b.version + fmt(L" (CL %lld) - player \"", (long long)b.cl) + c.displayName + fmt(L"\", %d bots in Battle Royale", c.bots));
    PlaySession s(b, c, log);
    if (!s.start()) return 1;
    s.waitClient();
    std::wstring code = s.clientExitCode();
    if (code == L"0x00000000" || code == L"0x00000001" || code == L"?") log(L"The game closed: stopping the local servers.");
    else log(L"Error: the game crashed (exit code " + code + fmt(L", after %lld s). ", (long long)s.secondsSinceStart()) +
             L"Details: offline\\redirect-client.log and the game log in offline\\sandbox-client. Stopping the local servers.");
    s.stop();
    return 0;
}

} // namespace cli
} // namespace rn
