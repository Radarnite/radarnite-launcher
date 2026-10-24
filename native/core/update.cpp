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
const wchar_t* kAccept = L"Accept: application/vnd.github+json\r\n";
const DWORD kTimeout = 10 * 60 * 1000;

// version.h: the numbers of the version resource must be those of RN_VERSION_STR (a build that only overrides
// RN_VERSION_MAJOR/MINOR/PATCH, the pre-RN_VERSION way, stops here instead of reporting version.h's default).
constexpr int versionField(const char* s, int index) {
    int field = 0, value = 0;
    for (; *s && *s != '-' && *s != '+'; s++) {
        if (*s == '.') { if (field == index) return value; field++; value = 0; }
        else value = value * 10 + (*s - '0');
    }
    return field == index ? value : -1;
}
static_assert(versionField(RN_VERSION_STR, 0) == RN_VERSION_MAJOR && versionField(RN_VERSION_STR, 1) == RN_VERSION_MINOR &&
              versionField(RN_VERSION_STR, 2) == RN_VERSION_PATCH,
              "version.h: RN_VERSION_STR differs from RN_VERSION_MAJOR/MINOR/PATCH (build with RN_VERSION=<semver>)");

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

bool numeric(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s) if (c < L'0' || c > L'9') return false;
    return true;
}

int sign(int c) { return c < 0 ? -1 : c > 0 ? 1 : 0; }

// One pre-release identifier: numbers by value (any length: by digit count, then digits), before alphanumerics,
// which compare in ASCII order.
int compareIdentifier(const std::wstring& a, const std::wstring& b) {
    bool na = numeric(a), nb = numeric(b);
    if (na != nb) return na ? -1 : 1;
    if (!na) return sign(a.compare(b));
    size_t i = a.find_first_not_of(L'0'), j = b.find_first_not_of(L'0');
    std::wstring x = i == std::wstring::npos ? L"" : a.substr(i), y = j == std::wstring::npos ? L"" : b.substr(j);
    if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
    return sign(x.compare(y));
}

std::vector<std::wstring> dotted(const std::wstring& s) {
    std::vector<std::wstring> parts;
    for (size_t a = 0;;) {
        size_t d = s.find(L'.', a);
        parts.push_back(s.substr(a, d == std::wstring::npos ? std::wstring::npos : d - a));
        if (d == std::wstring::npos) return parts;
        a = d + 1;
    }
}

bool isHex(wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'); }

} // namespace

std::wstring sha256Token(const std::wstring& s) {
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

namespace {

std::wstring lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

} // namespace

std::wstring Version::str() const { return fmt(L"%d.%d.%d", major, minor, patch) + (pre.empty() ? L"" : L"-" + pre); }

Version current() {
    static const Version v = [] { Version x; parseTag(L"" RN_VERSION_STR, &x); return x; }();
    return v;
}

bool parseTag(const std::wstring& tag, Version* v) {
    *v = Version();
    std::wstring s = trim(tag);
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s = s.substr(1);
    size_t cut = s.find_first_of(L"-+");
    std::wstring core = s.substr(0, cut), pre;
    if (cut != std::wstring::npos && s[cut] == L'-') {
        size_t plus = s.find(L'+', cut + 1);
        pre = s.substr(cut + 1, plus == std::wstring::npos ? std::wstring::npos : plus - cut - 1);
    }
    std::vector<std::wstring> parts = dotted(core);
    int maj, min, pat = 0;
    if (parts.size() < 2 || !tryInt(parts[0], &maj) || !tryInt(parts[1], &min)) return false;
    if (parts.size() >= 3 && !tryInt(parts[2], &pat)) return false;
    if (maj < 0 || min < 0 || pat < 0) return false;   // new Version(...) would have thrown
    *v = Version{ maj, min, pat, pre };
    return true;
}

int compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre.empty() || b.pre.empty()) return a.pre.empty() == b.pre.empty() ? 0 : a.pre.empty() ? 1 : -1;
    std::vector<std::wstring> x = dotted(a.pre), y = dotted(b.pre);
    for (size_t i = 0; i < x.size() && i < y.size(); i++)
        if (int c = compareIdentifier(x[i], y[i])) return c;
    return x.size() == y.size() ? 0 : x.size() < y.size() ? -1 : 1;
}

