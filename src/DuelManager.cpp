#include "DuelManager.h"
#include "../ocgcore/ocgapi_constants.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <random>
#include <chrono>
#include <algorithm>
#include <functional>
#include <climits>

static uint8_t  r8 (const uint8_t*& p) { return *p++; }
static uint16_t r16(const uint8_t*& p) { uint16_t v; memcpy(&v,p,2); p+=2; return v; }
static uint32_t r32(const uint8_t*& p) { uint32_t v; memcpy(&v,p,4); p+=4; return v; }
static uint64_t r64(const uint8_t*& p) { uint64_t v; memcpy(&v,p,8); p+=8; return v; }

// ── Engine pump tuning ───────────────────────────────────────────────────────
// kMaxStepsPerFrame bounds how many OCG_DuelProcess steps we advance per UI
// frame so the window stays responsive; unfinished work resumes next frame.
// kMaxRetries bounds consecutive MSG_RETRY before we park the duel instead of
// retrying forever.
static const int kMaxStepsPerFrame = 64;
static const int kMaxRetries       = 16;

DuelManager::DuelManager(CardDB& db, SnapshotManager& snap)
    : m_db(db), m_snap(snap) {}

DuelManager::~DuelManager() { endDuel(); }

void DuelManager::endDuel() {
    if (m_duel) { OCG_DestroyDuel(m_duel); m_duel = nullptr; }
    m_running        = false;
    m_done           = false;
    m_winner         = -1;
    m_winReason      = -1;
    m_awaiting       = false;
    m_responseQueued = false;
    m_blocked        = false;
    m_retryCount     = 0;
    m_selection      = {};
    m_lastReqLog.clear();
    m_missingScripts.clear();
    m_attackEvents.clear();
    m_moveEvents.clear();
    m_pendingSummonCode   = 0;
    m_pendingSummonName.clear();
    m_lastActionDesc.clear();
    m_lastActionPlayer    = 0xFF;
    m_postSummonPending   = false;
    m_postSummonChainSeen = false;
    m_postSummonEffYnSeen = false;
    m_postSummonMsgTrail.clear();
    m_snap.clear();
}

bool DuelManager::startDuel(const Deck& p0deck, const Deck& p1deck,
                             uint32_t lp, uint32_t handCount, uint32_t drawCount) {
    endDuel();
    m_field = {};   // clear stale board/turn/phase state from any prior duel

    // Seed strategy:
    //   * If a replay forced a seed via setForcedSeed(), consume it once
    //     so playback re-runs the duel deterministically.
    //   * Otherwise: mix the high-resolution clock with std::random_device
    //     so two duels started in the same millisecond still differ.
    // ocgcore shuffles each deck at OCG_StartDuel from this seed.
    uint64_t base;
    if (m_forcedSeedValid) {
        base = m_forcedSeed;
        m_forcedSeedValid = false;
        m_forcedSeed      = 0;
        addLog("[REPLAY] starting seeded duel  seed=" +
               std::to_string(base));
    } else {
        base = (uint64_t)std::chrono::high_resolution_clock::now()
                            .time_since_epoch().count();
        std::random_device rd;
        base ^= ((uint64_t)rd() << 32) ^ (uint64_t)rd();
    }
    std::mt19937_64 rng(base);
    // Capture the canonical seed used for this duel so replays can stamp
    // it.
    m_duelSeed = base;

    OCG_DuelOptions opts{};
    opts.seed[0]        = rng();
    opts.seed[1]        = rng();
    opts.seed[2]        = rng();
    opts.seed[3]        = rng();
    // Modern master-rule zone flags: Pendulum zones, the Extra Monster Zone
    // and the post-Link revision. flags=0 is a pre-2014 rule set under which
    // modern card scripts (e.g. recent hand traps) can misbehave. These names
    // come straight from ocgcore/ocgapi_constants.h. If a specific ruleset is
    // needed, adjust this combination.
    opts.flags = DUEL_PZONE | DUEL_EMZONE | DUEL_FSX_MMZONE |
                 DUEL_TRAP_MONSTERS_NOT_USE_ZONE | DUEL_TRIGGER_ONLY_IN_LOCATION;
    // "No shuffle" (custom duel setting / combo practice): the main deck plays
    // in registered order. Hands are still randomised by the engine.
    if (m_noShuffle) opts.flags |= DUEL_PSEUDO_SHUFFLE;
    addLog("=== Duel start ===");
    addLog("Duel seed: " + std::to_string(base));
    {
        char fb[24]; snprintf(fb, sizeof(fb), "0x%llx",
                              (unsigned long long)opts.flags);
        addLog(std::string("Duel rule flags: ") + fb +
               " (modern master-rule zones)");
    }
    addLog("Card DB: " + (m_db.dbPath().empty() ? std::string("(not open)")
                                                 : m_db.dbPath()));
    if (m_db.databaseCount() > 1)
        addLog("Card DB fallbacks: " + std::to_string(m_db.databaseCount() - 1) +
               " additional database(s) loaded");
    opts.team1.startingLP         = lp;
    opts.team1.startingDrawCount  = handCount;
    opts.team1.drawCountPerTurn   = drawCount;
    opts.team2 = opts.team1;
    opts.cardReader     = CardDB::cardReaderCb;
    opts.payload1       = &m_db;
    opts.cardReaderDone = CardDB::cardReaderDoneCb;
    opts.payload4       = &m_db;
    opts.scriptReader   = scriptReaderCb;
    opts.payload2       = this;
    opts.logHandler     = logHandlerCb;
    opts.payload3       = this;

    int status = OCG_CreateDuel(&m_duel, &opts);
    if (status != OCG_DUEL_CREATION_SUCCESS) {
        addLog("ERROR: OCG_CreateDuel failed (code " + std::to_string(status) + ")");
        return false;
    }

    loadInitScripts();

    // Load a deck:
    //  (1) Route every card by its REAL card type. Fusion/Synchro/Xyz/Link
    //      monsters always go to the Extra Deck and can never be drawn. A card
    //      missing from cards.cdb (type 0) trusts its .ydk section.
    //  (2) SHUFFLE the main deck before registering it. ocgcore's Startup
    //      process (processor.cpp) draws the opening hand straight off
    //      list_main and does NOT shuffle it — it expects the caller to hand
    //      over a pre-shuffled deck. Without this, the seed changes but the
    //      opening hand is identical every duel. The Extra Deck is never mixed
    //      into the main deck.
    auto loadDeck = [&](const Deck& deck, uint8_t player) {
        std::vector<uint32_t> mainCards, extraCards;
        auto classify = [&](uint32_t code, bool fromExtraSection) {
            CardInfo ci    = m_db.getCard(code);
            bool     known = (ci.id != 0);
            // Routing priority: (1) Extra-Deck type / override -> EXTRA;
            // (2) known non-Extra card -> MAIN; (3) unknown -> trust the .ydk
            // section. An unknown card listed under #extra therefore can NEVER
            // reach the Main Deck.
            bool toExtra;
            if (isExtraDeckCard(code)) toExtra = true;
            else if (known)            toExtra = false;
            else                       toExtra = fromExtraSection;
            if (toExtra) extraCards.push_back(code);
            else         mainCards.push_back(code);
            if (!known)
                addLog("[warn] card #" + std::to_string(code) +
                       " NOT in cards.cdb (.ydk section=" +
                       std::string(fromExtraSection ? "EXTRA" : "MAIN") +
                       ") -> routed " + (toExtra ? "EXTRA" : "MAIN"));
            else if (m_debugMsgs) {
                char tb[16]; snprintf(tb, sizeof(tb), "0x%x", (unsigned)ci.type);
                addLog("[deck] P" + std::to_string(player + 1) + " #" +
                       std::to_string(code) + " [" + ci.name + "] section=" +
                       (fromExtraSection ? "EXTRA" : "MAIN") + " type=" + tb +
                       " -> routed " + (toExtra ? "EXTRA" : "MAIN"));
            }
        };
        for (uint32_t c : deck.main)  classify(c, false);
        for (uint32_t c : deck.extra) classify(c, true);

        auto orderStr = [](const std::vector<uint32_t>& v) {
            std::string s;
            for (size_t i = 0; i < v.size() && i < 12; ++i)
                s += std::to_string(v[i]) + " ";
            if (v.size() > 12) s += "...";
            return s;
        };
        if (m_debugMsgs)
            addLog("[deck] P" + std::to_string(player+1) +
                   " main BEFORE shuffle: " + orderStr(mainCards));
        std::shuffle(mainCards.begin(), mainCards.end(), rng);   // <-- the fix
        if (m_debugMsgs)
            addLog("[deck] P" + std::to_string(player+1) +
                   " main AFTER  shuffle: " + orderStr(mainCards));

        auto reg = [&](uint32_t code, uint32_t loc, uint32_t seq) {
            OCG_NewCardInfo info{};
            info.team = player; info.duelist = 0; info.code = code;
            info.con  = player; info.loc = loc; info.seq = seq;
            info.pos  = POS_FACEDOWN_DEFENSE;
            OCG_DuelNewCard(m_duel, &info);
        };
        for (uint32_t i = 0; i < mainCards.size();  ++i) reg(mainCards[i],  LOCATION_DECK,  i);
        for (uint32_t i = 0; i < extraCards.size(); ++i) reg(extraCards[i], LOCATION_EXTRA, i);

        m_field.deckCount[player]  = (int)mainCards.size();
        m_field.extraCount[player] = (int)extraCards.size();
        addLog("Deck P" + std::to_string(player + 1) + ": main=" +
               std::to_string(mainCards.size()) + " extra=" +
               std::to_string(extraCards.size()) + " (shuffled)");
    };
    loadDeck(p0deck, 0);
    loadDeck(p1deck, 1);
    if (m_field.deckCount[0] == 0 || m_field.deckCount[1] == 0)
        addLog("[warn] a main deck is empty — duel may end immediately");

    // Flag any deck card whose script is missing — such a card has no effects
    // at all, so its on-summon triggers can never appear.
    auditDeckScripts(p0deck, 1);
    auditDeckScripts(p1deck, 2);

    m_field.lp[0] = m_field.lp[1] = lp;

    OCG_StartDuel(m_duel);
    m_running = true;
    addLog("Duel started!");
    return true;
}

// Load ocgcore's standard init/proc Lua scripts into the freshly created duel.
void DuelManager::loadInitScripts() {
    static const char* kInitScripts[] = {
        "constant.lua", "utility.lua",
        "archetype_setcode_constants.lua", "card_counter_constants.lua",
        "cards_specific_functions.lua", "debug_utility.lua",
        "deprecated_functions.lua", "proc_equip.lua", "proc_fusion.lua",
        "proc_fusion_spell.lua", "proc_gemini.lua", "proc_link.lua",
        "proc_maximum.lua", "proc_normal.lua", "proc_pendulum.lua",
        "proc_persistent.lua", "proc_ritual.lua", "proc_rush.lua",
        "proc_skill.lua", "proc_spirit.lua", "proc_synchro.lua",
        "proc_union.lua", "proc_workaround.lua", "proc_xyz.lua", nullptr
    };
    for (int i = 0; kInitScripts[i]; ++i) {
        std::string path = std::string("assets/scripts/") + kInitScripts[i];
        std::ifstream f(path, std::ios::binary);
        if (!f) { addLog("WARN: missing init script: " + path); continue; }
        f.seekg(0, std::ios::end);
        auto sz = (uint32_t)f.tellg(); f.seekg(0);
        std::string buf(sz, '\0'); f.read(&buf[0], sz);
        int r = OCG_LoadScript(m_duel, buf.c_str(), sz, kInitScripts[i]);
        if (!r) addLog(std::string("WARN: script failed: ") + kInitScripts[i]);
    }
}

// ── Puzzle / Challenge mode ──────────────────────────────────────────────────
// Build a preset board: place each card directly into its requested zone, set
// each player's LP, then start. No opening hand is drawn (startingDrawCount=0)
// — the hand is whatever the puzzle places. The opponent is left passive so the
// player can work the board uninterrupted. Deck order is fixed (PSEUDO_SHUFFLE)
// so a puzzle always presents the same draws if the solution needs them.
bool DuelManager::startPuzzle(const PuzzleSetup& pz) {
    endDuel();
    m_field = {};

    uint64_t base = (uint64_t)std::chrono::high_resolution_clock::now()
                        .time_since_epoch().count();
    std::mt19937_64 rng(base);
    m_duelSeed = base;

    OCG_DuelOptions opts{};
    opts.seed[0] = rng(); opts.seed[1] = rng();
    opts.seed[2] = rng(); opts.seed[3] = rng();
    opts.flags = DUEL_PZONE | DUEL_EMZONE | DUEL_FSX_MMZONE |
                 DUEL_TRAP_MONSTERS_NOT_USE_ZONE | DUEL_TRIGGER_ONLY_IN_LOCATION |
                 DUEL_PSEUDO_SHUFFLE;
    addLog("=== Puzzle start: " + pz.name + " ===");
    opts.team1.startingLP        = pz.lpYou;
    opts.team1.startingDrawCount = 0;          // hand is placed, not drawn
    opts.team1.drawCountPerTurn  = 1;
    opts.team2 = opts.team1;
    opts.team2.startingLP        = pz.lpOpp;
    opts.cardReader     = CardDB::cardReaderCb;     opts.payload1 = &m_db;
    opts.cardReaderDone = CardDB::cardReaderDoneCb; opts.payload4 = &m_db;
    opts.scriptReader   = scriptReaderCb;           opts.payload2 = this;
    opts.logHandler     = logHandlerCb;             opts.payload3 = this;

    int status = OCG_CreateDuel(&m_duel, &opts);
    if (status != OCG_DUEL_CREATION_SUCCESS) {
        addLog("ERROR: OCG_CreateDuel failed (puzzle)");
        return false;
    }
    loadInitScripts();

    auto reg = [&](uint32_t code, uint8_t player, uint32_t loc,
                   uint32_t seq, uint32_t pos) {
        OCG_NewCardInfo info{};
        info.team = player; info.duelist = 0; info.code = code;
        info.con  = player; info.loc = loc; info.seq = seq; info.pos = pos;
        OCG_DuelNewCard(m_duel, &info);
    };

    // Track per-zone sequence counters so multiple cards in the same
    // location (hand, GY, deck...) get distinct slots.
    int handSeq[2] = {0, 0}, graveSeq[2] = {0, 0}, banishSeq[2] = {0, 0};
    int deckSeq[2] = {0, 0}, extraSeq[2] = {0, 0};
    int mField[2] = {0, 0}, sField[2] = {0, 0};   // routed counters
    for (const auto& c : pz.cards) {
        uint8_t pl = c.player & 1;
        uint32_t pos = c.pos ? c.pos : POS_FACEUP_ATTACK;
        uint32_t seq = c.seq;
        switch (c.loc) {
            case LOCATION_HAND:    seq = handSeq[pl]++;
                                   pos = POS_FACEDOWN; break;
            case LOCATION_GRAVE:   seq = graveSeq[pl]++;
                                   pos = POS_FACEUP; break;
            case LOCATION_REMOVED: seq = banishSeq[pl]++; break;
            case LOCATION_DECK:    seq = deckSeq[pl]++;
                                   pos = POS_FACEDOWN_DEFENSE; break;
            case LOCATION_EXTRA:   seq = extraSeq[pl]++;
                                   pos = POS_FACEDOWN_DEFENSE; break;
            case LOCATION_MZONE:   if (!c.seq) seq = mField[pl];
                                   mField[pl] = (int)seq + 1; break;
            case LOCATION_SZONE:   if (!c.seq) seq = sField[pl];
                                   sField[pl] = (int)seq + 1; break;
            default: break;
        }
        reg(c.code, pl, c.loc, seq, pos);
    }
    // Filler deck so neither side is at 0 cards (avoids instant deckout loss).
    for (uint32_t code : pz.deckYou) reg(code, 0, LOCATION_DECK, deckSeq[0]++,
                                         POS_FACEDOWN_DEFENSE);
    for (uint32_t code : pz.deckOpp) reg(code, 1, LOCATION_DECK, deckSeq[1]++,
                                         POS_FACEDOWN_DEFENSE);

    m_field.deckCount[0] = deckSeq[0];  m_field.deckCount[1] = deckSeq[1];
    m_field.extraCount[0] = extraSeq[0]; m_field.extraCount[1] = extraSeq[1];
    m_field.lp[0] = pz.lpYou; m_field.lp[1] = pz.lpOpp;

    OCG_StartDuel(m_duel);
    m_running = true;
    addLog("Puzzle ready — goal: " + pz.goal);
    return true;
}

