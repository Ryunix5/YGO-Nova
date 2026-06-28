#ifdef _MSC_VER
#  define _CRT_SECURE_NO_WARNINGS
#  pragma warning(disable: 4996)   // strncpy
#endif

#include "UI.h"
#include "UIStyle.h"
#include "AudioManager.h"
#include "Version.h"
#include "imgui.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <array>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <chrono>
#include <ctime>
#include <random>

// Forward decls for file-static helpers used before their definition.
static std::string presetLabel(const std::string& file);

// ─── Card type flags ──────────────────────────────────────────────────────────
static const uint32_t TYPE_MONSTER  = 0x1;
static const uint32_t TYPE_SPELL    = 0x2;
static const uint32_t TYPE_TRAP     = 0x4;
static const uint32_t TYPE_FUSION   = 0x40;
static const uint32_t TYPE_RITUAL   = 0x80;
static const uint32_t TYPE_SYNCHRO  = 0x2000;
static const uint32_t TYPE_XYZ      = 0x800000;
static const uint32_t TYPE_LINK     = 0x4000000;
static const uint32_t TYPE_PENDULUM = 0x1000000;

// ─── Colour palette ───────────────────────────────────────────────────────────
static const ImVec4 COL_ACCENT   = {0.85f, 0.70f, 0.20f, 1.f};
static const ImVec4 COL_P1       = {0.35f, 0.60f, 0.95f, 1.f};   // blue — local player
static const ImVec4 COL_P2       = {0.95f, 0.35f, 0.30f, 1.f};   // red  — opponent
static const ImVec4 COL_MONSTER  = {0.18f, 0.28f, 0.58f, 0.80f};
static const ImVec4 COL_SPELL    = {0.10f, 0.42f, 0.25f, 0.80f};
static const ImVec4 COL_TRAP     = {0.48f, 0.10f, 0.36f, 0.80f};
static const ImVec4 COL_EMPTY    = {0.10f, 0.12f, 0.19f, 0.90f};

// ─── Human-readable names for MP status lines ─────────────────────────────────
// Player-facing prompt-type names (the technical names live in
// DuelManager::waitName; these read like game terms, not enum tags).
static const char* waitTypeHuman(uint32_t wt) {
    switch ((WaitType)wt) {
        case WaitType::SelectIdleCmd:   return "Main Phase action";
        case WaitType::SelectBattleCmd: return "Battle Phase action";
        case WaitType::SelectYesNo:     return "Yes / No choice";
        case WaitType::SelectEffectYn:  return "effect activation";
        case WaitType::SelectOption:    return "effect choice";
        case WaitType::SelectCard:      return "card selection";
        case WaitType::SelectChain:     return "Chain Response";
        case WaitType::SelectPlace:     return "zone placement";
        case WaitType::SelectPosition:  return "battle position";
        case WaitType::SelectTribute:   return "Tribute selection";
        case WaitType::SelectCounter:   return "counter selection";
        case WaitType::SelectSum:       return "sum selection";
        case WaitType::SelectUnselect:  return "material selection";
        default:                        return "decision";
    }
}
// ─── PromptChoiceRow ──────────────────────────────────────────────────────────
// A selectable "action card" row for effect/chain choice lists: a bright
// title line plus the FULL effect description wrapped underneath. The row
// height adapts to the wrapped text, so long effect text is never cut off.
// Returns true when clicked; *outHovered reports hover for preview wiring.
static bool PromptChoiceRow(const char* id, const std::string& title,
                            const std::string& desc, float width,
                            bool* outHovered = nullptr) {
    // Thin wrapper over the design-system ActionCard so every choice list
    // in the app (effect options, chain picks, viewers) shares one look.
    return UIStyle::ActionCard(id, title.c_str(), desc.c_str(),
                               width, outHovered);
}

static const char* phaseHuman(uint16_t ph) {
    switch (ph) {
        case 0x01:  return "Draw Phase";
        case 0x02:  return "Standby Phase";
        case 0x04:  return "Main Phase 1";
        // Battle Phase sub-steps (start / step / damage / damage-calc /
        // battle) all read as "Battle Phase" to the player.
        case 0x08: case 0x10: case 0x20: case 0x40: case 0x80:
                    return "Battle Phase";
        case 0x100: return "Main Phase 2";
        case 0x200: return "End Phase";
        default:    return "—";
    }
}

// Uppercase phase label for the centre banner ("MAIN PHASE 1", "BATTLE
// PHASE", …). Distinct from phaseHuman so the HUD pill / info lines keep
// their title-case wording.
static const char* phaseBannerText(uint16_t ph) {
    switch (ph) {
        case 0x01:  return "DRAW PHASE";
        case 0x02:  return "STANDBY PHASE";
        case 0x04:  return "MAIN PHASE 1";
        case 0x08: case 0x10: case 0x20: case 0x40: case 0x80:
                    return "BATTLE PHASE";
        case 0x100: return "MAIN PHASE 2";
        case 0x200: return "END PHASE";
        default:    return "";
    }
}

// True for monster card types that live in the Extra Deck (used to colour /
// label the boss entrance as a Special Summon and pick the summon-type word).
static bool isExtraDeckType(uint32_t type) {
    return (type & (0x40u /*FUSION*/ | 0x2000u /*SYNCHRO*/ |
                    0x800000u /*XYZ*/ | 0x4000000u /*LINK*/)) != 0;
}

// ─── Constructor ─────────────────────────────────────────────────────────────
UI::UI(DuelManager& dm, CardDB& db, Renderer& rend, SnapshotManager& snap)
    : m_dm(dm), m_db(db), m_rend(rend), m_snap(snap)
{
    strncpy(m_deckNameBuf, "New Deck", sizeof(m_deckNameBuf) - 1);
    m_deckNameBuf[sizeof(m_deckNameBuf) - 1] = '\0';
    refreshDeckFiles();
}

// ─── Settings — load from disk, mirror into UI fields ────────────────────────
//
// Called once from Game::init after the audio device is open so the loaded
// mute/volume can be pushed into AudioManager immediately. The UI's local
// toggle fields are the source of truth at runtime; this routine copies the
// persisted values into them, and saveSettings() reverses the mirror before
// writing back.
// ─── Multiplayer plumbing ──────────────────────────────────────────────────
//
// Design — see README_DEV.md "Multiplayer" section.
//
//   * Host owns the seed. StartDuel carries seed + decks + rules so both
//     peers run an identical seeded ocgcore duel.
//   * Player responses are the network unit. The local user's clicks
//     produce response bytes via the existing DuelManager respond*
//     helpers. submitResponse already calls our recorder callback; we
//     route the same callback into NetSession::send when in MP.
//   * Remote responses arrive as EngineResponse packets, are fed via
//     m_dm.respond(...), and the m_mpFeedingRemote flag prevents the
//     recorder callback from echoing them back.
//
void UI::sendMpHello() {
    if (m_net.isOffline()) return;
    edo::NetMessage m; m.type = edo::NetMsgType::Hello;
    edo::putU32(m.payload, edo::kNetProtocolVersion);
    edo::putStr(m.payload, m_settings.mpDisplayName);
    m_net.send(m);
    m_dm.logEvent("[MULTI SEND] Hello");
}

void UI::sendMpDeckInfo() {
    if (m_net.isOffline()) return;
    if (m_mpDeckIdx < 0 || m_mpDeckIdx >= (int)m_deckFiles.size()) return;
    Deck d = loadYdk("assets/decks/" + m_deckFiles[m_mpDeckIdx]);
    edo::NetMessage m; m.type = edo::NetMsgType::DeckInfo;
    edo::putStr(m.payload, m_deckFiles[m_mpDeckIdx]);
    edo::putU32(m.payload, (uint32_t)d.main.size());
    for (uint32_t c : d.main)  edo::putU32(m.payload, c);
    edo::putU32(m.payload, (uint32_t)d.extra.size());
    for (uint32_t c : d.extra) edo::putU32(m.payload, c);
    edo::putU32(m.payload, (uint32_t)d.side.size());
    for (uint32_t c : d.side)  edo::putU32(m.payload, c);
    m_net.send(m);
    m_dm.logEvent("[MULTI SEND] DeckInfo  main=" +
                  std::to_string(d.main.size()) +
                  " extra=" + std::to_string(d.extra.size()) +
                  " side="  + std::to_string(d.side.size()));
}

void UI::sendMpReady(bool r) {
    if (m_net.isOffline()) return;
    edo::NetMessage m; m.type = edo::NetMsgType::Ready;
    edo::putU8(m.payload, r ? 1 : 0);
    m_net.send(m);
    m_dm.logEvent(std::string("[MULTI SEND] Ready=") +
                  (r ? "yes" : "no"));
}

// ── Online relay helpers (Stage C) ────────────────────────────────────────
// Persist the identity/relay fields, then open the relay connection. Creating
// makes us the authoritative host; joining makes us the guest. The room-
// handshake responses (RoomCreated / RoomJoined / RoomPeerJoined) arrive via
// handleNetMessage, which then drives mpKickoffHandshake().
void UI::startRelayCreate() {
    if (!m_net.isOffline()) return;
    m_settings.mpDisplayName = m_mpNameBuf[0] ? m_mpNameBuf : "Player";
    m_settings.mpHostIP      = m_mpRelayAddrBuf[0] ? m_mpRelayAddrBuf : "127.0.0.1";
    m_settings.mpMode        = "online";
    saveSettings();
    m_mpRoomError.clear();
    m_mpRoomCode.clear();
    m_mpRoomActive      = false;
    m_mpHandshakeSent   = false;
    m_mpRelayConnecting = true;
    if (!m_net.joinRelay(m_settings.mpHostIP, m_mpRelayPortBuf,
                         m_settings.mpDisplayName, /*createRoom*/true, "")) {
        m_mpRelayConnecting = false;
        pushToast("Could not start relay connection",
                  IM_COL32(232, 110, 100, 255), 3.0);
    }
}

void UI::startRelayJoin() {
    if (!m_net.isOffline()) return;
    // Normalise the typed room code (uppercase, trim spaces).
    std::string code;
    for (char* p = m_mpRoomCodeBuf; *p; ++p)
        if (*p != ' ') code += (char)toupper((unsigned char)*p);
    if (code.empty()) {
        pushToast("Enter a room code to join",
                  IM_COL32(232, 182, 72, 255), 2.4);
        return;
    }
    m_settings.mpDisplayName = m_mpNameBuf[0] ? m_mpNameBuf : "Player";
    m_settings.mpHostIP      = m_mpRelayAddrBuf[0] ? m_mpRelayAddrBuf : "127.0.0.1";
    m_settings.mpMode        = "online";
    saveSettings();
    m_mpRoomError.clear();
    m_mpRoomCode        = code;
    m_mpRoomActive      = false;
    m_mpHandshakeSent   = false;
    m_mpRelayConnecting = true;
    if (!m_net.joinRelay(m_settings.mpHostIP, m_mpRelayPortBuf,
                         m_settings.mpDisplayName, /*createRoom*/false, code)) {
        m_mpRelayConnecting = false;
        pushToast("Could not start relay connection",
                  IM_COL32(232, 110, 100, 255), 3.0);
    }
}

// Kick off the existing Hello/deck/ready handshake once the room is formed.
// Sending our Hello makes the peer's Hello handler reply with their deck +
// ready state; we reply in kind, reaching the same pre-duel state as LAN.
// Guarded so it runs exactly once per session.
void UI::mpKickoffHandshake() {
    if (m_mpHandshakeSent) return;
    m_mpHandshakeSent = true;
    sendMpHello();
    if (m_mpDeckIdx >= 0) sendMpDeckInfo();
    if (m_mpReady)        sendMpReady(true);
}

void UI::sendMpStartDuel() {
    if (!m_net.isHost() || m_mpInDuel) return;
    if (m_mpDeckIdx < 0 || !m_mpRemoteDeckRcvd) return;
    if (!m_mpReady || !m_mpRemoteReady) return;
    // Host = P1 = local deck.
    Deck p1 = loadYdk("assets/decks/" + m_deckFiles[m_mpDeckIdx]);
    Deck p2 = m_mpRemoteDeck;
    // Generate the seed locally; we'll force the engine to use it.
    uint64_t seed =
        (uint64_t)std::chrono::high_resolution_clock::now()
            .time_since_epoch().count();
    {
        std::random_device rd;
        seed ^= ((uint64_t)rd() << 32) ^ (uint64_t)rd();
    }
    // Build StartDuel packet.
    edo::NetMessage m; m.type = edo::NetMsgType::StartDuel;
    edo::putU64(m.payload, seed);
    edo::putU64(m.payload, 0);          // rule flags reserved (engine default)
    edo::putU32(m.payload, 8000);       // lp
    edo::putU32(m.payload, 5);          // hand
    edo::putU32(m.payload, 1);          // draw
    edo::putStr(m.payload, m_settings.mpDisplayName);   // P1 name
    edo::putStr(m.payload, m_net.peer().displayName);   // P2 name
    auto putDeck = [](std::vector<uint8_t>& b, const Deck& d) {
        edo::putU32(b, (uint32_t)d.main.size());
        for (uint32_t c : d.main)  edo::putU32(b, c);
        edo::putU32(b, (uint32_t)d.extra.size());
        for (uint32_t c : d.extra) edo::putU32(b, c);
        edo::putU32(b, (uint32_t)d.side.size());
        for (uint32_t c : d.side)  edo::putU32(b, c);
    };
    putDeck(m.payload, p1);
    putDeck(m.payload, p2);
    m_net.send(m);
    // Helper: short fingerprint of the first 5 card codes so the operator
    // can eyeball that both peers agree on each deck.
    auto first5 = [](const Deck& d) {
        std::string s;
        for (size_t i = 0; i < d.main.size() && i < 5; ++i) {
            if (!s.empty()) s += ",";
            s += std::to_string(d.main[i]);
        }
        return s;
    };
    m_dm.logEvent("[MULTI DECK MAP] localRole=Host  localPlayerIndex=0"
                  "  P1 name=" + m_deckFiles[m_mpDeckIdx] +
                  "  main/extra/side=" + std::to_string(p1.main.size()) +
                  "/" + std::to_string(p1.extra.size()) +
                  "/" + std::to_string(p1.side.size()) +
                  "  first5=" + first5(p1) +
                  "  P2 name=" + (p2.name.empty() ? "(remote)" : p2.name) +
                  "  main/extra/side=" + std::to_string(p2.main.size()) +
                  "/" + std::to_string(p2.extra.size()) +
                  "/" + std::to_string(p2.side.size()) +
                  "  first5=" + first5(p2));
    m_dm.logEvent("[MULTI START] seed=" + std::to_string(seed) +
                  "  P1 decks=" + std::to_string(p1.main.size()) +
                  "/" + std::to_string(p1.extra.size()) +
                  "  P2 decks=" + std::to_string(p2.main.size()) +
                  "/" + std::to_string(p2.extra.size()));
    // P2 Extra Deck fingerprint — proves the host engine is being seeded
    // with the client's real Extra Deck, not a dropped/empty one.
    {
        std::string ef5;
        for (size_t i = 0; i < p2.extra.size() && i < 5; ++i) {
            if (!ef5.empty()) ef5 += ",";
            ef5 += std::to_string(p2.extra[i]);
        }
        if (ef5.empty()) ef5 = "(none)";
        m_dm.logEvent("[MP START DECK MAP] P1 name=" +
                      m_deckFiles[m_mpDeckIdx] +
                      "  main=" + std::to_string(p1.main.size()) +
                      "  extra=" + std::to_string(p1.extra.size()) +
                      "  side=" + std::to_string(p1.side.size()) +
                      "  P2 name=" + (p2.name.empty() ? "(remote)" : p2.name) +
                      "  main=" + std::to_string(p2.main.size()) +
                      "  extra=" + std::to_string(p2.extra.size()) +
                      "  side=" + std::to_string(p2.side.size()) +
                      "  P2 extraFirst5=" + ef5);
    }
    // Start the local duel with the same seed.
    finalizeReplay("entering multiplayer");
    if (m_dm.isRunning()) m_dm.endDuel();
    m_dm.setForcedSeed(seed);
    m_dm.setLocalMode(false);           // no auto-AI in MP
    // Critical: in MP both peers run identical DuelManagers; if either
    // auto-resolves a 0-option chain window, the client also broadcasts
    // the auto-pass — host receives a duplicate response → desync. With
    // suppression on, the UI's owner-side auto-pass (see
    // maybeAutoPassMpZeroOptionChain) drives the response on the prompt
    // owner's machine and the bytes flow over the network once.
    m_dm.setSuppressAutoResolve(true);
    resetMpResponseState();
    m_dm.setResponseRecorder(
        [this](const void* d, uint32_t n){ mpOnLocalResponse(d, n); });
    m_dm.logEvent("[MULTI START LOCAL] engineP0=" +
                  m_deckFiles[m_mpDeckIdx] + " (main " +
                  std::to_string(p1.main.size()) + ")  engineP1=" +
                  (p2.name.empty() ? "(remote)" : p2.name) + " (main " +
                  std::to_string(p2.main.size()) + ")");
    // Multiplayer uses the net-mode seat (host = 0). Clear any leftover offline
    // coin-toss seat override so localPlayerIndex() falls back to the role.
    m_dm.setHumanSeat(0);
    m_net.clearSeatOverride();
    if (m_dm.startDuel(p1, p2, 8000, 5, 1)) {
        m_mpInDuel = true;
        m_screen   = Screen::Duel;
        m_anim.clear();
        m_sfxObsInited    = false;
        m_endGameSfxFired = false;
        m_dm.logEvent(std::string("[MP MODE] ") +
            (m_mpHostAuth ? "host-authoritative" : "dual-engine (lockstep)") +
            "  role=Host");
        pushToast("Multiplayer duel started",
                  IM_COL32(180, 220, 255, 255), 2.2);
        // Push an initial FieldSnapshot so the client has something
        // to render the moment its StartDuel ack lands.
        if (m_mpHostAuth) buildAndSendFieldSnapshot();
    }
}

void UI::mpOnLocalResponse(const void* data, uint32_t len) {
    // If this response was fed BY us from the network (remote player's
    // bytes), don't echo it back — the recorder still captures it for
    // the replay file as a normal turn entry.
    if (m_mpFeedingRemote) return;
    if (m_net.isOffline()) return;
    if (!m_mpInDuel) return;
    // Every response — host's own click OR a client choice applied via
    // handleClientChoice — advances the engine to a new state. Reset
    // the cached prompt identity so the next call to
    // buildAndSendPromptSnapshotIfRemote treats the engine's new
    // prompt (if any) as fresh, even if it has the same shape as the
    // one just answered. Cleared unconditionally regardless of MP
    // mode so the legacy path doesn't see stale identity either.
    m_mpHostLastPromptIdentity = 0;
    // Host-authoritative path: client never sends raw EngineResponse
    // (client doesn't even run an engine in that mode); host doesn't
    // need to echo its own engine's responses to the client either —
    // the FieldSnapshot it sends after `process()` advances is the
    // authoritative state delta. Replay recording still captures the
    // bytes via DuelManager's recorder hook.
    if (m_mpHostAuth) return;
    auto& dm = m_dm;                  // engine-state alias — legacy path
    ++m_mpOutSeq;
    // Wire format:
    //   [u8 owner][u32 seq][u32 waitTypeAtSend][u32 byteLen][bytes]
    // waitTypeAtSend is the SENDER's selection.type at the moment the
    // response was submitted (m_selection is still populated when our
    // recorder hook fires inside submitResponse — it's cleared AFTER).
    // The peer uses this to drop stale responses whose intended prompt
    // the receiver's engine has already moved past.
    edo::NetMessage m; m.type = edo::NetMsgType::EngineResponse;
    edo::putU8(m.payload, (uint8_t)m_net.localPlayerIndex());
    edo::putU32(m.payload, m_mpOutSeq);
    edo::putU32(m.payload, (uint32_t)dm.selection().type);
    edo::putU32(m.payload, len);
    const uint8_t* p = (const uint8_t*)data;
    m.payload.insert(m.payload.end(), p, p + len);
    m_net.send(m);
    const char* src = (m_mpLastAutoPassKey != 0 &&
                       dm.selection().type == WaitType::SelectChain)
        ? "auto-pass" : "user";
    m_dm.logEvent("[MULTI LOCAL RESPONSE] owner=" +
                  std::to_string(m_net.localPlayerIndex()) +
                  "  seq=" + std::to_string(m_mpOutSeq) +
                  "  bytes=" + std::to_string(len) +
                  "  waitType=" +
                  std::to_string((int)dm.selection().type) +
                  "  source=" + src);
}

void UI::resetMpResponseState() {
    m_mpQueue.clear();
    m_mpOutSeq = 0;
    m_mpLastSeenSeq[0] = 0;
    m_mpLastSeenSeq[1] = 0;
    m_mpLastAutoPassKey = 0;
    m_mpLastWaitKey     = 0;
    m_mpLocalPrompt = PromptInfo{};
    m_mpRemotePrompt = PromptInfo{};
    m_mpLocalPromptSeq      = 0;
    m_mpLastSentPromptHash  = 0;
    m_mpDesynced            = false;
    m_mpDesyncSummary.clear();
    // Host-authoritative state.
    m_mpRemoteField = FieldState{};
    m_mpRemoteFieldValid = false;
    m_mpRemoteSel = edo::PromptSnapshotPayload{};
    m_mpRemoteSelCached = SelectionRequest{};
    m_mpRemoteSelValid  = false;
    m_mpRemoteOwnExtra.clear();
    m_mpOppPromptWait   = 0;
    m_mpHostPromptSeq      = 0;
    m_mpHostLastSentPromptSeq = 0;
    m_mpHostChoices.clear();
    m_mpHostChoicesForSeq  = 0;
    m_mpHostNextChoiceId   = 1;
    m_mpHostLastFieldHash  = 0;
    m_mpHostLastPromptIdentity = 0;
    m_mpRemoteDuelActive   = false;
    m_mpLastSnapshotSeq    = 0;
    m_mpAwaitingHostUpdate = false;
    m_mpLastDuelStateKey   = 0;
    // Game-over propagation latches.
    m_mpGameOverSent = false;
    m_mpRemoteDone   = false;
    m_mpRemoteWinner = -1;
    m_mpRemoteReason = -1;
}

// ── Source-of-truth helpers ───────────────────────────────────────────
// In host-authoritative client mode the local engine is NOT running.
// Every render path call site routes through these getters so the
// snapshot drives the UI exactly the same way the engine would in
// offline mode.
bool UI::usingRemoteField() const {
    return m_mpHostAuth && m_mpInDuel &&
           m_net.isClient() && m_mpRemoteFieldValid;
}
const FieldState& UI::currentField() const {
    // NOTE: dm-alias indirection so a global replace_all of
    // "currentField()" → "currentField()" doesn't recurse into us.
    auto& dm = m_dm;
    return usingRemoteField() ? m_mpRemoteField : dm.field();
}
const SelectionRequest& UI::currentSelection() const {
    auto& dm = m_dm;
    if (usingRemoteField() && m_mpRemoteSelValid)
        return m_mpRemoteSelCached;
    return dm.selection();
}

bool UI::isDuelVisiblyRunning() const {
    auto& dm = m_dm;
    // Host-auth client: the local DuelManager is intentionally NOT
    // started, so we trust the network-driven latch.
    if (m_mpHostAuth && m_net.isClient() && m_mpInDuel) {
        return m_mpRemoteDuelActive;
    }
    return dm.isRunning();
}

bool UI::isDuelVisiblyBlocked() const {
    auto& dm = m_dm;
    // Host-auth client never blocks locally — the host owns the engine.
    if (m_mpHostAuth && m_net.isClient() && m_mpInDuel) return false;
    return dm.isBlocked();
}

// Emits one [CLIENT DUEL STATE] line per change in the client's
// snapshot-driven duel state. Keyed by (remoteActive, awaiting,
// remotePromptValid, localDmRunning, usingRemoteField) so a click that
// flips m_mpAwaitingHostUpdate logs exactly once, not every frame.
void UI::logClientDuelStateIfChanged() {
    if (!(m_mpHostAuth && m_net.isClient() && m_mpInDuel)) {
        m_mpLastDuelStateKey = 0;       // reset so we relog on re-entry
        return;
    }
    uint64_t key =
        ((uint64_t)(m_mpRemoteDuelActive    ? 1 : 0) <<  0) |
        ((uint64_t)(m_mpAwaitingHostUpdate  ? 1 : 0) <<  1) |
        ((uint64_t)(m_mpRemoteSelValid      ? 1 : 0) <<  2) |
        ((uint64_t)(m_dm.isRunning()        ? 1 : 0) <<  3) |
        ((uint64_t)(usingRemoteField()      ? 1 : 0) <<  4) |
        ((uint64_t)m_mpLastSnapshotSeq      << 16);
    if (key == m_mpLastDuelStateKey) return;
    m_mpLastDuelStateKey = key;
    m_dm.logEvent(std::string("[CLIENT DUEL STATE]"
                  "  remoteActive=") + (m_mpRemoteDuelActive ? "yes" : "no") +
                  "  lastSnapshotSeq=" + std::to_string(m_mpLastSnapshotSeq) +
                  "  awaitingHostUpdate=" +
                  (m_mpAwaitingHostUpdate ? "yes" : "no") +
                  "  localDmRunning=" + (m_dm.isRunning() ? "yes" : "no") +
                  "  usingRemoteField=" + (usingRemoteField() ? "yes" : "no") +
                  "  remotePromptValid=" +
                  (m_mpRemoteSelValid ? "yes" : "no"));
}

// Captures the current engine prompt into a portable fingerprint. The
// content is intentionally minimal so it stays cheap to serialise every
// frame; deeper fields (field hash, full candidate metadata) can be
// added later if the basic fingerprint catches the mismatches we care
// about.
void UI::capturePromptInfo(PromptInfo& out) {
    auto& dm = m_dm;                  // engine-state alias — legacy handshake
    const SelectionRequest& sel = dm.selection();
    out = PromptInfo{};
    out.waitType    = (uint32_t)sel.type;
    out.owner       = sel.player;
    out.turnPlayer  = dm.field().turnPlayer;
    out.phase       = dm.field().phase;
    out.minSel      = sel.min;
    out.maxSel      = sel.max;
    out.optionCount = (uint32_t)sel.options.size();
    out.forced      = sel.forced;
    out.chainCount  = (uint32_t)sel.cards.size();
    // Candidate card codes — first 12 to cap packet size.
    size_t cap = std::min<size_t>(sel.cards.size(), 12);
    out.candidateCodes.reserve(cap);
    for (size_t i = 0; i < cap; ++i)
        out.candidateCodes.push_back(sel.cards[i].code);
    out.valid = true;
}

// Stable hash of the prompt fingerprint so we can detect "did this
// change since last frame?" cheaply.
uint64_t UI::hashPrompt(const PromptInfo& p) const {
    uint64_t h = 1469598103934665603ull;            // FNV-1a basis
    auto mix = [&](uint64_t v) {
        h ^= v; h *= 1099511628211ull;
    };
    mix((uint64_t)p.waitType);
    mix((uint64_t)p.owner);
    mix((uint64_t)p.turnPlayer);
    mix((uint64_t)p.phase);
    mix((uint64_t)(uint32_t)p.minSel);
    mix((uint64_t)(uint32_t)p.maxSel);
    mix((uint64_t)p.optionCount);
    mix((uint64_t)(p.forced ? 1 : 0));
    mix((uint64_t)p.chainCount);
    for (uint32_t c : p.candidateCodes) mix((uint64_t)c);
    return h;
}

void UI::sendPromptStateIfChanged() {
    if (m_net.isOffline() || !m_mpInDuel)         return;
    if (!m_dm.isRunning())                        return;
    capturePromptInfo(m_mpLocalPrompt);
    if (!DuelManager::isRealSelect((WaitType)m_mpLocalPrompt.waitType) &&
        m_mpLocalPrompt.waitType != (uint32_t)WaitType::RawPrompt) {
        // No real prompt right now — don't broadcast None/Waiting state.
        return;
    }
    uint64_t hash = hashPrompt(m_mpLocalPrompt);
    if (hash == m_mpLastSentPromptHash) return;     // unchanged
    m_mpLastSentPromptHash = hash;
    ++m_mpLocalPromptSeq;
    m_mpLocalPrompt.promptSeq = m_mpLocalPromptSeq;

    edo::NetMessage m; m.type = edo::NetMsgType::PromptState;
    edo::putU64(m.payload, m_mpLocalPrompt.promptSeq);
    edo::putU32(m.payload, m_mpLocalPrompt.waitType);
    edo::putU8 (m.payload, m_mpLocalPrompt.owner);
    edo::putU8 (m.payload, m_mpLocalPrompt.turnPlayer);
    // phase as u32 for alignment ease.
    edo::putU32(m.payload, (uint32_t)m_mpLocalPrompt.phase);
    edo::putU32(m.payload, (uint32_t)m_mpLocalPrompt.minSel);
    edo::putU32(m.payload, (uint32_t)m_mpLocalPrompt.maxSel);
    edo::putU32(m.payload, m_mpLocalPrompt.optionCount);
    edo::putU32(m.payload, m_mpLocalPrompt.forced ? 1u : 0u);
    edo::putU32(m.payload, m_mpLocalPrompt.chainCount);
    edo::putU32(m.payload, (uint32_t)m_mpLocalPrompt.candidateCodes.size());
    for (uint32_t c : m_mpLocalPrompt.candidateCodes)
        edo::putU32(m.payload, c);
    m_net.send(m);
    m_dm.logEvent("[MULTI PROMPT SEND] seq=" +
                  std::to_string(m_mpLocalPrompt.promptSeq) +
                  "  owner=" + std::to_string((int)m_mpLocalPrompt.owner) +
                  "  waitType=" + std::to_string((int)m_mpLocalPrompt.waitType) +
                  "  candidates=" + std::to_string(m_mpLocalPrompt.chainCount) +
                  "  forced=" + (m_mpLocalPrompt.forced ? "yes" : "no") +
                  "  hash=" + std::to_string(hash));
}

void UI::handleRemotePromptState(const edo::NetMessage& m) {
    edo::NetReader r(m.payload);
    PromptInfo p{};
    p.promptSeq   = r.u64();
    p.waitType    = r.u32();
    p.owner       = r.u8();
    p.turnPlayer  = r.u8();
    p.phase       = (uint16_t)r.u32();
    p.minSel      = (int32_t)r.u32();
    p.maxSel      = (int32_t)r.u32();
    p.optionCount = r.u32();
    p.forced      = (r.u32() != 0);
    p.chainCount  = r.u32();
    uint32_t cn   = r.u32();
    for (uint32_t i = 0; i < cn && r.ok; ++i)
        p.candidateCodes.push_back(r.u32());
    if (!r.ok) {
        m_dm.logEvent("[MULTI PROMPT RECV] truncated payload");
        return;
    }
    p.valid = true;
    m_mpRemotePrompt = p;
    m_dm.logEvent("[MULTI PROMPT RECV] seq=" + std::to_string(p.promptSeq) +
                  "  owner=" + std::to_string((int)p.owner) +
                  "  waitType=" + std::to_string((int)p.waitType) +
                  "  candidates=" + std::to_string(p.chainCount) +
                  "  forced=" + (p.forced ? "yes" : "no") +
                  "  hash=" + std::to_string(hashPrompt(p)));

    // Compare against our current local snapshot. We can't compare seq
    // (they're independent counters) but the FINGERPRINT should match:
    // same engine state means same waitType / owner / counts.
    capturePromptInfo(m_mpLocalPrompt);
    if (!DuelManager::isRealSelect((WaitType)m_mpLocalPrompt.waitType) &&
        m_mpLocalPrompt.waitType != (uint32_t)WaitType::RawPrompt) {
        // Local engine hasn't reached this prompt yet — engines run at
        // slightly different rates between network round-trips, so we
        // don't flag this as a desync. The next sendPromptStateIfChanged
        // will reconcile when both sides advance.
        m_dm.logEvent("[MULTI PROMPT MATCH] localSeq=(no-local-prompt)"
                      "  remoteSeq=" + std::to_string(p.promptSeq) +
                      "  (deferred until local advances)");
        return;
    }

    bool match =
        m_mpLocalPrompt.waitType    == p.waitType &&
        m_mpLocalPrompt.owner       == p.owner &&
        m_mpLocalPrompt.forced      == p.forced &&
        m_mpLocalPrompt.chainCount  == p.chainCount &&
        m_mpLocalPrompt.optionCount == p.optionCount;
    if (match) {
        m_dm.logEvent("[MULTI PROMPT MATCH] localSeq=" +
                      std::to_string(m_mpLocalPromptSeq) +
                      "  remoteSeq=" + std::to_string(p.promptSeq) +
                      "  owner=" + std::to_string((int)p.owner) +
                      "  waitType=" + std::to_string((int)p.waitType));
        m_mpDesynced = false;
        m_mpDesyncSummary.clear();
    } else {
        std::ostringstream os;
        os << "local{wt=" << m_mpLocalPrompt.waitType
           << " own=" << (int)m_mpLocalPrompt.owner
           << " forced=" << (m_mpLocalPrompt.forced ? 1 : 0)
           << " chain=" << m_mpLocalPrompt.chainCount
           << " opts=" << m_mpLocalPrompt.optionCount << "}  remote{wt="
           << p.waitType << " own=" << (int)p.owner
           << " forced=" << (p.forced ? 1 : 0)
           << " chain=" << p.chainCount
           << " opts=" << p.optionCount << "}";
        m_dm.logEvent("[MULTI PROMPT MISMATCH] " + os.str());
        m_mpDesynced = true;
        m_mpDesyncSummary = os.str();
        pushToast("Multiplayer prompt desync — see Debug Log",
                  IM_COL32(232, 110, 100, 255), 4.0);
    }
}

bool UI::isLocalPromptOwner() const {
    if (m_net.isOffline()) return true;
    // Host-authoritative client: trust the snapshot's prompt owner
    // rather than the local (idle) engine.
    if (m_mpHostAuth && m_net.isClient()) {
        if (!m_mpRemoteSelValid) return false;
        return m_mpRemoteSel.owner == (uint8_t)m_net.localPlayerIndex();
    }
    auto& dm = m_dm;                  // legacy / host: engine state
    const SelectionRequest& sel = dm.selection();
    if (!DuelManager::isRealSelect(sel.type)) return true;   // no prompt
    return sel.player == (uint8_t)m_net.localPlayerIndex();
}

// ─────────────────────────────────────────────────────────────────────
// Host-authoritative multiplayer — host send path
// ─────────────────────────────────────────────────────────────────────
//
// The host builds a FieldSnapshot from currentField() after every engine
// advance and ships it to the client. We dedup by a coarse hash so we
// don't flood the wire while the engine is idle waiting for input.
void UI::buildAndSendFieldSnapshot() {
    if (!m_mpHostAuth || !m_net.isHost() || !m_mpInDuel) return;
    if (!m_dm.isRunning()) return;
    auto& dm = m_dm;                  // engine-state alias — see currentField()
    const FieldState& f = dm.field();
    // Snapshot is per-recipient — the client is the remote player.
    // The recipient's OWN Extra Deck contents ride along (engine order)
    // so the client's ED viewer + ED summon list work without a local
    // engine. The opponent's Extra Deck stays a bare count.
    int recipient = m_net.remotePlayerIndex();
    edo::FieldSnapshotPayload snap =
        edo::buildFieldSnapshot(f, recipient,
                                m_dm.extraDeckCodes(recipient));

    // Cheap dedup: hash some volatile fields. Phase/turn/LP changes
    // and any zone-size change reliably bump this.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v){ h ^= v; h *= 1099511628211ull; };
    mix(snap.phase); mix(snap.turnPlayer); mix(snap.turn);
    mix(snap.lp[0]); mix(snap.lp[1]);
    mix(snap.handCount[0]); mix(snap.handCount[1]);
    mix(snap.deckCount[0]); mix(snap.deckCount[1]);
    mix(snap.extraCount[0]); mix(snap.extraCount[1]);
    mix(snap.ourExtra.size());
    for (int p = 0; p < 2; ++p) {
        mix(snap.monsters[p].size());
        mix(snap.spells  [p].size());
        mix(snap.gy      [p].size());
        mix(snap.banished[p].size());
    }
    mix(snap.ourHand.size());
    if (h == m_mpHostLastFieldHash) return;   // unchanged — skip
    m_mpHostLastFieldHash = h;

    edo::NetMessage m; m.type = edo::NetMsgType::FieldSnapshot;
    edo::serialiseFieldSnapshot(m.payload, snap);
    m_net.send(m);
    m_dm.logEvent("[HOST SNAPSHOT SEND] turn=" +
                  std::to_string(snap.turn) +
                  "  phase=" + std::to_string(snap.phase) +
                  "  tp=" + std::to_string(snap.turnPlayer) +
                  "  lp=" + std::to_string(snap.lp[0]) +
                  "/" + std::to_string(snap.lp[1]) +
                  "  recipient=" + std::to_string(snap.recipient));
}

// If the engine is currently waiting for a response from the remote
// player, build a PromptSnapshot with one PromptChoice per legal
// option and ship it to the client. Each choice is registered with
// the host's local choice table so we can validate-and-respond when
// the client sends ClientChoice back.
void UI::buildAndSendPromptSnapshotIfRemote(const char* reason) {
    if (!m_mpHostAuth || !m_net.isHost() || !m_mpInDuel) return;
    if (!m_dm.isRunning()) return;
    auto& dm = m_dm;                  // engine-state alias — host-only
    const SelectionRequest& sel = dm.selection();
    if (!DuelManager::isRealSelect(sel.type)) {
        // Engine is no longer waiting on a remote prompt. Reset the
        // identity so the *next* prompt (even if structurally
        // identical) ships with a fresh promptSeq.
        m_mpHostLastPromptIdentity = 0;
        return;
    }
    int remoteIdx = m_net.remotePlayerIndex();
    // Prompt owned by the HOST: the host's local UI drives it, but the
    // client still gets a zero-choice "notice" snapshot so it can show
    // "Waiting for <opponent> — <prompt type>" instead of a blank status.
    // The notice carries owner + waitType only — no candidate data, so
    // nothing hidden can leak. Deduped by the same identity hash below.
    const bool noticeOnly = ((int)sel.player != remoteIdx);

    // Compute a stable identity hash for THIS prompt. Two consecutive
    // frames sitting on the same engine prompt must produce the SAME
    // identity so we don't ship a new PromptSnapshot every frame.
    // The hash must include every field that influences the legal
    // choice set; if the engine genuinely transitions to a new prompt,
    // the identity will change and a fresh seq is bumped.
    uint64_t identity = 1469598103934665603ull;
    auto mix = [&](uint64_t v){ identity ^= v; identity *= 1099511628211ull; };
    mix((uint64_t)(uint32_t)sel.type);
    mix((uint64_t)sel.player);
    mix((uint64_t)(sel.forced ? 1 : 0));
    mix((uint64_t)(uint32_t)sel.min);
    mix((uint64_t)(uint32_t)sel.max);
    mix((uint64_t)sel.cards.size());
    mix((uint64_t)sel.options.size());
    for (const auto& c : sel.cards) {
        mix((uint64_t)c.code);
        mix((uint64_t)c.loc);
        mix((uint64_t)c.seq);
    }
    for (int o : sel.options) mix((uint64_t)(uint32_t)o);
    // Idle / battle actions + phase permissions + placement mask MUST be
    // part of the identity — without these, two consecutive idle prompts
    // with different legal actions (e.g. after a summon) would hash the
    // same and the client would keep a stale action list.
    mix((uint64_t)sel.idle.size());
    for (const auto& a : sel.idle) {
        mix((uint64_t)(uint32_t)a.cmd);
        mix((uint64_t)(uint32_t)a.index);
        mix((uint64_t)a.code);
        mix((uint64_t)a.loc);
        mix((uint64_t)a.seq);
    }
    mix((uint64_t)(sel.toBP ? 1 : 0));
    mix((uint64_t)(sel.toM2 ? 1 : 0));
    mix((uint64_t)(sel.toEP ? 1 : 0));
    mix((uint64_t)sel.placeFlag);
    mix((uint64_t)(uint32_t)sel.placeCount);

    // Already shipped this exact prompt — client still has the choice
    // table keyed by m_mpHostChoicesForSeq, so any ClientChoice that
    // arrives will still match. Skip the resend entirely.
    if (identity == m_mpHostLastPromptIdentity) return;
    m_mpHostLastPromptIdentity = identity;

    // New (or structurally different) prompt — bump the wire seq once.
    ++m_mpHostPromptSeq;
    m_mpHostLastSentPromptSeq = m_mpHostPromptSeq;

    edo::PromptSnapshotPayload p;
    p.promptSeq  = m_mpHostPromptSeq;
    p.waitType   = (uint32_t)sel.type;
    p.owner      = sel.player;
    p.turnPlayer = dm.field().turnPlayer;
    p.phase      = dm.field().phase;
    p.minSel     = sel.min;
    p.maxSel     = sel.max;
    p.forced     = sel.forced ? 1u : 0u;
    p.toBP       = sel.toBP ? 1u : 0u;
    p.toM2       = sel.toM2 ? 1u : 0u;
    p.toEP       = sel.toEP ? 1u : 0u;
    p.placeFlag  = sel.placeFlag;
    p.placeCount = (uint32_t)sel.placeCount;

    // Timing window + prompt-source card: lets the client label choices
    // "Trigger Effect" vs "Quick Effect" and show WHICH card is asking,
    // exactly like the offline renderer does from SelectionRequest.
    p.timing = (uint32_t)sel.timing;
    if (!noticeOnly &&
        (sel.type == WaitType::SelectYesNo ||
         sel.type == WaitType::SelectEffectYn)) {
        // EffectYn carries the card; a bare Yes/No does not, so fall back to the
        // resolving card (last MSG_CHAINING). Prefer the engine's specific line;
        // when it's missing (optional "then you can ..." costs) show the card's
        // full effect text instead of a bare "Activate effect?".
        uint32_t srcCode = !sel.cards.empty() ? sel.cards[0].code
                                              : m_dm.chainSourceCode();
        p.srcCode = srcCode;
        std::string decoded = (!sel.chainEffects.empty() &&
                               !sel.chainEffects[0].text.empty())
            ? sel.chainEffects[0].text : std::string();
        p.srcDesc = !decoded.empty() ? decoded
                  : (srcCode ? m_db.getCard(srcCode).desc : std::string());
    }

    // Per-waitType title — human-readable header for the client's prompt
    // panel. Counts are baked in for the card-selection prompts.
    auto countPhrase = [&]() -> std::string {
        if (sel.min == sel.max)
            return "Select " + std::to_string(sel.min) +
                   (sel.min == 1 ? " card" : " cards");
        return "Select " + std::to_string(sel.min) + "-" +
               std::to_string(sel.max) + " cards";
    };
    switch (sel.type) {
        case WaitType::SelectYesNo:    p.title = "Yes / No"; break;
        case WaitType::SelectEffectYn: p.title = "Activate effect?"; break;
        case WaitType::SelectChain:
            p.title = sel.forced ? "You must chain a card"
                                 : "Respond with a card effect?";
            break;
        case WaitType::SelectCard:     p.title = countPhrase(); break;
        case WaitType::SelectTribute:  p.title = countPhrase() +
                                                 " to Tribute"; break;
        case WaitType::SelectUnselect: p.title = "Select material"; break;
        case WaitType::SelectOption:   p.title = "Choose an effect"; break;
        case WaitType::SelectIdleCmd:  p.title = "Your move"; break;
        case WaitType::SelectBattleCmd:p.title = "Battle phase"; break;
        case WaitType::SelectPlace:    p.title = "Choose a zone"; break;
        case WaitType::SelectPosition: p.title = "Select position"; break;
        default:                       p.title = "Select"; break;
    }

    // Host-owned prompt notice: strip everything except owner/waitType/
    // turn/phase/timing. min/max counts, zone masks, source card and
    // candidate details could leak hidden information (e.g. which set
    // trap the host is being asked to activate) — the client only needs
    // "the opponent is deciding a <type>".
    if (noticeOnly) {
        p.minSel = 0; p.maxSel = 0; p.forced = 0;
        p.toBP = p.toM2 = p.toEP = 0;
        p.placeFlag = 0; p.placeCount = 0;
        p.srcCode = 0; p.srcDesc.clear();
    }

    // Reset the choice table — it only covers the current prompt.
    m_mpHostChoices.clear();
    m_mpHostChoicesForSeq = m_mpHostPromptSeq;
    m_mpHostNextChoiceId  = 1;

    auto addChoice = [&](uint32_t code, uint32_t loc, uint32_t seq,
                         const std::string& label, uint32_t iconHint,
                         std::vector<uint8_t> bytes) -> uint32_t {
        uint32_t id = m_mpHostNextChoiceId++;
        edo::PromptChoice c;
        c.choiceId = id; c.code = code; c.loc = loc; c.seq = seq;
        c.iconHint = iconHint; c.label = label;
        p.choices.push_back(c);
        HostChoice hc; hc.responseBytes = std::move(bytes);
        m_mpHostChoices[id] = std::move(hc);
        return id;
    };
    // Stamp the v2 routing fields onto the choice just added — the client
    // maps its local (cmd, index) click back to this choiceId.
    auto tagIdle = [&](uint32_t cmd, uint32_t index, uint8_t con,
                       uint32_t flags) {
        auto& pc = p.choices.back();
        pc.cmd = cmd; pc.index = index; pc.con = con; pc.flags = flags;
    };
    // Stamp the v3 description fields (decoded effect text + raw desc)
    // onto the choice just added. Empty text + raw=0 is a no-op.
    auto tagDesc = [&](const EffectDesc& d) {
        auto& pc = p.choices.back();
        pc.rawDesc = d.raw;
        pc.desc    = !d.text.empty()
            ? d.text
            : (d.raw ? ("desc#" + std::to_string(d.raw)) : std::string());
    };

    // Helpers to build the engine response bytes for the canonical
    // simple prompts. Anything more complex falls back to a single
    // generic "advance" choice and we log the gap — Stage 3 work.
    auto bytesInt = [](int v) {
        std::vector<uint8_t> b(4);
        b[0] = (uint8_t)(v & 0xff);
        b[1] = (uint8_t)((v >> 8) & 0xff);
        b[2] = (uint8_t)((v >> 16) & 0xff);
        b[3] = (uint8_t)((v >> 24) & 0xff);
        return b;
    };

    if (!noticeOnly) switch (sel.type) {
        case WaitType::SelectYesNo:
        case WaitType::SelectEffectYn:
            addChoice(0, 0, 0, "Yes", 1, bytesInt(1));
            addChoice(0, 0, 0, "No",  2, bytesInt(0));
            break;
        case WaitType::SelectChain: {
            // One option per chainable + a Pass entry. label = card name
            // (the client prefixes "Trigger Effect:/Quick Effect:" from
            // the transmitted timing); desc = decoded effect text so a
            // multi-effect card names WHICH effect would chain.
            for (size_t i = 0; i < sel.cards.size(); ++i) {
                const auto& cs = sel.cards[i];
                std::string label = cs.name.empty()
                    ? ("Card #" + std::to_string(cs.code))
                    : cs.name;
                addChoice(cs.code, cs.loc, cs.seq, label, 0,
                          bytesInt((int)i));
                if (i < sel.chainEffects.size())
                    tagDesc(sel.chainEffects[i]);
            }
            if (!sel.forced)
                addChoice(0, 0, 0, "Pass / No Response", 3, bytesInt(-1));
            break;
        }
        case WaitType::SelectOption: {
            // Real effect text per option (decoded from the engine desc
            // via cards.cdb on the host). "Effect N" is the fallback only
            // when no description resolves — never a bare "Option N" when
            // text exists.
            for (size_t i = 0; i < sel.options.size(); ++i) {
                const EffectDesc* d = (i < sel.chainEffects.size())
                                      ? &sel.chainEffects[i] : nullptr;
                std::string label = "Effect " + std::to_string(i + 1);
                if (d && !d->text.empty()) label += ": " + d->text;
                // choice.code carries the option's SOURCE card (decoded
                // from the desc) so the client can hover-preview it.
                uint32_t srcCode = (d && d->isCardString) ? d->cardCode : 0;
                addChoice(srcCode, 0, 0, label, 0, bytesInt((int)i));
                if (d) tagDesc(*d);
            }
            break;
        }
        case WaitType::SelectCard:
        case WaitType::SelectTribute: {
            // One choice per candidate. Single-pick (min==max==1)
            // registers the FULL MSG_SELECT_CARD response shape
            // {i32 type=0, u32 count=1, u32 index} — a bare int32 is NOT
            // a valid response and would park the engine in MSG_RETRY.
            // Multi-pick adds a Confirm entry; the client ships the
            // ticked engine indices via ClientChoice.extraIndices and
            // the host assembles the buffer at apply time.
            const bool single = (sel.min == 1 && sel.max == 1);
            for (size_t i = 0; i < sel.cards.size(); ++i) {
                const auto& cs = sel.cards[i];
                std::string label = cs.name.empty()
                    ? ("Card #" + std::to_string(cs.code))
                    : cs.name;
                std::vector<uint8_t> bytes;
                if (single) {
                    bytes = bytesInt(0);                    // i32 type = 0
                    auto cnt = bytesInt(1);                 // u32 count = 1
                    auto idx = bytesInt((int)i);            // u32 index
                    bytes.insert(bytes.end(), cnt.begin(), cnt.end());
                    bytes.insert(bytes.end(), idx.begin(), idx.end());
                }
                addChoice(cs.code, cs.loc, cs.seq, label, 0,
                          std::move(bytes));
            }
            if (!single) {
                uint32_t id = addChoice(0, 0, 0, "Confirm selection", 8, {});
                m_mpHostChoices[id].multiPick = true;
            }
            break;
        }
        case WaitType::SelectUnselect: {
            // Pick-one response shape: {i32 mode=1, i32 index}; Finish
            // (when not forced) is a plain int32 -1 — mirrors
            // respondUnselect / respondInt(-1).
            for (size_t i = 0; i < sel.cards.size(); ++i) {
                const auto& cs = sel.cards[i];
                std::string label = cs.name.empty()
                    ? ("Card #" + std::to_string(cs.code))
                    : cs.name;
                std::vector<uint8_t> bytes = bytesInt(1);
                auto idx = bytesInt((int)i);
                bytes.insert(bytes.end(), idx.begin(), idx.end());
                addChoice(cs.code, cs.loc, cs.seq, label, 0,
                          std::move(bytes));
            }
            if (!sel.forced)
                addChoice(0, 0, 0, "Finish selection", 7, bytesInt(-1));
            break;
        }
        case WaitType::SelectIdleCmd:
        case WaitType::SelectBattleCmd: {
            // One choice per engine-legal action + phase pseudo-choices.
            // Response bytes mirror respondIdleCmd(t, s):
            //   int32 (index << 16) | cmd.
            // The label carries the decoded effect text when there is
            // one — the client resolves the card NAME from its own
            // cards.cdb and re-derives the verb from cmd, exactly like
            // the offline renderer.
            const bool battle = (sel.type == WaitType::SelectBattleCmd);
            for (const auto& a : sel.idle) {
                // label = card name; desc = decoded effect text (cmd 5 /
                // battle activations). The client re-derives the verb
                // ("Summon:", "Set S/T:", "Activate:") from cmd, exactly
                // like the offline action popup.
                std::string label = a.name.empty()
                    ? ("Card #" + std::to_string(a.code)) : a.name;
                int32_t v = (int32_t)(((uint32_t)a.index << 16) |
                                      ((uint32_t)a.cmd & 0xffff));
                addChoice(a.code, a.loc, a.seq, label, 0, bytesInt(v));
                tagIdle((uint32_t)a.cmd, (uint32_t)a.index, a.con,
                        a.canDirect ? 1u : 0u);
                tagDesc(a.effect);
            }
            if (!battle && sel.toBP) {
                addChoice(0, 0, 0, "Battle Phase", 4, bytesInt(6));
                tagIdle(6, 0, sel.player, 0);
            }
            if (battle && sel.toM2) {
                addChoice(0, 0, 0, "Main Phase 2", 6, bytesInt(2));
                tagIdle(2, 0, sel.player, 0);
            }
            if (sel.toEP) {
                addChoice(0, 0, 0,
                          battle ? "End Battle Phase" : "End Turn", 5,
                          bytesInt(battle ? 3 : 7));
                tagIdle(battle ? 3u : 7u, 0, sel.player, 0);
            }
            break;
        }
        case WaitType::SelectPlace: {
            // One choice per LEGAL zone (a SET placeFlag bit means the
            // zone is forbidden/occupied). Response is the 3-byte
            // respondPlace shape {player, loc, seq}. The client maps a
            // clicked (loc, seq) tile back via the choice's loc/seq.
            auto addZone = [&](uint8_t loc, uint32_t seqZ,
                               const std::string& lbl) {
                std::vector<uint8_t> b = { (uint8_t)sel.player, loc,
                                           (uint8_t)seqZ };
                addChoice(0, loc, seqZ, lbl, 0, std::move(b));
            };
            uint32_t flag = sel.placeFlag;
            for (uint32_t s2 = 0; s2 < 7; ++s2)        // MZ 1-5 + EMZ 1-2
                if (!(flag & (1u << s2)))
                    addZone(0x04 /*LOC_MZONE*/, s2,
                            s2 <= 4 ? ("Monster Zone " + std::to_string(s2 + 1))
                                    : "Extra Monster Zone");
            for (uint32_t s2 = 0; s2 < 5; ++s2)        // S/T 1-5
                if (!(flag & (1u << (s2 + 8))))
                    addZone(0x08 /*LOC_SZONE*/, s2,
                            "Spell/Trap Zone " + std::to_string(s2 + 1));
            if (!(flag & (1u << 13)))                  // Field Zone (seq 5)
                addZone(0x08, 5, "Field Zone");
            for (uint32_t s2 = 6; s2 < 8; ++s2)        // Pendulum zones
                if (!(flag & (1u << (s2 + 8))))
                    addZone(0x08, s2, "Pendulum Zone");
            break;
        }
        default:
            // Position / Counter / Sum — not yet representable. Send a
            // single "Waiting (host will resolve)" placeholder so the
            // client at least sees the prompt; the host UI keeps
            // ownership of the actual interaction for now.
            addChoice(0, 0, 0, "(host-handled — not yet implemented)", 0, {});
            break;
    }

    // Per-choice label audit — one line per shipped choice (capped so a
    // 60-card search prompt doesn't flood the log; the cap note says how
    // many were elided).
    {
        const size_t kLabelLogCap = 16;
        for (size_t i = 0; i < p.choices.size() && i < kLabelLogCap; ++i) {
            const auto& c = p.choices[i];
            m_dm.logEvent("[PROMPT LABEL] waitType=" +
                          std::to_string((int)p.waitType) +
                          "  choiceId=" + std::to_string(c.choiceId) +
                          "  rawDesc=" + std::to_string(c.rawDesc) +
                          "  label=" + c.label +
                          (c.desc.empty() ? "" : ("  desc=" + c.desc)));
        }
        if (p.choices.size() > kLabelLogCap)
            m_dm.logEvent("[PROMPT LABEL] (+" +
                          std::to_string(p.choices.size() - kLabelLogCap) +
                          " more choices elided)");
    }

    edo::NetMessage m; m.type = edo::NetMsgType::PromptSnapshot;
    edo::serialisePromptSnapshot(m.payload, p);
    m_net.send(m);
    m_dm.logEvent("[HOST PROMPT SEND] promptSeq=" + std::to_string(p.promptSeq) +
                  "  owner=" + std::to_string((int)p.owner) +
                  "  waitType=" + std::to_string((int)p.waitType) +
                  "  choiceCount=" + std::to_string(p.choices.size()) +
                  "  reason=" + (reason ? reason : "frame") +
                  (noticeOnly ? "  kind=opponent-notice" : "") +
                  "  hash=" + std::to_string(identity));
}

// Host receives a ClientChoice — validate seq + owner + choiceId,
// apply via the existing DuelManager respond path. Stale or illegal
// picks get a SyncError reply and are otherwise ignored.
void UI::handleClientChoice(const edo::NetMessage& m) {
    if (!m_mpHostAuth || !m_net.isHost()) return;
    auto& dm = m_dm;                  // engine-state alias — host-only
    edo::ClientChoicePayload cc;
    if (!edo::parseClientChoice(m.payload, cc)) {
        m_dm.logEvent("[HOST CHOICE RECV] truncated payload");
        return;
    }
    auto bail = [&](uint32_t code, const std::string& detail) {
        edo::NetMessage err; err.type = edo::NetMsgType::SyncError;
        edo::SyncErrorPayload p; p.code = code; p.detail = detail;
        edo::serialiseSyncError(err.payload, p);
        m_net.send(err);
        m_dm.logEvent("[HOST CHOICE RECV] promptSeq=" +
                      std::to_string(cc.promptSeq) +
                      "  choiceId=" + std::to_string(cc.choiceId) +
                      "  valid=no  reason=" + detail);
        m_dm.logEvent("[MP SYNC ERROR] code=" + std::to_string(code) +
                      "  detail=" + detail);
    };
    if (cc.promptSeq != m_mpHostPromptSeq) {
        bail(1, "stale promptSeq " + std::to_string(cc.promptSeq) +
                " expected " + std::to_string(m_mpHostPromptSeq));
        return;
    }
    if (cc.promptSeq != m_mpHostChoicesForSeq) {
        bail(1, "choice table out of sync");
        return;
    }
    const SelectionRequest& sel = dm.selection();
    if (!DuelManager::isRealSelect(sel.type)) {
        bail(2, "engine not awaiting input");
        return;
    }
    if ((int)sel.player != m_net.remotePlayerIndex()) {
        bail(2, "prompt not owned by client");
        return;
    }
    auto it = m_mpHostChoices.find(cc.choiceId);
    if (it == m_mpHostChoices.end()) {
        bail(3, "illegal choiceId " + std::to_string(cc.choiceId));
        return;
    }
    std::vector<uint8_t> bytes = it->second.responseBytes;
    if (it->second.multiPick) {
        // Multi-pick confirm — assemble the MSG_SELECT_CARD response
        // {i32 type=0, u32 count, u32 indices...} from extraIndices,
        // validating count + every index against the live selection.
        int n = (int)cc.extraIndices.size();
        if (n < sel.min || n > sel.max) {
            bail(3, "multi-pick count " + std::to_string(n) +
                    " outside " + std::to_string(sel.min) + ".." +
                    std::to_string(sel.max));
            return;
        }
        for (uint32_t v : cc.extraIndices) {
            if (v >= sel.cards.size()) {
                bail(3, "multi-pick index " + std::to_string(v) +
                        " out of range");
                return;
            }
        }
        bytes.assign(8 + 4u * (size_t)n, 0);
        // type=0 already zeroed; write count + indices little-endian.
        uint32_t cnt = (uint32_t)n;
        memcpy(bytes.data() + 4, &cnt, 4);
        for (int i = 0; i < n; ++i) {
            uint32_t v = cc.extraIndices[(size_t)i];
            memcpy(bytes.data() + 8 + 4u * (size_t)i, &v, 4);
        }
    }
    if (bytes.empty()) {
        bail(3, "unimplemented prompt, no bytes registered");
        return;
    }
    // Identify the bytes for the human-readable apply log.
    auto bytesToRespondHint = [&]() -> std::string {
        // Single-int responses (Yes/No, Chain index, Option, Card idx)
        // are the only Stage 2 shapes we emit. Decode the int so the
        // operator can sanity-check "Pass=-1 / Yes=1 / No=0 / card 0".
        if (bytes.size() == 4) {
            int32_t v = (int32_t)((uint32_t)bytes[0] |
                                  ((uint32_t)bytes[1] <<  8) |
                                  ((uint32_t)bytes[2] << 16) |
                                  ((uint32_t)bytes[3] << 24));
            std::ostringstream os;
            switch ((WaitType)sel.type) {
                case WaitType::SelectYesNo:
                case WaitType::SelectEffectYn:
                    os << "respondYesNo(" << (v ? "yes" : "no") << ")"; break;
                case WaitType::SelectChain:
                    os << "respondChain(" << v << ")"; break;
                case WaitType::SelectUnselect:
                    os << "respondInt(" << v << ")"; break;   // Finish = -1
                case WaitType::SelectOption:
                    os << "respondInt(" << v << ")"; break;
                case WaitType::SelectIdleCmd:
                case WaitType::SelectBattleCmd:
                    os << "respondIdleCmd(" << (v & 0xffff) << ", "
                       << ((uint32_t)v >> 16) << ")"; break;
                default:
                    os << "respondInt(" << v << ")"; break;
            }
            return os.str();
        }
        if (bytes.size() == 3 && (WaitType)sel.type == WaitType::SelectPlace) {
            std::ostringstream os;
            os << "respondPlace(" << (int)bytes[0] << ", " << (int)bytes[1]
               << ", " << (int)bytes[2] << ")";
            return os.str();
        }
        if (it->second.multiPick)
            return "respondMultipleCards(" +
                   std::to_string(cc.extraIndices.size()) + " picks)";
        if (bytes.size() == 8 &&
            (WaitType)sel.type == WaitType::SelectUnselect)
            return "respondUnselect(pick)";
        if (bytes.size() == 12 &&
            ((WaitType)sel.type == WaitType::SelectCard ||
             (WaitType)sel.type == WaitType::SelectTribute))
            return "respondSingleCard(full-shape)";
        return "respond(" + std::to_string(bytes.size()) + " bytes)";
    };
    m_dm.logEvent("[HOST CHOICE RECV] promptSeq=" +
                  std::to_string(cc.promptSeq) +
                  "  choiceId=" + std::to_string(cc.choiceId) +
                  "  valid=yes  waitType=" + std::to_string((int)sel.type));
    m_dm.logEvent("[HOST CHOICE APPLY] promptSeq=" +
                  std::to_string(cc.promptSeq) +
                  "  choiceId=" + std::to_string(cc.choiceId) +
                  "  response=" + bytesToRespondHint());
    // Apply on the host's authoritative engine. This goes through the
    // exact same respond() path as a local click — recorder hook fires,
    // ocgcore advances, and the next FieldSnapshot we send will reflect
    // the new state.
    m_dm.respond(bytes.data(), (uint32_t)bytes.size());
    // Defence-in-depth — also done in the recorder hook. Once the
    // engine has applied the response, any cached prompt identity is
    // stale and the next prompt must ship a fresh seq.
    m_mpHostLastPromptIdentity = 0;
    m_mpHostChoices.clear();   // consumed
    m_mpHostChoicesForSeq = 0;
    // Diagnostic + immediate push so the client doesn't have to wait
    // a frame for the engine state to catch up.
    const SelectionRequest& post = dm.selection();
    size_t postChoices = post.cards.size() + post.options.size() +
                         post.idle.size();
    m_dm.logEvent(std::string("[HOST POST-CHOICE PROCESS]"
                  "  running=") + (dm.isRunning() ? "yes" : "no") +
                  "  blocked=" + (dm.isBlocked() ? "yes" : "no") +
                  "  turn=" + std::to_string((int)dm.field().turnPlayer) +
                  "  phase=" + std::to_string((unsigned)dm.field().phase) +
                  "  waitType=" + std::to_string((int)post.type) +
                  "  owner=" + (DuelManager::isRealSelect(post.type)
                                ? std::to_string((int)post.player)
                                : std::string("(none)")) +
                  "  choiceCount=" + std::to_string(postChoices));
    buildAndSendFieldSnapshot();
    buildAndSendPromptSnapshotIfRemote("after-choice");
}

// ─────────────────────────────────────────────────────────────────────
// Host-authoritative multiplayer — client receive path
// ─────────────────────────────────────────────────────────────────────
void UI::handleFieldSnapshot(const edo::NetMessage& m) {
    if (!m_mpHostAuth || !m_net.isClient()) return;
    edo::FieldSnapshotPayload snap;
    if (!edo::parseFieldSnapshot(m.payload, snap)) {
        m_dm.logEvent("[CLIENT SNAPSHOT RECV] truncated payload");
        return;
    }
    int localIdx = m_net.localPlayerIndex();
    edo::applySnapshotToField(snap, m_mpRemoteField, localIdx);
    // Own Extra Deck contents (engine order) — drives the ED viewer.
    m_mpRemoteOwnExtra     = snap.ourExtra;
    m_mpRemoteFieldValid   = true;
    m_mpRemoteDuelActive   = true;
    m_mpAwaitingHostUpdate = false;     // host has caught up
    ++m_mpLastSnapshotSeq;
    m_dm.logEvent("[CLIENT SNAPSHOT RECV] turn=" +
                  std::to_string(snap.turn) +
                  "  phase=" + std::to_string(snap.phase) +
                  "  tp=" + std::to_string(snap.turnPlayer) +
                  "  lp=" + std::to_string(snap.lp[0]) +
                  "/" + std::to_string(snap.lp[1]) +
                  "  ourHand=" + std::to_string(snap.ourHand.size()) +
                  "  ourExtra=" + std::to_string(snap.ourExtra.size()) +
                  "  snapshotSeq=" + std::to_string(m_mpLastSnapshotSeq));
    // Perspective-mapping audit — the bottom row is ALWAYS the local
    // player; both extra counts come straight from the engine snapshot.
    int oppIdx = localIdx ^ 1;
    m_dm.logEvent("[CLIENT SNAPSHOT MAP] localPlayer=" +
                  std::to_string(localIdx) +
                  "  bottomEnginePlayer=" + std::to_string(localIdx) +
                  "  topEnginePlayer=" + std::to_string(oppIdx) +
                  "  bottomExtraCount=" +
                  std::to_string(snap.extraCount[localIdx]) +
                  "  topExtraCount=" +
                  std::to_string(snap.extraCount[oppIdx]) +
                  "  ourHand=" + std::to_string(snap.ourHand.size()) +
                  "  oppHand=" + std::to_string(snap.handCount[oppIdx]));
    logClientDuelStateIfChanged();
}

// Rebuild a synthetic SelectionRequest from the most recent
// PromptSnapshot so currentSelection() can drive the existing
// renderer without any further awareness of the snapshot layer.
void UI::rebuildRemoteSelectionFromPrompt() {
    SelectionRequest s;
    s.type   = (WaitType)m_mpRemoteSel.waitType;
    s.player = m_mpRemoteSel.owner;
    s.min    = m_mpRemoteSel.minSel;
    s.max    = m_mpRemoteSel.maxSel;
    s.forced = (m_mpRemoteSel.forced != 0);
    // SelectPlace: zone mask drives the existing field-tile glow +
    // click handler with zero renderer changes.
    s.placeFlag  = m_mpRemoteSel.placeFlag;
    s.placeCount = (int)m_mpRemoteSel.placeCount;
    // Timing window — drives the offline renderer's "Trigger Effect" /
    // "Quick Effect" / "Chain Response" labelling verbatim.
    s.timing = (TimingContext)m_mpRemoteSel.timing;
    // Rebuild an EffectDesc from the transmitted decoded text + raw id.
    auto descOf = [](const edo::PromptChoice& c) {
        EffectDesc d;
        d.raw  = c.rawDesc;
        d.text = c.desc;
        return d;
    };
    // Map choices into cards[]/options[]/idle[] for renderer compatibility.
    if (s.type == WaitType::SelectOption) {
        for (size_t i = 0; i < m_mpRemoteSel.choices.size(); ++i) {
            const auto& c = m_mpRemoteSel.choices[i];
            s.options.push_back((int)i);
            EffectDesc d = descOf(c);
            // choice.code = the option's source card (host-decoded) —
            // lets the client show "<name> — Effect N" and hover-preview
            // the card exactly like offline.
            if (c.code) { d.cardCode = c.code; d.isCardString = true; }
            s.chainEffects.push_back(std::move(d));
        }
    } else if (s.type == WaitType::SelectIdleCmd ||
               s.type == WaitType::SelectBattleCmd) {
        // Rebuild engine-shaped IdleActions so the field glow, the
        // card-anchored action popup and the bottom strip all render
        // exactly like offline. Phase pseudo-choices (iconHints 4/5/6)
        // become the toBP/toEP/toM2 flags instead of idle entries.
        for (const auto& c : m_mpRemoteSel.choices) {
            if (c.iconHint == 4) { s.toBP = true; continue; }
            if (c.iconHint == 6) { s.toM2 = true; continue; }
            if (c.iconHint == 5) { s.toEP = true; continue; }
            if (c.iconHint != 0) continue;
            IdleAction a;
            a.code      = c.code;
            a.cmd       = (int)c.cmd;
            a.index     = (int)c.index;
            a.con       = c.con;
            a.loc       = (uint8_t)c.loc;
            a.seq       = c.seq;
            a.canDirect = (c.flags & 1u) != 0;
            // Resolve the display name from the local card DB; the wire
            // label is the host-resolved name fallback. The decoded
            // effect description rides in the v3 desc fields so the
            // action popup shows WHICH effect a cmd-5 row activates.
            CardInfo ci = m_db.getCard(c.code);
            a.name   = !ci.name.empty() ? ci.name : c.label;
            a.effect = descOf(c);
            s.idle.push_back(std::move(a));
        }
    } else if (s.type == WaitType::SelectYesNo ||
               s.type == WaitType::SelectEffectYn) {
        // The choices are just Yes/No (iconHints 1/2) — the card CAUSING
        // the prompt rides in srcCode/srcDesc so the modal can render
        // "Activate <Trigger Effect> of <name>?  <effect text>" exactly
        // like offline.
        if (m_mpRemoteSel.srcCode) {
            CardState cs;
            cs.code   = m_mpRemoteSel.srcCode;
            cs.player = m_mpRemoteSel.owner;
            CardInfo ci = m_db.getCard(cs.code);
            cs.name = !ci.name.empty()
                ? ci.name : ("#" + std::to_string(cs.code));
            s.cards.push_back(cs);
            EffectDesc d;
            d.text = m_mpRemoteSel.srcDesc;
            s.chainEffects.push_back(d);
        }
    } else {
        for (const auto& c : m_mpRemoteSel.choices) {
            // Skip non-card placeholder entries (Yes/No/Pass/phase/
            // confirm/finish rows) — only real candidates become cards.
            if (c.iconHint != 0) continue;
            CardState cs;
            cs.code = c.code; cs.loc = c.loc; cs.seq = c.seq;
            cs.name = c.label;
            cs.player = m_mpRemoteSel.owner;
            s.cards.push_back(cs);
            // Parallel effect-description array (SelectChain labels).
            s.chainEffects.push_back(descOf(c));
        }
    }
    m_mpRemoteSelCached = s;
    m_mpRemoteSelValid  = true;
}

void UI::handlePromptSnapshot(const edo::NetMessage& m) {
    if (!m_mpHostAuth || !m_net.isClient()) return;
    edo::PromptSnapshotPayload p;
    if (!edo::parsePromptSnapshot(m.payload, p)) {
        m_dm.logEvent("[CLIENT PROMPT RECV] truncated payload");
        return;
    }
    // Idempotent on seq — if the host (incorrectly) resends the same
    // PromptSnapshot, keep the existing cached SelectionRequest so an
    // in-flight click isn't clobbered. The host's identity-hash dedup
    // means this should normally never fire, but it's also a safety
    // net for transient duplicate packets on the wire.
    if (m_mpRemoteSelValid && p.promptSeq == m_mpRemoteSel.promptSeq) {
        return;
    }
    // Opponent-owned notice (zero choices, owner = host): the host's
    // engine is waiting on the HOST. Drive the "Waiting for opponent —
    // <prompt type>" status line and drop any stale local prompt; do
    // NOT build a selection — there is nothing for this player to pick.
    if (p.owner != (uint8_t)m_net.localPlayerIndex()) {
        m_mpOppPromptWait      = p.waitType;
        m_mpRemoteSelValid     = false;
        m_mpRemoteSelCached    = SelectionRequest{};
        m_mpAwaitingHostUpdate = false;
        m_dm.logEvent("[CLIENT PROMPT RECV] promptSeq=" +
                      std::to_string(p.promptSeq) +
                      "  owner=" + std::to_string((int)p.owner) +
                      "  waitType=" + std::to_string((int)p.waitType) +
                      "  choiceCount=0  (opponent notice)");
        logClientDuelStateIfChanged();
        return;
    }
    m_mpOppPromptWait = 0;              // it's OUR prompt now
    m_mpRemoteSel = std::move(p);
    rebuildRemoteSelectionFromPrompt();
    m_mpAwaitingHostUpdate = false;     // host caught up with a new prompt
    m_dm.logEvent("[CLIENT PROMPT RECV] promptSeq=" +
                  std::to_string(m_mpRemoteSel.promptSeq) +
                  "  owner=" + std::to_string((int)m_mpRemoteSel.owner) +
                  "  waitType=" + std::to_string((int)m_mpRemoteSel.waitType) +
                  "  choiceCount=" + std::to_string(m_mpRemoteSel.choices.size()));
    logClientDuelStateIfChanged();
}

void UI::sendClientChoice(uint32_t choiceId,
                          const std::vector<uint32_t>& extra) {
    if (!m_mpHostAuth || !m_net.isClient()) return;
    if (!m_mpRemoteSelValid) return;
    edo::ClientChoicePayload cc;
    cc.promptSeq    = m_mpRemoteSel.promptSeq;
    cc.choiceId     = choiceId;
    cc.extraIndices = extra;
    edo::NetMessage m; m.type = edo::NetMsgType::ClientChoice;
    edo::serialiseClientChoice(m.payload, cc);
    m_net.send(m);
    m_dm.logEvent("[CLIENT CHOICE SEND] promptSeq=" +
                  std::to_string(cc.promptSeq) +
                  "  choiceId=" + std::to_string(cc.choiceId));
    // Optimistically clear the local prompt so the renderer goes back
    // to "Waiting for host update..." until the next FieldSnapshot or
    // PromptSnapshot arrives. We DO NOT clear m_mpRemoteFieldValid or
    // m_mpRemoteDuelActive — the board stays visible, and the duel
    // state stays "active". Any UI path that confuses an empty
    // selection with "no duel" would incorrectly blank the screen.
    m_mpRemoteSelValid = false;
    m_mpRemoteSelCached = SelectionRequest{};
    m_mpAwaitingHostUpdate = true;
    logClientDuelStateIfChanged();
}

void UI::submitMpChoice(WaitType wt, int idx) {
    // Host / offline / replay → dispatch straight to the engine.
    if (!(m_mpHostAuth && m_net.isClient() && m_mpInDuel)) {
        switch (wt) {
            case WaitType::SelectYesNo:
            case WaitType::SelectEffectYn: m_dm.respondYesNo(idx != 0);  return;
            case WaitType::SelectChain:    m_dm.respondChain(idx);       return;
            case WaitType::SelectCard:
            case WaitType::SelectUnselect: m_dm.respondSingleCard(idx);  return;
            case WaitType::SelectOption:   m_dm.respondInt(idx);         return;
            default:                       m_dm.respondInt(idx);         return;
        }
    }
    // Host-authoritative client: map the local choice into the host's
    // choiceId table and ship a ClientChoice packet.
    if (!m_mpRemoteSelValid) {
        m_dm.logEvent("[CLIENT CHOICE SEND] dropped — no active prompt");
        return;
    }
    auto byIcon = [&](uint32_t hint) -> uint32_t {
        for (const auto& c : m_mpRemoteSel.choices)
            if (c.iconHint == hint) return c.choiceId;
        return 0;
    };
    auto byIndex = [&](int i) -> uint32_t {
        if (i < 0 || (size_t)i >= m_mpRemoteSel.choices.size()) return 0;
        return m_mpRemoteSel.choices[(size_t)i].choiceId;
    };
    uint32_t cid = 0;
    switch (wt) {
        case WaitType::SelectYesNo:
        case WaitType::SelectEffectYn:
            cid = byIcon(idx ? 1u : 2u);
            break;
        case WaitType::SelectChain:
            cid = (idx < 0) ? byIcon(3u) : byIndex(idx);
            break;
        case WaitType::SelectCard:
        case WaitType::SelectUnselect:
        case WaitType::SelectOption:
            cid = byIndex(idx);
            break;
        default:
            cid = byIndex(idx);
            break;
    }
    if (!cid) {
        m_dm.logEvent("[CLIENT CHOICE SEND] dropped — no choiceId for "
                      "wt=" + std::to_string((int)wt) +
                      "  idx=" + std::to_string(idx));
        return;
    }
    // Look up the label for the log line so the operator can confirm
    // (e.g.) "user clicked Pass / No Response" matches the host's
    // applied response.
    std::string label;
    for (const auto& c : m_mpRemoteSel.choices)
        if (c.choiceId == cid) { label = c.label; break; }
    m_dm.logEvent("[CLIENT CHOICE SEND] promptSeq=" +
                  std::to_string(m_mpRemoteSel.promptSeq) +
                  "  choiceId=" + std::to_string(cid) +
                  "  waitType=" + std::to_string((int)wt) +
                  "  label=" + label);
    sendClientChoice(cid);
}

// SelectIdleCmd / SelectBattleCmd router. Every UI click that used to call
// m_dm.respondIdleCmd directly goes through here so the host-auth client
// converts it into a ClientChoice instead of feeding its (stopped) engine.
void UI::submitIdleCmd(int cmd, int index, const char* label) {
    if (!(m_mpHostAuth && m_net.isClient() && m_mpInDuel)) {
        m_dm.respondIdleCmd(cmd, index);
        return;
    }
    if (!m_mpRemoteSelValid) {
        m_dm.logEvent("[CLIENT CHOICE SEND] dropped — no active prompt"
                      "  (idleCmd " + std::to_string(cmd) + "," +
                      std::to_string(index) + ")");
        return;
    }
    uint32_t cid = 0;
    for (const auto& c : m_mpRemoteSel.choices) {
        if (c.iconHint != 0 && c.iconHint != 4 &&
            c.iconHint != 5 && c.iconHint != 6) continue;
        if ((int)c.cmd == cmd && (int)c.index == index) {
            cid = c.choiceId;
            break;
        }
    }
    if (!cid) {
        m_dm.logEvent("[CLIENT CHOICE SEND] dropped — no choiceId for "
                      "idleCmd cmd=" + std::to_string(cmd) +
                      "  index=" + std::to_string(index));
        return;
    }
    m_dm.logEvent("[CLIENT CHOICE SEND] promptSeq=" +
                  std::to_string(m_mpRemoteSel.promptSeq) +
                  "  choiceId=" + std::to_string(cid) +
                  "  waitType=" + std::to_string((int)m_mpRemoteSel.waitType) +
                  "  label=" + ((label && label[0]) ? label : "(idle cmd)"));
    sendClientChoice(cid);
}

// SelectPlace router — the clicked (loc, seq) tile maps onto the host's
// per-zone choice table.
void UI::submitPlace(int player, int loc, int seq) {
    if (!(m_mpHostAuth && m_net.isClient() && m_mpInDuel)) {
        m_dm.respondPlace(player, loc, seq);
        return;
    }
    if (!m_mpRemoteSelValid) {
        m_dm.logEvent("[CLIENT CHOICE SEND] dropped — no active prompt"
                      "  (place loc=" + std::to_string(loc) +
                      " seq=" + std::to_string(seq) + ")");
        return;
    }
    uint32_t cid = 0;
    std::string label;
    for (const auto& c : m_mpRemoteSel.choices) {
        if ((int)c.loc == loc && c.seq == (uint32_t)seq) {
            cid = c.choiceId;
            label = c.label;
            break;
        }
    }
    if (!cid) {
        m_dm.logEvent("[CLIENT CHOICE SEND] dropped — no choiceId for "
                      "place loc=" + std::to_string(loc) +
                      "  seq=" + std::to_string(seq));
        return;
    }
    m_dm.logEvent("[CLIENT CHOICE SEND] promptSeq=" +
                  std::to_string(m_mpRemoteSel.promptSeq) +
                  "  choiceId=" + std::to_string(cid) +
                  "  waitType=" + std::to_string((int)m_mpRemoteSel.waitType) +
                  "  label=" + label);
    sendClientChoice(cid);
}

// SelectCard / SelectTribute multi-pick confirm router. `indices` are the
// engine indices the user ticked — shipped via extraIndices; the host
// validates and assembles the actual response buffer.
void UI::submitMultiCards(const std::vector<int>& indices) {
    if (!(m_mpHostAuth && m_net.isClient() && m_mpInDuel)) {
        m_dm.respondMultipleCards(indices);
        return;
    }
    if (!m_mpRemoteSelValid) {
        m_dm.logEvent("[CLIENT CHOICE SEND] dropped — no active prompt"
                      "  (multi-pick)");
        return;
    }
    uint32_t cid = 0;
    for (const auto& c : m_mpRemoteSel.choices)
        if (c.iconHint == 8) { cid = c.choiceId; break; }
    if (!cid) {
        m_dm.logEvent("[CLIENT CHOICE SEND] dropped — no multi-pick "
                      "confirm choice in prompt");
        return;
    }
    std::vector<uint32_t> extra;
    extra.reserve(indices.size());
    for (int i : indices) extra.push_back((uint32_t)i);
    m_dm.logEvent("[CLIENT CHOICE SEND] promptSeq=" +
                  std::to_string(m_mpRemoteSel.promptSeq) +
                  "  choiceId=" + std::to_string(cid) +
                  "  waitType=" + std::to_string((int)m_mpRemoteSel.waitType) +
                  "  label=Confirm selection (" +
                  std::to_string(indices.size()) + " picks)");
    sendClientChoice(cid, extra);
}

// SelectUnselect router. idx >= 0 picks the idx-th candidate; idx == -1 is
// Finish (legal only when the prompt is not forced).
void UI::submitUnselect(int idx) {
    if (!(m_mpHostAuth && m_net.isClient() && m_mpInDuel)) {
        if (idx < 0) m_dm.respondInt(-1);
        else         m_dm.respondUnselect(idx);
        return;
    }
    if (!m_mpRemoteSelValid) {
        m_dm.logEvent("[CLIENT CHOICE SEND] dropped — no active prompt"
                      "  (unselect " + std::to_string(idx) + ")");
        return;
    }
    uint32_t cid = 0;
    if (idx < 0) {
        for (const auto& c : m_mpRemoteSel.choices)
            if (c.iconHint == 7) { cid = c.choiceId; break; }
    } else if ((size_t)idx < m_mpRemoteSel.choices.size()) {
        // Candidate rows come FIRST in the host's choice order, so the
        // engine index maps 1:1 onto the choice array prefix.
        const auto& c = m_mpRemoteSel.choices[(size_t)idx];
        if (c.iconHint == 0) cid = c.choiceId;
    }
    if (!cid) {
        m_dm.logEvent("[CLIENT CHOICE SEND] dropped — no choiceId for "
                      "unselect idx=" + std::to_string(idx));
        return;
    }
    m_dm.logEvent("[CLIENT CHOICE SEND] promptSeq=" +
                  std::to_string(m_mpRemoteSel.promptSeq) +
                  "  choiceId=" + std::to_string(cid) +
                  "  waitType=" + std::to_string((int)m_mpRemoteSel.waitType) +
                  "  label=" + (idx < 0 ? std::string("Finish selection")
                                        : ("pick #" + std::to_string(idx))));
    sendClientChoice(cid);
}

// Extra Deck contents for the GY/BN/ED viewer. The host-auth client has
// no running engine, so its OWN list comes from the snapshot mirror; the
// opponent's list is always empty there (hidden info — count only).
// Offline / host / replay query the live engine exactly as before.
std::vector<uint32_t> UI::viewerExtraDeckCodes(int player) {
    if (usingRemoteField()) {
        if (player == m_net.localPlayerIndex()) return m_mpRemoteOwnExtra;
        return {};
    }
    return m_dm.extraDeckCodes(player);
}

void UI::handleSyncError(const edo::NetMessage& m) {
    edo::SyncErrorPayload p;
    if (!edo::parseSyncError(m.payload, p)) return;
    m_dm.logEvent("[MP SYNC ERROR] code=" + std::to_string(p.code) +
                  "  detail=" + p.detail);
    m_mpDesynced      = true;
    m_mpDesyncSummary = "sync error from peer: " + p.detail;
    pushToast("Multiplayer sync error — see Debug Log",
              IM_COL32(232, 110, 100, 255), 4.0);
}

// Host → client, sent exactly once when the host's authoritative engine ends.
// The client never runs an ocgcore, so without this it would never learn the
// duel is over. winner is the engine seat (0/1, else a draw); reason is a
// DuelManager win-reason code (see winReasonText).
void UI::sendGameOver(int winner, int reason) {
    if (!m_mpHostAuth || !m_net.isHost()) return;
    edo::NetMessage m; m.type = edo::NetMsgType::GameOver;
    edo::putU32(m.payload, (uint32_t)(int32_t)winner);
    edo::putU32(m.payload, (uint32_t)(int32_t)reason);
    m_net.send(m);
    m_dm.logEvent("[MP GAMEOVER SEND] winner=" + std::to_string(winner) +
                  "  reason=" + std::to_string(reason));
}

void UI::handleGameOver(const edo::NetMessage& m) {
    // Client-only: the host's GameOver lets us show the end screen even though
    // our local DuelManager never ran. (Host ignores its own — it already
    // knows via m_dm.isDone().)
    if (!m_mpHostAuth || !m_net.isClient()) return;
    edo::NetReader r(m.payload);
    m_mpRemoteWinner = (int)(int32_t)r.u32();
    m_mpRemoteReason = (int)(int32_t)r.u32();
    m_mpRemoteDone   = true;
    const int me  = m_net.localPlayerIndex();
    const bool won = (m_mpRemoteWinner == me);
    const char* txt = (m_mpRemoteWinner != 0 && m_mpRemoteWinner != 1)
                          ? "Duel ended in a draw"
                          : (won ? "You win!" : "You lose");
    pushToast(txt, won ? IM_COL32(110, 220, 140, 255)
                       : IM_COL32(232, 110, 100, 255), 4.0);
    pushGameLog(txt, IM_COL32(232, 196, 110, 255));
    m_dm.logEvent("[MP GAMEOVER RECV] winner=" +
                  std::to_string(m_mpRemoteWinner) + "  reason=" +
                  std::to_string(m_mpRemoteReason) + "  localSeat=" +
                  std::to_string(me));
}

// Client → host. The client has no authoritative engine, so it can't end the
// duel itself — it tells the host which seat is conceding and the host forfeits
// that seat. The resulting GameOver flows back to the client normally.
void UI::sendSurrender() {
    if (!m_mpHostAuth || !m_net.isClient()) return;
    edo::NetMessage m; m.type = edo::NetMsgType::Surrender;
    edo::putU32(m.payload, (uint32_t)(int32_t)m_net.localPlayerIndex());
    m_net.send(m);
    m_dm.logEvent("[MP SURRENDER SEND] seat=" +
                  std::to_string(m_net.localPlayerIndex()));
    pushToast("You surrendered", IM_COL32(232, 110, 100, 255), 3.0);
}

void UI::handleSurrender(const edo::NetMessage& m) {
    // Host-only: a client conceded. Forfeit its seat on the authoritative
    // engine; the per-frame pump then ships GameOver to the client.
    if (!m_mpHostAuth || !m_net.isHost()) return;
    edo::NetReader r(m.payload);
    int seat = (int)(int32_t)r.u32();
    if (seat != 0 && seat != 1) seat = 1;   // defensive: clients are seat 1
    m_dm.logEvent("[MP SURRENDER RECV] seat=" + std::to_string(seat));
    m_dm.forfeit(seat);
    pushToast("Opponent surrendered", IM_COL32(110, 220, 140, 255), 3.0);
}

void UI::handleNetMessage(const edo::NetMessage& m) {
    edo::NetReader r(m.payload);
    switch (m.type) {
    case edo::NetMsgType::Hello: {
        uint32_t ver  = r.u32();
        std::string nm = r.str();
        m_net.setPeerName(nm);
        m_dm.logEvent("[MULTI CONNECTED] peer=" + nm +
                      "  ver=" + std::to_string(ver) +
                      "  localIdx=" +
                      std::to_string(m_net.localPlayerIndex()));
        pushToast("Connected: " + nm,
                  IM_COL32(110, 220, 140, 255), 2.2);
        // After hello, if we already had a deck picked, advertise it.
        if (m_mpDeckIdx >= 0) sendMpDeckInfo();
        if (m_mpReady) sendMpReady(true);
        break;
    }
    case edo::NetMsgType::DeckInfo: {
        std::string deckName = r.str();
        auto main  = r.u32arr();
        auto extra = r.u32arr();
        auto side  = r.u32arr();
        if (!r.ok || main.empty()) {
            pushToast("Bad deck from opponent",
                      IM_COL32(232, 110, 100, 255), 2.4);
            break;
        }
        m_mpRemoteDeck.name  = deckName;
        m_mpRemoteDeck.main  = main;
        m_mpRemoteDeck.extra = extra;
        m_mpRemoteDeck.side  = side;
        m_mpRemoteDeckRcvd = true;
        auto extraFirst5 = [&]() {
            std::string s;
            for (size_t i = 0; i < extra.size() && i < 5; ++i) {
                if (!s.empty()) s += ",";
                s += std::to_string(extra[i]);
            }
            return s.empty() ? std::string("(none)") : s;
        };
        m_dm.logEvent("[MP DECK RECEIVED] from=" +
                      std::string(m_net.isHost() ? "P2" : "P1") +
                      "  name=" + deckName +
                      "  main=" + std::to_string(main.size()) +
                      "  extra=" + std::to_string(extra.size()) +
                      "  side="  + std::to_string(side.size()) +
                      "  extraFirst5=" + extraFirst5());
        break;
    }
    case edo::NetMsgType::Ready: {
        uint8_t v = r.u8();
        m_mpRemoteReady = (v != 0);
        m_dm.logEvent(std::string("[MULTI RECV] Ready=") +
                      (m_mpRemoteReady ? "yes" : "no"));
        break;
    }
    case edo::NetMsgType::StartDuel: {
        uint64_t seed = r.u64();
        (void)r.u64();                    // ruleFlags reserved
        uint32_t lp = r.u32();
        uint32_t hand = r.u32();
        uint32_t draw = r.u32();
        std::string p1Name = r.str();
        std::string p2Name = r.str();
        auto readDeck = [](edo::NetReader& rd) {
            Deck d;
            auto m  = rd.u32arr();
            auto e  = rd.u32arr();
            auto s  = rd.u32arr();
            d.main  = m;
            d.extra = e;
            d.side  = s;
            return d;
        };
        Deck p1 = readDeck(r);
        Deck p2 = readDeck(r);
        if (!r.ok) {
            pushToast("Bad StartDuel from host",
                      IM_COL32(232, 110, 100, 255), 3.0);
            break;
        }
        auto first5c = [](const Deck& d) {
            std::string s;
            for (size_t i = 0; i < d.main.size() && i < 5; ++i) {
                if (!s.empty()) s += ",";
                s += std::to_string(d.main[i]);
            }
            return s;
        };
        m_dm.logEvent("[MULTI DECK MAP] localRole=Client  localPlayerIndex=1"
                      "  P1 name=" + p1Name +
                      "  main/extra/side=" + std::to_string(p1.main.size()) +
                      "/" + std::to_string(p1.extra.size()) +
                      "/" + std::to_string(p1.side.size()) +
                      "  first5=" + first5c(p1) +
                      "  P2 name=" + p2Name +
                      "  main/extra/side=" + std::to_string(p2.main.size()) +
                      "/" + std::to_string(p2.extra.size()) +
                      "/" + std::to_string(p2.side.size()) +
                      "  first5=" + first5c(p2));
        m_dm.logEvent("[MULTI START] seed=" + std::to_string(seed) +
                      "  P1=" + p1Name + "  P2=" + p2Name +
                      "  P1 main=" + std::to_string(p1.main.size()));
        finalizeReplay("entering multiplayer (client)");
        if (m_dm.isRunning()) m_dm.endDuel();
        resetMpResponseState();
        // Client seat comes from net mode (Client = 1). Clear any leftover
        // offline coin-toss seat override.
        m_dm.setHumanSeat(0);
        m_net.clearSeatOverride();
        if (m_mpHostAuth) {
            // Host-authoritative: the client renders entirely from
            // snapshots. We do NOT start a local ocgcore — that was the
            // root cause of every prompt desync. The renderer's
            // currentField()/currentSelection() will switch over to
            // m_mpRemoteField as soon as the first FieldSnapshot
            // arrives from the host.
            m_mpInDuel = true;
            m_mpRemoteDuelActive = true;   // network-side latch
            m_screen   = Screen::Duel;
            m_anim.clear();
            m_sfxObsInited    = false;
            m_endGameSfxFired = false;
            m_dm.logEvent("[MP MODE] host-authoritative  role=Client"
                          "  seed=" + std::to_string(seed) +
                          "  P1=" + p1Name + "  P2=" + p2Name);
            m_dm.logEvent("[MULTI START LOCAL] client-side ocgcore NOT "
                          "started — rendering from host snapshots");
            pushToast(std::string("Multiplayer duel: ") + p1Name +
                      " vs " + p2Name,
                      IM_COL32(180, 220, 255, 255), 2.4);
        } else {
            // Legacy dual-engine lockstep — kept for debug/regression.
            m_dm.setForcedSeed(seed);
            m_dm.setLocalMode(false);
            m_dm.setSuppressAutoResolve(true);
            m_dm.setResponseRecorder(
                [this](const void* d, uint32_t n){ mpOnLocalResponse(d, n); });
            m_dm.logEvent("[MP MODE] dual-engine (lockstep)  role=Client");
            m_dm.logEvent("[MULTI START LOCAL] engineP0=" + p1Name +
                          " (main " + std::to_string(p1.main.size()) +
                          ")  engineP1=" + p2Name + " (main " +
                          std::to_string(p2.main.size()) + ")");
            if (m_dm.startDuel(p1, p2, lp, hand, draw)) {
                m_mpInDuel = true;
                m_screen   = Screen::Duel;
                m_anim.clear();
                m_sfxObsInited    = false;
                m_endGameSfxFired = false;
                pushToast(std::string("Multiplayer duel: ") + p1Name +
                          " vs " + p2Name,
                          IM_COL32(180, 220, 255, 255), 2.4);
            }
        }
        break;
    }
    case edo::NetMsgType::EngineResponse: {
        // Legacy dual-engine path — only honored when host-auth is off.
        // In host-auth mode the client never sends responses (it sends
        // ClientChoice instead) and the host doesn't need to ingest its
        // own engine's bytes back — they were applied locally.
        if (m_mpHostAuth) {
            m_dm.logEvent("[MULTI RECV] EngineResponse ignored "
                          "(host-authoritative mode)");
            break;
        }
        uint8_t player          = r.u8();
        uint32_t seq            = r.u32();
        uint32_t waitTypeAtSend = r.u32();
        uint32_t n              = r.u32();
        if (!r.ok || r.left < n) {
            m_dm.logEvent("[MULTI RECV] EngineResponse: truncated payload");
            break;
        }
        // Duplicate guard — anything with seq we've already processed for
        // this owner is dropped silently (apart from a log line). The
        // outgoing seq is strictly increasing per local sender.
        if (player < 2 && seq != 0 && seq <= m_mpLastSeenSeq[player]) {
            m_dm.logEvent("[MULTI DUP RESPONSE IGNORED] owner=" +
                          std::to_string((int)player) +
                          "  seq=" + std::to_string(seq) +
                          "  lastSeen=" +
                          std::to_string(m_mpLastSeenSeq[player]));
            break;
        }
        if (player < 2) m_mpLastSeenSeq[player] = seq;

        MpQueuedResponse q;
        q.owner = (int)player;
        q.seq   = seq;
        q.waitTypeAtSend = (int)waitTypeAtSend;
        q.bytes.assign(r.p, r.p + n);
        m_mpQueue.push_back(std::move(q));
        {
            auto& dm = m_dm;          // legacy queue logging — engine state
            m_dm.logEvent("[MULTI QUEUE RESPONSE] owner=" +
                          std::to_string((int)player) +
                          "  seq=" + std::to_string(seq) +
                          "  bytes=" + std::to_string(n) +
                          "  currentPromptOwner=" +
                          (DuelManager::isRealSelect(dm.selection().type)
                              ? std::to_string((int)dm.selection().player)
                              : std::string("(none)")) +
                          "  currentWaitType=" +
                          std::to_string((int)dm.selection().type));
        }
        // Try to drain immediately — if the engine is already at the
        // matching prompt, no point waiting another frame.
        tryFeedQueuedMpResponses();
        break;
    }
    case edo::NetMsgType::Chat: {
        std::string txt = r.str();
        pushGameLog(std::string("<chat> ") + txt,
                    IM_COL32(180, 220, 255, 255));
        break;
    }
    case edo::NetMsgType::Disconnect: {
        std::string reason = r.str();
        m_dm.logEvent("[MULTI DISCONNECT] reason=" + reason);
        pushToast(std::string("Peer disconnected: ") + reason,
                  IM_COL32(232, 110, 100, 255), 3.0);
        // Stay in the duel so the user can review state; the gate will
        // pause the engine because remote responses will never arrive.
        break;
    }
    case edo::NetMsgType::Ping: {
        edo::NetMessage pong; pong.type = edo::NetMsgType::Pong;
        edo::putU64(pong.payload, r.u64());
        m_net.send(pong);
        break;
    }
    case edo::NetMsgType::Pong: break;
    case edo::NetMsgType::PromptState: {
        // Legacy dual-engine handshake — only meaningful when host-auth
        // is OFF. In host-auth mode the snapshot stream is the truth;
        // ignore stray packets from older peers.
        if (!m_mpHostAuth) handleRemotePromptState(m);
        break;
    }
    case edo::NetMsgType::FieldSnapshot:   handleFieldSnapshot(m); break;
    case edo::NetMsgType::PromptSnapshot:  handlePromptSnapshot(m); break;
    case edo::NetMsgType::ClientChoice:    handleClientChoice(m); break;
    case edo::NetMsgType::GameEvent:       /* Stage 3 fx hints */    break;
    case edo::NetMsgType::SyncError:       handleSyncError(m); break;
    case edo::NetMsgType::GameOver:        handleGameOver(m); break;
    case edo::NetMsgType::Surrender:       handleSurrender(m); break;
    case edo::NetMsgType::Error: {
        std::string txt = r.str();
        m_dm.logEvent("[MULTI ERROR] " + txt);
        break;
    }
    // ── Relay / room control (online play) ───────────────────────────────
    case edo::NetMsgType::RoomCreated: {
        // We are the host; the server allocated a room code. Surface it so
        // the player can share it. The guest's arrival comes later as
        // RoomPeerJoined, which kicks off the Hello/deck handshake.
        m_mpRoomCode       = r.str();
        m_mpRoomActive     = true;
        m_mpRelayConnecting = false;
        m_mpRoomError.clear();
        m_dm.logEvent("[ONLINE] room created, code=" + m_mpRoomCode);
        pushToast("Room created: " + m_mpRoomCode,
                  IM_COL32(110, 220, 140, 255), 3.0);
        break;
    }
    case edo::NetMsgType::RoomJoined: {
        // We are the guest; we're in the room. isHost should be 0. The host
        // name rides along so the lobby can show the peer immediately.
        uint8_t  isHost   = r.u8();
        std::string hostNm = r.str();
        (void)isHost;
        m_mpRoomActive      = true;
        m_mpRelayConnecting = false;
        m_mpRoomError.clear();
        if (!hostNm.empty()) m_net.setPeerName(hostNm);
        m_dm.logEvent("[ONLINE] joined room, host=" + hostNm);
        pushToast("Joined room — host: " +
                  (hostNm.empty() ? std::string("(host)") : hostNm),
                  IM_COL32(110, 220, 140, 255), 3.0);
        // Guest drives the handshake now that the room is formed.
        mpKickoffHandshake();
        break;
    }
    case edo::NetMsgType::RoomPeerJoined: {
        // Host side: the guest has arrived. Start the existing Hello/deck/
        // ready handshake (relayed through the server to the guest).
        std::string guestNm = r.str();
        if (!guestNm.empty()) m_net.setPeerName(guestNm);
        m_dm.logEvent("[ONLINE] guest joined: " + guestNm);
        pushToast(std::string("Opponent joined: ") +
                  (guestNm.empty() ? std::string("guest") : guestNm),
                  IM_COL32(110, 220, 140, 255), 3.0);
        mpKickoffHandshake();
        break;
    }
    case edo::NetMsgType::RoomError: {
        std::string msg = r.str();
        m_mpRoomError       = msg;
        m_mpRelayConnecting = false;
        m_dm.logEvent("[ONLINE] room error: " + msg);
        pushToast("Online: " + msg, IM_COL32(232, 110, 100, 255), 4.0);
        // A failed create/join leaves the relay socket up but useless —
        // drop it so the user can retry cleanly.
        m_net.disconnect("room error");
        m_mpRoomActive = false;
        break;
    }
    case edo::NetMsgType::RoomClosed: {
        std::string reason = r.str();
        m_dm.logEvent("[ONLINE] room closed: " + reason);
        pushToast("Opponent left: " + reason,
                  IM_COL32(232, 110, 100, 255), 4.0);
        // If we were mid-duel this trips the same Connection Lost path as a
        // LAN disconnect; otherwise we just fall back to the lobby state.
        m_mpRemoteDeckRcvd = false;
        m_mpRemoteReady    = false;
        m_mpHandshakeSent  = false;
        break;
    }
    }
}

void UI::pumpMultiplayer() {
    if (m_net.isOffline()) return;
    while (m_net.poll()) {
        edo::NetMessage m;
        if (!m_net.receive(m)) break;
        handleNetMessage(m);
    }
    // Surface connection-lost as a one-shot modal trigger.
    if (m_mpInDuel && m_net.state() == edo::NetState::Disconnected &&
        !m_mpConnLostShown) {
        m_mpConnLostShown = true;
        pushToast("Connection lost — duel paused",
                  IM_COL32(232, 110, 100, 255), 4.0);
    }
    // ── Host-authoritative path (the default) ───────────────────────
    // Host: rebuild + ship a FieldSnapshot whenever the engine state
    //       has actually changed, and a PromptSnapshot whenever the
    //       current selection belongs to the client.
    // Client: nothing to do here; rendering is driven purely from
    //       inbound FieldSnapshot / PromptSnapshot messages.
    if (m_mpHostAuth) {
        if (m_net.isHost() && m_mpInDuel) {
            buildAndSendFieldSnapshot();
            buildAndSendPromptSnapshotIfRemote("frame");
            // The authoritative engine ended — push a final board state then
            // tell the client once so it can render the Game Over panel (its
            // own DuelManager never runs in host-auth mode).
            if (m_dm.isDone() && !m_mpGameOverSent) {
                sendGameOver(m_dm.winner(), m_dm.winReason());
                m_mpGameOverSent = true;
            }
        }
        // Per-frame state log for the client — fires only once per
        // state transition (key dedup inside the helper).
        logClientDuelStateIfChanged();
        return;
    }

    // ── Legacy dual-engine lockstep (opt-in only) ───────────────────
    // Drain the response queue against the engine's current prompt
    // and broadcast prompt-state fingerprints for the desync detector.
    tryFeedQueuedMpResponses();
    sendPromptStateIfChanged();
}

// Drains queued remote EngineResponse bytes into the engine — but only
// for prompts whose owner matches the response owner. Loops until the
// queue is empty or the next item doesn't match the current prompt;
// each successful feed may advance the engine to a new prompt that the
// next queued item might now match.
void UI::tryFeedQueuedMpResponses() {
    if (!m_mpInDuel || m_net.isOffline()) return;
    if (!m_dm.isRunning())                return;
    auto& dm = m_dm;                  // engine-state alias — legacy path
    int safety = 16;                  // guard against pathological loops
    while (safety-- > 0) {
        const SelectionRequest& sel = dm.selection();
        bool isReal    = DuelManager::isRealSelect(sel.type);
        bool isBlocked = dm.isBlocked();
        if (!isReal && !isBlocked) return;  // engine idle — nothing to do
        int promptOwner = isReal ? (int)sel.player : -1;

        // Build a single fingerprint for throttle-once-per-state-change.
        uint64_t waitKey =
            ((uint64_t)(int)sel.type      << 48) |
            ((uint64_t)(promptOwner + 1)  << 40) |   // +1 so -1 ≠ 0
            ((uint64_t)(isBlocked ? 1 : 0) << 32) |
            (uint64_t)m_mpQueue.size();

        if (promptOwner < 0) {
            // We don't know who the engine is asking. This happens when
            // m_blocked is true but the message parser never populated
            // m_selection (an unrecognised prompt type, or a transient
            // mid-process gap). We CANNOT safely feed a queued response
            // here — it would target the wrong owner. Surface once per
            // unique state via the MP diagnostic modal in drawDuel.
            if (waitKey != m_mpLastWaitKey) {
                m_mpLastWaitKey = waitKey;
                m_dm.logEvent("[MULTI QUEUED BUT OWNER UNKNOWN]"
                              "  queuedCount=" +
                              std::to_string(m_mpQueue.size()) +
                              "  currentWaitType=" +
                              std::to_string((int)sel.type) +
                              "  blocked=" + (isBlocked ? "yes" : "no"));
            }
            return;
        }
        // Local-owned prompts are handled by the UI / local auto-pass;
        // the queue only serves remote responses.
        if (promptOwner == m_net.localPlayerIndex()) {
            m_mpLastWaitKey = 0;   // clear throttle so next remote wait logs
            return;
        }
        // Find a queued response whose owner matches the prompt owner
        // AND was sent against a matching waitType. Mismatched-waitType
        // entries are STALE — they were intended for an earlier prompt
        // the engine has already passed — and feeding them now would
        // get rejected (MSG_RETRY storm + duel parked). Drop them.
        //
        // EXCEPTION: when the engine is parked in RawPrompt (parser gap),
        // we don't know what it's really asking for, so we can't safely
        // declare anything stale. Preserve the queue intact — the user
        // can copy the diagnostic and exit; nothing gets blindly fed.
        size_t hit = (size_t)-1;
        const bool inRawPrompt = (sel.type == WaitType::RawPrompt);
        for (size_t i = 0; i < m_mpQueue.size(); ) {
            const auto& q = m_mpQueue[i];
            if (q.owner != promptOwner) { ++i; continue; }
            if (inRawPrompt) { ++i; continue; }    // never discard in RawPrompt
            // waitTypeAtSend == 0 is the bootstrap value (recv side stamps
            // its OWN selection.type, which may have been None at receive
            // time even though the engine has since advanced). Treat 0
            // as a wildcard so we don't accidentally discard valid pre-
            // routing responses.
            if (q.waitTypeAtSend != 0 &&
                q.waitTypeAtSend != (int)sel.type) {
                m_dm.logEvent("[MULTI STALE RESPONSE DISCARDED] owner=" +
                              std::to_string(q.owner) +
                              "  seq=" + std::to_string(q.seq) +
                              "  waitTypeAtSend=" +
                              std::to_string(q.waitTypeAtSend) +
                              "  currentWaitType=" +
                              std::to_string((int)sel.type));
                m_mpQueue.erase(m_mpQueue.begin() + i);
                continue;
            }
            hit = i;
            break;
        }
        // In RawPrompt we never feed — the engine state is unknown and
        // any blind feed would corrupt the duel. Bail out so the MP
        // diagnostic modal can surface the captured msg id + rawHex.
        if (inRawPrompt) {
            return;
        }
        if (hit == (size_t)-1) {
            // Nothing matches — engine has to keep waiting. Log ONCE
            // per state transition (not every frame).
            if (waitKey != m_mpLastWaitKey) {
                m_mpLastWaitKey = waitKey;
                // Include chain option count + forced flag so the operator
                // can audit "did the host correctly wait for a real chain
                // choice, or is it stuck on something the engine should
                // have auto-resolved?" When type==SelectChain and
                // chainOptions==0 && !forced, BOTH peers' DuelManagers
                // auto-pass internally — if the host is logging WAIT
                // REMOTE here, the chain must have real options to pick.
                int chainOptions = (sel.type == WaitType::SelectChain)
                    ? (int)sel.cards.size() : -1;
                m_dm.logEvent("[MULTI WAIT REMOTE] promptOwner=" +
                              std::to_string(promptOwner) +
                              "  localPlayer=" +
                              std::to_string(m_net.localPlayerIndex()) +
                              "  waitType=" + std::to_string((int)sel.type) +
                              "  chainOptions=" + std::to_string(chainOptions) +
                              "  forced=" + (sel.forced ? "yes" : "no") +
                              "  queuedCount=" +
                              std::to_string(m_mpQueue.size()));
            }
            return;
        }
        MpQueuedResponse q = std::move(m_mpQueue[hit]);
        m_mpQueue.erase(m_mpQueue.begin() + hit);
        m_dm.logEvent("[MULTI FEED QUEUED RESPONSE] owner=" +
                      std::to_string(q.owner) +
                      "  seq=" + std::to_string(q.seq) +
                      "  bytes=" + std::to_string(q.bytes.size()) +
                      "  promptOwner=" + std::to_string(promptOwner) +
                      "  waitType=" + std::to_string((int)sel.type));
        m_mpFeedingRemote = true;
        m_dm.respond(q.bytes.data(), (uint32_t)q.bytes.size());
        m_mpFeedingRemote = false;
        m_mpLastWaitKey = 0;   // engine just advanced — clear throttle
        // Loop — the engine may have advanced to another remote-owned
        // prompt that another queued response answers.
    }
}

// LEGACY — kept as a no-op so callers in older patches still link.
// 0-option chain auto-pass is now handled silently inside DuelManager
// (see m_internalAutoResolve in DuelManager.cpp): both peers reach the
// same state deterministically and pass locally without any network
// traffic. No UI-side driver needed.
void UI::maybeAutoPassMpZeroOptionChain() { return; }

// Original implementation preserved below in a #if-0 block for review;
// removable once we're confident the silent path is stable.
#if 0
void UI::maybeAutoPassMpZeroOptionChain_LEGACY() {
    if (!m_mpInDuel || m_net.isOffline()) return;
    if (!m_dm.isRunning()) return;
    const SelectionRequest& sel = currentSelection();
    // Reset the anti-repeat key whenever we're not on a SelectChain — so
    // a fresh chain prompt with the same shape as the previous one (same
    // owner, same 0 options) gets a fresh auto-pass instead of being
    // mistaken for the old one.
    if (sel.type != WaitType::SelectChain) {
        m_mpLastAutoPassKey = 0;
        return;
    }
    if (!sel.cards.empty()) return;       // there ARE options → user chooses
    if (sel.forced)         return;       // forced chains can't pass at all
    // Build an identity key for THIS prompt instance so we never double-fire.
    // (waitType<<48) | (owner<<40) | (forced<<32) | cardCount as a coarse
    // fingerprint — once the engine moves to a new prompt the key changes.
    uint64_t key =
        ((uint64_t)(int)sel.type    << 48) |
        ((uint64_t)(sel.player & 1) << 40) |
        ((uint64_t)(sel.forced ? 1 : 0) << 32) |
        (uint64_t)sel.cards.size();
    if (key == m_mpLastAutoPassKey) return;     // already passed this one
    if (!isLocalPromptOwner()) return;          // remote will pass

    m_mpLastAutoPassKey = key;
    m_dm.logEvent(std::string("[MULTI AUTO PASS] owner=") +
                  std::to_string((int)sel.player) +
                  "  localPlayer=" + std::to_string(m_net.localPlayerIndex()) +
                  "  waitType=SelectChain  reason=zero-options  response=-1");
    // Routes through the standard submitResponse path → recorder hook →
    // mpOnLocalResponse broadcasts ONE EngineResponse packet → peer feeds
    // via m_dm.respond with m_mpFeedingRemote=true so no echo.
    m_dm.respondChain(-1);
}
#endif

// ─── Startup health check ──────────────────────────────────────────────────
//
// Runs once at app start (called from Game::init via loadSettings). The
// summary text is preserved in m_healthSummary for display in the Assets
// popup and inclusion in the "Copy Full Diagnostics" payload. A toast is
// emitted only when warnings exist so normal launches stay quiet.
//
void UI::runStartupHealthCheck() {
    if (m_mpStartupHealthRan) return;
    m_mpStartupHealthRan = true;
    namespace fs = std::filesystem;
    std::ostringstream os;
    int warnings = 0;
    auto note = [&](bool ok, const char* tag, const std::string& msg) {
        os << (ok ? "[PASS] " : "[WARN] ") << tag << ": " << msg << "\n";
        if (!ok) ++warnings;
    };
    std::error_code ec;
    bool cdbOk = fs::is_regular_file("assets/cards.cdb", ec);
    note(cdbOk, "cards.cdb",
         cdbOk ? std::string("found at ") +
                 fs::absolute("assets/cards.cdb", ec).string()
               : "missing — duels will not start");
    bool scriptsOk = fs::is_directory("assets/scripts", ec);
    int scriptCount = 0;
    if (scriptsOk) {
        for (auto& e : fs::recursive_directory_iterator(
                 "assets/scripts", fs::directory_options::skip_permission_denied, ec))
            if (!ec && e.is_regular_file() && e.path().extension() == ".lua")
                ++scriptCount;
    }
    note(scriptsOk && scriptCount > 50, "scripts",
         scriptsOk ? std::to_string(scriptCount) + " .lua files"
                   : "assets/scripts folder missing");
    bool decksOk = fs::is_directory("assets/decks", ec);
    note(decksOk, "decks",
         decksOk ? std::string("found at ") +
                   fs::absolute("assets/decks", ec).string()
                 : "assets/decks folder missing");
    bool sfxOk = fs::is_directory("assets/sfx", ec);
    int sfxLoaded = gAudio().loadedCount();
    int sfxExpected = AudioManager::expectedSfxCount();
    note(sfxOk && sfxLoaded == sfxExpected, "sfx",
         sfxOk
           ? std::to_string(sfxLoaded) + " / " +
             std::to_string(sfxExpected) + " loaded"
           : "assets/sfx folder missing");
    bool backOk = fs::is_regular_file("assets/card_back.png", ec);
    note(backOk, "card_back",
         backOk ? "ok" : "missing (procedural fallback in use)");
    bool replayDirOk = true;
    if (!fs::is_directory("assets/replays", ec)) {
        fs::create_directories("assets/replays", ec);
        replayDirOk = fs::is_directory("assets/replays", ec);
    }
    note(replayDirOk, "replays",
         replayDirOk ? std::string("ready at ") +
                       fs::absolute("assets/replays", ec).string()
                     : "assets/replays could not be created");
    bool settingsOk = fs::is_directory(
        fs::path(edo::Settings::defaultPath()).parent_path(), ec);
    note(settingsOk, "settings dir",
         settingsOk ? std::string("ready at ") +
                      fs::absolute(
                          fs::path(edo::Settings::defaultPath()).parent_path(),
                          ec).string()
                    : "assets/config could not be created");
    m_healthSummary  = os.str();
    m_healthWarnings = warnings;
    m_dm.logEvent("[health] " +
                  std::to_string(warnings) + " warning" +
                  (warnings == 1 ? "" : "s"));
    if (warnings > 0) {
        pushToast(std::string("Startup health: ") +
                  std::to_string(warnings) +
                  " warning" + (warnings == 1 ? "" : "s") +
                  " — see Assets popup",
                  IM_COL32(255, 200, 90, 255), 3.0);
    }
}

std::string UI::buildFullDiagnostics() const {
    namespace fs = std::filesystem;
    std::ostringstream os;
    std::error_code ec;
    os << "=== EdoPro+ diagnostics ===\n";
    os << "Generated      : " << edo::Replay::nowTimestamp() << "\n";
    os << "Working dir    : " << fs::current_path(ec).string() << "\n";
    os << "Settings path  : " << edo::Settings::defaultPath() << "\n";
    os << "Replays dir    : " << edo::Replay::defaultDir() << "\n";
    os << "cards.cdb      : " << m_db.dbPath() << "\n";
    os << "Card DBs       : " << m_db.databaseCount() << "\n";
    os << "Card back      : " << m_rend.cardBackInfo() << "\n";
    os << "SFX loaded     : " << gAudio().loadedCount() << " / "
                              << AudioManager::expectedSfxCount() << "\n";
    int ydk = 0;
    if (fs::is_directory("assets/decks", ec)) {
        for (auto& e : fs::directory_iterator("assets/decks", ec))
            if (!ec && e.path().extension() == ".ydk") ++ydk;
    }
    os << "Decks (.ydk)   : " << ydk << "\n";
    int rpy = 0;
    if (fs::is_directory("assets/replays", ec)) {
        for (auto& e : fs::directory_iterator("assets/replays", ec))
            if (!ec && e.path().extension() == ".json") ++rpy;
    }
    os << "Replays (.json): " << rpy << "\n";
    const char* scr =
        m_screen == Screen::Lobby       ? "Lobby"       :
        m_screen == Screen::Duel        ? "Duel"        :
        m_screen == Screen::DeckBuilder ? "DeckBuilder" :
        m_screen == Screen::Replays     ? "Replays"    : "Multiplayer";
    os << "Current screen : " << scr << "\n";
    os << "Duel active    : " << (m_dm.isRunning() ? "yes" : "no") << "\n";
    os << "Replay mode    : " << (m_replayMode    ? "yes" : "no") << "\n";
    os << "MP mode        : " <<
        (m_net.isHost() ? "host" : m_net.isClient() ? "client" : "offline")
        << "\n";
    os << "MP state       : " << (int)m_net.state() << "\n";
    os << "Pending request: " << (int)currentSelection().type << "\n";
    os << "Testing mode   : " << (m_testingMode ? "yes" : "no") << "\n";
    os << "Timeline size  : " << m_timeline.size() << "\n";
    os << "Timeline index : " << m_timeline.applied()
       << (m_timeline.atHead() ? " (head)" : " (rewound)") << "\n";
    os << "Rebuild active : " << (m_testingRebuilding ? "yes" : "no") << "\n";
    os << "Last restore   : "
       << (m_testingLastRestore.empty() ? "(none)" : m_testingLastRestore)
       << "\n";
    os << "\n=== Health summary ===\n" << m_healthSummary;
    os << "\n=== Last 100 debug lines ===\n";
    const auto& log = m_dm.log();
    size_t start = log.size() > 100 ? log.size() - 100 : 0;
    for (size_t i = start; i < log.size(); ++i)
        os << log[i] << "\n";
    return os.str();
}

void UI::loadSettings() {
    bool existed = m_settings.load();   // may return false if file missing
    // Mirror persisted values into the existing UI toggle fields.
    m_debugLog        = m_settings.debugLog;
    m_logCollapsed    = m_settings.logCollapsed;
    m_logTab          = m_settings.selectedLogTab;
    if (m_logTab < 0 || m_logTab > 1) m_logTab = 0;
    m_showFieldNames  = m_settings.showFieldNames;
    m_largePreview    = m_settings.largePreview;
    m_showZoneLabels  = m_settings.showZoneLabels;
    m_showLegalGlow   = m_settings.showLegalGlow;
    // Push the on-demand card-art download preference into the renderer.
    m_rend.setImageDownload(m_settings.downloadCardImages);
    // Apply the saved card sleeve, if any (falls back silently to the default
    // card back when the file is missing).
    if (!m_settings.cardSleeve.empty())
        m_rend.setCardBack("assets/sleeves/" + m_settings.cardSleeve);
    loadCardTags();      // deck-consistency role tags (assets/card_tags.txt)
    loadMatchHistory();  // win/loss log (assets/match_history.txt)
    loadBanlists();      // format/banlist files (assets/lflists/*.lflist.conf)
    loadPresetDecks();   // bundled AI opponent decks (assets/decks/presets/)
    // Kick off the in-app update check (no-op unless a repo was baked in).
    m_update.setEnabled(m_settings.checkForUpdates);
    m_update.start(edo::kAppVersion, edo::kUpdateRepo);
    syncAnimConfig();
    // Push audio settings into the live device. Safe even when audio failed
    // to open — AudioManager::setMuted/setVolume are no-ops in that case.
    gAudio().setMuted (m_settings.sfxMuted);
    gAudio().setVolume(m_settings.sfxVolume);
    // Restore last-used decks for the lobby setup popup.
    if (!m_settings.lastDeckP1.empty())
        strncpy(m_deck0Path, m_settings.lastDeckP1.c_str(),
                sizeof(m_deck0Path) - 1);
    if (!m_settings.lastDeckP2.empty())
        strncpy(m_deck1Path, m_settings.lastDeckP2.c_str(),
                sizeof(m_deck1Path) - 1);
    // Surface a warning if the file existed but had malformed lines.
    if (existed && !m_settings.lastLoadWarning.empty()) {
        m_dm.logEvent("[settings] " + m_settings.lastLoadWarning);
    } else if (!existed) {
        // No file — first run. We'll write defaults on the first save().
        m_dm.logEvent("[settings] no config found, using defaults");
        m_showWelcome = true;   // greet the player + ask for a display name
    }
    m_dm.setDebugMessages(m_debugLog);

    // Run the startup health check once now that settings + audio are
    // initialised. Its summary is surfaced in the Assets / Debug popups.
    runStartupHealthCheck();
}

void UI::saveSettings() {
    // Reverse mirror — pull current UI toggle state into the settings POD
    // so anything the user changed via popups/in-line toggles is captured.
    m_settings.debugLog          = m_debugLog;
    m_settings.logCollapsed      = m_logCollapsed;
    m_settings.selectedLogTab    = m_logTab;
    m_settings.showFieldNames    = m_showFieldNames;
    m_settings.largePreview      = m_largePreview;
    m_settings.showZoneLabels    = m_showZoneLabels;
    m_settings.showLegalGlow     = m_showLegalGlow;
    m_settings.sfxMuted          = gAudio().muted();
    m_settings.sfxVolume         = gAudio().volume();
    m_settings.lastDeckP1        = m_deck0Path[0] ? m_deck0Path : "";
    m_settings.lastDeckP2        = m_deck1Path[0] ? m_deck1Path : "";
    if (!m_settings.save())
        m_dm.logEvent("[settings] WARN: could not write settings file");
}

void UI::syncAnimConfig() {
    edo::AnimConfig c;
    c.enabled      = m_settings.animationsEnabled;
    c.bigSummons   = m_settings.animBigSummons;
    c.phaseBanners = m_settings.animPhaseBanners;
    c.screenShake  = m_settings.animScreenShake;
    c.reduceMotion = m_settings.animReduceMotion;
    // Bias animation duration by the game-speed preset (Relaxed = longer holds
    // so animations are readable; Fast = snappier). 0 (instant) stays instant.
    const float kAnimMul[3] = { 0.78f, 1.0f, 1.35f };
    int gs = (m_settings.gameSpeed >= 0 && m_settings.gameSpeed <= 2)
                 ? m_settings.gameSpeed : 0;
    c.speed        = (m_settings.animSpeed > 0.f)
                         ? m_settings.animSpeed * kAnimMul[gs] : 0.f;
    c.phaseDelay   = m_settings.animPhaseDelay;
    m_anim.setConfig(c);
}

// ── Boss / big-monster classifier (Stage A) ──────────────────────────────
// A monster qualifies for the centre-screen entrance animation when it is
// high-impact: Level ≥ 7, Rank ≥ 7, Link rating ≥ 4, ATK ≥ 2500, or any
// Extra-Deck monster (Fusion / Synchro / Xyz / Link). Spells and Traps
// never qualify.
bool UI::isBossCard(const CardInfo& ci) const {
    if (!(ci.type & TYPE_MONSTER)) return false;
    if (isExtraDeckType(ci.type))  return true;
    if (ci.type & TYPE_LINK) {
        int rating = ci.def > 0 ? ci.def : (int)ci.level;
        if (rating >= 4) return true;
    } else if (ci.type & TYPE_XYZ) {
        if ((int)ci.level >= 7) return true;     // Rank
    } else {
        if ((int)ci.level >= 7) return true;     // Level
    }
    if (ci.atk >= 2500) return true;
    return false;
}

// Summon-type banner label for the boss entrance. We can't always know the
// exact summon method from the snapshot, so we infer from the card type:
// Extra-Deck monsters report their mechanic (FUSION / SYNCHRO / XYZ / LINK);
// everything else is NORMAL or SPECIAL based on the caller's `special` flag.
const char* UI::summonTypeLabel(const CardInfo& ci, bool special) const {
    if (ci.type & TYPE_FUSION)  return "FUSION SUMMON";
    if (ci.type & TYPE_SYNCHRO) return "SYNCHRO SUMMON";
    if (ci.type & TYPE_XYZ)     return "XYZ SUMMON";
    if (ci.type & TYPE_LINK)    return "LINK SUMMON";
    return special ? "SPECIAL SUMMON" : "NORMAL SUMMON";
}

// ── Shared card-art draw helper (orientation source of truth) ────────────
// Compact a prompt description to a single short line (compact prompts on).
std::string UI::compactPromptDesc(const std::string& full) const {
    if (full.empty()) return full;
    if (!m_settings.compactPrompts) return full;
    std::string s = full;
    for (char& c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    std::string out;
    out.reserve(s.size());
    bool sp = false;
    for (char c : s) {
        if (c == ' ') { if (!sp) out.push_back(' '); sp = true; }
        else { out.push_back(c); sp = false; }
    }
    const size_t kMax = 72;
    if (out.size() > kMax) out = out.substr(0, kMax - 1) + "...";
    return out;
}

// Aspect-preserving fit of a card inside slot [a,b], centred. `landscape`
// fits the rotated (defense) 614:421 footprint; otherwise the upright
// 421:614 card. The visible art NEVER stretches — the slot can be any size
// and the card keeps its true proportions inside it.
void UI::fitCardRect(ImVec2 a, ImVec2 b, bool landscape,
                     ImVec2* o0, ImVec2* o1) {
    float sw = b.x - a.x, sh = b.y - a.y;
    if (sw < 1.f) sw = 1.f; if (sh < 1.f) sh = 1.f;
    const float ar = landscape ? (614.f / 421.f) : (421.f / 614.f); // w/h
    float h = sh, w = h * ar;
    if (w > sw) { w = sw; h = w / ar; }
    float ox = a.x + (sw - w) * 0.5f;
    float oy = a.y + (sh - h) * 0.5f;
    *o0 = {ox, oy};
    *o1 = {ox + w, oy + h};
}

void UI::drawCardArt(ImDrawList* dl, uint32_t code, void* tex,
                     ImVec2 a, ImVec2 b, bool rotateDefenseCW,
                     bool dbgCheck) {
    if (!dl || !tex) return;
    // Aspect-fit the visible rect inside the slot so card art is never
    // stretched (the reported Pendulum-scale distortion). The caller passes
    // the full slot; the click/hit-test still uses that full slot elsewhere.
    ImVec2 f0, f1;
    fitCardRect(a, b, /*landscape*/ rotateDefenseCW, &f0, &f1);

    // One-shot [CARD RENDER CHECK] per Pendulum code so the user can confirm
    // (in Debug Log) that no path flips the art. UV0/UV1 are always the
    // upright (0,0)-(1,1) pair; defense applies a pure 90° rotation, not a
    // mirror — so flipped is always "no".
    if (dbgCheck && m_debugLog) {
        CardInfo ci = m_db.getCard(code);
        if (ci.type & TYPE_PENDULUM) {
            static std::unordered_map<uint32_t, bool> s_seen;
            if (!s_seen[code]) {
                s_seen[code] = true;
                m_dm.logEvent(std::string("[CARD RENDER CHECK] code=") +
                    std::to_string(code) + " name=" + ci.name +
                    " type=pendulum uv0=0,0 uv1=1,1 flipped=no" +
                    (rotateDefenseCW ? " (defense 90CW rotation)" : ""));
            }
        }
    }
    if (rotateDefenseCW) {
        // Screen winding TL,TR,BR,BL ← image bottom-left,top-left,top-right,
        // bottom-right = a pure 90° clockwise rotation (orientation-preserving;
        // det > 0). NOT a horizontal flip. Drawn into the fitted landscape rect.
        dl->AddImageQuad((ImTextureID)tex,
            {f0.x, f0.y}, {f1.x, f0.y}, {f1.x, f1.y}, {f0.x, f1.y},
            {0.f, 1.f}, {0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f});
    } else {
        // Upright: explicit UVs make the non-flipped contract obvious.
        dl->AddImage((ImTextureID)tex, f0, f1, {0.f, 0.f}, {1.f, 1.f});
    }
}

// ── Phase banner queue (Stage A bug-fix) ─────────────────────────────────
// Canonical phase order so we can fill in phases the engine skipped through.
// Battle sub-steps (0x08..0x80) all map to the single Battle slot.
static int phaseOrderIndex(uint16_t ph) {
    switch (ph) {
        case 0x01:  return 0;   // Draw
        case 0x02:  return 1;   // Standby
        case 0x04:  return 2;   // Main 1
        case 0x08: case 0x10: case 0x20: case 0x40: case 0x80:
                    return 3;   // Battle (any sub-step)
        case 0x100: return 4;   // Main 2
        case 0x200: return 5;   // End
        default:    return -1;
    }
}
static const uint16_t kPhaseOrder[6] =
    {0x01, 0x02, 0x04, 0x08, 0x100, 0x200};

// Detect a phase change on the observed (engine/snapshot) phase and enqueue
// the banner(s). When "show skipped phases" is on we fill in every phase
// between the last shown one and the new one — so an empty turn that the
// engine fast-forwards from Draw→Main still shows DRAW → STANDBY → MAIN 1.
void UI::observePhaseForBanners() {
    if (!m_settings.animationsEnabled || !m_settings.animPhaseBanners) return;
    const FieldState& pf = currentField();
    uint16_t newPhase = pf.phase;
    if (newPhase == m_animObservedPhase) return;          // no change
    int newIdx = phaseOrderIndex(newPhase);
    if (newIdx < 0) { m_animObservedPhase = newPhase; return; }
    // Board-break puzzle: don't queue phase banners for the opponent's empty
    // first turn — just track state so the player isn't held up. (The turn
    // banner for YOUR turn still fires when it flips back.)
    if (m_puzzleMode && (int)pf.turnPlayer != m_dm.humanSeat()) {
        m_animObservedPhase  = newPhase;
        m_animLastEnqueued   = newPhase;
        m_animPrevTurnPlayer = pf.turnPlayer;
        return;
    }

    bool turnChanged = (pf.turnPlayer != m_animPrevTurnPlayer);
    // Turn-start banner — a big "YOUR TURN" / "OPPONENT'S TURN" so whose turn it
    // is reads instantly. Shown before the phase banners (push their pacing out
    // so they don't overlap).
    if (turnChanged && m_animLastEnqueued != 0) {
        bool mine = ((int)pf.turnPlayer == m_net.localPlayerIndex());
        m_anim.emitPhaseBanner(mine ? "YOUR TURN" : "OPPONENT'S TURN",
                               ImGui::GetIO().DisplaySize,
                               mine ? IM_COL32(110, 220, 140, 255)
                                    : IM_COL32(255, 150, 90, 255));
        m_phaseQueueNextAt = ImGui::GetTime() + 0.75;
        if (gAudio().isLoaded("phase")) gAudio().play("phase");
    }
    if (m_debugLog)
        m_dm.logEvent(std::string("[PHASE OBSERVE] old=") +
                      phaseBannerText(m_animObservedPhase) +
                      " new=" + phaseBannerText(newPhase) +
                      " turnChanged=" + (turnChanged ? "yes" : "no") +
                      " queued=yes");

    // Queue ONLY the phase the engine is actually on now. Backfilling the
    // phases the engine fast-forwarded through (Draw/Standby) made the banner
    // lag the real game state — it could still read "Draw Phase" while the
    // opponent was already summoning in Main Phase. The engine's per-phase
    // pacing already holds each phase that has content long enough to read.
    m_phaseQueue.clear();                  // never let stale phases pile up
    m_phaseQueue.push_back(newPhase);

    if (m_debugLog)
        m_dm.logEvent(std::string("[PHASE BANNER QUEUE] phase=") +
                      phaseBannerText(newPhase) +
                      " queueSize=" + std::to_string(m_phaseQueue.size()));

    m_animLastEnqueued   = newPhase;
    m_animObservedPhase  = newPhase;
    m_animPrevTurnPlayer = pf.turnPlayer;
}

// Pop one queued phase banner per minimum-duration interval so a burst of
// skipped phases displays sequentially instead of all at once.
void UI::pumpPhaseBannerQueue() {
    if (m_phaseQueue.empty()) return;
    double now = ImGui::GetTime();
    if (now < m_phaseQueueNextAt) return;
    uint16_t ph = m_phaseQueue.front();
    m_phaseQueue.erase(m_phaseQueue.begin());
    const char* txt = phaseBannerText(ph);
    if (txt[0]) {
        bool battle = (ph & 0xFCu) != 0;
        ImU32 bcol = battle ? IM_COL32(255, 132, 80, 255)
                            : IM_COL32(232, 196, 110, 255);
        m_anim.emitPhaseBanner(txt, ImGui::GetIO().DisplaySize, bcol);
        if (gAudio().isLoaded("phase")) gAudio().play("phase");
        m_animPrevPhase = ph;     // keep HUD-adjacent state coherent
    }
    // Pace: at least the configured minimum, shortened under reduce-motion.
    double interval = (double)m_settings.animPhaseMinDuration;
    if (m_settings.animReduceMotion) interval *= 0.5;
    if (m_settings.animSpeed > 0.f)  interval /= (double)m_settings.animSpeed;
    if (interval < 0.08) interval = 0.08;
    m_phaseQueueNextAt = now + interval;
}

void UI::pushToast(const std::string& text, ImU32 color, double dur) {
    Toast t;
    t.text  = text;
    t.color = color;
    t.dur   = dur;
    t.at    = ImGui::GetTime();
    m_toasts.push_back(t);
    // Cap to a reasonable number so spam can't grow the buffer.
    if (m_toasts.size() > 16)
        m_toasts.erase(m_toasts.begin(),
                       m_toasts.begin() + (m_toasts.size() - 16));
}

void UI::openExternalUrl(const std::string& url) {
    if (url.empty()) return;
    edo::openUrl(url);
    pushToast("Opening download page in your browser…",
              IM_COL32(180, 220, 255, 255), 2.4);
}

void UI::pushGameLog(const std::string& text, ImU32 color) {
    GameLogLine g;
    g.text  = text;
    g.color = color ? color : IM_COL32(220, 232, 250, 255);
    g.at    = ImGui::GetTime();
    m_gameLog.push_back(g);
    // Soft cap so a long duel doesn't grow forever.
    if (m_gameLog.size() > 1024)
        m_gameLog.erase(m_gameLog.begin(),
                        m_gameLog.begin() + (m_gameLog.size() - 1024));
    // Mirror into the active replay's event timeline so the saved file has
    // the player-facing narrative. Recording is gated by the duel lifecycle
    // (beginReplayRecording / finalizeReplay) so this is a cheap no-op
    // outside a live duel.
    if (m_replayRecording) {
        edo::ReplayEvent e;
        e.t = ImGui::GetTime() - m_replayStartTime;
        e.text = text;
        m_replay.events.push_back(e);
    }
}

// Backwards-compatible alias kept so any new call site that thinks "I want
// to put this in the replay too" still reads correctly. The behaviour now
// lives entirely in pushGameLog().
void UI::pushGameAndReplay(const std::string& text, ImU32 color) {
    pushGameLog(text, color);
}

void UI::onResponseRecorded(const void* data, uint32_t len) {
    // Suppress while a Testing-Mode rebuild is replaying recorded responses
    // into a fresh engine — those are NOT new user input and must not be
    // appended to the replay (would duplicate / corrupt it).
    if (m_testingRebuilding) return;
    if (!m_replayRecording) return;
    edo::ReplayResponse r;
    r.t = ImGui::GetTime() - m_replayStartTime;
    r.data.assign((const uint8_t*)data,
                  (const uint8_t*)data + len);
    m_replay.responses.push_back(r);
}

void UI::beginReplayRecording(const std::string& d0Path,
                              const std::string& d1Path) {
    // Hard guard — never start a new recording while a replay is playing
    // back. The playback path passes through startDuel just like a live
    // duel; without this check we'd silently capture the replay as if it
    // were original gameplay.
    if (m_replayMode) return;
    // Reset and populate metadata for the duel that's about to start.
    m_replay = edo::Replay{};
    m_replay.timestamp = edo::Replay::nowTimestamp();
    m_replay.seed      = m_dm.duelSeed();
    // ruleFlags / lp / counts are pulled from the engine's known defaults.
    // The actual rule_flags constant lives in DuelManager (not currently
    // exposed) — we encode 0 here and let future versions surface it.
    m_replay.lp        = 8000;
    m_replay.handCount = 5;
    m_replay.drawCount = 1;
    auto fillDeck = [&](edo::ReplayDeck& d, const std::string& path) {
        d.path = path;
        if (path.empty()) return;
        d.name = path;
        auto sl = d.name.find_last_of("/\\");
        if (sl != std::string::npos) d.name = d.name.substr(sl + 1);
        if (d.name.size() > 4 && d.name.substr(d.name.size() - 4) == ".ydk")
            d.name = d.name.substr(0, d.name.size() - 4);
        Deck deck = loadYdk(path);
        d.main  = deck.main;
        d.extra = deck.extra;
        d.side  = deck.side;
    };
    fillDeck(m_replay.deck1, d0Path);
    fillDeck(m_replay.deck2, d1Path);
    m_replay.cardDb      = m_db.dbPath();
    m_replay.scriptCount = 0; // best-effort; UI can compute via std::filesystem
    m_replayStartTime    = ImGui::GetTime();
    m_replayRecording    = true;
    m_replaySavedOnce    = false;
    // Install the response recorder on the manager. It fans out to BOTH the
    // replay recorder AND the Testing-Mode timeline (each self-gates).
    m_dm.setResponseRecorder(
        [this](const void* d, uint32_t n){
            onResponseRecorded(d, n);
            recordTestingAction(d, n);
        });
}

// ─── Testing Mode timeline (offline deterministic rewind) ───────────────────
//
// See TestingTimeline.h for the architecture. captureTestingRoot stores the
// deterministic root at duel start; recordTestingAction records every response
// with a label; testingJumpTo rebuilds the engine from the root + replays the
// recorded responses up to the chosen point.

bool UI::testingRewindAvailable() const {
    // Offline only — never rewind during LAN / online / host-auth multiplayer
    // (one peer can't rewind independently) or during replay playback.
    return m_net.isOffline() && !m_replayMode && m_timeline.hasRoot();
}

void UI::captureTestingRoot(const std::string& team0Path,
                            const std::string& team1Path,
                            uint32_t lp, uint32_t handCount, uint32_t drawCount) {
    // Offline only. The seed is now canonical (startDuel consumed it).
    if (!m_net.isOffline()) { m_timeline.clear(); return; }
    edo::TestingRoot root;
    root.seed      = m_dm.duelSeed();
    root.deck0     = loadYdk(team0Path);
    root.deck1     = loadYdk(team1Path);
    root.lp        = lp;
    root.handCount = handCount;
    root.drawCount = drawCount;
    auto baseName = [](const std::string& p) {
        std::string s = p;
        auto sl = s.find_last_of("/\\");
        if (sl != std::string::npos) s = s.substr(sl + 1);
        if (s.size() > 4 && s.substr(s.size() - 4) == ".ydk")
            s = s.substr(0, s.size() - 4);
        return s.empty() ? std::string("Deck") : s;
    };
    root.deck0Name = baseName(team0Path);
    root.deck1Name = baseName(team1Path);
    m_timeline.beginDuel(root);
    m_testingLastRestore.clear();
    m_dm.logEvent("[TESTING START] seed=" + std::to_string(root.seed) +
                  " p1Deck=" + root.deck0Name + "(" +
                  std::to_string(root.deck0.main.size()) + ")" +
                  " p2Deck=" + root.deck1Name + "(" +
                  std::to_string(root.deck1.main.size()) + ")");
}

// Start an offline duel with a coin toss for who takes the first turn.
//
// ocgcore has no "first player" knob — team 0 ALWAYS moves first. So the toss
// is realised by deck order: the deck registered as team 0 goes first. The
// human always controls the P1 deck; when the toss says the human goes second
// we register the opponent's deck as team 0 and tell the rest of the app the
// human now sits in seat 1 (m_humanSeat / seat override). Replay + Testing Mode
// capture the REGISTERED order, so deterministic rebuilds reproduce the toss.
bool UI::startOfflineDuelWithCoinToss(const std::string& p1Path,
                                      const std::string& p2Path,
                                      int lp, int handCount, int drawCount) {
    bool humanFirst = true;
    if (m_settings.coinTossEnabled) {
        std::random_device rd;
        humanFirst = (rd() & 1u) == 0u;
    }
    // Team 0 = first turn. Human owns the P1 deck regardless of order.
    const std::string& t0 = humanFirst ? p1Path : p2Path;
    const std::string& t1 = humanFirst ? p2Path : p1Path;
    const int humanSeat   = humanFirst ? 0 : 1;

    Deck d0 = loadYdk(t0);
    Deck d1 = loadYdk(t1);
    m_puzzleMode = false;   // a normal practice duel is not a puzzle
    m_dm.setHumanSeat(humanSeat);
    m_net.setSeatOverride(humanSeat);
    if (!m_dm.startDuel(d0, d1, lp, handCount, drawCount)) {
        // Engine refused — restore neutral seat so nothing downstream is skewed.
        m_dm.setHumanSeat(0);
        m_net.clearSeatOverride();
        return false;
    }
    m_snap.clear();
    // Seed AI pacing up-front so the engine can't pump the opponent's whole
    // first turn instantly on frame 1 (before drawDuel's pacing block runs).
    m_dm.setAiComboBeat(1.3);
    // Replay + testing capture the registered (toss) order so playback and
    // rewind reproduce the exact same opening.
    beginReplayRecording(t0, t1);
    captureTestingRoot(t0, t1, lp, handCount, drawCount);

    // Coin-result banner + log. The toast is the quiet record; when enabled the
    // big phase-banner gives it presence at duel start.
    if (m_settings.coinTossEnabled) {
        const char* msg = humanFirst ? "Coin toss — you go first!"
                                      : "Coin toss — opponent goes first";
        ImU32 col = humanFirst ? IM_COL32(110, 220, 140, 255)
                               : IM_COL32(255, 200, 90, 255);
        pushToast(msg, col, 3.2);
        pushGameLog(msg, IM_COL32(232, 196, 110, 255));
        m_anim.emitPhaseBanner(humanFirst ? "COIN TOSS — YOU GO FIRST"
                                          : "COIN TOSS — OPPONENT FIRST",
                               ImGui::GetIO().DisplaySize, col);
        // Hold the phase-banner queue so the first Draw Phase banner doesn't
        // pop on top of (and instantly replace) the coin-toss banner.
        m_phaseQueueNextAt = ImGui::GetTime() + 1.9;
    }
    m_dm.logEvent(std::string("[COIN TOSS] first=") +
                  (humanFirst ? "P1(you)" : "P2(opp)") +
                  " humanSeat=" + std::to_string(humanSeat) +
                  " team0Deck=" + t0);
    return true;
}

// #14 — start the next game of a best-of-3 match from the stored decks. Each
// game gets its own coin toss; side-deck edits between games are written to a
// temp .ydk that m_matchPlayerPath points at.
void UI::startMatchGame() {
    m_matchGameScored = false;
    m_dm.setNoShuffle(m_setupNoShuffle);
    m_dm.setPassiveAI(m_setupPassiveAI);
    if (startOfflineDuelWithCoinToss(m_matchPlayerPath, m_matchOppPath,
                                     (uint32_t)m_setupLP,
                                     (uint32_t)m_setupHand, 1)) {
        m_screen = Screen::Duel;
        gAudio().play("duel_start");
        pushToast("Match — Game " + std::to_string(m_matchGameNo) +
                  "  (You " + std::to_string(m_matchWins[0]) + " – " +
                  std::to_string(m_matchWins[1]) + " Opp)",
                  IM_COL32(200, 180, 255, 255), 2.8);
    } else {
        m_matchActive = false;
        gAudio().play("error");
    }
}

// Build a short label for the response about to be recorded, from the live
// selection it answers. Called BEFORE the engine consumes the response (the
// recorder fires before OCG_DuelSetResponse), so m_dm.selection() is valid.
std::string UI::testingLabelForResponse() const {
    const FieldState&       f = m_dm.field();
    const SelectionRequest& s = m_dm.selection();
    char head[48];
    snprintf(head, sizeof(head), "T%d %s — ", f.turnCount, [&]{
        switch (f.phase) {
            case 0x01: return "DP"; case 0x02: return "SP"; case 0x04: return "M1";
            case 0x08: case 0x10: case 0x20: case 0x40: case 0x80: return "BP";
            case 0x100: return "M2"; case 0x200: return "EP"; default: return "--";
        }
    }());
    std::string body;
    switch (s.type) {
        case WaitType::SelectIdleCmd:   body = "Main Phase action"; break;
        case WaitType::SelectBattleCmd: body = "Battle action";     break;
        case WaitType::SelectYesNo:     body = "Yes / No";          break;
        case WaitType::SelectEffectYn:  body = "Activate effect?";  break;
        case WaitType::SelectOption:    body = "Choose effect";     break;
        case WaitType::SelectCard:      body = "Select card";       break;
        case WaitType::SelectChain:     body = "Chain response";    break;
        case WaitType::SelectPlace:     body = "Place card";        break;
        case WaitType::SelectPosition:  body = "Battle position";   break;
        case WaitType::SelectTribute:   body = "Select tribute";    break;
        case WaitType::SelectSum:       body = "Sum selection";     break;
        case WaitType::SelectUnselect:  body = "Material select";   break;
        default:                        body = "Response";          break;
    }
    return std::string(head) + "P" + std::to_string((int)s.player + 1) +
           " " + body;
}

void UI::recordTestingAction(const void* data, uint32_t len) {
    // Record every live offline response so Undo (and Testing-Mode rewind)
    // always works in practice duels — never during a rebuild (those are
    // replays of already-recorded actions) or in MP/replay.
    if (m_testingRebuilding) return;
    if (!m_net.isOffline() || m_replayMode) return;
    if (!m_timeline.hasRoot()) return;

    const FieldState&       f = m_dm.field();
    const SelectionRequest& s = m_dm.selection();
    edo::TestingAction a;
    a.label    = testingLabelForResponse();
    a.turn     = f.turnCount;
    a.phase    = f.phase;
    a.player   = (int)s.player;
    a.waitType = (int)s.type;
    a.responseBytes.assign((const uint8_t*)data, (const uint8_t*)data + len);

    int discarded = 0;
    int idx = m_timeline.record(std::move(a), &discarded);
    if (discarded > 0)
        m_dm.logEvent("[TESTING BRANCH] fromIndex=" + std::to_string(idx) +
                      " discardedFuture=" + std::to_string(discarded));
    // The replay stream must mirror the branch so it doesn't keep stale
    // future responses (they were already truncated on the rewind that
    // created this branch, but guard here too).
    if (m_replayRecording && (int)m_replay.responses.size() > idx)
        m_replay.responses.resize((size_t)idx + 1);

    m_dm.logEvent("[TESTING RECORD] index=" + std::to_string(idx) +
                  " turn=" + std::to_string(a.turn) +
                  " phase=" + std::to_string(a.phase) +
                  " player=" + std::to_string(a.player) +
                  " waitType=" + std::to_string(a.waitType) +
                  " label=" + a.label +
                  " bytes=" + std::to_string(len));
}

void UI::testingJumpTo(int applyCount, const char* reason) {
    if (!testingRewindAvailable()) {
        pushToast("Testing rewind is available in offline testing mode only.",
                  IM_COL32(232, 182, 72, 255), 2.6);
        return;
    }
    const edo::TestingRoot& root = m_timeline.root();
    applyCount = std::max(0, std::min(applyCount, m_timeline.size()));

    // ── Rebuild: tear down + fresh seeded engine + replay responses ────────
    // NOTE: do NOT finalizeReplay() here — that would auto-save a replay on
    // every rewind and stop recording. We keep m_replayRecording on and just
    // truncate m_replay.responses to the applied prefix (below) so the replay
    // tracks the new line of play. The recorder is suppressed during the
    // rebuild itself via m_testingRebuilding.
    m_testingRebuilding = true;                  // suppress recorder/SFX/anim
    m_dm.setPhaseDelay(0.0);                      // rebuild must run instantly
    m_dm.setAiComboBeat(0.0);                     // ...including the AI beat
    bool prevLocal = m_dm.localMode();
    if (m_dm.isRunning()) m_dm.endDuel();
    m_dm.setForcedSeed(root.seed);
    m_dm.setLocalMode(false);                    // feed recorded P2 bytes too

    bool ok = m_dm.startDuel(root.deck0, root.deck1,
                             root.lp, root.handCount, root.drawCount);
    int replayed = 0;
    if (ok) {
        auto advance = [&]() {
            int guard = 0;
            while (m_dm.isRunning() && !m_dm.isDone() && guard++ < 20000) {
                const SelectionRequest& s = m_dm.selection();
                if (DuelManager::isRealSelect(s.type) || m_dm.isBlocked()) break;
                if (!m_dm.process()) break;
            }
        };
        advance();                               // reach the first prompt
        for (int i = 0; i < applyCount; ++i) {
            if (!m_dm.isRunning() || m_dm.isDone()) break;
            const auto& bytes = m_timeline.actions()[(size_t)i].responseBytes;
            m_dm.respond(bytes.data(), (uint32_t)bytes.size());
            ++replayed;
            advance();
        }
    }
    m_dm.setLocalMode(prevLocal);
    m_testingRebuilding = false;

    // Sync bookkeeping + re-seed the presentation observers so the next frame
    // doesn't fire a burst of SFX/banners for the rebuilt state.
    m_timeline.setApplied(applyCount);
    if (m_replayRecording && (int)m_replay.responses.size() > applyCount)
        m_replay.responses.resize((size_t)applyCount);
    m_anim.clear();
    m_sfxObsInited       = false;
    m_bossObsInited      = false;
    m_zoneRectsReady     = false;
    m_animObservedPhase  = 0xFFFF;
    m_animLastEnqueued   = 0;
    m_phaseQueue.clear();
    m_testingJustRestored = true;   // skip the re-seed "duel started" sting
    clearSelection();
    m_viewerLoc = 0;
    m_viewerExtraCache.clear();

    std::string label = (applyCount == 0)
        ? std::string("Duel start")
        : m_timeline.actions()[(size_t)applyCount - 1].label;
    m_testingLastRestore = ok ? ("OK · " + label) : "FAILED";
    m_dm.logEvent(std::string("[TESTING RESTORE] target=") +
                  std::to_string(applyCount) +
                  " method=rebuild responsesReplayed=" +
                  std::to_string(replayed) +
                  " success=" + (ok ? "yes" : "no") +
                  " reason=" + (reason ? reason : "jump"));
    if (!ok)
        m_dm.logEvent("[TESTING ERROR] reason=startDuel failed during rebuild");
    pushToast(ok ? ("Restored to: " + label) : "Testing restore failed",
              ok ? IM_COL32(180, 220, 255, 255) : IM_COL32(232, 110, 100, 255),
              2.4);
}

void UI::testingStepBack() {
    if (!testingRewindAvailable()) return;
    int target = m_timeline.applied() - 1;
    if (target < 0) target = 0;
    testingJumpTo(target, "step-back");
}

void UI::testingStepForward() {
    if (!testingRewindAvailable()) return;
    int target = m_timeline.applied() + 1;
    if (target > m_timeline.size()) target = m_timeline.size();
    testingJumpTo(target, "step-forward");
}

// Undo: rewind to just before the human's most recent decision (skipping the
// AI's and inline auto-responses in between), so the player simply re-makes
// their last move. Offline practice only.
void UI::testingUndoHuman() {
    if (!testingRewindAvailable()) {
        pushToast("Undo is available in offline practice duels.",
                  IM_COL32(232, 182, 72, 255), 2.4);
        return;
    }
    const int applied = m_timeline.applied();
    int target = -1;
    for (int i = applied - 1; i >= 0; --i)
        if (m_timeline.actions()[(size_t)i].player == m_dm.humanSeat()) {
            target = i; break;
        }
    if (target < 0) {
        pushToast("Nothing to undo yet.", IM_COL32(180, 200, 230, 255), 1.8);
        return;
    }
    testingJumpTo(target, "undo");
    pushToast("Undid your last move", IM_COL32(140, 215, 255, 255), 1.8);
}

// ─── Replay playback ────────────────────────────────────────────────────────
//
// Replay playback is purely UI-side. The DuelManager runs a normal duel
// with the recorded seed; the UI feeds the recorded response bytes back
// in order whenever the engine asks for input. Live click handlers exit
// early while m_replayMode is true so the user can't perturb the engine.
//
void UI::startReplayPlayback(const std::string& path) {
    // Load the file fresh — even if it's the viewer's selection, we want
    // a stable copy independent of further browser navigation.
    edo::Replay r;
    if (!r.load(path)) {
        pushToast("Replay load failed — file is missing or corrupt",
                  IM_COL32(232, 110, 100, 255), 3.0);
        return;
    }
    if (r.deck1.main.empty() || r.deck2.main.empty()) {
        pushToast("Replay has no deck data — cannot play back",
                  IM_COL32(232, 110, 100, 255), 3.0);
        return;
    }
    // Pre-flight: warn (don't block) if the CDB path has shifted since
    // recording. Replays from an older runtime can desync on script edits.
    if (!r.cardDb.empty() && r.cardDb != m_db.dbPath()) {
        pushToast("Replay was recorded against a different cards.cdb — "
                  "desync may occur",
                  IM_COL32(255, 200,  90, 255), 3.5);
    }
    // End any current duel + recording cleanly before swapping in the
    // replay's decks. finalizeReplay no-ops when nothing is recording.
    finalizeReplay("entered replay mode");
    if (m_dm.isRunning()) m_dm.endDuel();

    // Build engine-ready decks from the recorded card lists. We bypass
    // the .ydk file path so the duel uses the EXACT cards in the replay.
    Deck d0, d1;
    d0.name  = r.deck1.name;  d0.main = r.deck1.main;
    d0.extra = r.deck1.extra; d0.side = r.deck1.side;
    d1.name  = r.deck2.name;  d1.main = r.deck2.main;
    d1.extra = r.deck2.extra; d1.side = r.deck2.side;

    // Force the engine seed so the deck shuffle + every randomised choice
    // matches the original duel byte-for-byte. setForcedSeed is one-shot.
    m_dm.setForcedSeed(r.seed);
    // Replay is recorded in REGISTERED (toss) order: deck1 = team 0 = the
    // player who went first, shown at the bottom. Clear any leftover offline
    // coin-toss seat override so the perspective is neutral seat 0.
    m_dm.setHumanSeat(0);
    m_net.clearSeatOverride();
    // During replay, BOTH players' prompts must come through the UI/feeder
    // so the recorded byte stream lands in order. localMode would route P2
    // prompts through the auto-AI, skipping recorded P2 responses and
    // causing immediate desync. We force it off here AND snapshot the
    // previous setting for stopReplayPlayback to restore.
    m_replayPrevLocal = m_dm.localMode();
    m_dm.setLocalMode(false);
    if (!m_dm.startDuel(d0, d1, r.lp ? r.lp : 8000,
                        r.handCount ? r.handCount : 5,
                        r.drawCount ? r.drawCount : 1)) {
        pushToast("Replay: startDuel failed",
                  IM_COL32(232, 110, 100, 255), 3.0);
        m_dm.setLocalMode(m_replayPrevLocal);
        return;
    }
    // Now enter replay mode — AFTER startDuel so the live "duel started"
    // observer fires once before we lock input.
    m_replayMode       = true;
    m_replayPlaying    = true;
    m_replayIdx        = 0;
    m_replaySpeed      = 1.0f;
    m_replayNextAt     = ImGui::GetTime();
    m_replayDesyncMsg.clear();
    m_replayActive     = std::move(r);
    m_replayActivePath = path;
    // Auto-recording must NOT activate during playback. The recorder hook
    // is still cleared by finalizeReplay above; m_replayRecording is false.
    m_replayRecording  = false;
    m_replaySavedOnce  = true;   // prevents finalize from auto-saving
    m_screen           = Screen::Duel;
    pushToast(std::string("Replay started: ") + r.deck1.name +
              " vs " + r.deck2.name,
              IM_COL32(180, 220, 255, 255), 2.4);
    m_dm.logEvent("[REPLAY] mode entered  file=" + path +
                  "  seed=" + std::to_string(r.seed) +
                  "  responses=" +
                  std::to_string(m_replayActive.responses.size()));
}

void UI::stopReplayPlayback() {
    if (m_dm.isRunning()) m_dm.endDuel();
    m_replayMode       = false;
    m_replayPlaying    = false;
    m_replayIdx        = 0;
    m_replayDesyncMsg.clear();
    m_replayActive     = edo::Replay{};
    m_replayActivePath.clear();
    m_anim.clear();
    m_zoneRectsReady   = false;
    m_sfxObsInited     = false;
    m_endGameSfxFired  = false;
    // Restore live-duel localMode so the user's next live game keeps P2
    // auto-played as they'd expect.
    m_dm.setLocalMode(m_replayPrevLocal);
}

void UI::feedReplayTick() {
    if (!m_replayMode || !m_dm.isRunning()) return;
    if (!m_replayDesyncMsg.empty()) return;   // frozen on desync

    const SelectionRequest& sel = currentSelection();
    // Two paths the engine signals "I need a response":
    //   1. A recognised selection type (sel.type passes isRealSelect).
    //   2. m_blocked — the engine parked on an unhandled prompt that the
    //      live UI doesn't implement. In replay mode we trust the recorded
    //      bytes are valid for THIS exact engine state and feed them
    //      anyway. The live unhandled-pause path stays in place for live
    //      duels; this just lets replay bypass it.
    bool needsResponse = DuelManager::isRealSelect(sel.type) ||
                         m_dm.isBlocked();
    if (!needsResponse) return;
    int total = (int)m_replayActive.responses.size();

    if (m_debugLog) {
        m_dm.logEvent("[REPLAY WAIT] pendingType=" +
                      std::to_string((int)sel.type) +
                      (m_dm.isBlocked() ? " (blocked)" : "") +
                      "  responseIndex=" + std::to_string(m_replayIdx) +
                      "/" + std::to_string(total));
    }

    // Step-pulse: fed by the Step button — one response then auto-clear.
    bool feedNow = m_replayStepPulse;
    if (m_replayPlaying) {
        if (ImGui::GetTime() >= m_replayNextAt) feedNow = true;
    }
    if (!feedNow) return;

    if (m_replayIdx >= total) {
        // Engine wants input but we have nothing left to feed.
        m_replayDesyncMsg =
            "Engine awaits a response but the replay has none left (idx=" +
            std::to_string(m_replayIdx) + " / " + std::to_string(total) + ").";
        m_replayPlaying  = false;
        m_replayStepPulse = false;
        m_dm.logEvent("[REPLAY DESYNC] file=" + m_replayActivePath +
                      "  seed=" + std::to_string(m_replayActive.seed) +
                      "  index=" + std::to_string(m_replayIdx) +
                      "  pendingType=" + std::to_string((int)sel.type));
        m_dm.logEvent("[REPLAY PAUSE] reason=no-response-left");
        return;
    }

    const edo::ReplayResponse& r = m_replayActive.responses[m_replayIdx];
    if (r.data.empty()) {
        m_replayDesyncMsg = "Empty recorded response at index " +
                            std::to_string(m_replayIdx) + ".";
        m_replayPlaying  = false;
        m_replayStepPulse = false;
        m_dm.logEvent("[REPLAY PAUSE] reason=empty-recorded-response");
        return;
    }
    if (m_debugLog) {
        m_dm.logEvent("[REPLAY FEED] index=" + std::to_string(m_replayIdx) +
                      "  bytes=" + std::to_string(r.data.size()) +
                      "  pendingType=" +
                      std::to_string((int)sel.type));
    }
    m_dm.respond(r.data.data(), (uint32_t)r.data.size());
    ++m_replayIdx;
    m_replayStepPulse = false;
    // Inter-response delay scaled by speed. 0.40s at 1x is comfortable.
    double base = 0.40 / (m_replaySpeed > 0.f ? m_replaySpeed : 1.f);
    m_replayNextAt = ImGui::GetTime() + base;
}

void UI::finalizeReplay(const std::string& reason) {
    if (!m_replayRecording) return;
    m_replayRecording = false;
    m_dm.setResponseRecorder(nullptr);
    // Capture end-state metadata.
    {
        auto& dm = m_dm;              // replay records the local engine
        m_replay.winner = dm.isDone() ? dm.winner() : -2;
        m_replay.turns  = dm.field().turnCount;
        m_replay.durationSec = ImGui::GetTime() - m_replayStartTime;
        m_replay.finalLP[0]  = dm.field().lp[0];
        m_replay.finalLP[1]  = dm.field().lp[1];
    }
    // Also stamp a final reason line into the event timeline.
    edo::ReplayEvent e;
    e.t = m_replay.durationSec;
    e.text = std::string("Replay end: ") + reason;
    m_replay.events.push_back(e);

    if (!m_settings.autoSaveReplays || m_replaySavedOnce) return;
    // Auto-save path.
    std::string filename = m_replay.suggestedFilename();
    std::string path = edo::Replay::defaultDir() + "/" + filename;
    if (m_replay.save(path)) {
        m_replaySavedOnce = true;
        pushToast(std::string("Replay saved: ") + filename,
                  IM_COL32(110, 220, 140, 255), 2.6);
        m_dm.logEvent("[replay] saved: " + path);
    } else {
        pushToast("Replay save failed", IM_COL32(232, 110, 100, 255), 2.6);
        m_dm.logEvent("[replay] WARN: could not save " + path);
    }
}

// ─── Top-level draw ───────────────────────────────────────────────────────────
bool UI::draw(int winW, int winH) {
    // Apply the EdoPro+ global theme once — skins every raw ImGui widget
    // (combos, inputs, checkboxes, sliders, popups, scrollbars, tooltips)
    // so the whole app shares the custom chrome's visual language.
    static bool s_themeApplied = false;
    if (!s_themeApplied) { UIStyle::ApplyTheme(); s_themeApplied = true; }

    // Drain any network messages BEFORE rendering this frame's screen.
    // EngineResponse arrivals may unblock the engine; handling them here
    // means drawDuel sees the freshest selection state.
    pumpMultiplayer();

    // Background music: loop it on the menus, silence it during a live duel so
    // the duel SFX have the stage. Cheap idempotent toggle by screen.
    {
        bool wantMusic = (m_screen != Screen::Duel);
        if (wantMusic) {
            if (gAudio().musicLoaded() && !gAudio().musicPlaying())
                gAudio().playMusic();
        } else if (gAudio().musicPlaying()) {
            gAudio().stopMusic();
        }
    }

    switch (m_screen) {
        case Screen::Lobby:       drawLobby(winW, winH);       break;
        case Screen::Duel:        drawDuel(winW, winH);         break;
        case Screen::DeckBuilder: drawDeckBuilder(winW, winH);  break;
        case Screen::Replays:     drawReplays(winW, winH);      break;
        case Screen::Multiplayer: drawMultiplayer(winW, winH);  break;
    }
    // F11 toggles fullscreen on any screen (Game applies it to the window).
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) &&
        !ImGui::GetIO().WantTextInput)
        m_fullscreenToggleReq = true;
    // Duel keyboard shortcuts + help overlay (drawn after the screen so the
    // overlay sits on top; hotkeys are UI-only — see handleDuelHotkeys).
    if (m_screen == Screen::Duel) {
        handleDuelHotkeys();
        drawChainResponsePopup(winW, winH);
        drawCardZoom(winW, winH);
        drawHelpOverlay(winW, winH);
        if (m_puzzleMode) drawPuzzleOverlay(winW, winH);
        drawExcavateReveal(winW, winH);
        drawCardContextMenu();
        drawPauseMenu(winW, winH);
    }
    // Toasts rendered LAST so they sit above every screen. Uses the
    // foreground draw list so they're not clipped by any active window.
    if (!m_toasts.empty()) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const double now = ImGui::GetTime();
        const float  W = 320.f;
        const float  H = 38.f;
        float yCursor = 18.f;
        for (size_t i = 0; i < m_toasts.size(); ) {
            Toast& t = m_toasts[i];
            double age = now - t.at;
            if (age >= t.dur) { m_toasts.erase(m_toasts.begin() + i); continue; }
            float alpha = 1.f;
            if (age < 0.18) alpha = (float)(age / 0.18);                // fade-in
            else if (age > t.dur - 0.45) alpha = (float)((t.dur - age) / 0.45);
            if (alpha < 0.f) alpha = 0.f; if (alpha > 1.f) alpha = 1.f;
            float x0 = (float)winW - W - 18.f;
            ImVec2 a{x0, yCursor};
            ImVec2 b{x0 + W, yCursor + H};
            unsigned aA = (unsigned)(220 * alpha);
            dl->AddRectFilled(a, b,
                IM_COL32(14, 18, 28, (unsigned)(225 * alpha)), 6.f);
            ImU32 stripe = (t.color & 0x00FFFFFF) | (aA << 24);
            dl->AddRectFilled(a, {a.x + 4.f, b.y}, stripe, 3.f);
            dl->AddRect(a, b,
                IM_COL32(70, 90, 130, (unsigned)(200 * alpha)), 6.f, 0, 1.f);
            ImU32 txt = IM_COL32(232, 240, 252, (unsigned)(255 * alpha));
            dl->AddText({a.x + 14.f, a.y + 10.f}, txt, t.text.c_str());
            yCursor += H + 6.f;
            ++i;
        }
    }
    return true;
}

// ─── Lobby (Master Duel-inspired LAYOUT, fully original artwork) ─────────────
// The previous version was a centred card with three buttons — that's the
// thing the user described as "still a centred ImGui panel". This rewrite
// uses the screenshot's STRUCTURE only (left vertical nav, top-left profile,
// top-right icons, central hero, lower-left news card) and paints every
// pixel of art procedurally via ImDrawList — abstract geometry + particles,
// no copied assets, no reference to dragons / mascot art / brand glyphs.
void UI::drawLobby(int w, int h) {
    m_viewerLoc = 0;
    const auto& C = UIStyle::C();
    const float W = (float)w, H = (float)h;

    // ── Fullscreen procedural background (drawn under everything) ───────────
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    UIStyle::DrawAppBackdrop(bg, {0.f, 0.f}, {W, H});

    // Subtle diagonal "duel-field perspective" lines on the right half.
    // Original abstract geometry — no derivative imagery.
    {
        ImU32 line = IM_COL32(150, 70, 76, 24);
        float ox = W * 0.55f, oy = H * 0.55f;
        for (int i = -8; i <= 8; ++i) {
            float k = (float)i * 0.12f;
            bg->AddLine({ox - 800.f * k, oy + 600.f * (1 - std::abs(k))},
                        {W, oy - 200.f + 60.f * i}, line, 1.f);
        }
    }
    // Procedural "starfield" particles (deterministic — same seed every frame
    // so they don't dance). Tiny dots scattered across the canvas.
    {
        unsigned s = 0x12345u;
        auto frand = [&](){ s = s * 1664525u + 1013904223u; return (s >> 8) * (1.f / 16777216.f); };
        for (int i = 0; i < 220; ++i) {
            float x = frand() * W;
            float y = frand() * H;
            float r = 0.6f + frand() * 1.6f;
            ImU32 c = IM_COL32(210, 150, 156, (int)(24 + frand() * 90.f));
            bg->AddCircleFilled({x, y}, r, c, 6);
        }
    }
    // Central hero composition (abstract glowing emblem at ~60% width).
    {
        float cx = W * 0.62f, cy = H * 0.52f;
        // Concentric energy rings.
        for (int i = 0; i < 5; ++i) {
            float r = 80.f + i * 38.f;
            int alpha = 90 - i * 14;
            bg->AddCircle({cx, cy}, r,
                          IM_COL32(200, 70, 76, alpha), 64, 1.4f);
        }
        // Hexagonal lattice — six rotated diamonds around a centre.
        for (int i = 0; i < 6; ++i) {
            float a = (float)i * 6.2831853f / 6.f - 1.5707963f;
            float ax = cx + std::cos(a) * 170.f;
            float ay = cy + std::sin(a) * 170.f;
            float a2 = a + 6.2831853f / 6.f;
            float bx = cx + std::cos(a2) * 170.f;
            float by = cy + std::sin(a2) * 170.f;
            bg->AddLine({ax, ay}, {bx, by},
                        IM_COL32(205, 80, 86, 100), 1.3f);
        }
        // Centre diamond (suggests a card silhouette in plan view).
        {
            float s = 90.f;
            ImVec2 dp[4] = { {cx, cy - s}, {cx + s, cy},
                             {cx, cy + s}, {cx - s, cy} };
            bg->AddPolyline(dp, 4, IM_COL32(214, 64, 70, 190),
                            ImDrawFlags_Closed, 1.6f);
            // Inner small diamond.
            float s2 = s * 0.55f;
            ImVec2 ip[4] = { {cx, cy - s2}, {cx + s2, cy},
                             {cx, cy + s2}, {cx - s2, cy} };
            bg->AddPolyline(ip, 4, IM_COL32(255, 120, 120, 130),
                            ImDrawFlags_Closed, 1.f);
        }
        // Soft red core glow.
        for (int i = 0; i < 7; ++i)
            bg->AddCircleFilled({cx, cy}, 28.f - i * 3.f,
                                IM_COL32(220, 56, 62, 24), 32);
        // Vertical light shaft behind the emblem.
        bg->AddRectFilledMultiColor({cx - 6.f, cy - 260.f},
                                    {cx + 6.f, cy + 260.f},
                                    IM_COL32(220, 60, 66,   0),
                                    IM_COL32(220, 60, 66,   0),
                                    IM_COL32(220, 60, 66, 110),
                                    IM_COL32(220, 60, 66, 110));
    }

    // ── Top-left profile / status HUD (glass, no boxy panel) ────────────────
    // Slim left gold accent bar + transparent glass tint + diamond avatar.
    // No filled background rectangle — the strip blends into the backdrop.
    {
        ImVec2 a = {26.f, 24.f};
        const float CW = 298.f, CH = 90.f;
        ImVec2 b = {a.x + CW, a.y + CH};
        // Premium glass card: soft gold halo, glass surface, top sheen — sits
        // on the backdrop as one material rather than a flat tinted box.
        UIStyle::DrawGlow(bg, a, b, (C.accent & 0x00FFFFFF) | 0x14000000,
                          UIStyle::M().radL, 2);
        UIStyle::DrawGlassPanel(bg, a, b, UIStyle::M().radL,
                                IM_COL32(28, 15, 18, 230));
        // Slim left accent bar reads as premium without a loud full border.
        bg->AddRectFilled({a.x + 2.f, a.y + 12.f}, {a.x + 5.f, b.y - 12.f},
                          (C.accent & 0x00FFFFFF) | 0xCC000000, 2.f);

        // Emblem — concentric gold diamond (the logo motif; the texture icon
        // ships as the app/window icon).
        ImVec2 cd = {a.x + 42.f, a.y + CH * 0.5f};
        auto diamond = [&](float r, ImU32 col, bool fill, float th) {
            ImVec2 p[4] = {{cd.x, cd.y - r}, {cd.x + r, cd.y},
                           {cd.x, cd.y + r}, {cd.x - r, cd.y}};
            if (fill) bg->AddConvexPolyFilled(p, 4, col);
            else      bg->AddPolyline(p, 4, col, ImDrawFlags_Closed, th);
        };
        diamond(25.f, IM_COL32(58, 26, 30, 230), true, 0.f);
        diamond(25.f, (C.accent & 0x00FFFFFF) | 0x99000000, false, 1.5f);
        diamond(13.f, IM_COL32(228, 74, 80, 235), true, 0.f);
        diamond(6.f,  IM_COL32(30, 16, 20, 255), true, 0.f);

        // Title in the header face, with a gold version pill + BETA tag on the
        // same baseline.
        const float tx = a.x + 80.f;
        UIStyle::PushFont(UIStyle::fHeader);
        bg->AddText({tx, a.y + 13.f}, C.textHi, edo::kAppName);
        ImVec2 nameSz = ImGui::CalcTextSize(edo::kAppName);
        UIStyle::PopFont();
        UIStyle::PushFont(UIStyle::fSmall);
        char ver[24]; snprintf(ver, sizeof(ver), "v%s", edo::kAppVersion);
        ImVec2 vs = ImGui::CalcTextSize(ver);
        float py = a.y + 17.f;
        ImVec2 pA{tx + nameSz.x + 10.f, py}, pB{pA.x + vs.x + 14.f, py + 17.f};
        bg->AddRectFilled(pA, pB, (C.accent & 0x00FFFFFF) | 0x33000000, 8.f);
        bg->AddRect(pA, pB, (C.accent & 0x00FFFFFF) | 0x99000000, 8.f, 0, 1.f);
        bg->AddText({pA.x + 7.f, pA.y + 2.f}, C.accentText, ver);
        const char* beta = "BETA";
        ImVec2 bs = ImGui::CalcTextSize(beta);
        ImVec2 betaA{pB.x + 6.f, py}, betaB{betaA.x + bs.x + 14.f, py + 17.f};
        bg->AddRectFilled(betaA, betaB, IM_COL32(70, 38, 44, 120), 8.f);
        bg->AddRect(betaA, betaB, IM_COL32(150, 78, 84, 190), 8.f, 0, 1.f);
        bg->AddText({betaA.x + 7.f, betaA.y + 2.f},
                    IM_COL32(228, 200, 204, 255), beta);
        UIStyle::PopFont();

        // Sub-lines.
        bg->AddText({tx, a.y + 44.f}, C.textLo, "Local Profile");
        UIStyle::PushFont(UIStyle::fSmall);
        bg->AddText({tx, a.y + 66.f}, C.textMuted,
                    "Modern Yu-Gi-Oh duel simulator");
        UIStyle::PopFont();
    }

    // ── Top-right action icons (Settings / Audio / Debug / Exit) ────────────
    // Hosted in a small transparent window so click handlers work; the panel
    // decoration paints via that window's own draw list (no foreground-list
    // z-order ghosting like the previous version).
    {
        const float ICONS_W = 360.f;
        ImGui::SetNextWindowPos({W - ICONS_W - 24.f, 22.f});
        ImGui::SetNextWindowSize({ICONS_W, 44.f});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2{0.f, 0.f});
        ImGui::Begin("##lobby_icons", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoSavedSettings);
        // Buttons use the existing UIStyle::GhostButton so they keep the
        // unified design language.
        if (UIStyle::GhostButton("Audio",  {84.f, 36.f}))
            m_audioPopupOpen = true;
        ImGui::SameLine(0.f, 4.f);
        if (UIStyle::GhostButton("Assets", {84.f, 36.f})) {
            m_assetsPopupOpen = true;
            gAudio().play("click");
        }
        // Debug panel is a developer-only surface — hidden for release.
        if (m_settings.developerMode) {
            ImGui::SameLine(0.f, 4.f);
            if (UIStyle::GhostButton("Debug",  {84.f, 36.f})) {
                m_debugPopupOpen = true;
                gAudio().play("click");
            }
        }
        ImGui::SameLine(0.f, 4.f);
        if (UIStyle::GhostButton("Settings", {96.f, 36.f})) {
            m_settingsPopupOpen = true;
            gAudio().play("click");
        }
        ImGui::SameLine(0.f, 4.f);
        if (UIStyle::GhostButton("Exit",   {88.f, 36.f})) {
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            extern bool g_quit;
            g_quit = true;
            return;
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // ── Update notice (top-centre) — newer release found on GitHub ──────────
    if (m_update.updateAvailable() && !m_updateDismissed) {
        std::string ver = m_update.latestVersion();
        std::string label = "Update available: v" + ver;
        ImGui::SetNextWindowPos({W * 0.5f, 16.f}, ImGuiCond_Always, {0.5f, 0.f});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 24, 14, 235));
        ImGui::PushStyleColor(ImGuiCol_Border,   C.accent);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{12.f, 8.f});
        ImGui::Begin("##update_notice", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C.accentHi));
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0.f, 12.f);

        using DL = edo::UpdateChecker::DownloadState;
        const DL ds = m_update.downloadState();
        if (m_update.canSelfUpdate()) {
            // One-click auto-update: download the installer, then launch it and
            // quit so it can overwrite the running app.
            if (ds == DL::Idle) {
                if (UIStyle::PrimaryButton("Update now", {120.f, 28.f}))
                    m_update.beginDownload();
            } else if (ds == DL::Running) {
                ImGui::TextDisabled("Downloading...");
                ImGui::SameLine(0.f, 8.f);
                char pct[16];
                snprintf(pct, sizeof(pct), "%d%%",
                         (int)(m_update.downloadProgress() * 100.0));
                ImGui::TextUnformatted(pct);
            } else if (ds == DL::Ready) {
                if (UIStyle::PrimaryButton("Install & Restart", {150.f, 28.f})) {
                    if (m_update.runInstaller()) {
                        extern bool g_quit; g_quit = true;   // let installer replace files
                    }
                }
            } else { // Failed
                ImGui::TextDisabled("Download failed");
                ImGui::SameLine(0.f, 8.f);
                if (UIStyle::GhostButton("Open page##upd", {96.f, 28.f}))
                    openExternalUrl(m_update.releaseUrl());
            }
        } else {
            // No installer asset (or unsupported platform): open the release page.
            if (UIStyle::PrimaryButton("Download", {110.f, 28.f}))
                openExternalUrl(m_update.releaseUrl());
        }
        ImGui::SameLine(0.f, 6.f);
        if (ds != DL::Running && UIStyle::GhostButton("Later##upd", {64.f, 28.f}))
            m_updateDismissed = true;
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // ── Left vertical navigation: DUEL / DECK / QUIT ────────────────────────
    {
        const float NAV_X = 48.f;
        const float NAV_Y = H * 0.26f;
        const float NAV_W = 300.f;
        ImGui::SetNextWindowPos({NAV_X, NAV_Y});
        ImGui::SetNextWindowSize({NAV_W, 430.f});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2{0.f, 0.f});
        ImGui::Begin("##lobby_nav", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoSavedSettings);
        // Custom nav row: gold accent bar + large label + subtitle.
        auto navItem = [&](const char* label, const char* subtitle,
                           bool primary) -> bool {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 sz  = {NAV_W, subtitle ? 68.f : 54.f};
            ImGui::InvisibleButton(label, sz);
            bool hov = ImGui::IsItemHovered();
            bool clk = ImGui::IsItemClicked();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 br = {pos.x + sz.x, pos.y + sz.y};
            // Hover background pill — a soft crimson wash + left-edge fade so
            // the row reads as selected without a boxy outline.
            if (hov) {
                dl->AddRectFilledMultiColor(pos, br,
                    (C.accent & 0x00FFFFFF) | 0x2A000000,
                    (C.accent & 0x00FFFFFF) | 0x06000000,
                    (C.accent & 0x00FFFFFF) | 0x06000000,
                    (C.accent & 0x00FFFFFF) | 0x2A000000);
            }
            // Left accent bar — bright crimson on primary/hover, deep red idle.
            ImU32 barCol = (primary || hov) ? C.accentHi
                                            : IM_COL32(96, 40, 46, 210);
            dl->AddRectFilled(
                {pos.x, pos.y + 9.f},
                {pos.x + 4.f, pos.y + sz.y - 9.f}, barCol, 2.f);
            // Label.
            if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
            ImU32 textCol = (primary || hov) ? C.textHi : C.textMd;
            dl->AddText({pos.x + 20.f, pos.y + (subtitle ? 10.f : 16.f)},
                        textCol, label);
            if (UIStyle::fHeader) ImGui::PopFont();
            // Subtitle.
            if (subtitle) {
                UIStyle::PushFont(UIStyle::fSmall);
                dl->AddText({pos.x + 20.f, pos.y + 36.f},
                            hov ? C.textLo : C.textMuted, subtitle);
                UIStyle::PopFont();
            }
            return clk;
        };
        // Primary entry point — the live online lobby. Jumps straight to the
        // Multiplayer screen with the Online (relay) tab active and forces an
        // immediate room-list refresh so the list is warm on arrival.
        if (navItem("ONLINE ROOMS", "Browse and join live online rooms", true)) {
            gAudio().play("click");
            strncpy(m_mpNameBuf, m_settings.mpDisplayName.c_str(),
                    sizeof(m_mpNameBuf) - 1);
            m_mpNameBuf[sizeof(m_mpNameBuf) - 1] = '\0';
            strncpy(m_mpRelayAddrBuf, m_settings.mpHostIP.c_str(),
                    sizeof(m_mpRelayAddrBuf) - 1);
            m_mpRelayAddrBuf[sizeof(m_mpRelayAddrBuf) - 1] = '\0';
            m_mpTransport        = 1;     // Online (relay) tab
            m_lobbyNextRefreshAt = 0.0;   // refresh the lobby immediately
            m_screen             = Screen::Multiplayer;
        }
        // Single-player practice duel (the offline duel we have).
        if (navItem("TESTING", "Single-player practice duel", false)) {
            gAudio().play("click");
            m_duelSetupOpen = true;
        }
        if (navItem("DECK BUILDER", "Build and edit your decks", false)) {
            gAudio().play("click");
            refreshDeckFiles();
            m_screen = Screen::DeckBuilder;
        }
        if (navItem("REPLAYS", "Browse and play back saved matches", false)) {
            gAudio().play("click");
            m_replayFiles = edo::Replay::list();
            m_selectedReplay = m_replayFiles.empty() ? -1 : 0;
            m_viewerReplayValid = false;
            if (!m_replayFiles.empty())
                m_viewerReplayValid = m_viewerReplay.load(m_replayFiles[0]);
            m_screen = Screen::Replays;
        }
        if (navItem("PUZZLES", "Solve preset boards — win in one turn", false)) {
            gAudio().play("click");
            if (m_puzzles.empty()) loadPuzzles();
            m_puzzleBrowserOpen = true;
        }
        // Win/loss record + current streak from match history (#D).
        std::string histSub = "Win/loss record and stats";
        if (!m_matchHistory.empty()) {
            int wins = 0, losses = 0;
            for (auto& r : m_matchHistory) {
                if (r.result == 'W') wins++; else if (r.result == 'L') losses++;
            }
            char last = m_matchHistory.back().result;
            int streak = 0;
            for (auto it = m_matchHistory.rbegin();
                 it != m_matchHistory.rend() && it->result == last; ++it)
                streak++;
            char sb[80];
            if (last == 'W' || last == 'L')
                snprintf(sb, sizeof(sb), "%d W - %d L   ·   %d %s streak",
                         wins, losses, streak, last == 'W' ? "win" : "loss");
            else
                snprintf(sb, sizeof(sb), "%d W - %d L", wins, losses);
            histSub = sb;
        }
        if (navItem("MATCH HISTORY", histSub.c_str(), false)) {
            gAudio().play("click");
            m_historyOpen = true;
        }
        if (navItem("LAN MULTIPLAYER", "Direct match on your local network",
                    false)) {
            gAudio().play("click");
            strncpy(m_mpNameBuf, m_settings.mpDisplayName.c_str(),
                    sizeof(m_mpNameBuf) - 1);
            strncpy(m_mpIPBuf,   m_settings.mpHostIP.c_str(),
                    sizeof(m_mpIPBuf)   - 1);
            m_mpPortBuf  = m_settings.mpPort;
            m_mpTransport = 0;     // LAN (direct) tab
            m_screen = Screen::Multiplayer;
        }
        if (navItem("QUIT", "Exit the application", false)) {
            gAudio().play("cancel");
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            extern bool g_quit;
            g_quit = true;
            return;
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // ── Lower-left news / status card ────────────────────────────────────────
    int dbCount = m_db.databaseCount();
    int scriptCount = 0;
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (fs::is_directory("assets/scripts/official", ec))
            for (auto& e : fs::directory_iterator("assets/scripts/official", ec))
                if (e.path().extension() == ".lua") ++scriptCount;
    }
    // Lower-left SYSTEM STATUS panel — a premium game panel with coloured
    // status chips (DB / Scripts / Audio / Online) so the readiness of the
    // runtime reads at a glance instead of a plain text strip.
    {
        const float NW = 300.f, NH = 120.f;
        ImVec2 a = {26.f, H - NH - 28.f};
        ImVec2 b = {a.x + NW, a.y + NH};
        // Match the identity card's material: gold halo + glass + top sheen.
        UIStyle::DrawGlow(bg, a, b, (C.accent & 0x00FFFFFF) | 0x20000000,
                          UIStyle::M().radL, 3);
        UIStyle::DrawGlassPanel(bg, a, b, UIStyle::M().radL,
                                IM_COL32(28, 15, 18, 230));
        bg->AddLine({a.x + 16.f, a.y + 1.5f}, {b.x - 16.f, a.y + 1.5f},
                    (C.accent & 0x00FFFFFF) | 0x55000000, 1.f);
        // Header row: small gold marker + label.
        bg->AddRectFilled({a.x + 18.f, a.y + 16.f}, {a.x + 25.f, a.y + 23.f},
                          C.accent, 2.f);
        UIStyle::PushFont(UIStyle::fSmall);
        bg->AddText({a.x + 33.f, a.y + 15.f}, C.textLo, "SYSTEM STATUS");
        UIStyle::PopFont();

        // Chip drawer: status dot + label, coloured by state, in a soft pill.
        auto chip = [&](ImVec2 p, ImU32 col, const char* text) {
            ImVec2 ts = ImGui::CalcTextSize(text);
            ImVec2 c0{p.x, p.y}, c1{p.x + ts.x + 28.f, p.y + 24.f};
            bg->AddRectFilled(c0, c1, (col & 0x00FFFFFF) | 0x26000000, 12.f);
            bg->AddRect(c0, c1, (col & 0x00FFFFFF) | 0x99000000, 12.f, 0, 1.f);
            bg->AddCircleFilled({p.x + 13.f, p.y + 12.f}, 4.f, col, 12);
            bg->AddText({p.x + 23.f, p.y + 5.f}, C.textHi, text);
            return c1.x;
        };
        bool audioOk = gAudio().isAvailable() && !gAudio().muted();
        bool relayCfg = !m_settings.mpHostIP.empty() &&
                        m_settings.mpHostIP != "127.0.0.1";
        char dbTxt[40]; snprintf(dbTxt, sizeof(dbTxt), "Card DB  %d", dbCount);
        char scTxt[40]; snprintf(scTxt, sizeof(scTxt), "Scripts  %d", scriptCount);
        const char* auTxt = !gAudio().isAvailable() ? "Audio  off"
                          : gAudio().muted()        ? "Audio  muted"
                                                    : "Audio  ready";
        const char* rlTxt = relayCfg ? "Relay  set" : "Relay  local";
        // 2x2 grid so the panel reads at a glance.
        float rowA = a.y + 42.f, rowB = a.y + 76.f;
        chip({a.x + 18.f, rowA}, dbCount > 0 ? C.success : C.danger, dbTxt);
        chip({a.x + 162.f, rowA}, scriptCount > 0 ? C.success : C.warning, scTxt);
        chip({a.x + 18.f, rowB}, audioOk ? C.success : C.warning, auTxt);
        chip({a.x + 162.f, rowB}, relayCfg ? C.primaryHi : C.textMuted, rlTxt);
    }

    // ── Bottom-right footer build line ──────────────────────────────────────
    UIStyle::PushFont(UIStyle::fSmall);
    {
        char foot[64];
        snprintf(foot, sizeof(foot), "%s  ·  v%s  ·  modern duel client",
                 edo::kAppName, edo::kAppVersion);
        ImVec2 fsz = ImGui::CalcTextSize(foot);
        bg->AddText({W - fsz.x - 26.f, H - 26.f}, C.textMuted, foot);
    }
    UIStyle::PopFont();

    // Match-history popup (opened from the MATCH HISTORY nav item).
    drawHistory();
    // Puzzle browser popup (opened from the PUZZLES nav item).
    drawPuzzleBrowser();

    // ── Audio settings popup (opened by the top-right Audio button) ────────
    if (m_audioPopupOpen) { ImGui::OpenPopup("Audio Settings"); m_audioPopupOpen = false; }
    ImGui::SetNextWindowSize({460.f, 0.f}, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Audio Settings", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::TextUnformatted("Audio");
        if (UIStyle::fHeader) ImGui::PopFont();
        ImGui::TextDisabled("Procedural SFX bank — placeholder sounds.");
        ImGui::Separator();
        bool av     = gAudio().isAvailable();
        bool muted  = gAudio().muted();
        float vol   = gAudio().volume();
        ImGui::Text("Device: %s", av ? "open" : "unavailable");
        if (ImGui::Checkbox("Mute SFX", &muted)) gAudio().setMuted(muted);
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderFloat("Volume", &vol, 0.f, 1.f, "%.2f"))
            gAudio().setVolume(vol);
        ImGui::Spacing();
        if (UIStyle::SecondaryButton("Test Sound", {160.f, 32.f}))
            gAudio().play("confirm");
        ImGui::SameLine(0.f, 10.f);
        ImGui::TextDisabled("(plays confirm.wav)");
        ImGui::Separator();

        // ── Loaded/missing diagnostics — authoritative numbers come from
        //    AudioManager so they can never drift from what was actually
        //    cached at startup.
        const char* const* names = AudioManager::expectedSfx();
        int expected = AudioManager::expectedSfxCount();
        int loaded   = gAudio().loadedCount();
        ImVec4 okCol  = {0.55f, 0.85f, 0.55f, 1.f};
        ImVec4 badCol = {0.95f, 0.55f, 0.45f, 1.f};
        ImGui::Text("SFX bank: %d / %d loaded", loaded, expected);
        if (loaded < expected) {
            ImGui::TextColored(badCol, "Missing:");
            ImGui::BeginChild("##missing_sfx", {-1.f, 90.f}, true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            for (int i = 0; names[i]; ++i) {
                if (!gAudio().isLoaded(names[i])) {
                    ImGui::BulletText("%s.wav", names[i]);
                }
            }
            ImGui::EndChild();
        } else {
            ImGui::TextColored(okCol, "All SFX loaded.");
        }
        ImGui::Spacing();
        ImGui::TextDisabled("To generate placeholder WAVs, run:");
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(C.accent));
        ImGui::TextUnformatted("  python tools/generate_sfx.py");
        ImGui::PopStyleColor();
        if (UIStyle::SecondaryButton("Copy command", {160.f, 28.f})) {
            ImGui::SetClipboardText("python tools/generate_sfx.py");
            m_dm.logEvent("[audio] copied generator command to clipboard");
        }
        ImGui::Spacing();
        if (UIStyle::GhostButton("Close", {-1.f, 30.f}))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ── Assets diagnostics popup (top-right Assets button) ─────────────────
    if (m_assetsPopupOpen) { ImGui::OpenPopup("Assets"); m_assetsPopupOpen = false; }
    ImGui::SetNextWindowSize({580.f, 0.f}, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Assets", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::TextUnformatted("Assets");
        if (UIStyle::fHeader) ImGui::PopFont();
        ImGui::TextDisabled("Where the simulator is reading its data from.");
        ImGui::Separator();

        namespace fs = std::filesystem;
        std::error_code ec;
        auto absOf = [](const char* rel) -> std::string {
            std::error_code ec2;
            auto p = std::filesystem::absolute(rel, ec2);
            return ec2 ? std::string(rel) : p.string();
        };
        auto fileSizeKB = [](const char* rel) -> long long {
            std::error_code ec2;
            auto sz = std::filesystem::file_size(rel, ec2);
            return ec2 ? -1 : (long long)(sz / 1024);
        };
        auto countFiles = [](const char* dir, const char* ext) -> int {
            std::error_code ec2;
            if (!std::filesystem::is_directory(dir, ec2)) return -1;
            int n = 0;
            for (auto& e : std::filesystem::recursive_directory_iterator(
                     dir, std::filesystem::directory_options::skip_permission_denied, ec2)) {
                if (ec2) break;
                if (!e.is_regular_file()) continue;
                if (!ext || e.path().extension() == ext) ++n;
            }
            return n;
        };

        std::string cwd = fs::current_path(ec).string();
        ImGui::Text("Working dir : %s", cwd.c_str());
        // Card back load status — full path when the asset loaded, or the
        // searched locations when the procedural fallback is in use.
        {
            const std::string& cb = m_rend.cardBackInfo();
            bool ok = cb.rfind("card_back loaded", 0) == 0;
            ImGui::PushStyleColor(ImGuiCol_Text,
                ok ? ImVec4{0.55f, 0.92f, 0.65f, 1.f}
                   : ImVec4{1.00f, 0.78f, 0.30f, 1.f});
            ImGui::PushTextWrapPos(560.f);
            ImGui::TextWrapped("Card back   : %s", cb.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::Separator();

        // Card database
        std::string cdbPath = absOf("assets/cards.cdb");
        bool cdbExists = fs::is_regular_file("assets/cards.cdb", ec);
        long long cdbKB = cdbExists ? fileSizeKB("assets/cards.cdb") : -1;
        ImGui::Text("cards.cdb   : %s%s",
                    cdbPath.c_str(),
                    cdbExists ? "" : "  (missing)");
        if (cdbKB >= 0)
            ImGui::TextDisabled("              size %lld KB", cdbKB);
        int babelCount = countFiles("assets/BabelCDB-master", ".cdb");
        if (babelCount >= 0)
            ImGui::TextDisabled("              + %d .cdb in BabelCDB-master/", babelCount);

        // Scripts
        std::string scrPath = absOf("assets/scripts");
        bool scrExists = fs::is_directory("assets/scripts", ec);
        int scrCount = scrExists ? countFiles("assets/scripts", ".lua") : -1;
        ImGui::Text("scripts/    : %s", scrPath.c_str());
        if (scrCount >= 0)
            ImGui::TextDisabled("              %d .lua files", scrCount);
        else
            ImGui::TextDisabled("              (folder missing)");

        // SFX
        std::string sfxPath = absOf("assets/sfx");
        int sfxOnDisk = countFiles("assets/sfx", ".wav");
        int sfxLoaded = gAudio().loadedCount();
        int sfxExpected = AudioManager::expectedSfxCount();
        ImGui::Text("sfx/        : %s", sfxPath.c_str());
        ImGui::TextDisabled("              %d .wav on disk, %d / %d loaded",
                            sfxOnDisk < 0 ? 0 : sfxOnDisk, sfxLoaded, sfxExpected);

        // Card back / decks
        bool backExists = fs::is_regular_file("assets/card_back.png", ec);
        ImGui::Text("card_back   : %s",
                    backExists ? absOf("assets/card_back.png").c_str()
                               : "(missing — using procedural fallback)");
        std::string deckPath = absOf("assets/decks");
        int ydk = countFiles("assets/decks", ".ydk");
        ImGui::Text("decks/      : %s", deckPath.c_str());
        if (ydk >= 0)
            ImGui::TextDisabled("              %d .ydk deck(s)", ydk);

        // ── Health summary ──────────────────────────────────────────────
        // Surfaces the startup health check inline. Lines are PASS / WARN
        // prefixed; we colour the WARN ones so they're scannable.
        if (!m_healthSummary.empty()) {
            ImGui::Separator();
            UIStyle::SectionHeader(
                m_healthWarnings > 0 ? "Health (warnings)" : "Health (OK)");
            ImGui::BeginChild("##health_summary", {-1.f, 130.f}, true);
            std::string line;
            const std::string& s = m_healthSummary;
            for (size_t i = 0; i <= s.size(); ++i) {
                if (i == s.size() || s[i] == '\n') {
                    if (!line.empty()) {
                        bool warn = line.rfind("[WARN]", 0) == 0;
                        ImVec4 col = warn
                            ? ImVec4{1.f, 0.78f, 0.30f, 1.f}
                            : ImVec4{0.55f, 0.92f, 0.65f, 1.f};
                        ImGui::PushStyleColor(ImGuiCol_Text, col);
                        ImGui::TextWrapped("%s", line.c_str());
                        ImGui::PopStyleColor();
                    }
                    line.clear();
                } else line += s[i];
            }
            ImGui::EndChild();
        }

        ImGui::Separator();
        if (UIStyle::SecondaryButton("Copy diagnostics", {180.f, 30.f})) {
            std::ostringstream os;
            os << "[assets diagnostics]\n"
               << "cwd        : " << cwd << "\n"
               << "cards.cdb  : " << cdbPath
               << (cdbExists ? "" : "  (missing)") << "\n";
            if (cdbKB >= 0) os << "             size " << cdbKB << " KB\n";
            if (babelCount >= 0)
                os << "             + " << babelCount << " .cdb in BabelCDB-master/\n";
            os << "scripts/   : " << scrPath << "\n";
            if (scrCount >= 0) os << "             " << scrCount << " .lua files\n";
            os << "sfx/       : " << sfxPath << "\n"
               << "             " << (sfxOnDisk < 0 ? 0 : sfxOnDisk)
               << " .wav on disk, " << sfxLoaded << " / " << sfxExpected << " loaded\n"
               << "card_back  : " << (backExists ? "ok" : "missing") << "\n"
               << "decks/     : " << deckPath << "\n";
            if (ydk >= 0) os << "             " << ydk << " .ydk\n";
            ImGui::SetClipboardText(os.str().c_str());
            m_dm.logEvent("[assets] diagnostics copied to clipboard");
        }
        ImGui::SameLine(0.f, 8.f);
        if (UIStyle::GhostButton("Close", {120.f, 30.f}))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ── Debug diagnostics popup (top-right Debug button) ───────────────────
    if (m_debugPopupOpen) { ImGui::OpenPopup("Debug Status"); m_debugPopupOpen = false; }
    ImGui::SetNextWindowSize({560.f, 0.f}, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Debug Status", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::TextUnformatted("Debug");
        if (UIStyle::fHeader) ImGui::PopFont();
        ImGui::TextDisabled("Internal state — useful when filing a bug report.");
        ImGui::Separator();

        // Screen + duel state.
        const char* scrName = m_screen == Screen::Lobby ? "Lobby"
                            : m_screen == Screen::Duel ? "Duel"
                            : "DeckBuilder";
        ImGui::Text("Screen      : %s", scrName);
        bool inDuel = m_dm.isRunning();
        ImGui::Text("Duel active : %s", inDuel ? "yes" : "no");
        if (inDuel) {
            ImGui::Text("Turn player : P%d", (int)currentField().turnPlayer);
            ImGui::Text("Phase       : 0x%X", (unsigned)currentField().phase);
            ImGui::Text("Done        : %s", m_dm.isDone() ? "yes" : "no");
        }

        // Audio status.
        ImGui::Separator();
        ImGui::Text("Audio device: %s", gAudio().isAvailable() ? "open" : "closed");
        ImGui::Text("SFX loaded  : %d / %d  (muted=%s, vol=%.2f)",
                    gAudio().loadedCount(), AudioManager::expectedSfxCount(),
                    gAudio().muted() ? "yes" : "no",
                    gAudio().volume());

        // Toggles — let the user flip diagnostics behaviours from here without
        // hunting the Testing panel.
        ImGui::Separator();
        ImGui::TextDisabled("Toggles");
        if (ImGui::Checkbox("Verbose engine log", &m_debugLog))
            m_dm.setDebugMessages(m_debugLog);
        ImGui::Checkbox("Zone labels",        &m_showZoneLabels);
        ImGui::Checkbox("Legal-action glow",  &m_showLegalGlow);
        ImGui::Checkbox("Card name strip",    &m_showFieldNames);
        ImGui::Checkbox("Large card preview", &m_largePreview);
        bool muted = gAudio().muted();
        if (ImGui::Checkbox("Mute SFX", &muted)) gAudio().setMuted(muted);

        ImGui::Separator();
        ImGui::TextDisabled("Last warning:");
        ImGui::TextWrapped("%s",
            m_lastWarning.empty() ? "(none)" : m_lastWarning.c_str());

        ImGui::Separator();
        if (UIStyle::SecondaryButton("Copy debug report", {180.f, 30.f})) {
            std::ostringstream os;
            os << "[debug report]\n"
               << "screen      : " << scrName << "\n"
               << "duel active : " << (inDuel ? "yes" : "no") << "\n";
            if (inDuel) {
                os << "turn player : P" << (int)currentField().turnPlayer << "\n"
                   << "phase       : 0x" << std::hex << (unsigned)currentField().phase
                   << std::dec << "\n"
                   << "done        : " << (m_dm.isDone() ? "yes" : "no") << "\n";
            }
            os << "audio       : "
               << (gAudio().isAvailable() ? "open" : "closed")
               << "  sfx " << gAudio().loadedCount() << "/"
               << AudioManager::expectedSfxCount()
               << "  muted=" << (gAudio().muted() ? "yes" : "no")
               << "  vol=" << gAudio().volume() << "\n"
               << "last warn   : "
               << (m_lastWarning.empty() ? "(none)" : m_lastWarning) << "\n";
            ImGui::SetClipboardText(os.str().c_str());
            m_dm.logEvent("[debug] report copied to clipboard");
        }
        ImGui::SameLine(0.f, 6.f);
        // Full diagnostics — includes the health summary and the last 100
        // debug log lines, formatted for inclusion in a bug report.
        if (UIStyle::SecondaryButton("Copy Full Diagnostics", {200.f, 30.f})) {
            ImGui::SetClipboardText(buildFullDiagnostics().c_str());
            pushToast("Full diagnostics copied",
                      IM_COL32(180, 220, 255, 255), 2.4);
            m_dm.logEvent("[debug] full diagnostics copied");
        }
        ImGui::SameLine(0.f, 8.f);
        if (UIStyle::GhostButton("Close", {120.f, 30.f}))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ── First-run welcome (display-name setup) ──────────────────────────────
    if (m_showWelcome) {
        ImGui::OpenPopup("Welcome##firstrun");
        m_showWelcome = false;
        strncpy(m_mpNameBuf, m_settings.mpDisplayName.c_str(),
                sizeof(m_mpNameBuf) - 1);
        m_mpNameBuf[sizeof(m_mpNameBuf) - 1] = '\0';
    }
    ImGui::SetNextWindowSize({420.f, 0.f}, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Welcome##firstrun", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::TextUnformatted("Welcome to YGO: Nova");
        if (UIStyle::fHeader) ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextWrapped("Pick a display name — it's shown to opponents in "
            "online and LAN duels. You can change it any time in Settings.");
        ImGui::Dummy({1.f, 8.f});
        ImGui::TextUnformatted("Display name");
        ImGui::SetNextItemWidth(-1.f);
        bool enter = ImGui::InputTextWithHint("##welcomename", "Player",
            m_mpNameBuf, sizeof(m_mpNameBuf),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Dummy({1.f, 10.f});
        if (UIStyle::PrimaryButton("Let's duel", {-1.f, 36.f}) || enter) {
            m_settings.mpDisplayName = m_mpNameBuf[0] ? m_mpNameBuf : "Player";
            saveSettings();
            gAudio().play("confirm");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Settings popup ──────────────────────────────────────────────────────
    // Multi-section modal that surfaces every persisted user preference.
    // Changes apply immediately to the live UI/audio subsystems AND save to
    // disk on every toggle so a crash never loses settings.
    if (m_settingsPopupOpen) {
        ImGui::OpenPopup("Settings");
        m_settingsPopupOpen = false;
    }
    ImGui::SetNextWindowSize({560.f, 0.f}, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Settings", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::TextUnformatted("Settings");
        if (UIStyle::fHeader) ImGui::PopFont();
        ImGui::TextDisabled("Changes apply immediately and persist on disk.");
        ImGui::Separator();

        auto savedToggle = [this](const char* label, bool* val) {
            if (ImGui::Checkbox(label, val)) saveSettings();
        };

        // — Audio
        UIStyle::SectionHeader("Audio");
        bool muted = gAudio().muted();
        if (ImGui::Checkbox("Mute SFX", &muted)) {
            gAudio().setMuted(muted);
            pushToast(muted ? "SFX muted" : "SFX unmuted",
                      IM_COL32(255, 214, 108, 255), 1.5);
            saveSettings();
        }
        float vol = gAudio().volume();
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderFloat("Volume", &vol, 0.f, 1.f, "%.2f")) {
            gAudio().setVolume(vol);
            saveSettings();
        }
        if (UIStyle::SecondaryButton("Test Sound", {160.f, 30.f}))
            gAudio().play("confirm");
        ImGui::Separator();

        // — Visual
        UIStyle::SectionHeader("Visual");
        savedToggle("Card name strip on field", &m_showFieldNames);
        savedToggle("Large card preview",       &m_largePreview);
        savedToggle("Show zone labels",         &m_showZoneLabels);
        savedToggle("Show legal-action glow",   &m_showLegalGlow);

        // — Card sleeve picker — click a thumbnail to set the card back. —
        UIStyle::SectionHeader("Card sleeve");
        {
            namespace fs = std::filesystem;
            std::vector<std::string> sleeves;
            sleeves.push_back("");                    // "Default" tile first
            std::error_code ec;
            if (fs::is_directory("assets/sleeves", ec))
                for (auto& e : fs::directory_iterator("assets/sleeves", ec)) {
                    std::string ext = e.path().extension().string();
                    for (char& ch : ext) ch = (char)tolower((unsigned char)ch);
                    if (ext == ".png" || ext == ".jpg")
                        sleeves.push_back(e.path().filename().string());
                }
            if (sleeves.size() > 2)
                std::sort(sleeves.begin() + 1, sleeves.end());

            const float thumbW = 52.f, thumbH = 74.f;
            float availW = ImGui::GetContentRegionAvail().x;
            int perRow = std::max(1, (int)(availW / (thumbW + 10.f)));
            int col = 0;
            for (size_t i = 0; i < sleeves.size(); ++i) {
                const std::string& s = sleeves[i];
                bool selected = (s == m_settings.cardSleeve);
                ImGui::PushID((int)i);
                void* tex = s.empty()
                    ? m_rend.loadCachedImage("assets/card_back.png")
                    : m_rend.loadCachedImage("assets/sleeves/" + s);
                if (!tex) tex = m_rend.getBackTexture();
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##slv", {thumbW, thumbH});
                bool clicked = ImGui::IsItemClicked();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 br{p.x + thumbW, p.y + thumbH};
                if (tex) dl->AddImage(tex, p, br);
                else     dl->AddRectFilled(p, br, IM_COL32(40, 46, 70, 255), 4.f);
                if (selected)
                    dl->AddRect(p, br, IM_COL32(232, 196, 110, 255), 4.f, 0, 3.f);
                else if (ImGui::IsItemHovered())
                    dl->AddRect(p, br, IM_COL32(255, 255, 255, 160), 4.f, 0, 1.5f);
                if (clicked) {
                    m_settings.cardSleeve = s;
                    m_rend.setCardBack(s.empty() ? "assets/card_back.png"
                                                 : ("assets/sleeves/" + s));
                    saveSettings();
                }
                ImGui::PopID();
                if (++col < perRow && i + 1 < sleeves.size())
                    ImGui::SameLine(0.f, 10.f);
                else col = 0;
            }
            ImGui::TextDisabled("Drop more PNGs in assets/sleeves/ to add sleeves.");
        }
        ImGui::Separator();

        // — Animations (Stage A) — every change re-syncs the live AnimManager.
        UIStyle::SectionHeader("Animations");
        auto animToggle = [this](const char* label, bool* val) {
            if (ImGui::Checkbox(label, val)) { syncAnimConfig(); saveSettings(); }
        };
        animToggle("Enable animations",          &m_settings.animationsEnabled);
        if (!m_settings.animationsEnabled) ImGui::BeginDisabled();
        // Game speed — the headline pacing control (Relaxed slows each
        // summon/activation so you can read what's happening).
        ImGui::TextDisabled("Game speed (duel pacing)");
        struct { const char* l; int v; } kGS[] = {
            {"Relaxed", 0}, {"Normal", 1}, {"Fast", 2} };
        for (int i = 0; i < 3; ++i) {
            bool active = (m_settings.gameSpeed == kGS[i].v);
            if (UIStyle::SegmentedButton(kGS[i].l, active, true, {112.f, 26.f})) {
                m_settings.gameSpeed = kGS[i].v;
                syncAnimConfig(); saveSettings();
            }
            if (i < 2) ImGui::SameLine(0.f, 4.f);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Holds briefly after each summon/effect so the "
                              "board is readable. Offline duels only.");
        animToggle("Big monster summon animation", &m_settings.animBigSummons);
        animToggle("Phase banners",              &m_settings.animPhaseBanners);
        animToggle("Screen shake on big events", &m_settings.animScreenShake);
        animToggle("Reduce motion (accessibility)", &m_settings.animReduceMotion);
        // Animation speed — 0.5x / 1x / 2x / Instant segmented buttons.
        ImGui::TextDisabled("Animation speed");
        struct { const char* l; float v; } kSpeeds[] = {
            {"0.5x", 0.5f}, {"1x", 1.0f}, {"2x", 2.0f}, {"Instant", 0.0f} };
        for (int i = 0; i < 4; ++i) {
            bool active = (m_settings.animSpeed == kSpeeds[i].v);
            if (UIStyle::SegmentedButton(kSpeeds[i].l, active, true, {84.f, 26.f})) {
                m_settings.animSpeed = kSpeeds[i].v;
                syncAnimConfig(); saveSettings();
            }
            if (i < 3) ImGui::SameLine(0.f, 4.f);
        }
        // Phase delay slider — cosmetic hold between phases (ms).
        int delayMs = (int)(m_settings.animPhaseDelay * 1000.f);
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderInt("Phase delay (ms)", &delayMs, 0, 1000)) {
            m_settings.animPhaseDelay = (float)delayMs / 1000.f;
            syncAnimConfig(); saveSettings();
        }
        // Show skipped-phase banners (Draw/Standby on an empty turn).
        animToggle("Show skipped phase banners",
                   &m_settings.animShowSkippedPhases);
        int pminMs = (int)(m_settings.animPhaseMinDuration * 1000.f);
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderInt("Phase banner min (ms)", &pminMs, 100, 900)) {
            m_settings.animPhaseMinDuration = (float)pminMs / 1000.f;
            saveSettings();
        }
        if (!m_settings.animationsEnabled) ImGui::EndDisabled();

        // Engine phase pacing — independent of animations (works even with
        // animations off). Offline only. 0 = instant.
        ImGui::TextDisabled("Phase pacing (offline) — hold each phase so you "
                            "see every phase + end-of-phase effects");
        int paceMs = (int)(m_settings.enginePhasePacing * 1000.f);
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderInt("Phase pacing (ms)##pace", &paceMs, 0, 2000)) {
            m_settings.enginePhasePacing = (float)paceMs / 1000.f;
            saveSettings();
        }
        ImGui::Separator();

        // — Developer / Logs
        UIStyle::SectionHeader("Developer");
        savedToggle("Developer mode (unlocks dev tools + Debug panel)",
                    &m_settings.developerMode);
        if (m_settings.developerMode) {
            if (ImGui::Checkbox("Debug Log (verbose engine traces)",
                                &m_debugLog)) {
                m_dm.setDebugMessages(m_debugLog);
                saveSettings();
            }
            savedToggle("Verbose internal messages", &m_settings.verboseLog);
        }
        savedToggle("Collapse log panel by default", &m_logCollapsed);
        ImGui::Separator();

        // — Gameplay UI
        UIStyle::SectionHeader("Gameplay UI");
        savedToggle("Confirm before Restart Duel", &m_settings.confirmRestart);
        savedToggle("Show click-first hints",      &m_settings.clickFirstHints);
        savedToggle("Compact prompts (full text in side panel)",
                    &m_settings.compactPrompts);
        savedToggle("Coin toss decides who goes first (offline)",
                    &m_settings.coinTossEnabled);
        savedToggle("Fast turns (skip the pause between phases)",
                    &m_settings.fastTurns);
        if (ImGui::Checkbox("Download missing card art on demand",
                            &m_settings.downloadCardImages)) {
            m_rend.setImageDownload(m_settings.downloadCardImages);
            saveSettings();
        }
        if (ImGui::Checkbox("Check for updates on launch",
                            &m_settings.checkForUpdates)) {
            m_update.setEnabled(m_settings.checkForUpdates);
            saveSettings();
        }
        ImGui::Separator();

        // — Replays
        UIStyle::SectionHeader("Replays");
        savedToggle("Auto-save replays on duel end", &m_settings.autoSaveReplays);
        ImGui::TextDisabled("Saved to %s", edo::Replay::defaultDir().c_str());
        ImGui::Separator();

        // — Assets summary (read-only)
        UIStyle::SectionHeader("Assets");
        ImGui::Text("SFX bank   : %d / %d loaded",
            gAudio().loadedCount(), AudioManager::expectedSfxCount());
        ImGui::Text("Card DBs   : %d", m_db.databaseCount());
        ImGui::TextDisabled("(open the Assets popup on the lobby for full paths)");
        ImGui::Separator();

        if (UIStyle::PrimaryButton("Close", {-1.f, 32.f})) {
            saveSettings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // The previous code-path opened the duel setup popup right after the
    // menu was drawn; that flow is preserved below verbatim.

    // ── Duel setup popup ──────────────────────────────────────────────────────
    if (m_duelSetupOpen) {
        refreshDeckFiles();
        // Auto-select first deck for any player that has no deck chosen
        if (!m_deckFiles.empty()) {
            if (m_deck0Path[0] == '\0') {
                m_deck0Idx = 0;
                std::string p = "assets/decks/" + m_deckFiles[0];
                strncpy(m_deck0Path, p.c_str(), sizeof(m_deck0Path) - 1);
            }
            if (m_deck1Path[0] == '\0') {
                m_deck1Idx = (m_deckFiles.size() > 1) ? 1 : 0;
                std::string p = "assets/decks/" + m_deckFiles[m_deck1Idx];
                strncpy(m_deck1Path, p.c_str(), sizeof(m_deck1Path) - 1);
            }
        }
        ImGui::OpenPopup("Duel Setup");
        m_duelSetupOpen = false;
    }
    // Modal sizing: AlwaysAutoResize fits to the actual content, so the
    // setup dialog can't render a "blank big modal" — its height matches
    // the form. The constraint clamps width to a comfortable readable range.
    ImGui::SetNextWindowSizeConstraints({480.f, 0.f}, {480.f, FLT_MAX});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{22.f, 20.f});
    if (ImGui::BeginPopupModal("Duel Setup", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings)) {

        UIStyle::PushFont(UIStyle::fHeader);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(UIStyle::C().accentHi));
        ImGui::TextUnformatted("Duel Setup");
        ImGui::PopStyleColor();
        UIStyle::PopFont();
        ImGui::TextDisabled("Pick a deck for each side, then start the duel.");
        UIStyle::DrawDivider(6.f, 6.f);

        auto deckPicker = [&](const char* title, const char* badge,
                              int& idx, char* pathBuf, size_t pathSz) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.85f, 0.88f, 0.95f, 1.f});
            ImGui::Text("%s  %s", badge, title);
            ImGui::PopStyleColor();
            const char* preview = (idx >= 0 && idx < (int)m_deckFiles.size())
                ? m_deckFiles[idx].c_str() : "(choose a deck file)";
            ImGui::SetNextItemWidth(-1.f);
            std::string comboId = std::string("##dc") + title;
            if (ImGui::BeginCombo(comboId.c_str(), preview)) {
                for (int i = 0; i < (int)m_deckFiles.size(); i++) {
                    bool sel = (idx == i);
                    if (ImGui::Selectable(m_deckFiles[i].c_str(), sel)) {
                        idx = i;
                        std::string p = "assets/decks/" + m_deckFiles[i];
                        strncpy(pathBuf, p.c_str(), pathSz - 1);
                        pathBuf[pathSz - 1] = '\0';
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(-1.f);
            std::string inpId = std::string("##dp") + title;
            if (ImGui::InputText(inpId.c_str(), pathBuf, pathSz))
                idx = -1;
            // Compact deck-size preview if the file resolves.
            if (pathBuf[0]) {
                Deck d = loadYdk(pathBuf);
                ImGui::TextDisabled("    Main %d  ·  Extra %d  ·  Side %d",
                                    (int)d.main.size(), (int)d.extra.size(),
                                    (int)d.side.size());
            } else {
                ImGui::TextDisabled("    (no deck selected)");
            }
        };

        deckPicker("Player 1 deck",  "P1", m_deck0Idx,
                   m_deck0Path, sizeof(m_deck0Path));
        ImGui::Dummy({1.f, 8.f});
        deckPicker("Player 2 deck",  "P2", m_deck1Idx,
                   m_deck1Path, sizeof(m_deck1Path));

        // Preset opponent — override P2 with a bundled AI archetype deck.
        if (!m_presetFiles.empty()) {
            ImGui::Dummy({1.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.85f, 0.88f, 0.95f, 1.f});
            ImGui::Text("AI  Preset opponent  (overrides P2)");
            ImGui::PopStyleColor();
            std::string curOpp =
                m_opponentPreset == -1 ? std::string("Off - use P2 deck above")
              : m_opponentPreset == 0  ? std::string("Random preset")
              : presetLabel(m_presetFiles[(size_t)m_opponentPreset - 1]);
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::BeginCombo("##oppPreset", curOpp.c_str())) {
                if (ImGui::Selectable("Off - use P2 deck above", m_opponentPreset == -1))
                    m_opponentPreset = -1;
                if (ImGui::Selectable("Random preset", m_opponentPreset == 0))
                    m_opponentPreset = 0;
                for (int i = 0; i < (int)m_presetFiles.size(); ++i)
                    if (ImGui::Selectable(presetLabel(m_presetFiles[(size_t)i]).c_str(),
                                          m_opponentPreset == i + 1))
                        m_opponentPreset = i + 1;
                ImGui::EndCombo();
            }
        }

        ImGui::Dummy({1.f, 10.f});
        ImGui::Separator();
        ImGui::Dummy({1.f, 4.f});

        // ── Custom duel options ──────────────────────────────────────────────
        UIStyle::SectionHeader("Options");
        ImGui::TextDisabled("Starting LP");
        struct { const char* l; int v; } kLP[] = {
            {"8000", 8000}, {"4000", 4000}, {"16000", 16000} };
        for (int i = 0; i < 3; ++i) {
            if (UIStyle::SegmentedButton(kLP[i].l, m_setupLP == kLP[i].v, true,
                                         {84.f, 26.f}))
                m_setupLP = kLP[i].v;
            if (i < 2) ImGui::SameLine(0.f, 4.f);
        }
        ImGui::SetNextItemWidth(220.f);
        ImGui::SliderInt("Starting hand", &m_setupHand, 1, 7);
        ImGui::Checkbox("No shuffle (deck plays in order)", &m_setupNoShuffle);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw your deck top-down — for testing exact draws");
        ImGui::Checkbox("Goldfish — passive opponent (does nothing)",
                        &m_setupPassiveAI);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The opponent just passes, so you can practise combos");
        ImGui::Checkbox("Best of 3 match (with side decking)", &m_setupMatchMode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Play a 3-game match; adjust your deck from your Side "
                              "Deck between games");

        ImGui::Dummy({1.f, 8.f});
        bool canStart = (m_deck0Path[0] != '\0') &&
                        (m_deck1Path[0] != '\0' || m_opponentPreset != -1);
        // Primary "Start Duel" — gold gradient button via UIStyle.
        if (!canStart) ImGui::BeginDisabled();
        if (UIStyle::PrimaryButton("Start Duel", {280.f, 42.f})) {
            // Resolve the opponent deck: a chosen/random preset overrides P2.
            std::string oppPath = m_deck1Path;
            std::string oppName;
            if (m_opponentPreset == 0 && !m_presetFiles.empty()) {
                std::random_device rd;
                size_t r = rd() % m_presetFiles.size();
                oppPath = "assets/decks/presets/" + m_presetFiles[r];
                oppName = presetLabel(m_presetFiles[r]);
            } else if (m_opponentPreset >= 1 &&
                       m_opponentPreset - 1 < (int)m_presetFiles.size()) {
                oppPath = "assets/decks/presets/" +
                          m_presetFiles[(size_t)m_opponentPreset - 1];
                oppName = presetLabel(m_presetFiles[(size_t)m_opponentPreset - 1]);
            }
            // Apply the custom options before the duel registers its engine.
            m_dm.setNoShuffle(m_setupNoShuffle);
            m_dm.setPassiveAI(m_setupPassiveAI);
            // Deck-legality heads-up (#E) — non-blocking so casual/test decks
            // still play, but the player is told why a deck is non-tournament.
            {
                std::string issue = deckLegality(loadYdk(m_deck0Path));
                if (!issue.empty())
                    pushToast("Heads up — your deck isn't legal: " + issue,
                              IM_COL32(235, 185, 60, 255), 4.0);
            }
            // Best-of-3: store the decks and run a match; otherwise a single duel.
            m_matchActive = m_setupMatchMode;
            bool ok;
            if (m_matchActive) {
                m_matchWins[0] = m_matchWins[1] = 0;
                m_matchGameNo  = 1;
                m_matchPlayerPath = m_deck0Path;
                m_matchOppPath    = oppPath;
                m_matchOppName    = oppName;
                m_matchSiding     = false;
                startMatchGame();
                ok = m_dm.isRunning();
            } else {
                // Coin toss decides who takes the first turn; the helper registers
                // the decks in toss order and wires replay + testing capture.
                ok = startOfflineDuelWithCoinToss(m_deck0Path, oppPath.c_str(),
                                                  (uint32_t)m_setupLP,
                                                  (uint32_t)m_setupHand, 1);
            }
            if (ok) {
                m_screen = Screen::Duel;
                gAudio().play("duel_start");
                if (!oppName.empty())
                    pushToast("Opponent: " + oppName,
                              IM_COL32(200, 180, 255, 255), 2.6);
            } else {
                gAudio().play("error");
            }
            ImGui::CloseCurrentPopup();
        }
        if (!canStart) ImGui::EndDisabled();
        ImGui::SameLine(0.f, 10.f);
        if (UIStyle::GhostButton("Cancel", {140.f, 42.f}))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();   // window padding pushed before BeginPopupModal
}

// ─── Duel — root window + a CONDITIONAL fixed-size side overlay ─────────────
//
// The permanent right "command center" column is gone. The root duel window
// lays out: top bar | log (narrow) | field (wide) | bottom testing bar. A
// fixed-size ImGui overlay window is drawn AFTER the root End() and floats
// over the right edge of the field — but ONLY when there is something for
// it to show (a hovered/selected card, an engine prompt, a zone viewer, the
// duel paused or finished). When the field is "just sitting there", the
// overlay is hidden and the field gets the full freed width.
// Duel keyboard shortcuts. Deliberately limited to SAFE, UI-only actions —
// F1 (help) and Esc (close the top panel). We never bind keys to game
// selections here: an untested mis-fire could submit a wrong response or
// desync a multiplayer duel. Reducing clicks on actual plays is handled by the
// gated "auto-pass" option instead.
void UI::handleDuelHotkeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;   // don't hijack typing (chat, names, search)

    if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
        m_helpOverlayOpen = !m_helpOverlayOpen;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        // Close the top-most open panel first; if nothing else is open, Esc
        // toggles the pause menu (a running, non-finished duel).
        if      (m_pauseMenuOpen)   { m_pauseMenuOpen = false;
                                      m_pauseConfirmSurrender = false; }
        else if (m_zoomCard)        m_zoomCard = 0;
        else if (m_helpOverlayOpen) m_helpOverlayOpen = false;
        else if (m_infoCtxCode)     m_infoCtxCode = 0;
        else if (m_viewerLoc != 0)  m_viewerLoc = 0;
        else if (m_toolsDrawerOpen) m_toolsDrawerOpen = false;
        else if (m_dm.isRunning() && !m_dm.isDone())
            m_pauseMenuOpen = true;
    }

    // Ctrl+Z — undo your last move (offline practice; testingUndoHuman self-
    // gates on offline + rewind availability).
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) &&
        ImGui::IsKeyPressed(ImGuiKey_Z, false))
        testingUndoHuman();

    // ── Prompt shortcuts — OFFLINE ONLY ──────────────────────────────────
    // Bound to game decisions, so restricted to offline practice duels where a
    // mis-press only affects a local, rewindable duel — never multiplayer
    // (where a wrong/duplicate response could desync) or replay playback.
    if (!(m_net.isOffline() && !m_replayMode &&
          m_dm.isRunning() && !m_dm.isDone()))
        return;
    const SelectionRequest& sel = m_dm.selection();
    if (!DuelManager::isRealSelect(sel.type)) return;
    if (sel.player != m_dm.humanSeat()) return;   // only the human's own prompt

    switch (sel.type) {
        case WaitType::SelectYesNo:
        case WaitType::SelectEffectYn:
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
                submitMpChoice(sel.type, 1);          // Yes / activate
            else if (ImGui::IsKeyPressed(ImGuiKey_N, false))
                submitMpChoice(sel.type, 0);          // No / decline
            // Right-click anywhere = No, for fast declines. Skip when the same
            // click just pinned a card zoom (right-click-to-zoom still works);
            // m_zoomCard is set earlier this frame by the field/hand renderers.
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                     m_zoomCard == 0)
                submitMpChoice(sel.type, 0);          // No / decline
            break;
        case WaitType::SelectOption: {
            int n = (int)sel.options.size();
            for (int k = 0; k < n && k < 9; ++k) {
                if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + k), false)) {
                    submitMpChoice(WaitType::SelectOption, k);
                    break;
                }
            }
            break;
        }
        // ── Turn / phase control ────────────────────────────────────────────
        // Only the engine-permitted transitions fire (toBP / toM2 / toEP), so a
        // hotkey can never submit an illegal command. submitIdleCmd matches the
        // bottom-bar buttons: 6=to Battle, 7=End Turn, 2=to Main 2, 3=End BP.
        case WaitType::SelectIdleCmd: {
            bool space = ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
            if (sel.toBP && ImGui::IsKeyPressed(ImGuiKey_B, false))
                submitIdleCmd(6, 0, "Battle Phase");
            else if (sel.toEP && ImGui::IsKeyPressed(ImGuiKey_E, false))
                submitIdleCmd(7, 0, "End Turn");
            else if (space) {                      // advance to the next phase
                if (sel.toBP)      submitIdleCmd(6, 0, "Battle Phase");
                else if (sel.toEP) submitIdleCmd(7, 0, "End Turn");
            }
            break;
        }
        case WaitType::SelectBattleCmd: {
            bool space = ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
            if (sel.toEP && ImGui::IsKeyPressed(ImGuiKey_E, false))
                submitIdleCmd(3, 0, "End Battle Phase");
            else if (sel.toM2 && ImGui::IsKeyPressed(ImGuiKey_M, false))
                submitIdleCmd(2, 0, "Main Phase 2");
            else if (space) {                      // advance out of the Battle Phase
                if (sel.toM2)      submitIdleCmd(2, 0, "Main Phase 2");
                else if (sel.toEP) submitIdleCmd(3, 0, "End Battle Phase");
            }
            break;
        }
        default: break;   // cards / chains stay click-only (index ambiguity)
    }
}

// Right-click "zoom": a pinned, readable card — big art + full effect text.
void UI::drawCardZoom(int w, int h) {
    if (!m_zoomCard) return;
    const auto& C = UIStyle::C();
    CardInfo ci = m_db.getCard(m_zoomCard);

    // Dim the field behind the reader.
    UIStyle::DrawModalBackdrop(ImGui::GetBackgroundDrawList(),
                               {0.f, 0.f}, {(float)w, (float)h});

    const float imgW = 280.f, imgH = imgW * 1.45f;
    const float PW = 660.f, PH = imgH + 96.f;
    ImGui::SetNextWindowPos({w * 0.5f, h * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({PW, PH});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(15, 19, 29, 252));
    ImGui::PushStyleColor(ImGuiCol_Border,   C.accent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2{16.f, 14.f});
    ImGui::Begin("##card_zoom", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings);

    void* tex = m_rend.getCardTexture(m_zoomCard);
    if (tex) ImGui::Image(tex, {imgW, imgH});
    else     ImGui::Dummy({imgW, imgH});
    ImGui::SameLine(0.f, 16.f);

    ImGui::BeginGroup();
    const float colW = PW - imgW - 50.f;
    if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + colW);
    ImGui::TextWrapped("%s", ci.name.empty()
                       ? ("#" + std::to_string(m_zoomCard)).c_str()
                       : ci.name.c_str());
    ImGui::PopTextWrapPos();
    if (UIStyle::fHeader) ImGui::PopFont();

    char typeLine[96] = {0};
    if (ci.type & TYPE_MONSTER) {
        if (ci.type & 0x4000000)
            snprintf(typeLine, sizeof(typeLine), "Monster   ATK %d / LINK-%d",
                     ci.atk, (int)ci.def);
        else
            snprintf(typeLine, sizeof(typeLine), "Monster   Lv %u   ATK %d / DEF %d",
                     ci.level, ci.atk, ci.def);
    } else if (ci.type & TYPE_SPELL) snprintf(typeLine, sizeof(typeLine), "Spell");
    else if (ci.type & TYPE_TRAP)    snprintf(typeLine, sizeof(typeLine), "Trap");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(C.accentHi), "%s", typeLine);
    UIStyle::DrawDivider(4.f, 4.f);

    ImGui::BeginChild("##zoom_text", {colW, imgH - 64.f}, false);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextWrapped("%s", ci.desc.empty() ? "(no card text)" : ci.desc.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::Spacing();
    if (UIStyle::GhostButton("Close  (Esc)", {-1.f, 30.f}))
        m_zoomCard = 0;
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void UI::drawHelpOverlay(int w, int h) {
    if (!m_helpOverlayOpen) return;
    const auto& C = UIStyle::C();
    ImGui::SetNextWindowPos({w * 0.5f, h * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({460.f, 0.f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(18, 22, 32, 246));
    ImGui::PushStyleColor(ImGuiCol_Border,   C.accent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{18.f, 14.f});
    ImGui::Begin("##help_overlay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings);
    UIStyle::SectionHeader("Controls & Shortcuts");
    auto row = [&](const char* key, const char* desc) {
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(C.accentHi));
        ImGui::TextUnformatted(key);
        ImGui::PopStyleColor();
        ImGui::SameLine(150.f);
        ImGui::TextDisabled("%s", desc);
    };
    row("Left-click",  "Play a card / pick an action or option");
    row("Right-click", "Zoom a card — big art + full text (Esc to close)");
    row("Hover",       "Preview a card + its text");
    row("Ctrl+Z",      "Undo your last move (offline practice)");
    row("F1",          "Toggle this help");
    row("F11",         "Toggle fullscreen");
    row("Esc",         "Close the open panel (help / viewer / info / tools)");
    ImGui::Dummy({1.f, 4.f});
    UIStyle::Subtle("Offline prompts");
    row("1 - 9",       "Pick option N when choosing an effect");
    row("Y / Enter",   "Yes / activate on a Yes-No prompt");
    row("N",           "No / decline on a Yes-No prompt");
    ImGui::Dummy({1.f, 4.f});
    UIStyle::Subtle("Turn control (offline)");
    row("Space",       "Advance to the next phase");
    row("B",           "Go to Battle Phase");
    row("M",           "Go to Main Phase 2");
    row("E",           "End Turn / End Battle Phase");
    ImGui::Dummy({1.f, 6.f});
    ImGui::TextDisabled("Tip: the bottom bar shows whose turn it is and the");
    ImGui::TextDisabled("phase; the \"Fast\" button skips phase pauses.");
    ImGui::Dummy({1.f, 10.f});
    if (UIStyle::PrimaryButton("Close", {-1.f, 30.f}))
        m_helpOverlayOpen = false;
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void UI::drawDuel(int w, int h) {
    // ── computeDuelLayout — the duel screen's named rects ───────────────
    // Exactly THREE layout bands plus floating overlays; nothing else may
    // reserve width or height:
    //   windowRect        = (0, 0, w, h)
    //   topHudRect        = (0, 0, w, TOP_H)            turn/phase/badges
    //   arenaRect         = (0, TOP_H, w, h - ACT_H)    field child, FULL w
    //   bottomCommandRect = (0, h - ACT_H, w, h)        hints/Pass/phases
    //   rightInfoPanelRect= floating glass overlay at (w-INFO_W-16, TOP_H+12)
    //                       — reserves NO layout width, never shifts the
    //                       arena. Collapses on narrow windows.
    //   log / tools       = floating drawers toggled from the command bar;
    //                       they reserve no layout space either.
    //   visibleFieldRect / handRects / lpPanelRects are derived inside
    //   drawField from the zone grid and cached in the m_rect* members.
    // The old permanent bottom "testing bar" strip is GONE — its 28 px now
    // belong to the arena, and Log/Tools live as ghost buttons in the
    // command bar's right corner.
    const float TOP_H       = 54.f;
    const float ACT_H       = 56.f;
    const float MID_H       = (float)h - TOP_H - ACT_H;     // arena height
    const float INFO_W      = ((float)w >= 1100.f) ? 330.f : 0.f;
    const float FLD_W       = (float)w;

    // One-shot layout audit (re-logged when the geometry changes).
    {
        static int   s_lw = -1, s_lh = -1;
        if (s_lw != w || s_lh != h) {
            s_lw = w; s_lh = h;
            m_dm.logEvent("[DUEL LAYOUT] window=" + std::to_string(w) + "," +
                          std::to_string(h) +
                          "  rightPanel=overlay," +
                          std::to_string((int)INFO_W) + "px" +
                          "  fieldArea=0," + std::to_string((int)TOP_H) +
                          "," + std::to_string((int)FLD_W) +
                          "," + std::to_string((int)MID_H) +
                          "  (field spans FULL window; arena centres on "
                          "window centre)");
        }
    }

    // Per-phase engine pacing — applied ONLY to offline live duels so phases
    // are visible and end-of-phase effects aren't blown past. Disabled in
    // multiplayer (would delay snapshots for both peers), replay playback, and
    // during a Testing-Mode rebuild (must run instantly). Synced every frame
    // so mode changes take effect immediately.
    // Game-speed presets (Relaxed / Normal / Fast) drive the per-event "read
    // beat" and phase pacing so the player can follow each summon/activation.
    //                                    Relaxed  Normal  Fast
    const double kBeat[3]  = { 1.30,  1.00,  0.60 };
    const double kPhase[3] = { 1.30,  0.80,  0.35 };
    int gs = (m_settings.gameSpeed >= 0 && m_settings.gameSpeed <= 2)
                 ? m_settings.gameSpeed : 0;
    bool pace = m_net.isOffline() && !m_replayMode && !m_testingRebuilding &&
                !m_settings.fastTurns;
    // Board-break puzzles: the opponent's preset board takes turn 1 and does
    // nothing, so skip ALL pacing while it's not your turn — you land on your
    // own turn instantly instead of sitting through an empty opponent turn.
    bool skipOppTurn = m_puzzleMode &&
        (int)m_dm.field().turnPlayer != m_dm.humanSeat();
    if (skipOppTurn) pace = false;
    m_dm.setPhaseDelay(pace ? kPhase[gs] : 0.0);
    // AI pacing — DECOUPLED from the phase slider / Fast-turns / game-speed so a
    // visible 1-second-class beat between every AI action ALWAYS applies in an
    // offline live duel (only a Testing rewind/replay/puzzle-skip turns it off).
    // The DuelManager gates its per-action hold on this combo-beat value, so a
    // non-zero value here guarantees the hold fires.
    bool aiPace = m_net.isOffline() && !m_replayMode && !m_testingRebuilding &&
                  !skipOppTurn;
    m_dm.setAiComboBeat(aiPace ? 1.3 : 0.0);

    // Opponent-action notifications: toast each new summon / activation /
    // attack the opponent makes, so the player can follow the game state even
    // when they have no response to give. One toast per action (seq-guarded).
    if (m_dm.lastActionSeq() != m_uiLastActionSeq) {
        m_uiLastActionSeq = m_dm.lastActionSeq();
        if (m_dm.lastActionPlayer() != (uint8_t)m_net.localPlayerIndex() &&
            !m_dm.lastActionDesc().empty()) {
            pushToast("Opponent: " + m_dm.lastActionDesc(),
                      IM_COL32(255, 178, 92, 235), 2.6);
        }
    }

    // Replay playback — auto-feed recorded responses BEFORE any input UI
    // runs this frame. The feeder is a no-op outside replay mode.
    feedReplayTick();

    // While in replay mode, disable every live interactive widget so the
    // user can't accidentally send a response that would diverge from
    // what's recorded. BeginDisabled / EndDisabled propagate through child
    // windows and gates ImGui::Button / ImGui::InvisibleButton (which the
    // custom UIStyle buttons + card-zone hit-testers both use under the
    // hood). The replay controls overlay is drawn AFTER EndDisabled so
    // those buttons remain clickable.
    // Lock live input when:
    //   (a) we're in replay mode (recorded bytes are authoritative), OR
    //   (b) we're in a multiplayer duel and the current engine prompt is
    //       NOT for the local player — only the prompt owner can answer.
    bool mpRemoteTurn = (m_mpInDuel && !m_net.isOffline() &&
                         !isLocalPromptOwner());
    // Desync pause: when the prompt-state handshake reports a mismatch
    // we lock input on BOTH peers — neither side can advance until the
    // user explicitly bails out via the diagnostic modal. Any input
    // beyond this point could compound the divergence.
    bool mpDesyncPause = (m_mpInDuel && !m_net.isOffline() && m_mpDesynced);
    bool inputLocked  = m_replayMode || mpRemoteTurn || mpDesyncPause;
    // One-shot debug logs on the frame the gating decision changes so the
    // user can audit prompt ownership without log spam every frame.
    // The host-auth client ALWAYS logs these (not just with Debug Log on):
    // [CLIENT PROMPT GATE] is the primary diagnostic for "owner=me but
    // input disabled" failures and only fires on state transitions.
    if ((m_debugLog || (m_mpHostAuth && m_net.isClient())) &&
        m_mpInDuel && !m_net.isOffline()) {
        const SelectionRequest& selDbg = currentSelection();
        static int s_lastPromptType  = -99;
        static int s_lastPromptOwner = -99;
        static bool s_lastGated      = false;
        int curType  = (int)selDbg.type;
        int curOwner = DuelManager::isRealSelect(selDbg.type)
                       ? (int)selDbg.player : -1;
        if (curType != s_lastPromptType || curOwner != s_lastPromptOwner ||
            mpRemoteTurn != s_lastGated) {
            m_dm.logEvent(std::string("[MULTI PROMPT] waitType=") +
                          std::to_string(curType) +
                          "  promptOwner=" + std::to_string(curOwner) +
                          "  localPlayer=" +
                          std::to_string(m_net.localPlayerIndex()) +
                          "  isLocalOwner=" +
                          (isLocalPromptOwner() ? "yes" : "no") +
                          (mpRemoteTurn ? "  (input gated)" : ""));
            // Host-authoritative client gets a dedicated gate line so
            // the failure case "owner=me but disabled=yes" is easy to
            // spot in the log.
            if (m_mpHostAuth && m_net.isClient()) {
                m_dm.logEvent(std::string("[CLIENT PROMPT GATE]"
                              "  owner=") + std::to_string(curOwner) +
                              "  localPlayer=" +
                              std::to_string(m_net.localPlayerIndex()) +
                              "  isLocalOwner=" +
                              (isLocalPromptOwner() ? "yes" : "no") +
                              "  waitType=" + std::to_string(curType) +
                              "  disabled=" +
                              (inputLocked ? "yes" : "no"));
            }
            if (mpRemoteTurn) {
                m_dm.logEvent(std::string("[MULTI INPUT GATED] reason=remote-prompt"
                              "  promptOwner=") + std::to_string(curOwner) +
                              "  localPlayer=" +
                              std::to_string(m_net.localPlayerIndex()) +
                              "  waitType=" + std::to_string(curType));
            }
            s_lastPromptType  = curType;
            s_lastPromptOwner = curOwner;
            s_lastGated       = mpRemoteTurn;
        }
    }
    if (inputLocked) ImGui::BeginDisabled();

    auto& selNow = currentSelection();
    // When the engine asks for placement, auto-close any open zone viewer so
    // the field is fully visible and the glowing tiles are unobstructed.
    if (selNow.type == WaitType::SelectPlace && m_viewerLoc != 0) {
        m_viewerLoc = 0;
        m_viewerExtraCache.clear();
    }
    // Fire victory / defeat SFX exactly ONCE when the duel resolves.
    if (m_dm.isDone() && !m_endGameSfxFired) {
        m_endGameSfxFired = true;
        int w = m_dm.winner();
        gAudio().play(w == 0 ? "victory" :
                      w == 1 ? "defeat"  : "confirm");
        // Player-facing end-of-duel line + toast.
        ImU32 col = (w == 0) ? IM_COL32(110, 220, 140, 255)
                  : (w == 1) ? IM_COL32(232, 110, 100, 255)
                             : IM_COL32(232, 232, 232, 255);
        const char* line = (w == 0) ? "Player 1 wins the duel."
                         : (w == 1) ? "Player 2 wins the duel."
                                    : "The duel ended in a draw.";
        pushGameLog(line, col);
        pushToast(w == 0 ? "Victory!" : w == 1 ? "Defeat" : "Draw",
                  col, 3.0);
        // Auto-save the replay (if enabled) at the natural end of the duel.
        // finalizeReplay no-ops on the second call so manual Save Replay +
        // auto-save don't double-write.
        finalizeReplay(w == 0 ? "P1 victory" :
                        w == 1 ? "P2 victory" : "draw");
        // Log the result to match history (offline real duels only — not
        // replays or Testing-Mode rebuilds). The human is always the P1-deck
        // side, so w==0 is a win for them.
        if (m_net.isOffline() && !m_replayMode && !m_testingRebuilding) {
            auto base = [](const char* p) -> std::string {
                std::string s = p ? p : "";
                auto sl = s.find_last_of("/\\");
                if (sl != std::string::npos) s = s.substr(sl + 1);
                if (s.size() > 4 && s.substr(s.size() - 4) == ".ydk")
                    s = s.substr(0, s.size() - 4);
                return s.empty() ? std::string("Deck") : s;
            };
            recordMatch(base(m_deck0Path), base(m_deck1Path),
                        w == 0 ? 'W' : w == 1 ? 'L' : 'D');
        }
        // Multiplayer duel resolved naturally — drop the in-duel flag so
        // returning to lobby / multiplayer screen behaves correctly, and
        // clear the auto-resolve suppression so the next live duel keeps
        // the offline 0-option auto-pass UX.
        m_mpInDuel = false;
        m_dm.setSuppressAutoResolve(false);
        resetMpResponseState();
    } else if (!m_dm.isDone() && m_endGameSfxFired) {
        m_endGameSfxFired = false;       // reset for next duel
    }

    // ── Field-state delta observer for in-game SFX + animations ───────────
    // Compares this frame's field counters to the previous frame and queues
    // one sound + one animation per observed change. Purely presentation —
    // never touches the engine. Animations use cached zone rects from the
    // previous frame's drawField paint; the first-frame "init" branch seeds
    // counters and skips animations so we never play a "everything moved at
    // once" burst at duel start.
    {
        const FieldState& fobs = currentField();
        auto& f = fobs;

        // Animation palette — kept inline to avoid pulling UIStyle::C() into
        // this block. Tuned to read on the dark playmat.
        const ImU32 kColDamage = IM_COL32(232,  82,  82, 255);
        const ImU32 kColDraw   = IM_COL32(255, 214, 116, 255);
        const ImU32 kColGY     = IM_COL32(170, 184, 208, 255);
        const ImU32 kColBN     = IM_COL32(204, 132, 232, 255);
        const ImU32 kColSummon = IM_COL32(255, 214, 108, 255);   // gold
        const ImU32 kColSpec   = IM_COL32(112, 220, 255, 255);   // cyan

        // Cached previous-frame zone codes per monster zone, used to find
        // which exact slot a new monster appeared in. We rebuild the
        // "current" view from f.monsters each frame and diff.
        uint32_t curMZ[2][7] = {{0}};
        for (int p = 0; p < 2; ++p)
            for (const CardState& cs : f.monsters[p])
                if (cs.seq < 7) curMZ[p][cs.seq] = cs.code;

        if (!m_sfxObsInited && m_dm.isRunning()) {
            // First frame after a duel starts: seed prev state + sting.
            // No animations on the seed frame — counters were "0 vs N"
            // moments ago and we don't want to fire a summon ring per card.
            for (int p = 0; p < 2; ++p) {
                m_sfxPrevLP[p]   = f.lp[p];
                m_lpShown[p]     = (float)f.lp[p];   // snap animated LP to start
                m_lpGhost[p]     = (float)f.lp[p];
                m_sfxPrevHand[p] = (int)f.hand[p].size();
                m_sfxPrevGY[p]   = (int)f.gy[p].size();
                m_sfxPrevBN[p]   = (int)f.banished[p].size();
                m_sfxPrevMon[p]  = (int)f.monsters[p].size();
                for (int z = 0; z < 7; ++z)
                    m_sfxPrevMZcode[p][z] = curMZ[p][z];
            }
            // Skip the "duel started" sting/toast when this seed frame is a
            // Testing-Mode rebuild re-seed (we already showed a "Restored to"
            // toast) — otherwise every rewind would re-announce the duel.
            if (!m_testingJustRestored) {
                gAudio().play("duel_start");
                pushGameAndReplay("Duel started. Good luck!",
                            IM_COL32(255, 214, 108, 255));
                pushToast  ("Duel started",
                            IM_COL32(255, 214, 108, 255), 2.0);
            }
            m_testingJustRestored = false;
            m_sfxObsInited = true;
        } else if (m_sfxObsInited && m_dm.isRunning()) {
            // Damage / heal: LP changed this frame.
            const ImU32 kColHeal = IM_COL32(110, 220, 140, 255);
            for (int p = 0; p < 2; ++p) {
                if (f.lp[p] != m_sfxPrevLP[p]) {
                    int delta = (int)f.lp[p] - (int)m_sfxPrevLP[p];
                    m_dm.logEvent(
                        std::string("[DAMAGE EVENT] player P") +
                        std::to_string(p + 1) +
                        "  oldLP=" + std::to_string(m_sfxPrevLP[p]) +
                        "  newLP=" + std::to_string(f.lp[p]) +
                        "  delta=" +
                        (delta > 0 ? "+" + std::to_string(delta)
                                   : std::to_string(delta)));
                    if (delta < 0) {
                        gAudio().play("damage");
                        // Player-facing damage line.
                        pushGameLog(
                            "Player " + std::to_string(p + 1) +
                            " took " + std::to_string(-delta) + " damage.",
                            IM_COL32(232, 110, 100, 255));
                        if (m_zoneRectsReady) {
                            m_anim.lpFlash(m_rectLP_tl[p], m_rectLP_br[p],
                                           kColDamage, 0.55);
                            ImVec2 c{
                                (m_rectLP_tl[p].x + m_rectLP_br[p].x) * 0.5f,
                                (m_rectLP_tl[p].y + m_rectLP_br[p].y) * 0.5f };
                            m_anim.damageNum(c, -delta, kColDamage, 1.15);
                        }
                    } else if (delta > 0) {
                        // Healing — green flash + "+NNN" number popup.
                        pushGameLog(
                            "Player " + std::to_string(p + 1) +
                            " was healed for " + std::to_string(delta) + ".",
                            IM_COL32(110, 220, 140, 255));
                        if (m_zoneRectsReady) {
                            m_anim.lpFlash(m_rectLP_tl[p], m_rectLP_br[p],
                                           kColHeal, 0.55);
                            ImVec2 c{
                                (m_rectLP_tl[p].x + m_rectLP_br[p].x) * 0.5f,
                                (m_rectLP_tl[p].y + m_rectLP_br[p].y) * 0.5f };
                            char buf[16];
                            snprintf(buf, sizeof(buf), "+%d", delta);
                            m_anim.floatText(c, buf, kColHeal, 1.15);
                            m_anim.ring(c, 36.f, kColHeal, 0.55);
                        }
                    }
                    if (delta < 0) break;   // only one damage SFX per frame
                }
            }
            // Draw: hand went up.
            for (int p = 0; p < 2; ++p) {
                int drawn = (int)f.hand[p].size() - m_sfxPrevHand[p];
                if (drawn > 0) {
                    gAudio().play("draw");
                    // Highlight your freshly drawn cards (#B).
                    if (p == m_net.localPlayerIndex()) {
                        m_newDrawAt    = ImGui::GetTime();
                        m_newDrawCount = drawn;
                    }
                    pushGameLog(
                        "Player " + std::to_string(p + 1) +
                        " drew " + std::to_string(drawn) +
                        (drawn == 1 ? " card." : " cards."));
                    if (m_zoneRectsReady) {
                        ImVec2 c{
                            (m_rectDeck_tl[p].x + m_rectDeck_br[p].x) * 0.5f,
                            (m_rectDeck_tl[p].y + m_rectDeck_br[p].y) * 0.5f
                        };
                        m_anim.pulse(c, 34.f, kColDraw, 0.50);
                    }
                }
            }
            // Sent to GY: gy count rose.
            for (int p = 0; p < 2; ++p) {
                int sent = (int)f.gy[p].size() - m_sfxPrevGY[p];
                if (sent > 0) {
                    gAudio().play("send_gy");
                    // Best-effort name lookup — use the LAST entry in the
                    // GY (most recently added). If we can't resolve a name
                    // gracefully fall back to a safe generic line.
                    std::string nm;
                    if (!f.gy[p].empty())
                        nm = m_db.getCard(f.gy[p].back().code).name;
                    if (!nm.empty() && sent == 1)
                        pushGameLog(nm + " was sent to Player " +
                                    std::to_string(p + 1) + "'s Graveyard.");
                    else
                        pushGameLog(std::to_string(sent) +
                                    (sent == 1 ? " card was" : " cards were") +
                                    " sent to Player " +
                                    std::to_string(p + 1) + "'s Graveyard.");
                    if (m_zoneRectsReady) {
                        ImVec2 c{
                            (m_rectGY_tl[p].x + m_rectGY_br[p].x) * 0.5f,
                            (m_rectGY_tl[p].y + m_rectGY_br[p].y) * 0.5f
                        };
                        m_anim.pulse(c, 30.f, kColGY, 0.45);
                    }
                }
            }
            // Banish: banished count rose.
            for (int p = 0; p < 2; ++p) {
                int banished = (int)f.banished[p].size() - m_sfxPrevBN[p];
                if (banished > 0) {
                    gAudio().play("banish");
                    std::string nm;
                    if (!f.banished[p].empty())
                        nm = m_db.getCard(f.banished[p].back().code).name;
                    if (!nm.empty() && banished == 1)
                        pushGameLog(nm + " was banished from Player " +
                                    std::to_string(p + 1) + ".",
                                    IM_COL32(204, 132, 232, 255));
                    else
                        pushGameLog(std::to_string(banished) +
                                    (banished == 1 ? " card was" : " cards were") +
                                    " banished from Player " +
                                    std::to_string(p + 1) + ".",
                                    IM_COL32(204, 132, 232, 255));
                    if (m_zoneRectsReady) {
                        ImVec2 c{
                            (m_rectBN_tl[p].x + m_rectBN_br[p].x) * 0.5f,
                            (m_rectBN_tl[p].y + m_rectBN_br[p].y) * 0.5f
                        };
                        m_anim.ring(c, 32.f, kColBN, 0.50);
                    }
                }
            }
            // Monster appeared on a player's field: best-effort summon SFX.
            // Suppress SFX when the user's click already played a summon-
            // class sound recently — animations always fire so the user
            // sees the zone respond regardless of cooldown.
            const double tobs = ImGui::GetTime();
            const double kSummonGuardSec = 0.40;
            bool summonGuardActive =
                (m_lastSummonSfxAt > 0.0 &&
                 (tobs - m_lastSummonSfxAt) < kSummonGuardSec);
            for (int p = 0; p < 2; ++p) {
                if ((int)f.monsters[p].size() > m_sfxPrevMon[p]
                    && !summonGuardActive) {
                    int monDelta  = (int)f.monsters[p].size() - m_sfxPrevMon[p];
                    int handDelta = m_sfxPrevHand[p] - (int)f.hand[p].size();
                    bool isNormal = (handDelta >= monDelta && monDelta > 0);
                    if (isNormal) gAudio().play("summon");
                    else          gAudio().play("special_summon");
                }
                // Per-zone animation — fires regardless of the SFX guard so
                // the visual feedback is always present.
                if (m_zoneRectsReady) {
                    for (int z = 0; z < 7; ++z) {
                        if (m_sfxPrevMZcode[p][z] == 0 && curMZ[p][z] != 0) {
                            ImVec2 tl = m_rectMZ_tl[p][z];
                            ImVec2 br = m_rectMZ_br[p][z];
                            ImVec2 c{ (tl.x + br.x) * 0.5f,
                                      (tl.y + br.y) * 0.5f };
                            int monDelta  = (int)f.monsters[p].size()
                                          - m_sfxPrevMon[p];
                            int handDelta = m_sfxPrevHand[p]
                                          - (int)f.hand[p].size();
                            bool isNormal =
                                (handDelta >= monDelta && monDelta > 0);
                            ImU32 col = isNormal ? kColSummon : kColSpec;
                            m_anim.zoneFlash(tl, br, col, 0.45);
                            m_anim.ring(c, isNormal ? 42.f : 58.f,
                                        col, isNormal ? 0.55 : 0.70);
                            // (Boss-entrance + phase banners live in the
                            // mode-agnostic presentation observer below, so
                            // they also play on the host-auth MP client.)
                            // Player-facing summon line — best-effort
                            // identification using the new code at this seq.
                            std::string nm = m_db.getCard(curMZ[p][z]).name;
                            std::string verb = isNormal
                                ? " Normal Summoned "
                                : " Special Summoned ";
                            if (!nm.empty())
                                pushGameLog("Player " + std::to_string(p + 1) +
                                            verb + nm + ".", col);
                            else
                                pushGameLog("Player " + std::to_string(p + 1) +
                                            verb + "a monster.", col);
                        }
                    }
                }
            }
            // Snapshot for next frame.
            for (int p = 0; p < 2; ++p) {
                m_sfxPrevLP[p]   = f.lp[p];
                m_lpShown[p]     = (float)f.lp[p];   // snap animated LP to start
                m_lpGhost[p]     = (float)f.lp[p];
                m_sfxPrevHand[p] = (int)f.hand[p].size();
                m_sfxPrevGY[p]   = (int)f.gy[p].size();
                m_sfxPrevBN[p]   = (int)f.banished[p].size();
                m_sfxPrevMon[p]  = (int)f.monsters[p].size();
                for (int z = 0; z < 7; ++z)
                    m_sfxPrevMZcode[p][z] = curMZ[p][z];
            }
        } else if (!m_dm.isRunning() && m_sfxObsInited) {
            m_sfxObsInited = false;        // reset for next duel
            m_anim.clear();                // drop stale animations
            m_zoneRectsReady = false;
        }
    }

    // ── Presentation observer (mode-agnostic — offline / host / replay /
    //    host-auth client) ─────────────────────────────────────────────────
    // Reads currentField() (snapshot-backed on the client) so phase banners
    // and boss entrances play on BOTH peers without the client needing a
    // local ocgcore. Purely cosmetic — never touches engine or network state.
    if (isDuelVisiblyRunning() && !m_dm.isDone()) {
        const FieldState& pf = currentField();
        // Current monster-zone occupancy (codes by seq) for the boss diff.
        uint32_t pbMZ[2][7] = {{0}};
        for (int p = 0; p < 2; ++p)
            for (const CardState& cs : pf.monsters[p])
                if (cs.seq < 7) pbMZ[p][cs.seq] = cs.code;

        if (!m_bossObsInited) {
            // Seed the BOSS diff + turn tracker, but deliberately leave
            // m_animObservedPhase = 0xFFFF and m_animLastEnqueued = 0 so the
            // FIRST observed phase triggers the Draw → Standby → Main 1
            // banner fill for turn 1 (otherwise turn 1's banners are missed
            // because the engine is already in Main Phase 1 by the first
            // rendered frame).
            m_animPrevTurnPlayer = pf.turnPlayer;
            m_phaseQueue.clear();
            for (int p = 0; p < 2; ++p)
                for (int z = 0; z < 7; ++z) m_bossPrevMZ[p][z] = pbMZ[p][z];
            m_bossObsInited = true;
        } else {
            // Phase banners — enqueue any phase transition (including the
            // Draw/Standby the engine skips through instantly), then pace
            // them out below so none get visually skipped.
            observePhaseForBanners();
            // Boss entrance — a high-impact monster newly occupying a zone.
            for (int p = 0; p < 2; ++p) {
                for (int z = 0; z < 7; ++z) {
                    uint32_t code = pbMZ[p][z];
                    if (code != 0 && m_bossPrevMZ[p][z] != code) {
                        CardInfo bci = m_db.getCard(code);
                        if (isBossCard(bci) && m_zoneRectsReady) {
                            ImVec2 tl = m_rectMZ_tl[p][z], br = m_rectMZ_br[p][z];
                            ImVec2 c{ (tl.x+br.x)*0.5f, (tl.y+br.y)*0.5f };
                            ImVec2 winSz = ImGui::GetIO().DisplaySize;
                            // Infer special vs normal: Extra-Deck monster or a
                            // monster appearing without a matching hand drop is
                            // "special"; otherwise normal. Good enough for the
                            // label/colour choice.
                            bool special = isExtraDeckType(bci.type);
                            ImU32 bcol = special ? IM_COL32(112,220,255,255)
                                                 : IM_COL32(255,214,108,255);
                            m_anim.emitBossSummon(m_rend.getCardTexture(code),
                                winSz, c, summonTypeLabel(bci, special), bcol);
                            m_anim.emitShake(8.f, 0.34);
                        }
                    }
                }
            }
            for (int p = 0; p < 2; ++p)
                for (int z = 0; z < 7; ++z) m_bossPrevMZ[p][z] = pbMZ[p][z];
        }
        // Pace the queued phase banners regardless of init state so the very
        // first turn's Draw/Standby still display.
        pumpPhaseBannerQueue();
    } else if (m_bossObsInited && !isDuelVisiblyRunning()) {
        m_bossObsInited = false;
        m_animPrevPhase = 0xFFFF;
        m_animObservedPhase = 0xFFFF;
        m_animLastEnqueued = 0;
        m_animPrevTurnPlayer = 0xFF;
        m_phaseQueue.clear();
    }

    // ── Attack animation hook ─────────────────────────────────────────────
    // Drain MSG_ATTACK events the engine reported since the last frame and
    // schedule a beam + impact-ring + zone-flash for each one. Uses cached
    // zone rects (set by drawField on the previous frame). Direct attacks
    // point at the opponent's LP panel rect. SFX plays once per event; the
    // observer's "damage" branch above already handles LP delta + number,
    // so we don't double-fire damage SFX here.
    //
    // Hold off on draining until rects are ready — otherwise an attack
    // declared on the engine's very first frame would be drained AND
    // silently discarded because we'd have nowhere to anchor the animation.
    if (m_zoneRectsReady && m_dm.isRunning()) {
        auto evts = m_dm.drainAttackEvents();
        if (!evts.empty()) {
            const ImU32 kColAtk    = IM_COL32(255, 110,  80, 255); // hot orange
            const ImU32 kColDirect = IM_COL32(255, 200,  80, 255); // gold-orange
            for (const AttackEvent& ev : evts) {
                ImVec2 aTl, aBr;
                if (!locInfoToRect(ev.attackerCon, ev.attackerLoc,
                                   ev.attackerSeq, &aTl, &aBr)) {
                    // Attacker not on a cached monster zone — fall back to
                    // its LP panel so we still draw *something* visible.
                    if (ev.attackerCon < 2) {
                        aTl = m_rectLP_tl[ev.attackerCon];
                        aBr = m_rectLP_br[ev.attackerCon];
                    } else continue;
                }
                ImVec2 aCtr{ (aTl.x + aBr.x) * 0.5f,
                             (aTl.y + aBr.y) * 0.5f };

                ImVec2 tTl, tBr;
                bool gotTarget = false;
                if (ev.direct) {
                    // Direct attack → opponent LP panel. Opponent of the
                    // attacker is (con ^ 1).
                    uint8_t opp = ev.attackerCon ^ 1u;
                    if (opp < 2) {
                        tTl = m_rectLP_tl[opp];
                        tBr = m_rectLP_br[opp];
                        gotTarget = true;
                    }
                } else if (locInfoToRect(ev.targetCon, ev.targetLoc,
                                         ev.targetSeq, &tTl, &tBr)) {
                    gotTarget = true;
                }
                if (!gotTarget) continue;
                ImVec2 tCtr{ (tTl.x + tBr.x) * 0.5f,
                             (tTl.y + tBr.y) * 0.5f };

                // Brief flash on attacker tile + beam + impact ring/pulse.
                ImU32 col = ev.direct ? kColDirect : kColAtk;
                m_anim.zoneFlash(aTl, aBr, col, 0.35);
                m_anim.beam(aCtr, tCtr, col, 0.35);
                m_anim.ring(tCtr, ev.direct ? 56.f : 40.f, col, 0.55);
                m_anim.pulse(tCtr, ev.direct ? 30.f : 24.f, col, 0.40);

                // Combat math caption — "3000 vs 2500" above the clash, so the
                // outcome reads at a glance. (Direct attacks have no defender.)
                if (!ev.direct) {
                    auto cardAt = [&](uint8_t cn, uint8_t lc, uint32_t sq)->uint32_t{
                        if (cn > 1 || lc != LOC_MZONE) return 0;
                        for (const auto& m : m_dm.field().monsters[cn])
                            if (m.seq == sq) return m.code;
                        return 0;
                    };
                    uint32_t aCode = cardAt(ev.attackerCon, ev.attackerLoc, ev.attackerSeq);
                    uint32_t tCode = cardAt(ev.targetCon, ev.targetLoc, ev.targetSeq);
                    if (aCode && tCode) {
                        CardInfo aci = m_db.getCard(aCode), tci = m_db.getCard(tCode);
                        bool tdef = (ev.targetPos & (POS_FACEUP_DEFENSE |
                                                     POS_FACEDOWN_DEFENSE)) != 0;
                        int tval = tdef ? tci.def : tci.atk;
                        char mathbuf[32];
                        snprintf(mathbuf, sizeof(mathbuf), "%d vs %d", aci.atk, tval);
                        ImVec2 mid{ (aCtr.x + tCtr.x) * 0.5f,
                                    (aCtr.y + tCtr.y) * 0.5f - 20.f };
                        m_anim.floatText(mid, mathbuf, IM_COL32(255, 236, 180, 255),
                                         1.10);
                    }
                }

                // Attack SFX — single hit per event.
                gAudio().play("attack");
            }
        }

        // ── Chain-activation + targeting animations ──────────────────────
        // Resolve any card's (con,loc,seq) to a screen centre — field zones
        // via the cached rects, GY/banished/deck via their pile tiles.
        auto centerOf = [&](uint8_t con, uint8_t loc, uint32_t seq,
                            ImVec2* out) -> bool {
            ImVec2 tl, br;
            if (locInfoToRect(con, loc, seq, &tl, &br)) {
                *out = {(tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f};
                return true;
            }
            if (con > 1) return false;
            auto mid = [](ImVec2 a, ImVec2 b) {
                return ImVec2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}; };
            if (loc == LOC_GY)   { *out = mid(m_rectGY_tl[con], m_rectGY_br[con]); return true; }
            if (loc == LOC_REM)  { *out = mid(m_rectBN_tl[con], m_rectBN_br[con]); return true; }
            if (loc == LOC_DECK || loc == LOC_HAND)
                                 { *out = mid(m_rectDeck_tl[con], m_rectDeck_br[con]); return true; }
            return false;
        };
        // Short "intent verb" derived from a card's effect text, so the chain
        // pop reads "Baronne de Fleur — Negate & Destroy" not just a flash.
        auto effectVerb = [&](uint32_t code) -> std::string {
            std::string d = m_db.getCard(code).desc;
            for (char& ch : d) ch = (char)tolower((unsigned char)ch);
            bool neg = d.find("negate") != std::string::npos;
            bool des = d.find("destroy") != std::string::npos;
            bool ban = d.find("banish") != std::string::npos;
            if (neg && des) return "Negate & Destroy";
            if (neg)        return "Negate";
            if (ban)        return "Banish";
            if (des)        return "Destroy";
            if (d.find("special summon") != std::string::npos) return "Special Summon";
            if (d.find("draw")  != std::string::npos) return "Draw";
            if (d.find("add 1") != std::string::npos ||
                d.find("to your hand") != std::string::npos) return "Search";
            return "";
        };
        if (m_settings.animationsEnabled) {
            auto chains = m_dm.drainChainEvents();
            int cshown = 0;
            bool haveSrc = false; ImVec2 srcCenter{};
            for (const ChainEvent& ce : chains) {
                if (cshown++ >= 4) break;
                ImVec2 c;
                if (!centerOf(ce.con, ce.loc, ce.seq, &c)) continue;
                srcCenter = c; haveSrc = true;   // tether anchor for targets
                CardInfo ci = m_db.getCard(ce.code);
                std::string nm = ci.name.empty()
                    ? ("#" + std::to_string(ce.code)) : ci.name;
                const ImU32 gold = IM_COL32(255, 212, 96, 255);
                ImVec2 tl, br;
                bool onField = locInfoToRect(ce.con, ce.loc, ce.seq, &tl, &br);
                if (onField) m_anim.zoneFlash(tl, br, gold, 0.45);
                // Trap-spring: a set Trap snapping face-up gets a board jolt.
                if ((ci.type & TYPE_TRAP) && onField) {
                    m_anim.emitShake(5.f, 0.25);
                    gAudio().play("set");
                }
                std::string verb = effectVerb(ce.code);
                m_anim.chainPop(c, nm.c_str(), ce.link, gold,
                                verb.empty() ? nullptr : verb.c_str(), 1.25);
                gAudio().play("confirm");
            }
            auto targets = m_dm.drainTargetEvents();
            int tshown = 0;
            for (const TargetEvent& te : targets) {
                if (tshown++ >= 6) break;
                ImVec2 c;
                if (!centerOf(te.con, te.loc, te.seq, &c)) continue;
                // Source → target tether so causality is obvious.
                if (haveSrc)
                    m_anim.beam(srcCenter, c, IM_COL32(255, 150, 90, 255), 0.45);
                m_anim.targetLock(c, IM_COL32(255, 86, 86, 255), 0.85);
            }
            // Negated effects — shatter + "NEGATED" on the negated card.
            for (const NegateEvent& ne : m_dm.drainNegateEvents()) {
                ImVec2 c;
                if (!centerOf(ne.con, ne.loc, ne.seq, &c)) continue;
                m_anim.negate(c, m_db.getCard(ne.code).name.c_str(), 1.05);
                m_anim.emitShake(6.f, 0.28);
                gAudio().play("error");
            }
            // Chain resolving — a brief spotlight pulse per link (LIFO order).
            for (const ResolveEvent& re : m_dm.drainResolveEvents()) {
                ImVec2 c;
                if (!centerOf(re.con, re.loc, re.seq, &c)) continue;
                m_anim.pulse(c, 30.f, IM_COL32(120, 200, 255, 255), 0.45);
            }
            // Counter add/remove — a "+N" / "-N" pop on the card.
            for (const CounterEvent& ce : m_dm.drainCounterEvents()) {
                ImVec2 c;
                if (ce.delta == 0 || !centerOf(ce.con, ce.loc, ce.seq, &c))
                    continue;
                char cb[12];
                snprintf(cb, sizeof(cb), "%+d", ce.delta);
                ImU32 col = ce.delta > 0 ? IM_COL32(120, 200, 255, 255)
                                         : IM_COL32(235, 150, 90, 255);
                m_anim.floatText({c.x, c.y - 14.f}, cb, col, 0.9);
                m_anim.ring(c, 22.f, col, 0.45);
            }
            // Excavate — flip the revealed deck cards up, Master-Duel style.
            for (const ExcavateEvent& ex : m_dm.drainExcavateEvents()) {
                if (ex.codes.empty()) continue;
                m_excavateCards = ex.codes;
                m_excavateAt    = ImGui::GetTime();
                gAudio().play("draw");
            }
        } else {
            m_dm.drainChainEvents();    // keep queues from growing when off
            m_dm.drainTargetEvents();
            m_dm.drainNegateEvents();
            m_dm.drainResolveEvents();
            m_dm.drainCounterEvents();
            m_dm.drainExcavateEvents();
        }

        // ── Card-movement animations ─────────────────────────────────────
        // A card sent to the GY (destroyed / milled / sent) glides from its
        // source to the GY tile; a card placed on the field (summon / set)
        // gets a placement pop. Makes the board feel alive. Capped per frame
        // so a mass mill/destroy doesn't flood the screen.
        {
            auto moves = m_dm.drainMoveEvents();   // always drain to clear queue
            int shown = m_settings.animationsEnabled ? 0 : 6;  // off => emit none
            for (const MoveEvent& mv : moves) {
                if (shown >= 6) break;
                if (mv.prevLoc == mv.curLoc) continue;   // reposition, not a move
                uint8_t cc = mv.curCon & 1, pc = mv.prevCon & 1;

                if (mv.curLoc == LOC_GY) {                // → Graveyard
                    ImVec2 gTl = m_rectGY_tl[cc], gBr = m_rectGY_br[cc];
                    ImVec2 gCtr{ (gTl.x+gBr.x)*0.5f, (gTl.y+gBr.y)*0.5f };
                    ImVec2 sTl, sBr; bool haveSrc = false;
                    if ((mv.prevLoc == LOC_MZONE || mv.prevLoc == LOC_SZONE) &&
                        locInfoToRect(pc, mv.prevLoc, mv.prevSeq, &sTl, &sBr))
                        haveSrc = true;
                    else if (mv.prevLoc == LOC_DECK)
                        { sTl = m_rectDeck_tl[pc]; sBr = m_rectDeck_br[pc]; haveSrc = true; }
                    ImU32 col = IM_COL32(150, 165, 200, 255);   // GY grey-blue
                    if (haveSrc) {
                        ImVec2 sCtr{ (sTl.x+sBr.x)*0.5f, (sTl.y+sBr.y)*0.5f };
                        m_anim.cardTrail(sCtr, gCtr,
                                         m_rend.getCardTexture(mv.code), col, 0.50);
                        m_anim.zoneFlash(sTl, sBr, col, 0.28);
                    }
                    m_anim.ring(gCtr, 32.f, col, 0.50);
                    ++shown;
                }
                else if ((mv.curLoc == LOC_MZONE || mv.curLoc == LOC_SZONE) &&
                         (mv.prevLoc == LOC_HAND || mv.prevLoc == LOC_DECK ||
                          mv.prevLoc == LOC_EXTRA)) {       // → placed on field
                    ImVec2 dTl, dBr;
                    if (locInfoToRect(cc, mv.curLoc, mv.curSeq, &dTl, &dBr)) {
                        ImVec2 dCtr{ (dTl.x+dBr.x)*0.5f, (dTl.y+dBr.y)*0.5f };
                        ImU32 col = IM_COL32(120, 200, 255, 255);  // place cyan
                        // Source point: the deck tile, else a point toward the
                        // controller's hand (below = local, above = opponent).
                        ImVec2 sCtr;
                        if (mv.prevLoc == LOC_DECK) {
                            ImVec2 kTl = m_rectDeck_tl[pc], kBr = m_rectDeck_br[pc];
                            sCtr = { (kTl.x+kBr.x)*0.5f, (kTl.y+kBr.y)*0.5f };
                        } else {
                            bool ctrlBottom =
                                (cc == (uint8_t)m_net.localPlayerIndex());
                            sCtr = { dCtr.x, ctrlBottom ? dBr.y + 210.f
                                                        : dTl.y - 210.f };
                        }
                        // Face-down sets must show the card back, never the art
                        // (would leak an opponent's set card identity).
                        bool faceDown = (mv.curPos & (POS_FACEDOWN_ATTACK |
                                                      POS_FACEDOWN_DEFENSE)) != 0;
                        void* tex = faceDown ? m_rend.getBackTexture()
                                             : m_rend.getCardTexture(mv.code);
                        m_anim.cardTrail(sCtr, dCtr, tex, col, 0.46);
                        m_anim.zoneFlash(dTl, dBr, col, 0.40);
                        m_anim.ring(dCtr, 28.f, col, 0.50);
                        // Summon-type entrance for a face-up monster.
                        if (mv.curLoc == LOC_MZONE && !faceDown) {
                            CardInfo mci = m_db.getCard(mv.code);
                            const char* lbl; ImU32 scol;
                            if      (mci.type & TYPE_LINK)    { lbl="LINK";    scol=IM_COL32(80,150,255,255); }
                            else if (mci.type & TYPE_XYZ)     { lbl="XYZ";     scol=IM_COL32(235,210,90,255); }
                            else if (mci.type & TYPE_SYNCHRO) { lbl="SYNCHRO"; scol=IM_COL32(225,235,235,255); }
                            else if (mci.type & TYPE_FUSION)  { lbl="FUSION";  scol=IM_COL32(190,110,235,255); }
                            else if (mci.type & TYPE_RITUAL)  { lbl="RITUAL";  scol=IM_COL32(120,200,255,255); }
                            else                              { lbl="SUMMON";  scol=IM_COL32(255,212,96,255); }
                            float rr = (dBr.x - dTl.x) * 0.6f;
                            m_anim.summonBurst(dCtr, rr, lbl, scol, 0.85);
                        }
                        ++shown;
                    }
                }
                else if (mv.curLoc == LOC_REM) {          // → Banished
                    ImVec2 bTl = m_rectBN_tl[cc], bBr = m_rectBN_br[cc];
                    ImVec2 bCtr{ (bTl.x+bBr.x)*0.5f, (bTl.y+bBr.y)*0.5f };
                    ImVec2 sTl, sBr;
                    if ((mv.prevLoc == LOC_MZONE || mv.prevLoc == LOC_SZONE) &&
                        locInfoToRect(pc, mv.prevLoc, mv.prevSeq, &sTl, &sBr)) {
                        ImVec2 sCtr{ (sTl.x+sBr.x)*0.5f, (sTl.y+sBr.y)*0.5f };
                        m_anim.vortex(sCtr, 0.75);
                        m_anim.cardTrail(sCtr, bCtr,
                            m_rend.getCardTexture(mv.code),
                            IM_COL32(186,116,240,255), 0.55);
                    }
                    m_anim.vortex(bCtr, 0.75);
                    ++shown;
                }
                else if (mv.prevLoc == LOC_DECK && mv.curLoc == LOC_HAND &&
                         cc == (uint8_t)m_net.localPlayerIndex()) {
                    // Search / add-to-hand (your own) — reveal the card centre-
                    // screen, then shrink toward your hand. Only your side, so an
                    // opponent's private draw is never exposed.
                    ImVec2 hCtr{ (float)w * 0.5f, (float)h - 90.f };
                    ImVec2 center{ (float)w * 0.5f, (float)h * 0.44f };
                    m_anim.reveal(center, hCtr, m_rend.getCardTexture(mv.code),
                                  m_db.getCard(mv.code).name.c_str(), 1.15);
                    ++shown;
                }
            }
        }
    }

    ImGui::SetNextWindowPos({0.f, 0.f});
    ImGui::SetNextWindowSize({(float)w, (float)h});
    ImGui::Begin("##duel", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // ── Top bar background ────────────────────────────────────────────────────
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        ImU32 barBgTop = m_replayMode ? IM_COL32(34, 20, 46, 255)
                                      : IM_COL32(14, 17, 30, 255);
        ImU32 barBgBot = m_replayMode ? IM_COL32(22, 12, 32, 255)
                                      : IM_COL32( 7,  9, 18, 255);
        ImU32 barEdge = m_replayMode ? IM_COL32(180, 110, 220, 220)
                                     : IM_COL32( 70,  86, 140, 210);
        dl->AddRectFilledMultiColor(wp, {wp.x + w, wp.y + TOP_H},
                                    barBgTop, barBgTop, barBgBot, barBgBot);
        dl->AddLine({wp.x, wp.y + TOP_H - 1}, {wp.x + w, wp.y + TOP_H - 1},
                    barEdge);
        // REPLAY badge — positioned LEFT of the top-right Lobby/Exit
        // button so the two don't overlap. ~120 px reserved on the right
        // edge for the button cluster.
        if (m_replayMode) {
            const char* label = "REPLAY MODE";
            ImVec2 ts = ImGui::CalcTextSize(label);
            float bx = wp.x + (float)w - ts.x - 24.f - 130.f;
            float by = wp.y + (TOP_H - ts.y) * 0.5f;
            dl->AddRectFilled({bx - 10.f, by - 4.f},
                              {bx + ts.x + 10.f, by + ts.y + 4.f},
                              IM_COL32(120, 60, 180, 200), 4.f);
            dl->AddRect      ({bx - 10.f, by - 4.f},
                              {bx + ts.x + 10.f, by + ts.y + 4.f},
                              IM_COL32(220, 160, 255, 255), 4.f, 0, 1.2f);
            dl->AddText({bx, by},
                        IM_COL32(240, 220, 255, 255), label);
        }
        // MULTIPLAYER badge — same right-side anchor, different palette.
        // Includes the local player seat (P1 / P2) so the user knows
        // which prompts they're meant to answer.
        if (m_mpInDuel && !m_net.isOffline()) {
            char buf2[64];
            snprintf(buf2, sizeof(buf2),
                     "MULTIPLAYER  ·  you = P%d",
                     m_net.localPlayerIndex() + 1);
            ImVec2 ts = ImGui::CalcTextSize(buf2);
            float bx = wp.x + (float)w - ts.x - 24.f - 130.f;
            float by = wp.y + (TOP_H - ts.y) * 0.5f;
            dl->AddRectFilled({bx - 10.f, by - 4.f},
                              {bx + ts.x + 10.f, by + ts.y + 4.f},
                              IM_COL32(40, 120, 80, 220), 4.f);
            dl->AddRect      ({bx - 10.f, by - 4.f},
                              {bx + ts.x + 10.f, by + ts.y + 4.f},
                              IM_COL32(120, 220, 160, 255), 4.f, 0, 1.2f);
            dl->AddText({bx, by},
                        IM_COL32(220, 255, 230, 255), buf2);
        }
    }

    auto& f = currentField();
    char buf[64];

    // ── Top HUD ──────────────────────────────────────────────────────────────
    // Left: turn plate + turn-player chip. Centre: phase pills, centred
    // over the FIELD AREA (not the whole window) so the HUD lines up with
    // the centred board. Right: REPLAY/MULTIPLAYER badges + Lobby (drawn
    // by the existing right-anchored blocks).
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        float cy0 = wp.y + (TOP_H - 28.f) * 0.5f;
        // Turn plate.
        snprintf(buf, sizeof(buf), "TURN %d", f.turnCount);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        float x0 = wp.x + 14.f;
        dl->AddRectFilled({x0, cy0}, {x0 + ts.x + 22.f, cy0 + 28.f},
                          IM_COL32(24, 30, 54, 255), 14.f);
        dl->AddRect({x0, cy0}, {x0 + ts.x + 22.f, cy0 + 28.f},
                    IM_COL32(96, 116, 172, 170), 14.f, 0, 1.f);
        dl->AddText({x0 + 11.f, cy0 + 7.f}, IM_COL32(226, 233, 248, 255), buf);
        // Turn-player chip — local-perspective wording in multiplayer.
        bool mp     = m_mpInDuel && !m_net.isOffline();
        bool myTurn = (int)f.turnPlayer == m_net.localPlayerIndex();
        const char* who = mp ? (myTurn ? "YOUR TURN" : "OPPONENT'S TURN")
                             : (f.turnPlayer == 0 ? "P1'S TURN" : "P2'S TURN");
        bool coolSide = mp ? myTurn : (f.turnPlayer == 0);
        ImU32 chipBg  = coolSide ? IM_COL32(26, 52, 96, 255)
                                 : IM_COL32(86, 28, 38, 255);
        ImU32 chipBrd = coolSide ? IM_COL32(96, 170, 255, 230)
                                 : IM_COL32(255, 110, 120, 230);
        ImVec2 ws = ImGui::CalcTextSize(who);
        float x1 = x0 + ts.x + 30.f;
        dl->AddRectFilled({x1, cy0}, {x1 + ws.x + 22.f, cy0 + 28.f},
                          chipBg, 14.f);
        dl->AddRect({x1, cy0}, {x1 + ws.x + 22.f, cy0 + 28.f},
                    chipBrd, 14.f, 0, 1.2f);
        dl->AddText({x1 + 11.f, cy0 + 7.f}, IM_COL32(240, 246, 255, 255), who);
    }

    static const struct { uint16_t ph; const char* name; } kPhases[] = {
        {0x01,"Draw"},{0x02,"Stby"},{0x04,"M1"},
        {0x08,"Batt"},{0x100,"M2"},{0x200,"End"},
    };
    // After the duel ends the phase indicator is frozen/inactive — no phase is
    // highlighted so the bar reads as "not in play". Snapshot-aware: the
    // host-auth client's local engine is idle by design, so this must use
    // isDuelVisiblyRunning(), not m_dm.isRunning(), or the client's phase
    // bar would never highlight anything.
    bool phasesLive = isDuelVisiblyRunning() && !m_dm.isDone();
    // Phase pills — centred over the field area, drawn with the design-
    // system HudPill so the bar shares one look with every other chip row.
    const float pillW = 62.f, pillH = 28.f, pillGap = 5.f;
    const float pillRowW = 6.f * pillW + 5.f * pillGap;
    float pillX = (FLD_W - pillRowW) * 0.5f;
    if (pillX < 340.f) pillX = 340.f;
    ImGui::SetCursorPos({pillX, (TOP_H - pillH) * 0.5f});
    // A phase pill is clickable when the engine offers that transition for the
    // local player's current idle/battle prompt — a third way to advance the
    // turn alongside the bottom-bar buttons and the Space/B/M/E hotkeys.
    const SelectionRequest& psel = m_dm.selection();
    const int kLocalPhase = m_net.localPlayerIndex();
    const bool myIdle   = psel.type == WaitType::SelectIdleCmd  &&
                          psel.player == kLocalPhase;
    const bool myBattle = psel.type == WaitType::SelectBattleCmd &&
                          psel.player == kLocalPhase;
    for (int pi = 0; pi < 6; ++pi) {
        bool active = phasesLive && (f.phase == kPhases[pi].ph);
        uint16_t ph = kPhases[pi].ph;
        bool clickable =
            (myIdle   && ph == 0x08  && psel.toBP) ||   // -> Battle Phase
            (myBattle && ph == 0x100 && psel.toM2) ||   // -> Main Phase 2
            ((myIdle || myBattle) && ph == 0x200 && psel.toEP);  // -> End
        if (UIStyle::HudPill(kPhases[pi].name, active, true, {pillW, pillH}) &&
            clickable) {
            if      (ph == 0x08)  submitIdleCmd(6, 0, "Battle Phase");
            else if (ph == 0x100) submitIdleCmd(2, 0, "Main Phase 2");
            else if (ph == 0x200) submitIdleCmd(myBattle ? 3 : 7, 0,
                                       myBattle ? "End Battle Phase" : "End Turn");
        }
        if (clickable && ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to advance");
        if (pi < 5) ImGui::SameLine(0.f, pillGap);
    }
    // Park the cursor after the pill row for the top-right button cluster.
    ImGui::SameLine(0.f, 4.f);

    // ── Top-right button (Lobby in live, Exit Replay in playback) ─────────
    // Right-aligned. In replay mode the button needs to be interactive even
    // though the rest of the live UI is wrapped in BeginDisabled, so we
    // temporarily pop the disabled stack around it and restore on the way
    // out (ImGui::EndDisabled later in drawDuel still balances the outer
    // BeginDisabled call). We use ImGui::IsItemHovered + IsMouseClicked
    // style isn't necessary because EndDisabled / BeginDisabled is the
    // sanctioned ImGui idiom for "let one widget through" mid-scope.
    // Surrender is now available online too (host forfeits its own engine;
    // client asks the host to). The top-right cluster width must account for
    // BOTH the Lobby and Surrender buttons or Surrender clips off the edge.
    const bool showSurrender = !m_replayMode && isDuelVisiblyRunning() &&
                               !m_dm.isDone() &&
                               (m_net.isOffline() || m_mpInDuel);
    const float kLobbyW = 72.f, kSurrW = 96.f, kBtnGap = 8.f, kEdgePad = 6.f;
    const float clusterW = m_replayMode
        ? 112.f
        : kLobbyW + (showSurrender ? kBtnGap + kSurrW : 0.f);
    float topRightX = ImGui::GetWindowContentRegionMax().x - clusterW - kEdgePad;
    ImGui::SameLine(topRightX < 0.f ? 0.f : topRightX, 0.f);
    if (m_replayMode) {
        ImGui::EndDisabled();
        if (UIStyle::GhostButton("Exit Replay##back", {112.f, 28.f})) {
            stopReplayPlayback();
            m_screen = Screen::Replays;
            m_replayFiles = edo::Replay::list();
            ImGui::BeginDisabled();    // restore matching state for outer
            ImGui::End();
            return;
        }
        ImGui::BeginDisabled();        // restore matching state for outer
    } else {
        if (UIStyle::GhostButton("Lobby##back", {kLobbyW, 28.f})) {
            finalizeReplay("returned to lobby");
            // Online: tear the MP session down so latches don't leak.
            if (!m_net.isOffline()) {
                m_mpInDuel = false;
                m_dm.setSuppressAutoResolve(false);
                resetMpResponseState();
                m_net.disconnect("returned to lobby");
            }
            m_dm.endDuel();
            m_screen = Screen::Lobby;
            ImGui::End();
            return;
        }
        // Surrender — concede the running duel. Confirmed via a modal so it
        // can't be mis-clicked.
        if (showSurrender) {
            ImGui::SameLine(0.f, kBtnGap);
            if (UIStyle::GhostButton("Surrender##sr", {kSurrW, 28.f}))
                ImGui::OpenPopup("Surrender?##srpop");
        }
        // Confirm popup (positioned centrally; auto-sizes to content).
        ImGui::SetNextWindowPos({(float)w * 0.5f, (float)h * 0.5f},
                                ImGuiCond_Always, {0.5f, 0.5f});
        if (ImGui::BeginPopupModal("Surrender?##srpop", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::TextUnformatted("Concede this duel? Your opponent wins.");
            ImGui::Dummy({1.f, 8.f});
            if (UIStyle::DangerButton("Surrender", {150.f, 32.f})) {
                const int me = m_net.localPlayerIndex();
                if (m_net.isOffline() || m_net.isHost()) {
                    // We own the authoritative engine — forfeit directly. In
                    // host-auth MP the per-frame pump then ships GameOver.
                    m_dm.forfeit(me);
                } else {
                    // Host-auth client: no local engine — ask the host to
                    // forfeit our seat; GameOver comes back over the wire.
                    sendSurrender();
                }
                gAudio().play("defeat");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0.f, 8.f);
            if (UIStyle::GhostButton("Cancel##sr", {110.f, 32.f}))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    if (m_dm.isDone()) {
        int winner = m_dm.winner();
        ImGui::SameLine(0.f, 16.f);
        // winner is 0 or 1; anything else (PLAYER_NONE / -1) is a draw.
        if (winner == 0 || winner == 1)
            snprintf(buf, sizeof(buf), "Player %d wins!", winner + 1);
        else
            snprintf(buf, sizeof(buf), "Draw!");
        ImGui::TextColored({1.f, 0.85f, 0.25f, 1.f}, "%s", buf);
    }

    // ── Middle row: Field | Card Info (no permanent log column) ─────────────
    ImGui::SetCursorPos({0.f, TOP_H});

    // Log drawer — floating overlay toggled from the bottom bar ("Log"
    // button). Shows the same two tabs (Game / Debug) with per-tab Clear +
    // Copy buttons; it floats OVER the field's left edge and reserves no
    // layout width, so the board stays centered when it opens.
    const float kLogDrawerW = 340.f;
    if (m_logDrawerOpen) {
        ImGui::SetNextWindowPos({10.f, TOP_H + 10.f});
        ImGui::SetNextWindowSize({kLogDrawerW, MID_H - 20.f});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.05f, 0.06f, 0.11f, 0.96f});
        ImGui::PushStyleColor(ImGuiCol_Border,   {0.40f, 0.48f, 0.78f, 0.85f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.2f);
        ImGui::Begin("##log_drawer", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize  | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);
        const float LOG_W = kLogDrawerW;   // content-width alias (tabs below)
        ImGui::TextColored(COL_ACCENT, "Duel Log");
        ImGui::SameLine(LOG_W - 70.f);
        if (ImGui::SmallButton("Close##logdrawer")) m_logDrawerOpen = false;
        ImGui::Separator();

        // Tab strip — Game / Debug. The selected tab persists.
        bool gameActive = (m_logTab == 0);
        bool debugActive= (m_logTab == 1);
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.10f,0.13f,0.20f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.16f,0.20f,0.30f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.22f,0.28f,0.40f,1.f});
        ImU32 actCol = IM_COL32(255, 214, 108, 60);
        ImVec2 tp = ImGui::GetCursorScreenPos();
        float tabW = (LOG_W - 16.f) * 0.5f;
        if (ImGui::Button(gameActive ? "[Game]##gametab" : "Game##gametab",
                          {tabW - 3.f, 22.f})) {
            m_logTab = 0; saveSettings();
        }
        if (gameActive) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(tp, {tp.x + tabW - 3.f, tp.y + 22.f}, actCol, 3.f);
        }
        ImGui::SameLine(0.f, 4.f);
        ImVec2 tp2 = ImGui::GetCursorScreenPos();
        if (ImGui::Button(debugActive ? "[Debug]##dbgtab" : "Debug##dbgtab",
                          {tabW - 3.f, 22.f})) {
            m_logTab = 1; saveSettings();
        }
        if (debugActive) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(tp2, {tp2.x + tabW - 3.f, tp2.y + 22.f}, actCol, 3.f);
        }
        ImGui::PopStyleColor(3);

        // Clear / Copy buttons — operate on the visible tab.
        if (ImGui::SmallButton(m_logTab == 0 ? "Clear Game##gclear"
                                              : "Clear Debug##dclear")) {
            if (m_logTab == 0) m_gameLog.clear();
            else               m_dm.clearLog();
        }
        ImGui::SameLine(0.f, 6.f);
        if (ImGui::SmallButton(m_logTab == 0 ? "Copy Game##gcopy"
                                              : "Copy Debug##dcopy")) {
            std::ostringstream os;
            if (m_logTab == 0) {
                for (const auto& g : m_gameLog) os << g.text << "\n";
            } else {
                for (const auto& s : m_dm.log()) os << s << "\n";
            }
            ImGui::SetClipboardText(os.str().c_str());
        }
        ImGui::Separator();

        // ── Game tab ─────────────────────────────────────────────────────
        if (m_logTab == 0) {
            if (m_gameLog.empty()) {
                ImGui::TextDisabled("(no events yet — start a duel)");
            }
            for (const auto& g : m_gameLog) {
                ImVec4 col = ImGui::ColorConvertU32ToFloat4(g.color);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextWrapped("%s", g.text.c_str());
                ImGui::PopStyleColor();
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.f)
                ImGui::SetScrollHereY(1.f);
        } else {
        // ── Debug tab (legacy renderer below — preserved styling) ────────
        // Colour rules — same heuristic as before, but with extra cases:
        //   bright blue : Draw / Summon / Turn change
        //   warm orange : Damage / LP / Player wins
        //   red         : ERROR / WARN / RETRY (genuinely loud)
        //   yellow      : [POST-SUMMON ...] / trace headers
        //   green       : [PLACE …] / [EXTRA ACTION] / [DECK SCRIPT AUDIT]
        //   grey        : [dbg] lines and other debug crumbs
        //   default     : everything else (normal events)
        auto& log = m_dm.log();
        for (int i = 0; i < (int)log.size(); i++) {
            const std::string& s = log[i];
            auto starts = [&](const char* p) {
                size_t n = strlen(p);
                return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
            };
            // Normal-mode filter: hide technical traces unless Debug Log is
            // explicitly enabled, so the duel feels like a game and not a
            // console. Errors / warnings / missing scripts ALWAYS show.
            if (!m_debugLog) {
                if (starts("[dbg]") || starts("[trace]") || starts("[deck]") ||
                    starts("[POST-SUMMON") ||
                    starts("[DECK SCRIPT AUDIT]") ||
                    starts("[IDLE NOTE]") ||
                    starts("[RAZEN TARGET CHECK") ||
                    starts("[PLACE CLICK]") || starts("[PLACE RESPONSE]") ||
                    starts("[PLACE REQUEST]") || starts("[CHAIN CLICK]") ||
                    starts("[EXTRA ACTION]") ||
                    starts("[EMZ DEBUG]") || starts("[NEW CARD DEBUG]") ||
                    starts("[effect-option]") ||
                    starts("[Assets]") || starts("  candidate:") ||
                    starts("  option ") || starts("  next ocgcore") ||
                    starts("  concluded by:") || starts("  MSG_SELECT_") ||
                    starts("  effect options for") || starts("  auto-pass:") ||
                    starts("  summoned card:") || starts("  summon type:") ||
                    starts("  script:") || starts("  script path") ||
                    starts("  filter mirrored:") || starts("  Deck cards") ||
                    starts("  VALID target") || starts("  rejected") ||
                    starts("  summary:") || starts("  legal-zone count:") ||
                    starts("  MISSING script"))
                    continue;
            }
            bool inSubstr = [&](const char* p){ return s.find(p) != std::string::npos; }("ERROR");
            if (starts("[dbg]") || starts("[trace]") || starts("[deck]"))
                ImGui::TextColored({0.55f, 0.58f, 0.68f, 1.f}, "%s", s.c_str());
            else if (starts("[POST-SUMMON") || starts("  >>>") ||
                     starts("[RAZEN TARGET CHECK") || starts("[NEW CARD DEBUG]"))
                ImGui::TextColored({1.00f, 0.86f, 0.45f, 1.f}, "%s", s.c_str());
            else if (starts("[PLACE") || starts("[EXTRA ACTION]") ||
                     starts("[DECK SCRIPT AUDIT]") || starts("[Assets]"))
                ImGui::TextColored({0.55f, 0.92f, 0.65f, 1.f}, "%s", s.c_str());
            else if (starts("[Retry]") || starts("[error]") || starts("[ERROR]") ||
                     starts("[SCRIPT MISSING]") || starts("[STATE ERROR]") || inSubstr)
                ImGui::TextColored({1.00f, 0.45f, 0.40f, 1.f}, "%s", s.c_str());
            else if (starts("[warn]") || starts("WARN"))
                ImGui::TextColored({1.00f, 0.78f, 0.30f, 1.f}, "%s", s.c_str());
            else if (s.rfind("Draw", 0) == 0 || s.rfind("Summon", 0) == 0 ||
                     s.rfind("Turn ", 0) == 0)
                ImGui::TextColored({0.60f, 0.82f, 1.00f, 1.f}, "%s", s.c_str());
            else if (s.rfind("Player ", 0) == 0 || s.find(" damage") != std::string::npos ||
                     s.find(" LP ") != std::string::npos)
                ImGui::TextColored({1.00f, 0.72f, 0.42f, 1.f}, "%s", s.c_str());
            else
                ImGui::TextWrapped("%s", s.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.f)
            ImGui::SetScrollHereY(1.f);
        }   // closes the m_logTab == 1 (Debug) else-branch
        ImGui::End();           // ##log_drawer
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }       // closes if (m_logDrawerOpen)

    // Screen shake — offset the whole field child (cards + hit-tests move
    // together, so clicks stay aligned). Zero when no shake is active.
    ImVec2 shake = m_anim.shakeOffset();
    if (shake.x != 0.f || shake.y != 0.f)
        ImGui::SetCursorPos({0.f + shake.x, TOP_H + shake.y});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.05f, 0.07f, 0.11f, 1.f});
    ImGui::BeginChild("##field_child", {FLD_W, MID_H}, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawField((int)FLD_W, (int)MID_H);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ── Overlay animations (phase banner + boss entrance) ───────────────────
    // Foreground list so they sit above the field, info panel and HUD. The
    // duel window is full-screen at (0,0), so the top-left origin is (0,0).
    m_anim.renderTop(ImGui::GetForegroundDrawList(), {0.f, 0.f});

    // ── "Opponent is thinking…" indicator ───────────────────────────────────
    // Shown while the AI is acting on its own turn and you have no prompt, so a
    // long combo (or a Relaxed-speed beat) never looks like the game froze.
    {
        const SelectionRequest& sel = currentSelection();
        bool humanPrompt = DuelManager::isRealSelect(sel.type) &&
                           sel.player == (uint8_t)m_net.localPlayerIndex();
        bool oppThinking = m_net.isOffline() && m_dm.isRunning() &&
            !m_dm.isDone() &&
            (int)m_dm.field().turnPlayer != m_net.localPlayerIndex() &&
            !humanPrompt;
        if (oppThinking) {
            ImDrawList* fdl = ImGui::GetForegroundDrawList();
            int dots = 1 + ((int)(ImGui::GetTime() * 2.0) % 3);
            char txt[40] = "Opponent is thinking";
            for (int d = 0; d < dots; ++d) strncat(txt, ".", sizeof(txt) - 1);
            ImVec2 ts = ImGui::CalcTextSize(txt);
            float pad = 12.f, bw = ts.x + 24.f + pad * 2, bh = ts.y + 12.f;
            ImVec2 p0 { (float)w * 0.5f - bw * 0.5f, 44.f };
            ImVec2 p1 { p0.x + bw, p0.y + bh };
            float pulse = 0.6f + 0.4f * sinf((float)ImGui::GetTime() * 4.f);
            fdl->AddRectFilled(p0, p1, IM_COL32(26, 22, 40, 230), 8.f);
            fdl->AddRect(p0, p1,
                IM_COL32(255, 150, 90, (int)(120 + 110 * pulse)), 8.f, 0, 1.6f);
            // Little spinner.
            float cx = p0.x + pad + 8.f, cy = (p0.y + p1.y) * 0.5f;
            float ang = (float)ImGui::GetTime() * 6.f;
            for (int s = 0; s < 8; ++s) {
                float a = ang + s / 8.f * 6.2831853f;
                fdl->AddCircleFilled({cx + cosf(a) * 7.f, cy + sinf(a) * 7.f},
                    1.8f, IM_COL32(255, 180, 120, (unsigned)(40 + s * 24)), 6);
            }
            fdl->AddText({p0.x + pad + 24.f, p0.y + 6.f},
                         IM_COL32(245, 220, 200, 255), txt);
        }
    }

    // ── Recent-actions strip ────────────────────────────────────────────────
    // A transient feed of the last few game-log lines at the top-left, fading
    // by age, so you can catch what just happened without the toast scrolling
    // away. Disappears when nothing has happened recently.
    if (m_dm.isRunning() && !m_gameLog.empty()) {
        ImDrawList* fdl = ImGui::GetForegroundDrawList();
        double now = ImGui::GetTime();
        float y = 74.f;
        int shown = 0;
        for (auto it = m_gameLog.rbegin();
             it != m_gameLog.rend() && shown < 3; ++it) {
            double age = now - it->at;
            if (age > 7.0) break;                 // only recent lines
            float a = (age < 5.0) ? 1.f : (float)(1.0 - (age - 5.0) / 2.0);
            if (a <= 0.f) continue;
            ImU32 col = it->color ? it->color : IM_COL32(210, 218, 235, 255);
            col = (col & 0x00FFFFFFu) | ((unsigned)(a * 230) << 24);
            ImU32 sh  = IM_COL32(0, 0, 0, (unsigned)(a * 150));
            const char* s = it->text.c_str();
            fdl->AddText({13.f, y + 1.f}, sh,  s);
            fdl->AddText({12.f, y},       col, s);
            y += 18.f;
            ++shown;
        }
    }

    // ── Right: FLOATING card-info overlay ───────────────────────────────────
    // Drawn as a separate glass window on top of the field's right margin.
    // It reserves no layout width, so the arena's centring is untouched —
    // at 1080p the centred board leaves a ~560px margin, far wider than
    // the overlay, so it doesn't cover any zone either.
    if (INFO_W > 0.f) {
        ImGui::SetNextWindowPos({(float)w - INFO_W - 16.f, TOP_H + 12.f});
        ImGui::SetNextWindowSize({INFO_W, MID_H - 24.f});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.045f, 0.055f, 0.095f, 0.92f});
        ImGui::PushStyleColor(ImGuiCol_Border,   {0.36f, 0.44f, 0.70f, 0.75f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.2f);
        ImGui::Begin("##cardinfo_overlay", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize  | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings);
        drawCardInfoPanel((int)INFO_W, (int)(MID_H - 24.f));
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // ── Bottom: Action strip (selected-card actions + phase buttons) ────────
    ImGui::SetCursorPosY(TOP_H + MID_H);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        float ay = wp.y + TOP_H + MID_H;
        // Command-bar gradient + accent hairline (matches the top HUD).
        dl->AddRectFilledMultiColor({wp.x, ay}, {wp.x + w, ay + ACT_H},
            IM_COL32(16, 19, 33, 255), IM_COL32(16, 19, 33, 255),
            IM_COL32(9, 11, 21, 255),  IM_COL32(9, 11, 21, 255));
        dl->AddLine({wp.x, ay}, {wp.x + w, ay},
                    IM_COL32(70, 86, 140, 210));
    }
    ImGui::BeginChild("##action_strip", {(float)w, ACT_H}, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawBottomActionStrip(w, ACT_H);
    ImGui::EndChild();

    // ── Floating Tools drawer ────────────────────────────────────────────
    // Replaces the old permanent bottom testing strip. Opens from the
    // "Tools" ghost button in the command bar; floats over the field's
    // bottom-right corner and reserves no layout space. All debug/testing
    // toggles live here, fully out of sight during normal play.
    if (m_toolsDrawerOpen) {
        const float TOOLS_W = 280.f;
        ImGui::SetNextWindowPos({(float)w - TOOLS_W - 12.f,
                                 TOP_H + MID_H - 392.f});
        ImGui::SetNextWindowSize({TOOLS_W, 380.f});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.05f, 0.06f, 0.11f, 0.97f});
        ImGui::PushStyleColor(ImGuiCol_Border,   {0.40f, 0.48f, 0.78f, 0.85f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.2f);
        ImGui::Begin("##tools_drawer", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize  | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);
        drawTestingBar(w);
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    ImGui::End();

    // Live UI lockout ends here — replay controls AND the MP HUD below
    // are interactive. Mirror the same predicate used at BeginDisabled
    // so the disabled stack balances even if mode changes mid-frame.
    {
        bool mpRemoteTurnEnd = (m_mpInDuel && !m_net.isOffline() &&
                                !isLocalPromptOwner());
        bool mpDesyncEnd     = (m_mpInDuel && !m_net.isOffline() &&
                                m_mpDesynced);
        if (m_replayMode || mpRemoteTurnEnd || mpDesyncEnd) ImGui::EndDisabled();
    }

    // ── Replay controls overlay ─────────────────────────────────────────────
    // In replay mode, a compact strip floats over the bottom of the duel
    // screen with Play/Pause, Step, Speed, Restart, Exit + a status line
    // showing the response index vs total. Drawn AFTER the field/action
    // strip so it sits on top and intercepts clicks before they reach the
    // (locked) live UI underneath.
    if (m_replayMode) {
        const float CTRL_W = 720.f;
        const float CTRL_H = 56.f;
        float cx = ((float)w - CTRL_W) * 0.5f;
        float cy = (float)h - ACT_H - CTRL_H - 8.f;
        if (cy < (float)h * 0.55f) cy = (float)h * 0.55f;
        ImGui::SetNextWindowPos({cx, cy}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({CTRL_W, CTRL_H}, ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.07f, 0.05f, 0.12f, 0.94f});
        ImGui::PushStyleColor(ImGuiCol_Border,   {0.78f, 0.55f, 0.95f, 0.95f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.4f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{12.f, 8.f});
        ImGui::Begin("##replay_controls", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);

        // Play/Pause toggle. Going from paused → playing also clears any
        // desync message so the feeder isn't permanently frozen by an
        // earlier dead-end; the desync re-arms on the next bad frame if
        // the underlying issue persists.
        const char* pp = m_replayPlaying ? "II  Pause" : ">  Play";
        if (UIStyle::SecondaryButton(pp, {104.f, 32.f})) {
            m_replayPlaying = !m_replayPlaying;
            if (m_replayPlaying) {
                m_replayNextAt = ImGui::GetTime();
                m_replayDesyncMsg.clear();
            }
            gAudio().play("click");
        }
        ImGui::SameLine(0.f, 6.f);
        // Step — feeds exactly one response then auto-pauses. Also clears
        // any desync state so the user can retry from the controls strip
        // without opening the modal first (the modal also has a Step).
        if (UIStyle::SecondaryButton("Step", {72.f, 32.f})) {
            m_replayPlaying  = false;
            m_replayStepPulse = true;
            m_replayDesyncMsg.clear();
            gAudio().play("click");
        }
        ImGui::SameLine(0.f, 10.f);
        // Speed cluster.
        const float kSpeeds[] = {0.5f, 1.0f, 2.0f, 4.0f};
        const char* kLbl[]    = {"0.5x", "1x", "2x", "4x"};
        for (int i = 0; i < 4; ++i) {
            bool active = (std::abs(m_replaySpeed - kSpeeds[i]) < 0.01f);
            if (UIStyle::SegmentedButton(kLbl[i], active, true, {44.f, 28.f}))
                m_replaySpeed = kSpeeds[i];
            ImGui::SameLine(0.f, 4.f);
        }
        ImGui::SameLine(0.f, 8.f);
        if (UIStyle::SecondaryButton("Restart", {88.f, 32.f})) {
            // Restart with same replay file — start a fresh seeded duel
            // and reset feed index to zero.
            std::string path = m_replayActivePath;
            stopReplayPlayback();
            if (!path.empty()) startReplayPlayback(path);
        }
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::DangerButton("Exit", {76.f, 32.f})) {
            stopReplayPlayback();
            m_screen = Screen::Replays;
            // Refresh the browser so the deletion / save state is current.
            m_replayFiles = edo::Replay::list();
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
            return;
        }
        // Second line — status / progress.
        char status[160];
        int total = (int)m_replayActive.responses.size();
        snprintf(status, sizeof(status),
                 "Response %d / %d   ·   %.1fx   ·   %s",
                 m_replayIdx, total, m_replaySpeed,
                 m_replayPlaying ? "playing" : "paused");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, {0.84f, 0.80f, 0.96f, 1.f});
        ImGui::TextUnformatted(status);
        ImGui::PopStyleColor();

        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);

        // ── Desync modal ────────────────────────────────────────────────
        // Frozen-state warning when the feeder couldn't find a response.
        // Non-blocking — user can Exit replay or Restart from here.
        if (!m_replayDesyncMsg.empty()) {
            const float DW = 480.f, DH = 0.f;
            ImGui::SetNextWindowSize({DW, DH}, ImGuiCond_Always);
            ImGui::SetNextWindowPos({((float)w - DW) * 0.5f,
                                     (float)h * 0.30f},
                                    ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.12f, 0.06f, 0.10f, 0.97f});
            ImGui::PushStyleColor(ImGuiCol_Border,   {1.f, 0.45f, 0.45f, 0.95f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.6f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
            ImGui::Begin("##replay_desync", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings);
            if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
            ImGui::PushStyleColor(ImGuiCol_Text, {1.f, 0.7f, 0.7f, 1.f});
            ImGui::TextUnformatted("Replay desync detected");
            ImGui::PopStyleColor();
            if (UIStyle::fHeader) ImGui::PopFont();
            ImGui::TextWrapped("%s", m_replayDesyncMsg.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("File : %s", m_replayActivePath.c_str());
            ImGui::TextDisabled("Seed : %llu",
                (unsigned long long)m_replayActive.seed);
            ImGui::TextDisabled("Idx  : %d / %d   pendingType: %d",
                m_replayIdx, (int)m_replayActive.responses.size(),
                (int)currentSelection().type);
            ImGui::Spacing();
            // Try-Step: clears the desync message and pulses one more feed
            // attempt. If the underlying cause was "no responses left" the
            // desync re-arms immediately, but that's a useful diagnostic
            // for the user. For "empty recorded response" this lets them
            // skip past the bad entry.
            if (UIStyle::SecondaryButton("Step (try once)", {150.f, 30.f})) {
                m_replayDesyncMsg.clear();
                m_replayPlaying   = false;
                m_replayStepPulse = true;
                m_dm.logEvent("[REPLAY] desync recovery: Step pressed");
            }
            ImGui::SameLine(0.f, 6.f);
            if (UIStyle::SecondaryButton("Restart Replay", {150.f, 30.f})) {
                std::string path = m_replayActivePath;
                stopReplayPlayback();
                if (!path.empty()) startReplayPlayback(path);
            }
            ImGui::SameLine(0.f, 6.f);
            if (UIStyle::DangerButton("Exit Replay", {130.f, 30.f})) {
                stopReplayPlayback();
                m_screen = Screen::Replays;
                m_replayFiles = edo::Replay::list();
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }
    }

    // ── Multiplayer overlays ────────────────────────────────────────────────
    // "Waiting for opponent..." badge when the engine is paused on a
    // remote prompt. Drawn after the live-UI disabled scope so it sits
    // on top, but it's purely visual — it doesn't affect input gating
    // (that's already handled by BeginDisabled above).
    if (m_mpInDuel && !m_net.isOffline()) {
        const SelectionRequest& selR = currentSelection();
        bool waiting = DuelManager::isRealSelect(selR.type) &&
                       selR.player != (uint8_t)m_net.localPlayerIndex();
        // Host-auth client: between sending a ClientChoice and getting
        // the host's reply, render a distinct "Waiting for host..."
        // badge so the user knows the click was registered.
        bool awaitingHost = (m_mpHostAuth && m_net.isClient() &&
                             m_mpAwaitingHostUpdate &&
                             !m_mpRemoteSelValid);
        if (awaitingHost) {
            const char* line = "Waiting for host update...";
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImVec2 ts = ImGui::CalcTextSize(line);
            float bx = (float)w * 0.5f - ts.x * 0.5f;
            float by = (float)h * 0.42f;
            dl->AddRectFilled({bx - 22.f, by - 10.f},
                              {bx + ts.x + 22.f, by + ts.y + 10.f},
                              IM_COL32(18, 22, 36, 230), 6.f);
            dl->AddRect      ({bx - 22.f, by - 10.f},
                              {bx + ts.x + 22.f, by + ts.y + 10.f},
                              IM_COL32(180, 200, 240, 255), 6.f, 0, 1.4f);
            dl->AddText({bx, by}, IM_COL32(220, 230, 255, 255), line);
            // Suppress the "Waiting for opponent..." overlay below so
            // the two don't double up.
            waiting = false;
        }
        if (waiting) {
            // Build a two-line overlay so the user can see whether the
            // remote peer has actually entered the prompt we're waiting
            // for (top line = generic, bottom line = remote fingerprint
            // + queued response count).
            std::string l1 = "Waiting for opponent...";
            std::ostringstream sub;
            sub << "remote: ";
            if (m_mpRemotePrompt.valid) {
                sub << "wt=" << (int)m_mpRemotePrompt.waitType
                    << "  own=" << (int)m_mpRemotePrompt.owner
                    << "  seq=" << m_mpRemotePrompt.promptSeq;
            } else {
                sub << "(no PromptState yet)";
            }
            sub << "    queued=" << m_mpQueue.size();
            std::string l2 = sub.str();
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImVec2 ts1 = ImGui::CalcTextSize(l1.c_str());
            ImVec2 ts2 = ImGui::CalcTextSize(l2.c_str());
            float bw = std::max(ts1.x, ts2.x);
            float bh = ts1.y + ts2.y + 4.f;
            float bx = (float)w * 0.5f - bw * 0.5f;
            float by = (float)h * 0.42f;
            ImU32 borderC = m_mpDesynced
                ? IM_COL32(232, 110, 100, 255)
                : IM_COL32(120, 220, 160, 255);
            dl->AddRectFilled({bx - 22.f, by - 10.f},
                              {bx + bw + 22.f, by + bh + 10.f},
                              IM_COL32(14, 18, 28, 230), 6.f);
            dl->AddRect      ({bx - 22.f, by - 10.f},
                              {bx + bw + 22.f, by + bh + 10.f},
                              borderC, 6.f, 0, 1.4f);
            dl->AddText({bx + (bw - ts1.x) * 0.5f, by},
                        IM_COL32(220, 255, 230, 255), l1.c_str());
            dl->AddText({bx + (bw - ts2.x) * 0.5f, by + ts1.y + 4.f},
                        IM_COL32(180, 210, 220, 255), l2.c_str());
        }
        // Connection-lost modal — non-blocking; user can Save Replay or
        // return to the Multiplayer screen to reconnect.
        if (m_net.state() == edo::NetState::Disconnected ||
            m_net.state() == edo::NetState::Error) {
            const float DW = 460.f;
            ImGui::SetNextWindowSize({DW, 0.f}, ImGuiCond_Always);
            ImGui::SetNextWindowPos({((float)w - DW) * 0.5f, (float)h * 0.30f},
                                    ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_WindowBg,
                                  {0.12f, 0.06f, 0.10f, 0.97f});
            ImGui::PushStyleColor(ImGuiCol_Border,
                                  {1.f, 0.45f, 0.45f, 0.95f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.6f);
            ImGui::Begin("##mp_conn_lost", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings);
            if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
            ImGui::PushStyleColor(ImGuiCol_Text, {1.f, 0.7f, 0.7f, 1.f});
            ImGui::TextUnformatted("Connection lost");
            ImGui::PopStyleColor();
            if (UIStyle::fHeader) ImGui::PopFont();
            ImGui::TextWrapped("The peer disconnected or the network "
                "went down. The duel is paused — you can save a replay "
                "of what happened, then exit.");
            ImGui::Spacing();
            if (UIStyle::SecondaryButton("Save Replay", {160.f, 30.f})) {
                std::string fn = m_replay.suggestedFilename();
                std::string path = edo::Replay::defaultDir() + "/" + fn;
                if (m_replay.save(path)) {
                    pushToast(std::string("Replay saved: ") + fn,
                              IM_COL32(110, 220, 140, 255), 2.4);
                }
            }
            ImGui::SameLine(0.f, 6.f);
            if (UIStyle::DangerButton("Return to Lobby", {180.f, 30.f})) {
                finalizeReplay("connection lost");
                if (m_dm.isRunning()) m_dm.endDuel();
                m_mpInDuel = false;
                m_dm.setSuppressAutoResolve(false);
                resetMpResponseState();
                m_mpConnLostShown = true;
                m_screen = Screen::Lobby;
            }
            ImGui::End();
            ImGui::PopStyleVar(1);
            ImGui::PopStyleColor(2);
        }

        // ── MP diagnostic modal ─────────────────────────────────────────
        // Engine parked AND we're in MP, OR a prompt-state desync has
        // been observed. The standard "Duel paused" modal is suppressed
        // here; this purple-bordered diagnostic surfaces the same
        // information in a multiplayer-aware way. It also acts as the
        // PromptState desync pause — when m_mpDesynced is set the
        // surrounding BeginDisabled scope keeps input locked and this
        // modal explains why.
        if ((m_dm.isBlocked() || m_mpDesynced) &&
            m_net.state() != edo::NetState::Disconnected &&
            m_net.state() != edo::NetState::Error) {
            const float DW = 520.f;
            ImGui::SetNextWindowSize({DW, 0.f}, ImGuiCond_Always);
            ImGui::SetNextWindowPos({((float)w - DW) * 0.5f, (float)h * 0.30f},
                                    ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.10f, 0.06f, 0.16f, 0.97f});
            ImGui::PushStyleColor(ImGuiCol_Border,   {0.78f, 0.55f, 0.95f, 0.95f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.6f);
            ImGui::Begin("##mp_sync_diag", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings);
            if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
            ImGui::PushStyleColor(ImGuiCol_Text,
                m_mpDesynced ? ImVec4{1.f, 0.62f, 0.62f, 1.f}
                             : ImVec4{0.95f, 0.78f, 1.f, 1.f});
            ImGui::TextUnformatted(m_mpDesynced
                ? "Multiplayer prompt DESYNC"
                : "Multiplayer prompt sync");
            ImGui::PopStyleColor();
            if (UIStyle::fHeader) ImGui::PopFont();
            if (m_mpDesynced) {
                ImGui::TextWrapped(
                    "Both clients are paused because their engine "
                    "prompts no longer agree. This means a previous "
                    "response wasn't applied identically on both sides, "
                    "and continuing would deepen the divergence. Save a "
                    "replay and exit so the run can be diagnosed.");
                ImGui::Separator();
                ImGui::TextDisabled("Fingerprint mismatch:");
                ImGui::TextWrapped("%s", m_mpDesyncSummary.c_str());
                ImGui::Spacing();
                ImGui::TextDisabled("Local prompt:");
                ImGui::Text("  seq=%llu  waitType=%u  owner=%u  forced=%s",
                    (unsigned long long)m_mpLocalPromptSeq,
                    m_mpLocalPrompt.waitType,
                    (unsigned)m_mpLocalPrompt.owner,
                    m_mpLocalPrompt.forced ? "yes" : "no");
                ImGui::Text("  chain=%u  opts=%u  phase=%u  turnPlayer=%u",
                    m_mpLocalPrompt.chainCount,
                    m_mpLocalPrompt.optionCount,
                    (unsigned)m_mpLocalPrompt.phase,
                    (unsigned)m_mpLocalPrompt.turnPlayer);
                ImGui::TextDisabled("Remote prompt:");
                ImGui::Text("  seq=%llu  waitType=%u  owner=%u  forced=%s",
                    (unsigned long long)m_mpRemotePrompt.promptSeq,
                    m_mpRemotePrompt.waitType,
                    (unsigned)m_mpRemotePrompt.owner,
                    m_mpRemotePrompt.forced ? "yes" : "no");
                ImGui::Text("  chain=%u  opts=%u  phase=%u  turnPlayer=%u",
                    m_mpRemotePrompt.chainCount,
                    m_mpRemotePrompt.optionCount,
                    (unsigned)m_mpRemotePrompt.phase,
                    (unsigned)m_mpRemotePrompt.turnPlayer);
            } else {
                ImGui::TextWrapped(
                    "The engine is parked on a prompt the live UI didn't "
                    "recognise, OR is waiting for a response that hasn't "
                    "arrived yet. The MP response queue keeps trying to "
                    "drain — if a matching response arrives this modal "
                    "closes itself automatically. If it persists, the "
                    "duel has hit a parser gap that isn't safe to guess "
                    "past.");
            }
            ImGui::Separator();
            const SelectionRequest& diag = currentSelection();
            ImGui::Text("waitType        : %d  %s",
                (int)diag.type,
                diag.type == WaitType::RawPrompt
                    ? "(RawPrompt — parser gap)"
                    : "");
            ImGui::Text("selection player: %d", (int)diag.player);
            ImGui::Text("isBlocked       : %s",
                m_dm.isBlocked() ? "yes" : "no");
            ImGui::Text("local player    : %d", m_net.localPlayerIndex());
            ImGui::Text("net mode        : %s",
                m_net.isHost() ? "host" :
                m_net.isClient() ? "client" : "offline");
            ImGui::Text("queued responses: %d", (int)m_mpQueue.size());
            // If the engine parked on an unhandled message, surface the
            // captured frame here too — the user can copy it for a bug
            // report without scrolling the debug log.
            if (diag.type == WaitType::RawPrompt) {
                std::string hex;
                const auto& raw = m_dm.lastMsgPayload();
                for (size_t i = 0; i < raw.size(); ++i) {
                    char buf[4];
                    snprintf(buf, sizeof(buf), "%02x", raw[i]);
                    if (i) hex += " ";
                    hex += buf;
                }
                ImGui::Separator();
                ImGui::TextDisabled("Missing parser for MSG_* — please report:");
                ImGui::Text("msg id          : %d",
                    (int)m_dm.lastMsgType());
                ImGui::TextWrapped("rawHex (first %zu bytes):\n%s",
                    raw.size(), hex.c_str());
            }
            // Last engine log line (best-effort).
            const auto& dlog = m_dm.log();
            if (!dlog.empty())
                ImGui::TextWrapped("Last engine log: %s",
                                   dlog.back().c_str());
            ImGui::Spacing();
            if (UIStyle::SecondaryButton("Copy diagnostics", {180.f, 30.f})) {
                std::ostringstream os;
                os << "[MP sync diagnostic]\n"
                   << "waitType="     << (int)diag.type
                   << "  player="     << (int)diag.player
                   << "  blocked="    << (m_dm.isBlocked() ? "yes":"no") << "\n"
                   << "localPlayer="  << m_net.localPlayerIndex()
                   << "  netMode="    << (m_net.isHost() ? "host"
                                          : m_net.isClient() ? "client"
                                          : "offline") << "\n"
                   << "queuedCount="  << m_mpQueue.size() << "\n";
                if (diag.type == WaitType::RawPrompt) {
                    const auto& raw = m_dm.lastMsgPayload();
                    os << "rawMsgId="   << (int)m_dm.lastMsgType()
                       << "  rawLen="   << raw.size() << "\n"
                       << "rawHex=";
                    for (size_t i = 0; i < raw.size(); ++i) {
                        char buf[4];
                        snprintf(buf, sizeof(buf), "%02x", raw[i]);
                        if (i) os << " ";
                        os << buf;
                    }
                    os << "\n";
                }
                os << "Recent log:\n";
                size_t start = dlog.size() > 20 ? dlog.size() - 20 : 0;
                for (size_t i = start; i < dlog.size(); ++i)
                    os << "  " << dlog[i] << "\n";
                ImGui::SetClipboardText(os.str().c_str());
                pushToast("MP diagnostics copied",
                          IM_COL32(180, 220, 255, 255), 2.0);
            }
            ImGui::SameLine(0.f, 6.f);
            if (UIStyle::SecondaryButton("Save Replay", {160.f, 30.f})) {
                std::string fn = m_replay.suggestedFilename();
                std::string path = edo::Replay::defaultDir() + "/" + fn;
                if (m_replay.save(path))
                    pushToast(std::string("Replay saved: ") + fn,
                              IM_COL32(110, 220, 140, 255), 2.4);
            }
            ImGui::SameLine(0.f, 6.f);
            if (UIStyle::DangerButton("Exit duel", {140.f, 30.f})) {
                finalizeReplay("MP prompt sync exit");
                if (m_dm.isRunning()) m_dm.endDuel();
                m_mpInDuel = false;
                m_dm.setSuppressAutoResolve(false);
                resetMpResponseState();
                m_net.disconnect("user exit");
                m_screen = Screen::Lobby;
            }
            ImGui::End();
            ImGui::PopStyleVar(1);
            ImGui::PopStyleColor(2);
        }
    }

    // ── Floating overlays drawn ON TOP of the field ─────────────────────────
    // 1) Compact card preview (top-right, small) — on hover/selection.
    // The floating preview overlay is superseded by the fixed right-side
    // card info panel; keep it ONLY on narrow windows where the panel is
    // collapsed (same threshold as INFO_W above).
    if ((float)w < 1100.f) drawCompactPreviewOverlay(w, TOP_H);
    // 2) Card-anchored action popup — appears next to the clicked card with
    //    only that card's legal actions. Replaces the old right command
    //    center for idle/battle actions.
    drawCardActionPopup(w, h);
    // 3) Centered modal — ONLY for real engine prompts/viewers/game-over
    //    (MSG_SELECT_CARD, chain prompt, yes/no, options, GY/BN/ED viewer,
    //    blocked, done). Never opens for hover or simple selection.
    drawCenteredModal(w, h);
}

// ─── drawSideZone ─────────────────────────────────────────────────────────────
// Draws a utility zone (GY, Deck, Extra Deck) with a count badge.
// No hit-testing needed — purely informational.
void UI::drawSideZone(const char* label, int count,
                       ImVec2 sp, float zW, float zH, ImVec4 col,
                       uint32_t topCode, bool topHidden) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br = {sp.x + zW, sp.y + zH};
    // Classify the pile from its label so each reads distinctly.
    bool isGY = label && label[0]=='G';
    bool isBN = label && label[0]=='B';
    bool isED = label && label[0]=='E';
    bool isDK = label && label[0]=='D';
    // GY / Banishment show the TOP card that most recently entered, so the
    // player can read the game state at a glance instead of a generic icon.
    bool showTop = (isGY || isBN) && topCode != 0;

    // Glass pile tile with a downward hue fade. Drop shadow for depth.
    dl->AddRectFilled({sp.x + 2.f, sp.y + 3.f}, {br.x + 2.f, br.y + 3.f},
                      IM_COL32(0, 0, 0, 90), 6.f);
    ImU32 hue = ImGui::ColorConvertFloat4ToU32(
        {col.x, col.y, col.z, col.w * 0.9f});
    ImU32 hueFade = ImGui::ColorConvertFloat4ToU32(
        {col.x * 0.32f, col.y * 0.32f, col.z * 0.42f, 0.9f});
    dl->AddRectFilledMultiColor(sp, br, hue, hue, hueFade, hueFade);
    // Coloured top accent bar so the pile type is identifiable at a glance.
    ImU32 accent =
        isGY ? IM_COL32(150, 165, 200, 255) :   // GY  — cool grey-blue
        isBN ? IM_COL32(208, 120, 230, 255) :   // BN  — magenta
        isED ? IM_COL32(120, 200, 255, 255) :   // ED  — cyan
               IM_COL32(232, 196, 110, 255);    // DK  — gold
    dl->AddRectFilled(sp, {br.x, sp.y + 4.f}, accent, 2.f);
    dl->AddRect(sp, br, IM_COL32(104, 118, 162, 200), 6.f, 0, 1.2f);

    if (showTop) {
        // Top card art fills the tile beneath the accent bar (back if hidden,
        // e.g. a face-down banished card the player isn't allowed to see).
        void* tex = topHidden ? m_rend.getBackTexture()
                              : m_rend.getCardTexture(topCode);
        if (tex)
            dl->AddImage((ImTextureID)tex, {sp.x + 2.f, sp.y + 6.f},
                         {br.x - 2.f, br.y - 2.f});
        // Subtle darkening at the bottom so the count pill stays legible.
        dl->AddRectFilledMultiColor({sp.x + 2.f, br.y - zH * 0.34f},
                                    {br.x - 2.f, br.y - 2.f},
                                    IM_COL32(0,0,0,0), IM_COL32(0,0,0,0),
                                    IM_COL32(8,8,14,200), IM_COL32(8,8,14,200));
    } else {
        // Pile glyph behind the count — original simple geometry per pile type.
        ImVec2 gc{ sp.x + zW * 0.5f, sp.y + zH * 0.40f };
        float gr = zW * 0.20f;
        ImU32 gcol = (accent & 0x00FFFFFF) | 0x40000000;
        if (isGY) {                                   // tombstone arch + base
            dl->AddRectFilled({gc.x - gr, gc.y}, {gc.x + gr, gc.y + gr*1.1f}, gcol, 3.f);
            dl->AddCircleFilled({gc.x, gc.y}, gr, gcol, 16);
        } else if (isBN) {                            // X (banished)
            dl->AddLine({gc.x - gr, gc.y - gr}, {gc.x + gr, gc.y + gr}, gcol, 3.f);
            dl->AddLine({gc.x + gr, gc.y - gr}, {gc.x - gr, gc.y + gr}, gcol, 3.f);
        } else if (isED) {                            // diamond (extra)
            ImVec2 d[4]={{gc.x,gc.y-gr},{gc.x+gr,gc.y},{gc.x,gc.y+gr},{gc.x-gr,gc.y}};
            dl->AddConvexPolyFilled(d, 4, gcol);
        } else {                                       // stacked deck lines
            for (int k=0;k<3;++k)
                dl->AddRectFilled({gc.x-gr, gc.y-gr+k*gr*0.8f},
                                  {gc.x+gr, gc.y-gr*0.6f+k*gr*0.8f}, gcol, 1.f);
        }
    }

    // Label (top-left).
    dl->AddText({sp.x + 5.f, sp.y + 5.f}, IM_COL32(224, 232, 248, 235), label);

    // Count — gold-rimmed pill near the bottom so it never overlaps the glyph.
    char cbuf[8];
    snprintf(cbuf, 8, "%d", count);
    ImVec2 ts = ImGui::CalcTextSize(cbuf);
    float cx = sp.x + (zW - ts.x) * 0.5f;
    float cy = sp.y + zH - ts.y - 7.f;
    dl->AddRectFilled({cx - 8.f, cy - 3.f}, {cx + ts.x + 8.f, cy + ts.y + 3.f},
                      IM_COL32(10, 12, 22, 225), 9.f);
    dl->AddRect({cx - 8.f, cy - 3.f}, {cx + ts.x + 8.f, cy + ts.y + 3.f},
                IM_COL32(212, 178, 102, 200), 9.f, 0, 1.f);
    dl->AddText({cx, cy}, IM_COL32(255, 244, 214, 245), cbuf);
}

// True if the engine is currently asking the LOCAL player (P0) for a
// placement zone. Drives the field-tile glow + direct click handler so the
// player picks a zone on the BOARD, not from the right-side overlay.
bool UI::isPlacementMode() const {
    const SelectionRequest& s = currentSelection();
    // LOCAL player, not hardcoded P0 — the MP client's local seat is
    // engine player 1, and its placement prompts must glow/click too.
    return s.type == WaitType::SelectPlace &&
           s.player == (uint8_t)m_net.localPlayerIndex();
}

// Translate the engine's placeFlag bitmask into "is this (loc,seq) legal?".
// Engine encoding (a SET bit means FORBIDDEN/occupied):
//   LOC_MZONE: bits 0-4 = M1-M5, bits 5-6 = EMZ1-EMZ2
//   LOC_SZONE: bits 8-12 = ST1-ST5, bit 13 = Field Zone, bits 14-15 = Pendulum
bool UI::isPlacementLegal(uint8_t loc, uint32_t seq) const {
    if (!isPlacementMode()) return false;
    uint32_t flag = currentSelection().placeFlag;
    if (loc == LOC_MZONE) {
        if (seq <= 4) return !(flag & (1u << seq));
        if (seq == 5) return !(flag & (1u << 5));
        if (seq == 6) return !(flag & (1u << 6));
    } else if (loc == LOC_SZONE) {
        if (seq <= 4) return !(flag & (1u << (seq + 8)));
        if (seq == 5) return !(flag & (1u << 13));
        if (seq == 6) return !(flag & (1u << 14));
        if (seq == 7) return !(flag & (1u << 15));
    }
    return false;
}

// True if any idle action targets the (player, loc, seq) slot the engine
// currently offers. Used to glow clickable cards in hand/field.
bool UI::hasLegalActionFor(uint8_t player, uint8_t loc, uint32_t seq) const {
    auto& sel = currentSelection();
    if (sel.type != WaitType::SelectIdleCmd &&
        sel.type != WaitType::SelectBattleCmd) return false;
    for (const auto& a : sel.idle)
        if (a.con == player && a.loc == loc && a.seq == seq) return true;
    return false;
}

// Right-click card context menu (#6).
bool UI::tryOpenCardContext(uint8_t con, uint8_t loc, uint32_t seq,
                            uint32_t code) {
    if (con != (uint8_t)m_net.localPlayerIndex()) return false;
    if (!hasLegalActionFor(con, loc, seq)) return false;
    m_ctxRequest = true;
    m_ctxCode = code; m_ctxPlayer = con; m_ctxLoc = loc; m_ctxSeq = seq;
    m_ctxPos = ImGui::GetMousePos();
    // Also select the card so the action panel mirrors the menu.
    m_selCode = code; m_selPlayer = con; m_selLoc = loc; m_selSeq = seq;
    return true;
}

void UI::drawCardContextMenu() {
    if (m_ctxRequest) {
        ImGui::OpenPopup("##cardctx");
        ImGui::SetNextWindowPos(m_ctxPos);
        m_ctxRequest = false;
    }
    if (!ImGui::BeginPopup("##cardctx")) return;
    const SelectionRequest& sel = currentSelection();
    bool any = false;
    if (sel.type == WaitType::SelectIdleCmd ||
        sel.type == WaitType::SelectBattleCmd) {
        for (const IdleAction& a : sel.idle) {
            if (a.con != m_ctxPlayer || a.loc != m_ctxLoc || a.seq != m_ctxSeq)
                continue;
            std::string nm = a.name.empty() ? ("#" + std::to_string(a.code))
                                            : a.name;
            const char* verb =
                a.cmd == 0 ? "Normal Summon"
              : a.cmd == 1 ? (sel.type == WaitType::SelectBattleCmd
                                  ? (a.canDirect ? "Attack directly" : "Attack")
                                  : "Special Summon")
              : a.cmd == 2 ? "Change Position"
              : a.cmd == 3 ? "Set (face-down)"
              : a.cmd == 4 ? "Set Spell/Trap"
              : a.cmd == 5 ? "Activate effect"
                           : "Action";
            std::string lbl = verb;
            if (a.cmd == 5 && !a.effect.text.empty())
                lbl += "  —  " + a.effect.text;
            if (ImGui::Selectable(lbl.c_str())) {
                submitIdleCmd(a.cmd, a.index, "context menu");
                ImGui::CloseCurrentPopup();
            }
            any = true;
        }
    }
    if (!any) ImGui::TextDisabled("No actions available.");
    ImGui::Separator();
    if (ImGui::Selectable("Zoom card")) { m_zoomCard = m_ctxCode;
                                          ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

bool UI::isAttackerLegal(uint8_t player, uint8_t loc, uint32_t seq,
                         int* outIdx, bool* outCanDirect) const {
    auto& sel = currentSelection();
    if (sel.type != WaitType::SelectBattleCmd) return false;
    for (const auto& a : sel.idle) {
        if (a.cmd == 1 && a.con == player && a.loc == loc && a.seq == seq) {
            if (outIdx)       *outIdx = a.index;
            if (outCanDirect) *outCanDirect = a.canDirect;
            return true;
        }
    }
    return false;
}

bool UI::locInfoToRect(uint8_t con, uint8_t loc, uint32_t seq,
                       ImVec2* tl, ImVec2* br) const {
    if (!m_zoneRectsReady || !tl || !br) return false;
    if (con > 1) return false;
    // Monster zones: seq 0..6 mapped via cached m_rectMZ_*.
    if (loc == LOC_MZONE && seq < 7) {
        *tl = m_rectMZ_tl[con][seq];
        *br = m_rectMZ_br[con][seq];
        return true;
    }
    // S/T zones 0..4 (now cached) — lets spell/trap placement + activation
    // animations anchor to the correct zone.
    if (loc == LOC_SZONE && seq < 5) {
        *tl = m_rectSZ_tl[con][seq];
        *br = m_rectSZ_br[con][seq];
        return true;
    }
    return false;
}

// ─── drawCardZone ─────────────────────────────────────────────────────────────
// All rendering via DrawList (no ImGui cursor manipulation until InvisibleButton).
// screenPos is the top-left corner in SCREEN coordinates.
void UI::drawCardZone(const char* label, const CardState* card,
                       ImVec2 sp, float zW, float zH,
                       bool faceDown, int uid,
                       int zonePlayer, uint8_t zoneLoc, uint32_t zoneSeq) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br = {sp.x + zW, sp.y + zH};

    // Background — "dark glass" tiles. Empty zones get a subtle vertical
    // gradient + thin neon hairline; occupied zones a slightly warmer base
    // so card edges read against them, plus a soft drop shadow.
    if (card && card->code) {
        ImU32 base = (card->loc == LOC_MZONE) ? IM_COL32(26, 34, 62, 235)
                   : (card->loc == LOC_SZONE) ? IM_COL32(20, 42, 36, 235)
                                              : IM_COL32(34, 34, 40, 200);
        // Drop shadow under the card.
        dl->AddRectFilled({sp.x + 2.f, sp.y + 3.f}, {br.x + 2.f, br.y + 3.f},
                          IM_COL32(0, 0, 0, 110), 6.f);
        dl->AddRectFilled(sp, br, base, 6.f);
        dl->AddRect(sp, br, IM_COL32(96, 116, 168, 215), 6.f, 0, 1.2f);
    } else {
        dl->AddRectFilledMultiColor(sp, br,
            IM_COL32(22, 28, 50, 165), IM_COL32(22, 28, 50, 165),
            IM_COL32(12, 16, 30, 185), IM_COL32(12, 16, 30, 185));
        dl->AddRect(sp, br, IM_COL32(72, 92, 148, 150), 6.f, 0, 1.f);
        // Corner ticks — modern "target frame" accent on empty tiles.
        const float t = 7.f;
        ImU32 tickCol = IM_COL32(110, 134, 196, 120);
        dl->AddLine({sp.x + 2.f, sp.y + 2.f + t}, {sp.x + 2.f, sp.y + 2.f}, tickCol, 1.4f);
        dl->AddLine({sp.x + 2.f, sp.y + 2.f}, {sp.x + 2.f + t, sp.y + 2.f}, tickCol, 1.4f);
        dl->AddLine({br.x - 2.f - t, br.y - 2.f}, {br.x - 2.f, br.y - 2.f}, tickCol, 1.4f);
        dl->AddLine({br.x - 2.f, br.y - 2.f}, {br.x - 2.f, br.y - 2.f - t}, tickCol, 1.4f);
    }

    if (card && card->code) {
        // Defense rotation applies ONLY to monsters. Spells / Traps may have
        // POS_FACEDOWN_DEFENSE on their position bits when set face-down but
        // they must stay upright in their S/T zone (and face-up Spells/Traps
        // obviously do not have a "defense" orientation at all).
        CardInfo zci = m_db.getCard(card->code);
        bool isMonsterCard = (zci.type & TYPE_MONSTER) != 0;
        // Defense rotation applies ONLY to monsters sitting in a MONSTER
        // ZONE. A Pendulum monster used as a SCALE lives in the S/T (Pendulum)
        // zone and ocgcore may report it with a defense position bit — but it
        // must render UPRIGHT like a Spell, never rotated. Gating on
        // LOC_MZONE is the fix for the "Pendulum scale looks distorted /
        // mirrored" report.
        bool isDefense = isMonsterCard && card->loc == LOC_MZONE &&
                         (card->pos & (POS_FACEUP_DEFENSE |
                                       POS_FACEDOWN_DEFENSE)) != 0;
        void* tex = faceDown ? m_rend.getBackTexture()
                              : m_rend.getCardTexture(card->code);
        if (tex) {
            ImVec2 a = {sp.x + 2.f, sp.y + 2.f};
            ImVec2 b = {br.x  - 2.f, br.y  - 2.f};
            // Route through the shared helper — aspect-fit (never stretched),
            // guaranteed-correct UVs for upright cards, a true 90° rotation
            // only for monster-zone defense. Face-down backs never rotate.
            drawCardArt(dl, card->code, tex, a, b,
                        /*rotateDefenseCW*/ isDefense && !faceDown,
                        /*dbgCheck*/ !faceDown);
            // [PENDULUM SCALE RENDER] — a face-up Pendulum monster in an S/T
            // zone (i.e. a scale). Verifies upright + aspect-fit at runtime.
            if (m_debugLog && !faceDown && (zci.type & TYPE_PENDULUM) &&
                card->loc == LOC_SZONE) {
                static std::unordered_map<uint32_t, bool> s_psSeen;
                if (!s_psSeen[card->code]) {
                    s_psSeen[card->code] = true;
                    ImVec2 f0, f1; fitCardRect(a, b, false, &f0, &f1);
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "[PENDULUM SCALE RENDER] code=%u name=%s zone=ST "
                        "slot=%.0f,%.0f,%.0f,%.0f fit=%.0f,%.0f,%.0f,%.0f "
                        "uv0=0,0 uv1=1,1 flipped=no aspect=%.3f",
                        (unsigned)card->code, zci.name.c_str(),
                        a.x, a.y, b.x - a.x, b.y - a.y,
                        f0.x, f0.y, f1.x - f0.x, f1.y - f0.y,
                        (f1.x - f0.x) / std::max(1.f, (f1.y - f0.y)));
                    m_dm.logEvent(buf);
                }
            }
        } else {
            // No art: name truncated in zone — but an opponent's face-down
            // card must never reveal its name (hidden-info rule).
            bool hiddenFromUs = faceDown &&
                card->player != (uint8_t)m_net.localPlayerIndex();
            if (hiddenFromUs) {
                dl->AddText({sp.x + 4.f, sp.y + 4.f},
                            IM_COL32(160, 170, 200, 210), "Set");
            } else {
                CardInfo ci = m_db.getCard(card->code);
                const char* nm = ci.name.empty() ? label : ci.name.c_str();
                dl->AddText({sp.x + 4.f, sp.y + 4.f},
                            IM_COL32(190, 200, 230, 230), nm);
            }
        }

        // Counter badge — Spell Counters, A-Counters, Bushido Counters, etc.
        // are public info and many archetypes hinge on them, so show the running
        // total in the card's top-right corner whenever the card holds any.
        if (card->counters > 0) {
            ImVec2 cc = {br.x - 13.f, sp.y + 13.f};
            UIStyle::CountBadge(dl, cc, (int)card->counters,
                                IM_COL32(120, 200, 255, 255));
        }

        // Faint zone label in the top-left of occupied zones (M1, ST3, FZ, …)
        // — gated by the `Zone labels` testing-bar toggle.
        if (m_showZoneLabels && label && label[0]) {
            float lw = ImGui::CalcTextSize(label).x + 6.f;
            dl->AddRectFilled({sp.x + 1.f, sp.y + 1.f},
                              {sp.x + 1.f + lw, sp.y + 14.f},
                              IM_COL32(0, 0, 0, 130), 3.f);
            dl->AddText({sp.x + 4.f, sp.y + 1.f},
                        IM_COL32(230, 235, 255, 200), label);
        }

        // Optional card-name strip at the bottom — toggled via the testing bar.
        // Drawn ABOVE the ATK stripe (for monsters) so neither obscures the other.
        // Hidden-info rule: never print an opponent's face-down card name.
        bool nameHidden = faceDown &&
            card->player != (uint8_t)m_net.localPlayerIndex();
        if (m_showFieldNames && !nameHidden) {
            CardInfo ci = m_db.getCard(card->code);
            if (!ci.name.empty()) {
                std::string nm = ci.name;
                // Naive truncation by length; ImGui-side truncation would
                // need precise text width measurement vs zone width.
                if (nm.size() > 18) nm = nm.substr(0, 17) + "..";
                float nameH = 13.f;
                bool isMonsterStripe = (ci.type & TYPE_MONSTER) &&
                                       card->loc == LOC_MZONE && !faceDown &&
                                       !(card->pos & POS_FACEDOWN_ATTACK);
                float nameY = (isMonsterStripe ? br.y - 15.f - nameH : br.y - nameH)
                              - 1.f;
                dl->AddRectFilled({sp.x + 2.f, nameY},
                                  {br.x - 2.f, nameY + nameH},
                                  IM_COL32(0, 0, 0, 175), 3.f);
                dl->AddText({sp.x + 4.f, nameY + 1.f},
                            IM_COL32(230, 235, 245, 230), nm.c_str());
            }
        }

        // ATK overlay stripe at bottom for face-up attack-position monsters
        if (card->loc == LOC_MZONE && !faceDown &&
            !(card->pos & POS_FACEDOWN_ATTACK)) {
            CardInfo ci = m_db.getCard(card->code);
            if (ci.type & TYPE_MONSTER) {
                char stat[20];
                snprintf(stat, 20, "%d / %d", ci.atk, ci.def);
                float stripeY = br.y - 15.f;
                dl->AddRectFilled({sp.x, stripeY}, br,
                                  IM_COL32(0, 0, 0, 170), 0.f);
                dl->AddText({sp.x + 3.f, stripeY + 2.f},
                            IM_COL32(255, 220, 80, 255), stat);
            }
        }
    } else {
        // Empty zone label (kept centered + larger for empty slots so the
        // grid still reads as the engine's zone map).
        dl->AddText({sp.x + 4.f, sp.y + 4.f},
                    IM_COL32(75, 88, 130, 200), label);
    }

    // Placement-target glow (bright green) — engine asked the local player
    // to choose a zone and THIS empty zone is a legal target.
    bool placementHere = isPlacementMode() && (!card || !card->code) &&
                         zonePlayer >= 0 &&
                         (uint8_t)zonePlayer == currentSelection().player &&
                         isPlacementLegal(zoneLoc, zoneSeq);
    if (placementHere) {
        dl->AddRectFilled(sp, br, IM_COL32(40, 180, 80, 90), 5.f);
        dl->AddRect      (sp, br, IM_COL32(120, 255, 140, 255), 5.f, 0, 3.f);
    }
    // Chain-candidate glow (bright magenta) — engine asked for a chain
    // response and THIS card is one of the chainable sources. Click to
    // respondChain with that card's engine index.
    int chainIdx = -1;
    bool chainHere = card && card->code &&
                     isChainCandidate(card->player, (uint8_t)card->loc,
                                      card->seq, &chainIdx);
    if (chainHere) {
        dl->AddRect(sp, br, IM_COL32(255, 120, 240, 255), 5.f, 0, 3.f);
    }
    // Selection candidate (target / summon material) — green glow so it can be
    // clicked on the board, alongside the gallery picker.
    int selCandIdx = -1;
    bool selHere = card && card->code &&
                   isSelectCandidate(card->player, (uint8_t)card->loc,
                                     card->seq, &selCandIdx);
    if (selHere) {
        UIStyle::DrawGlow(dl, sp, br, IM_COL32(110, 230, 140, 220), 5.f, 3);
        dl->AddRect(sp, br, IM_COL32(140, 255, 165, 255), 5.f, 0, 3.f);
    }
    // Selected outline (bright cyan) — drawn under the InvisibleButton so it
    // shows on top of the card background but below tooltips/hover effects.
    if (card && card->code && isSelectedCard(*card))
        dl->AddRect(sp, br, IM_COL32(120, 230, 255, 255), 5.f, 0, 3.f);
    // Battle Phase attacker glow (hot red/orange) — a monster that has a
    // legal attack action this Battle Phase. Outranks the generic legal-
    // action glow so attackers read distinctly during BP.
    else if (m_showLegalGlow && card && card->code &&
             isAttackerLegal(card->player, (uint8_t)card->loc, card->seq))
        dl->AddRect(sp, br, IM_COL32(255, 110,  80, 240), 5.f, 0, 2.6f);
    // Legal-action glow (soft orange) — gated by the testing-bar toggle.
    else if (m_showLegalGlow && card && card->code &&
             hasLegalActionFor(card->player, (uint8_t)card->loc, card->seq))
        dl->AddRect(sp, br, IM_COL32(255, 200, 90, 200), 5.f, 0, 2.2f);

    // Hover / click via InvisibleButton at screen position
    ImGui::SetCursorScreenPos(sp);
    char btnId[24];
    snprintf(btnId, 24, "##z%d", uid);
    bool clicked = ImGui::InvisibleButton(btnId, {zW, zH});

    if (ImGui::IsItemHovered()) {
        // Hidden-info rule: an opponent's face-down card must not reveal
        // its identity on hover — not in the tooltip and not in the info
        // panel. (Own set cards stay inspectable: the owner may look.)
        bool hiddenFromUs = card && card->code && faceDown &&
                            card->player != (uint8_t)m_net.localPlayerIndex();
        if (hiddenFromUs) {
            dl->AddRect(sp, br, IM_COL32(120, 140, 200, 180), 5.f, 0, 1.8f);
            ImGui::SetTooltip("Set card");
        } else if (card && card->code) {
            m_hoveredCard = card->code;
            m_hoveredInfo = m_db.getCard(card->code);
            setInfoCtx(card->player, (uint8_t)card->loc, card->seq,
                       card->pos);
            // Right-click → a context menu of this card's legal actions if it's
            // yours and can act; otherwise pin a large card view.
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                if (!tryOpenCardContext(card->player, (uint8_t)card->loc,
                                        card->seq, card->code))
                    m_zoomCard = card->code;
            }
            // Battle-Phase aiming line: when hovering a monster that can attack,
            // draw a line from it to the cursor so it's clear this monster is
            // about to declare an attack (click = attack).
            if (isAttackerLegal(card->player, (uint8_t)card->loc, card->seq)) {
                ImVec2 ctr = {(sp.x + br.x) * 0.5f, (sp.y + br.y) * 0.5f};
                ImVec2 mp  = ImGui::GetMousePos();
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                fg->AddLine(ctr, mp, IM_COL32(255, 110, 80, 230), 3.f);
                fg->AddCircleFilled(mp, 6.f, IM_COL32(255, 110, 80, 235));
                fg->AddCircle(mp, 6.f, IM_COL32(255, 220, 200, 230), 16, 1.5f);
            }
            // Count engine-legal actions for this exact (player, loc, seq).
            int actCount = 0;
            for (const auto& a : currentSelection().idle)
                if (a.con == card->player && a.loc == card->loc &&
                    a.seq == card->seq) ++actCount;
            const char* posName =
                (card->pos & POS_FACEUP_ATTACK)    ? "face-up ATK"  :
                (card->pos & POS_FACEDOWN_ATTACK)  ? "face-down ATK":
                (card->pos & POS_FACEUP_DEFENSE)   ? "face-up DEF"  :
                (card->pos & POS_FACEDOWN_DEFENSE) ? "face-down DEF": "?";
            // Type-aware stats line: Spells and Traps must NEVER show ATK/DEF.
            // Link monsters show ATK / LINK-rating; other monsters show ATK/DEF.
            char stats[64];
            if (m_hoveredInfo.type & TYPE_MONSTER) {
                if (m_hoveredInfo.type & 0x4000000) {
                    int rating = m_hoveredInfo.def > 0 ? m_hoveredInfo.def
                                                       : (int)m_hoveredInfo.level;
                    snprintf(stats, sizeof(stats), "ATK %d   LINK-%d",
                             m_hoveredInfo.atk, rating);
                } else {
                    snprintf(stats, sizeof(stats), "ATK %d / DEF %d",
                             m_hoveredInfo.atk, m_hoveredInfo.def);
                }
            } else if (m_hoveredInfo.type & TYPE_SPELL) {
                snprintf(stats, sizeof(stats), "Spell card");
            } else if (m_hoveredInfo.type & TYPE_TRAP) {
                snprintf(stats, sizeof(stats), "Trap card");
            } else {
                snprintf(stats, sizeof(stats), "Unknown type");
            }
            ImGui::SetTooltip(
                "%s\nP%d  %s  seq %u  (%s)\n%s\nLegal actions: %d",
                m_hoveredInfo.name.empty() ? label : m_hoveredInfo.name.c_str(),
                card->player + 1,
                label && label[0] ? label : "?",
                (unsigned)card->seq, posName,
                stats, actCount);
        } else {
            // Highlight empty zone on hover + show zone tooltip
            dl->AddRect(sp, br, IM_COL32(120, 140, 200, 180), 5.f, 0, 1.8f);
            ImGui::SetTooltip("%s  (empty)", label && label[0] ? label : "Zone");
        }
    }
    // Placement takes priority during MSG_SELECT_PLACE: clicking a glowing
    // empty zone sends respondPlace directly, no overlay button needed.
    if (clicked && placementHere) {
        m_dm.logEvent("[PLACE CLICK] P" +
                      std::to_string((int)currentSelection().player + 1) +
                      " " + (zoneLoc == LOC_MZONE ? "MZONE" : "SZONE") +
                      " seq " + std::to_string(zoneSeq));
        gAudio().play("confirm");
        submitPlace((int)currentSelection().player,
                    (int)zoneLoc, (int)zoneSeq);
        return;
    }
    // Selection candidate — clicking a glowing green card on the board submits
    // it as the target / summon material (mirrors the gallery picker).
    if (clicked && selHere && selCandIdx >= 0) {
        const SelectionRequest& s = currentSelection();
        gAudio().play("confirm");
        if (s.type == WaitType::SelectUnselect) submitUnselect(selCandIdx);
        else                                    submitMpChoice(s.type, selCandIdx);
        return;
    }
    // Chain response — clicking a glowing magenta card sends respondChain
    // with that card's engine index. If multiple chain options come from the
    // same card the engine modal lists them; this fast path covers the
    // overwhelmingly common single-effect case.
    if (clicked && chainHere && chainIdx >= 0) {
        m_dm.logEvent("[CHAIN CLICK] card #" +
                      std::to_string(card->code) + " chainIdx=" +
                      std::to_string(chainIdx));
        gAudio().play("chain");
        submitMpChoice(WaitType::SelectChain, chainIdx);
        clearSelection();
        return;
    }
    // Click-first: clicking a card in a field zone selects it (or deselects
    // if it was already the selected one). Anchor the action popup above
    // this card so it floats over the field, not in a side panel.
    if (clicked && card && card->code) {
        // Hidden-info rule: never select/reveal an opponent's face-down card
        // (the action popup + preview would otherwise expose its identity).
        bool hiddenFromUs = faceDown &&
            card->player != (uint8_t)m_net.localPlayerIndex();
        if (!hiddenFromUs) {
            selectCardFrom(*card);
            m_actionAnchorX = sp.x + zW * 0.5f;
            m_actionAnchorY = sp.y;
        }
    }
}

// ─── drawField ───────────────────────────────────────────────────────────────
//
//  Exact layout per user spec:
//
//  P2 hand (card backs, top)
//  ┌──────┬─────────────────────────────┬──────┬──────┐
//  │  FZ  │  M1   M2   M3   M4   M5    │  BN  │      │
//  │  ED  │  ST1  ST2  ST3  ST4  ST5   │  GY  │      │ ← right col: 3 stacked
//  │      ├──── EMZ1 ────── EMZ2 ───── ┤  DK  │      │
//  │  DK  │  ST1  ST2  ST3  ST4  ST5   │      │      │
//  │  FZ  │  M1   M2   M3   M4   M5    │  GY  │      │
//  └──────┴─────────────────────────────┴──────┴──────┘
//  P1 hand (face-up, bottom)
//
//  P1 right side (top→bottom): Banishment, GY, DK
//  P1 left side  (top→bottom): FZ, ED
//  P2 mirror
//
void UI::drawField(int fw, int fh) {
    auto& f     = currentField();
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImVec2      wPos = ImGui::GetWindowPos();

    // Two gap dimensions:
    //   GAP_Y  fixed row-gap — must stay small so the seven stacked rows
    //          (hand, MZ, S/T, EMZ, S/T, MZ, hand) all fit vertically.
    //   GAP_X  column-gap, stretches up to GAP_X_MAX to fill the available
    //          horizontal space when the window is height-limited.
    // The previous patch used a single GAP for both, so stretching the
    // column gap also blew up gridH and clipped the bottom hand row.
    const float GAP_Y       = 3.f;           // tight rows = taller zones
    float       GAP_X       = 4.f;
    const float GAP_X_MAX   = 34.f;          // wide, cinematic column spread
    // Hand rows now nearly zone-height: hand cards are the main thing the
    // player reads and clicks, so they get real screen estate (the cards
    // themselves stay aspect-true inside the slot via fitCard).
    const float HAND_FR     = 0.96f;

    // Compact LP overlays — slightly generous so LP numbers are easy to read
    // and the health bar has a meaningful size.
    const float LP_W = 148.f;
    const float LP_H = 54.f;

    float avW = (float)fw - 2.f * GAP_X;
    float avH = (float)fh - 2.f * GAP_Y;

    // Width: 7 cols + 6 gaps. Height: 5 zone rows + 2 hand rows + 6 gaps.
    // Both use the BASE gap value — GAP_X is stretched only after final
    // zW/zH are decided, and the row layout uses GAP_Y throughout.
    float zW = (avW - 6.f * GAP_X) / 7.f;
    float zH_h = (avH - 6.f * GAP_Y) / (5.f + 2.f * HAND_FR);
    float zH_w = zW * (614.f / 421.f);
    float zH   = std::min(zH_h, zH_w);
    zW = zH * (421.f / 614.f);
    // Soft cap on the zone size — generous so large windows get a big,
    // readable board; the height limit clamps below this on most screens.
    if (zW > 232.f) { zW = 232.f; zH = zW * (614.f / 421.f); }

    float hH = zH * HAND_FR;

    // ── Horizontal stretch — column gap only ──────────────────────────────
    // When the window is height-limited, zW comes out small. Spread the
    // 6 inter-column gaps to fill the remaining horizontal room, BUT cap
    // at GAP_X_MAX so the field never feels disconnected. gridH is built
    // from GAP_Y below, so this stretch does not affect vertical fit.
    {
        float naturalGridW = 7.f * zW + 6.f * GAP_X;
        // Target ~96% of the available width for a wide, cinematic board,
        // but always leave room for the LP gutter (the centring below
        // treats gutter + grid as one block, so the grid itself must fit
        // in fw − gutter with a small breathing margin).
        float gutterReserve = LP_W + 24.f;
        float targetGridW   = std::min(avW * 0.96f,
                                       (float)fw - gutterReserve - 16.f);
        if (targetGridW > naturalGridW) {
            float extraPerGap = (targetGridW - naturalGridW) / 6.f;
            float newGap = GAP_X + extraPerGap;
            if (newGap > GAP_X_MAX) newGap = GAP_X_MAX;
            GAP_X = newGap;
        }
    }

    float gridW = 7.f * zW + 6.f * GAP_X;
    float gridH = 2.f * hH + 5.f * zH + 6.f * GAP_Y;

    // ── Centering V2: centre the VISIBLE ARENA, nothing else ────────────
    // The previous pass centred an invisible "LP gutter + grid" block,
    // which pushed the visible mat half-a-gutter (~78px) right of the
    // play-area centre — the log said delta=0 while the SCREEN looked
    // shifted, because the eye sees the mat, not the gutter. The centred
    // unit is now the visible arena itself (zone grid + mat frame). LP
    // panels are pure OVERLAYS placed into whatever left margin results;
    // they never participate in the centering math.
    float ox = wPos.x + ((float)fw - gridW) * 0.5f;
    float oy = wPos.y + ((float)fh - gridH) * 0.5f;
    if (oy < wPos.y + 2.f) oy = wPos.y + 2.f;

    // Visible-arena centering audit (V2) — re-logged on geometry change.
    {
        static int s_lastFw = -1, s_lastFh = -1;
        if (s_lastFw != fw || s_lastFh != fh) {
            s_lastFw = fw; s_lastFh = fh;
            const float matPad = 8.f;                 // mat frame padding
            float arenaCx   = (float)fw * 0.5f;
            float visibleCx = (ox - wPos.x) + gridW * 0.5f;
            m_dm.logEvent("[DUEL LAYOUT V2] window=0,0," +
                          std::to_string(fw) + "," + std::to_string(fh) +
                          "  arenaAvailable=0,0," + std::to_string(fw) +
                          "," + std::to_string(fh) +
                          "  visibleArena=" +
                          std::to_string((int)(ox - wPos.x - matPad)) + "," +
                          std::to_string((int)(oy - wPos.y - matPad)) + "," +
                          std::to_string((int)(gridW + 2.f * matPad)) + "," +
                          std::to_string((int)(gridH + 2.f * matPad)) +
                          "  zoneGrid=" +
                          std::to_string((int)(ox - wPos.x)) + "," +
                          std::to_string((int)(oy - wPos.y)) + "," +
                          std::to_string((int)gridW) + "," +
                          std::to_string((int)gridH) +
                          "  arenaCenter=" + std::to_string((int)arenaCx) +
                          "  visibleCenter=" + std::to_string((int)visibleCx) +
                          "  delta=" +
                          std::to_string((int)(visibleCx - arenaCx)) +
                          "  rightPanelMode=overlay" +
                          "  zone=" + std::to_string((int)zW) + "x" +
                          std::to_string((int)zH) +
                          "  hand=" +
                          std::to_string((int)(hH * (421.f / 614.f))) +
                          "x" + std::to_string((int)hH));
            // HARD INVARIANT: the field child now spans the FULL window,
            // so fw's centre IS the window centre. The visible arena must
            // sit within 2px of it or the layout is wrong.
            int hardDelta = (int)(visibleCx - arenaCx);
            m_dm.logEvent(std::string("[DUEL CENTER HARD CHECK]"
                          "  windowCenterX=") +
                          std::to_string((int)arenaCx) +
                          "  visibleArenaCenterX=" +
                          std::to_string((int)visibleCx) +
                          "  delta=" + std::to_string(hardDelta) +
                          ((hardDelta <= 2 && hardDelta >= -2)
                               ? "  PASS" : "  FAIL"));
        }
    }

    // ── Layout Guides overlay (Tools drawer toggle) ─────────────────────
    // Gold line = visible-arena centre, teal line = play-area centre.
    // When the layout is right the two lines coincide and the on-screen
    // badge reads 0 px — verifiable by eye, not just by log.
    if (m_showLayoutGuides) {
        // Foreground draw list → renders on top of the playmat, zones and
        // cards regardless of paint order.
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        float arenaCx = wPos.x + (float)fw * 0.5f;
        float visCx   = ox + gridW * 0.5f;
        fg->AddLine({arenaCx, wPos.y}, {arenaCx, wPos.y + (float)fh},
                    IM_COL32(0, 230, 190, 170), 1.5f);
        fg->AddLine({visCx, wPos.y}, {visCx, wPos.y + (float)fh},
                    IM_COL32(255, 214, 108, 210), 1.5f);
        char gb[64];
        snprintf(gb, sizeof(gb), "centre delta = %d px",
                 (int)(visCx - arenaCx));
        fg->AddRectFilled({arenaCx + 8.f, wPos.y + 8.f},
                          {arenaCx + 190.f, wPos.y + 30.f},
                          IM_COL32(0, 0, 0, 190), 4.f);
        fg->AddText({arenaCx + 14.f, wPos.y + 12.f},
                    IM_COL32(255, 255, 255, 235), gb);
    }

    // ── Perspective mapping ───────────────────────────────────────────────
    // Offline / replay duels always render engine player 0 at the bottom
    // (legacy convention). In multiplayer, the LOCAL player must always
    // see their own hand at the bottom and the opponent's face-down hand
    // at the top — otherwise the P2 client would reveal the host's hand
    // (info leak) and never see their own cards. The mapping lives ONLY
    // in drawField; engine player ownership (selection.player, prompt
    // gating, network response routing) is unchanged.
    // Bottom seat = the local human's engine seat. In MP that's
    // localPlayerIndex(); offline it's normally 0, but a coin toss can set a
    // seat override (the local player going SECOND controls engine player 1),
    // which localPlayerIndex() reflects — so this is correct in every mode.
    int botP = m_net.localPlayerIndex();
    int topP = botP ^ 1;
    // One-shot debug log on the first frame after a duel starts so the
    // hand-render mapping is auditable from the Debug Log.
    static int s_lastBotP = -1, s_lastTopP = -1;
    if (m_debugLog && (botP != s_lastBotP || topP != s_lastTopP)) {
        m_dm.logEvent(std::string("[HAND RENDER MAP] localPlayerIndex=") +
                      std::to_string(m_net.isOffline() ? 0
                                     : m_net.localPlayerIndex()) +
                      "  bottomEnginePlayer=" + std::to_string(botP) +
                      "  topEnginePlayer="    + std::to_string(topP));
        s_lastBotP = botP;
        s_lastTopP = topP;
    }

    // Per-row gap "GAP" is what later code (cx lambda, gridW math)
    // expects as a single name. It points at GAP_X — the column gap —
    // because that's the only one used for X math. rY[] math below uses
    // GAP_Y directly.
    const float GAP = GAP_X;

    // Column stride uses the (possibly stretched) GAP_X. Row stride uses
    // the smaller fixed GAP_Y so vertical fit is preserved no matter how
    // far GAP_X stretched.
    auto cx = [&](int col) { return ox + col * (zW + GAP_X); };

    // Row Y values — every row gap is GAP_Y (NOT the stretched GAP_X).
    float rY[8];
    rY[0] = oy;                                 // P2 hand
    rY[1] = rY[0] + hH  + GAP_Y;                // P2 Monster
    rY[2] = rY[1] + zH  + GAP_Y;                // P2 S/T
    rY[3] = rY[2] + zH  + GAP_Y;                // EMZ row
    rY[4] = rY[3] + zH  + GAP_Y;                // P1 S/T
    rY[5] = rY[4] + zH  + GAP_Y;                // P1 Monster
    rY[6] = rY[5] + zH  + GAP_Y;                // P1 hand
    rY[7] = rY[6] + hH;

    // ── Cache screen-space zone rects for the animation layer ──────────────
    // The duel observer (in drawDuel, runs BEFORE this paint each frame)
    // anchors animations at on-screen positions using these caches. The
    // first frame's caches are computed below and used on the SECOND frame
    // onwards — m_zoneRectsReady gates against the bootstrap frame.
    {
        // Monster zones — main rows seq 0..4. The cache is keyed by engine
        // player; with perspective flip the BOTTOM display row belongs to
        // botP and the TOP display row belongs to topP. Animation hooks
        // read by engine-player index and get the right pixels in both
        // cases without further branching.
        for (int seq = 0; seq < 5; ++seq) {
            m_rectMZ_tl[botP][seq] = {cx(1 + seq), rY[4]};
            m_rectMZ_br[botP][seq] = {cx(1 + seq) + zW, rY[4] + zH};
            m_rectMZ_tl[topP][seq] = {cx(1 + seq), rY[2]};
            m_rectMZ_br[topP][seq] = {cx(1 + seq) + zW, rY[2] + zH};
        }
        // EMZ — shared between players, both entries point at the same spots.
        for (int p = 0; p < 2; ++p) {
            m_rectMZ_tl[p][5] = {cx(2), rY[3]};
            m_rectMZ_br[p][5] = {cx(2) + zW, rY[3] + zH};
            m_rectMZ_tl[p][6] = {cx(4), rY[3]};
            m_rectMZ_br[p][6] = {cx(4) + zW, rY[3] + zH};
        }
        // Side zones map to display position via botP / topP.
        // Bottom: col 6 deck (rY[5]) / col 6 GY (rY[4]) / col 6 BN (rY[3])
        // Top   : col 0 deck (rY[1]) / col 0 GY (rY[2]) / col 0 BN (rY[3])
        m_rectDeck_tl[botP] = {cx(6), rY[5]};
        m_rectDeck_br[botP] = {cx(6) + zW, rY[5] + zH};
        m_rectDeck_tl[topP] = {cx(0), rY[1]};
        m_rectDeck_br[topP] = {cx(0) + zW, rY[1] + zH};
        m_rectGY_tl[botP]   = {cx(6), rY[4]};
        m_rectGY_br[botP]   = {cx(6) + zW, rY[4] + zH};
        m_rectGY_tl[topP]   = {cx(0), rY[2]};
        m_rectGY_br[topP]   = {cx(0) + zW, rY[2] + zH};
        m_rectBN_tl[botP]   = {cx(6), rY[3]};
        m_rectBN_br[botP]   = {cx(6) + zW, rY[3] + zH};
        m_rectBN_tl[topP]   = {cx(0), rY[3]};
        m_rectBN_br[topP]   = {cx(0) + zW, rY[3] + zH};
        // ── LP overlay placement ─────────────────────────────────────────
        // Compact LP panels float OVER the natural side margin of the
        // playmat (or barely overlap the deck/GY/BN column on tight
        // windows). They're anchored to each player's monster row so the
        // panel feels visually attached to that player's half.
        //
        // X = a few px outside the playmat's left edge when there's room,
        //     otherwise clamped to the field-child's left edge.
        // Y = aligned with each player's monster row centre, clamped to
        //     stay inside the field child so they're never clipped.
        float lpX = ox - LP_W - 10.f;                   // outside playmat
        if (lpX < wPos.x + 4.f) lpX = wPos.x + 4.f;     // clamp to window
        // Align with each player's MAIN monster row, keyed by display
        // position (bottom = botP at rY[5], top = topP at rY[1]).
        float lpY_p2 = rY[1] + (zH - LP_H) * 0.5f;      // top MZ row (topP)
        float lpY_p1 = rY[5] + (zH - LP_H) * 0.5f;      // centred on P1 MZ row
        // Hard-clamp Y into the visible window so the panel can never be
        // clipped off-screen on extreme heights.
        if (lpY_p2 < wPos.y + 4.f) lpY_p2 = wPos.y + 4.f;
        if (lpY_p1 + LP_H > wPos.y + fh - 4.f)
            lpY_p1 = wPos.y + fh - LP_H - 4.f;
        // Display-keyed assignment: topP gets the rY[1]-aligned rect,
        // botP gets the rY[5]-aligned rect. In offline / host this is
        // [1]=top / [0]=bottom (unchanged); in client view it swaps so
        // engine player 1's LP visually anchors at the bottom.
        m_rectLP_tl[topP] = {lpX, lpY_p2};
        m_rectLP_br[topP] = {lpX + LP_W, lpY_p2 + LP_H};
        m_rectLP_tl[botP] = {lpX, lpY_p1};
        m_rectLP_br[botP] = {lpX + LP_W, lpY_p1 + LP_H};
        m_zoneRectsReady = true;
    }

    // ── Backgrounds (playmat) ─────────────────────────────────────────────────
    // Modern layered arena: deep base → soft glow behind the mat → GRADIENT
    // half tints (strongest at each player's edge, fading to the centre, so
    // there are no flat colour blocks) → faint geometric grid → rounded
    // glass mat panel → neon centre divider with glow → edge vignette.
    dl->AddRectFilled({wPos.x,wPos.y},{wPos.x+fw,wPos.y+fh}, IM_COL32(5,7,14,255));
    float midY = rY[3] + zH * 0.5f;
    // Soft radial-ish glow behind the playmat (two nested rounded rects).
    {
        ImVec2 c  = {ox + gridW * 0.5f, oy + gridH * 0.5f};
        float  rw = gridW * 0.62f, rh = gridH * 0.62f;
        dl->AddRectFilled({c.x - rw, c.y - rh}, {c.x + rw, c.y + rh},
                          IM_COL32(70, 26, 32, 36), 90.f);
        rw *= 0.78f; rh *= 0.78f;
        dl->AddRectFilled({c.x - rw, c.y - rh}, {c.x + rw, c.y + rh},
                          IM_COL32(96, 32, 38, 30), 70.f);
    }
    // Gradient territory tints: top = opponent (deep red), bottom = you
    // (brighter crimson) — both red-family but distinct top/bottom.
    dl->AddRectFilledMultiColor({wPos.x, wPos.y}, {wPos.x + fw, midY},
        IM_COL32(66, 18, 24, 100), IM_COL32(66, 18, 24, 100),
        IM_COL32(66, 18, 24, 6),   IM_COL32(66, 18, 24, 6));
    dl->AddRectFilledMultiColor({wPos.x, midY}, {wPos.x + fw, wPos.y + fh},
        IM_COL32(120, 34, 40, 6),  IM_COL32(120, 34, 40, 6),
        IM_COL32(120, 34, 40, 95), IM_COL32(120, 34, 40, 95));
    // Faint geometric column lines across the mat (subtle circuit feel).
    {
        ImU32 gl = IM_COL32(190, 96, 102, 14);
        for (float gx = ox; gx <= ox + gridW + 1.f; gx += zW + GAP_X)
            dl->AddLine({gx, oy - 6.f}, {gx, oy + gridH + 6.f}, gl, 1.f);
        dl->AddLine({ox - 6.f, midY - zH * 0.55f},
                    {ox + gridW + 6.f, midY - zH * 0.55f}, gl, 1.f);
        dl->AddLine({ox - 6.f, midY + zH * 0.55f},
                    {ox + gridW + 6.f, midY + zH * 0.55f}, gl, 1.f);
    }
    // Glass mat panel framing the grid (rounded, with inner hairline).
    float padX = 8.f, padY = 8.f;
    dl->AddRectFilled({ox - padX, oy - padY},
                      {ox + gridW + padX, oy + gridH + padY},
                      IM_COL32(18, 10, 12, 218), 12.f);
    dl->AddRect      ({ox - padX, oy - padY},
                      {ox + gridW + padX, oy + gridH + padY},
                      IM_COL32(168, 60, 66, 175), 12.f, 0, 1.6f);
    dl->AddRect      ({ox - padX + 3.f, oy - padY + 3.f},
                      {ox + gridW + padX - 3.f, oy + gridH + padY - 3.f},
                      IM_COL32(104, 44, 50, 95), 9.f, 0, 1.f);
    // Centre divider — neon gold core with a soft outer glow band.
    dl->AddRectFilled({ox - padX, midY - 4.f},
                      {ox + gridW + padX, midY + 4.f},
                      IM_COL32(220, 180, 90, 28));
    dl->AddRectFilled({ox - padX, midY - 1.2f},
                      {ox + gridW + padX, midY + 1.2f},
                      IM_COL32(232, 196, 110, 215));
    // EMZ row highlight — a faint tinted overlay behind the centre row
    // (columns 2 and 4: Extra Monster Zones) to distinguish them from the
    // standard monster/spell rows. Subtle gold border around the EMZ band.
    {
        float emzY0 = rY[3];
        float emzY1 = rY[3] + zH;
        // Tinted band across the full grid width.
        dl->AddRectFilled({ox - padX + 2.f, emzY0 - 2.f},
                          {ox + gridW + padX - 2.f, emzY1 + 2.f},
                          IM_COL32(200, 170, 60, 14), 8.f);
        // Thin accent border on the top and bottom of the EMZ band.
        dl->AddLine({ox - padX + 4.f, emzY0 - 2.f},
                    {ox + gridW + padX - 4.f, emzY0 - 2.f},
                    IM_COL32(220, 180, 80, 55), 1.2f);
        dl->AddLine({ox - padX + 4.f, emzY1 + 2.f},
                    {ox + gridW + padX - 4.f, emzY1 + 2.f},
                    IM_COL32(220, 180, 80, 55), 1.2f);
        // "EMZ" corner label at the left edge of the band.
        dl->AddText({ox - padX + 5.f, emzY0 + (zH - 12.f) * 0.5f},
                    IM_COL32(220, 196, 100, 100), "EMZ");
    }
    // Edge vignette: gentle darkening at the top/bottom edges of the area.
    dl->AddRectFilledMultiColor(
        {wPos.x, wPos.y}, {wPos.x + fw, wPos.y + 26.f},
        IM_COL32(0,0,0,130), IM_COL32(0,0,0,130),
        IM_COL32(0,0,0,0),   IM_COL32(0,0,0,0));
    dl->AddRectFilledMultiColor(
        {wPos.x, wPos.y + fh - 26.f}, {wPos.x + fw, wPos.y + fh},
        IM_COL32(0,0,0,0),   IM_COL32(0,0,0,0),
        IM_COL32(0,0,0,130), IM_COL32(0,0,0,130));

    // ── Per-player LP panels (overlaid on the field, near each player) ──────
    // P2 LP sits at the top-left of the playfield; P1 LP at the bottom-left.
    // Each panel: rounded background + "P# LP" header + large LP number +
    // colour-graded fill bar. Damage is obvious because the number is big.
    auto drawLpPanel = [&](int pl, ImVec2 pos) {
        const float W = LP_W, H = LP_H;
        ImVec2 br = {pos.x + W, pos.y + H};
        bool isLocal = (pl == m_net.localPlayerIndex());
        // YOU = crimson red; OPPONENT = slate steel-grey (clear contrast on the
        // red/black mat).
        ImU32 sideFill = isLocal
            ? IM_COL32( 52,  18,  22, 215)
            : IM_COL32( 28,  28,  34, 215);
        ImU32 sideBorder = isLocal
            ? IM_COL32(220,  64,  70, 235)
            : IM_COL32(130, 140, 156, 230);
        // Soft outer glow on the active-turn player.
        if ((int)f.turnPlayer == pl) {
            dl->AddRectFilled({pos.x - 3.f, pos.y - 3.f},
                              {br.x + 3.f,  br.y + 3.f},
                              isLocal ? IM_COL32(240, 64, 70, 45)
                                      : IM_COL32(150, 160, 180, 40), 9.f);
        }
        dl->AddRectFilled(pos, br, sideFill, 7.f);
        // Top highlight shimmer.
        dl->AddRectFilledMultiColor(pos, {br.x, pos.y + H * 0.45f},
            IM_COL32(255,255,255,20), IM_COL32(255,255,255,20),
            IM_COL32(255,255,255,0), IM_COL32(255,255,255,0));
        dl->AddRect(pos, br, sideBorder, 7.f, 0, 1.4f);

        // Label: "YOU" / "OPP" in multiplayer, "P1" / "P2" offline.
        const char* tag = (m_mpInDuel && !m_net.isOffline())
            ? (isLocal ? "YOU" : "OPP")
            : (pl == 0 ? "P1" : "P2");
        UIStyle::PushFont(UIStyle::fSmall);
        dl->AddText({pos.x + 9.f, pos.y + 6.f},
                    IM_COL32(180, 200, 240, 200), tag);
        UIStyle::PopFont();

        // Animated LP: tick m_lpShown toward the real value; m_lpGhost lingers
        // above it after damage so the loss is visible as a draining red trail.
        float dt = ImGui::GetIO().DeltaTime;
        if (dt > 0.1f) dt = 0.1f;
        float tgt = (float)f.lp[pl];
        if (m_lpShown[pl] < tgt) m_lpShown[pl] = tgt;          // healing: snap up
        else m_lpShown[pl] += (tgt - m_lpShown[pl]) * std::min(1.f, dt * 7.f);
        if (m_lpShown[pl] < tgt + 0.5f && m_lpShown[pl] > tgt - 0.5f)
            m_lpShown[pl] = tgt;
        if (m_lpGhost[pl] < m_lpShown[pl]) m_lpGhost[pl] = m_lpShown[pl];
        else m_lpGhost[pl] += (m_lpShown[pl] - m_lpGhost[pl]) *
                              std::min(1.f, dt * 2.6f);

        // LP number — large, right of label, showing the ticking value.
        char lpStr[16]; snprintf(lpStr, 16, "%d", (int)(m_lpShown[pl] + 0.5f));
        UIStyle::PushFont(UIStyle::fHeader);
        ImVec2 lts = ImGui::CalcTextSize(lpStr);
        dl->AddText({br.x - lts.x - 9.f, pos.y + 4.f},
                    IM_COL32(255, 232, 140, 252), lpStr);
        UIStyle::PopFont();

        // HP bar — thicker, taller track.
        float frac  = std::min(std::max(m_lpShown[pl], 0.f) / 8000.f, 1.f);
        float gfrac = std::min(std::max(m_lpGhost[pl], 0.f) / 8000.f, 1.f);
        float trackY0 = pos.y + H - 13.f;
        float trackY1 = pos.y + H - 5.f;
        float trackX0 = pos.x + 9.f, trackX1 = br.x - 9.f;
        dl->AddRectFilled({trackX0, trackY0}, {trackX1, trackY1},
                          IM_COL32(0, 0, 0, 150), 4.f);
        // Ghost (recent-damage) segment in red behind the live bar.
        if (gfrac > frac + 0.001f) {
            float gw = (trackX1 - trackX0) * gfrac;
            dl->AddRectFilled({trackX0, trackY0}, {trackX0 + gw, trackY1},
                              IM_COL32(220, 70, 70, 220), 4.f);
        }
        if (frac > 0.f) {
            ImU32 barCol =
                frac > 0.66f ? IM_COL32( 78, 210, 100, 250)
              : frac > 0.33f ? IM_COL32(240, 196,  60, 250)
                             : IM_COL32(238,  80,  80, 250);
            float fw = (trackX1 - trackX0) * frac;
            dl->AddRectFilled({trackX0, trackY0}, {trackX0 + fw, trackY1},
                              barCol, 4.f);
            // Sheen highlight.
            dl->AddRectFilledMultiColor(
                {trackX0, trackY0}, {trackX0 + fw, trackY0 + 3.f},
                IM_COL32(255,255,255,60), IM_COL32(255,255,255,60),
                IM_COL32(255,255,255,0), IM_COL32(255,255,255,0));
        }
        // Turn indicator: gold dot in top-right corner.
        if ((int)f.turnPlayer == pl) {
            dl->AddCircleFilled({br.x - 10.f, pos.y + 10.f}, 4.5f,
                                IM_COL32(255, 216, 90, 255));
            dl->AddCircle      ({br.x - 10.f, pos.y + 10.f}, 6.5f,
                                IM_COL32(255, 230, 130, 190), 20, 1.2f);
        }
    };
    // LP panels — drawn at the cached rects so they always land at the same
    // pixels the observer queues damage flashes / number popups against.
    // Compact 132x46 overlays sit just OUTSIDE the playmat's left edge,
    // vertically centred on each player's main monster row (P2 = rY[1],
    // P1 = rY[5]). Each Y is clamped to stay inside the field-child clip
    // rect so panels never escape the visible area on extreme heights.
    // The lambda receives the engine player index so it pulls the right
    // f.lp[] value; the cached rect already encodes the display position.
    drawLpPanel(topP, m_rectLP_tl[topP]);
    drawLpPanel(botP, m_rectLP_tl[botP]);

    // ── Helpers ───────────────────────────────────────────────────────────────
    auto getCard = [](const std::vector<CardState>& v, int seq) -> const CardState* {
        for (auto& c : v) if ((int)c.seq == seq) return &c;
        return nullptr;
    };
    int uid = 0;

    // Top card of a pile (most recently entered) for the GY/BN tile preview.
    auto topGYCode = [&](int pl) -> uint32_t {
        return f.gy[pl].empty() ? 0u : f.gy[pl].back().code;
    };
    auto topBNCode = [&](int pl, bool& hidden) -> uint32_t {
        hidden = false;
        if (f.banished[pl].empty()) return 0u;
        const CardState& c = f.banished[pl].back();
        // A face-down banished card stays hidden from everyone but its owner.
        hidden = (c.pos & (POS_FACEDOWN_ATTACK | POS_FACEDOWN_DEFENSE)) != 0 &&
                 c.player != (uint8_t)m_net.localPlayerIndex();
        return c.code;
    };

    const ImVec4 COL_GY  = {0.42f,0.22f,0.07f,0.90f};
    const ImVec4 COL_ED  = {0.16f,0.14f,0.42f,0.90f};
    const ImVec4 COL_FZ  = {0.10f,0.34f,0.20f,0.90f};
    const ImVec4 COL_BN  = {0.30f,0.10f,0.10f,0.90f};
    const ImVec4 COL_DK  = {0.12f,0.26f,0.48f,0.90f};

    auto drawDeck = [&](const char* lbl, int count, ImVec2 sp) {
        void* back = m_rend.getBackTexture();
        if (count > 0 && back) {
            for (int s = 2; s >= 0; s--) {
                float o = s * 1.2f;
                dl->AddRectFilled({sp.x+o,sp.y+o},{sp.x+o+zW,sp.y+o+zH}, IM_COL32(32,16,20,220),5.f);
            }
            dl->AddImage((ImTextureID)back, sp, {sp.x+zW,sp.y+zH});
            // Modern count badge — centred red-rimmed pill.
            char cb[8]; snprintf(cb,8,"%d",count);
            ImVec2 ts=ImGui::CalcTextSize(cb);
            float bx=sp.x+(zW-ts.x)*0.5f, by=sp.y+zH-ts.y-7.f;
            dl->AddRectFilled({bx-9.f,by-3.f},{bx+ts.x+9.f,by+ts.y+3.f},
                              IM_COL32(16,9,11,218),10.f);
            dl->AddRect({bx-9.f,by-3.f},{bx+ts.x+9.f,by+ts.y+3.f},
                        IM_COL32(220,92,98,190),10.f,0,1.f);
            dl->AddText({bx,by},IM_COL32(255,212,212,255),cb);
        } else {
            drawSideZone(lbl, count, sp, zW, zH, COL_DK);
        }
    };

    // ─────────────────────────────────────────────────────────────────────────
    // TOP HAND (opponent — engine player `topP`, card backs only).
    // In MP-client view this is the HOST's hand, kept face-down so its
    // contents are NEVER revealed to the local player.
    // ─────────────────────────────────────────────────────────────────────────
    // Aspect-preserving card fit: a card drawn inside a (sw × sh) slot
    // keeps the real 421:614 card proportions, centred, NEVER stretched.
    // Both hand rows use this — their slots are zW × (0.7·zH), which used
    // to squash card art and backs vertically.
    auto fitCard = [](ImVec2 tl, float sw, float sh, ImVec2* a, ImVec2* b) {
        float ch = sh;
        float cw2 = ch * (421.f / 614.f);
        if (cw2 > sw) { cw2 = sw; ch = cw2 * (614.f / 421.f); }
        a->x = tl.x + (sw - cw2) * 0.5f;
        a->y = tl.y + (sh - ch)  * 0.5f;
        b->x = a->x + cw2;
        b->y = a->y + ch;
    };
    {
        void* back = m_rend.getBackTexture();
        int n = (int)f.hand[topP].size();
        dl->AddRectFilled({ox,rY[0]},{ox+gridW,rY[0]+hH}, IM_COL32(18,18,32,55), 4.f);
        if (n > 0) {
            float cw=zW, cg=GAP;
            float needed = n*cw+(n-1)*cg;
            if (needed > gridW) cw=(gridW-(n-1)*cg)/n;
            float sx = ox+(gridW-(n*cw+(n-1)*cg))*0.5f;
            for (int i=0;i<n;i++) {
                float hx=sx+i*(cw+cg);
                ImVec2 hp, hbr;
                fitCard({hx, rY[0]}, cw, hH, &hp, &hbr);
                if (back) dl->AddImage((ImTextureID)back,hp,hbr);
                else { dl->AddRectFilled(hp,hbr,IM_COL32(48,24,28,220),5.f);
                       dl->AddRect(hp,hbr,IM_COL32(120,56,62,200),5.f); }
            }
        } else {
            char lab[20]; snprintf(lab, 20, "P%d Hand", topP + 1);
            ImVec2 ts=ImGui::CalcTextSize(lab);
            dl->AddText({ox+(gridW-ts.x)*0.5f,rY[0]+(hH-ts.y)*0.5f},IM_COL32(55,65,95,130),lab);
        }
    }

    // Clickable hit-test over a side zone, used for the GY / Banished viewers.
    auto clickZone = [&](ImVec2 sp, const char* id) -> bool {
        ImGui::SetCursorScreenPos(sp);
        return ImGui::InvisibleButton(id, {zW, zH});
    };

    // ─────────────────────────────────────────────────────────────────────────
    // TOP SIDE ZONES — engine player `topP`. In MP-client view that's the
    // host (engine 0); in offline / host view it's engine 1.
    // col 0: DK rY[1] · GY rY[2] · BN rY[3];
    // col 6: ED rY[1] (Monster-row level) · FZ rY[2].
    // ─────────────────────────────────────────────────────────────────────────
    drawDeck     ("DK",  f.deckCount[topP],            {cx(0),rY[1]});
    drawSideZone ("GY", (int)f.gy[topP].size(),        {cx(0),rY[2]}, zW,zH, COL_GY,
                  topGYCode(topP));
    if (clickZone({cx(0),rY[2]}, "##zgytop")) {
        m_viewerPlayer=topP; m_viewerLoc=LOC_GY; m_viewerFilter[0]=0;
        m_dm.logEvent("Opened P" + std::to_string(topP+1) +
                      " Graveyard viewer (" +
                      std::to_string(f.gy[topP].size()) + " cards)");
    }
    { bool bnHidden; uint32_t bnTop = topBNCode(topP, bnHidden);
      drawSideZone ("BN", (int)f.banished[topP].size(), {cx(0),rY[3]}, zW,zH, COL_BN,
                    bnTop, bnHidden); }
    if (clickZone({cx(0),rY[3]}, "##zbntop")) {
        m_viewerPlayer=topP; m_viewerLoc=LOC_REM; m_viewerFilter[0]=0;
        m_dm.logEvent("Opened P" + std::to_string(topP+1) +
                      " Banished viewer (" +
                      std::to_string(f.banished[topP].size()) + " cards)");
    }

    // Extra Deck and Field zones on col 6 (top half). Hidden-info rule:
    // we never expose the opponent's Extra Deck list, so the top ED is
    // just a count badge — no clickable viewer.
    drawSideZone ("ED", f.extraCount[topP],            {cx(6),rY[1]}, zW,zH, COL_ED);
    {
        const CardState* fz2 = getCard(f.spells[topP], 5);
        drawCardZone("FZ", fz2, {cx(6),rY[2]}, zW,zH, false, uid++,
                     /*zonePlayer*/topP, /*zoneLoc*/LOC_SZONE, /*zoneSeq*/5);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TOP S/T ROW (engine player `topP`, cols 1-5, rY[1])
    // ─────────────────────────────────────────────────────────────────────────
    for (int i=0;i<5;i++) {
        const CardState* cs=getCard(f.spells[topP],i);
        char lbl[8]; snprintf(lbl,8,"ST%d",i+1);
        bool fd=cs&&!!(cs->pos&(POS_FACEDOWN_DEFENSE|POS_FACEDOWN_ATTACK));
        m_rectSZ_tl[topP][i] = {cx(1+i), rY[1]};
        m_rectSZ_br[topP][i] = {cx(1+i)+zW, rY[1]+zH};
        drawCardZone(lbl,cs,{cx(1+i),rY[1]},zW,zH,fd,uid++,
                     topP, LOC_SZONE, (uint32_t)i);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TOP MONSTER ROW (engine player `topP`, cols 1-5, rY[2])
    // ─────────────────────────────────────────────────────────────────────────
    for (int i=0;i<5;i++) {
        const CardState* cs=getCard(f.monsters[topP],i);
        char lbl[8]; snprintf(lbl,8,"M%d",i+1);
        bool fd=cs&&!!(cs->pos&(POS_FACEDOWN_DEFENSE|POS_FACEDOWN_ATTACK));
        drawCardZone(lbl,cs,{cx(1+i),rY[2]},zW,zH,fd,uid++,
                     topP, LOC_MZONE, (uint32_t)i);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // EMZ ROW (cols 2 and 4, rY[3]) — full zone size.
    // EMZ slots are LOCATION_MZONE sequence 5 (EMZ1) and 6 (EMZ2), SHARED
    // between players: either P1 or P2 may own the card in either slot. Look
    // the card up in both players' monster vectors before falling back to a
    // decorative empty tile.
    auto findEMZ = [&](uint32_t seq) -> const CardState* {
        for (int pl = 0; pl < 2; ++pl)
            for (auto& c : f.monsters[pl])
                if (c.seq == seq) return &c;
        return nullptr;
    };
    for (int e = 0; e < 2; ++e) {
        uint32_t seq = 5 + (uint32_t)e;   // EMZ1 -> 5, EMZ2 -> 6
        const CardState* cs = findEMZ(seq);
        float ex = cx(2 + e * 2), ey = rY[3];
        char lbl[8]; snprintf(lbl, 8, "EMZ%d", e + 1);
        if (cs && cs->code) {
            if (m_debugLog)
                m_dm.logEvent("[EMZ DEBUG] card #" + std::to_string(cs->code) +
                              " loc=" + std::to_string(cs->loc) +
                              " seq=" + std::to_string(cs->seq) +
                              " controller=P" + std::to_string(cs->player + 1) +
                              " mapped to EMZ slot " + std::to_string(e + 1));
            bool fd = (cs->pos & (POS_FACEDOWN_DEFENSE | POS_FACEDOWN_ATTACK)) != 0;
            drawCardZone(lbl, cs, {ex, ey}, zW, zH, fd, uid++,
                         (int)cs->player, LOC_MZONE, seq);
        } else {
            // Decorative EMZ tile — violet glass with a hairline so the
            // shared zones read as "special" without shouting.
            dl->AddRectFilledMultiColor({ex, ey}, {ex + zW, ey + zH},
                IM_COL32(64, 50, 130, 175), IM_COL32(64, 50, 130, 175),
                IM_COL32(30, 24, 66, 195),  IM_COL32(30, 24, 66, 195));
            dl->AddRect({ex, ey}, {ex + zW, ey + zH},
                        IM_COL32(132, 110, 210, 150), 6.f, 0, 1.2f);
            // Run an empty drawCardZone so placement-mode glow + click work
            // here too (engine may offer EMZ as a legal placement target).
            // zonePlayer = the player currently being asked to place; both
            // EMZ slots are shared so we tag them with the asker.
            int askerP = (int)currentSelection().player;
            drawCardZone(lbl, nullptr, {ex, ey}, zW, zH, false, uid++,
                         askerP, LOC_MZONE, seq);
        }
    }
    // Fill the genuinely-empty EMZ-row cols (1,3,5). Col 0 = P2 Banish and
    // col 6 = P1 Banish, both drawn with the side zones.
    for (int c=1;c<=5;c++) {
        if (c==2||c==4) continue;
        float ex=cx(c),ey=rY[3];
        dl->AddRectFilled({ex,ey},{ex+zW,ey+zH}, IM_COL32(10,12,22,120),5.f);
        dl->AddRect      ({ex,ey},{ex+zW,ey+zH}, IM_COL32(40,50,80,80),5.f);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // BOTTOM SIDE ZONES (local player, engine `botP`). col 0: FZ rY[4] /
    // ED rY[5]; col 6: BN rY[3] / GY rY[4] / DK rY[5].
    // ─────────────────────────────────────────────────────────────────────────
    {
        const CardState* fz1 = getCard(f.spells[botP], 5);
        drawCardZone("FZ", fz1, {cx(0),rY[4]}, zW,zH, false, uid++,
                     botP, LOC_SZONE, 5);
    }
    drawSideZone("ED", f.extraCount[botP], {cx(0),rY[5]}, zW,zH, COL_ED);
    // Legal-action glow on the ED pile: outline when any idle action
    // targets an Extra Deck card for the local engine player.
    {
        bool edHasAction = false;
        for (const auto& a : currentSelection().idle)
            if (a.con == botP && a.loc == LOC_EXTRA) { edHasAction = true; break; }
        if (edHasAction) {
            dl->AddRectFilled({cx(0),rY[5]}, {cx(0)+zW, rY[5]+zH},
                              IM_COL32(255, 200, 90, 60), 5.f);
            dl->AddRect({cx(0),rY[5]}, {cx(0)+zW, rY[5]+zH},
                        IM_COL32(255, 200, 90, 230), 5.f, 0, 2.6f);
        }
    }
    // The local player can view their own Extra Deck (hidden info rule).
    if (clickZone({cx(0),rY[5]}, "##zedbot")) {
        m_viewerPlayer = botP; m_viewerLoc = LOC_EXTRA;
        m_viewerFilter[0] = 0;
        m_viewerExtraCache = viewerExtraDeckCodes(botP);
        m_dm.logEvent("Opened P" + std::to_string(botP+1) +
                      " Extra Deck viewer (" +
                      std::to_string(m_viewerExtraCache.size()) + " cards)");
    }
    // Pile-effect glow: when the local player has an activatable effect in a
    // pile (GY / Banished), the tile gets a magenta "effect available" glow so
    // it's discoverable — clicking opens the viewer where the effect can be
    // activated. This is the fix for "I can't activate the GY effect": the
    // effect was always reachable via the viewer, but the tile gave no cue.
    auto pileHasLocalAction = [&](uint8_t loc) -> bool {
        const SelectionRequest& s = currentSelection();
        if (s.type != WaitType::SelectIdleCmd &&
            s.type != WaitType::SelectBattleCmd) return false;
        for (const auto& a : s.idle)
            if (a.con == (uint8_t)botP && a.loc == loc) return true;
        return false;
    };
    auto pileEffectGlow = [&](ImVec2 tl, bool on) {
        if (!on) return;
        ImVec2 z1{tl.x + zW, tl.y + zH};
        UIStyle::DrawGlow(dl, tl, z1, IM_COL32(244, 132, 232, 230), 5.f, 3);
        dl->AddRect(tl, z1, IM_COL32(255, 150, 240, 255), 5.f, 0, 2.6f);
        // Small "EFFECT" tag at the top of the tile.
        dl->AddRectFilled({tl.x + 2.f, tl.y + 2.f},
                          {tl.x + 46.f, tl.y + 15.f},
                          IM_COL32(120, 30, 110, 220), 3.f);
        dl->AddText({tl.x + 5.f, tl.y + 2.f},
                    IM_COL32(255, 220, 250, 255), "EFFECT");
    };
    bool bnAct = pileHasLocalAction(LOC_REM);
    bool gyAct = pileHasLocalAction(LOC_GY);
    { bool bnHidden; uint32_t bnTop = topBNCode(botP, bnHidden);
      drawSideZone ("BN", (int)f.banished[botP].size(), {cx(6),rY[3]}, zW,zH, COL_BN,
                    bnTop, bnHidden); }
    pileEffectGlow({cx(6),rY[3]}, bnAct);
    if (clickZone({cx(6),rY[3]}, "##zbnbot")) {
        m_viewerPlayer=botP; m_viewerLoc=LOC_REM; m_viewerFilter[0]=0;
        int actN = 0;
        for (const auto& a : currentSelection().idle)
            if (a.con==(uint8_t)botP && a.loc==LOC_REM) ++actN;
        m_dm.logEvent("[GY VIEWER] loc=BN player=" + std::to_string(botP) +
                      " count=" + std::to_string(f.banished[botP].size()) +
                      " activatable=" + std::to_string(actN));
    }
    drawSideZone ("GY", (int)f.gy[botP].size(),       {cx(6),rY[4]}, zW,zH, COL_GY,
                  topGYCode(botP));
    pileEffectGlow({cx(6),rY[4]}, gyAct);
    if (clickZone({cx(6),rY[4]}, "##zgybot")) {
        m_viewerPlayer=botP; m_viewerLoc=LOC_GY; m_viewerFilter[0]=0;
        int actN = 0;
        for (const auto& a : currentSelection().idle)
            if (a.con==(uint8_t)botP && a.loc==LOC_GY) ++actN;
        m_dm.logEvent("[GY VIEWER] loc=GY player=" + std::to_string(botP) +
                      " count=" + std::to_string(f.gy[botP].size()) +
                      " activatable=" + std::to_string(actN));
    }
    drawDeck     ("DK",  f.deckCount[botP],           {cx(6),rY[5]});

    // ─────────────────────────────────────────────────────────────────────────
    // BOTTOM MONSTER ROW (engine player `botP`, cols 1-5, rY[4])
    // ─────────────────────────────────────────────────────────────────────────
    for (int i=0;i<5;i++) {
        const CardState* cs=getCard(f.monsters[botP],i);
        char lbl[8]; snprintf(lbl,8,"M%d",i+1);
        bool fd=cs&&!!(cs->pos&(POS_FACEDOWN_DEFENSE|POS_FACEDOWN_ATTACK));
        drawCardZone(lbl,cs,{cx(1+i),rY[4]},zW,zH,fd,uid++,
                     botP, LOC_MZONE, (uint32_t)i);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // BOTTOM S/T ROW (engine player `botP`, cols 1-5, rY[5])
    // ─────────────────────────────────────────────────────────────────────────
    for (int i=0;i<5;i++) {
        const CardState* cs=getCard(f.spells[botP],i);
        char lbl[8]; snprintf(lbl,8,"ST%d",i+1);
        bool fd=cs&&!!(cs->pos&(POS_FACEDOWN_DEFENSE|POS_FACEDOWN_ATTACK));
        m_rectSZ_tl[botP][i] = {cx(1+i), rY[5]};
        m_rectSZ_br[botP][i] = {cx(1+i)+zW, rY[5]+zH};
        drawCardZone(lbl,cs,{cx(1+i),rY[5]},zW,zH,fd,uid++,
                     botP, LOC_SZONE, (uint32_t)i);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // BOTTOM HAND (local player, engine `botP`, face-up)
    // ─────────────────────────────────────────────────────────────────────────
    {
        int n=(int)f.hand[botP].size();
        if (n>0) {
            float cw=zW,cg=GAP;
            float needed=n*cw+(n-1)*cg;
            if (needed>gridW) cw=(gridW-(n-1)*cg)/n;
            float sx=ox+(gridW-(n*cw+(n-1)*cg))*0.5f;
            for (int i=0;i<n;i++) {
                auto& c=f.hand[botP][i];
                float hx=sx+i*(cw+cg);
                // Aspect-true card rect inside the slot (slot stays the
                // hit-test area; art + outlines use the fitted rect).
                ImVec2 hp, hbr;
                fitCard({hx, rY[6]}, cw, hH, &hp, &hbr);
                void* tex=m_rend.getCardTexture(c.code);
                // Hand cards are always upright — shared helper guarantees
                // non-flipped UVs (Pendulum monsters included).
                if (tex) drawCardArt(dl, c.code, tex, hp, hbr,
                                     /*rotateDefenseCW*/false, /*dbgCheck*/true);
                else {
                    dl->AddRectFilled(hp,hbr,IM_COL32(28,36,62,220),5.f);
                    dl->AddRect(hp,hbr,IM_COL32(65,78,118,200),5.f);
                    CardInfo ci=m_db.getCard(c.code);
                    if (!ci.name.empty())
                        dl->AddText({hp.x+3.f,hp.y+4.f},IM_COL32(170,185,220,230),ci.name.c_str());
                }
                dl->AddRect(hp,hbr,IM_COL32(70,90,140,70),5.f);

                // Selected outline (cyan) / legal-action glow (orange) /
                // chain candidate (magenta). Hand uses LOC_HAND=2 and c.seq
                // is the hand index — matches the engine's chain entry.
                int hChainIdx = -1;
                bool hChainHere = isChainCandidate(c.player, (uint8_t)c.loc,
                                                   c.seq, &hChainIdx);
                int hSelIdx = -1;
                bool hSelHere = isSelectCandidate(c.player, (uint8_t)c.loc,
                                                  c.seq, &hSelIdx);
                if (isSelectedCard(c))
                    dl->AddRect(hp,hbr,IM_COL32(120,230,255,255),5.f,0,3.f);
                else if (hSelHere) {
                    UIStyle::DrawGlow(dl, hp, hbr, IM_COL32(110,230,140,220), 5.f, 3);
                    dl->AddRect(hp,hbr,IM_COL32(140,255,165,255),5.f,0,3.f);
                }
                else if (hChainHere)
                    dl->AddRect(hp,hbr,IM_COL32(255,120,240,255),5.f,0,3.f);
                else if (m_showLegalGlow &&
                         hasLegalActionFor(c.player, (uint8_t)c.loc, c.seq))
                    dl->AddRect(hp,hbr,IM_COL32(255,200,90,200),5.f,0,2.2f);

                // Freshly drawn cards (#B): a brief blue pulse on the newest
                // tiles (drawn cards are appended, so they're the last ones).
                if (m_newDrawCount > 0 && i >= n - m_newDrawCount) {
                    double age = ImGui::GetTime() - m_newDrawAt;
                    if (age >= 0.0 && age < 1.6) {
                        float fade = 1.f - (float)(age / 1.6);
                        float pl = 0.5f + 0.5f * sinf((float)ImGui::GetTime()*7.f);
                        ImU32 g = IM_COL32(120, 210, 255,
                                           (int)((90 + 110 * pl) * fade));
                        UIStyle::DrawGlow(dl, hp, hbr, g, 5.f, 3);
                    }
                }

                ImGui::SetCursorScreenPos({hx, rY[6]});   // full slot = hit area
                char hid[24]; snprintf(hid,24,"##h%d",i);
                bool hClicked = ImGui::InvisibleButton(hid,{cw,hH});
                if (ImGui::IsItemHovered()) {
                    m_hoveredInfo=m_db.getCard(c.code);
                    m_hoveredCard=c.code;
                    setInfoCtx(c.player, (uint8_t)c.loc, c.seq, c.pos);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        if (!tryOpenCardContext(c.player, (uint8_t)c.loc,
                                                c.seq, c.code))
                            m_zoomCard = c.code;      // pin large card reader
                    }
                    // Hover lift — re-draw the card slightly larger and
                    // raised with a shadow so the hand feels tactile. The
                    // outline + glow colour reflects the card's state:
                    // magenta = chain candidate, gold = legal action,
                    // cyan = plain hover (local-player accent).
                    if (tex) {
                        float lw = (hbr.x - hp.x) * 0.06f;
                        float lh = (hbr.y - hp.y) * 0.06f;
                        ImVec2 la = {hp.x - lw,  hp.y - lh - 9.f};
                        ImVec2 lb = {hbr.x + lw, hbr.y + lh - 9.f};
                        ImU32 stateCol =
                            hChainHere ? IM_COL32(255, 120, 240, 235)
                          : hasLegalActionFor(c.player, (uint8_t)c.loc, c.seq)
                                       ? IM_COL32(255, 206, 100, 235)
                                       : IM_COL32(140, 215, 255, 235);
                        UIStyle::DrawGlow(dl, la, lb, stateCol, 5.f, 3);
                        dl->AddRectFilled({la.x + 3.f, la.y + 6.f},
                                          {lb.x + 3.f, lb.y + 6.f},
                                          IM_COL32(0, 0, 0, 160), 5.f);
                        drawCardArt(dl, c.code, tex, la, lb, false, false);
                        dl->AddRect(la, lb, stateCol, 5.f, 0, 2.f);
                    } else {
                        dl->AddRect(hp,hbr,IM_COL32(180,210,255,230),5.f,0,2.f);
                    }
                    // Type-aware: Spells/Traps must NEVER read "ATK/DEF".
                    const char* line =
                        (m_hoveredInfo.type & TYPE_SPELL) ? "Spell card" :
                        (m_hoveredInfo.type & TYPE_TRAP)  ? "Trap card"  :
                                                           nullptr;
                    if (line)
                        ImGui::SetTooltip("%s\n%s",
                            m_hoveredInfo.name.c_str(), line);
                    else if (m_hoveredInfo.type & TYPE_MONSTER) {
                        if (m_hoveredInfo.type & 0x4000000) {
                            int rating = m_hoveredInfo.def > 0
                                ? m_hoveredInfo.def : (int)m_hoveredInfo.level;
                            ImGui::SetTooltip("%s\nATK %d  LINK-%d",
                                m_hoveredInfo.name.c_str(),
                                m_hoveredInfo.atk, rating);
                        } else {
                            ImGui::SetTooltip("%s\nATK %d / DEF %d",
                                m_hoveredInfo.name.c_str(),
                                m_hoveredInfo.atk, m_hoveredInfo.def);
                        }
                    } else {
                        ImGui::SetTooltip("%s", m_hoveredInfo.name.c_str());
                    }
                }
                if (hClicked) {
                    // Selection candidate: click a glowing green hand card to
                    // submit it as target / summon material.
                    if (hSelHere && hSelIdx >= 0) {
                        const SelectionRequest& s = currentSelection();
                        gAudio().play("confirm");
                        if (s.type == WaitType::SelectUnselect) submitUnselect(hSelIdx);
                        else                                    submitMpChoice(s.type, hSelIdx);
                    } else
                    // Chain response: click a glowing magenta hand card to
                    // send respondChain(index) directly.
                    if (hChainHere && hChainIdx >= 0) {
                        m_dm.logEvent("[CHAIN CLICK] card #" +
                                      std::to_string(c.code) + " chainIdx=" +
                                      std::to_string(hChainIdx));
                        submitMpChoice(WaitType::SelectChain, hChainIdx);
                        clearSelection();
                    } else {
                        selectCardFrom(c);
                        // Anchor the floating action popup just above the
                        // hand card so it appears next to the click.
                        m_actionAnchorX = (hp.x + hbr.x) * 0.5f;
                        m_actionAnchorY = hp.y;
                    }
                }
            }
        }
    }

    // ── Animation pass — paints on TOP of the field so pulses/rings/LP
    //    flashes / damage numbers / zone flashes appear above the zones
    //    they're anchored to. Anims auto-evict themselves on render.
    m_anim.render(dl);

    ImGui::SetCursorScreenPos({wPos.x, wPos.y+(float)fh-1.f});
    ImGui::Dummy({1.f,1.f});
}

// ─── drawSelectionPanel (content-only) ───────────────────────────────────────
void UI::drawOpponentActionHint() {
    // What the opponent (AI or remote player) is attempting, captured by the
    // engine on the last summon / activation / attack. Only shown when this is
    // genuinely a response to the OPPONENT (their turn) — never on the player's
    // own-turn quick effects / triggers, where the captured action is stale.
    if (currentSelection().timing != TimingContext::OpponentChainWindow) return;
    const std::string& desc = m_dm.lastActionDesc();
    if (desc.empty()) return;
    if (m_dm.lastActionPlayer() == (uint8_t)m_net.localPlayerIndex()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 178, 92, 255));
    ImGui::TextWrapped("Opponent is %s", desc.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void UI::drawChainResponsePopup(int w, int h) {
    const SelectionRequest& sel = currentSelection();
    // Only during the LOCAL player's chain window. (Card targeting / other
    // prompts keep their own UI.)
    if (sel.type != WaitType::SelectChain ||
        sel.player != (uint8_t)m_net.localPlayerIndex())
        return;

    // Centred near the top so it never sits over the player's own glowing
    // cards (hand + backrow) that they click to chain. Non-modal: the field
    // stays clickable so a card can be chosen as the response.
    ImGui::SetNextWindowPos({w * 0.5f, h * 0.13f}, ImGuiCond_Always, {0.5f, 0.f});
    ImGui::SetNextWindowSize({460.f, 0.f});
    // Pulse the magenta border so the "you can respond" window catches the eye
    // — easy to miss otherwise during the opponent's turn.
    float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4.5f);   // 0..1
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(26, 20, 36, 250));
    ImGui::PushStyleColor(ImGuiCol_Border,
        IM_COL32(232, 140, 224, (int)(150 + 105 * pulse)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.f + 2.f * pulse);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{18.f, 14.f});
    ImGui::Begin("##chain_response", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

    UIStyle::PushFont(UIStyle::fHeader);
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertU32ToFloat4(UIStyle::C().accentHi));
    ImGui::TextUnformatted(sel.forced ? "You must chain a card"
                                      : "Respond?");
    ImGui::PopStyleColor();
    UIStyle::PopFont();

    // What triggered this — only when genuinely responding to the opponent.
    if (sel.timing == TimingContext::OpponentChainWindow &&
        !m_dm.lastActionDesc().empty() &&
        m_dm.lastActionPlayer() != (uint8_t)m_net.localPlayerIndex()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 184, 100, 255));
        ImGui::TextWrapped("Opponent is %s", m_dm.lastActionDesc().c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(190, 200, 220, 230));
    ImGui::TextWrapped("Click a glowing card on the field to chain it"
                       "%s.", sel.forced ? "" : ", or pass");
    ImGui::PopStyleColor();
    ImGui::Dummy({1.f, 10.f});

    if (!sel.forced) {
        bool pass = UIStyle::PrimaryButton("Pass / No Response", {-1.f, 36.f});
        // Right-click anywhere = pass, for fast no-responses. Skip when the
        // same click just pinned a card zoom (right-click-to-zoom on a glowing
        // card still works); m_zoomCard is set earlier this frame by the field
        // renderer, which runs before this popup.
        if (!pass && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
            m_zoomCard == 0)
            pass = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tip: right-click anywhere = pass");
        if (pass) {
            gAudio().play("cancel");
            submitMpChoice(WaitType::SelectChain, -1);
            clearSelection();
        }
    } else {
        ImGui::TextDisabled("This chain is forced — you must respond.");
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void UI::drawSelectionPanel(int pw, int ph) {
    // ph = the modal's height CAP (maxH). Inner scrollable lists size their
    // child against the room left after the panel's fixed chrome, so the
    // auto-resizing window grows to fit content and never shows empty space.
    const float kHeightCap = (ph > 0) ? (float)ph : 560.f;
    // The card preview was moved to a SEPARATE compact overlay
    // (drawCompactPreviewOverlay) so card descriptions can never reshape this
    // panel. The block below would normally show a preview here — it's left
    // as a no-op placeholder so the rest of the function (viewer / prompts)
    // renders below at a stable Y. We still keep the legacy block guarded so
    // the helper code paths inside it compile, but it never produces UI.
    if (false) {
    ImGui::BeginChild("##preview_box_legacy", {(float)pw - 8.f, 1.f}, false);
    if (m_hoveredCard && m_hoveredInfo.id) {
        void* tex = m_rend.getCardTexture(m_hoveredCard);
        float imgW = (float)pw - 24.f;
        float imgH = std::min(imgW * (614.f / 421.f), 170.f);
        imgW = imgH * (421.f / 614.f);
        if (tex) ImGui::Image(tex, {imgW, imgH});
        else     ImGui::Dummy({imgW, imgH});
        ImGui::TextColored(COL_ACCENT, "%s", m_hoveredInfo.name.c_str());

        const auto& ci = m_hoveredInfo;
        // Sub-type breakdown (Monster / Spell / Trap + extra flags).
        std::string sub;
        auto addSub = [&](const char* s){ if (!sub.empty()) sub += " / "; sub += s; };
        if (ci.type & TYPE_MONSTER) {
            addSub("Monster");
            if (ci.type & 0x20)        addSub("Effect");
            if (ci.type & 0x10)        addSub("Normal");
            if (ci.type & 0x1000)      addSub("Tuner");
            if (ci.type & 0x40)        addSub("Fusion");
            if (ci.type & 0x2000)      addSub("Synchro");
            if (ci.type & 0x800000)    addSub("Xyz");
            if (ci.type & 0x1000000)   addSub("Pendulum");
            if (ci.type & 0x4000000)   addSub("Link");
        } else if (ci.type & TYPE_SPELL) {
            addSub("Spell");
            if (ci.type & 0x10000)     addSub("Quick-Play");
            if (ci.type & 0x20000)     addSub("Continuous");
            if (ci.type & 0x40000)     addSub("Equip");
            if (ci.type & 0x80000)     addSub("Field");
            if (ci.type & 0x80)        addSub("Ritual");
        } else if (ci.type & TYPE_TRAP) {
            addSub("Trap");
            if (ci.type & 0x20000)     addSub("Continuous");
            if (ci.type & 0x100000)    addSub("Counter");
        }
        if (!sub.empty())
            ImGui::TextDisabled("%s", sub.c_str());

        if (ci.type & TYPE_MONSTER) {
            if (ci.type & 0x4000000) {
                // Link rating sits in the `def` field for Link monsters in
                // most ProjectIgnis builds; print whichever value is positive.
                int rating = ci.def > 0 ? ci.def : (int)ci.level;
                ImGui::TextDisabled("ATK %d   LINK-%d", ci.atk, rating);
            } else if (ci.type & 0x800000) {
                ImGui::TextDisabled("ATK %d / DEF %d   Rank %d",
                                    ci.atk, ci.def, ci.level);
            } else {
                ImGui::TextDisabled("ATK %d / DEF %d   Lv%d",
                                    ci.atk, ci.def, ci.level);
            }
        }

        if (!ci.desc.empty()) {
            // Cap to keep panel content visible; effect text in cards.cdb can
            // run several hundred chars and would push action buttons off-panel.
            std::string d = ci.desc;
            if (d.size() > 320) d = d.substr(0, 318) + "..";
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (float)pw - 16.f);
            ImGui::TextWrapped("%s", d.c_str());
            ImGui::PopTextWrapPos();
        }
    } else {
        ImGui::TextDisabled("Hover or click a card to preview it.");
    }
    ImGui::EndChild();   // ##preview_box_legacy
    }   // close the `if (false)` legacy preview guard

    // Content width: clamp to the actual region so the modal's window
    // padding (now 16px per side) never causes 2-3px row clipping.
    float bw  = std::min((float)pw - 16.f, ImGui::GetContentRegionAvail().x);

    // ── GY / Banished / Extra Deck viewer ────────────────────────────────────
    // Opened by clicking a GY / BN / ED zone tile. Shows a scrollable, search-
    // filtered list with thumbnails + name + code + type + sequence. Clicking
    // a row previews the card above. Hidden-info rule: the GY and Banished
    // are public for both players; Extra Deck contents are only viewable for
    // the local player (P0). The opening click in drawField enforces that —
    // P2's ED tile has no click handler.
    if (m_viewerLoc != 0) {
        const FieldState& fv = currentField();
        int pl = (m_viewerPlayer == 0 || m_viewerPlayer == 1) ? m_viewerPlayer : 0;
        const int kViewerLocal = m_net.localPlayerIndex();
        bool ownZone = (pl == kViewerLocal);

        const char* zname =
            (m_viewerLoc == LOC_GY)    ? "Graveyard" :
            (m_viewerLoc == LOC_REM)   ? "Banished"  :
            (m_viewerLoc == LOC_EXTRA) ? "Extra Deck": "Zone";
        const char* zshort =
            (m_viewerLoc == LOC_GY)  ? "GY" :
            (m_viewerLoc == LOC_REM) ? "BN" :
            (m_viewerLoc == LOC_EXTRA) ? "ED" : "ZN";
        uint8_t expectedLoc =
            (m_viewerLoc == LOC_EXTRA) ? LOC_EXTRA :
            (m_viewerLoc == LOC_GY)    ? LOC_GY    :
            (m_viewerLoc == LOC_REM)   ? LOC_REM   : 0;

        // Build a uniform row list of (code, real-seq). For GY/BN we use the
        // CardState's REAL engine sequence (not the array index) so action
        // matching against idle/chain entries is robust even if the pile
        // ordering and the engine's seq numbering differ.
        struct Row { uint32_t code; uint32_t seq; };
        std::vector<Row> rows;
        if (m_viewerLoc == LOC_GY) {
            for (const auto& c : fv.gy[pl]) rows.push_back({c.code, c.seq});
        } else if (m_viewerLoc == LOC_REM) {
            for (const auto& c : fv.banished[pl]) rows.push_back({c.code, c.seq});
        } else if (m_viewerLoc == LOC_EXTRA) {
            if (m_viewerExtraCache.empty())
                m_viewerExtraCache = viewerExtraDeckCodes(pl);
            for (size_t i = 0; i < m_viewerExtraCache.size(); ++i)
                rows.push_back({m_viewerExtraCache[i], (uint32_t)i});
        }

        // Owner-aware title: "Your Graveyard" / "Opponent Graveyard".
        std::string ownerLbl =
            (m_mpInDuel && !m_net.isOffline())
                ? (ownZone ? std::string("Your ") + zname
                           : std::string("Opponent ") + zname)
                : (std::string("P") + std::to_string(pl + 1) + " " + zname);
        UIStyle::PushFont(UIStyle::fHeader);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(UIStyle::C().accentHi));
        ImGui::Text("%s", ownerLbl.c_str());
        ImGui::PopStyleColor();
        UIStyle::PopFont();
        // Count + activatable-effect count.
        int activatableTotal = 0;
        {
            const SelectionRequest& s = currentSelection();
            if (expectedLoc && ownZone) {
                if (s.type == WaitType::SelectIdleCmd ||
                    s.type == WaitType::SelectBattleCmd) {
                    for (const auto& a : s.idle)
                        if (a.con == (uint8_t)kViewerLocal && a.loc == expectedLoc)
                            ++activatableTotal;
                } else if (s.type == WaitType::SelectChain) {
                    for (const auto& c : s.cards)
                        if (c.player == (uint8_t)kViewerLocal &&
                            (uint8_t)c.loc == expectedLoc) ++activatableTotal;
                }
            }
        }
        ImGui::TextDisabled("%d card%s%s", (int)rows.size(),
            rows.size() == 1 ? "" : "s",
            activatableTotal > 0
                ? ("  ·  " + std::to_string(activatableTotal) +
                   " activatable").c_str() : "");

        // Active-prompt hint for NON-activatable prompts (yes-no / select card)
        // that the viewer would mask.
        {
            const SelectionRequest& s = currentSelection();
            if (DuelManager::isRealSelect(s.type) &&
                s.type != WaitType::SelectIdleCmd &&
                s.type != WaitType::SelectBattleCmd &&
                s.type != WaitType::SelectChain &&
                s.player == (uint8_t)kViewerLocal) {
                ImGui::TextColored({1.f, 0.8f, 0.3f, 1.f},
                    "Engine is waiting for a response — close this viewer.");
            }
        }

        if (UIStyle::GhostButton("Close##zv", {bw, 26.f})) {
            m_viewerLoc = 0;
            m_viewerExtraCache.clear();
            m_viewerFilter[0] = 0;
            return;
        }

        // Search + filter chips ONLY when the pile has cards — an empty pile
        // shows a compact empty-state instead of an unusable search row.
        static bool fMon = true, fSpl = true, fTrp = true, fActOnly = false;
        std::string flt;
        if (!rows.empty()) {
            ImGui::SetNextItemWidth(bw);
            ImGui::InputTextWithHint("##zvfilter", "Search name or #code",
                                     m_viewerFilter, sizeof(m_viewerFilter));
            if (UIStyle::SegmentedButton("Monster##gf", fMon, true, {78.f, 24.f})) fMon = !fMon;
            ImGui::SameLine(0.f, 4.f);
            if (UIStyle::SegmentedButton("Spell##gf", fSpl, true, {62.f, 24.f})) fSpl = !fSpl;
            ImGui::SameLine(0.f, 4.f);
            if (UIStyle::SegmentedButton("Trap##gf", fTrp, true, {58.f, 24.f})) fTrp = !fTrp;
            if (expectedLoc && ownZone) {
                ImGui::SameLine(0.f, 4.f);
                if (UIStyle::SegmentedButton("Activatable##gf", fActOnly, true, {100.f, 24.f}))
                    fActOnly = !fActOnly;
            }
            flt = m_viewerFilter;
            for (char& c : flt) c = (char)tolower((unsigned char)c);
        }

        // Resolve the action for a row: idle (cmd 1/5) OR chain candidate.
        // Returns kind: 0 none, 1 idle, 2 chain. For idle, matches by
        // con+loc+code+seq, falling back to "single action of this code+loc"
        // when seq numbering doesn't line up (robustness). For chain, matches
        // the SelectChain candidate list by player+loc+code(+seq).
        struct RowAct { int kind=0; int idleIdx=-1; int chainIdx=-1;
                        int cmd=0; std::string label; };
        auto resolveRowAct = [&](uint32_t code, uint32_t seq) -> RowAct {
            RowAct out;
            if (!expectedLoc || !ownZone) return out;
            const SelectionRequest& s = currentSelection();
            if (s.type == WaitType::SelectIdleCmd ||
                s.type == WaitType::SelectBattleCmd) {
                int exact = -1, codeMatch = -1, codeMatches = 0;
                for (int k = 0; k < (int)s.idle.size(); ++k) {
                    const IdleAction& a = s.idle[k];
                    if (a.con != (uint8_t)kViewerLocal || a.loc != expectedLoc ||
                        a.code != code) continue;
                    ++codeMatches; codeMatch = k;
                    if (a.seq == seq) { exact = k; break; }
                }
                int use = (exact >= 0) ? exact
                        : (codeMatches == 1) ? codeMatch : -1;
                if (use >= 0) {
                    out.kind = 1; out.idleIdx = use; out.cmd = s.idle[use].cmd;
                    const IdleAction& a = s.idle[use];
                    const char* verb = (a.cmd == 1) ? "Special Summon"
                                                    : "Activate effect";
                    out.label = verb;
                    if (a.cmd == 5 && !a.effect.text.empty()) {
                        std::string et = a.effect.text;
                        if (et.size() > 32) et = et.substr(0, 30) + "..";
                        out.label = std::string(verb) + ": " + et;
                    }
                }
            } else if (s.type == WaitType::SelectChain) {
                int exact = -1, codeMatch = -1, codeMatches = 0;
                for (int k = 0; k < (int)s.cards.size(); ++k) {
                    const CardState& c = s.cards[k];
                    if (c.player != (uint8_t)kViewerLocal ||
                        (uint8_t)c.loc != expectedLoc || c.code != code) continue;
                    ++codeMatches; codeMatch = k;
                    if (c.seq == seq) { exact = k; break; }
                }
                int use = (exact >= 0) ? exact
                        : (codeMatches == 1) ? codeMatch : -1;
                if (use >= 0) {
                    out.kind = 2; out.chainIdx = use; out.cmd = 5;
                    out.label = "Activate (chain)";
                }
            }
            return out;
        };

        // ── Empty pile → compact centred empty-state, no big list box ──────
        if (rows.empty()) {
            ImGui::Dummy({1.f, 14.f});
            ImDrawList* edl = ImGui::GetWindowDrawList();
            ImVec2 ep = ImGui::GetCursorScreenPos();
            float cxp = ep.x + bw * 0.5f;
            // Subtle pile glyph.
            edl->AddCircle({cxp, ep.y + 14.f}, 13.f,
                           IM_COL32(120, 134, 170, 120), 20, 1.6f);
            edl->AddLine({cxp - 7.f, ep.y + 14.f}, {cxp + 7.f, ep.y + 14.f},
                         IM_COL32(120, 134, 170, 120), 1.6f);
            std::string msg = std::string("No cards in ") +
                (m_mpInDuel && !m_net.isOffline()
                    ? (ownZone ? "your " : "the opponent's ") : "this ") +
                std::string(zname);
            ImGui::Dummy({1.f, 34.f});
            ImVec2 ts = ImGui::CalcTextSize(msg.c_str());
            ImGui::SetCursorPosX((bw - ts.x) * 0.5f + 8.f);
            ImGui::TextDisabled("%s", msg.c_str());
            ImGui::Dummy({1.f, 8.f});
            if (m_debugLog)
                m_dm.logEvent(std::string("[VIEWER SIZE] loc=") + zshort +
                    " count=0 size=" + std::to_string((int)bw) +
                    ",compact scroll=no");
            return;
        }

        // ── Pre-count VISIBLE rows so the list child fits exactly ──────────
        auto rowVisible = [&](size_t i) -> bool {
            CardInfo ci = m_db.getCard(rows[i].code);
            bool isMon = (ci.type & TYPE_MONSTER) != 0;
            bool isSpl = (ci.type & TYPE_SPELL)   != 0;
            bool isTrp = (ci.type & TYPE_TRAP)    != 0;
            if (isMon && !fMon) return false;
            if (isSpl && !fSpl) return false;
            if (isTrp && !fTrp) return false;
            if (!flt.empty()) {
                std::string lc = (ci.name.empty()
                    ? ("#" + std::to_string(rows[i].code)) : ci.name) +
                    "#" + std::to_string(rows[i].code);
                for (char& c : lc) c = (char)tolower((unsigned char)c);
                if (lc.find(flt) == std::string::npos) return false;
            }
            if (fActOnly && ownZone && expectedLoc &&
                resolveRowAct(rows[i].code, rows[i].seq).kind == 0) return false;
            return true;
        };
        int visibleRows = 0;
        for (size_t i = 0; i < rows.size(); ++i) if (rowVisible(i)) ++visibleRows;

        // Content-driven list height: rows*rowH, capped at the room left under
        // the modal cap. Scrolls only when the rows exceed that cap. rowH is
        // slightly generous (rows with an Activate button are taller) so a
        // perfectly-fitting list doesn't sprout a 1px scrollbar.
        const float kRowH = 62.f;
        float maxListH = std::max(120.f, kHeightCap - 210.f);
        float wantH = (visibleRows > 0) ? (visibleRows * kRowH + 6.f) : 40.f;
        float listH = std::min(maxListH, wantH);
        bool  listScroll = (wantH > maxListH);
        if (m_debugLog)
            m_dm.logEvent(std::string("[VIEWER SIZE] loc=") + zshort +
                " count=" + std::to_string(visibleRows) +
                " size=" + std::to_string((int)bw) + "," +
                std::to_string((int)listH) +
                " scroll=" + (listScroll ? "yes" : "no"));

        ImGui::BeginChild("##zvlist", {bw, listH}, true);
        if (visibleRows == 0) ImGui::TextDisabled("No matching cards.");
        int shown = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            CardInfo ci = m_db.getCard(rows[i].code);
            bool isMon = (ci.type & TYPE_MONSTER) != 0;
            bool isSpl = (ci.type & TYPE_SPELL)   != 0;
            bool isTrp = (ci.type & TYPE_TRAP)    != 0;
            if (isMon && !fMon) continue;
            if (isSpl && !fSpl) continue;
            if (isTrp && !fTrp) continue;
            std::string nm = ci.name.empty()
                ? ("#" + std::to_string(rows[i].code)) : ci.name;
            if (!flt.empty()) {
                std::string lc = nm + "#" + std::to_string(rows[i].code);
                for (char& c : lc) c = (char)tolower((unsigned char)c);
                if (lc.find(flt) == std::string::npos) continue;
            }
            RowAct act = resolveRowAct(rows[i].code, rows[i].seq);
            if (fActOnly && ownZone && expectedLoc && act.kind == 0) continue;
            ++shown;
            const char* tline =
                isMon ? "Monster" : isSpl ? "Spell" : isTrp ? "Trap" : "Card";

            if (act.kind != 0 && m_debugLog)
                m_dm.logEvent(std::string("[ACTION MATCH] clicked con=") +
                    std::to_string(kViewerLocal) + " loc=" +
                    std::to_string(expectedLoc) + " seq=" +
                    std::to_string(rows[i].seq) + " code=" +
                    std::to_string(rows[i].code) + " -> " +
                    (act.kind == 1 ? "idle" : "chain") +
                    " cmd=" + std::to_string(act.cmd) + " result=yes");

            ImGui::PushID((int)i);
            void* tex = m_rend.getCardTexture(rows[i].code);
            ImGui::Image(tex ? tex : (void*)0, {32.f, 47.f});
            // Right-click the thumbnail to zoom/read the card, like hand+field.
            if (ImGui::IsItemHovered()) {
                m_hoveredCard = rows[i].code; m_hoveredInfo = ci;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                    m_zoomCard = rows[i].code;
            }
            ImGui::SameLine();
            ImGui::BeginGroup();
            // Name — orange when an action is available; otherwise selectable.
            if (act.kind != 0)
                ImGui::TextColored({1.f, 0.78f, 0.30f, 1.f}, "%s", nm.c_str());
            else if (ImGui::Selectable(nm.c_str(), false,
                                       ImGuiSelectableFlags_None,
                                       {bw - 54.f, 16.f})) {
                m_hoveredCard = rows[i].code; m_hoveredInfo = ci;
            }
            if (ImGui::IsItemHovered()) {
                m_hoveredCard = rows[i].code; m_hoveredInfo = ci;
            }
            ImGui::TextDisabled("%s  #%u  seq %u",
                tline, (unsigned)rows[i].code, (unsigned)rows[i].seq);
            if (act.kind != 0) {
                std::string lbl = act.label + "##va" + std::to_string(i);
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImGui::ColorConvertU32ToFloat4(IM_COL32(120, 40, 110, 255)));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImGui::ColorConvertU32ToFloat4(IM_COL32(160, 60, 150, 255)));
                bool go = ImGui::Button(lbl.c_str(), {-1.f, 24.f});
                ImGui::PopStyleColor(2);
                if (go) {
                    const char* gysrc = zshort;
                    m_dm.logEvent(std::string("[GY VIEWER ACTIVATE] code=") +
                        std::to_string(rows[i].code) + " [" + nm + "]" +
                        " seq=" + std::to_string(rows[i].seq) +
                        " engineIdx=" +
                        std::to_string(act.kind == 1
                            ? currentSelection().idle[act.idleIdx].index
                            : act.chainIdx) +
                        " kind=" + (act.kind == 1 ? "idle" : "chain") +
                        " src=" + gysrc);
                    gAudio().play(act.cmd == 1 ? "special_summon"
                                  : act.kind == 2 ? "chain" : "activate");
                    if (act.cmd == 1) m_lastSummonSfxAt   = ImGui::GetTime();
                    else              m_lastActivateSfxAt = ImGui::GetTime();
                    if (m_zoneRectsReady) {
                        int apl = (int)currentSelection().player & 1;
                        ImVec2 c{
                            (m_rectGY_tl[apl].x + m_rectGY_br[apl].x) * 0.5f,
                            (m_rectGY_tl[apl].y + m_rectGY_br[apl].y) * 0.5f };
                        ImU32 col = (act.cmd == 1)
                            ? IM_COL32(112, 220, 255, 255)
                            : IM_COL32(244, 132, 232, 255);
                        m_anim.ring(c, act.cmd == 1 ? 56.f : 40.f, col, 0.60);
                    }
                    if (act.kind == 1) {
                        const IdleAction& a = currentSelection().idle[act.idleIdx];
                        submitIdleCmd(a.cmd, a.index, "viewer action");
                    } else {
                        submitMpChoice(WaitType::SelectChain, act.chainIdx);
                        clearSelection();
                    }
                    m_viewerLoc = 0;
                    m_viewerExtraCache.clear();
                    m_viewerFilter[0] = 0;
                    ImGui::EndGroup();
                    ImGui::PopID();
                    ImGui::EndChild();
                    return;
                }
            }
            ImGui::EndGroup();
            ImGui::PopID();
            ImGui::Separator();
        }
        if (!flt.empty() || !fMon || !fSpl || !fTrp || fActOnly)
            ImGui::TextDisabled("showing %d / %d", shown, (int)rows.size());
        ImGui::EndChild();
        return;
    }

    // ── Game Over ────────────────────────────────────────────────────────────
    // The duel has ended. Show a frozen-board Game Over panel rather than the
    // misleading "No duel in progress" while the duel screen is still open.
    // On a host-authoritative client the local engine never runs, so the duel
    // ends only when the host's GameOver lands (m_mpRemoteDone). Either source
    // shows the panel.
    const bool clientView =
        m_mpHostAuth && m_net.isClient() && m_mpRemoteDone && !m_dm.isDone();
    // Best-of-3 match owns its own end screen: score the game, then offer Side
    // Deck / Next Game, or end the match when someone reaches 2 wins.
    if (m_matchActive && m_dm.isDone()) {
        const int me = m_net.localPlayerIndex();
        bool youWon = (m_dm.winner() == me);
        bool draw   = (m_dm.winner() != 0 && m_dm.winner() != 1);
        if (!m_matchGameScored) {
            if (!draw) m_matchWins[youWon ? 0 : 1]++;
            m_matchGameScored = true;
            gAudio().play(youWon ? "victory" : "defeat");
        }
        bool matchOver = m_matchWins[0] >= 2 || m_matchWins[1] >= 2;
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::TextColored({1.f, 0.85f, 0.25f, 1.f}, "Game %d — %s",
            m_matchGameNo, draw ? "Draw" : (youWon ? "You win" : "You lose"));
        if (UIStyle::fHeader) ImGui::PopFont();
        ImGui::Text("Match score — You %d : %d Opponent",
                    m_matchWins[0], m_matchWins[1]);
        ImGui::Spacing();
        if (matchOver) {
            bool wonMatch = m_matchWins[0] > m_matchWins[1];
            ImGui::TextColored(wonMatch ? ImVec4{0.45f,0.92f,0.55f,1.f}
                                        : ImVec4{0.95f,0.55f,0.45f,1.f},
                wonMatch ? "You won the match!" : "You lost the match.");
            ImGui::Spacing();
            if (UIStyle::GhostButton("Return to Lobby", {bw, 32.f})) {
                if (m_dm.isRunning()) m_dm.endDuel();
                m_matchActive = false;
                m_screen = Screen::Lobby;
                m_anim.clear(); m_zoneRectsReady = false;
            }
        } else {
            ImGui::TextDisabled("Adjust your deck from your Side Deck, then "
                                "play the next game.");
            ImGui::Spacing();
            if (UIStyle::PrimaryButton("Side Deck", {bw, 34.f})) {
                if (m_dm.isRunning()) m_dm.endDuel();
                // Load your match deck into the builder in side-deck mode.
                m_editDeck  = loadYdk(m_matchPlayerPath);
                m_savedDeck = m_editDeck;
                m_matchSiding = true;
                m_screen = Screen::DeckBuilder;
                m_anim.clear(); m_zoneRectsReady = false;
            }
            if (UIStyle::GhostButton("Next Game (keep deck)", {bw, 30.f})) {
                if (m_dm.isRunning()) m_dm.endDuel();
                m_matchGameNo++;
                m_anim.clear(); m_zoneRectsReady = false;
                startMatchGame();
            }
        }
        return;
    }
    // Puzzle mode owns its own end screen (solved / failed + Retry), so it
    // never shows the deck-based Rematch flow.
    if (m_puzzleMode && m_dm.isDone()) {
        bool solved = (m_dm.winner() == m_dm.humanSeat());   // you won
        if (m_puzzleResult == 0) {
            m_puzzleResult = solved ? 1 : 2;
            gAudio().play(solved ? "victory" : "defeat");
        }
        if (solved) {
            if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
            ImGui::TextColored({0.45f, 0.92f, 0.55f, 1.f}, "Puzzle Solved!");
            if (UIStyle::fHeader) ImGui::PopFont();
            ImGui::TextWrapped("Nicely done — you found the line.");
        } else {
            if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
            ImGui::TextColored({0.95f, 0.55f, 0.45f, 1.f}, "Not solved");
            if (UIStyle::fHeader) ImGui::PopFont();
            ImGui::TextWrapped("The opponent survived. Give it another go.");
        }
        ImGui::Spacing();
        if (UIStyle::PrimaryButton("Retry puzzle", {bw, 34.f}))
            startPuzzleByIndex(m_activePuzzle);
        if (UIStyle::GhostButton("Puzzle list", {bw, 30.f})) {
            if (m_dm.isRunning()) m_dm.endDuel();
            m_puzzleMode = false;
            m_screen = Screen::Lobby;
            m_anim.clear(); m_zoneRectsReady = false;
            m_puzzleBrowserOpen = true;
        }
        if (UIStyle::GhostButton("Return to Lobby", {bw, 30.f})) {
            if (m_dm.isRunning()) m_dm.endDuel();
            m_puzzleMode = false;
            m_screen = Screen::Lobby;
            m_anim.clear(); m_zoneRectsReady = false;
        }
        return;
    }
    if (m_dm.isDone() || clientView) {
        const int  w      = clientView ? m_mpRemoteWinner : m_dm.winner();
        const int  reason = clientView ? m_mpRemoteReason : m_dm.winReason();
        const int  me     = m_net.localPlayerIndex();
        const bool isMp   = !m_net.isOffline();
        ImGui::TextColored({1.f, 0.85f, 0.25f, 1.f}, "Duel Ended");
        ImGui::Spacing();
        // Result is shown from the LOCAL player's seat (offline coin toss can
        // seat the human at 1; online the client is seat 1).
        if (w == 0 || w == 1) {
            const bool youWon = (w == me);
            ImGui::TextColored(youWon ? COL_P1 : COL_P2,
                               youWon ? "You win!" : "You lose.");
        } else {
            ImGui::Text("The duel was a draw.");
        }
        const char* reasonTxt = DuelManager::winReasonText(reason);
        if (reasonTxt && reasonTxt[0])
            ImGui::TextWrapped("Reason: %s", reasonTxt);
        ImGui::Spacing();
        // Manual Save Replay — only when a local engine actually recorded the
        // duel (host / offline). A host-auth client has no replay to save.
        if (!clientView && ImGui::Button("Save Replay", {bw, 30.f})) {
            // Make sure the replay carries the final metadata before write.
            if (m_replayRecording) {
                auto& dm = m_dm;      // replay records the local engine
                m_replay.winner = dm.winner();
                m_replay.turns  = dm.field().turnCount;
                m_replay.durationSec = ImGui::GetTime() - m_replayStartTime;
                m_replay.finalLP[0]  = dm.field().lp[0];
                m_replay.finalLP[1]  = dm.field().lp[1];
            }
            std::string filename = m_replay.suggestedFilename();
            std::string path = edo::Replay::defaultDir() + "/" + filename;
            if (m_replay.save(path)) {
                pushToast(std::string("Replay saved: ") + filename,
                          IM_COL32(110, 220, 140, 255), 2.6);
                ImGui::SetClipboardText(path.c_str());
                m_dm.logEvent("[replay] manual save → " + path);
                m_replaySavedOnce = true;
            } else {
                pushToast("Replay save failed",
                          IM_COL32(232, 110, 100, 255), 2.6);
            }
        }
        ImGui::Spacing();
        // Rematch — offline only (online rematch is a separate feature that
        // needs a RematchRequest/Accept handshake). Replays the SAME decks
        // with a fresh seed + coin toss.
        if (!isMp && m_deck0Path[0] && m_deck1Path[0] &&
            UIStyle::PrimaryButton("Rematch", {bw, 34.f})) {
            finalizeReplay("rematch");
            // Reset observers BEFORE the toss so the coin banner survives.
            m_anim.clear();
            m_zoneRectsReady  = false;
            m_sfxObsInited    = false;
            m_endGameSfxFired = false;
            // Fresh coin toss each rematch — re-decides who goes first.
            if (startOfflineDuelWithCoinToss(m_deck0Path, m_deck1Path,
                                             8000, 5, 1))
                gAudio().play("duel_start");
        }
        if (UIStyle::GhostButton("Return to Lobby", {bw, 30.f})) {
            finalizeReplay("return to lobby");
            if (m_dm.isRunning()) m_dm.endDuel();
            // Online: tear the session down so we don't leak MP latches into
            // the next duel.
            if (isMp) {
                m_mpInDuel = false;
                m_dm.setSuppressAutoResolve(false);
                resetMpResponseState();
                m_net.disconnect("returned to lobby");
            }
            m_screen = Screen::Lobby;
            m_anim.clear();
            m_zoneRectsReady = false;
        }
        return;
    }

    if (!isDuelVisiblyRunning()) {
        // In host-auth client mode, the local DuelManager is idle by
        // design — never show "No duel in progress" while we still
        // have a valid snapshot stream from the host.
        if (m_mpHostAuth && m_net.isClient() && m_mpInDuel) {
            m_dm.logEvent("[CLIENT MODAL SUPPRESSED] "
                          "reason=host-authoritative-client"
                          "  localDmRunning=" +
                          std::string(m_dm.isRunning() ? "true" : "false") +
                          "  remoteActive=" +
                          (m_mpRemoteDuelActive ? "true" : "false"));
            ImGui::TextDisabled("Waiting for host...");
        } else {
            ImGui::TextDisabled("No duel in progress");
        }
        return;
    }
    // Host-auth client may be sitting in the gap between sending a
    // ClientChoice and receiving the next snapshot/prompt. Surface
    // that state so the panel doesn't look frozen.
    if (m_mpHostAuth && m_net.isClient() && m_mpAwaitingHostUpdate &&
        !m_mpRemoteSelValid) {
        ImGui::TextDisabled("Waiting for host update...");
        ImGui::Separator();
    }

    float hw  = (bw - 6.f) * 0.5f;
    auto& sel = currentSelection();
    // Click-first selection only applies to the idle/battle command panels.
    // For any other state (chain window, card selection, placement, etc.)
    // the engine drives the panel, so a leftover selected card would be
    // misleading. Clear it here once on state change.
    if (sel.type != WaitType::SelectIdleCmd &&
        sel.type != WaitType::SelectBattleCmd)
        clearSelection();
    // The MSG_SELECT_CARD multi-select buffer is scoped to ONE prompt — when
    // the engine moves on (response consumed, new prompt, idle, …) we wipe
    // the staged indices and the filter so they cannot leak into the next
    // prompt or get sent twice.
    if (sel.type != WaitType::SelectCard &&
        sel.type != WaitType::SelectTribute) {
        m_selSelIdx.clear();
        m_selFilter[0] = 0;
    }

    // Engine parked on an action the simulator cannot satisfy — say so plainly
    // instead of silently hanging with a misleading "waiting" message.
    //
    // Gated for two specialised modes:
    //   * Replay — the replay feeder drives the engine even for prompt
    //     types the live UI doesn't implement; replay desync modal owns
    //     the failure case.
    //   * Multiplayer — the MP diagnostic modal in drawDuel owns the
    //     failure case; the network response queue may unblock the
    //     engine asynchronously, so this modal would mislead the user.
    // Host-auth client never sees local "Duel paused" — its engine is
    // idle by design and the host owns the only authoritative state.
    if (isDuelVisiblyBlocked() && !m_replayMode && !m_mpInDuel) {
        ImGui::TextColored({1.f, 0.5f, 0.4f, 1.f}, "Duel paused");
        ImGui::TextWrapped("The engine requested an action the simulator does not "
                           "handle yet. See the log on the left for details.");
        ImGui::Spacing();
        if (ImGui::Button("Return to Lobby", {bw, 30.f})) {
            finalizeReplay("engine paused");
            m_dm.endDuel();
            m_screen = Screen::Lobby;
            m_anim.clear();
            m_zoneRectsReady = false;
        }
        return;
    }

    // This panel only renders the LOCAL player's prompt. Pre-MP that was
    // always engine player 0; in multiplayer the client's local seat is
    // engine player 1, so we compare against m_net.localPlayerIndex().
    // Offline / host both have localPlayerIndex == 0 so the previous
    // semantics are preserved.
    const int kLocalPlayer = m_net.localPlayerIndex();
    if (DuelManager::isRealSelect(sel.type) && sel.player != (uint8_t)kLocalPlayer) {
        std::string who = m_net.isOffline() || m_net.peer().displayName.empty()
            ? ("P" + std::to_string(sel.player + 1))
            : m_net.peer().displayName;
        ImGui::TextDisabled("Waiting for %s  —  %s...",
            who.c_str(), waitTypeHuman((uint32_t)sel.type));
        return;
    }

    // SelectIdleCmd and SelectBattleCmd are NOT handled here anymore — those
    // moved to the bottom action strip (drawBottomActionStrip). The large
    // overlay is now reserved for engine prompts (chain / card / place /
    // yes-no / option / unselect), zone viewers, and the paused/done states.
    if (sel.type == WaitType::SelectIdleCmd ||
        sel.type == WaitType::SelectBattleCmd) {
        ImGui::TextDisabled("(action panel moved to bottom strip)");
        return;
    }

    switch (sel.type) {
    // ── Main Phase idle ───────────────────────────────────────────────────────
    case WaitType::SelectIdleCmd: {
        ImGui::TextColored(COL_P1, "Main Phase  (P%d)", sel.player + 1);
        ImGui::Spacing();

        // Click-first: when a card is selected, the panel shows only THAT
        // card's actions. Otherwise it falls back to the full grouped list,
        // so the old workflow keeps working too. Hovering an action button
        // still updates the preview; the selected card's preview is shown
        // by default (until you hover something else).
        bool haveSel = (m_selCode != 0);
        // Esc backs out of a picked card before any action is committed (you can
        // also click the card again, or the Deselect button). Note: once a
        // Summon is actually declared YGO has no take-backs — use Testing Mode
        // rewind to undo a committed play.
        if (haveSel && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            clearSelection();
            haveSel = false;
        }
        if (haveSel) {
            // Force the top-of-panel card preview to the selected card.
            m_hoveredCard = m_selCode;
            m_hoveredInfo = m_db.getCard(m_selCode);
            const char* locName =
                (m_selLoc == LOC_HAND)  ? "Hand"  :
                (m_selLoc == LOC_MZONE) ? "Monster Zone" :
                (m_selLoc == LOC_SZONE) ? "S/T Zone"     :
                (m_selLoc == LOC_GY)    ? "Graveyard"    :
                (m_selLoc == LOC_REM)   ? "Banished"     :
                (m_selLoc == LOC_EXTRA) ? "Extra Deck"   : "?";
            ImGui::TextColored(COL_ACCENT, "Selected: %s",
                m_hoveredInfo.name.empty()
                    ? ("#" + std::to_string(m_selCode)).c_str()
                    : m_hoveredInfo.name.c_str());
            ImGui::TextDisabled("P%d %s [seq %u]",
                m_selPlayer + 1, locName, (unsigned)m_selSeq);
            if (ImGui::Button("Cancel / Deselect  (Esc)", {bw, 24.f}))
                clearSelection();
            ImGui::Separator();
        } else {
            ImGui::TextDisabled("Tip: click a card in hand/field to see only "
                                "its legal actions.");
            ImGui::Spacing();
        }

        // Every button here is an action the ENGINE listed as legal — the UI
        // never invents one. Clicking sends respondIdleCmd(cmd, index).
        // Engine cmd: 0 Summon  1 SpSummon  2 Reposition  3 Set(monster)
        //             4 Set S/T  5 Activate
        // Display groups: 0 Summon/Set  1 Special Summon  2 Spell/Trap
        //                 3 Monster Effects  4 Position
        auto groupOf = [&](const IdleAction& a) -> int {
            switch (a.cmd) {
                case 0: case 3: return 0;
                case 1:         return 1;
                case 4:         return 2;
                case 2:         return 4;
                case 5: {
                    uint32_t t = m_db.getCard(a.code).type;
                    return (t & (TYPE_SPELL | TYPE_TRAP)) ? 2 : 3;
                }
            }
            return 3;
        };
        static const char* kGroup[5] = {
            "Summon / Set", "Special Summon", "Spell / Trap",
            "Monster Effects", "Position"
        };
        // Filter predicate: this panel is CONTEXT — without a selected card
        // there is no "command center" listing every legal move. Click a
        // card on the field/hand to see its actions; otherwise only the
        // phase buttons render below.
        auto matchesSelected = [&](const IdleAction& a) {
            if (!haveSel) return false;
            return a.code == m_selCode && a.con == m_selPlayer
                && a.loc == m_selLoc && a.seq == m_selSeq;
        };

        bool anyShown = false;
        for (int g = 0; g < 5; g++) {
            bool header = false;
            for (int i = 0; i < (int)sel.idle.size(); i++) {
                const IdleAction& a = sel.idle[i];
                if (groupOf(a) != g) continue;
                if (!matchesSelected(a)) continue;
                if (!header) {
                    ImGui::TextColored(COL_ACCENT, "%s", kGroup[g]);
                    header = true;
                }
                std::string nm = a.name.empty() ? ("#" + std::to_string(a.code))
                                                  : a.name;
                std::string lbl;
                float       btnH = 26.f;
                if (a.cmd == 5) {
                    // Activatable effect. A Spell/Trap reads "Activate:"; a
                    // monster's ignition/quick effect reads "Activate effect:".
                    // The decoded description names WHICH effect, so a
                    // multi-effect card does not collapse to a bare name.
                    uint32_t ct = m_db.getCard(a.code).type;
                    bool isST = (ct & (TYPE_SPELL | TYPE_TRAP)) != 0;
                    lbl = (isST ? "Activate: " : "Activate effect: ") + nm;
                    if (!a.effect.text.empty())
                        lbl += "  —  " + a.effect.text;
                    else if (a.effect.raw)
                        lbl += "  —  desc#" + std::to_string(a.effect.raw);
                    btnH = 30.f;
                } else {
                    const char* verb =
                        (a.cmd == 0) ? "Summon"         : (a.cmd == 1) ? "Special Summon" :
                        (a.cmd == 2) ? "Change Position": (a.cmd == 3) ? "Set monster"    :
                        (a.cmd == 4) ? "Set S/T"        : "Activate";
                    lbl = std::string(verb) + ": " + nm;
                }
                lbl += "##idle" + std::to_string(i);
                if (ImGui::Button(lbl.c_str(), {bw, btnH}))
                    submitIdleCmd(a.cmd, a.index, nm.c_str());
                if (ImGui::IsItemHovered()) {           // hover -> preview card
                    m_hoveredCard = a.code;
                    m_hoveredInfo = m_db.getCard(a.code);
                }
                anyShown = true;
            }
            if (header) ImGui::Spacing();
        }
        if (haveSel && !anyShown)
            ImGui::TextDisabled("No legal actions for this card.");

        ImGui::Separator();
        ImGui::TextColored(COL_ACCENT, "Phase");
        // Phase transitions — only offered when the engine allows them.
        // Always shown regardless of card selection (they belong to no card).
        if (sel.toBP && ImGui::Button("Go to Battle Phase", {bw, 30.f}))
            submitIdleCmd(6, 0, "Battle Phase");
        if (sel.toEP) {
            // Misclick guard (#A): if you can still Normal Summon, confirm.
            bool canNormalSummon = false;
            for (const auto& a : sel.idle)
                if (a.cmd == 0) { canNormalSummon = true; break; }
            if (ImGui::Button("End Turn", {bw, 30.f})) {
                if (canNormalSummon) ImGui::OpenPopup("End turn?##etpop");
                else submitIdleCmd(7, 0, "End Turn");
            }
            ImGui::SetNextWindowPos(
                {ImGui::GetIO().DisplaySize.x * 0.5f,
                 ImGui::GetIO().DisplaySize.y * 0.5f},
                ImGuiCond_Always, {0.5f, 0.5f});
            if (ImGui::BeginPopupModal("End turn?##etpop", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
                ImGui::TextUnformatted("You can still Normal Summon this turn.");
                ImGui::TextDisabled("End your turn anyway?");
                ImGui::Dummy({1.f, 8.f});
                if (UIStyle::PrimaryButton("End Turn", {130.f, 32.f})) {
                    submitIdleCmd(7, 0, "End Turn");
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine(0.f, 8.f);
                if (UIStyle::GhostButton("Keep playing", {130.f, 32.f}))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        break;
    }

    // ── Choose a zone (after a summon/set/activate) ──────────────────────────
    // ── Choose a battle position (Summon / Set / flip) ──────────────────────
    case WaitType::SelectPosition: {
        ImGui::PushStyleColor(ImGuiCol_Text, {1.f, 0.95f, 0.5f, 1.f});
        const char* pnm = (sel.cards.empty() || sel.cards[0].name.empty())
                          ? "this monster" : sel.cards[0].name.c_str();
        ImGui::TextWrapped("Battle position for %s", pnm);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        const int pmask = sel.positionMask;   // POS_FACEUP_ATTACK=1 ... DEF_DOWN=8
        if (pmask & 0x1)
            if (ImGui::Button("Face-up Attack##pos_fa", {bw, 30.f}))
                submitMpChoice(WaitType::SelectPosition, 0x1);
        if (pmask & 0x4)
            if (ImGui::Button("Face-up Defense##pos_fd", {bw, 30.f}))
                submitMpChoice(WaitType::SelectPosition, 0x4);
        if (pmask & 0x8)
            if (ImGui::Button("Set (face-down Defense)##pos_dd", {bw, 30.f}))
                submitMpChoice(WaitType::SelectPosition, 0x8);
        if (pmask & 0x2)
            if (ImGui::Button("Face-down Attack##pos_da", {bw, 30.f}))
                submitMpChoice(WaitType::SelectPosition, 0x2);
        break;
    }

    // ── Declare a card name (e.g. Mind Crush, Crush Card Virus) ──────────────
    case WaitType::AnnounceCard: {
        ImGui::PushStyleColor(ImGuiCol_Text, {1.f, 0.95f, 0.5f, 1.f});
        ImGui::TextWrapped("Declare a card");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Type a name, then click a card to declare it.");
        ImGui::SetNextItemWidth(bw);
        ImGui::InputTextWithHint("##announce_search", "Card name...",
                                 m_announceBuf, sizeof(m_announceBuf));
        if (m_announceBuf[0]) {
            std::vector<CardInfo> hits = m_db.search(m_announceBuf, 14);
            ImGui::BeginChild("##announce_results", {bw, 190.f}, true);
            for (const CardInfo& ci : hits) {
                std::string lbl = (ci.name.empty()
                                     ? ("#" + std::to_string(ci.id)) : ci.name)
                                  + "##acard" + std::to_string(ci.id);
                if (ImGui::Selectable(lbl.c_str())) {
                    // Declare this code; the engine validates against its filter.
                    submitMpChoice(WaitType::AnnounceCard, (int)ci.id);
                    m_announceBuf[0] = '\0';
                }
                if (ImGui::IsItemHovered()) {
                    m_hoveredCard = ci.id;
                    m_hoveredInfo = ci;
                }
            }
            ImGui::EndChild();
        }
        break;
    }

    case WaitType::SelectPlace: {
        ImGui::PushStyleColor(ImGuiCol_Text, {1.f, 0.95f, 0.5f, 1.f});
        ImGui::TextWrapped("Choose a zone");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Glowing green tiles on the field are clickable. "
                            "Or use these buttons as a fallback.");
        ImGui::Spacing();
        // placeFlag: a SET bit means the zone is FORBIDDEN/occupied.
        //   bits 0-4  : monster zones 1-5      bits 5-6  : Extra Monster Zones
        //   bits 8-12 : spell/trap zones 1-5   bit 13    : Field Zone
        //   bits 14-15: Pendulum zones
        uint32_t flag = sel.placeFlag;
        bool any = false;
        for (int s = 0; s < 5; s++) {
            if (flag & (1u << s)) continue;
            char lbl[32]; snprintf(lbl, 32, "Monster Zone %d##mz%d", s + 1, s);
            if (ImGui::Button(lbl, {bw, 26.f}))
                submitPlace(sel.player, 0x04 /*LOCATION_MZONE*/, s);
            any = true;
        }
        for (int s = 5; s < 7; s++) {                       // Extra Monster Zones
            if (flag & (1u << s)) continue;
            char lbl[36]; snprintf(lbl, 36, "Extra Monster Zone##emz%d", s);
            if (ImGui::Button(lbl, {bw, 26.f}))
                submitPlace(sel.player, 0x04 /*LOCATION_MZONE*/, s);
            any = true;
        }
        for (int s = 0; s < 5; s++) {
            if (flag & (1u << (s + 8))) continue;
            char lbl[36]; snprintf(lbl, 36, "Spell/Trap Zone %d##sz%d", s + 1, s);
            if (ImGui::Button(lbl, {bw, 26.f}))
                submitPlace(sel.player, 0x08 /*LOCATION_SZONE*/, s);
            any = true;
        }
        if (!(flag & (1u << 13))) {                          // Field Zone (seq 5)
            if (ImGui::Button("Field Zone##fz", {bw, 26.f}))
                submitPlace(sel.player, 0x08 /*LOCATION_SZONE*/, 5);
            any = true;
        }
        for (int s = 6; s < 8; s++) {                        // Pendulum zones
            if (flag & (1u << (s + 8))) continue;
            char lbl[36]; snprintf(lbl, 36, "Pendulum Zone##pz%d", s);
            if (ImGui::Button(lbl, {bw, 26.f}))
                submitPlace(sel.player, 0x08 /*LOCATION_SZONE*/, s);
            any = true;
        }
        if (!any) ImGui::TextDisabled("(no legal zone reported by engine)");
        break;
    }

    // ── Battle Phase ─────────────────────────────────────────────────────────
    case WaitType::SelectBattleCmd: {
        ImGui::TextColored(COL_P1, "Battle Phase  (P%d)", sel.player + 1);
        ImGui::Spacing();
        // sel.idle holds engine-validated battle actions: cmd 1 = attack with a
        // monster, cmd 0 = activate an effect. Response uses the same encoding
        // as IDLECMD (t=1 attack, 0 activate, 2 -> M2, 3 -> End Phase).
        int attackers = 0;
        for (int i = 0; i < (int)sel.idle.size(); i++) {
            const IdleAction& a = sel.idle[i];
            std::string nm = a.name.empty() ? ("#" + std::to_string(a.code)) : a.name;
            const char* verb = (a.cmd == 1) ? "Attack with" : "Activate";
            if (a.cmd == 1) ++attackers;
            std::string lbl = std::string(verb) + ": " + nm + "##bc" + std::to_string(i);
            if (ImGui::Button(lbl.c_str(), {bw, 26.f}))
                submitIdleCmd(a.cmd, a.index, nm.c_str());
        }
        if (sel.idle.empty())
            ImGui::TextDisabled("No monster can attack and no effects are usable.");
        else if (attackers == 0)
            ImGui::TextDisabled("(no monster can attack)");
        ImGui::Spacing();
        if (sel.toM2 && ImGui::Button("Go to Main Phase 2", {bw, 30.f}))
            submitIdleCmd(2, 0, "Main Phase 2");
        if (sel.toEP && ImGui::Button("End Battle Phase", {bw, 30.f}))
            submitIdleCmd(3, 0, "End Battle Phase");
        break;
    }

    // ── Yes / No ──────────────────────────────────────────────────────────────
    case WaitType::SelectYesNo: {
        // Show the engine's decoded prompt text; when it's empty/generic (common
        // for optional "then you can discard 1 card?" costs that carry no card
        // code) fall back to the resolving card's full effect text so the player
        // knows exactly what they're being asked.
        std::string yn = sel.chainEffects.empty() ? std::string()
                                                   : sel.chainEffects[0].text;
        uint32_t src = m_dm.chainSourceCode();
        if (yn.empty() && src) yn = m_db.getCard(src).desc;
        ImGui::Text("Activate this effect?");
        if (!yn.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw);
            ImGui::TextDisabled("%s", yn.c_str());
            ImGui::PopTextWrapPos();
        }
        drawOpponentActionHint();
        ImGui::Spacing();
        if (ImGui::Button("Yes##yn", {hw, 36.f})) submitMpChoice(WaitType::SelectYesNo, 1);
        ImGui::SameLine(0.f, 8.f);
        if (ImGui::Button("No##yn",  {hw, 36.f})) submitMpChoice(WaitType::SelectYesNo, 0);
        if (m_net.isOffline() && ImGui::IsItemHovered())
            ImGui::SetTooltip("Tip: right-click anywhere = No");
        break;
    }

    // ── Effect Yes / No ───────────────────────────────────────────────────────
    // The engine's "do you want to activate THIS effect?" prompt. Show which
    // card and — via the decoded description — which of its effects, plus the
    // timing window, so a Trigger Effect is never confused with a Quick Effect.
    case WaitType::SelectEffectYn: {
        std::string nm  = sel.cards.empty()
            ? std::string("an effect")
            : (sel.cards[0].name.empty()
                   ? ("#" + std::to_string(sel.cards[0].code))
                   : sel.cards[0].name);
        std::string eff = sel.chainEffects.empty() ? std::string()
                                                   : sel.chainEffects[0].text;
        // Many optional "then you can ..." costs (e.g. "discard a card to ...")
        // arrive with NO specific description, leaving only a bare "Activate
        // effect?" that doesn't tell the player what they're agreeing to. Fall
        // back to the card's full effect text so the prompt is always clear.
        if (eff.empty() && !sel.cards.empty())
            eff = m_db.getCard(sel.cards[0].code).desc;
        // System/hint strings keep their %ls placeholders ("Activate the
        // Trigger Effect of "%ls" from [%ls]?") — substitute the card name
        // and its location so the line reads like a real sentence.
        if (!eff.empty() && eff.find("%ls") != std::string::npos) {
            const char* locName = "the field";
            if (!sel.cards.empty()) {
                switch (sel.cards[0].loc) {
                    case LOC_HAND:  locName = "the hand";       break;
                    case LOC_MZONE: locName = "a Monster Zone"; break;
                    case LOC_SZONE: locName = "a Spell/Trap Zone"; break;
                    case LOC_GY:    locName = "the Graveyard";  break;
                    case LOC_REM:   locName = "the Banished pile"; break;
                    case LOC_EXTRA: locName = "the Extra Deck"; break;
                    case LOC_DECK:  locName = "the Deck";       break;
                    default: break;
                }
            }
            bool first = true;
            size_t pos;
            while ((pos = eff.find("%ls")) != std::string::npos) {
                eff.replace(pos, 3, first ? nm : std::string(locName));
                first = false;
            }
        }
        const char* kind =
            (sel.timing == TimingContext::PostSummonTrigger) ? "Trigger Effect"
                                                             : "Effect";
        ImGui::TextWrapped("Activate %s of %s?", kind, nm.c_str());
        if (!eff.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw);
            ImGui::TextDisabled("%s", eff.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TextDisabled("Window: %s", DuelManager::timingName(sel.timing));
        drawOpponentActionHint();
        ImGui::Spacing();
        if (ImGui::Button("Yes##eyn", {hw, 36.f})) submitMpChoice(WaitType::SelectEffectYn, 1);
        ImGui::SameLine(0.f, 8.f);
        if (ImGui::Button("No##eyn",  {hw, 36.f})) submitMpChoice(WaitType::SelectEffectYn, 0);
        if (m_net.isOffline() && ImGui::IsItemHovered())
            ImGui::SetTooltip("Tip: right-click anywhere = No");
        break;
    }

    // ── Choose option ─────────────────────────────────────────────────────────
    case WaitType::SelectOption: {
        // Real effect text per option, decoded from the engine's desc=u64
        // value (via DuelManager::decodeDesc → CardDB::cardString). Falls
        // back gracefully: decoded text → "Unknown option — desc=N" →
        // "Option N". Selecting still sends the original engine index via
        // m_dm.respondInt(i) — no response-format change.
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(UIStyle::C().textHi));
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::TextUnformatted("Choose an effect");
        if (UIStyle::fHeader) ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::TextDisabled("%d option%s — hover for full text",
                            (int)sel.options.size(),
                            sel.options.size() == 1 ? "" : "s");
        ImGui::Dummy({1.f, 6.f});
        // Content-sized choice list: ~46px per compact row, capped at the
        // room left under the modal cap so the auto-resizing window fits the
        // rows exactly (no empty space) and scrolls only when they overflow.
        const float kOptRowH = 46.f;
        float optMaxH = std::max(120.f, kHeightCap - 130.f);
        float optWant = std::max(kOptRowH, sel.options.size() * kOptRowH);
        float optH    = std::min(optMaxH, optWant);
        if (m_debugLog)
            m_dm.logEvent(std::string("[PROMPT SIZE] type=option rows=") +
                std::to_string(sel.options.size()) +
                " listH=" + std::to_string((int)optH) +
                " scroll=" + (optWant > optMaxH ? "yes" : "no"));
        ImGui::BeginChild("##optlist", {bw, optH}, false);
        for (int i = 0; i < (int)sel.options.size(); i++) {
            const EffectDesc* d = (i < (int)sel.chainEffects.size())
                                  ? &sel.chainEffects[i] : nullptr;
            // Resolve the source card (for the row title and the hover
            // preview / right-side info panel).
            uint32_t srcCode = (d && d->isCardString) ? d->cardCode : 0;
            std::string srcName;
            if (srcCode) {
                CardInfo sci = m_db.getCard(srcCode);
                if (!sci.name.empty()) srcName = sci.name;
            }
            std::string effN  = "Effect " + std::to_string(i + 1);
            std::string title = srcName.empty()
                ? effN : (srcName + "  —  " + effN);
            std::string full  = (d && !d->text.empty()) ? d->text
                                                        : std::string();
            // Compact prompts: short single-line label in the row; the FULL
            // text rides into the right info panel on hover (set below).
            std::string rowDesc = compactPromptDesc(full);
            std::string rid   = "##optrow" + std::to_string(i);
            bool hov = false;
            if (PromptChoiceRow(rid.c_str(), title, rowDesc,
                                bw - 14.f, &hov))
                submitMpChoice(WaitType::SelectOption, (int)i);
            if (hov) {
                // Feed the right-side info panel: related card + the
                // decoded option ("Selected Prompt Option" section).
                if (srcCode) {
                    m_hoveredCard = srcCode;
                    m_hoveredInfo = m_db.getCard(srcCode);
                }
                m_promptHoverTitle = effN;
                m_promptHoverText  = full.empty() ? title : full;
                m_promptHoverFrame = ImGui::GetFrameCount();
                // Raw desc diagnostics are debug-only.
                if (m_debugLog && d && d->raw)
                    ImGui::SetTooltip("desc=%llu  %s",
                        (unsigned long long)d->raw,
                        d->isCardString ? "card-string" : "system/hint");
            }
        }
        ImGui::EndChild();
        ImGui::TextDisabled(
            "Hover a choice to preview the related card and full text.");
        break;
    }

    // ── Chain ────────────────────────────────────────────────────────────────
    case WaitType::SelectChain: {
        // Label each option by its effect KIND (derived from the timing
        // window) and its decoded effect TEXT — so an on-summon Trigger Effect
        // and a Quick Effect of the same card are visibly distinct instead of
        // both reading "Activate: <name>".
        const char* kindWord =
            (sel.timing == TimingContext::PostSummonTrigger)   ? "Trigger Effect" :
            (sel.timing == TimingContext::OpponentChainWindow) ? "Chain Response" :
                                                                 "Quick Effect";
        // Compact header — header font, gold. The technical timing-window
        // name is debug-only so normal play reads cleanly.
        UIStyle::PushFont(UIStyle::fHeader);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(UIStyle::C().accentHi));
        ImGui::TextUnformatted(sel.forced ? "You must chain a card"
                                          : "Respond with a chain?");
        ImGui::PopStyleColor();
        UIStyle::PopFont();
        drawOpponentActionHint();
        if (m_debugLog)
            ImGui::TextDisabled("Window: %s", DuelManager::timingName(sel.timing));
        ImGui::Spacing();
        // Content-sized chain list — fits the rows, scrolls only when many.
        const float kChRowH = 46.f;
        float chMaxH = std::max(120.f, kHeightCap - 140.f);
        float chWant = std::max(kChRowH, sel.cards.size() * kChRowH);
        ImGui::BeginChild("##chlist", {bw, std::min(chMaxH, chWant)}, false);
        for (int i = 0; i < (int)sel.cards.size(); i++) {
            auto& c = sel.cards[i];
            std::string nm = c.name.empty() ? ("#" + std::to_string(c.code)) : c.name;
            std::string effFull;
            if (i < (int)sel.chainEffects.size()) {
                const EffectDesc& d = sel.chainEffects[i];
                if (!d.text.empty()) effFull = d.text;
                // Raw desc is debug-only — never in normal gameplay labels.
                else if (d.raw && m_debugLog)
                    effFull = "desc#" + std::to_string(d.raw);
            }
            // Compact action row — title carries the effect kind + card name,
            // a SHORT effect snippet below; the full text goes to the right
            // info panel on hover.
            std::string title = std::string(kindWord) + ": " + nm;
            std::string rowDesc = compactPromptDesc(effFull);
            std::string rid   = "##chrow" + std::to_string(i);
            bool hov = false;
            if (PromptChoiceRow(rid.c_str(), title, rowDesc,
                                bw - 14.f, &hov))
                submitMpChoice(WaitType::SelectChain, (int)i);
            if (hov) {                          // hover -> preview + panel
                m_hoveredCard = c.code;
                m_hoveredInfo = m_db.getCard(c.code);
                m_promptHoverTitle = title;
                m_promptHoverText  = effFull.empty() ? nm : effFull;
                m_promptHoverFrame = ImGui::GetFrameCount();
            }
        }
        ImGui::EndChild();
        // A forced chain must pick a link — no "Pass" option in that case.
        if (!sel.forced && ImGui::Button("Pass (no response)##chain", {bw, 26.f}))
            submitMpChoice(WaitType::SelectChain, -1);
        break;
    }

    // ── Select card ───────────────────────────────────────────────────────────
    case WaitType::SelectCard:
    case WaitType::SelectTribute: {
        // Multi-select card prompt. min..max from the engine; the user toggles
        // rows in/out of `m_selSelIdx` and Confirm sends the original engine
        // indices in a single response. min=max=1 prompts (e.g. a Foolish
        // Burial search) confirm on a single click for fast play.
        // Header: prefer the host's human title ("Select 1 card", "Select
        // 2 cards to Tribute", "Select material") when rendering from a
        // PromptSnapshot; offline keeps the classic header.
        if (usingRemoteField() && m_mpRemoteSelValid &&
            !m_mpRemoteSel.title.empty())
            ImGui::TextColored(COL_ACCENT, "%s", m_mpRemoteSel.title.c_str());
        else
            ImGui::TextColored(COL_ACCENT, "Select Card");
        if (sel.min == sel.max)
            ImGui::Text("Select %d of %d", sel.min, (int)sel.cards.size());
        else
            ImGui::Text("Select %d-%d cards (of %d)",
                        sel.min, sel.max, (int)sel.cards.size());
        ImGui::TextDisabled("Selected: %d / %d",
                            (int)m_selSelIdx.size(),
                            sel.max > 0 ? sel.max : sel.min);

        // Search/filter is UI-only — never re-orders or alters the engine's
        // original indices, only hides non-matching rows.
        ImGui::SetNextItemWidth(bw);
        ImGui::InputTextWithHint("##scfilter",
            "Search name / #code", m_selFilter, sizeof(m_selFilter));
        std::string flt = m_selFilter;
        for (char& c : flt) c = (char)tolower((unsigned char)c);

        const bool fastSingle = (sel.min == 1 && sel.max == 1);
        // Horizontal gallery of big card thumbnails. Click a card to pick it
        // (single-pick) or toggle it (multi-pick); the row scrolls sideways
        // when there are many candidates. Hovering shows the full preview.
        const float kCardW = 104.f, kCardH = 152.f;
        const ImU32 kSelCol = IM_COL32(232, 196, 110, 255);   // gold (selected)
        // Height = card + name line + horizontal scrollbar, so the row never
        // needs a vertical scroll — only sideways to see more options.
        float listH = kCardH + 62.f;   // card + name + scrollbar + padding (fits)
        ImGui::BeginChild("##sclist", {bw, listH}, true,
                          ImGuiWindowFlags_HorizontalScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
        bool first = true;
        for (int i = 0; i < (int)sel.cards.size(); i++) {
            const auto& c = sel.cards[i];
            CardInfo ci = m_db.getCard(c.code);
            std::string nm = c.name.empty() && ci.name.empty()
                ? ("#" + std::to_string(c.code))
                : (c.name.empty() ? ci.name : c.name);
            if (!flt.empty()) {
                std::string lc = nm + "#" + std::to_string(c.code);
                for (char& ch : lc) ch = (char)tolower((unsigned char)ch);
                if (lc.find(flt) == std::string::npos) continue;
            }
            bool already = false;
            for (int x : m_selSelIdx) if (x == i) { already = true; break; }

            if (!first) ImGui::SameLine(0.f, 8.f);
            first = false;
            ImGui::PushID(i);
            ImGui::BeginGroup();
            // Clickable card image (drawn manually for version-proof clicks).
            ImVec2 cp = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##scimg", {kCardW, kCardH});
            bool clicked = ImGui::IsItemClicked();
            bool hov     = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            void* tex = m_rend.getCardTexture(c.code);
            ImVec2 cbr{ cp.x + kCardW, cp.y + kCardH };
            if (tex) dl->AddImage(tex, cp, cbr);
            else     dl->AddRectFilled(cp, cbr, IM_COL32(40, 46, 70, 255), 4.f);
            if (already)   dl->AddRect(cp, cbr, kSelCol, 4.f, 0, 3.5f);
            else if (hov)  dl->AddRect(cp, cbr, IM_COL32(255,255,255,170), 4.f, 0, 2.f);
            if (hov) { m_hoveredCard = c.code; m_hoveredInfo = ci; }
            // Short name under the card.
            ImGui::PushFont(UIStyle::fSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, already
                ? ImGui::ColorConvertU32ToFloat4(kSelCol)
                : ImVec4{0.86f, 0.88f, 0.94f, 1.f});
            // Truncate to keep each cell the card's width (no wrap, which would
            // reflow to the whole child and break the horizontal row).
            std::string shortNm = nm.size() > 13 ? nm.substr(0, 12) + "…" : nm;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX());
            ImGui::TextUnformatted(shortNm.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::EndGroup();
            ImGui::PopID();

            if (clicked) {
                m_hoveredCard = c.code; m_hoveredInfo = ci;
                if (fastSingle) {
                    m_dm.logEvent("Selected card #" + std::to_string(c.code) +
                                  " [" + nm + "] for effect");
                    submitMpChoice(WaitType::SelectCard, i);
                    m_selSelIdx.clear();
                    m_selFilter[0] = 0;
                } else if (already) {
                    m_selSelIdx.erase(std::remove(m_selSelIdx.begin(),
                                                  m_selSelIdx.end(), i),
                                      m_selSelIdx.end());
                } else if ((int)m_selSelIdx.size() < sel.max) {
                    m_selSelIdx.push_back(i);
                }
            }
        }
        // The gallery is a single horizontal row — map the mouse wheel to
        // SIDEWAYS scroll so spinning the wheel pans through the cards.
        if (ImGui::IsWindowHovered()) {
            float wh = ImGui::GetIO().MouseWheel + ImGui::GetIO().MouseWheelH;
            if (wh != 0.f)
                ImGui::SetScrollX(ImGui::GetScrollX() - wh * (kCardW + 8.f));
        }
        ImGui::EndChild();

        if (!fastSingle) {
            bool ok = (int)m_selSelIdx.size() >= sel.min &&
                      (int)m_selSelIdx.size() <= sel.max;
            ImGui::BeginDisabled(!ok);
            if (ImGui::Button("Confirm selection", {bw, 30.f})) {
                std::string codes;
                for (size_t k = 0; k < m_selSelIdx.size(); ++k) {
                    int i = m_selSelIdx[k];
                    codes += (k ? ", #" : "#") +
                             std::to_string(sel.cards[i].code);
                }
                m_dm.logEvent("Confirmed selection: " +
                              std::to_string(m_selSelIdx.size()) +
                              " card(s)  " + codes);
                submitMultiCards(m_selSelIdx);
                m_selSelIdx.clear();
                m_selFilter[0] = 0;
            }
            ImGui::EndDisabled();
        }
        break;
    }

    // ── Select / unselect card (summon materials) ────────────────────────────
    // Horizontal gallery of big card thumbnails — click a card to add it as
    // material; the engine re-asks one card at a time until the count is met.
    case WaitType::SelectUnselect: {
        ImGui::Text("Select material (%d-%d):", sel.min, sel.max);
        ImGui::Spacing();
        const float kCardW = 104.f, kCardH = 152.f;
        float listH = kCardH + 62.f;   // card + name + scrollbar + padding (fits)
        ImGui::BeginChild("##uslist", {bw, listH}, true,
                          ImGuiWindowFlags_HorizontalScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
        bool first = true;
        for (int i = 0; i < (int)sel.cards.size(); i++) {
            const auto& c = sel.cards[i];
            CardInfo ci = m_db.getCard(c.code);
            std::string nm = c.name.empty()
                ? (ci.name.empty() ? ("#" + std::to_string(c.code)) : ci.name)
                : c.name;
            if (!first) ImGui::SameLine(0.f, 8.f);
            first = false;
            ImGui::PushID(i);
            ImGui::BeginGroup();
            ImVec2 cp = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##usimg", {kCardW, kCardH});
            bool clicked = ImGui::IsItemClicked();
            bool hov     = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            void* tex = m_rend.getCardTexture(c.code);
            ImVec2 cbr{ cp.x + kCardW, cp.y + kCardH };
            if (tex) dl->AddImage(tex, cp, cbr);
            else     dl->AddRectFilled(cp, cbr, IM_COL32(40, 46, 70, 255), 4.f);
            if (hov) {
                dl->AddRect(cp, cbr, IM_COL32(255,255,255,170), 4.f, 0, 2.f);
                m_hoveredCard = c.code; m_hoveredInfo = ci;
            }
            ImGui::PushFont(UIStyle::fSmall);
            std::string shortNm = nm.size() > 13 ? nm.substr(0, 12) + "…" : nm;
            ImGui::TextUnformatted(shortNm.c_str());
            ImGui::PopFont();
            ImGui::EndGroup();
            ImGui::PopID();
            if (clicked) submitUnselect(i);
        }
        // Single horizontal row — map the mouse wheel to sideways scroll.
        if (ImGui::IsWindowHovered()) {
            float wh = ImGui::GetIO().MouseWheel + ImGui::GetIO().MouseWheelH;
            if (wh != 0.f)
                ImGui::SetScrollX(ImGui::GetScrollX() - wh * (kCardW + 8.f));
        }
        ImGui::EndChild();
        // "Finish" is legal only when the engine said the selection is
        // finishable/cancelable (m_selection.forced is false in that case).
        if (!sel.forced && UIStyle::PrimaryButton("Finish selection", {-1.f, 30.f}))
            submitUnselect(-1);
        break;
    }

    // ── Waiting ───────────────────────────────────────────────────────────────
    case WaitType::Waiting:
        ImGui::TextDisabled("Waiting for opponent...");
        break;

    default:
        ImGui::TextDisabled("Engine processing...");
        break;
    }
}

// ─── drawTestingBar (content-only) ───────────────────────────────────────────
// ─── drawBottomActionStrip ──────────────────────────────────────────────────
// Replaces the old right-side "command center". When a card is selected, the
// strip shows that card's filtered legal actions (matching code + controller
// + location + sequence — exactly the Stage A action-mapping path). Phase
// buttons (Battle Phase / Main Phase 2 / End Turn) live on the right side
// always and are enabled only when the engine offers them. Click a button →
// the existing m_dm.respondIdleCmd / respondMultipleCards plumbing runs.
void UI::drawBottomActionStrip(int /*w*/, float /*h*/) {
    auto& sel = currentSelection();
    // The bottom-strip prompts (idle / battle / chain / place) belong to
    // the LOCAL player. Pre-MP this was always engine player 0 (the only
    // human seat); in multiplayer the client's local seat is engine
    // player 1, so this needs to compare against localPlayerIndex (which
    // returns 0 offline, 0 for host, 1 for client). Without this, the
    // P2 client never sees their own idle / chain / battle UI — the very
    // bug that left P2 stuck after P1's End Turn.
    const int kLocal = m_net.localPlayerIndex();
    bool inIdle  = (sel.type == WaitType::SelectIdleCmd  && sel.player == kLocal);
    bool inBattle= (sel.type == WaitType::SelectBattleCmd && sel.player == kLocal);
    bool inChain = (sel.type == WaitType::SelectChain     && sel.player == kLocal);
    bool inPlace = (sel.type == WaitType::SelectPlace     && sel.player == kLocal);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
    ImGui::SetCursorPosX(8.f);

    // Left hint — styled coloured label + dimmer description.
    auto hintLine = [](const char* tag, ImU32 tagCol,
                       const char* desc, ImU32 descCol) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        // Tag chip.
        if (tag && tag[0]) {
            UIStyle::PushFont(UIStyle::fSmall);
            ImVec2 ts = ImGui::CalcTextSize(tag);
            float chipW = ts.x + 14.f, chipH = 20.f;
            dl->AddRectFilled(pos, {pos.x + chipW, pos.y + chipH},
                              (tagCol & 0x00FFFFFF) | 0x44000000, 4.f);
            dl->AddRect(pos, {pos.x + chipW, pos.y + chipH},
                        tagCol, 4.f, 0, 1.f);
            dl->AddText({pos.x + 7.f, pos.y + (chipH - ts.y) * 0.5f},
                        tagCol, tag);
            UIStyle::PopFont();
            ImGui::Dummy({chipW + 8.f, chipH});
            ImGui::SameLine(0.f, 0.f);
        }
        if (desc && desc[0]) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(descCol));
            ImGui::TextUnformatted(desc);
            ImGui::PopStyleColor();
        }
    };

    if (inIdle) {
        hintLine("MAIN PHASE", IM_COL32(112, 176, 255, 220),
                 "  Click a card to see its actions",
                 IM_COL32(140, 158, 195, 200));
    } else if (inBattle) {
        int attackers = 0, directCnt = 0, activatable = 0;
        for (const auto& a : sel.idle) {
            if (a.cmd == 1) { ++attackers; if (a.canDirect) ++directCnt; }
            else if (a.cmd == 0) ++activatable;
        }
        char desc[128];
        if (attackers > 0)
            snprintf(desc, sizeof(desc),
                     "  Click a glowing monster to attack  "
                     "(attackers: %d%s)",
                     attackers, directCnt > 0 ? "  •  direct available" : "");
        else if (activatable > 0)
            snprintf(desc, sizeof(desc),
                     "  %d activatable effect%s — or advance phase",
                     activatable, activatable == 1 ? "" : "s");
        else
            snprintf(desc, sizeof(desc),
                     "  No attackers — advance to Main Phase 2 or End Turn");
        hintLine("BATTLE PHASE", IM_COL32(240, 120, 80, 220),
                 desc, IM_COL32(180, 150, 140, 200));
    } else if (inChain) {
        hintLine("CHAIN WINDOW", IM_COL32(220, 110, 210, 220),
                 "  Click a glowing magenta card, or Pass",
                 IM_COL32(180, 148, 178, 200));
    } else if (inPlace) {
        hintLine("PLACE CARD", IM_COL32(110, 210, 130, 220),
                 "  Click a glowing green zone on the field",
                 IM_COL32(140, 178, 148, 200));
    } else if (DuelManager::isRealSelect(sel.type) && sel.player == kLocal) {
        hintLine(nullptr, 0,
                 "Respond to the prompt above",
                 IM_COL32(140, 158, 195, 200));
    } else if (DuelManager::isRealSelect(sel.type) && sel.player != kLocal) {
        std::string who = m_net.peer().displayName.empty()
            ? std::string("opponent") : m_net.peer().displayName;
        const FieldState& fw = currentField();
        char wbuf[128];
        snprintf(wbuf, sizeof(wbuf), "  Waiting for %s  —  %s  (Turn %d, %s)",
            who.c_str(), waitTypeHuman((uint32_t)sel.type),
            fw.turnCount, phaseHuman(fw.phase));
        hintLine("WAITING", IM_COL32(200, 200, 100, 200),
                 wbuf, IM_COL32(150, 150, 120, 180));
    } else if (m_mpHostAuth && m_net.isClient() && m_mpInDuel &&
               m_mpOppPromptWait != 0) {
        std::string who = m_net.peer().displayName.empty()
            ? std::string("opponent") : m_net.peer().displayName;
        const FieldState& fw = currentField();
        char wbuf[128];
        snprintf(wbuf, sizeof(wbuf), "  Waiting for %s  —  %s  (Turn %d, %s)",
            who.c_str(), waitTypeHuman(m_mpOppPromptWait),
            fw.turnCount, phaseHuman(fw.phase));
        hintLine("WAITING", IM_COL32(200, 200, 100, 200),
                 wbuf, IM_COL32(150, 150, 120, 180));
    } else if (m_mpHostAuth && m_net.isClient() && m_mpInDuel &&
               m_mpAwaitingHostUpdate) {
        hintLine("SENT", IM_COL32(140, 210, 255, 200),
                 "  Waiting for host to resolve...",
                 IM_COL32(140, 158, 195, 180));
    } else {
        hintLine(nullptr, 0, "Duel in progress",
                 IM_COL32(100, 118, 155, 160));
    }

    // NOTE: the chain-window "Pass / No Response" button now lives in the
    // centred drawChainResponsePopup (which also names the opponent's action),
    // so it is intentionally NOT duplicated here in the bottom strip.

    // ── Right corner: Log / Tools ghost buttons (always present) ────────────
    // The permanent dev strip is gone; these two small ghosts are the only
    // trace of it in normal play. Phase buttons sit to their left.
    const float kGhostW  = 52.f;
    const float kGhostRsv = 2.f * (kGhostW + 4.f) + 8.f;
    {
        float winW = ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine(winW - kGhostRsv, 0.f);
        ImGui::SetCursorPosY(10.f);
        if (UIStyle::GhostButton(m_logDrawerOpen ? "Log *##logbtn"
                                                 : "Log##logbtn",
                                 {kGhostW, 30.f}))
            m_logDrawerOpen = !m_logDrawerOpen;
        ImGui::SameLine(0.f, 4.f);
        if (UIStyle::GhostButton(m_toolsDrawerOpen ? "Tools*##tools"
                                                   : "Tools##tools",
                                 {kGhostW, 30.f}))
            m_toolsDrawerOpen = !m_toolsDrawerOpen;
        ImGui::SameLine(0.f, 4.f);
        if (UIStyle::GhostButton("?##help", {30.f, 30.f}))
            m_helpOverlayOpen = !m_helpOverlayOpen;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Controls & shortcuts (F1)");
        // Fast turns toggle (offline only — pacing has no effect in MP/replay).
        if (m_net.isOffline() && !m_replayMode) {
            ImGui::SameLine(0.f, 4.f);
            if (UIStyle::GhostButton(m_settings.fastTurns ? "Fast*##fast"
                                                          : "Fast##fast",
                                     {kGhostW, 30.f})) {
                m_settings.fastTurns = !m_settings.fastTurns;
                saveSettings();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Skip the pause between phases");
            // Undo — rewind to your last decision (offline practice).
            ImGui::SameLine(0.f, 4.f);
            bool canUndo = testingRewindAvailable() && m_timeline.applied() > 0;
            if (!canUndo) ImGui::BeginDisabled();
            if (UIStyle::GhostButton("Undo##undo", {kGhostW, 30.f}))
                testingUndoHuman();
            if (!canUndo) ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Undo your last move (Ctrl+Z)");
        }
    }

    // ── Right-aligned phase buttons (left of the ghost corner) ──────────────
    const float kBtnW = 136.f;
    int nBtn = 0;
    if (inIdle   && sel.toBP) ++nBtn;
    if (inBattle && sel.toM2) ++nBtn;
    if ((inIdle || inBattle) && sel.toEP) ++nBtn;
    if (nBtn > 0) {
        float winW = ImGui::GetWindowContentRegionMax().x;
        float rightX = winW - kGhostRsv - 10.f - nBtn * (kBtnW + 6.f);
        ImGui::SameLine(rightX < 0.f ? 0.f : rightX, 0.f);
        ImGui::SetCursorPosY(10.f);
        if (inIdle && sel.toBP) {
            if (UIStyle::SecondaryButton("Battle Phase##bp", {kBtnW, 36.f}))
                submitIdleCmd(6, 0, "Battle Phase");
            ImGui::SameLine(0.f, 6.f);
        }
        if (inBattle && sel.toM2) {
            if (UIStyle::SecondaryButton("Main Phase 2##m2", {kBtnW, 36.f}))
                submitIdleCmd(2, 0, "Main Phase 2");
            ImGui::SameLine(0.f, 6.f);
        }
        if ((inIdle || inBattle) && sel.toEP) {
            const char* lbl = inBattle ? "End Battle Phase##ep"
                                       : "End Turn##ep";
            if (UIStyle::PrimaryButton(lbl, {kBtnW, 36.f}))
                submitIdleCmd(inBattle ? 3 : 7, 0,
                              inBattle ? "End Battle Phase" : "End Turn");
        }
    }
}

// ─── drawCompactPreviewOverlay ──────────────────────────────────────────────
// Small floating preview anchored top-right. Fixed size — long effect text
// scrolls inside; the field below it never reshapes. Spells/Traps NEVER show
// ATK/DEF. Hidden when nothing's hovered or selected.
// True if any chain entry the engine offered comes from a card sitting at
// (player, loc, seq). Used to glow visible chain candidates on the field so
// the player can click the card itself instead of fishing in a side panel.
bool UI::isChainCandidate(uint8_t player, uint8_t loc, uint32_t seq,
                          int* outIdx) const {
    const SelectionRequest& sel = currentSelection();
    // Chain candidates only highlight for the LOCAL player's chain
    // window — pre-MP this was hardcoded to engine player 0; in
    // multiplayer the client's local seat is engine player 1. Use
    // m_net.localPlayerIndex() so the right cards glow on the right
    // machine for the right player.
    if (sel.type != WaitType::SelectChain ||
        sel.player != (uint8_t)m_net.localPlayerIndex()) return false;
    for (size_t i = 0; i < sel.cards.size(); ++i) {
        const CardState& c = sel.cards[i];
        if (c.player == player && c.loc == loc && c.seq == seq) {
            if (outIdx) *outIdx = (int)i;
            return true;
        }
    }
    return false;
}

bool UI::isSelectCandidate(uint8_t player, uint8_t loc, uint32_t seq,
                           int* outIdx) const {
    const SelectionRequest& sel = currentSelection();
    if ((sel.type != WaitType::SelectCard &&
         sel.type != WaitType::SelectTribute &&
         sel.type != WaitType::SelectUnselect) ||
        sel.player != (uint8_t)m_net.localPlayerIndex())
        return false;
    for (size_t i = 0; i < sel.cards.size(); ++i) {
        const CardState& c = sel.cards[i];
        if (c.player == player && (uint8_t)c.loc == loc && c.seq == seq) {
            if (outIdx) *outIdx = (int)i;
            return true;
        }
    }
    return false;
}

// Floating action popup anchored ABOVE the selected card. Replaces the old
// right command center for normal idle/battle actions: only the actions
// matching the selected (code, player, loc, seq) render, in a small fixed-
// width window pinned next to the click. Closes when m_selCode is cleared,
// when the engine state moves out of idle/battle, or when the user clicks
// Cancel. The engine response path is the same as before — same cmd/index
// passed to m_dm.respondIdleCmd().
void UI::drawCardActionPopup(int screenW, int screenH) {
    if (m_selCode == 0) return;
    const SelectionRequest& sel = currentSelection();
    if (sel.type != WaitType::SelectIdleCmd &&
        sel.type != WaitType::SelectBattleCmd) return;

    // Gather actions for THIS selected card only — matched by the FULL
    // identity (code + controller + location + sequence). This is what keeps
    // a field card's action distinct from a same-code card's GY action:
    // clicking the field card shows only its field action; the GY action
    // stays reachable via the GY viewer. Never collapse by code alone.
    std::vector<int> matching;
    matching.reserve(8);
    for (int i = 0; i < (int)sel.idle.size(); ++i) {
        const IdleAction& a = sel.idle[i];
        if (a.code == m_selCode && a.con == m_selPlayer &&
            a.loc == m_selLoc  && a.seq == m_selSeq)
            matching.push_back(i);
    }
    if (m_debugLog)
        m_dm.logEvent(std::string("[ACTION MATCH] clicked con=") +
            std::to_string(m_selPlayer) + " loc=" + std::to_string(m_selLoc) +
            " seq=" + std::to_string(m_selSeq) + " code=" +
            std::to_string(m_selCode) + " -> matched=" +
            std::to_string(matching.size()) + " field action(s) result=" +
            (matching.empty() ? "no" : "yes"));
    if (matching.empty()) return;

    // Size + clamp to screen. Anchor pivot is bottom-centre, so the popup
    // floats above the clicked card. If the card is too close to the top,
    // flip below.
    const float POPUP_W = 220.f;
    float estH = 30.f + matching.size() * 32.f + 30.f;   // rough autosize hint
    if (estH > 280.f) estH = 280.f;
    float px = m_actionAnchorX - POPUP_W * 0.5f;
    if (px < 6.f) px = 6.f;
    if (px + POPUP_W > (float)screenW - 6.f) px = (float)screenW - 6.f - POPUP_W;
    float py = m_actionAnchorY - 6.f;
    bool below = (py - estH < 60.f);
    if (below) py = m_actionAnchorY + 6.f;

    ImGui::SetNextWindowPos({px, py}, ImGuiCond_Always,
                            {0.f, below ? 0.f : 1.f});
    ImGui::SetNextWindowSizeConstraints({POPUP_W, 0.f},
                                        {POPUP_W, (float)screenH * 0.7f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.10f, 0.11f, 0.18f, 0.97f});
    ImGui::PushStyleColor(ImGuiCol_Border,   {1.f,  0.78f, 0.30f, 0.85f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.6f);
    ImGui::Begin("##cardActionPopup", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize  | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings);

    CardInfo ci = m_db.getCard(m_selCode);
    std::string nm = ci.name.empty() ? ("#" + std::to_string(m_selCode))
                                     : ci.name;
    // Header strip behind the card name — small accent block so the popup
    // reads like a tagged context menu, not a floating list of buttons.
    {
        ImDrawList* dlp = ImGui::GetWindowDrawList();
        ImVec2 hp = ImGui::GetCursorScreenPos();
        float  hw = ImGui::GetContentRegionAvail().x;
        dlp->AddRectFilled(hp,
                           {hp.x + hw, hp.y + 22.f},
                           IM_COL32(60, 50, 14, 235), 4.f);
        dlp->AddRect      (hp,
                           {hp.x + hw, hp.y + 22.f},
                           IM_COL32(220, 178, 78, 200), 4.f, 0, 1.2f);
        ImGui::SetCursorScreenPos({hp.x + 6.f, hp.y + 3.f});
        ImGui::TextColored({1.f, 0.92f, 0.55f, 1.f}, "%s", nm.c_str());
        ImGui::SetCursorScreenPos({hp.x, hp.y + 26.f});
    }
    ImGui::Spacing();

    for (int idx : matching) {
        const IdleAction& a = sel.idle[idx];
        const char* verb;
        if (sel.type == WaitType::SelectBattleCmd) {
            // "Attack" or "Attack (Direct)" depending on the engine's
            // can_direct flag for this attacker. The button is one click —
            // the engine handles target selection automatically when there
            // is exactly one or zero possible targets.
            verb = (a.cmd == 1)
                ? (a.canDirect ? "Attack (Direct)" : "Attack")
                : "Activate effect";
        } else if (a.cmd == 5) {
            uint32_t ct = m_db.getCard(a.code).type;
            verb = (ct & (TYPE_SPELL | TYPE_TRAP)) ? "Activate"
                                                  : "Activate effect";
        } else {
            verb = (a.cmd == 0) ? "Summon"          :
                   (a.cmd == 1) ? "Special Summon"  :
                   (a.cmd == 2) ? "Change Position" :
                   (a.cmd == 3) ? "Set monster"     :
                   (a.cmd == 4) ? "Set"             : "Activate";
        }
        // Show the decoded effect description on the same line for cmd 5.
        // The label is truncated to keep the popup compact — hovering the
        // button pushes the FULL text into the right-side info panel.
        std::string lbl = verb;
        if (a.cmd == 5 && !a.effect.text.empty()) {
            std::string et = a.effect.text;
            if (et.size() > 40) et = et.substr(0, 38) + "..";
            lbl = std::string(verb) + ": " + et;
        }
        lbl += "##ap" + std::to_string(idx);
        if (ImGui::Button(lbl.c_str(), {-1.f, 28.f})) {
            // Play the right SFX for this action category. AudioManager is a
            // no-op when files are missing or the user has muted, so this is
            // always safe — and it never delays the engine response (which
            // happens on the same call, below).
            // SFX choice depends on the *prompt* type. Inside Battle Phase
            // (SelectBattleCmd) cmd==1 means "declare attack" and cmd==0
            // means "activate during battle"; outside Battle Phase (idle
            // main phase) cmd==1 means Special Summon. The label is already
            // mapped correctly above; mirror that here for the sound.
            const char* sfx;
            if (sel.type == WaitType::SelectBattleCmd) {
                sfx = (a.cmd == 1) ? "attack" : "activate";
            } else {
                sfx =
                    (a.cmd == 0) ? "summon"          :
                    (a.cmd == 1) ? "special_summon"  :
                    (a.cmd == 3 || a.cmd == 4) ? "set" :
                    (a.cmd == 5) ? "activate"        :
                    (a.cmd == 2) ? "click"           : "click";
            }
            gAudio().play(sfx);
            // Battle Phase debug logs.
            if (sel.type == WaitType::SelectBattleCmd && a.cmd == 1) {
                int directCnt = 0, total = 0;
                for (const auto& x : sel.idle) {
                    if (x.cmd == 1) { ++total; if (x.canDirect) ++directCnt; }
                }
                m_dm.logEvent(std::string("[ATTACK SELECT] attacker #") +
                              std::to_string(a.code) +
                              " [" + a.name + "] engineIdx=" +
                              std::to_string(a.index) +
                              "  legalAttackers=" + std::to_string(total) +
                              "  canDirect=" +
                              (a.canDirect ? "yes" : "no"));
                m_dm.logEvent(std::string("[ATTACK RESPONSE] respondIdleCmd(1, ")
                              + std::to_string(a.index) + ")");
            }
            // Mark the time the explicit action SFX fired so the field-
            // state observer can skip its own follow-up summon/activate/set.
            double tnow = ImGui::GetTime();
            if (a.cmd == 0 || a.cmd == 1)        m_lastSummonSfxAt   = tnow;
            else if (a.cmd == 3 || a.cmd == 4)   m_lastSetSfxAt      = tnow;
            else if (a.cmd == 5)                 m_lastActivateSfxAt = tnow;
            // Animation cue at the clicked card's anchor (m_actionAnchorX/Y).
            // Pulse colour follows the action category — gold for summon /
            // set, cyan for special summon, magenta for activation, red for
            // attack-declared (the MSG_ATTACK that comes back from the
            // engine paints the beam itself).
            ImVec2 anchor{m_actionAnchorX, m_actionAnchorY};
            if (sel.type == WaitType::SelectBattleCmd && a.cmd == 1) {
                m_anim.pulse(anchor, 30.f, IM_COL32(255, 110,  80, 255), 0.45);
                m_anim.ring (anchor, 46.f, IM_COL32(255, 110,  80, 255), 0.55);
            } else if (a.cmd == 0) {
                m_anim.pulse(anchor, 32.f, IM_COL32(255, 214, 108, 255), 0.50);
                m_anim.ring (anchor, 44.f, IM_COL32(255, 214, 108, 255), 0.55);
            } else if (a.cmd == 1) {
                m_anim.pulse(anchor, 36.f, IM_COL32(112, 220, 255, 255), 0.55);
                m_anim.ring (anchor, 56.f, IM_COL32(112, 220, 255, 255), 0.70);
            } else if (a.cmd == 3 || a.cmd == 4) {
                m_anim.pulse(anchor, 28.f, IM_COL32(180, 196, 220, 255), 0.45);
            } else if (a.cmd == 5) {
                m_anim.pulse(anchor, 28.f, IM_COL32(244, 132, 232, 255), 0.55);
                m_anim.ring (anchor, 40.f, IM_COL32(244, 132, 232, 255), 0.55);
            }
            submitIdleCmd(a.cmd, a.index, nm.c_str());
            clearSelection();
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            return;
        }
        // Hovering an action with effect text pushes the FULL description
        // into the right-side info panel ("Selected Prompt Option").
        if (ImGui::IsItemHovered() && !a.effect.text.empty()) {
            m_promptHoverTitle = std::string(verb) + " — " + nm;
            m_promptHoverText  = a.effect.text;
            m_promptHoverFrame = ImGui::GetFrameCount();
        }
    }
    ImGui::Spacing();
    if (ImGui::Button("Cancel##apcancel", {-1.f, 24.f}))
        clearSelection();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// Centered modal for engine prompts that genuinely need a list (MSG_SELECT_CARD
// search, GY/BN/ED viewers, chain options, yes/no, options, unselect) plus
// the paused/done states. Drawn at the centre of the screen, fixed size, so
// the field underneath never reshapes. drawSelectionPanel still hosts all
// case bodies — only the container changed.
void UI::drawCenteredModal(int screenW, int screenH) {
    const SelectionRequest& sel = currentSelection();
    // SelectChain is handled by FIELD GLOW + bottom-strip Pass button. It
    // does NOT open a centered modal — that was the "huge intrusive chain
    // prompt" complaint. SelectPlace is also field-driven. SelectIdleCmd /
    // SelectBattleCmd live in the bottom strip + card-anchored action popup.
    // The centered modal is for real engine prompts + viewers + game-over.
    // In replay AND multiplayer modes we suppress the "engine blocked"
    // trigger — those modes have their own diagnostics (replay desync
    // modal, MP diagnostic modal). An empty generic modal here would just
    // confuse the user. Done / viewer / genuine prompts still open as
    // normal.
    bool blockedTrigger = m_dm.isBlocked();
    if (m_replayMode || m_mpInDuel) blockedTrigger = false;
    bool needModal =
        m_dm.isDone() || blockedTrigger ||
        m_viewerLoc != 0 ||
        (DuelManager::isRealSelect(sel.type) &&
         sel.type != WaitType::SelectIdleCmd  &&
         sel.type != WaitType::SelectBattleCmd &&
         sel.type != WaitType::SelectPlace    &&
         sel.type != WaitType::SelectChain);
    if (!needModal) return;

    // Pick a size that fits the content type. The VIEWER (GY/BN/ED browse) is
    // a compact FLOATING panel anchored to the side — it never dims the field
    // and never covers the centre. Real decision prompts (select-card,
    // option, yes/no) stay centred but are kept compact: short action rows,
    // with full effect text pushed to the right info panel on hover.
    const bool isViewer = (m_viewerLoc != 0);
    const bool listLike =
        sel.type == WaitType::SelectCard     ||
        sel.type == WaitType::SelectTribute  ||
        sel.type == WaitType::SelectUnselect;
    const bool optionLike =
        sel.type == WaitType::SelectOption   ||
        sel.type == WaitType::SelectEffectYn;
    // ── Content-driven sizing ──────────────────────────────────────────────
    // The window is AlwaysAutoResize: it grows to fit exactly the content,
    // so a tiny Yes/No no longer renders inside a tall empty box. A WIDTH-
    // LOCKED size constraint fixes the width to MW and caps the height at
    // maxH; any list that could exceed maxH lives in a child sized to its
    // own row count (see the viewer / option lists), so the window auto-fits
    // below maxH and the child scrolls only when it actually overflows.
    float MW, maxH;
    if (isViewer)        { MW = 440.f; maxH = 600.f; }
    else if (listLike)   { MW = 860.f; maxH = 600.f; }   // wide card gallery
    else if (optionLike) { MW = 460.f; maxH = 440.f; }
    else if (m_dm.isDone()) { MW = 440.f; maxH = 500.f; } // game over panel
    else                 { MW = 400.f; maxH = 340.f; }   // yes/no
    if (MW > (float)screenW - 80.f) MW = (float)screenW - 80.f;
    maxH = std::min((float)screenH - 100.f, maxH);

    // Dim the duel underneath ONLY for real decision modals — the viewer is a
    // browse panel and must keep the field fully visible/interactive. Card /
    // material selection (listLike) also skips the dim so the player can see +
    // click the glowing eligible cards on the board behind the picker.
    if (!isViewer && !listLike) {
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({(float)screenW, (float)screenH});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.f, 0.f, 0.f, 0.f});
        ImGui::PushStyleColor(ImGuiCol_Border,   {0.f, 0.f, 0.f, 0.f});
        ImGui::Begin("##modal_dim", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize  | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoInputs  | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        UIStyle::DrawModalBackdrop(ImGui::GetWindowDrawList(),
                                   {0.f, 0.f},
                                   {(float)screenW, (float)screenH});
        ImGui::End();
        ImGui::PopStyleColor(2);
    }

    // Positioning — decision modals centre via a {0.5,0.5} pivot so they stay
    // centred at ANY auto-resized height; the viewer top-anchors to the right
    // (near the GY/BN/ED piles) and grows downward.
    if (isViewer) {
        const float infoW = ((float)screenW >= 1100.f) ? 330.f : 0.f;
        float mx = (float)screenW - infoW - MW - 28.f;
        if (mx < 16.f) mx = 16.f;
        ImGui::SetNextWindowPos({mx, 70.f}, ImGuiCond_Always, {0.f, 0.f});
    } else {
        // Card/material pickers centre once on appear, then can be dragged
        // aside; other prompts stay locked at centre.
        ImGui::SetNextWindowPos({(float)screenW * 0.5f, (float)screenH * 0.5f},
            listLike ? ImGuiCond_Appearing : ImGuiCond_Always, {0.5f, 0.5f});
    }
    // Width locked to MW (min == max width); height free up to maxH.
    ImGui::SetNextWindowSizeConstraints({MW, 0.f}, {MW, maxH});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.085f, 0.095f, 0.17f, 0.985f});
    ImGui::PushStyleColor(ImGuiCol_Border,   {0.55f, 0.60f, 0.92f, 0.85f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  10.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2{16.f, 14.f});
    ImGuiWindowFlags mflags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings;
    if (!listLike) mflags |= ImGuiWindowFlags_NoMove;   // pickers are draggable
    ImGui::Begin("##engineModal", nullptr, mflags);
    // [PROMPT SIZE] — actual auto-sized result, debug-only.
    if (m_debugLog) {
        ImVec2 wsz = ImGui::GetWindowSize();
        m_dm.logEvent(std::string("[PROMPT SIZE] type=") +
            std::to_string((int)sel.type) +
            " options=" + std::to_string((int)(sel.options.size() +
                                               sel.cards.size())) +
            " size=" + std::to_string((int)wsz.x) + "," +
            std::to_string((int)wsz.y) +
            " scroll=" + (wsz.y >= maxH - 1.f ? "yes" : "no"));
    }
    // Gold accent hairline along the modal's top edge.
    {
        ImDrawList* mdl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        mdl->AddRectFilled({wp.x + 10.f, wp.y},
                           {wp.x + MW - 10.f, wp.y + 2.f},
                           IM_COL32(232, 182, 72, 200), 1.f);
    }
    // Pass the height CAP as the panel's height hint so inner lists can size
    // their scroll children against the available room (the window itself
    // auto-resizes below this).
    drawSelectionPanel((int)MW, (int)maxH);
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ─── drawCardInfoPanel ──────────────────────────────────────────────────────
// Fixed-width right-side card info column (modern-simulator style). Shows
// the hovered card, falling back to the selected card; keeps the LAST card
// when the cursor moves off so the panel never flickers empty mid-read.
// Hidden-info note: every code that reaches m_hoveredCard / m_selCode is
// already visibility-filtered at the hover/click sites (opponent hands and
// face-down cards never set it), so the panel can render freely.
static const char* attrName(uint32_t a) {
    switch (a) {
        case 0x01: return "EARTH"; case 0x02: return "WATER";
        case 0x04: return "FIRE";  case 0x08: return "WIND";
        case 0x10: return "LIGHT"; case 0x20: return "DARK";
        case 0x40: return "DIVINE";
        default:   return nullptr;
    }
}
static const char* raceName(uint64_t r) {
    switch (r) {
        case 0x1:        return "Warrior";      case 0x2:       return "Spellcaster";
        case 0x4:        return "Fairy";        case 0x8:       return "Fiend";
        case 0x10:       return "Zombie";       case 0x20:      return "Machine";
        case 0x40:       return "Aqua";         case 0x80:      return "Pyro";
        case 0x100:      return "Rock";         case 0x200:     return "Winged Beast";
        case 0x400:      return "Plant";        case 0x800:     return "Insect";
        case 0x1000:     return "Thunder";      case 0x2000:    return "Dragon";
        case 0x4000:     return "Beast";        case 0x8000:    return "Beast-Warrior";
        case 0x10000:    return "Dinosaur";     case 0x20000:   return "Fish";
        case 0x40000:    return "Sea Serpent";  case 0x80000:   return "Reptile";
        case 0x100000:   return "Psychic";      case 0x200000:  return "Divine-Beast";
        case 0x400000:   return "Creator God";  case 0x800000:  return "Wyrm";
        case 0x1000000:  return "Cyberse";      case 0x2000000: return "Illusion";
        default:         return nullptr;
    }
}
void UI::drawCardInfoPanel(int w, int /*h*/) {
    const float pad = 10.f;
    ImGui::SetCursorPos({pad, 8.f});
    ImGui::BeginGroup();
    float bw = (float)w - 2.f * pad;

    uint32_t code = m_hoveredCard ? m_hoveredCard
                                  : (m_selCode ? m_selCode : 0u);
    if (code == 0) {
        UIStyle::Heading("Card Info");
        UIStyle::DrawDivider(2.f, 6.f);
        // A hovered prompt option without a source card (system/hint
        // option) still gets its full text shown here.
        if ((ImGui::GetFrameCount() - m_promptHoverFrame) <= 2 &&
            !m_promptHoverText.empty()) {
            ImGui::Dummy({1.f, 6.f});
            ImGui::TextColored({1.f, 0.86f, 0.45f, 1.f},
                               "Selected Prompt Option");
            ImGui::TextColored({1.f, 0.95f, 0.75f, 1.f}, "%s",
                               m_promptHoverTitle.c_str());
            ImGui::PushTextWrapPos(pad + bw);
            ImGui::TextWrapped("%s", m_promptHoverText.c_str());
            ImGui::PopTextWrapPos();
        } else {
            // Empty state — a faint card-silhouette placeholder so the
            // panel reads as intentional, not broken.
            float phH = 180.f, phW = phH * (421.f / 614.f);
            float phx = pad + (bw - phW) * 0.5f;
            ImGui::Dummy({1.f, 24.f});
            ImGui::SetCursorPosX(phx);
            ImVec2 tl = ImGui::GetCursorScreenPos();
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            pdl->AddRect(tl, {tl.x + phW, tl.y + phH},
                         IM_COL32(70, 86, 130, 110), 6.f, 0, 1.2f);
            pdl->AddLine({tl.x + phW * 0.30f, tl.y + phH * 0.5f},
                         {tl.x + phW * 0.70f, tl.y + phH * 0.5f},
                         IM_COL32(70, 86, 130, 90), 1.f);
            ImGui::Dummy({1.f, phH + 14.f});
            ImVec2 hs = ImGui::CalcTextSize("Hover a card to view details");
            ImGui::SetCursorPosX(pad + (bw - hs.x) * 0.5f);
            ImGui::TextDisabled("Hover a card to view details");
        }
        ImGui::EndGroup();
        return;
    }
    CardInfo ci = (m_hoveredCard && m_hoveredInfo.id == code)
                  ? m_hoveredInfo : m_db.getCard(code);
    if (ci.id == 0) {
        ImGui::TextColored(COL_ACCENT, "Card Info");
        ImGui::Separator();
        ImGui::TextDisabled("Unknown card  #%u", (unsigned)code);
        ImGui::EndGroup();
        return;
    }

    // ── Card art (fixed box so the panel never reflows) ───────────────
    // Mounted-card frame: soft ambient glow + drop shadow + double border.
    {
        const float imgH = 226.f;
        const float imgW = imgH * (421.f / 614.f);
        float ix = pad + (bw - imgW) * 0.5f;
        ImGui::SetCursorPosX(ix);
        ImVec2 tl = ImGui::GetCursorScreenPos();
        ImVec2 brI = {tl.x + imgW, tl.y + imgH};
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        UIStyle::DrawGlow(pdl, tl, brI, IM_COL32(90, 130, 220, 130), 4.f, 2);
        pdl->AddRectFilled({tl.x + 3.f, tl.y + 5.f},
                           {brI.x + 3.f, brI.y + 5.f},
                           IM_COL32(0, 0, 0, 140), 4.f);
        void* tex = m_rend.getCardTexture(code);
        if (tex) ImGui::Image((ImTextureID)tex, {imgW, imgH});
        else     ImGui::Dummy({imgW, imgH});
        pdl->AddRect(tl, brI, IM_COL32(118, 138, 190, 220), 3.f, 0, 1.4f);
        pdl->AddRect({tl.x - 1.5f, tl.y - 1.5f}, {brI.x + 1.5f, brI.y + 1.5f},
                     IM_COL32(60, 74, 116, 130), 4.f, 0, 1.f);
        ImGui::Dummy({1.f, 6.f});
    }

    // ── Name (header font, gold) ──────────────────────────────────────
    UIStyle::PushFont(UIStyle::fHeader);
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertU32ToFloat4(UIStyle::C().accentHi));
    ImGui::PushTextWrapPos(pad + bw);
    ImGui::TextWrapped("%s", ci.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    UIStyle::PopFont();

    // ── Type line ─────────────────────────────────────────────────────
    if (ci.type & TYPE_MONSTER) {
        std::string tl = "Monster";
        if (ci.type & TYPE_FUSION)    tl += " / Fusion";
        if (ci.type & TYPE_SYNCHRO)   tl += " / Synchro";
        if (ci.type & TYPE_XYZ)       tl += " / Xyz";
        if (ci.type & TYPE_LINK)      tl += " / Link";
        if (ci.type & TYPE_RITUAL)    tl += " / Ritual";
        if (ci.type & TYPE_PENDULUM)  tl += " / Pendulum";
        if (ci.type & 0x1000)         tl += " / Tuner";
        if (ci.type & 0x20)           tl += " / Effect";
        else if (!(ci.type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ |
                              TYPE_LINK | TYPE_RITUAL)))
            tl += " / Normal";
        ImGui::TextDisabled("%s", tl.c_str());
        // Attribute / race / level line.
        std::string ar;
        if (const char* a = attrName(ci.attribute)) ar += a;
        if (const char* r = raceName(ci.race)) {
            if (!ar.empty()) ar += " / ";
            ar += r;
        }
        char lvl[32];
        if (ci.type & TYPE_LINK)
            snprintf(lvl, sizeof(lvl), "LINK-%d",
                     ci.def > 0 ? ci.def : (int)ci.level);
        else if (ci.type & TYPE_XYZ)
            snprintf(lvl, sizeof(lvl), "Rank %u", ci.level);
        else
            snprintf(lvl, sizeof(lvl), "Lv %u", ci.level);
        ImGui::TextDisabled("%s%s%s", ar.c_str(),
                            ar.empty() ? "" : "  ·  ", lvl);
        // ATK/DEF — chip row so the numbers read at a glance.
        {
            char atkChip[24];
            snprintf(atkChip, sizeof(atkChip), "ATK %d", ci.atk);
            UIStyle::StatusChip(atkChip, IM_COL32(255, 150, 100, 230));
            if (!(ci.type & TYPE_LINK)) {
                char defChip[24];
                snprintf(defChip, sizeof(defChip), "DEF %d", ci.def);
                ImGui::SameLine(0.f, 6.f);
                UIStyle::StatusChip(defChip, IM_COL32(120, 180, 255, 230));
            }
        }
    } else if (ci.type & TYPE_SPELL) {
        const char* sub =
            (ci.type & 0x10000)  ? "Spell — Quick-Play" :
            (ci.type & 0x20000)  ? "Spell — Continuous" :
            (ci.type & 0x40000)  ? "Spell — Equip"      :
            (ci.type & 0x80000)  ? "Spell — Field"      :
            (ci.type & 0x80)     ? "Spell — Ritual"     : "Spell — Normal";
        ImGui::TextDisabled("%s", sub);
    } else if (ci.type & TYPE_TRAP) {
        const char* sub =
            (ci.type & 0x20000)  ? "Trap — Continuous" :
            (ci.type & 0x100000) ? "Trap — Counter"    : "Trap — Normal";
        ImGui::TextDisabled("%s", sub);
    }

    // ── Location / controller / position — grouped CHIPS (controller-
    //    coloured: cyan = your card, red = opponent) ───────────────────
    if (m_infoCtxCode == code) {
        const char* locName =
            (m_infoLoc == LOC_HAND)  ? "Hand"          :
            (m_infoLoc == LOC_MZONE) ? "Monster Zone"  :
            (m_infoLoc == LOC_SZONE) ? "S/T Zone"      :
            (m_infoLoc == LOC_GY)    ? "Graveyard"     :
            (m_infoLoc == LOC_REM)   ? "Banished"      :
            (m_infoLoc == LOC_EXTRA) ? "Extra Deck"    :
            (m_infoLoc == LOC_DECK)  ? "Deck"          : nullptr;
        const char* posName =
            (m_infoLoc != LOC_MZONE && m_infoLoc != LOC_SZONE) ? nullptr :
            (m_infoPos & POS_FACEUP_ATTACK)    ? "face-up ATK"  :
            (m_infoPos & POS_FACEDOWN_ATTACK)  ? "face-down ATK":
            (m_infoPos & POS_FACEUP_DEFENSE)   ? "face-up DEF"  :
            (m_infoPos & POS_FACEDOWN_DEFENSE) ? "set"          : nullptr;
        if (locName) {
            ImGui::Dummy({1.f, 2.f});
            bool mine = (m_infoCon == (uint8_t)m_net.localPlayerIndex());
            UIStyle::StatusChip(mine ? "You" : "Opponent",
                                mine ? IM_COL32(110, 200, 255, 230)
                                     : IM_COL32(240, 120, 110, 230));
            ImGui::SameLine(0.f, 6.f);
            UIStyle::StatusChip(locName, UIStyle::C().textMd);
            if (posName) {
                ImGui::SameLine(0.f, 6.f);
                UIStyle::StatusChip(posName, UIStyle::C().textLo);
            }
        }
    }
    // Card code, small + muted, on its own quiet line.
    UIStyle::PushFont(UIStyle::fSmall);
    ImGui::TextDisabled("#%u", (unsigned)code);
    UIStyle::PopFont();
    UIStyle::DrawDivider(4.f, 6.f);

    // ── Effect text — wraps inside a scroll area; the panel itself never
    //    resizes with text length. While the player hovers a prompt
    //    choice, the decoded option rides on top ("Selected Prompt
    //    Option") so the full text is always comfortably readable. ─────
    bool promptCtx =
        (ImGui::GetFrameCount() - m_promptHoverFrame) <= 2 &&
        !m_promptHoverText.empty();
    ImGui::BeginChild("##cardinfo_text", {bw, -8.f}, false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (promptCtx) {
        ImGui::TextColored({1.f, 0.86f, 0.45f, 1.f}, "Selected Prompt Option");
        ImGui::TextColored({1.f, 0.95f, 0.75f, 1.f}, "%s",
                           m_promptHoverTitle.c_str());
        ImGui::PushTextWrapPos(bw - 14.f);
        ImGui::TextWrapped("%s", m_promptHoverText.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy({1.f, 6.f});
        ImGui::Separator();
        ImGui::TextDisabled("Card text");
        ImGui::Dummy({1.f, 2.f});
    }
    if (ci.desc.empty())
        ImGui::TextDisabled("(no card text available)");
    else {
        ImGui::PushTextWrapPos(bw - 14.f);
        ImGui::TextWrapped("%s", ci.desc.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::EndGroup();
}

void UI::drawCompactPreviewOverlay(int screenW, float topH) {
    uint32_t code = m_hoveredCard ? m_hoveredCard
                                  : (m_selCode ? m_selCode : 0u);
    if (code == 0) return;
    CardInfo ci = (m_hoveredCard && m_hoveredInfo.id == m_hoveredCard)
                  ? m_hoveredInfo : m_db.getCard(code);
    if (ci.id == 0) return;

    // Large-preview toggle: roughly doubles the box so the card art reads
    // cleanly without ever resizing the field underneath.
    const float W = m_largePreview ? 340.f : 240.f;
    const float H = m_largePreview ? 540.f : 380.f;
    ImGui::SetNextWindowPos({(float)screenW - W - 8.f, topH + 6.f});
    ImGui::SetNextWindowSize({W, H});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.08f, 0.09f, 0.16f, 0.96f});
    ImGui::Begin("##preview_overlay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize  | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings);

    void* tex = m_rend.getCardTexture(code);
    float imgW = W - 16.f;
    float imgH = imgW * (614.f / 421.f);
    float imgCap = m_largePreview ? 260.f : 170.f;
    if (imgH > imgCap) { imgH = imgCap; imgW = imgH * (421.f / 614.f); }
    if (tex) ImGui::Image(tex, {imgW, imgH});
    ImGui::TextColored(COL_ACCENT, "%s", ci.name.c_str());

    // Type-aware stats — no ATK/DEF for non-monsters.
    if (ci.type & TYPE_MONSTER) {
        if (ci.type & 0x4000000) {           // Link
            int rating = ci.def > 0 ? ci.def : (int)ci.level;
            ImGui::TextDisabled("Monster — ATK %d   LINK-%d",
                                ci.atk, rating);
        } else if (ci.type & 0x800000) {     // Xyz
            ImGui::TextDisabled("Monster — Rank %d   ATK %d / DEF %d",
                                ci.level, ci.atk, ci.def);
        } else {
            ImGui::TextDisabled("Monster — Lv %d   ATK %d / DEF %d",
                                ci.level, ci.atk, ci.def);
        }
    } else if (ci.type & TYPE_SPELL) {
        const char* sub =
            (ci.type & 0x10000)  ? "Spell (Quick-Play)" :
            (ci.type & 0x20000)  ? "Spell (Continuous)" :
            (ci.type & 0x40000)  ? "Spell (Equip)"      :
            (ci.type & 0x80000)  ? "Spell (Field)"      :
            (ci.type & 0x80)     ? "Spell (Ritual)"     : "Spell";
        ImGui::TextDisabled("%s", sub);
    } else if (ci.type & TYPE_TRAP) {
        const char* sub =
            (ci.type & 0x20000)  ? "Trap (Continuous)" :
            (ci.type & 0x100000) ? "Trap (Counter)"    : "Trap";
        ImGui::TextDisabled("%s", sub);
    } else {
        ImGui::TextDisabled("Unknown type");
    }

    // Effect text inside a scrolling child — long descriptions never push
    // anything outside the fixed overlay.
    if (!ci.desc.empty()) {
        ImGui::BeginChild("##cpdesc", {0.f, 0.f}, false,
            ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextWrapped("%s", ci.desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

void UI::drawTestingBar(int /*w*/) {
    // ── Tools drawer CONTENT (vertical) ─────────────────────────────────
    // Hosted in the floating "##tools_drawer" window opened from the
    // command bar's Tools ghost button. Everything here is dev/debug
    // surface — invisible during normal play.
    ImGui::TextColored(COL_ACCENT, "Tools");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50.f);
    if (ImGui::SmallButton("Close##toolsdrawer")) m_toolsDrawerOpen = false;
    UIStyle::DrawDivider(2.f, 6.f);

    UIStyle::Subtle("Visual");
    ImGui::Checkbox("Names on field", &m_showFieldNames);
    ImGui::Checkbox("Large preview",  &m_largePreview);
    ImGui::Checkbox("Zone labels",    &m_showZoneLabels);
    ImGui::Checkbox("Legal glow",     &m_showLegalGlow);

    UIStyle::DrawDivider(6.f, 6.f);
    UIStyle::Subtle("Audio");
    bool sfxMuted = gAudio().muted();
    if (ImGui::Checkbox("Mute SFX", &sfxMuted))
        gAudio().setMuted(sfxMuted);

    // ── Practice tools (offline only) ────────────────────────────────────
    // Testing Mode (rewind) is a legitimate player practice feature, so it
    // lives here in normal play — not behind Developer mode.
    if (m_net.isOffline()) {
        UIStyle::DrawDivider(6.f, 6.f);
        UIStyle::Subtle("Practice");
        if (UIStyle::DangerButton("Restart Duel (new seed)", {-1.f, 30.f}) &&
            m_deck0Path[0] && m_deck1Path[0]) {
            finalizeReplay("restart");
            startOfflineDuelWithCoinToss(m_deck0Path, m_deck1Path, 8000, 5, 1);
            m_viewerLoc = 0;
        }
        if (ImGui::Checkbox("Testing Mode (rewind)", &m_testingMode)) {
            m_snap.setEnabled(m_testingMode);
            m_timeline.setEnabled(m_testingMode);
        }
        if (m_testingMode) {
            UIStyle::DrawDivider(6.f, 6.f);
            drawTestingTimeline();
        }
    }

    // ── Developer-only tools (hidden for release) ────────────────────────
    if (m_settings.developerMode) {
        UIStyle::DrawDivider(6.f, 6.f);
        UIStyle::Subtle("Developer");
        ImGui::Checkbox("Layout guides", &m_showLayoutGuides);
        if (ImGui::Checkbox("Debug Log", &m_debugLog))
            m_dm.setDebugMessages(m_debugLog);
    }
}

// ─── Testing Mode timeline panel (inside the Tools drawer) ──────────────────
void UI::drawTestingTimeline() {
    UIStyle::Subtle("Timeline");
    if (!testingRewindAvailable()) {
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(UIStyle::C().textLo));
        ImGui::TextWrapped(m_replayMode
            ? "Rewind is disabled during replay playback."
            : (!m_net.isOffline()
                ? "Testing rewind is available in offline testing mode only."
                : "Start an offline duel to record a timeline."));
        ImGui::PopStyleColor();
        return;
    }

    const int total   = m_timeline.size();
    const int applied = m_timeline.applied();

    // ── Step / restart controls ────────────────────────────────────────
    if (ImGui::Button("|< Start")) testingJumpTo(0, "restart");
    ImGui::SameLine(0.f, 4.f);
    bool canBack = applied > 0;
    if (!canBack) ImGui::BeginDisabled();
    if (ImGui::Button("< Back")) testingStepBack();
    if (!canBack) ImGui::EndDisabled();
    ImGui::SameLine(0.f, 4.f);
    bool canFwd = applied < total;
    if (!canFwd) ImGui::BeginDisabled();
    if (ImGui::Button("Fwd >")) testingStepForward();
    if (!canFwd) ImGui::EndDisabled();
    ImGui::SameLine(0.f, 4.f);
    if (!canFwd) ImGui::BeginDisabled();
    if (ImGui::Button("Head >|")) testingJumpTo(total, "to-head");
    if (!canFwd) ImGui::EndDisabled();

    ImGui::TextDisabled("Step %d / %d%s", applied, total,
                        m_timeline.atHead() ? "  (live)" : "  (rewound)");

    // ── Action list — click a row to jump to AFTER that action ──────────
    float listH = std::min(220.f,
                           std::max(40.f, (float)(total + 1) * 22.f + 8.f));
    ImGui::BeginChild("##tl_list", {-1.f, listH}, true);
    // Row 0 = "Duel start" (apply 0 responses).
    {
        bool cur = (applied == 0);
        if (cur) ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(UIStyle::C().accentHi));
        if (ImGui::Selectable("● Duel start##tl0", cur))
            testingJumpTo(0, "list-click");
        if (cur) ImGui::PopStyleColor();
    }
    for (int i = 0; i < total; ++i) {
        const edo::TestingAction& a = m_timeline.actions()[(size_t)i];
        // applied == i+1  → this action is the current head.
        bool cur     = (applied == i + 1);
        bool future  = (i + 1 > applied);
        char id[64];
        snprintf(id, sizeof(id), "%s##tl%d", a.label.c_str(), i + 1);
        if (cur) ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(UIStyle::C().accentHi));
        else if (future) ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(UIStyle::C().textMuted));
        if (ImGui::Selectable(id, cur))
            testingJumpTo(i + 1, "list-click");
        if (cur || future) ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    if (!m_testingLastRestore.empty())
        ImGui::TextDisabled("Last restore: %s", m_testingLastRestore.c_str());
    // Debug diagnostics.
    if (m_debugLog) {
        ImGui::TextDisabled("rebuilding=%s  seed=%llu",
            m_testingRebuilding ? "yes" : "no",
            (unsigned long long)m_timeline.root().seed);
    }
}

// ─── Deck Builder ─────────────────────────────────────────────────────────────
// ─── Deck Builder — three-column modern editor ──────────────────────────────
//
// Layout (replaces the previous 50/50 search-vs-list pane that still felt
// like a developer tool):
//
//   ┌─ top bar ────────────────────────────────────────────────────────────┐
//   │ < Back   Deck Builder      [deck combo] [deck name] [Save] [Refresh] │
//   ├──────────────┬───────────────────────────────────┬───────────────────┤
//   │ Card Search  │ Deck Editor — Main + Extra + Side │ Card Preview      │
//   └──────────────┴───────────────────────────────────┴───────────────────┘
//
// Hovering any card (search row OR a deck-grid tile) updates the preview
// panel on the right. Clicking a deck tile removes one copy.
// ── Deck consistency calculator (#2/#3) ──────────────────────────────────────
// Per-card role tags are persisted globally in assets/card_tags.txt (one
// "code=tag" line per tagged card) so a card keeps its role across decks.
void UI::loadCardTags() {
    m_cardTags.clear();
    std::ifstream f("assets/card_tags.txt");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        try {
            uint32_t code = (uint32_t)std::stoul(line.substr(0, eq));
            int tag = std::stoi(line.substr(eq + 1));
            if (code && tag > 0 && tag <= 3) m_cardTags[code] = tag;
        } catch (...) {}
    }
}

void UI::saveCardTags() {
    std::ofstream f("assets/card_tags.txt", std::ios::trunc);
    if (!f) return;
    for (const auto& kv : m_cardTags)
        if (kv.second != 0) f << kv.first << "=" << kv.second << "\n";
}

// P(zero of K "successes" in a hand of h drawn from a deck of N) — the
// hypergeometric tail, computed as a stable running product (no big factorials).
static double hyperZero(int N, int K, int h) {
    if (K <= 0 || h <= 0 || N <= 0) return 1.0;
    if (h > N) h = N;
    double p = 1.0;
    for (int i = 0; i < h; ++i) {
        int good = N - K - i;                 // non-success cards still in deck
        if (good <= 0) return 0.0;            // can't avoid drawing a success
        p *= (double)good / (double)(N - i);
    }
    return p;
}

// #2 — draw a random opening hand of n cards from the current main deck.
void UI::drawSampleHand(int n) {
    m_sampleHand.clear();
    std::vector<uint32_t> deck = m_editDeck.main;
    if (deck.empty()) return;
    std::random_device rd; std::mt19937 g(rd());
    std::shuffle(deck.begin(), deck.end(), g);
    n = std::min(n, (int)deck.size());
    m_sampleHand.assign(deck.begin(), deck.begin() + n);
    gAudio().play("draw");
}

void UI::drawDeckConsistency() {
    if (m_consistencyOpen) {
        ImGui::OpenPopup("Deck Consistency");
        m_consistencyOpen = false;
    }
    ImGui::SetNextWindowSize({660.f, 0.f}, ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Deck Consistency", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
    ImGui::TextUnformatted("Deck Consistency");
    if (UIStyle::fHeader) ImGui::PopFont();
    ImGui::TextDisabled("Tag each main-deck card by its role, then read your "
                        "opening odds. A 'Starter' is a 1-card combo.");
    ImGui::Separator();

    // ── Deck breakdown (#3): type counts + level/rank curve ──────────────────
    {
        int mons = 0, spells = 0, traps = 0;
        int lvlCount[13] = {0};   // levels/ranks 1..12 (index 0 unused)
        for (uint32_t code : m_editDeck.main) {
            CardInfo ci = m_db.getCard(code);
            if (ci.type & TYPE_MONSTER) {
                mons++;
                int lv = ci.level;
                if (lv >= 1 && lv <= 12) lvlCount[lv]++;
            } else if (ci.type & TYPE_SPELL) spells++;
            else if (ci.type & TYPE_TRAP)    traps++;
        }
        if (UIStyle::fSmall) ImGui::PushFont(UIStyle::fSmall);
        ImGui::TextDisabled("DECK BREAKDOWN");
        if (UIStyle::fSmall) ImGui::PopFont();
        ImGui::TextColored({0.95f, 0.80f, 0.40f, 1.f}, "Monsters %d", mons);
        ImGui::SameLine(0.f, 14.f);
        ImGui::TextColored({0.40f, 0.80f, 0.55f, 1.f}, "Spells %d", spells);
        ImGui::SameLine(0.f, 14.f);
        ImGui::TextColored({0.85f, 0.40f, 0.65f, 1.f}, "Traps %d", traps);
        // Compact level/rank histogram.
        int maxLv = 1;
        for (int l = 1; l <= 12; ++l) maxLv = std::max(maxLv, lvlCount[l]);
        ImDrawList* hdl = ImGui::GetWindowDrawList();
        ImVec2 hp = ImGui::GetCursorScreenPos();
        float colW = 26.f, gap = 4.f, barMaxH = 34.f;
        float baseY = hp.y + barMaxH + 2.f;
        for (int l = 1; l <= 12; ++l) {
            float x = hp.x + (l - 1) * (colW + gap);
            float h = (lvlCount[l] > 0)
                        ? 4.f + barMaxH * (lvlCount[l] / (float)maxLv) : 2.f;
            ImU32 c = lvlCount[l] > 0 ? IM_COL32(120, 160, 240, 255)
                                      : IM_COL32(60, 66, 86, 160);
            hdl->AddRectFilled({x, baseY - h}, {x + colW, baseY}, c, 2.f);
            char lab[8]; snprintf(lab, sizeof(lab), "%d", l);
            ImVec2 ts = ImGui::CalcTextSize(lab);
            hdl->AddText({x + (colW - ts.x) * 0.5f, baseY + 1.f},
                         IM_COL32(150, 156, 176, 255), lab);
            if (lvlCount[l] > 0) {
                char cnt[8]; snprintf(cnt, sizeof(cnt), "%d", lvlCount[l]);
                ImVec2 cs = ImGui::CalcTextSize(cnt);
                hdl->AddText({x + (colW - cs.x) * 0.5f, baseY - h - 13.f},
                             IM_COL32(200, 214, 245, 255), cnt);
            }
        }
        ImGui::Dummy({12 * (colW + gap), barMaxH + 18.f});
        ImGui::TextDisabled("Level / Rank distribution");
    }
    ImGui::Separator();

    // ── Sample opening hand (#2) ─────────────────────────────────────────────
    {
        if (UIStyle::fSmall) ImGui::PushFont(UIStyle::fSmall);
        ImGui::TextDisabled("SAMPLE OPENING HAND");
        if (UIStyle::fSmall) ImGui::PopFont();
        if (UIStyle::GhostButton("Draw 5", {90.f, 26.f})) drawSampleHand(5);
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::GhostButton("Draw 6", {90.f, 26.f})) drawSampleHand(6);
        if (!m_sampleHand.empty()) {
            ImGui::SameLine(0.f, 6.f);
            if (UIStyle::GhostButton("Redraw", {90.f, 26.f}))
                drawSampleHand((int)m_sampleHand.size());
        }
        if (m_sampleHand.empty()) {
            ImGui::TextDisabled("Draw a random opening hand to feel the deck.");
        } else {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 hp = ImGui::GetCursorScreenPos();
            float cw = 50.f, ch = 73.f, gap = 6.f;
            for (size_t i = 0; i < m_sampleHand.size(); ++i) {
                ImVec2 p0 {hp.x + i * (cw + gap), hp.y};
                ImVec2 p1 {p0.x + cw, p0.y + ch};
                if (void* tex = m_rend.getCardTexture(m_sampleHand[i]))
                    dl->AddImageRounded((ImTextureID)tex, p0, p1, {0,0}, {1,1},
                                        IM_COL32_WHITE, 3.f);
                else
                    dl->AddRectFilled(p0, p1, IM_COL32(30, 36, 56, 255), 3.f);
                dl->AddRect(p0, p1, IM_COL32(70, 84, 130, 220), 3.f, 0, 1.f);
                if (ImGui::IsMouseHoveringRect(p0, p1))
                    ImGui::SetTooltip("%s",
                        m_db.getCard(m_sampleHand[i]).name.c_str());
            }
            ImGui::Dummy({m_sampleHand.size() * (cw + gap), ch + 4.f});
        }
    }
    ImGui::Separator();

    // Distinct main-deck cards (code -> copies), sorted by name.
    std::vector<std::pair<uint32_t, int>> distinct;
    {
        std::unordered_map<uint32_t, int> cnt;
        for (uint32_t c : m_editDeck.main) cnt[c]++;
        distinct.assign(cnt.begin(), cnt.end());
        std::sort(distinct.begin(), distinct.end(), [&](auto& a, auto& b) {
            return m_db.getCard(a.first).name < m_db.getCard(b.first).name;
        });
    }
    const int N = (int)m_editDeck.main.size();

    // Role palette, shared with the deck-tile badges (S/E/N corner pip).
    const ImU32 kRoleCol[4] = {
        IM_COL32(150, 150, 160, 255),   // 0 Other  (grey)
        IM_COL32( 90, 215, 125, 255),   // 1 Starter (green)
        IM_COL32(110, 180, 255, 255),   // 2 Engine  (blue)
        IM_COL32(210, 130, 240, 255),   // 3 Non-eng (purple)
    };
    const char* kRoleShort[4] = { "—", "Starter", "Engine", "Non-eng" };

    // Aggregate by role (weighted by copies) up-front so the summary sits on top.
    int roleCount[4] = {0, 0, 0, 0};
    for (auto& d : distinct) {
        auto it = m_cardTags.find(d.first);
        int t = (it == m_cardTags.end()) ? 0 : it->second;
        if (t >= 0 && t < 4) roleCount[t] += d.second;
    }
    int starters = roleCount[1], engine = roleCount[2], nonEng = roleCount[3];

    // ── Summary chips (counts per role) ──────────────────────────────────────
    auto roleChip = [&](const char* label, int n, ImU32 col) {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
        ImGui::PushStyleColor(ImGuiCol_Button,        {c.x, c.y, c.z, 0.18f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {c.x, c.y, c.z, 0.18f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {c.x, c.y, c.z, 0.18f});
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        char buf[64]; snprintf(buf, sizeof(buf), "%s  %d", label, n);
        ImGui::Button(buf); ImGui::PopStyleColor(4);
    };
    roleChip("Main", N, IM_COL32(200, 200, 210, 255)); ImGui::SameLine();
    roleChip("Starter", starters, kRoleCol[1]); ImGui::SameLine();
    roleChip("Engine", engine, kRoleCol[2]); ImGui::SameLine();
    roleChip("Non-eng", nonEng, kRoleCol[3]);
    ImGui::Spacing();

    // ── Per-card tagging list (thumbnail · name · role toggles) ──────────────
    ImGui::BeginChild("##tags", {-1.f, 300.f}, true);
    if (distinct.empty())
        ImGui::TextDisabled("Add cards to the Main Deck to analyse it.");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (auto& d : distinct) {
        uint32_t code = d.first;
        ImGui::PushID((int)code);
        int& tag = m_cardTags[code];          // default-inserts 0 (Other)
        std::string nm = m_db.getCard(code).name;
        if (nm.empty()) nm = "#" + std::to_string(code);

        float rowY = ImGui::GetCursorScreenPos().y;
        // Thumbnail.
        if (void* tex = m_rend.getCardTexture(code))
            ImGui::Image(tex, {26.f, 38.f});
        else
            ImGui::Dummy({26.f, 38.f});
        ImGui::SameLine();

        // Name + copies, vertically centred against the thumb.
        ImGui::BeginGroup();
        ImGui::Dummy({0.f, 3.f});
        ImGui::Text("%dx", d.second);
        ImGui::SameLine(0.f, 6.f);
        float nameW = 230.f;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + nameW);
        ImGui::TextUnformatted(nm.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        // Role toggle buttons, right-aligned.
        const char* kBtn[4] = { "O", "S", "E", "N" };
        float btnW = 26.f, gap = 4.f;
        float startX = ImGui::GetWindowContentRegionMax().x - (btnW * 4 + gap * 3);
        ImGui::SameLine();
        ImGui::SetCursorPosX(startX);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.f);
        for (int r = 0; r < 4; ++r) {
            ImGui::PushID(r);
            bool active = (tag == r);
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(kRoleCol[r]);
            ImGui::PushStyleColor(ImGuiCol_Button,
                active ? ImVec4{c.x, c.y, c.z, 0.85f} : ImVec4{c.x, c.y, c.z, 0.14f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {c.x, c.y, c.z, 0.55f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {c.x, c.y, c.z, 0.95f});
            ImGui::PushStyleColor(ImGuiCol_Text,
                active ? ImVec4{0.06f, 0.06f, 0.08f, 1.f} : ImVec4{c.x, c.y, c.z, 1.f});
            if (ImGui::Button(kBtn[r], {btnW, 34.f})) { tag = r; saveCardTags(); }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", kRoleShort[r]);
            ImGui::PopStyleColor(4);
            ImGui::PopID();
            if (r < 3) ImGui::SameLine(0.f, gap);
        }
        (void)rowY; (void)dl;
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Spacing();

    auto rateCol = [](double p) -> ImVec4 {
        if (p >= 0.85) return {0.45f, 0.85f, 0.50f, 1.f};   // green
        if (p >= 0.70) return {0.95f, 0.80f, 0.35f, 1.f};   // amber
        return {0.95f, 0.50f, 0.45f, 1.f};                  // red
    };
    double open5 = 1.0 - hyperZero(N, starters, 5);
    double open6 = 1.0 - hyperZero(N, starters, 6);
    double brick5 = hyperZero(N, starters, 5);

    // Visual odds bars.
    auto oddsBar = [&](const char* label, double p) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(190.f);
        ImVec4 col = rateCol(p);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        char ov[16]; snprintf(ov, sizeof(ov), "%.1f%%", p * 100.0);
        ImGui::ProgressBar((float)p, {-1.f, 18.f}, ov);
        ImGui::PopStyleColor();
    };
    if (UIStyle::fSmall) ImGui::PushFont(UIStyle::fSmall);
    ImGui::TextDisabled("ODDS TO OPEN AT LEAST ONE STARTER");
    if (UIStyle::fSmall) ImGui::PopFont();
    ImGui::Spacing();
    oddsBar("Going first  (5)", open5);
    oddsBar("Going second (6)", open6);
    ImGui::Spacing();
    ImGui::TextColored(rateCol(1.0 - brick5),
        "Brick chance (0 starters on 5 cards): %.1f%%", brick5 * 100.0);
    ImGui::TextDisabled("Tip: 40-card decks usually aim for ~90%%+ to open a starter.");

    ImGui::Spacing();
    if (UIStyle::GhostButton("Close", {-1.f, 30.f})) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ── Match history + win/loss stats (#8) ──────────────────────────────────────
void UI::recordMatch(const std::string& myDeck, const std::string& oppDeck,
                     char result) {
    MatchRecord r;
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[24];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    r.when = buf; r.myDeck = myDeck; r.oppDeck = oppDeck; r.result = result;
    m_matchHistory.push_back(r);
    auto san = [](std::string s) {
        for (char& c : s) if (c == '|' || c == '\n' || c == '\r') c = ' ';
        return s;
    };
    std::ofstream f("assets/match_history.txt", std::ios::app);
    if (f) f << r.when << "|" << san(myDeck) << "|" << san(oppDeck) << "|"
             << result << "\n";
}

void UI::loadMatchHistory() {
    m_matchHistory.clear();
    std::ifstream f("assets/match_history.txt");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        std::vector<std::string> parts; size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i)
            if (i == line.size() || line[i] == '|') {
                parts.push_back(line.substr(start, i - start)); start = i + 1;
            }
        if (parts.size() >= 4 && !parts[3].empty()) {
            MatchRecord r;
            r.when = parts[0]; r.myDeck = parts[1]; r.oppDeck = parts[2];
            r.result = parts[3][0];
            m_matchHistory.push_back(r);
        }
    }
}

void UI::drawHistory() {
    if (m_historyOpen) { ImGui::OpenPopup("Match History"); m_historyOpen = false; }
    ImGui::SetNextWindowSize({640.f, 0.f}, ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Match History", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
    ImGui::TextUnformatted("Match History");
    if (UIStyle::fHeader) ImGui::PopFont();

    if (m_matchHistory.empty()) {
        ImGui::TextDisabled("No duels recorded yet. Finish an offline duel "
                            "and your result lands here.");
        ImGui::Spacing();
        if (UIStyle::GhostButton("Close", {-1.f, 30.f})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    int W = 0, L = 0, D = 0;
    std::map<std::string, std::array<int, 3>> perDeck;   // myDeck -> {W,L,D}
    for (const auto& r : m_matchHistory) {
        int idx = (r.result == 'W') ? 0 : (r.result == 'L') ? 1 : 2;
        if (idx == 0) ++W; else if (idx == 1) ++L; else ++D;
        perDeck[r.myDeck][idx]++;
    }
    int decided = W + L;
    double overall = decided ? (double)W / decided * 100.0 : 0.0;
    ImGui::Text("Overall:  %d-%d-%d   (%.0f%% win rate)", W, L, D, overall);
    ImGui::Separator();

    ImGui::TextDisabled("By deck");
    if (ImGui::BeginTable("##bydeck", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Deck");
        ImGui::TableSetupColumn("W-L-D", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Win %", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableHeadersRow();
        for (auto& kv : perDeck) {
            int dw = kv.second[0], dl = kv.second[1], dd = kv.second[2];
            int dec = dw + dl;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(kv.first.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d-%d-%d", dw, dl, dd);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f%%", dec ? (double)dw / dec * 100.0 : 0.0);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Recent");
    ImGui::BeginChild("##recent", {-1.f, 170.f}, true);
    for (int i = (int)m_matchHistory.size() - 1, shown = 0;
         i >= 0 && shown < 30; --i, ++shown) {
        const MatchRecord& r = m_matchHistory[i];
        ImVec4 col = r.result == 'W' ? ImVec4{0.45f, 0.85f, 0.5f, 1.f}
                   : r.result == 'L' ? ImVec4{0.95f, 0.5f, 0.45f, 1.f}
                                     : ImVec4{0.8f, 0.8f, 0.8f, 1.f};
        const char* rl = r.result == 'W' ? "WIN " : r.result == 'L' ? "LOSS" : "DRAW";
        ImGui::TextColored(col, "%s", rl);
        ImGui::SameLine(0.f, 8.f);
        ImGui::Text("%s   %s  vs  %s", r.when.c_str(),
                    r.myDeck.c_str(), r.oppDeck.c_str());
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (UIStyle::DangerButton("Clear history", {150.f, 30.f})) {
        m_matchHistory.clear();
        std::ofstream f("assets/match_history.txt", std::ios::trunc);  // wipe
    }
    ImGui::SameLine(0.f, 8.f);
    if (UIStyle::GhostButton("Close", {-1.f, 30.f})) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ── Puzzle / Challenge mode (#4) ─────────────────────────────────────────────
//
// Puzzles live in assets/puzzles/*.puzzle as a small line-based format:
//   name=...        difficulty=...   desc=...    goal=...
//   lp=8000         opplp=3000
//   <me|opp> <zone> [pos] <code>     ; one card per line
// zones: hand mzone szone grave banish deck extra ; pos: atk def set setdef up
//
void UI::loadPuzzles() {
    m_puzzles.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory("assets/puzzles", ec)) return;
    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator("assets/puzzles", ec))
        if (e.path().extension() == ".puzzle")
            files.push_back(e.path().string());
    std::sort(files.begin(), files.end());

    auto zoneOf = [](const std::string& s) -> uint32_t {
        if (s == "hand")   return LOC_HAND;
        if (s == "mzone")  return LOC_MZONE;
        if (s == "szone")  return LOC_SZONE;
        if (s == "grave")  return LOC_GY;
        if (s == "banish") return LOC_REM;
        if (s == "deck")   return LOC_DECK;
        if (s == "extra")  return LOC_EXTRA;
        return 0;
    };
    auto posOf = [](const std::string& s) -> uint32_t {
        if (s == "def")    return POS_FACEUP_DEFENSE;
        if (s == "set" || s == "setdef") return POS_FACEDOWN_DEFENSE;
        if (s == "up")     return POS_FACEUP_ATTACK | POS_FACEUP_DEFENSE;
        return POS_FACEUP_ATTACK;   // "atk" / default
    };

    for (auto& path : files) {
        std::ifstream f(path);
        if (!f) continue;
        PuzzleEntry pe;
        pe.setup.lpYou = 8000; pe.setup.lpOpp = 8000;
        std::string line;
        while (std::getline(f, line)) {
            // Trim CR + leading space.
            while (!line.empty() && (line.back()=='\r' || line.back()=='\n'))
                line.pop_back();
            size_t s = line.find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            line = line.substr(s);
            if (line[0] == '#' || line[0] == ';') continue;
            auto eq = line.find('=');
            if (eq != std::string::npos &&
                line.find(' ') > eq) {           // key=value line
                std::string k = line.substr(0, eq), v = line.substr(eq + 1);
                if      (k == "name")       pe.setup.name = v;
                else if (k == "goal")       pe.setup.goal = v;
                else if (k == "difficulty") pe.difficulty = v;
                else if (k == "desc")       pe.desc = v;
                else if (k == "mode")       pe.setup.boardBreak = (v == "break");
                else if (k == "lp")         pe.setup.lpYou = (uint32_t)atoi(v.c_str());
                else if (k == "opplp")      pe.setup.lpOpp = (uint32_t)atoi(v.c_str());
                continue;
            }
            // Card line: tokens separated by whitespace.
            std::vector<std::string> tok; std::string cur;
            std::istringstream iss(line);
            while (iss >> cur) tok.push_back(cur);
            if (tok.size() < 3) continue;
            DuelManager::PuzzleCard pc;
            pc.player = (tok[0] == "opp") ? 1 : 0;
            pc.loc    = zoneOf(tok[1]);
            if (!pc.loc) continue;
            // Optional position token sits between zone and code.
            if (tok.size() >= 4) {
                pc.pos  = posOf(tok[2]);
                pc.code = (uint32_t)strtoul(tok[3].c_str(), nullptr, 10);
            } else {
                pc.pos  = posOf("atk");
                pc.code = (uint32_t)strtoul(tok[2].c_str(), nullptr, 10);
            }
            if (pc.code) pe.setup.cards.push_back(pc);
        }
        // Anti-deckout filler: give each side a couple of spare deck cards if
        // the puzzle didn't specify any. (Pot of Greed code — never drawn, the
        // human goes first with no draw and the opponent stays passive.)
        bool youHaveDeck = false, oppHaveDeck = false;
        for (auto& c : pe.setup.cards) {
            if (c.loc == LOC_DECK) (c.player ? oppHaveDeck : youHaveDeck) = true;
        }
        if (!youHaveDeck) { pe.setup.deckYou = {55144522, 55144522}; }
        if (!oppHaveDeck) { pe.setup.deckOpp = {55144522, 55144522}; }
        if (!pe.setup.name.empty() && !pe.setup.cards.empty())
            m_puzzles.push_back(std::move(pe));
    }
}

void UI::startPuzzleByIndex(int idx) {
    if (idx < 0 || idx >= (int)m_puzzles.size()) return;
    const PuzzleEntry& pe = m_puzzles[idx];
    // Tear down any prior replay/testing capture so puzzle responses are never
    // appended to a stale stream (puzzles are not recorded).
    finalizeReplay("entering puzzle");
    m_replayMode  = false;
    m_matchActive = false;   // a puzzle is not a match
    m_dm.setLocalMode(true);
    m_snap.clear();

    bool ok = false;
    if (pe.setup.boardBreak) {
        // Board-break: load the chosen deck, go SECOND, opponent defends.
        if (m_deckFiles.empty()) refreshDeckFiles();
        if (m_deckFiles.empty()) {
            pushToast("No decks found — build one in the Deck Builder first.",
                      IM_COL32(232, 182, 72, 255), 3.0);
            gAudio().play("error");
            return;
        }
        if (m_puzzleDeckIdx < 0 || m_puzzleDeckIdx >= (int)m_deckFiles.size())
            m_puzzleDeckIdx = 0;
        Deck humanDeck = loadYdk("assets/decks/" + m_deckFiles[m_puzzleDeckIdx]);
        // You control team 1 and go second — the UI must view the field and
        // route input from seat 1, or the board shows on your side and the
        // engine looks "stuck waiting for opponent".
        m_dm.setHumanSeat(1);
        m_net.setSeatOverride(1);
        ok = m_dm.startBoardBreak(pe.setup, humanDeck);
    } else {
        // Classic "solve it this turn" — you go first, opponent passive.
        m_dm.setHumanSeat(0);
        m_net.clearSeatOverride();
        m_dm.setPassiveAI(true);
        m_dm.setNoShuffle(true);
        ok = m_dm.startPuzzle(pe.setup);
    }
    if (!ok) {
        pushToast("Puzzle failed to start", IM_COL32(232, 110, 100, 255), 2.6);
        gAudio().play("error");
        return;
    }
    m_puzzleMode   = true;
    m_activePuzzle = idx;
    m_puzzleResult = 0;
    m_puzzleGoal   = pe.setup.goal;
    m_screen       = Screen::Duel;
    m_anim.clear();
    m_zoneRectsReady  = false;
    m_sfxObsInited    = false;
    m_endGameSfxFired = false;
    gAudio().play("duel_start");
    pushToast("Puzzle: " + pe.setup.name, IM_COL32(200, 180, 255, 255), 2.6);
}

void UI::drawPuzzleBrowser() {
    if (m_puzzleBrowserOpen) { ImGui::OpenPopup("Puzzles"); m_puzzleBrowserOpen = false; }
    ImGui::SetNextWindowSize({560.f, 0.f}, ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Puzzles", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
    ImGui::TextUnformatted("Puzzles & Challenges");
    if (UIStyle::fHeader) ImGui::PopFont();
    ImGui::TextDisabled("Board-break: pick your deck, go second, draw 6, and "
                        "break the boss board.");
    ImGui::Separator();

    // Deck picker — the deck you'll bring into board-break challenges.
    bool anyBreak = false;
    for (auto& p : m_puzzles) if (p.setup.boardBreak) { anyBreak = true; break; }
    if (anyBreak) {
        if (m_deckFiles.empty()) refreshDeckFiles();
        ImGui::TextUnformatted("Your deck:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.f);
        if (m_deckFiles.empty()) {
            ImGui::TextDisabled("(no decks — build one in the Deck Builder)");
        } else {
            if (m_puzzleDeckIdx < 0 || m_puzzleDeckIdx >= (int)m_deckFiles.size())
                m_puzzleDeckIdx = 0;
            std::string cur = m_deckFiles[m_puzzleDeckIdx];
            if (ImGui::BeginCombo("##puzzledeck", cur.c_str())) {
                for (int d = 0; d < (int)m_deckFiles.size(); ++d) {
                    bool sel = (d == m_puzzleDeckIdx);
                    if (ImGui::Selectable(m_deckFiles[d].c_str(), sel))
                        m_puzzleDeckIdx = d;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Separator();
    }

    if (m_puzzles.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No puzzles found in assets/puzzles/.");
        ImGui::Spacing();
        if (UIStyle::GhostButton("Close", {-1.f, 30.f})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::BeginChild("##puzzlelist", {-1.f, 360.f}, true);
    for (int i = 0; i < (int)m_puzzles.size(); ++i) {
        const PuzzleEntry& pe = m_puzzles[i];
        ImGui::PushID(i);
        ImU32 dcol = pe.difficulty == "Hard"   ? IM_COL32(235, 110, 95, 255)
                   : pe.difficulty == "Medium" ? IM_COL32(235, 185, 60, 255)
                                               : IM_COL32(110, 210, 130, 255);
        UIStyle::DrawGlassPanel(ImGui::GetWindowDrawList(),
            ImGui::GetCursorScreenPos(),
            {ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x,
             ImGui::GetCursorScreenPos().y + 78.f});
        ImGui::BeginGroup();
        ImGui::Dummy({4.f, 4.f});
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::TextUnformatted(pe.setup.name.c_str());
        if (UIStyle::fHeader) ImGui::PopFont();
        ImGui::SameLine(0.f, 8.f);
        UIStyle::StatusChip(pe.difficulty.empty() ? "Easy" : pe.difficulty.c_str(),
                            dcol);
        if (!pe.desc.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420.f);
            ImGui::TextDisabled("%s", pe.desc.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 84.f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.f);
        if (UIStyle::PrimaryButton("Play", {78.f, 32.f})) {
            ImGui::CloseCurrentPopup();
            startPuzzleByIndex(i);
        }
        ImGui::Dummy({1.f, 8.f});
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (UIStyle::GhostButton("Close", {-1.f, 30.f})) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// In-progress goal bar — a slim top-centre window showing the objective with
// Restart / Quit. The solved/failed screen is the Game Over panel.
void UI::drawPuzzleOverlay(int w, int h) {
    if (!m_puzzleMode || m_dm.isDone()) return;
    ImGui::SetNextWindowPos({w * 0.5f, 8.f}, ImGuiCond_Always, {0.5f, 0.f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 22, 42, 235));
    ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(210, 130, 240, 230));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{12.f, 8.f});
    if (ImGui::Begin("##puzzle_goal", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(232, 210, 255, 255));
        ImGui::TextUnformatted("PUZZLE");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.f, 8.f);
        ImGui::TextUnformatted(m_puzzleGoal.c_str());
        ImGui::SameLine(0.f, 16.f);
        if (UIStyle::GhostButton("Restart", {78.f, 22.f}))
            startPuzzleByIndex(m_activePuzzle);
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::GhostButton("Quit", {58.f, 22.f})) {
            if (m_dm.isRunning()) m_dm.endDuel();
            m_puzzleMode = false;
            m_screen = Screen::Lobby;
            m_anim.clear(); m_zoneRectsReady = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ── Excavate reveal — cards flip up from the deck (Master-Duel style) ─────────
void UI::drawExcavateReveal(int w, int h) {
    if (m_excavateCards.empty()) return;
    double age = ImGui::GetTime() - m_excavateAt;
    const double LIFE = 2.0;
    if (age < 0.0 || age > LIFE) { m_excavateCards.clear(); return; }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    int n = (int)m_excavateCards.size();
    float ch = std::min((float)h * 0.30f, 230.f);
    float cw = ch * (421.f / 614.f);
    float gap = 14.f;
    float totalW = n * cw + (n - 1) * gap;
    float startX = w * 0.5f - totalW * 0.5f;
    float cy = h * 0.42f;
    // Soft dim behind the reveal.
    float dimA = (age < 0.2) ? (float)(age / 0.2)
               : (age > LIFE - 0.4) ? (float)((LIFE - age) / 0.4) : 1.f;
    dl->AddRectFilled({0.f, 0.f}, {(float)w, (float)h},
                      IM_COL32(4, 2, 3, (unsigned)(150 * dimA)));
    void* back = m_rend.getBackTexture();
    for (int i = 0; i < n; ++i) {
        // Per-card stagger so they flip up one after another.
        double cd = age - i * 0.12;
        float flip = cd <= 0.0 ? 0.f
                   : cd >= 0.32 ? 1.f : (float)(cd / 0.32);   // 0=edge,1=face
        float cx = startX + i * (cw + gap) + cw * 0.5f;
        // Flip: horizontal scale 0→1; show back for the first half, face after.
        float sx = std::abs(flip - 0.5f) * 2.f;   // 1 → 0 → 1 (edge at mid)
        if (flip <= 0.f) sx = 0.04f;
        float halfW = cw * 0.5f * (0.06f + 0.94f * sx);
        ImVec2 p0{cx - halfW, cy - ch * 0.5f}, p1{cx + halfW, cy + ch * 0.5f};
        bool faceUp = flip > 0.5f;
        void* tex = faceUp ? m_rend.getCardTexture(m_excavateCards[i]) : back;
        // Rising offset as it settles.
        float rise = (flip < 1.f) ? (1.f - flip) * 22.f : 0.f;
        p0.y -= rise; p1.y -= rise;
        ImU32 tint = IM_COL32_WHITE & 0x00FFFFFF;
        tint |= ((unsigned)(255 * dimA) << 24);
        if (tex)
            dl->AddImageRounded((ImTextureID)tex, p0, p1, {0,0}, {1,1}, tint, 6.f);
        else
            dl->AddRectFilled(p0, p1,
                IM_COL32(40, 20, 24, (unsigned)(235 * dimA)), 6.f);
        dl->AddRect(p0, p1, IM_COL32(220, 90, 96, (unsigned)(235 * dimA)),
                    6.f, 0, 2.f);
    }
    // Caption.
    const char* cap = "Excavating...";
    ImVec2 ts = ImGui::CalcTextSize(cap);
    dl->AddText({w * 0.5f - ts.x * 0.5f, cy - ch * 0.5f - 30.f},
                IM_COL32(245, 210, 210, (unsigned)(255 * dimA)), cap);
}

// ── In-duel pause menu (#5) ──────────────────────────────────────────────────
void UI::drawPauseMenu(int w, int h) {
    if (!m_pauseMenuOpen) return;
    if (!m_dm.isRunning() || m_dm.isDone()) { m_pauseMenuOpen = false; return; }
    const UIStyle::Colors& C = UIStyle::C();
    ImDrawList* fdl = ImGui::GetForegroundDrawList();
    // Dim the board behind the menu.
    fdl->AddRectFilled({0.f, 0.f}, {(float)w, (float)h}, IM_COL32(4, 2, 3, 180));

    ImGui::SetNextWindowPos({w * 0.5f, h * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({320.f, 0.f}, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(C.bgPopup));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImGui::ColorConvertU32ToFloat4(C.accent));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{20.f, 18.f});
    if (ImGui::Begin("##pausemenu", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize)) {
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(C.accentHi), "Paused");
        if (UIStyle::fHeader) ImGui::PopFont();
        ImGui::Spacing();
        if (UIStyle::PrimaryButton("Resume", {-1.f, 38.f})) {
            m_pauseMenuOpen = false; m_pauseConfirmSurrender = false;
        }
        ImGui::Spacing();
        if (!m_pauseConfirmSurrender) {
            if (UIStyle::DangerButton("Surrender", {-1.f, 34.f}))
                m_pauseConfirmSurrender = true;
        } else {
            ImGui::TextDisabled("Concede this duel?");
            if (UIStyle::DangerButton("Yes, surrender", {-1.f, 32.f})) {
                const int me = m_net.localPlayerIndex();
                if (m_net.isOffline() || m_net.isHost()) m_dm.forfeit(me);
                else sendSurrender();
                gAudio().play("defeat");
                m_pauseMenuOpen = false; m_pauseConfirmSurrender = false;
            }
            if (UIStyle::GhostButton("No, keep playing", {-1.f, 30.f}))
                m_pauseConfirmSurrender = false;
        }
        ImGui::Spacing();
        if (UIStyle::GhostButton("Quit to Lobby", {-1.f, 32.f})) {
            finalizeReplay("pause -> lobby");
            if (m_dm.isRunning()) m_dm.endDuel();
            if (!m_net.isOffline()) {
                m_mpInDuel = false;
                resetMpResponseState();
                m_net.disconnect("returned to lobby");
            }
            m_pauseMenuOpen = false; m_pauseConfirmSurrender = false;
            m_puzzleMode = false; m_matchActive = false;
            m_screen = Screen::Lobby;
            m_anim.clear(); m_zoneRectsReady = false;
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Esc to resume");
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ── Banlist / format validation (#15) ────────────────────────────────────────
void UI::loadBanlists() {
    m_banlists.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory("assets/lflists", ec)) return;
    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator("assets/lflists", ec))
        if (e.path().filename().string().find(".lflist.conf") != std::string::npos)
            files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    for (auto& path : files) {
        std::ifstream f(path);
        if (!f) continue;
        Banlist bl;
        std::string fn = fs::path(path).filename().string();
        auto p = fn.find(".lflist.conf");
        bl.name = (p != std::string::npos) ? fn.substr(0, p) : fn;
        bool gotName = false;
        std::string line;
        while (std::getline(f, line)) {
            size_t s = line.find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            if (line[s] == '!') {
                if (!gotName) { bl.name = line.substr(s + 1); gotName = true; }
                continue;
            }
            if (line[s] == '#' || !isdigit((unsigned char)line[s])) continue;
            uint64_t code = 0; size_t i = s;
            while (i < line.size() && isdigit((unsigned char)line[i]))
                code = code * 10 + (line[i++] - '0');
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            int limit = 3;
            if (i < line.size() && isdigit((unsigned char)line[i]))
                limit = line[i] - '0';
            if (code && limit >= 0 && limit <= 3)
                bl.limits[(uint32_t)code] = limit;
        }
        if (!bl.limits.empty()) m_banlists.push_back(std::move(bl));
    }
}

int UI::cardLimit(uint32_t code) const {
    if (m_selectedBanlist < 0 || m_selectedBanlist >= (int)m_banlists.size())
        return 3;
    const auto& m = m_banlists[(size_t)m_selectedBanlist].limits;
    auto it = m.find(code);
    return it == m.end() ? 3 : it->second;
}

// Preset opponent decks (#6) — the bundled AI_*.ydk archetype decks.
void UI::loadPresetDecks() {
    m_presetFiles.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory("assets/decks/presets", ec)) return;
    for (auto& e : fs::directory_iterator("assets/decks/presets", ec))
        if (e.path().extension() == ".ydk")
            m_presetFiles.push_back(e.path().filename().string());
    std::sort(m_presetFiles.begin(), m_presetFiles.end());
}

// Pretty label for a preset deck file ("AI_BlueEyes.ydk" -> "BlueEyes").
static std::string presetLabel(const std::string& file) {
    std::string s = file;
    if (s.size() > 4 && s.substr(s.size() - 4) == ".ydk") s = s.substr(0, s.size() - 4);
    if (s.rfind("AI_", 0) == 0) s = s.substr(3);
    return s.empty() ? file : s;
}

void UI::drawDeckBuilder(int w, int h) {
    // Fullscreen backdrop — same atmosphere as lobby/duel.
    UIStyle::DrawAppBackdrop(ImGui::GetBackgroundDrawList(),
                             {0.f, 0.f}, {(float)w, (float)h});
    const UIStyle::Colors& C = UIStyle::C();

    // ── helpers ─────────────────────────────────────────────────────────────
    auto isExtraCard = [](uint32_t type) {
        return (type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ | TYPE_LINK)) != 0;
    };
    auto decksEqual = [](const Deck& a, const Deck& b) {
        return a.main == b.main && a.extra == b.extra && a.side == b.side;
    };
    bool dirty = !decksEqual(m_editDeck, m_savedDeck);

    // ── geometry ────────────────────────────────────────────────────────────
    const float BAR_H     = 56.f;
    const float SEARCH_W  = std::max(320.f, (float)w * 0.27f);
    const float PREVIEW_W = std::max(300.f, (float)w * 0.26f);
    const float DECK_W    = (float)w - SEARCH_W - PREVIEW_W;
    const float COL_Y     = BAR_H;
    const float COL_H     = (float)h - BAR_H;

    // ── Top bar ─────────────────────────────────────────────────────────────
    {
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({(float)w, BAR_H});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2{12.f, 10.f});
        ImGui::Begin("##db_topbar", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBackground);

        ImDrawList* bdl = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetWindowPos();
        bdl->AddRectFilled(bp, {bp.x + w, bp.y + BAR_H},
                           IM_COL32(8, 11, 22, 240));
        bdl->AddLine({bp.x, bp.y + BAR_H - 1}, {bp.x + w, bp.y + BAR_H - 1},
                     C.borderSoft, 1.f);

        // In match side-deck mode the Back button becomes "Start next game":
        // it writes the sided deck to a temp file and continues the match.
        if (m_matchSiding) {
            char lbl[40];
            snprintf(lbl, sizeof(lbl), "Start Game %d >", m_matchGameNo + 1);
            if (UIStyle::PrimaryButton(lbl, {150.f, 32.f})) {
                const std::string tmp = "assets/decks/.match_player.ydk";
                saveYdk(m_editDeck, tmp);
                m_matchPlayerPath = tmp;
                m_matchSiding = false;
                m_matchGameNo++;
                startMatchGame();
                ImGui::End();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
                return;
            }
            ImGui::SameLine(0.f, 14.f);
            if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(C.textHi));
            ImGui::Text("Side Deck — Game %d  (You %d : %d Opp)",
                        m_matchGameNo + 1, m_matchWins[0], m_matchWins[1]);
            ImGui::PopStyleColor();
            if (UIStyle::fHeader) ImGui::PopFont();
        } else {
        if (UIStyle::GhostButton("< Back", {110.f, 32.f})) {
            if (dirty) {
                ImGui::OpenPopup("Leave deck?##leavedeck");
            } else {
                m_screen = Screen::Lobby;
                ImGui::End();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
                return;
            }
        }
        // Unsaved-changes guard (#F). Leaving sets the screen and lets the frame
        // finish normally (no early return → balanced End/Pop at function exit).
        ImGui::SetNextWindowPos({(float)w * 0.5f, (float)h * 0.5f},
                                ImGuiCond_Always, {0.5f, 0.5f});
        if (ImGui::BeginPopupModal("Leave deck?##leavedeck", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::TextUnformatted("You have unsaved changes to this deck.");
            ImGui::TextDisabled("Leaving now will discard them.");
            ImGui::Dummy({1.f, 8.f});
            if (UIStyle::DangerButton("Leave & discard", {150.f, 32.f})) {
                m_screen = Screen::Lobby;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0.f, 8.f);
            if (UIStyle::GhostButton("Stay", {110.f, 32.f}))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.f, 14.f);
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C.textHi));
        ImGui::TextUnformatted("Deck Builder");
        ImGui::PopStyleColor();
        if (UIStyle::fHeader) ImGui::PopFont();
        // Legality chip (#E) — green Legal / red Illegal with the reason.
        {
            std::string issue = deckLegality(m_editDeck);
            ImGui::SameLine(0.f, 12.f);
            ImGui::AlignTextToFramePadding();
            if (issue.empty())
                UIStyle::StatusChip("Legal", IM_COL32(90, 200, 120, 255));
            else {
                UIStyle::StatusChip("Illegal", IM_COL32(230, 90, 80, 255));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", issue.c_str());
            }
        }
        }

        // Right cluster: combo + name input + Save + Refresh + the ghost
        // buttons (Sort / Stats / Copy / Paste) — laid out from the right edge.
        // EXTRA reserves room for the four trailing ghost buttons so they don't
        // run off the right edge (which hid the Stats button entirely).
        const float SAVE_W = 96.f, REF_W = 96.f, NAME_W = 220.f, COMBO_W = 220.f;
        const float EXTRA = (52.f+6.f) + (62.f+6.f) + (52.f+6.f) + (56.f+6.f);
        float rightX = w - 12.f - EXTRA - SAVE_W - 6.f - REF_W;
        ImGui::SameLine(rightX - NAME_W - 6.f - COMBO_W);

        ImGui::SetNextItemWidth(COMBO_W);
        const char* preview = (m_selDeckIdx >= 0 &&
                               m_selDeckIdx < (int)m_deckFiles.size())
            ? m_deckFiles[m_selDeckIdx].c_str() : "(choose a deck)";
        if (ImGui::BeginCombo("##db_combo", preview)) {
            for (int i = 0; i < (int)m_deckFiles.size(); i++) {
                bool sel = (m_selDeckIdx == i);
                if (ImGui::Selectable(m_deckFiles[i].c_str(), sel)) {
                    m_selDeckIdx = i;
                    std::string path = "assets/decks/" + m_deckFiles[i];
                    m_editDeck  = loadYdk(path);
                    m_savedDeck = m_editDeck;             // freshly clean
                    std::string nm = m_deckFiles[i];
                    if (nm.size() > 4 && nm.substr(nm.size() - 4) == ".ydk")
                        nm = nm.substr(0, nm.size() - 4);
                    strncpy(m_deckNameBuf, nm.c_str(),
                            sizeof(m_deckNameBuf) - 1);
                    m_deckNameBuf[sizeof(m_deckNameBuf) - 1] = '\0';
                    m_deckHoverCode = 0;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine(0.f, 6.f);
        ImGui::SetNextItemWidth(NAME_W);
        ImGui::InputTextWithHint("##db_name", "Deck name",
                                 m_deckNameBuf, sizeof(m_deckNameBuf));

        ImGui::SameLine(0.f, 6.f);
        bool canSave = m_deckNameBuf[0] != '\0';
        if (!canSave) ImGui::BeginDisabled();
        if (UIStyle::PrimaryButton("Save", {SAVE_W, 32.f})) {
            std::string path =
                std::string("assets/decks/") + m_deckNameBuf + ".ydk";
            saveYdk(m_editDeck, path);
            // Re-read to confirm it landed — if the file exists we treat it
            // as success; otherwise show an error toast.
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec)) {
                m_savedDeck    = m_editDeck;
                m_deckToastMsg = std::string("Saved \"") + m_deckNameBuf + ".ydk\"";
                m_deckToastIsErr = false;
                gAudio().play("confirm");
                pushToast(std::string("Deck saved: ") + m_deckNameBuf,
                          IM_COL32(110, 220, 140, 255), 2.4);
            } else {
                m_deckToastMsg = "Save failed — check the deck name / folder";
                m_deckToastIsErr = true;
                gAudio().play("error");
                pushToast("Deck save failed",
                          IM_COL32(232, 110, 100, 255), 2.4);
            }
            m_deckToastAt = ImGui::GetTime();
            refreshDeckFiles();
        }
        if (!canSave) ImGui::EndDisabled();

        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::GhostButton("Refresh", {REF_W, 32.f})) refreshDeckFiles();

        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::GhostButton("Sort", {52.f, 32.f})) {
            sortEditDeck();
            pushToast("Deck sorted", IM_COL32(180, 220, 255, 255), 1.8);
        }
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::GhostButton("Stats", {62.f, 32.f}))
            m_consistencyOpen = true;

        // Clipboard share — copy the current deck as .ydk text, or paste one in.
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::GhostButton("Copy", {52.f, 32.f})) {
            ImGui::SetClipboardText(deckToYdkText(m_editDeck).c_str());
            int total = (int)(m_editDeck.main.size() + m_editDeck.extra.size() +
                              m_editDeck.side.size());
            pushToast("Deck copied to clipboard (" + std::to_string(total) +
                      " cards)", IM_COL32(110, 220, 140, 255), 2.4);
        }
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::GhostButton("Paste", {56.f, 32.f})) {
            const char* clip = ImGui::GetClipboardText();
            Deck pasted = clip ? deckFromYdkText(clip) : Deck{};
            int total = (int)(pasted.main.size() + pasted.extra.size() +
                              pasted.side.size());
            if (total > 0) {
                pasted.name = m_editDeck.name;   // keep the current name
                m_editDeck  = pasted;
                gAudio().play("confirm");
                pushToast("Deck pasted (" + std::to_string(total) + " cards)",
                          IM_COL32(110, 220, 140, 255), 2.4);
            } else {
                gAudio().play("error");
                pushToast("Clipboard has no valid deck (.ydk text or ydke:// URL)",
                          IM_COL32(232, 110, 100, 255), 2.6);
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // Deck-consistency calculator popup (opened by the toolbar "Stats" button).
    drawDeckConsistency();

    // ── LEFT — Card Search ──────────────────────────────────────────────────
    {
        ImGui::SetNextWindowPos({0.f, COL_Y});
        ImGui::SetNextWindowSize({SEARCH_W, COL_H});
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
            ImGui::ColorConvertU32ToFloat4(C.bgPanel));
        ImGui::Begin("##db_search", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);

        UIStyle::SectionHeader("Card Search");

        // Themed search box with a leading magnifier glyph.
        if (UIStyle::SearchInput("##db_search_in", m_searchBuf,
                                 sizeof(m_searchBuf),
                                 "Search card name or code...", -1.f)) {
            if (strlen(m_searchBuf) >= 2)
                m_searchResults = m_db.search(m_searchBuf, 80);
            else
                m_searchResults.clear();
        }

        ImGui::Dummy({1.f, 6.f});
        // Filter chips — first row: card kinds; second: deck zone.
        if (UIStyle::SegmentedButton("Monster", m_dbFilterMon, true, {84.f, 26.f}))
            m_dbFilterMon = !m_dbFilterMon;
        ImGui::SameLine(0.f, 4.f);
        if (UIStyle::SegmentedButton("Spell", m_dbFilterSpl, true, {68.f, 26.f}))
            m_dbFilterSpl = !m_dbFilterSpl;
        ImGui::SameLine(0.f, 4.f);
        if (UIStyle::SegmentedButton("Trap", m_dbFilterTrp, true, {68.f, 26.f}))
            m_dbFilterTrp = !m_dbFilterTrp;
        ImGui::Dummy({1.f, 4.f});
        if (UIStyle::SegmentedButton("Main Deck", m_dbFilterMain, true, {100.f, 26.f}))
            m_dbFilterMain = !m_dbFilterMain;
        ImGui::SameLine(0.f, 4.f);
        if (UIStyle::SegmentedButton("Extra Deck", m_dbFilterExtra, true, {100.f, 26.f}))
            m_dbFilterExtra = !m_dbFilterExtra;

        ImGui::Dummy({1.f, 8.f});
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 sp = ImGui::GetCursorScreenPos();
            dl->AddLine({sp.x, sp.y},
                        {sp.x + ImGui::GetContentRegionAvail().x, sp.y},
                        C.borderSoft, 1.f);
        }
        ImGui::Dummy({1.f, 6.f});

        // Results scroll area.
        ImGui::BeginChild("##db_sresults",
            {-1.f, ImGui::GetContentRegionAvail().y - 8.f},
            false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

        if (!m_db.isOpen()) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(C.danger));
            ImGui::TextUnformatted("No card database found.");
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Place cards.cdb in assets/ next to the exe.");
        } else if (m_searchBuf[0] == '\0') {
            UIStyle::EmptyState(ImGui::GetContentRegionAvail().y - 8.f,
                "Search for cards to add",
                "Type at least 2 characters to start");
        } else if (m_searchResults.empty()) {
            char es[96];
            snprintf(es, sizeof(es), "No cards match \"%s\"", m_searchBuf);
            UIStyle::EmptyState(ImGui::GetContentRegionAvail().y - 8.f,
                es, "Try a shorter or different term");
        } else {
            int rendered = 0;
            // Cap displayed rows so the panel never lags on huge searches —
            // CardDB::search already limits to 80, this caps post-filter too.
            const int kMaxRows = 80;
            for (auto& card : m_searchResults) {
                if (rendered >= kMaxRows) break;
                bool isMonster = (card.type & TYPE_MONSTER) != 0;
                bool isSpell   = (card.type & TYPE_SPELL)   != 0;
                bool isTrap    = (card.type & TYPE_TRAP)    != 0;
                bool isExtra   = isExtraCard(card.type);
                if (isMonster && !m_dbFilterMon) continue;
                if (isSpell   && !m_dbFilterSpl) continue;
                if (isTrap    && !m_dbFilterTrp) continue;
                if (isExtra && !m_dbFilterExtra) continue;
                if (!isExtra && (isMonster || isSpell || isTrap)
                    && !m_dbFilterMain) continue;
                ++rendered;

                // ── Row layout, warning-free pattern ────────────────────
                // 1) Reserve the whole row up front via Dummy so the parent
                //    child's max-content already covers the row's rectangle.
                // 2) Paint everything via DrawList (no cursor moves yet).
                // 3) Position the row hit-test + Add button WITHIN the
                //    reserved area using SetCursorScreenPos. Both submit
                //    items inside the reserved rect → no boundary extension.
                // 4) Restore cursor to row-bottom, then Dummy for the gap.
                ImVec2 rp = ImGui::GetCursorScreenPos();
                float  rw = ImGui::GetContentRegionAvail().x;
                float  rh = 72.f;
                ImGui::Dummy({rw, rh});                // step 1
                ImDrawList* dl = ImGui::GetWindowDrawList();
                UIStyle::DrawRaisedPanel(dl, rp, {rp.x + rw, rp.y + rh});

                // Thumbnail.
                void* tex = m_rend.getCardTexture(card.id);
                ImVec2 thumb = {rp.x + 8.f, rp.y + 8.f};
                ImVec2 thumbEnd = {thumb.x + 42.f, thumb.y + 56.f};
                if (tex)
                    dl->AddImage((ImTextureID)tex, thumb, thumbEnd);
                else
                    dl->AddRectFilled(thumb, thumbEnd, IM_COL32(28, 32, 50, 255), 3.f);

                // Type chip beside the name.
                ImU32 chipBg = isMonster ? IM_COL32(46, 62, 110, 255)
                              : isSpell  ? IM_COL32(28, 96, 60, 255)
                              : isTrap   ? IM_COL32(120, 36, 92, 255)
                                         : IM_COL32(54, 60, 80, 255);
                const char* chipText =
                    isExtra   ? (card.type & TYPE_FUSION)  ? "FUSION"
                              : (card.type & TYPE_SYNCHRO) ? "SYNCHRO"
                              : (card.type & TYPE_XYZ)     ? "XYZ"
                              : (card.type & TYPE_LINK)    ? "LINK"
                              : "EXTRA"
                    : isMonster ? "MONSTER"
                    : isSpell   ? "SPELL"
                    : isTrap    ? "TRAP" : "CARD";
                ImVec2 ts = ImGui::CalcTextSize(chipText);
                float chipX = thumb.x + 52.f;
                float chipY = rp.y + 8.f;
                dl->AddRectFilled({chipX, chipY},
                                  {chipX + ts.x + 12.f, chipY + ts.y + 6.f},
                                  chipBg, 4.f);
                dl->AddText({chipX + 6.f, chipY + 3.f}, C.textHi, chipText);

                // Name (right of chip).
                float nameX = chipX + ts.x + 22.f;
                dl->AddText({nameX, rp.y + 8.f}, C.textHi, card.name.c_str());

                // Metadata line.
                char meta[160];
                if (isMonster)
                    snprintf(meta, sizeof(meta),
                             "ATK %d / DEF %d  •  Lv%d  •  #%u",
                             card.atk, card.def, card.level,
                             (unsigned)card.id);
                else
                    snprintf(meta, sizeof(meta), "#%u", (unsigned)card.id);
                dl->AddText({thumb.x + 52.f, rp.y + 32.f}, C.textLo, meta);

                // Desc snippet — single line, ellipsis.
                if (!card.desc.empty()) {
                    std::string s = card.desc;
                    for (auto& ch : s) if (ch == '\n' || ch == '\r') ch = ' ';
                    if (s.size() > 70) s = s.substr(0, 68) + "...";
                    dl->AddText({thumb.x + 52.f, rp.y + 50.f}, C.textMuted, s.c_str());
                }

                // Step 3a — row hit-test (left side, leaves the Add button
                // free on the right). This stays inside the reserved rect.
                char sid[40];
                snprintf(sid, sizeof(sid), "##srow_%u", (unsigned)card.id);
                ImGui::SetCursorScreenPos(rp);
                ImGui::InvisibleButton(sid, {rw - 92.f, rh});
                bool rowHov = ImGui::IsItemHovered();
                if (rowHov) {
                    dl->AddRect(rp, {rp.x + rw, rp.y + rh},
                                C.accent, 4.f, 0, 1.4f);
                    m_hoveredCard = card.id;
                    m_hoveredInfo = card;
                    m_deckHoverCode = card.id;
                }

                // Step 3b — Add button placed inside the reserved rect.
                // Label uses ##id so the visible text is just "+ Add"; the
                // custom button now strips ##id from the rendered text.
                ImGui::SetCursorScreenPos({rp.x + rw - 88.f, rp.y + 20.f});
                char addLbl[40];
                snprintf(addLbl, sizeof(addLbl), "+ Add##s%u",
                         (unsigned)card.id);
                if (UIStyle::SecondaryButton(addLbl, {80.f, 32.f})) {
                    auto& zone = isExtra ? m_editDeck.extra : m_editDeck.main;
                    int copies = (int)std::count(zone.begin(), zone.end(), card.id);
                    int limit  = isExtra ? 15 : 60;
                    if (copies < 3 && (int)zone.size() < limit) {
                        zone.push_back(card.id);
                        gAudio().play("confirm");
                    } else {
                        gAudio().play("error");
                    }
                }

                // Step 4 — cursor back to row-bottom, plus a small gap that
                // grows the parent properly (no SetCursor-beyond-bounds).
                ImGui::SetCursorScreenPos({rp.x, rp.y + rh});
                ImGui::Dummy({1.f, 6.f});
            }
            if (rendered == 0)
                ImGui::TextDisabled("No matches after filtering.");
        }
        ImGui::EndChild();
        ImGui::End();
        ImGui::PopStyleColor();
    }

    // ── CENTER — Deck editor (Main + Extra + Side) ──────────────────────────
    {
        ImGui::SetNextWindowPos({SEARCH_W, COL_Y});
        ImGui::SetNextWindowSize({DECK_W, COL_H});
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
            ImGui::ColorConvertU32ToFloat4(C.bgDeep));
        ImGui::Begin("##db_deck", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);

        // Save toast (auto-fades after ~2s).
        if (m_deckToastAt > 0.0 &&
            (ImGui::GetTime() - m_deckToastAt) < 2.4) {
            double age = ImGui::GetTime() - m_deckToastAt;
            float  a = (age < 1.8) ? 1.f : (float)(1.0 - (age - 1.8) / 0.6);
            ImU32 col = m_deckToastIsErr ? C.danger : C.success;
            ImVec2 tp = ImGui::GetCursorScreenPos();
            float  tw = ImGui::GetContentRegionAvail().x;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 colA = (col & 0x00FFFFFF) | ((unsigned)(180 * a) << 24);
            ImU32 txtA = (C.textHi & 0x00FFFFFF) | ((unsigned)(255 * a) << 24);
            dl->AddRectFilled(tp, {tp.x + tw, tp.y + 26.f}, colA, 4.f);
            dl->AddText({tp.x + 10.f, tp.y + 5.f}, txtA, m_deckToastMsg.c_str());
            ImGui::Dummy({1.f, 30.f});
        }

        // Format / banlist selector — validates copies against the chosen list.
        {
            const char* cur = (m_selectedBanlist >= 0 &&
                               m_selectedBanlist < (int)m_banlists.size())
                ? m_banlists[(size_t)m_selectedBanlist].name.c_str()
                : "No banlist (3 each)";
            ImGui::TextDisabled("Format");
            ImGui::SameLine(0.f, 8.f);
            ImGui::SetNextItemWidth(220.f);
            if (ImGui::BeginCombo("##banlist", cur)) {
                if (ImGui::Selectable("No banlist (3 each)", m_selectedBanlist < 0))
                    m_selectedBanlist = -1;
                for (int i = 0; i < (int)m_banlists.size(); ++i)
                    if (ImGui::Selectable(m_banlists[(size_t)i].name.c_str(),
                                          m_selectedBanlist == i))
                        m_selectedBanlist = i;
                ImGui::EndCombo();
            }
            // Legend for the per-card badges (only meaningful with a list).
            if (m_selectedBanlist >= 0) {
                ImGui::SameLine(0.f, 14.f);
                ImGui::TextColored({0.92f, 0.30f, 0.25f, 1.f}, "(/) Forbidden");
                ImGui::SameLine(0.f, 10.f);
                ImGui::TextColored({0.92f, 0.40f, 0.30f, 1.f}, "1 Limited");
                ImGui::SameLine(0.f, 10.f);
                ImGui::TextColored({0.92f, 0.73f, 0.25f, 1.f}, "2 Semi");
            }
        }

        // Status / validation chips row.
        int mainCount  = (int)m_editDeck.main.size();
        int extraCount = (int)m_editDeck.extra.size();
        int sideCount  = (int)m_editDeck.side.size();
        // Validation predicates.
        bool warnLowMain  = (mainCount < 40);
        bool errHighMain  = (mainCount > 60);
        bool errHighExtra = (extraCount > 15);
        bool errCopyLimit = false;
        std::vector<std::string> banViol;     // over-limit cards (current list)
        {
            std::unordered_map<uint32_t, int> all;
            for (auto c : m_editDeck.main)  all[c]++;
            for (auto c : m_editDeck.extra) all[c]++;
            for (auto c : m_editDeck.side)  all[c]++;
            for (auto& kv : all) {
                int lim = cardLimit(kv.first);
                if (kv.second > lim) {
                    errCopyLimit = true;
                    std::string nm = m_db.getCard(kv.first).name;
                    if (nm.empty()) nm = "#" + std::to_string(kv.first);
                    banViol.push_back(nm + "  " + std::to_string(kv.second) +
                                      "/" + std::to_string(lim));
                }
            }
            std::sort(banViol.begin(), banViol.end());
        }

        char chip[64];
        snprintf(chip, sizeof(chip), "Main %d/60", mainCount);
        UIStyle::StatusChip(chip,
            errHighMain ? C.danger : warnLowMain ? C.warning : C.success);
        ImGui::SameLine(0.f, 6.f);
        snprintf(chip, sizeof(chip), "Extra %d/15", extraCount);
        UIStyle::StatusChip(chip, errHighExtra ? C.danger : C.success);
        ImGui::SameLine(0.f, 6.f);
        snprintf(chip, sizeof(chip), "Side %d/15", sideCount);
        UIStyle::StatusChip(chip, C.textMuted);
        ImGui::SameLine(0.f, 12.f);
        if (dirty)
            UIStyle::StatusChip("Unsaved changes", C.warning);
        else
            UIStyle::StatusChip("Saved", C.textMuted);
        if (errCopyLimit) {
            ImGui::SameLine(0.f, 6.f);
            UIStyle::StatusChip(m_selectedBanlist >= 0 ? "Banlist violation"
                                                       : "Copy limit exceeded",
                                C.danger);
            if (ImGui::IsItemHovered() && !banViol.empty()) {
                ImGui::BeginTooltip();
                ImGui::TextDisabled("Over the limit:");
                for (auto& v : banViol) ImGui::TextUnformatted(v.c_str());
                ImGui::EndTooltip();
            }
        }
        if (!m_db.isOpen()) {
            ImGui::SameLine(0.f, 6.f);
            UIStyle::StatusChip("No card DB", C.danger);
        }

        ImGui::Dummy({1.f, 6.f});

        // ── Inner scroll region for the three deck sections. The grid auto-
        //    wraps to fit the panel width. Tile size keeps rows readable on
        //    small windows; we compute tilesPerRow per section.
        ImGui::BeginChild("##db_deck_scroll", {-1.f, -1.f}, false,
            ImGuiWindowFlags_AlwaysVerticalScrollbar);

        // ── 10-column deck grid ─────────────────────────────────────────
        // A standard 40-card Main Deck reads as exactly 4 rows × 10
        // columns (the classic deck-editor layout); bigger decks continue
        // into further rows with the SAME column count. Tile size is
        // derived from the centre column's width so the ten columns
        // always fit, clamped so cards stay recognizable, and the tile
        // keeps the true 421:614 card aspect.
        const float TILE_PAD_X = 6.f;
        const float TILE_PAD_Y = 6.f;
        const int   kDeckCols  = 10;
        float TILE_W = (ImGui::GetContentRegionAvail().x - 16.f /*scrollbar*/
                        - (kDeckCols - 1) * TILE_PAD_X) / (float)kDeckCols;
        if (TILE_W < 44.f)  TILE_W = 44.f;
        if (TILE_W > 116.f) TILE_W = 116.f;
        float TILE_H = TILE_W * (614.f / 421.f);

        // ── Drag-and-drop payload ────────────────────────────────────────
        // Identifies the source tile by section ('m'/'e'/'s'), index in
        // the zone vector, and the card code (sanity check on accept).
        struct DeckDragPayload { char sec; int idx; uint32_t code; };
        // Pending move, applied after all three sections render so we
        // never erase from a vector while iterating it.
        struct PendingMove {
            bool valid = false;
            char srcSec = 0, dstSec = 0;
            int  srcIdx = -1, dstIdx = -1;   // dstIdx = -1 means append
            uint32_t code = 0;
        };
        PendingMove pending;

        // Section→zone lookup used when applying the pending move.
        auto zonePtr = [&](char sec) -> std::vector<uint32_t>* {
            if (sec == 'm') return &m_editDeck.main;
            if (sec == 'e') return &m_editDeck.extra;
            if (sec == 's') return &m_editDeck.side;
            return nullptr;
        };
        auto isExtraOnly = [&](uint32_t code) {
            CardInfo ci = m_db.getCard(code);
            return isExtraCard(ci.type);
        };
        // True if a card with `code` is legal in section `dstSec`.
        auto legalIn = [&](char dstSec, uint32_t code) {
            bool extra = isExtraOnly(code);
            if (dstSec == 'm') return !extra;       // main = no extra-only cards
            if (dstSec == 'e') return  extra;       // extra = ONLY extra-only cards
            return true;                            // side = anything goes
        };

        // Grid renderer — draws one section (label + tile grid). New click
        // model:
        //   Left-click      → select / update preview only
        //   Right-click     → remove one copy at THIS index
        //   Shift+Rclick    → remove all copies of this code in this section
        //   Left-drag       → drag-and-drop source for reorder / move
        // Each section is also a drop target on its empty area (append).
        auto drawSection = [&](const char* label, char sec,
                               std::vector<uint32_t>& zone,
                               const char* idPrefix) {
            // ── Section header — also a drop target so users can drag
            //    onto the title bar to append to this section.
            if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(C.textHi));
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            if (UIStyle::fHeader) ImGui::PopFont();

            // Live count badge: n / max, red when the section is out of legal
            // range (main 40-60, extra/side 0-15).
            {
                int n = (int)zone.size();
                int lo = (sec == 'm') ? 40 : 0;
                int hi = (sec == 'm') ? 60 : 15;
                bool ok = n >= lo && n <= hi;
                ImVec4 cc = ok ? ImGui::ColorConvertU32ToFloat4(C.textMuted)
                               : ImVec4{0.95f, 0.45f, 0.40f, 1.f};
                ImGui::SameLine(0.f, 10.f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(cc, "%d / %d", n, hi);
            }
            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload* p = ImGui::AcceptDragDropPayload("DECK_CARD");
                if (p && p->DataSize == (int)sizeof(DeckDragPayload)) {
                    const DeckDragPayload* dp =
                        (const DeckDragPayload*)p->Data;
                    if (legalIn(sec, dp->code)) {
                        pending = {true, dp->sec, sec, dp->idx, -1, dp->code};
                    } else {
                        gAudio().play("error");
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::Dummy({1.f, 4.f});

            // Empty-section placeholder doubles as a drop zone.
            if (zone.empty()) {
                ImVec2 sp = ImGui::GetCursorScreenPos();
                float  sw = ImGui::GetContentRegionAvail().x;
                float  sh = TILE_H + 6.f;
                // Reserve via an invisible button so it's also a real ImGui
                // item — that lets BeginDragDropTarget below attach to it.
                char eid[40];
                snprintf(eid, sizeof(eid), "##empty_drop_%s", idPrefix);
                ImGui::InvisibleButton(eid, {sw, sh});
                ImDrawList* edl = ImGui::GetWindowDrawList();
                edl->AddRect(sp, {sp.x + sw, sp.y + sh},
                             C.borderSoft, 6.f, 0, 1.f);
                const char* hint =
                    "(empty — drag cards here or use the search panel)";
                ImVec2 ts = ImGui::CalcTextSize(hint);
                edl->AddText({sp.x + (sw - ts.x) * 0.5f,
                              sp.y + (sh - ts.y) * 0.5f},
                             C.textMuted, hint);
                if (ImGui::BeginDragDropTarget()) {
                    const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload("DECK_CARD");
                    if (p && p->DataSize == (int)sizeof(DeckDragPayload)) {
                        const DeckDragPayload* dp =
                            (const DeckDragPayload*)p->Data;
                        if (legalIn(sec, dp->code)) {
                            pending = {true, dp->sec, sec,
                                       dp->idx, -1, dp->code};
                        } else {
                            gAudio().play("error");
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::Dummy({1.f, 10.f});
                return;
            }

            float availW = ImGui::GetContentRegionAvail().x;
            // Fixed 10-wide rows whenever the tiles fit (they always do —
            // TILE_W is derived from this width); the fit computation is
            // only the narrow-window fallback so tiles never overflow.
            int   perRow = std::max(1,
                (int)((availW + TILE_PAD_X) / (TILE_W + TILE_PAD_X)));
            if (perRow > kDeckCols) perRow = kDeckCols;
            int   totalRows = ((int)zone.size() + perRow - 1) / perRow;
            float totalH    = totalRows * TILE_H
                            + (totalRows - 1) * TILE_PAD_Y;

            ImVec2 startScreen = ImGui::GetCursorScreenPos();
            // Reserve the entire grid up front so the parent's max-content
            // already covers every tile we'll place. Avoids ImGui's
            // "SetCursorScreenPos extending parent boundaries" warning.
            ImGui::Dummy({availW, totalH});
            ImDrawList* dl = ImGui::GetWindowDrawList();

            for (size_t i = 0; i < zone.size(); ++i) {
                uint32_t code = zone[i];
                int col_ = (int)(i % perRow);
                int row_ = (int)(i / perRow);
                ImVec2 tp = {
                    startScreen.x + col_ * (TILE_W + TILE_PAD_X),
                    startScreen.y + row_ * (TILE_H + TILE_PAD_Y)
                };
                ImVec2 te = {tp.x + TILE_W, tp.y + TILE_H};

                // Tile background — subtle raised plate.
                dl->AddRectFilled(tp, te, IM_COL32(26, 14, 17, 230), 4.f);
                dl->AddRect(tp, te, IM_COL32(120, 52, 58, 200),
                            4.f, 0, 1.f);

                // Card image (or coloured placeholder by type).
                CardInfo ci = m_db.getCard(code);
                void* tex = m_rend.getCardTexture(code);
                ImVec2 ip = {tp.x + 3.f, tp.y + 3.f};
                ImVec2 ie = {te.x - 3.f, te.y - 3.f};
                if (tex) {
                    dl->AddImage((ImTextureID)tex, ip, ie);
                } else {
                    ImU32 fb = (ci.type & TYPE_SPELL) ? IM_COL32(28, 96, 60, 255)
                             : (ci.type & TYPE_TRAP)  ? IM_COL32(120, 36, 92, 255)
                                                     : IM_COL32(60, 72, 120, 255);
                    dl->AddRectFilled(ip, ie, fb, 3.f);
                    if (!ci.name.empty()) {
                        std::string nm = ci.name;
                        if (nm.size() > 8) nm = nm.substr(0, 7) + ".";
                        dl->AddText({ip.x + 4.f, ip.y + 4.f},
                                    C.textHi, nm.c_str());
                    }
                }

                // Banlist status badge (top-left corner): Forbidden / Limited /
                // Semi-Limited, per the selected format. 3-of cards show nothing.
                if (m_selectedBanlist >= 0) {
                    int lim = cardLimit(code);
                    if (lim < 3) {
                        ImVec2 bc = {tp.x + 11.f, tp.y + 11.f};
                        ImU32 bcol = (lim == 0) ? IM_COL32(230, 60, 50, 255)
                                   : (lim == 1) ? IM_COL32(235, 90, 70, 255)
                                                : IM_COL32(235, 185, 60, 255);
                        dl->AddCircleFilled(bc, 9.f, IM_COL32(18, 18, 24, 240), 16);
                        dl->AddCircle(bc, 9.f, bcol, 16, 2.f);
                        if (lim == 0) {                       // Forbidden: slash
                            dl->AddLine({bc.x - 4.5f, bc.y - 4.5f},
                                        {bc.x + 4.5f, bc.y + 4.5f}, bcol, 2.2f);
                        } else {                              // 1 / 2 allowed
                            char n[2] = { (char)('0' + lim), 0 };
                            ImVec2 ns = ImGui::CalcTextSize(n);
                            dl->AddText({bc.x - ns.x * 0.5f, bc.y - ns.y * 0.5f},
                                        bcol, n);
                        }
                    }
                }

                // Role-tag badge (top-right): Starter / Engine / Non-engine,
                // set in the Stats panel and persisted globally — so a tagged
                // card shows its role everywhere it appears.
                {
                    auto it = m_cardTags.find(code);
                    int role = (it == m_cardTags.end()) ? 0 : it->second;
                    if (role > 0) {
                        ImVec2 bc = {te.x - 11.f, tp.y + 11.f};
                        ImU32 rcol = role == 1 ? IM_COL32(90, 215, 125, 255)
                                   : role == 2 ? IM_COL32(110, 180, 255, 255)
                                               : IM_COL32(210, 130, 240, 255);
                        const char* rl = role == 1 ? "S" : role == 2 ? "E" : "N";
                        dl->AddCircleFilled(bc, 9.f, IM_COL32(18, 18, 24, 240), 16);
                        dl->AddCircle(bc, 9.f, rcol, 16, 2.f);
                        ImVec2 ns = ImGui::CalcTextSize(rl);
                        dl->AddText({bc.x - ns.x * 0.5f, bc.y - ns.y * 0.5f},
                                    rcol, rl);
                    }
                }

                // Hit-test / drag-and-drop / right-click remove.
                ImGui::SetCursorScreenPos(tp);
                char tid[40];
                snprintf(tid, sizeof(tid), "##%s%zu", idPrefix, i);
                ImGui::InvisibleButton(tid, {TILE_W, TILE_H});

                bool hov = ImGui::IsItemHovered();
                if (hov) {
                    dl->AddRect(tp, te, C.accent, 4.f, 0, 2.f);
                    m_hoveredCard = code;
                    m_hoveredInfo = ci;
                    m_deckHoverCode = code;
                    if (ImGui::BeginTooltip()) {
                        ImGui::TextUnformatted(ci.name.empty()
                            ? "(unknown card)" : ci.name.c_str());
                        ImGui::TextDisabled(
                            "Left-click: preview  •  Right-click: remove  "
                            "•  Shift+Right: remove all  •  Drag: move");
                        ImGui::EndTooltip();
                    }
                }

                // Right-click: remove. Shift+Right: remove all copies.
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyShift) {
                        zone.erase(
                            std::remove(zone.begin(), zone.end(), code),
                            zone.end());
                    } else {
                        zone.erase(zone.begin() + i);
                    }
                    gAudio().play("cancel");
                    // Iteration is now invalidated — bail out cleanly.
                    // Restore cursor to row-below-Dummy before returning so
                    // the next section's layout starts in the right spot.
                    ImGui::SetCursorScreenPos(
                        {startScreen.x, startScreen.y + totalH});
                    ImGui::Dummy({1.f, 6.f});
                    return;
                }

                // Drag source — left-drag starts a move. ImGui handles the
                // press-and-move detection; a plain left-click never enters
                // BeginDragDropSource, so it harmlessly updates the
                // preview (set above via hover state).
                if (ImGui::BeginDragDropSource(
                        ImGuiDragDropFlags_SourceAllowNullID)) {
                    DeckDragPayload pl{sec, (int)i, code};
                    ImGui::SetDragDropPayload("DECK_CARD",
                                              &pl, sizeof(pl));
                    // Lightweight preview while dragging.
                    if (tex) ImGui::Image(tex, {40.f, 56.f});
                    ImGui::TextUnformatted(ci.name.empty()
                        ? "(unknown card)" : ci.name.c_str());
                    ImGui::EndDragDropSource();
                }

                // Drop target — drop onto another tile inserts BEFORE it.
                if (ImGui::BeginDragDropTarget()) {
                    const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload("DECK_CARD");
                    if (p && p->DataSize == (int)sizeof(DeckDragPayload)) {
                        const DeckDragPayload* dp =
                            (const DeckDragPayload*)p->Data;
                        if (legalIn(sec, dp->code)) {
                            pending = {true, dp->sec, sec,
                                       dp->idx, (int)i, dp->code};
                        } else {
                            gAudio().play("error");
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
            }
            // Done with the grid — cursor sits at last-tile end; reset to
            // the row immediately below the reserved Dummy block and add a
            // small inter-section gap that grows the parent properly.
            ImGui::SetCursorScreenPos(
                {startScreen.x, startScreen.y + totalH});
            ImGui::Dummy({1.f, 6.f});
        };

        drawSection("Main Deck",  'm', m_editDeck.main,  "mtile");
        ImGui::Dummy({1.f, 8.f});
        drawSection("Extra Deck", 'e', m_editDeck.extra, "etile");
        ImGui::Dummy({1.f, 8.f});
        drawSection("Side Deck",  's', m_editDeck.side,  "stile");

        // ── Apply the pending drag-drop move now that all sections have
        //    rendered. Doing it post-iteration keeps vector iterators
        //    valid and avoids "move card off itself" edge cases.
        if (pending.valid) {
            std::vector<uint32_t>* src = zonePtr(pending.srcSec);
            std::vector<uint32_t>* dst = zonePtr(pending.dstSec);
            if (src && dst &&
                pending.srcIdx >= 0 && pending.srcIdx < (int)src->size() &&
                (*src)[pending.srcIdx] == pending.code) {
                uint32_t code = (*src)[pending.srcIdx];
                src->erase(src->begin() + pending.srcIdx);
                // If moving within the same vector, adjust dstIdx when
                // we just removed an earlier index.
                int di = pending.dstIdx;
                if (src == dst && di > pending.srcIdx) --di;
                if (di < 0 || di > (int)dst->size())
                    dst->push_back(code);
                else
                    dst->insert(dst->begin() + di, code);
                gAudio().play("click");
            }
        }

        ImGui::EndChild();
        ImGui::End();
        ImGui::PopStyleColor();
    }

    // ── RIGHT — Card Preview ────────────────────────────────────────────────
    {
        ImGui::SetNextWindowPos({(float)w - PREVIEW_W, COL_Y});
        ImGui::SetNextWindowSize({PREVIEW_W, COL_H});
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
            ImGui::ColorConvertU32ToFloat4(C.bgPanel));
        ImGui::Begin("##db_preview", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);

        UIStyle::SectionHeader("Card Preview");

        uint32_t code = m_deckHoverCode ? m_deckHoverCode : m_hoveredCard;
        CardInfo info = code ? m_db.getCard(code) : CardInfo{};
        if (code == 0 || info.id == 0) {
            UIStyle::EmptyState(ImGui::GetContentRegionAvail().y - 8.f,
                "Hover a card to preview",
                "Name, type, stats and effect text appear here");
        } else {
            // Card image area — fixed slot so the layout never resizes.
            const float IMG_W = std::min(PREVIEW_W - 32.f, 260.f);
            const float IMG_H = IMG_W * 1.46f;
            ImVec2 ip = ImGui::GetCursorScreenPos();
            ImVec2 ie = {ip.x + IMG_W, ip.y + IMG_H};
            ImDrawList* dl = ImGui::GetWindowDrawList();
            void* tex = m_rend.getCardTexture(code);
            if (tex) {
                dl->AddImage((ImTextureID)tex, ip, ie);
                dl->AddRect(ip, ie, C.borderSoft, 6.f, 0, 1.f);
            } else {
                dl->AddRectFilled(ip, ie, IM_COL32(20, 26, 44, 255), 6.f);
                dl->AddRect      (ip, ie, C.borderSoft,           6.f, 0, 1.f);
                ImVec2 ts = ImGui::CalcTextSize("No image");
                dl->AddText({ip.x + (IMG_W - ts.x) / 2,
                             ip.y + (IMG_H - ts.y) / 2},
                            C.textLo, "No image");
            }
            ImGui::Dummy({IMG_W, IMG_H + 10.f});

            // Name.
            if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(C.textHi));
            ImGui::TextWrapped("%s", info.name.c_str());
            ImGui::PopStyleColor();
            if (UIStyle::fHeader) ImGui::PopFont();

            // Type chips line.
            bool isMonster = (info.type & TYPE_MONSTER) != 0;
            bool isSpell   = (info.type & TYPE_SPELL)   != 0;
            bool isTrap    = (info.type & TYPE_TRAP)    != 0;
            bool isExtra   = isExtraCard(info.type);
            const char* typeStr =
                isMonster ?
                    ((info.type & TYPE_FUSION)   ? "Fusion Monster"   :
                     (info.type & TYPE_SYNCHRO)  ? "Synchro Monster"  :
                     (info.type & TYPE_XYZ)      ? "XYZ Monster"      :
                     (info.type & TYPE_LINK)     ? "Link Monster"     :
                     (info.type & TYPE_RITUAL)   ? "Ritual Monster"   :
                     (info.type & TYPE_PENDULUM) ? "Pendulum Monster" :
                     "Monster")
                : isSpell ? "Spell Card"
                : isTrap  ? "Trap Card"
                          : "Card";
            ImU32 tCol = isMonster ? IM_COL32(72, 96, 160, 255)
                       : isSpell   ? IM_COL32(28, 130, 80, 255)
                       : isTrap    ? IM_COL32(150, 50, 110, 255)
                                   : C.textMuted;
            UIStyle::StatusChip(typeStr, tCol);
            ImGui::SameLine(0.f, 6.f);
            char idc[24];
            snprintf(idc, sizeof(idc), "#%u", (unsigned)info.id);
            UIStyle::StatusChip(idc, C.textMuted);
            if (isExtra) {
                ImGui::SameLine(0.f, 6.f);
                UIStyle::StatusChip("Extra Deck", C.accent);
            }
            // Role tag chip (set in the Stats panel) — shown next to the stats
            // so the card's deckbuilding role is visible while browsing.
            {
                auto it = m_cardTags.find(code);
                int role = (it == m_cardTags.end()) ? 0 : it->second;
                if (role > 0) {
                    ImU32 rcol = role == 1 ? IM_COL32(90, 215, 125, 255)
                               : role == 2 ? IM_COL32(110, 180, 255, 255)
                                           : IM_COL32(210, 130, 240, 255);
                    const char* rl = role == 1 ? "Starter"
                                   : role == 2 ? "Engine" : "Non-engine";
                    ImGui::SameLine(0.f, 6.f);
                    UIStyle::StatusChip(rl, rcol);
                }
            }

            ImGui::Dummy({1.f, 6.f});

            // Stats panel (monsters) or spell/trap subtype line.
            if (isMonster) {
                ImDrawList* sd = ImGui::GetWindowDrawList();
                ImVec2 sp = ImGui::GetCursorScreenPos();
                float  sw = ImGui::GetContentRegionAvail().x;
                float  sh = 56.f;
                UIStyle::DrawRaisedPanel(sd, sp, {sp.x + sw, sp.y + sh});
                char sline1[64], sline2[64];
                snprintf(sline1, sizeof(sline1), "ATK %d",  info.atk);
                snprintf(sline2, sizeof(sline2), "DEF %d",  info.def);
                sd->AddText({sp.x + 12.f, sp.y + 6.f},  C.textLo, "ATK");
                sd->AddText({sp.x + 12.f, sp.y + 22.f}, C.textHi, sline1);
                sd->AddText({sp.x + sw / 2 + 6.f, sp.y + 6.f},  C.textLo, "DEF");
                sd->AddText({sp.x + sw / 2 + 6.f, sp.y + 22.f}, C.textHi, sline2);
                char lv[24];
                if (info.type & TYPE_LINK)
                    snprintf(lv, sizeof(lv), "LINK %d", info.level);
                else if (info.type & TYPE_XYZ)
                    snprintf(lv, sizeof(lv), "RANK %d", info.level);
                else
                    snprintf(lv, sizeof(lv), "LV %d",   info.level);
                sd->AddText({sp.x + sw - 96.f, sp.y + 6.f},  C.textLo, "LEVEL");
                sd->AddText({sp.x + sw - 96.f, sp.y + 22.f}, C.textHi, lv);
                ImGui::Dummy({1.f, sh + 8.f});
            }

            // Effect / description text — wrapped, scrollable.
            UIStyle::SectionHeader("Effect");
            ImGui::BeginChild("##db_pv_eff", {-1.f, -1.f}, false,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            if (info.desc.empty())
                ImGui::TextDisabled("(no card text available)");
            else
                ImGui::TextWrapped("%s", info.desc.c_str());
            ImGui::EndChild();
        }

        ImGui::End();
        ImGui::PopStyleColor();
    }
}

// ─── Replays browser screen ─────────────────────────────────────────────────
//
// Two-column layout: a scrollable list of replay files on the left and a
// metadata + event timeline view of the selected replay on the right. Full
// engine playback (re-feeding responses to ocgcore) is deferred — this view
// already covers the "match history" + "audit what happened" workflow.
//
void UI::drawReplays(int w, int h) {
    UIStyle::DrawAppBackdrop(ImGui::GetBackgroundDrawList(),
                             {0.f, 0.f}, {(float)w, (float)h});
    const UIStyle::Colors& C = UIStyle::C();

    const float BAR_H  = 56.f;
    const float LIST_W = std::max(380.f, (float)w * 0.36f);

    // ── Top bar — Back + title + Refresh + Open Folder + Auto-save chip ──
    {
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({(float)w, BAR_H});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2{12.f, 10.f});
        ImGui::Begin("##rep_topbar", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBackground);
        ImDrawList* bdl = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetWindowPos();
        bdl->AddRectFilled(bp, {bp.x + w, bp.y + BAR_H},
                           IM_COL32(8, 11, 22, 240));
        bdl->AddLine({bp.x, bp.y + BAR_H - 1}, {bp.x + w, bp.y + BAR_H - 1},
                     C.borderSoft, 1.f);

        if (UIStyle::GhostButton("< Back", {110.f, 32.f})) {
            m_screen = Screen::Lobby;
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            return;
        }
        ImGui::SameLine(0.f, 14.f);
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C.textHi));
        ImGui::TextUnformatted("Replays");
        ImGui::PopStyleColor();
        if (UIStyle::fHeader) ImGui::PopFont();

        // Right side toolbar.
        const float REF_W = 100.f, OPEN_W = 130.f, AUTO_W = 180.f;
        float rightX = w - 12.f - REF_W - 6.f - OPEN_W - 6.f - AUTO_W;
        ImGui::SameLine(rightX);
        // Auto-save toggle — surfaced here AND inside Settings popup for
        // discoverability.
        bool autoSave = m_settings.autoSaveReplays;
        ImGui::SetNextItemWidth(AUTO_W);
        if (ImGui::Checkbox("Auto-save replays", &autoSave)) {
            m_settings.autoSaveReplays = autoSave;
            saveSettings();
            pushToast(autoSave ? "Auto-save replays: ON"
                               : "Auto-save replays: OFF",
                      IM_COL32(255, 214, 108, 255), 2.0);
        }
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::GhostButton("Open Folder", {OPEN_W, 32.f})) {
            // Best-effort: copy the absolute folder path so the user can
            // paste it into Explorer/Finder. Cross-platform launchers are
            // outside the scope of this UI-only patch.
            namespace fs = std::filesystem;
            std::error_code ec;
            std::string abs = fs::absolute(edo::Replay::defaultDir(), ec).string();
            ImGui::SetClipboardText(abs.c_str());
            pushToast("Folder path copied to clipboard",
                      IM_COL32(180, 220, 255, 255), 2.2);
        }
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::GhostButton("Refresh", {REF_W, 32.f})) {
            m_replayFiles = edo::Replay::list();
            if (m_selectedReplay >= (int)m_replayFiles.size())
                m_selectedReplay = m_replayFiles.empty() ? -1 : 0;
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // Lazy initial load on first frame.
    if (m_replayFiles.empty() && m_selectedReplay == -1) {
        m_replayFiles = edo::Replay::list();
        if (!m_replayFiles.empty()) {
            m_selectedReplay = 0;
            m_viewerReplayValid = m_viewerReplay.load(m_replayFiles[0]);
        }
    }

    // ── Left list ────────────────────────────────────────────────────────
    ImGui::SetNextWindowPos({0.f, BAR_H});
    ImGui::SetNextWindowSize({LIST_W, (float)h - BAR_H});
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImGui::ColorConvertU32ToFloat4(C.bgPanel));
    ImGui::Begin("##rep_list", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings);
    UIStyle::SectionHeader("Match History");
    if (m_replayFiles.empty()) {
        UIStyle::EmptyState((float)h - BAR_H - 80.f, "No replays yet",
                            "Finish a duel with Auto-save on to record one");
    } else {
        ImGui::TextDisabled("%d match%s", (int)m_replayFiles.size(),
                            m_replayFiles.size() == 1 ? "" : "es");
        ImGui::Dummy({1.f, 6.f});
        ImGui::BeginChild("##rep_list_inner", {-1.f, -1.f}, false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
        // Lazy per-file metadata cache so each match card can show deck
        // names / winner / turns without re-parsing every frame.
        static std::unordered_map<std::string, edo::Replay> s_meta;
        for (int i = 0; i < (int)m_replayFiles.size(); ++i) {
            const std::string& path = m_replayFiles[i];
            auto it = s_meta.find(path);
            if (it == s_meta.end()) {
                edo::Replay meta;
                meta.load(path);                 // best-effort; empty if corrupt
                it = s_meta.emplace(path, std::move(meta)).first;
            }
            const edo::Replay& m = it->second;
            bool parsed = !m.deck1.main.empty() || !m.deck2.main.empty();
            std::string fname = path;
            auto sl = fname.find_last_of("/\\");
            if (sl != std::string::npos) fname = fname.substr(sl + 1);

            ImVec2 rp = ImGui::GetCursorScreenPos();
            float rw = ImGui::GetContentRegionAvail().x - 4.f;
            float rh = 60.f;
            ImGui::PushID(i);
            bool clicked = ImGui::InvisibleButton("##repcard", {rw, rh});
            bool hov = ImGui::IsItemHovered();
            bool selnow = (m_selectedReplay == i);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 rb{rp.x + rw, rp.y + rh};
            // Card surface.
            dl->AddRectFilled(rp, rb,
                selnow ? IM_COL32(34, 46, 78, 255)
                       : hov ? IM_COL32(28, 37, 62, 255)
                             : IM_COL32(20, 27, 46, 235), 7.f);
            dl->AddRect(rp, rb,
                selnow ? C.accentHi : hov ? C.border : C.borderSoft,
                7.f, 0, selnow ? 1.6f : 1.f);
            // Winner colour stripe on the left edge.
            ImU32 wcol = !parsed ? C.danger
                       : m.winner == 0 ? IM_COL32(110, 220, 140, 255)
                       : m.winner == 1 ? IM_COL32(232, 110, 100, 255)
                                       : C.textMuted;
            dl->AddRectFilled({rp.x, rp.y + 6.f}, {rp.x + 4.f, rb.y - 6.f},
                              wcol, 2.f);
            // Title: Deck1 vs Deck2 (or filename if unparsed).
            UIStyle::PushFont(UIStyle::fHeader);
            std::string title = parsed
                ? ((m.deck1.name.empty() ? "P1" : m.deck1.name) + "  vs  " +
                   (m.deck2.name.empty() ? "P2" : m.deck2.name))
                : fname;
            // Clip title to width.
            dl->PushClipRect(rp, {rb.x - 8.f, rb.y}, true);
            dl->AddText({rp.x + 14.f, rp.y + 9.f}, C.textHi, title.c_str());
            dl->PopClipRect();
            UIStyle::PopFont();
            // Subline: date · winner · turns.
            UIStyle::PushFont(UIStyle::fSmall);
            char sub[128];
            if (parsed)
                snprintf(sub, sizeof(sub), "%s   ·   %s   ·   T%d",
                    m.timestamp.empty() ? "—" : m.timestamp.c_str(),
                    m.winner == 0 ? "P1 win" : m.winner == 1 ? "P2 win" : "draw",
                    m.turns);
            else
                snprintf(sub, sizeof(sub), "unreadable replay");
            dl->AddText({rp.x + 14.f, rp.y + 34.f}, C.textLo, sub);
            UIStyle::PopFont();
            ImGui::PopID();
            ImGui::Dummy({1.f, 5.f});

            if (clicked) {
                m_selectedReplay = i;
                m_viewerReplayValid = m_viewerReplay.load(path);
                if (!m_viewerReplayValid)
                    pushToast("Replay parse failed (corrupt file?)",
                              IM_COL32(232, 110, 100, 255), 2.4);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // ── Right detail view ────────────────────────────────────────────────
    ImGui::SetNextWindowPos({LIST_W, BAR_H});
    ImGui::SetNextWindowSize({(float)w - LIST_W, (float)h - BAR_H});
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImGui::ColorConvertU32ToFloat4(C.bgDeep));
    ImGui::Begin("##rep_detail", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings);

    if (m_selectedReplay < 0 ||
        m_selectedReplay >= (int)m_replayFiles.size()) {
        ImGui::Dummy({1.f, 60.f});
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::TextDisabled("Select a replay from the left.");
        if (UIStyle::fHeader) ImGui::PopFont();
    } else if (!m_viewerReplayValid) {
        ImGui::Dummy({1.f, 20.f});
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(C.danger));
        ImGui::TextUnformatted("Could not parse this replay file.");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("File: %s",
            m_replayFiles[m_selectedReplay].c_str());
        ImGui::Dummy({1.f, 8.f});
        if (UIStyle::DangerButton("Delete file", {180.f, 32.f})) {
            std::error_code ec;
            std::filesystem::remove(m_replayFiles[m_selectedReplay], ec);
            m_replayFiles = edo::Replay::list();
            m_selectedReplay = m_replayFiles.empty() ? -1 : 0;
            pushToast("Replay deleted", IM_COL32(232, 110, 100, 255), 2.0);
        }
    } else {
        const edo::Replay& r = m_viewerReplay;
        const std::string& path = m_replayFiles[m_selectedReplay];

        UIStyle::SectionHeader("Match details");
        ImGui::Text("Date     : %s", r.timestamp.c_str());
        ImGui::Text("File     : %s", path.c_str());
        ImGui::Text("App      : %s", r.app.c_str());
        ImGui::Text("Seed     : %llu", (unsigned long long)r.seed);
        ImGui::Text("Card DB  : %s",
                    r.cardDb.empty() ? "(unknown)" : r.cardDb.c_str());
        ImGui::Dummy({1.f, 6.f});

        // Decks side-by-side.
        UIStyle::SectionHeader("Decks");
        ImGui::Columns(2, "##repdecks", false);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(C.accent));
        ImGui::TextUnformatted("Player 1");
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", r.deck1.name.empty()
            ? "(unnamed)" : r.deck1.name.c_str());
        ImGui::TextDisabled("Main %d  Extra %d  Side %d",
            (int)r.deck1.main.size(), (int)r.deck1.extra.size(),
            (int)r.deck1.side.size());
        ImGui::NextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(C.accent));
        ImGui::TextUnformatted("Player 2");
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", r.deck2.name.empty()
            ? "(unnamed)" : r.deck2.name.c_str());
        ImGui::TextDisabled("Main %d  Extra %d  Side %d",
            (int)r.deck2.main.size(), (int)r.deck2.extra.size(),
            (int)r.deck2.side.size());
        ImGui::Columns(1);
        ImGui::Dummy({1.f, 6.f});

        // Outcome chips.
        UIStyle::SectionHeader("Outcome");
        const char* winText =
            r.winner == 0 ? "Player 1 wins" :
            r.winner == 1 ? "Player 2 wins" :
            r.winner == -1 ? "Draw"         : "(unknown)";
        ImU32 winCol =
            r.winner == 0 ? IM_COL32(110, 220, 140, 255) :
            r.winner == 1 ? IM_COL32(232, 110, 100, 255) :
                            C.textMuted;
        UIStyle::StatusChip(winText, winCol);
        ImGui::SameLine(0.f, 6.f);
        char chip[48];
        snprintf(chip, sizeof(chip), "Turns %d", r.turns);
        UIStyle::StatusChip(chip, C.textMd);
        ImGui::SameLine(0.f, 6.f);
        snprintf(chip, sizeof(chip), "Duration %.0fs", r.durationSec);
        UIStyle::StatusChip(chip, C.textMd);
        ImGui::SameLine(0.f, 6.f);
        snprintf(chip, sizeof(chip), "Final LP %u / %u",
                 r.finalLP[0], r.finalLP[1]);
        UIStyle::StatusChip(chip, C.textMd);
        ImGui::SameLine(0.f, 6.f);
        snprintf(chip, sizeof(chip), "Responses %d  Events %d",
                 (int)r.responses.size(), (int)r.events.size());
        UIStyle::StatusChip(chip, C.textMuted);

        ImGui::Dummy({1.f, 8.f});

        // Action row — Play Replay is the primary action; secondary buttons
        // are the copy/delete utilities from before.
        if (UIStyle::PrimaryButton("Play Replay", {180.f, 34.f})) {
            startReplayPlayback(path);
        }
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::SecondaryButton("Copy file path", {160.f, 30.f})) {
            ImGui::SetClipboardText(path.c_str());
            pushToast("Replay path copied", IM_COL32(180, 220, 255, 255), 2.0);
        }
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::SecondaryButton("Copy event log", {160.f, 30.f})) {
            std::ostringstream os;
            for (const auto& e : r.events) {
                char tb[24];
                snprintf(tb, sizeof(tb), "[%7.2fs] ", e.t);
                os << tb << e.text << "\n";
            }
            ImGui::SetClipboardText(os.str().c_str());
            pushToast("Event log copied", IM_COL32(180, 220, 255, 255), 2.0);
        }
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::DangerButton("Delete", {120.f, 30.f})) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            m_replayFiles = edo::Replay::list();
            m_selectedReplay = m_replayFiles.empty() ? -1 : 0;
            m_viewerReplayValid = false;
            pushToast("Replay deleted", IM_COL32(232, 110, 100, 255), 2.0);
        }

        ImGui::Dummy({1.f, 10.f});

        // Event timeline — scrollable; the closest thing to playback we
        // offer in this round. Engine re-feed is deferred to a future
        // round so we never risk replay/engine version drift today.
        UIStyle::SectionHeader("Event timeline");
        ImGui::BeginChild("##rep_events", {-1.f, -1.f}, false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
        if (r.events.empty()) {
            ImGui::TextDisabled("(no events recorded)");
        } else {
            for (const auto& e : r.events) {
                ImGui::TextDisabled("[%7.2fs]", e.t);
                ImGui::SameLine(110.f);
                ImGui::TextWrapped("%s", e.text.c_str());
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// ─── Multiplayer screen ─────────────────────────────────────────────────────
//
// Foundation-only in this round. The architecture (NetSession + protocol
// message types) is in place; the actual socket layer is deferred so the
// UI scaffolding shows the layout, persists preferences, and exposes the
// intended Host/Join flow without making promises the network code can't
// keep yet. A clear "FOUNDATION ONLY — LAN sockets not implemented yet"
// banner sits at the top so the user isn't misled.
//
void UI::drawMultiplayer(int w, int h) {
    UIStyle::DrawAppBackdrop(ImGui::GetBackgroundDrawList(),
                             {0.f, 0.f}, {(float)w, (float)h});
    const UIStyle::Colors& C = UIStyle::C();

    const float BAR_H = 56.f;
    const float COL_W = std::max(420.f, (float)w * 0.40f);

    // ── Top bar ─────────────────────────────────────────────────────────
    {
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({(float)w, BAR_H});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2{12.f, 10.f});
        ImGui::Begin("##mp_topbar", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBackground);
        ImDrawList* bdl = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetWindowPos();
        bdl->AddRectFilled(bp, {bp.x + w, bp.y + BAR_H},
                           IM_COL32(8, 11, 22, 240));
        bdl->AddLine({bp.x, bp.y + BAR_H - 1}, {bp.x + w, bp.y + BAR_H - 1},
                     C.borderSoft, 1.f);
        if (UIStyle::GhostButton("< Back", {110.f, 32.f})) {
            // Tear down any in-progress connection state on the way out.
            if (!m_net.isOffline()) m_net.disconnect("user left screen");
            m_screen = Screen::Lobby;
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            return;
        }
        ImGui::SameLine(0.f, 14.f);
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C.textHi));
        ImGui::TextUnformatted("Multiplayer");
        ImGui::PopStyleColor();
        if (UIStyle::fHeader) ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // ── How-it-works banner ─────────────────────────────────────────────
    {
        ImGui::SetNextWindowPos({(float)w * 0.5f - 360.f, BAR_H + 10.f});
        ImGui::SetNextWindowSize({720.f, 64.f});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.07f, 0.09f, 0.16f, 0.96f});
        ImGui::PushStyleColor(ImGuiCol_Border,   {0.40f, 0.48f, 0.78f, 0.85f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.2f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
        ImGui::Begin("##mp_banner", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);
        if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
        ImGui::TextColored({1.f, 0.86f, 0.45f, 1.f},
            m_mpTransport == 0 ? "LAN Multiplayer" : "Online Multiplayer");
        if (UIStyle::fHeader) ImGui::PopFont();
        ImGui::TextDisabled(m_mpTransport == 0
            ? "Same network: one player hosts, the other joins by IP. "
              "Pick a deck, both press Ready, then the host starts the duel."
            : "Over the internet: start the relay server, one player creates "
              "a room and shares the code, the other joins it.");
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    const float BODY_Y = BAR_H + 80.f;
    const float BODY_H = (float)h - BODY_Y - 16.f;

    // ── Left column: Identity + connection settings ─────────────────────
    {
        ImGui::SetNextWindowPos({16.f, BODY_Y});
        ImGui::SetNextWindowSize({COL_W, BODY_H});
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
            ImGui::ColorConvertU32ToFloat4(C.bgPanel));
        ImGui::Begin("##mp_identity", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);

        UIStyle::SectionHeader("Identity");
        ImGui::Text("Display name");
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::InputText("##mpname", m_mpNameBuf, sizeof(m_mpNameBuf))) {
            m_settings.mpDisplayName = m_mpNameBuf;
            saveSettings();
        }
        ImGui::Dummy({1.f, 8.f});

        // ── Transport selector: LAN (direct) vs Online (relay) ──────────
        // Seed the relay buffers from persisted settings on first view.
        if (m_mpRelayAddrBuf[0] == '\0')
            strncpy(m_mpRelayAddrBuf, m_settings.mpHostIP.c_str(),
                    sizeof(m_mpRelayAddrBuf) - 1);
        UIStyle::SectionHeader("Mode");
        bool lockTransport = !m_net.isOffline();   // can't switch mid-session
        if (lockTransport) ImGui::BeginDisabled();
        if (UIStyle::SegmentedButton("LAN (direct)", m_mpTransport == 0,
                                     true, {150.f, 28.f}))
            m_mpTransport = 0;
        ImGui::SameLine(0.f, 6.f);
        if (UIStyle::SegmentedButton("Online (relay)", m_mpTransport == 1,
                                     true, {150.f, 28.f}))
            m_mpTransport = 1;
        if (lockTransport) ImGui::EndDisabled();
        ImGui::TextDisabled(m_mpTransport == 0
            ? "Same network — one player hosts, the other joins by IP."
            : "Over the internet — both players connect to a relay server.");
        ImGui::Dummy({1.f, 10.f});

        if (m_mpTransport == 0) {
            // ── LAN direct connection ───────────────────────────────────
            UIStyle::SectionHeader("Connection");
            ImGui::Text("Host IP / address");
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::InputText("##mpip", m_mpIPBuf, sizeof(m_mpIPBuf))) {
                m_settings.mpHostIP = m_mpIPBuf;
                saveSettings();
            }
            ImGui::Text("Port");
            ImGui::SetNextItemWidth(140.f);
            if (ImGui::InputInt("##mpport", &m_mpPortBuf)) {
                if (m_mpPortBuf < 1)     m_mpPortBuf = 1;
                if (m_mpPortBuf > 65535) m_mpPortBuf = 65535;
                m_settings.mpPort = m_mpPortBuf;
                saveSettings();
            }
            ImGui::Dummy({1.f, 10.f});
            if (m_net.isOffline()) {
                if (UIStyle::PrimaryButton("Host Game", {-1.f, 36.f})) {
                    if (m_net.host(m_mpPortBuf, m_mpNameBuf)) {
                        m_settings.mpMode = "host";
                        saveSettings();
                        pushToast("Listening on port " +
                                  std::to_string(m_mpPortBuf),
                                  IM_COL32(180, 220, 255, 255), 2.4);
                        m_dm.logEvent("[MULTI HOST] port=" +
                                      std::to_string(m_mpPortBuf));
                        m_mpRemoteDeckRcvd = false;
                        m_mpRemoteReady    = false;
                    }
                }
                ImGui::Dummy({1.f, 6.f});
                if (UIStyle::SecondaryButton("Join Game", {-1.f, 34.f})) {
                    if (m_net.joinHost(m_mpIPBuf, m_mpPortBuf, m_mpNameBuf)) {
                        m_settings.mpMode = "client";
                        saveSettings();
                        pushToast(std::string("Connecting to ") + m_mpIPBuf,
                                  IM_COL32(180, 220, 255, 255), 2.4);
                        m_dm.logEvent("[MULTI JOIN] " +
                                      std::string(m_mpIPBuf) + ":" +
                                      std::to_string(m_mpPortBuf));
                        m_mpRemoteDeckRcvd = false;
                        m_mpRemoteReady    = false;
                    }
                }
            }
        } else {
            // ── Online relay room ───────────────────────────────────────
            UIStyle::SectionHeader("Relay server");
            ImGui::Text("Server address");
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::InputText("##mprelay", m_mpRelayAddrBuf,
                                 sizeof(m_mpRelayAddrBuf))) {
                m_settings.mpHostIP = m_mpRelayAddrBuf;
                saveSettings();
            }
            ImGui::Text("Port");
            ImGui::SetNextItemWidth(140.f);
            ImGui::InputInt("##mprelayport", &m_mpRelayPortBuf);
            if (m_mpRelayPortBuf < 1)     m_mpRelayPortBuf = 1;
            if (m_mpRelayPortBuf > 65535) m_mpRelayPortBuf = 65535;
            ImGui::Dummy({1.f, 10.f});

            if (m_net.isOffline()) {
                // ── Online lobby ─────────────────────────────────────────
                // Auto-refreshing list of open rooms + quick actions, so the
                // Online tab behaves like a live lobby instead of a one-shot
                // search box.
                const double now = ImGui::GetTime();
                auto rlStatus = m_net.roomListStatus();
                const bool querying =
                    (rlStatus == edo::NetSession::RoomListStatus::Querying);
                auto doRefresh = [&]{
                    std::string addr = m_mpRelayAddrBuf[0] ? m_mpRelayAddrBuf
                                                           : "127.0.0.1";
                    m_net.requestRoomList(addr, m_mpRelayPortBuf,
                                          m_mpNameBuf[0] ? m_mpNameBuf : "Player");
                    m_lobbyLastRefreshAt = now;
                    m_lobbyNextRefreshAt = now + 4.0;
                };
                // Initial load on entry + keep fresh while we sit here.
                if (m_lobbyAutoRefresh && !querying && now >= m_lobbyNextRefreshAt)
                    doRefresh();

                auto rooms = m_net.roomList();
                auto firstOpen = [&]() -> const edo::RoomInfo* {
                    for (const auto& r : rooms) if (r.joinable()) return &r;
                    return nullptr;
                };
                auto joinCode = [&](const std::string& code){
                    strncpy(m_mpRoomCodeBuf, code.c_str(),
                            sizeof(m_mpRoomCodeBuf) - 1);
                    m_mpRoomCodeBuf[sizeof(m_mpRoomCodeBuf) - 1] = '\0';
                    startRelayJoin();
                };

                // ── Quick actions: Quick Match + Create Room ─────────────
                float colW = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
                if (UIStyle::PrimaryButton("Quick Match", {colW, 38.f})) {
                    // Join the first open room, or host a fresh one if none.
                    if (const edo::RoomInfo* pick = firstOpen())
                        joinCode(pick->code);
                    else
                        startRelayCreate();
                }
                ImGui::SameLine(0.f, 8.f);
                if (UIStyle::SecondaryButton("Create Room", {colW, 38.f}))
                    startRelayCreate();

                // ── Join by code ─────────────────────────────────────────
                ImGui::Dummy({1.f, 8.f});
                ImGui::SetNextItemWidth(-92.f);
                bool codeEnter = ImGui::InputTextWithHint("##mproom",
                                 "ENTER ROOM CODE", m_mpRoomCodeBuf,
                                 sizeof(m_mpRoomCodeBuf),
                                 ImGuiInputTextFlags_CharsUppercase |
                                 ImGuiInputTextFlags_CharsNoBlank |
                                 ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine(0.f, 6.f);
                {
                    bool canJoin = m_mpRoomCodeBuf[0] != '\0';
                    if (!canJoin) ImGui::BeginDisabled();
                    if (UIStyle::SecondaryButton("Join##bycode", {86.f, 30.f}) ||
                        (codeEnter && canJoin))
                        startRelayJoin();
                    if (!canJoin) ImGui::EndDisabled();
                }

                // ── Lobby header: count (left) + auto/refresh (right) ────
                UIStyle::DrawDivider(10.f, 6.f);
                int openCount = 0;
                for (const auto& r : rooms) if (r.joinable()) ++openCount;
                UIStyle::SectionHeader("Game Lobby");
                char hdr[64];
                if (rlStatus == edo::NetSession::RoomListStatus::Ready)
                    snprintf(hdr, sizeof(hdr), "%d room%s · %d open",
                             (int)rooms.size(), rooms.size() == 1 ? "" : "s",
                             openCount);
                else
                    snprintf(hdr, sizeof(hdr), "%s",
                             querying ? "Searching…" : "Connecting…");
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertU32ToFloat4(C.textLo));
                ImGui::TextUnformatted(hdr);
                ImGui::PopStyleColor();
                // Right-aligned auto toggle + manual refresh on the same row.
                ImGui::SameLine();
                float rowAvail = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     (rowAvail - 156.f > 0.f ? rowAvail - 156.f : 0.f));
                ImGui::Checkbox("Auto##lobby", &m_lobbyAutoRefresh);
                ImGui::SameLine(0.f, 6.f);
                if (querying) ImGui::BeginDisabled();
                if (UIStyle::GhostButton(querying ? "…##rl" : "Refresh##rl",
                                         {84.f, 26.f}))
                    doRefresh();
                if (querying) ImGui::EndDisabled();
                // Freshness line.
                if (m_lobbyLastRefreshAt > 0.0 &&
                    rlStatus == edo::NetSession::RoomListStatus::Ready) {
                    UIStyle::PushFont(UIStyle::fSmall);
                    ImGui::TextDisabled("updated %.0fs ago",
                                        now - m_lobbyLastRefreshAt);
                    UIStyle::PopFont();
                }

                // ── Room cards ───────────────────────────────────────────
                if (rlStatus == edo::NetSession::RoomListStatus::Error) {
                    ImGui::Dummy({1.f, 4.f});
                    ImGui::TextColored({1.f, 0.6f, 0.6f, 1.f}, "%s",
                        m_net.roomListError().c_str());
                } else if (rooms.empty() &&
                           rlStatus == edo::NetSession::RoomListStatus::Ready) {
                    ImGui::Dummy({1.f, 4.f});
                    UIStyle::EmptyState(120.f, "No open rooms",
                        "Quick Match will host one for you");
                } else if (!rooms.empty()) {
                    ImGui::BeginChild("##roomlist", {-1.f,
                        std::min(300.f, 14.f + rooms.size() * 64.f)}, false);
                    for (size_t i = 0; i < rooms.size(); ++i) {
                        const edo::RoomInfo& rm = rooms[i];
                        ImGui::PushID((int)i);
                        ImVec2 rp = ImGui::GetCursorScreenPos();
                        float rw = ImGui::GetContentRegionAvail().x;
                        float rh = 58.f;
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        bool joinable = rm.joinable();
                        UIStyle::DrawGlassPanel(dl, rp, {rp.x + rw, rp.y + rh},
                                                7.f, 0);
                        // Status pill (left accent): open / ready / in duel.
                        ImU32 stCol = rm.state == 0 ? IM_COL32(110, 220, 140, 255)
                                    : rm.state == 1 ? IM_COL32(232, 196, 110, 255)
                                                    : IM_COL32(150, 150, 158, 255);
                        const char* stTxt = rm.state == 0 ? "OPEN"
                                          : rm.state == 1 ? "READY"
                                                          : "IN DUEL";
                        dl->AddRectFilled(rp, {rp.x + 4.f, rp.y + rh},
                                          stCol, 7.f, ImDrawFlags_RoundCornersLeft);
                        // Host name.
                        UIStyle::PushFont(UIStyle::fHeader);
                        dl->AddText({rp.x + 14.f, rp.y + 8.f}, C.textHi,
                                    rm.hostName.empty() ? "Host"
                                                        : rm.hostName.c_str());
                        UIStyle::PopFont();
                        // Sub: code · players · status.
                        char sub[96];
                        snprintf(sub, sizeof(sub), "Code %s   ·   %d/2   ·   %s",
                                 rm.code.c_str(), rm.players, stTxt);
                        UIStyle::PushFont(UIStyle::fSmall);
                        dl->AddText({rp.x + 14.f, rp.y + 32.f}, C.textLo, sub);
                        UIStyle::PopFont();
                        // Status dot near the right of the title.
                        dl->AddCircleFilled({rp.x + rw - 100.f, rp.y + 16.f},
                                            4.f, stCol);
                        // Join button on the right (joinable only).
                        ImGui::SetCursorScreenPos({rp.x + rw - 88.f,
                                                   rp.y + 15.f});
                        if (!joinable) ImGui::BeginDisabled();
                        if (UIStyle::SecondaryButton(
                                joinable ? "Join##rj" : "Full##rj",
                                {78.f, 28.f}))
                            joinCode(rm.code);
                        if (!joinable) ImGui::EndDisabled();
                        ImGui::SetCursorScreenPos({rp.x, rp.y + rh + 6.f});
                        ImGui::PopID();
                    }
                    ImGui::EndChild();
                }
            } else if (m_mpRoomActive && !m_mpRoomCode.empty()) {
                // Show the active room code prominently + a copy button.
                UIStyle::SectionHeader("Room");
                if (UIStyle::fHeader) ImGui::PushFont(UIStyle::fHeader);
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertU32ToFloat4(C.accentHi));
                ImGui::Text("Code:  %s", m_mpRoomCode.c_str());
                ImGui::PopStyleColor();
                if (UIStyle::fHeader) ImGui::PopFont();
                if (UIStyle::SecondaryButton("Copy room code", {-1.f, 30.f})) {
                    ImGui::SetClipboardText(m_mpRoomCode.c_str());
                    pushToast("Room code copied",
                              IM_COL32(180, 220, 255, 255), 1.8);
                }
                ImGui::TextDisabled("Share this code with your opponent.");
            } else if (m_mpRelayConnecting) {
                ImGui::TextDisabled("Connecting to relay...");
            }
            if (!m_mpRoomError.empty()) {
                ImGui::TextColored({1.f, 0.55f, 0.55f, 1.f},
                    "%s", m_mpRoomError.c_str());
            }
        }
        ImGui::Dummy({1.f, 12.f});

        // ── Status (shared by both transports) ──────────────────────────
        UIStyle::SectionHeader("Status");
        const char* modeStr =
            m_net.isHost()    ? "HOST"   :
            m_net.isClient()  ? "CLIENT" : "OFFLINE";
        ImU32 modeCol = m_net.isOffline() ? C.textMuted : C.accent;
        UIStyle::StatusChip(modeStr, modeCol);
        ImGui::SameLine(0.f, 6.f);
        const char* stateStr =
            m_net.state() == edo::NetState::Disconnected ? "disconnected" :
            m_net.state() == edo::NetState::Listening    ? "listening"    :
            m_net.state() == edo::NetState::Connecting   ? "connecting"   :
            m_net.state() == edo::NetState::Handshaking  ? "handshaking"  :
            m_net.state() == edo::NetState::Connected    ? "connected"    :
            m_net.state() == edo::NetState::InDuel       ? "in duel"      :
                                                            "error";
        UIStyle::StatusChip(stateStr, C.textMd);
        if (m_mpTransport == 1 && !m_net.isOffline())
            UIStyle::StatusChip("ONLINE", C.glowCyan);
        if (!m_net.lastError().empty()) {
            ImGui::TextColored({1.f, 0.55f, 0.55f, 1.f},
                "Last error: %s", m_net.lastError().c_str());
        }
        ImGui::Dummy({1.f, 12.f});

        // ── Disconnect (shared) ─────────────────────────────────────────
        if (!m_net.isOffline()) {
            if (UIStyle::DangerButton("Disconnect", {-1.f, 34.f})) {
                m_net.disconnect("user requested");
                m_settings.mpMode = "offline";
                saveSettings();
                m_mpReady          = false;
                m_mpRemoteDeckRcvd = false;
                m_mpRemoteReady    = false;
                m_mpRoomActive     = false;
                m_mpRelayConnecting= false;
                m_mpHandshakeSent  = false;
                m_mpRoomCode.clear();
                pushToast("Disconnected",
                          IM_COL32(232, 110, 100, 255), 2.0);
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    // ── Right column: Deck choice + ready + activity log ────────────────
    {
        ImGui::SetNextWindowPos({COL_W + 32.f, BODY_Y});
        ImGui::SetNextWindowSize({(float)w - COL_W - 48.f, BODY_H});
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
            ImGui::ColorConvertU32ToFloat4(C.bgDeep));
        ImGui::Begin("##mp_session", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);

        UIStyle::SectionHeader("Session");
        // Deck picker — reuses the existing deck-files cache.
        refreshDeckFiles();
        const char* deckLabel = (m_mpDeckIdx >= 0 &&
                                 m_mpDeckIdx < (int)m_deckFiles.size())
            ? m_deckFiles[m_mpDeckIdx].c_str()
            : "(pick your deck)";
        ImGui::Text("Your deck");
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::BeginCombo("##mpdeck", deckLabel)) {
            for (int i = 0; i < (int)m_deckFiles.size(); ++i) {
                bool sel = (m_mpDeckIdx == i);
                if (ImGui::Selectable(m_deckFiles[i].c_str(), sel)) {
                    m_mpDeckIdx = i;
                    // Announce the new deck to the peer immediately if
                    // we're already connected; otherwise it'll go out
                    // after the Hello handshake completes.
                    if (m_net.state() == edo::NetState::Connected ||
                        m_net.state() == edo::NetState::Handshaking)
                        sendMpDeckInfo();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::Dummy({1.f, 8.f});
        if (ImGui::Checkbox("Ready", &m_mpReady)) {
            sendMpReady(m_mpReady);
        }
        ImGui::Dummy({1.f, 8.f});

        // Host-only Start Duel button: enabled only when both peers have
        // sent DeckInfo and both are Ready.
        if (m_net.isHost() && !m_mpInDuel) {
            bool canStart = m_mpReady && m_mpRemoteReady &&
                            m_mpDeckIdx >= 0 && m_mpRemoteDeckRcvd;
            if (!canStart) ImGui::BeginDisabled();
            if (UIStyle::PrimaryButton("Start Duel", {-1.f, 36.f})) {
                sendMpStartDuel();
            }
            if (!canStart) ImGui::EndDisabled();
            if (!canStart) {
                ImGui::TextDisabled("(need both ready + both decks)");
            }
            ImGui::Dummy({1.f, 8.f});
        }

        // Peer info — populated by the Hello / DeckInfo / Ready packets.
        UIStyle::SectionHeader("Opponent");
        bool havePeer = !m_net.peer().displayName.empty() ||
                        !m_net.peer().addr.empty();
        if (havePeer) {
            ImGui::Text("Name: %s",
                m_net.peer().displayName.empty() ? "(connecting…)"
                : m_net.peer().displayName.c_str());
            ImGui::Text("Deck: %s",
                m_mpRemoteDeckRcvd
                    ? (m_mpRemoteDeck.name.empty() ? "(unnamed)"
                       : m_mpRemoteDeck.name.c_str())
                    : "(awaiting deck)");
            if (m_mpRemoteDeckRcvd)
                ImGui::TextDisabled("Main %d  Extra %d  Side %d",
                    (int)m_mpRemoteDeck.main.size(),
                    (int)m_mpRemoteDeck.extra.size(),
                    (int)m_mpRemoteDeck.side.size());
            ImGui::Dummy({1.f, 4.f});
            // Readiness chips — connected / deck / ready.
            UIStyle::StatusChip("Connected", C.success);
            ImGui::SameLine(0.f, 6.f);
            UIStyle::StatusChip(m_mpRemoteDeckRcvd ? "Deck received"
                                                   : "No deck yet",
                                m_mpRemoteDeckRcvd ? C.success : C.warning);
            ImGui::SameLine(0.f, 6.f);
            UIStyle::StatusChip(m_mpRemoteReady ? "Ready" : "Not ready",
                                m_mpRemoteReady ? C.success : C.textMuted);
        } else {
            UIStyle::StatusChip("Waiting for opponent…", C.textMuted);
        }
        if (!m_net.lastError().empty()) {
            ImGui::Dummy({1.f, 6.f});
            ImGui::PushStyleColor(ImGuiCol_Text, {1.f, 0.55f, 0.55f, 1.f});
            ImGui::TextWrapped("Network error: %s",
                m_net.lastError().c_str());
            ImGui::PopStyleColor();
        }

        // Raw socket counters are debug-only — hidden from normal users.
        if (m_debugLog) {
            ImGui::Dummy({1.f, 8.f});
            UIStyle::SectionHeader("Network (debug)");
            auto st = m_net.stats();
            ImGui::TextDisabled("Msgs sent %llu  recv %llu",
                (unsigned long long)st.messagesSent,
                (unsigned long long)st.messagesReceived);
            ImGui::TextDisabled("Bytes sent %llu  recv %llu",
                (unsigned long long)st.bytesSent,
                (unsigned long long)st.bytesReceived);
            ImGui::TextDisabled("Local seat = P%d  addr=%s",
                m_net.localPlayerIndex() + 1,
                m_net.peer().addr.empty() ? "(local)"
                                          : m_net.peer().addr.c_str());
        }

        ImGui::End();
        ImGui::PopStyleColor();
    }
}

// ─── refreshDeckFiles ─────────────────────────────────────────────────────────
// Deck legality (#E): size limits + per-card copy limit (banlist-aware).
std::string UI::deckLegality(const Deck& d) {
    int m = (int)d.main.size(), e = (int)d.extra.size(), s = (int)d.side.size();
    if (m < 40) return "Main deck has " + std::to_string(m) + " cards (need 40+)";
    if (m > 60) return "Main deck has " + std::to_string(m) + " cards (max 60)";
    if (e > 15) return "Extra deck has " + std::to_string(e) + " cards (max 15)";
    if (s > 15) return "Side deck has " + std::to_string(s) + " cards (max 15)";
    std::unordered_map<uint32_t,int> cnt;
    for (uint32_t c : d.main)  cnt[c]++;
    for (uint32_t c : d.extra) cnt[c]++;
    for (uint32_t c : d.side)  cnt[c]++;
    for (auto& kv : cnt) {
        int lim = cardLimit(kv.first);          // banlist-aware (3 default)
        if (kv.second > lim) {
            std::string nm = m_db.getCard(kv.first).name;
            if (nm.empty()) nm = "#" + std::to_string(kv.first);
            return nm + ": " + std::to_string(kv.second) + " copies (limit " +
                   std::to_string(lim) + ")";
        }
    }
    return "";
}

void UI::refreshDeckFiles() {
    m_deckFiles.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator("assets/decks", ec)) {
        if (entry.path().extension() == ".ydk") {
            std::string fn = entry.path().filename().string();
            if (!fn.empty() && fn[0] == '.') continue;   // hide temp/.match files
            m_deckFiles.push_back(fn);
        }
    }
    std::sort(m_deckFiles.begin(), m_deckFiles.end());
    if (m_selDeckIdx >= (int)m_deckFiles.size())
        m_selDeckIdx = m_deckFiles.empty() ? -1 : 0;
}

// ─── YDK helpers ─────────────────────────────────────────────────────────────
// Decode standard base64 (skips whitespace/newlines; stops at '=' padding).
static std::vector<uint8_t> b64decode(const std::string& in) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int rev[256]; for (int i = 0; i < 256; ++i) rev[i] = -1;
    for (int i = 0; i < 64; ++i) rev[(uint8_t)tbl[i]] = i;
    rev[(uint8_t)'-'] = 62; rev[(uint8_t)'_'] = 63;   // accept base64url too
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (uint8_t c : in) {
        if (c == '=') break;
        int dv = rev[c];
        if (dv < 0) continue;                       // skip non-b64 bytes
        val = (val << 6) | dv; bits += 6;
        if (bits >= 0) { out.push_back((uint8_t)((val >> bits) & 0xff)); bits -= 8; }
    }
    return out;
}

// Parse a YDKE share URL — ydke://<b64 main>!<b64 extra>!<b64 side>! — where
// each section is base64 of packed little-endian uint32 passcodes. This is the
// format YGOPRODeck / EDOPro "copy deck" produces, so pasting one Just Works.
static Deck deckFromYdke(const std::string& url) {
    Deck d;
    const std::string scheme = "ydke://";
    std::string body = url.substr(url.find(scheme) + scheme.size());
    std::vector<std::string> parts; size_t start = 0;
    for (size_t i = 0; i <= body.size(); ++i)
        if (i == body.size() || body[i] == '!') {
            parts.push_back(body.substr(start, i - start));
            start = i + 1;
        }
    auto fill = [](const std::string& b64, std::vector<uint32_t>& out) {
        std::vector<uint8_t> b = b64decode(b64);
        for (size_t i = 0; i + 4 <= b.size(); i += 4) {
            uint32_t code = (uint32_t)b[i] | ((uint32_t)b[i+1] << 8) |
                            ((uint32_t)b[i+2] << 16) | ((uint32_t)b[i+3] << 24);
            if (code) out.push_back(code);
        }
    };
    if (parts.size() > 0) fill(parts[0], d.main);
    if (parts.size() > 1) fill(parts[1], d.extra);
    if (parts.size() > 2) fill(parts[2], d.side);
    return d;
}

// Parse the standard .ydk text format (shared by file load + clipboard paste).
// Tolerates CRLF line endings so a deck copied from another app pastes cleanly.
// Also accepts a YDKE share URL (ydke://...) for one-click imports.
Deck UI::deckFromYdkText(const std::string& text) {
    if (text.find("ydke://") != std::string::npos)
        return deckFromYdke(text);
    Deck d;
    std::istringstream f(text);
    std::string line;
    int zone = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if      (line.substr(0, 6) == "#extra") zone = 1;
            else if (line.substr(0, 5) == "#main")  zone = 0;
            continue;
        }
        if (line[0] == '!') {
            if (line.substr(0, 5) == "!side") zone = 2;
            continue;
        }
        try {
            uint32_t code = (uint32_t)std::stoul(line);
            if (code) {
                if      (zone == 0) d.main.push_back(code);
                else if (zone == 1) d.extra.push_back(code);
                else                d.side.push_back(code);
            }
        } catch (...) {}
    }
    return d;
}

// Serialise a deck to the standard .ydk text format (file save + clipboard copy).
std::string UI::deckToYdkText(const Deck& d) {
    std::string s = "#created by YGO: Nova\n#main\n";
    for (auto c : d.main)  s += std::to_string(c) + "\n";
    s += "#extra\n";
    for (auto c : d.extra) s += std::to_string(c) + "\n";
    s += "!side\n";
    for (auto c : d.side)  s += std::to_string(c) + "\n";
    return s;
}

// Organise the deck-builder deck: monsters (high level/ATK first) → spells →
// traps, alphabetical within a tier. Deck order is irrelevant to play (the
// engine shuffles), so this is purely a tidy-up for the builder.
void UI::sortEditDeck() {
    auto cat = [](uint32_t t) -> int {
        if (t & TYPE_MONSTER) return 0;
        if (t & TYPE_SPELL)   return 1;
        if (t & TYPE_TRAP)    return 2;
        return 3;
    };
    auto less = [&](uint32_t a, uint32_t b) {
        CardInfo ca = m_db.getCard(a), cb = m_db.getCard(b);
        int xa = cat(ca.type), xb = cat(cb.type);
        if (xa != xb)             return xa < xb;
        if (ca.level != cb.level) return ca.level > cb.level;  // higher first
        if (ca.atk   != cb.atk)   return ca.atk   > cb.atk;
        if (ca.name  != cb.name)  return ca.name  < cb.name;
        return a < b;
    };
    std::stable_sort(m_editDeck.main.begin(),  m_editDeck.main.end(),  less);
    std::stable_sort(m_editDeck.extra.begin(), m_editDeck.extra.end(), less);
    std::stable_sort(m_editDeck.side.begin(),  m_editDeck.side.end(),  less);
}

Deck UI::loadYdk(const std::string& path) {
    std::ifstream f(path);
    if (!f) return Deck{};
    std::stringstream ss;
    ss << f.rdbuf();
    return deckFromYdkText(ss.str());
}

void UI::saveYdk(const Deck& d, const std::string& path) {
    std::ofstream f(path);
    if (!f) return;
    f << deckToYdkText(d);
}
