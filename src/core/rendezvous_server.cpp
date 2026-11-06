#include "rendezvous_server.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <set>

#include <nlohmann/json.hpp>

#include "crypto.h"
#include "model.h"
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
    std::string acc;                 // its account (verified certificate), for usernames
    std::string mnonce;              // challenge the device signs to open its mailbox
    bool mailAuthed = false;         // proved it owns `pub` (may take out / delete its mail)
    bool hidden = false;             // not listed in its room (except to devices of its own account)
    std::vector<std::string> links;  // private link tokens: devices sharing one meet across rooms
    bool SharesLink(const Device& o) const {
        for (auto& a : links)
            for (auto& b : o.links)
                if (a == b) return true;
        return false;
    }
};

struct MailItem {
    std::string id, data;  // data: base64 of the sealed envelope
    int64_t ts = 0;        // wall clock ms, for expiry
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

// A group call through the server ("hub"): every member sends its packets once, the server passes each on to the
// members it is addressed to. The packets are end-to-end encrypted with a key only the people in the call have.
struct HubMember {
    bool used = false;
    sockaddr_in ep{};
    in_addr via{};
    std::string id;
    int64_t lastSeen = 0;
    double tok = 0;
    int64_t ts = 0;
};
constexpr int kHubMax = 32;
struct Hub {
    std::string id;  // 32 hex characters
    HubMember m[kHubMax];
    uint64_t bytes = 0;
    int Count() const {
        int n = 0;
        for (auto& x : m) n += x.used;
        return n;
    }
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

size_t RendezvousServer::MailCount() {
    std::lock_guard<std::mutex> l(m_);
    return mail_;
}

void RendezvousServer::Loop() {
    sock_t s = (sock_t)sock_;
    std::map<std::string, std::map<std::string, Device>> rooms;  // room -> id -> device
    std::map<uint32_t, Bucket> limits;                             // per source IP
    std::mt19937_64 rng(std::random_device{}() ^ (uint64_t)NowMs());
    char buf[8192];
    std::map<uint32_t, Relay> relays;  // conv -> relay
    std::map<uint32_t, Hub> hubs;      // first 4 bytes of the hub id -> group call
    auto pushHub = [&](uint32_t h) {
        auto it = hubs.find(h);
        if (it == hubs.end()) return;
        json ids = json::array();
        for (auto& x : it->second.m) ids.push_back(x.used ? x.id : "");
        for (int i = 0; i < kHubMax; i++)
            if (it->second.m[i].used)
                SendFrom(s, "R" + json{{"t", "hub"}, {"hub", it->second.id}, {"you", i}, {"members", ids}}.dump(), it->second.m[i].ep, it->second.m[i].via);
    };
    int64_t lastSweep = SteadyMs();
    auto log = [&](const std::string& m) { if (opt_.log) opt_.log(m); };
    in_addr via{};  // local address of the packet being handled
    auto sendTo = [&](const sockaddr_in& to, in_addr src, const json& j) {
        SendFrom(s, "R" + j.dump(-1, ' ', false, json::error_handler_t::replace), to, src);
    };
    auto sendJson = [&](const sockaddr_in& to, const json& j) { sendTo(to, via, j); };
    auto peerJson = [](const Device& d) {
        return json{{"id", d.id}, {"name", d.name}, {"pf", d.pf}, {"pub", d.pub}, {"ep", Ep(d.pubEp)}, {"local", d.locals}, {"extra", d.extra}};
    };
    // what a device sees: its room (without hidden devices of other accounts) and every device it shares a link with
    auto peersOf = [&](const std::string& room, const std::string& self) {
        json arr = json::array();
        auto& r = rooms[room];
        auto me = r.find(self);
        const Device* m = me == r.end() ? nullptr : &me->second;
        std::set<std::string> seen{self};
        for (auto& [id, d] : r) {
            if (id == self || (d.hidden && !(m && !m->acc.empty() && m->acc == d.acc))) continue;
            arr.push_back(peerJson(d));
            seen.insert(id);
        }
        if (m && !m->links.empty())
            for (auto& [rn, rr] : rooms)
                for (auto& [id, d] : rr)
                    if (!seen.count(id) && m->SharesLink(d)) {
                        json p = peerJson(d);
                        p["link"] = true;
                        arr.push_back(p);
                        seen.insert(id);
                    }
        return arr;
    };
    auto pushOne = [&](const std::string& room, Device& d) {
        sendTo(d.ep, d.via, {{"t", "peers"}, {"you", Ep(d.pubEp)}, {"peers", peersOf(room, d.id)}, {"lk", true}});
    };
    // a device came, moved or went: tell the others in its room and those it shares a link with
    auto pushAround = [&](const std::string& room, const std::string& except, const std::vector<std::string>& links) {
        for (auto& [id, d] : rooms[room])
            if (id != except) pushOne(room, d);
        if (links.empty()) return;
        Device probe;
        probe.links = links;
        for (auto& [rn, rr] : rooms) {
            if (rn == room) continue;
            for (auto& [id, d] : rr)
                if (id != except && probe.SharesLink(d)) pushOne(rn, d);
        }
    };
    auto pushRoom = [&](const std::string& room, const std::string& except) { pushAround(room, except, {}); };
    // the device `to` that `me` may be introduced to: one in its room, or one it shares a link with
    auto findTarget = [&](const std::string& room, const Device& me, const std::string& to) -> Device* {
        auto& r = rooms[room];
        auto it = r.find(to);
        if (it != r.end()) return &it->second;
        if (me.links.empty()) return nullptr;
        for (auto& [rn, rr] : rooms) {
            auto x = rr.find(to);
            if (x != rr.end() && me.SharesLink(x->second)) return &x->second;
        }
        return nullptr;
    };
    // link token -> device keys seen with it (wall ms): mail for a linked device that is offline goes to its
    // own mailbox ("L/" + key), which it reads whatever room it is in
    std::map<std::string, std::map<std::string, int64_t>> linkPubs;
    // ---- mailboxes: "room/recipient key" -> sealed envelopes
    std::map<std::string, std::vector<MailItem>> boxes;
    size_t mailBytes = 0;
    bool mailDirty = false;
    std::string mailFile = opt_.dataDir.empty() ? std::string() : JoinPath(opt_.dataDir, "mailbox.json");
    auto boxBytes = [](const std::vector<MailItem>& v) {
        size_t b = 0;
        for (auto& m : v) b += m.data.size();
        return b;
    };
    if (opt_.mailbox && !mailFile.empty()) {
        json mj;
        if (!LoadJsonFile(mailFile, mj) || !mj.is_object()) mj = json::object();
        json bj = mj.value("boxes", json::object());  // (kept alive for items())
        if (bj.is_object())
            for (auto& [key, arr] : bj.items())
                for (auto& it : arr) {
                    MailItem m{it.value("id", ""), it.value("d", ""), it.value("ts", (int64_t)0)};
                    if (m.id.empty() || m.data.empty()) continue;
                    boxes[key].push_back(m);
                    mailBytes += m.data.size();
                }
        json lj = mj.value("links", json::object());
        if (lj.is_object())
            for (auto& [tok, m] : lj.items())
                if (m.is_object())
                    for (auto& [pub, ts] : m.items())
                        if (ts.is_number_integer()) linkPubs[tok][pub] = ts.get<int64_t>();
        if (!boxes.empty()) log("mailbox: loaded " + std::to_string(boxes.size()) + " mailbox(es)");
    }
    auto saveMail = [&] {
        if (!mailDirty || mailFile.empty()) return;
        json bj = json::object();
        for (auto& [key, v] : boxes) {
            json arr = json::array();
            for (auto& m : v) arr.push_back({{"id", m.id}, {"d", m.data}, {"ts", m.ts}});
            bj[key] = arr;
        }
        json lj = json::object();
        for (auto& [tok, m] : linkPubs) lj[tok] = m;
        MakeDirs(opt_.dataDir);
        SaveJsonFile(mailFile, json{{"v", 1}, {"boxes", bj}, {"links", lj}});
        mailDirty = false;
    };
    auto counts = [&] {
        size_t n = 0, mails = 0;
        for (auto& [r, m] : rooms) n += m.size();
        for (auto& [k, v] : boxes) mails += v.size();
        std::lock_guard<std::mutex> l(m_);
        devices_ = n;
        rooms_ = rooms.size();
        mail_ = mails;
        return n;
    };
    // ---- usernames: room -> name -> account key (first come; only that account can change or release it)
    std::map<std::string, std::map<std::string, std::string>> names;
    bool namesDirty = false;
    std::string namesFile = opt_.dataDir.empty() ? std::string() : JoinPath(opt_.dataDir, "usernames.json");
    if (!namesFile.empty()) {
        json nj;
        if (LoadJsonFile(namesFile, nj) && nj.is_object()) {
            json rj = nj.value("rooms", json::object());
            if (rj.is_object())
                for (auto& [room, m] : rj.items())
                    if (m.is_object())
                        for (auto& [n, a] : m.items())
                            if (a.is_string()) names[room][n] = a.get<std::string>();
        }
    }
    auto saveNames = [&] {
        if (!namesDirty || namesFile.empty()) return;
        json rj = json::object();
        for (auto& [room, m] : names) rj[room] = m;
        MakeDirs(opt_.dataDir);
        SaveJsonFile(namesFile, json{{"v", 1}, {"rooms", rj}});
        namesDirty = false;
    };
    auto nameOf = [&](const std::string& room, const std::string& acc) -> std::string {
        if (acc.empty()) return {};
        auto it = names.find(room);
        if (it == names.end()) return {};
        for (auto& [n, a] : it->second)
            if (a == acc) return n;
        return {};
    };
    auto validName = [](std::string& n) {
        for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
        if (n.size() < 3 || n.size() > 32 || !isalnum((unsigned char)n[0])) return false;
        for (char ch : n)
            if (!isalnum((unsigned char)ch) && ch != '.' && ch != '_' && ch != '-') return false;
        return true;
    };
    auto mailCount = [&](const std::string& room, const std::string& pub) {
        int n = 0;
        for (auto& key : {room + "/" + pub, "L/" + pub}) {
            auto it = boxes.find(key);
            if (it != boxes.end()) n += (int)it->second.size();
        }
        return n;
    };
    // the registered device a packet comes from (and its room)
    auto findSender = [&](const sockaddr_in& f, std::string* roomOut) -> Device* {
        for (auto& [room, r] : rooms)
            for (auto& [id, d] : r)
                if (d.ep.sin_addr.s_addr == f.sin_addr.s_addr && d.ep.sin_port == f.sin_port) {
                    if (roomOut) *roomOut = room;
                    return &d;
                }
        return nullptr;
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
                        std::vector<std::string> links = it->second.links;
                        std::string gone = it->first;
                        it = rit->second.erase(it);
                        if (!links.empty()) pushAround(rit->first, gone, links);  // (its room is told below)
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
            if (opt_.mailbox) {
                int64_t wall = NowMs(), maxAge = (int64_t)opt_.mailboxDays * 86400000;
                for (auto bit = boxes.begin(); bit != boxes.end();) {
                    auto& v = bit->second;
                    size_t before = v.size();
                    v.erase(std::remove_if(v.begin(), v.end(), [&](const MailItem& m) {
                                if (wall - m.ts <= maxAge) return false;
                                mailBytes -= m.data.size();
                                return true;
                            }),
                            v.end());
                    if (v.size() != before) mailDirty = true;
                    if (v.empty()) bit = boxes.erase(bit);
                    else ++bit;
                }
                for (auto lit = linkPubs.begin(); lit != linkPubs.end();) {
                    for (auto p = lit->second.begin(); p != lit->second.end();)
                        p = wall - p->second > maxAge ? (mailDirty = true, lit->second.erase(p)) : std::next(p);
                    lit = lit->second.empty() ? linkPubs.erase(lit) : std::next(lit);
                }
                saveMail();
            }
            for (auto it = hubs.begin(); it != hubs.end();) {
                bool changed = false;
                for (auto& x : it->second.m)
                    if (x.used && now - x.lastSeen > 15000) {
                        x = HubMember();
                        changed = true;
                    }
                if (it->second.Count() == 0) {
                    log("group call " + it->second.id.substr(0, 8) + " ended (" + std::to_string(it->second.bytes / 1024) + " KB)");
                    it = hubs.erase(it);
                    continue;
                }
                if (changed) pushHub(it->first);
                ++it;
            }
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
        if (n >= 9 && buf[0] == 'H') {  // group call: ['H'][hub u32][to mask u32][payload] -> ['H'][hub u32][from u8][payload]
            const uint8_t* hb = (const uint8_t*)buf + 1;
            uint32_t h = hb[0] | (uint32_t)hb[1] << 8 | (uint32_t)hb[2] << 16 | (uint32_t)hb[3] << 24;
            uint32_t mask = hb[4] | (uint32_t)hb[5] << 8 | (uint32_t)hb[6] << 16 | (uint32_t)hb[7] << 24;
            auto it = hubs.find(h);
            if (it == hubs.end()) continue;
            Hub& hub = it->second;
            int me = -1;
            for (int i = 0; i < kHubMax; i++)
                if (hub.m[i].used && hub.m[i].ep.sin_addr.s_addr == from.sin_addr.s_addr && hub.m[i].ep.sin_port == from.sin_port) me = i;
            if (me < 0) continue;
            HubMember& sm = hub.m[me];
            if (opt_.relayKbps > 0) {
                double rate = opt_.relayKbps * 125.0;
                if (sm.ts) sm.tok = std::min(rate, sm.tok + (double)(now - sm.ts) * rate / 1000.0);
                sm.ts = now;
                if (sm.tok < n) continue;
                sm.tok -= n;
            }
            sm.lastSeen = now;
            std::string out(buf, 5);
            out += (char)me;
            out.append(buf + 9, (size_t)n - 9);
            for (int i = 0; i < kHubMax; i++)
                if (i != me && hub.m[i].used && (mask >> i & 1)) {
                    SendFrom(s, out, hub.m[i].ep, hub.m[i].via);
                    hub.bytes += out.size();
                }
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
            bool wasHidden = d.hidden;
            std::vector<std::string> oldLinks = d.links;
            d.hidden = j.value("hidden", false);
            d.links.clear();
            for (auto& l : j.value("links", json::array()))
                if (l.is_string() && IsHex64(l.get<std::string>()) && d.links.size() < 500) d.links.push_back(l.get<std::string>());
            bool linksChanged = d.links != oldLinks || d.hidden != wasHidden;
            if (opt_.mailbox && IsHex64(d.pub))
                for (auto& l : d.links) {
                    auto& ts = linkPubs[l][d.pub];
                    if (NowMs() - ts > 3600000) mailDirty = true;  // (saved with the mailbox now and then)
                    ts = NowMs();
                }
            d.lastSeen = now;
            {
                std::string acc = Clip(j.value("acc", ""), 64), acs = Clip(j.value("acs", ""), 128);
                d.acc = !acc.empty() && IsHex64(d.pub) && VerifyData(acc, "device", id + "\n" + d.pub, acs) ? acc : std::string();
            }
            if (d.mnonce.empty() || moved) {
                char nn[33];
                snprintf(nn, sizeof(nn), "%016llx%016llx", (unsigned long long)rng(), (unsigned long long)rng());
                d.mnonce = nn;
                d.mailAuthed = false;
            }
            // a device whose address changed (NAT rebinding) keeps its relayed connections
            if (moved)
                for (auto& [conv, rl] : relays) {
                    if (rl.idA == id) rl.a = from, rl.viaA = via;
                    if (rl.idB == id) rl.b = from, rl.viaB = via;
                }
            json reply = {{"t", "peers"}, {"you", Ep(d.pubEp)}, {"peers", peersOf(room, id)}, {"lk", true}};
            if (!d.acc.empty()) reply["uname"] = nameOf(room, d.acc);
            if (opt_.mailbox) {
                reply["mail"] = mailCount(room, d.pub);
                reply["mnonce"] = d.mnonce;
            }
            sendJson(from, reply);
            if (isNew) counts();
            if (isNew || moved || linksChanged) {
                if (isNew || moved)
                    log(std::string(isNew ? "joined " : "moved ") + d.name + " (" + id.substr(0, 8) + ") from " + Ep(from) + " room " + room.substr(0, 8));
                std::vector<std::string> all = d.links;
                all.insert(all.end(), oldLinks.begin(), oldLinks.end());  // (a removed link: the other side forgets me)
                pushAround(room, id, all);
            }
        } else if (t == "connect") {
            // find the requester by its source endpoint, then the target in the same room
            std::string to = j.value("to", "");
            for (auto& [room, r] : rooms) {
                Device* me = nullptr;
                for (auto& [id, d] : r)
                    if (d.ep.sin_addr.s_addr == from.sin_addr.s_addr && d.ep.sin_port == from.sin_port) me = &d;
                if (!me) continue;
                Device* target = findTarget(room, *me, to);
                if (!target) break;
                Device& other = *target;
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
                Device* target = findTarget(room, *me, to);
                if (!target) break;
                Device& other = *target;
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
        } else if (t == "hjoin" || t == "hleave") {
            // join (and stay in: sent every few seconds) / leave a group call through the server
            std::string hid = Clip(j.value("hub", ""), 32);
            Device* me = findSender(from, nullptr);
            if (!me || hid.size() != 32) continue;
            uint8_t hb[4];
            if (!FromHex(hid.substr(0, 8), hb, 4)) continue;
            uint32_t h = hb[0] | (uint32_t)hb[1] << 8 | (uint32_t)hb[2] << 16 | (uint32_t)hb[3] << 24;
            if (t == "hleave") {
                auto it = hubs.find(h);
                if (it == hubs.end() || it->second.id != hid) continue;
                for (auto& x : it->second.m)
                    if (x.used && x.id == me->id) x = HubMember();
                if (it->second.Count() == 0) hubs.erase(it);
                else pushHub(h);
                continue;
            }
            if (!opt_.relay) {
                sendJson(from, {{"t", "herror"}, {"hub", hid}, {"msg", "this server doesn't relay calls"}});
                continue;
            }
            auto it = hubs.find(h);
            if (it == hubs.end()) {
                if (hubs.size() >= opt_.maxRelays) {
                    sendJson(from, {{"t", "herror"}, {"hub", hid}, {"msg", "relay busy"}});
                    continue;
                }
                it = hubs.emplace(h, Hub()).first;
                it->second.id = hid;
                log("group call " + hid.substr(0, 8) + " started by " + me->name);
            } else if (it->second.id != hid) {
                continue;  // (a different call with the same first bytes)
            }
            Hub& hub = it->second;
            int slot = -1, freeSlot = -1;
            for (int i = 0; i < kHubMax; i++) {
                if (hub.m[i].used && hub.m[i].id == me->id) slot = i;
                if (!hub.m[i].used && freeSlot < 0) freeSlot = i;
            }
            bool changed = false;
            if (slot < 0) {
                if (freeSlot < 0) {
                    sendJson(from, {{"t", "herror"}, {"hub", hid}, {"msg", "call full"}});
                    continue;
                }
                slot = freeSlot;
                hub.m[slot].used = true;
                hub.m[slot].id = me->id;
                changed = true;
            }
            hub.m[slot].ep = from;
            hub.m[slot].via = via;
            hub.m[slot].lastSeen = now;
            if (changed) pushHub(h);
            else {
                json ids = json::array();
                for (auto& x : hub.m) ids.push_back(x.used ? x.id : "");
                sendJson(from, {{"t", "hub"}, {"hub", hid}, {"you", slot}, {"members", ids}});
            }
        } else if (t == "mput" && opt_.mailbox) {
            // a room member leaves a sealed message for a device (identified by its public key)
            std::string room, to = j.value("to", ""), id = Clip(j.value("id", ""), 64), data = j.value("d", "");
            Device* me = findSender(from, &room);
            if (!me || !IsHex64(to) || id.empty() || data.empty() || data.size() > 7000) continue;
            std::string key = room + "/" + to;
            bool inRoom = false;
            for (auto& [did, d] : rooms[room]) inRoom |= d.pub == to;
            if (!inRoom)
                for (auto& l : me->links) {
                    auto lit = linkPubs.find(l);
                    if (lit != linkPubs.end() && lit->second.count(to)) key = "L/" + to;  // a linked device
                }
            auto& v = boxes[key];
            bool dup = false;
            for (auto& m : v) dup |= m.id == id;
            if (!dup) {
                if (boxBytes(v) + data.size() > (size_t)opt_.mailboxMB * 1048576 || v.size() >= 20000 ||
                    mailBytes + data.size() > (size_t)opt_.mailboxTotalMB * 1048576) {
                    sendJson(from, {{"t", "mfull"}, {"id", id}});
                    if (v.empty()) boxes.erase(key);
                    continue;
                }
                v.push_back({id, data, NowMs()});
                mailBytes += data.size();
                mailDirty = true;
                // the recipient is online right now: tell it
                for (auto& [rn, rr] : rooms)
                    if (rn == room || key[0] == 'L')
                        for (auto& [did, d] : rr)
                            if (d.pub == to) sendTo(d.ep, d.via, {{"t", "mail"}, {"n", mailCount(rn, to)}, {"mnonce", d.mnonce}});
            }
            sendJson(from, {{"t", "mok"}, {"id", id}});
        } else if (t == "mget" && opt_.mailbox) {
            // the owner of a key takes out its mail: it signs the challenge it got when registering
            std::string room;
            Device* me = findSender(from, &room);
            if (!me || me->pub.empty() || me->mnonce.empty()) continue;
            if (!me->mailAuthed) {
                if (!VerifyData(me->pub, "mailbox", room + "\n" + me->mnonce, j.value("sig", ""))) {
                    sendJson(from, {{"t", "error"}, {"msg", "mailbox: invalid signature"}});
                    continue;
                }
                me->mailAuthed = true;
            }
            int sent = 0;
            for (auto& key : {room + "/" + me->pub, "L/" + me->pub}) {
                auto it = boxes.find(key);
                if (it != boxes.end())
                    for (auto& m : it->second) {
                        if (sent >= 8) break;  // the rest after the device confirmed these
                        sendJson(from, {{"t", "mitem"}, {"id", m.id}, {"d", m.data}, {"ts", m.ts}});
                        sent++;
                    }
            }
            sendJson(from, {{"t", "mail"}, {"n", mailCount(room, me->pub)}, {"mnonce", me->mnonce}});
        } else if (t == "mack" && opt_.mailbox) {
            // delivered: the owner deletes these
            std::string room;
            Device* me = findSender(from, &room);
            if (!me || !me->mailAuthed) continue;
            auto ids = j.value("ids", json::array());
            for (auto& key : {room + "/" + me->pub, "L/" + me->pub}) {
                auto it = boxes.find(key);
                if (it == boxes.end()) continue;
                auto& v = it->second;
                v.erase(std::remove_if(v.begin(), v.end(), [&](const MailItem& m) {
                            for (auto& x : ids)
                                if (x.is_string() && x.get<std::string>() == m.id) {
                                    mailBytes -= m.data.size();
                                    mailDirty = true;
                                    return true;
                                }
                            return false;
                        }),
                        v.end());
                if (v.empty()) boxes.erase(it);
            }
            sendJson(from, {{"t", "mail"}, {"n", mailCount(room, me->pub)}, {"mnonce", me->mnonce}});
        } else if (t == "uclaim" || t == "urel") {
            // claim / release my account's username in this room (signed with the account key)
            std::string room, name = Clip(j.value("name", ""), 40);
            Device* me = findSender(from, &room);
            if (!me) continue;
            if (me->acc.empty()) {
                sendJson(from, {{"t", "uerr"}, {"name", name}, {"msg", "this device has no account"}});
                continue;
            }
            std::string what = t == "uclaim" ? name : std::string("release");
            if (t == "uclaim" && !validName(name)) {
                sendJson(from, {{"t", "uerr"}, {"name", name}, {"msg", "invalid name"}});
                continue;
            }
            if (!VerifyData(me->acc, "username", room + "\n" + (t == "uclaim" ? name : what), j.value("sig", ""))) {
                sendJson(from, {{"t", "uerr"}, {"name", name}, {"msg", "invalid signature"}});
                continue;
            }
            auto& m = names[room];
            if (t == "uclaim") {
                auto it = m.find(name);
                if (it != m.end() && it->second != me->acc) {
                    sendJson(from, {{"t", "uerr"}, {"name", name}, {"msg", "taken"}});
                    continue;
                }
                if (m.size() >= 20000 && it == m.end()) {
                    sendJson(from, {{"t", "uerr"}, {"name", name}, {"msg", "server full"}});
                    continue;
                }
            }
            for (auto it = m.begin(); it != m.end();) it = it->second == me->acc ? m.erase(it) : std::next(it);  // one name per account
            if (t == "uclaim") m[name] = me->acc;
            if (m.empty()) names.erase(room);
            namesDirty = true;
            saveNames();
            log(t == "uclaim" ? "username " + name + " claimed" : "username released");
            sendJson(from, {{"t", "uok"}, {"name", t == "uclaim" ? name : std::string()}});
        } else if (t == "ulook") {
            // who is this username: its account and the account's devices online in the room
            std::string room, name = Clip(j.value("name", ""), 40);
            Device* me = findSender(from, &room);
            if (!me) continue;
            for (auto& ch : name) ch = (char)tolower((unsigned char)ch);
            auto rit = names.find(room);
            auto it = rit == names.end() ? std::map<std::string, std::string>::iterator() : rit->second.find(name);
            if (rit == names.end() || it == rit->second.end()) {
                sendJson(from, {{"t", "uerr"}, {"name", name}, {"msg", "not found"}});
                continue;
            }
            json devs = json::array();
            for (auto& [id, d] : rooms[room])
                if (d.acc == it->second) devs.push_back({{"id", d.id}, {"name", d.name}, {"pf", d.pf}, {"pub", d.pub}});
            sendJson(from, {{"t", "uinfo"}, {"name", name}, {"acc", it->second}, {"devs", devs}});
        } else if (t == "bye") {
            std::string room = j.value("room", ""), id = j.value("id", "");
            auto rit = rooms.find(room);
            if (rit == rooms.end()) continue;
            auto it = rit->second.find(id);
            if (it != rit->second.end() && it->second.ep.sin_addr.s_addr == from.sin_addr.s_addr && it->second.ep.sin_port == from.sin_port) {
                log("left " + it->second.name);
                std::vector<std::string> links = it->second.links;
                rit->second.erase(it);
                pushAround(room, id, links);
                if (rit->second.empty()) rooms.erase(rit);
                counts();
            }
        }
    }
    saveMail();
}

}  // namespace ls
