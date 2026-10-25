// RadarniteLauncher.exe command line (launcher/Program.cs Main): the console verbs, with the same stdout and exit
// codes as 2.2.x. rn-core-cli.exe and the UI's WinMain both call run() before anything else.
//
//   --check [build]             report: version, support, cosmetics, firewall, preflight (starts nothing)
//   --scan [root]               the builds under root (default: the builds folder) and the folders ignored
//   --set-builds-folder <dir>   (installer) where the builds live
//   --firewall <build>          (elevated) create the firewall rules of that build; exit 1 if still missing
//   --firewall-remove           (elevated) remove every rule of the group (RADARNITE_FW_GROUP for tests)
//   --update-check              is a newer public release available? exit 10 when it is (installs nothing)
//   --cosmetics <build>         download keys/mappings and generate cosmetics.json
//   --stop                      stop every process running from inside the builds folder
//   --selftest-inject           the game launch is not in this core: prints why, exit 1
#pragma once

namespace rn {
namespace cli {

// handled = false when argv[1] is not a console verb (no argument, --play, --selftest-ui, --new-instance...): the
// caller opens the window. Otherwise returns the process exit code.
int run(int argc, wchar_t** argv, bool* handled);
// --play <build> without a window (rn-core-cli.exe): the session's Activity lines on stdout. 0 = the game ran,
// 1 = the launch failed, 2 = no such build.
int playConsole(const wchar_t* build);

} // namespace cli
} // namespace rn
