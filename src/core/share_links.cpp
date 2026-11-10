#include "share_links.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <nlohmann/json.hpp>

#include "crypto.h"
#include "hash.h"
#include "link_crypto.h"
#include "link_page.h"
#include "model.h"
#include "platform.h"

namespace ls {

using json = nlohmann::json;

static constexpr int kMaxPinTries = 5;
static constexpr int64_t kAskTimeoutMs = 10 * 60 * 1000;   // an unanswered "ask me first" is a no
static constexpr int64_t kGraceMs = 6 * 3600 * 1000LL;     // who started before the end may finish
static constexpr int64_t kKeepMs = 24 * 3600 * 1000LL;     // ended links stay listed this long
static constexpr uint32_t kMaxTunFrame = 4u << 20;

struct ShareLinks::Link {
    std::string id, tok, path, name, pin, sha256;
    uint8_t secret[16] = {};
    LinkKeys keys{};
    int64_t size = 0, mtime = 0, created = 0, expires = 0;
    int maxPeople = 1, people = 0, pinFails = 0;
    bool ask = false, revoked = false, ready = false;
    std::vector<ShareVisit> visits;

    bool Unchanged() const { return FileSize(path) == size && FileMTime(path) == mtime; }
    int64_t Parts() const { return std::max<int64_t>(1, (size + kLinkChunk - 1) / kLinkChunk); }
    ShareVisit* Visit(const std::string& sid) {
        for (auto& v : visits)
            if (v.sid == sid) return &v;
        return nullptr;
    }
};

static bool ValidSid(const std::string& s) {
    if (s.size() < 8 || s.size() > 64) return false;
    for (char c : s)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') return false;
    return true;
}

static std::string RandomB64(int bytes) {
    uint8_t b[32];
    SecureRandom(b, bytes);
    return B64Url(b, bytes);
}

static std::string JsonReply(const json& j) { return j.dump(); }

void ShareLinks::Start(const std::string& stateDir, uint16_t lanPort) {
    if (running_) return;
    dir_ = stateDir;
    cancel_ = false;
    Load();
    uint16_t bound = 0;
    lanSock_ = TcpListen(lanPort, &bound);
    if (lanSock_ == kInvalidSocket) lanSock_ = TcpListen(0, &bound);
    lanPort_ = lanSock_ == kInvalidSocket ? 0 : bound;
    running_ = true;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& [id, l] : links_)
            if (!l->ready && !l->revoked) HashLater(l);
    }
    if (lanSock_ != kInvalidSocket) lanThread_ = std::thread([this] { LanLoop(); });
    tunThread_ = std::thread([this] { TunnelLoop(); });
}

