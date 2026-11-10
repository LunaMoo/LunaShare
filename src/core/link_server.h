// LunaShare - the rendezvous server's part of "a link for anyone" (share_links.h): a small web server for the link
// pages (HTTPS when given a certificate; plain HTTP to sit behind a reverse proxy), and a TCP port devices keep a
// connection to (a tunnel) so the pages can get the files from them. The server passes on parts it can't read:
// they're encrypted with the key after '#' in the link, which browsers never send.
//   GET /s/<id>                the page          GET /sw.js     its service worker (big downloads)
//   GET /api/<id>/meta?sid=&pp= GET /api/<id>/p/<k>?sid=        forwarded to the device that has the link
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ls {

class LinkServer {
public:
    struct Options {
        std::string bind = "0.0.0.0";
        uint16_t httpPort = 0;    // the pages
        uint16_t tunnelPort = 0;  // devices (TCP; the rendezvous server's port number)
        std::string certFile, keyFile;  // PEM; both empty = plain HTTP
        bool trustProxy = false;        // take the browser's address from X-Forwarded-For (from 127.0.0.1 only)
        std::function<bool(const std::string& pub)> allowed;  // may this device offer links
        std::function<void(const std::string&)> log;
    };
    ~LinkServer() { Stop(); }
    bool Start(const Options& o, std::string* err);
    void Stop();
    static bool TlsSupported();  // built with OpenSSL
    size_t TunnelCount();

    struct Tunnel;
    struct Reply {
        int status = 200;
        std::string type, body;
    };

private:
    void HttpLoop();
    void TunnelAcceptLoop();
    void TunnelConn(intptr_t s, std::string ip);
    void HttpConn(intptr_t s, std::string ip);
    Reply Forward(const std::string& id, const std::string& reqJson);
    bool LoadTls(std::string* err);

    Options opt_;
    std::atomic<bool> running_{false};
    std::thread httpThread_, tunThread_;
    intptr_t httpSock_ = -1, tunSock_ = -1;
    std::mutex m_;
    std::map<std::string, std::shared_ptr<Tunnel>> tunnels_;  // by device key
    std::map<std::string, std::string> owner_;                // link id -> device key
    std::shared_ptr<void> tls_;                               // SSL_CTX
    int64_t certMtime_ = 0;
    std::atomic<int> conns_{0};
    std::map<std::string, std::pair<double, int64_t>> buckets_;  // per browser address: tokens, last
};

}  // namespace ls
