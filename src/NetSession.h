#pragma once
// ─── NetSession.h ──────────────────────────────────────────────────────────
//
// Real LAN multiplayer: TCP socket session with a dedicated worker thread.
// The UI thread enqueues outgoing messages via send() and drains incoming
// messages via receive() — all socket I/O happens off-thread so the
// render loop never blocks on accept/recv/connect.
//
// Wire format (every message is exactly):
//   [4 bytes magic 0xEDOPMAGIC][4 bytes protocol version]
//   [4 bytes type][4 bytes payload length][N bytes payload]
// All integers little-endian. Payload bytes are message-specific (see the
// enum below).
//
// Threading model:
//   * `host()` / `joinHost()` spawn a worker. Calls return immediately.
//   * UI calls `poll()` once per frame; messages drained via receive().
//   * UI calls `send(...)` to enqueue an outgoing message; the worker
//     drains the outbox and writes to the socket.
//   * `disconnect()` flips an atomic stop flag, joins the worker, and
//     closes sockets.
//
// All shared state (queues, peer info, state enum) is protected by a
// single mutex. The worker uses blocking sockets — disconnect detection
// is via recv() returning 0 or error.
//
#include <string>
#include <vector>
#include <deque>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <thread>

namespace edo {

constexpr uint32_t kNetProtocolVersion = 1;
constexpr uint32_t kNetMagic           = 0x45444F50u; // 'EDOP'

enum class NetMode {
    Offline = 0,
    Host,
    Client
};

enum class NetState {
    Disconnected = 0,
    Listening,       // host: socket open, waiting for client
    Connecting,      // client: TCP connect in progress
    Handshaking,     // exchanging Hello + protocol version
    Connected,       // post-handshake; deck exchange / ready
    InDuel,          // duel running; responses streaming
    Error            // see lastError
};

enum class NetMsgType : uint32_t {
    Hello          = 1,
    DeckInfo       = 2,
    Ready          = 3,
    StartDuel      = 4,
    EngineResponse = 5,
    Chat           = 6,
    Disconnect     = 7,
    Ping           = 8,
    Pong           = 9,
    Error          = 10,
    // Prompt-state handshake — used ONLY by the legacy dual-engine
    // lockstep path (gated behind m_mpHostAuth=false / experimental
    // mode). The default multiplayer mode is host-authoritative and
    // does not send these.
    PromptState    = 11,

    // ── Host-authoritative protocol (the new default) ────────────────
    // Host owns the only authoritative ocgcore. Client renders the
    // duel from snapshots and sends back ClientChoice when host
    // declares that a prompt is owned by the client. Schemas + parse
    // helpers live in NetSnapshots.h.
    FieldSnapshot   = 12, // host → client: sanitised board state per recipient
    PromptSnapshot  = 13, // host → client: "engine is awaiting client choice"
    ClientChoice    = 14, // client → host: chosen option for a PromptSnapshot
    GameEvent       = 15, // host → client: optional fx hint (summon/attack/…)
    SyncError       = 16  // either direction: hard-fault notice, pause the duel
};

struct NetMessage {
    NetMsgType           type{NetMsgType::Hello};
    std::vector<uint8_t> payload;
};

struct NetPeer {
    std::string displayName;
    std::string addr;           // "host:port" string
};

// Packet counters surfaced in the UI / diagnostics.
struct NetStats {
    uint64_t messagesSent     = 0;
    uint64_t messagesReceived = 0;
    uint64_t bytesSent        = 0;
    uint64_t bytesReceived    = 0;
};

class NetSession {
public:
    NetSession();
    ~NetSession();

    // No copies — the worker thread + sockets are non-copyable.
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────
    bool host(int port, const std::string& displayName);
    bool joinHost(const std::string& addr, int port,
                  const std::string& displayName);
    void disconnect(const std::string& reason = "user requested");

    // Lightweight per-frame pump — promotes the worker's state changes
    // to the UI thread (currently just returns whether any messages are
    // available). The worker pushes directly into the inbox.
    bool poll();

    // ── Send / receive ────────────────────────────────────────────────
    void send(const NetMessage& m);
    bool receive(NetMessage& out);     // pop one message; false if empty
    size_t inboxSize()  const;
    size_t outboxSize() const;

