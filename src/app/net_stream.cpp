// LunaShare - reading a URL as it comes (see net_stream.h).
#include "net_stream.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "bluetooth.h"  // Bridge (Android)
#include "platform.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#elif !defined(__ANDROID__)
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#ifdef LS_HTTP_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif
#endif

namespace ls {

namespace {
const char* kAgent = "LunaShare/1.6 (internet radio)";

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

struct Url {
    bool tls = false;
    std::string host, path;
    int port = 0;
};
bool ParseUrl(const std::string& u, Url& out) {
    size_t p;
    if (u.compare(0, 7, "http://") == 0) out.tls = false, p = 7;
    else if (u.compare(0, 8, "https://") == 0) out.tls = true, p = 8;
    else return false;
    size_t slash = u.find_first_of("/?#", p);
    std::string hostport = u.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
    out.path = slash == std::string::npos ? "/" : u.substr(slash);
    if (out.path[0] != '/') out.path = "/" + out.path;
    size_t hash = out.path.find('#');
    if (hash != std::string::npos) out.path.resize(hash);
    size_t at = hostport.rfind('@');  // (user:password@ - not used)
    if (at != std::string::npos) hostport = hostport.substr(at + 1);
    out.port = out.tls ? 443 : 80;
    if (!hostport.empty() && hostport[0] == '[') {  // [IPv6]:port
        size_t e = hostport.find(']');
        if (e == std::string::npos) return false;
        out.host = hostport.substr(1, e - 1);
        if (e + 2 < hostport.size() && hostport[e + 1] == ':') out.port = atoi(hostport.c_str() + e + 2);
    } else {
        size_t c = hostport.rfind(':');
        if (c != std::string::npos) {
            out.host = hostport.substr(0, c);
            out.port = atoi(hostport.c_str() + c + 1);
        } else {
            out.host = hostport;
        }
    }
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}
}  // namespace

std::string ResolveUrl(const std::string& base, const std::string& ref) {
    if (ref.find("://") != std::string::npos) return ref;
    Url b;
    if (!ParseUrl(base, b)) return ref;
    std::string origin = std::string(b.tls ? "https://" : "http://") + (b.host.find(':') != std::string::npos ? "[" + b.host + "]" : b.host) +
                         ((b.tls && b.port == 443) || (!b.tls && b.port == 80) ? "" : ":" + std::to_string(b.port));
    if (ref.compare(0, 2, "//") == 0) return (b.tls ? "https:" : "http:") + ref;
    if (!ref.empty() && ref[0] == '/') return origin + ref;
    std::string dir = b.path.substr(0, b.path.find('?'));
    dir = dir.substr(0, dir.rfind('/') + 1);
    return origin + dir + ref;
}

// ============================================================================ Windows: WinHTTP
#if defined(_WIN32)
namespace {
std::wstring W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
std::string U(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

class WinStream : public NetStream {
public:
    ~WinStream() override { Close(); }
    bool Open(const std::string& url, bool icy, std::string* err) override {
        Url u;
        if (!ParseUrl(url, u)) {
            if (err) *err = "not an http(s) address";
            return false;
        }
        s_ = WinHttpOpen(W(kAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s_) s_ = WinHttpOpen(W(kAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s_) {
            if (err) *err = "WinHTTP unavailable";
            return false;
        }
        WinHttpSetTimeouts(s_, 10000, 10000, 15000, 15000);
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;  // (radio servers do send https -> http)
        WinHttpSetOption(s_, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        c_ = WinHttpConnect(s_, W(u.host).c_str(), (INTERNET_PORT)u.port, 0);
        r_ = c_ ? WinHttpOpenRequest(c_, L"GET", W(u.path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     u.tls ? WINHTTP_FLAG_SECURE : 0)
                : nullptr;
        if (!r_) {
            if (err) *err = "cannot connect";
            return false;
        }
        std::wstring hdr = icy ? L"Icy-MetaData: 1\r\n" : L"";
        if (!WinHttpSendRequest(r_, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(r_, nullptr)) {
            if (err) *err = "no answer (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r_, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        status = (int)code;
        // all the headers, one per line
        DWORD sz = 0;
        WinHttpQueryHeaders(r_, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &sz, WINHTTP_NO_HEADER_INDEX);
        if (sz > 0) {
            std::wstring raw(sz / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(r_, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &sz, WINHTTP_NO_HEADER_INDEX)) {
                std::string all = U(raw.c_str());
                size_t p = 0;
                while (p < all.size()) {
                    size_t e = all.find("\r\n", p);
                    std::string line = all.substr(p, e == std::string::npos ? std::string::npos : e - p);
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
                    if (e == std::string::npos) break;
                    p = e + 2;
                }
            }
        }
        DWORD ul = 0;
        WinHttpQueryOption(r_, WINHTTP_OPTION_URL, nullptr, &ul);
        if (ul > 0) {
            std::wstring fu(ul / sizeof(wchar_t), L'\0');
            if (WinHttpQueryOption(r_, WINHTTP_OPTION_URL, &fu[0], &ul)) finalUrl = U(fu.c_str());
        }
        if (finalUrl.empty()) finalUrl = url;
        if (status < 200 || status >= 300) {
            if (err) *err = "HTTP " + std::to_string(status);
            return false;
        }
        return true;
    }
    int Read(uint8_t* buf, int len) override {
        if (!r_ || (cancel && *cancel)) return -1;
        DWORD got = 0;
        if (!WinHttpReadData(r_, buf, (DWORD)len, &got)) return -1;
        if (cancel && *cancel) return -1;
        return (int)got;
    }
    void Close() {
        if (r_) WinHttpCloseHandle(r_);
        if (c_) WinHttpCloseHandle(c_);
        if (s_) WinHttpCloseHandle(s_);
        r_ = c_ = s_ = nullptr;
    }

private:
    HINTERNET s_ = nullptr, c_ = nullptr, r_ = nullptr;
};
}  // namespace

std::unique_ptr<NetStream> NewNetStream() { return std::make_unique<WinStream>(); }

// ============================================================================ Android: through Java
#elif defined(__ANDROID__)
namespace {
class JavaStream : public NetStream {
public:
    ~JavaStream() override {
        if (id_ > 0 && Bridge().netClose) Bridge().netClose(id_);
    }
    bool Open(const std::string& url, bool icy, std::string* err) override {
        if (!Bridge().netOpen) {
            if (err) *err = "no network bridge";
            return false;
        }
        std::string hdrs, fu, e;
        id_ = Bridge().netOpen(url, icy, status, hdrs, fu, e);
        if (id_ <= 0) {
            if (err) *err = e.empty() ? "cannot connect" : e;
            return false;
        }
        finalUrl = fu.empty() ? url : fu;
        size_t p = 0;  // "name: value" lines
        while (p < hdrs.size()) {
            size_t n = hdrs.find('\n', p);
            std::string line = hdrs.substr(p, n == std::string::npos ? std::string::npos : n - p);
            size_t colon = line.find(':');
            if (colon != std::string::npos) headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
            if (n == std::string::npos) break;
            p = n + 1;
        }
        if (status < 200 || status >= 300) {
            if (err) *err = "HTTP " + std::to_string(status);
            return false;
        }
        return true;
    }
    int Read(uint8_t* buf, int len) override {
        if (id_ <= 0 || (cancel && *cancel) || !Bridge().netRead) return -1;
        int n = Bridge().netRead(id_, buf, len);
        if (cancel && *cancel) return -1;
        return n;
    }

private:
    int id_ = 0;
};
}  // namespace

std::unique_ptr<NetStream> NewNetStream() { return std::make_unique<JavaStream>(); }

// ============================================================================ Linux / macOS: sockets (+ OpenSSL)
#else
namespace {
#ifdef LS_HTTP_OPENSSL
SSL_CTX* TlsContext() {
    static SSL_CTX* ctx = [] {
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        if (!c) return c;
        SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
        SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
        bool loaded = false;
        if (const char* f = getenv("SSL_CERT_FILE")) loaded = SSL_CTX_load_verify_locations(c, f, nullptr) == 1;
        for (const char* p : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem",
                              "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"})
            if (!loaded && access(p, R_OK) == 0) loaded = SSL_CTX_load_verify_locations(c, p, nullptr) == 1;
        if (!loaded) SSL_CTX_set_default_verify_paths(c);
        return c;
    }();
    return ctx;
}
#endif

class SockStream : public NetStream {
public:
    ~SockStream() override { Close(); }
    bool Open(const std::string& url, bool icy, std::string* err) override {
        std::string at = url;
        for (int hop = 0; hop < 6; hop++) {
            Close();
            if (!Connect(at, icy, err)) return false;
            if (status >= 300 && status < 400 && !Header("location").empty()) {  // redirected
                at = ResolveUrl(at, Header("location"));
                continue;
            }
            finalUrl = at;
            if (status < 200 || status >= 300) {
                if (err) *err = "HTTP " + std::to_string(status);
                return false;
            }
            return true;
        }
        if (err) *err = "too many redirects";
        return false;
    }
    int Read(uint8_t* buf, int len) override {
        if (fd_ < 0 || done_) return done_ ? 0 : -1;
        if (chunked_) {
            while (chunkLeft_ == 0) {  // the next chunk's size line
                std::string line;
                if (!ReadLine(line)) return -1;
                if (line.empty()) continue;
                chunkLeft_ = (int64_t)strtoll(line.c_str(), nullptr, 16);
                if (chunkLeft_ <= 0) {
                    done_ = true;
                    return 0;
                }
            }
            int n = Raw(buf, (int)std::min<int64_t>(len, chunkLeft_));
            if (n > 0) {
                chunkLeft_ -= n;
                if (chunkLeft_ == 0) {
                    std::string crlf;
                    ReadLine(crlf);
                }
            }
            return n;
        }
        if (length_ >= 0) {
            if (length_ == 0) {
                done_ = true;
                return 0;
            }
            int n = Raw(buf, (int)std::min<int64_t>(len, length_));
            if (n > 0) length_ -= n;
            return n;
        }
        return Raw(buf, len);  // until the server closes it
    }

private:
    int fd_ = -1;
#ifdef LS_HTTP_OPENSSL
    SSL* ssl_ = nullptr;
#endif
    std::string pend_;  // read past the headers
    bool chunked_ = false, done_ = false;
    int64_t length_ = -1, chunkLeft_ = 0;

    void Close() {
#ifdef LS_HTTP_OPENSSL
        if (ssl_) SSL_free(ssl_);
        ssl_ = nullptr;
#endif
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        pend_.clear();
        headers.clear();
        chunked_ = done_ = false;
        length_ = -1;
        chunkLeft_ = 0;
    }
    bool Canceled() const { return cancel && *cancel; }
    // waits (in small steps, so a cancel is seen) until the socket can be read / written; 15 s at most
    bool Wait(bool write) {
        for (int i = 0; i < 60; i++) {
            if (Canceled()) return false;
            pollfd p{fd_, (short)(write ? POLLOUT : POLLIN), 0};
            int r = poll(&p, 1, 250);
            if (r > 0) return true;
            if (r < 0 && errno != EINTR) return false;
        }
        return false;
    }
    int RawOnce(uint8_t* buf, int len) {
#ifdef LS_HTTP_OPENSSL
        if (ssl_) {
            for (;;) {
                if (Canceled()) return -1;
                int n = SSL_read(ssl_, buf, len);
                if (n > 0) return n;
                int e = SSL_get_error(ssl_, n);
                if (e == SSL_ERROR_ZERO_RETURN) return 0;
                if (e == SSL_ERROR_WANT_READ) {
                    if (!Wait(false)) return -1;
                    continue;
                }
                if (e == SSL_ERROR_WANT_WRITE) {
                    if (!Wait(true)) return -1;
                    continue;
                }
                if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) return 0;  // (closed without a goodbye: the end)
                return -1;
            }
        }
#endif
        for (;;) {
            if (!Wait(false)) return -1;
            ssize_t n = recv(fd_, buf, (size_t)len, 0);
            if (n >= 0) return (int)n;
            if (errno != EINTR && errno != EAGAIN) return -1;
        }
    }
    int Raw(uint8_t* buf, int len) {
        if (!pend_.empty()) {
            int n = std::min<int>(len, (int)pend_.size());
            memcpy(buf, pend_.data(), (size_t)n);
            pend_.erase(0, (size_t)n);
            return n;
        }
        return RawOnce(buf, len);
    }
    bool ReadLine(std::string& line) {
        line.clear();
        for (;;) {
            size_t e = pend_.find('\n');
            if (e != std::string::npos) {
                line = pend_.substr(0, e);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pend_.erase(0, e + 1);
                return true;
            }
            if (pend_.size() > 65536) return false;
            uint8_t b[4096];
            int n = RawOnce(b, sizeof(b));
            if (n <= 0) return false;
            pend_.append((const char*)b, (size_t)n);
        }
    }
    bool SendAll(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
#ifdef LS_HTTP_OPENSSL
            if (ssl_) {
                int n = SSL_write(ssl_, s.data() + off, (int)(s.size() - off));
                if (n > 0) {
                    off += (size_t)n;
                    continue;
                }
                int e = SSL_get_error(ssl_, n);
                if ((e == SSL_ERROR_WANT_WRITE && Wait(true)) || (e == SSL_ERROR_WANT_READ && Wait(false))) continue;
                return false;
            }
#endif
            if (!Wait(true)) return false;
            ssize_t n = send(fd_, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n < 0 && errno != EINTR && errno != EAGAIN) return false;
            if (n > 0) off += (size_t)n;
        }
        return true;
    }
    bool Connect(const std::string& url, bool icy, std::string* err) {
        Url u;
        if (!ParseUrl(url, u)) {
            if (err) *err = "not an http(s) address";
            return false;
        }
#ifndef LS_HTTP_OPENSSL
        if (u.tls) {
            if (err) *err = "https isn't available in this build";
            return false;
        }
#endif
        addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res) {
            if (err) *err = "unknown host " + u.host;
            return false;
        }
        for (addrinfo* a = res; a && fd_ < 0; a = a->ai_next) {
            int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
            if (fd < 0) continue;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            int r = connect(fd, a->ai_addr, a->ai_addrlen);
            if (r < 0 && errno == EINPROGRESS) {
                fd_ = fd;
                bool ok = false;
                for (int i = 0; i < 40 && !Canceled(); i++) {  // 10 s
                    pollfd p{fd, POLLOUT, 0};
                    if (poll(&p, 1, 250) > 0) {
                        int so = 0;
                        socklen_t sl = sizeof(so);
                        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl);
                        ok = so == 0;
                        break;
                    }
                }
                fd_ = -1;
                r = ok ? 0 : -1;
            }
            if (r == 0) fd_ = fd;
            else close(fd);
        }
        freeaddrinfo(res);
        if (fd_ < 0) {
            if (err) *err = "cannot connect to " + u.host;
            return false;
        }
#ifdef LS_HTTP_OPENSSL
        if (u.tls) {
            SSL_CTX* ctx = TlsContext();
            ssl_ = ctx ? SSL_new(ctx) : nullptr;
            if (!ssl_) {
                if (err) *err = "TLS unavailable";
                return false;
            }
            SSL_set_fd(ssl_, fd_);
            SSL_set_tlsext_host_name(ssl_, u.host.c_str());
            SSL_set1_host(ssl_, u.host.c_str());
            for (;;) {
                int r = SSL_connect(ssl_);
                if (r == 1) break;
                int e = SSL_get_error(ssl_, r);
                if ((e == SSL_ERROR_WANT_READ && Wait(false)) || (e == SSL_ERROR_WANT_WRITE && Wait(true))) continue;
                if (err) *err = "secure connection failed (" + std::string(ERR_reason_error_string(ERR_peek_last_error()) ? ERR_reason_error_string(ERR_peek_last_error()) : "?") + ")";
                return false;
            }
        }
#endif
        std::string host = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
        if ((u.tls && u.port != 443) || (!u.tls && u.port != 80)) host += ":" + std::to_string(u.port);
        std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: " + kAgent +
                          "\r\nAccept: */*\r\nConnection: close\r\n" + (icy ? "Icy-MetaData: 1\r\n" : "") + "\r\n";
        if (!SendAll(req)) {
            if (err) *err = "connection lost";
            return false;
        }
        // "HTTP/1.1 200 OK" - or an old Shoutcast's "ICY 200 OK"
        std::string line;
        if (!ReadLine(line)) {
            if (err) *err = "no answer";
            return false;
        }
        size_t sp = line.find(' ');
        status = sp == std::string::npos ? 0 : atoi(line.c_str() + sp + 1);
        if (status == 0) {
            if (err) *err = "not an HTTP answer";
            return false;
        }
        while (ReadLine(line) && !line.empty()) {
            size_t colon = line.find(':');
            if (colon != std::string::npos) headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
        }
        chunked_ = Lower(Header("transfer-encoding")).find("chunked") != std::string::npos;
        if (!chunked_ && !Header("content-length").empty()) length_ = strtoll(Header("content-length").c_str(), nullptr, 10);
        return true;
    }
};
}  // namespace

std::unique_ptr<NetStream> NewNetStream() { return std::make_unique<SockStream>(); }
#endif

// ============================================================================ titles in the stream
IcyStream::IcyStream(std::unique_ptr<NetStream> s) : s_(std::move(s)) {
    metaint_ = atoi(s_->Header("icy-metaint").c_str());
    if (metaint_ < 0 || metaint_ > (1 << 20)) metaint_ = 0;
    left_ = metaint_;
}

int IcyStream::Read(uint8_t* buf, int len) {
    if (metaint_ <= 0) return s_->Read(buf, len);
    while (left_ == 0) {  // a title block: one length byte (x16), then "StreamTitle='...';"
        uint8_t l = 0;
        int n = s_->Read(&l, 1);
        if (n <= 0) return n;
        std::string meta((size_t)l * 16, '\0');
        size_t got = 0;
        while (got < meta.size()) {
            int k = s_->Read((uint8_t*)&meta[got], (int)(meta.size() - got));
            if (k <= 0) return k;
            got += (size_t)k;
        }
        left_ = metaint_;
        size_t a = meta.find("StreamTitle='");
        if (a != std::string::npos && onTitle) {
            a += 13;
            size_t b = meta.find("';", a);
            if (b == std::string::npos) b = meta.find_last_of('\'');
            std::string t = b != std::string::npos && b >= a ? meta.substr(a, b - a) : std::string();
            onTitle(Trim(t));
        }
    }
    int n = s_->Read(buf, std::min(len, left_));
    if (n > 0) left_ -= n;
    return n;
}

bool HttpGetAll(const std::string& url, std::string& body, size_t maxBytes, std::string* contentType, std::atomic<bool>* cancel) {
    auto s = NewNetStream();
    s->cancel = cancel;
    std::string err;
    if (!s->Open(url, false, &err)) return false;
    if (contentType) *contentType = s->Header("content-type");
    body.clear();
    uint8_t buf[16384];
    for (;;) {
        int n = s->Read(buf, sizeof(buf));
        if (n < 0) return false;
        if (n == 0) break;
        body.append((const char*)buf, (size_t)n);
        if (body.size() > maxBytes) return false;
    }
    return true;
}

}  // namespace ls
