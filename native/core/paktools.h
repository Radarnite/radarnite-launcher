// The pak tools in <launcher>\tools, one copy pinned by sha256 (paktools/ in the repo, tools/vendor-pak-tools.sh):
// fn-modkit sigfit (fn-modkit-sigfit\ and sigfit-installed.ps1, run before each classic-pak launch) and
// game-guard.ps1 (refuses automated launches next to an anti-cheat or another game). paktools_manifest.h holds the
// hashes this launcher was built with; a copy that does not match them is never run as if it were current.
// Scripts outside the launcher still call a dev tree's copy (<builds folder>\launcher\tools\sigfit-installed.ps1):
// syncDevTree() keeps that copy identical to the launcher's (the installer runs it through --sync-pak-tools).
#pragma once
#include "util.h"
#include <string>

namespace rn {
namespace paktools {

const wchar_t* version();                 // the fn-viewer commit the tools were built from
std::wstring dir();                       // <launcher>\tools
std::wstring sigfitScript();              // <launcher>\tools\sigfit-installed.ps1
std::wstring sigfitTool();                // <launcher>\tools\fn-modkit-sigfit\fn-modkit.dll
// "" = every file of the manifest is in folder with its sha256; else the first one that is missing or differs.
std::wstring verify(const std::wstring& folder);
// <builds folder>\launcher\tools when a dev tree's sigfit-installed.ps1 is there and it is not this launcher's own
// tools folder; "" otherwise.
std::wstring devTreeDir();
// The dev tree's copy replaced by the launcher's when it differs (the old one moved to
// <builds folder>\launcher\backup-pak-tools-<time>), then checked again. true = no dev tree, up to date or refreshed
// and verified; false = it still differs (every line logged starts with "Error:").
bool syncDevTree(const LogFn& log);
// game-guard.ps1: "" = free; otherwise "busy: <what>", or why the guard could not answer (refused then too).
std::wstring gameGuard();

} // namespace paktools
} // namespace rn