void ShareLinks::Stop() {
    if (!running_) return;
    running_ = false;
    cancel_ = true;
    if (lanSock_ != kInvalidSocket) {
        ShutdownSocket(lanSock_);
        CloseSocket(lanSock_);
    }
    {
        std::lock_guard<std::mutex> lk(sendM_);
        if (tunSock_ != kInvalidSocket) ShutdownSocket(tunSock_);
    }
    cv_.notify_all();
    if (lanThread_.joinable()) lanThread_.join();
    if (tunThread_.joinable()) tunThread_.join();
    for (;;) {
        std::vector<std::thread> t;
        {
            std::lock_guard<std::mutex> lk(m_);
            t.swap(hashers_);
        }
        if (t.empty()) break;
        for (auto& h : t)
            if (h.joinable()) h.join();
    }
    for (int i = 0; i < 300 && (lanConns_ > 0 || workers_ > 0); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    lanSock_ = kInvalidSocket;
    std::lock_guard<std::mutex> lk(m_);
    if (dirty_) Save();
}

void ShareLinks::SetServer(const std::string& hostPort, const std::string& webBase) {
    std::lock_guard<std::mutex> lk(m_);
    if (server_ == hostPort && webBase_ == webBase) return;
    server_ = hostPort;
    webBase_ = webBase;
    std::lock_guard<std::mutex> sk(sendM_);
    if (tunSock_ != kInvalidSocket) ShutdownSocket(tunSock_);  // reconnect to the new one
    cv_.notify_all();
}

bool ShareLinks::WebAvailable() {
    std::lock_guard<std::mutex> lk(m_);
    return !webBase_.empty() && !server_.empty();
}

std::string ShareLinks::TunnelStatus() {
    std::lock_guard<std::mutex> lk(m_);
    return tunnelStatus_;
}

std::string ShareLinks::LanBase() {
    if (!lanPort_) return "";
    std::string best, other;
    for (auto& a : LocalIPv4Addresses()) {
        const std::string& ip = a.ip;
        int b1 = 0, b2 = 0;
        if (sscanf(ip.c_str(), "%d.%d", &b1, &b2) != 2 || b1 == 127 || b1 == 0 || (b1 == 169 && b2 == 254)) continue;
        bool priv = b1 == 10 || (b1 == 192 && b2 == 168) || (b1 == 172 && b2 >= 16 && b2 < 32);
        if (!priv) {
            if (other.empty()) other = ip;  // (no private address: whatever this device has)
            continue;
        }
        if (best.empty() || (b1 == 192 && best.rfind("192.", 0) != 0)) best = ip;  // home networks first
    }
    if (best.empty()) best = other;
    return best.empty() ? "" : "http://" + best + ":" + std::to_string(lanPort_);
}

std::string ShareLinks::Create(const std::string& path, const Options& o, std::string& err) {
    if (!FileExists(path) || IsDirectory(path)) {
        err = "not a file";
        return "";
    }
    auto l = std::make_shared<Link>();
    l->path = path;
    l->name = BaseName(path);
    l->size = FileSize(path);
    l->mtime = FileMTime(path);
    l->created = NowMs();
    l->expires = l->created + std::max<int64_t>(60 * 1000, o.validMs);
    l->maxPeople = std::max(0, o.maxPeople);
    l->pin = o.pin;
    l->ask = o.ask;
    SecureRandom(l->secret, sizeof(l->secret));
    l->keys = DeriveLinkKeys(l->secret, sizeof(l->secret));
    std::lock_guard<std::mutex> lk(m_);
    do l->id = RandomB64(9);  // 12 characters
    while (links_.count(l->id));
    l->tok = RandomB64(12);
    links_[l->id] = l;
    linksChanged_ = true;
    Save();
    HashLater(l);
    cv_.notify_all();
    return l->id;
}

void ShareLinks::HashLater(std::shared_ptr<Link> l) {
    // (m_ held)
    hashers_.erase(std::remove_if(hashers_.begin(), hashers_.end(), [](std::thread& t) { return !t.joinable(); }), hashers_.end());
    hashers_.emplace_back([this, l] {
        std::string h = Sha256File(l->path, nullptr, &cancel_);
        std::lock_guard<std::mutex> lk(m_);
        if (h.empty()) return;
        if (FileSize(l->path) != l->size || FileMTime(l->path) != l->mtime) {
            // changed while we read it: take the new one (only while nobody got it yet)
            if (!l->visits.empty()) return;
            l->size = FileSize(l->path);
            l->mtime = FileMTime(l->path);
            HashLater(l);
            return;
        }
        l->sha256 = h;
        l->ready = true;
        Save();
    });
}

void ShareLinks::Revoke(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = links_.find(id);
    if (it == links_.end()) return;
    it->second->revoked = true;
    for (auto a = asks_.begin(); a != asks_.end();) a = a->link == id ? asks_.erase(a) : a + 1;
    linksChanged_ = true;
    Save();
}

void ShareLinks::Remove(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_);
    if (!links_.erase(id)) return;
    for (auto a = asks_.begin(); a != asks_.end();) a = a->link == id ? asks_.erase(a) : a + 1;
    linksChanged_ = true;
    Save();
}

std::vector<ShareLinkInfo> ShareLinks::List() {
    std::string lan = LanBase();
    std::lock_guard<std::mutex> lk(m_);
    std::vector<ShareLinkInfo> out;
    for (auto& [id, l] : links_) {
        ShareLinkInfo i;
        i.id = id;
        i.name = l->name;
        i.path = l->path;
        if (!webBase_.empty()) i.webUrl = webBase_ + "/s/" + id + "#" + B64Url(l->secret, sizeof(l->secret));
        if (!lan.empty()) i.lanUrl = lan + "/f/" + l->tok;
        i.size = l->size;
        i.created = l->created;
        i.expires = l->expires;
        i.maxPeople = l->maxPeople;
        i.people = l->people;
        i.pin = !l->pin.empty();
        i.ask = l->ask;
        i.revoked = l->revoked;
        i.ready = l->ready;
        i.sha256 = l->sha256;
        i.visits = l->visits;
        out.push_back(std::move(i));
    }
    std::sort(out.begin(), out.end(), [](const ShareLinkInfo& a, const ShareLinkInfo& b) { return a.created > b.created; });
    return out;
}

