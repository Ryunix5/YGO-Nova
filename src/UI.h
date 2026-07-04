#pragma once
#include "imgui.h"
#include "DuelManager.h"
#include "CardDB.h"
#include "Renderer.h"
#include "SnapshotManager.h"
#include "Anim.h"
#include "Settings.h"
#include "Replay.h"
#include "TestingTimeline.h"
#include "NetSession.h"
#include "UpdateChecker.h"
#include "NetSnapshots.h"
#include "ArcadeSave.h"
#include <string>
#include <vector>
#include <set>
#include <unordered_map>

enum class Screen { Lobby, Duel, DeckBuilder, Replays, Multiplayer, Arcade };

class UI {
public:
    UI(DuelManager& dm, CardDB& db, Renderer& rend, SnapshotManager& snap);

    Screen currentScreen() const { return m_screen; }
    bool draw(int winW, int winH);

    // ── Settings — persisted user preferences ────────────────────────────
    // Game::init() calls loadSettings() once, then pushes audio + visual
    // toggles into their respective subsystems. saveSettings() is invoked
    // immediately after any Settings popup change AND from Game::shutdown.
    void loadSettings();
    void saveSettings();
    const edo::Settings& settings() const { return m_settings; }
    // Mirror the persisted animation settings into the live AnimManager.
    // Called from loadSettings() and after any animation-settings change so
    // toggles/speed/reduce-motion take effect immediately.
    void syncAnimConfig();

    // ── Toast notifications ──────────────────────────────────────────────
    // Pushes a non-blocking notification onto the top-right toast stack.
    // Safe to call from any UI code path; rendering is deferred to draw().
    void pushToast(const std::string& text, ImU32 color, double dur = 2.4);
    // Pushes a player-facing event into the Game log (rendered in the
    // Game tab). Distinct from m_dm.logEvent() which writes the technical
    // stream.
    void pushGameLog(const std::string& text, ImU32 color = 0);

private:
    DuelManager&     m_dm;
    CardDB&          m_db;
    Renderer&        m_rend;
    SnapshotManager& m_snap;

    edo::UpdateChecker m_update;       // in-app "newer release?" check
    bool   m_updateDismissed = false;  // user closed the update notice
    // Open a URL in the user's default browser (release page, etc.).
    void openExternalUrl(const std::string& url);

    Screen  m_screen = Screen::Lobby;

    // Currently loaded BGM track ("" = silence). Chosen per context (menu vs
    // duel) from assets/music/{menu,duel}/*.wav; see UI::draw.
    std::string m_musicPath;

    // Startup splash: fade "Sponsored by Dark Side" (with logo), then
    // "Made by Ryunix", then hand off to the lobby. Skippable.
    bool    m_introActive = true;
    double  m_introStart  = -1.0;
    void    drawIntro(int w, int h);

    // ── Screen draw functions ──────────────────────────────────────────────
    void drawLobby(int w, int h);
    void drawDuel(int w, int h);
    void drawDeckBuilder(int w, int h);
    void drawReplays(int w, int h);
    void drawMultiplayer(int w, int h);
    // EDOPro-style full-width online lobby (room table + toolbar), shown
    // while browsing on the relay transport; drawMultiplayer branches here.
    void drawOnlineLobby(int w, int h, float topY);
    // In-room screen (any transport, once hosting/connecting/connected):
    // room code header, YOU vs OPPONENT player cards, ready-up, Start Duel.
    void drawRoomScreen(int w, int h, float topY);

    // ── Arcade (Master Saga) ───────────────────────────────────────────────
    // Save-file campaigns: open 10 Master Packs + 10 Secret Packs (real MD
    // rarities), build from your pulls, duel friends online. One save per
    // friend group; the pool is fixed once the packs are gone.
    void drawArcade(int w, int h);
    void loadMdRarity();                         // lazy, one-time table build
    std::vector<uint32_t> rollMasterPack();      // 8 cards, MD slot odds
    std::vector<uint32_t> rollSecretPack(uint16_t setcode);
    const std::vector<uint32_t>& mdArchetypeBucket(uint16_t setcode);
    edo::ArcadeSave m_arcade;                    // the loaded campaign
    bool        m_arcadeLoaded = false;
    std::vector<std::string> m_arcadeFiles;      // save names on disk
    char        m_arcadeNameBuf[48] = {};
    std::vector<uint32_t> m_arcadeReveal;        // last pack, reveal order
    std::vector<bool>     m_arcadeRevealNew;     // first copy? ("NEW" badge)
    std::string m_arcadeLastPack;                // last pack's display name
    double      m_arcadeRevealAt = 0.0;          // reveal animation start
    int         m_arcadeView = 0;                // 0 = pack view, 1 = collection
    uint16_t    m_arcadeSecretPick = 0;          // selected key (setcode)
    bool        m_poolMode = false;              // deck builder pool restriction
    // Group-duel invites: the campaign is SHARED — you invite a friend with
    // an invite code (room code + PIN), the host syncs the campaign over the
    // wire (ArcadeSync) and the friend's save is created automatically.
    bool        m_arcadeGroupDuel = false;       // current MP session is one
    std::string m_arcadeInvitePin;               // PIN of the room we host
    char        m_arcadeInviteBuf[24] = {};      // invite-code join input
    void arcadePrimeRelayBufs();                 // fill name/relay from settings
    void arcadeHostInvite();                     // locked room + fresh PIN
    void arcadeJoinInvite();                     // join via m_arcadeInviteBuf
    void sendArcadeSync();                       // campaign name + leaderboard
    // Post-duel rewards: winner spins a prize wheel, loser banks 5 wild pack
    // tokens. Wheel overlay state machine: 0 hidden, 1 spinning, 2 result up.
    int         m_arcadeWheelState = 0;
    double      m_arcadeWheelT0 = 0.0;           // spin start time
    int         m_arcadeWheelResult = -1;        // landed segment index
    uint16_t    m_arcadeCraftPick = 0;           // craft view: selected pack
    char        m_arcadeKeySearch[48] = {};      // craft view: card filter
    // Master Duel rarity table (assets/arcade/md_rarity.txt) + derived data.
    std::unordered_map<uint32_t, uint8_t>  m_mdRarity;      // code -> 0..3
    std::unordered_map<uint32_t, uint64_t> m_mdSetcodes;    // code -> packed
    std::vector<uint32_t> m_mdByRarity[4];
    std::unordered_map<uint16_t, std::vector<uint32_t>> m_mdArchBuckets;
    bool        m_mdLoaded = false;

