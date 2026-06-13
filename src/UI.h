#pragma once
#include "imgui.h"
#include "DuelManager.h"
#include "CardDB.h"
#include "Renderer.h"
#include "SnapshotManager.h"
#include "Anim.h"
#include "Settings.h"
#include "Replay.h"
#include "NetSession.h"
#include "NetSnapshots.h"
#include <string>
#include <vector>
#include <unordered_map>

enum class Screen { Lobby, Duel, DeckBuilder, Replays, Multiplayer };

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

    Screen  m_screen = Screen::Lobby;

    // ── Screen draw functions ──────────────────────────────────────────────
    void drawLobby(int w, int h);
    void drawDuel(int w, int h);
    void drawDeckBuilder(int w, int h);
    void drawReplays(int w, int h);
    void drawMultiplayer(int w, int h);

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
                      ImVec2 screenPos, float zW, float zH, ImVec4 col);
    void drawSelectionPanel(int w, int h);
    void drawTestingBar(int w);
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

    // Deck builder
    char       m_searchBuf[128] = {};
    std::vector<CardInfo>  m_searchResults;
    Deck                   m_editDeck;
    Deck                   m_savedDeck;        // last loaded/saved snapshot —
                                                // diffed for the "unsaved changes" chip
    char                   m_deckNameBuf[64];
    std::vector<std::string> m_deckFiles;   // .ydk files found in assets/decks/
    int                    m_selDeckIdx   = -1;
    // Filter chips: monster / spell / trap / main-only / extra-only.
    bool                   m_dbFilterMon  = true;
    bool                   m_dbFilterSpl  = true;
    bool                   m_dbFilterTrp  = true;
    bool                   m_dbFilterMain = true;
    bool                   m_dbFilterExtra= true;
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

    // Testing mode
    bool   m_testingMode = false;
    bool   m_debugLog    = false;   // verbose ocgcore message logging

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
    // Per-zone "boss already announced" guard so a big monster sitting in a
    // zone doesn't re-trigger the centre entrance every frame it's present.
    uint32_t m_bossPrevMZ[2][7] = {{0}};
    bool     m_bossObsInited = false;
    // Helper: classify the summon-type label + whether a card qualifies as a
    // "boss" for the big-entrance animation. Defined in UI.cpp.
    bool     isBossCard(const CardInfo& ci) const;
    const char* summonTypeLabel(const CardInfo& ci, bool special) const;

    // Field-state delta observer — drives in-game SFX (draw / send_gy /
    // banish / damage / monster appear). Initialised on the first frame
    // after the engine boots so we never play "everything moved" at start.
    bool     m_sfxObsInited = false;
    uint32_t m_sfxPrevLP[2]   = {0, 0};
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
    // Room state once connected to the relay.
    std::string m_mpRoomCode;                // our active room code
    bool        m_mpRoomActive       = false; // room formed (created/joined)
    bool        m_mpRelayConnecting  = false; // socket up, awaiting room reply
    std::string m_mpRoomError;               // last friendly room error
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
    double      m_replayNextAt      = 0.0;       // next auto-feed time
    std::string m_replayDesyncMsg;
    edo::Replay m_replayActive;                  // the loaded replay being played
    std::string m_replayActivePath;              // file path, for header display

    // Helpers — actually wire the replay screen / duel screen together.
    void startReplayPlayback(const std::string& path);
    void stopReplayPlayback();
    void feedReplayTick();      // called once per frame in drawDuel

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
    void refreshDeckFiles();
};

