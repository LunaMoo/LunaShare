// UPnP IGD port mapping (most home routers have UPnP on by default): asks the router to forward a UDP port to this
// device, so other devices on the internet can reach it directly even when hole punching alone would fail.
// Runs on its own thread: discovery (SSDP), the router's description, then AddPortMapping, renewed while running.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace ls {

class UpnpMapper {
public:
    ~UpnpMapper() { Stop(); }
    void Start(uint16_t localPort);
    void Stop();                 // removes the mapping
    // The network changed: the router is looked for again now (not after the usual wait of up to 20 minutes).
    // Doesn't wait for anything.
    void Rediscover();
    std::string Mapping();       // "public-ip:port" once the router granted it, else ""
    std::string Status();

    // exposed for tests
    static bool ParseControlUrl(const std::string& descXml, const std::string& location, std::string* controlUrl, std::string* serviceType);

private:
    void Loop();
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool rediscover_ = false;
    uint16_t port_ = 0;
    std::string mapping_, status_;
};

}  // namespace ls
