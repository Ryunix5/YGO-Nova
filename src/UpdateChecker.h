#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <mutex>

namespace edo {

// Lightweight "is there a newer release?" check.
//
// On launch it asks GitHub's API for the repo's latest release tag and compares
// it to the running version. No server of our own is needed — GitHub hosts the
// version info. The result is just a notice + a link to the release page; we
// never download or replace anything automatically.
//
// The target repo is baked in at build time via EDOPRO_UPDATE_REPO ("owner/name");
// when it's blank the checker is a no-op, so dev builds stay quiet/offline.
class UpdateChecker {
public:
    ~UpdateChecker();

    // Kick off the async check once. `repo` is "owner/name" (empty => disabled).
    void start(const std::string& currentVersion, const std::string& repo);
    void setEnabled(bool on) { m_enabled.store(on); }
    bool enabled() const     { return m_enabled.load(); }

    bool finished() const        { return m_finished.load(); }
    bool updateAvailable() const { return m_updateAvailable.load(); }
    std::string latestVersion() const;   // e.g. "1.2.0" (no leading v)
    std::string releaseUrl() const;      // GitHub release page URL

private:
    void run(std::string current, std::string repo);

    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_updateAvailable{false};
    std::thread       m_worker;
    mutable std::mutex m_mx;
    std::string       m_latest;   // guarded by m_mx
    std::string       m_url;      // guarded by m_mx
};

// Open a URL in the user's default browser (no-op on unsupported platforms).
void openUrl(const std::string& url);

} // namespace edo
