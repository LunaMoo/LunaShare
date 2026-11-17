// A headless device that another program drives through its stdin, one command a line - built from this tree, or from
// an older release's (tools/build_old_peer.sh), so tests/compat_engine.cpp can check that versions get along.
//
//   lunashare_peer_bot <configdir> <name> <tcpport>
//
// Commands:  connect <port> | chat <key> <text...> | send <key> <path> | call <key> | answer | end | media <n>
//            group <name> <@acc> [@acc...] | drop <dev> | netchange | restart | state <file> | quit
// It accepts every file and contact by itself; "state" writes what it has as JSON (to a temporary name, then renamed).
// Only what every version since 1.8.4 has is used here - it must build against those.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "engine.h"
#include "platform.h"

using namespace ls;

static std::unique_ptr<Engine> g_e;
static std::mutex g_eM;  // (the event thread vs a restart)
static std::atomic<int> g_media{0};
static std::string g_cfg, g_name;
static int g_port = 0;

// (older versions have no DropConnectionTo test hook: then a plain disconnect)
template <class E>
auto DropTo(E& e, const std::string& d, int) -> decltype(e.DropConnectionTo(d), void()) {
    e.DropConnectionTo(d);
}
template <class E>
void DropTo(E& e, const std::string& d, long) {
    e.Disconnect(d);
}

static void Start() {
    MakeDirs(g_cfg);
    json j;
    if (!LoadJsonFile(JoinPath(g_cfg, "settings.json"), j)) {
        Settings s;
        s.displayName = g_name;
        s.downloadDir = JoinPath(g_cfg, "../downloads");
        s.tcpPort = (uint16_t)g_port;
        s.autoAcceptAll = true;
        s.discovery = s.broadcast = s.mdns = true;
        s.autoConnect = true;
        j = {{"settings", ToJson(s, false)}, {"devices", json::array()}};
        SaveJsonFile(JoinPath(g_cfg, "settings.json"), j);
    }
    g_e = std::make_unique<Engine>();
    g_e->configDir = g_cfg;
    g_e->localCaps = {"voice"};
    g_e->Start();
    g_e->onMedia = [](uint64_t, int, const uint8_t*, size_t) { g_media++; };
}

static const char* CallStateName(CallState s) {
    switch (s) {
    case CallState::None: return "none";
    case CallState::OutgoingRinging: return "calling";
    case CallState::IncomingRinging: return "ringing";
    case CallState::Active: return "active";
    default: return "?";
    }
}

static void WriteState(const std::string& file) {
    Engine& e = *g_e;
    json s;
    s["dev"] = e.settings.deviceId;
    s["acc"] = "@" + e.MyAccount();
    s["port"] = e.boundPort;
    s["media"] = g_media.load();
    auto l = e.Lock();
    json conns = json::object();
    for (auto& [id, d] : e.devices) conns[id] = (bool)e.ReadyConn(id);
    s["connected"] = conns;
    json chats = json::object();
    for (auto& [key, c] : e.chats) {
        json ms = json::array();
        for (auto& m : c.messages) ms.push_back({{"text", m.text}, {"out", m.outgoing}, {"mid", m.mid}});
        chats[key] = ms;
    }
    s["chats"] = chats;
    json ts = json::array();
    for (auto& [tid, t] : e.transfers)
        ts.push_back({{"tid", tid}, {"peer", t->peer}, {"out", t->outgoing}, {"state", TStateName(t->state)}, {"path", t->localPath},
                      {"size", t->size}, {"done", t->done.load()}});
    s["transfers"] = ts;
    s["call"] = {{"state", CallStateName(e.call.state)}, {"peer", e.call.peer}, {"id", e.call.id}};
    std::string tmp = file + ".tmp";
    FILE* f = OpenFile(tmp, "wb");
    if (!f) return;
    std::string out = s.dump();
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    std::rename(tmp.c_str(), file.c_str());
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage: lunashare_peer_bot <configdir> <name> <tcpport>\n");
        return 1;
    }
    g_cfg = argv[1];
    g_name = argv[2];
    g_port = atoi(argv[3]);
    Start();
    // events: accept what asks (contacts, a call is answered only when told)
    std::atomic<bool> stop{false};
    std::thread ev([&] {
        while (!stop) {
            {
                std::lock_guard<std::mutex> l(g_eM);
                if (g_e) g_e->PopEvents();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        Engine& e = *g_e;
        if (cmd == "connect") {
            int port;
            in >> port;
            e.ConnectAddress("127.0.0.1", (uint16_t)port);
        } else if (cmd == "chat") {
            std::string key, text;
            in >> key;
            std::getline(in, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            e.SendChat(key, text);
        } else if (cmd == "send") {
            std::string key, path;
            in >> key >> path;
            e.SendPaths(key, {path});
        } else if (cmd == "call") {
            std::string key;
            in >> key;
            e.StartCall(key);
        } else if (cmd == "answer") {
            e.AcceptCall();
        } else if (cmd == "end") {
            e.EndCall();
        } else if (cmd == "media") {
            int n = 0;
            in >> n;
            for (int i = 0; i < n; i++) {
                e.SendMedia("bot", 3);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        } else if (cmd == "group") {
            std::string name, p;
            std::vector<std::string> people;
            in >> name;
            while (in >> p) people.push_back(p);
            e.CreateGroup(name, people);
        } else if (cmd == "drop") {
            std::string dev;
            in >> dev;
            DropTo(e, dev, 0);
        } else if (cmd == "netchange") {
            e.NetworkChanged(true);
        } else if (cmd == "restart") {
            std::lock_guard<std::mutex> l(g_eM);
            g_e->Stop();
            g_e.reset();
            Start();
        } else if (cmd == "state") {
            std::string file;
            in >> file;
            WriteState(file);
        } else if (cmd == "quit") {
            break;
        }
        printf("ok %s\n", cmd.c_str());
        fflush(stdout);
    }
    stop = true;
    ev.join();
    g_e->Stop();
    return 0;
}
