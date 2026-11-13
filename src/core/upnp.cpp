#include "upnp.h"

#include <chrono>
#include <cstring>
#include <vector>

#include "net.h"

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
namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// "http://host:port/path" -> parts
bool SplitUrl(const std::string& url, std::string* host, uint16_t* port, std::string* path) {
    if (Lower(url.substr(0, 7)) != "http://") return false;
    size_t a = 7, slash = url.find('/', a);
    std::string hp = url.substr(a, slash == std::string::npos ? std::string::npos : slash - a);
    *path = slash == std::string::npos ? "/" : url.substr(slash);
    size_t colon = hp.rfind(':');
    *port = 80;
    if (colon != std::string::npos) {
        *port = (uint16_t)atoi(hp.c_str() + colon + 1);
        hp.resize(colon);
    }
    *host = hp;
    return !host->empty() && *port;
}

std::string Tag(const std::string& xml, const std::string& name, size_t from = 0, size_t* end = nullptr) {
    std::string lx = Lower(xml), ln = Lower(name);
    size_t a = lx.find("<" + ln + ">", from);
    if (a == std::string::npos) {
        // namespaced (<u:NewExternalIPAddress>, <s:Body>): match the local name
        a = lx.find(":" + ln + ">", from);
        if (a == std::string::npos) return "";
        a = lx.rfind('<', a);
        if (a == std::string::npos) return "";
    }
    size_t open = lx.find('>', a);
    size_t close = lx.find("</", open);
    if (open == std::string::npos || close == std::string::npos) return "";
    if (end) *end = close;
    std::string v = xml.substr(open + 1, close - open - 1);
    while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
    while (!v.empty() && isspace((unsigned char)v.front())) v.erase(v.begin());
    return v;
}

// minimal HTTP/1.1 client (Connection: close), returns the status code and body
int Http(const std::string& url, const std::string& method, const std::string& extraHeaders, const std::string& body,
         std::string* out, std::string* localIp) {
    std::string host, path;
    uint16_t port;
    if (!SplitUrl(url, &host, &port, &path)) return 0;
    SocketHandle s = TcpConnect(host, port, 3000);
    if (s == kInvalidSocket) return 0;
    if (localIp) {
        sockaddr_in me{};
        socklen_t l = sizeof(me);
        if (getsockname((decltype(socket(0, 0, 0)))s, (sockaddr*)&me, &l) == 0) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &me.sin_addr, ip, sizeof(ip));
            *localIp = ip;
        }
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                      "\r\nConnection: close\r\nUser-Agent: LunaShare UPnP/1.0\r\n" + extraHeaders +
                      "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    if (!SendAll(s, req.data(), req.size())) {
        CloseSocket(s);
        return 0;
    }
    std::string resp;
    char buf[4096];
    auto t0 = std::chrono::steady_clock::now();
    while (resp.size() < (1u << 20) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        if (WaitReadable(s, 1000) != 1) continue;
        int n = (int)recv((decltype(socket(0, 0, 0)))s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, (size_t)n);
    }
    CloseSocket(s);
    size_t he = resp.find("\r\n\r\n");
    if (resp.compare(0, 5, "HTTP/") != 0 || he == std::string::npos) return 0;
    int code = atoi(resp.c_str() + resp.find(' ') + 1);
    std::string headers = Lower(resp.substr(0, he)), b = resp.substr(he + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        std::string d;
        size_t p = 0;
        while (p < b.size()) {
            size_t le = b.find("\r\n", p);
            if (le == std::string::npos) break;
            size_t len = strtoul(b.substr(p, le - p).c_str(), nullptr, 16);
            if (len == 0) break;
            d += b.substr(le + 2, len);
            p = le + 2 + len + 2;
        }
        b = d;
    }
    *out = b;
    return code;
}

int Soap(const std::string& control, const std::string& service, const std::string& action, const std::string& args,
         std::string* out, std::string* localIp = nullptr) {
    std::string body = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" + service +
                       "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    std::string h = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + service + "#" + action + "\"\r\n";
    return Http(control, "POST", h, body, out, localIp);
}

}  // namespace

