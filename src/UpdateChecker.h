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

    // ── Self-update (auto-updater) ───────────────────────────────────────────
    // The check stores the release's installer (.exe) asset URL. beginDownload()
    // fetches it to a temp file on a worker thread; runInstaller() launches it.
    // The caller should quit the app right after a successful runInstaller() so
    // the installer can overwrite the running files.
    enum class DownloadState { Idle, Running, Ready, Failed };
    bool        canSelfUpdate() const;            // an installer asset was found
    void        beginDownload();                  // async download of the asset
    DownloadState downloadState() const { return m_dlState.load(); }
    double      downloadProgress() const { return m_dlProgress.load(); }  // 0..1
    std::string installerPath() const;            // valid once Ready
    bool        runInstaller();                   // launch it; true on success

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
    std::string       m_assetUrl; // installer download URL, guarded by m_mx

    std::atomic<DownloadState> m_dlState{DownloadState::Idle};
    std::atomic<double>        m_dlProgress{0.0};
    std::thread                m_dlWorker;
    std::string                m_installerPath;   // guarded by m_mx
};

// Open a URL in the user's default browser (no-op on unsupported platforms).
void openUrl(const std::string& url);

} // namespace edo
