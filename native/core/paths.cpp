#include "paths.h"
#include "util.h"
#include "vendor/yyjson/yyjson.h"

namespace rn {
namespace paths {

static Lock g_lock;
static std::wstring g_root;
static bool g_rootSet = false;

const std::wstring& launcherDir() {
    static const std::wstring dir = [] {
        std::wstring o = getEnv(L"RADARNITE_LAUNCHER_DIR");
        if (!o.empty()) return trimEndSlash(fullPath(o));
        return trimEndSlash(parentDir(selfExe()));
    }();
    return dir;
}

// config.json "buildsFolder" (created when missing), or null when absent/blank/unusable (Paths.BuildsFolder).
static bool buildsFolder(std::wstring* outDir) {
    std::string text;
    if (!readFile(config(), &text)) return false;
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), YYJSON_READ_ALLOW_BOM | YYJSON_READ_ALLOW_INVALID_UNICODE);
    if (!doc) return false;
    yyjson_val* v = yyjson_obj_get(yyjson_doc_get_root(doc), "buildsFolder");   // case-sensitive, like JsonNode["..."]
    bool ok = false;
    if (v && yyjson_is_str(v)) {
        std::wstring f = widen(std::string(yyjson_get_str(v), yyjson_get_len(v)));
        if (!blank(f)) {
            std::wstring full = fullPath(expandEnv(f));
            if (createDirs(full)) { *outDir = trimEndSlash(full); ok = true; }
        }
    }
    yyjson_doc_free(doc);
    return ok;
}

std::wstring root() {
    Guard g(g_lock);
    if (!g_rootSet) {
        std::wstring r;
        if (!buildsFolder(&r)) r = parentDir(launcherDir());
        g_root = r;
        g_rootSet = true;
    }
    return g_root;
}

void resetRoot() { Guard g(g_lock); g_rootSet = false; }

std::wstring config() { return join(launcherDir(), L"config.json"); }
std::wstring node() { return join(launcherDir(), L"node\\node.exe"); }
std::wstring backendScript() { return join(launcherDir(), L"backend\\server.mjs"); }
std::wstring backend() { return join(launcherDir(), L"backend\\radarnite-backend.exe"); }
std::wstring redirectDll() { return join(launcherDir(), L"redirect\\fnredirect.dll"); }
std::wstring gameServerDll() { return join(launcherDir(), L"gameserver\\radarnite-server.dll"); }
std::wstring pakPatchDll() { return join(launcherDir(), L"patches\\fnpakpatch.dll"); }
std::wstring cosmeticsGen() { return join(launcherDir(), L"tools\\cosmetics-gen\\fn-cosmetics-gen.exe"); }
std::wstring win64(const std::wstring& build) { return join(build, L"FortniteGame\\Binaries\\Win64"); }
std::wstring shipping(const std::wstring& build) { return join(win64(build), L"FortniteClient-Win64-Shipping.exe"); }

std::wstring offline(const std::wstring& build) {
    std::wstring d = join(build, L"offline");
    createDirs(d);
    return fullPath(d);
}

} // namespace paths
} // namespace rn