// Advance the duel engine. Called once per UI frame.
//
// ocgcore protocol: OCG_DuelProcess() returns END / AWAITING / CONTINUE.
//   - AWAITING : the engine needs a response; the last parsed message says who
//                and what. We must NOT call OCG_DuelProcess again until a valid
//                response has been set, or the engine emits MSG_RETRY forever.
//   - CONTINUE : the engine advanced on its own; keep pumping.
//   - END      : the duel is over.
// Flow control is driven entirely by this status plus the pending-request
// state, so a parsing miss can never turn into an every-frame retry storm.
bool DuelManager::process() {
    if (!m_duel || !m_running) return false;
    if (m_done) { m_running = false; return false; }

    // ── Snapshot rewind: replay every recorded response from T=0 ─────────────
    if (!m_snap.pendingResponses().empty()) {
        m_blocked = false;   // replaying a rewound timeline — clear any park
        for (auto& resp : m_snap.pendingResponses()) {
            OCG_DuelSetResponse(m_duel, resp.data(), (uint32_t)resp.size());
            int s = OCG_DuelProcess(m_duel);
            uint32_t l = 0; void* b = OCG_DuelGetMessage(m_duel, &l);
            if (b && l) parseMessages(b, l);
            if (s == OCG_DUEL_STATUS_END) {
                m_done = true; m_running = false; queryField(); return false;
            }
        }
        m_snap.clearPending();
        m_selection      = {};
        m_awaiting       = false;
        m_responseQueued = false;
        m_retryCount     = 0;
        queryField();
    }

    if (m_blocked) return true;   // parked on an unsupported request — see log

    // Pacing hold: a phase change or AI combo beat parked the pump below; keep
    // holding until the armed deadline elapses. Honour ANY armed hold (not only
    // when the phase-pacing slider is up) so AI pacing works even if phase
    // pacing is turned down. This only gates WHEN we advance — engine state and
    // responses are untouched.
    if (std::chrono::steady_clock::now() < m_phaseHoldUntil) {
        return true;
    }

    bool sawMessages = false;

    // Bounded pump: advance until the engine needs a P0 decision, the duel
    // ends, or we hit the per-frame step cap (work resumes next frame).
    for (int step = 0; step < kMaxStepsPerFrame; ++step) {

        // The engine is awaiting input we have not produced yet — decide who
        // answers before touching OCG_DuelProcess again.
        if (m_awaiting && !m_responseQueued) {
            if (isRealSelect(m_selection.type)) {
                if (m_selection.player == m_humanSeat) {
                    // Genuine local-player turn — hand control to the UI.
                    if (sawMessages) queryField();
                    return true;
                }
                // The OTHER seat (opponent).
                if (!m_localMode) {
                    // A real remote opponent: genuinely waiting on them.
                    if (sawMessages) queryField();
                    return true;
                }
                if (m_retryCount > kMaxRetries) {
                    m_blocked = true;
                    logRequestOnce("[error] auto-AI response repeatedly rejected "
                                   "by engine — duel paused");
                    if (sawMessages) queryField();
                    return true;
                }
                if (!autoRespondP2()) {
                    m_blocked = true;
                    logRequestOnce("[error] no auto-AI response available — duel paused");
                    if (sawMessages) queryField();
                    return true;
                }
                // Pace the AI so its turn is watchable. Main decisions (Summon /
                // Attack) get the full beat; combo sub-steps (effect choices,
                // chains, material picks) get a shorter beat so each card hitting
                // the board reads one-at-a-time instead of bursting all at once.
                // (The old loop that made paced sub-steps drag to ~110s is fixed,
                // so light pacing here is safe.) Honours the pacing switch; Fast
                // turns disables it.
                double aiHold = 0.0;
                switch (m_selection.type) {
                    case WaitType::SelectIdleCmd:
                    case WaitType::SelectBattleCmd:
                        aiHold = m_phaseDelaySec < 0.5 ? m_phaseDelaySec : 0.5;
                        break;
                    case WaitType::SelectEffectYn:
                    case WaitType::SelectYesNo:
                    case WaitType::SelectChain:
                    case WaitType::SelectOption:
                    case WaitType::SelectCard:
                    case WaitType::SelectTribute:
                    case WaitType::SelectUnselect:
                        aiHold = m_phaseDelaySec < 0.28 ? m_phaseDelaySec : 0.28;
                        break;
                    default: break;
                }
                if (m_phaseDelaySec > 0.0 && aiHold > 0.0) {
                    m_phaseHoldUntil = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(aiHold));
                    if (sawMessages) queryField();
                    return true;
                }
            } else {
                // AWAITING but no recognised selection was parsed — a request
                // type the simulator does not implement. Pause cleanly rather
                // than feeding the engine a guessed response: a bad guess can
                // corrupt duel state or hand the game away.
                m_blocked = true;
                // Surface enough info to diagnose / implement the missing
                // parser. m_lastMsgType + m_lastMsgPayload are captured at
                // the top of handleMsg() so they correspond to the very
                // message that triggered the block.
                std::string hex;
                for (size_t i = 0; i < m_lastMsgPayload.size(); ++i) {
                    char buf[4];
                    snprintf(buf, sizeof(buf), "%02x", m_lastMsgPayload[i]);
                    if (i) hex += " ";
                    hex += buf;
                }
                // Best-effort player byte: most SELECT_* messages place the
                // target player as the byte right after the type id.
                int rawPlayer = -1;
                if (m_lastMsgPayload.size() >= 2)
                    rawPlayer = m_lastMsgPayload[1] & 1;
                addLog(std::string("[UNHANDLED ENGINE REQUEST]")
                       + " msg=" + std::to_string((int)m_lastMsgType)
                       + " msgName=" + msgName(m_lastMsgType)
                       + " player=" + (rawPlayer < 0 ? std::string("?")
                                                     : std::to_string(rawPlayer))
                       + " rawLen=" + std::to_string(m_lastMsgPayload.size())
                       + " rawHex=" + hex);
                // Preserve a RawPrompt state so the UI (especially MP
                // gating + the diagnostic modal) can still inspect what
                // the engine was asking about. m_selection.player gets
                // the best-effort player byte; consumers should treat
                // it as "ownerKnown=false" when type==RawPrompt.
                m_selection = {};
                m_selection.type   = WaitType::RawPrompt;
                m_selection.player = (uint8_t)(rawPlayer < 0 ? 0 : rawPlayer);
                logRequestOnce("[error] unhandled engine request — duel paused "
                               "(this selection type is not implemented yet)");
                if (sawMessages) queryField();
                return true;
            }
        }

        // A response is ready (queued by the UI, autoRespondP2, or an inline
        // handler). It is already set in the engine via OCG_DuelSetResponse;
        // the next OCG_DuelProcess call consumes it.
        if (m_responseQueued) {
            m_responseQueued = false;
            m_awaiting       = false;
        }

        int status = OCG_DuelProcess(m_duel);

        uint32_t msgLen = 0;
        void* msgBuf = OCG_DuelGetMessage(m_duel, &msgLen);
        if (msgBuf && msgLen > 0) { parseMessages(msgBuf, msgLen); sawMessages = true; }

        // A Summon landed or an effect started this step — arm a short "combo
        // beat" and YIELD so a cascade of Special Summons plays out one card at
        // a time instead of the whole combo snapping onto the board at once.
        // This is what actually slows the AI's combos down to watchable speed
        // (the per-decision holds don't, because the cascade resolves between
        // decision points). Skipped under Fast turns / testing rebuild (delay 0).
        if (m_aiComboBeat > 0.0 && m_pacedEventThisProcess && !m_done) {
            m_pacedEventThisProcess = false;
            double beat = m_aiComboBeat;
            m_phaseHoldUntil = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(beat));
            queryField();
            return true;
        }

        // A phase changed in this step — arm the pacing hold and YIELD so the
        // player sees the new phase (and any end-of-phase prompt) before the
        // engine races on. Resumes automatically once the hold elapses.
        if (m_phaseDelaySec > 0.0 && m_phaseChangedThisProcess && !m_done) {
            m_phaseChangedThisProcess = false;
            m_phaseHoldUntil = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(m_phaseDelaySec));
            queryField();
            return true;
        }

        // MSG_WIN inside the buffer sets m_done — stop the pump immediately,
        // before any further OCG_DuelProcess / autoresponse, even if the
        // engine status has not yet flipped to END.
        if (m_done) {
            m_running = false;
            if (m_debugMsgs) addLog("[dbg] process() stop: duel ended");
            queryField();
            return false;
        }

        if (status == OCG_DUEL_STATUS_END) {
            m_done = true; m_running = false;
            if (m_debugMsgs) addLog("[dbg] process() stop: engine returned END");
            queryField();
            return false;
        }
        if (status == OCG_DUEL_STATUS_AWAITING) {
            m_awaiting = true;
            continue;   // loop back: the top of the loop picks the responder
        }
        // OCG_DUEL_STATUS_CONTINUE — the engine made progress unassisted.
        m_awaiting   = false;
        m_retryCount = 0;
        m_lastReqLog.clear();
    }

    if (sawMessages) queryField();
    return true;
}

// Central response setter. Every response — from the UI, the test AI, or an
// inline message handler — goes through here so it is recorded for snapshot
// rewind and the pending-request state stays consistent.
void DuelManager::submitResponse(const void* data, uint32_t len) {
    if (!m_duel) return;
    m_snap.recordResponse(data, len);
    // Replay/MP recorder fan-out — invoked BEFORE the engine actually
    // receives the response, so the recorder sees exactly what gets fed
    // to ocgcore. SKIPPED when m_internalAutoResolve is set: that's the
    // engine's own auto-resolve path (e.g. 0-option chain auto-pass),
    // which fires deterministically on both peers in MP. Broadcasting it
    // would deliver a duplicate response to the peer's engine that's
    // already consumed its local auto-pass — the historical desync.
    if (m_responseRecorder && !m_internalAutoResolve) {
        try { m_responseRecorder(data, len); }
        catch (...) { /* never let a recorder bug break a duel */ }
    }
    m_internalAutoResolve = false;   // one-shot
    OCG_DuelSetResponse(m_duel, data, len);
    m_responseQueued = true;
    // Any response unblocks a parked duel. The block flag is the engine's
    // "I asked for input I couldn't get" parking brake — once we've handed
    // input back to ocgcore, we're no longer parked. Critical for replay
    // playback, where the feeder needs to be able to drive the engine
    // through prompt types the live UI hasn't implemented.
    m_blocked = false;
    if (m_debugMsgs) {
        char b[64];
        snprintf(b, sizeof(b), "[dbg] response set (%u bytes)", len);
        addLog(b);
    }
    m_selection = {};
}

void DuelManager::respond(const void* data, uint32_t len) {
    submitResponse(data, len);
}

void DuelManager::respondInt(int value)        { int v = value; submitResponse(&v, sizeof(int)); }
void DuelManager::respondYesNo(bool yes)       { int v = yes?1:0; submitResponse(&v, sizeof(int)); }
void DuelManager::respondChain(int index)      { int32_t v = index; submitResponse(&v, 4); }

// SelectPlace response: 3 bytes { player, location, sequence }.
void DuelManager::respondPlace(int player, int loc, int seq) {
    uint8_t resp[3] = { (uint8_t)player, (uint8_t)loc, (uint8_t)seq };
    addLog("[PLACE RESPONSE] player=P" + std::to_string(player + 1) +
           " loc=" + std::to_string(loc) +
           " seq=" + std::to_string(seq) +
           "  bytes=[" + std::to_string(resp[0]) + "," +
           std::to_string(resp[1]) + "," + std::to_string(resp[2]) + "]");
    submitResponse(resp, 3);
}

// SelectUnselect response: pick one card -> { mode=1, index }. (Finish/cancel
// is respondInt(-1), a plain 4-byte -1.) The engine re-asks until min/max met.
void DuelManager::respondUnselect(int index) {
    int32_t buf[2] = { 1, (int32_t)index };
    submitResponse(buf, 8);
}

void DuelManager::respondSingleCard(int index) {
    uint8_t buf[12];
    int32_t  type = 0; uint32_t cnt = 1; uint32_t idx = (uint32_t)index;
    memcpy(buf,   &type, 4); memcpy(buf+4, &cnt, 4); memcpy(buf+8, &idx, 4);
    respond(buf, 12);
}

// MSG_SELECT_CARD multi-pick response: { int32 type=0, uint32 count, uint32
// indices[count] } in the order the engine listed them. Sent in one shot so
// the engine consumes a single response (no double-click duplication).
void DuelManager::respondMultipleCards(const std::vector<int>& indices) {
    std::vector<uint8_t> buf(8 + 4u * indices.size());
    int32_t  type = 0;
    uint32_t cnt  = (uint32_t)indices.size();
    memcpy(buf.data(),     &type, 4);
    memcpy(buf.data() + 4, &cnt,  4);
    for (size_t i = 0; i < indices.size(); ++i) {
        uint32_t idx = (uint32_t)indices[i];
        memcpy(buf.data() + 8 + 4u * i, &idx, 4);
    }
    respond(buf.data(), (uint32_t)buf.size());
}

void DuelManager::respondIdleCmd(int t, int s) {
    int32_t v = (int32_t)(((uint32_t)s << 16) | ((uint32_t)t & 0xffff));
    respond(&v, 4);
}

// OCG_DuelGetMessage returns a buffer of FRAMED messages:
//
//     [uint32 size][message bytes...][uint32 size][message bytes...] ...
//
// The first byte of each "message bytes" block is the 1-byte message type
// (see ocgcore duel::generate_buffer / duel_message). The previous version of
// this function skipped the 4-byte size prefix entirely and read the low byte
// of `size` as the message type — desyncing the cursor on the very first
// message and turning the whole stream into garbage. That is why the engine
// never saw a real MSG_SELECT_IDLECMD, the duel hung, and MSG_RETRY spammed
// the log every frame.
//
// We now honour the size prefix and ALWAYS resync the cursor to the next
// frame boundary, so even a wrong byte count inside a single handleMsg() can
// never cascade into the rest of the stream.
void DuelManager::parseMessages(void* buf, uint32_t len) {
    const uint8_t* p   = static_cast<const uint8_t*>(buf);
    const uint8_t* end = p + len;
    while (p + 4 <= end) {
        uint32_t msgSize;
        memcpy(&msgSize, p, 4);
        p += 4;
        if (msgSize == 0) continue;
        const uint8_t* msgEnd = p + msgSize;
        if (msgEnd > end) {                       // truncated / corrupt frame
            addLog("[warn] truncated engine message frame — skipped");
            break;
        }
        const uint8_t* mp = p;                    // cursor handed to handleMsg
        handleMsg(mp, msgEnd);
        p = msgEnd;                               // resync to the frame boundary
    }
}

namespace {
// ── MSG_SELECT_SUM solver ────────────────────────────────────────────────────
// Ritual tributes, "send monsters whose Levels total N" costs, and similar use
// MSG_SELECT_SUM: pick a subset of the offered cards whose summed values hit an
// exact target (mode 0) or just cover it (mode 1). Each card carries a packed
// param: low 16 bits = value A, high 16 bits = value B (0 = single-valued).
//
// sumReachable mirrors ocgcore's select_sum_check1 exactly so a subset we pass
// the validity test here is accepted by the engine without a retry.
bool sumReachable(const std::vector<uint32_t>& params, size_t idx, int acc) {
    if (acc == 0 || idx == params.size()) return false;
    int o1 = (int)(params[idx] & 0xffff);
    int o2 = (int)(params[idx] >> 16);
    if (idx == params.size() - 1) return acc == o1 || acc == o2;
    return (acc > o1 && sumReachable(params, idx + 1, acc - o1)) ||
           (o2 > 0 && acc > o2 && sumReachable(params, idx + 1, acc - o2));
}

// Find a subset of `opt` (count in [minc,maxc]) so that must ++ subset satisfies
// the engine's sum rule. Returns the chosen indices into `opt`. Bounded DFS so a
// pathological field can't hang the search.
bool solveSelectSum(const std::vector<uint32_t>& must,
                    const std::vector<uint32_t>& opt,
                    int acc, int minc, int maxc, int mode,
                    std::vector<int>& outPick) {
    const int n = (int)opt.size();
    if (maxc <= 0) maxc = n;                 // mode 1 / unbounded count
    if (maxc > n) maxc = n;
    std::vector<int> cur;
    int budget = 300000;
    std::function<bool(int)> dfs = [&](int start) -> bool {
        if (--budget < 0) return false;
        const int cnt = (int)cur.size();
        if (cnt >= minc && cnt <= maxc) {
            std::vector<uint32_t> all = must;
            for (int k : cur) all.push_back(opt[k]);
            if (mode == 0) {
                if (sumReachable(all, 0, acc)) { outPick = cur; return true; }
            } else {
                // mode 1 (ocgcore: valid iff maxsum >= acc and minsum-min < acc).
                int sumMin = 0, mx = 0, mn = INT_MAX;
                for (uint32_t pr : all) {
                    int o1 = (int)(pr & 0xffff), o2 = (int)(pr >> 16);
                    int ms = (o2 && o2 < o1) ? o2 : o1;
                    sumMin += ms; mx += (o2 > o1) ? o2 : o1;
                    if (ms < mn) mn = ms;
                }
                if (!all.empty() && mx >= acc && sumMin - mn < acc) {
                    outPick = cur; return true;
                }
            }
        }
        if (cnt >= maxc) return false;
        for (int j = start; j < n; ++j) {
            cur.push_back(j);
            if (dfs(j + 1)) return true;
            cur.pop_back();
        }
        return false;
    };
    return dfs(0);
}
} // namespace

