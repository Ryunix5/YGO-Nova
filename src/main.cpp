#include "Game.h"
#include <cstdio>

int main(int /*argc*/, char** /*argv*/) {
    Game game;
    if (!game.init("EdoPro+", 1280, 800)) {
        fprintf(stderr, "Failed to initialise EdoPro+\n");
        return 1;
    }
    game.run();
    game.shutdown();
    return 0;
}
