// Scriptable headless peer used for demos/screenshots: demo_peer <cfgdir> <name> <host> <port> [files...]
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "engine.h"
#include "platform.h"

using namespace ls;

int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: demo_peer <cfgdir> <name> <host> <port> [files...]\n"); return 1; }
    MakeDirs(argv[1]);
    json j = {{"settings", {{"displayName", argv[2]}, {"downloadDir", JoinPath(argv[1], "dl")}, {"tcpPort", 48300},
                            {"discovery", false}, {"autoConnect", false}, {"autoAcceptAll", true}, {"speedLimitKBs", 1500}}}};
    json existing;
    if (!LoadJsonFile(JoinPath(argv[1], "settings.json"), existing)) SaveJsonFile(JoinPath(argv[1], "settings.json"), j);
    Engine e;
    e.configDir = argv[1];
    e.Start();
    std::string peer;
    for (int i = 0; i < 150 && peer.empty(); i++) {
        if (i % 10 == 0) e.ConnectAddress(argv[3], (uint16_t)atoi(argv[4]));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto lk = e.Lock();
        for (auto& c : e.conns) if (c->ready) peer = c->peerId;
    }
    if (peer.empty()) { printf("could not connect\n"); return 1; }
    e.SendChat(peer, "Hi! Sending you the photos from Saturday :)");
    std::vector<std::string> imgs, other;
    for (int i = 5; i < argc; i++) (std::string(argv[i]).find(".bin") != std::string::npos ? other : imgs).push_back(argv[i]);
    if (!imgs.empty()) e.SendPaths(peer, imgs);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    e.SendChat(peer, u8"Zażółć gęślą jaźń - Polish characters work too. Long-press or right-click a message to copy it.");
    if (!other.empty()) e.SendPaths(peer, other);
    int secs = getenv("DEMO_SECONDS") ? atoi(getenv("DEMO_SECONDS")) : 30;
    std::this_thread::sleep_for(std::chrono::seconds(secs));
    e.Stop();
    return 0;
}
