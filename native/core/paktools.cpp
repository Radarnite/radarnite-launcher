#include "paktools.h"
#include "net.h"
#include "paths.h"
#include "proc.h"
#include "paktools_manifest.h"
#include <mutex>
#include <vector>

namespace rn {
namespace paktools {

namespace {
// the top-level entries of a tools folder that belong to the pak tools (what a refresh replaces)
const wchar_t* const kTop[] = { L"sigfit-installed.ps1", L"game-guard.ps1", L"pak-tools.manifest", L"fn-modkit-sigfit" };
std::mutex g_sync;
}

const wchar_t* version() { return RN_PAKTOOLS_VERSION; }
std::wstring dir() { return join(paths::launcherDir(), L"tools"); }
std::wstring sigfitScript() { return join(dir(), L"sigfit-installed.ps1"); }
std::wstring sigfitTool() { return join(dir(), L"fn-modkit-sigfit\\fn-modkit.dll"); }

std::wstring verify(const std::wstring& folder) {
    for (auto& e : kPakTools) {
        std::wstring p = join(folder, e.path);
        std::string data;
        if (!readFile(p, &data)) return std::wstring(e.path) + L" is missing";
        if (net::sha256(data.data(), data.size()) != widen(e.sha256)) return std::wstring(e.path) + L" is not the one of pak tools " + version();
    }
    return L"";
}

std::wstring devTreeDir() {
    std::wstring d = join(join(paths::root(), L"launcher"), L"tools");
    if (!fileExists(join(d, L"sigfit-installed.ps1"))) return L"";
    if (ieq(trimEndSlash(fullPath(d)), trimEndSlash(fullPath(dir())))) return L"";   // portable layout: it is ours
    return d;
}

bool syncDevTree(const LogFn& log) {
    std::lock_guard<std::mutex> g(g_sync);
    std::wstring dev = devTreeDir();
    if (dev.empty() || verify(dev).empty()) return true;
    std::wstring own = verify(dir());
    if (!own.empty()) {
        log(L"Error: the launcher's own pak tools are damaged (" + own + L"): " + dev + L" was NOT refreshed. Reinstall the launcher.");
        return false;
    }
    // the old copy goes aside whole: a stale DLL left in fn-modkit-sigfit could still be loaded by the new tool
    std::wstring bak = join(parentDir(dev), L"backup-pak-tools-" + nowTime(L"yyyyMMddHHmmss"));
    if (!createDirs(bak)) { log(L"Error: " + ioError(GetLastError(), bak) + L": " + dev + L" still has an old sigfit."); return false; }
    for (auto* t : kTop) {
        std::wstring from = join(dev, t);
        if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (!MoveFileExW(from.c_str(), join(bak, t).c_str(), MOVEFILE_COPY_ALLOWED)) {
            log(L"Error: could not move " + from + L" aside (" + winError(GetLastError()) + L"; is a sigfit running?): " + dev + L" still has an old sigfit.");
            return false;
        }
    }
    std::vector<std::wstring> files;
    for (auto& e : kPakTools) files.push_back(e.path);
    files.push_back(L"pak-tools.manifest");
    for (auto& f : files) {
        std::wstring to = join(dev, f);
        createDirs(parentDir(to));
        if (!CopyFileW(join(dir(), f).c_str(), to.c_str(), FALSE)) {
            log(L"Error: could not copy " + f + L" to " + dev + L": " + winError(GetLastError()));
            return false;
        }
    }
    std::wstring after = verify(dev);
    if (!after.empty()) { log(L"Error: " + dev + L" still differs from the launcher's pak tools after the refresh: " + after + L"."); return false; }
    log(L"Pak tools: " + dev + L" refreshed to " + version() + L" (the old copy is in " + bak + L").");
    return true;
}

std::wstring gameGuard() {
    std::wstring script = join(dir(), L"game-guard.ps1");
    std::wstring bad = verify(dir());
    if (!bad.empty()) return L"the game guard cannot be trusted: " + bad;
    proc::Start s;
    s.exe = L"powershell.exe";
    s.args = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + script + L"\"";
    s.job = proc::toolsJob();
    std::vector<std::wstring> lines;
    std::mutex m;
    std::wstring err;
    int code = proc::run(s, [&](const std::wstring& line, bool isErr) {
        std::wstring t = trim(line);
        std::lock_guard<std::mutex> g(m);
        if (!isErr && !t.empty()) lines.push_back(t);
    }, &err);
    if (code == 0 && !lines.empty() && lines.back() == L"free") return L"";
    if (code == 6 && !lines.empty() && starts(lines.back(), L"busy")) return lines.back();
    return fmt(L"the game guard gave no answer (exit %d)", code) + (err.empty() ? L"" : L": " + err);
}

} // namespace paktools
} // namespace rn
