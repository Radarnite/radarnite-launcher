#include "buildinfo.h"
#include "json.h"
#include "paths.h"
#include "util.h"
#include <algorithm>
#include <map>
#include <string.h>

namespace rn {

const wchar_t* supportName(Support s) { return s == Support::Yes ? L"Yes" : s == Support::Experimental ? L"Experimental" : L"No"; }

std::wstring BuildInfo::name() const { return fileName(folder.empty() ? dir : folder); }

bool BuildInfo::hasEosAntiCheat() const { return fileExists(join(paths::win64(dir), L"FortniteClient-Win64-Shipping_EAC_EOS.exe")); }

bool BuildInfo::usesIoStore() const {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(join(dir, L"FortniteGame\\Content\\Paks\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do found = !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && iends(fd.cFileName, L".utoc");
    while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// Lobby (backend + redirect): the curl hook and the classic -noeac launch exist up to Chapter 4 (UE 5.1); from 24.x
// (EOS anticheat, EOS login) the method does not apply.
std::pair<Support, std::wstring> BuildInfo::lobby() const {
    if (hasEosAntiCheat() || major >= 24) return { Support::No, L"uses EOS Easy Anti-Cheat and EOS login (24.x+): this method does not apply" };
    if (major >= 20) return { Support::Experimental, L"UE5 (20.x-23.x): curl redirect likely, untested" };
    if (major < 3) return { Support::Experimental, L"Seasons 1-2: untested" };
    return { Support::Yes, L"UE4 (3.x-19.x): curl redirect + -noeac launch" };
}

// Local match: Radarnite match server (S3-S15 supported, S1-2 and S16-19 may not work perfectly, S20+ not supported).
std::pair<Support, std::wstring> BuildInfo::match() const {
    if (lobby().first == Support::No || major >= 20) return { Support::No, L"no match server for this version (Seasons 1-19 only)" };
    if (major >= 16 || major < 3) return { Support::Experimental, L"Seasons 1-2 and 16-19 may not work perfectly" };
    return { Support::Yes, L"Seasons 3-15 supported" };
}

// Engine version per Fortnite version (CUE4Parse needs it to read the Paks).
std::wstring BuildInfo::engineFor(int major, int minor) {
    if (major <= 4) return L"4.20";
    switch (major) {
    case 5: case 6: return L"4.21";
    case 7: return L"4.22";
    case 8: return minor >= 30 ? L"4.23" : L"4.22";
    case 9: case 10: return L"4.23";
    case 11: case 12: return L"4.24";
    case 13: return L"4.25";
    case 14: return minor >= 30 ? L"4.26" : L"4.25";
    case 15: case 16: case 17: case 18: return L"4.26";
    case 19: case 20: return L"5.0";
    case 21: case 22: return L"5.1";
    case 23: return L"5.2";
    case 24: case 25: case 26: case 27: return L"5.3";
    case 28: case 29: return L"5.4";
    case 30: case 31: case 32: case 33: return L"5.5";
    case 34: case 35: case 36: return L"5.6";
    default: return L"5.7";
    }
}

namespace {

bool digit(wchar_t c) { return c >= L'0' && c <= L'9'; }

// int.Parse / long.Parse of an ASCII digit run; false = OverflowException.
bool parseDigits(const std::wstring& s, size_t a, size_t b, int64_t limit, int64_t* out) {
    int64_t v = 0;
    for (size_t i = a; i < b; i++) {
        int d = s[i] - L'0';
        if (v > (limit - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

// Regex.Match(full, @"Release-((\d+)\.(\d+)[\d.]*)-CL-(\d+)"). false = an Int32/Int64 overflow (the exception text in err).
bool make(const std::wstring& dir, const std::wstring& full, const std::wstring& folder, BuildInfo* b, std::wstring* err) {
    for (size_t p = full.find(L"Release-"); p != std::wstring::npos; p = full.find(L"Release-", p + 1)) {
        size_t i = p + 8, a = i;
        while (i < full.size() && digit(full[i])) i++;
        size_t majEnd = i;
        if (majEnd == a || i >= full.size() || full[i] != L'.') continue;
        size_t mb = ++i;
        while (i < full.size() && digit(full[i])) i++;
        size_t minEnd = i;
        if (minEnd == mb) continue;
        while (i < full.size() && (digit(full[i]) || full[i] == L'.')) i++;
        size_t verEnd = i;
        if (full.compare(i, 4, L"-CL-") != 0) continue;
        i += 4;
        size_t cb = i;
        while (i < full.size() && digit(full[i])) i++;
        if (i == cb) continue;
        int64_t maj, min, cl;
        if (!parseDigits(full, a, majEnd, 2147483647ll, &maj) || !parseDigits(full, mb, minEnd, 2147483647ll, &min)) {
            *err = L"Value was either too large or too small for an Int32.";
            return false;
        }
        if (!parseDigits(full, cb, i, 9223372036854775807ll, &cl)) { *err = L"Value was either too large or too small for an Int64."; return false; }
        *b = BuildInfo();
        b->dir = dir; b->version = full.substr(a, verEnd - a); b->major = (int)maj; b->minor = (int)min; b->cl = cl;
        b->engine = BuildInfo::engineFor((int)maj, (int)min); b->folder = folder;
        return true;
    }
    *b = BuildInfo();
    b->dir = dir; b->version = full; b->engine = L"?"; b->folder = folder;
    return true;
}

// \+\+Fortnite\+Release-[\d.]+-CL-\d+ starting at s[i]; returns the match length (0 = none).
template <typename C>
size_t matchVersion(const C* s, size_t n, size_t i) {
    static const char lit[] = "++Fortnite+Release-";
    const size_t L = sizeof lit - 1;
    if (n - i < L) return 0;
    for (size_t k = 0; k < L; k++) if ((unsigned)s[i + k] != (unsigned char)lit[k]) return 0;
    size_t j = i + L, a = j;
    while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == '.')) j++;
    if (j == a || n - j < 4 || s[j] != '-' || s[j + 1] != 'C' || s[j + 2] != 'L' || s[j + 3] != '-') return 0;
    j += 4;
    size_t c = j;
    while (j < n && s[j] >= '0' && s[j] <= '9') j++;
    return j == c ? 0 : j - i;
}

// Boyer-Moore-Horspool over the mapped file: first occurrence of pat at or after from.
size_t findBytes(const uint8_t* d, size_t n, const uint8_t* pat, size_t m, size_t from) {
    if (m == 0 || n < m) return SIZE_MAX;
    size_t skip[256];
    for (auto& s : skip) s = m;
    for (size_t i = 0; i + 1 < m; i++) skip[pat[i]] = m - 1 - i;
    size_t i = from;
    while (i + m <= n) {
        uint8_t last = d[i + m - 1];
        if (last == pat[m - 1] && memcmp(d + i, pat, m - 1) == 0) return i;
        i += skip[last];
    }
    return SIZE_MAX;
}

} // namespace

bool BuildInfo::scanVersion(const std::wstring& exe, std::wstring* version, std::wstring* error) {
    version->clear();
    Handle f(CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!f.ok()) { *error = ioError(GetLastError(), exe); return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz)) { *error = ioError(GetLastError(), exe); return false; }
    if (sz.QuadPart == 0) return true;
    Handle m(CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!m.ok()) { *error = ioError(GetLastError(), exe); return false; }
    const uint8_t* d = (const uint8_t*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!d) { *error = ioError(GetLastError(), exe); return false; }
    size_t n = (size_t)sz.QuadPart;

    // UTF-16: each hit is decoded over 120 bytes and searched like Regex.Match on that text
    static const char lit[] = "++Fortnite+Release-";
    uint8_t wpat[2 * (sizeof lit - 1)];
    for (size_t k = 0; k < sizeof lit - 1; k++) { wpat[2 * k] = (uint8_t)lit[k]; wpat[2 * k + 1] = 0; }
    for (size_t pos = findBytes(d, n, wpat, sizeof wpat, 0); pos != SIZE_MAX; pos = findBytes(d, n, wpat, sizeof wpat, pos + 2)) {
        size_t len = std::min<size_t>(120, n - pos);
        size_t chars = (len - len % 2) / 2;
        wchar_t text[60];
        for (size_t k = 0; k < chars; k++) text[k] = (wchar_t)(d[pos + 2 * k] | (d[pos + 2 * k + 1] << 8));
        for (size_t k = 0; k < chars; k++) {
            size_t l = matchVersion(text, chars, k);
            if (l) { version->assign(text + k, l); UnmapViewOfFile(d); return true; }
        }
    }
    // Some builds keep it only as ASCII.
    for (size_t pos = findBytes(d, n, (const uint8_t*)lit, sizeof lit - 1, 0); pos != SIZE_MAX; pos = findBytes(d, n, (const uint8_t*)lit, sizeof lit - 1, pos + 1)) {
        size_t l = matchVersion(d, n, pos);
        if (l) { for (size_t k = 0; k < l; k++) *version += (wchar_t)d[pos + k]; break; }
    }
    UnmapViewOfFile(d);
    return true;
}

bool BuildInfo::read(const std::wstring& dir, BuildInfo* b, std::wstring* why, std::wstring* error) {
    why->clear();
    if (error) error->clear();
    std::wstring gameDir = dir;
    if (!fileExists(paths::shipping(gameDir))) {
        std::vector<std::wstring> nested;
        auto top = subdirs(dir);
        for (auto& d : top) {
            if (fileExists(paths::shipping(d))) nested.push_back(d);
            for (auto& dd : subdirs(d)) if (fileExists(paths::shipping(dd))) nested.push_back(dd);
        }
        if (nested.size() != 1) {
            bool hasGame = dirExists(join(dir, L"FortniteGame"));
            for (size_t i = 0; !hasGame && i < top.size(); i++) hasGame = dirExists(join(top[i], L"FortniteGame"));
            *why = nested.size() > 1 ? L"more than one game inside"
                 : hasGame ? L"FortniteGame\\Binaries\\Win64\\FortniteClient-Win64-Shipping.exe is missing (incomplete extraction?)"
                 : L"no FortniteGame folder (not a Fortnite build)";
            return false;
        }
        gameDir = nested[0];
    }
    std::wstring exe = paths::shipping(gameDir);
    std::wstring folder = gameDir == dir ? L"" : dir;
    std::wstring cache = join(gameDir, L"offline\\build.json");
    int64_t exeLen = fileSize(exe);
    std::wstring err;
    std::string text;
    if (readFile(cache, &text)) {
        yyjson_doc* doc = json::parse(text, YYJSON_READ_NUMBER_AS_RAW);
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
        yyjson_val* len = yyjson_obj_get(root, "exeLength");
        yyjson_val* ver = yyjson_obj_get(root, "version");
        bool hit = false;
        if (len && yyjson_is_raw(len) && ver && yyjson_is_str(ver)) {
            std::string raw(yyjson_get_raw(len), yyjson_get_len(len));
            hit = raw == std::to_string(exeLen) && make(gameDir, json::wstr(ver), folder, b, &err);
        }
        yyjson_doc_free(doc);
        if (hit) return true;
    }
    std::wstring v, readErr;
    if (!scanVersion(exe, &v, &readErr)) { *why = L"cannot read the game executable: " + readErr; return false; }
    if (v.empty()) {
        *b = BuildInfo();
        b->dir = gameDir; b->version = L"?"; b->engine = L"?"; b->folder = folder;
        return true;
    }
    // read-only or locked folder: the version is still known, only not cached
    if (createDirs(join(gameDir, L"offline")))
        writeFile(cache, "{\"version\":" + json::strict(v) + ",\"exeLength\":" + std::to_string(exeLen) + "}");
    if (!make(gameDir, v, folder, b, &err)) { if (error) *error = err; return false; }
    return true;
}

std::vector<BuildInfo> BuildInfo::scan(const std::wstring& root, std::vector<std::pair<std::wstring, std::wstring>>* skipped) {
    std::vector<BuildInfo> list;
    for (auto& d : subdirs(root)) {
        if (ieq(fileName(d), L"launcher")) continue;
        BuildInfo b;
        std::wstring why, error;
        if (read(d, &b, &why, &error)) list.push_back(b);
        else if (skipped) skipped->emplace_back(d, !error.empty() ? error : !why.empty() ? why : L"not a build");
    }
    std::stable_sort(list.begin(), list.end(), [](const BuildInfo& a, const BuildInfo& b) {
        if (a.major != b.major) return a.major < b.major;
        if (a.minor != b.minor) return a.minor < b.minor;
        return a.name().compare(b.name()) < 0;   // ordinal (the 2.x build runs with InvariantGlobalization)
    });
    return list;
}

std::vector<BuildInfo> BuildInfo::all() { return scan(paths::root(), nullptr); }

int cosmeticsCount(const std::wstring& buildDir) {
    struct Entry { int64_t size; FILETIME mtime; int count; };
    static Lock lock;
    static std::map<std::wstring, Entry> cache;
    std::wstring f = join(buildDir, L"offline\\cosmetics.json");
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    int64_t size = ((int64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    std::wstring key = upper(f);
    {
        Guard g(lock);
        auto it = cache.find(key);
        if (it != cache.end() && it->second.size == size && CompareFileTime(&it->second.mtime, &a.ftLastWriteTime) == 0) return it->second.count;
    }
    int count = 0;
    std::string text;
    if (readFile(f, &text)) {
        yyjson_doc* doc = json::parse(text, YYJSON_READ_NUMBER_AS_RAW);
        yyjson_val* items = yyjson_obj_get(yyjson_doc_get_root(doc), "items");
        if (items && yyjson_is_arr(items)) count = (int)yyjson_arr_size(items);
        yyjson_doc_free(doc);
    }
    Guard g(lock);
    cache[key] = Entry{ size, a.ftLastWriteTime, count };
    return count;
}

} // namespace rn
