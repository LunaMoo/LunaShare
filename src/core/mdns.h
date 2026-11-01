// Bonjour / mDNS (RFC 6762) + DNS-SD (RFC 6763) discovery for the "_lunashare._tcp" service.
//
// Why: UDP broadcast beacons are dropped by some routers/APs and are unavailable to iOS apps without a special
// Apple entitlement, while Bonjour is allowed everywhere.
//
//  - macOS / iOS: the system Bonjour API (dns_sd.h). Required on iOS; the app needs NSBonjourServices
//    ["_lunashare._tcp"] + NSLocalNetworkUsageDescription in Info.plist.
//  - Windows / Linux / Android: a small built-in responder + browser on 224.0.0.251:5353 that shares the port
//    with the OS stack (Avahi, Windows DNS client, ...). Android must hold a multicast lock (the app does).
//
// TXT record: id=<device uuid>, v=<protocol>, name=<display name>, pf=<platform>.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net.h"

namespace ls {

struct MdnsPeer {
    std::string id, name, platform, ip;
    uint16_t port = 0;
    bool goodbye = false;  // the service was withdrawn (TTL 0)
};

namespace mdns {
constexpr const char* kServiceType = "_lunashare._tcp.local";
constexpr uint16_t kPort = 5353;
constexpr const char* kGroup = "224.0.0.251";

struct Record {
    std::string name;
    uint16_t type = 0, cls = 0;
    uint32_t ttl = 0;
    // decoded rdata
    std::string target;                         // PTR / SRV target
    uint16_t port = 0;                          // SRV
    std::map<std::string, std::string> txt;     // TXT
    std::string ip;                             // A
};
struct Question {
    std::string name;
    uint16_t type = 0, cls = 0;
};
struct Packet {
    uint16_t id = 0, flags = 0;
    std::vector<Question> questions;
    std::vector<Record> records;  // answers + authority + additional
    bool IsResponse() const { return (flags & 0x8000) != 0; }
};

struct Service {
    std::string instance;  // "<name> (<id8>)._lunashare._tcp.local"
    std::string host;      // "ls-<id8>.local"
    uint16_t port = 0;
    std::map<std::string, std::string> txt;
};

enum : uint16_t { A = 1, PTR = 12, TXT = 16, SRV = 33, ANY = 255 };

std::string InstanceLabel(const std::string& displayName, const std::string& id);
bool NameEq(const std::string& a, const std::string& b);  // case-insensitive DNS name compare
std::vector<uint8_t> BuildResponse(const Service& s, const std::vector<std::string>& ips, uint32_t ttl, uint16_t id = 0,
                                   const std::vector<Question>* echo = nullptr);
std::vector<uint8_t> BuildQuery(const std::string& name, uint16_t type);
bool Parse(const uint8_t* p, size_t n, Packet& out);
// Extracts complete LunaShare peers (SRV + TXT id + address) from a response. `fromIp` is used when no A record.
std::vector<MdnsPeer> ExtractPeers(const Packet& pkt, const std::string& fromIp);
// Does `q` ask for something our service should answer?
bool ShouldAnswer(const Question& q, const Service& s);
}  // namespace mdns

class Mdns {
public:
    using Found = std::function<void(const MdnsPeer&)>;
    ~Mdns() { Stop(); }
    bool Start(const std::string& id, const std::string& name, const std::string& platform, uint16_t port, Found cb);
    void Stop();
    bool Running() const { return running_; }
    void Update(const std::string& name, uint16_t port);  // re-announce after a rename / port change
    void QueryNow() { queryNow_ = true; }
    std::string Status();

private:
    void Loop();
    void SetStatus(const std::string& s);
    std::atomic<bool> running_{false}, queryNow_{false}, reannounce_{false};
    std::thread thread_;
    Found cb_;
    std::mutex m_;
    std::string id_, name_, platform_, status_;
    uint16_t port_ = 0;
};

}  // namespace ls
