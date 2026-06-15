#include "ImageFetcher.h"

#include <filesystem>
#include <vector>
#include <cstdio>

#ifdef _WIN32
  #include <windows.h>
  #include <winhttp.h>
  #pragma comment(lib, "winhttp.lib")
#endif

namespace edo {

// Public CDN for card art, keyed by Konami passcode. Most standard cards
// resolve here; misses (alt-art high codes, etc.) just fall back to the
// placeholder. Kept as a single host/path constant so it is easy to retarget.
static const wchar_t* kCdnHost = L"images.ygoprodeck.com";
static const wchar_t* kCdnPathPrefix = L"/images/cards/";   // + <code>.jpg

ImageFetcher::~ImageFetcher() { stop(); }

void ImageFetcher::start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) return;
    m_worker = std::thread([this]{ workerLoop(); });
}

void ImageFetcher::stop() {
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false)) return;
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void ImageFetcher::request(uint32_t code, const std::string& destPath) {
    if (!m_enabled.load() || !m_running.load()) return;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_state.find(code);
        if (it != m_state.end()) {
            // Already None-with-entry should not happen; In-flight/Done/Failed
            // are all terminal-or-pending — don't re-enqueue.
            if (it->second != State::None) return;
        }
        m_state[code] = State::InFlight;
        m_queue.push_back(Job{ code, destPath });
    }
    m_inFlightCount.fetch_add(1);
    m_cv.notify_one();
}

ImageFetcher::State ImageFetcher::state(uint32_t code) {
    std::lock_guard<std::mutex> lk(m_mx);
    auto it = m_state.find(code);
    return it == m_state.end() ? State::None : it->second;
}

void ImageFetcher::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            m_cv.wait(lk, [this]{ return !m_running.load() || !m_queue.empty(); });
            if (!m_running.load() && m_queue.empty()) return;
            job = m_queue.front();
            m_queue.pop_front();
        }

        bool ok = fetchToFile(job.code, job.dest);

        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_state[job.code] = ok ? State::Done : State::Failed;
        }
        m_inFlightCount.fetch_sub(1);
        if (ok) m_doneCount.fetch_add(1);
        else    m_failCount.fetch_add(1);
    }
}

#ifdef _WIN32
bool ImageFetcher::fetchToFile(uint32_t code, const std::string& dest) {
    // Ensure the destination directory exists (release bundles omit
    // assets/cards/ entirely, so it may be created here on first run).
    try {
        std::filesystem::path p(dest);
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path());
    } catch (...) { /* fall through; the write below will report failure */ }

    std::wstring path = std::wstring(kCdnPathPrefix) +
                        std::to_wstring(code) + L".jpg";

    HINTERNET hSession = WinHttpOpen(L"EdoProPlus/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    // Reasonable timeouts so a stalled CDN can't wedge the worker.
    WinHttpSetTimeouts(hSession, 8000, 8000, 12000, 12000);

    HINTERNET hConnect = WinHttpConnect(hSession, kCdnHost,
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
    std::vector<uint8_t> body;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {

        // Only accept HTTP 200 — a 404 must become a Failed state, never a
        // truncated/HTML file written to the cache.
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
            WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            DWORD avail = 0;
            ok = true;
            do {
                avail = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &avail)) { ok = false; break; }
                if (avail == 0) break;
                size_t off = body.size();
                body.resize(off + avail);
                DWORD read = 0;
                if (!WinHttpReadData(hRequest, body.data() + off, avail, &read)) {
                    ok = false; break;
                }
                body.resize(off + read);
            } while (avail > 0);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!ok || body.empty()) return false;

    // Write atomically: temp file then rename, so a half-written image is
    // never observed by the renderer's loadTexture.
    std::string tmp = dest + ".part";
    {
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return false;
        size_t w = std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);
        if (w != body.size()) { std::remove(tmp.c_str()); return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        // Cross-volume or locked — fall back to copy+remove.
        std::filesystem::copy_file(tmp, dest,
            std::filesystem::copy_options::overwrite_existing, ec);
        std::remove(tmp.c_str());
        if (ec) return false;
    }
    return true;
}
#else
bool ImageFetcher::fetchToFile(uint32_t, const std::string&) {
    return false;   // no downloader on non-Windows builds
}
#endif

} // namespace edo