    // ── Duel sub-renders (content-only — called inside BeginChild blocks) ──
    void drawField(int w, int h);
    // Screen-space zone renderers — use SetCursorScreenPos + InvisibleButton
    void drawCardZone(const char* label, const CardState* card,
                      ImVec2 screenPos, float zW, float zH,
                      bool faceDown, int uid,
                      int zonePlayer = -1, uint8_t zoneLoc = 0,
                      uint32_t zoneSeq = 0);
    // True if the engine is currently asking the LOCAL player to pick a
    // placement zone — used by drawCardZone to glow legal field tiles.
    bool isPlacementMode() const;
    // True if `loc`/`seq` (engine-style: LOC_MZONE seq 0-6 incl. EMZ; LOC_SZONE
    // seq 0-4 + 5(FZ) + 6-7(Pendulum)) is a legal placement target according
    // to the engine's placeFlag bitmask for the current SelectPlace request.
    bool isPlacementLegal(uint8_t loc, uint32_t seq) const;
    void drawSideZone(const char* label, int count,
                      ImVec2 screenPos, float zW, float zH, ImVec4 col,
                      uint32_t topCode = 0, bool topHidden = false);
    void drawSelectionPanel(int w, int h);
    void drawTestingBar(int w);
    // Shorten an effect description for a compact prompt row: collapse
    // newlines and clip to one line when Settings.compactPrompts is on. The
    // FULL text is still shown in the right info panel on hover. Returns the
    // text unchanged when compact prompts are off.
    std::string compactPromptDesc(const std::string& full) const;
    // Fixed-width right-side card info panel (modern-simulator style).
    // Shows the hovered card (falling back to the selected / last hovered
    // one): image, name, type line, attribute/race/level, ATK/DEF, full
    // effect text (scrollable) and — when known — location/controller/
    // position. Width is constant so the field never reshapes.
    void drawCardInfoPanel(int w, int h);
    // Capture where a hovered card lives (controller/loc/seq/pos) so the
    // info panel can print "P2 · Monster Zone 3 · face-up ATK". Called from
    // the zone/hand hover handlers; prompt-button hovers leave it unset.
    void setInfoCtx(uint8_t con, uint8_t loc, uint32_t seq, uint32_t pos) {
        m_infoCon = con; m_infoLoc = loc; m_infoSeq = seq; m_infoPos = pos;
        m_infoCtxCode = m_hoveredCard;
    }
    // Stage C revised — compact, click-first UI pieces.
    void drawBottomActionStrip(int w, float h);
    void drawCompactPreviewOverlay(int screenW, float topH);
    // Field-first input model: floating popup anchored ABOVE the selected card
    // showing only that card's legal actions, plus the centered modal that
    // hosts MSG_SELECT_CARD / viewers / yes-no / option / chain / game-over.
    void drawCardActionPopup(int screenW, int screenH);
    void drawCenteredModal(int screenW, int screenH);
    bool isChainCandidate(uint8_t player, uint8_t loc, uint32_t seq,
                          int* outIdx = nullptr) const;
    // True if a SelectCard / SelectTribute / SelectUnselect prompt for the
    // local player offers the card at (player, loc, seq). `outIdx` receives the
    // candidate index so a board click can submit it (glow + click on the field
    // alongside the gallery picker).
    bool isSelectCandidate(uint8_t player, uint8_t loc, uint32_t seq,
                           int* outIdx = nullptr) const;
    // True if the engine is currently in SelectBattleCmd AND offers an
    // attack action originating at this card. `outIdx` receives the
    // matching engine index (cmd=1 entry) so the click handler can
    // respondIdleCmd(1, idx) without re-scanning. `outCanDirect` reports
    // whether direct attack is legal for this attacker.
    bool isAttackerLegal(uint8_t player, uint8_t loc, uint32_t seq,
                         int* outIdx = nullptr,
                         bool* outCanDirect = nullptr) const;
    // Resolves engine loc-info (controller / location / sequence) to a
    // screen-space rect using the field's cached zone rects. Returns false
    // if the rect cache isn't ready yet or the loc is not on the field.
    bool locInfoToRect(uint8_t con, uint8_t loc, uint32_t seq,
                       ImVec2* tl, ImVec2* br) const;

    // ── State ──────────────────────────────────────────────────────────────
    // Card hover preview
    uint32_t   m_hoveredCard = 0;
    CardInfo   m_hoveredInfo;
    // Info-panel context: the on-field placement of the card the panel is
    // showing. Only meaningful while m_infoCtxCode == m_hoveredCard (a
    // prompt-button hover changes the card without touching the context).
    uint32_t   m_infoCtxCode = 0;
    // Right-click "zoom" — a pinned large card reader (big art + full text),
    // dismissed with Esc / click-away / the close button. 0 = not showing.
    uint32_t   m_zoomCard    = 0;
    void       drawCardZoom(int w, int h);
    uint8_t    m_infoCon     = 0;
    uint8_t    m_infoLoc     = 0;
    uint32_t   m_infoSeq     = 0;
    uint32_t   m_infoPos     = 0;
    // Hovered prompt-choice context for the info panel: while the player
    // hovers an effect-choice row, the panel shows "Prompt option — <title>"
    // plus the full decoded text. Stamped with the frame count so stale
    // context fades out automatically (no per-frame clearing needed).
    std::string m_promptHoverTitle;
    std::string m_promptHoverText;
    int         m_promptHoverFrame = -10;

    // Click-first selection: a clicked card in hand/field/spell row narrows
    // the action panel to ONLY that card's legal actions. Cleared by clicking
    // the same card again, the Deselect button, or any state transition that
    // resets the action list (new turn, new selection request, etc.).
    uint32_t   m_selCode   = 0;
    uint8_t    m_selPlayer = 0;
    uint8_t    m_selLoc    = 0;
    uint32_t   m_selSeq    = 0;
    bool       isSelectedCard(const CardState& c) const {
        return m_selCode != 0
            && c.code   == m_selCode
            && c.player == m_selPlayer
            && c.loc    == m_selLoc
            && c.seq    == m_selSeq;
    }
    void selectCardFrom(const CardState& c) {
        if (isSelectedCard(c)) { clearSelection(); return; }
        m_selCode = c.code; m_selPlayer = c.player;
        m_selLoc  = c.loc;  m_selSeq    = c.seq;
    }
    void clearSelection() {
        m_selCode = 0; m_selPlayer = 0; m_selLoc = 0; m_selSeq = 0;
    }
    // True if any engine-legal idle action targets the card at (player, loc, seq).
    bool hasLegalActionFor(uint8_t player, uint8_t loc, uint32_t seq) const;

    // Duel: "declare a card name" (MSG_ANNOUNCE_CARD) search box.
    char       m_announceBuf[64] = {};

