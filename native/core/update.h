// Self-update from the public GitHub releases (launcher/Update.cs): the latest release's tag compared as semver with
// this build's version; its RadarniteLauncherSetup-<ver>.exe downloaded to %TEMP%, checked against the release's
// .sha256 asset (or the asset's own "digest"), and only then run. Offline-first: every error goes to
// <launcher>\update.log and is otherwise ignored.
// A release build (version.h without a pre-release) does exactly what 2.x did: releases/latest only, which GitHub
// never answers with a prerelease. A pre-release build (3.0.0-rc.N, published as a GitHub prerelease) also reads the
// newest release of the list (/releases, first non-draft) and is offered whichever of the two is newer: the next rc
// or the final.
// Test channel (the owner's own installs, off for everyone else): HKCU\Software\Radarnite\Launcher "TestFeed" holds
// the URL of a feed.json ({"version", "installer", "sha256", "notes"}), set once with --test-channel <url>. check()
// then also reads that feed and offers whichever is newer; a test build found there is installed without asking
// (silent installer, launcher relaunched without taking the focus), never while a game runs.
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
    std::wstring pre;                // pre-release identifiers ("rc.1"), "" for a release; build metadata is dropped
    std::wstring str() const;        // "2.2.1" (System.Version.ToString() for a release), "3.0.0-rc.1"
};
Version current();                   // version.h RN_VERSION_STR
// Tag "v2.2.1", "2.3", "v3.0.0-rc.1", "v3.0.0+meta" -> version (the X.Y[.Z] part parsed as 2.x did; the pre-release
// kept, build metadata stripped). false = not a version.
bool parseTag(const std::wstring& tag, Version* v);
// Semantic Versioning 2.0.0 precedence (section 11): < 0, 0 or > 0. Numeric identifiers compare as numbers and sort
// before alphanumeric ones (ASCII order); a release sorts after its pre-releases (3.0.0-rc.1 < 3.0.0-rc.2 <
// 3.0.0-rc.10 < 3.0.0); with equal leading identifiers, more identifiers sort later.
int compare(const Version& a, const Version& b);

struct Release {
    bool found = false;              // false = up to date, offline or any error (Updater.CheckAsync returned null)
    Version version;
    std::wstring tag, installerUrl, sha256Url, digest, notes;
    bool test = false;               // from the test feed: installed silently, without asking
};
// feedOnly: the test feed alone (the test channel's periodic check; nothing when it is off).
Release check(bool feedOnly = false);

// The test channel's feed URL ("" = off) and its setter ("" or "off" turns it off). false = not http(s) or not saved.
std::wstring testFeed();
bool setTestFeed(const std::wstring& url);
// feed.json read into r (version, installer resolved against feedUrl, digest "sha256:<hex>", notes). false = *error.
bool parseFeed(const std::string& body, const std::wstring& feedUrl, Release* r, std::wstring* error);
// A test build was just installed by the updater: the version it was meant to be ("" = none); cleared once read.
std::wstring takeInstalledTest();
// The digest in a .sha256 asset: the first white-space separated token that starts with 64 hex digits, lower-cased
// ("" = none).
std::wstring sha256Token(const std::wstring& text);
// Download, verify, start the installer (with args, e.g. "/S /RELAUNCH=bg"). ready, when given, runs once the file is
// verified and may change args; false = not now (the installer is not started). true = it was started (the launcher
// must exit so NSIS can replace it).
bool downloadVerifyRun(const Release& r, const std::wstring& args = L"", const std::function<bool(std::wstring* args)>& ready = nullptr);

// The whole window flow (Updater.RunInteractiveAsync): check, ask (MessageBox owned by owner), download, run.
// true = an install was started, the caller exits. canInstall = false while a game runs (only announced).
// testOnly (the test channel's periodic check): only the test feed is read; GitHub is asked at each start.
// recheck: asked again once a test build is downloaded and verified (a game may have started meanwhile); false = not
// now; *foreground = the launcher's window is the active one (the relaunch takes the focus back only then).
bool runInteractive(HWND owner, bool manual, bool canInstall, const std::function<void(const std::wstring&, Level)>& toast,
                    bool testOnly = false, const std::function<bool(bool* foreground)>& recheck = nullptr);

} // namespace update
} // namespace rn
