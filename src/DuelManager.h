#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <chrono>
#include "CardDB.h"
#include "SnapshotManager.h"
#include "ocgapi.h"
#include "ocgapi_types.h"

// Card location constants (mirror ocgcore)
enum Location : uint32_t {
    LOC_DECK  = 0x01,
    LOC_HAND  = 0x02,
    LOC_MZONE = 0x04,
    LOC_SZONE = 0x08,
    LOC_GY    = 0x10,
    LOC_REM   = 0x20,  // banished
    LOC_EXTRA = 0x40,
};

enum CardPosition : uint32_t {
    POS_FACEUP_ATTACK    = 0x1,
    POS_FACEDOWN_ATTACK  = 0x2,
    POS_FACEUP_DEFENSE   = 0x4,
    POS_FACEDOWN_DEFENSE = 0x8,
};

struct CardState {
    uint32_t code     = 0;
    uint32_t loc      = 0;
    uint32_t seq      = 0;
    uint32_t pos      = POS_FACEUP_ATTACK;
    uint8_t  player   = 0;
    std::string name;
};

struct FieldState {
    uint32_t lp[2]    = {8000, 8000};
    uint16_t phase    = 0;
    uint8_t  turn     = 0;
    uint8_t  turnPlayer = 0;
    int      turnCount  = 0;   // becomes 1 on the first MSG_NEW_TURN

    std::vector<CardState> monsters[2];   // 5 zones each
    std::vector<CardState> spells[2];     // 5 S/T + 1 field zone
    std::vector<CardState> hand[2];
    std::vector<CardState> gy[2];
    std::vector<CardState> banished[2];
    int deckCount[2]  = {0, 0};
    int extraCount[2] = {0, 0};
};

// What the engine is currently waiting for
enum class WaitType {
    None,
    SelectIdleCmd,
    SelectBattleCmd,
    SelectYesNo,
    SelectEffectYn,
    SelectOption,
    SelectCard,
    SelectChain,
    SelectPlace,
    SelectPosition,
    SelectTribute,
    SelectCounter,
    SelectSum,
    SelectUnselect,   // MSG_SELECT_UNSELECT_CARD — summon material selection
    Waiting,
    RawPrompt,        // engine awaits input but parser doesn't cover this
                      // MSG_* yet. m_selection.player is best-effort (the
                      // 2nd byte of the captured frame); m_lastUnknownMsg*
                      // members hold the raw type + payload for logging.
};

// A decoded ocgcore effect "description" value. Engine descriptions for
// card-specific effect strings are aux.Stringid(code,idx) == code*16+idx;
// small values that do not resolve to a real card are generic system/hint
// strings. Decoding lets the log and UI name *which* effect of a multi-effect
// card is being offered — e.g. distinguishing Vanquish Soul Razen's on-summon
// Trigger Effect from his Quick Effect, which a bare card name/code cannot.
struct EffectDesc {
    uint64_t    raw          = 0;   // raw engine description value
    bool        isCardString = false;
    uint32_t    cardCode     = 0;   // raw >> 4   (valid when isCardString)
    int         index        = -1;  // raw & 0xF  (0 -> str1, 1 -> str2, ...)
    std::string text;               // decoded effect text, when resolvable
};

// Where, in the turn flow, a selection/effect window was opened. Lets the log
// and UI label an effect option by timing instead of guessing from the name.
enum class TimingContext {
    Unknown,
    PostSummonTrigger,    // chain window opened immediately after a Summon
    IdleMainPhase,        // activatable listed inside MSG_SELECT_IDLECMD
    QuickChainWindow,     // chain window during your own turn (not post-summon)
    OpponentChainWindow,  // chain window during the opponent's action
};