void DuelManager::handleMsg(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return;
    // Capture the raw frame (type byte + up to 63 bytes of payload) for
    // diagnostics — if the engine later awaits input on a message we
    // don't recognise, this is what we'll dump in the
    // [UNHANDLED ENGINE REQUEST] log so a parser gap can be diagnosed.
    {
        const uint8_t* frameStart = p;
        size_t avail = std::min<size_t>(
            (size_t)(end - frameStart), (size_t)64);
        m_lastMsgPayload.assign(frameStart, frameStart + avail);
        m_lastMsgType = frameStart[0];
    }
    uint8_t type = r8(p);
    if (m_debugMsgs) addLog("[dbg] " + msgName(type));
    // While a post-summon trace is armed, record every engine message that
    // arrives so the trace can show exactly what ocgcore did after the Summon.
    if (m_postSummonPending) {
        if (!m_postSummonMsgTrail.empty()) m_postSummonMsgTrail += ", ";
        m_postSummonMsgTrail += msgName(type);
    }
    switch (type) {

    case MSG_START:
        r8(p); m_field.lp[0]=r32(p); m_field.lp[1]=r32(p);
        r16(p); r16(p); r16(p); r16(p);
        addLog("Duel begins!");
        break;

    case MSG_WIN: {
        // MSG_WIN: player(u8), reason(u8). player 0/1 = winner; anything else
        // (PLAYER_NONE) = draw.
        m_winner    = r8(p);
        m_winReason = r8(p);
        if (m_winner != 0 && m_winner != 1)
            addLog("Duel ended in a draw");
        else
            addLog("Player " + std::to_string(m_winner + 1) + " wins! (" +
                   winReasonText(m_winReason) + ")");
        addLog("[Win] winner=" + std::to_string(m_winner) +
               " reason=" + std::to_string(m_winReason) +
               " turn=" + std::to_string(m_field.turnCount));
        m_done = true;   // process() halts the pump as soon as it sees this
        break;
    }

    case MSG_NEW_TURN:
        if (m_postSummonPending)
            endPostSummonTrace("MSG_NEW_TURN", {}, "n/a (turn changed)");
        m_field.turnPlayer = r8(p)&1; m_field.turnCount++;
        // Drop the captured "last opponent action" so it can never bleed into a
        // new turn's response prompts (e.g. stale "Activating Fuwalos").
        m_lastActionDesc.clear(); m_lastActionPlayer = 0xFF;
        m_chainSourceCode = 0;   // don't let a prior turn's effect text linger
        addLog("Turn "+std::to_string(m_field.turnCount)+" - Player "+std::to_string(m_field.turnPlayer+1));
        m_snap.save("Turn "+std::to_string(m_field.turnCount), m_field.turnCount, m_field.phase);
        break;

    case MSG_NEW_PHASE: {
        if (m_postSummonPending)
            endPostSummonTrace("MSG_NEW_PHASE", {}, "n/a (phase changed)");
        m_field.phase = r16(p);
        m_phaseChangedThisProcess = true;   // arm the per-phase pacing hold
        static const char* names[]={"Draw","Standby","Main 1","Battle","","","","","Main 2","","End"};
        int pi=0; uint16_t ph=m_field.phase; while(ph>1){ph>>=1;pi++;}
        if(pi<11) addLog(std::string(names[pi])+" Phase");
        break;
    }

    case MSG_DRAW: {
        uint8_t pl=r8(p)&1; uint32_t cnt=r32(p);
        addLog("Player "+std::to_string(pl+1)+" draws "+std::to_string(cnt));
        for(uint32_t i=0;i<cnt;i++){
            uint32_t code=r32(p); r32(p);   // code, position/flag
            CardInfo ci = m_db.getCard(code);
            if (m_debugMsgs)
                addLog("[dbg] draw P"+std::to_string(pl+1)+" #"+std::to_string(code)+
                       " ["+(ci.name.empty()?"unknown":ci.name)+"]");
            // A normal draw must never yield an Extra-Deck card. isExtraDeckCard
            // also consults the override table, so cards missing from cards.cdb
            // are caught. This is the precise "from normal draw" check, so it
            // pauses the duel (validateState only warns, to avoid false pauses
            // on legitimate effects that move Extra cards to the hand).
            if (isExtraDeckCard(code)) {
                addLog("[ERROR] Extra Deck card DRAWN from the main deck: #"+
                       std::to_string(code)+" ["+
                       (ci.name.empty()?"unknown":ci.name)+
                       "] — duel paused. Fix cards.cdb / .ydk / override table.");
                m_blocked = true;
            }
        }
        m_field.deckCount[pl]-=(int)cnt;
        break;
    }

    case MSG_MOVE: {
        // code, previous (con,loc,seq,pos), current (con,loc,seq,pos), reason.
        MoveEvent mv;
        mv.code    = r32(p);
        mv.prevCon = r8(p) & 1; mv.prevLoc = r8(p); mv.prevSeq = r32(p); r32(p);
        mv.curCon  = r8(p) & 1; mv.curLoc  = r8(p); mv.curSeq  = r32(p);
        mv.curPos  = r32(p);
        r32(p);   // reason
        m_moveEvents.push_back(mv);
        break;
    }

    case MSG_LPUPDATE: {
        uint8_t pl=r8(p)&1; uint32_t lp=r32(p);
        m_field.lp[pl]=lp;
        addLog("Player "+std::to_string(pl+1)+" LP -> "+std::to_string(lp));
        break;
    }

    case MSG_DAMAGE: {
        // ocgcore has ALREADY subtracted the LP internally and does NOT emit a
        // follow-up MSG_LPUPDATE for damage — the client must apply the delta
        // itself. Without this the LP bar never moved after an attack.
        uint8_t pl=r8(p)&1; uint32_t amt=r32(p);
        uint32_t before=m_field.lp[pl];
        m_field.lp[pl] = (amt>=before) ? 0u : before-amt;
        addLog("Player "+std::to_string(pl+1)+" takes "+std::to_string(amt)+
               " damage  (LP "+std::to_string(before)+" -> "+
               std::to_string(m_field.lp[pl])+")");
        break;
    }

    case MSG_RECOVER: {
        uint8_t pl=r8(p)&1; uint32_t amt=r32(p);
        uint32_t before=m_field.lp[pl];
        m_field.lp[pl] = before+amt;
        addLog("Player "+std::to_string(pl+1)+" gains "+std::to_string(amt)+
               " LP  (LP "+std::to_string(before)+" -> "+
               std::to_string(m_field.lp[pl])+")");
        break;
    }

    case MSG_SUMMONING:
    case MSG_SPSUMMONING:
    case MSG_FLIPSUMMONING: {
        uint32_t code=r32(p);
        uint8_t  con=r8(p)&1;
        r8(p);r32(p);r32(p);                 // location, sequence, position
        CardInfo ci=m_db.getCard(code);
        std::string nm = ci.name.empty()?("#"+std::to_string(code)):ci.name;
        addLog("Summoning: "+nm);
        // Remember the card being summoned. The post-summon trace is armed
        // later, at MSG_*SUMMONED, when the Summon has actually resolved.
        m_pendingSummonCode   = code;
        m_pendingSummonName   = nm;
        m_pendingSummonType   = (type==MSG_SUMMONING) ? 0
                              : (type==MSG_SPSUMMONING) ? 1 : 2;
        m_pendingSummonPlayer = con;
        setLastAction(con, std::string(
            type==MSG_SPSUMMONING ? "Special Summoning " :
            type==MSG_FLIPSUMMONING ? "Flip Summoning " : "Summoning ") + nm);
        m_pacedEventThisProcess = true;   // beat: let each summon land visibly
        break;
    }

    case MSG_CHAINING: {
        uint32_t code=r32(p);
        uint8_t con = r8(p) & 1;             // controller of the chaining card
        r8(p);r32(p);r32(p);
        r8(p);r8(p);r32(p);
        p+=8; r32(p);
        CardInfo ci=m_db.getCard(code);
        std::string nm = ci.name.empty()?("#"+std::to_string(code)):ci.name;
        addLog("Activating: "+nm);
        setLastAction(con, "Activating " + nm);
        m_chainSourceCode = code;         // for card-less Yes/No fallback text
        m_pacedEventThisProcess = true;   // beat: let each activation read
        break;
    }

    case MSG_ATTACK: {
        // ocgcore layout (20 bytes): attacker loc-info + target loc-info.
        //   attacker: u8 con, u8 loc, u32 seq, u32 pos
        //   target  : u8 con, u8 loc, u32 seq, u32 pos
        // Direct attack: target.location == 0 (LOCATION_NULL) — no live target
        // card exists on the field for the attack to point at.
        AttackEvent ev;
        ev.attackerCon = r8(p);
        ev.attackerLoc = r8(p);
        ev.attackerSeq = r32(p);
        ev.attackerPos = r32(p);
        ev.targetCon   = r8(p);
        ev.targetLoc   = r8(p);
        ev.targetSeq   = r32(p);
        ev.targetPos   = r32(p);
        ev.direct      = (ev.targetLoc == 0);
        m_attackEvents.push_back(ev);

        // Try to resolve attacker / target names for the log.
        auto lookupCardCode = [&](uint8_t con, uint8_t loc, uint32_t seq)
            -> uint32_t {
            if (con > 1) return 0;
            if (loc == LOC_MZONE) {
                for (const auto& c : m_field.monsters[con])
                    if (c.seq == seq) return c.code;
            } else if (loc == LOC_SZONE) {
                for (const auto& c : m_field.spells[con])
                    if (c.seq == seq) return c.code;
            }
            return 0;
        };
        uint32_t atkCode = lookupCardCode(ev.attackerCon, ev.attackerLoc,
                                          ev.attackerSeq);
        uint32_t tgtCode = ev.direct ? 0
            : lookupCardCode(ev.targetCon, ev.targetLoc, ev.targetSeq);
        std::string atkName = atkCode ? m_db.getCard(atkCode).name
                                      : "(unknown attacker)";
        std::string tgtName = ev.direct ? "(Direct Attack)"
            : tgtCode ? m_db.getCard(tgtCode).name : "(unknown target)";
        setLastAction(ev.attackerCon, ev.direct
            ? ("Attacking directly with " + atkName)
            : ("Attacking " + tgtName + " with " + atkName));

        addLog("[ATTACK EVENT] attacker #" + std::to_string(atkCode) +
               " [" + atkName + "] (P" +
               std::to_string(ev.attackerCon + 1) +
               " loc=" + std::to_string(ev.attackerLoc) +
               " seq=" + std::to_string(ev.attackerSeq) + ")"
               " → target " +
               (ev.direct
                  ? "(Direct Attack)"
                  : "#" + std::to_string(tgtCode) +
                    " [" + tgtName + "] (P" +
                    std::to_string(ev.targetCon + 1) +
                    " loc=" + std::to_string(ev.targetLoc) +
                    " seq=" + std::to_string(ev.targetSeq) + ")"));
        break;
    }

    // ── Selection messages ────────────────────────────────────────────────────
    case MSG_SELECT_IDLECMD: {
        // The Main Phase idle prompt. If a post-summon trace is still armed
        // when we reach here, the engine went straight back to idle WITHOUT
        // opening a chain window — i.e. the on-summon Trigger Effect was NOT
        // offered. Conclude the trace loudly before parsing the idle actions.
        if (m_postSummonPending) {
            addLog("[POST-SUMMON] no chain window opened before the Main Phase "
                   "idle prompt — on-summon Trigger Effect was NOT offered.");
            endPostSummonTrace("MSG_SELECT_IDLECMD (idle Main Phase prompt)",
                               {}, "n/a (no post-summon chain window opened)");
        }

        // Capture EVERY action the engine offers — summon, special summon,
        // reposition, set, set-S/T, activate — so the UI can present exactly
        // the legal choices and nothing else. cmd values match respondIdleCmd.
        m_selection = {};
        m_selection.type   = WaitType::SelectIdleCmd;
        m_selection.player = r8(p)&1;
        m_selection.timing = TimingContext::IdleMainPhase;

        auto addAction = [&](uint32_t code, int cmd, int index,
                             uint8_t con=0, uint8_t loc=0, uint32_t seq=0,
                             uint64_t desc=0) {
            IdleAction a;
            a.code = code; a.cmd = cmd; a.index = index;
            a.con = con; a.loc = loc; a.seq = seq;
            a.name = m_db.getCard(code).name;
            if (cmd == 5) a.effect = decodeDesc(desc);  // effect.raw holds desc
            m_selection.idle.push_back(a);
        };
        // summonable / spsummonable / msetable / ssetable: code,con,loc,seq(u32)
        // repositionable: code,con,loc,seq(u8)
        // activatable: code,con,loc,seq(u32),desc(u64),client_mode(u8)
        // con/loc/seq are captured for every action so the click-first UI can
        // map a clicked card to ONLY its own legal actions.
        uint32_t sumCnt=r32(p);
        for(uint32_t i=0;i<sumCnt;i++){
            uint32_t c=r32(p); uint8_t cn=r8(p),lc=r8(p); uint32_t sq=r32(p);
            addAction(c,0,(int)i,cn,lc,sq);
        }
        uint32_t spCnt=r32(p);
        for(uint32_t i=0;i<spCnt;i++){
            uint32_t c=r32(p); uint8_t cn=r8(p),lc=r8(p); uint32_t sq=r32(p);
            addAction(c,1,(int)i,cn,lc,sq);
        }
        uint32_t repoCnt=r32(p);
        for(uint32_t i=0;i<repoCnt;i++){
            uint32_t c=r32(p); uint8_t cn=r8(p),lc=r8(p); uint8_t sq=r8(p);
            addAction(c,2,(int)i,cn,lc,(uint32_t)sq);
        }
        uint32_t msetCnt=r32(p);
        for(uint32_t i=0;i<msetCnt;i++){
            uint32_t c=r32(p); uint8_t cn=r8(p),lc=r8(p); uint32_t sq=r32(p);
            addAction(c,3,(int)i,cn,lc,sq);
        }
        uint32_t ssetCnt=r32(p);
        for(uint32_t i=0;i<ssetCnt;i++){
            uint32_t c=r32(p); uint8_t cn=r8(p),lc=r8(p); uint32_t sq=r32(p);
            addAction(c,4,(int)i,cn,lc,sq);
        }
        uint32_t actCnt=r32(p);
        for(uint32_t i=0;i<actCnt;i++){
            uint32_t c=r32(p);
            uint8_t  con=r8(p), loc=r8(p);
            uint32_t seq=r32(p);
            uint64_t desc=r64(p);
            r8(p);                              // client_mode
            addAction(c,5,(int)i,con,loc,seq,desc);
        }

        m_selection.toBP = r8(p)!=0;   // can go to Battle Phase
        m_selection.toEP = r8(p)!=0;   // can end turn
        r8(p);                         // can shuffle hand (not exposed yet)

        addLog(m_selection.player == m_humanSeat ? "[Your turn - Main Phase]"
                                                 : "[Player 2 - Main Phase]");
        // Per-action source dump so a GY effect and a field effect of the
        // same card are visibly distinct (different loc/seq/index). Each
        // activatable source is uniquely identified by con+loc+seq+code+
        // cmd+index — the UI must never collapse these by code alone.
        if (m_debugMsgs) {
            for (const auto& a : m_selection.idle)
                addLog(std::string("[IDLE ACTION SOURCE] cmd=") +
                       std::to_string(a.cmd) +
                       " idx=" + std::to_string(a.index) +
                       " code=" + std::to_string(a.code) + " [" + a.name + "]" +
                       " con=" + std::to_string(a.con) +
                       " loc=" + std::to_string(a.loc) +
                       " seq=" + std::to_string(a.seq) +
                       " label=" + (a.cmd == 0 ? "Summon" :
                                    a.cmd == 1 ? "SpSummon" :
                                    a.cmd == 2 ? "Reposition" :
                                    a.cmd == 3 ? "SetMonster" :
                                    a.cmd == 4 ? "SetS/T" :
                                    a.cmd == 5 ? "Activate" : "?"));
        }
        // Effect-option logging (always on): every Activate action with its
        // decoded effect description, so a multi-effect card's offered effect
        // is identifiable. Timing context here is always "idle Main Phase".
        for (const auto& a : m_selection.idle) {
            if (a.cmd != 5) continue;
            std::string line =
                "[effect-option] timing=idle Main Phase effect"
                " | engineIdx=" + std::to_string(a.index) +
                " | card #" + std::to_string(a.code) + " [" + a.name + "]" +
                " | loc=" + std::to_string(a.loc) +
                " seq=" + std::to_string(a.seq) +
                " | desc=" + std::to_string(a.effect.raw);
            if (a.effect.isCardString)
                line += " (id " + std::to_string(a.effect.cardCode) +
                        " str" + std::to_string(a.effect.index + 1) +
                        ": \"" + a.effect.text + "\")";
            else if (a.effect.raw)
                line += " (system/hint string)";
            else
                line += " (no description)";
            addLog(line);
        }
        // Explain a MISSING "Activate" option. A card offered only as Set
        // (cmd 3/4) with no matching Activate (cmd 5) whose script file is
        // absent has NO activatable effect — so the engine correctly never
        // listed it. This is the usual cause of "Foolish Burial only shows
        // Set S/T" and is NOT an idle-command parser/UI bug.
        if (m_debugMsgs) {
            std::vector<uint32_t> noted;
            for (const auto& a : m_selection.idle) {
                if (a.cmd != 3 && a.cmd != 4) continue;
                bool activatable = false, seen = false;
                for (const auto& b : m_selection.idle)
                    if (b.cmd == 5 && b.code == a.code) activatable = true;
                for (uint32_t c : noted) if (c == a.code) seen = true;
                if (activatable || seen) continue;
                noted.push_back(a.code);
                if (findCardScript(a.code).empty())
                    addLog("[IDLE NOTE] #" + std::to_string(a.code) + " [" +
                           a.name + "] is offered as Set only — script c" +
                           std::to_string(a.code) + ".lua is MISSING, so the "
                           "card has NO activatable effect (engine is "
                           "correct; not a parser/UI bug).");
            }
        }
        if (m_debugMsgs) {
            addLog("[dbg] idlecmd: summon=" + std::to_string(sumCnt) +
                   " spsummon=" + std::to_string(spCnt) +
                   " repos=" + std::to_string(repoCnt) +
                   " mset=" + std::to_string(msetCnt) +
                   " sset=" + std::to_string(ssetCnt) +
                   " activate=" + std::to_string(actCnt) +
                   " toBP=" + std::to_string(m_selection.toBP) +
                   " toEP=" + std::to_string(m_selection.toEP));
            for (const auto& a : m_selection.idle)
                addLog("[dbg]   action cmd=" + std::to_string(a.cmd) +
                       " idx=" + std::to_string(a.index) + " #" +
                       std::to_string(a.code) + " [" + a.name + "]");
        }
        break;
    }

    case MSG_SELECT_BATTLECMD: {
        // activatable: code,con,loc,seq(u32),desc(u64),mode = 19 bytes
        // attackable:  code,con,loc,seq(u8),can_direct   = 8 bytes
        // Response uses respondIdleCmd: t=0 activate, 1 attack, 2 M2, 3 EP.
        m_selection = {};
        m_selection.type   = WaitType::SelectBattleCmd;
        m_selection.player = r8(p)&1;
        auto addAction = [&](uint32_t code, int cmd, int index,
                             uint8_t con=0, uint8_t loc=0, uint32_t seq=0,
                             uint64_t desc=0, bool canDir=false) {
            IdleAction a; a.code=code; a.cmd=cmd; a.index=index;
            a.con=con; a.loc=loc; a.seq=seq;
            a.name=m_db.getCard(code).name;
            if (cmd == 0) a.effect = decodeDesc(desc);   // battle activate
            a.canDirect = canDir;
            m_selection.idle.push_back(a);
        };
        uint32_t actCnt=r32(p);
        for(uint32_t i=0;i<actCnt;i++){
            uint32_t c=r32(p); uint8_t cn=r8(p),lc=r8(p); uint32_t sq=r32(p);
            uint64_t desc=r64(p); r8(p);
            addAction(c,0,(int)i,cn,lc,sq,desc);
        }
        uint32_t attCnt=r32(p);
        int directCnt = 0;
        for(uint32_t i=0;i<attCnt;i++){
            uint32_t c=r32(p); uint8_t cn=r8(p),lc=r8(p); uint8_t sq=r8(p);
            uint8_t canDir = r8(p);     // can_direct flag (was previously discarded)
            if (canDir) ++directCnt;
            addAction(c,1,(int)i,cn,lc,(uint32_t)sq,0,canDir != 0);
        }
        m_selection.toM2 = r8(p)!=0;
        m_selection.toEP = r8(p)!=0;
        addLog("[Battle Phase - Player "+std::to_string(m_selection.player+1)+" - "+
               std::to_string(attCnt)+" attacker(s)]");
        addLog("[BTL CMD] attackers=" + std::to_string(attCnt) +
               " activatable=" + std::to_string(actCnt) +
               " canDirect=" + std::to_string(directCnt) +
               " toM2=" + std::to_string((int)m_selection.toM2) +
               " toEP=" + std::to_string((int)m_selection.toEP));
        if (m_debugMsgs) {
            for (const auto& a : m_selection.idle) {
                const char* tag = (a.cmd == 1) ? "ATK" : "ACT";
                addLog(std::string("[BTL CMD]   ") + tag + " idx=" +
                       std::to_string(a.index) + " #" +
                       std::to_string(a.code) + " [" + a.name + "]" +
                       " con=" + std::to_string(a.con) +
                       " loc=" + std::to_string(a.loc) +
                       " seq=" + std::to_string(a.seq) +
                       (a.cmd == 1 && a.canDirect ? " (DIRECT)" : ""));
            }
        }
        break;
    }

    case MSG_SELECT_YESNO: {
        // Format: player(u8), description(u64).
        m_selection = {};
        m_selection.type   = WaitType::SelectYesNo;
        m_selection.player = r8(p)&1;
        uint64_t desc = r64(p);
        m_selection.chainEffects.push_back(decodeDesc(desc));
        addLog("[Select Yes/No - Player "+std::to_string(m_selection.player+1)+"]");
        if (m_postSummonPending)
            endPostSummonTrace("MSG_SELECT_YESNO", {},
                std::string("no (yes/no prompt handed to ") +
                (m_selection.player==0 ? "you)" : "the opponent)"));
        break;
    }

    case MSG_SELECT_EFFECTYN: {
        // Format: player(u8), code(u32), loc_info(controler u8, location u8,
        // sequence u32, position u32 = 10 bytes), description(u64). This is the
        // engine's "do you want to activate this effect?" prompt — used for
        // some optional effects instead of a full chain window.
        m_selection = {};
        m_selection.type   = WaitType::SelectEffectYn;
        m_selection.player = r8(p)&1;
        uint32_t code = r32(p);
        uint8_t  con  = r8(p), loc = r8(p);
        uint32_t seq  = r32(p);
        r32(p);                                  // position
        uint64_t desc = r64(p);
        CardState cs{};
        cs.code=code; cs.name=m_db.getCard(code).name;
        cs.player=con; cs.loc=loc; cs.seq=seq;
        EffectDesc d = decodeDesc(desc);
        m_selection.cards.push_back(cs);
        m_selection.chainEffects.push_back(d);
        m_selection.timing =
            m_postSummonPending                    ? TimingContext::PostSummonTrigger :
            (m_selection.player==m_field.turnPlayer)? TimingContext::QuickChainWindow  :
                                                      TimingContext::OpponentChainWindow;
        if (m_postSummonPending) m_postSummonEffYnSeen = true;

        std::string line =
            "[effect-option] timing=" + std::string(timingName(m_selection.timing)) +
            " | MSG_SELECT_EFFECTYN | Player " +
            std::to_string(m_selection.player+1) +
            " | card #" + std::to_string(code) + " [" +
            (cs.name.empty()?"unknown":cs.name) + "]" +
            " | loc=" + std::to_string(loc) + " seq=" + std::to_string(seq) +
            " | desc=" + std::to_string(d.raw);
        if (d.isCardString)
            line += " (id " + std::to_string(d.cardCode) + " str" +
                    std::to_string(d.index + 1) + ": \"" + d.text + "\")";
        else if (d.raw)
            line += " (system/hint string)";
        addLog(line);

        if (m_postSummonPending) {
            std::vector<std::string> opt;
            std::string l = "option 0: card #" + std::to_string(code) + " [" +
                (cs.name.empty()?"unknown":cs.name) + "] desc=" +
                std::to_string(d.raw);
            if (d.isCardString)
                l += " (id " + std::to_string(d.cardCode) + " str" +
                     std::to_string(d.index + 1) + ": \"" + d.text + "\")";
            opt.push_back(l);
            endPostSummonTrace("MSG_SELECT_EFFECTYN",
                m_selection.player==0 ? opt : std::vector<std::string>{},
                m_selection.player==0
                    ? "no (effect yes/no prompt — handed to you)"
                    : "n/a (prompt belongs to the opponent)");
        }
        break;
    }

    case MSG_SELECT_OPTION: {
        // Format: player(u8), count(u8), then count * uint64 descriptions —
        // SAME encoding as the per-effect description used in MSG_SELECT_CHAIN.
        // Decode every desc via the existing decoder so the UI can show real
        // effect text ("Send 1 card from Deck to GY", "Apply FIRE effect", …)
        // instead of a generic "Option 1 / Option 2" list.
        m_selection = {};
        m_selection.type   = WaitType::SelectOption;
        m_selection.player = r8(p)&1;
        uint8_t optCnt=r8(p);
        m_selection.options.clear();
        m_selection.chainEffects.clear();
        for (int i = 0; i < optCnt; ++i) {
            uint64_t desc = r64(p);
            m_selection.options.push_back(i);
            m_selection.chainEffects.push_back(decodeDesc(desc));
        }
        addLog("[Select option - Player " +
               std::to_string(m_selection.player+1) + "]  " +
               std::to_string((int)optCnt) + " choice(s)");
        for (size_t i = 0; i < m_selection.chainEffects.size(); ++i) {
            const EffectDesc& d = m_selection.chainEffects[i];
            std::string line = "  option " + std::to_string(i) +
                ": desc=" + std::to_string(d.raw);
            if (d.isCardString)
                line += " (id " + std::to_string(d.cardCode) + " str" +
                        std::to_string(d.index + 1) + ": \"" + d.text + "\")";
            else if (d.raw)
                line += " (system/hint string)";
            else
                line += " (no description)";
            addLog(line);
        }
        break;
    }

    case MSG_SELECT_CARD: {
        // Per card: code(u32) + loc_info(controler u8, location u8,
        // sequence u32, position u32) = 14 bytes.
        m_selection.type   = WaitType::SelectCard;
        m_selection.player = r8(p)&1;
        r8(p);                                       // cancelable
        m_selection.min=(int)r32(p); m_selection.max=(int)r32(p);
        uint32_t cnt=r32(p);
        m_selection.cards.clear();
        for(uint32_t i=0;i<cnt;i++){
            uint32_t code=r32(p);
            uint8_t con=r8(p)&1; uint8_t loc=r8(p);
            uint32_t seq=r32(p); uint32_t pos=r32(p);
            CardState cs{}; cs.code=code; cs.name=m_db.getCard(code).name;
            cs.player=con; cs.loc=loc; cs.seq=seq; cs.pos=pos;
            m_selection.cards.push_back(cs);
        }
        addLog("[Select "+std::to_string(m_selection.min)+"-"+std::to_string(m_selection.max)+
               " card(s) - Player "+std::to_string(m_selection.player+1)+"]");
        break;
    }

    case MSG_SELECT_TRIBUTE: {
        // MSG_SELECT_TRIBUTE differs from MSG_SELECT_CARD: each card is
        // code(u32) + controler(u8) + location(u8) + sequence(u32) +
        // release_param(u8) = 11 bytes (no position field).
        m_selection.type   = WaitType::SelectTribute;
        m_selection.player = r8(p)&1;
        r8(p);                                       // cancelable
        m_selection.min=(int)r32(p); m_selection.max=(int)r32(p);
        uint32_t cnt=r32(p);
        m_selection.cards.clear();
        for(uint32_t i=0;i<cnt;i++){
            uint32_t code=r32(p);
            uint8_t con=r8(p)&1; uint8_t loc=r8(p);
            uint32_t seq=r32(p); r8(p);             // release_param (no pos)
            CardState cs{}; cs.code=code; cs.name=m_db.getCard(code).name;
            cs.player=con; cs.loc=loc; cs.seq=seq;
            m_selection.cards.push_back(cs);
        }
        addLog("[Select "+std::to_string(m_selection.min)+"-"+std::to_string(m_selection.max)+
               " tribute(s) - Player "+std::to_string(m_selection.player+1)+"]");
        break;
    }

    case MSG_SELECT_CHAIN: {
        // Per entry: code(u32), loc_info(controler u8, location u8,
        // sequence u32, position u32 = 10 bytes), description(u64),
        // client_mode(u8) = 23 bytes. The description is what tells a
        // multi-effect card's Trigger Effect apart from its Quick Effect.
        uint8_t  pid    = r8(p)&1;
        uint8_t  spe    = r8(p); (void)spe;
        bool     forced = r8(p)!=0;
        r32(p); r32(p);                       // hint timings
        uint32_t cnt=r32(p);
        std::vector<CardState>  chain;
        std::vector<EffectDesc> descs;
        for(uint32_t i=0;i<cnt;i++){
            uint32_t code=r32(p);
            uint8_t  con=r8(p), loc=r8(p);
            uint32_t seq=r32(p);
            r32(p);                            // position
            uint64_t desc=r64(p);
            r8(p);                             // client_mode
            CardState cs{};
            cs.code=code; cs.name=m_db.getCard(code).name;
            cs.player=con; cs.loc=loc; cs.seq=seq;
            chain.push_back(cs);
            descs.push_back(decodeDesc(desc));
        }

        // Classify the window. A chain window opened while a post-summon trace
        // is armed IS the on-summon trigger window — the thing we are hunting.
        TimingContext tc =
            m_postSummonPending         ? TimingContext::PostSummonTrigger :
            (pid == m_field.turnPlayer) ? TimingContext::QuickChainWindow  :
                                          TimingContext::OpponentChainWindow;
        if (m_postSummonPending) m_postSummonChainSeen = true;

        // Build the per-option diagnostic lines (shared by the normal log and
        // the post-summon trace).
        std::vector<std::string> optionLines;
        for (uint32_t i = 0; i < chain.size(); ++i) {
            const EffectDesc& d = descs[i];
            std::string l = "option " + std::to_string(i) +
                ": card #" + std::to_string(chain[i].code) +
                " [" + (chain[i].name.empty()?"unknown":chain[i].name) + "]" +
                " | loc=" + std::to_string(chain[i].loc) +
                " seq=" + std::to_string(chain[i].seq) +
                " | engineIdx=" + std::to_string(i) +
                " | desc=" + std::to_string(d.raw);
            if (d.isCardString)
                l += " (id " + std::to_string(d.cardCode) + " str" +
                     std::to_string(d.index + 1) + ": \"" + d.text + "\")";
            else if (d.raw)
                l += " (system/hint string)";
            else
                l += " (no description)";
            optionLines.push_back(l);
        }

        // A chain window with no activatable cards has exactly one legal
        // answer: pass. Both peers in MP (and the playback engine during
        // a replay) reach this same state deterministically, so a SILENT
        // local auto-pass is safe — we just need to make sure the
        // response recorder doesn't fan it out over the network or into
        // the replay file. The m_internalAutoResolve flag set below is
        // honoured by submitResponse() exactly once for this path.
        if (cnt == 0 && !forced) {
            addLog(std::string("[trace] chain window for ") +
                   (pid==0 ? "YOU" : "opponent") + " [" + timingName(tc) +
                   "]: 0 activatable option(s) -> silent local auto-pass");
            if (m_postSummonPending)
                endPostSummonTrace("MSG_SELECT_CHAIN (0 options)", {},
                    "yes — engine opened the post-summon chain window but "
                    "offered 0 options (the on-summon Trigger Effect's "
                    "activation condition was not met, e.g. no legal target)");
            m_internalAutoResolve = true;
            respondChain(-1);
            break;
        }

        m_selection = {};
        m_selection.type         = WaitType::SelectChain;
        m_selection.player       = pid;
        m_selection.forced       = forced;
        m_selection.cards        = chain;
        m_selection.chainEffects = descs;
        m_selection.timing       = tc;

        // Always logged — this is the "can I activate an effect?" window.
        addLog("[trace] chain window for " +
               std::string(pid==0 ? "YOU" : "opponent") + " [" +
               timingName(tc) + "]: " + std::to_string(cnt) +
               " activatable option(s)" + (forced ? " (FORCED)" : ""));
        for (auto& l : optionLines) addLog("   " + l);

        if (m_postSummonPending) {
            bool forYou = (pid == 0);
            endPostSummonTrace("MSG_SELECT_CHAIN",
                forYou ? optionLines : std::vector<std::string>{},
                forYou
                    ? (forced ? "no (forced chain — you must respond)"
                              : "no (options offered — handed to you to decide)")
                    : "n/a (chain window belongs to the opponent)");
        }
        break;
    }

    case MSG_SELECT_PLACE:
    case MSG_SELECT_DISFIELD: {
        // Format: player(u8), count(u8), flag(u32). `flag` bits mark zones that
        // are FORBIDDEN/occupied: bits 0-6 = the asked player's monster zones,
        // bits 8-15 = their spell/trap zones. The response is 3 bytes PER place
        // { player, location, sequence }.
        uint8_t  pid   = r8(p) & 1;
        uint8_t  count = r8(p);
        uint32_t flag  = r32(p);

        // For the local human placing a single card, let them pick the zone.
        // (Opponent, multi-zone placements and DISFIELD stay auto-resolved.)
        // NOTE: must compare against m_humanSeat, not 0 — the coin toss can seat
        // the human at index 1, in which case the AI is player 0 and its zone
        // choice must fall through to the auto-resolver below (else the duel
        // hangs on "no auto-AI response available").
        if (type == MSG_SELECT_PLACE && pid == m_humanSeat && count == 1) {
            m_selection = {};
            m_selection.type       = WaitType::SelectPlace;
            m_selection.player     = pid;
            m_selection.placeFlag  = flag;
            m_selection.placeCount = 1;
            addLog("[Choose a zone]");
            // Enumerate the legal zones so a wrong/empty placeFlag is obvious.
            {
                char fb[24]; snprintf(fb, sizeof(fb), "0x%x", (unsigned)flag);
                addLog(std::string("[PLACE REQUEST] player=P") +
                       std::to_string(pid + 1) + " count=" +
                       std::to_string(count) + " placeFlag=" + fb);
            }
            auto bit = [&](int b) { return (flag & (1u << b)) == 0; };
            int legalCount = 0;
            auto note = [&](const char* zname, int loc, int seq) {
                if (m_debugMsgs)
                    addLog("  candidate: " + std::string(zname) +
                           "  loc=" + std::to_string(loc) +
                           " seq=" + std::to_string(seq));
                ++legalCount;
            };
            for (int s = 0; s < 5; ++s) if (bit(s))      note((std::string("M") + char('1'+s)).c_str(), 4, s);
            if (bit(5))  note("EMZ1", 4, 5);
            if (bit(6))  note("EMZ2", 4, 6);
            for (int s = 0; s < 5; ++s) if (bit(s + 8))  note((std::string("ST") + char('1'+s)).c_str(), 8, s);
            if (bit(13)) note("FZ",   8, 5);
            if (bit(14)) note("Pend-L", 8, 6);
            if (bit(15)) note("Pend-R", 8, 7);
            addLog("  legal-zone count: " + std::to_string(legalCount));
            break;
        }

        if (count == 0) count = 1;
        if (count > 8)  count = 8;
        uint8_t  resp[8 * 3];
        uint32_t used = flag;               // SET bit = unavailable zone
        // Zone groups exactly as ocgcore encodes the place flag (see its
        // SIMPLE_AI placer): the low 16 bits are the asked player's own zones,
        // bits 16-31 are the opponent's. CRUCIALLY this includes the Extra
        // Monster Zones (bits 5-6) and never lets a monster land in an S/T zone
        // — the old picker missed the EMZ and fell back to S/T, so a Fusion /
        // Synchro / Link Summon whose only legal spot was an EMZ got rejected
        // (MSG_RETRY) and paused the duel.
        struct ZGroup { int base, n, locMZ, seqBase, opp; };
        static const ZGroup kGroups[] = {
            { 0,  5, 1, 0, 0 },   // own main monster zones -> seq 0..4
            { 5,  2, 1, 5, 0 },   // own Extra Monster Zones -> seq 5,6
            { 8,  5, 0, 0, 0 },   // own spell/trap zones    -> seq 0..4
            { 13, 1, 0, 5, 0 },   // own field zone          -> seq 5
            { 14, 2, 0, 6, 0 },   // own pendulum zones      -> seq 6,7
            { 16, 5, 1, 0, 1 },   // opponent main monster
            { 21, 2, 1, 5, 1 },   // opponent EMZ
            { 24, 5, 0, 0, 1 },   // opponent spell/trap
            { 29, 1, 0, 5, 1 },   // opponent field zone
            { 30, 2, 0, 6, 1 },   // opponent pendulum
        };
        for (uint8_t k = 0; k < count; ++k) {
            uint8_t rplayer = pid, loc = (uint8_t)LOC_MZONE, seq = 0;
            bool found = false;
            for (const ZGroup& g : kGroups) {
                for (int i = 0; i < g.n && !found; ++i) {
                    int bit = g.base + i;
                    if (used & (1u << bit)) continue;          // unavailable
                    used   |= (1u << bit);
                    rplayer = (uint8_t)(g.opp ? (pid ^ 1) : pid);
                    loc     = (uint8_t)(g.locMZ ? LOC_MZONE : LOC_SZONE);
                    seq     = (uint8_t)(g.seqBase + i);
                    found   = true;
                }
                if (found) break;
            }
            resp[k*3+0] = rplayer; resp[k*3+1] = loc; resp[k*3+2] = seq;
        }
        submitResponse(resp, (uint32_t)count * 3u);
        break;
    }

    case MSG_SELECT_POSITION: {
        // Format: player(u8), code(u32), positions-mask(u8). Response: int32 of
        // one position bit that is set in the mask.
        uint8_t who = r8(p); uint32_t code = r32(p); uint8_t mask = r8(p);

        // The local human chooses (attack vs defence) via the UI; the opponent
        // and AI auto-resolve. Online MP keeps auto-resolving (the host drives).
        if ((who & 1) == m_humanSeat && m_localMode) {
            m_selection = {};
            m_selection.type         = WaitType::SelectPosition;
            m_selection.player       = who & 1;
            m_selection.positionMask = mask;
            CardState cs{}; cs.code = code; cs.name = m_db.getCard(code).name;
            m_selection.cards.push_back(cs);
            addLog("[Select position - Player " +
                   std::to_string((who & 1) + 1) + "]");
            break;
        }

        int32_t pos = POS_FACEUP_ATTACK;
        if      (mask & 0x1) pos = 0x1; else if (mask & 0x2) pos = 0x2;
        else if (mask & 0x4) pos = 0x4; else if (mask & 0x8) pos = 0x8;
        // AI: put a defensive monster (DEF > ATK, low ATK) face-up in defence.
        if ((who & 1) != m_humanSeat) {
            CardInfo ci = m_db.getCard(code);
            if ((mask & 0x4) && ci.def > ci.atk && ci.atk < 1500) pos = 0x4;
            else if (mask & 0x1)                                  pos = 0x1;
        }
        submitResponse(&pos, 4);
        break;
    }

    case MSG_SORT_CHAIN:
    case MSG_SORT_CARD: {
        r8(p); uint8_t cnt=r8(p);
        for(int i=0;i<cnt;i++){r32(p);r8(p);r8(p);r32(p);r32(p);}
        if(cnt>0&&cnt<16){
            uint8_t resp[16];
            for(int i=0;i<cnt;i++) resp[i]=(uint8_t)i;   // identity order
            submitResponse(resp,cnt);
        }
        break;
    }

    case MSG_SELECT_COUNTER: {
        // Format: player(u8), countertype(u16), count(u16), card_count(u32),
        //   then per card: code(u32) controler(u8) location(u8) sequence(u8)
        //   current_counters(u16). Response: one int16 per card = how many to
        //   remove from it; the sum must equal `count` and each <= its current.
        uint8_t  pid    = r8(p) & 1;
        r16(p);                                   // counter type
        uint16_t count  = r16(p);                 // total counters to remove
        uint32_t cc     = r32(p);
        std::vector<uint16_t> avail;
        avail.reserve(cc);
        for (uint32_t i = 0; i < cc; ++i) {
            r32(p);                               // code
            r8(p); r8(p); r8(p);                  // controler, location, sequence
            avail.push_back(r16(p));              // current counters on this card
        }
        // Greedily spread the removals across the cards (each <= its available).
        // The engine guarantees total available >= count, so this is always a
        // valid distribution — it resolves the cost for both seats without a
        // hard pause. (A picker UI could let the human choose which card later.)
        std::vector<int16_t> resp(cc ? cc : 1, 0);
        int remaining = (int)count;
        for (uint32_t i = 0; i < cc && remaining > 0; ++i) {
            int take = (int)avail[i];
            if (take > remaining) take = remaining;
            resp[i] = (int16_t)take;
            remaining -= take;
        }
        submitResponse(resp.data(), (uint32_t)(cc * sizeof(int16_t)));
        addLog("[auto] select counter - Player " + std::to_string(pid + 1) +
               " removed " + std::to_string(count) + " across " +
               std::to_string(cc) + " card(s)");
        break;
    }

    case MSG_SELECT_SUM: {
        // Layout: player(u8), mode(u8), acc(u32), min(u32), max(u32),
        //   must_count(u32) + must[ code(u32), loc_info(10), sum_param(u32) ],
        //   sel_count(u32)  + sel [ code(u32), loc_info(10), sum_param(u32) ].
        // loc_info = controller(u8) location(u8) sequence(u32) position(u32).
        uint8_t  pid  = r8(p) & 1;
        uint8_t  mode = r8(p);
        int      acc  = (int)(r32(p) & 0xffff);
        int      mn   = (int)r32(p);
        int      mx   = (int)r32(p);
        std::vector<uint32_t> mustParams, selParams;
        std::vector<uint32_t> selCodes;
        uint32_t mc = r32(p);
        for (uint32_t i = 0; i < mc; ++i) {
            r32(p);                       // code
            r8(p); r8(p); r32(p); r32(p); // loc_info (controller/loc/seq/pos)
            mustParams.push_back(r32(p)); // sum_param
        }
        uint32_t fc = r32(p);
        for (uint32_t i = 0; i < fc; ++i) {
            uint32_t code = r32(p);
            r8(p); r8(p); r32(p); r32(p); // loc_info
            selParams.push_back(r32(p));  // sum_param
            selCodes.push_back(code);
        }
        addLog("[Select sum - Player " + std::to_string(pid + 1) + " - target " +
               std::to_string(acc) + ", " + std::to_string(fc) + " option(s)]");

        // Auto-solve a valid subset and respond. The engine re-validates, so an
        // accepted subset summons cleanly; this turns the old hard "duel paused"
        // on Ritual/sum costs into a working action for both seats.
        std::vector<int> pick;
        bool ok = solveSelectSum(mustParams, selParams, acc, mn, mx, mode, pick);
        if (!ok) {
            // No exact subset found in budget: fall back to the first `min`
            // options so we still send a well-formed response (cancel is not
            // allowed for SUM). A wrong guess just draws an engine retry.
            for (int i = 0; i < mn && i < (int)selParams.size(); ++i)
                pick.push_back(i);
            addLog("[Select sum] no exact subset found — sent fallback");
        }
        respondMultipleCards(pick);
        break;
    }

    case MSG_SELECT_UNSELECT_CARD: {
        // player, finishable, cancelable, min, max, then the selectable cards
        // and the already-selected cards. Used for summon material selection
        // (Link/Synchro/Xyz/Fusion). The engine re-asks one card at a time.
        uint8_t pid       = r8(p)&1;
        bool    finishable= r8(p)!=0;
        bool    cancelable= r8(p)!=0;
        int     mn=(int)r32(p), mx=(int)r32(p);
        std::vector<CardState> sel;
        uint32_t sc=r32(p);
        for(uint32_t i=0;i<sc;i++){
            uint32_t code=r32(p);
            uint8_t con=r8(p)&1; uint8_t loc=r8(p);
            uint32_t seq=r32(p); uint32_t pos=r32(p);
            CardState cs{}; cs.code=code; cs.name=m_db.getCard(code).name;
            cs.player=con; cs.loc=loc; cs.seq=seq; cs.pos=pos;
            sel.push_back(cs);
        }
        uint32_t uc=r32(p);
        for(uint32_t i=0;i<uc;i++){r32(p);r8(p);r8(p);r32(p);r32(p);}
        m_selection = {};
        m_selection.type   = WaitType::SelectUnselect;
        m_selection.player = pid;
        m_selection.cards  = sel;
        m_selection.min=mn; m_selection.max=mx;
        m_selection.finishable = finishable;
        m_selection.forced = !(finishable || cancelable);
        addLog("[Select card/material - Player "+std::to_string(pid+1)+" - "+
               std::to_string(sc)+" option(s)]");
        if (m_debugMsgs)
            addLog("[dbg] unselect: select="+std::to_string(sc)+" unselect="+
                   std::to_string(uc)+" min="+std::to_string(mn)+" max="+
                   std::to_string(mx)+" finishable="+std::to_string(finishable));
        break;
    }

    case MSG_WAITING:
        m_selection.type = WaitType::Waiting;
        break;

    case MSG_HINT:
        r8(p); r8(p); p+=8;
        break;

    case MSG_POS_CHANGE:
        r32(p); r8(p);r8(p);r32(p);r32(p); r8(p);r8(p);r32(p);r32(p);
        break;

    case MSG_TOSS_COIN:
    case MSG_TOSS_DICE: {
        r8(p); uint8_t cnt=r8(p);
        for(int i=0;i<cnt;i++) r8(p);
        break;
    }

    case MSG_SUMMONED:
    case MSG_SPSUMMONED:
    case MSG_FLIPSUMMONED:
        // The Summon has resolved — ocgcore now raises EVENT_SUMMON_SUCCESS,
        // the timing an on-summon Trigger Effect activates at. Arm the trace
        // so the very next selection window is recorded against this Summon.
        beginPostSummonTrace();
        break;

    case MSG_CHAIN_END:
    case MSG_DAMAGE_STEP_START:
    case MSG_DAMAGE_STEP_END:
    case MSG_REVERSE_DECK:
    case MSG_ATTACK_DISABLED:
    case MSG_REFRESH_DECK:
    case MSG_SWAP_GRAVE_DECK:
        break;

    case MSG_CHAINED:
    case MSG_CHAIN_SOLVING:
    case MSG_CHAIN_SOLVED:
    case MSG_CHAIN_NEGATED:
    case MSG_CHAIN_DISABLED:
        r8(p);
        break;

    case MSG_FIELD_DISABLED:
        r32(p);
        break;

    case MSG_SET:
        r32(p); r8(p);r8(p);r32(p);r32(p);
        break;

    case MSG_SWAP:
        r8(p);r8(p);r32(p);r32(p); r8(p);r8(p);r32(p);r32(p);
        break;

    case MSG_BATTLE:
        p+=19; p+=19;
        break;

    case MSG_MISSED_EFFECT:
        r8(p);r8(p);r32(p);r32(p); r32(p);
        break;

    case MSG_PAY_LPCOST: {
        uint8_t pl=r8(p)&1; uint32_t amt=r32(p);
        uint32_t before=m_field.lp[pl];
        m_field.lp[pl] = (amt>=before) ? 0u : before-amt;
        addLog("Player "+std::to_string(pl+1)+" pays "+std::to_string(amt)+" LP");
        break;
    }

    case MSG_DECK_TOP:
        r8(p); r32(p); r32(p); r32(p);
        break;

    case MSG_CARD_SELECTED: {
        uint32_t cnt=r32(p);
        for(uint32_t i=0;i<cnt;i++){r8(p);r8(p);r32(p);r32(p);}
        break;
    }

    case MSG_RANDOM_SELECTED: {
        r8(p); uint32_t cnt=r32(p);
        for(uint32_t i=0;i<cnt;i++){r8(p);r8(p);r32(p);r32(p);}
        break;
    }

    case MSG_CONFIRM_CARDS:
    case MSG_CONFIRM_DECKTOP:
    case MSG_CONFIRM_EXTRATOP: {
        r8(p); uint32_t cnt=r32(p);
        for(uint32_t i=0;i<cnt;i++){r32(p);r8(p);r8(p);r32(p);r32(p);}
        break;
    }

    case MSG_BECOME_TARGET:
    case MSG_BE_CHAIN_TARGET: {
        uint32_t cnt=r32(p);
        for(uint32_t i=0;i<cnt;i++){r8(p);r8(p);r32(p);r32(p);}
        break;
    }

    case MSG_SHUFFLE_SET_CARD: {
        r8(p); uint8_t cnt=r8(p);
        for(int i=0;i<cnt;i++){r8(p);r8(p);r32(p);r32(p);}
        for(int i=0;i<cnt;i++){r8(p);r8(p);r32(p);r32(p);}
        break;
    }

    case MSG_SHUFFLE_DECK:
        r8(p);
        break;

    case MSG_SHUFFLE_HAND:
    case MSG_SHUFFLE_EXTRA: {
        r8(p); uint32_t cnt=r32(p);
        for(uint32_t i=0;i<cnt;i++) r32(p);
        break;
    }

    case MSG_MATCH_KILL:
        r32(p);
        break;

    case MSG_ROCK_PAPER_SCISSORS:
    case MSG_HAND_RES:
        r8(p);
        break;

    case MSG_EQUIP:
        r8(p);r8(p);r32(p);r32(p); r8(p);r8(p);r32(p);r32(p);
        break;

    case MSG_UNEQUIP:
        r8(p);r8(p);r32(p);r32(p);
        break;

    case MSG_CARD_TARGET:
    case MSG_CANCEL_TARGET:
        r8(p);r8(p);r32(p);r32(p); r8(p);r8(p);r32(p);r32(p);
        break;

    case MSG_ADD_COUNTER:
    case MSG_REMOVE_COUNTER:
        r16(p); r8(p);r8(p);r32(p);r32(p); r16(p);
        break;

    case MSG_CREATE_RELATION:
    case MSG_RELEASE_RELATION:
        r8(p);r8(p);r32(p);r32(p); r8(p);r8(p);r32(p);r32(p); r8(p);
        break;

    case MSG_ANNOUNCE_RACE:
    case MSG_ANNOUNCE_ATTRIB: {
        // player, count, available-bitmask. Respond with the `count` lowest
        // available race/attribute bits so the effect can resolve.
        r8(p); uint8_t cnt = r8(p); uint32_t avail = r32(p);
        uint32_t resp = 0; int picked = 0;
        for (int b = 0; b < 32 && picked < (int)cnt; ++b)
            if (avail & (1u << b)) { resp |= (1u << b); ++picked; }
        if (resp == 0) resp = avail;     // fallback — never send an empty pick
        submitResponse(&resp, 4);
        { char hb[16]; snprintf(hb, sizeof(hb), "0x%x", (unsigned)resp);
          addLog(std::string("[auto] announce race/attribute -> ") + hb); }
        break;
    }

    case MSG_ANNOUNCE_CARD: {
        // Format: player(u8), opcode_count(u8), then opcode_count * uint64 — an
        // opcode filter describing which card codes are legal. We don't evaluate
        // the filter; instead the local human declares a card via a name-search
        // picker and the engine validates (a wrong pick just draws a retry). AI
        // / opponent / MP fall back to declaring code 0.
        uint8_t pid = r8(p) & 1;
        uint8_t opc = r8(p);
        for (uint8_t i = 0; i < opc; ++i) r64(p);     // skip the opcode filter

        if (m_localMode && pid == m_humanSeat) {
            m_selection = {};
            m_selection.type   = WaitType::AnnounceCard;
            m_selection.player = pid;
            addLog("[Declare a card - Player " + std::to_string(pid + 1) + "]");
            break;
        }
        int32_t code = 0;
        submitResponse(&code, 4);
        break;
    }

    case MSG_ANNOUNCE_NUMBER: {
        // Format: player(u8), count(u8), then count * uint64 numbers. Response is
        // the INDEX of the chosen number. The local human picks via the same UI
        // as SelectOption (identical int-index response); the AI / opponent and
        // online MP auto-pick option 0 (e.g. "banish 3" for Pot of Prosperity).
        uint8_t pid = r8(p) & 1;
        uint8_t cnt = r8(p);
        std::vector<uint64_t> nums;
        nums.reserve(cnt);
        for (int i = 0; i < cnt; ++i) nums.push_back(r64(p));

        if (m_localMode && pid == m_humanSeat && cnt > 1) {
            m_selection = {};
            m_selection.type   = WaitType::SelectOption;
            m_selection.player = pid;
            for (int i = 0; i < cnt; ++i) {
                m_selection.options.push_back(i);
                EffectDesc d; d.text = "Choose " + std::to_string(nums[i]);
                m_selection.chainEffects.push_back(d);
            }
            addLog("[Announce number - Player " + std::to_string(pid + 1) +
                   "] " + std::to_string((int)cnt) + " choice(s)");
            break;
        }
        int32_t idx = 0;
        submitResponse(&idx, 4);
        addLog("[auto] announce number -> option 0");
        break;
    }

    case MSG_CARD_HINT:
        r8(p);r8(p);r32(p);r32(p); r8(p); p+=8;
        break;

    case MSG_PLAYER_HINT:
        r8(p); r8(p); p+=8;
        break;

    case MSG_SHOW_HINT:
        p+=8;
        break;

    case MSG_TAG_SWAP: {
        r8(p); r32(p); r32(p); r32(p); r32(p);
        uint32_t cnt=r32(p);
        for(uint32_t i=0;i<cnt;i++){r32(p);r32(p);}
        break;
    }

    case MSG_RELOAD_FIELD:
        p=end;
        break;

    case MSG_AI_NAME: {
        uint16_t len=r16(p);
        if(p+len<=end) p+=len; else p=end;
        break;
    }

    case MSG_REMOVE_CARDS: {
        uint32_t cnt=r32(p);
        for(uint32_t i=0;i<cnt;i++){r32(p);r8(p);r8(p);r32(p);r32(p);}
        break;
    }

    case MSG_RETRY:
        // The engine rejected the last response. It re-emits the original
        // select message right after this, so m_selection is refreshed below.
        // Log only the first retry of a burst — process() caps the count and
        // parks the duel if retries keep coming, so this never floods.
        ++m_retryCount;
        if (m_retryCount == 1)
            addLog("[Retry] engine rejected the last response");
        else if (m_debugMsgs)
            addLog("[dbg] retry x" + std::to_string(m_retryCount));
        break;

    case MSG_REQUEST_DECK:
        break;

    case MSG_UPDATE_DATA: {
        r8(p); r8(p); uint32_t ulen=r32(p);
        if(p+ulen<=end) p+=ulen; else p=end;
        break;
    }

    case MSG_UPDATE_CARD: {
        r8(p); r8(p); r8(p); uint32_t ulen=r32(p);
        if(p+ulen<=end) p+=ulen; else p=end;
        break;
    }

    case MSG_CUSTOM_MSG: {
        uint32_t ulen=r32(p);
        if(p+ulen<=end) p+=ulen; else p=end;
        break;
    }

    default:
        p=end;
        break;
    }
}