    // ── Inspection ────────────────────────────────────────────────────
    NetMode   mode()  const { return m_mode.load(); }
    NetState  state() const { return m_state.load(); }
    bool      isOffline()   const { return mode() == NetMode::Offline; }
    bool      isHost()      const { return mode() == NetMode::Host; }
    bool      isClient()    const { return mode() == NetMode::Client; }
    int       localPlayerIndex() const {
        if (mode() == NetMode::Host)   return 0;
        if (mode() == NetMode::Client) return 1;
        return 0;
    }
    int       remotePlayerIndex() const { return localPlayerIndex() ^ 1; }

    NetPeer       peer()       const;
    std::string   lastError()  const;
    NetStats      stats()      const;
    std::string   displayName()const;

    // Override peer name (used after Hello is parsed).
    void setPeer(const NetPeer& p);
    void setPeerName(const std::string& name);

private:
    // Worker entry points.
    void runHostWorker(int port);
    void runClientWorker(std::string addr, int port);
    void recvLoop();                   // shared inner loop once connected
    void writeOutbox();                // flush pending outgoing messages
    void closeSocket();
    void setError(const std::string& msg);

    // Wire helpers.
    bool sendRaw(const void* data, size_t len);   // worker-only
    bool readExact(void* data, size_t len);       // worker-only
    bool readMessage(NetMessage& out);            // worker-only
    bool writeMessage(const NetMessage& m);       // worker-only

    // ── State ─────────────────────────────────────────────────────────
    std::atomic<NetMode>   m_mode{NetMode::Offline};
    std::atomic<NetState>  m_state{NetState::Disconnected};
    std::atomic<bool>      m_stop{false};

    // Sockets — held as int / SOCKET behind a void* for header isolation.
    // Real type lives in the .cpp.
    intptr_t               m_listenSock = -1;   // host only
    intptr_t               m_clientSock = -1;   // active peer socket

    mutable std::mutex     m_mtx;
    std::deque<NetMessage> m_inbox;
    std::deque<NetMessage> m_outbox;

    NetPeer                m_peer;
    NetStats               m_stats;
    std::string            m_displayName;
    std::string            m_lastError;

    std::thread            m_worker;
};

// ── Wire-format helpers — small endian-safe encoders ──────────────────
inline void putU8 (std::vector<uint8_t>& b, uint8_t  v) { b.push_back(v); }
inline void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v       & 0xff));
    b.push_back((uint8_t)((v >>  8) & 0xff));
    b.push_back((uint8_t)((v >> 16) & 0xff));
    b.push_back((uint8_t)((v >> 24) & 0xff));
}
inline void putU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back((uint8_t)((v >> (i * 8)) & 0xff));
}
inline void putStr(std::vector<uint8_t>& b, const std::string& s) {
    putU32(b, (uint32_t)s.size());
    b.insert(b.end(), s.begin(), s.end());
}
inline void putBytes(std::vector<uint8_t>& b,
                     const std::vector<uint8_t>& src) {
    putU32(b, (uint32_t)src.size());
    b.insert(b.end(), src.begin(), src.end());
}

// Reader cursor — tracks position into a payload buffer with bounds
// checking. Returns default values + sets `ok=false` on overflow.
struct NetReader {
    const uint8_t* p;
    size_t left;
    bool   ok;
    NetReader(const std::vector<uint8_t>& b)
        : p(b.data()), left(b.size()), ok(true) {}
    uint8_t u8() {
        if (left < 1) { ok = false; return 0; }
        uint8_t v = p[0]; ++p; --left; return v;
    }
    uint32_t u32() {
        if (left < 4) { ok = false; return 0; }
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4; left -= 4; return v;
    }
    uint64_t u64() {
        if (left < 8) { ok = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
        p += 8; left -= 8; return v;
    }
    std::string str() {
        uint32_t n = u32();
        if (!ok || left < n) { ok = false; return {}; }
        std::string s((const char*)p, n);
        p += n; left -= n; return s;
    }
    std::vector<uint8_t> bytes() {
        uint32_t n = u32();
        if (!ok || left < n) { ok = false; return {}; }
        std::vector<uint8_t> v(p, p + n);
        p += n; left -= n; return v;
    }
    std::vector<uint32_t> u32arr() {
        uint32_t n = u32();
        if (!ok || left < (size_t)n * 4) { ok = false; return {}; }
        std::vector<uint32_t> v;
        v.reserve(n);
        for (uint32_t i = 0; i < n; ++i) v.push_back(u32());
        return v;
    }
};

} // namespace edo
