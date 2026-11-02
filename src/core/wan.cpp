#include "wan.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <nlohmann/json.hpp>

#include "ikcp.h"
#include "monocypher.h"
#include "platform.h"
#include "upnp.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define LS_POLL WSAPoll
typedef WSAPOLLFD ls_pollfd;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define LS_POLL poll
typedef pollfd ls_pollfd;
#endif

namespace ls {

using json = nlohmann::json;
using sock_t = decltype(socket(0, 0, 0));

std::string RoomHash(const std::string& room, const std::string& password) {
    std::string msg = "LunaShare room v1\n" + room + "\n" + password;
    uint8_t h[32];
    crypto_blake2b(h, sizeof(h), (const uint8_t*)msg.data(), msg.size());
    static const char* hx = "0123456789abcdef";
    std::string s(64, '0');
    for (int i = 0; i < 32; i++) { s[i * 2] = hx[h[i] >> 4]; s[i * 2 + 1] = hx[h[i] & 15]; }
    return s;
}

bool ParseEndpoint(const std::string& s, std::string& host, uint16_t& port) {
    size_t c = s.rfind(':');
    if (c == std::string::npos) {
        host = s;
        return !host.empty();
    }
    host = s.substr(0, c);
    int p = atoi(s.c_str() + c + 1);
    if (p <= 0 || p > 65535) return false;
    port = (uint16_t)p;
    return !host.empty();
}

static std::string EpStr(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

static bool EpParse(const std::string& s, sockaddr_in& a) {
    std::string h;
    uint16_t p = 0;
    if (!ParseEndpoint(s, h, p) || !p) return false;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(p);
    return inet_pton(AF_INET, h.c_str(), &a.sin_addr) == 1;
}

static bool SameEp(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

static void SetNonBlock(SocketHandle s) {
#ifdef _WIN32
    u_long m = 1;
    ioctlsocket((SOCKET)s, FIONBIO, &m);
#else
    fcntl((int)s, F_SETFL, fcntl((int)s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static bool WouldBlock() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void WanTransport::SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(m_);
    status_ = s;
}

std::string WanTransport::Status() {
    std::lock_guard<std::mutex> l(m_);
    return status_;
}

std::string WanTransport::MappedEndpoint() {
    std::lock_guard<std::mutex> l(m_);
    return mappedEp_;
}

std::string WanTransport::PublicEndpoint() {
    std::lock_guard<std::mutex> l(m_);
    return publicEp_;
}

bool WanTransport::Start(const Config& cfg, OnPeers onPeers, OnSocket onSocket) {
    Stop();
    {
        std::lock_guard<std::mutex> l(m_);
        cfg_ = cfg;
        status_ = "starting";
        publicEp_.clear();
        connectQueue_.clear();
    }
    onPeers_ = std::move(onPeers);
    onSocket_ = std::move(onSocket);
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void WanTransport::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void WanTransport::Connect(const std::string& peerId) {
    std::lock_guard<std::mutex> l(m_);
    if (std::find(connectQueue_.begin(), connectQueue_.end(), peerId) == connectQueue_.end()) connectQueue_.push_back(peerId);
}

void WanTransport::UpdateName(const std::string& name) {
    std::lock_guard<std::mutex> l(m_);
    if (cfg_.name != name) {
        cfg_.name = name;
        nameChanged_ = true;
    }
}

namespace {

// KCP segments (~1.2 KB) allowed to wait for the network per session. Small, so a slow path shows up as a full
// socket / engine queue right away (calls adapt to that) instead of seconds of hidden delay. ~600 KB.
constexpr int kMaxKcpQueue = 512;

struct Session {
    uint32_t conv = 0;
    ikcpcb* kcp = nullptr;
    sock_t udp;
    sockaddr_in peer{};
    SocketHandle local = kInvalidSocket;  // our end of the socket pair
    std::string toLocal;                   // received from KCP, not yet written to the engine
    int64_t lastRecv = 0;
    std::string peerId;
    bool closed = false;
    bool relayed = false;  // peer is the server, packets are 'Y'
};

int KcpOutput(const char* buf, int len, ikcpcb*, void* user) {
    Session* s = (Session*)user;
    char pkt[2048];
    if (len + 1 > (int)sizeof(pkt)) return -1;
    pkt[0] = s->relayed ? 'Y' : 'K';
    memcpy(pkt + 1, buf, (size_t)len);
    sendto(s->udp, pkt, len + 1, 0, (sockaddr*)&s->peer, sizeof(s->peer));
    return 0;
}

struct Punch {
    std::string nonce, peerId;
    bool initiator = false;
    std::vector<sockaddr_in> candidates;
    int64_t started = 0, lastSend = 0;
    bool done = false, relayAsked = false;
};

uint32_t ConvFromNonce(const std::string& n) {
    uint8_t h[4];
    crypto_blake2b(h, 4, (const uint8_t*)n.data(), n.size());
    uint32_t c = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
    return c ? c : 1;
}

}  // namespace

void WanTransport::Loop() {
    NetInit();
    sock_t udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    any.sin_port = htons(cfg_.localPort);
    if (bind(udp, (sockaddr*)&any, sizeof(any)) != 0 && cfg_.localPort) {
        SetStatus("UDP port " + std::to_string(cfg_.localPort) + " is busy, using a random one");
        any.sin_port = 0;
        bind(udp, (sockaddr*)&any, sizeof(any));
    }
    int rcvbuf = 4 << 20;
    setsockopt(udp, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
    setsockopt(udp, SOL_SOCKET, SO_SNDBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
    SetNonBlock((SocketHandle)udp);
    socklen_t al = sizeof(any);
    getsockname(udp, (sockaddr*)&any, &al);
    uint16_t localPort = ntohs(any.sin_port);

    // tests: pretend the NATs can't be punched (probes ignored), so the relay is used
    const bool noDirect = getenv("LUNASHARE_TEST_NO_DIRECT") != nullptr;
    std::map<uint32_t, std::unique_ptr<Session>> sessions;
    std::map<std::string, Punch> punches;
    sockaddr_in server{};
    bool serverOk = false;
    int64_t lastResolve = -100000, lastReg = -100000, lastReply = 0;
    std::vector<char> buf(65536);
    // NAT-PMP (RFC 6886): ask the gateway to map our UDP port so peers can reach us even behind NATs that
    // randomise ports per destination (pf/OPNsense, many enterprise firewalls) where plain hole punching fails.
    std::vector<sockaddr_in> gateways;
    sockaddr_in natGw{};
    bool natGwKnown = false;
    std::string natExtIp;
    uint16_t natExtPort = 0;
    int64_t natNext = 0, natLifetimeMs = 0, natGwRefresh = -1000000;
    auto natRequest = [&](const sockaddr_in& gw, uint8_t op, uint32_t lifetime) {
        uint8_t req[12] = {0, op};
        int len = 2;
        if (op == 1) {
            req[4] = (uint8_t)(localPort >> 8); req[5] = (uint8_t)localPort;  // internal port
            req[6] = (uint8_t)(localPort >> 8); req[7] = (uint8_t)localPort;  // suggested external port
            req[8] = (uint8_t)(lifetime >> 24); req[9] = (uint8_t)(lifetime >> 16); req[10] = (uint8_t)(lifetime >> 8); req[11] = (uint8_t)lifetime;
            len = 12;
        }
        sendto(udp, (const char*)req, len, 0, (const sockaddr*)&gw, sizeof(gw));
    };

    // UPnP IGD as well (more home routers speak it than NAT-PMP); runs on its own thread
    UpnpMapper upnp;
    std::string upnpEp;
    int64_t upnpCheck = 0;
    if (cfg_.natpmp) upnp.Start(localPort);

    auto send = [&](const sockaddr_in& to, char type, const std::string& body) {
        std::string pkt(1, type);
        pkt += body;
        sendto(udp, pkt.data(), (int)pkt.size(), 0, (const sockaddr*)&to, sizeof(to));
    };
    auto resolve = [&] {
        std::string hostPort;
        {
            std::lock_guard<std::mutex> l(m_);
            hostPort = cfg_.server;
        }
        std::string host;
        uint16_t port = kRendezvousPort;
        if (!ParseEndpoint(hostPort, host, port)) { SetStatus("invalid server address"); return; }
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            SetStatus("cannot resolve " + host);
            serverOk = false;
            return;
        }
        server = *(sockaddr_in*)res->ai_addr;
        server.sin_port = htons(port);
        freeaddrinfo(res);
        serverOk = true;
    };
    auto registerNow = [&] {
        if (!serverOk) return;
        json locals = json::array();
        for (auto& a : LocalIPv4Addresses()) locals.push_back(a.ip + ":" + std::to_string(localPort));
        json extra = json::array();
        if (!natExtIp.empty() && natExtPort) extra.push_back(natExtIp + ":" + std::to_string(natExtPort));
        if (!upnpEp.empty() && (extra.empty() || extra[0] != upnpEp)) extra.push_back(upnpEp);
        json j;
        {
            std::lock_guard<std::mutex> l(m_);
            j = {{"t", "reg"}, {"v", 1}, {"room", cfg_.roomHash}, {"id", cfg_.id}, {"name", cfg_.name}, {"pf", cfg_.platform},
                 {"pub", cfg_.pub}, {"local", locals}, {"extra", extra}, {"fwd", cfg_.forwardedPort}, {"relay", cfg_.relay}};
            nameChanged_ = false;
        }
        send(server, 'R', j.dump());
    };
    auto closeSession = [&](uint32_t conv, bool notify) {
        auto it = sessions.find(conv);
        if (it == sessions.end()) return;
        Session* s = it->second.get();
        if (notify) {
            char x[5] = {'X'};
            memcpy(x + 1, &conv, 4);
            for (int i = 0; i < 3; i++) sendto(udp, x, 5, 0, (sockaddr*)&s->peer, sizeof(s->peer));
        }
        ikcp_release(s->kcp);
        CloseSocket(s->local);
        sessions.erase(it);
        sessions_ = (int)sessions.size();
    };
    auto openSession = [&](uint32_t conv, const sockaddr_in& peer, const std::string& peerId, bool initiator, bool relayed) {
        if (sessions.count(conv)) return;
        SocketHandle pair[2];
        if (!LocalSocketPair(pair)) return;
        auto s = std::make_unique<Session>();
        s->conv = conv;
        s->udp = udp;
        s->peer = peer;
        s->local = pair[0];
        s->peerId = peerId;
        s->lastRecv = SteadyMs();
        s->relayed = relayed;
        SetNonBlock(s->local);
        s->kcp = ikcp_create(conv, s.get());
        s->kcp->output = KcpOutput;
        s->kcp->stream = 1;
        ikcp_setmtu(s->kcp, 1200);
        ikcp_nodelay(s->kcp, 1, 10, 2, 1);
        ikcp_wndsize(s->kcp, 1024, 1024);
        sessions[conv] = std::move(s);
        sessions_ = (int)sessions.size();
        if (onSocket_) onSocket_(pair[1], peerId, relayed ? "relay" : EpStr(peer), initiator);
    };

    SetStatus("connecting to server");
    while (running_) {
        int64_t now = SteadyMs();
        if (now - lastResolve > 120000 || (!serverOk && now - lastResolve > 10000)) {
            resolve();
            lastResolve = now;
        }
        bool renamed;
        {
            std::lock_guard<std::mutex> l(m_);
            renamed = nameChanged_;
        }
        if (cfg_.natpmp && now - upnpCheck > 2000) {
            upnpCheck = now;
            std::string m = upnp.Mapping();
            if (m != upnpEp) {
                upnpEp = m;
                lastReg = -100000;  // tell the server about the new way in
                std::lock_guard<std::mutex> l(m_);
                if (natExtPort == 0) mappedEp_ = m;
            }
        }
        if (now - lastReg > 15000 || renamed) {
            registerNow();
            lastReg = now;
        }
        if (cfg_.natpmp && now >= natNext) {
            if (now - natGwRefresh > 300000) {
                gateways.clear();
                for (auto& g : DefaultGateways()) {
                    sockaddr_in a{};
                    a.sin_family = AF_INET;
                    a.sin_port = htons(5351);
                    if (inet_pton(AF_INET, g.c_str(), &a.sin_addr) == 1) gateways.push_back(a);
                }
                natGwRefresh = now;
            }
            if (natGwKnown) {
                natRequest(natGw, 0, 0);
                natRequest(natGw, 1, 3600);
            } else {
                for (auto& g : gateways) natRequest(g, 0, 0);  // whoever answers is a NAT-PMP gateway
            }
            natNext = now + (natLifetimeMs ? natLifetimeMs / 2 : (natGwKnown ? 5000 : 60000));
        }
        if (serverOk && lastReply && now - lastReply > 50000) SetStatus("server not responding");
        else if (serverOk && !lastReply && now - lastReg > 5000) SetStatus("server not responding (check address, port and firewall)");
        {
            std::vector<std::string> q;
            {
                std::lock_guard<std::mutex> l(m_);
                q.swap(connectQueue_);
            }
            for (auto& id : q)
                if (serverOk) send(server, 'R', json{{"t", "connect"}, {"to", id}}.dump());
        }
        // punching: probe every candidate every 250 ms for up to 15 s
        for (auto it = punches.begin(); it != punches.end();) {
            Punch& p = it->second;
            if (p.done || now - p.started > 15000) { it = punches.erase(it); continue; }
            // no direct path after a few seconds: ask the server to relay (the dialling side asks; keeps punching
            // meanwhile - whichever comes first is used)
            if (p.initiator && !p.relayAsked && cfg_.relay && serverOk && now - p.started > 5000) {
                p.relayAsked = true;
                send(server, 'R', json{{"t", "relay"}, {"to", p.peerId}, {"nonce", p.nonce}, {"conv", ConvFromNonce(p.nonce)}}.dump());
            }
            if (now - p.lastSend > 250) {
                std::string body = json{{"n", p.nonce}, {"id", cfg_.id}}.dump();
                for (auto& c : p.candidates) send(c, 'P', body);
                p.lastSend = now;
            }
            ++it;
        }

        // ---- wait for UDP or engine data
        std::vector<ls_pollfd> fds(1);
        fds[0].fd = udp;
        fds[0].events = POLLIN;
        std::vector<uint32_t> order;
        for (auto& [conv, s] : sessions) {
            ls_pollfd p{};
            p.fd = (decltype(p.fd))s->local;
            p.events = (short)((ikcp_waitsnd(s->kcp) < kMaxKcpQueue ? POLLIN : 0) | (!s->toLocal.empty() ? POLLOUT : 0));
            fds.push_back(p);
            order.push_back(conv);
        }
        LS_POLL(fds.data(), (unsigned long)fds.size(), sessions.empty() && punches.empty() ? 200 : 5);
        now = SteadyMs();

        // ---- UDP in
        for (int burst = 0; burst < 512; burst++) {
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            int n = (int)recvfrom(udp, buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fl);
            if (n <= 0) break;
            char type = buf[0];
            if (type == 0 && ntohs(from.sin_port) == 5351 && n >= 8) {  // NAT-PMP response
                const uint8_t* r = (const uint8_t*)buf.data();
                uint8_t op = r[1];
                uint16_t result = (uint16_t)(r[2] << 8 | r[3]);
                if (result != 0) continue;
                if (op == 128 && n >= 12) {
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, r + 8, ip, sizeof(ip));
                    natExtIp = ip;
                    if (!natGwKnown) {
                        natGw = from;
                        natGwKnown = true;
                        natRequest(natGw, 1, 3600);
                    }
                } else if (op == 129 && n >= 16) {
                    uint16_t ext = (uint16_t)(r[10] << 8 | r[11]);
                    uint32_t life = (uint32_t)r[12] << 24 | (uint32_t)r[13] << 16 | (uint32_t)r[14] << 8 | r[15];
                    bool changed = ext != natExtPort;
                    natExtPort = ext;
                    natLifetimeMs = (int64_t)life * 1000;
                    natNext = now + natLifetimeMs / 2;
                    {
                        std::lock_guard<std::mutex> l(m_);
                        mappedEp_ = natExtIp.empty() ? "" : natExtIp + ":" + std::to_string(ext);
                    }
                    if (changed) lastReg = -100000;  // re-register with the new candidate right away
                }
                continue;
            }
            if (type == 'R' && serverOk && SameEp(from, server)) {
                json j = json::parse(std::string(buf.data() + 1, (size_t)n - 1), nullptr, false);
                if (j.is_discarded()) continue;
                lastReply = now;
                std::string t = j.value("t", "");
                if (t == "peers") {
                    {
                        std::lock_guard<std::mutex> l(m_);
                        publicEp_ = j.value("you", "");
                    }
                    SetStatus("connected - " + std::to_string(j.value("peers", json::array()).size()) + " other device(s) in the room");
                    std::vector<WanPeer> peers;
                    for (auto& pj : j.value("peers", json::array())) {
                        WanPeer p;
                        p.id = pj.value("id", "");
                        p.name = pj.value("name", "");
                        p.platform = pj.value("pf", "");
                        p.pub = pj.value("pub", "");
                        p.publicEp = pj.value("ep", "");
                        for (auto& l : pj.value("local", json::array()))
                            if (l.is_string()) p.localEps.push_back(l.get<std::string>());
                        for (auto& l : pj.value("extra", json::array()))
                            if (l.is_string()) p.extraEps.push_back(l.get<std::string>());
                        if (!p.id.empty()) peers.push_back(p);
                    }
                    if (onPeers_) onPeers_(peers);
                } else if (t == "punch") {
                    Punch p;
                    p.nonce = j.value("nonce", "");
                    p.initiator = j.value("initiator", false);
                    json pj = j.value("peer", json::object());
                    p.peerId = pj.value("id", "");
                    sockaddr_in a;
                    if (EpParse(pj.value("ep", ""), a)) p.candidates.push_back(a);
                    for (auto& l : pj.value("extra", json::array()))
                        if (l.is_string() && EpParse(l.get<std::string>(), a)) p.candidates.push_back(a);
                    for (auto& l : pj.value("local", json::array()))
                        if (l.is_string() && EpParse(l.get<std::string>(), a)) p.candidates.push_back(a);
                    p.started = now;
                    if (!p.nonce.empty() && !p.peerId.empty() && !sessions.count(ConvFromNonce(p.nonce))) punches[p.nonce] = p;
                } else if (t == "relayed") {
                    std::string nonce = j.value("nonce", "");
                    uint32_t conv = j.value("conv", 0u);
                    std::string peerId = j.value("peer", "");
                    auto it = punches.find(nonce);
                    // only for a connection we're actually setting up with that device
                    if (conv && conv == ConvFromNonce(nonce) && it != punches.end() && it->second.peerId == peerId && !sessions.count(conv)) {
                        it->second.done = true;
                        openSession(conv, server, peerId, j.value("initiator", false), true);
                    }
                } else if (t == "error") {
                    SetStatus("server: " + j.value("msg", std::string("error")));
                }
            } else if (type == 'P' && !noDirect) {
                json j = json::parse(std::string(buf.data() + 1, (size_t)n - 1), nullptr, false);
                if (j.is_discarded()) continue;
                std::string nonce = j.value("n", "");
                if (!j.value("ack", false)) send(from, 'P', json{{"n", nonce}, {"id", cfg_.id}, {"ack", true}}.dump());
                auto it = punches.find(nonce);
                if (it != punches.end() && !it->second.done && j.value("id", "") == it->second.peerId) {
                    it->second.done = true;  // first working path wins
                    openSession(ConvFromNonce(nonce), from, it->second.peerId, it->second.initiator, false);
                }
            } else if (type == 'K' && n > 5) {
                uint32_t conv = ikcp_getconv(buf.data() + 1);
                auto it = sessions.find(conv);
                if (it == sessions.end()) continue;
                Session* s = it->second.get();
                if (s->relayed) continue;
                s->peer = from;  // follow NAT rebinding
                s->lastRecv = now;
                ikcp_input(s->kcp, buf.data() + 1, n - 1);
            } else if (type == 'Y' && n > 5 && serverOk && SameEp(from, server)) {
                uint32_t conv = ikcp_getconv(buf.data() + 1);
                auto it = sessions.find(conv);
                if (it == sessions.end() || !it->second->relayed) continue;
                it->second->lastRecv = now;
                ikcp_input(it->second->kcp, buf.data() + 1, n - 1);
            } else if (type == 'X' && n >= 5) {
                uint32_t conv;
                memcpy(&conv, buf.data() + 1, 4);
                auto it = sessions.find(conv);
                if (it != sessions.end() && SameEp(it->second->peer, from)) closeSession(conv, false);
            }
        }

        // ---- sessions: engine <-> KCP
        std::vector<uint32_t> dead;
        for (auto& [conv, sp] : sessions) {
            Session* s = sp.get();
            ikcp_update(s->kcp, (IUINT32)(now & 0xFFFFFFFF));
            // engine -> peer
            while (ikcp_waitsnd(s->kcp) < kMaxKcpQueue) {
                int r = (int)recv((sock_t)s->local, buf.data(), 32768, 0);
                if (r > 0) { ikcp_send(s->kcp, buf.data(), r); continue; }
                if (r == 0 || !WouldBlock()) s->closed = true;
                break;
            }
            // peer -> engine
            while (s->toLocal.size() < (4u << 20)) {
                int sz = ikcp_peeksize(s->kcp);
                if (sz <= 0) break;
                std::string chunk((size_t)sz, '\0');
                if (ikcp_recv(s->kcp, chunk.data(), sz) <= 0) break;
                s->toLocal += chunk;
            }
            while (!s->toLocal.empty()) {
                int w = (int)::send((sock_t)s->local, s->toLocal.data(), (int)std::min<size_t>(s->toLocal.size(), 1 << 20), 0);
                if (w > 0) { s->toLocal.erase(0, (size_t)w); continue; }
                if (!WouldBlock()) s->closed = true;
                break;
            }
            ikcp_flush(s->kcp);
            if (s->closed || now - s->lastRecv > 60000) dead.push_back(conv);
        }
        for (auto c : dead) closeSession(c, true);
    }
    for (auto& [conv, s] : sessions) {
        char x[5] = {'X'};
        memcpy(x + 1, &conv, 4);
        sendto(udp, x, 5, 0, (sockaddr*)&s->peer, sizeof(s->peer));
        ikcp_release(s->kcp);
        CloseSocket(s->local);
    }
    sessions.clear();
    sessions_ = 0;
    if (serverOk) send(server, 'R', json{{"t", "bye"}, {"room", cfg_.roomHash}, {"id", cfg_.id}}.dump());
    if (natGwKnown && natExtPort) natRequest(natGw, 1, 0);  // release the router mapping
    upnp.Stop();
    CloseSocket((SocketHandle)udp);
    SetStatus("off");
}

}  // namespace ls