std::vector<ShareAsk> ShareLinks::Asks() {
    std::lock_guard<std::mutex> lk(m_);
    return asks_;
}

void ShareLinks::Answer(uint64_t askId, bool allow) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = asks_.begin(); it != asks_.end(); ++it) {
        if (it->id != askId) continue;
        auto li = links_.find(it->link);
        if (li != links_.end()) {
            Link& l = *li->second;
            if (ShareVisit* v = l.Visit(it->sid); v && v->state == 1) {
                if (allow && !l.revoked && (l.maxPeople == 0 || l.people < l.maxPeople)) {
                    v->state = 0;
                    l.people++;
                } else {
                    v->state = 2;
                }
            }
        }
        asks_.erase(it);
        Save();
        return;
    }
}

std::shared_ptr<ShareLinks::Link> ShareLinks::Find(const std::string& id, bool lanToken) {
    // (m_ held)
    if (!lanToken) {
        auto it = links_.find(id);
        return it == links_.end() ? nullptr : it->second;
    }
    for (auto& [k, l] : links_)
        if (l->tok == id) return l;
    return nullptr;
}

int ShareLinks::Gate(Link& l, const std::string& sid, const std::string& pinProof, const std::string& ip, const std::string& via,
                     Reply& r) {
    // (m_ held)
    auto fail = [&](int status, json j) {
        r.status = status;
        r.type = "application/json";
        r.body = JsonReply(j);
        return status;
    };
    int64_t now = NowMs();
    if (!ValidSid(sid)) return fail(400, {{"st", "bad"}});
    ShareVisit* v = l.Visit(sid);
    if (l.revoked) return fail(410, {{"st", "gone"}});
    if (now >= l.expires && !(v && v->state == 0 && now < l.expires + kGraceMs)) return fail(410, {{"st", "gone"}});
    if (!l.ready) return fail(202, {{"st", "preparing"}});
    if (!l.Unchanged()) return fail(410, {{"st", "changed"}});
    if (v) {
        if (v->state == 1) return fail(202, {{"st", "wait"}});
        if (v->state == 2) return fail(403, {{"st", "refused"}});
        return 0;
    }
    // someone new
    if (!l.pin.empty()) {
        if (pinProof.empty()) return fail(401, {{"st", "pin"}, {"left", kMaxPinTries - l.pinFails}});
        std::string want = LinkPinProof(l.keys, l.pin);
        if (pinProof != want) {
            if (++l.pinFails >= kMaxPinTries) {
                l.revoked = true;
                linksChanged_ = true;
                if (log) log("share link " + l.name + ": too many wrong PINs, ended");
                Save();
                return fail(410, {{"st", "gone"}});
            }
            dirty_ = true;
            return fail(401, {{"st", "pin"}, {"left", kMaxPinTries - l.pinFails}, {"wrong", true}});
        }
    }
    if (l.maxPeople > 0 && l.people >= l.maxPeople) return fail(410, {{"st", "used"}});
    ShareVisit nv;
    nv.sid = sid;
    nv.ip = ip;
    nv.via = via;
    nv.at = now;
    if (l.ask) {
        nv.state = 1;
        l.visits.push_back(nv);
        ShareAsk a;
        a.id = nextAsk_++;
        a.link = l.id;
        a.name = l.name;
        a.ip = ip;
        a.via = via;
        a.sid = sid;
        a.at = now;
        asks_.push_back(a);
        newAsks_.push_back(a);
        Save();
        return fail(202, {{"st", "wait"}});
    }
    l.visits.push_back(nv);
    l.people++;
    Save();
    return 0;
}

// ---- through the server -------------------------------------------------------------------------------------------

ShareLinks::Reply ShareLinks::WebMeta(const std::string& id, const std::string& sid, const std::string& pp, const std::string& ip) {
    Reply r;
    std::lock_guard<std::mutex> lk(m_);
    auto l = Find(id, false);
    if (!l) {
        r.status = 404;
        r.type = "application/json";
        r.body = JsonReply({{"st", "gone"}});
        return r;
    }
    if (Gate(*l, sid, pp, ip, "web", r)) return r;
    json m = {{"n", l->name}, {"s", l->size}, {"h", l->sha256}, {"c", kLinkChunk}};
    std::string plain = m.dump();
    r.type = "application/octet-stream";
    r.body = LinkSeal(l->keys, kLinkMetaIndex, true, (const uint8_t*)plain.data(), plain.size());
    return r;
}

