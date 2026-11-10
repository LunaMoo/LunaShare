#include "link_server.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <set>

#include <nlohmann/json.hpp>

#include "crypto.h"
#include "link_page.h"
#include "net.h"
#include "platform.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

#ifdef LUNASHARE_LINK_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace ls {

using json = nlohmann::json;

static constexpr uint32_t kMaxTunFrame = 4u << 20;
static constexpr int kMaxOutstanding = 8;  // requests waiting on one device

struct LinkServer::Tunnel {
    std::string pub;
    SocketHandle s = kInvalidSocket;
    std::mutex sendM, m;
    std::condition_variable cv;
    struct Pending {
        bool done = false;
        Reply r;
    };
    std::map<uint32_t, std::shared_ptr<Pending>> pending;
    uint32_t nextRid = 1;
    bool alive = true;
    std::set<std::string> ids;

    bool Send(uint8_t type, const std::string& p) {
        std::lock_guard<std::mutex> lk(sendM);
        uint8_t h[5] = {uint8_t(p.size() >> 24), uint8_t(p.size() >> 16), uint8_t(p.size() >> 8), uint8_t(p.size()), type};
        return SendAll(s, h, 5) && SendAll(s, p.data(), p.size());
    }
};

static bool ReadTunFrame(SocketHandle s, uint8_t& type, std::string& payload) {
    uint8_t h[5];
    if (!RecvAll(s, h, 5)) return false;
    uint32_t n = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
    if (n > kMaxTunFrame) return false;
    type = h[4];
    payload.resize(n);
    return n == 0 || RecvAll(s, &payload[0], n);
}

static SocketHandle Listen(const std::string& bindAddr, uint16_t port) {
    NetInit();
    auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if ((SocketHandle)s == kInvalidSocket) return kInvalidSocket;
#ifndef _WIN32
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, bindAddr.c_str(), &a.sin_addr) != 1 || bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 128) != 0) {
        CloseSocket((SocketHandle)s);
        return kInvalidSocket;
    }
    return (SocketHandle)s;
}

