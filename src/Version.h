#pragma once
// App version + name. EDOPROPLUS_VERSION is injected by CMake
// (target_compile_definitions) from the project() VERSION. The fallback keeps
// non-CMake / IDE-intellisense builds compiling.
#ifndef EDOPROPLUS_VERSION
#define EDOPROPLUS_VERSION "dev"
#endif
// GitHub "owner/name" for in-app update checks; blank disables the check.
#ifndef EDOPRO_UPDATE_REPO
#define EDOPRO_UPDATE_REPO ""
#endif

namespace edo {
inline constexpr const char* kAppName     = "EdoPro+";
inline constexpr const char* kAppVersion  = EDOPROPLUS_VERSION;
inline constexpr const char* kUpdateRepo  = EDOPRO_UPDATE_REPO;
}