// One engine-validated action offered inside MSG_SELECT_IDLECMD. The duel is
// driven entirely from these — the UI never invents an action the engine did
// not list. `cmd`/`index` are passed straight back via respondIdleCmd().
//
// Inside SelectBattleCmd specifically:
//   * cmd == 0  → activatable (chainable spell/trap/effect from the field)
//   * cmd == 1  → attackable  (this monster may declare an attack)
struct IdleAction {
    uint32_t    code  = 0;
    int         cmd   = 0;   // 0 Summon  1 SpSummon  2 Reposition
                             // 3 Set(monster)  4 Set(S/T)  5 Activate
    int         index = 0;   // position within that command's list
    std::string name;
    uint8_t     con   = 0;   // controller of the card
    uint8_t     loc   = 0;   // location of the card
    uint32_t    seq   = 0;   // zone sequence of the card
    EffectDesc  effect;      // decoded effect description (only for cmd 5)
    bool        canDirect = false;  // SelectBattleCmd attackable: direct attack legal
};

// Parsed MSG_ATTACK payload. Pushed by the message loop and drained by the
// UI animation layer once per frame. The UI translates loc-info to screen-
// rects using its own zone-rect cache; a direct attack is signalled by
// targetLoc == 0 (LOCATION_NULL) — there is no live target card.
struct AttackEvent {
    uint8_t  attackerCon  = 0;
    uint8_t  attackerLoc  = 0;
    uint32_t attackerSeq  = 0;
    uint32_t attackerPos  = 0;
    uint8_t  targetCon    = 0;
    uint8_t  targetLoc    = 0;     // 0 → direct attack
    uint32_t targetSeq    = 0;
    uint32_t targetPos    = 0;
    bool     direct       = false;
};

// Parsed MSG_MOVE payload (a card changed location). Drained by the UI to play
// movement animations (placed on field, sent to GY, milled from deck, etc.).
struct MoveEvent {
    uint32_t code     = 0;
    uint8_t  prevCon  = 0; uint8_t prevLoc = 0; uint32_t prevSeq = 0;
    uint8_t  curCon   = 0; uint8_t curLoc  = 0; uint32_t curSeq  = 0;
    uint32_t curPos   = 0;   // destination position (face-down sets stay hidden)
};

struct SelectionRequest {
    WaitType     type        = WaitType::None;
    uint8_t      player      = 0;
    int          min         = 0;
    int          max         = 0;
    bool         forced      = false;
    int          summonCount = 0;  // how many Normal-Summonable cards are in cards[]
    int          msetCount   = 0;  // how many Monster-Settable cards follow
    std::vector<CardState> cards;   // select targets (SelectCard / SelectChain)
    std::vector<int>       options; // for SelectOption

    // SelectChain / SelectEffectYn: the decoded effect description for each
    // entry in cards[] (parallel array). SelectEffectYn has exactly one entry.
    std::vector<EffectDesc> chainEffects;
    // When/where this window opened — drives Trigger-Effect-vs-Quick-Effect
    // labelling in the log and UI.
    TimingContext timing = TimingContext::Unknown;

    // SelectIdleCmd / SelectBattleCmd: every action the engine permits.
    std::vector<IdleAction> idle;
    bool        toBP        = false;   // IDLECMD: "go to Battle Phase" allowed
    bool        toM2        = false;   // BATTLECMD: "go to Main Phase 2" allowed
    bool        toEP        = false;   // "end turn / end battle phase" allowed
    // SelectPlace: bitmask of occupied/forbidden zones, and how many to pick.
    uint32_t    placeFlag   = 0;
    int         placeCount  = 0;
};

struct Deck {
    std::string name;
    std::vector<uint32_t> main;
    std::vector<uint32_t> extra;
    std::vector<uint32_t> side;
};

class DuelManager {
public:
    DuelManager(CardDB& db, SnapshotManager& snap);
    ~DuelManager();

    bool startDuel(const Deck& p0deck, const Deck& p1deck,
                   uint32_t lp = 8000, uint32_t handCount = 5,
                   uint32_t drawCount = 1);
    void endDuel();

