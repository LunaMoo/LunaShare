#include "rendezvous_server.h"

#include <algorithm>
#include <cstring>
#include <random>

#include <nlohmann/json.hpp>

#include "net.h"
#include "platform.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#endif

namespace ls {

using json = nlohmann::json;
using sock_t = decltype(socket(0, 0, 0));

namespace {
struct Device {
    std::string id, name, pf, pub;
    sockaddr_in ep{};       // where its packets come from (replies go here)
    in_addr via{};          // the server address it talks to (replies are sent from it)
    sockaddr_in pubEp{};    // what other devices are told to use (differs for devices on the server's own LAN)
    std::vector<std::string> locals;
    std::vector<std::string> extra;  // router mappings / forwarded port, already "ip:port"
    bool relayOk = true;             // the device allows its connections to be relayed
    int64_t lastSeen = 0;
};

// a relayed connection: 'Y' packets from one side go to the other
struct Relay {
    sockaddr_in a{}, b{};
    in_addr viaA{}, viaB{};
    std::string room, idA, idB;
    int64_t lastSeen = 0;
    double tokA = 0, tokB = 0;  // byte buckets for the bandwidth limit
    int64_t ts = 0;
    uint64_t bytes = 0;
};

std::string Ep(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

bool IsHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!isxdigit((unsigned char)c)) return false;
    return true;
}

bool IsPrivate(in_addr a) {
    uint32_t h = ntohl(a.s_addr);
    return (h >> 24) == 10 || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8 || (h >> 24) == 127 || (h >> 16) == 0xA9FE;
}

// recvfrom() that also reports which local address the packet was sent to, and sendto() that replies from it.
// Without this, a device on the server's own LAN talking to the router's public address gets replies from the
// router's LAN address and ignores them.
int RecvFromTo(sock_t s, char* buf, int len, sockaddr_in& from, in_addr& dst) {
    dst.s_addr = 0;
#if defined(_WIN32)
    socklen_t fl = sizeof(from);
    return (int)recvfrom(s, buf, len, 0, (sockaddr*)&from, &fl);
#else
    iovec iov{buf, (size_t)len};
    alignas(cmsghdr) char ctl[256];
    msghdr mh{};
    mh.msg_name = &from;
    mh.msg_namelen = sizeof(from);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl;
    mh.msg_controllen = sizeof(ctl);
    int n = (int)recvmsg(s, &mh, 0);
    if (n < 0) return n;
    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != IPPROTO_IP) continue;
#if defined(IP_PKTINFO)
        if (c->cmsg_type == IP_PKTINFO) dst = ((in_pktinfo*)CMSG_DATA(c))->ipi_addr;
#elif defined(IP_RECVDSTADDR)
        if (c->cmsg_type == IP_RECVDSTADDR) memcpy(&dst, CMSG_DATA(c), sizeof(dst));
#endif
    }
    return n;
#endif
}

void SendFrom(sock_t s, const std::string& p, const sockaddr_in& to, in_addr src) {
#if !defined(_WIN32) && (defined(IP_PKTINFO) || defined(IP_SENDSRCADDR))
    if (src.s_addr != 0) {
        iovec iov{(void*)p.data(), p.size()};
        msghdr mh{};
        mh.msg_name = (void*)&to;
        mh.msg_namelen = sizeof(to);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
#if defined(IP_PKTINFO)
        alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(in_pktinfo))] = {};
        mh.msg_control = ctl;
        mh.msg_controllen = sizeof(ctl);
        cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = IPPROTO_IP;
        c->cmsg_type = IP_PKTINFO;
        c->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
        in_pktinfo pi{};
        pi.ipi_spec_dst = src;
        memcpy(CMSG_DATA(c), &pi, sizeof(pi));
#else
        alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(in_addr))] = {};
        mh.msg_control = ctl;
        mh.msg_controllen = sizeof(ctl);
        cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = IPPROTO_IP;
        c->cmsg_type = IP_SENDSRCADDR;
        c->cmsg_len = CMSG_LEN(sizeof(in_addr));
        memcpy(CMSG_DATA(c), &src, sizeof(src));
#endif
        if (sendmsg(s, &mh, 0) >= 0) return;
    }
#else
    (void)src;
#endif
    sendto(s, p.data(), (int)p.size(), 0, (const sockaddr*)&to, sizeof(to));
}