bool UpnpMapper::ParseControlUrl(const std::string& xml, const std::string& location, std::string* controlUrl, std::string* serviceType) {
    static const char* wanted[] = {"urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
                                   "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    std::string lx = Lower(xml);
    for (const char* w : wanted) {
        size_t at = lx.find(Lower(std::string(w)) + "<");
        if (at == std::string::npos) continue;
        size_t svcEnd = lx.find("</service>", at);
        std::string svc = xml.substr(at, svcEnd == std::string::npos ? std::string::npos : svcEnd - at);
        std::string ctl = Tag(svc, "controlURL");
        if (ctl.empty()) continue;
        if (Lower(ctl.substr(0, 7)) != "http://") {
            std::string base = Tag(xml, "URLBase");
            if (base.empty()) base = location;
            std::string host, path;
            uint16_t port;
            if (!SplitUrl(base, &host, &port, &path)) return false;
            ctl = "http://" + host + ":" + std::to_string(port) + (ctl[0] == '/' ? "" : "/") + ctl;
        }
        *controlUrl = ctl;
        *serviceType = w;
        return true;
    }
    return false;
}

void UpnpMapper::Start(uint16_t localPort) {
    Stop();
    std::lock_guard<std::mutex> l(m_);
    stop_ = false;
    port_ = localPort;
    mapping_.clear();
    status_ = "searching";
    thread_ = std::thread([this] { Loop(); });
}

void UpnpMapper::Stop() {
    {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void UpnpMapper::Rediscover() {
    {
        std::lock_guard<std::mutex> l(m_);
        rediscover_ = true;
        mapping_.clear();  // (that was the old router's)
    }
    cv_.notify_all();
}

std::string UpnpMapper::Mapping() {
    std::lock_guard<std::mutex> l(m_);
    return mapping_;
}

std::string UpnpMapper::Status() {
    std::lock_guard<std::mutex> l(m_);
    return status_;
}

void UpnpMapper::Loop() {
    NetInit();
    auto wait = [&](int seconds) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait_for(l, std::chrono::seconds(seconds), [&] { return stop_ || rediscover_; });
        return !stop_;
    };
    auto set = [&](const std::string& st, const std::string& mapping) {
        std::lock_guard<std::mutex> l(m_);
        status_ = st;
        mapping_ = mapping;
    };
    auto stopping = [&] {
        std::lock_guard<std::mutex> l(m_);
        return stop_;
    };
    std::string control, service, localIp, extIp;
    uint16_t extPort = 0;
    bool stopped = false;
    while (!stopped) {
        {
            // another network: the router found before (and the port it gave) is not this one's
            std::lock_guard<std::mutex> l(m_);
            if (rediscover_) {
                rediscover_ = false;
                control.clear();
                service.clear();
                localIp.clear();
                extIp.clear();
                extPort = 0;
                status_ = "searching";
            }
        }
        if (control.empty()) {
            // ---- discovery
            SocketHandle u = UdpOpen(0);
            std::string location;
            if (u != kInvalidSocket) {
                for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                       "urn:schemas-upnp-org:service:WANIPConnection:1"}) {
                    std::string msg = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") +
                                      st + "\r\n\r\n";
                    UdpSendTo(u, "239.255.255.250", 1900, msg.data(), msg.size());
                }
                char buf[2048];
                auto t0 = std::chrono::steady_clock::now();
                while (location.empty() && !stopping() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
                    std::string from;
                    int n = UdpRecvFrom(u, buf, sizeof(buf) - 1, &from, 500);
                    if (n <= 0) continue;
                    std::string r(buf, (size_t)n), lr = Lower(r);
                    size_t at = lr.find("\r\nlocation:");
                    if (at == std::string::npos) continue;
                    size_t e = r.find("\r\n", at + 2);
                    location = r.substr(at + 11, e - at - 11);
                    while (!location.empty() && isspace((unsigned char)location.front())) location.erase(location.begin());
                }
                CloseSocket(u);
            }
            std::string desc;
            if (stopping()) break;
            if (location.empty() || Http(location, "GET", "", "", &desc, &localIp) != 200 || !ParseControlUrl(desc, location, &control, &service)) {
                control.clear();
                set(location.empty() ? "no UPnP router found" : "router has no usable UPnP service", "");
                if (!wait(300)) break;
                continue;
            }
        }
        // ---- map (or renew)
        std::string out;
        if (Soap(control, service, "GetExternalIPAddress", "", &out, &localIp) == 200) extIp = Tag(out, "NewExternalIPAddress");
        bool ok = false;
        uint16_t tryPort = extPort ? extPort : port_;
        for (int attempt = 0; attempt < 6 && !ok; attempt++) {
            for (int lease : {3600, 0}) {  // some routers only take permanent mappings
                std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(tryPort) +
                                   "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(port_) +
                                   "</NewInternalPort><NewInternalClient>" + localIp +
                                   "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>LunaShare</NewPortMappingDescription>"
                                   "<NewLeaseDuration>" + std::to_string(lease) + "</NewLeaseDuration>";
                int code = Soap(control, service, "AddPortMapping", args, &out, &localIp);
                if (code == 200) {
                    ok = true;
                    break;
                }
                std::string err = Tag(out, "errorCode");
                if (err == "718") break;  // that external port is taken: try the next one
            }
            if (!ok) tryPort = (uint16_t)(tryPort + 1);
        }
        if (ok && !extIp.empty() && extIp != "0.0.0.0") {
            extPort = tryPort;
            set("mapped " + extIp + ":" + std::to_string(extPort), extIp + ":" + std::to_string(extPort));
        } else {
            set(ok ? "mapped, but the router has no public address (carrier-grade NAT?)" : "router refused the port mapping", "");
            if (!ok) control.clear();
        }
        if (!wait(ok ? 1200 : 300)) break;  // renew every 20 minutes
    }
    (void)stopped;
    if (extPort && !control.empty()) {
        std::string out;
        Soap(control, service, "DeletePortMapping",
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(extPort) + "</NewExternalPort><NewProtocol>UDP</NewProtocol>", &out);
    }
}

}  // namespace ls
