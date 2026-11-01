#include "mdns.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "platform.h"

#if defined(__APPLE__)
#include <dns_sd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#define LS_MDNS_DNSSD 1
#elif defined(_WIN32)
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
#include <unistd.h>
#endif

namespace ls {
namespace mdns {

// ============================================================================ packet encoding

static void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
static void Put32(std::vector<uint8_t>& b, uint32_t v) { Put16(b, (uint16_t)(v >> 16)); Put16(b, (uint16_t)v); }

// Splits on '.', but the first label of an instance name may itself contain dots: callers pass it pre-split.
static void PutName(std::vector<uint8_t>& b, const std::vector<std::string>& labels) {
    for (auto& l : labels) {
        size_t n = std::min<size_t>(l.size(), 63);
        b.push_back((uint8_t)n);
        b.insert(b.end(), l.begin(), l.begin() + (ptrdiff_t)n);
    }
    b.push_back(0);
}

static std::vector<std::string> Split(const std::string& name) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : name) {
        if (c == '.') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Instance names are "<label>._lunashare._tcp.local" where <label> may contain dots.
static std::vector<std::string> InstanceLabels(const std::string& instance) {
    std::string suffix = std::string(".") + kServiceType;
    if (instance.size() > suffix.size() && NameEq(instance.substr(instance.size() - suffix.size()), suffix)) {
        std::vector<std::string> v{instance.substr(0, instance.size() - suffix.size())};
        for (auto& l : Split(kServiceType)) v.push_back(l);
        return v;
    }
    return Split(instance);
}

static void PutRecordHeader(std::vector<uint8_t>& b, const std::vector<std::string>& name, uint16_t type, bool flush, uint32_t ttl) {
    PutName(b, name);
    Put16(b, type);
    Put16(b, (uint16_t)(flush ? 0x8001 : 0x0001));
    Put32(b, ttl);
}

static void PutRdata(std::vector<uint8_t>& b, const std::vector<uint8_t>& rd) {
    Put16(b, (uint16_t)rd.size());
    b.insert(b.end(), rd.begin(), rd.end());
}

std::string InstanceLabel(const std::string& displayName, const std::string& id) {
    std::string n;
    for (char c : displayName)
        if ((unsigned char)c >= 32) n += c;
    std::string tail = " (" + id.substr(0, 8) + ")";
    if (n.size() + tail.size() > 63) {
        n.resize(63 - tail.size());
        while (!n.empty() && ((unsigned char)n.back() & 0xC0) == 0x80) n.pop_back();  // don't cut UTF-8 in half
        if (!n.empty() && ((unsigned char)n.back() & 0x80)) n.pop_back();
    }
    return (n.empty() ? "LunaShare" : n) + tail;
}

bool NameEq(const std::string& a, const std::string& b) {
    std::string x = a, y = b;
    while (!x.empty() && x.back() == '.') x.pop_back();
    while (!y.empty() && y.back() == '.') y.pop_back();
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); i++)
        if (tolower((unsigned char)x[i]) != tolower((unsigned char)y[i])) return false;
    return true;
}