    // Deck builder
    char       m_searchBuf[128] = {};
    std::vector<CardInfo>  m_searchResults;
    Deck                   m_editDeck;
    Deck                   m_savedDeck;        // last loaded/saved snapshot —
                                                // diffed for the "unsaved changes" chip
    char                   m_deckNameBuf[64];
    std::vector<std::string> m_deckFiles;   // .ydk files found in assets/decks/
    int                    m_selDeckIdx   = -1;
    // Favorites (persisted to assets/config/favorites.txt) + a dropdown filter.
    std::set<std::string>  m_favDecks;      // favorite .ydk filenames
    char                   m_deckFilterBuf[64] = {};
    void   loadFavorites();
    void   saveFavorites();
    // Per-deck sleeve overrides: deck filename -> sleeve filename. Applied at
    // duel start; decks without an entry use the global Settings sleeve.
    std::unordered_map<std::string, std::string> m_deckSleeves;
    void   loadDeckSleeves();
    void   saveDeckSleeves();
    void   applySleeveForDeck(const std::string& deckPath);
    // Filter chips: monster / spell / trap / main-only / extra-only.
    bool                   m_dbFilterMon  = true;
    bool                   m_dbFilterSpl  = true;
    bool                   m_dbFilterTrp  = true;
    bool                   m_dbFilterMain = true;
    bool                   m_dbFilterExtra= true;
    // One-shot "focus the search box" flag (Ctrl+F in the deck builder).
    bool                   m_focusDeckSearch = false;
    // Advanced search: match effect text instead of names, plus attribute /
    // level filters applied to the result set.
    bool                   m_dbTextSearch  = false;  // search desc, not name
    uint32_t               m_dbAttrMask    = 0;      // OR of ATTRIBUTE_* (0=any)
    int                    m_dbLevelFilter = 0;      // exact level/rank (0=any)
    // Save toast — non-modal confirmation/error pinned at the top of the
    // deck panel for ~2s after Save runs.
    std::string            m_deckToastMsg;
    bool                   m_deckToastIsErr = false;
    double                 m_deckToastAt    = -1.0;
    // Currently hovered card in the deck grid — drives the preview panel.
    // Distinct from m_hoveredCard which is reused across screens.
    uint32_t               m_deckHoverCode  = 0;

    // Duel setup (lobby popup)
    char   m_deck0Path[256] = {};
    char   m_deck1Path[256] = {};
    int    m_deck0Idx       = -1;   // index into m_deckFiles
    int    m_deck1Idx       = -1;
    bool   m_duelSetupOpen  = false;
    // Deck consistency calculator (#2/#3): per-card role tag + popup.
    //   0 = Other, 1 = Starter (1-card combo), 2 = Engine, 3 = Non-engine.
    std::unordered_map<uint32_t, int> m_cardTags;
    bool   m_consistencyOpen = false;
    std::vector<uint32_t> m_sampleHand;   // #2 sample-hand tester (drawn cards)
    void   drawSampleHand(int n);         // draw n random cards from the main deck
    void   drawDeckConsistency();
    void   loadCardTags();
    void   saveCardTags();

    // Puzzle / Challenge mode (#4): preset boards the player must solve.
    struct PuzzleEntry {
        DuelManager::PuzzleSetup setup;
        std::string difficulty;   // "Easy" / "Medium" / "Hard"
        std::string desc;         // flavour / hint shown in the browser
    };
    std::vector<PuzzleEntry> m_puzzles;
    bool   m_puzzleBrowserOpen = false;
    bool   m_puzzleMode    = false;   // active duel is a puzzle
    int    m_activePuzzle  = -1;      // index into m_puzzles for Retry
    int    m_puzzleResult  = 0;       // 0 none, 1 solved, 2 failed
    int    m_puzzleDeckIdx = 0;       // chosen deck (board-break) into m_deckFiles
    std::string m_puzzleGoal;
    void   loadPuzzles();
    void   drawPuzzleBrowser();
    void   startPuzzleByIndex(int idx);
    void   drawPuzzleOverlay(int w, int h);   // goal banner + solved/failed

    // In-duel pause menu (#5): Esc opens Resume / Surrender / Quit to Lobby.
    bool   m_pauseMenuOpen   = false;
    bool   m_pauseConfirmSurrender = false;
    void   drawPauseMenu(int w, int h);

    // Right-click card context menu (#6): list a card's legal actions at the
    // cursor. Requested by the field/hand right-click handlers, drawn once/frame.
    bool     m_ctxRequest = false;     // a right-click asked to open the menu
    uint32_t m_ctxCode = 0;
    uint8_t  m_ctxPlayer = 0, m_ctxLoc = 0;
    uint32_t m_ctxSeq = 0;
    ImVec2   m_ctxPos = {0, 0};
    // Open the context menu for a card if it is yours and has legal actions.
    // Returns true if it took ownership of the right-click (so the caller skips
    // its zoom). loc uses LOC_* values.
    bool   tryOpenCardContext(uint8_t con, uint8_t loc, uint32_t seq, uint32_t code);
    void   drawCardContextMenu();

    // Best-of-3 match + side decking (#14). Offline only.
    bool   m_setupMatchMode = false;     // Duel Setup toggle
    bool   m_matchActive    = false;     // a best-of-3 is in progress
    int    m_matchWins[2]   = {0, 0};    // [0] = you, [1] = opponent
    int    m_matchGameNo    = 1;
    bool   m_matchGameScored = false;    // current game's result already counted
    bool   m_matchSiding    = false;     // deck builder is in side-deck mode
    std::string m_matchPlayerPath;       // your deck for this match (temp once sided)
    std::string m_matchOppPath;          // opponent deck (fixed across games)
    std::string m_matchOppName;
    void   startMatchGame();             // begin the next match game from stored decks

    // Match history + win/loss stats (#8).
    struct MatchRecord {
        std::string when, myDeck, oppDeck; char result = 'D';
        int  turns = 0;            // total turns the duel lasted (0 = unknown)
        int  wentFirst = -1;       // 1 = you went first, 0 = second, -1 unknown
    };
    std::vector<MatchRecord> m_matchHistory;
    bool   m_historyOpen = false;
    void   recordMatch(const std::string& myDeck, const std::string& oppDeck,
                       char result, int turns = 0, int wentFirst = -1);
    void   loadMatchHistory();
    void   drawHistory();

    // Banlist / format validation (#15).
    struct Banlist { std::string name;
                     std::unordered_map<uint32_t, int> limits; };  // code->0..3
    std::vector<Banlist> m_banlists;
    int    m_selectedBanlist = -1;   // -1 = no list (max 3 each)
    void   loadBanlists();
    int    cardLimit(uint32_t code) const;   // copies allowed under current list
    // Custom banlist / format editor.
    bool    m_banlistEditorOpen = false;
    Banlist m_editBanlist;                       // working copy being edited
    char    m_banlistNameBuf[64]   = {};
    char    m_banlistSearchBuf[64] = {};
    std::vector<CardInfo> m_banlistSearchResults;
    void   openBanlistEditor(bool fromCurrent);
    void   drawBanlistEditor();
    bool   saveBanlist(const Banlist& bl);       // write .lflist.conf; ret ok

    // Custom duel settings (Duel Setup popup).
    int    m_setupLP        = 8000;
    int    m_setupHand      = 5;
    bool   m_setupNoShuffle = false;
    bool   m_setupPassiveAI = false;
    // Hand-trap gauntlet: opponent opens with hand traps and fires them at
    // the player's combo. Forces the player to take turn 1.
    bool   m_setupGauntlet  = false;
    bool   m_forceHumanFirstOnce = false;
    // Preset opponent decks (#6): assets/decks/presets/*.ydk.
    //   m_opponentPreset: -1 = off (use chosen P2 deck), 0 = random, >=1 = file.
    std::vector<std::string> m_presetFiles;   // filenames in presets/
    int    m_opponentPreset = -1;
    void   loadPresetDecks();
    // Queue all of a deck's card art for background download up front, so the
    // field/builder don't pop in card-by-card on first view.
    void   prefetchDeckArt(const Deck& d);
    // Download any missing card scripts for these decks before the duel
    // starts (blocking, but a no-op unless the deck holds cards newer than
    // the installed script collection).
    void   ensureDeckScripts(const Deck& a, const Deck& b);