void DuelManager::queryField() {
    if (!m_duel) return;
    const uint32_t Q_FLAGS = QUERY_CODE | QUERY_POSITION | QUERY_COUNTERS;

    // Parse a location-query buffer. Layout (see ocgcore OCG_DuelQueryLocation
    // + card::get_infos):
    //   [uint32 totalSize] then ONE block per zone slot, IN SLOT ORDER:
    //     - empty slot   : a single uint16 == 0  (2 bytes total)
    //     - occupied slot: a run of [uint16 entryLen][uint32 flag][value...]
    //                      entries, terminated by a QUERY_END entry.
    // The previous parser had no empty-slot case: it read the 2-byte zero as
    // an entry size and then read the next slot's bytes as a flag, desyncing
    // the whole stream. That is exactly why a card placed in S/T or Monster
    // zone 2-5 (i.e. after an empty slot) vanished. `seq` is the real slot
    // index, so it now maps straight onto the zone sequence.
    auto parseBuffer = [&](const void* rawBuf, uint32_t bufLen,
                            uint8_t controller, uint32_t loc) -> std::vector<CardState> {
        std::vector<CardState> out;
        if (!rawBuf || bufLen < 4) return out;
        const uint8_t* p   = static_cast<const uint8_t*>(rawBuf);
        const uint8_t* end = p + bufLen;
        p += 4;                                  // skip leading uint32 totalSize
        uint32_t seq = 0;
        while (p + 2 <= end) {
            uint16_t firstLen; memcpy(&firstLen, p, 2);
            if (firstLen == 0) {                 // empty zone slot
                p += 2;
                ++seq;
                continue;
            }
            CardState cs{};
            cs.player=controller; cs.loc=loc; cs.seq=seq; cs.pos=POS_FACEDOWN_DEFENSE;
            bool gotEnd=false;
            while (p+6<=end) {
                uint16_t entrySize; memcpy(&entrySize,p,2); p+=2;
                uint32_t flag; memcpy(&flag,p,4);
                if(flag==QUERY_END){ p+=4; gotEnd=true; break; }
                int valBytes=(int)entrySize-4;
                if(valBytes<0 || p+4+valBytes>end){ p=end; gotEnd=true; break; }
                p+=4;
                if(flag==QUERY_CODE&&valBytes>=4) memcpy(&cs.code,p,4);
                else if(flag==QUERY_POSITION&&valBytes>=4){uint32_t pos;memcpy(&pos,p,4);cs.pos=pos;}
                else if(flag==QUERY_COUNTERS&&valBytes>=4){
                    // value: uint32 count, then count * uint32 (low16 = counter
                    // type, high16 = number of that type). Sum to a total.
                    uint32_t cc; memcpy(&cc,p,4);
                    uint32_t total=0;
                    for(uint32_t i=0;i<cc && (int)(4+i*4+4)<=valBytes;++i){
                        uint32_t v; memcpy(&v,p+4+i*4,4);
                        total += (v>>16);
                    }
                    cs.counters=(uint16_t)total;
                }
                p+=valBytes;
            }
            if(!gotEnd) break;
            if(cs.code!=0) out.push_back(cs);
            ++seq;
        }
        return out;
    };

    auto queryLoc = [&](uint8_t con, uint32_t loc) -> std::vector<CardState> {
        OCG_QueryInfo qi{}; qi.flags=Q_FLAGS; qi.con=con; qi.loc=loc;
        uint32_t len=0; void* buf=OCG_DuelQueryLocation(m_duel,&len,&qi);
        return parseBuffer(buf,len,con,loc);
    };

    for (int pl=0; pl<2; ++pl) {
        m_field.hand[pl]     = queryLoc(pl, LOCATION_HAND);
        m_field.monsters[pl] = queryLoc(pl, LOCATION_MZONE);
        m_field.spells[pl]   = queryLoc(pl, LOCATION_SZONE);
        m_field.gy[pl]       = queryLoc(pl, LOCATION_GRAVE);
        m_field.banished[pl] = queryLoc(pl, LOCATION_REMOVED);
        m_field.deckCount[pl]  = (int)OCG_DuelQueryCount(m_duel, pl, LOCATION_DECK);
        m_field.extraCount[pl] = (int)OCG_DuelQueryCount(m_duel, pl, LOCATION_EXTRA);
    }
    validateState();
}