std::string Clip(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }

struct Bucket {
    double tokens = 40;
    int64_t ts = 0;
};
}  // namespace

bool RendezvousServer::Start(const Options& o, std::string* err) {
    Stop();
    opt_ = o;
    NetInit();
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(o.port);
    if (inet_pton(AF_INET, o.bind.c_str(), &a.sin_addr) != 1) {
        if (err) *err = "invalid bind address " + o.bind;
        CloseSocket((SocketHandle)s);
        return false;
    }
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
        if (err) *err = "cannot bind UDP " + o.bind + ":" + std::to_string(o.port);
        CloseSocket((SocketHandle)s);
        return false;
    }
#if !defined(_WIN32)
    int one = 1;
#if defined(IP_PKTINFO)
    setsockopt(s, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
#elif defined(IP_RECVDSTADDR)
    setsockopt(s, IPPROTO_IP, IP_RECVDSTADDR, &one, sizeof(one));
#endif
#endif
    sock_ = (intptr_t)s;
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void RendezvousServer::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (sock_ != -1) CloseSocket((SocketHandle)sock_);
    sock_ = -1;
}

size_t RendezvousServer::DeviceCount() {
    std::lock_guard<std::mutex> l(m_);
    return devices_;
}

size_t RendezvousServer::RoomCount() {
    std::lock_guard<std::mutex> l(m_);
    return rooms_;
}

