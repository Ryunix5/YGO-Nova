#pragma once
#include "CardDB.h"
#include "DuelManager.h"
#include "Renderer.h"
#include "SnapshotManager.h"
#include "UI.h"
#include <SDL.h>
#include <string>

// Global quit flag (set by UI quit button or SDL_QUIT event)
extern bool g_quit;

class Game {
public:
    Game();
    ~Game();

    bool init(const std::string& title, int w, int h);
    void run();
    void shutdown();

private:
    SDL_Window*   m_window  = nullptr;
    SDL_GLContext m_glCtx   = nullptr;
    int           m_width   = 1280;
    int           m_height  = 800;

    CardDB          m_db;
    SnapshotManager m_snap;
    DuelManager*    m_duel = nullptr;
    Renderer        m_rend;
    UI*             m_ui   = nullptr;

    void processEvents();
    void render();
};