    // Run the engine — returns false when duel ends
    bool process();

    // Called by UI to submit a player response
    void respond(const void* data, uint32_t len);

    // Respond helpers for common cases
    void respondInt(int value);
    void respondYesNo(bool yes);
    void respondSingleCard(int index);
    void respondIdleCmd(int t, int s = 0);  // t=6→BP, t=7→EP, t=0+s→Summon card s
    void respondChain(int index);           // -1 = pass, otherwise chain index
    void respondPlace(int player, int loc, int seq);  // SelectPlace: pick a zone
    void respondUnselect(int index);        // SelectUnselect: pick one card
    void respondMultipleCards(const std::vector<int>& indices); // SelectCard N

    // Append a UI-emitted line to the duel log (for "opened GY viewer" etc.).
    void logEvent(const std::string& s) { addLog(s); }
    // Drop every line in the duel log (UI "Clear log" button).
    void clearLog() { m_log.clear(); m_lastReqLog.clear(); }

    // Return the card codes currently in `player`'s Extra Deck. Used by the
    // Extra Deck viewer in the UI. Hidden-info rule: callers should only
    // expose this for the local player's own Extra Deck.
    std::vector<uint32_t> extraDeckCodes(int player) {
        return queryLocationCodes((uint8_t)player, 0x40 /*LOC_EXTRA*/);
    }

    // Local/offline test mode: player 1 is auto-played by a built-in test AI.
    void setLocalMode(bool on)     { m_localMode = on; }
    bool localMode() const         { return m_localMode; }
    // Verbose ocgcore message/response logging into the duel log.
    void setDebugMessages(bool on) { m_debugMsgs = on; }

    // Recording hook — invoked from submitResponse() with the raw bytes that
    // just landed on the engine. Purely passive; never alters the response
    // path. UI sets this on duel start to capture replays.
    using ResponseRecorder = std::function<void(const void* data, uint32_t len)>;
    void setResponseRecorder(ResponseRecorder cb) { m_responseRecorder = std::move(cb); }
    // The 64-bit seed mixed into ocgcore's OCG_DuelOptions.seed[*]. Captured
    // once per startDuel() so replays can stamp it. 0 means "not started".
    uint64_t duelSeed() const { return m_duelSeed; }

    // Replay playback support — set immediately before startDuel() to make
    // the engine use exactly this seed (replays need it for determinism).
    // One-shot: consumed and cleared by startDuel(). Leaves live duel
    // randomisation untouched when not set.
    void setForcedSeed(uint64_t seed) {
        m_forcedSeed = seed;
        m_forcedSeedValid = true;
    }
    bool forcedSeedPending() const { return m_forcedSeedValid; }

    // Multiplayer support — disables the engine's internal auto-resolve
    // shortcuts (e.g. 0-option chain auto-pass) so BOTH peers wait for an
    // explicit response from the prompt owner. Without this, both peers'
    // DuelManagers auto-resolve the same prompt and the client double-
    // broadcasts an EngineResponse the host has already processed,
    // throwing the response queue out of sync. Live offline duels leave
    // this false so the auto-pass UX stays as-is.
    void setSuppressAutoResolve(bool on) { m_suppressAutoResolve = on; }
    bool suppressAutoResolve() const { return m_suppressAutoResolve; }

    // Per-phase presentation pacing. When > 0, process() holds the engine for
    // `sec` seconds after each phase transition so the player can SEE every
    // phase (Draw / Standby / Main / Battle / End) and isn't blown past
    // end-of-phase effect windows. Pure timing — it NEVER changes engine
    // state, responses or message order; it only delays WHEN the next
    // OCG_DuelProcess runs. The UI gates this to offline live duels (0 in
    // multiplayer / replay / rebuild). 0 = off (instant, as before).
    void setPhaseDelay(double sec) { m_phaseDelaySec = sec < 0.0 ? 0.0 : sec; }
    double phaseDelay() const { return m_phaseDelaySec; }