    // Testing mode
    bool   m_testingMode = false;
    // Testing-only "add card to hand" search box + results.
    char                   m_testHandSearch[64] = {};
    std::vector<CardInfo>  m_testHandResults;
    bool   m_debugLog    = false;   // verbose ocgcore message logging

    // ── Testing Mode timeline (offline deterministic rewind) ──────────────
    // A chess-style move history. Every response is recorded with the root
    // duel config; rewinding tears the engine down and rebuilds it from the
    // seed + the recorded responses, restoring FULL internal state (deck
    // order, once-per-turn flags, chain/lingering effects), not just the
    // visible field. Offline-only; disabled in MP/replay. See TestingTimeline.h.
    edo::TestingTimeline m_timeline;
    // True only WHILE a rebuild is replaying recorded responses into a fresh
    // engine — suppresses the response/replay recorders, SFX, animations and
    // network so the rebuild is silent and never double-records.
    bool   m_testingRebuilding = false;
    // Set when a rewind just rebuilt the engine, so the observer's "duel
    // started" seed sting + toast are skipped on the re-seed frame.
    bool   m_testingJustRestored = false;
    std::string m_testingLastRestore;       // diagnostics: last restore result
    int    m_testingHoverIdx   = -1;         // timeline row hovered (preview)
    // Capture the deterministic root at the start of an offline duel. The two
    // paths are the decks in REGISTERED order (team 0 = goes first), which the
    // coin toss may swap relative to P1/P2.
    void captureTestingRoot(const std::string& team0Path,
                            const std::string& team1Path,
                            uint32_t lp = 8000, uint32_t handCount = 5,
                            uint32_t drawCount = 1);
    // Start an offline duel, flipping a coin (when enabled) to decide who takes
    // the first turn. Registers decks in toss order, sets the local seat, wires
    // replay + testing capture, and shows a coin-result banner. Returns false if
    // the engine refused to start (decks restored to P1/P2 order on failure).
    bool startOfflineDuelWithCoinToss(const std::string& p1Path,
                                      const std::string& p2Path,
                                      int lp, int handCount, int drawCount);
    // Record one response into the timeline (called from the response
    // recorder). Gated to offline + testing-on + not-rebuilding.
    void recordTestingAction(const void* data, uint32_t len);
    // Rebuild the engine so exactly `applyCount` recorded responses are
    // applied (0 = just the opening hands). Offline only. `reason` is log-only.
    void testingJumpTo(int applyCount, const char* reason);
    void testingStepBack();
    void testingStepForward();
    void testingUndoHuman();   // undo to the human's last decision (offline)
    // True when the timeline rewind controls are usable (offline, has root,
    // not in replay). Multiplayer / replay get a friendly disabled note.
    bool testingRewindAvailable() const;
    // Build a short human label for an action from the live selection.
    std::string testingLabelForResponse() const;
    // The Testing Mode timeline panel (rendered inside the Tools drawer).
    void drawTestingTimeline();

    // Stage C visual toggles
    bool   m_showFieldNames = false; // small name strip overlay on every card

    // Log collapsed by default during play — the duel board is the focus,
    // and the player flips this open via the `>` button if they want logs.
    bool   m_logCollapsed = true;
    // Floating log drawer (replaces the old permanent left log column).
    // Toggled from the bottom bar; reserves no layout width.
    bool   m_logDrawerOpen = false;
    // Debug/testing toggles collapsed behind a "Tools" button so the
    // gameplay bottom bar doesn't read like a dev console.
    bool   m_toolsDrawerOpen = false;
    // Controls/help overlay (F1) + duel keyboard shortcuts.
    bool   m_helpOverlayOpen = false;
    void   handleDuelHotkeys();   // F1 = help, Esc = close top panel
    void   drawHelpOverlay(int w, int h);
    void   drawChainStack(int w, int h);   // numbered chain-link visualizer
    // Keybindings: a settings field awaiting a key press (nullptr = not
    // capturing). The Settings popup "Rebind" buttons set this; the next key
    // pressed is written into *m_rebindTarget. Resolve via keyFor().
    int*   m_rebindTarget = nullptr;
    // In a response window, show what the opponent is attempting (summon /
    // activation / attack) so the player can decide whether to chain.
    void   drawOpponentActionHint();
    // Centred response pop-out shown during the local player's chain window:
    // states what the opponent did + a Pass button; the player chains by
    // clicking a glowing card on the field.
    void   drawChainResponsePopup(int w, int h);
    // First-run welcome (name setup) — shown once when no config exists.
    bool   m_showWelcome = false;
    // Last opponent-action sequence we toasted, so each new summon/activation/
    // attack notifies the player exactly once.
    uint64_t m_uiLastActionSeq = 0;
    // Fullscreen toggle request (F11) — Game applies it to the SDL window.
    bool   m_fullscreenToggleReq = false;
    void   sortEditDeck();        // organise the deck-builder deck by type
    int    tidyEditDeck();        // drop over-limit / over-cap cards; ret count
    // Representative "boss" card for a deck (splashiest Extra monster, else
    // top Main monster). 0 if the deck is empty.
    uint32_t deckSignatureCard(const Deck& d) const;
    // Dominant archetypes in a deck, ranked by how many (copies of) cards
    // belong to each. Grouped by real setcode (so non-archetype cards like
    // Pot of Duality never form a group), labelled by the group's common
    // name-prefix. Returns {name, count} most-mentioned first.
    std::vector<std::pair<std::string,int>> deckArchetypes(const Deck& d) const;
    // Cached lobby boss art (from the last-edited deck), recomputed when the
    // source path changes so the lobby doesn't reload a deck every frame.
    uint32_t    m_lobbyBossCode = 0;
    std::string m_lobbyBossPath;
public:
    // Polled by Game once per frame to apply an F11 fullscreen toggle.
    bool   consumeFullscreenToggle() {
        bool r = m_fullscreenToggleReq; m_fullscreenToggleReq = false; return r;
    }
private:

    // Visual polish toggles
    bool   m_largePreview = false;   // 320x520 floating preview instead of 240x380
    // Layout Guides (Tools drawer): draws the play-area centre line, the
    // visible-arena centre line and the pixel delta on screen, so the
    // centering claim is verifiable by eye instead of trusting a log.
    bool   m_showLayoutGuides = false;
    bool   m_showZoneLabels = true;  // corner zone labels on occupied tiles
    bool   m_showLegalGlow = true;   // orange legal-action glow on cards/zones

    // One-shot transition state — used to play victory/defeat SFX exactly
    // once when the duel ends, instead of every frame.
    bool   m_endGameSfxFired = false;