// Override table lookup + real card type. See header.
bool DuelManager::isExtraDeckCard(uint32_t code) const {
    // VERIFIED Extra-Deck cards that may be absent from a stale runtime
    // cards.cdb. With the BabelCDB-master databases loaded these are normally
    // resolved by real type anyway — this table is only a backstop. Both are
    // confirmed Synchro in BabelCDB-master/release-blzd.cdb.
    // NOTE: 17209452 "Kewl Tune Rotary" is type 0x1021 — a MAIN-deck tuner —
    // and is deliberately NOT listed here (an earlier guess routed it wrong).
    static const uint32_t kExtraOverride[] = {
        39576656u,  // "Kewl Tune Crackle"     — Synchro
        65961304u,  // "Kewl Tune Back 2 Back" — Synchro
        0u
    };
    for (int i = 0; kExtraOverride[i]; ++i)
        if (kExtraOverride[i] == code) return true;
    const uint32_t EX = TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ | TYPE_LINK;
    return (m_db.getCard(code).type & EX) != 0;
}

// Sanity check over the tracked field state. The Extra-Deck-in-hand check runs
// always and PAUSES the duel; the rest is debug-only. Never crashes the app.
void DuelManager::validateState() {
    int errors = 0;
    auto err = [&](const std::string& s){ addLog("[STATE ERROR] "+s); ++errors; };
    for (int pl=0; pl<2; ++pl) {
        std::string P = "P" + std::to_string(pl+1) + " ";
        // ALWAYS checked: an Extra-Deck card in the hand is illegal. Warn
        // loudly. The actual pause is done by the MSG_DRAW handler, which can
        // tell a normal draw from a legitimate effect.
        for (auto& c : m_field.hand[pl]) {
            if (isExtraDeckCard(c.code)) {
                CardInfo ci = m_db.getCard(c.code);
                err(P + "Extra-Deck card #" + std::to_string(c.code) + " [" +
                    (ci.name.empty() ? "unknown" : ci.name) + "] is in the HAND");
            }
        }
        if (!m_debugMsgs) continue;   // remaining checks are debug-only
        for (auto& c : m_field.monsters[pl]) {
            CardInfo ci = m_db.getCard(c.code);
            if (c.seq > 6)
                err(P+"card "+std::to_string(c.code)+" in invalid MZONE seq="+
                    std::to_string(c.seq));
            if (ci.id && (ci.type & (TYPE_SPELL|TYPE_TRAP)) && !(ci.type & TYPE_MONSTER))
                err(P+"Spell/Trap "+std::to_string(c.code)+" ["+ci.name+
                    "] is in a Monster Zone");
        }
        for (auto& c : m_field.spells[pl]) {
            CardInfo ci = m_db.getCard(c.code);
            if (c.seq > 7)
                err(P+"card "+std::to_string(c.code)+" in invalid SZONE seq="+
                    std::to_string(c.seq));
            if (ci.id && (ci.type & TYPE_FIELD) && c.seq != 5)
                err(P+"Field Spell "+std::to_string(c.code)+" ["+ci.name+
                    "] in SZONE seq="+std::to_string(c.seq)+" (should be 5)");
            if (ci.id && (ci.type & TYPE_MONSTER) &&
                !(ci.type & (TYPE_SPELL|TYPE_TRAP)) && c.seq < 5)
                err(P+"Monster "+std::to_string(c.code)+" ["+ci.name+
                    "] is in a Spell/Trap Zone");
        }
    }
    (void)errors;   // errors are logged individually; silence when state is clean
}

