#include "builddata.h"
#include "json.h"
#include "net.h"
#include "paths.h"
#include "proc.h"
#include <deque>
#include <mutex>

namespace rn {
namespace builddata {
namespace {

const wchar_t* kUa = L"fn-offline-launcher/1.0";
const DWORD kTimeout = 5 * 60 * 1000;

std::string noBom(std::string s) {
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

// Directory.EnumerateDirectories(dir, name, AllDirectories).FirstOrDefault(): breadth-first, "" = none.
std::wstring findDir(const std::wstring& dir, const wchar_t* name) {
    std::deque<std::wstring> pending{ dir };
    while (!pending.empty()) {
        std::wstring d = pending.front();
        pending.pop_front();
        for (auto& s : subdirs(d)) {
            if (ieq(fileName(s), name)) return s;
            DWORD a = GetFileAttributesW(s.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) pending.push_back(s);
        }
    }
    return L"";
}

// Every file under dir (recursive), full paths.
void allFiles(const std::wstring& dir, std::vector<std::wstring>* out) {
    std::deque<std::wstring> pending{ dir };
    while (!pending.empty()) {
        std::wstring d = pending.front();
        pending.pop_front();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((d + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = d + L"\\" + fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out->push_back(p);
            else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) pending.push_back(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// Directory.Delete(dir, recursive: true), best effort (junctions removed, not followed).
void deleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) deleteTree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
            else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Uncompressed size of every entry, in central directory order (zip and zip64). false = not readable as a zip.
bool zipSizes(const std::wstring& zip, std::vector<uint64_t>* sizes) {
    Handle f(CreateFileW(zip.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr));
    if (!f.ok()) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 22) return false;
    auto readAt = [&](uint64_t off, void* buf, DWORD n) {
        OVERLAPPED o = {};
        o.Offset = (DWORD)off;
        o.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        return ReadFile(f, buf, n, &got, &o) && got == n;
    };
    auto u16 = [](const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); };
    auto u32 = [](const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); };
    auto u64 = [&](const uint8_t* p) { return (uint64_t)u32(p) | ((uint64_t)u32(p + 4) << 32); };
    uint64_t tailLen = std::min<uint64_t>((uint64_t)sz.QuadPart, 65535 + 22);
    std::vector<uint8_t> tail((size_t)tailLen);
    uint64_t tailOff = (uint64_t)sz.QuadPart - tailLen;
    if (!readAt(tailOff, tail.data(), (DWORD)tailLen)) return false;
    size_t e = SIZE_MAX;
    for (size_t i = tail.size() - 22 + 1; i-- > 0;) if (u32(&tail[i]) == 0x06054b50) { e = i; break; }
    if (e == SIZE_MAX) return false;
    uint64_t count = u16(&tail[e + 10]), cdSize = u32(&tail[e + 12]), cdOff = u32(&tail[e + 16]);
    if (count == 0xFFFF || cdSize == 0xFFFFFFFF || cdOff == 0xFFFFFFFF) {
        uint8_t loc[20], rec[56];
        uint64_t locOff = tailOff + e - 20;
        if (tailOff + e < 20 || !readAt(locOff, loc, 20) || u32(loc) != 0x07064b50) return false;
        if (!readAt(u64(loc + 8), rec, 56) || u32(rec) != 0x06064b50) return false;
        count = u64(rec + 32); cdSize = u64(rec + 40); cdOff = u64(rec + 48);
    }
    if (cdSize > (512ull << 20)) return false;
    std::vector<uint8_t> cd((size_t)cdSize);
    if (cdSize && !readAt(cdOff, cd.data(), (DWORD)cdSize)) return false;
    size_t p = 0;
    for (uint64_t k = 0; k < count && p + 46 <= cd.size() && u32(&cd[p]) == 0x02014b50; k++) {
        uint64_t size = u32(&cd[p + 24]);
        size_t nameLen = u16(&cd[p + 28]), extraLen = u16(&cd[p + 30]), commentLen = u16(&cd[p + 32]);
        if (size == 0xFFFFFFFF) {
            size_t x = p + 46 + nameLen, end = std::min(cd.size(), x + extraLen);
            while (x + 4 <= end) {
                size_t id = u16(&cd[x]), len = u16(&cd[x + 2]);
                if (id == 0x0001 && len >= 8 && x + 12 <= end) { size = u64(&cd[x + 4]); break; }
                x += 4 + len;
            }
        }
        sizes->push_back(size);
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

// Register: moves the build to <root>\<version> (or build-<time>), removes cleanup, reads it back.
bool registerBuild(const std::wstring& buildRoot, const LogFn& log, const std::wstring& cleanup, BuildInfo* added) {
    std::wstring v, err;
    if (!BuildInfo::scanVersion(paths::shipping(buildRoot), &v, &err)) { log(L"Could not add the build: " + err); return false; }
    std::wstring name;
    for (size_t p = v.find(L"Release-"); p != std::wstring::npos && name.empty(); p = v.find(L"Release-", p + 1)) {
        size_t i = p + 8, a = i;
        while (i < v.size() && ((v[i] >= L'0' && v[i] <= L'9') || v[i] == L'.')) i++;
        if (i > a && v.compare(i, 3, L"-CL") == 0) name = v.substr(a, i - a);
    }
    if (name.empty()) name = L"build-" + nowTime(L"yyyyMMddHHmm");
    std::wstring dest = join(paths::root(), name);
    if (dirExists(dest)) dest += L"-" + nowTime(L"HHmmss");
    if (!MoveFileExW(buildRoot.c_str(), dest.c_str(), 0)) {
        log(L"Could not move " + buildRoot + L" to " + dest + L": " + winError(GetLastError()));
        return false;
    }
    if (!cleanup.empty() && dirExists(cleanup)) deleteTree(cleanup);
    log(L"Build " + (v.empty() ? std::wstring(L"?") : v) + L" added at " + dest);
    std::wstring why;
    return BuildInfo::read(dest, added, &why);
}

} // namespace

std::wstring fetch(const BuildInfo& b, const LogFn& log) {
    std::wstring off = paths::offline(b.dir);
    std::wstring aes = join(off, L"aes.json");
    if (!fileExists(aes)) {
        // The archive names files "3.60", "17.50", "2.4.2"...: try the version as read and its two-digit form.
        std::vector<std::wstring> names;
        for (auto& n : { b.version, fmt(L"%d.%02d", b.major, b.minor), fmt(L"%d.%d0", b.major, b.minor), fmt(L"%d.%d.0", b.major, b.minor) }) {
            bool dup = false;
            for (auto& x : names) dup = dup || x == n;
            if (!dup) names.push_back(n);
        }
        std::string body;
        std::wstring got;
        for (auto& n : names) {
            net::Response r = net::get(L"https://raw.githubusercontent.com/dippyshere/fortnite-aes-archive/master/api/archive/" + n + L".json", kUa, nullptr, kTimeout);
            if (r.ok()) { body = r.body; got = n; break; }
        }
        std::wstring tried;
        for (auto& n : names) tried += (tried.empty() ? L"" : L", ") + n;
        std::wstring tail = L". Not needed if the build is not encrypted; otherwise put aes.json in " + off + L".";
        if (got.empty()) log(L"No AES keys for " + b.version + L" (not in the archive (tried " + tried + L"))" + tail);
        else if (!writeFile(aes, noBom(body))) log(L"No AES keys for " + b.version + L" (" + ioError(GetLastError(), aes) + L")" + tail);
        else log(L"AES keys for " + b.version + L" downloaded (dippyshere/fortnite-aes-archive, " + got + L".json).");
    }
    std::wstring usmap = join(off, L"mappings.usmap");
    if (!fileExists(usmap) && b.major >= 11) {   // unversioned packages (mappings needed) from ~S11
        const std::wstring repo = L"taiothegamer-tech/Fortnite-Mappings-AES-Keys", branch = L"main";
        net::Response t = net::get(L"https://api.github.com/repos/" + repo + L"/git/trees/" + branch + L"?recursive=1", kUa, nullptr, kTimeout);
        if (!t.ok()) log(L"Mappings: " + repo + L": " + t.error);
        else {
            std::vector<std::wstring> paths;
            yyjson_doc* doc = json::parse(t.body);
            yyjson_val* tree = yyjson_obj_get(doc ? yyjson_doc_get_root(doc) : nullptr, "tree");
            if (tree && yyjson_is_arr(tree)) {
                size_t i, n;
                yyjson_val* e;
                yyjson_arr_foreach(tree, i, n, e) paths.push_back(json::wstr(yyjson_obj_get(e, "path")));
            }
            yyjson_doc_free(doc);
            std::wstring hit;
            std::wstring exact = L"Release-" + b.version + fmt(L"-CL-%lld", (long long)b.cl), loose = L"Release-" + b.version + L"-";
            auto usmapFile = [](const std::wstring& p) { return p.size() >= 6 && p.compare(p.size() - 6, 6, L".usmap") == 0; };
            for (auto& p : paths) if (hit.empty() && usmapFile(p) && contains(p, exact)) hit = p;
            for (auto& p : paths) if (hit.empty() && usmapFile(p) && contains(p, loose)) hit = p;
            if (!tree || !yyjson_is_arr(tree)) log(L"Mappings: " + repo + L": Object reference not set to an instance of an object.");
            else if (!hit.empty()) {
                std::wstring url = L"https://raw.githubusercontent.com/" + repo + L"/" + branch + L"/";
                for (size_t a = 0;;) {
                    size_t s = hit.find(L'/', a);
                    url += net::escapeData(hit.substr(a, s == std::wstring::npos ? std::wstring::npos : s - a));
                    if (s == std::wstring::npos) break;
                    url += L"/";
                    a = s + 1;
                }
                net::Response m = net::get(url, kUa, nullptr, kTimeout);
                if (!m.ok()) log(L"Mappings: " + repo + L": " + m.error);
                else if (!writeFile(usmap, m.body)) log(L"Mappings: " + repo + L": " + ioError(GetLastError(), usmap));
                else log(L"Mappings for " + b.version + L" downloaded (" + repo + L").");
            }
        }
        if (!fileExists(usmap)) log(L"No mappings for " + b.version + L": put mappings.usmap in " + off + L" (S11+ cosmetics cannot be read without them).");
    }
    return off;
}

bool generateCosmetics(const BuildInfo& b, const LogFn& log, std::wstring* error) {
    std::wstring off = paths::offline(b.dir);
    std::wstring logFile = join(off, L"cosmetics-gen.log");
    std::mutex m;
    std::string lines;
    LogFn L = [&](const std::wstring& s) {
        {
            std::lock_guard<std::mutex> g(m);
            lines += narrow(nowTime(L"HH:mm:ss") + L" " + s + L"\r\n");
        }
        log(s);
    };
    std::wstring why;
    bool ok = false;
    fetch(b, L);
    std::wstring aes = join(off, L"aes.json"), usmap = join(off, L"mappings.usmap");
    std::wstring args = L"--build \"" + b.dir + L"\" --out \"" + join(off, L"cosmetics.json") + L"\" --engine " + b.engine + L" --version " + b.version;
    if (fileExists(aes)) args += L" --aes \"" + aes + L"\"";
    if (fileExists(usmap)) args += L" --mappings \"" + usmap + L"\"";
    if (!fileExists(paths::cosmeticsGen())) why = L"launcher\\tools\\cosmetics-gen\\fn-cosmetics-gen.exe is missing";
    else {
        L(L"fn-cosmetics-gen " + args);
        proc::Start s;
        s.exe = paths::cosmeticsGen();
        s.args = args;
        s.job = proc::toolsJob();
        s.priority = BELOW_NORMAL_PRIORITY_CLASS;
        std::wstring lastErr, startErr;
        bool hasLastErr = false;
        int code = proc::run(s, [&](const std::wstring& line, bool isErr) {
            if (isErr) { std::lock_guard<std::mutex> g(m); lastErr = line; hasLastErr = true; }
            L(line);
        }, &startErr);
        if (code == -1 && !startErr.empty()) { why = startErr; L(L"Error: " + startErr); }
        else if (code == 0) ok = true;
        else if (code == 3) why = L"the build's Paks could not be opened: AES key missing or wrong (put aes.json in the build's offline folder)";
        else if (code == 4) why = std::wstring(L"no cosmetics found in the Paks") + (b.major >= 11 && !fileExists(usmap) ? L" (mappings.usmap missing)" : L"");
        else why = hasLastErr ? lastErr : fmt(L"the generator exited with code %d", code);
    }
    if (!ok) L(L"Failed: " + why);
    {
        std::lock_guard<std::mutex> g(m);
        writeFile(logFile, lines);
    }
    if (error) *error = ok ? L"" : why;
    return ok;
}

bool addZip(const std::wstring& zip, const LogFn& log, BuildInfo* added) {
    std::wstring tmp = join(paths::root(), L"_extrayendo_" + nowTime(L"yyyyMMddHHmmss"));
    std::vector<uint64_t> sizes;
    zipSizes(zip, &sizes);
    uint64_t total = 0;
    for (auto s : sizes) total += s;
    if (!createDirs(tmp)) { log(L"Could not extract the zip: " + ioError(GetLastError(), tmp)); return false; }
    wchar_t sys[MAX_PATH];
    UINT sl = GetSystemDirectoryW(sys, MAX_PATH);
    proc::Start s;
    s.exe = join(std::wstring(sys, sl), L"tar.exe");
    s.args = L"-xvf \"" + zip + L"\" -C \"" + tmp + L"\"";
    s.job = proc::toolsJob();
    s.priority = BELOW_NORMAL_PRIORITY_CLASS;
    size_t entry = 0;
    uint64_t done = 0;
    int lastPct = -1;
    std::wstring lastErr, startErr;
    std::mutex m;
    int code = proc::run(s, [&](const std::wstring& line, bool) {
        std::lock_guard<std::mutex> g(m);
        if (line.size() > 2 && line[0] == L'x' && line[1] == L' ') {
            if (entry < sizes.size()) done += sizes[entry];
            entry++;
            int pct = (int)(done * 100 / (total ? total : 1));
            if (pct != lastPct && pct % 5 == 0) { lastPct = pct; log(fmt(L"Extracting... %d %%", pct)); }
        } else if (!blank(line)) lastErr = trim(line);
    }, &startErr);
    if (code != 0) {
        log(L"Could not extract the zip: " + (code == -1 ? startErr : lastErr.empty() ? fmt(L"tar exited with code %d", code) : lastErr) +
            L" (partial files in " + tmp + L").");
        return false;
    }
    std::wstring game = findDir(tmp, L"FortniteGame");
    if (game.empty()) { log(L"The zip does not contain a build (no FortniteGame folder)."); return false; }
    return registerBuild(parentDir(game), log, tmp, added);
}

bool addFolder(const std::wstring& folder, const LogFn& log, BuildInfo* added) {
    std::wstring root;
    if (fileExists(paths::shipping(folder))) root = folder;
    else {
        std::wstring game = findDir(folder, L"FortniteGame");
        if (!game.empty()) root = parentDir(game);
    }
    if (root.empty() || !fileExists(paths::shipping(root))) {
        log(L"That folder has no FortniteGame\\Binaries\\Win64\\FortniteClient-Win64-Shipping.exe.");
        return false;
    }
    std::wstring R = paths::root();
    if (isUnder(root, R) && ieq(parentDir(trimEndSlash(fullPath(root))), R)) {
        std::wstring why;
        return BuildInfo::read(root, added, &why);
    }
    if (ieq(pathRoot(fullPath(root)), pathRoot(R))) return registerBuild(root, log, L"", added);
    std::wstring tmp = join(R, L"_copiando_" + nowTime(L"yyyyMMddHHmmss"));
    std::vector<std::wstring> files;
    allFiles(trimEndSlash(root), &files);
    int prio = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::wstring base = trimEndSlash(root);
    bool copied = true;
    for (auto& f : files) {
        std::wstring dest = join(tmp, f.substr(base.size() + 1));
        createDirs(parentDir(dest));
        if (!CopyFileW(f.c_str(), dest.c_str(), FALSE)) {
            log(L"Could not copy " + f + L": " + winError(GetLastError()));
            copied = false;
            break;
        }
    }
    SetThreadPriority(GetCurrentThread(), prio);
    if (!copied) return false;
    log(L"Copy finished.");
    return registerBuild(tmp, log, L"", added);
}

} // namespace builddata
} // namespace rn
