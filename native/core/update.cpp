#include "update.h"
#include "json.h"
#include "net.h"
#include "paths.h"
#include "util.h"
#include "version.h"
#include <shellapi.h>

namespace rn {
namespace update {

const wchar_t* const kRepo = L"Radarnite/radarnite-launcher";

namespace {

const wchar_t* kUa = L"radarnite-launcher";
const DWORD kTimeout = 10 * 60 * 1000;

void log(const std::wstring& s) { appendFile(join(paths::launcherDir(), L"update.log"), narrow(nowIso() + L" " + s + L"\r\n")); }

// int.TryParse (NumberStyles.Integer, invariant): optional white space and sign around ASCII digits.
bool tryInt(const std::wstring& s, int* out) {
    size_t i = 0, n = s.size();
    auto ws = [](wchar_t c) { return c == L' ' || (c >= 0x09 && c <= 0x0D); };
    while (i < n && ws(s[i])) i++;
    while (n > i && ws(s[n - 1])) n--;
    bool neg = false;
    if (i < n && (s[i] == L'+' || s[i] == L'-')) neg = s[i++] == L'-';
    if (i >= n) return false;
    long long v = 0;
    for (; i < n; i++) {
        if (s[i] < L'0' || s[i] > L'9') return false;
        v = v * 10 + (s[i] - L'0');
        if (v > 2147483648ll) return false;
    }
    if (neg) v = -v;
    if (v > 2147483647ll) return false;
    *out = (int)v;
    return true;
}

bool lessOrEqual(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major;
    if (a.minor != b.minor) return a.minor < b.minor;
    return a.patch <= b.patch;
}

bool isHex(wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'); }

// The first white-space separated token that starts with 64 hex digits (the .sha256 file format).
std::wstring first64Hex(const std::wstring& s) {
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isWhite(s[i])) i++;
        size_t a = i;
        while (i < s.size() && !isWhite(s[i])) i++;
        if (i - a >= 64) {
            bool hexOk = true;
            for (size_t k = a; k < a + 64; k++) if (!isHex(s[k])) { hexOk = false; break; }
            if (hexOk) {
                std::wstring h = s.substr(a, 64);
                for (auto& c : h) c = (wchar_t)towlower(c);
                return h;
            }
        }
    }
    return L"";
}

std::wstring lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

} // namespace

std::wstring Version::str() const { return fmt(L"%d.%d.%d", major, minor, patch); }

Version current() { return Version{ RN_VERSION_MAJOR, RN_VERSION_MINOR, RN_VERSION_PATCH }; }

bool parseTag(const std::wstring& tag, Version* v) {
    *v = Version();
    std::wstring s = trim(tag);
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s = s.substr(1);
    std::wstring core = s.substr(0, s.find_first_of(L"-+"));
    std::vector<std::wstring> parts;
    for (size_t a = 0;;) {
        size_t d = core.find(L'.', a);
        parts.push_back(core.substr(a, d == std::wstring::npos ? std::wstring::npos : d - a));
        if (d == std::wstring::npos) break;
        a = d + 1;
    }
    int maj, min, pat = 0;
    if (parts.size() < 2 || !tryInt(parts[0], &maj) || !tryInt(parts[1], &min)) return false;
    if (parts.size() >= 3 && !tryInt(parts[2], &pat)) return false;
    if (maj < 0 || min < 0 || pat < 0) return false;   // new Version(...) would have thrown
    *v = Version{ maj, min, pat };
    return true;
}

Release check() {
    Release r;
    net::Response res = net::get(L"https://api.github.com/repos/" + std::wstring(kRepo) + L"/releases/latest", kUa,
                                 L"Accept: application/vnd.github+json\r\n", kTimeout);
    if (!res.ok()) { log(L"check failed: " + res.error); return r; }
    yyjson_doc* doc = json::parse(res.body);
    yyjson_val* j = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!j || !yyjson_is_obj(j)) { yyjson_doc_free(doc); log(L"check failed: the latest release is not a JSON object"); return r; }
    std::wstring tag = json::wstr(yyjson_obj_get(j, "tag_name"));
    Version ver;
    if (!parseTag(tag, &ver)) { yyjson_doc_free(doc); log(L"latest release tag is not a version: '" + tag + L"'"); return r; }
    std::wstring installer, sha, digest;
    bool hasInstaller = false, hasSha = false;
    yyjson_val* assets = yyjson_obj_get(j, "assets");
    if (assets && yyjson_is_arr(assets)) {
        size_t i, n;
        yyjson_val* a;
        yyjson_arr_foreach(assets, i, n, a) {
            std::wstring name = json::wstr(yyjson_obj_get(a, "name"));
            std::wstring url = json::wstr(yyjson_obj_get(a, "browser_download_url"));
            if (istarts(name, L"RadarniteLauncherSetup-") && iends(name, L".exe")) {
                installer = url; hasInstaller = yyjson_is_str(yyjson_obj_get(a, "browser_download_url"));
                digest = json::wstr(yyjson_obj_get(a, "digest"));
            } else if (iends(name, L".sha256")) { sha = url; hasSha = yyjson_is_str(yyjson_obj_get(a, "browser_download_url")); }
        }
    }
    std::wstring notes = json::wstr(yyjson_obj_get(j, "body"));
    yyjson_doc_free(doc);
    Version cur = current();
    if (lessOrEqual(ver, cur)) { log(L"up to date (installed " + cur.str() + L", latest " + ver.str() + L")"); return r; }
    if (!hasInstaller) { log(L"release " + tag + L" has no RadarniteLauncherSetup-*.exe asset"); return r; }
    log(L"update available: " + ver.str() + L" > " + cur.str());
    r.found = true;
    r.version = ver;
    r.tag = tag;
    r.installerUrl = installer;
    r.sha256Url = hasSha ? sha : L"";
    r.digest = digest;
    r.notes = notes;
    return r;
}

