#include "ScriptFetcher.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace {

// GET https://raw.githubusercontent.com<path> -> body. Returns false on any
// non-200 or network failure. Timeouts are short: duel start blocks on this,
// and an offline machine must fail fast, not hang.
bool httpGet(const std::wstring& path, std::string& out) {
    HINTERNET s = WinHttpOpen(L"YGONova-Scripts/1.0",
                              WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 3000, 3000, 5000, 5000);
    HINTERNET c = WinHttpConnect(s, L"raw.githubusercontent.com",
                                 INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!c) { WinHttpCloseHandle(s); return false; }
    HINTERNET r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
    bool ok = false;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE |
                               WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                            WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            ok = true;
            DWORD avail = 0;
            do {
                avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) { ok = false; break; }
                if (!avail) break;
                size_t off = out.size();
                out.resize(off + avail);
                DWORD read = 0;
                if (!WinHttpReadData(r, &out[off], avail, &read)) {
                    ok = false; break;
                }
                out.resize(off + read);
            } while (avail > 0);
        }
    }
    if (r) WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok && !out.empty();
}

// Fetch one script, trying official/ then unofficial/ (pre-release cards),
// and write it into the matching local folder the script reader searches.
bool fetchOne(uint32_t code) {
    const wchar_t* repoDirs[]  = {L"official", L"unofficial"};
    const char*    localDirs[] = {"assets/scripts/official",
                                  "assets/scripts/unofficial"};
    for (int i = 0; i < 2; ++i) {
        std::wstring path = L"/ProjectIgnis/CardScripts/master/";
        path += repoDirs[i];
        path += L"/c" + std::to_wstring(code) + L".lua";
        std::string body;
        if (!httpGet(path, body)) continue;
        std::error_code ec;
        std::filesystem::create_directories(localDirs[i], ec);
        std::string file = std::string(localDirs[i]) + "/c" +
                           std::to_string(code) + ".lua";
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(body.data(), (std::streamsize)body.size());
        return f.good();
    }
    return false;
}

} // namespace

namespace edo {

int fetchCardScripts(const std::vector<uint32_t>& codes) {
    if (codes.empty()) return 0;
    std::atomic<int> next{0}, fetched{0};
    const int kWorkers = (int)codes.size() < 4 ? (int)codes.size() : 4;
    std::vector<std::thread> pool;
    for (int i = 0; i < kWorkers; ++i)
        pool.emplace_back([&] {
            for (;;) {
                int idx = next.fetch_add(1);
                if (idx >= (int)codes.size()) return;
                if (fetchOne(codes[(size_t)idx])) fetched.fetch_add(1);
            }
        });
    for (auto& t : pool) t.join();
    return fetched.load();
}

} // namespace edo

#else
namespace edo {
int fetchCardScripts(const std::vector<uint32_t>&) { return 0; }
}
#endif
