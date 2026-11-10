// LunaShare - "a link for anyone": a file someone without LunaShare downloads in a browser, from a link sent by SMS or
// any messenger. This device serves it itself (nothing is uploaded anywhere):
//   on the same network   http://<this device>:<port>/f/<token>           - a small page, then the file (resumable)
//   over the Internet     https://<rendezvous server>/s/<id>#<secret>     - when the server offers it: the page comes
//       from the server, the file comes from this device through a connection it keeps to the server (tunnel), in
//       parts encrypted with the secret after '#' (which browsers never send): the server passes on what it can't
//       read, and the browser checks every part (link_crypto.h).
// Each link: valid for a time, for a number of people (a "person" = one browser that started it: it may resume),
// optional PIN (checked here, 5 wrong tries end the link), optional "ask me first", revocable, with a list of who
// got it. Kept in share_links.json.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "net.h"

namespace ls {

struct ShareVisit {
    std::string sid, ip, via;  // via: "web" (through the server) or "lan"
    int64_t at = 0;            // wall ms
    int64_t bytes = 0;         // sent to it
    bool done = false;         // got all of it
    int state = 0;             // 0 allowed, 1 waiting for me, 2 refused
};

struct ShareLinkInfo {
    std::string id, name, path, webUrl, lanUrl;  // webUrl "" = no server that offers links
    int64_t size = 0, created = 0, expires = 0;  // wall ms
    int maxPeople = 0, people = 0;               // 0 = anyone with the link
    bool pin = false, ask = false, revoked = false, ready = false;  // ready: hashed (it can be served)
    std::string sha256;
    std::vector<ShareVisit> visits;
    bool Active(int64_t now) const { return !revoked && now < expires && (maxPeople == 0 || people < maxPeople); }
};

struct ShareAsk {
    uint64_t id = 0;
    std::string link, name, ip, via, sid;
    int64_t at = 0;
};

class ShareLinks {
public:
    // set before Start
    std::function<std::string(const std::string& challenge)> sign;  // SignData(identity, "links", challenge)
    std::function<std::string()> myPub;
    std::function<void(const ShareAsk&)> onAsk;  // someone waits for "ask me first" (any thread, no lock of ours held)
    std::function<void(const std::string&)> log;

    ~ShareLinks() { Stop(); }
    void Start(const std::string& stateDir, uint16_t lanPort);
    void Stop();
    // the rendezvous server ("host:port") and the address its link pages are at ("https://host:port"; "" = it doesn't
    // offer links)
    void SetServer(const std::string& hostPort, const std::string& webBase);

    struct Options {
        int64_t validMs = 24 * 3600 * 1000LL;
        int maxPeople = 1;
        std::string pin;
        bool ask = false;
    };
    std::string Create(const std::string& path, const Options& o, std::string& err);  // its id
    void Revoke(const std::string& id);
    void Remove(const std::string& id);
    std::vector<ShareLinkInfo> List();
    std::vector<ShareAsk> Asks();
    void Answer(uint64_t askId, bool allow);
    bool WebAvailable();          // a server that offers links
    std::string TunnelStatus();   // "" = not needed; for the UI / tests
    uint16_t LanPort() const { return lanPort_; }
    std::string LanBase();        // "http://<this device on its network>:<port>", "" = not on a network

private:
    struct Link;
    struct Reply {
        int status = 200;
        std::string type, body, disposition;
        int64_t total = -1;  // (ranges: the whole file)
        std::string range;   // "bytes a-b/total"
    };
    std::shared_ptr<Link> Find(const std::string& id, bool lanToken);
    // the checks every request goes through: 0 = go on; else an HTTP status with a JSON reason in r
    int Gate(Link& l, const std::string& sid, const std::string& pinProof, const std::string& ip, const std::string& via, Reply& r);
    Reply WebMeta(const std::string& id, const std::string& sid, const std::string& pp, const std::string& ip);
    Reply WebPart(const std::string& id, const std::string& sid, uint64_t k, const std::string& ip);
    void LanLoop();
    void LanConn(SocketHandle s, std::string ip);
    void TunnelLoop();
    void Save();
    void Load();
    void Housekeeping();
    void FireAsks();  // onAsk for new asks, outside m_
    std::vector<ShareAsk> newAsks_;
    bool TunnelSend(uint8_t type, const std::string& payload);
    void TunnelRequest(const std::string& reqJson);
    void HashLater(std::shared_ptr<Link> l);

    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<Link>> links_;  // by id
    std::vector<ShareAsk> asks_;
    uint64_t nextAsk_ = 1;
    std::string dir_, server_, webBase_, tunnelStatus_;
    uint16_t lanPort_ = 0;
    SocketHandle lanSock_ = kInvalidSocket, tunSock_ = kInvalidSocket;
    std::atomic<bool> running_{false}, cancel_{false};
    std::mutex sendM_;  // tunnel writes
    std::atomic<int> lanConns_{0}, workers_{0};
    std::thread lanThread_, tunThread_;
    std::vector<std::thread> hashers_;
    bool linksChanged_ = false, dirty_ = false;
};

}  // namespace ls
