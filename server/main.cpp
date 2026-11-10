#include <algorithm>
// lunashare-rendezvous: optional server that lets LunaShare devices find each other over the internet.
// Introductions pass through it; chat and files go directly between the devices (end-to-end encrypted), or through
// it as an encrypted relay when two devices can't reach each other directly.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "link_server.h"
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
        "      --no-relay         never relay connections (devices that can't connect directly then can't connect)\n"
        "      --relay-limit KBPS bandwidth limit per relayed connection and direction (default: unlimited)\n"
        "  -d, --data DIR         keep offline messages in DIR (survive restarts; default: memory only)\n"
        "      --no-mailbox       don't keep messages for devices that are offline\n"
        "      --mailbox-days N   keep offline messages at most N days (default 10)\n"
        "      --mailbox-mb N     limit per receiving device in MB (default 10)\n"
        "      --mailbox-total-mb N   limit for all offline messages together (default 1024)\n"
        "  -q, --quiet            no log output\n\n"
        "Links for anyone (people without LunaShare download a file in their browser; it comes from the sender's\n"
        "device through this server, end-to-end encrypted):\n"
        "      --link-port N      serve the link pages on TCP port N (devices also connect to TCP on --port)\n"
        "      --cert FILE        certificate (PEM, full chain) for HTTPS; reloaded when it changes\n"
        "      --key FILE         its private key (PEM)\n"
        "      --link-url URL     the public address of the pages when a reverse proxy serves them with HTTPS\n"
        "                         (then this server speaks plain HTTP and trusts X-Forwarded-For from 127.0.0.1)\n\n"
        "      --hash-room NAME PASSWORD   print the hash of a room (for the --rooms file) and exit\n\n"
        "People who joined with an invite code are remembered (in the -d directory) and stay limited to the people who\n"
        "invited them, even if they later enter the server's address by hand:\n"
        "      --list-invited [--json]     list them and exit (needs -d)\n"
        "      --release KEY               treat that device (and its account) like everyone else (needs -d)\n"
        "      --restrict KEY              limit it again\n"
        "      --forget KEY                forget it (it is remembered again if it uses an invite)\n"
        "  KEY is a device key or its first 8+ characters, as --list-invited shows. A running server applies\n"
        "  the change within a few seconds.\n");
}