    // ── Phase banner / boss-summon animation state (Stage A) ──────────────
    // Last phase value we showed a banner for. Works in offline / replay /
    // MP-client because it reads currentField().phase (snapshot-backed for
    // the client), so the banner plays on both peers without local ocgcore.
    // 0xFFFF = uninitialised (no banner on the very first observed frame).
    uint16_t m_animPrevPhase = 0xFFFF;
    // ── Phase banner presentation queue ───────────────────────────────────
    // The engine flashes through Draw/Standby instantly when there's nothing
    // to do, so sampling currentField().phase only ever sees the latest phase
    // and the intermediate banners get skipped. Instead we QUEUE the missing
    // phases and pace them out so the player always sees DRAW → STANDBY →
    // MAIN PHASE 1 even on an empty turn. Presentation only — never gates the
    // engine or networking; works in offline / replay / host / client because
    // it reads currentField().phase (snapshot-backed on the client).
    std::vector<uint16_t> m_phaseQueue;       // phases awaiting their banner
    double   m_phaseQueueNextAt   = 0.0;      // when the next banner may show
    uint16_t m_animObservedPhase  = 0xFFFF;   // last actual phase we sampled
    uint16_t m_animLastEnqueued    = 0;       // last phase pushed to the queue
    uint8_t  m_animPrevTurnPlayer  = 0xFF;    // detect new-turn wrap
    void     observePhaseForBanners();        // enqueue on phase change
    void     pumpPhaseBannerQueue();          // pop + emit one per interval
    // Per-zone "boss already announced" guard so a big monster sitting in a
    // zone doesn't re-trigger the centre entrance every frame it's present.
    uint32_t m_bossPrevMZ[2][7] = {{0}};
    bool     m_bossObsInited = false;
    // Helper: classify the summon-type label + whether a card qualifies as a
    // "boss" for the big-entrance animation. Defined in UI.cpp.
    bool     isBossCard(const CardInfo& ci) const;
    const char* summonTypeLabel(const CardInfo& ci, bool special) const;

    // ── Shared card-art draw helper ───────────────────────────────────────
    // Single source of truth for drawing a card image into a screen rect with
    // GUARANTEED-correct orientation: upright cards use UV (0,0)-(1,1); a
    // defense monster is rotated 90° CW (a true rotation, never a mirror).
    // All zone/preview paths route through this so no path can accidentally
    // flip a card (the reported Pendulum-flip symptom). When `dbgCheck` and
    // Debug Log are on, emits one [CARD RENDER CHECK] line per Pendulum code
    // so orientation is verifiable at runtime.
    void drawCardArt(ImDrawList* dl, uint32_t code, void* tex,
                     ImVec2 a, ImVec2 b, bool rotateDefenseCW,
                     bool dbgCheck = false);
    // Aspect-preserving fit of a card inside a slot rect (never stretches).
    // `landscape` fits the rotated defense footprint. Shared by every card
    // draw so S/T-zone Pendulum scales, monster zones and piles all keep the
    // true 421:614 proportion.
    void fitCardRect(ImVec2 a, ImVec2 b, bool landscape,
                     ImVec2* o0, ImVec2* o1);

    // Field-state delta observer — drives in-game SFX (draw / send_gy /
    // banish / damage / monster appear). Initialised on the first frame
    // after the engine boots so we never play "everything moved" at start.
    bool     m_sfxObsInited = false;
    uint32_t m_sfxPrevLP[2]   = {0, 0};
    // Animated LP: m_lpShown ticks toward the real value; m_lpGhost lingers
    // above it after damage (fighting-game style drain trail).
    float    m_lpShown[2]     = {8000.f, 8000.f};
    float    m_lpGhost[2]     = {8000.f, 8000.f};
    // New-draw highlight (#B): glow the last m_newDrawCount hand tiles briefly.
    double   m_newDrawAt      = -10.0;
    int      m_newDrawCount   = 0;
    // Excavate reveal: cards flipped up from the deck, shown fan-style.
    std::vector<uint32_t> m_excavateCards;
    double   m_excavateAt     = -10.0;
    void     drawExcavateReveal(int w, int h);
    int      m_sfxPrevHand[2] = {0, 0};
    int      m_sfxPrevGY[2]   = {0, 0};
    int      m_sfxPrevBN[2]   = {0, 0};
    int      m_sfxPrevMon[2]  = {0, 0};

    // Track per-zone occupancy so we can find which monster zone is the one
    // that JUST got populated (needed to anchor the summon ring animation).
    // 7 entries per player: 5 main zones + 2 EMZ slots (LOC_MZONE seq 5/6).
    uint32_t m_sfxPrevMZcode[2][7] = {{0}};

    // Lightweight animation system (presentation-only). Queued in the
    // observer + click handlers; rendered after the field paint so it sits
    // on top. Cleared on lobby return + on duel reset.
    edo::AnimManager m_anim;

    // Cached zone screen-rects from the latest drawField() — the observer
    // (which runs before field paint each frame) uses these to anchor
    // animations to real on-screen positions. Stale by one frame in the
    // very first frame after start; the observer's "init" branch skips
    // animations on that frame.
    bool   m_zoneRectsReady = false;
    ImVec2 m_rectMZ_tl[2][7], m_rectMZ_br[2][7];   // monster zones 0..4 + EMZ 5/6
    ImVec2 m_rectSZ_tl[2][5], m_rectSZ_br[2][5];   // spell/trap zones 0..4
    ImVec2 m_rectDeck_tl[2],  m_rectDeck_br[2];
    ImVec2 m_rectGY_tl[2],    m_rectGY_br[2];
    ImVec2 m_rectBN_tl[2],    m_rectBN_br[2];
    ImVec2 m_rectLP_tl[2],    m_rectLP_br[2];
    // Tracks when the user's explicit click most recently played a summon-
    // class SFX (summon / special_summon). The observer uses this to skip
    // its own "monster appeared" SFX so we don't get a double-thunk on a
    // Normal Summon (button click → engine resolves → field counter rises).
    // Negative = none yet this duel. ImGui::GetTime() seconds domain.
    double m_lastSummonSfxAt   = -1.0;
    double m_lastActivateSfxAt = -1.0;
    double m_lastSetSfxAt      = -1.0;

    // Audio settings popup toggle.
    bool   m_audioPopupOpen = false;
    // Assets and Debug diagnostic popups (top-right buttons in the lobby).
    bool   m_assetsPopupOpen = false;
    bool   m_debugPopupOpen  = false;
    // Master Settings popup (modern, multi-section, persists to disk).
    bool   m_settingsPopupOpen = false;

    // ── Settings + game log + toasts ───────────────────────────────────────
    // Persisted user preferences. The UI mirrors these into its local
    // toggle fields on startup and writes them back when the user changes
    // anything via the Settings popup. Save lives in saveSettings().
    edo::Settings m_settings;

    // Player-facing event stream. Distinct from m_dm.log() (which carries
    // technical traces) so normal play reads like a game instead of a
    // console. Populated by the state-delta observer in drawDuel and by
    // explicit calls in click handlers / duel lifecycle events.
    struct GameLogLine {
        std::string text;
        ImU32       color;
        double      at;     // ImGui::GetTime() seconds at insert
    };
    std::vector<GameLogLine> m_gameLog;

    // Toast notification queue — small floating message stack in the
    // top-right of the active screen. Auto-fades after `dur` seconds.
    struct Toast {
        std::string text;
        ImU32       color;
        double      at;
        double      dur;
    };
    std::vector<Toast> m_toasts;

