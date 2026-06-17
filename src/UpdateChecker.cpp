#include "UpdateChecker.h"

#include <vector>
#include <cctype>
#include <cstdio>

#ifdef _WIN32
  #include <windows.h>
  #include <winhttp.h>
  #include <shellapi.h>
  #pragma comment(lib, "winhttp.lib")
  #pragma comment(lib, "shell32.lib")
#endif

namespace edo {

void openUrl(const std::string& url) {
#ifdef _WIN32
    if (!url.empty())
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
#else
    (void)url;
#endif
}

// Parse up to three dotted integers from a version string ("v1.2.3" -> 1,2,3).
// Non-numeric suffixes (e.g. "-beta") stop parsing; missing parts are 0.
static void parseVer(const std::string& s, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    size_t i = 0;
    if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i;
    for (int part = 0; part < 3 && i < s.size(); ++part) {
        int v = 0; bool any = false;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) {
            v = v * 10 + (s[i] - '0'); ++i; any = true;
        }
        out[part] = v;
        if (!any) break;
        if (i < s.size() && s[i] == '.') ++i; else break;
    }
}

// Returns true if `latest` is strictly newer than `current`.
static bool isNewer(const std::string& latest, const std::string& current) {
    int a[3], b[3];
    parseVer(latest, a);
    parseVer(current, b);
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

// Extract the string value of a top-level JSON key: "key":"value".
// Minimal — avoids pulling in a JSON dependency for two fields.
static std::string jsonStr(const std::string& body, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t k = body.find(needle);
    if (k == std::string::npos) return "";
    size_t colon = body.find(':', k + needle.size());
    if (colon == std::string::npos) return "";
    size_t q1 = body.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    std::string out;
    for (size_t i = q1 + 1; i < body.size(); ++i) {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size()) { out += body[++i]; continue; }
        if (c == '"') break;
        out += c;
    }
    return out;
}

UpdateChecker::~UpdateChecker() {
    if (m_worker.joinable())   m_worker.join();
    if (m_dlWorker.joinable()) m_dlWorker.join();
}

// Scan a release JSON for the installer asset's download URL. GitHub lists each
// asset's "browser_download_url"; we pick the .exe (preferring a *Setup* one).
static std::string findInstallerAsset(const std::string& body) {
    const std::string key = "\"browser_download_url\"";
    std::string best;
    size_t pos = 0;
    while ((pos = body.find(key, pos)) != std::string::npos) {
        size_t colon = body.find(':', pos + key.size());
        size_t q1 = colon == std::string::npos ? std::string::npos
                                               : body.find('"', colon + 1);
        size_t q2 = q1 == std::string::npos ? std::string::npos
                                            : body.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string url = body.substr(q1 + 1, q2 - q1 - 1);
        pos = q2 + 1;
        std::string low = url;
        for (char& c : low) c = (char)std::tolower((unsigned char)c);
        if (low.size() >= 4 && low.compare(low.size() - 4, 4, ".exe") == 0) {
            if (low.find("setup") != std::string::npos) return url;  // best match
            if (best.empty()) best = url;
        }
    }
    return best;
}

bool UpdateChecker::canSelfUpdate() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return !m_assetUrl.empty();
}
std::string UpdateChecker::installerPath() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_installerPath;
}

std::string UpdateChecker::latestVersion() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_latest;
}
std::string UpdateChecker::releaseUrl() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_url;
}

void UpdateChecker::start(const std::string& currentVersion,
                          const std::string& repo) {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) return;
    if (!m_enabled.load() || repo.empty()) { m_finished.store(true); return; }
    m_worker = std::thread([this, currentVersion, repo]{
        run(currentVersion, repo);
    });
}

