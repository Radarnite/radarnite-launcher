// Placeholder for launch.cpp (see launch.h): this core does not start the game.
#include "launch.h"

namespace rn {

HANDLE LaunchGame(const LaunchRequest&, std::wstring* error) {
    if (error) *error = L"game launch not implemented in this core";
    return nullptr;
}

} // namespace rn
