// config.json (launcher/Core.cs Config): same keys, defaults, "_help" text and file format as 2.x. Unlike 2.x, keys
// the core does not know are kept (written back after the known ones, before "_help").
#pragma once
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace rn {

struct LauncherConfig {
    std::wstring displayName;
    std::wstring accountId;
    int port = 3551;
    std::wstring mode = L"practice";                 // legacy, kept for config.json compatibility
    std::wstring lastPlaylist = L"playlist_defaultsolo";
    int bots = 20;
    bool infiniteMaterials = true;
    bool infiniteAmmo = false;
    bool lateGame = false;
    std::wstring practicePlaylist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
    std::wstring matchPlaylist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
    bool botsAI = false;
    int autoStartSeconds = 60;
    std::wstring buildsFolder;

    // string keys written as JSON null (a hand-edited "key": null round-trips like 2.x), bit = field index
    uint32_t nulls = 0;
    // unknown top-level members: UTF-8 key -> value already written indented at depth 1
    std::vector<std::pair<std::string, std::string>> extra;
    int helpCount = 0;                               // entries of "_help" as read (-1 = "_help": null)

    // Config.Load: defaults when missing; a file with a JSON/type error is copied to config.json.broken and only its
    // name and account id are kept; a missing name or account id is generated; saved when anything was generated,
    // the file was missing or "_help" is not the current one.
    static LauncherConfig load();
    bool save() const;                               // rewrites "_help"; resets the cached root
    std::string serialize() const;                   // the exact bytes save() writes
};

// Window preferences ("lastBuild", "activityHeight"): HKCU\Software\Radarnite\Launcher\UI.
std::wstring uiPref(const wchar_t* key);
void setUiPref(const wchar_t* key, const std::wstring& value);

} // namespace rn