#ifdef _WIN32
static bool httpGetString(const wchar_t* host, const std::wstring& path,
                          std::string& outBody) {
    HINTERNET hSession = WinHttpOpen(L"EdoProPlus-UpdateCheck/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 6000, 6000, 8000, 8000);

    HINTERNET hConnect = WinHttpConnect(hSession, host,
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    bool ok = false;
    // GitHub's API requires a User-Agent and a JSON Accept header.
    const wchar_t* headers =
        L"User-Agent: EdoProPlus\r\nAccept: application/vnd.github+json\r\n";
    if (WinHttpSendRequest(hRequest, headers, (DWORD)-1L,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {

        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
            WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            std::string body;
            DWORD avail = 0;
            ok = true;
            do {
                avail = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &avail)) { ok = false; break; }
                if (avail == 0) break;
                size_t off = body.size();
                body.resize(off + avail);
                DWORD read = 0;
                if (!WinHttpReadData(hRequest, &body[off], avail, &read)) {
                    ok = false; break;
                }
                body.resize(off + read);
            } while (avail > 0);
            if (ok) outBody = std::move(body);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

void UpdateChecker::run(std::string current, std::string repo) {
    std::wstring path = L"/repos/";
    for (char c : repo) path += (wchar_t)c;
    path += L"/releases/latest";

    std::string body;
    if (httpGetString(L"api.github.com", path, body)) {
        std::string tag = jsonStr(body, "tag_name");
        std::string url = jsonStr(body, "html_url");
        if (!tag.empty()) {
            std::string asset = findInstallerAsset(body);
            std::lock_guard<std::mutex> lk(m_mx);
            // Store the display version without a leading 'v'.
            m_latest = (tag.size() && (tag[0] == 'v' || tag[0] == 'V'))
                       ? tag.substr(1) : tag;
            m_url = url;
            m_assetUrl = asset;
            if (isNewer(tag, current)) m_updateAvailable.store(true);
        }
    }
    m_finished.store(true);
}

// Split "https://host/path..." into host + path. http(s) only.
static bool parseHttpsUrl(const std::string& url, std::wstring& host,
                          std::wstring& path) {
    const std::string pfx = "https://";
    if (url.rfind(pfx, 0) != 0) return false;
    size_t slash = url.find('/', pfx.size());
    std::string h = url.substr(pfx.size(),
                               slash == std::string::npos ? std::string::npos
                                                          : slash - pfx.size());
    std::string p = slash == std::string::npos ? "/" : url.substr(slash);
    host.assign(h.begin(), h.end());
    path.assign(p.begin(), p.end());
    return !host.empty();
}

// Download an https URL to `dest` (follows redirects — GitHub asset URLs 302 to
// a CDN host). Reports 0..1 progress when Content-Length is known.
static bool httpDownloadToFile(const std::string& url, const std::string& dest,
                               std::atomic<double>* progress) {
    std::wstring host, path;
    if (!parseHttpsUrl(url, host, path)) return false;

    HINTERNET hSession = WinHttpOpen(L"YGONova-Updater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 15000, 15000, 30000, 120000);
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false;
    }

    bool ok = false;
    const wchar_t* headers = L"User-Agent: YGONova\r\n";
    if (WinHttpSendRequest(hRequest, headers, (DWORD)-1L,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {

        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            DWORD64 total = 0; DWORD tlen = sizeof(total);
            WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &total, &tlen, WINHTTP_NO_HEADER_INDEX);

            std::string tmp = dest + ".part";
            FILE* f = nullptr; fopen_s(&f, tmp.c_str(), "wb");
            if (f) {
                ok = true;
                DWORD64 got = 0; DWORD avail = 0;
                std::vector<char> buf;
                do {
                    avail = 0;
                    if (!WinHttpQueryDataAvailable(hRequest, &avail)) { ok = false; break; }
                    if (avail == 0) break;
                    buf.resize(avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) { ok = false; break; }
                    if (read && fwrite(buf.data(), 1, read, f) != read) { ok = false; break; }
                    got += read;
                    if (progress && total > 0)
                        progress->store((double)got / (double)total);
                } while (avail > 0);
                fclose(f);
                if (ok && got > 0) {
                    std::remove(dest.c_str());
                    ok = (std::rename(tmp.c_str(), dest.c_str()) == 0);
                } else ok = false;
                if (!ok) std::remove(tmp.c_str());
            }
        }
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

void UpdateChecker::beginDownload() {
    if (m_dlState.load() == DownloadState::Running) return;
    std::string url;
    { std::lock_guard<std::mutex> lk(m_mx); url = m_assetUrl; }
    if (url.empty()) { m_dlState.store(DownloadState::Failed); return; }
    if (m_dlWorker.joinable()) m_dlWorker.join();
    m_dlProgress.store(0.0);
    m_dlState.store(DownloadState::Running);
    m_dlWorker = std::thread([this, url] {
        wchar_t tmpDir[MAX_PATH] = {0};
        DWORD n = GetTempPathW(MAX_PATH, tmpDir);
        std::wstring wdest = (n ? std::wstring(tmpDir, n) : std::wstring(L"."))
                             + L"YGONova-Update-Setup.exe";
        std::string dest;
        for (wchar_t c : wdest) dest += (char)c;   // ASCII temp path
        bool ok = httpDownloadToFile(url, dest, &m_dlProgress);
        if (ok) {
            std::lock_guard<std::mutex> lk(m_mx);
            m_installerPath = dest;
            m_dlState.store(DownloadState::Ready);
        } else {
            m_dlState.store(DownloadState::Failed);
        }
    });
}

bool UpdateChecker::runInstaller() {
    std::string p;
    { std::lock_guard<std::mutex> lk(m_mx); p = m_installerPath; }
    if (p.empty()) return false;
    HINSTANCE r = ShellExecuteA(nullptr, "open", p.c_str(), nullptr, nullptr,
                                SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}
#else
void UpdateChecker::run(std::string, std::string) { m_finished.store(true); }
void UpdateChecker::beginDownload() { m_dlState.store(DownloadState::Failed); }
bool UpdateChecker::runInstaller() { return false; }
#endif

} // namespace edo
