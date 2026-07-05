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

// Image host for card art, keyed by Konami passcode. Configurable at build
// time so a self-hosted mirror (e.g. a sponsor's server, reachable in regions
// where the public CDN is blocked) can serve art without a code change:
//   cmake -DEDOPRO_IMAGE_CDN_HOST=images.myserver.com
//         -DEDOPRO_IMAGE_CDN_PATH=/cards/
// Defaults to the public ygoprodeck CDN. Misses just fall back to placeholder.
#ifndef EDOPRO_IMAGE_CDN_HOST
#define EDOPRO_IMAGE_CDN_HOST "images.ygoprodeck.com"
#endif
#ifndef EDOPRO_IMAGE_CDN_PATH
#define EDOPRO_IMAGE_CDN_PATH "/images/cards/"
#endif
#define EDO_WIDEN2(x) L##x
#define EDO_WIDEN(x)  EDO_WIDEN2(x)
static const wchar_t* kCdnHost       = EDO_WIDEN(EDOPRO_IMAGE_CDN_HOST);
static const wchar_t* kCdnPathPrefix = EDO_WIDEN(EDOPRO_IMAGE_CDN_PATH); // +<code>.jpg

ImageFetcher::~ImageFetcher() { stop(); }

// Parse "host[/path/]" entries, ';' separated. Hostnames are ASCII, so a
// naive widen is fine. A missing path means "same layout as the default
// CDN" (mirrors are usually straight proxies of it).
void ImageFetcher::setMirrors(const std::string& semicolonList) {
    std::vector<Endpoint> parsed;
    size_t pos = 0;
    while (pos <= semicolonList.size()) {
        size_t end = semicolonList.find(';', pos);
        if (end == std::string::npos) end = semicolonList.size();
        std::string entry = semicolonList.substr(pos, end - pos);
        pos = end + 1;
        // Trim spaces and a leading scheme if the user pasted a URL.
        while (!entry.empty() && entry.front() == ' ') entry.erase(entry.begin());
        while (!entry.empty() && entry.back()  == ' ') entry.pop_back();
        if (entry.rfind("https://", 0) == 0) entry.erase(0, 8);
        if (entry.rfind("http://",  0) == 0) entry.erase(0, 7);
        if (entry.empty()) continue;
        std::string host = entry, path = EDOPRO_IMAGE_CDN_PATH;
        size_t slash = entry.find('/');
        if (slash != std::string::npos) {
            host = entry.substr(0, slash);
            path = entry.substr(slash);
            if (path.back() != '/') path += '/';
        }
        Endpoint ep;
        ep.host.assign(host.begin(), host.end());
        ep.path.assign(path.begin(), path.end());
        parsed.push_back(std::move(ep));
    }
    std::lock_guard<std::mutex> lk(m_mx);
    m_mirrors = std::move(parsed);
    // A new chain deserves a fresh shot at everything that failed on the
    // old one — re-arm Failed codes so the next view retries them.
    for (auto& kv : m_state)
        if (kv.second == State::Failed) kv.second = State::None;
}

// The chain a fetch walks: user mirrors first (their region may block the
// default host — trying a dead host first would add a timeout per image),
// then the built-in CDN.
std::vector<ImageFetcher::Endpoint> ImageFetcher::endpointChain() {
    std::vector<Endpoint> chain;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        chain = m_mirrors;
    }
    chain.push_back(Endpoint{ kCdnHost, kCdnPathPrefix });
    return chain;
}

void ImageFetcher::start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) return;
    // A handful of parallel connections to the CDN so a deck page's worth of
    // art arrives together instead of trickling in one image at a time. Kept
    // modest to stay polite to the CDN and bounded on memory.
    const int kWorkers = 6;
    m_workers.reserve(kWorkers);
    for (int i = 0; i < kWorkers; ++i)
        m_workers.emplace_back([this]{ workerLoop(); });
}

void ImageFetcher::stop() {
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false)) return;
    m_cv.notify_all();
    for (auto& t : m_workers)
        if (t.joinable()) t.join();
    m_workers.clear();
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
// One HTTPS GET from one host. Returns true on a 200 with a body.
static bool fetchFromHost(const std::wstring& host,
                          const std::wstring& pathPrefix,
                          uint32_t code, std::vector<uint8_t>& body) {
    std::wstring path = pathPrefix + std::to_wstring(code) + L".jpg";

    HINTERNET hSession = WinHttpOpen(L"EdoProPlus/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    // Reasonable timeouts so a stalled CDN can't wedge the worker.
    WinHttpSetTimeouts(hSession, 8000, 8000, 12000, 12000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
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
    body.clear();
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
    return ok && !body.empty();
}

bool ImageFetcher::fetchToFile(uint32_t code, const std::string& dest) {
    // Ensure the destination directory exists (release bundles omit
    // assets/cards/ entirely, so it may be created here on first run).
    try {
        std::filesystem::path p(dest);
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path());
    } catch (...) { /* fall through; the write below will report failure */ }

    // Walk the endpoint chain — user mirrors first, built-in CDN last —
    // until any host serves the image.
    std::vector<uint8_t> body;
    bool ok = false;
    for (const Endpoint& ep : endpointChain())
        if (fetchFromHost(ep.host, ep.path, code, body)) { ok = true; break; }
    if (!ok) return false;

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
