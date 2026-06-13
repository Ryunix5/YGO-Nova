#pragma once
// ─── TestingTimeline.h ──────────────────────────────────────────────────────
//
// Offline "Testing Mode" timeline — a chess-style move history that lets the
// player rewind to any previous point in the duel and continue from there.
//
// Design — DETERMINISTIC REBUILD (not a visual rewind, not an engine
// snapshot):
//   * At duel start we capture a deterministic ROOT: the exact seed, both
//     decks (card lists, pre-shuffle — ocgcore shuffles from the seed), and
//     the LP / hand / draw config.
//   * Every player/auto response fed to ocgcore is recorded here as the exact
//     RESPONSE BYTES plus a human-readable label + context.
//   * To restore to action K we tear down the engine and start a FRESH duel
//     with the same seed + decks, then replay responses[0..K) into it. Because
//     ocgcore is fully deterministic for a fixed seed + response stream, this
//     reproduces ALL internal state — deck order, once-per-turn flags, chain
//     state, lingering effects, used effects — not just the visible field.
//
// FieldState alone is NOT enough to restore: it is only the public/visual
// projection of the board. The engine's private flags live inside ocgcore and
// can only be reconstructed by re-running it from the seed.
//
// This is header-only (like Settings.h / Anim.h) so it needs no CMake change.
// The restore ORCHESTRATION (which drives DuelManager) lives in UI.cpp; this
// file is just the data model + branch bookkeeping.
//
#include "CardDB.h"          // Deck
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

namespace edo {

// One recorded response = one timeline step.
struct TestingAction {
    int         index    = 0;          // position in the timeline
    std::string label;                 // "Turn 1 M1 — Normal Summon Foo"
    int         turn     = 0;
    int         phase    = 0;
    int         player   = 0;
    int         waitType = 0;          // WaitType the response answered
    std::vector<uint8_t> responseBytes;// EXACT bytes fed to ocgcore
    // Source context (best-effort; -1/0 when not card-bound).
    uint32_t    cardCode   = 0;
    std::string cardName;
    int         controller = -1;
    int         location   = 0;
    int         sequence   = 0;
};

// Deterministic root captured once per duel.
struct TestingRoot {
    bool        valid = false;
    uint64_t    seed  = 0;
    Deck        deck0, deck1;
    uint32_t    lp = 8000, handCount = 5, drawCount = 1;
    std::string deck0Name, deck1Name;
};

class TestingTimeline {
public:
    bool enabled() const     { return m_enabled; }
    void setEnabled(bool on)  { m_enabled = on; }

    void clear() { m_root = TestingRoot{}; m_actions.clear(); m_applied = 0; }

    // Capture the deterministic root at duel start.
    void beginDuel(const TestingRoot& root) {
        m_root = root; m_root.valid = true;
        m_actions.clear();
        m_applied = 0;
    }
    bool                hasRoot() const { return m_root.valid; }
    const TestingRoot&  root()    const { return m_root; }

    // Record a new action at the current head. If the head is not at the end
    // of the timeline (the user rewound and is now acting), the "future"
    // actions are discarded first (a new branch). `outDiscarded` receives how
    // many were dropped so the caller can log [TESTING BRANCH].
    int record(TestingAction a, int* outDiscarded = nullptr) {
        int discarded = 0;
        if (m_applied < (int)m_actions.size()) {
            discarded = (int)m_actions.size() - m_applied;
            m_actions.resize((size_t)m_applied);
        }
        a.index = (int)m_actions.size();
        m_actions.push_back(std::move(a));
        m_applied = (int)m_actions.size();
        if (outDiscarded) *outDiscarded = discarded;
        return (int)m_actions.size() - 1;
    }

    const std::vector<TestingAction>& actions() const { return m_actions; }
    int  size()    const { return (int)m_actions.size(); }
    // Number of responses currently applied to the live engine. Equal to
    // size() when "live at head"; smaller after a rewind.
    int  applied() const { return m_applied; }
    void setApplied(int n) {
        m_applied = std::max(0, std::min(n, (int)m_actions.size()));
    }
    bool atHead()  const { return m_applied == (int)m_actions.size(); }

private:
    bool        m_enabled = false;
    TestingRoot m_root;
    std::vector<TestingAction> m_actions;
    int         m_applied = 0;
};

} // namespace edo
