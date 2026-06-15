#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

namespace edo {

// Background card-image downloader.
//
// Online releases do NOT bundle the ~9k card images (Konami IP + ~300 MB).
// Instead the first time a card is shown, its art is fetched from a public CDN
// on a worker thread and cached to assets/cards/<code>.jpg, so every later view
// (and every later run) loads from disk. The UI never blocks: while a download
// is in flight the renderer shows the card-back / unknown placeholder.
class ImageFetcher {
public:
    enum class State { None, InFlight, Done, Failed };

    ImageFetcher() = default;
    ~ImageFetcher();

    // Spin up the worker thread. Safe to call once after construction.
    void start();
    void stop();

    void setEnabled(bool on) { m_enabled.store(on); }
    bool enabled() const     { return m_enabled.load(); }

    // Request a download of `code` to `destPath` (relative to the exe's cwd,
    // e.g. "assets/cards/12345.jpg"). No-op when disabled or when the code is
    // already in flight / done / failed. Thread-safe; returns immediately.
    void request(uint32_t code, const std::string& destPath);

    // Current state for `code`. Thread-safe.
    State state(uint32_t code);

    // Diagnostics.
    int   inFlight() const { return m_inFlightCount.load(); }
    int   completed() const { return m_doneCount.load(); }
    int   failed() const   { return m_failCount.load(); }

private:
    struct Job { uint32_t code; std::string dest; };

    void workerLoop();
    // Platform HTTPS GET → file. Returns true on a 200 with bytes written.
    static bool fetchToFile(uint32_t code, const std::string& dest);

    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_running{false};
    std::thread       m_worker;

    std::mutex                          m_mx;
    std::condition_variable             m_cv;
    std::deque<Job>                     m_queue;
    std::unordered_map<uint32_t, State> m_state;   // guarded by m_mx

    std::atomic<int> m_inFlightCount{0};
    std::atomic<int> m_doneCount{0};
    std::atomic<int> m_failCount{0};
};

} // namespace edo
