// LunaShare rendezvous server: introduces devices that share a room so they can connect peer-to-peer.
// It only stores, per room: device id, display name, platform, identity public key, public + LAN endpoints.
// When two devices can't reach each other directly (both behind NATs that can't be punched: carrier-grade NAT,
// strict firewalls) it can relay their UDP packets - those carry the end-to-end encrypted LunaShare protocol, so
// the server sees only ciphertext. Relaying can be turned off or limited. Rooms are 64-hex BLAKE2b hashes of
// (room name, password) computed by clients,
// so the server never learns the password. An optional allow-list restricts which rooms may be used.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ls {

class RendezvousServer {
public:
    struct Options {
        std::string bind = "0.0.0.0";
        uint16_t port = 47830;
        size_t maxDevices = 5000;
        size_t maxPerRoom = 64;
        std::set<std::string> allowedRooms;  // empty = any room
        bool relay = true;                   // relay for devices that can't connect directly
        int relayKbps = 0;                   // per relayed connection and direction, 0 = unlimited
        size_t maxRelays = 64;
        // offline messages: sealed envelopes kept for devices that are away (the server can't read them)
        bool mailbox = true;
        int mailboxDays = 10;                // kept at most this long
        int mailboxMB = 10;                  // per recipient device
        int mailboxTotalMB = 1024;           // all mailboxes together
        std::string dataDir;                 // where mailboxes are saved (empty = kept in memory only)
        std::function<void(const std::string&)> log;
    };
    ~RendezvousServer() { Stop(); }
    bool Start(const Options& o, std::string* err);
    void Stop();
    size_t DeviceCount();
    size_t RoomCount();
    size_t MailCount();  // envelopes waiting in all mailboxes

private:
    void Loop();
    Options opt_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    intptr_t sock_ = -1;
    std::mutex m_;
    size_t devices_ = 0, rooms_ = 0, mail_ = 0;
};

}  // namespace ls
