#include "Game.h"
#include "Version.h"
#include "CrashHandler.h"
#include <cstdio>
#include <string>

int main(int /*argc*/, char** /*argv*/) {
    // First thing: arm the crash reporter so even init-time faults produce a
    // dump + a one-click GitHub issue instead of a silent exit.
    crash::install(edo::kAppVersion, edo::kUpdateRepo);
    Game game;
    std::string title = std::string(edo::kAppName) + "  v" + edo::kAppVersion;
    if (!game.init(title, 1280, 800)) {
        fprintf(stderr, "Failed to initialise EdoPro+\n");
        return 1;
    }
    game.run();
    game.shutdown();
    return 0;
}