namespace {

// http://127.0.0.1:<port>[/...] or http://localhost:<port>[/...]: nothing that could name another host
bool loopbackHttp(const std::wstring& u) {
    for (const wchar_t* p : { L"http://127.0.0.1:", L"http://localhost:" }) {
        if (!istarts(u, p)) continue;
        size_t i = wcslen(p), digits = i;
        while (i < u.size() && u[i] >= L'0' && u[i] <= L'9') i++;
        return i > digits && (i == u.size() || u[i] == L'/');
    }
    return false;
}

// The GitHub API root. Test switch RADARNITE_UPDATE_API (native/core/test/update-check.ps1): a fake API on this PC's
// loopback; any other value is ignored. Either way it is logged, so an update.log shows where a check went.
std::wstring apiBase() {
    std::wstring o = getEnv(L"RADARNITE_UPDATE_API");
    if (o.empty()) return L"https://api.github.com";
    while (!o.empty() && o.back() == L'/') o.pop_back();
    if (loopbackHttp(o)) { log(L"RADARNITE_UPDATE_API: using " + o); return o; }
    log(L"RADARNITE_UPDATE_API ignored (only http://127.0.0.1:<port> or http://localhost:<port>): " + o);
    return L"https://api.github.com";
}

// One release object of the API (releases/latest, or an entry of /releases), read as Update.cs CheckAsync did.
struct Candidate {
    Version version;
    std::wstring tag, installer, sha, digest, notes;
    bool hasInstaller = false, hasSha = false;
};

// false = its tag is not a version
bool readRelease(yyjson_val* j, Candidate* c) {
    c->tag = json::wstr(yyjson_obj_get(j, "tag_name"));
    if (!parseTag(c->tag, &c->version)) return false;
    yyjson_val* assets = yyjson_obj_get(j, "assets");
    if (assets && yyjson_is_arr(assets)) {
        size_t i, n;
        yyjson_val* a;
        yyjson_arr_foreach(assets, i, n, a) {
            std::wstring name = json::wstr(yyjson_obj_get(a, "name"));
            std::wstring url = json::wstr(yyjson_obj_get(a, "browser_download_url"));
            if (istarts(name, L"RadarniteLauncherSetup-") && iends(name, L".exe")) {
                c->installer = url; c->hasInstaller = yyjson_is_str(yyjson_obj_get(a, "browser_download_url"));
                c->digest = json::wstr(yyjson_obj_get(a, "digest"));
            } else if (iends(name, L".sha256")) { c->sha = url; c->hasSha = yyjson_is_str(yyjson_obj_get(a, "browser_download_url")); }
        }
    }
    c->notes = json::wstr(yyjson_obj_get(j, "body"));
    return true;
}

// c is offered when it is newer than cur and has the installer; upToDate = the update.log line otherwise
Release offer(const Candidate& c, const Version& cur, const std::wstring& upToDate) {
    Release r;
    if (compare(c.version, cur) <= 0) { log(upToDate); return r; }
    if (!c.hasInstaller) { log(L"release " + c.tag + L" has no RadarniteLauncherSetup-*.exe asset"); return r; }
    log(L"update available: " + c.version.str() + L" > " + cur.str());
    r.found = true;
    r.version = c.version;
    r.tag = c.tag;
    r.installerUrl = c.installer;
    r.sha256Url = c.hasSha ? c.sha : L"";
    r.digest = c.digest;
    r.notes = c.notes;
    return r;
}

// The GitHub releases (2.x's check, then the pre-release builds' list).
Release checkGithub(const Version& cur) {
    std::wstring repo = apiBase() + L"/repos/" + kRepo + L"/releases";
    if (cur.pre.empty()) {
        // A release build: 2.x exactly. releases/latest is never a GitHub prerelease, and its tag is read as 2.x read
        // it (a pre-release suffix on a full release's tag ignored, also in the version shown and the file name).
        net::Response res = net::get(repo + L"/latest", kUa, kAccept, kTimeout);
        if (!res.ok()) { log(L"check failed: " + res.error); return Release(); }
        yyjson_doc* doc = json::parse(res.body);
        yyjson_val* j = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!j || !yyjson_is_obj(j)) { yyjson_doc_free(doc); log(L"check failed: the latest release is not a JSON object"); return Release(); }
        Candidate c;
        bool isVersion = readRelease(j, &c);
        yyjson_doc_free(doc);
        if (!isVersion) { log(L"latest release tag is not a version: '" + c.tag + L"'"); return Release(); }
        c.version.pre.clear();
        return offer(c, cur, L"up to date (installed " + cur.str() + L", latest " + c.version.str() + L")");
    }

