// Self-update from the public GitHub releases (launcher/Update.cs): the latest release's tag compared as semver with
// this build's version; its RadarniteLauncherSetup-<ver>.exe downloaded to %TEMP%, checked against the release's
// .sha256 asset (or the asset's own "digest"), and only then run. Offline-first: every error goes to
// <launcher>\update.log and is otherwise ignored.
#pragma once
#include "../ui/api.h"
#include <windows.h>
#include <functional>
#include <string>

namespace rn {
namespace update {

extern const wchar_t* const kRepo;   // "Radarnite/radarnite-launcher"

struct Version {
    int major = 0, minor = 0, patch = 0;
    std::wstring str() const;        // System.Version.ToString(): "2.2.1"
};
Version current();                   // version.h
// Tag "v2.2.1", "2.3", "v3.0.0-rc.1" -> version (pre-release / build metadata stripped). false = not a version.
bool parseTag(const std::wstring& tag, Version* v);

struct Release {
    bool found = false;              // false = up to date, offline or any error (Updater.CheckAsync returned null)
    Version version;
    std::wstring tag, installerUrl, sha256Url, digest, notes;
};
Release check();
// The digest in a .sha256 asset: the first white-space separated token that starts with 64 hex digits, lower-cased
// ("" = none).
std::wstring sha256Token(const std::wstring& text);
// Download, verify, start the installer. true = it was started (the launcher must exit so NSIS can replace it).
bool downloadVerifyRun(const Release& r);

// The whole window flow (Updater.RunInteractiveAsync): check, ask (MessageBox owned by owner), download, run.
// true = an install was started, the caller exits. canInstall = false while a game runs (only announced).
bool runInteractive(HWND owner, bool manual, bool canInstall, const std::function<void(const std::wstring&, Level)>& toast);

} // namespace update
} // namespace rn
