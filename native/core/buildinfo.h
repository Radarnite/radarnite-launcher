// BuildInfo (launcher/Core.cs): a Fortnite build folder, its version (cached in offline\build.json), and what the
// launcher supports on it.
#pragma once
#include "../ui/api.h"
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace rn {

const wchar_t* supportName(Support s);   // "Yes" | "Experimental" | "No" (the C# enum names)

struct BuildInfo {
    std::wstring dir;        // where FortniteGame is
    std::wstring version;    // "17.50"; the raw build string when it has no Release-x.y-CL-n; "?" when unknown
    int major = 0, minor = 0;
    int64_t cl = 0;
    std::wstring engine;     // "4.26", "?"
    std::wstring folder;     // the folder under the root when the game is one level deeper ("" = dir)

    // Name shown and used to pick the build: the folder under the root (a zip extracted with its own top folder,
    // e.g. 3.6-CL-4019403\Fortnite\FortniteGame, keeps the outer name; dir is where FortniteGame is).
    std::wstring name() const;
    bool hasEosAntiCheat() const;
    bool usesIoStore() const;   // any .utoc in FortniteGame\Content\Paks (UE 4.26+/UE5 containers)
    std::pair<Support, std::wstring> lobby() const;
    std::pair<Support, std::wstring> match() const;

    static std::wstring engineFor(int major, int minor);
    // Reads the build in dir, or in a single nested folder of it (archives that carry their own top folder).
    // false = not a usable build (why says why). error = the exception text 2.x would have caught (Scan lists it).
    static bool read(const std::wstring& dir, BuildInfo* b, std::wstring* why, std::wstring* error = nullptr);
    // Every build under root, sorted by major, minor, name; the rest in skipped with the reason.
    static std::vector<BuildInfo> scan(const std::wstring& root, std::vector<std::pair<std::wstring, std::wstring>>* skipped);
    static std::vector<BuildInfo> all();
    // The build string "++Fortnite+Release-17.50-CL-17388565" stored in the client executable (memory-mapped).
    // false = the file could not be read (error); true with "" = not found.
    static bool scanVersion(const std::wstring& exe, std::wstring* version, std::wstring* error);
};

// Items in <build>\offline\cosmetics.json (0 = missing or unreadable); cached by size and write time.
int cosmeticsCount(const std::wstring& buildDir);

} // namespace rn