    // A pre-release build: the latest release and the newest one of the list (the first that is not a draft, a
    // prerelease included), both asked at once; the newer of the two is offered when it is newer than this build.
    // Either request may fail on its own.
    net::Response latest, list;
    parallel({ [&] { latest = net::get(repo + L"/latest", kUa, kAccept, kTimeout); },
               [&] { list = net::get(repo + L"?per_page=10", kUa, kAccept, kTimeout); } });
    Candidate a, b;
    bool hasA = false, hasB = false;
    if (!latest.ok()) log(L"check failed (latest release): " + latest.error);
    else {
        yyjson_doc* doc = json::parse(latest.body);
        yyjson_val* j = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!j || !yyjson_is_obj(j)) log(L"check failed: the latest release is not a JSON object");
        else if (!(hasA = readRelease(j, &a))) log(L"latest release tag is not a version: '" + a.tag + L"'");
        yyjson_doc_free(doc);
    }
    if (!list.ok()) log(L"check failed (release list): " + list.error);
    else {
        yyjson_doc* doc = json::parse(list.body);
        yyjson_val* j = doc ? yyjson_doc_get_root(doc) : nullptr;
        yyjson_val* first = nullptr;
        if (j && yyjson_is_arr(j)) {
            size_t i, n;
            yyjson_val* e;
            yyjson_arr_foreach(j, i, n, e) {
                if (yyjson_is_obj(e) && !yyjson_is_true(yyjson_obj_get(e, "draft"))) { first = e; break; }
            }
        }
        if (!j || !yyjson_is_arr(j)) log(L"check failed: the release list is not a JSON array");
        else if (!first) log(L"the release list has no published release");
        else if (!(hasB = readRelease(first, &b))) log(L"newest release tag is not a version: '" + b.tag + L"'");
        yyjson_doc_free(doc);
    }
    if (!hasA && !hasB) return Release();
    const Candidate& best = !hasB || (hasA && compare(a.version, b.version) >= 0) ? a : b;
    return offer(best, cur, L"up to date (installed " + cur.str() + L", latest " + (hasA ? a.version.str() : L"-") + L", newest " +
                                (hasB ? b.version.str() : L"-") + L")");
}

const wchar_t* kAppKey = L"Software\\Radarnite\\Launcher";
const DWORD kFeedTimeout = 8000;   // the feed is on the owner's own network: a few seconds or it is not there

std::wstring regString(const wchar_t* name) {
    wchar_t buf[2048];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kAppKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}
bool setRegString(const wchar_t* name, const std::wstring& v) {
    if (v.empty()) {
        LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, kAppKey, name);
        return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
    }
    return RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, name, REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool httpUrl(const std::wstring& u) { return (istarts(u, L"http://") || istarts(u, L"https://")) && u.find_first_of(L" \t\r\n\"") == std::wstring::npos; }

// The test feed: a test build newer than cur, or nothing (off, offline, older, broken: logged).
Release checkFeed(const Version& cur) {
    std::wstring url = testFeed();
    if (url.empty()) return Release();
    net::Response res = net::get(url, kUa, nullptr, kFeedTimeout);
    if (!res.ok()) { log(L"test feed " + url + L": " + res.error); return Release(); }
    Release r;
    std::wstring error;
    if (!parseFeed(res.body, url, &r, &error)) { log(L"test feed " + url + L": " + error); return Release(); }
    if (compare(r.version, cur) <= 0) { log(L"test feed: up to date (installed " + cur.str() + L", feed " + r.version.str() + L")"); return Release(); }
    log(L"test build available: " + r.version.str() + L" > " + cur.str());
    return r;
}


} // namespace