bool downloadVerifyRun(const Release& r) {
    wchar_t tmp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    std::wstring dst = join(std::wstring(tmp, n), L"RadarniteLauncherSetup-" + r.version.str() + L".exe");
    log(L"downloading " + r.installerUrl + L" -> " + dst);
    std::wstring got, error;
    if (!net::download(r.installerUrl, kUa, kTimeout, dst, &got, &error)) {
        DeleteFileW(dst.c_str());
        log(L"download/run failed: " + error);
        return false;
    }
    std::wstring want;
    if (!r.sha256Url.empty()) {
        net::Response s = net::get(r.sha256Url, kUa, nullptr, kTimeout);
        if (s.ok()) want = first64Hex(widen(s.body));
        else log(L"sha256 asset download failed: " + s.error);
    }
    if (want.empty() && istarts(r.digest, L"sha256:")) want = lower(trim(r.digest.substr(7)));
    if (want.empty()) { log(L"no sha256 to verify against; refusing to run the installer"); return false; }
    if (!ieq(got, want)) {
        log(L"sha256 mismatch: got " + got + L", want " + want + L"; not running");
        DeleteFileW(dst.c_str());
        return false;
    }
    log(L"sha256 ok (" + got + L"); starting installer");
    SHELLEXECUTEINFOW se = { sizeof se };
    se.fMask = SEE_MASK_NOASYNC;
    se.lpVerb = L"open";
    se.lpFile = dst.c_str();
    se.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&se)) { log(L"download/run failed: " + winError(GetLastError())); return false; }
    return true;
}

bool runInteractive(HWND owner, bool manual, bool canInstall, const std::function<void(const std::wstring&, Level)>& toast) {
    if (manual) toast(L"Checking for updates…", Level::Info);
    Release r = check();
    if (!r.found) {
        if (manual) toast(L"Radarnite Launcher is up to date (" + current().str() + L").", Level::Ok);
        return false;
    }
    if (!canInstall) {
        toast(L"Radarnite Launcher " + r.version.str() + L" is available (finish playing, then reopen the launcher).", Level::Info);
        return false;
    }
    std::wstring text = L"Radarnite Launcher " + r.version.str() + L" is available (you have " + current().str() +
                        L").\r\n\r\nDownload and install it now? The launcher will close while it updates.";
    if (MessageBoxW(owner, text.c_str(), L"Radarnite Launcher update", MB_YESNO | MB_ICONINFORMATION) != IDYES) return false;
    toast(L"Downloading Radarnite Launcher " + r.version.str() + L"…", Level::Info);
    if (downloadVerifyRun(r)) return true;
    toast(L"Update failed (see launcher\\update.log).", Level::Bad);
    return false;
}

} // namespace update
} // namespace rn