ShareLinks::Reply ShareLinks::WebPart(const std::string& id, const std::string& sid, uint64_t k, const std::string& ip) {
    Reply r;
    std::shared_ptr<Link> l;
    {
        std::lock_guard<std::mutex> lk(m_);
        l = Find(id, false);
        if (!l) {
            r.status = 404;
            r.type = "application/json";
            r.body = JsonReply({{"st", "gone"}});
            return r;
        }
        ShareVisit* v = l->Visit(sid);
        if (!v || v->state != 0) {  // parts only for whoever got through the checks (meta)
            r.status = 403;
            r.type = "application/json";
            r.body = JsonReply({{"st", "refused"}});
            return r;
        }
        if (Gate(*l, sid, "", ip, "web", r)) return r;
        if ((int64_t)k >= l->Parts()) {
            r.status = 416;
            r.type = "application/json";
            r.body = JsonReply({{"st", "bad"}});
            return r;
        }
    }
    int64_t off = (int64_t)k * kLinkChunk, n = std::min<int64_t>(kLinkChunk, l->size - off);
    std::string buf((size_t)std::max<int64_t>(0, n), '\0');
    FILE* f = OpenFile(l->path, "rb");
    bool ok = f && FileSeek(f, off) && (n <= 0 || fread(&buf[0], 1, (size_t)n, f) == (size_t)n);
    if (f) fclose(f);
    if (!ok) {
        r.status = 410;
        r.type = "application/json";
        r.body = JsonReply({{"st", "changed"}});
        return r;
    }
    bool last = (int64_t)k == l->Parts() - 1;
    r.type = "application/octet-stream";
    r.body = LinkSeal(l->keys, k, last, (const uint8_t*)buf.data(), buf.size());
    std::lock_guard<std::mutex> lk(m_);
    if (ShareVisit* v = l->Visit(sid)) {
        v->bytes = std::min(l->size, v->bytes + n);
        if (last && !v->done) {
            v->done = true;
            if (log) log("share link " + l->name + ": downloaded through the server by " + ip);
            Save();
        } else {
            dirty_ = true;
        }
    }
    return r;
}

// ---- tunnel to the server -----------------------------------------------------------------------------------------
// frames: [u32 BE length of what follows the type][u8 type][payload]. 1 = JSON; 2 = a reply:
// [u32 rid][u16 status][u8 content-type length][content type][body]

bool ShareLinks::TunnelSend(uint8_t type, const std::string& payload) {
    std::lock_guard<std::mutex> lk(sendM_);
    if (tunSock_ == kInvalidSocket) return false;
    uint8_t h[5] = {uint8_t(payload.size() >> 24), uint8_t(payload.size() >> 16), uint8_t(payload.size() >> 8), uint8_t(payload.size()), type};
    return SendAll(tunSock_, h, 5) && SendAll(tunSock_, payload.data(), payload.size());
}

static bool ReadFrame(SocketHandle s, uint8_t& type, std::string& payload) {
    uint8_t h[5];
    if (!RecvAll(s, h, 5)) return false;
    uint32_t n = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
    if (n > kMaxTunFrame) return false;
    type = h[4];
    payload.resize(n);
    return n == 0 || RecvAll(s, &payload[0], n);
}

void ShareLinks::TunnelRequest(const std::string& reqJson) {
    json j = json::parse(reqJson, nullptr, false);
    if (!j.is_object()) return;
    uint32_t rid = j.value("rid", 0u);
    std::string what = j.value("what", ""), id = j.value("link", ""), sid = j.value("sid", ""), ip = j.value("ip", "");
    Reply r;
    if (what == "meta") {
        r = WebMeta(id, sid, j.value("pp", ""), ip);
        FireAsks();
    }
    else if (what == "part") r = WebPart(id, sid, j.value("k", (uint64_t)0), ip);
    else {
        r.status = 400;
        r.type = "application/json";
        r.body = "{}";
    }
    std::string out;
    out.reserve(r.body.size() + 40);
    for (int i = 3; i >= 0; i--) out.push_back(char(rid >> (i * 8)));
    out.push_back(char(r.status >> 8));
    out.push_back(char(r.status));
    out.push_back(char(r.type.size()));
    out += r.type;
    out += r.body;
    TunnelSend(2, out);
}