std::vector<uint8_t> BuildResponse(const Service& s, const std::vector<std::string>& ips, uint32_t ttl, uint16_t id,
                                   const std::vector<Question>* echo) {
    std::vector<uint8_t> b;
    Put16(b, id);
    Put16(b, 0x8400);  // response, authoritative
    Put16(b, (uint16_t)(echo ? echo->size() : 0));
    Put16(b, 1);       // answer: PTR
    Put16(b, 0);
    Put16(b, (uint16_t)(2 + ips.size()));  // additional: SRV, TXT, A...
    if (echo)
        for (auto& q : *echo) {
            PutName(b, Split(q.name));
            Put16(b, q.type);
            Put16(b, 1);
        }
    std::vector<std::string> svc = Split(kServiceType), inst = InstanceLabels(s.instance), host = Split(s.host);
    bool legacy = echo != nullptr;  // legacy unicast replies must not set the cache-flush bit
    uint32_t ttlShort = std::min<uint32_t>(ttl, legacy ? 10 : ttl);
    // PTR
    PutRecordHeader(b, svc, PTR, false, ttlShort);
    {
        std::vector<uint8_t> rd;
        PutName(rd, inst);
        PutRdata(b, rd);
    }
    // SRV
    PutRecordHeader(b, inst, SRV, !legacy, ttlShort);
    {
        std::vector<uint8_t> rd;
        Put16(rd, 0);
        Put16(rd, 0);
        Put16(rd, s.port);
        PutName(rd, host);
        PutRdata(b, rd);
    }
    // TXT
    PutRecordHeader(b, inst, TXT, !legacy, ttlShort);
    {
        std::vector<uint8_t> rd;
        for (auto& [k, v] : s.txt) {
            std::string e = k + "=" + v;
            if (e.size() > 255) e.resize(255);
            rd.push_back((uint8_t)e.size());
            rd.insert(rd.end(), e.begin(), e.end());
        }
        if (rd.empty()) rd.push_back(0);
        PutRdata(b, rd);
    }
    // A
    for (auto& ip : ips) {
        in_addr a{};
        if (inet_pton(AF_INET, ip.c_str(), &a) != 1) continue;
        PutRecordHeader(b, host, A, !legacy, ttlShort);
        std::vector<uint8_t> rd(4);
        memcpy(rd.data(), &a, 4);
        PutRdata(b, rd);
    }
    return b;
}

std::vector<uint8_t> BuildQuery(const std::string& name, uint16_t type) {
    std::vector<uint8_t> b;
    Put16(b, 0);
    Put16(b, 0);
    Put16(b, 1);
    Put16(b, 0);
    Put16(b, 0);
    Put16(b, 0);
    PutName(b, Split(name));
    Put16(b, type);
    Put16(b, 1);
    return b;
}

// ============================================================================ packet decoding

static bool ReadName(const uint8_t* p, size_t n, size_t& off, std::string& out) {
    out.clear();
    size_t pos = off;
    bool jumped = false;
    int hops = 0;
    while (true) {
        if (pos >= n) return false;
        uint8_t len = p[pos];
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= n || ++hops > 32) return false;
            size_t ptr = ((size_t)(len & 0x3F) << 8) | p[pos + 1];
            if (!jumped) off = pos + 2;
            jumped = true;
            pos = ptr;
            continue;
        }
        if (len & 0xC0) return false;
        pos++;
        if (len == 0) break;
        if (pos + len > n) return false;
        if (!out.empty()) out += '.';
        out.append((const char*)p + pos, len);
        pos += len;
        if (out.size() > 1024) return false;
    }
    if (!jumped) off = pos;
    return true;
}

bool Parse(const uint8_t* p, size_t n, Packet& out) {
    out = Packet();
    if (n < 12) return false;
    auto g16 = [&](size_t o) { return (uint16_t)(p[o] << 8 | p[o + 1]); };
    out.id = g16(0);
    out.flags = g16(2);
    uint16_t qd = g16(4), an = g16(6), ns = g16(8), ar = g16(10);
    size_t off = 12;
    for (int i = 0; i < qd; i++) {
        Question q;
        if (!ReadName(p, n, off, q.name) || off + 4 > n) return false;
        q.type = g16(off);
        q.cls = g16(off + 2);
        off += 4;
        out.questions.push_back(q);
    }
    int total = an + ns + ar;
    for (int i = 0; i < total; i++) {
        Record r;
        if (!ReadName(p, n, off, r.name) || off + 10 > n) return false;
        r.type = g16(off);
        r.cls = g16(off + 2);
        r.ttl = (uint32_t)g16(off + 4) << 16 | g16(off + 6);
        uint16_t rdlen = g16(off + 8);
        off += 10;
        if (off + rdlen > n) return false;
        size_t rd = off, end = off + rdlen;
        switch (r.type) {
            case PTR: {
                size_t o = rd;
                ReadName(p, n, o, r.target);
                break;
            }
            case SRV: {
                if (rdlen >= 7) {
                    r.port = g16(rd + 4);
                    size_t o = rd + 6;
                    ReadName(p, n, o, r.target);
                }
                break;
            }
            case TXT: {
                size_t o = rd;
                while (o < end) {
                    uint8_t l = p[o++];
                    if (o + l > end) break;
                    std::string e((const char*)p + o, l);
                    o += l;
                    size_t eq = e.find('=');
                    if (eq != std::string::npos) r.txt[e.substr(0, eq)] = e.substr(eq + 1);
                    else if (!e.empty()) r.txt[e] = "";
                }
                break;
            }
            case A: {
                if (rdlen == 4) {
                    char buf[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, p + rd, buf, sizeof(buf));
                    r.ip = buf;
                }
                break;
            }
            default: break;
        }
        off = end;
        out.records.push_back(std::move(r));
    }
    return true;
}

