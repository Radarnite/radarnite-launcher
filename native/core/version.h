// Version of the launcher (2.x: FnOffline.csproj <Version>, read by the updater). The release build may override it:
// RN_DEFINES='-DRN_VERSION_MAJOR=3 -DRN_VERSION_MINOR=0 -DRN_VERSION_PATCH=1' native/core/build.sh
#pragma once

#ifndef RN_VERSION_MAJOR
#define RN_VERSION_MAJOR 3
#endif
#ifndef RN_VERSION_MINOR
#define RN_VERSION_MINOR 0
#endif
#ifndef RN_VERSION_PATCH
#define RN_VERSION_PATCH 0
#endif
