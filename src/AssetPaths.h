#pragma once
#include <string>

// Single canonical place to resolve the app's assets directory so CardDB,
// DuelManager, Renderer and UI cannot disagree about where assets live.
//
// Why this exists: the app has two on-disk asset trees -
//   1) project root          : <repo>\assets
//   2) build output          : <repo>\build\windows\Release\assets
// CMake's POST_BUILD mirrors (1) into (2) after every build, but during
// debugging it's easy to update (1) and run the (2) copy stale - making it
// look like a script update never landed. resolve() picks ONE canonical root
// (preferring (1) when both exist), logs it, and chdirs the process to the
// canonical root's parent so every existing "assets/..." relative path in the
// app automatically points at the same place.
//
// Resolution order:
//   1. EDOPROPLUS_ASSETS environment variable, if it names a directory.
//   2. Walk up from CWD: the first ancestor that has BOTH an "assets/"
//      directory AND a project marker ("CMakeLists.txt" or ".git") - this
//      deterministically picks the project root over a build/release copy.
//   3. <cwd>/assets, if it exists.
//   4. <exe-dir>/assets as a last resort.
namespace AssetPaths {

// Resolve the canonical assets directory and chdir the process there.
// Must be called once, very early in Game::init (before openAuto, Renderer,
// font load). Safe to call repeatedly - subsequent calls are no-ops.
void resolve();

// Absolute path of the canonical "assets" directory. Empty until resolve().
const std::string& root();

// root() + "/" + rel, suitable as a relative path open even after chdir.
std::string path(const std::string& rel);

} // namespace AssetPaths
