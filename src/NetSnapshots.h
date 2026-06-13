#pragma once
// ─── NetSnapshots.h ────────────────────────────────────────────────────────
//
// Host-authoritative multiplayer payloads.
//
// The HOST owns the only authoritative ocgcore engine. After every
// engine advance, the host serialises a sanitised view of its FieldState
// + SelectionRequest into the structs in this header and ships them to
// the client over the existing NetSession socket layer. The client
// renders the duel ENTIRELY from these snapshots — it never feeds its
// own engine and so cannot desync against the host.
//
// Design rules:
//   * No engine pointers / no ocgcore types in here. These structs are
//     POD-ish carriers built from CardState / SelectionRequest copies.
//   * Sanitisation happens on the host side at build time, NOT here —
//     this header is just the wire format. We never serialise opponent
//     hand contents (only counts).
//   * Forward compatibility: every payload begins with a u32 version
//     field. Parsers must tolerate older host versions by checking the
//     version and falling back to defaults for new fields.
//   * The candidate list inside PromptSnapshotPayload tags every legal
//     choice with a stable `choiceId`. The client returns that id in
//     ClientChoicePayload; the host maps id→responseBytes locally so
//     the wire never carries raw engine bytes from the client.
//
// All ints little-endian (via the put/get helpers in NetSession.h).
//
#include <cstdint>
#include <string>
#include <vector>
#include "NetSession.h"
#include "DuelManager.h"   // CardState, SelectionRequest, WaitType, FieldState

namespace edo {

// v2: PromptChoice carries cmd/index/con/flags (SelectIdleCmd /
//     SelectBattleCmd routing) and PromptSnapshotPayload carries
//     toBP/toM2/toEP + placeFlag/placeCount (phase buttons + zone glow).
// v3: FieldSnapshotPayload carries ourExtra (the RECIPIENT's own Extra
//     Deck codes, engine order) so the client's ED viewer works without
//     a local engine; PromptChoice carries rawDesc/desc (decoded effect
//     text) and PromptSnapshotPayload carries timing/srcCode/srcDesc so
//     the client renders human-readable labels exactly like offline.
constexpr uint32_t kSnapshotVersion = 3;

// ── A single card rendered on the snapshot field ──────────────────────
struct SnapCard {
    uint32_t code   = 0;
    uint32_t loc    = 0;
    uint32_t seq    = 0;
    uint32_t pos    = 0;
    uint8_t  player = 0;
    bool     hidden = false;     // true = face-down / opponent hand card
};

inline void putCard(std::vector<uint8_t>& b, const SnapCard& c) {
    putU32(b, c.code);
    putU32(b, c.loc);
    putU32(b, c.seq);
    putU32(b, c.pos);
    putU8 (b, c.player);
    putU8 (b, c.hidden ? 1u : 0u);
}
inline SnapCard readCard(NetReader& r) {
    SnapCard c;
    c.code   = r.u32();
    c.loc    = r.u32();
    c.seq    = r.u32();
    c.pos    = r.u32();
    c.player = r.u8();
    c.hidden = (r.u8() != 0);
    return c;
}

// ── FieldSnapshotPayload — full board state for one recipient ─────────
//
// The host builds one of these per recipient (opponent hand contents
// are masked off — only counts are revealed). Wire layout:
//
//   [u32 version][u32 recipient][u32 lp[2]][u32 turn][u32 phase]
//   [u32 turnPlayer][u32 turnCount]
//   [u32 deckCount[2]][u32 extraCount[2]]
//   [u32 handCount[2]]
//   [u32 ourHandN ][SnapCard ourHand[ourHandN]]      // hand[recipient]
//   [u32 monsters0N][SnapCard monsters0[..]] ...     // monsters[0], monsters[1]
//   [u32 spells0N  ][SnapCard spells0[..]]   ...     // spells[0],   spells[1]
//   [u32 gy0N      ][SnapCard gy0[..]]       ...     // gy[0],       gy[1]
//   [u32 ban0N     ][SnapCard ban0[..]]      ...     // banished[0], banished[1]
//
struct FieldSnapshotPayload {
    uint32_t version    = kSnapshotVersion;
    uint32_t recipient  = 0;          // 0=host(P1), 1=client(P2)
    uint32_t lp[2]      = {8000, 8000};
    uint32_t turn       = 0;
    uint32_t phase      = 0;
    uint32_t turnPlayer = 0;
    uint32_t turnCount  = 0;
    uint32_t deckCount [2] = {0, 0};
    uint32_t extraCount[2] = {0, 0};
    uint32_t handCount [2] = {0, 0};

