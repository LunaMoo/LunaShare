// Optional internet connectivity through a rendezvous server (see server/ and docs/INTERNET.md).
//
// The server only introduces devices that share a room (BLAKE2b hash of room name + password): it learns each
// device's public UDP endpoint and, on request, tells two devices to punch through their NATs towards each other.
// Everything after that is peer-to-peer: KCP (reliable, ordered) over the punched UDP path, carrying the normal
// end-to-end encrypted LunaShare protocol. The server never sees or relays chat or file data.
//
// One UDP socket per engine carries everything; the first byte of every datagram says what it is:
//   'R' JSON rendezvous message (client <-> server)
//   'P' JSON punch probe / ack (client <-> client)
//   'K' KCP segment            (client <-> client)
//   'X' u32 conv: session closed
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

constexpr uint16_t kRendezvousPort = 47830;

struct WanPeer {
    std::string id, name, platform, pub;
    std::string publicEp;              // "ip:port" as seen by the server
    std::vector<std::string> localEps; // LAN endpoints (useful when both sit behind the same NAT)
    std::vector<std::string> extraEps; // router mappings (NAT-PMP) / manually forwarded ports
};

std::string RoomHash(const std::string& room, const std::string& password);
bool ParseEndpoint(const std::string& s, std::string& host, uint16_t& port);

class WanTransport {
public:
    struct Config {
        std::string server;  // "host[:port]"
        std::string roomHash;
        std::string id, name, platform, pub;
        uint16_t localPort = 0;   // fixed UDP port (0 = automatic); useful with a manual port forward
        uint16_t forwardedPort = 0;  // external port forwarded to localPort on the router (0 = none)
        bool natpmp = true;       // ask the router for a mapping with NAT-PMP (miniupnpd, OPNsense os-upnp, ...)
    };
    using OnPeers = std::function<void(const std::vector<WanPeer>&)>;
    using OnSocket = std::function<void(SocketHandle s, const std::string& peerId, const std::string& addr, bool initiator)>;

    ~WanTransport() { Stop(); }
    bool Start(const Config& cfg, OnPeers onPeers, OnSocket onSocket);
    void Stop();
    bool Running() const { return running_; }
    void Connect(const std::string& peerId);  // asks the server to coordinate a hole punch
    void UpdateName(const std::string& name);
    std::string Status();
    std::string PublicEndpoint();
    std::string MappedEndpoint();  // NAT-PMP mapping, if the router granted one
    int Sessions() const { return sessions_.load(); }

private:
    void Loop();
    void SetStatus(const std::string& s);
    std::atomic<bool> running_{false};
    std::atomic<int> sessions_{0};
    std::thread thread_;
    std::mutex m_;
    Config cfg_;
    std::string status_, publicEp_, mappedEp_;
    std::vector<std::string> connectQueue_;
    bool nameChanged_ = false;
    OnPeers onPeers_;
    OnSocket onSocket_;
};

}  // namespace ls
