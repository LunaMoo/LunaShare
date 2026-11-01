// LunaShare rendezvous server: introduces devices that share a room so they can connect peer-to-peer.
// It only stores, per room: device id, display name, platform, identity public key, public + LAN endpoints.
// It never relays chat or files. Rooms are 64-hex BLAKE2b hashes of (room name, password) computed by clients,
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
        std::function<void(const std::string&)> log;
    };
    ~RendezvousServer() { Stop(); }
    bool Start(const Options& o, std::string* err);
    void Stop();
    size_t DeviceCount();
    size_t RoomCount();

private:
    void Loop();
    Options opt_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    intptr_t sock_ = -1;
    std::mutex m_;
    size_t devices_ = 0, rooms_ = 0;
};

}  // namespace ls
