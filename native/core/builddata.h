// BuildData (launcher/Core.cs): the data a build needs to list its cosmetics (AES keys and type mappings, public
// archives on GitHub; data only, nothing is executed), the cosmetics generator, and adding a build from a zip or a
// folder.
#pragma once
#include "buildinfo.h"
#include "util.h"
#include <string>

namespace rn {
namespace builddata {

// FetchAsync: aes.json / mappings.usmap into <build>\offline when missing. Returns the offline folder.
std::wstring fetch(const BuildInfo& b, const LogFn& log);
// GenerateCosmeticsAsync: fetch, then tools\cosmetics-gen\fn-cosmetics-gen.exe -> offline\cosmetics.json (log in
// offline\cosmetics-gen.log). false = failed, why in *error (BuildData.LastCosmeticsError).
bool generateCosmetics(const BuildInfo& b, const LogFn& log, std::wstring* error);
// AddZipAsync: extracted (Windows tar.exe, below-normal priority) to <root>\_extrayendo_<time>, then moved to
// <root>\<version>. false = nothing added (logged).
bool addZip(const std::wstring& zip, const LogFn& log, BuildInfo* added);
// AddFolderAsync: moved when it is on the builds folder's drive, otherwise copied.
bool addFolder(const std::wstring& folder, const LogFn& log, BuildInfo* added);

} // namespace builddata
} // namespace rn