// Built-in test AI for player 1 in local/offline mode. Picks a safe, legal
// default so the duel always progresses. Returns true if a response was set.
// (submitResponse clears m_selection, so capture what we need up front.)
// ── Offline opponent AI ──────────────────────────────────────────────────────
//
// A lightweight heuristic AI for solo practice. It plays only from the actions
// the engine actually offers (m_selection.idle), so it can never make an illegal
// move. Strategy, in priority order:
//   Main Phase : activate one un-used effect -> Special Summon (highest ATK) ->
//                Normal Summon (highest ATK) -> Set a Spell/Trap -> Battle (if
//                it has a monster) -> End.
//   Battle     : attack directly when possible; otherwise attack with the
//                strongest monster only when it can beat the opponent's weakest
//                (avoids throwing monsters away); else end the Battle Phase.
// A per-turn guard stops it re-activating the same effect forever; the engine's
// own retry cap is the final backstop.
bool DuelManager::aiIdlePhase() {
    // Goldfish: a passive opponent develops no board — it ends its turn
    // immediately so the human can practise combos uninterrupted.
    if (m_passiveAI) { respondIdleCmd(7, 0); return true; }
    const std::vector<IdleAction>& idle = m_selection.idle;
    const int aiSeat = m_selection.player & 1;

    if (m_field.turnCount != m_aiTurnSeen) {
        m_aiTurnSeen = m_field.turnCount;
        m_aiDoneThisTurn.clear();
    }
    auto atkOf = [&](uint32_t code){ return m_db.getCard(code).atk; };

    int  spIdx = -1, spAtk = -1;     // best Special Summon
    int  nsIdx = -1, nsAtk = -1;     // best Normal Summon
    int  actIdx = -1; uint64_t actKey = 0;   // first un-used activation
    int  setIdx = -1;                // first Spell/Trap set
    for (int i = 0; i < (int)idle.size(); ++i) {
        const IdleAction& a = idle[i];
        switch (a.cmd) {
            case 1: {
                // Special Summon. Attempt each card at most once per turn:
                // keyed by code so a summon the AI cannot satisfy (e.g. it can't
                // pick legal Synchro materials and ends up cancelling) is not
                // re-offered and re-attempted on the very next idle loop.
                uint64_t key = (uint64_t)a.code | (1ull << 40);   // tag: SP summon
                bool used = false;
                for (uint64_t k : m_aiDoneThisTurn) if (k == key) { used = true; break; }
                if (used) break;
                int v = atkOf(a.code); if (v > spAtk) { spAtk = v; spIdx = i; }
                break;
            }
            case 0: { int v = atkOf(a.code); if (v > nsAtk) { nsAtk = v; nsIdx = i; } break; }
            case 5: {
                // Key on the card code alone: the AI activates each card's
                // effect at most once per turn, which prevents a single card's
                // ignition effect from re-listing and looping. Also cap total
                // activations per turn so the AI develops a board instead of
                // spiralling into a huge, chaotic combo.
                if ((int)m_aiDoneThisTurn.size() >= 4) break;
                uint64_t key = (uint64_t)a.code;
                bool used = false;
                for (uint64_t k : m_aiDoneThisTurn) if (k == key) { used = true; break; }
                if (!used && actIdx < 0) { actIdx = i; actKey = key; }
                break;
            }
            case 4: if (setIdx < 0) setIdx = i; break;
            default: break;   // skip reposition / set-monster
        }
    }

    if (actIdx >= 0) {
        m_aiDoneThisTurn.push_back(actKey);
        respondIdleCmd(idle[actIdx].cmd, idle[actIdx].index); return true;
    }
    if (spIdx  >= 0) {
        m_aiDoneThisTurn.push_back((uint64_t)idle[spIdx].code | (1ull << 40));
        respondIdleCmd(idle[spIdx].cmd, idle[spIdx].index); return true;
    }
    if (nsIdx  >= 0) { respondIdleCmd(idle[nsIdx].cmd,  idle[nsIdx].index);  return true; }
    if (setIdx >= 0) { respondIdleCmd(idle[setIdx].cmd, idle[setIdx].index); return true; }
    if (m_selection.toBP && !m_field.monsters[aiSeat].empty()) {
        respondIdleCmd(6, 0); return true;   // -> Battle Phase
    }
    respondIdleCmd(7, 0); return true;        // -> End Phase
}

