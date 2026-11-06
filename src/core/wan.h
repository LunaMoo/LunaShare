// Optional internet connectivity through a rendezvous server (see server/ and docs/INTERNET.md).
//
// The server only introduces devices that share a room (BLAKE2b hash of room name + password): it learns each
// device's public UDP endpoint and, on request, tells two devices to punch through their NATs towards each other.
// Everything after that is peer-to-peer: KCP (reliable, ordered) over the punched UDP path, carrying the normal
// end-to-end encrypted LunaShare protocol. If no direct path works within a few seconds (carrier-grade NAT on both
// sides, strict firewalls), the server can relay the KCP packets instead ('Y'); they stay end-to-end encrypted.
//
// One UDP socket per engine carries everything; the first byte of every datagram says what it is:
//   'R' JSON rendezvous message (client <-> server)
//   'P' JSON punch probe / ack (client <-> client)
//   'K' KCP segment            (client <-> client)
//   'X' u32 conv: session closed
//   'Y' KCP segment through the server (relay)
//   'H' group call through the server: [hub u32][to mask u32][fragment] up, [hub u32][from u8][fragment] down.
//       Fragments: [msg u16][index u8][count u8][total length u32][data]; indices >= count are XOR parity over
//       groups of 8 fragments (one lost fragment per group is recovered). Messages are sealed by the engine.
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
    bool viaLink = false;              // met through a private link (not in my room)
};

std::string RoomHash(const std::string& room, const std::string& password);
std::string LinkToken(const std::string& secretHex);  // what the server sees of a private link
// Invite codes for private links: "LSI1-" + base64 of {"s": server, "k": secret, "n": inviter's name, "r": room name}
struct InviteInfo {
    std::string server, secret, name, room;
};
std::string MakeInviteCode(const InviteInfo& i);
bool ParseInviteCode(const std::string& code, InviteInfo& i);
bool ParseEndpoint(const std::string& s, std::string& host, uint16_t& port);

class WanTransport {
public:
    struct Config {
        std::string server;  // "host[:port]"
        std::string roomHash;
        std::string id, name, platform, pub;
        uint16_t localPort = 0;   // fixed UDP port (0 = automatic); useful with a manual port forward
        uint16_t forwardedPort = 0;  // external port forwarded to localPort on the router (0 = none)
        bool natpmp = true;       // ask the router for a mapping (NAT-PMP / PCP, and UPnP IGD)
        bool relay = true;        // allow the server to relay when a direct connection fails
        bool hidden = false;      // not listed in the room, except to my account's devices
        std::vector<std::string> links;  // private link tokens (LinkToken)
        std::vector<std::string> inv;    // ...of those, the ones from someone else's invite (the server remembers me as invited)
        std::string hw;                  // hash of this computer's / phone's id and the server (same after reinstalling)
        struct Group {
            std::string token, name;          // the room (its name is shown to the server's admin)
            std::vector<std::string> links;   // my codes in it
        };
        std::vector<Group> groups;            // my invite rooms
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
    // the server knows private links (an older server ignores them: links then don't work, hidden devices are listed)
    bool ServerHasLinks() const { return serverLinks_.load(); }
    std::string MappedEndpoint();  // NAT-PMP mapping, if the router granted one
    int Sessions() const { return sessions_.load(); }

    // ---- offline messages (server mailbox). Envelopes are base64 text, sealed by the engine.
    // Leaves an envelope for the device with public key `toPub` (retried until the server confirms).
    void MailPut(const std::string& toPub, const std::string& id, const std::string& dataB64);
    bool Registered() const { return registered_.load(); }  // the server answers (mail can be left)
    std::function<std::string(const std::string& challenge)> signMail;  // identity signature (context "mailbox")
    // An envelope from my mailbox (network thread). Return true when it was taken (it is then deleted on the server).
    std::function<bool(const std::string& id, const std::string& dataB64)> onMail;
    std::function<void(const std::string& id, bool stored)> onMailSent;  // stored=false: the mailbox is full
    // ---- other requests to the server (usernames): JSON text; replies whose "t" starts with 'u' come back here
    void SendServer(const std::string& jsonText);
    std::function<void(const std::string& jsonText)> onServerReply;
    // the account this device belongs to (registered with the server, which then lists the account's online devices)
    void SetAccount(const std::string& acc, const std::string& cert);

    // ---- group calls through the server ("hub", see the 'H' packets above)
    void HubJoin(const std::string& hubId);  // 32 hex characters; "" leaves
    bool HubSend(uint32_t toMask, const std::string& msg);  // any thread; up to ~200 KB per message
    std::function<void(int from, const std::string& msg)> onHub;                               // network thread
    std::function<void(int you, const std::vector<std::string>& memberIds)> onHubMembers;      // index -> device id
    std::function<void(const std::string& error)> onHubError;

private:
    void Loop();
    void SetStatus(const std::string& s);
    std::atomic<bool> running_{false};
    std::atomic<int> sessions_{0};
    std::thread thread_;
    std::mutex m_;
    Config cfg_;
    std::string status_, publicEp_, mappedEp_;
    std::atomic<bool> registered_{false};
    std::atomic<bool> serverLinks_{false};
    struct MailOut {
        std::string to, id, data;
        int tries = 0;
        int64_t next = 0;
    };
    std::vector<MailOut> mailOut_;
    std::vector<std::string> serverOut_;
    std::string acc_, accCert_;
    std::vector<std::string> connectQueue_;
    bool nameChanged_ = false;
    OnPeers onPeers_;
    OnSocket onSocket_;
    // hub
    std::mutex hm_;
    std::string hubId_, hubLeave_;
    uint32_t hubHandle_ = 0;
    int64_t hubJoinAt_ = 0;
    uint16_t hubMsg_ = 0;
    SocketHandle udp_ = kInvalidSocket;
    uint32_t hubIp_ = 0;    // server address (network byte order), for sending from other threads
    uint16_t hubPort_ = 0;
    struct Reasm {
        int count = 0;
        uint32_t total = 0;
        std::vector<std::string> frags;  // data, then parity
        int64_t t = 0;
        bool done = false;
    };
    std::map<uint32_t, Reasm> reasm_;  // (from << 16 | msg) -> fragments (network thread)
    void HubInput(const uint8_t* d, size_t n);
};

}  // namespace ls