void ShareLinks::TunnelLoop() {
    int backoff = 2000;
    while (running_) {
        std::string server, webBase;
        json ids = json::array();
        {
            std::unique_lock<std::mutex> lk(m_);
            int64_t now = NowMs();
            for (auto& [id, l] : links_)
                if (!l->revoked && now < l->expires + kGraceMs) ids.push_back(id);
            server = server_;
            webBase = webBase_;
            if (server.empty() || webBase.empty() || ids.empty()) {
                tunnelStatus_ = "";
                cv_.wait_for(lk, std::chrono::seconds(2));
                continue;
            }
            tunnelStatus_ = "connecting";
            linksChanged_ = false;
        }
        std::string host = server;
        uint16_t port = 0;
        size_t c = server.rfind(':');
        if (c != std::string::npos) {
            host = server.substr(0, c);
            port = (uint16_t)atoi(server.c_str() + c + 1);
        }
        if (host.size() > 2 && host.front() == '[') host = host.substr(1, host.size() - 2);
        SocketHandle s = port ? TcpConnect(host, port, 5000) : kInvalidSocket;
        std::string why = "can't reach the server";
        if (s != kInvalidSocket) {
            {
                std::lock_guard<std::mutex> sk(sendM_);
                tunSock_ = s;
            }
            uint8_t t = 0;
            std::string p;
            bool ok = TunnelSend(1, json{{"t", "hello"}, {"pub", myPub ? myPub() : ""}, {"v", 1}}.dump()) &&
                      WaitReadable(s, 8000) == 1 && ReadFrame(s, t, p) && t == 1;
            json ch = ok ? json::parse(p, nullptr, false) : json();
            ok = ok && ch.is_object() && ch.value("t", "") == "chal";
            if (ok) ok = TunnelSend(1, json{{"t", "auth"}, {"sig", sign ? sign(ch.value("n", "")) : ""}}.dump()) &&
                         WaitReadable(s, 8000) == 1 && ReadFrame(s, t, p) && t == 1;
            json ack = ok ? json::parse(p, nullptr, false) : json();
            if (ok && ack.is_object() && ack.value("t", "") == "ok") {
                TunnelSend(1, json{{"t", "links"}, {"ids", ids}}.dump());
                {
                    std::lock_guard<std::mutex> lk(m_);
                    tunnelStatus_ = "online";
                }
                if (log) log("share links: connected to " + server);
                backoff = 2000;
                int64_t lastPing = SteadyMs(), lastRecv = SteadyMs();
                while (running_) {
                    int w = WaitReadable(s, 500);
                    if (w < 0) break;
                    if (w == 1) {
                        if (!ReadFrame(s, t, p)) break;
                        lastRecv = SteadyMs();
                        if (t != 1) continue;
                        json j = json::parse(p, nullptr, false);
                        if (!j.is_object()) continue;
                        std::string ty = j.value("t", "");
                        if (ty == "req") {
                            if (workers_ >= 6) {
                                TunnelRequest(p);  // enough helpers: answer this one here
                            } else {
                                workers_++;
                                std::thread([this, p] {
                                    TunnelRequest(p);
                                    workers_--;
                                }).detach();
                            }
                        } else if (ty == "ping") {
                            TunnelSend(1, json{{"t", "pong"}}.dump());
                        }
                    }
                    bool changed;
                    json now = json::array();
                    {
                        std::lock_guard<std::mutex> lk(m_);
                        changed = linksChanged_ || server_ != server || webBase_ != webBase;
                        if (server_ != server || webBase_ != webBase) break;
                        linksChanged_ = false;
                        int64_t t2 = NowMs();
                        for (auto& [id, l] : links_)
                            if (!l->revoked && t2 < l->expires + kGraceMs) now.push_back(id);
                    }
                    if (changed) {
                        if (now.empty()) break;  // nothing to offer any more
                        TunnelSend(1, json{{"t", "links"}, {"ids", now}}.dump());
                    }
                    if (SteadyMs() - lastPing > 25000) {
                        TunnelSend(1, json{{"t", "ping"}}.dump());
                        lastPing = SteadyMs();
                    }
                    if (SteadyMs() - lastRecv > 90000) break;  // the server went quiet
                }
                why = "connection lost";
            } else if (ack.is_object() && ack.value("t", "") == "err") {
                why = ack.value("why", "refused");
            } else {
                why = "the server doesn't offer links";
            }
            {
                std::lock_guard<std::mutex> sk(sendM_);
                tunSock_ = kInvalidSocket;
            }
            CloseSocket(s);
        }
        if (!running_) break;
        std::unique_lock<std::mutex> lk(m_);
        tunnelStatus_ = "offline: " + why;
        cv_.wait_for(lk, std::chrono::milliseconds(backoff));
        backoff = std::min(backoff * 2, 60000);
    }
    std::lock_guard<std::mutex> lk(m_);
    tunnelStatus_ = "";
}

