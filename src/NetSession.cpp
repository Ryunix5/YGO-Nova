// ─── NetSession.cpp ────────────────────────────────────────────────────────
// Real LAN socket implementation. See NetSession.h for design.
//
// Worker thread model:
//   * On host(), a thread runs runHostWorker() which:
//       1. socket() + bind() + listen()
//       2. accept() (blocks)
//       3. enters recvLoop()
//   * On joinHost(), a thread runs runClientWorker() which:
//       1. socket() + connect() (blocks)
//       2. enters recvLoop()
//   * recvLoop() reads full messages, enqueues them into m_inbox, and
//     between reads checks m_outbox and flushes pending sends. The loop
//     exits when recv() returns 0 / error, or m_stop is set, or the
//     socket is closed by another thread.
//
#include "NetSession.h"
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  using socket_t = SOCKET;
  static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
  static int  netLastError()        { return WSAGetLastError(); }
  static void closeSock(socket_t s) { if (s != kInvalidSocket) closesocket(s); }
  static int  recvN(socket_t s, void* b, int n) {
      return ::recv(s, (char*)b, n, 0);
  }
  static int  sendN(socket_t s, const void* b, int n) {
      return ::send(s, (const char*)b, n, 0);
  }
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  using socket_t = int;
  static constexpr socket_t kInvalidSocket = -1;
  static int  netLastError()        { return errno; }
  static void closeSock(socket_t s) { if (s != kInvalidSocket) ::close(s); }
  static int  recvN(socket_t s, void* b, int n) {
      return (int)::recv(s, b, n, 0);
  }
  static int  sendN(socket_t s, const void* b, int n) {
      return (int)::send(s, b, n, 0);
  }
#endif

namespace edo {

// Per-process WinSock init/teardown. NetSession instances are typically
// long-lived; we ref-count so we can clean up properly if multiple
// sessions ever live in the same process.
static std::atomic<int> g_wsaRefs{0};

static bool wsaStartupOnce() {
#ifdef _WIN32
    if (g_wsaRefs.fetch_add(1) == 0) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
            g_wsaRefs.fetch_sub(1);
            return false;
        }
    }
#else
    (void)g_wsaRefs;
#endif
    return true;
}
static void wsaCleanupOnce() {
#ifdef _WIN32
    if (g_wsaRefs.fetch_sub(1) == 1) WSACleanup();
#endif
}

// ── ctor / dtor ──────────────────────────────────────────────────────────────
NetSession::NetSession()  { wsaStartupOnce(); }
NetSession::~NetSession() { disconnect("session destroyed"); wsaCleanupOnce(); }

// ── State / accessor helpers ─────────────────────────────────────────────────
NetPeer     NetSession::peer()        const {
    std::lock_guard<std::mutex> lk(m_mtx); return m_peer;
}
std::string NetSession::lastError()   const {
    std::lock_guard<std::mutex> lk(m_mtx); return m_lastError;
}
NetStats    NetSession::stats()       const {
    std::lock_guard<std::mutex> lk(m_mtx); return m_stats;
}
std::string NetSession::displayName() const {
    std::lock_guard<std::mutex> lk(m_mtx); return m_displayName;
}
size_t      NetSession::inboxSize()  const {
    std::lock_guard<std::mutex> lk(m_mtx); return m_inbox.size();
}
size_t      NetSession::outboxSize() const {
    std::lock_guard<std::mutex> lk(m_mtx); return m_outbox.size();
}
void NetSession::setPeer(const NetPeer& p) {
    std::lock_guard<std::mutex> lk(m_mtx); m_peer = p;
}
void NetSession::setPeerName(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_mtx); m_peer.displayName = name;
}
void NetSession::setError(const std::string& msg) {
    std::lock_guard<std::mutex> lk(m_mtx); m_lastError = msg;
}

// ── Public lifecycle ─────────────────────────────────────────────────────────
bool NetSession::host(int port, const std::string& displayName) {
    if (mode() != NetMode::Offline) return false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_displayName = displayName;
        m_lastError.clear();
        m_inbox.clear();
        m_outbox.clear();
        m_stats = NetStats{};
        m_peer  = NetPeer{};
    }
    m_mode  = NetMode::Host;
    m_state = NetState::Listening;
    m_stop  = false;
    m_worker = std::thread(&NetSession::runHostWorker, this, port);
    return true;
}

bool NetSession::joinHost(const std::string& addr, int port,
                          const std::string& displayName) {
    if (mode() != NetMode::Offline) return false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_displayName = displayName;
        m_lastError.clear();
        m_inbox.clear();
        m_outbox.clear();
        m_stats = NetStats{};
        m_peer  = NetPeer{};
        m_peer.addr = addr + ":" + std::to_string(port);
    }
    m_mode  = NetMode::Client;
    m_state = NetState::Connecting;
    m_stop  = false;
    m_worker = std::thread(&NetSession::runClientWorker, this, addr, port);
    return true;
}