    // Currently visible log tab (0 = Game, 1 = Debug). Mirrors into
    // m_settings.selectedLogTab on change.
    int m_logTab = 0;

    // ── Replay recording state ─────────────────────────────────────────────
    // m_replay accumulates metadata + responses + events for the LIVE duel.
    // It is reset on startDuel-from-lobby and finalised on duel end.
    edo::Replay m_replay;
    bool        m_replayRecording = false;
    double      m_replayStartTime = 0.0;     // ImGui::GetTime() at startDuel
    bool        m_replaySavedOnce = false;   // suppress duplicate auto-saves

    // ── Replay browser / viewer state ──────────────────────────────────────
    // Cached list of replay paths shown by Screen::Replays. Refreshed lazily.
    std::vector<std::string> m_replayFiles;
    int                      m_selectedReplay = -1;   // index into m_replayFiles
    edo::Replay              m_viewerReplay;          // metadata for selection
    bool                     m_viewerReplayValid = false;

    // ── Replay PLAYBACK state ──────────────────────────────────────────────
    // m_replayMode is the master flag — true from "Play Replay" click until
    // the user exits replay. Recording is disabled, live input is locked,
    // and the per-frame feeder advances m_replayIdx through the recorded
    // responses array. m_replaySpeed scales the inter-response delay
    // (1.0x = ~0.40s between feeds). m_replayDesyncMsg, when non-empty,
    // freezes the feeder and shows a modal warning.
    bool        m_replayMode        = false;
    bool        m_replayPlaying     = false;     // auto-feed on/off
    // Snapshot of DuelManager::localMode() taken at startReplayPlayback so
    // stopReplayPlayback can restore the user's prior setting. Without
    // this, exiting replay would leave P2 auto-AI disabled for the next
    // live duel (regression).
    bool        m_replayPrevLocal   = true;

    // ── Multiplayer ────────────────────────────────────────────────────
    edo::NetSession m_net;
    // Scratch buffers for the Multiplayer screen text inputs (mirrored
    // into m_settings on every edit so the values persist across runs).
    char        m_mpNameBuf[64]  = {};
    char        m_mpIPBuf[64]    = {};
    int         m_mpPortBuf      = 7878;

    // ── Online relay (Stage B/C) ────────────────────────────────────────
    // The Multiplayer screen has two transports: LAN (direct host/join) and
    // Online (both peers connect out to a relay server). 0 = LAN, 1 = Online.
    int         m_mpTransport      = 0;
    char        m_mpRelayAddrBuf[64] = {};   // relay server IP/host
    int         m_mpRelayPortBuf     = 7879;
    char        m_mpRoomCodeBuf[16]  = {};   // code typed when joining
    char        m_mpRoomPwBuf[33]    = {};   // optional room password
                                             // (host sets, joiners must match)
    // Room state once connected to the relay.
    std::string m_mpRoomCode;                // our active room code
    bool        m_mpRoomActive       = false; // room formed (created/joined)
    bool        m_mpRelayConnecting  = false; // socket up, awaiting room reply
    std::string m_mpRoomError;               // last friendly room error
    // ── Online lobby (auto-refreshing open-room list) ───────────────────
    bool        m_lobbyAutoRefresh   = true;  // poll the relay periodically
    double      m_lobbyNextRefreshAt = 0.0;   // ImGui::GetTime() of next poll
    double      m_lobbyLastRefreshAt = 0.0;   // when the last list landed
    // Drives the room-handshake kickoff exactly once when the peer appears.
    bool        m_mpHandshakeSent    = false;
    // Helpers for the online flow.
    void startRelayCreate();
    void startRelayJoin();
    void mpKickoffHandshake();               // send Hello/Deck/Ready once
    int         m_mpDeckIdx      = -1;   // chosen .ydk for our seat
    bool        m_mpReady        = false;
    // Mirror of the remote peer's state, populated from Hello / DeckInfo
    // / Ready packets.
    Deck        m_mpRemoteDeck;
    bool        m_mpRemoteDeckRcvd = false;
    bool        m_mpRemoteReady    = false;
    // Active session flag — true once StartDuel has been exchanged and
    // both sides should be inside the seeded duel.
    bool        m_mpInDuel         = false;
    // Suppression flag — set true while we're feeding bytes received
    // FROM the network, so the recorder hook doesn't echo them back.
    bool        m_mpFeedingRemote  = false;
    bool        m_mpStartupHealthRan = false;
    // Connection-lost modal latch.
    bool        m_mpConnLostShown  = false;
    // ── Multiplayer game-over propagation ───────────────────────────────
    // Host: send a GameOver{winner,reason} exactly once when its engine ends.
    bool        m_mpGameOverSent   = false;
    // Client (host-auth): set from the inbound GameOver so the Game Over
    // panel can render even though the client never runs an ocgcore.
    bool        m_mpRemoteDone      = false;
    int         m_mpRemoteWinner    = -1;   // engine seat: 0/1, else draw
    int         m_mpRemoteReason    = -1;   // DuelManager win-reason code
    // Anti-repeat guard for the MP-side zero-option chain auto-pass.
    // A unique key per (waitType + owner + cardCount + forced) fingerprint;
    // once we auto-pass for that key, we won't try again until the engine
    // moves to a new selection. Reset on any duel teardown OR whenever the
    // current prompt is no longer a SelectChain (so a fresh chain prompt
    // with the same fingerprint won't be silently skipped).
    uint64_t    m_mpLastAutoPassKey = 0;

    // ── Owner-aware MP response queue ─────────────────────────────────
    // Remote EngineResponse packets are NOT fed into ocgcore on arrival.
    // We queue them tagged with the response owner; the queue is drained
    // every frame against the current engine prompt — a response is only
    // fed when its owner matches the prompt owner. Without this, a P1
    // response arriving while the local engine is parked on a P2 chain
    // prompt would be shoved into the wrong state and the duel would
    // stall (the symptom in the earlier bug report).
    struct MpQueuedResponse {
        int      owner;
        uint32_t seq;
        int      waitTypeAtSend;
        std::vector<uint8_t> bytes;
    };
    std::vector<MpQueuedResponse> m_mpQueue;
    // Outgoing sequence — increments per local response. Embedded in
    // every EngineResponse packet for the peer's dedup logic.
    uint32_t    m_mpOutSeq          = 0;
    // Per-remote-owner last-seen seq; anything ≤ this is a duplicate
    // (network retransmit, replay attack, etc.) and ignored.
    uint32_t    m_mpLastSeenSeq[2]  = {0, 0};
    // Throttle state for the "WAIT REMOTE" diagnostic — fingerprint of
    // (waitType + promptOwner + queuedCount + blockedFlag). The drain
    // loop runs twice per frame; without this, the log would emit two
    // identical lines per frame while waiting. Set to 0 on duel teardown.
    uint64_t    m_mpLastWaitKey     = 0;

