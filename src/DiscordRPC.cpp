#include "DiscordRPC.h"

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

HANDLE g_pipe = INVALID_HANDLE_VALUE;
bool   g_handshook = false;
double g_backoffUntil = 0.0;   // GetTickCount64-based retry gate (ms)

std::string jsonEscape(const char* s) {
    std::string out;
    for (; s && *s; ++s) {
        if (*s == '"' || *s == '\\') { out += '\\'; out += *s; }
        else if ((unsigned char)*s >= 0x20) out += *s;
    }
    return out;
}

// One IPC frame: i32 opcode LE, i32 length LE, payload.
bool sendFrame(int op, const std::string& json) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    std::string buf(8 + json.size(), '\0');
    int len = (int)json.size();
    memcpy(&buf[0], &op, 4);
    memcpy(&buf[4], &len, 4);
    memcpy(&buf[8], json.data(), json.size());
    DWORD written = 0;
    if (!WriteFile(g_pipe, buf.data(), (DWORD)buf.size(), &written, nullptr) ||
        written != buf.size()) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
        g_handshook = false;
        return false;
    }
    // Drain any pending response bytes so the pipe buffer never fills. The
    // replies (READY / ack) aren't needed for fire-and-forget presence.
    DWORD avail = 0;
    while (PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr) &&
           avail > 0) {
        char sink[512];
        DWORD rd = 0;
        if (!ReadFile(g_pipe, sink, (DWORD)min((DWORD)sizeof(sink), avail),
                      &rd, nullptr) || rd == 0)
            break;
        avail = 0;
    }
    return true;
}

bool connectAndHandshake(const char* clientId) {
    if (g_pipe != INVALID_HANDLE_VALUE && g_handshook) return true;
    double now = (double)GetTickCount64();
    if (now < g_backoffUntil) return false;       // don't hammer a dead pipe
    for (int i = 0; i < 10 && g_pipe == INVALID_HANDLE_VALUE; ++i) {
        char name[40];
        snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
        g_pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
        if (g_pipe != INVALID_HANDLE_VALUE) break;
    }
    if (g_pipe == INVALID_HANDLE_VALUE) {
        g_backoffUntil = now + 30000.0;           // Discord not running: 30s
        return false;
    }
    std::string hs = std::string("{\"v\":1,\"client_id\":\"") +
                     jsonEscape(clientId) + "\"}";
    g_handshook = sendFrame(0, hs);
    if (!g_handshook) g_backoffUntil = now + 30000.0;
    return g_handshook;
}

} // namespace

namespace discordrpc {

void update(const char* clientId, const char* details, const char* state) {
    if (!clientId || !clientId[0]) return;
    if (!connectAndHandshake(clientId)) return;
    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{"
        "\"details\":\"%s\",\"state\":\"%s\"}},\"nonce\":\"%lu\"}",
        (unsigned long)GetCurrentProcessId(),
        jsonEscape(details).c_str(), jsonEscape(state).c_str(),
        (unsigned long)GetTickCount64());
    sendFrame(1, payload);
}

void shutdown() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    g_handshook = false;
}

} // namespace discordrpc

#else
namespace discordrpc {
void update(const char*, const char*, const char*) {}
void shutdown() {}
}
#endif