std::vector<MdnsPeer> ExtractPeers(const Packet& pkt, const std::string& fromIp) {
    std::vector<MdnsPeer> out;
    std::string suffix = std::string(".") + kServiceType;
    for (auto& srv : pkt.records) {
        if (srv.type != SRV) continue;
        if (srv.name.size() <= suffix.size() || !NameEq(srv.name.substr(srv.name.size() - suffix.size()), suffix)) continue;
        const Record* txt = nullptr;
        std::vector<std::string> addrs;
        for (auto& r : pkt.records) {
            if (r.type == TXT && NameEq(r.name, srv.name)) txt = &r;
            if (r.type == A && NameEq(r.name, srv.target) && !r.ip.empty()) addrs.push_back(r.ip);
        }
        if (!txt) continue;
        auto id = txt->txt.find("id");
        if (id == txt->txt.end() || id->second.empty()) continue;
        MdnsPeer p;
        p.id = id->second;
        auto nm = txt->txt.find("name");
        p.name = nm != txt->txt.end() ? nm->second : srv.name.substr(0, srv.name.size() - suffix.size());
        auto pf = txt->txt.find("pf");
        if (pf != txt->txt.end()) p.platform = pf->second;
        p.port = srv.port;
        p.goodbye = srv.ttl == 0;
        // The sender's own address on our link is the most reliable; fall back to its A records.
        if (std::find(addrs.begin(), addrs.end(), fromIp) != addrs.end() || addrs.empty()) p.ip = fromIp;
        else p.ip = addrs[0];
        if (!p.ip.empty() && (p.port || p.goodbye)) out.push_back(p);
    }
    return out;
}

bool ShouldAnswer(const Question& q, const Service& s) {
    if ((q.type == PTR || q.type == ANY) && NameEq(q.name, kServiceType)) return true;
    if ((q.type == PTR || q.type == ANY) && NameEq(q.name, "_services._dns-sd._udp.local")) return true;
    if ((q.type == SRV || q.type == TXT || q.type == ANY) && NameEq(q.name, s.instance)) return true;
    if ((q.type == A || q.type == ANY) && NameEq(q.name, s.host)) return true;
    return false;
}

}  // namespace mdns

// ============================================================================ runtime

std::string Mdns::Status() {
    std::lock_guard<std::mutex> l(m_);
    return status_;
}

void Mdns::SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(m_);
    status_ = s;
}

bool Mdns::Start(const std::string& id, const std::string& name, const std::string& platform, uint16_t port, Found cb) {
    Stop();
    {
        std::lock_guard<std::mutex> l(m_);
        id_ = id;
        name_ = name;
        platform_ = platform;
        port_ = port;
    }
    cb_ = std::move(cb);
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void Mdns::Stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (thread_.joinable()) thread_.join();
}

void Mdns::Update(const std::string& name, uint16_t port) {
    std::lock_guard<std::mutex> l(m_);
    if (name == name_ && port == port_) return;
    name_ = name;
    port_ = port;
    reannounce_ = true;
}

#if defined(LS_MDNS_DNSSD)
// ============================================================================ Apple: dns_sd

