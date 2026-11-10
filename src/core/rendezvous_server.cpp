#include "rendezvous_server.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
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
    bool restricted = false;         // joined with someone's invite (remembered): sees and reaches only its links and account
    std::string hw;                  // hash of the computer's / phone's id (the same after reinstalling the app)
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
    // invite rooms: the inviter groups some of its invite codes; everyone holding a code of one group meets the others
    std::map<std::string, std::map<std::string, std::set<std::string>>> groups;  // inviter key -> group -> tokens
    std::map<std::string, std::string> tokGroup;                                   // token -> inviter key + "/" + group
    std::map<std::string, std::string> groupNames;                                 // inviter key + "/" + group -> room name
    auto rebuildGroups = [&] {
        tokGroup.clear();
        for (auto& [owner, gs] : groups)
            for (auto& [g, toks] : gs)
                for (auto& t : toks) tokGroup[t] = owner + "/" + g;
    };
    // the tokens a device reaches: its own, and all tokens of the invite rooms they belong to
    auto reach = [&](const std::vector<std::string>& links) {
        std::set<std::string> r(links.begin(), links.end());
        for (auto& l : links) {
            auto it = tokGroup.find(l);
            if (it == tokGroup.end()) continue;
            size_t sl = it->second.find('/');
            auto& toks = groups[it->second.substr(0, sl)][it->second.substr(sl + 1)];
            r.insert(toks.begin(), toks.end());
        }
        return r;
    };
    auto relatedR = [](const std::set<std::string>& r, const Device& b) {
        for (auto& l : b.links)
            if (r.count(l)) return true;
        return false;
    };
    auto related = [&](const Device& a, const Device& b) { return !a.links.empty() && relatedR(reach(a.links), b); };
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
            bool sameAcc = m && !m->acc.empty() && m->acc == d.acc;
            if (id == self || (d.hidden && !sameAcc) || (m && m->restricted && !sameAcc)) continue;
            arr.push_back(peerJson(d));
            seen.insert(id);
        }
        std::set<std::string> mr = m ? reach(m->links) : std::set<std::string>();
        if (!mr.empty())
            for (auto& [rn, rr] : rooms)
                for (auto& [id, d] : rr)
                    if (!seen.count(id) && relatedR(mr, d)) {
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
        std::set<std::string> pr = reach(links);
        for (auto& [rn, rr] : rooms) {
            if (rn == room) continue;
            for (auto& [id, d] : rr)
                if (id != except && relatedR(pr, d)) pushOne(rn, d);
        }
    };
    auto pushRoom = [&](const std::string& room, const std::string& except) { pushAround(room, except, {}); };
    // the device `to` that `me` may be introduced to: one in its room, or one it shares a link with
    auto findTarget = [&](const std::string& room, const Device& me, const std::string& to) -> Device* {
        auto ok = [&](const Device& t) {  // invited devices only reach / are reached through links and their account
            if (!me.restricted && !t.restricted) return true;
            return (!me.acc.empty() && me.acc == t.acc) || related(me, t);
        };
        auto& r = rooms[room];
        auto it = r.find(to);
        if (it != r.end() && ok(it->second)) return &it->second;
        if (me.links.empty()) return nullptr;
        for (auto& [rn, rr] : rooms) {
            auto x = rr.find(to);
            if (x != rr.end() && related(me, x->second)) return &x->second;
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
    // ---- people who joined with an invite: remembered by device key (and account), so they stay limited to their
    // links even if they later enter the server's address by hand. The admin can release them (invited-cmd file).
    struct Invited {
        std::string acc, name, id, by, room, hw;  // room: the invite room(s) it was invited into
        std::string byKey, byAcc;  // the inviter's device key and account (by = its current name)
        int64_t first = 0, last = 0;
        bool released = false;
    };
    std::map<std::string, Invited> invited;       // device key -> record
    std::map<std::string, std::string> owners;    // link token -> key of the device that created the invite
    bool invDirty = false;
    int64_t invSaved = 0;
    std::string invFile = opt_.dataDir.empty() ? std::string() : JoinPath(opt_.dataDir, "invited.json");
    std::string cmdFile = opt_.dataDir.empty() ? std::string() : JoinPath(opt_.dataDir, "invited-cmd");
    if (!invFile.empty()) {
        json ij;
        if (LoadJsonFile(invFile, ij) && ij.is_object()) {
            json dj = ij.value("devices", json::object()), oj = ij.value("owners", json::object());
            for (auto& [pub, v] : dj.items()) {
                if (!v.is_object() || !IsHex64(pub)) continue;
                Invited x;
                x.acc = v.value("acc", "");
                x.name = v.value("name", "");
                x.id = v.value("id", "");
                x.by = v.value("by", "");
                x.room = v.value("room", "");
                x.hw = v.value("hw", "");
                x.byKey = v.value("byKey", "");
                x.byAcc = v.value("byAcc", "");
                x.first = v.value("first", (int64_t)0);
                x.last = v.value("last", (int64_t)0);
                x.released = v.value("released", false);
                invited[pub] = x;
            }
            for (auto& [tok, pub] : oj.items())
                if (pub.is_string()) owners[tok] = pub.get<std::string>();
            json gj = ij.value("groups", json::object());
            for (auto& [owner, gs] : gj.items())
                if (gs.is_object())
                    for (auto& [g, toks] : gs.items())
                        if (toks.is_array())
                            for (auto& t : toks)
                                if (t.is_string()) groups[owner][g].insert(t.get<std::string>());
            json nj = ij.value("groupNames", json::object());
            for (auto& [k, n] : nj.items())
                if (n.is_string()) groupNames[k] = n.get<std::string>();
            rebuildGroups();
        }
    }
    auto saveInvited = [&](bool force) {
        if (!invDirty || invFile.empty() || (!force && NowMs() - invSaved < 60000)) return;
        json dj = json::object();
        for (auto& [pub, x] : invited)
            dj[pub] = {{"acc", x.acc}, {"name", x.name}, {"id", x.id}, {"by", x.by}, {"byKey", x.byKey}, {"byAcc", x.byAcc},
                       {"room", x.room}, {"hw", x.hw}, {"first", x.first}, {"last", x.last}, {"released", x.released}};
        MakeDirs(opt_.dataDir);
        json gj = json::object();
        for (auto& [owner, gs] : groups)
            for (auto& [g, toks] : gs) gj[owner][g] = toks;
        SaveJsonFile(invFile, json{{"v", 1}, {"devices", dj}, {"owners", owners}, {"groups", gj}, {"groupNames", groupNames}});
        invDirty = false;
        invSaved = NowMs();
    };
    // an invited device, another device of an invited account, or a new install on an invited computer / phone
    // (returns the record it matched, for the admin's list)
    auto restrictedBy = [&](const std::string& pub, const std::string& acc, const std::string& hw) -> const Invited* {
        auto it = invited.find(pub);
        if (it != invited.end()) return it->second.released ? nullptr : &it->second;
        for (auto& [p, x] : invited)
            if (!x.released && ((!acc.empty() && x.acc == acc) || (!hw.empty() && x.hw == hw))) return &x;
        return nullptr;
    };
    // admin commands: lines "release KEY", "restrict KEY", "forget KEY" (KEY: a device key or its first 8+ characters)
    bool cmdUnreadable = false;
    auto runAdmin = [&] {
        if (cmdFile.empty()) return;
        std::ifstream f(cmdFile);
        if (!f) {
            // (there but not readable: written by another user without giving it to this one - say so once)
            if (FileSize(cmdFile) >= 0 && !cmdUnreadable) log("can't read " + cmdFile + " (its owner / permissions): admin commands wait");
            cmdUnreadable = FileSize(cmdFile) >= 0;
            return;
        }
        cmdUnreadable = false;
        std::string line;
        std::vector<std::pair<std::string, std::string>> cmds;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string c, k;
            ss >> c >> k;
            for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
            if (k.size() >= 8) cmds.push_back({c, k});
        }
        f.close();
        std::remove(cmdFile.c_str());
        for (auto& [c, k] : cmds) {
            std::set<std::string> accs, hws;
            for (auto& [pub, x] : invited)
                if (pub.compare(0, k.size(), k) == 0) {
                    if (!x.acc.empty()) accs.insert(x.acc);
                    if (!x.hw.empty()) hws.insert(x.hw);
                }
            for (auto it = invited.begin(); it != invited.end();) {
                // (the whole account and every install on that computer / phone)
                bool hit = it->first.compare(0, k.size(), k) == 0 || accs.count(it->second.acc) || hws.count(it->second.hw);
                if (hit) log(c + " " + it->second.name + " (" + it->first.substr(0, 8) + ")");
                if (hit && c == "forget") { it = invited.erase(it); invDirty = true; continue; }
                if (hit && (c == "release" || c == "restrict")) { it->second.released = c == "release"; invDirty = true; }
                ++it;
            }
        }
        saveInvited(true);
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
            runAdmin();
            saveInvited(false);
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
            {
                std::string acc = Clip(j.value("acc", ""), 64), acs = Clip(j.value("acs", ""), 128);
                d.acc = !acc.empty() && IsHex64(d.pub) && VerifyData(acc, "device", id + "\n" + d.pub, acs) ? acc : std::string();
            }
            bool wasHidden = d.hidden, wasRestricted = d.restricted;
            std::vector<std::string> oldLinks = d.links;
            d.hidden = j.value("hidden", false);
            d.hw = IsHex64(j.value("hw", "")) ? j.value("hw", "") : std::string();
            std::vector<std::string> links, inv;
            for (auto& l : j.value("links", json::array()))
                if (l.is_string() && IsHex64(l.get<std::string>()) && links.size() < 500) links.push_back(l.get<std::string>());
            for (auto& l : j.value("inv", json::array()))
                if (l.is_string()) inv.push_back(l.get<std::string>());
            // whose invite is each link: the first device to register a code it made owns it; anyone else is invited
            const Invited* match = IsHex64(d.pub) ? restrictedBy(d.pub, d.acc, d.hw) : nullptr;
            bool restricted = match != nullptr;
            std::string inviter, inviteRoom;
            bool created = false;
            if (match && !invited.count(d.pub)) {
                // a new device key on a remembered computer / phone or account: listed too, so the admin sees it
                Invited x;
                x.first = NowMs();
                x.by = match->by;
                x.byKey = match->byKey;
                x.byAcc = match->byAcc;
                x.room = match->room;
                invited[d.pub] = x;
                created = true;
                log("remembered " + d.name + " (" + d.pub.substr(0, 8) + ") as invited: same device or account as before");
            }
            for (auto& l : links) {
                auto o = owners.find(l);
                bool received = std::find(inv.begin(), inv.end(), l) != inv.end() || (o != owners.end() && o->second != d.pub);
                if (received) {
                    if (o != owners.end()) inviter = o->second;
                    auto tg = tokGroup.find(l);
                    if (tg != tokGroup.end()) {
                        auto gn = groupNames.find(tg->second);
                        if (gn != groupNames.end() && !gn->second.empty())
                            inviteRoom += (inviteRoom.empty() ? "" : ", ") + gn->second;
                    }
                    if (IsHex64(d.pub) && !invited.count(d.pub)) {
                        Invited x;
                        x.first = NowMs();
                        invited[d.pub] = x;
                        restricted = created = true;
                        log("remembered " + d.name + " (" + d.pub.substr(0, 8) + ") as invited");
                    }
                } else if (o == owners.end() && !restricted && IsHex64(d.pub)) {
                    owners[l] = d.pub;
                    invDirty = true;
                }
            }
            if (IsHex64(d.pub) && invited.count(d.pub)) {
                auto& x = invited[d.pub];
                if (!inviter.empty() && x.room != inviteRoom) {  // the invite room(s) it is in now
                    x.room = inviteRoom;
                    invDirty = true;
                }
                if (x.name != d.name || x.acc != d.acc || x.id != id || x.hw != d.hw) invDirty = true;
                x.name = d.name;
                if (!d.acc.empty()) x.acc = d.acc;
                if (!d.hw.empty()) x.hw = d.hw;
                x.id = id;
                if (!inviter.empty() && x.byKey != inviter) {  // (first invite, or one from someone else later)
                    x.byKey = inviter;
                    invDirty = true;
                }
                if (!x.byKey.empty())
                    for (auto& [rn, rr] : rooms)
                        for (auto& [oid, od] : rr)
                            if (od.pub == x.byKey && (x.by != od.name || (!od.acc.empty() && x.byAcc != od.acc))) {
                                x.by = od.name;
                                if (!od.acc.empty()) x.byAcc = od.acc;
                                invDirty = true;
                            }
                if (NowMs() - x.last > 3600000) invDirty = true;
                x.last = NowMs();
                if (created) saveInvited(true);
            }
            if (!restricted && IsHex64(d.pub))
                for (auto& [p, x] : invited)
                    if (x.byKey == d.pub && (x.by != d.name || (!d.acc.empty() && x.byAcc != d.acc))) {
                        x.by = d.name;  // the inviter's current name
                        if (!d.acc.empty()) x.byAcc = d.acc;
                        invDirty = true;
                    }
            d.restricted = restricted;
            if (restricted) {
                // only links to someone else's invite count: no new invites of their own to this server
                d.hidden = true;
                links.erase(std::remove_if(links.begin(), links.end(), [&](const std::string& l) {
                                auto o = owners.find(l);
                                return o == owners.end() || o->second == d.pub;
                            }),
                            links.end());
            }
            d.links = links;
            bool linksChanged = d.links != oldLinks || d.hidden != wasHidden || d.restricted != wasRestricted;
            // the inviter's invite rooms: which of its codes belong together (only codes it owns count)
            std::vector<std::string> groupToks;  // (devices to tell when a room changed)
            if (IsHex64(d.pub) && !restricted) {
                std::map<std::string, std::set<std::string>> ng;
                for (auto& gj : j.value("groups", json::array())) {
                    if (!gj.is_object() || !IsHex64(gj.value("g", "")) || ng.size() >= 100) continue;
                    std::string key = d.pub + "/" + gj.value("g", ""), nm = Clip(gj.value("n", ""), 64);
                    if (groupNames[key] != nm) {
                        groupNames[key] = nm;
                        invDirty = true;
                    }
                    auto& set = ng[gj.value("g", "")];
                    for (auto& t : gj.value("t", json::array()))
                        if (t.is_string() && set.size() < 1000) {
                            auto o = owners.find(t.get<std::string>());
                            if (o != owners.end() && o->second == d.pub) set.insert(t.get<std::string>());
                        }
                }
                for (auto it = ng.begin(); it != ng.end();) it = it->second.empty() ? ng.erase(it) : std::next(it);
                auto old = groups.find(d.pub);
                bool had = old != groups.end();
                if ((had && old->second != ng) || (!had && !ng.empty())) {
                    if (had)
                        for (auto& [g, toks] : old->second) groupToks.insert(groupToks.end(), toks.begin(), toks.end());
                    for (auto& [g, toks] : ng) groupToks.insert(groupToks.end(), toks.begin(), toks.end());
                    if (had)
                        for (auto& [g, toks] : old->second)
                            if (!ng.count(g)) groupNames.erase(d.pub + "/" + g);  // a deleted room
                    if (ng.empty()) groups.erase(d.pub);
                    else groups[d.pub] = ng;
                    rebuildGroups();
                    invDirty = true;
                    saveInvited(true);
                }
            }
            if (opt_.mailbox && IsHex64(d.pub))
                for (auto& l : d.links) {
                    auto& ts = linkPubs[l][d.pub];
                    if (NowMs() - ts > 3600000) mailDirty = true;  // (saved with the mailbox now and then)
                    ts = NowMs();
                }
            d.lastSeen = now;
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
            if (d.restricted) reply["inv"] = true;
            else if (IsHex64(d.pub)) {  // released by the admin: the app may stop hiding this server from its user
                bool rel = false;
                for (auto& [p, x] : invited)
                    if (x.released && (p == d.pub || (!d.acc.empty() && x.acc == d.acc) || (!d.hw.empty() && x.hw == d.hw))) rel = true;
                if (rel) reply["rel"] = true;
            }
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
            if (!groupToks.empty()) pushAround(room, "", groupToks);  // a room changed: its members see the new list
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
            if (me->restricted) {  // only to its links and its own account
                bool mine = false;
                for (auto& [rn, rr] : rooms)
                    for (auto& [did, d] : rr) mine |= d.pub == to && !me->acc.empty() && d.acc == me->acc;
                inRoom = mine;
            }
            if (!inRoom)
                for (auto& l : reach(me->links)) {
                    auto lit = linkPubs.find(l);
                    if (lit != linkPubs.end() && lit->second.count(to)) key = "L/" + to;  // a linked device
                }
            if (me->restricted && !inRoom && key[0] != 'L') continue;  // not one of its links
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
            if (me->restricted) {
                sendJson(from, {{"t", "uerr"}, {"name", name}, {"msg", "not allowed"}});
                continue;
            }
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
            if (me->restricted) {
                sendJson(from, {{"t", "uerr"}, {"name", name}, {"msg", "not allowed"}});
                continue;
            }
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
