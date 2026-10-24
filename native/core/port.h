// Cosmetics of the current game imported into an offline classic-pak build as new items (launcher/Port.cs Porter).
// The conversion is FortnitePorting's fn-port: `fn-port list` gives every cosmetic of the installed game, its
// port.ps1 converts and installs them in batches (own encrypted pak per item + offline\custom-cosmetics.json, which
// only port.ps1 writes). This module finds those tools, lists, runs port.ps1 for the picked items and follows it.
// Same files as 2.x in <launcher>\port (items.json, names.json, list-ms.txt, speed.txt, state-<build>.json,
// import-*.json/.log, batch\items.json + batch\stdout.txt), so 2.x and 3.0 share them.
#pragma once
#include "../ui/api.h"
#include "buildinfo.h"
#include "util.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rn {
namespace port {

bool supports(const BuildInfo& b);                  // classic paks and a build fn-port has donors for (14.30)
std::wstring work();                                // <launcher>\port (created)
int importedCount(const std::wstring& buildDir);    // custom-cosmetics.json entries with an "fn-port:" source
PortType typeOfClass(const std::string& cls, bool* ok);   // AthenaCharacterItemDefinition -> Outfit ...
PortType typeOfName(const std::string& t, bool* ok);      // fortnite-api's type value: "outfit", "backpack" ...

// One output line of port.ps1 or fn-port, classified (the import's progress). Pure: the batch-status test runs it.
struct Line {
    enum Kind { Other, Ok, Failed, Batch, Done, Cpu, Items, Localization } kind = Other;
    std::wstring path;   // Ok / Failed: the source path (trailing ':' dropped)
    int toPort = 0;      // Items: "items: N listed, M to port" -> M
};
Line classify(const std::wstring& line);

// Porter.CatalogAsync: every cosmetic of the current game (fn-port list, cached until the game updates) with names and
// icons (fortnite-api.com, a day) and its state in the build. stage(name, estimated ms, done); early gets
// fortnite-api's list (no paths) while fn-port still reads the game. Errors come back in the catalog.
std::shared_ptr<PortCatalog> catalog(const BuildInfo& b, bool refresh, const LogFn& log,
                                     const std::function<void(const wchar_t*, int64_t, bool)>& stage,
                                     const std::function<void(std::shared_ptr<PortCatalog>)>& early);

// A running import (one at a time): Porter.Job.
class Job {
public:
    std::wstring build;
    std::atomic<bool> cancelled{ false };
    void cancel();                                  // stops now, or after the batch being installed
    void cancelAndWait(DWORD ms);                   // cancel, wait for port.ps1 to exit, kill it anyway after ms
    std::shared_ptr<PortJob> view();                // snapshot for the window
    int okCount();
    int failedCount();
    bool exited() const { return exited_; }

private:
    friend bool importItems(const BuildInfo&, const std::vector<std::wstring>&, const std::shared_ptr<Job>&, const LogFn&, const std::function<void()>&);
    friend void tailBatch(const std::shared_ptr<Job>&, const std::function<void()>&, std::atomic<bool>*);
    struct State { std::wstring path, st; };
    std::mutex m_;
    int total_ = 0, done_ = 0, ok_ = 0, failed_ = 0, batch_ = 0, cpu_ = -1;
    std::wstring line_, phase_ = L"start", current_;
    ULONGLONG started_ = GetTickCount64(), phaseSince_ = started_, currentSince_ = started_;
    FILETIME startedFt_{};
    double secPerItem_ = 14;
    std::vector<State> items_;                      // every item in import order, st = pending|queued|converting|ok|failed
    std::vector<std::pair<std::wstring, std::wstring>> recent_;
    std::atomic<DWORD> pid_{ 0 };
    std::atomic<bool> installing_{ false }, exited_{ false };
    void setPhase(const wchar_t* p);                // m_ held
    void note(const std::wstring& line);
    void result(const std::wstring& path, bool ok);
    void next();
    void setInstalling(bool on);
    void kill();
    State* find(const std::wstring& path);          // m_ held
};

// Porter.ImportAsync: converts and installs paths into b with port.ps1 (blocking; run it on a worker thread).
// progress() is called on every line and every half second (the caller throttles what it posts).
bool importItems(const BuildInfo& b, const std::vector<std::wstring>& paths, const std::shared_ptr<Job>& job, const LogFn& log,
                 const std::function<void()>& progress);

// radarnite.com/catalog.json (cached a day in work()), kept only for the given ids: the preview of an item.
class Previews {
public:
    // Downloads (when older than a day) and parses it, keeping the ids of `keep` (lower-case). false = no data (error).
    bool load(const std::vector<std::wstring>& keepLower, const LogFn& log);
    // Parse a catalog.json text (tests): keeps the ids of keepLower.
    bool parse(std::string& text, const std::vector<std::wstring>& keepLower);
    PortPreview get(const std::wstring& id);
    bool loaded();
    size_t size();

private:
    struct P { std::string idLower, desc, intro; uint32_t fngg = 0; uint8_t flags = 0; };   // flags: 1 video, 2 hd, 4 page
    std::mutex m_;
    std::vector<P> items_;                          // sorted by idLower
    bool loaded_ = false;
};

} // namespace port
} // namespace rn
