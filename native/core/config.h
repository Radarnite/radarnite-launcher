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

    // Keys 3.x added after 2.x, kept among the extra members so a config that never set them is written exactly as
    // 2.x wrote it: "playMode" ("" = absent: every cosmetic owned, no profiles; legal | legal-vbucks | unlocked) and
    // "profile" (the progression's save, default "Main"). Read by the backend (backend/progress.mjs).
    std::wstring extraStr(const char* key) const;    // "" when missing or not a string
    void setExtraStr(const char* key, const std::wstring& value);
    std::wstring playMode() const { return extraStr("playMode"); }
    std::wstring profile() const { std::wstring p = extraStr("profile"); return p.empty() ? L"Main" : p; }
    // "botSkill": the difficulty 1-10 of the bots on the game's AI (6 = the game's own skill, the default)
    int extraInt(const char* key, int def) const;   // def when missing or not a number
    void setExtraInt(const char* key, int value);
    int botSkill() const { int v = extraInt("botSkill", 6); return v < 1 ? 1 : v > 10 ? 10 : v; }

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