Release check(bool feedOnly) {
    Version cur = current();
    if (feedOnly) return checkFeed(cur);
    Release gh, feed;
    parallel({ [&] { gh = checkGithub(cur); }, [&] { feed = checkFeed(cur); } });
    if (feed.found && (!gh.found || compare(feed.version, gh.version) > 0)) return feed;
    return gh;   // a release at least as new as the test build wins
}

std::wstring testFeed() {
    std::wstring u = trim(regString(L"TestFeed"));
    return httpUrl(u) ? u : L"";
}

bool setTestFeed(const std::wstring& url) {
    std::wstring u = trim(url);
    if (ieq(u, L"off")) u.clear();
    if (!u.empty() && !httpUrl(u)) return false;
    return setRegString(L"TestFeed", u);
}

bool parseFeed(const std::string& body, const std::wstring& feedUrl, Release* r, std::wstring* error) {
    *r = Release();
    yyjson_doc* doc = json::parse(body);
    yyjson_val* j = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!j || !yyjson_is_obj(j)) { yyjson_doc_free(doc); *error = L"not a JSON object"; return false; }
    std::wstring ver = json::wstr(yyjson_obj_get(j, "version")), inst = trim(json::wstr(yyjson_obj_get(j, "installer")));
    std::wstring sha = json::wstr(yyjson_obj_get(j, "sha256"));
    r->notes = json::wstr(yyjson_obj_get(j, "notes"));
    yyjson_doc_free(doc);
    if (!parseTag(ver, &r->version)) { *error = L"version is not a version: '" + ver + L"'"; return false; }
    std::wstring hex = sha256Token(sha);
    if (hex.empty() || trim(sha).size() != 64) { *error = L"sha256 is not 64 hex digits: '" + sha + L"'"; return false; }
    if (inst.empty()) { *error = L"no installer"; return false; }
    if (inst.find(L"://") == std::wstring::npos) {
        // a file name next to feed.json (the name only: no folder, no query)
        if (inst.find_first_of(L"/\\?#:") != std::wstring::npos || inst[0] == L'.') { *error = L"installer is not a file name: '" + inst + L"'"; return false; }
        size_t slash = feedUrl.find_last_of(L'/');
        inst = feedUrl.substr(0, slash + 1) + inst;
    }
    if (!httpUrl(inst)) { *error = L"installer is not an http(s) URL: '" + inst + L"'"; return false; }
    r->found = true;
    r->test = true;
    r->tag = ver;
    r->installerUrl = inst;
    r->digest = L"sha256:" + hex;
    return true;
}

std::wstring takeInstalledTest() {
    std::wstring v = regString(L"TestInstalling");
    if (!v.empty()) setRegString(L"TestInstalling", L"");
    return v;
}

bool downloadVerifyRun(const Release& r, const std::wstring& args) {
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
        if (s.ok()) want = sha256Token(widen(s.body));
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
    if (!args.empty()) se.lpParameters = args.c_str();
    se.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&se)) { log(L"download/run failed: " + winError(GetLastError())); return false; }
    return true;
}

bool runInteractive(HWND owner, bool manual, bool canInstall, const std::function<void(const std::wstring&, Level)>& toast,
                    bool testOnly, bool foreground) {
    if (manual) toast(L"Checking for updates…", Level::Info);
    Release r = check(testOnly);   // the periodic check reads the feed alone (GitHub's API is asked at each start)
    if (!r.found) {
        if (manual) toast(L"Radarnite Launcher is up to date (" + current().str() + L").", Level::Ok);
        return false;
    }
    if (r.test) {
        // the test channel: no question, no window of the installer; a game is never interrupted (the next check runs
        // after it closes)
        static std::wstring announced;
        if (!canInstall) {
            if (manual || announced != r.version.str())
                toast(L"Test build " + r.version.str() + L" is ready: it installs once the game is closed.", Level::Info);
            announced = r.version.str();
            return false;
        }
        toast(L"Installing test build " + r.version.str() + L"… the launcher reopens in a moment.", Level::Info);
        if (!setRegString(L"TestInstalling", r.version.str())) log(L"could not note the test build being installed");
        if (downloadVerifyRun(r, foreground ? L"/S /RELAUNCH=fg" : L"/S /RELAUNCH=bg")) return true;
        setRegString(L"TestInstalling", L"");
        toast(L"Test build update failed (see launcher\\update.log).", Level::Bad);
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