bool DuelManager::aiBattlePhase() {
    if (m_passiveAI) { respondIdleCmd(3, 0); return true; }   // end Battle Phase
    const std::vector<IdleAction>& idle = m_selection.idle;
    const int aiSeat  = m_selection.player & 1;
    const int oppSeat = aiSeat ^ 1;
    auto statOf = [&](const CardState& c) -> int {
        CardInfo ci = m_db.getCard(c.code);
        // Defence-position monsters block with DEF, attack-position with ATK.
        return (c.pos & (POS_FACEUP_DEFENSE | POS_FACEDOWN_DEFENSE)) ? ci.def
                                                                     : ci.atk;
    };

    int directIdx = -1;
    int bestIdx = -1, bestAtk = -1;
    for (int i = 0; i < (int)idle.size(); ++i) {
        const IdleAction& a = idle[i];
        if (a.cmd != 1) continue;                  // only attack actions
        if (a.canDirect && directIdx < 0) directIdx = i;
        int v = m_db.getCard(a.code).atk;
        if (v > bestAtk) { bestAtk = v; bestIdx = i; }
    }

    if (directIdx >= 0) { respondIdleCmd(1, idle[directIdx].index); return true; }
    if (bestIdx >= 0) {
        int oppMin = 0x7fffffff;
        for (const CardState& m : m_field.monsters[oppSeat]) {
            int s = statOf(m);
            if (s < oppMin) oppMin = s;
        }
        // Attack only when nothing's there or we can beat the weakest blocker.
        if (m_field.monsters[oppSeat].empty() || bestAtk > oppMin) {
            respondIdleCmd(1, idle[bestIdx].index); return true;
        }
    }
    respondIdleCmd(3, 0); return true;            // -> End Battle Phase
}

bool DuelManager::autoRespondP2() {
    const WaitType type     = m_selection.type;
    const bool     forced   = m_selection.forced;
    const bool     hasCards = !m_selection.cards.empty();

    if (m_debugMsgs)
        addLog(std::string("[dbg] auto-AI (P2) <- ") + waitName(type));

    // ── Hard per-turn loop backstop ──────────────────────────────────────
    // Count every AI response this turn. A real combo rarely exceeds a few
    // dozen; if we blow past the cap an effect is looping, so bail out of the
    // current window with the most passive legal answer instead of forever.
    if (m_field.turnCount != m_aiActionTurn) {
        m_aiActionTurn = m_field.turnCount;
        m_aiActionsThisTurn = 0;
    }
    if (++m_aiActionsThisTurn > 120) {
        if (m_aiActionsThisTurn == 121)
            addLog("[auto-AI] loop guard tripped — bailing out of the phase");
        switch (type) {
            case WaitType::SelectIdleCmd:   respondIdleCmd(7, 0); return true;
            case WaitType::SelectBattleCmd: respondIdleCmd(3, 0); return true;
            case WaitType::SelectChain:     respondChain(forced ? 0 : -1); return true;
            case WaitType::SelectYesNo:
            case WaitType::SelectEffectYn:  respondYesNo(false); return true;
            case WaitType::SelectOption:    respondInt(0); return true;
            case WaitType::SelectCard:
            case WaitType::SelectTribute:
                if (hasCards) respondSingleCard(0); else respondInt(0);
                return true;
            // Finish/cancel the material selection (-1) so an unsatisfiable
            // summon is abandoned instead of re-asked forever.
            case WaitType::SelectUnselect:  respondInt(-1); return true;
            default:                        return false;
        }
    }

    switch (type) {
    case WaitType::SelectIdleCmd:    return aiIdlePhase();
    case WaitType::SelectBattleCmd:  return aiBattlePhase();
    case WaitType::SelectYesNo:      respondYesNo(false); return true;  // generic — decline
    // Decline optional trigger effects. (Activating them here could re-open the
    // same window and loop; the AI still uses ignition effects via the Main
    // Phase activate path, which is per-turn guarded.)
    case WaitType::SelectEffectYn:   respondYesNo(false); return true;
    case WaitType::SelectOption:     respondInt(0);        return true;
    case WaitType::SelectCard: {
        if (!hasCards) { respondInt(0); return true; }
        const int aiSeat = m_selection.player & 1;
        // If every option is an OPPONENT card, this is almost certainly a
        // removal / attack target — take the strongest. Otherwise (own cards:
        // costs, summon material, etc.) take the first valid one.
        bool allOpp = true;
        for (const CardState& c : m_selection.cards)
            if ((c.player & 1) == aiSeat) { allOpp = false; break; }
        int pick = 0;
        if (allOpp) {
            int best = -1;
            for (int i = 0; i < (int)m_selection.cards.size(); ++i) {
                int v = m_db.getCard(m_selection.cards[i].code).atk;
                if (v > best) { best = v; pick = i; }
            }
        }
        respondSingleCard(pick); return true;
    }
    case WaitType::SelectTribute: {
        if (!hasCards) { respondInt(0); return true; }
        // Tribute the weakest available monster (least to lose).
        int pick = 0, worst = 0x7fffffff;
        for (int i = 0; i < (int)m_selection.cards.size(); ++i) {
            int v = m_db.getCard(m_selection.cards[i].code).atk;
            if (v < worst) { worst = v; pick = i; }
        }
        respondSingleCard(pick); return true;
    }
    case WaitType::SelectChain:
        respondChain(forced ? 0 : -1);   // never start a chain unless forced
        return true;
    case WaitType::SelectUnselect:
        // Material selection during the AI's own summon (Synchro/Link/Xyz/
        // Fusion). The engine re-asks one card at a time and only sets
        // `finishable` once the current picks form a LEGAL material set.
        //   - finishable        -> finish (respond -1): we have a valid set.
        //   - still has options  -> pick one (build toward a legal set). The
        //     engine only offers cards that can still lead to a legal sum, so
        //     greedily taking the first offered card converges on a real summon
        //     instead of cancelling and re-attempting it forever.
        //   - nothing offered & not finishable -> give up (respond -1).
        // Previously a *cancelable* window made the AI bail immediately, so it
        // never completed an Extra-Deck summon and looped idle<->material.
        if (m_selection.finishable) {
            respondInt(-1);                                // legal set -> done
        } else if (hasCards) {
            // Take the WEAKEST eligible card as material so the AI spends its
            // cheap monsters first instead of feeding its strongest into a
            // summon. The engine only offers cards that still allow a legal set,
            // so any pick converges — weakest is just the smarter one.
            int pick = 0, worst = 0x7fffffff;
            for (int i = 0; i < (int)m_selection.cards.size(); ++i) {
                int v = m_db.getCard(m_selection.cards[i].code).atk;
                if (v < worst) { worst = v; pick = i; }
            }
            respondUnselect(pick);
        } else {
            respondInt(-1);                                // unsatisfiable -> drop
        }
        return true;
    case WaitType::SelectPosition: { int32_t pos = POS_FACEUP_ATTACK; submitResponse(&pos, 4); return true; }
    case WaitType::None:
    case WaitType::Waiting:
        return false;                    // nothing actionable
    default:
        // Unknown request type: do NOT guess a response. Returning false lets
        // process() pause the duel safely instead of risking invalid state.
        return false;
    }
}

void DuelManager::addLog(const std::string& s) {
    m_log.push_back(s);
    if(m_log.size()>200) m_log.erase(m_log.begin());
}

// Log a line at most once per distinct message. Used for unresolved-request
// warnings so an engine state that persists across frames is not re-logged
// every frame. m_lastReqLog is cleared whenever the engine makes progress
// (see the CONTINUE branch of process()).
void DuelManager::logRequestOnce(const std::string& s) {
    if (s == m_lastReqLog) return;
    m_lastReqLog = s;
    addLog(s);
}

// Decode an ocgcore effect-description value. Card-specific effect strings are
// encoded by Lua's aux.Stringid(code,id). MODERN cores (EDOPro / ProjectIgnis)
// encode this as (code << 20) | id; very old cores used code*16+id. We try
// both (validating the high part as a real card code), then fall back to the
// "!system" string table from assets/config/strings.conf for plain hint ids.
EffectDesc DuelManager::decodeDesc(uint64_t raw) const {
    EffectDesc d;
    d.raw = raw;
    if (raw == 0) return d;
    const char* method = "fallback";

    // (1) EDOPro-core encoding: (code << 20) | stringIndex, idx 0..15 maps
    //     onto texts.str1..str16. Example from a live log: Vanquish Soul
    //     Razen desc=30726273630210 = (29302858 << 20) | 2 → texts.str3.
    {
        uint64_t code64 = raw >> 20;
        int      idx    = (int)(raw & 0xFFFFF);
        if (code64 > 0 && code64 <= 0xFFFFFFFFull && idx >= 0 && idx < 16) {
            CardInfo ci = m_db.getCard((uint32_t)code64);
            if (ci.id != 0) {
                d.isCardString = true;
                d.cardCode     = (uint32_t)code64;
                d.index        = idx;
                d.text         = m_db.cardString(d.cardCode, idx);
                if (d.text.empty()) d.text = ci.name;  // name as last resort
                method = "card-string<<20";
            }
        }
    }
    // (2) Legacy encoding: code*16 + idx — only consulted when (1) found no
    //     real card, and only for values small enough to be plausible.
    if (!d.isCardString && (raw >> 36) == 0) {
        uint32_t code = (uint32_t)(raw >> 4);
        int      idx  = (int)(raw & 0xF);
        if (code != 0) {
            CardInfo ci = m_db.getCard(code);
            if (ci.id != 0) {
                d.isCardString = true;
                d.cardCode     = code;
                d.index        = idx;
                d.text         = m_db.cardString(code, idx);
                if (d.text.empty()) d.text = ci.name;
                method = "card-string*16";
            }
        }
    }
    // (3) System / hint string (strings.conf "!system <id> <text>").
    if (!d.isCardString && raw <= 0xFFFFF) {
        std::string sys = m_db.systemString((uint32_t)raw);
        if (!sys.empty()) {
            d.text = sys;
            method = "system";
        }
    }
    // Decode audit. Successful decodes log only in debug-message mode;
    // FAILURES always log — those are exactly the "Unknown option" cases
    // that need catching in the field.
    if (m_debugMsgs || d.text.empty()) {
        auto* self = const_cast<DuelManager*>(this);
        self->addLog("[DESC DECODE] desc=" + std::to_string(raw) +
                     "  method=" + method +
                     (d.isCardString
                          ? ("  card=#" + std::to_string(d.cardCode) +
                             "  strIdx=" + std::to_string(d.index))
                          : std::string()) +
                     "  result=" + (d.text.empty() ? "(empty)" : d.text));
    }
    return d;
}

const char* DuelManager::timingName(TimingContext t) {
    switch (t) {
    case TimingContext::PostSummonTrigger:   return "post-summon trigger window";
    case TimingContext::IdleMainPhase:       return "idle Main Phase effect";
    case TimingContext::QuickChainWindow:    return "quick chain window";
    case TimingContext::OpponentChainWindow: return "opponent chain window";
    case TimingContext::Unknown:             break;
    }
    return "unknown";
}

// Arm the post-summon trace. Called from MSG_*SUMMONED, once the Summon has
// resolved and ocgcore is about to raise EVENT_SUMMON_SUCCESS. The summoned
// card was captured earlier from MSG_*SUMMONING.
void DuelManager::beginPostSummonTrace() {
    if (m_pendingSummonCode == 0) return;
    m_postSummonPending   = true;
    m_postSummonChainSeen = false;
    m_postSummonEffYnSeen = false;
    m_postSummonMsgTrail.clear();
    m_traceSummonCode   = m_pendingSummonCode;
    m_traceSummonName   = m_pendingSummonName;
    m_traceSummonType   = m_pendingSummonType;
    m_traceSummonPlayer = m_pendingSummonPlayer;
    m_pendingSummonCode = 0;

    const char* st = m_traceSummonType == 0 ? "Normal"
                   : m_traceSummonType == 1 ? "Special" : "Flip";
    addLog("[POST-SUMMON TRACE START]");
    addLog("  summoned card: #" + std::to_string(m_traceSummonCode) +
           " [" + m_traceSummonName + "]  (player " +
           std::to_string(m_traceSummonPlayer + 1) + ")");
    addLog(std::string("  summon type: ") + st);
    // Report the card's database source and whether its Lua script exists.
    // A missing script means the card loaded as a vanilla body and CANNOT
    // register any trigger — the usual cause of a "newer card has no effect".
    {
        CardInfo tci = m_db.getCard(m_traceSummonCode);
        std::string sn   = "c" + std::to_string(m_traceSummonCode) + ".lua";
        std::string path = findCardScript(m_traceSummonCode);
        addLog(std::string("  CDB source: ") +
               (tci.id == 0 ? "(card NOT in any database!)"
                            : (tci.source.empty() ? "(unknown)" : tci.source)) +
               "  ot=" + std::to_string(tci.ot));
        if (path.empty()) {
            addLog("  script: " + sn + "  [MISSING]");
            addLog("  [NEW CARD DEBUG] this card has NO script — it loaded as "
                   "a vanilla body and CANNOT register any trigger/search "
                   "effect. Update assets/scripts to the same ProjectIgnis "
                   "snapshot as cards.cdb.");
        } else {
            addLog("  script: " + path + "  [found]");
        }
    }

    // Debug-only: when a "Vanquish Soul" monster (set code 0x196) is summoned,
    // enumerate the controller's Deck and classify every card against Razen's
    // on-summon search filter, so a 0-option chain window can be traced to
    // either "no legal target in Deck" or a card-metadata problem.
    if (m_debugMsgs) {
        CardInfo sci = m_db.getCard(m_traceSummonCode);
        bool isVanquishSoul = false;
        for (int i = 0; i < 4; ++i)
            if (((sci.setcode >> (i * 16)) & 0xFFFF) == 0x196)
                isVanquishSoul = true;
        if (isVanquishSoul)
            logRazenTargetCheck(m_traceSummonPlayer);
    }
}