// ---- on the same network ------------------------------------------------------------------------------------------

static std::string UrlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o.push_back((char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            o.push_back(s[i] == '+' ? ' ' : s[i]);
        }
    }
    return o;
}

static std::string QueryArg(const std::string& q, const std::string& key) {
    size_t p = 0;
    while (p <= q.size()) {
        size_t e = q.find('&', p);
        if (e == std::string::npos) e = q.size();
        std::string kv = q.substr(p, e - p);
        size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) return eq == std::string::npos ? "" : UrlDecode(kv.substr(eq + 1));
        p = e + 1;
    }
    return "";
}

static std::string StatusText(int s) {
    switch (s) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 410: return "Gone";
        case 416: return "Range Not Satisfiable";
        default: return "Error";
    }
}

static std::string AsciiName(const std::string& n) {
    std::string o;
    for (unsigned char c : n) o.push_back(c >= 32 && c < 127 && c != '"' && c != '\\' ? (char)c : '_');
    return o;
}

static std::string PctEncode(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') o.push_back((char)c);
        else {
            o.push_back('%');
            o.push_back(hx[c >> 4]);
            o.push_back(hx[c & 15]);
        }
    }
    return o;
}

static bool SendHead(SocketHandle s, int status, const std::string& type, int64_t len, const std::string& extra) {
    std::string h = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\nContent-Type: " + type +
                    "\r\nContent-Length: " + std::to_string(len) +
                    "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
                    "X-Robots-Tag: noindex\r\nConnection: close\r\n" + extra + "\r\n";
    return SendAll(s, h.data(), h.size());
}

static void SendSimple(SocketHandle s, int status, const std::string& type, const std::string& body, const std::string& extra = "") {
    if (SendHead(s, status, type, (int64_t)body.size(), extra)) SendAll(s, body.data(), body.size());
}

void ShareLinks::LanLoop() {
    int64_t lastHouse = 0;
    while (running_) {
        std::string ip;
        SocketHandle c = TcpAccept(lanSock_, &ip, 1000);
        if (!running_) {
            if (c != kInvalidSocket) CloseSocket(c);
            break;
        }
        if (SteadyMs() - lastHouse > 5000) {
            Housekeeping();
            lastHouse = SteadyMs();
        }
        if (c == kInvalidSocket) continue;
        if (lanConns_ >= 24) {
            CloseSocket(c);
            continue;
        }
        lanConns_++;
        std::thread([this, c, ip] {
            LanConn(c, ip);
            CloseSocket(c);
            lanConns_--;
        }).detach();
    }
}