namespace {
struct DnssdCtx;
struct Pending {
    DnssdCtx* owner = nullptr;
    std::string id, name, platform;
    uint16_t port = 0;
    DNSServiceRef ref = nullptr;
    bool done = false;
};
struct DnssdCtx {
    Mdns::Found cb;
    std::string myId;
    DNSServiceRef conn = nullptr;
    std::vector<DNSServiceRef> finished;  // subordinate refs to free after DNSServiceProcessResult returns
    std::vector<Pending*> pending;
    std::map<std::string, std::string> instanceToId;  // browse name -> device id (for removal events)
};
struct ResolveCtx {
    DnssdCtx* owner;
    std::string instance;
};

std::string TxtValue(uint16_t len, const unsigned char* txt, const char* key) {
    uint8_t vlen = 0;
    const void* v = TXTRecordGetValuePtr(len, txt, key, &vlen);
    return v ? std::string((const char*)v, vlen) : std::string();
}

void DNSSD_API AddrCb(DNSServiceRef, DNSServiceFlags flags, uint32_t, DNSServiceErrorType err, const char*, const struct sockaddr* addr,
                      uint32_t, void* ctx) {
    auto* p = (Pending*)ctx;
    if (err == kDNSServiceErr_NoError && addr && addr->sa_family == AF_INET) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((const sockaddr_in*)addr)->sin_addr, ip, sizeof(ip));
        MdnsPeer peer{p->id, p->name, p->platform, ip, p->port};
        if (p->owner->cb) p->owner->cb(peer);
    }
    if (!(flags & kDNSServiceFlagsMoreComing)) p->done = true;
}

void DNSSD_API ResolveCb(DNSServiceRef ref, DNSServiceFlags, uint32_t ifIndex, DNSServiceErrorType err, const char*, const char* host,
                         uint16_t portNet, uint16_t txtLen, const unsigned char* txt, void* ctx) {
    ResolveCtx* rc = (ResolveCtx*)ctx;
    DnssdCtx* c = rc->owner;
    std::string instance = rc->instance;
    delete rc;
    c->finished.push_back(ref);  // one answer is enough
    if (err != kDNSServiceErr_NoError) return;
    auto* p = new Pending();
    p->owner = c;
    p->id = TxtValue(txtLen, txt, "id");
    p->name = TxtValue(txtLen, txt, "name");
    p->platform = TxtValue(txtLen, txt, "pf");
    p->port = ntohs(portNet);
    if (p->id.empty() || p->id == c->myId) { delete p; return; }
    c->instanceToId[instance] = p->id;
    p->ref = c->conn;
    if (DNSServiceGetAddrInfo(&p->ref, kDNSServiceFlagsShareConnection, ifIndex, kDNSServiceProtocol_IPv4, host, AddrCb, p) ==
        kDNSServiceErr_NoError)
        c->pending.push_back(p);
    else
        delete p;
}

void DNSSD_API BrowseCb(DNSServiceRef, DNSServiceFlags flags, uint32_t ifIndex, DNSServiceErrorType err, const char* name,
                        const char* type, const char* domain, void* ctx) {
    DnssdCtx* c = (DnssdCtx*)ctx;
    if (err != kDNSServiceErr_NoError) return;
    if (!(flags & kDNSServiceFlagsAdd)) {  // service went away (goodbye or expired)
        auto it = c->instanceToId.find(name);
        if (it != c->instanceToId.end() && c->cb) {
            MdnsPeer gone;
            gone.id = it->second;
            gone.goodbye = true;
            c->instanceToId.erase(it);
            c->cb(gone);
        }
        return;
    }
    DNSServiceRef r = c->conn;
    auto* rc = new ResolveCtx{c, name};
    if (DNSServiceResolve(&r, kDNSServiceFlagsShareConnection, ifIndex, name, type, domain, ResolveCb, rc) != kDNSServiceErr_NoError) delete rc;
}
}  // namespace

