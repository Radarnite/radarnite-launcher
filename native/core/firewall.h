// Windows Firewall rules (launcher/Core.cs Firewall), group "FN Offline Builds": every exe of a build, the backend and
// the cosmetics tool may not reach anything but loopback (outbound blocked, inbound from non-loopback blocked).
// Read through INetFwPolicy2 (vtable calls, no IDispatch; no admin needed to read); created by "--firewall" elevated.
// Each operation walks the rule list once.
#pragma once
#include <set>
#include <string>
#include <vector>

namespace rn {
namespace firewall {

std::wstring group();                 // RADARNITE_FW_GROUP (tests) or "FN Offline Builds"
extern const wchar_t* const kNonLoopback;

// Every exe of the build (recursive, breadth-first like Directory.EnumerateFiles), then node.exe, the 3.0 backend and
// fn-cosmetics-gen.exe when present; full paths, without duplicates (OrdinalIgnoreCase).
std::vector<std::wstring> programsFor(const std::wstring& build);

// One pass over the rules: is the firewall on for the current profiles, and which programs have the group's
// outbound block rule.
struct Rules {
    std::wstring error;               // the COM failure ("" = read)
    std::wstring off;                 // "Windows Firewall is off for the current network profile" or ""
    std::set<std::wstring> blocked;   // upper-cased full paths
};
Rules read();
// "" = OK; otherwise what is missing (2.x Firewall.Check text).
std::wstring check(const Rules& r, const std::wstring& build);
std::wstring check(const Rules& r, const std::vector<std::wstring>& programs);   // programsFor() already walked
std::wstring check(const std::wstring& build);

// Creates the outbound + inbound rules of every program of the build (stale rules of the group for those programs
// removed first). Returns the number created; error = why it stopped (e.g. not elevated).
int create(const std::wstring& build, std::wstring* error);
// Removes every rule of the group; returns how many there were (like 2.x, also when removing them failed).
int removeAll(std::wstring* error);

} // namespace firewall
} // namespace rn
