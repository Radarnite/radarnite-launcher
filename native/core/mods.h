// mods.json: the settings of the game mods (patches\fnmods.dll), next to config.json in the launcher folder. The
// launcher passes its path to the game in FNMODS_CONFIG; fnmods.dll reads it at start and again when it changes, so a
// save while the game runs applies at once. Format and keys: pakpatch/fnconfig.c (the reader).
#pragma once
#include "../ui/api.h"

namespace rn {
namespace mods {

std::wstring path();             // <launcher>\mods.json
Mods load();                     // a missing or unreadable file gives the defaults (api.h Mods)
bool save(const Mods& m);        // written whole to a temporary file, then moved over mods.json

} // namespace mods
} // namespace rn
