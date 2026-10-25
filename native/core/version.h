// Version of the launcher (2.x: FnOffline.csproj <Version>), the one source for the updater (update::current()), the
// window's version resource (native/ui/app.rc includes this file, so only preprocessor lines here) and, through
// `just release <version>`, the installer (tools/release.sh refuses a launcher whose ProductVersion is not --version).
// A build for another version passes the full semver instead of editing this file:
//   RN_VERSION=3.0.0-rc.1 native/core/build.sh       (just release 3.0.0-rc.1 does it)
// build.sh writes <outdir>/obj/version.gen.h with native/core/version.sh and defines RN_VERSION_GEN; native/ui
// compiles its version resource with the header of the core it links (../core/out/obj).
#pragma once

#ifdef RN_VERSION_GEN
#include "version.gen.h"
#endif

#ifndef RN_VERSION_STR
#define RN_VERSION_STR "3.0.0"          // full semver: ProductVersion, the updater, the installer name
#endif
#ifndef RN_VERSION_MAJOR
#define RN_VERSION_MAJOR 3
#endif
#ifndef RN_VERSION_MINOR
#define RN_VERSION_MINOR 0
#endif
#ifndef RN_VERSION_PATCH
#define RN_VERSION_PATCH 0
#endif
#ifndef RN_VERSION_BUILD
#define RN_VERSION_BUILD 0              // 4th FILEVERSION field: N of a "-rc.N" pre-release, 0 for a release
#endif
#ifndef RN_VERSION_FILE_STR
#define RN_VERSION_FILE_STR "3.0.0.0"   // the FileVersion string, X.Y.Z.N (2.x: "2.2.1.0")
#endif
#ifndef RN_VERSION_FLAGS
#define RN_VERSION_FLAGS 0x0L           // FILEFLAGS: VS_FF_PRERELEASE (0x2L) for a pre-release
#endif