void ShareLinks::LanConn(SocketHandle s, std::string ip) {
    std::string req;
    char buf[2048];
    int64_t start = SteadyMs();
    while (req.find("\r\n\r\n") == std::string::npos) {
        if (req.size() > 8192 || SteadyMs() - start > 10000 || WaitReadable(s, 2000) != 1) return;
        int n = RecvSome(s, buf, sizeof(buf));
        if (n <= 0) return;
        req.append(buf, (size_t)n);
    }
    std::string line = req.substr(0, req.find("\r\n"));
    char method[16] = {}, target[2048] = {};
    if (sscanf(line.c_str(), "%15s %2047s", method, target) != 2) return;
    bool head = !strcmp(method, "HEAD");
    if (strcmp(method, "GET") && !head) {
        SendSimple(s, 405, "text/plain", "");
        return;
    }
    std::string path = target, query;
    if (size_t q = path.find('?'); q != std::string::npos) {
        query = path.substr(q + 1);
        path = path.substr(0, q);
    }
    std::string range;
    {
        std::string low = req;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return (char)tolower(ch); });
        if (size_t r = low.find("\r\nrange:"); r != std::string::npos) {
            size_t e = req.find("\r\n", r + 2);
            range = req.substr(r + 8, e - r - 8);
            while (!range.empty() && range.front() == ' ') range.erase(0, 1);
        }
    }
    // /f/<tok>[/start|/file]
    if (path.rfind("/f/", 0) != 0) {
        SendSimple(s, 404, "text/plain", "Not found");
        return;
    }
    std::string rest = path.substr(3), tok = rest, sub;
    if (size_t sl = rest.find('/'); sl != std::string::npos) {
        tok = rest.substr(0, sl);
        sub = rest.substr(sl + 1);
    }
    std::unique_lock<std::mutex> lk(m_);
    auto l = Find(tok, true);
    if (!l) {
        lk.unlock();
        if (sub.empty()) SendSimple(s, 404, "text/html; charset=utf-8", LinkPageHtml(json{{"mode", "lan"}, {"st", "gone"}}.dump()), kLinkPageHeaders);
        else SendSimple(s, 404, "application/json", JsonReply({{"st", "gone"}}));
        return;
    }
    if (sub.empty()) {
        json cfg = {{"mode", "lan"}, {"name", l->name}, {"size", l->size}, {"sha", l->sha256}, {"pin", !l->pin.empty()}};
        lk.unlock();
        std::string page = LinkPageHtml(cfg.dump());
        SendSimple(s, 200, "text/html; charset=utf-8", head ? "" : page, kLinkPageHeaders);
        return;
    }
    std::string sid = QueryArg(query, "sid");
    if (sub == "start") {
        std::string pin = QueryArg(query, "pin");
        Reply r;
        std::string pp = pin.empty() ? "" : LinkPinProof(l->keys, pin);
        if (Gate(*l, sid, pp, ip, "lan", r)) {
            lk.unlock();
            FireAsks();
            SendSimple(s, r.status, r.type, r.body);
            return;
        }
        json ok = {{"st", "ok"}, {"name", l->name}, {"size", l->size}, {"sha", l->sha256}};
        lk.unlock();
        SendSimple(s, 200, "application/json", ok.dump());
        return;
    }
    if (sub != "file") {
        lk.unlock();
        SendSimple(s, 404, "text/plain", "Not found");
        return;
    }
    ShareVisit* v = l->Visit(sid);
    Reply r;
    if (!v || v->state != 0) {
        lk.unlock();
        SendSimple(s, 403, "text/plain", "Open the link again.");
        return;
    }
    if (Gate(*l, sid, "", ip, "lan", r)) {
        lk.unlock();
        SendSimple(s, r.status, "text/plain", r.status == 410 ? "This link has ended." : "Not now.");
        return;
    }
    std::string fpath = l->path, name = l->name;
    int64_t size = l->size;
    lk.unlock();

    int64_t from = 0, to = size - 1;
    bool partial = false;
    if (range.rfind("bytes=", 0) == 0 && range.find(',') == std::string::npos) {
        long long a = -1, b = -1;
        std::string spec = range.substr(6);
        if (spec.size() && spec[0] == '-') {
            if (sscanf(spec.c_str() + 1, "%lld", &b) == 1 && b > 0) {
                from = std::max<int64_t>(0, size - b);
                partial = true;
            }
        } else if (sscanf(spec.c_str(), "%lld-%lld", &a, &b) >= 1 && a >= 0) {
            from = a;
            if (b >= a) to = std::min<int64_t>(b, size - 1);
            partial = true;
        }
        if (partial && from >= size && size > 0) {
            SendSimple(s, 416, "text/plain", "", "Content-Range: bytes */" + std::to_string(size) + "\r\n");
            return;
        }
    }
    int64_t len = size == 0 ? 0 : to - from + 1;
    std::string extra = "Accept-Ranges: bytes\r\nContent-Disposition: attachment; filename=\"" + AsciiName(name) +
                        "\"; filename*=UTF-8''" + PctEncode(name) + "\r\n";
    if (partial) extra += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" + std::to_string(size) + "\r\n";
    if (!SendHead(s, partial ? 206 : 200, "application/octet-stream", len, extra) || head) return;
    FILE* f = OpenFile(fpath, "rb");
    if (!f || !FileSeek(f, from)) {
        if (f) fclose(f);
        return;
    }
    std::vector<char> b(256 * 1024);
    int64_t left = len, sent = 0;
    while (left > 0 && running_) {
        size_t n = fread(b.data(), 1, (size_t)std::min<int64_t>(left, (int64_t)b.size()), f);
        if (n == 0 || !SendAll(s, b.data(), n)) break;
        left -= (int64_t)n;
        sent += (int64_t)n;
        if ((sent & ((8 << 20) - 1)) < (int64_t)n) {
            std::lock_guard<std::mutex> g(m_);
            if (ShareVisit* vv = l->Visit(sid)) vv->bytes = std::max(vv->bytes, from + sent);
            dirty_ = true;
        }
    }
    fclose(f);
    std::lock_guard<std::mutex> g(m_);
    if (ShareVisit* vv = l->Visit(sid)) {
        vv->bytes = std::max(vv->bytes, from + sent);
        if (left == 0 && to == size - 1 && !vv->done) {
            vv->done = true;
            if (log) log("share link " + name + ": downloaded on this network by " + ip);
        }
    }
    Save();
}