    // ── Prompt-state handshake ────────────────────────────────────────
    // Every time the local engine enters a new prompt, we hash a
    // fingerprint and broadcast it. The peer compares with its own
    // current prompt; a mismatch means the two engines have diverged
    // (one is on a different state than the other) and we surface it
    // as a desync diagnostic. The local prompt key is rebuilt every
    // frame; if it changes from the last sent key we transmit again.
    struct PromptInfo {
        uint64_t              promptSeq    = 0;
        uint32_t              waitType     = 0;
        uint8_t               owner        = 0;
        uint8_t               turnPlayer   = 0;
        uint16_t              phase        = 0;
        int32_t               minSel       = 0;
        int32_t               maxSel       = 0;
        uint32_t              optionCount  = 0;
        bool                  forced       = false;
        uint32_t              chainCount   = 0;
        std::vector<uint32_t> candidateCodes;
        // Set after the peer's PromptState arrives. nonzero promptSeq
        // means "we have a snapshot to compare against".
        bool                  valid        = false;
    };
    PromptInfo  m_mpLocalPrompt;
    PromptInfo  m_mpRemotePrompt;
    uint64_t    m_mpLocalPromptSeq  = 0;   // monotonic local prompt counter
    uint64_t    m_mpLastSentPromptHash = 0;
    bool        m_mpDesynced        = false;
    std::string m_mpDesyncSummary;          // human-readable mismatch detail
    void capturePromptInfo(PromptInfo& out);
    uint64_t hashPrompt(const PromptInfo& p) const;
    void sendPromptStateIfChanged();
    void handleRemotePromptState(const edo::NetMessage& m);

    void pumpMultiplayer();           // called once per frame
    void maybeAutoPassMpZeroOptionChain();
    void tryFeedQueuedMpResponses();
    void resetMpResponseState();      // clear queue + seq on duel teardown
    void handleNetMessage(const edo::NetMessage& m);
    void sendMpHello();
    void sendMpDeckInfo();
    void sendMpReady(bool r);
    void sendMpStartDuel();           // host only
    void mpOnLocalResponse(const void* data, uint32_t len);
    // True when the engine is awaiting input from the local player in MP.
    bool isLocalPromptOwner() const;

    // ── Host-authoritative multiplayer ────────────────────────────────
    // Default for new MP sessions. The host runs the only authoritative
    // ocgcore engine; the client renders entirely from FieldSnapshot
    // packets and submits choices via ClientChoice (mapped to engine
    // bytes on the host). When m_mpHostAuth is true:
    //   * client does NOT call m_dm.startDuel()
    //   * client does NOT send EngineResponse / PromptState
    //   * client does NOT run tryFeedQueuedMpResponses
    //   * client renders from m_mpRemoteField + m_mpRemoteSel
    //   * host sends FieldSnapshot after every engine advance
    //   * host sends PromptSnapshot when the selection belongs to
    //     the remote player; choice→bytes is held in m_mpHostChoices
    bool        m_mpHostAuth   = true;     // default for new MP sessions
    // Client-side mirror of host's authoritative state. Populated on
    // FieldSnapshot recv via edo::applySnapshotToField.
    FieldState        m_mpRemoteField;
    bool              m_mpRemoteFieldValid = false;
    // Client-side mirror of host's current prompt. Populated on
    // PromptSnapshot recv. The client converts this into a synthetic
    // SelectionRequest for the existing renderer via remoteSelection().
    edo::PromptSnapshotPayload m_mpRemoteSel;
    SelectionRequest           m_mpRemoteSelCached;   // built from m_mpRemoteSel
    bool                       m_mpRemoteSelValid     = false;
    // Client-side mirror of the LOCAL player's own Extra Deck contents
    // (codes in engine order) from the latest FieldSnapshot. The local
    // DuelManager is stopped in host-auth client mode, so the ED viewer
    // cannot query it — this list is the only source of own-ED contents.
    std::vector<uint32_t>      m_mpRemoteOwnExtra;
    // While the HOST owns the engine prompt, the host ships a zero-choice
    // "notice" PromptSnapshot; we keep just its waitType here to drive the
    // "Waiting for <opponent> — <prompt type>" status line. 0 = no notice.
    uint32_t                   m_mpOppPromptWait      = 0;
    // Host-side: monotonic prompt sequence + map of (seq → choice table).
    uint64_t                   m_mpHostPromptSeq      = 0;
    uint64_t                   m_mpHostLastSentPromptSeq = 0;
    struct HostChoice {
        std::vector<uint8_t>   responseBytes;   // exact bytes for respond()
        // Multi-pick confirm entry (SelectCard/SelectTribute min/max != 1).
        // responseBytes is empty; the host builds the {0, n, idx...} buffer
        // from ClientChoicePayload.extraIndices at apply time.
        bool                   multiPick = false;
    };
    std::unordered_map<uint32_t, HostChoice> m_mpHostChoices;  // choiceId → bytes
    uint64_t                   m_mpHostChoicesForSeq  = 0;
    uint32_t                   m_mpHostNextChoiceId   = 1;
    // Host needs to know "we already sent a snapshot for THIS engine
    // tick" so we don't flood the wire on every frame while idle.
    uint64_t                   m_mpHostLastFieldHash  = 0;
    // Client-side duel-active latch. The local DuelManager is stopped
    // in host-auth client mode, so `m_dm.isRunning()` is always false
    // and can NOT drive the "is there a duel?" decision. We set
    // m_mpRemoteDuelActive = true on the client's StartDuel handler
    // and on every FieldSnapshot we ingest, and clear it on
    // disconnect / return-to-lobby / explicit Game Over snapshot.
    // isDuelVisiblyRunning() consults this for the host-auth client
    // and m_dm.isRunning() everywhere else.
    bool                       m_mpRemoteDuelActive   = false;
    uint64_t                   m_mpLastSnapshotSeq    = 0;   // diagnostic
    // Client sent a ClientChoice and is waiting for the host's
    // updated FieldSnapshot / PromptSnapshot. Drives a small overlay
    // so the UI doesn't look frozen between click and ack.
    bool                       m_mpAwaitingHostUpdate = false;
    // One-shot log throttle for the [CLIENT DUEL STATE] diagnostic.
    uint64_t                   m_mpLastDuelStateKey   = 0;
    // Identity hash of the most recent PromptSnapshot the host shipped.
    // The host computes this from the SelectionRequest fingerprint and
    // ONLY bumps m_mpHostPromptSeq + resends when the identity changes.
    // Reset to 0 whenever the engine advances (via the recorder hook
    // in mpOnLocalResponse) or moves off a remote-owned prompt so a
    // structurally-identical future prompt still gets a fresh seq.
    uint64_t                   m_mpHostLastPromptIdentity = 0;