bool NetSession::joinRelay(const std::string& serverAddr, int port,
                           const std::string& displayName,
                           bool createRoom, const std::string& roomCode) {
    if (mode() != NetMode::Offline) return false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_displayName = displayName;
        m_lastError.clear();
        m_inbox.clear();
        m_outbox.clear();
        m_stats = NetStats{};
        m_peer  = NetPeer{};
        m_peer.addr = serverAddr + ":" + std::to_string(port) +
                      (createRoom ? "  (relay host)" : "  (relay guest)");
    }
    // The room creator behaves as the authoritative HOST; the joiner as the
    // CLIENT. This makes localPlayerIndex() / host-auth routing identical to
    // LAN — only the transport differs.
    m_mode  = createRoom ? NetMode::Host : NetMode::Client;
    m_state = NetState::Connecting;
    m_stop  = false;
    m_worker = std::thread(&NetSession::runRelayWorker, this,
                           serverAddr, port, createRoom, roomCode);
    return true;
}

void NetSession::disconnect(const std::string& reason) {
    if (mode() == NetMode::Offline && !m_worker.joinable()) return;
    // Try to send a polite disconnect packet first — best effort.
    if (state() == NetState::Connected || state() == NetState::InDuel) {
        NetMessage m; m.type = NetMsgType::Disconnect;
        putStr(m.payload, reason);
        // Lock briefly to push to outbox; the worker will flush.
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_outbox.push_back(m);
        }
        // Give the worker ~50ms to flush; this is best-effort.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    m_stop = true;
    closeSocket();
    if (m_worker.joinable()) m_worker.join();
    m_mode  = NetMode::Offline;
    m_state = NetState::Disconnected;
    if (!reason.empty()) setError(reason);
}

bool NetSession::poll() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return !m_inbox.empty();
}

bool NetSession::receive(NetMessage& out) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_inbox.empty()) return false;
    out = std::move(m_inbox.front());
    m_inbox.pop_front();
    return true;
}

void NetSession::send(const NetMessage& m) {
    if (mode() == NetMode::Offline) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    m_outbox.push_back(m);
}

// ── Worker: socket helpers ───────────────────────────────────────────────────
bool NetSession::sendRaw(const void* data, size_t len) {
    auto sock = (socket_t)m_clientSock;
    const char* p = (const char*)data;
    size_t left = len;
    while (left > 0 && !m_stop) {
        int n = sendN(sock, p, (int)left);
        if (n <= 0) return false;
        p    += n;
        left -= n;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stats.bytesSent += (uint64_t)n;
        }
    }
    return left == 0;
}

bool NetSession::readExact(void* data, size_t len) {
    auto sock = (socket_t)m_clientSock;
    char* p = (char*)data;
    size_t left = len;
    while (left > 0 && !m_stop) {
        int n = recvN(sock, p, (int)left);
        if (n <= 0) return false;
        p    += n;
        left -= n;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stats.bytesReceived += (uint64_t)n;
        }
    }
    return left == 0;
}

bool NetSession::readMessage(NetMessage& out) {
    uint8_t hdr[16];
    if (!readExact(hdr, sizeof(hdr))) return false;
    uint32_t magic =
        (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
        ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    uint32_t version =
        (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
        ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    uint32_t type =
        (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
        ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    uint32_t plen =
        (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) |
        ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
    if (magic != kNetMagic) {
        setError("bad packet magic");
        return false;
    }
    if (version != kNetProtocolVersion) {
        setError("protocol version mismatch (got " +
                 std::to_string(version) + ")");
        return false;
    }
    // Cap payload so malicious / corrupted streams can't OOM us. 16 MiB
    // is far more than any real EngineResponse will use.
    if (plen > 16u * 1024u * 1024u) {
        setError("packet payload too large (" +
                 std::to_string(plen) + " bytes)");
        return false;
    }
    out.type = (NetMsgType)type;
    out.payload.resize(plen);
    if (plen > 0 && !readExact(out.payload.data(), plen)) return false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        ++m_stats.messagesReceived;
    }
    return true;
}

bool NetSession::writeMessage(const NetMessage& m) {
    uint8_t hdr[16];
    auto put32 = [&](size_t off, uint32_t v) {
        hdr[off    ] = (uint8_t)( v        & 0xff);
        hdr[off + 1] = (uint8_t)((v >>  8) & 0xff);
        hdr[off + 2] = (uint8_t)((v >> 16) & 0xff);
        hdr[off + 3] = (uint8_t)((v >> 24) & 0xff);
    };
    put32(0,  kNetMagic);
    put32(4,  kNetProtocolVersion);
    put32(8,  (uint32_t)m.type);
    put32(12, (uint32_t)m.payload.size());
    if (!sendRaw(hdr, sizeof(hdr))) return false;
    if (!m.payload.empty() && !sendRaw(m.payload.data(), m.payload.size()))
        return false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        ++m_stats.messagesSent;
    }
    return true;
}

void NetSession::writeOutbox() {
    // Pop one at a time so we don't hold the lock through sendN.
    for (;;) {
        NetMessage m;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_outbox.empty()) return;
            m = std::move(m_outbox.front());
            m_outbox.pop_front();
        }
        if (!writeMessage(m)) {
            setError("socket write failed");
            m_stop = true;
            return;
        }
    }
}

