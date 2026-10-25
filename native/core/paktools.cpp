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
// the top-level entries of a tools folder that belong to the pak tools (what a refresh replaces). The tool folder
// first and the script last: a running sigfit holds its DLLs, so that move fails before anything else has changed.
const wchar_t* const kTop[] = { L"fn-modkit-sigfit", L"game-guard.ps1", L"pak-tools.manifest", L"sigfit-installed.ps1" };
const size_t kTopN = sizeof kTop / sizeof kTop[0];
bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool move(const std::wstring& from, const std::wstring& to) { return MoveFileExW(from.c_str(), to.c_str(), 0) != 0; }
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
    // The new copy is made and checked next to the backup first; then the old entries go aside whole (a stale DLL left
    // in fn-modkit-sigfit could still be loaded) and the new ones take their place. Any failure puts the old copy
    // back as it was, so the dev tree is never left half-replaced or without its script.
    std::wstring bak = join(parentDir(dev), L"backup-pak-tools-" + nowTime(L"yyyyMMddHHmmss"));
    std::wstring stage = join(bak, L"new");
    std::vector<std::wstring> files;
    for (auto& e : kPakTools) files.push_back(e.path);
    files.push_back(L"pak-tools.manifest");
    for (auto& f : files) {
        std::wstring to = join(stage, f);
        if (!createDirs(parentDir(to)) || !CopyFileW(join(dir(), f).c_str(), to.c_str(), FALSE)) {
            log(L"Error: could not copy the pak tools to " + stage + L" (" + winError(GetLastError()) + L"): " + dev + L" still has an old sigfit.");
            return false;
        }
    }
    std::wstring staged = verify(stage);
    if (!staged.empty()) { log(L"Error: the copy in " + stage + L" does not match (" + staged + L"): " + dev + L" still has an old sigfit."); return false; }
    size_t out = 0;   // old entries moved aside, in kTop order
    bool had[kTopN] = {};
    for (; out < kTopN; out++) {
        std::wstring from = join(dev, kTop[out]);
        if (!(had[out] = exists(from))) continue;
        if (!move(from, join(bak, kTop[out]))) break;
    }
    size_t in = 0;    // new entries moved in
    if (out == kTopN)
        for (; in < kTopN; in++) if (!move(join(stage, kTop[in]), join(dev, kTop[in]))) break;
    if (out < kTopN || in < kTopN) {
        DWORD e = GetLastError();
        for (size_t i = in; i-- > 0;) move(join(dev, kTop[i]), join(stage, kTop[i]));
        for (size_t i = out; i-- > 0;) if (had[i]) move(join(bak, kTop[i]), join(dev, kTop[i]));
        log(L"Error: could not replace " + join(dev, kTop[out < kTopN ? out : in]) + L" (" + winError(e) +
            L"; is a sigfit running from there?): " + dev + L" still has an old sigfit; trying again at the next start.");
        return false;
    }
    RemoveDirectoryW(stage.c_str());
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
