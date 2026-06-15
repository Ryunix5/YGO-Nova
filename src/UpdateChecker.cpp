#include "UpdateChecker.h"

#include <vector>
#include <cctype>

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
    if (m_worker.joinable()) m_worker.join();
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
            std::lock_guard<std::mutex> lk(m_mx);
            // Store the display version without a leading 'v'.
            m_latest = (tag.size() && (tag[0] == 'v' || tag[0] == 'V'))
                       ? tag.substr(1) : tag;
            m_url = url;
            if (isNewer(tag, current)) m_updateAvailable.store(true);
        }
    }
    m_finished.store(true);
}
#else
void UpdateChecker::run(std::string, std::string) { m_finished.store(true); }
#endif

} // namespace edo