void NetSession::closeSocket() {
    closeSock((socket_t)m_listenSock); m_listenSock = (intptr_t)kInvalidSocket;
    closeSock((socket_t)m_clientSock); m_clientSock = (intptr_t)kInvalidSocket;
}

// ── Worker: shared inner recv/send loop ──────────────────────────────────────
void NetSession::recvLoop() {
    auto sock = (socket_t)m_clientSock;
    // Set a short timeout so we can periodically flush the outbox and
    // notice m_stop. SO_RCVTIMEO is portable enough for our needs.
#ifdef _WIN32
    DWORD to = 50;   // 50ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
#else
    struct timeval to{0, 50 * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
#endif
    while (!m_stop) {
        // Try to read one message; readExact / recvN may time out.
        uint8_t hdr[16];
        int got = recvN(sock, (char*)hdr, sizeof(hdr));
        if (got == 0) {
            setError("peer closed connection");
            break;
        }
        if (got < 0) {
            // Timeout? On most platforms recv with SO_RCVTIMEO returns
            // -1 with errno=EAGAIN/EWOULDBLOCK or WSAETIMEDOUT. We just
            // fall through, flush the outbox, and loop.
            writeOutbox();
            continue;
        }
        // We got SOME of the header — read the rest.
        if (got < (int)sizeof(hdr)) {
            if (!readExact(hdr + got, sizeof(hdr) - got)) {
                setError("partial header read");
                break;
            }
        } else {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stats.bytesReceived += (uint64_t)got;
        }
        uint32_t magic =
            (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
            ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        uint32_t version =
            (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
            ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
        uint32_t type =
            (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
            ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
        uint32_t plen =
            (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) |
            ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
        if (magic != kNetMagic) {
            setError("bad packet magic");
            break;
        }
        if (version != kNetProtocolVersion) {
            setError("protocol version mismatch (got " +
                     std::to_string(version) + ")");
            break;
        }
        if (plen > 16u * 1024u * 1024u) {
            setError("payload too large (" + std::to_string(plen) + ")");
            break;
        }
        NetMessage msg;
        msg.type = (NetMsgType)type;
        msg.payload.resize(plen);
        if (plen > 0 && !readExact(msg.payload.data(), plen)) {
            setError("payload read failed");
            break;
        }
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            ++m_stats.messagesReceived;
            m_inbox.push_back(std::move(msg));
        }
        // Flush any outgoing messages between reads.
        writeOutbox();
    }
    closeSocket();
    if (state() != NetState::Disconnected) m_state = NetState::Disconnected;
}

// ── Worker: host ─────────────────────────────────────────────────────────────
void NetSession::runHostWorker(int port) {
    socket_t srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv == kInvalidSocket) {
        setError("socket() failed (err " + std::to_string(netLastError()) + ")");
        m_state = NetState::Error;
        return;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0) {
        setError("bind failed (err " + std::to_string(netLastError()) + ")");
        closeSock(srv);
        m_state = NetState::Error;
        return;
    }
    if (::listen(srv, 1) != 0) {
        setError("listen failed");
        closeSock(srv);
        m_state = NetState::Error;
        return;
    }
    m_listenSock = (intptr_t)srv;
    // Loop until a client connects or we're asked to stop. Use a small
    // SO_RCVTIMEO via select() so accept() doesn't block forever; this
    // lets disconnect() actually stop us.
    while (!m_stop) {
        fd_set fds; FD_ZERO(&fds); FD_SET(srv, &fds);
        struct timeval tv{0, 200 * 1000};  // 200ms
        int r = ::select((int)srv + 1, &fds, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(srv, &fds)) {
            sockaddr_in cli{}; socklen_t clen = sizeof(cli);
            socket_t c = ::accept(srv, (sockaddr*)&cli, &clen);
            if (c == kInvalidSocket) {
                setError("accept failed");
                continue;
            }
            m_clientSock = (intptr_t)c;
            // Pretty-print peer address.
            char ip[64] = "?";
#ifdef _WIN32
            InetNtopA(AF_INET, &cli.sin_addr, ip, sizeof(ip));
#else
            inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
#endif
            std::string addrStr = std::string(ip) + ":" +
                                  std::to_string(ntohs(cli.sin_port));
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_peer.addr = addrStr;
            }
            m_state = NetState::Handshaking;
            // Send our Hello immediately so the client knows our name.
            NetMessage h; h.type = NetMsgType::Hello;
            putU32(h.payload, kNetProtocolVersion);
            std::string nm; { std::lock_guard<std::mutex> lk(m_mtx); nm = m_displayName; }
            putStr(h.payload, nm);
            writeMessage(h);
            m_state = NetState::Connected;
            // Inner recv loop runs until peer disconnects or m_stop.
            recvLoop();
            break;
        }
    }
    closeSocket();
    if (!m_stop && state() != NetState::Error)
        m_state = NetState::Disconnected;
}

// ── Worker: client ───────────────────────────────────────────────────────────
void NetSession::runClientWorker(std::string addr, int port) {
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) {
        setError("socket() failed");
        m_state = NetState::Error;
        return;
    }
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)port);
#ifdef _WIN32
    if (InetPtonA(AF_INET, addr.c_str(), &dst.sin_addr) != 1) {
        setError("invalid host address: " + addr);
        closeSock(s);
        m_state = NetState::Error;
        return;
    }