void RendezvousServer::Loop() {
    sock_t s = (sock_t)sock_;
    std::map<std::string, std::map<std::string, Device>> rooms;  // room -> id -> device
    std::map<uint32_t, Bucket> limits;                             // per source IP
    std::mt19937_64 rng(std::random_device{}() ^ (uint64_t)NowMs());
    char buf[8192];
    std::map<uint32_t, Relay> relays;  // conv -> relay
    int64_t lastSweep = SteadyMs();
    auto log = [&](const std::string& m) { if (opt_.log) opt_.log(m); };
    in_addr via{};  // local address of the packet being handled
    auto sendTo = [&](const sockaddr_in& to, in_addr src, const json& j) {
        SendFrom(s, "R" + j.dump(-1, ' ', false, json::error_handler_t::replace), to, src);
    };
    auto sendJson = [&](const sockaddr_in& to, const json& j) { sendTo(to, via, j); };
    auto peersOf = [&](const std::string& room, const std::string& self) {
        json arr = json::array();
        for (auto& [id, d] : rooms[room]) {
            if (id == self) continue;
            arr.push_back({{"id", d.id}, {"name", d.name}, {"pf", d.pf}, {"pub", d.pub}, {"ep", Ep(d.pubEp)}, {"local", d.locals}, {"extra", d.extra}});
        }
        return arr;
    };
    auto pushRoom = [&](const std::string& room, const std::string& except) {
        for (auto& [id, d] : rooms[room])
            if (id != except) sendTo(d.ep, d.via, {{"t", "peers"}, {"you", Ep(d.pubEp)}, {"peers", peersOf(room, id)}});
    };
    auto counts = [&] {
        size_t n = 0;
        for (auto& [r, m] : rooms) n += m.size();
        std::lock_guard<std::mutex> l(m_);
        devices_ = n;
        rooms_ = rooms.size();
        return n;
    };

    while (running_) {
        int64_t now = SteadyMs();
        if (now - lastSweep > 5000) {
            lastSweep = now;
            for (auto rit = rooms.begin(); rit != rooms.end();) {
                bool changed = false;
                for (auto it = rit->second.begin(); it != rit->second.end();) {
                    if (now - it->second.lastSeen > 60000) {
                        log("expired " + it->second.name + " (" + it->first.substr(0, 8) + ")");
                        it = rit->second.erase(it);
                        changed = true;
                    } else {
                        ++it;
                    }
                }
                if (changed) pushRoom(rit->first, "");
                if (rit->second.empty()) rit = rooms.erase(rit);
                else ++rit;
            }
            if (limits.size() > 10000) limits.clear();
            for (auto it = relays.begin(); it != relays.end();) {
                if (now - it->second.lastSeen > 90000) {
                    log("relay " + std::to_string(it->first) + " closed (" + std::to_string(it->second.bytes / 1024) + " KB)");
                    it = relays.erase(it);
                } else {
                    ++it;
                }
            }
            counts();
        }
        if (WaitReadable((SocketHandle)s, 500) != 1) continue;
        sockaddr_in from{};
        int n = RecvFromTo(s, buf, (int)sizeof(buf), from, via);
        if (n >= 6 && buf[0] == 'Y') {  // relayed data: ['Y'][conv u32][payload]
            const uint8_t* cb = (const uint8_t*)buf + 1;
            uint32_t conv = cb[0] | (uint32_t)cb[1] << 8 | (uint32_t)cb[2] << 16 | (uint32_t)cb[3] << 24;  // KCP header
            auto it = relays.find(conv);
            if (it == relays.end()) continue;
            Relay& r = it->second;
            bool fromA = from.sin_addr.s_addr == r.a.sin_addr.s_addr && from.sin_port == r.a.sin_port;
            bool fromB = from.sin_addr.s_addr == r.b.sin_addr.s_addr && from.sin_port == r.b.sin_port;
            if (!fromA && !fromB) continue;
            if (opt_.relayKbps > 0) {
                double rate = opt_.relayKbps * 125.0;  // bytes per second
                if (r.ts) {
                    double add = (double)(now - r.ts) * rate / 1000.0;
                    r.tokA = std::min(rate, r.tokA + add);
                    r.tokB = std::min(rate, r.tokB + add);
                }
                r.ts = now;
                double& tok = fromA ? r.tokA : r.tokB;
                if (tok < n) continue;  // over the limit: dropped, KCP slows down
                tok -= n;
            }
            r.lastSeen = now;
            r.bytes += (uint64_t)n;
            SendFrom(s, std::string(buf, (size_t)n), fromA ? r.b : r.a, fromA ? r.viaB : r.viaA);
            continue;
        }
        if (n < 2 || buf[0] != 'R') continue;
        // rate limit: 20 packets/s per source address, burst 40
        Bucket& b = limits[from.sin_addr.s_addr];
        if (b.ts) b.tokens = std::min(40.0, b.tokens + (double)(now - b.ts) * 0.02);
        b.ts = now;
        if (b.tokens < 1) continue;
        b.tokens -= 1;

        json j = json::parse(std::string(buf + 1, (size_t)n - 1), nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        std::string t = j.value("t", "");
        if (t == "reg") {
            std::string room = j.value("room", ""), id = j.value("id", "");
            if (!IsHex64(room) || id.empty() || id.size() > 64) continue;
            if (!opt_.allowedRooms.empty() && !opt_.allowedRooms.count(room)) {
                sendJson(from, {{"t", "error"}, {"msg", "this room is not allowed on this server"}});
                continue;
            }
            auto& r = rooms[room];
            bool isNew = !r.count(id);
            if (isNew && (r.size() >= opt_.maxPerRoom || counts() >= opt_.maxDevices)) {
                sendJson(from, {{"t", "error"}, {"msg", "server full"}});
                continue;
            }
            Device& d = r[id];
            bool moved = !isNew && (d.ep.sin_addr.s_addr != from.sin_addr.s_addr || d.ep.sin_port != from.sin_port);
            d.id = id;
            d.name = Clip(j.value("name", ""), 100);
            d.pf = Clip(j.value("pf", ""), 20);
            d.pub = Clip(j.value("pub", ""), 64);
            d.ep = from;
            d.via = via;
            d.pubEp = from;
            // A device on the server's own network that reached us through the public address: the world sees it
            // behind that same public address, not its private one.
            if (IsPrivate(from.sin_addr) && via.s_addr != 0 && !IsPrivate(via)) d.pubEp.sin_addr = via;
            d.locals.clear();
            for (auto& l : j.value("local", json::array()))
                if (l.is_string() && d.locals.size() < 8) d.locals.push_back(Clip(l.get<std::string>(), 32));
            d.extra.clear();
            for (auto& l : j.value("extra", json::array()))
                if (l.is_string() && d.extra.size() < 4) d.extra.push_back(Clip(l.get<std::string>(), 32));
            int fwd = j.value("fwd", 0);
            if (fwd > 0 && fwd < 65536) {  // manually forwarded port on the device's public address
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &d.pubEp.sin_addr, ip, sizeof(ip));
                d.extra.push_back(std::string(ip) + ":" + std::to_string(fwd));
            }
            d.relayOk = j.value("relay", true);
            d.lastSeen = now;
            // a device whose address changed (NAT rebinding) keeps its relayed connections
            if (moved)
                for (auto& [conv, rl] : relays) {
                    if (rl.room == room && rl.idA == id) rl.a = from, rl.viaA = via;
                    if (rl.room == room && rl.idB == id) rl.b = from, rl.viaB = via;
                }
            sendJson(from, {{"t", "peers"}, {"you", Ep(d.pubEp)}, {"peers", peersOf(room, id)}});
            if (isNew) counts();
            if (isNew || moved) {
                log(std::string(isNew ? "joined " : "moved ") + d.name + " (" + id.substr(0, 8) + ") from " + Ep(from) + " room " + room.substr(0, 8));
                pushRoom(room, id);
            }
        } else if (t == "connect") {
            // find the requester by its source endpoint, then the target in the same room
            std::string to = j.value("to", "");
            for (auto& [room, r] : rooms) {
                Device* me = nullptr;
                for (auto& [id, d] : r)
                    if (d.ep.sin_addr.s_addr == from.sin_addr.s_addr && d.ep.sin_port == from.sin_port) me = &d;
                if (!me) continue;
                auto it = r.find(to);
                if (it == r.end()) break;
                Device& other = it->second;
                char nonce[33];
                snprintf(nonce, sizeof(nonce), "%016llx%016llx", (unsigned long long)rng(), (unsigned long long)rng());
                sendTo(me->ep, me->via, {{"t", "punch"}, {"nonce", nonce}, {"initiator", true},
                                  {"peer", {{"id", other.id}, {"ep", Ep(other.pubEp)}, {"local", other.locals}, {"extra", other.extra}}}});
                sendTo(other.ep, other.via, {{"t", "punch"}, {"nonce", nonce}, {"initiator", false},
                                    {"peer", {{"id", me->id}, {"ep", Ep(me->pubEp)}, {"local", me->locals}, {"extra", me->extra}}}});
                log("introduced " + me->name + " <-> " + other.name);
                break;
            }
        } else if (t == "relay") {
            // direct connection failed: carry this pair's packets (only within a room, only if both allow it)
            std::string to = j.value("to", ""), nonce = Clip(j.value("nonce", ""), 64);
            uint32_t conv = j.value("conv", 0u);
            if (!conv || nonce.empty()) continue;
            for (auto& [room, r] : rooms) {
                Device* me = nullptr;
                for (auto& [id, d] : r)
                    if (d.ep.sin_addr.s_addr == from.sin_addr.s_addr && d.ep.sin_port == from.sin_port) me = &d;
                if (!me) continue;
                auto it = r.find(to);
                if (it == r.end()) break;
                Device& other = it->second;
                if (!opt_.relay || !me->relayOk || !other.relayOk || (relays.size() >= opt_.maxRelays && !relays.count(conv))) {
                    sendTo(me->ep, me->via, {{"t", "error"}, {"msg", !opt_.relay ? "this server doesn't relay connections" : !other.relayOk ? "the other device doesn't allow relayed connections" : "relay busy"}});
                    break;
                }
                Relay& rl = relays[conv];
                rl.a = me->ep;
                rl.viaA = me->via;
                rl.b = other.ep;
                rl.viaB = other.via;
                rl.room = room;
                rl.idA = me->id;
                rl.idB = other.id;
                rl.lastSeen = now;
                sendTo(me->ep, me->via, {{"t", "relayed"}, {"nonce", nonce}, {"conv", conv}, {"initiator", true}, {"peer", other.id}});
                sendTo(other.ep, other.via, {{"t", "relayed"}, {"nonce", nonce}, {"conv", conv}, {"initiator", false}, {"peer", me->id}});
                log("relaying " + me->name + " <-> " + other.name);
                break;
            }
        } else if (t == "bye") {
            std::string room = j.value("room", ""), id = j.value("id", "");
            auto rit = rooms.find(room);
            if (rit == rooms.end()) continue;
            auto it = rit->second.find(id);
            if (it != rit->second.end() && it->second.ep.sin_addr.s_addr == from.sin_addr.s_addr && it->second.ep.sin_port == from.sin_port) {
                log("left " + it->second.name);
                rit->second.erase(it);
                pushRoom(room, "");
                if (rit->second.empty()) rooms.erase(rit);
                counts();
            }
        }
    }
}

}  // namespace ls