    // Engine seat the LOCAL human controls (0 or 1). The responder hands
    // prompts for THIS seat to the UI and routes the other seat to the
    // auto-AI (offline) or the remote peer (MP). A coin toss sets this so the
    // local player can go second (controlling team 1, since ocgcore always
    // gives the first turn to team 0). Default 0 — unchanged behaviour.
    void setHumanSeat(int s) { m_humanSeat = (s & 1); }
    int  humanSeat() const   { return m_humanSeat; }

    bool isRunning() const { return m_duel != nullptr && m_running; }
    bool isDone()    const { return m_done; }
    bool isBlocked() const { return m_blocked; }   // parked on an unsupported request

    // Duel result (valid once isDone()): winner() is 0/1, or -1/2 for a draw.
    int  winReason() const { return m_winReason; }
    static const char* winReasonText(int reason);

    // Surrender — end the duel immediately with `losingPlayer` (0/1) as the
    // loser. ocgcore has no surrender call, so this resolves the result at
    // the manager level: the duel stops and the Game-Over panel shows the
    // winner. A rematch starts a fresh engine, so leaving ocgcore parked
    // mid-state is fine. Safe to call any time the duel is running.
    void forfeit(int losingPlayer) {
        if (!m_running && !m_done && m_duel == nullptr) return;
        m_winner    = (losingPlayer == 0) ? 1 : 0;
        m_winReason = 4;                 // surrender
        m_done      = true;
        m_running   = false;
        addLog("Player " + std::to_string(losingPlayer + 1) +
               " surrendered — Player " + std::to_string(m_winner + 1) +
               " wins.");
        queryField();
    }
    // Human-readable name for a timing context (used by the log and UI).
    static const char* timingName(TimingContext t);

    // A "real" selection needs an actual decision (not None/Waiting).
    static bool isRealSelect(WaitType t) {
        return t != WaitType::None && t != WaitType::Waiting;
    }
    // True only when the engine genuinely awaits a decision from player p.
    bool awaitingPlayer(int p) const {
        return m_awaiting && isRealSelect(m_selection.type) &&
               m_selection.player == (uint8_t)p;
    }

    const FieldState&       field()      const { return m_field; }
    const SelectionRequest& selection()  const { return m_selection; }
    const std::vector<std::string>& log() const { return m_log; }
    // Most recent on-field action + who did it (0/1, or 0xFF if none).
    const std::string& lastActionDesc()   const { return m_lastActionDesc; }
    uint8_t            lastActionPlayer() const { return m_lastActionPlayer; }
    uint64_t           lastActionSeq()    const { return m_lastActionSeq; }

    // Pulls all MSG_ATTACK events the engine has reported since the last
    // call, then clears the queue. The UI animation layer drains this once
    // per frame to schedule attack beams / impact rings / SFX. Purely
    // presentational — does not influence engine response routing.
    std::vector<AttackEvent> drainAttackEvents() {
        std::vector<AttackEvent> out;
        out.swap(m_attackEvents);
        return out;
    }
    std::vector<MoveEvent> drainMoveEvents() {
        std::vector<MoveEvent> out;
        out.swap(m_moveEvents);
        return out;
    }
    int winner() const { return m_winner; }

private:
    CardDB&          m_db;
    SnapshotManager& m_snap;
    OCG_Duel         m_duel    = nullptr;
    bool             m_running = false;
    bool             m_done    = false;
    int              m_winner  = -1;
    int              m_winReason = -1;

    // ── Response / pending-request tracking ──────────────────────────────────
    bool        m_localMode      = true;   // P1 auto-played (offline/test mode)
    bool        m_debugMsgs      = false;  // verbose engine message logging
    bool        m_awaiting       = false;  // engine returned AWAITING, needs a response
    bool        m_responseQueued = false;  // a response is set, not yet consumed
    bool        m_blocked        = false;  // parked on an unsupported request
    int         m_retryCount     = 0;      // consecutive engine MSG_RETRY count
    std::string m_lastReqLog;              // de-dupes once-per-request log lines
    std::vector<std::string> m_missingScripts;  // de-dupes [SCRIPT MISSING] logs