void Mdns::Loop() {
    DnssdCtx ctx;
    {
        std::lock_guard<std::mutex> l(m_);
        ctx.myId = id_;
    }
    std::string myId = ctx.myId;
    Found user = cb_;
    ctx.cb = [user, myId](const MdnsPeer& p) { if (p.id != myId && user) user(p); };
    if (DNSServiceCreateConnection(&ctx.conn) != kDNSServiceErr_NoError) {
        SetStatus("Bonjour unavailable");
        return;
    }
    DNSServiceRef reg = nullptr, browse = nullptr;
    auto doRegister = [&] {
        if (reg) { DNSServiceRefDeallocate(reg); reg = nullptr; }
        std::string id, name, pf;
        uint16_t port;
        {
            std::lock_guard<std::mutex> l(m_);
            id = id_; name = name_; pf = platform_; port = port_;
        }
        TXTRecordRef txt;
        TXTRecordCreate(&txt, 0, nullptr);
        std::string v = "2";
        TXTRecordSetValue(&txt, "id", (uint8_t)id.size(), id.data());
        TXTRecordSetValue(&txt, "v", (uint8_t)v.size(), v.data());
        TXTRecordSetValue(&txt, "name", (uint8_t)std::min<size_t>(name.size(), 200), name.data());
        TXTRecordSetValue(&txt, "pf", (uint8_t)pf.size(), pf.data());
        reg = ctx.conn;
        std::string label = mdns::InstanceLabel(name, id);
        DNSServiceErrorType e = DNSServiceRegister(&reg, kDNSServiceFlagsShareConnection, 0, label.c_str(), "_lunashare._tcp", nullptr,
                                                   nullptr, htons(port), TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt), nullptr, nullptr);
        TXTRecordDeallocate(&txt);
        if (e != kDNSServiceErr_NoError) reg = nullptr;
        SetStatus(e == kDNSServiceErr_NoError ? "Bonjour: advertising" : "Bonjour register failed (" + std::to_string(e) + ")");
    };
    doRegister();
    browse = ctx.conn;
    DNSServiceBrowse(&browse, kDNSServiceFlagsShareConnection, 0, "_lunashare._tcp", nullptr, BrowseCb, &ctx);
    int fd = DNSServiceRefSockFD(ctx.conn);
    int64_t lastRestart = SteadyMs();
    while (running_) {
        if (reannounce_.exchange(false)) doRegister();
        if (queryNow_.exchange(false) || SteadyMs() - lastRestart > 60000) {
            // restart the browse so already-known services are reported again (cheap; the daemon caches)
            if (browse) DNSServiceRefDeallocate(browse);
            browse = ctx.conn;
            DNSServiceBrowse(&browse, kDNSServiceFlagsShareConnection, 0, "_lunashare._tcp", nullptr, BrowseCb, &ctx);
            lastRestart = SteadyMs();
        }
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 250) > 0 && (p.revents & POLLIN)) {
            if (DNSServiceProcessResult(ctx.conn) != kDNSServiceErr_NoError) break;
            for (auto r : ctx.finished) DNSServiceRefDeallocate(r);
            ctx.finished.clear();
            for (auto it = ctx.pending.begin(); it != ctx.pending.end();) {
                if ((*it)->done) { DNSServiceRefDeallocate((*it)->ref); delete *it; it = ctx.pending.erase(it); }
                else ++it;
            }
        }
    }
    for (auto* pp : ctx.pending) { DNSServiceRefDeallocate(pp->ref); delete pp; }
    if (browse) DNSServiceRefDeallocate(browse);
    if (reg) DNSServiceRefDeallocate(reg);
    DNSServiceRefDeallocate(ctx.conn);
}

#else
// ============================================================================ built-in responder / browser

namespace {
using sock_t = decltype(socket(0, 0, 0));

bool JoinGroup(sock_t s, const std::string& ifIp) {
    ip_mreq m{};
    inet_pton(AF_INET, mdns::kGroup, &m.imr_multiaddr);
    if (ifIp.empty()) m.imr_interface.s_addr = htonl(INADDR_ANY);
    else inet_pton(AF_INET, ifIp.c_str(), &m.imr_interface);
    return setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof(m)) == 0;
}

void SendTo(sock_t s, const std::vector<uint8_t>& b, const std::string& ip, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    sendto(s, (const char*)b.data(), (int)b.size(), 0, (sockaddr*)&a, sizeof(a));
}
}  // namespace