// Query the card codes in one zone. Mirrors queryField()'s buffer parser but
// keeps only the codes. Diagnostics only — never drives the duel.
std::vector<uint32_t> DuelManager::queryLocationCodes(uint8_t con, uint32_t loc) {
    std::vector<uint32_t> out;
    if (!m_duel) return out;
    OCG_QueryInfo qi{}; qi.flags = QUERY_CODE; qi.con = con; qi.loc = loc;
    uint32_t len = 0;
    void* raw = OCG_DuelQueryLocation(m_duel, &len, &qi);
    if (!raw || len < 4) return out;
    const uint8_t* p   = static_cast<const uint8_t*>(raw);
    const uint8_t* end = p + len;
    p += 4;                                       // leading uint32 total size
    while (p + 2 <= end) {
        uint16_t firstLen; memcpy(&firstLen, p, 2);
        if (firstLen == 0) { p += 2; continue; }  // empty slot
        uint32_t code = 0;
        bool gotEnd = false;
        while (p + 6 <= end) {
            uint16_t entrySize; memcpy(&entrySize, p, 2); p += 2;
            uint32_t flag; memcpy(&flag, p, 4);
            if (flag == QUERY_END) { p += 4; gotEnd = true; break; }
            int valBytes = (int)entrySize - 4;
            if (valBytes < 0 || p + 4 + valBytes > end) { p = end; gotEnd = true; break; }
            p += 4;
            if (flag == QUERY_CODE && valBytes >= 4) memcpy(&code, p, 4);
            p += valBytes;
        }
        if (!gotEnd) break;
        if (code != 0) out.push_back(code);
    }
    return out;
}

// Debug-only diagnostic. Enumerates `player`'s Main Deck and classifies each
// card against Vanquish Soul Razen's on-summon search filter (s.thfilter in
// c29302858.lua): a MONSTER, set code "Vanquish Soul" (0x196), NOT a Warrior
// (race 0x1). This is pure logging — it mirrors the script filter so a
// 0-option post-summon chain window can be explained, and does NOT change any
// engine legality decision.
void DuelManager::logRazenTargetCheck(uint8_t player) {
    const uint16_t SET_VANQUISH_SOUL = 0x196;
    addLog("[RAZEN TARGET CHECK]");
    addLog("  filter mirrored: monster + set 0x196 (Vanquish Soul) + "
           "race != 0x1 (Warrior), still in Deck");
    std::vector<uint32_t> deck = queryLocationCodes(player, LOCATION_DECK);
    addLog("  P" + std::to_string(player + 1) + " Deck cards remaining: " +
           std::to_string(deck.size()));

    int valid = 0, rejected = 0, nonVS = 0;
    for (uint32_t code : deck) {
        CardInfo ci = m_db.getCard(code);
        bool known = (ci.id != 0);
        bool isVS  = false;
        for (int i = 0; i < 4; ++i)
            if (((ci.setcode >> (i * 16)) & 0xFFFF) == SET_VANQUISH_SOUL)
                isVS = true;
        // Detail only Vanquish-Soul-relevant cards: anything with the set
        // code OR a name containing "Vanquish Soul" (the latter catches a
        // card whose name is right but whose set-code metadata is wrong).
        bool nameSaysVS = ci.name.find("Vanquish Soul") != std::string::npos;
        if (!isVS && !nameSaysVS) { ++nonVS; continue; }

        bool isMonster = (ci.type & TYPE_MONSTER) != 0;
        bool isWarrior = (ci.race & RACE_WARRIOR) != 0;
        char sc[24]; snprintf(sc, sizeof(sc), "0x%llx",
                              (unsigned long long)ci.setcode);
        char tp[16]; snprintf(tp, sizeof(tp), "0x%x", (unsigned)ci.type);
        std::string base = "#" + std::to_string(code) + " [" +
            (ci.name.empty() ? "unknown" : ci.name) + "] type=" + tp +
            " race=" + std::to_string(ci.race) + " setcode=" + sc +
            " src=" + (ci.source.empty() ? "(none)" : ci.source);

        std::string reason;
        if      (!known)              reason = "missing DB data";
        else if (!isVS && nameSaysVS) reason = "name says Vanquish Soul but "
                                               "set-code metadata is WRONG";
        else if (!isMonster)          reason = "not a monster";
        else if (isWarrior)           reason = "is a Warrior (excluded)";
        if (reason.empty()) {
            ++valid;
            addLog("  VALID target  : " + base);
        } else {
            ++rejected;
            addLog("  rejected      : " + base + "  -> " + reason);
        }
    }
    addLog("  summary: valid non-Warrior Vanquish Soul targets in Deck = " +
           std::to_string(valid) + ", rejected Vanquish Soul cards = " +
           std::to_string(rejected) + ", other cards = " +
           std::to_string(nonVS));
    if (valid == 0)
        addLog("  >>> 0 valid targets in Deck — Razen's search trigger is "
               "LEGALLY unavailable (engine correctly offers 0 options).");
    else
        addLog("  >>> " + std::to_string(valid) + " valid target(s) exist — "
               "if the trigger is still not offered, the cause is metadata "
               "(set codes) or rule options, not the deck.");
    addLog("[RAZEN TARGET CHECK END]");
}

// Flush the body + end of the post-summon trace. `optionLines` are the
// already-formatted "effect options for YOU" lines (empty -> none offered).
// Safe to call unconditionally — a no-op when no trace is armed.
void DuelManager::endPostSummonTrace(const char* concludedBy,
                                     const std::vector<std::string>& optionLines,
                                     const std::string& autoPassNote) {
    if (!m_postSummonPending) return;
    m_postSummonPending = false;
    addLog(std::string("  next ocgcore messages: ") +
           (m_postSummonMsgTrail.empty() ? "(none)" : m_postSummonMsgTrail));
    addLog(std::string("  concluded by: ") + concludedBy);
    addLog(std::string("  MSG_SELECT_CHAIN occurred: ") +
           (m_postSummonChainSeen ? "yes" : "no"));
    addLog(std::string("  MSG_SELECT_EFFECTYN occurred: ") +
           (m_postSummonEffYnSeen ? "yes" : "no"));
    if (optionLines.empty()) {
        addLog("  effect options for YOU: (none)");
    } else {
        addLog("  effect options for YOU:");
        for (auto& l : optionLines) addLog("    " + l);
    }
    addLog(std::string("  auto-pass: ") + autoPassNote);
    addLog("[POST-SUMMON TRACE END]");
    m_postSummonMsgTrail.clear();
}

const char* DuelManager::waitName(WaitType t) {
    switch (t) {
    case WaitType::None:            return "None";
    case WaitType::SelectIdleCmd:   return "SelectIdleCmd";
    case WaitType::SelectBattleCmd: return "SelectBattleCmd";
    case WaitType::SelectYesNo:     return "SelectYesNo";
    case WaitType::SelectEffectYn:  return "SelectEffectYn";
    case WaitType::SelectOption:    return "SelectOption";
    case WaitType::SelectCard:      return "SelectCard";
    case WaitType::SelectChain:     return "SelectChain";
    case WaitType::SelectPlace:     return "SelectPlace";
    case WaitType::SelectPosition:  return "SelectPosition";
    case WaitType::SelectTribute:   return "SelectTribute";
    case WaitType::SelectCounter:   return "SelectCounter";
    case WaitType::SelectSum:       return "SelectSum";
    case WaitType::SelectUnselect:  return "SelectUnselect";
    case WaitType::Waiting:         return "Waiting";
    }
    return "?";
}

// Human-readable text for the MSG_WIN reason byte. ocgcore mainly distinguishes
// "life points reached 0" from "deck-out"; other reasons are effect-driven.
const char* DuelManager::winReasonText(int reason) {
    switch (reason) {
    case 0:  return "Life Points reached 0";
    case 1:  return "deck-out — a player could not draw";
    case 2:  return "deck-out";
    case 3:  return "a card's win condition";
    case 4:  return "surrender";
    default: return "duel ended";
    }
}

std::string DuelManager::msgName(uint8_t type) {
    const char* n = nullptr;
    switch (type) {
    case MSG_RETRY:            n="MSG_RETRY";            break;
    case MSG_HINT:             n="MSG_HINT";             break;
    case MSG_WAITING:          n="MSG_WAITING";          break;
    case MSG_START:            n="MSG_START";            break;
    case MSG_WIN:              n="MSG_WIN";              break;
    case MSG_NEW_TURN:         n="MSG_NEW_TURN";         break;
    case MSG_NEW_PHASE:        n="MSG_NEW_PHASE";        break;
    case MSG_DRAW:             n="MSG_DRAW";             break;
    case MSG_MOVE:             n="MSG_MOVE";             break;
    case MSG_SELECT_IDLECMD:   n="MSG_SELECT_IDLECMD";   break;
    case MSG_SELECT_BATTLECMD: n="MSG_SELECT_BATTLECMD"; break;
    case MSG_SELECT_EFFECTYN:  n="MSG_SELECT_EFFECTYN";  break;
    case MSG_SELECT_YESNO:     n="MSG_SELECT_YESNO";     break;
    case MSG_SELECT_OPTION:    n="MSG_SELECT_OPTION";    break;
    case MSG_SELECT_CARD:      n="MSG_SELECT_CARD";      break;
    case MSG_SELECT_CHAIN:     n="MSG_SELECT_CHAIN";     break;
    case MSG_SELECT_PLACE:     n="MSG_SELECT_PLACE";     break;
    case MSG_SELECT_POSITION:  n="MSG_SELECT_POSITION";  break;
    case MSG_SELECT_TRIBUTE:   n="MSG_SELECT_TRIBUTE";   break;
    case MSG_SELECT_COUNTER:   n="MSG_SELECT_COUNTER";   break;
    case MSG_SELECT_SUM:       n="MSG_SELECT_SUM";       break;
    case MSG_SELECT_DISFIELD:  n="MSG_SELECT_DISFIELD";  break;
    case MSG_SELECT_UNSELECT_CARD: n="MSG_SELECT_UNSELECT_CARD"; break;
    case MSG_SORT_CARD:        n="MSG_SORT_CARD";        break;
    case MSG_SORT_CHAIN:       n="MSG_SORT_CHAIN";       break;
    }
    if (n) return n;
    return "MSG_" + std::to_string((int)type);
}

// The folders a card/init script may live in — shared by scriptReaderCb (which
// loads the bytes) and findCardScript (which only tests existence). Keep these
// two in sync.
static const char* const kScriptDirs[] = {
    "assets/scripts/",
    "assets/scripts/official/",
    "assets/scripts/unofficial/",
    "assets/scripts/rush/",
    "assets/scripts/skill/",
    "assets/scripts/goat/",
    "assets/scripts/pre-errata/",
    "../../../assets/scripts/",
    "../../../assets/scripts/official/",
    "../../../assets/scripts/unofficial/",
    nullptr
};

// Resolve "c<code>.lua" to a path on disk, or "" if no folder has it. A card
// in cards.cdb whose script is missing loads as a vanilla body with NO
// effects — exactly why a newer card (e.g. Lunalight Gold Leo #8379983,
// Kewl Tune Reco #89392810) shows no on-summon trigger even though the card
// database knows the card.
std::string DuelManager::findCardScript(uint32_t code) {
    std::string fn = "c" + std::to_string(code) + ".lua";
    for (int i = 0; kScriptDirs[i]; ++i) {
        std::ifstream f(std::string(kScriptDirs[i]) + fn, std::ios::binary);
        if (f.good()) return std::string(kScriptDirs[i]) + fn;
    }
    return std::string();
}

// Duel-start audit: list every distinct deck card whose script is missing.
// This surfaces a script-collection that is out of date relative to cards.cdb
// BEFORE the duel, instead of waiting for a silent no-effect card mid-game.
void DuelManager::auditDeckScripts(const Deck& deck, int playerNo) {
    std::vector<uint32_t> seen;
    std::vector<std::pair<uint32_t, std::string>> missing;
    auto check = [&](uint32_t code) {
        for (uint32_t s : seen) if (s == code) return;
        seen.push_back(code);
        if (!findCardScript(code).empty()) return;
        // Follow the alias before declaring a card script-less: ocgcore loads an
        // alias card's script from its base code, so an aliased reprint (e.g.
        // Foolish Burial 81439174 -> 81439173) DOES have effects. Only the
        // literal-code check false-flagged it.
        uint32_t alias = m_db.getCard(code).alias;
        if (alias && alias != code && !findCardScript(alias).empty()) return;
        missing.push_back({ code, m_db.getCard(code).name });
    };
    for (uint32_t c : deck.main)  check(c);
    for (uint32_t c : deck.extra) check(c);

    if (missing.empty()) {
        addLog("[DECK SCRIPT AUDIT] P" + std::to_string(playerNo) + ": all " +
               std::to_string(seen.size()) +
               " distinct card(s) have a script.");
        return;
    }
    addLog("[DECK SCRIPT AUDIT] P" + std::to_string(playerNo) + ": " +
           std::to_string(missing.size()) + " of " +
           std::to_string(seen.size()) + " distinct card(s) have NO script "
           "-> those cards load as vanilla bodies with NO effects "
           "(no triggers, no searches, no activations):");
    for (auto& m : missing)
        addLog("  MISSING script c" + std::to_string(m.first) + ".lua  [" +
               (m.second.empty() ? "unknown — also absent from cards.cdb"
                                  : m.second) + "]");
    addLog("[DECK SCRIPT AUDIT] fix: update the assets/scripts collection to "
           "the same ProjectIgnis snapshot as cards.cdb.");
}

int DuelManager::scriptReaderCb(void* payload, OCG_Duel duel, const char* name) {
    auto* self = static_cast<DuelManager*>(payload);
    // Search EVERY script collection. Custom / unofficial card scripts — and
    // proc_unofficial.lua — live under scripts/unofficial; omitting that
    // folder is exactly why those cards loaded with no effects (no on-summon
    // trigger, Quick-Plays only settable, etc.). Parent-relative paths cover
    // running the exe from build/windows/Release.
    static const char* dirs[] = {
        "assets/scripts/",
        "assets/scripts/official/",
        "assets/scripts/unofficial/",
        "assets/scripts/rush/",
        "assets/scripts/skill/",
        "assets/scripts/goat/",
        "assets/scripts/pre-errata/",
        "../../../assets/scripts/",
        "../../../assets/scripts/official/",
        "../../../assets/scripts/unofficial/",
        nullptr
    };
    int folderCount = 0;
    while (dirs[folderCount]) ++folderCount;
    for(int i=0;dirs[i];++i){
        std::string path=std::string(dirs[i])+name;
        std::ifstream f(path,std::ios::binary);
        if(!f) continue;
        f.seekg(0,std::ios::end); auto sz=(uint32_t)f.tellg(); f.seekg(0);
        std::string buf(sz,'\0'); if(sz) f.read(&buf[0],sz);
        int r = OCG_LoadScript(duel,buf.c_str(),sz,name);
        // Proves the resolver loaded from the right subfolder. ocgcore asks
        // for each script once, so this is naturally first-load-only.
        if(self && self->m_debugMsgs)
            self->addLog(std::string("[SCRIPT LOAD] ")+name+" -> "+path+" ("+
                         std::to_string(sz)+" bytes, load="+(r?"ok":"FAIL")+")");
        return 1;
    }
    // Missing — log once per distinct script name per duel (no spam).
    if(self){
        bool seen=false;
        for(auto& s:self->m_missingScripts) if(s==name){ seen=true; break; }
        if(!seen){
            self->m_missingScripts.push_back(name);
            self->addLog(std::string("[SCRIPT MISSING] ")+name+" (searched "+
                         std::to_string(folderCount)+" folders) — no effects "
                         "for that card/procedure");
        }
    }
    return 0;
}

void DuelManager::logHandlerCb(void* payload, const char* msg, int /*type*/) {
    if(!msg) return;
    if(strstr(msg,"CallCardFunction")&&strstr(msg,"attempt to call")) return;
    if(strstr(msg,"attempt to call a nil value")) return;
    static_cast<DuelManager*>(payload)->addLog(std::string("[Engine] ")+msg);
}