static int ListInvited(const std::string& dir, bool asJson) {
    std::ifstream f(dir + "/invited.json");
    nlohmann::json j = nlohmann::json::object();
    if (f) j = nlohmann::json::parse(f, nullptr, false);
    nlohmann::json out = nlohmann::json::array();
    nlohmann::json devs = j.is_object() ? j.value("devices", nlohmann::json::object()) : nlohmann::json::object();
    if (devs.is_object())
        for (auto& [pub, v] : devs.items()) {
            if (!v.is_object()) continue;
            nlohmann::json e = v;
            e["key"] = pub;
            out.push_back(e);
        }
    if (asJson) {
        printf("%s\n", out.dump().c_str());
        return 0;
    }
    if (out.empty()) printf("no one has joined with an invite\n");
    for (auto& e : out) {
        auto day = [](int64_t ms) {
            char b[32] = "-";
            time_t t = (time_t)(ms / 1000);
            if (ms > 0) strftime(b, sizeof(b), "%Y-%m-%d", localtime(&t));
            return std::string(b);
        };
        std::string by = e.value("by", "").empty() ? "?" : e.value("by", "");
        if (!e.value("byKey", "").empty()) by += " (" + e.value("byKey", "").substr(0, 8) + ")";
        std::string room = e.value("room", "").empty() ? "-" : e.value("room", "");
        printf("%s  %-8s  %-22s  invited by %-30s  room %-16s  first %s  last %s\n", e.value("key", "").substr(0, 16).c_str(),
               e.value("released", false) ? "released" : "limited", e.value("name", "").c_str(), by.c_str(), room.c_str(),
               day(e.value("first", (int64_t)0)).c_str(), day(e.value("last", (int64_t)0)).c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    ls::RendezvousServer::Options o;
    bool quiet = false, listInv = false, asJson = false;
    uint16_t linkPort = 0;
    std::string cert, key, linkUrl;
    std::vector<std::string> adminCmds;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "-p" || a == "--port") o.port = (uint16_t)atoi(next().c_str());
        else if (a == "-b" || a == "--bind") o.bind = next();
        else if (a == "--max-devices") o.maxDevices = (size_t)atol(next().c_str());
        else if (a == "--no-relay") o.relay = false;
        else if (a == "--relay-limit") o.relayKbps = atoi(next().c_str());
        else if (a == "-q" || a == "--quiet") quiet = true;
        else if (a == "-d" || a == "--data") o.dataDir = next();
        else if (a == "--no-mailbox") o.mailbox = false;
        else if (a == "--mailbox-days") o.mailboxDays = std::max(1, atoi(next().c_str()));
        else if (a == "--mailbox-mb") o.mailboxMB = std::max(1, atoi(next().c_str()));
        else if (a == "--mailbox-total-mb") o.mailboxTotalMB = std::max(1, atoi(next().c_str()));
        else if (a == "--list-invited") listInv = true;
        else if (a == "--link-port") linkPort = (uint16_t)atoi(next().c_str());
        else if (a == "--cert") cert = next();
        else if (a == "--key") key = next();
        else if (a == "--link-url") linkUrl = next();
        else if (a == "--json") asJson = true;
        else if (a == "--release" || a == "--restrict" || a == "--forget") {
            std::string k = next();
            bool ok = k.size() >= 8 && k.size() <= 64;
            for (char c : k) ok &= isxdigit((unsigned char)c) != 0;
            if (!ok) { fprintf(stderr, "%s needs a device key (8 or more hex characters)\n", a.c_str()); return 1; }
            adminCmds.push_back(a.substr(2) + " " + k);
        }
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
        } else if (a == "--version") {
            printf("lunashare-rendezvous %s\n", LUNASHARE_VERSION);
            return 0;
        } else if (a == "-h" || a == "--help") {
            Usage();
            return 0;
        } else {
            fprintf(stderr, "unknown option %s\n", a.c_str());
            Usage();
            return 1;
        }
    }
    if (listInv || !adminCmds.empty()) {
        if (o.dataDir.empty()) { fprintf(stderr, "give the server's data directory with -d\n"); return 1; }
        if (!adminCmds.empty()) {
            std::string path = o.dataDir + "/invited-cmd";
            {
                std::ofstream f(path, std::ios::app);
                for (auto& c : adminCmds) f << c << "\n";
                if (!f) { fprintf(stderr, "cannot write to %s\n", o.dataDir.c_str()); return 1; }
            }
#ifndef _WIN32
            // the running server reads this file as its own user (e.g. nobody): this command often runs as root
            // (OPNsense's configd, sudo) with a umask that would leave it unreadable - hand it to the folder's owner
            struct stat ds;
            if (stat(o.dataDir.c_str(), &ds) == 0 && chown(path.c_str(), ds.st_uid, ds.st_gid) != 0) {}
            chmod(path.c_str(), 0644);
#endif
            printf("done - a running server applies it within a few seconds\n");
        }
        return listInv ? ListInvited(o.dataDir, asJson) : 0;
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
    if (linkPort) {
        if (cert.empty() != key.empty()) { fprintf(stderr, "error: give both --cert and --key\n"); return 1; }
        if (!cert.empty() && !ls::LinkServer::TlsSupported()) {
            fprintf(stderr, "error: this build has no HTTPS (OpenSSL); serve the pages behind a reverse proxy with --link-url\n");
            return 1;
        }
        while (!linkUrl.empty() && linkUrl.back() == '/') linkUrl.pop_back();
        o.linkPort = linkPort;
        o.linkTls = !cert.empty();
        o.linkUrl = linkUrl;
    }
    ls::RendezvousServer srv;
    ls::LinkServer links;
    std::string err;
    if (!srv.Start(o, &err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (linkPort) {
        ls::LinkServer::Options lo;
        lo.bind = o.bind;
        lo.httpPort = linkPort;
        lo.tunnelPort = o.port;
        lo.certFile = cert;
        lo.keyFile = key;
        lo.trustProxy = !linkUrl.empty() && cert.empty();
        lo.allowed = [&srv](const std::string& pub) { return srv.LinkAllowed(pub); };
        lo.log = o.log;
        if (!links.Start(lo, &err)) {
            fprintf(stderr, "error: %s\n", err.c_str());
            srv.Stop();
            return 1;
        }
        if (o.log)
            o.log("links for anyone: " + std::string(cert.empty() ? "http" : "https") + " on tcp " + o.bind + ":" + std::to_string(linkPort) +
                  (linkUrl.empty() ? "" : " (public address " + linkUrl + ")") + ", devices on tcp " + std::to_string(o.port) +
                  (cert.empty() && linkUrl.empty() ? " - no HTTPS: browsers only open such links on this computer" : ""));
    }
    if (o.log)
        o.log("listening on udp " + o.bind + ":" + std::to_string(o.port) +
              (o.allowedRooms.empty() ? " (any room)" : " (" + std::to_string(o.allowedRooms.size()) + " allowed rooms)") +
              (o.mailbox ? ", offline messages " + std::to_string(o.mailboxDays) + " days / " + std::to_string(o.mailboxMB) + " MB per device" +
                               (o.dataDir.empty() ? " (in memory)" : " in " + o.dataDir)
                         : ", no offline messages"));
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    links.Stop();
    srv.Stop();
    if (o.log) o.log("stopped");
    return 0;
}