static void SetTimeouts(SocketHandle s, int ms) {
#ifdef _WIN32
    DWORD t = ms;
    setsockopt((SOCKET)s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    setsockopt((SOCKET)s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
#else
    timeval t{ms / 1000, (ms % 1000) * 1000};
    setsockopt((int)s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
    setsockopt((int)s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
#endif
}

bool LinkServer::TlsSupported() {
#ifdef LUNASHARE_LINK_TLS
    return true;
#else
    return false;
#endif
}

bool LinkServer::LoadTls(std::string* err) {
#ifdef LUNASHARE_LINK_TLS
    SSL_CTX* c = SSL_CTX_new(TLS_server_method());
    if (!c) {
        if (err) *err = "TLS setup failed";
        return false;
    }
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    SSL_CTX_set_options(c, SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE);
    if (SSL_CTX_use_certificate_chain_file(c, opt_.certFile.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(c, opt_.keyFile.c_str(), SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(c) != 1) {
        char e[256] = "";
        ERR_error_string_n(ERR_get_error(), e, sizeof(e));
        if (err) *err = "can't use the certificate " + opt_.certFile + " / key " + opt_.keyFile + ": " + e;
        SSL_CTX_free(c);
        return false;
    }
    std::lock_guard<std::mutex> lk(m_);
    tls_ = std::shared_ptr<void>(c, [](void* p) { SSL_CTX_free((SSL_CTX*)p); });
    certMtime_ = FileMTime(opt_.certFile) + FileMTime(opt_.keyFile);
    return true;
#else
    if (err) *err = "this server was built without TLS (OpenSSL) - use plain HTTP behind a reverse proxy";
    return false;
#endif
}

bool LinkServer::Start(const Options& o, std::string* err) {
    opt_ = o;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);  // TLS writes to a closed connection
#endif
    if (!opt_.certFile.empty() || !opt_.keyFile.empty())
        if (!LoadTls(err)) return false;
    httpSock_ = Listen(opt_.bind, opt_.httpPort);
    if (httpSock_ == kInvalidSocket) {
        if (err) *err = "cannot listen on tcp " + opt_.bind + ":" + std::to_string(opt_.httpPort);
        return false;
    }
    tunSock_ = Listen(opt_.bind, opt_.tunnelPort);
    if (tunSock_ == kInvalidSocket) {
        if (err) *err = "cannot listen on tcp " + opt_.bind + ":" + std::to_string(opt_.tunnelPort);
        CloseSocket(httpSock_);
        httpSock_ = kInvalidSocket;
        return false;
    }
    running_ = true;
    httpThread_ = std::thread([this] { HttpLoop(); });
    tunThread_ = std::thread([this] { TunnelAcceptLoop(); });
    return true;
}

void LinkServer::Stop() {
    if (!running_) return;
    running_ = false;
    if (httpThread_.joinable()) httpThread_.join();
    if (tunThread_.joinable()) tunThread_.join();
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& [p, t] : tunnels_) ShutdownSocket(t->s);
    }
    for (int i = 0; i < 500 && conns_ > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CloseSocket(httpSock_);
    CloseSocket(tunSock_);
    httpSock_ = tunSock_ = kInvalidSocket;
}

size_t LinkServer::TunnelCount() {
    std::lock_guard<std::mutex> lk(m_);
    return tunnels_.size();
}

// ---- devices ------------------------------------------------------------------------------------------------------

void LinkServer::TunnelAcceptLoop() {
    int64_t lastCheck = SteadyMs();
    while (running_) {
        std::string ip;
        SocketHandle c = TcpAccept(tunSock_, &ip, 1000);
        if (SteadyMs() - lastCheck > 15000) {  // devices that left (or were limited) don't keep a tunnel
            lastCheck = SteadyMs();
            std::lock_guard<std::mutex> lk(m_);
            for (auto& [p, t] : tunnels_)
                if (opt_.allowed && !opt_.allowed(p)) ShutdownSocket(t->s);
        }
        if (c == kInvalidSocket) continue;
        if (conns_ >= 1500) {
            CloseSocket(c);
            continue;
        }
        conns_++;
        std::thread([this, c, ip] {
            TunnelConn(c, ip);
            conns_--;
        }).detach();
    }
}

void LinkServer::TunnelConn(intptr_t sock, std::string ip) {
    SocketHandle s = sock;
    TuneStreamSocket(s, true);
    auto t = std::make_shared<Tunnel>();
    t->s = s;
    uint8_t ty = 0;
    std::string p;
    auto fail = [&](const std::string& why) {
        t->Send(1, json{{"t", "err"}, {"why", why}}.dump());
        CloseSocket(s);
    };
    if (WaitReadable(s, 10000) != 1 || !ReadTunFrame(s, ty, p) || ty != 1) return CloseSocket(s);
    json h = json::parse(p, nullptr, false);
    std::string pub = h.is_object() ? h.value("pub", "") : "";
    if (pub.size() != 64) return fail("bad hello");
    if (opt_.allowed && !opt_.allowed(pub)) return fail("this device isn't registered here (or was joined with an invite)");
    std::string n = RandomHex(16);
    if (!t->Send(1, json{{"t", "chal"}, {"n", n}}.dump())) return CloseSocket(s);
    if (WaitReadable(s, 10000) != 1 || !ReadTunFrame(s, ty, p) || ty != 1) return CloseSocket(s);
    json a = json::parse(p, nullptr, false);
    if (!a.is_object() || !VerifyData(pub, "links", n, a.value("sig", ""))) return fail("bad signature");
    t->pub = pub;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto old = tunnels_.find(pub);
        if (old != tunnels_.end()) ShutdownSocket(old->second->s);  // the newest connection wins
        tunnels_[pub] = t;
    }
    t->Send(1, json{{"t", "ok"}}.dump());
    if (opt_.log) opt_.log("links: device " + pub.substr(0, 8) + " connected from " + ip);
    int64_t lastRecv = SteadyMs(), lastPing = SteadyMs();
    while (running_) {
        int w = WaitReadable(s, 1000);
        if (w < 0) break;
        if (w == 1) {
            if (!ReadTunFrame(s, ty, p)) break;
            lastRecv = SteadyMs();
            if (ty == 1) {
                json j = json::parse(p, nullptr, false);
                if (!j.is_object()) continue;
                std::string k = j.value("t", "");
                if (k == "links" && j["ids"].is_array()) {
                    std::set<std::string> ids;
                    for (auto& x : j["ids"])
                        if (x.is_string() && x.get<std::string>().size() <= 32 && ids.size() < 5000) ids.insert(x.get<std::string>());
                    std::lock_guard<std::mutex> lk(m_);
                    for (auto& id : t->ids)
                        if (!ids.count(id)) {
                            auto o = owner_.find(id);
                            if (o != owner_.end() && o->second == pub) owner_.erase(o);
                        }
                    for (auto& id : ids) {
                        auto o = owner_.find(id);
                        if (o == owner_.end() || o->second == pub || !tunnels_.count(o->second)) owner_[id] = pub;
                    }
                    t->ids = ids;
                } else if (k == "ping") {
                    t->Send(1, json{{"t", "pong"}}.dump());
                }
            } else if (ty == 2 && p.size() >= 7) {
                uint32_t rid = (uint32_t(uint8_t(p[0])) << 24) | (uint32_t(uint8_t(p[1])) << 16) | (uint32_t(uint8_t(p[2])) << 8) | uint8_t(p[3]);
                int status = (uint8_t(p[4]) << 8) | uint8_t(p[5]);
                size_t tl = uint8_t(p[6]);
                if (p.size() < 7 + tl) continue;
                std::lock_guard<std::mutex> lk(t->m);
                auto it = t->pending.find(rid);
                if (it == t->pending.end()) continue;
                it->second->r.status = status;
                it->second->r.type = p.substr(7, tl);
                it->second->r.body = p.substr(7 + tl);
                it->second->done = true;
                t->pending.erase(it);
                t->cv.notify_all();
            }
        }
        if (SteadyMs() - lastPing > 30000) {
            t->Send(1, json{{"t", "ping"}}.dump());
            lastPing = SteadyMs();
        }
        if (SteadyMs() - lastRecv > 100000) break;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = tunnels_.find(pub);
        if (it != tunnels_.end() && it->second == t) {
            tunnels_.erase(it);
            for (auto& id : t->ids) {
                auto o = owner_.find(id);
                if (o != owner_.end() && o->second == pub) owner_.erase(o);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(t->m);
        t->alive = false;
        t->pending.clear();
        t->cv.notify_all();
    }
    if (opt_.log) opt_.log("links: device " + pub.substr(0, 8) + " disconnected");
    CloseSocket(s);
}

LinkServer::Reply LinkServer::Forward(const std::string& id, const std::string& reqTail) {
    Reply off;
    off.status = 503;
    off.type = "application/json";
    off.body = R"({"st":"offline"})";
    std::shared_ptr<Tunnel> t;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto o = owner_.find(id);
        if (o == owner_.end()) return off;
        auto it = tunnels_.find(o->second);
        if (it == tunnels_.end()) return off;
        t = it->second;
    }
    auto pend = std::make_shared<Tunnel::Pending>();
    uint32_t rid;
    {
        std::unique_lock<std::mutex> lk(t->m);
        if (!t->cv.wait_for(lk, std::chrono::seconds(30), [&] { return !t->alive || (int)t->pending.size() < kMaxOutstanding; }) || !t->alive)
            return off;
        rid = t->nextRid++;
        t->pending[rid] = pend;
    }
    // reqTail: the request's JSON without its closing brace
    std::string req = reqTail + ",\"rid\":" + std::to_string(rid) + "}";
    if (!t->Send(1, req)) return off;
    std::unique_lock<std::mutex> lk(t->m);
    if (!t->cv.wait_for(lk, std::chrono::seconds(90), [&] { return pend->done || !t->alive; }) || !pend->done) {
        t->pending.erase(rid);
        t->cv.notify_all();
        return off;
    }
    return pend->r;
}

// ---- browsers -----------------------------------------------------------------------------------------------------

namespace {
struct Http {
    SocketHandle s = kInvalidSocket;
#ifdef LUNASHARE_LINK_TLS
    SSL* ssl = nullptr;
#endif
    int Read(char* b, int n) {
#ifdef LUNASHARE_LINK_TLS
        if (ssl) return SSL_read(ssl, b, n);
#endif
        return RecvSome(s, b, (size_t)n);
    }
    bool Write(const std::string& d) {
#ifdef LUNASHARE_LINK_TLS
        if (ssl) {
            size_t off = 0;
            while (off < d.size()) {
                int w = SSL_write(ssl, d.data() + off, (int)std::min<size_t>(d.size() - off, 1 << 20));
                if (w <= 0) return false;
                off += (size_t)w;
            }
            return true;
        }
#endif
        return SendAll(s, d.data(), d.size());
    }
};

const char* StatusText(int s) {
    switch (s) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 410: return "Gone";
        case 416: return "Range Not Satisfiable";
        case 429: return "Too Many Requests";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

bool ValidId(const std::string& s) {
    if (s.size() < 8 || s.size() > 32) return false;
    for (char c : s)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') return false;
    return true;
}

std::string Arg(const std::string& q, const std::string& key) {
    size_t p = 0;
    while (p <= q.size()) {
        size_t e = q.find('&', p);
        if (e == std::string::npos) e = q.size();
        std::string kv = q.substr(p, e - p);
        size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key && eq != std::string::npos) {
            std::string v = kv.substr(eq + 1);
            for (char c : v)  // only what the page sends (base64url / hex)
                if (!isalnum((unsigned char)c) && c != '-' && c != '_') return "";
            return v.size() <= 128 ? v : "";
        }
        p = e + 1;
    }
    return "";
}
}  // namespace

void LinkServer::HttpLoop() {
    int64_t lastCert = SteadyMs();
    while (running_) {
        std::string ip;
        SocketHandle c = TcpAccept(httpSock_, &ip, 1000);
        if (!opt_.certFile.empty() && SteadyMs() - lastCert > 30000) {  // a renewed certificate (ACME) is picked up
            lastCert = SteadyMs();
            int64_t mt = FileMTime(opt_.certFile) + FileMTime(opt_.keyFile);
            if (mt != certMtime_) {
                std::string err;
                if (LoadTls(&err)) {
                    if (opt_.log) opt_.log("links: certificate reloaded");
                } else {
                    certMtime_ = mt;
                    if (opt_.log) opt_.log("links: " + err);
                }
            }
        }
        if (c == kInvalidSocket) continue;
        if (conns_ >= 1500) {
            CloseSocket(c);
            continue;
        }
        conns_++;
        std::thread([this, c, ip] {
            HttpConn(c, ip);
            conns_--;
        }).detach();
    }
}

void LinkServer::HttpConn(intptr_t sock, std::string ip) {
    SocketHandle s = sock;
    SetTimeouts(s, 30000);
    Http h;
    h.s = s;
#ifdef LUNASHARE_LINK_TLS
    std::shared_ptr<void> ctx;
    {
        std::lock_guard<std::mutex> lk(m_);
        ctx = tls_;
    }
    if (ctx) {
        h.ssl = SSL_new((SSL_CTX*)ctx.get());
        SSL_set_fd(h.ssl, (int)s);
        if (SSL_accept(h.ssl) != 1) {
            SSL_free(h.ssl);
            CloseSocket(s);
            return;
        }
    }
#endif
    std::string buf;
    char tmp[4096];
    for (int served = 0; served < 500 && running_; served++) {
        size_t end;
        while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
            if (buf.size() > 16384) goto done;
            int n = h.Read(tmp, sizeof(tmp));
            if (n <= 0) goto done;
            buf.append(tmp, (size_t)n);
        }
        {
            std::string head = buf.substr(0, end);
            buf.erase(0, end + 4);  // (GET requests: no body)
            std::string line = head.substr(0, head.find("\r\n"));
            char method[16] = {}, target[2048] = {};
            if (sscanf(line.c_str(), "%15s %2047s", method, target) != 2) break;
            std::string lower = head;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return (char)tolower(ch); });
            bool closeAfter = lower.find("\r\nconnection: close") != std::string::npos || line.find("HTTP/1.0") != std::string::npos;
            std::string client = ip;
            if (opt_.trustProxy && ip == "127.0.0.1") {
                size_t x = lower.find("\r\nx-forwarded-for:");
                if (x != std::string::npos) {
                    size_t e = head.find("\r\n", x + 2);
                    std::string v = head.substr(x + 18, e - x - 18);
                    v = v.substr(0, v.find(','));
                    v.erase(0, v.find_first_not_of(' '));
                    while (!v.empty() && v.back() == ' ') v.pop_back();
                    if (!v.empty() && v.size() < 64) client = v;
                }
            }
            // a modest limit per browser address
            bool limited = false;
            {
                std::lock_guard<std::mutex> lk(m_);
                auto& b = buckets_[client];
                int64_t now = SteadyMs();
                if (b.second == 0) b = {300.0, now};
                b.first = std::min(300.0, b.first + (now - b.second) * 0.03);
                b.second = now;
                if (b.first < 1) limited = true;
                else b.first -= 1;
                if (buckets_.size() > 20000) buckets_.clear();
            }
            std::string path = target, query;
            if (size_t q = path.find('?'); q != std::string::npos) {
                query = path.substr(q + 1);
                path = path.substr(0, q);
            }
            bool isHead = !strcmp(method, "HEAD");
            int status = 200;
            std::string type = "text/plain; charset=utf-8", body, extra;
            if (strcmp(method, "GET") && !isHead) {
                status = 405;
            } else if (limited) {
                status = 429;
                type = "application/json";
                body = R"({"st":"busy"})";
                extra = "Retry-After: 5\r\n";
            } else if (path == "/" || path == "/robots.txt") {
                body = path == "/" ? "LunaShare\n" : "User-agent: *\nDisallow: /\n";
            } else if (path == "/sw.js") {
                type = "application/javascript";
                body = LinkServiceWorkerJs();
                extra = "Service-Worker-Allowed: /s/\r\n";
            } else if (path.rfind("/s/", 0) == 0 && ValidId(path.substr(3))) {
                type = "text/html; charset=utf-8";
                body = LinkPageHtml(R"({"mode":"web"})");
                extra = kLinkPageHeaders;
            } else if (path.rfind("/api/", 0) == 0) {
                std::string rest = path.substr(5), id = rest.substr(0, rest.find('/')), what = rest.size() > id.size() ? rest.substr(id.size() + 1) : "";
                std::string sid = Arg(query, "sid");
                json r = {{"t", "req"}, {"link", id}, {"sid", sid}, {"ip", client}};
                bool ok = ValidId(id) && !sid.empty();
                if (ok && what == "meta") {
                    r["what"] = "meta";
                    r["pp"] = Arg(query, "pp");
                } else if (ok && what.rfind("p/", 0) == 0 && what.size() > 2 && what.size() < 14 &&
                           what.find_first_not_of("0123456789", 2) == std::string::npos) {
                    r["what"] = "part";
                    r["k"] = std::stoull(what.substr(2));
                } else {
                    ok = false;
                }
                if (!ok) {
                    status = 400;
                    type = "application/json";
                    body = R"({"st":"bad"})";
                } else {
                    std::string j = r.dump();
                    j.pop_back();
                    Reply rep = Forward(id, j);
                    status = rep.status;
                    type = rep.type.empty() ? "application/octet-stream" : rep.type;
                    body = std::move(rep.body);
                }
            } else {
                status = 404;
                body = "Not found\n";
            }
            std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\nContent-Type: " + type +
                               "\r\nContent-Length: " + std::to_string(body.size()) +
                               "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
                               "X-Robots-Tag: noindex\r\n" + extra + (closeAfter ? "Connection: close\r\n" : "Connection: keep-alive\r\n") + "\r\n";
            if (!isHead) resp += body;
            if (!h.Write(resp) || closeAfter) break;
        }
    }
done:
#ifdef LUNASHARE_LINK_TLS
    if (h.ssl) {
        SSL_shutdown(h.ssl);
        SSL_free(h.ssl);
    }
#endif
    CloseSocket(s);
}

}  // namespace ls