    // ── Post-summon trigger-window trace ─────────────────────────────────────
    // A Summon raises EVENT_SUMMON_SUCCESS, which is the timing an on-summon
    // Trigger Effect (e.g. Vanquish Soul Razen's search) activates at. We arm
    // a trace when a Summon completes so the very next selection window is
    // logged against it — proving whether the trigger window actually opened.
    uint32_t    m_pendingSummonCode   = 0;  // card from the last MSG_*SUMMONING
    std::string m_pendingSummonName;
    int         m_pendingSummonType   = 0;  // 0 Normal, 1 Special, 2 Flip
    uint8_t     m_pendingSummonPlayer = 0;  // controller of that card
    bool        m_postSummonPending   = false;
    // The most recent on-field action (summon / activation / attack) and who
    // performed it. Surfaced in the UI so that when a response window opens, the
    // player can see what their opponent is attempting before deciding to chain.
    std::string m_lastActionDesc;
    uint8_t     m_lastActionPlayer    = 0xFF;   // 0xFF = none recorded
    uint64_t    m_lastActionSeq       = 0;      // bumps on every new action
    void setLastAction(uint8_t player, const std::string& desc) {
        m_lastActionPlayer = player & 1; m_lastActionDesc = desc;
        ++m_lastActionSeq;
    }
    uint32_t    m_traceSummonCode     = 0;
    std::string m_traceSummonName;
    int         m_traceSummonType     = 0;
    uint8_t     m_traceSummonPlayer   = 0;
    bool        m_postSummonChainSeen = false;
    bool        m_postSummonEffYnSeen = false;
    std::string m_postSummonMsgTrail;       // engine message names since summon

    FieldState       m_field;
    SelectionRequest m_selection;
    std::vector<std::string> m_log;
    // MSG_ATTACK events pushed by handleMsg() and drained by the UI animation
    // layer. Bounded by the message loop — at most a handful per turn.
    std::vector<AttackEvent> m_attackEvents;
    std::vector<MoveEvent>   m_moveEvents;
    // Replay-recording hook + the seed actually used for this duel.
    ResponseRecorder         m_responseRecorder;
    uint64_t                 m_duelSeed = 0;
    // Replay playback — when valid, startDuel will use m_forcedSeed instead
    // of generating one. Cleared after consumption so subsequent live duels
    // get fresh seeds normally.
    uint64_t                 m_forcedSeed       = 0;
    bool                     m_forcedSeedValid  = false;
    // (legacy) Multiplayer gate — older approach where MP routed every
    // response (including the 0-option chain auto-pass) through the UI.
    // Now that internal auto-resolve responses skip the recorder via
    // m_internalAutoResolve, this flag is functionally a no-op; kept so
    // external callers (and any future internal short-circuit that
    // genuinely needs UI ownership) still have an explicit knob.
    bool                     m_suppressAutoResolve = false;
    // One-shot guard for engine-internal auto-resolves (e.g. 0-option
    // chain auto-pass). When true on entry to submitResponse(), the
    // response recorder callback is skipped — so the response advances
    // ocgcore locally but is NOT broadcast to the MP peer nor mirrored
    // into the live replay's response stream. The peer's engine runs
    // the same deterministic path and auto-resolves identically.
    bool                     m_internalAutoResolve = false;