#else
    if (inet_pton(AF_INET, addr.c_str(), &dst.sin_addr) != 1) {
        setError("invalid host address: " + addr);
        closeSock(s);
        m_state = NetState::Error;
        return;
    }
#endif
    if (::connect(s, (sockaddr*)&dst, sizeof(dst)) != 0) {
        setError("connect failed (err " +
                 std::to_string(netLastError()) + ")");
        closeSock(s);
        m_state = NetState::Error;
        return;
    }
    m_clientSock = (intptr_t)s;
    m_state = NetState::Handshaking;
    // Send our Hello.
    NetMessage h; h.type = NetMsgType::Hello;
    putU32(h.payload, kNetProtocolVersion);
    std::string nm; { std::lock_guard<std::mutex> lk(m_mtx); nm = m_displayName; }
    putStr(h.payload, nm);
    writeMessage(h);
    m_state = NetState::Connected;
    recvLoop();
    closeSocket();
    if (!m_stop && state() != NetState::Error)
        m_state = NetState::Disconnected;
}

// ── Worker: relay (online) ─────────────────────────────────────────────────
// Connects out to the relay server exactly like the LAN client, but instead
// of auto-sending Hello it sends the room-control message (CreateRoom /
// JoinRoom). The room handshake responses (RoomCreated / RoomJoined /
// RoomPeerJoined / RoomError) arrive through the normal inbox and are handled
// by the UI, which then drives the existing Hello/deck/ready handshake over
// the relayed channel.
void NetSession::runRelayWorker(std::string addr, int port, bool createRoom,
                                std::string roomCode) {
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) {
        setError("socket() failed");
        m_state = NetState::Error;
        return;
    }
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)port);
#ifdef _WIN32
    if (InetPtonA(AF_INET, addr.c_str(), &dst.sin_addr) != 1) {
        setError("invalid relay address: " + addr);
        closeSock(s); m_state = NetState::Error; return;
    }
#else
    if (inet_pton(AF_INET, addr.c_str(), &dst.sin_addr) != 1) {
        setError("invalid relay address: " + addr);
        closeSock(s); m_state = NetState::Error; return;
    }
#endif
    if (::connect(s, (sockaddr*)&dst, sizeof(dst)) != 0) {
        setError("relay connect failed (err " +
                 std::to_string(netLastError()) + ")");
        closeSock(s); m_state = NetState::Error; return;
    }
    m_clientSock = (intptr_t)s;
    m_state = NetState::Handshaking;
    // Room-control message. Name + (for join) room code. The server replies
    // RoomCreated / RoomJoined / RoomError, which the UI handles.
    std::string nm; { std::lock_guard<std::mutex> lk(m_mtx); nm = m_displayName; }
    NetMessage ctl;
    if (createRoom) {
        ctl.type = NetMsgType::CreateRoom;
        putStr(ctl.payload, nm);
    } else {
        ctl.type = NetMsgType::JoinRoom;
        putStr(ctl.payload, nm);
        putStr(ctl.payload, roomCode);
    }
    writeMessage(ctl);
    // "Connected" = the relay socket is up. Room formation + the gameplay
    // handshake proceed through the inbox; the UI tracks room state.
    m_state = NetState::Connected;
    recvLoop();
    closeSocket();
    if (!m_stop && state() != NetState::Error)
        m_state = NetState::Disconnected;
}

} // namespace edo