    std::vector<SnapCard> ourHand;        // recipient's hand, face-up
    std::vector<SnapCard> monsters[2];
    std::vector<SnapCard> spells[2];
    std::vector<SnapCard> gy[2];
    std::vector<SnapCard> banished[2];
    // v3: the RECIPIENT's own Extra Deck card codes, in engine sequence
    // order (seq 0..n-1). Hidden-info rule: the host NEVER fills this
    // with the opponent's Extra Deck — the opponent side stays a bare
    // extraCount. Drives the client's own-ED viewer + ED summon list.
    std::vector<uint32_t> ourExtra;
};

inline void serialiseFieldSnapshot(std::vector<uint8_t>& b,
                                   const FieldSnapshotPayload& s) {
    putU32(b, s.version);
    putU32(b, s.recipient);
    putU32(b, s.lp[0]); putU32(b, s.lp[1]);
    putU32(b, s.turn); putU32(b, s.phase);
    putU32(b, s.turnPlayer); putU32(b, s.turnCount);
    putU32(b, s.deckCount[0]);  putU32(b, s.deckCount[1]);
    putU32(b, s.extraCount[0]); putU32(b, s.extraCount[1]);
    putU32(b, s.handCount[0]);  putU32(b, s.handCount[1]);

    auto putList = [&](const std::vector<SnapCard>& v) {
        putU32(b, (uint32_t)v.size());
        for (const auto& c : v) putCard(b, c);
    };
    putList(s.ourHand);
    putList(s.monsters[0]); putList(s.monsters[1]);
    putList(s.spells[0]);   putList(s.spells[1]);
    putList(s.gy[0]);       putList(s.gy[1]);
    putList(s.banished[0]); putList(s.banished[1]);
    // v3 trailer.
    putU32(b, (uint32_t)s.ourExtra.size());
    for (uint32_t c : s.ourExtra) putU32(b, c);
}

inline bool parseFieldSnapshot(const std::vector<uint8_t>& payload,
                               FieldSnapshotPayload& out) {
    NetReader r(payload);
    out.version    = r.u32();
    out.recipient  = r.u32();
    out.lp[0]      = r.u32();
    out.lp[1]      = r.u32();
    out.turn       = r.u32();
    out.phase      = r.u32();
    out.turnPlayer = r.u32();
    out.turnCount  = r.u32();
    out.deckCount[0]  = r.u32();
    out.deckCount[1]  = r.u32();
    out.extraCount[0] = r.u32();
    out.extraCount[1] = r.u32();
    out.handCount[0]  = r.u32();
    out.handCount[1]  = r.u32();
    auto getList = [&](std::vector<SnapCard>& v) {
        uint32_t n = r.u32();
        v.clear();
        v.reserve(n);
        for (uint32_t i = 0; i < n && r.ok; ++i) v.push_back(readCard(r));
    };
    getList(out.ourHand);
    getList(out.monsters[0]); getList(out.monsters[1]);
    getList(out.spells[0]);   getList(out.spells[1]);
    getList(out.gy[0]);       getList(out.gy[1]);
    getList(out.banished[0]); getList(out.banished[1]);
    if (out.version >= 3) {
        uint32_t n = r.u32();
        out.ourExtra.clear();
        out.ourExtra.reserve(n);
        for (uint32_t i = 0; i < n && r.ok; ++i)
            out.ourExtra.push_back(r.u32());
    }
    return r.ok;
}

// ── PromptSnapshotPayload — host tells client "your move" ─────────────
//
// One legal choice per `PromptChoice`. The client picks a choiceId, the
// host maps id→bytes via its local table and applies via respond().
//
// Wire layout:
//   [u32 version][u64 promptSeq][u32 waitType]
//   [u8 owner][u8 turnPlayer][u32 phase]
//   [i32 min][i32 max][u32 forced=0|1]
//   [u32 titleLen][title]
//   [u32 N][ PromptChoice * N ]
//
struct PromptChoice {
    uint32_t    choiceId = 0;        // host-assigned, opaque to client
    uint32_t    code     = 0;        // 0 if not card-bound (e.g. Yes/No)
    uint32_t    loc      = 0;
    uint32_t    seq      = 0;
    uint32_t    iconHint = 0;        // 0=generic/card-bound, 1=yes, 2=no,
                                     // 3=pass, 4=to-Battle-Phase,
                                     // 5=end-turn/end-phase, 6=to-Main-Phase-2,
                                     // 7=finish(-1), 8=confirm-multi-pick
    // ── v2 fields ──────────────────────────────────────────────────────
    // SelectIdleCmd / SelectBattleCmd: the exact respondIdleCmd(t, s)
    // arguments the host registered for this choice. The client uses
    // (cmd, index) to map its local click back onto the host's choiceId;
    // it NEVER builds response bytes itself. Phase pseudo-choices reuse
    // cmd (6=BP, 7=EP in idle; 2=M2, 3=EP in battle) with index 0.
    uint32_t    cmd      = 0;
    uint32_t    index    = 0;
    uint32_t    flags    = 0;        // bit0 = canDirect (battle attackers)
    uint8_t     con      = 0;        // controller of the card (idle actions)
    // ── v3 fields ──────────────────────────────────────────────────────
    // Decoded engine effect description for this choice (SelectOption /
    // SelectChain entries / activatable idle actions). `rawDesc` is the
    // raw u64 desc value for diagnostics; `desc` is the human-readable
    // text resolved from cards.cdb on the HOST. The client renders
    // labels from (label = card name) + (desc = effect text), exactly
    // like the offline renderer builds them from SelectionRequest.
    uint64_t    rawDesc  = 0;
    std::string desc;
    std::string label;
};

struct PromptSnapshotPayload {
    uint32_t  version    = kSnapshotVersion;
    uint64_t  promptSeq  = 0;
    uint32_t  waitType   = 0;       // (uint32_t)WaitType
    uint8_t   owner      = 0;
    uint8_t   turnPlayer = 0;
    uint32_t  phase      = 0;
    int32_t   minSel     = 0;
    int32_t   maxSel     = 0;
    uint32_t  forced     = 0;
    // ── v2 fields ──────────────────────────────────────────────────────
    // SelectIdleCmd / SelectBattleCmd phase-transition permissions —
    // the client rebuilds sel.toBP/toM2/toEP from these so the phase
    // buttons render exactly like offline.
    uint32_t  toBP       = 0;
    uint32_t  toM2       = 0;
    uint32_t  toEP       = 0;
    // SelectPlace: engine zone bitmask (set bit = forbidden) + pick count
    // so the client's field-tile glow works from the snapshot.
    uint32_t  placeFlag  = 0;
    uint32_t  placeCount = 0;
    // ── v3 fields ──────────────────────────────────────────────────────
    // TimingContext of the window (Trigger Effect vs Quick Effect vs
    // chain-response labelling) + the card that CAUSED the prompt
    // (SelectYesNo / SelectEffectYn) with its decoded effect text.
    uint32_t  timing     = 0;        // (uint32_t)TimingContext
    uint32_t  srcCode    = 0;        // card the prompt is about (0 = none)
    std::string srcDesc;             // decoded effect text for srcCode
    std::string title;
    std::vector<PromptChoice> choices;
    bool      valid      = false;   // set true on successful parse
};

inline void putChoice(std::vector<uint8_t>& b, const PromptChoice& c) {
    putU32(b, c.choiceId);
    putU32(b, c.code);
    putU32(b, c.loc);
    putU32(b, c.seq);
    putU32(b, c.iconHint);
    // v2 routing fields.
    putU32(b, c.cmd);
    putU32(b, c.index);
    putU32(b, c.flags);
    putU8 (b, c.con);
    // v3 description fields.
    putU64(b, c.rawDesc);
    putStr(b, c.desc);
    putStr(b, c.label);
}
inline PromptChoice readChoice(NetReader& r, uint32_t version) {
    PromptChoice c;
    c.choiceId = r.u32();
    c.code     = r.u32();
    c.loc      = r.u32();
    c.seq      = r.u32();
    c.iconHint = r.u32();
    if (version >= 2) {
        c.cmd    = r.u32();
        c.index  = r.u32();
        c.flags  = r.u32();
        c.con    = r.u8();
    }
    if (version >= 3) {
        c.rawDesc = r.u64();
        c.desc    = r.str();
    }
    c.label    = r.str();
    return c;
}

inline void serialisePromptSnapshot(std::vector<uint8_t>& b,
                                    const PromptSnapshotPayload& p) {
    putU32(b, p.version);
    putU64(b, p.promptSeq);
    putU32(b, p.waitType);
    putU8 (b, p.owner);
    putU8 (b, p.turnPlayer);
    putU32(b, p.phase);
    putU32(b, (uint32_t)p.minSel);
    putU32(b, (uint32_t)p.maxSel);
    putU32(b, p.forced);
    // v2 fields.
    putU32(b, p.toBP);
    putU32(b, p.toM2);
    putU32(b, p.toEP);
    putU32(b, p.placeFlag);
    putU32(b, p.placeCount);
    // v3 fields.
    putU32(b, p.timing);
    putU32(b, p.srcCode);
    putStr(b, p.srcDesc);
    putStr(b, p.title);
    putU32(b, (uint32_t)p.choices.size());
    for (const auto& c : p.choices) putChoice(b, c);
}

inline bool parsePromptSnapshot(const std::vector<uint8_t>& payload,
                                PromptSnapshotPayload& out) {
    NetReader r(payload);
    out.version    = r.u32();
    out.promptSeq  = r.u64();
    out.waitType   = r.u32();
    out.owner      = r.u8();
    out.turnPlayer = r.u8();
    out.phase      = r.u32();
    out.minSel     = (int32_t)r.u32();
    out.maxSel     = (int32_t)r.u32();
    out.forced     = r.u32();
    if (out.version >= 2) {
        out.toBP       = r.u32();
        out.toM2       = r.u32();
        out.toEP       = r.u32();
        out.placeFlag  = r.u32();
        out.placeCount = r.u32();
    }
    if (out.version >= 3) {
        out.timing  = r.u32();
        out.srcCode = r.u32();
        out.srcDesc = r.str();
    }
    out.title      = r.str();
    uint32_t n     = r.u32();
    out.choices.clear();
    out.choices.reserve(n);
    for (uint32_t i = 0; i < n && r.ok; ++i)
        out.choices.push_back(readChoice(r, out.version));
    out.valid = r.ok;
    return r.ok;
}

// ── ClientChoicePayload — client picks one option for a prompt ────────
//
//   [u64 promptSeq][u32 choiceId]
//   [u32 N][u32 extraIndices[N]]    // unused at Stage 2, reserved for
//                                   // multi-pick prompts (SelectCard N>1)
//
struct ClientChoicePayload {
    uint64_t              promptSeq = 0;
    uint32_t              choiceId  = 0;
    std::vector<uint32_t> extraIndices;
};

inline void serialiseClientChoice(std::vector<uint8_t>& b,
                                  const ClientChoicePayload& c) {
    putU64(b, c.promptSeq);
    putU32(b, c.choiceId);
    putU32(b, (uint32_t)c.extraIndices.size());
    for (uint32_t v : c.extraIndices) putU32(b, v);
}
inline bool parseClientChoice(const std::vector<uint8_t>& payload,
                              ClientChoicePayload& out) {
    NetReader r(payload);
    out.promptSeq = r.u64();
    out.choiceId  = r.u32();
    uint32_t n    = r.u32();
    out.extraIndices.clear();
    out.extraIndices.reserve(n);
    for (uint32_t i = 0; i < n && r.ok; ++i)
        out.extraIndices.push_back(r.u32());
    return r.ok;
}

// ── SyncErrorPayload — hard-fault notice ──────────────────────────────
struct SyncErrorPayload {
    uint32_t    code = 0;    // 1=bad seq, 2=bad owner, 3=illegal choice, 99=other
    std::string detail;
};
inline void serialiseSyncError(std::vector<uint8_t>& b,
                               const SyncErrorPayload& s) {
    putU32(b, s.code);
    putStr(b, s.detail);
}
inline bool parseSyncError(const std::vector<uint8_t>& payload,
                           SyncErrorPayload& out) {
    NetReader r(payload);
    out.code   = r.u32();
    out.detail = r.str();
    return r.ok;
}

// ── Host-side helpers: build payloads from engine state ───────────────
//
// Convert a CardState (from DuelManager FieldState) into a SnapCard.
// Optionally mask the card's identity (face-down or opponent hand).
inline SnapCard makeSnapFrom(const CardState& cs, bool maskCode) {
    SnapCard s;
    s.code   = maskCode ? 0u : cs.code;
    s.loc    = cs.loc;
    s.seq    = cs.seq;
    s.pos    = cs.pos;
    s.player = cs.player;
    s.hidden = maskCode;
    return s;
}

// Build a sanitised FieldSnapshotPayload for the given recipient.
// opponentIdx = recipient ^ 1. Opponent hand contents are reduced to
// the count + a stack of `hidden=true` card backs; everything else is
// public and forwarded as-is.
inline FieldSnapshotPayload buildFieldSnapshot(const FieldState& f,
                                               int recipient,
                                               const std::vector<uint32_t>&
                                                   recipientExtra = {}) {
    FieldSnapshotPayload s;
    s.version    = kSnapshotVersion;
    s.recipient  = (uint32_t)recipient;
    // Recipient's OWN Extra Deck codes (engine order). Never the
    // opponent's — see hidden-info note on the struct.
    s.ourExtra   = recipientExtra;
    s.lp[0]      = f.lp[0]; s.lp[1] = f.lp[1];
    s.turn       = f.turn;
    s.phase      = f.phase;
    s.turnPlayer = f.turnPlayer;
    s.turnCount  = (uint32_t)f.turnCount;
    for (int p = 0; p < 2; ++p) {
        s.deckCount [p] = (uint32_t)f.deckCount [p];
        s.extraCount[p] = (uint32_t)f.extraCount[p];
        s.handCount [p] = (uint32_t)f.hand[p].size();
        for (const auto& c : f.monsters[p]) s.monsters[p].push_back(makeSnapFrom(c, false));
        for (const auto& c : f.spells  [p]) {
            // Face-down S/T have code visible to controller only.
            bool mask = (p != recipient) && (c.pos & POS_FACEDOWN_DEFENSE);
            s.spells[p].push_back(makeSnapFrom(c, mask));
        }
        for (const auto& c : f.gy      [p]) s.gy      [p].push_back(makeSnapFrom(c, false));
        for (const auto& c : f.banished[p]) s.banished[p].push_back(makeSnapFrom(c, false));
    }
    // Recipient hand: face-up.
    for (const auto& c : f.hand[recipient])
        s.ourHand.push_back(makeSnapFrom(c, false));
    return s;
}

// Apply a FieldSnapshotPayload onto a FieldState. The recipient is the
// LOCAL player on the client side; we materialise our hand as visible
// cards and the opponent hand as `count` hidden card backs so the
// existing hand renderer can draw it without code changes.
inline void applySnapshotToField(const FieldSnapshotPayload& s,
                                 FieldState& out, int localPlayer) {
    out = FieldState{};
    out.lp[0] = s.lp[0]; out.lp[1] = s.lp[1];
    out.turn       = (uint8_t)s.turn;
    out.phase      = (uint16_t)s.phase;
    out.turnPlayer = (uint8_t)s.turnPlayer;
    out.turnCount  = (int)s.turnCount;
    for (int p = 0; p < 2; ++p) {
        out.deckCount [p] = (int)s.deckCount [p];
        out.extraCount[p] = (int)s.extraCount[p];
        auto cpy = [&](const std::vector<SnapCard>& src, std::vector<CardState>& dst) {
            dst.clear(); dst.reserve(src.size());
            for (const auto& c : src) {
                CardState d;
                d.code   = c.code;
                d.loc    = c.loc;
                d.seq    = c.seq;
                d.pos    = c.pos;
                d.player = c.player;
                dst.push_back(d);
            }
        };
        cpy(s.monsters[p], out.monsters[p]);
        cpy(s.spells  [p], out.spells  [p]);
        cpy(s.gy      [p], out.gy      [p]);
        cpy(s.banished[p], out.banished[p]);
    }
    // Local hand from the snapshot's ourHand.
    for (const auto& c : s.ourHand) {
        CardState d;
        d.code = c.code; d.loc = c.loc; d.seq = c.seq;
        d.pos = c.pos;   d.player = (uint8_t)localPlayer;
        out.hand[localPlayer].push_back(d);
    }
    // Opponent hand: handCount face-down card backs (code=0).
    int oppIdx = localPlayer ^ 1;
    for (uint32_t i = 0; i < s.handCount[oppIdx]; ++i) {
        CardState d;
        d.code = 0; d.loc = 0x02 /*LOCATION_HAND*/; d.seq = i;
        d.pos = POS_FACEDOWN_DEFENSE;
        d.player = (uint8_t)oppIdx;
        out.hand[oppIdx].push_back(d);
    }
}

} // namespace edo

