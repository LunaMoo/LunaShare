// lunashare-rendezvous: optional server that lets LunaShare devices find each other over the internet.
// Only introductions pass through it; chat and files go directly between the devices (end-to-end encrypted).
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

#include "rendezvous_server.h"
#include "wan.h"

static volatile std::sig_atomic_t g_stop = 0;
static void OnSignal(int) { g_stop = 1; }

static void Usage() {
    printf(
        "lunashare-rendezvous - LunaShare internet rendezvous server (UDP)\n\n"
        "  -p, --port N           UDP port (default 47830)\n"
        "  -b, --bind ADDR        IPv4 address to listen on (default 0.0.0.0)\n"
        "  -r, --rooms FILE       only allow the room hashes listed in FILE (one per line, # comments)\n"
        "      --max-devices N    limit registered devices (default 5000)\n"
        "  -q, --quiet            no log output\n"
        "      --hash-room NAME PASSWORD   print the hash of a room (for the --rooms file) and exit\n");
}

int main(int argc, char** argv) {
    ls::RendezvousServer::Options o;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "-p" || a == "--port") o.port = (uint16_t)atoi(next().c_str());
        else if (a == "-b" || a == "--bind") o.bind = next();
        else if (a == "--max-devices") o.maxDevices = (size_t)atol(next().c_str());
        else if (a == "-q" || a == "--quiet") quiet = true;
        else if (a == "--hash-room" && i + 2 < argc) {
            printf("%s\n", ls::RoomHash(argv[i + 1], argv[i + 2]).c_str());
            return 0;
        } else if (a == "-r" || a == "--rooms") {
            std::ifstream f(next());
            if (!f) { fprintf(stderr, "cannot read rooms file\n"); return 1; }
            std::string line;
            while (std::getline(f, line)) {
                size_t h = line.find('#');
                if (h != std::string::npos) line.resize(h);
                while (!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
                while (!line.empty() && isspace((unsigned char)line.front())) line.erase(line.begin());
                if (line.size() == 64) o.allowedRooms.insert(line);
            }
        } else if (a == "-h" || a == "--help") {
            Usage();
            return 0;
        } else {
            fprintf(stderr, "unknown option %s\n", a.c_str());
            Usage();
            return 1;
        }
    }
    if (!quiet)
        o.log = [](const std::string& m) {
            time_t t = time(nullptr);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
            printf("%s %s\n", ts, m.c_str());
            fflush(stdout);
        };
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    ls::RendezvousServer srv;
    std::string err;
    if (!srv.Start(o, &err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (o.log)
        o.log("listening on udp " + o.bind + ":" + std::to_string(o.port) +
              (o.allowedRooms.empty() ? " (any room)" : " (" + std::to_string(o.allowedRooms.size()) + " allowed rooms)"));
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    srv.Stop();
    if (o.log) o.log("stopped");
    return 0;
}
