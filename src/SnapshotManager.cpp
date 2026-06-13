#include "SnapshotManager.h"
#include <algorithm>

void SnapshotManager::recordResponse(const void* data, uint32_t len) {
    std::vector<uint8_t> resp(len);
    memcpy(resp.data(), data, len);
    m_liveLog.push_back(std::move(resp));
}

int SnapshotManager::save(const std::string& label, int turnCount, uint16_t phase) {
    if (!m_enabled) return -1;
    SnapshotEntry entry;
    entry.responses = m_liveLog;   // copy log up to this moment
    entry.label     = label;
    entry.turnCount = turnCount;
    entry.phase     = phase;
    m_stack.push_back(std::move(entry));
    return (int)m_stack.size() - 1;
}

const SnapshotEntry* SnapshotManager::rewind() {
    if (m_stack.empty()) return nullptr;
    const SnapshotEntry& entry = m_stack.back();
    m_pendingResponses = entry.responses;
    m_liveLog          = entry.responses;
    m_stack.pop_back();
    return m_stack.empty() ? nullptr : &m_stack.back();
}

const SnapshotEntry* SnapshotManager::jumpTo(int idx) {
    if (idx < 0 || idx >= (int)m_stack.size()) return nullptr;
    // Pop down to idx
    while ((int)m_stack.size() - 1 > idx) {
        m_stack.pop_back();
    }
    const SnapshotEntry& entry = m_stack[idx];
    m_pendingResponses = entry.responses;
    m_liveLog          = entry.responses;
    return &m_stack[idx];
}

void SnapshotManager::clear() {
    m_liveLog.clear();
    m_stack.clear();
    m_pendingResponses.clear();
}
