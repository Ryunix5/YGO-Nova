#include "AssetPaths.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string g_root;
bool        g_resolved = false;

// A directory is "an assets dir" if it looks structurally like one - it
// should hold either cards.cdb or a script(s)/cards/fonts subfolder.
bool isAssetsDir(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(dir / "cards.cdb", ec) ||
           fs::is_directory(dir / "scripts", ec) ||
           fs::is_directory(dir / "script",  ec) ||
           fs::is_directory(dir / "cards",   ec) ||
           fs::is_directory(dir / "fonts",   ec);
}

// A "project root" is a dir that holds the assets dir AND a project marker
// (CMakeLists.txt or .git) - the build output dir has neither.
bool hasProjectMarker(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(dir / "CMakeLists.txt", ec) ||
           fs::exists(dir / ".git", ec);
}

#if defined(_WIN32)
fs::path exeDirectory() {
    // GetModuleFileNameW would be ideal but pulls in <windows.h>; argv[0]
    // is not accessible here. Fall back to the parent of CWD if needed.
    return fs::path();
}
#else
fs::path exeDirectory() { return fs::path(); }
#endif

} // namespace

void AssetPaths::resolve() {
    if (g_resolved) return;
    g_resolved = true;

    std::error_code ec;
    fs::path chosen;
    std::string how;

    // 1. explicit override -------------------------------------------------
    if (const char* env = std::getenv("EDOPROPLUS_ASSETS")) {
        fs::path p(env);
        if (fs::is_directory(p, ec)) {
            chosen = p;
            how    = "EDOPROPLUS_ASSETS env var";
        } else {
            printf("[Assets] EDOPROPLUS_ASSETS set but not a directory: %s\n",
                   env);
        }
    }

    fs::path cwd = fs::current_path(ec);

    // 2. walk up looking for a project root --------------------------------
    fs::path projectAssets, releaseAssets;
    if (chosen.empty()) {
        fs::path d = cwd;
        for (int i = 0; i < 8; ++i) {
            fs::path a = d / "assets";
            if (fs::is_directory(a, ec) && isAssetsDir(a)) {
                if (hasProjectMarker(d) && projectAssets.empty())
                    projectAssets = a;
                // remember an in-build assets copy too, for logging
                std::string ds = d.string();
                if (releaseAssets.empty() &&
                    ds.find("build") != std::string::npos)
                    releaseAssets = a;
            }
            fs::path up = d.parent_path();
            if (up == d) break;
            d = up;
        }
        if (!projectAssets.empty()) {
            chosen = projectAssets;
            how    = "project root (CMakeLists.txt or .git marker)";
        }
    }

    // 3. cwd/assets --------------------------------------------------------
    if (chosen.empty()) {
        fs::path a = cwd / "assets";
        if (fs::is_directory(a, ec)) {
            chosen = a;
            how    = "current working directory";
        }
    }

    // 4. last resort -------------------------------------------------------
    if (chosen.empty()) {
        fs::path a = exeDirectory();
        if (!a.empty() && fs::is_directory(a / "assets", ec)) {
            chosen = a / "assets";
            how    = "next to executable";
        }
    }

    if (chosen.empty()) {
        printf("[Assets] WARNING: no assets directory found on any search "
               "path. Falling back to %s/assets (may not exist).\n",
               cwd.string().c_str());
        g_root = (cwd / "assets").lexically_normal().string();
        return;
    }

    chosen = fs::absolute(chosen, ec).lexically_normal();
    g_root = chosen.string();

    printf("[Assets] canonical assets path : %s\n", g_root.c_str());
    if (!releaseAssets.empty()) {
        fs::path rel = fs::absolute(releaseAssets, ec).lexically_normal();
        printf("[Assets] release assets path detected: %s\n",
               rel.string().c_str());
        if (rel != chosen)
            printf("[Assets] WARNING: release assets DIFFER from canonical. "
                   "CMake POST_BUILD mirrors them - rebuild, or use "
                   "sync_projectignis_assets.py --mirror-release.\n");
    }
    printf("[Assets] using: %s   (resolved via %s)\n",
           g_root.c_str(), how.c_str());

    // chdir to the canonical assets' parent so every existing "assets/..."
    // relative path in CardDB, DuelManager, Renderer and Game now points at
    // the SAME place - we do not need to thread paths through every class.
    fs::path parent = chosen.parent_path();
    fs::current_path(parent, ec);
    if (ec) {
        printf("[Assets] WARNING: could not chdir to %s (%s)\n",
               parent.string().c_str(), ec.message().c_str());
    } else {
        printf("[Assets] working directory set to %s\n",
               parent.string().c_str());
    }
}

const std::string& AssetPaths::root() { return g_root; }

std::string AssetPaths::path(const std::string& rel) {
    if (g_root.empty()) return std::string("assets/") + rel;
    return g_root + "/" + rel;
}