    // ── Per-phase pacing (presentation timing only) ───────────────────────
    double                   m_phaseDelaySec = 0.0;
    int                      m_humanSeat = 0;   // engine seat the human controls
    // AI per-turn guard: which (card,effect) the offline AI already activated
    // this turn, so it never re-activates the same effect into an endless loop.
    int                      m_aiTurnSeen = -1;
    std::vector<uint64_t>    m_aiDoneThisTurn;
    // Hard loop backstop: total AI responses this turn. If an effect/summon
    // loop ever spins, this forces the AI to bail out of the phase rather than
    // respond forever.
    int                      m_aiActionTurn      = -1;
    int                      m_aiActionsThisTurn = 0;
    std::chrono::steady_clock::time_point m_phaseHoldUntil{};
    // Set by the MSG_NEW_PHASE handler; consumed by the process() pump to
    // arm the hold once per phase change.
    bool                     m_phaseChangedThisProcess = false;

    // Last engine message frame seen, captured at the top of handleMsg().
    // When the engine awaits input on a message type the parser doesn't
    // cover (default switch case), these tell us exactly which MSG_*
    // needs a parser. Surfaced in WaitType::RawPrompt + the
    // [UNHANDLED ENGINE REQUEST] log line.
    uint8_t                       m_lastMsgType = 0;
    std::vector<uint8_t>          m_lastMsgPayload;

public:
    uint8_t                       lastMsgType()    const { return m_lastMsgType; }
    const std::vector<uint8_t>&   lastMsgPayload() const { return m_lastMsgPayload; }
private:

    void parseMessages(void* buf, uint32_t len);
    void handleMsg(const uint8_t*& p, const uint8_t* end);
    void queryField();
    void validateState();   // log [STATE ERROR] lines for bad field state
    // True if a card belongs in the Extra Deck (Fusion/Synchro/Xyz/Link), using
    // a hard-coded override table for cards missing from cards.cdb.
    bool isExtraDeckCard(uint32_t code) const;
    bool autoRespondP2();                 // auto-handle P2 selections in local mode
    // Heuristic AI for the offline opponent's Main / Battle phases. Returns
    // true once a response is submitted. See DuelManager.cpp for the strategy.
    bool aiIdlePhase();
    bool aiBattlePhase();
    void submitResponse(const void* data, uint32_t len);  // central response setter
    void logRequestOnce(const std::string& s);            // log once per distinct msg
    void addLog(const std::string& s);
    static std::string  msgName(uint8_t type);
    static const char*  waitName(WaitType t);

    // Decode an engine effect-description value into card code + string index
    // + the decoded effect text (looked up from the card database).
    EffectDesc decodeDesc(uint64_t raw) const;
    // Post-summon trace: arm it when a Summon resolves; flush body+end when the
    // next selection window (or a phase change) concludes it.
    void beginPostSummonTrace();
    void endPostSummonTrace(const char* concludedBy,
                            const std::vector<std::string>& optionLines,
                            const std::string& autoPassNote);
    // Query the card codes currently in one zone (debug diagnostics only).
    std::vector<uint32_t> queryLocationCodes(uint8_t con, uint32_t loc);
    // Debug-only: after a "Vanquish Soul" monster is summoned, enumerate the
    // controller's Deck and classify each card against Razen's on-summon
    // search filter (non-Warrior "Vanquish Soul" monster). Purely diagnostic
    // logging — it does NOT influence engine legality.
    void logRazenTargetCheck(uint8_t player);

    // Script reader (loads .lua card scripts from assets/scripts/)
    static int  scriptReaderCb(void* payload, OCG_Duel duel, const char* name);
    static void logHandlerCb(void* payload, const char* msg, int type);

    // Resolve a card's Lua script file ("c<code>.lua") to a path on disk, or
    // "" if it is missing — searches the same folders as scriptReaderCb.
    static std::string findCardScript(uint32_t code);
    // At duel start, log which deck cards have NO script (= no effects). A
    // card present in cards.cdb but absent from the script collection loads
    // as a vanilla body: it cannot register triggers, search effects, etc.
    void auditDeckScripts(const Deck& deck, int playerNo);
};
