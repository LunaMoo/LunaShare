#include "rendezvous_server.h"

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
#endif

namespace ls {

using json = nlohmann::json;
using sock_t = decltype(socket(0, 0, 0));

namespace {
struct Device {
    std::string id, name, pf, pub;
    sockaddr_in ep{};
    std::vector<std::string> locals;
    std::vector<std::string> extra;  // router mappings / forwarded port, already "ip:port"
    int64_t lastSeen = 0;
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
    int64_t lastSweep = SteadyMs();
    auto log = [&](const std::string& m) { if (opt_.log) opt_.log(m); };
    auto sendJson = [&](const sockaddr_in& to, const json& j) {
        std::string p = "R" + j.dump(-1, ' ', false, json::error_handler_t::replace);
        sendto(s, p.data(), (int)p.size(), 0, (const sockaddr*)&to, sizeof(to));
    };
    auto peersOf = [&](const std::string& room, const std::string& self) {
        json arr = json::array();
        for (auto& [id, d] : rooms[room]) {
            if (id == self) continue;
            arr.push_back({{"id", d.id}, {"name", d.name}, {"pf", d.pf}, {"pub", d.pub}, {"ep", Ep(d.ep)}, {"local", d.locals}, {"extra", d.extra}});
        }
        return arr;
    };
    auto pushRoom = [&](const std::string& room, const std::string& except) {
        for (auto& [id, d] : rooms[room])
            if (id != except) sendJson(d.ep, {{"t", "peers"}, {"you", Ep(d.ep)}, {"peers", peersOf(room, id)}});
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
            counts();
        }
        if (WaitReadable((SocketHandle)s, 500) != 1) continue;
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int n = (int)recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
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
            d.locals.clear();
            for (auto& l : j.value("local", json::array()))
                if (l.is_string() && d.locals.size() < 8) d.locals.push_back(Clip(l.get<std::string>(), 32));
            d.extra.clear();
            for (auto& l : j.value("extra", json::array()))
                if (l.is_string() && d.extra.size() < 4) d.extra.push_back(Clip(l.get<std::string>(), 32));
            int fwd = j.value("fwd", 0);
            if (fwd > 0 && fwd < 65536) {  // manually forwarded port on the device's public address
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                d.extra.push_back(std::string(ip) + ":" + std::to_string(fwd));
            }
            d.lastSeen = now;
            sendJson(from, {{"t", "peers"}, {"you", Ep(from)}, {"peers", peersOf(room, id)}});
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
                sendJson(me->ep, {{"t", "punch"}, {"nonce", nonce}, {"initiator", true},
                                  {"peer", {{"id", other.id}, {"ep", Ep(other.ep)}, {"local", other.locals}, {"extra", other.extra}}}});
                sendJson(other.ep, {{"t", "punch"}, {"nonce", nonce}, {"initiator", false},
                                    {"peer", {{"id", me->id}, {"ep", Ep(me->ep)}, {"local", me->locals}, {"extra", me->extra}}}});
                log("introduced " + me->name + " <-> " + other.name);
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
