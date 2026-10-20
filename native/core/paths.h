// Paths (launcher/Core.cs Paths): builds live in <root>\<version>. The root is config.json "buildsFolder" (set by the
// installer or with "Change builds folder"); without it, the folder that contains the launcher (portable layout
// <root>\launcher).
#pragma once
#include <string>

namespace rn {
namespace paths {

// Folder of the launcher exe. RADARNITE_LAUNCHER_DIR overrides it (tests: run a copy of the core against another
// launcher tree without copying the exe into it).
const std::wstring& launcherDir();
std::wstring root();                 // cached until resetRoot()
void resetRoot();                    // config.json was saved
std::wstring config();               // <launcher>\config.json
std::wstring node();                 // <launcher>\node\node.exe (2.x dev tree backend runtime)
std::wstring backendScript();        // <launcher>\backend\server.mjs
std::wstring backend();              // <launcher>\backend\radarnite-backend.exe (3.0 backend, same CLI as server.mjs)
std::wstring redirectDll();
std::wstring gameServerDll();
std::wstring pakPatchDll();
std::wstring cosmeticsGen();
std::wstring win64(const std::wstring& build);
std::wstring shipping(const std::wstring& build);
std::wstring offline(const std::wstring& build);   // created on demand, like Paths.Offline

} // namespace paths
} // namespace rn
