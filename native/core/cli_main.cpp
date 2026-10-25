// rn-core-cli.exe: the core's command line without the window (console subsystem), for tests and timing.
// RadarniteLauncher.exe (native/ui) runs the same rn::cli::run() first and opens the window when it returns
// handled = false.
#include "cli.h"
#include "util.h"
#include <shellapi.h>

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool handled = false;
    int rc = rn::cli::run(argc, argv, &handled);
    if (!handled) {
        if (argc >= 2 && !wcscmp(argv[1], L"--play")) rc = rn::cli::playConsole(argc >= 3 ? argv[2] : nullptr);
        else {
            rn::err(L"rn-core-cli: no window in this build (the window is RadarniteLauncher.exe, native/ui); console commands only.");
            rc = 2;
        }
    }
    LocalFree(argv);
    return rc;
}
