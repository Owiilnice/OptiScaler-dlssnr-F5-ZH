//{{NO_DEPENDENCIES}}
// Microsoft Visual C++ generated include file.
// Used by OptiScaler.rc
//
#ifdef _DEBUG
#define VER_BUILD_DATE "Debug Build"
#define VER_BUILD_COMMIT "Debug"
#else
#include "resource_build_date.h"
#include "resource_build_commit.h"
#endif // !_DEBUG

#define VS_VERSION_INFO 1

// Next default values for new objects
//
#ifdef APSTUDIO_INVOKED
#ifndef APSTUDIO_READONLY_SYMBOLS
#define _APS_NEXT_RESOURCE_VALUE 101
#define _APS_NEXT_COMMAND_VALUE 40001
#define _APS_NEXT_CONTROL_VALUE 1001
#define _APS_NEXT_SYMED_VALUE 101
#endif
#endif

#define STRINGIZE_(s) #s
#define STRINGIZE(s) STRINGIZE_(s)

#define VER_MAJOR_VERSION 0
#define VER_MINOR_VERSION 7
#define VER_HOTFIX_VERSION 7
#define VER_BUILD_NUMBER 0

// This fork's own release train (git tags "vX.Y.Z-slug" on
// janblade/OptiScaler-F5-DLSSNR-Multipass, e.g. v0.1.6-dlssnr-enlarge-filter-simplify) --
// separate from VER_MAJOR/MINOR/HOTFIX_VERSION above, which track the upstream OptiScaler base
// this fork last synced to and are also read by the XeSS/FSR/FfxApi wrapper paths to spoof a
// reported version to games. Bump these three on every new fork release tag; version_check.cpp
// compares this against the fork repo's GitHub releases, not VER_MAJOR/MINOR/HOTFIX_VERSION.
#define NR_RELEASE_MAJOR_VERSION 0
#define NR_RELEASE_MINOR_VERSION 1
#define NR_RELEASE_HOTFIX_VERSION 20
#define NR_RELEASE_VERSION_STR                                                                                        \
    STRINGIZE(NR_RELEASE_MAJOR_VERSION) "." STRINGIZE(NR_RELEASE_MINOR_VERSION) "." STRINGIZE(NR_RELEASE_HOTFIX_VERSION)

// #define VER_DEV_RELEASE
// #define VER_PRE_RELEASE

// OPTI_VERSION stays tied to VER_MAJOR/MINOR/HOTFIX_VERSION (the frozen upstream-sync marker,
// see the comment above NR_RELEASE_* above) since it is what the XeSS/FSR/FfxApi wrapper paths
// report to games as the engine version -- not user-visible, and not this fork's own release
// number to begin with.
#define OPTI_VERSION STRINGIZE(VER_MAJOR_VERSION) "." STRINGIZE(VER_MINOR_VERSION) "." STRINGIZE(VER_HOTFIX_VERSION)

// FileVersion/ProductVersion (Explorer's Properties dialog, and VER_PRODUCT_NAME below, which is
// the in-game overlay's window title) show this fork's own release number instead -- otherwise
// both stay frozen at the upstream-sync marker (0.7.7) forever, since this fork stopped syncing
// upstream, and every release since has looked identical there regardless of what actually shipped.
#define VER_FILE_VERSION NR_RELEASE_MAJOR_VERSION, NR_RELEASE_MINOR_VERSION, NR_RELEASE_HOTFIX_VERSION, VER_BUILD_NUMBER
#define VER_FILE_VERSION_STR                                                                                           \
    NR_RELEASE_VERSION_STR "." STRINGIZE(VER_BUILD_NUMBER)

#define VER_PRODUCT_VERSION VER_FILE_VERSION

#ifdef VER_DEV_RELEASE
#define VER_PRODUCT_VERSION_STR NR_RELEASE_VERSION_STR "-dev (" VER_BUILD_COMMIT ") (" VER_BUILD_DATE ")"
#elif VER_PRE_RELEASE
#define VER_PRODUCT_VERSION_STR                                                                                        \
    NR_RELEASE_VERSION_STR "-pre" STRINGIZE(VER_BUILD_NUMBER) " (" VER_BUILD_COMMIT ") (" VER_BUILD_DATE ")"
#else
#define VER_PRODUCT_VERSION_STR NR_RELEASE_VERSION_STR "-final (" VER_BUILD_COMMIT ")"
#endif // VER_PRE_RELEASE

#define VER_PRODUCT_NAME "OptiScaler v" VER_PRODUCT_VERSION_STR