    // ── Host-side: snapshot send hooks ─────────────────────────────────
    void buildAndSendFieldSnapshot();         // host → client
    // host → client when sel.player != host. `reason` is log-only:
    // "frame" (per-frame pump) or "after-choice" (post-ClientChoice push).
    void buildAndSendPromptSnapshotIfRemote(const char* reason = "frame");
    void handleClientChoice(const edo::NetMessage& m); // host receives
    // host → client, exactly once when the authoritative engine ends.
    void sendGameOver(int winner, int reason);
    void handleGameOver(const edo::NetMessage& m);     // client receives
    // client → host concede; host forfeits the client's seat on its engine.
    void sendSurrender();
    void sendChatLine(const std::string& text);   // chat / emote / rematch
    void handleSurrender(const edo::NetMessage& m);    // host receives
    // ── Client-side: snapshot ingest ───────────────────────────────────
    void handleFieldSnapshot(const edo::NetMessage& m);
    void handlePromptSnapshot(const edo::NetMessage& m);
    void rebuildRemoteSelectionFromPrompt();
    void sendClientChoice(uint32_t choiceId,
                          const std::vector<uint32_t>& extra = {});
    void handleSyncError(const edo::NetMessage& m);
    // Local-click router. In host-auth client mode this maps the local
    // UI's WaitType + index into a remote choiceId and ships it via
    // ClientChoice. In every other mode (offline / host / replay) it
    // forwards straight to the appropriate m_dm.respond*() call.
    //   - SelectYesNo / SelectEffectYn: idx 1 = yes, 0 = no
    //   - SelectChain:                  idx -1 = Pass, ≥0 = chain index
    //   - SelectCard / SelectUnselect:  idx is the engine card index
    //   - SelectOption:                 idx is the option index
    void submitMpChoice(WaitType wt, int idx);
    // SelectIdleCmd / SelectBattleCmd router. Offline / host / replay →
    // m_dm.respondIdleCmd(cmd, index); host-auth client → maps (cmd, index)
    // onto the matching PromptChoice and ships ClientChoice. `label` is
    // log-only. Phase buttons use the pseudo-cmds (idle: 6=BP 7=EP;
    // battle: 2=M2 3=EP) with index 0, same as respondIdleCmd.
    void submitIdleCmd(int cmd, int index, const char* label = "");
    // SelectPlace router (field-tile click + fallback zone buttons).
    void submitPlace(int player, int loc, int seq);
    // SelectCard / SelectTribute multi-pick confirm router.
    void submitMultiCards(const std::vector<int>& indices);
    // SelectUnselect router. idx >= 0 picks a card; idx == -1 = Finish.
    void submitUnselect(int idx);
    // Source-of-truth for the GY/BN/ED viewer's Extra Deck contents:
    // host-auth client → snapshot mirror (own ED only, opponent hidden);
    // offline / host / replay → live engine query.
    std::vector<uint32_t> viewerExtraDeckCodes(int player);

    // Source-of-truth helpers — return either the live engine state
    // (offline / host / replay) or the snapshot mirror (host-auth
    // client). All render-path code MUST go through these.
    const FieldState&        currentField() const;
    const SelectionRequest&  currentSelection() const;
    bool                     usingRemoteField() const;   // host-auth client
    // True when "a duel is happening from this UI's perspective":
    //   - offline / host / replay: m_dm.isRunning()
    //   - host-auth client:        m_mpRemoteDuelActive
    // Replaces direct `m_dm.isRunning()` checks in render-path code so
    // the client doesn't blank out into "No duel in progress" while
    // its local engine sits idle by design.
    bool                     isDuelVisiblyRunning() const;
    // Snapshot-aware "engine is parked on an unhandled prompt" check.
    //   - host-auth client: always false (host owns the engine)
    //   - otherwise:        m_dm.isBlocked()
    bool                     isDuelVisiblyBlocked() const;
    // Emits a single [CLIENT DUEL STATE] log line per state transition.
    void                     logClientDuelStateIfChanged();
    // Aggregated health-check summary captured at app start, surfaced in
    // the Assets popup + Debug popup + diagnostics export.
    std::string m_healthSummary;
    int         m_healthWarnings = 0;
    void runStartupHealthCheck();
    std::string buildFullDiagnostics() const;
    bool        m_replayStepPulse   = false;     // one-shot "Step" trigger
    int         m_replayIdx         = 0;         // next response index
    float       m_replaySpeed       = 1.0f;
    // Seek (rewind / jump): replays are forward-only, so seeking restarts the
    // duel from the seed and fast-feeds responses (muted) up to the target
    // index. -1 = not seeking.
    int         m_replaySeekTarget  = -1;
    bool        m_replaySeekMutePrev = false;
    double      m_replayNextAt      = 0.0;       // next auto-feed time
    std::string m_replayDesyncMsg;
    edo::Replay m_replayActive;                  // the loaded replay being played
    std::string m_replayActivePath;              // file path, for header display
    char        m_replayRenameBuf[128] = {};     // rename field in the browser
    int         m_replayRenameFor      = -1;      // which list index it's seeded for

    // Helpers — actually wire the replay screen / duel screen together.
    void startReplayPlayback(const std::string& path);
    void stopReplayPlayback();
    void feedReplayTick();      // called once per frame in drawDuel
    void seekReplayTo(int responseIndex);   // rewind/jump via deterministic rebuild

    // Push a player-facing line into BOTH the on-screen Game Log and the
    // currently-recording replay's event timeline. Centralised so every
    // event the user sees is preserved in the replay.
    void pushGameAndReplay(const std::string& text, ImU32 color = 0);

    // Finalise + (optionally auto-)save the live replay. Called from the
    // observer when the duel resolves AND from "Return to Lobby" so we
    // capture both natural and forced ends.
    void finalizeReplay(const std::string& reason);

    // Hook installed on DuelManager at startDuel so every submitResponse
    // captures into m_replay.responses with a timestamp.
    void onResponseRecorded(const void* data, uint32_t len);
    void beginReplayRecording(const std::string& d0Path,
                              const std::string& d1Path);
    // Rolling in-app log used by the Debug popup. Captures the last ~200
    // messages from [audio]/[font]/[sfx]/[engine] paths so users can copy a
    // bug report without scraping the console.
    std::vector<std::string> m_debugLogBuf;
    // Last warning string surfaced for the Debug popup status line.
    std::string m_lastWarning;

    // GY / Banished / Extra Deck zone viewer (0 = closed; else a LOC_* value)
    int       m_viewerPlayer = -1;
    uint32_t  m_viewerLoc    = 0;
    char      m_viewerFilter[64] = {};            // name/code substring filter
    // Cached Extra Deck codes for the currently-open ED viewer.
    std::vector<uint32_t> m_viewerExtraCache;

    // MSG_SELECT_CARD multi-select state: indices into selection.cards[] that
    // the user has ticked. Cleared every time the engine moves out of the
    // SelectCard prompt (see drawSelectionPanel).
    std::vector<int> m_selSelIdx;
    char             m_selFilter[64] = {};        // search box for the prompt

    // Field-first action-popup anchor. Updated each time the user clicks a
    // hand or field card; the popup positions itself ABOVE this point so it
    // floats next to the clicked card instead of in a right-side panel.
    float            m_actionAnchorX = 0.f;
    float            m_actionAnchorY = 0.f;

    // ── Helpers ────────────────────────────────────────────────────────────
    Deck loadYdk(const std::string& path);
    void saveYdk(const Deck& d, const std::string& path);
    // Deck legality (#E): "" = legal, else a short reason (size / copy / banlist
    // limit). Uses the currently selected banlist for per-card limits.
    std::string deckLegality(const Deck& d);
    // .ydk text (de)serialisation — shared by file IO and clipboard share.
    Deck deckFromYdkText(const std::string& text);
    std::string deckToYdkText(const Deck& d);
    void refreshDeckFiles();
};

