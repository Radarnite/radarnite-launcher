// launcher/Port.cs in C++: the catalog (fn-port list + fortnite-api names + the build's state), the import through
// port.ps1 with its live status (stdout lines and fn-port's batch\stdout.txt), and radarnite.com's catalog.json for
// the preview. Large JSON is read once into a buffer, parsed in place, copied into small structs and freed.
#include "port.h"
#include "json.h"
#include "net.h"
#include "paths.h"
#include "proc.h"
#include "sheet.h"
#include <math.h>
#include <string.h>
#include <algorithm>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace rn {
namespace port {
namespace {

const int kChunkSlots = 8000;   // pak chunks port.ps1 hands out (2000-9999): one per item
const int kBatch = 25;          // items per fn-port process (port.ps1's -Batch)
const wchar_t* kUa = L"RadarniteLauncher";

struct TypeName { const char* cls; const char* name; PortType t; };
const TypeName kTypes[] = {
    { "AthenaCharacterItemDefinition", "outfit", PortType::Outfit }, { "AthenaBackpackItemDefinition", "backpack", PortType::Backpack },
    { "AthenaPickaxeItemDefinition", "pickaxe", PortType::Pickaxe }, { "AthenaDanceItemDefinition", "emote", PortType::Emote },
    { "AthenaGliderItemDefinition", "glider", PortType::Glider }, { "AthenaLoadingScreenItemDefinition", "loadingscreen", PortType::LoadingScreen },
    { "AthenaSprayItemDefinition", "spray", PortType::Spray }, { "AthenaEmojiItemDefinition", "emoji", PortType::Emoji },
    { "AthenaItemWrapDefinition", "wrap", PortType::Wrap },
};

std::string lowerA(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }
std::string str(yyjson_val* v) { return v && yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string(); }
yyjson_val* key(yyjson_val* o, const char* k) { return o && yyjson_is_obj(o) ? yyjson_obj_get(o, k) : nullptr; }

uint64_t mtime(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}
uint64_t nowFt() { FILETIME f; GetSystemTimeAsFileTime(&f); return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }
const uint64_t kDay = 864000000000ull;   // 100 ns ticks

// A whole JSON file parsed in place (the buffer lives as long as the doc). Freed by the destructor.
struct Doc {
    std::string buf;
    yyjson_doc* doc = nullptr;
    ~Doc() { if (doc) yyjson_doc_free(doc); }
    yyjson_val* root() const { return doc ? yyjson_doc_get_root(doc) : nullptr; }
    bool read(const std::wstring& path) {
        if (!readFile(path, &buf)) return false;
        return parse();
    }
    bool parse() {
        size_t n = buf.size();
        buf.append(YYJSON_PADDING_SIZE, '\0');
        doc = yyjson_read_opts(&buf[0], n, YYJSON_READ_INSITU | YYJSON_READ_ALLOW_BOM | YYJSON_READ_ALLOW_INVALID_UNICODE, nullptr, nullptr);
        return doc != nullptr;
    }
};

std::wstring itemsFile() { return join(work(), L"items.json"); }
std::wstring namesFile() { return join(work(), L"names.json"); }
std::wstring listMsFile() { return join(work(), L"list-ms.txt"); }
std::wstring speedFile() { return join(work(), L"speed.txt"); }
std::wstring dotnetRoot() { return join(getEnv(L"LOCALAPPDATA"), L"fp-dotnet"); }

struct Tool { std::wstring dotnet, dll, script; };

// fn-port (framework-dependent dll + the .NET runtime it was built for) and port.ps1, or why not.
bool findTool(Tool* t, std::wstring* why) {
    std::wstring dotnet = join(dotnetRoot(), L"dotnet.exe"), local = getEnv(L"LOCALAPPDATA");
    const std::pair<std::wstring, std::wstring> c[] = {
        { join(paths::launcherDir(), L"tools\\fn-port\\fn-port.dll"), join(paths::launcherDir(), L"tools\\fn-port\\port.ps1") },
        { join(local, L"fp-build\\port-cosmeticos\\src\\tools\\fn-port\\bin\\Release\\net10.0\\fn-port.dll"), L"S:\\port-cosmeticos\\ps\\port.ps1" },
    };
    for (auto& x : c)
        if (fileExists(x.first) && fileExists(x.second)) {
            if (!fileExists(dotnet)) { *why = L"the .NET runtime fn-port needs is missing (" + dotnet + L")"; return false; }
            *t = { dotnet, x.first, x.second };
            return true;
        }
    *why = L"the cosmetics importer (fn-port) is not installed: put fn-port.dll and port.ps1 in launcher\\tools\\fn-port";
    return false;
}

// The Paks folder of the installed game (Epic Games Launcher's install list), else the default path; "" = none.
std::wstring currentGamePaks() {
    Doc d;
    if (d.read(L"C:\\ProgramData\\Epic\\UnrealEngineLauncher\\LauncherInstalled.dat")) {
        yyjson_val* list = key(d.root(), "InstallationList");
        size_t i, n;
        yyjson_val* e;
        if (list && yyjson_is_arr(list))
            yyjson_arr_foreach(list, i, n, e) {
                if (str(key(e, "AppName")) != "Fortnite") continue;
                std::wstring loc = json::wstr(key(e, "InstallLocation"));
                std::wstring p = join(loc, L"FortniteGame\\Content\\Paks");
                if (!loc.empty() && dirExists(p)) return p;
            }
    }
    std::wstring def = L"C:\\Program Files\\Epic Games\\Fortnite\\FortniteGame\\Content\\Paks";
    return dirExists(def) ? def : L"";
}

uint64_t newestUtoc(const std::wstring& paks) {
    uint64_t best = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(join(paks, L"*.utoc").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do best = std::max(best, ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

std::wstring lastLine(const std::wstring& output) {
    size_t end = output.size();
    while (end > 0) {
        size_t start = output.rfind(L'\n', end - 1);
        start = start == std::wstring::npos ? 0 : start + 1;
        std::wstring l = trim(output.substr(start, end - start));
        if (!l.empty()) return l;
        if (start == 0) break;
        end = start - 1;
    }
    return L"no output";
}

// RunAsync of Port.cs: hidden, BelowNormal, the fn-port runtime and its 3 GB heap cap in the environment.
int runTool(const std::wstring& exe, const std::wstring& args, const std::function<void(const std::wstring&)>& onLine, std::wstring* all,
            std::atomic<DWORD>* pid = nullptr) {
    proc::Start s;
    s.exe = exe;
    s.args = args;
    s.env = { { L"DOTNET_ROOT", dotnetRoot() }, { L"DOTNET_GCHeapHardLimit", L"0xC0000000" } };
    s.priority = BELOW_NORMAL_PRIORITY_CLASS;
    if (pid) s.started = [pid](DWORD p) { *pid = p; };
    std::mutex m;
    std::wstring err, startErr;
    int code = proc::run(s, [&](const std::wstring& line, bool isErr) {
        std::lock_guard<std::mutex> g(m);
        if (isErr) { err += line; err += L"\n"; return; }
        *all += line; *all += L"\n";
        if (onLine) onLine(line);
    }, &startErr);
    *all += err;
    if (code == -1 && !startErr.empty()) *all += startErr + L"\n";
    return code;
}

// ---------------------------------------------------------------------------------------------- names
struct Name { std::string id, type, name, rarity, series, icon, featured, set, added; };

std::string jstr(const std::string& s) { return s.empty() ? "null" : json::strict(widen(s)); }

// id (lower case) -> name, rarity, icon, set, date added, from fortnite-api.com's BR list (names.json, a day).
std::unordered_map<std::string, Name> names(bool refresh, const LogFn& log) {
    std::wstring file = namesFile();
    bool stale = true;
    {
        std::string text;
        if (readFile(file, &text)) stale = text.find("\"type\":") == std::string::npos;
    }
    if (refresh || stale || mtime(file) + kDay < nowFt()) {
        std::wstring tmp = join(work(), L"names-api.tmp"), error;
        if (!net::fetchFile(L"https://fortnite-api.com/v2/cosmetics/br", kUa, 60000, tmp, &error)) {
            log(L"Warning: no cosmetic names and icons (fortnite-api.com: " + error + L"); the ids are shown instead.");
        } else {
            Doc d;
            d.read(tmp);
            yyjson_val* data = key(d.root(), "data");
            std::string o = "{";
            size_t i, n;
            yyjson_val* x;
            if (data && yyjson_is_arr(data))
                yyjson_arr_foreach(data, i, n, x) {
                    std::string id = str(key(x, "id"));
                    if (id.empty()) continue;
                    yyjson_val* im = key(x, "images");
                    std::string icon = str(key(im, "icon"));
                    if (icon.empty()) icon = str(key(im, "smallIcon"));
                    if (o.size() > 1) o += ",";
                    o += json::strict(widen(lowerA(id))) + ":{\"id\":" + jstr(id) + ",\"type\":" + jstr(str(key(key(x, "type"), "value"))) +
                         ",\"name\":" + jstr(str(key(x, "name"))) + ",\"rarity\":" + jstr(str(key(key(x, "rarity"), "value"))) +
                         ",\"series\":" + jstr(str(key(key(x, "series"), "value"))) + ",\"icon\":" + jstr(icon) +
                         ",\"featured\":" + jstr(str(key(im, "featured"))) + ",\"set\":" + jstr(str(key(key(x, "set"), "value"))) +
                         ",\"added\":" + jstr(str(key(x, "added"))) + "}";
                }
            o += "}";
            if (data && yyjson_is_arr(data)) writeFile(file, o);
            else log(L"Warning: no cosmetic names and icons (fortnite-api.com sent no list); the ids are shown instead.");
        }
        DeleteFileW(tmp.c_str());
    }
    std::unordered_map<std::string, Name> map;
    Doc d;
    if (d.read(file) && yyjson_is_obj(d.root())) {
        size_t i, n;
        yyjson_val *k, *v;
        map.reserve(yyjson_obj_size(d.root()));
        yyjson_obj_foreach(d.root(), i, n, k, v) {
            Name x{ str(key(v, "id")), str(key(v, "type")), str(key(v, "name")), str(key(v, "rarity")), str(key(v, "series")),
                    str(key(v, "icon")), str(key(v, "featured")), str(key(v, "set")), str(key(v, "added")) };
            map.emplace(str(k), std::move(x));
        }
    }
    return map;
}

// Source paths already imported into the build (custom-cosmetics.json entries with source "fn-port:<path>"), upper case.
std::unordered_set<std::wstring> imported(const std::wstring& buildDir) {
    std::unordered_set<std::wstring> set;
    Doc d;
    if (!d.read(join(buildDir, L"offline\\custom-cosmetics.json"))) return set;
    yyjson_val* items = key(d.root(), "items");
    size_t i, n;
    yyjson_val* it;
    if (items && yyjson_is_arr(items))
        yyjson_arr_foreach(items, i, n, it) {
            std::wstring s = json::wstr(key(it, "source"));
            if (starts(s, L"fn-port:")) set.insert(upper(s.substr(8)));
        }
    return set;
}

// Lower-case ids of the build's own cosmetics (offline\cosmetics.json): the same item already exists there.
std::unordered_set<std::string> buildIds(const std::wstring& buildDir) {
    std::unordered_set<std::string> set;
    Doc d;
    if (!d.read(join(buildDir, L"offline\\cosmetics.json"))) return set;
    yyjson_val* items = key(d.root(), "items");
    size_t i, n;
    yyjson_val* it;
    if (items && yyjson_is_arr(items))
        yyjson_arr_foreach(items, i, n, it) {
            std::string t = str(key(it, "templateId"));
            if (t.empty()) continue;
            size_t c = t.find(':');
            set.insert(lowerA(c == std::string::npos ? t : t.substr(c + 1)));
        }
    return set;
}

// port.ps1's state for the build: items it found already in the build or could not convert (upper-case path -> status).
std::unordered_map<std::wstring, PortStatus> failedItems(const std::wstring& buildDir) {
    std::unordered_map<std::wstring, PortStatus> map;
    Doc d;
    if (!d.read(join(work(), L"state-" + fileName(buildDir) + L".json")) || !yyjson_is_obj(d.root())) return map;
    size_t i, n;
    yyjson_val *k, *v;
    yyjson_obj_foreach(d.root(), i, n, k, v) {
        std::string st = str(key(v, "status"));
        if (st == "exists") map[upper(json::wstr(k))] = PortStatus::Build;
        else if (st == "failed" || st == "crashed" || st == "unsupported") map[upper(json::wstr(k))] = PortStatus::Failed;
    }
    return map;
}

void fill(PortItem& e, const Name* n) {
    if (!n) return;
    e.name = widen(n->name); e.set = widen(n->set); e.series = widen(n->series);
    e.rarity = lowerA(n->rarity); e.icon = n->icon; e.featured = n->featured; e.added = n->added;
}

// ---------------------------------------------------------------------------------------------- import helpers
// Whole-machine processor use (GetSystemTimes), averaged over about 2 s (Port.cs CpuMeter).
struct CpuMeter {
    struct S { uint64_t idle, total; };
    std::vector<S> q;
    int sample() {
        FILETIME i, k, u;
        if (!GetSystemTimes(&i, &k, &u)) return -1;
        auto v = [](FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; };
        q.push_back({ v(i), v(k) + v(u) });   // kernel time includes idle time
        if (q.size() > 5) q.erase(q.begin());
        if (q.size() < 2) return -1;
        uint64_t dt = q.back().total - q.front().total, di = q.back().idle - q.front().idle;
        return dt == 0 ? -1 : (int)lround(100.0 * (1 - (double)di / (double)dt));
    }
};

std::wstring after(const std::wstring& path) { size_t d = path.rfind(L'.'); return d == std::wstring::npos ? path : path.substr(d + 1); }

} // namespace

// ================================================================================================ public
bool supports(const BuildInfo& b) { return !b.usesIoStore() && b.version == L"14.30"; }

std::wstring work() {
    std::wstring w = join(paths::launcherDir(), L"port");
    createDirs(w);
    return w;
}

int importedCount(const std::wstring& buildDir) { return (int)imported(buildDir).size(); }

PortType typeOfClass(const std::string& cls, bool* ok) {
    for (auto& t : kTypes) if (cls == t.cls) { *ok = true; return t.t; }
    *ok = false;
    return PortType::Outfit;
}
PortType typeOfName(const std::string& n, bool* ok) {
    for (auto& t : kTypes) if (n == t.name) { *ok = true; return t.t; }
    *ok = false;
    return PortType::Outfit;
}

Line classify(const std::wstring& raw) {
    Line l;
    std::wstring s = trim(raw);
    auto word = [&](const wchar_t* w) { size_t n = wcslen(w); return s.size() > n && !s.compare(0, n, w) && s[n] == L' '; };
    if (word(L"OK") || word(L"FAILED")) {
        size_t a = s.find(L' ') + 1, b = a;
        while (b < s.size() && !iswspace(s[b])) b++;
        if (b > a) {
            l.kind = s[0] == L'O' ? Line::Ok : Line::Failed;
            l.path = s.substr(a, b - a);
            while (!l.path.empty() && l.path.back() == L':') l.path.pop_back();
            return l;
        }
    }
    if (starts(s, L"batch ")) { l.kind = Line::Batch; return l; }
    if (starts(s, L"done:")) { l.kind = Line::Done; return l; }
    if (starts(s, L"cpu:")) { l.kind = Line::Cpu; return l; }
    if (starts(s, L"localization")) { l.kind = Line::Localization; return l; }
    // "items: N listed, M to port"
    if (starts(s, L"items: ")) {
        size_t i = 7, j = i;
        while (j < s.size() && iswdigit(s[j])) j++;
        if (j > i && !s.compare(j, 9, L" listed, ")) {
            size_t k = j + 9, e = k;
            while (e < s.size() && iswdigit(s[e])) e++;
            if (e > k && !s.compare(e, 8, L" to port")) { l.kind = Line::Items; l.toPort = _wtoi(s.substr(k, e - k).c_str()); return l; }
        }
    }
    return l;
}

std::shared_ptr<PortCatalog> catalog(const BuildInfo& b, bool refresh, const LogFn& log, const std::function<void(const wchar_t*, int64_t, bool)>& stage,
                                     const std::function<void(std::shared_ptr<PortCatalog>)>& early) {
    auto cat = std::make_shared<PortCatalog>();
    cat->build = b.name();
    Tool tool;
    std::wstring why;
    if (!findTool(&tool, &why)) { cat->error = why; return cat; }
    std::wstring paks = currentGamePaks();
    if (paks.empty()) { cat->error = L"Fortnite is not installed on this PC (the current game is where the cosmetics come from)"; return cat; }
    std::wstring items = itemsFile();
    bool needList = refresh || !fileExists(items) || mtime(items) < newestUtoc(paks);
    std::wstring listErr;
    std::atomic<bool> listDone{ !needList };
    std::thread lister;
    if (needList) {
        // how long the last listing took (the first one, with no fn-port cache, takes minutes)
        int64_t est = 60000;
        std::string t;
        if (readFile(listMsFile(), &t) && atoll(t.c_str()) > 0) est = atoll(t.c_str());
        stage(L"list", est, false);
        log(L"Listing the cosmetics of the installed Fortnite (fn-port list)...");
        lister = std::thread([&] {
            ULONGLONG t0 = GetTickCount64();
            std::wstring tmp = items + L".tmp", all;
            DeleteFileW(tmp.c_str());
            int code = runTool(tool.dotnet, L"\"" + tool.dll + L"\" list --out \"" + tmp + L"\" --cache \"" + join(work(), L"cache") + L"\" --input \"" + paks + L"\"", nullptr, &all);
            if (code != 0 || !fileExists(tmp)) listErr = fmt(L"fn-port list failed (exit %d): ", code) + lastLine(all);
            else {
                MoveFileExW(tmp.c_str(), items.c_str(), MOVEFILE_REPLACE_EXISTING);
                writeFile(listMsFile(), std::to_string(GetTickCount64() - t0));
            }
            listDone = true;
        });
    }
    stage(L"names", 4000, false);
    auto nm = names(refresh, log);
    stage(L"names", 0, true);
    auto have = imported(b.dir);
    auto inBuild = buildIds(b.dir);
    auto failed = failedItems(b.dir);
    if (!listDone && early) {
        std::unordered_set<std::string> haveIds;
        for (auto& p : have) haveIds.insert(lowerA(narrow(after(p))));
        auto pre = std::make_shared<PortCatalog>();
        pre->build = b.name();
        pre->partial = true;
        pre->items.reserve(nm.size());
        for (auto& kv : nm) {
            bool ok;
            PortType t = typeOfName(kv.second.type, &ok);
            if (!ok) continue;
            PortItem e;
            e.id = widen(kv.second.id.empty() ? kv.first : kv.second.id);
            e.type = t;
            e.status = haveIds.count(kv.first) ? PortStatus::Imported : inBuild.count(kv.first) ? PortStatus::Build : PortStatus::New;
            fill(e, &kv.second);
            pre->items.push_back(std::move(e));
        }
        early(pre);
    }
    if (lister.joinable()) lister.join();
    if (!listErr.empty()) { cat->error = listErr; return cat; }
    if (needList) stage(L"list", 0, true);
    Doc d;
    if (!d.read(items) || !yyjson_is_arr(d.root())) { cat->error = L"could not read " + items; return cat; }
    size_t i, n;
    yyjson_val* it;
    cat->items.reserve(yyjson_arr_size(d.root()));
    yyjson_arr_foreach(d.root(), i, n, it) {
        bool ok;
        PortType t = typeOfClass(str(key(it, "class")), &ok);
        if (!ok) continue;
        PortItem e;
        e.path = json::wstr(key(it, "path"));
        if (e.path.empty()) continue;
        std::wstring id = json::wstr(key(it, "id"));
        e.id = id.empty() ? after(e.path) : id;
        std::string lid = lowerA(narrow(e.id));
        auto f = nm.find(lid);
        std::wstring up = upper(e.path);
        auto fl = failed.find(up);
        e.type = t;
        e.status = have.count(up) ? PortStatus::Imported : inBuild.count(lid) ? PortStatus::Build : fl != failed.end() ? fl->second : PortStatus::New;
        fill(e, f == nm.end() ? nullptr : &f->second);
        cat->items.push_back(std::move(e));
    }
    cat->slotsFree = std::max(0, kChunkSlots - (int)have.size());
    return cat;
}

// ================================================================================================ the job
void Job::setPhase(const wchar_t* p) { if (phase_ != p) { phase_ = p; phaseSince_ = GetTickCount64(); } }
void Job::note(const std::wstring& line) {
    std::lock_guard<std::mutex> g(m_);
    recent_.push_back({ nowTime(L"HH:mm:ss"), line });
    if (recent_.size() > 60) recent_.erase(recent_.begin(), recent_.begin() + (recent_.size() - 60));
}
Job::State* Job::find(const std::wstring& path) {
    for (auto& s : items_) if (ieq(s.path, path)) return &s;
    return nullptr;
}
// Marks an item converted (or not); counted once, whether fn-port's or port.ps1's line comes first.
void Job::result(const std::wstring& path, bool ok) {
    std::lock_guard<std::mutex> g(m_);
    State* s = find(path);
    if (s && (s->st == L"ok" || s->st == L"failed")) return;
    if (!s) { items_.push_back({ path, L"" }); s = &items_.back(); }
    s->st = ok ? L"ok" : L"failed";
    done_++;
    if (ok) ok_++; else failed_++;
    if (ieq(current_, path)) current_.clear();
}
// The next queued item of the batch becomes the one being converted.
void Job::next() {
    std::lock_guard<std::mutex> g(m_);
    for (auto& s : items_)
        if (s.st == L"queued" || s.st == L"converting") {
            if (!ieq(current_, s.path)) { current_ = s.path; currentSince_ = GetTickCount64(); s.st = L"converting"; }
            return;
        }
    current_.clear();
}
// After a batch's OK/FAILED lines port.ps1 copies its paks and rewrites custom-cosmetics.json: a stop then waits for
// the batch's "batch N:" line, so no pak is left installed without its entry. A batch still being converted is
// dropped (port.ps1 picks it up again from its state file next time).
void Job::setInstalling(bool on) { installing_ = on; if (!on && cancelled) kill(); }
void Job::cancel() { cancelled = true; if (!installing_) kill(); }
void Job::kill() {
    DWORD p = pid_;
    if (p && !exited_) proc::killTree(p, proc::list());
}
void Job::cancelAndWait(DWORD ms) {
    cancel();
    ULONGLONG t0 = GetTickCount64();
    while (!exited_ && GetTickCount64() - t0 < ms) Sleep(200);
    kill();
}
int Job::okCount() { std::lock_guard<std::mutex> g(m_); return ok_; }
int Job::failedCount() { std::lock_guard<std::mutex> g(m_); return failed_; }

std::shared_ptr<PortJob> Job::view() {
    std::lock_guard<std::mutex> g(m_);
    auto v = std::make_shared<PortJob>();
    ULONGLONG now = GetTickCount64();
    int left = std::max(0, total_ - done_);
    // seconds per item: this import's own pace once something finished, else the last import's
    double rate = ok_ > 0 ? (now - started_) / 1000.0 / done_ : secPerItem_;
    v->build = build; v->total = total_; v->done = done_; v->ok = ok_; v->failed = failed_; v->line = line_; v->phase = phase_;
    v->batch = batch_; v->cpu = cpu_; v->batches = (total_ + kBatch - 1) / kBatch;
    v->elapsedMs = (int64_t)(now - started_); v->phaseMs = (int64_t)(now - phaseSince_);
    v->current = current_; v->currentMs = (int64_t)(now - currentSince_);
    v->etaMs = rate > 0 && left > 0 ? (int64_t)(left * rate * 1000) : 0;
    std::vector<const State*> shown;
    for (auto& s : items_) if (s.st != L"pending") shown.push_back(&s);
    for (size_t i = shown.size() > 40 ? shown.size() - 40 : 0; i < shown.size(); i++) v->items.push_back({ shown[i]->path, shown[i]->st });
    for (size_t i = recent_.size() > 14 ? recent_.size() - 14 : 0; i < recent_.size(); i++) v->recent.push_back(recent_[i]);
    return v;
}

// port.ps1 only prints a batch's results once its fn-port process ends; meanwhile fn-port writes to
// <work>\batch\stdout.txt: reading it live gives the phase (reading the current game until its "localization" line,
// then converting item after item in the order of batch\items.json) and each result as it happens.
void tailBatch(const std::shared_ptr<Job>& job, const std::function<void()>& progress, std::atomic<bool>* stop) {
    std::wstring dir = join(work(), L"batch");
    std::wstring batchKey, partial, lastErr;
    int64_t pos = 0;
    CpuMeter cpu;
    uint64_t started = ((uint64_t)job->startedFt_.dwHighDateTime << 32) | job->startedFt_.dwLowDateTime;
    while (!*stop) {
        for (int i = 0; i < 10 && !*stop; i++) Sleep(50);
        if (*stop) break;
        // port.ps1 waits silently while the machine is above 70 %: the window shows the reading
        int c = cpu.sample();
        if (c >= 0) {
            std::lock_guard<std::mutex> g(job->m_);
            job->cpu_ = c;
            if (job->phase_ == L"wait" && c > 70 && GetTickCount64() - job->phaseSince_ > 5000) job->setPhase(L"cpu");
        }
        std::wstring itemsJson = join(dir, L"items.json");
        uint64_t mt = mtime(itemsJson);
        if (mt && mt >= started) {
            Doc d;
            if (!d.read(itemsJson) || !yyjson_is_arr(d.root())) {
                std::wstring e = L"(reading the batch: " + itemsJson + L" is being replaced)";
                if (e != lastErr) job->note(lastErr = e);
            } else {
                std::vector<std::wstring> batch;
                std::wstring k;
                size_t i, n;
                yyjson_val* it;
                yyjson_arr_foreach(d.root(), i, n, it) { batch.push_back(json::wstr(key(it, "path"))); k += batch.back() + L"|"; }
                if (k != batchKey) {   // a new batch: its items are queued, fn-port starts by reading the current game
                    batchKey = k; pos = 0; partial.clear();
                    int bn;
                    {
                        std::lock_guard<std::mutex> g(job->m_);
                        bn = ++job->batch_;
                        for (auto& p : batch) {
                            Job::State* s = job->find(p);
                            if (!s) job->items_.push_back({ p, L"queued" });
                            else if (s->st == L"pending" || s->st.empty()) s->st = L"queued";
                        }
                        job->setPhase(L"load");
                    }
                    job->note(fmt(L"batch %d: fn-port is reading the current Fortnite (%d items)", bn, (int)batch.size()));
                }
                Handle f(CreateFileW(join(dir, L"stdout.txt").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
                LARGE_INTEGER sz{};
                if (f.ok() && GetFileSizeEx(f, &sz)) {
                    if (sz.QuadPart < pos) pos = 0;
                    if (sz.QuadPart > pos) {
                        std::string data((size_t)(sz.QuadPart - pos), '\0');
                        LARGE_INTEGER at; at.QuadPart = pos;
                        DWORD got = 0;
                        SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
                        ReadFile(f, &data[0], (DWORD)data.size(), &got, nullptr);
                        data.resize(got);
                        pos += got;
                        std::wstring text = partial + widen(data);
                        size_t start = 0, nl;
                        while ((nl = text.find(L'\n', start)) != std::wstring::npos) {
                            std::wstring line = trim(text.substr(start, nl - start));
                            start = nl + 1;
                            if (line.empty()) continue;
                            Line l = classify(line);
                            if (l.kind == Line::Localization) { { std::lock_guard<std::mutex> g(job->m_); job->setPhase(L"convert"); } job->next(); continue; }
                            if (l.kind == Line::Ok || l.kind == Line::Failed) {
                                job->result(l.path, l.kind == Line::Ok);
                                job->note(l.kind == Line::Ok ? L"converted " + after(l.path) : line);
                                job->next();
                                std::lock_guard<std::mutex> g(job->m_);
                                if (job->current_.empty()) job->setPhase(L"install");
                            } else job->note(line);
                        }
                        partial = text.substr(start);
                    }
                }
            }
        }
        progress();   // every half second: the window's view of the import never lags behind
    }
}

bool importItems(const BuildInfo& b, const std::vector<std::wstring>& pathsIn, const std::shared_ptr<Job>& job, const LogFn& log,
                 const std::function<void()>& progress) {
    Tool tool;
    std::wstring why;
    if (!findTool(&tool, &why)) { log(L"Could not import: " + why + L"."); job->exited_ = true; return false; }
    std::unordered_map<std::wstring, std::string> classes;
    {
        Doc d;
        size_t i, n;
        yyjson_val* it;
        if (d.read(itemsFile()) && yyjson_is_arr(d.root()))
            yyjson_arr_foreach(d.root(), i, n, it) classes[upper(json::wstr(key(it, "path")))] = str(key(it, "class"));
    }
    std::vector<std::wstring> list;
    {
        std::set<std::wstring> seen;
        for (auto& p : pathsIn) if (seen.insert(p).second) list.push_back(p);
    }
    std::string arr = "[";
    for (auto& p : list) {
        auto c = classes.find(upper(p));
        if (arr.size() > 1) arr += ",";
        arr += "{\"path\":" + json::strict(p) + ",\"class\":" + json::strict(c == classes.end() ? L"" : widen(c->second)) + "}";
    }
    arr += "]";
    std::wstring file = join(work(), L"import-" + nowTime(L"yyyyMMdd-HHmmss") + L".json");
    std::wstring logFile = file.substr(0, file.size() - 5) + L".log";   // port.ps1's whole output, for when something goes wrong
    writeFile(file, arr);
    {
        std::lock_guard<std::mutex> g(job->m_);
        job->build = b.version;
        job->total_ = (int)list.size();
        for (auto& p : list) job->items_.push_back({ p, L"pending" });
        std::string sp;
        if (readFile(speedFile(), &sp) && atof(sp.c_str()) > 0) job->secPerItem_ = atof(sp.c_str());
        job->started_ = job->phaseSince_ = job->currentSince_ = GetTickCount64();
        GetSystemTimeAsFileTime(&job->startedFt_);
    }
    progress();   // the item count at once, not with the first half-second report
    log(fmt(L"Importing %d cosmetics of the current Fortnite into ", (int)list.size()) + b.version +
        L" (fn-port; the build's files are backed up in offline\\backup-port-*)...");
    std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -File \"" + tool.script + L"\" -Build \"" + b.dir + L"\" -Items \"" + file +
                        L"\" -Work \"" + work() + L"\" -Dll \"" + tool.dll + fmt(L"\" -Batch %d", kBatch);
    std::atomic<bool> stop{ false };
    std::thread tail([&] { tailBatch(job, progress, &stop); });
    wchar_t sys[MAX_PATH];
    UINT sl = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring all;
    int code = runTool(join(std::wstring(sys, sl), L"WindowsPowerShell\\v1.0\\powershell.exe"), args, [&](const std::wstring& raw) {
        appendFile(logFile, narrow(raw) + "\r\n");
        std::wstring line = trim(raw);
        if (line.empty()) return;
        job->note(line);
        Line l = classify(line);
        switch (l.kind) {
        case Line::Ok: case Line::Failed:
            job->result(l.path, l.kind == Line::Ok);
            { std::lock_guard<std::mutex> g(job->m_); job->setPhase(L"install"); }
            job->setInstalling(true);
            break;
        case Line::Batch: {
            job->setInstalling(false);
            std::lock_guard<std::mutex> g(job->m_);
            job->setPhase(job->done_ < job->total_ ? L"wait" : L"install");
            break;
        }
        case Line::Done: { job->setInstalling(false); std::lock_guard<std::mutex> g(job->m_); job->setPhase(L"sign"); break; }
        case Line::Cpu: { std::lock_guard<std::mutex> g(job->m_); job->setPhase(L"cpu"); break; }
        case Line::Items: { std::lock_guard<std::mutex> g(job->m_); job->total_ = l.toPort; job->setPhase(L"wait"); break; }
        default: break;
        }
        { std::lock_guard<std::mutex> g(job->m_); job->line_ = line; }
        if (!starts(line, L"OK ")) log(L"Import: " + line);
        progress();
    }, &all, &job->pid_);
    job->exited_ = true;
    stop = true;
    tail.join();
    int ok = job->okCount(), failed = job->failedCount();
    {
        std::lock_guard<std::mutex> g(job->m_);
        if (ok > 0) writeFile(speedFile(), narrow(fmt(L"%.1f", (GetTickCount64() - job->started_) / 1000.0 / std::max(1, job->done_))));
    }
    if (job->cancelled) { log(fmt(L"Import stopped: %d imported. Import again to continue where it stopped.", ok)); return false; }
    appendFile(logFile, narrow(fmt(L"exit %d\r\n", code) + all));
    if (code != 0 && ok == 0) { log(fmt(L"Could not import (exit %d): ", code) + lastLine(all) + L" (log: " + logFile + L")."); return false; }
    log(fmt(L"Import finished: %d imported, %d not converted (details: ", ok, failed) + join(work(), L"report-" + fileName(b.dir) + L".json") + L").");
    return true;
}

// ================================================================================================ previews
bool Previews::parse(std::string& text, const std::vector<std::wstring>& keepLower) {
    std::vector<std::string> keep;
    keep.reserve(keepLower.size());
    for (auto& k : keepLower) keep.push_back(narrow(k));
    std::sort(keep.begin(), keep.end());
    size_t n0 = text.size();
    text.append(YYJSON_PADDING_SIZE, '\0');
    yyjson_doc* doc = yyjson_read_opts(&text[0], n0, YYJSON_READ_INSITU | YYJSON_READ_ALLOW_BOM | YYJSON_READ_ALLOW_INVALID_UNICODE, nullptr, nullptr);
    if (!doc) return false;
    std::vector<P> out;
    yyjson_val* root = yyjson_doc_get_root(doc);
    std::string date = str(key(root, "date"));
    int today = sheet::dayOf(date.c_str(), date.size());
    yyjson_val* items = key(root, "items");
    size_t i, n;
    yyjson_val* it;
    // the numeric fortnite.gg id of "https://fnggcdn.com/items/<n>/video-sd.mp4" or "https://fortnite.gg/cosmetics?id=<n>"
    auto num = [](const std::string& s, const char* prefix, const char* suffix) -> uint32_t {
        size_t pl = strlen(prefix), sl = strlen(suffix);
        if (s.size() <= pl + sl || s.compare(0, pl, prefix) || s.compare(s.size() - sl, sl, suffix)) return 0;
        uint32_t v = 0;
        for (size_t k = pl; k < s.size() - sl; k++) { if (s[k] < '0' || s[k] > '9') return 0; v = v * 10 + (uint32_t)(s[k] - '0'); }
        return v;
    };
    auto day = [](yyjson_val* v) -> uint16_t { std::string d = str(v); int x = sheet::dayOf(d.c_str(), d.size()); return x > 0 && x < 65536 ? (uint16_t)x : 0; };
    auto whole = [](yyjson_val* v, int max) -> int { if (!v || !yyjson_is_num(v)) return -1; double d = yyjson_get_num(v); return d < 0 ? -1 : (int)std::min<double>(max, floor(d + .5)); };
    auto perMille = [](yyjson_val* v) -> int16_t { if (!v || !yyjson_is_num(v)) return -1; double d = yyjson_get_num(v); return (int16_t)std::max(0, std::min(1000, (int)floor(d * 1000 + .5))); };
    if (items && yyjson_is_arr(items))
        yyjson_arr_foreach(items, i, n, it) {
            std::string id = lowerA(str(key(it, "i")));
            if (id.empty() || !std::binary_search(keep.begin(), keep.end(), id)) continue;
            P p;
            p.idLower = id;
            p.desc = str(key(it, "ds"));
            p.intro = str(key(it, "sz"));
            p.set = str(key(it, "st"));
            int gg = whole(key(it, "gg"), INT32_MAX);
            if (gg > 0) {   // catalog.min.json ("v":2): the fortnite.gg id, vv = it has the two clips
                p.fngg = (uint32_t)gg;
                p.flags |= 4;
                yyjson_val* vv = key(it, "vv");
                if (vv && (yyjson_is_true(vv) || (yyjson_is_num(vv) && yyjson_get_num(vv) != 0))) p.flags |= 3;
            } else {
                uint32_t v = num(str(key(it, "v")), "https://fnggcdn.com/items/", "/video-sd.mp4");
                uint32_t vh = num(str(key(it, "vh")), "https://fnggcdn.com/items/", "/video.mp4");
                uint32_t g = num(str(key(it, "g")), "https://fortnite.gg/cosmetics?id=", "");
                p.fngg = g ? g : v ? v : vh;
                if (v && v == p.fngg) p.flags |= 1;
                if (vh && vh == p.fngg) p.flags |= 2;
                if (g) p.flags |= 4;
            }
            yyjson_val* away = key(it, "away");
            if (yyjson_is_true(key(it, "shop")) || (away && yyjson_is_num(away) && yyjson_get_num(away) == 0)) p.flags |= 8;
            p.first = day(key(it, "first"));
            p.last = day(key(it, "last"));
            p.occ = (uint16_t)std::max(0, whole(key(it, "occ"), 65535));
            yyjson_val* lp = key(it, "lp");
            p.price = (uint16_t)std::max(0, whole(key(lp, "p"), 65535));
            p.regular = (uint16_t)std::max(0, whole(key(lp, "r"), 65535));
            if (key(it, "p7") && yyjson_is_num(key(it, "p7"))) { p.p7 = perMille(key(it, "p7")); p.p30 = perMille(key(it, "p30")); p.p60 = perMille(key(it, "p60")); }
            out.push_back(std::move(p));
        }
    yyjson_doc_free(doc);
    std::sort(out.begin(), out.end(), [](const P& a, const P& b) { return a.idLower < b.idLower; });
    out.shrink_to_fit();
    std::lock_guard<std::mutex> g(m_);
    items_.swap(out);
    stays_.clear();
    today_ = today;
    loaded_ = true;
    return true;
}

// history.json = {"<id lower-case>": [[[start, end], ...newest first], [last starts]]}: the stays of the kept ids
bool Previews::parseHistory(std::string& text) {
    size_t n0 = text.size();
    text.append(YYJSON_PADDING_SIZE, '\0');
    yyjson_doc* doc = yyjson_read_opts(&text[0], n0, YYJSON_READ_INSITU | YYJSON_READ_ALLOW_BOM | YYJSON_READ_ALLOW_INVALID_UNICODE, nullptr, nullptr);
    if (!doc) return false;
    std::lock_guard<std::mutex> g(m_);
    stays_.clear();
    for (auto& p : items_) p.stay0 = p.stays = 0;
    size_t oi, on;
    yyjson_val *kv, *v;
    yyjson_obj_foreach(yyjson_doc_get_root(doc), oi, on, kv, v) {   // one pass over the file, the kept ids found by binary search
        std::string k(yyjson_get_str(kv), yyjson_get_len(kv));
        auto p = std::lower_bound(items_.begin(), items_.end(), k, [](const P& a, const std::string& x) { return a.idLower < x; });
        if (p == items_.end() || p->idLower != k) continue;
        yyjson_val* sh = yyjson_is_arr(v) ? yyjson_arr_get(v, 0) : nullptr;
        if (!sh || !yyjson_is_arr(sh)) continue;
        p->stay0 = (uint32_t)stays_.size();
        size_t i, n;
        yyjson_val* x;
        yyjson_arr_foreach(sh, i, n, x) {
            std::string a = str(yyjson_arr_get(x, 0)), b = str(yyjson_arr_get(x, 1));
            int da = sheet::dayOf(a.c_str(), a.size()), db = sheet::dayOf(b.c_str(), b.size());
            if (da <= 0 || da > 65535) continue;
            stays_.push_back({ (uint16_t)da, (uint16_t)(db > 0 && db < 65536 ? db : da) });
        }
        p->stays = (uint32_t)stays_.size() - p->stay0;
    }
    stays_.shrink_to_fit();
    yyjson_doc_free(doc);
    return true;
}

bool Previews::load(const std::vector<std::wstring>& keepLower, const LogFn& log) {
    std::wstring file = join(work(), L"radarnite-catalog.json"), hist = join(work(), L"radarnite-history.json");
    if (mtime(file) + kDay < nowFt() || mtime(hist) + kDay < nowFt()) {
        std::wstring error, e2;
        // the site's own files: catalog.min.json (the standard URLs left out), catalog.json on an older server
        if (!net::fetchFile(L"https://radarnite.com/catalog.min.json", kUa, 60000, file, &error) &&
            !net::fetchFile(L"https://radarnite.com/catalog.json", kUa, 60000, file, &e2))
            log(L"Warning: no previews of the cosmetics (radarnite.com: " + error + (fileExists(file) ? L"); the copy from before is used." : L")."));
        else if (!net::fetchFile(L"https://radarnite.com/history.json", kUa, 60000, hist, &error))
            log(L"Warning: no shop history of the cosmetics (radarnite.com: " + error + L").");
    }
    std::string text;
    if (!readFile(file, &text)) return false;
    bool ok = parse(text, keepLower);
    std::string().swap(text);
    if (ok && readFile(hist, &text)) parseHistory(text);
    return ok;
}

PortPreview Previews::get(const std::wstring& id) {
    PortPreview r;
    r.id = id;
    std::string k = lowerA(narrow(id));
    std::lock_guard<std::mutex> g(m_);
    r.today = today_;
    auto it = std::lower_bound(items_.begin(), items_.end(), k, [](const P& p, const std::string& x) { return p.idLower < x; });
    if (it == items_.end() || it->idLower != k) return r;
    r.found = true;
    r.description = widen(it->desc);
    r.introduced = widen(it->intro);
    r.set = widen(it->set);
    char b[96];
    if (it->flags & 1) { snprintf(b, sizeof b, "https://fnggcdn.com/items/%u/video-sd.mp4", it->fngg); r.video = b; }
    if (it->flags & 2) { snprintf(b, sizeof b, "https://fnggcdn.com/items/%u/video.mp4", it->fngg); r.videoHd = b; }
    if (it->flags & 4) { snprintf(b, sizeof b, "https://fortnite.gg/cosmetics?id=%u", it->fngg); r.page = b; }
    r.inShop = (it->flags & 8) != 0;
    r.first = it->first; r.last = it->last; r.occ = it->occ;
    r.price = it->price; r.regular = it->regular;
    r.p7 = it->p7; r.p30 = it->p30; r.p60 = it->p60;
    for (uint32_t s = 0; s < it->stays; s++) r.stays.push_back({ stays_[it->stay0 + s].first, stays_[it->stay0 + s].second });
    return r;
}

bool Previews::loaded() { std::lock_guard<std::mutex> g(m_); return loaded_; }
size_t Previews::size() { std::lock_guard<std::mutex> g(m_); return items_.size(); }

} // namespace port
} // namespace rn
