#include "Game.h"
#include "AssetPaths.h"
#include "UIStyle.h"
#include "AudioManager.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <SDL_opengl.h>
#include <cstdio>
#include <filesystem>

bool g_quit = false;

Game::Game() = default;

Game::~Game() { shutdown(); }

bool Game::init(const std::string& title, int w, int h) {
    m_width  = w;
    m_height = h;

    // ── Canonical assets path ────────────────────────────────────────────────
    // Resolve and chdir to the canonical assets root BEFORE anything reads
    // disk (fonts, CardDB::openAuto, Renderer card-back, DuelManager scripts).
    // After this, every existing "assets/..." relative path in the app
    // points at the SAME place (the project-root assets dir, by preference)
    // - we no longer pick up a stale build/.../Release/assets when run from
    // there. Logging shows both paths if a Release copy is also detected.
    AssetPaths::resolve();

    // ── SDL init ─────────────────────────────────────────────────────────────
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    m_window = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w, h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!m_window) {
        fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        return false;
    }

    m_glCtx = SDL_GL_CreateContext(m_window);
    if (!m_glCtx) {
        fprintf(stderr, "SDL_GL_CreateContext error: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(m_window, m_glCtx);
    SDL_GL_SetSwapInterval(1); // vsync

    // ── Dear ImGui init ───────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // ── App-wide design system ───────────────────────────────────────────────
    // One ImGui style applied to every screen so the menu, deck builder, duel
    // setup and the duel scene share the same palette / spacing / rounding /
    // typography hierarchy. Modern dark theme with a warm gold accent.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    // Rounding scale — softens the "boxy" XP-style controls.
    style.WindowRounding    = 8.f;
    style.ChildRounding     = 6.f;
    style.FrameRounding     = 6.f;
    style.PopupRounding     = 8.f;
    style.ScrollbarRounding = 8.f;
    style.GrabRounding      = 6.f;
    style.TabRounding       = 6.f;
    // Spacing scale — gives controls room to breathe.
    style.WindowPadding     = {16.f, 14.f};
    style.FramePadding      = {10.f, 6.f};
    style.CellPadding       = {8.f, 4.f};
    style.ItemSpacing       = {10.f, 8.f};
    style.ItemInnerSpacing  = {8.f, 6.f};
    style.IndentSpacing     = 18.f;
    style.ScrollbarSize     = 12.f;
    style.GrabMinSize       = 12.f;
    // Borders — subtle but present so panels read as layered.
    style.WindowBorderSize  = 1.f;
    style.ChildBorderSize   = 1.f;
    style.FrameBorderSize   = 0.f;
    style.PopupBorderSize   = 1.f;
    // Palette tokens. Dark navy base, warm gold accent, calm blue primary.
    const ImVec4 cBgDeep    = {0.045f, 0.058f, 0.082f, 1.00f};
    const ImVec4 cBgPanel   = {0.082f, 0.098f, 0.140f, 1.00f};
    const ImVec4 cBgRaised  = {0.115f, 0.135f, 0.190f, 1.00f};
    const ImVec4 cBgPopup   = {0.075f, 0.090f, 0.130f, 0.98f};
    const ImVec4 cBorder    = {0.230f, 0.275f, 0.380f, 0.55f};
    const ImVec4 cAccent    = {0.870f, 0.700f, 0.270f, 1.00f};
    const ImVec4 cAccentDim = {0.520f, 0.420f, 0.160f, 0.55f};
    const ImVec4 cPrimary   = {0.210f, 0.440f, 0.760f, 1.00f};
    const ImVec4 cPrimaryH  = {0.290f, 0.530f, 0.880f, 1.00f};
    const ImVec4 cTextHi    = {0.945f, 0.950f, 0.960f, 1.00f};
    const ImVec4 cTextMd    = {0.770f, 0.785f, 0.820f, 1.00f};
    const ImVec4 cTextLo    = {0.520f, 0.560f, 0.630f, 1.00f};
    auto& c = style.Colors;
    c[ImGuiCol_Text]                  = cTextHi;
    c[ImGuiCol_TextDisabled]          = cTextLo;
    c[ImGuiCol_WindowBg]              = cBgPanel;
    c[ImGuiCol_ChildBg]               = cBgPanel;
    c[ImGuiCol_PopupBg]               = cBgPopup;
    c[ImGuiCol_Border]                = cBorder;
    c[ImGuiCol_BorderShadow]          = {0.f, 0.f, 0.f, 0.f};
    c[ImGuiCol_FrameBg]               = cBgRaised;
    c[ImGuiCol_FrameBgHovered]        = {0.155f, 0.180f, 0.250f, 1.f};
    c[ImGuiCol_FrameBgActive]         = {0.195f, 0.225f, 0.305f, 1.f};
    c[ImGuiCol_TitleBg]               = cBgDeep;
    c[ImGuiCol_TitleBgActive]         = {0.155f, 0.190f, 0.270f, 1.f};
    c[ImGuiCol_TitleBgCollapsed]      = cBgDeep;
    c[ImGuiCol_MenuBarBg]             = cBgDeep;
    c[ImGuiCol_ScrollbarBg]           = {0.f, 0.f, 0.f, 0.2f};
    c[ImGuiCol_ScrollbarGrab]         = {0.240f, 0.285f, 0.395f, 0.85f};
    c[ImGuiCol_ScrollbarGrabHovered]  = {0.330f, 0.380f, 0.490f, 1.f};
    c[ImGuiCol_ScrollbarGrabActive]   = {0.400f, 0.450f, 0.560f, 1.f};
    c[ImGuiCol_CheckMark]             = cAccent;
    c[ImGuiCol_SliderGrab]            = cPrimary;
    c[ImGuiCol_SliderGrabActive]      = cPrimaryH;
    c[ImGuiCol_Button]                = {0.150f, 0.215f, 0.330f, 1.f};
    c[ImGuiCol_ButtonHovered]         = {0.220f, 0.310f, 0.460f, 1.f};
    c[ImGuiCol_ButtonActive]          = {0.270f, 0.370f, 0.540f, 1.f};
    c[ImGuiCol_Header]                = {0.155f, 0.190f, 0.275f, 1.f};
    c[ImGuiCol_HeaderHovered]         = {0.215f, 0.260f, 0.360f, 1.f};
    c[ImGuiCol_HeaderActive]          = cAccentDim;
    c[ImGuiCol_Separator]             = cBorder;
    c[ImGuiCol_SeparatorHovered]      = {0.300f, 0.360f, 0.480f, 1.f};
    c[ImGuiCol_SeparatorActive]       = cAccent;
    c[ImGuiCol_ResizeGrip]            = {0.f, 0.f, 0.f, 0.f};
    c[ImGuiCol_ResizeGripHovered]     = cAccentDim;
    c[ImGuiCol_ResizeGripActive]      = cAccent;
    c[ImGuiCol_Tab]                   = {0.115f, 0.135f, 0.190f, 1.f};
    c[ImGuiCol_TabHovered]            = {0.215f, 0.260f, 0.360f, 1.f};
    c[ImGuiCol_TabActive]             = {0.180f, 0.220f, 0.310f, 1.f};
    c[ImGuiCol_TabUnfocused]          = cBgPanel;
    c[ImGuiCol_TabUnfocusedActive]    = cBgRaised;
    c[ImGuiCol_PlotHistogram]         = cAccent;
    c[ImGuiCol_PlotHistogramHovered]  = cPrimaryH;
    c[ImGuiCol_TextSelectedBg]        = {cAccent.x, cAccent.y, cAccent.z, 0.32f};
    c[ImGuiCol_NavCursor]             = cAccent;
    c[ImGuiCol_NavWindowingHighlight] = {cAccent.x, cAccent.y, cAccent.z, 0.55f};
    c[ImGuiCol_NavWindowingDimBg]     = {0.f, 0.f, 0.f, 0.40f};
    c[ImGuiCol_ModalWindowDimBg]      = {0.f, 0.f, 0.f, 0.55f};

    // ── Typography hierarchy ─────────────────────────────────────────────────
    // Load four sizes from the cleanest UI font available on the system:
    //   1. project Inter (if shipped with assets)
    //   2. Windows Segoe UI Variable / Segoe UI
    //   3. ImGui default (last resort)
    // The four sizes (small / body / header / title) feed UIStyle so the
    // main menu title, section headings, body text and metadata all read at
    // their own scale instead of every label using the same 13-px debug font.
    {
        namespace fs = std::filesystem;
        const char* candidates[] = {
            "assets/fonts/Inter-Regular.ttf",
            "C:/Windows/Fonts/segoeuivar.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
            "/Library/Fonts/SF-Pro-Display-Regular.otf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            nullptr
        };
        const char* fontPath = nullptr;
        for (int i = 0; candidates[i]; ++i)
            if (fs::exists(candidates[i])) { fontPath = candidates[i]; break; }

        if (fontPath) {
            UIStyle::fBody   = io.Fonts->AddFontFromFileTTF(fontPath, 16.f);
            UIStyle::fSmall  = io.Fonts->AddFontFromFileTTF(fontPath, 13.f);
            UIStyle::fHeader = io.Fonts->AddFontFromFileTTF(fontPath, 22.f);
            UIStyle::fTitle  = io.Fonts->AddFontFromFileTTF(fontPath, 34.f);
            io.FontDefault   = UIStyle::fBody;
            printf("[font] loaded '%s' (body 16 / small 13 / header 22 / title 34)\n",
                   fontPath);
        } else {
            printf("[font] WARN: no UI font found; using ImGui default\n");
        }
    }

    ImGui_ImplSDL2_InitForOpenGL(m_window, m_glCtx);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ── Audio (best effort — missing files / no device do NOT block startup) ─
    if (gAudio().init()) {
        namespace fs = std::filesystem;
        // Source of truth is AudioManager::expectedSfx() so the count here can
        // never drift from what the diagnostics popups show.
        const char* const* kSfx = AudioManager::expectedSfx();
        int loaded = 0, expected = 0;
        for (int i = 0; kSfx[i]; ++i) {
            ++expected;
            std::string key  = kSfx[i];
            std::string path = std::string("assets/sfx/") + key + ".wav";
            std::error_code ec;
            bool present = fs::is_regular_file(path, ec);
            gAudio().load(key, path);
            if (present && gAudio().isLoaded(key)) ++loaded;
        }
        printf("[audio] sfx root  : %s\n",
               fs::absolute("assets/sfx").string().c_str());
        printf("[audio] sfx loaded: %d / %d  (muted=%s)\n",
               loaded, expected, gAudio().muted() ? "yes" : "no");
        if (loaded < expected)
            printf("[audio] some SFX are missing — run "
                   "`python tools/generate_sfx.py` to create placeholders.\n");
    } else {
        printf("[audio] no device — SFX disabled (gameplay unaffected).\n");
    }

    // ── Renderer and card DB ──────────────────────────────────────────────────
    m_rend.init();

    // Open all card databases: the runtime assets/cards.cdb plus every .cdb in
    // a BabelCDB-master folder (newer / extra cards). openAuto() prints the
    // absolute path, size and row counts of each database it finds.
    if (!m_db.openAuto())
        fprintf(stderr, "Warning: no card database found. "
                        "Place cards.cdb in assets/ or a BabelCDB-master folder.\n");

    // Create required folders
    std::filesystem::create_directories("assets/cards");
    std::filesystem::create_directories("assets/scripts");
    std::filesystem::create_directories("assets/decks");

    // Card scripts (.lua) define every card's EFFECT. Count them so it is
    // obvious if the script collection is missing/incomplete — without them
    // cards have no on-summon triggers and Spells/Traps cannot be activated.
    {
        namespace fs = std::filesystem;
        const char* subs[] = { "", "official", "unofficial", "rush",
                               "skill", "goat", "pre-errata" };
        int total = 0;
        for (auto sub : subs) {
            fs::path d = fs::path("assets/scripts") / sub;
            std::error_code ec;
            int n = 0;
            if (fs::is_directory(d, ec))
                for (auto& e : fs::directory_iterator(d, ec))
                    if (e.path().extension() == ".lua") ++n;
            if (n) printf("[scripts] assets/scripts/%s%s : %d .lua files\n",
                          sub, (*sub ? "" : "(root)"), n);
            total += n;
        }
        printf("[scripts] total card/procedure scripts: %d\n", total);
        if (total < 1000)
            printf("[SCRIPT ERROR] script collection looks incomplete or the "
                   "path is wrong - card effects will not work.\n");
    }

    m_duel = new DuelManager(m_db, m_snap);
    m_ui   = new UI(*m_duel, m_db, m_rend, m_snap);

    // Persistent user preferences — must happen AFTER audio init (we push
    // mute / volume into AudioManager) and AFTER the UI is constructed so
    // its toggle fields exist.
    m_ui->loadSettings();

    return true;
}

void Game::run() {
    while (!g_quit) {
        processEvents();
        if (g_quit) break;

        // Tick the duel engine if a duel is running
        if (m_duel->isRunning()) {
            m_duel->process();
        }

        // ── ImGui frame ───────────────────────────────────────────────────────
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        int winW, winH;
        SDL_GetWindowSize(m_window, &winW, &winH);
        m_ui->draw(winW, winH);

        // Apply an F11 fullscreen toggle requested by the UI this frame.
        if (m_ui->consumeFullscreenToggle()) {
            m_fullscreen = !m_fullscreen;
            SDL_SetWindowFullscreen(
                m_window, m_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }

        // ── Render ────────────────────────────────────────────────────────────
        render();
    }
}

void Game::processEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL2_ProcessEvent(&e);
        if (e.type == SDL_QUIT) g_quit = true;
        if (e.type == SDL_WINDOWEVENT &&
            e.window.event == SDL_WINDOWEVENT_RESIZED) {
            m_width  = e.window.data1;
            m_height = e.window.data2;
        }
    }
}

void Game::render() {
    ImGui::Render();

    int dispW, dispH;
    SDL_GL_GetDrawableSize(m_window, &dispW, &dispH);
    glViewport(0, 0, dispW, dispH);
    glClearColor(0.07f, 0.09f, 0.12f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_window);
}

void Game::shutdown() {
    // Defensive final save — covers the case where the user toggled
    // something via in-line UI but never opened the Settings popup. The
    // popup itself also saves on every change.
    if (m_ui) m_ui->saveSettings();
    delete m_ui;   m_ui   = nullptr;
    delete m_duel; m_duel = nullptr;

    gAudio().shutdown();
    m_rend.shutdown();
    m_db.close();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (m_glCtx)  { SDL_GL_DeleteContext(m_glCtx); m_glCtx  = nullptr; }
    if (m_window) { SDL_DestroyWindow(m_window);   m_window = nullptr; }
    SDL_Quit();
}
