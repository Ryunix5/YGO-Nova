#pragma once
#include <vector>
#include <string>
#include <cstdint>

struct SnapshotEntry {
    std::vector<std::vector<uint8_t>> responses; // all responses up to this point
    std::string label;
    int turnCount = 0;
    uint16_t phase = 0;
};

// SnapshotManager records every player response and can restore the duel
// to any previous state by replaying responses from T=0.
// DuelManager calls recordResponse() after every OCG_DuelSetResponse,
// and saveSnapshot() before each player action when Testing Mode is on.
class SnapshotManager {
public:
    SnapshotManager() = default;

    void setEnabled(bool on) { m_enabled = on; }
    bool isEnabled() const { return m_enabled; }

    // Record a response into the running log
    void recordResponse(const void* data, uint32_t len);

    // Push current state as a named snapshot
    int  save(const std::string& label, int turnCount, uint16_t phase);

    // Pop the last snapshot — returns the entry to replay from, or nullptr
    const SnapshotEntry* rewind();

    // Jump to a specific snapshot index (rewinds to that point)
    const SnapshotEntry* jumpTo(int idx);

    int  count() const { return (int)m_stack.size(); }
    void clear();

    const std::vector<SnapshotEntry>& snapshots() const { return m_stack; }

    // After rewind(), call this to get the response sequence to replay
    const std::vector<std::vector<uint8_t>>& pendingResponses() const {
        return m_pendingResponses;
    }
    void clearPending() { m_pendingResponses.clear(); }

private:
    bool m_enabled = false;
    std::vector<std::vector<uint8_t>> m_liveLog;  // all responses so far
    std::vector<SnapshotEntry>        m_stack;
    std::vector<std::vector<uint8_t>> m_pendingResponses; // set on rewind
};