void ShareLinks::FireAsks() {
    std::vector<ShareAsk> a;
    {
        std::lock_guard<std::mutex> lk(m_);
        a.swap(newAsks_);
    }
    if (onAsk)
        for (auto& x : a) onAsk(x);
}

// ---- upkeep, state ------------------------------------------------------------------------------------------------

void ShareLinks::Housekeeping() {
    std::lock_guard<std::mutex> lk(m_);
    int64_t now = NowMs();
    for (auto it = asks_.begin(); it != asks_.end();) {
        if (now - it->at > kAskTimeoutMs) {
            auto li = links_.find(it->link);
            if (li != links_.end())
                if (ShareVisit* v = li->second->Visit(it->sid); v && v->state == 1) v->state = 2;
            it = asks_.erase(it);
            dirty_ = true;
        } else {
            ++it;
        }
    }
    for (auto it = links_.begin(); it != links_.end();) {
        Link& l = *it->second;
        int64_t end = l.revoked ? std::max(l.created, std::min(l.expires, now)) : l.expires + kGraceMs;
        if (now > end + kKeepMs) {
            it = links_.erase(it);
            dirty_ = true;
            linksChanged_ = true;
        } else {
            ++it;
        }
    }
    if (dirty_) Save();
}

void ShareLinks::Save() {
    // (m_ held)
    dirty_ = false;
    if (dir_.empty()) return;
    json arr = json::array();
    for (auto& [id, l] : links_) {
        json vs = json::array();
        for (auto& v : l->visits)
            vs.push_back({{"sid", v.sid}, {"ip", v.ip}, {"via", v.via}, {"at", v.at}, {"bytes", v.bytes}, {"done", v.done}, {"state", v.state}});
        arr.push_back({{"id", l->id}, {"tok", l->tok}, {"path", l->path}, {"name", l->name}, {"pin", l->pin}, {"sha", l->sha256},
                       {"secret", B64Url(l->secret, sizeof(l->secret))}, {"size", l->size}, {"mtime", l->mtime},
                       {"created", l->created}, {"expires", l->expires}, {"max", l->maxPeople}, {"people", l->people},
                       {"fails", l->pinFails}, {"ask", l->ask}, {"revoked", l->revoked}, {"ready", l->ready}, {"visits", vs}});
    }
    SaveJsonFile(JoinPath(dir_, "share_links.json"), json{{"v", 1}, {"links", arr}});
}

void ShareLinks::Load() {
    json j;
    if (!LoadJsonFile(JoinPath(dir_, "share_links.json"), j) || !j.is_object() || !j["links"].is_array()) return;
    std::lock_guard<std::mutex> lk(m_);
    for (auto& e : j["links"]) {
        if (!e.is_object()) continue;
        auto l = std::make_shared<Link>();
        l->id = e.value("id", "");
        l->tok = e.value("tok", "");
        l->path = e.value("path", "");
        l->name = e.value("name", "");
        l->pin = e.value("pin", "");
        l->sha256 = e.value("sha", "");
        std::string sec;
        if (l->id.empty() || l->tok.empty() || !FromB64Url(e.value("secret", ""), sec) || sec.size() != sizeof(l->secret)) continue;
        memcpy(l->secret, sec.data(), sizeof(l->secret));
        l->keys = DeriveLinkKeys(l->secret, sizeof(l->secret));
        l->size = e.value("size", (int64_t)0);
        l->mtime = e.value("mtime", (int64_t)0);
        l->created = e.value("created", (int64_t)0);
        l->expires = e.value("expires", (int64_t)0);
        l->maxPeople = e.value("max", 1);
        l->people = e.value("people", 0);
        l->pinFails = e.value("fails", 0);
        l->ask = e.value("ask", false);
        l->revoked = e.value("revoked", false);
        l->ready = e.value("ready", false) && !l->sha256.empty();
        if (e["visits"].is_array())
            for (auto& v : e["visits"]) {
                ShareVisit sv;
                sv.sid = v.value("sid", "");
                sv.ip = v.value("ip", "");
                sv.via = v.value("via", "");
                sv.at = v.value("at", (int64_t)0);
                sv.bytes = v.value("bytes", (int64_t)0);
                sv.done = v.value("done", false);
                sv.state = v.value("state", 0);
                if (sv.state == 1) sv.state = 2;  // asks don't survive a restart
                l->visits.push_back(sv);
            }
        links_[l->id] = l;
    }
}

}  // namespace ls