void Mdns::Loop() {
    NetInit();
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#if defined(SO_REUSEPORT) && !defined(_WIN32)
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char*)&one, sizeof(one));
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(mdns::kPort);
    bool responder = bind(s, (sockaddr*)&a, sizeof(a)) == 0;
    if (!responder) {
        // Port 5353 owned exclusively by another stack: we can still browse using legacy unicast queries
        // (answers come back to our ephemeral port), we just cannot answer other devices.
        a.sin_port = 0;
        bind(s, (sockaddr*)&a, sizeof(a));
    }
    unsigned char ttl = 255, loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
    std::vector<std::string> joined;
    auto refreshInterfaces = [&]() -> std::vector<std::string> {
        std::vector<std::string> ips;
        for (auto& la : LocalIPv4Addresses()) ips.push_back(la.ip);
        if (responder) {
            if (joined.empty()) JoinGroup(s, "");
            for (auto& ip : ips)
                if (std::find(joined.begin(), joined.end(), ip) == joined.end()) {
                    JoinGroup(s, ip);
                    joined.push_back(ip);
                }
        }
        return ips;
    };
    std::vector<std::string> ips = refreshInterfaces();
    SetStatus(responder ? "mDNS: advertising and browsing" : "mDNS: browsing only (port 5353 is busy)");

    auto service = [&]() {
        mdns::Service sv;
        std::lock_guard<std::mutex> l(m_);
        sv.instance = mdns::InstanceLabel(name_, id_) + "." + mdns::kServiceType;
        sv.host = "ls-" + id_.substr(0, 8) + ".local";
        sv.port = port_;
        sv.txt = {{"id", id_}, {"v", "2"}, {"name", name_.substr(0, 200)}, {"pf", platform_}};
        return sv;
    };
    // Send on every interface (otherwise only the default route's link gets the packet).
    auto multicast = [&](const std::vector<uint8_t>& b) {
        if (ips.empty()) {
            SendTo(s, b, mdns::kGroup, mdns::kPort);
            return;
        }
        for (auto& ip : ips) {
            in_addr ifa{};
            inet_pton(AF_INET, ip.c_str(), &ifa);
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifa, sizeof(ifa));
            SendTo(s, b, mdns::kGroup, mdns::kPort);
        }
    };
    auto announce = [&](uint32_t ttlSec) {
        if (responder) multicast(mdns::BuildResponse(service(), ips, ttlSec));
    };
    auto query = [&]() { multicast(mdns::BuildQuery(mdns::kServiceType, mdns::PTR)); };

    std::string myId;
    {
        std::lock_guard<std::mutex> l(m_);
        myId = id_;
    }
    announce(120);
    query();
    int64_t start = SteadyMs(), lastAnnounce = start, lastQuery = start, lastIfCheck = start;
    int queryStep = 0;
    const int64_t schedule[] = {1000, 3000, 9000, 30000};
    int announced = 1;
    uint8_t buf[9000];
    while (running_) {
        int64_t now = SteadyMs();
        if (now - lastIfCheck > 10000) {
            ips = refreshInterfaces();
            lastIfCheck = now;
        }
        if (reannounce_.exchange(false) || (announced < 2 && now - start > 1000) || now - lastAnnounce > 100000) {
            announce(120);
            announced++;
            lastAnnounce = now;
        }
        int64_t wait = schedule[std::min(queryStep, 3)];
        if (queryNow_.exchange(false) || now - lastQuery > wait) {
            query();
            lastQuery = now;
            queryStep++;
        }
        if (WaitReadable((SocketHandle)s, 250) != 1) continue;
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int n = (int)recvfrom(s, (char*)buf, (int)sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        char fip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, fip, sizeof(fip));
        uint16_t fport = ntohs(from.sin_port);
        mdns::Packet pkt;
        if (!mdns::Parse(buf, (size_t)n, pkt)) continue;
        if (pkt.IsResponse()) {
            for (auto& p : mdns::ExtractPeers(pkt, fip))
                if (p.id != myId && cb_) cb_(p);
        } else if (responder) {
            mdns::Service sv = service();
            std::vector<mdns::Question> asked;
            for (auto& q : pkt.questions)
                if (mdns::ShouldAnswer(q, sv)) asked.push_back(q);
            if (asked.empty()) continue;
            if (fport != mdns::kPort) {
                SendTo(s, mdns::BuildResponse(sv, ips, 10, pkt.id, &asked), fip, fport);  // legacy unicast query
            } else if (SteadyMs() - lastAnnounce > 1000) {  // RFC 6762: at most one multicast answer per second
                multicast(mdns::BuildResponse(sv, ips, 120));
                lastAnnounce = SteadyMs();
            }
        }
    }
    announce(0);  // goodbye
    CloseSocket((SocketHandle)s);
}
#endif

}  // namespace ls
