// Versions together: Ana and Ben run this version; Dan runs another one - a peer bot built from an older release
// (tools/build_old_peer.sh), as people update at different times. Random chat, files, group messages, calls, dropped
// connections, network changes, restarts and slow links between them, then a calm spell and the rules: everyone
// connected again, every message there exactly once, every file there whole, no call left hanging.
//
//   lunashare_compat <peer bot> [seconds=240] [seed=random]      LUNASHARE_COMPAT_VERBOSE=1: every action as it's done
//
// (Linux / macOS: the bot runs as a process of its own, its commands go to its stdin.)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include "engine.h"
#include "hash.h"
#include "platform.h"
using namespace ls;
namespace fs = std::filesystem;

static std::mt19937 g_rng;
static bool g_verbose = false;
static int g_problems = 0;
static std::deque<std::string> g_log;
static int64_t g_t0 = 0;
static std::string g_root;

static int R(int n) { return n <= 1 ? 0 : (int)(g_rng() % (uint32_t)n); }
static bool Chance(int pct) { return R(100) < pct; }
static void Log(const std::string& s) {
    char t[32];
    snprintf(t, sizeof(t), "%7.1f ", (SteadyMs() - g_t0) / 1000.0);
    g_log.push_back(t + s);
    if (g_verbose) printf("  %s%s\n", t, s.c_str());
}
static void Problem(const std::string& s) {
    g_problems++;
    printf("\n  PROBLEM: %s\n", s.c_str());
}
static void DumpLog() {
    printf("  -- this round's actions (%zu) --\n", g_log.size());
    for (auto& s : g_log) printf("    %s\n", s.c_str());
}
static bool WaitFor(std::function<bool()> f, int ms) {
    int64_t end = SteadyMs() + ms;
    while (SteadyMs() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return f();
}
static void WriteRandomFile(const std::string& path, int64_t size, uint32_t seed) {
    MakeDirs(DirName(path));
    FILE* f = OpenFile(path, "wb");
    std::mt19937 rng(seed);
    std::vector<uint32_t> buf(1 << 14);
    for (int64_t left = size; left > 0;) {
        for (auto& v : buf) v = rng();
        size_t n = (size_t)(left < (int64_t)(buf.size() * 4) ? left : (int64_t)(buf.size() * 4));
        fwrite(buf.data(), 1, n, f);
        left -= (int64_t)n;
    }
    fclose(f);
}

// ---------------------------------------------------------------- the bot (Dan)
struct Bot {
    std::string exe, cfg, stateFile;
    uint16_t port;
    FILE* in = nullptr;
    json st;  // its last state
    std::string dev, acc;
    bool Start() {
        std::string cmd = "'" + exe + "' '" + cfg + "' Dan " + std::to_string(port) + " > '" + g_root + "/dan.log' 2>&1";
        in = popen(cmd.c_str(), "w");
        if (!in) return false;
        if (!State()) return false;
        dev = st.value("dev", "");
        acc = st.value("acc", "");
        return !dev.empty();
    }
    void Cmd(const std::string& c) {
        Log("  (Dan: " + c + ")");
        fprintf(in, "%s\n", c.c_str());
        fflush(in);
    }
    bool State() {
        std::error_code ec;
        fs::remove(stateFile, ec);
        fprintf(in, "state %s\n", stateFile.c_str());
        fflush(in);
        if (!WaitFor([&] { return fs::exists(stateFile); }, 15000)) return false;
        json j;
        if (!LoadJsonFile(stateFile, j)) return false;
        st = j;
        return true;
    }
    void Stop() {
        if (!in) return;
        fprintf(in, "quit\n");
        fflush(in);
        pclose(in);
        in = nullptr;
    }
};
static Bot g_dan;

// ---------------------------------------------------------------- this version's two
struct Person {
    std::string name;
    uint16_t port;
    std::unique_ptr<Engine> e;
    std::string dev, acc;
    std::atomic<int> media{0};
};
static Person g_p[2];
static std::string g_group;
static std::vector<std::string> g_files;

static void StartEngine(Person& p) {
    std::string cfg = JoinPath(g_root, p.name + "/config");
    MakeDirs(cfg);
    json j;
    if (!LoadJsonFile(JoinPath(cfg, "settings.json"), j)) {
        Settings s;
        s.displayName = p.name;
        s.downloadDir = JoinPath(g_root, p.name + "/downloads");
        s.tcpPort = p.port;
        s.autoAcceptAll = true;
        s.discovery = s.broadcast = s.mdns = true;
        s.autoConnect = true;
        j = {{"settings", ToJson(s, false)}, {"devices", json::array()}};
        SaveJsonFile(JoinPath(cfg, "settings.json"), j);
    }
    p.e = std::make_unique<Engine>();
    p.e->configDir = cfg;
    p.e->localCaps = {"voice"};
    p.e->Start();
    Person* pp = &p;
    p.e->onMedia = [pp](uint64_t, int, const uint8_t*, size_t) { pp->media++; };
    p.dev = p.e->settings.deviceId;
    p.acc = "@" + p.e->MyAccount();
}

// who: 0 Ana, 1 Ben, 2 Dan
static const char* Name(int who) { return who == 0 ? "Ana" : who == 1 ? "Ben" : "Dan"; }
static std::string Dev(int who) { return who < 2 ? g_p[who].dev : g_dan.dev; }
static std::string Acc(int who) { return who < 2 ? g_p[who].acc : g_dan.acc; }

static bool Connected(int a, int b) {
    if (a < 2) return g_p[a].e->IsConnected(Dev(b));
    auto& c = g_dan.st["connected"];
    return c.contains(Dev(b)) && c[Dev(b)].get<bool>();
}

static std::string CallStateOf(int who) {
    if (who == 2) return g_dan.st["call"].value("state", "?");
    auto l = g_p[who].e->Lock();
    switch (g_p[who].e->call.state) {
    case CallState::None: return "none";
    case CallState::OutgoingRinging: return "calling";
    case CallState::IncomingRinging: return "ringing";
    case CallState::Active: return "active";
    default: return "?";
    }
}

// a device's messages: every text in every chat of it
static std::vector<std::string> Texts(int who) {
    std::vector<std::string> r;
    if (who == 2) {
        for (auto& [k, ms] : g_dan.st["chats"].items())
            for (auto& m : ms) r.push_back(m.value("text", ""));
        return r;
    }
    auto l = g_p[who].e->Lock();
    for (auto& [k, c] : g_p[who].e->chats)
        for (auto& m : c.messages) r.push_back(m.text);
    return r;
}

// ---------------------------------------------------------------- what was sent
struct SentMsg {
    int from;
    int to;  // a person, or -1: the group
    std::string token;
};
static std::vector<SentMsg> g_msgs;
struct SentFile {
    int from, to;
    std::string path, sha;
    int64_t size;
};
static std::vector<SentFile> g_sentFiles;
static std::map<std::string, std::string> g_sha;  // path -> sha
static int g_n = 0, g_round = 0;

static void Act() {
    int a = R(3), b;
    do b = R(3);
    while (b == a);
    int k = R(100);
    if (k < 30) {  // a message: to the person, or the group
        bool group = Chance(30);
        std::string tok = "c" + std::to_string(g_round) + "_" + std::to_string(++g_n);
        std::string key = group ? g_group : Acc(b);
        Log(std::string(Name(a)) + " writes " + tok + " to " + (group ? "the group" : Name(b)));
        g_msgs.push_back({a, group ? -1 : b, tok});
        if (a == 2) g_dan.Cmd("chat " + key + " " + tok + " hi");
        else g_p[a].e->SendChat(key, tok + " hi");
    } else if (k < 45) {  // a file
        std::string f = g_files[(size_t)R((int)g_files.size())];
        Log(std::string(Name(a)) + " sends " + f.substr(f.rfind('/') + 1) + " to " + Name(b));
        g_sentFiles.push_back({a, b, f, g_sha[f], (int64_t)fs::file_size(f)});
        if (a == 2) g_dan.Cmd("send " + Acc(b) + " " + f);
        else g_p[a].e->SendPaths(Acc(b), {f});
    } else if (k < 60) {  // a call between Dan and one of the others: rings, answered, voice both ways, ended
        int o = a == 2 ? b : a;
        bool danCalls = Chance(50);
        Log(danCalls ? std::string("Dan calls ") + Name(o) : std::string(Name(o)) + " calls Dan");
        if (danCalls) g_dan.Cmd("call " + g_p[o].acc);
        else g_p[o].e->StartCall(g_dan.acc);
        bool rings = WaitFor([&] {
            if (danCalls) return CallStateOf(o) == "ringing";
            g_dan.State();
            return CallStateOf(2) == "ringing";
        }, 8000);
        if (!rings) {
            // (one of them may be busy, cut off or just restarted: not a rule broken by itself)
            Log("  it doesn't ring (" + CallStateOf(o) + " / " + CallStateOf(2) + ")");
            if (danCalls) g_dan.Cmd("end");
            else g_p[o].e->EndCall();
            return;
        }
        if (danCalls) g_p[o].e->AcceptCall();
        else g_dan.Cmd("answer");
        bool on = WaitFor([&] {
            g_dan.State();
            return CallStateOf(o) == "active" && CallStateOf(2) == "active";
        }, 8000);
        if (!on) {
            Problem(std::string("a call between ") + Name(o) + " and Dan wasn't on both sides after it was answered (" + CallStateOf(o) +
                    " / " + CallStateOf(2) + ")");
            DumpLog();
        } else {
            int before = g_p[o].media.load();
            int danBefore = g_dan.st.value("media", 0);
            g_dan.Cmd("media 25");
            for (int i = 0; i < 25; i++) {
                g_p[o].e->SendMedia("hey", 3);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            WaitFor([&] {
                g_dan.State();
                return g_p[o].media.load() - before >= 20 && g_dan.st.value("media", 0) - danBefore >= 20;
            }, 5000);
            int got = g_p[o].media.load() - before, danGot = g_dan.st.value("media", 0) - danBefore;
            Log("  voice: " + std::to_string(got) + " to " + Name(o) + ", " + std::to_string(danGot) + " to Dan (of 25)");
            if (got < 15 || danGot < 15) {
                Problem(std::string("in a call between ") + Name(o) + " and Dan the voice doesn't get through (" + std::to_string(got) +
                        " to " + Name(o) + ", " + std::to_string(danGot) + " to Dan, of 25)");
                DumpLog();
            }
        }
        if (Chance(50)) g_dan.Cmd("end");
        else g_p[o].e->EndCall();
    } else if (k < 72) {  // a connection drops
        Log(std::string(Name(a)) + "'s connection to " + Name(b) + " drops");
        if (a == 2) g_dan.Cmd("drop " + Dev(b));
        else g_p[a].e->DropConnectionTo(Dev(b));
    } else if (k < 80) {
        Log(std::string(Name(a)) + "'s network changes");
        if (a == 2) g_dan.Cmd("netchange");
        else g_p[a].e->NetworkChanged(true);
    } else if (k < 88) {  // a slow link (this version's side: it holds back what it sends)
        int o = a == 2 ? b : a, other = a == 2 ? 2 : b;
        int kbs = 20 + R(300), ms = 3000 + R(15000);
        Log(std::string(Name(o)) + "'s link to " + Name(other) + " is slow for " + std::to_string(ms / 1000) + " s (" + std::to_string(kbs) +
            " KB/s)");
        g_p[o].e->SlowConnectionTo(Dev(other), kbs, ms, R(20));
    } else if (k < 94) {
        Log(std::string(Name(a)) + " closes and reopens the app");
        if (a == 2) g_dan.Cmd("restart");
        else {
            g_p[a].e->Stop();
            g_p[a].e.reset();
            StartEngine(g_p[a]);
        }
    }
    // (else: a pause)
}

static void CheckRules() {
    g_dan.State();
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++)
            if (a != b && !Connected(a, b)) Problem(std::string(Name(a)) + " isn't connected to " + Name(b) + " again after the calm spell");
    for (int a = 0; a < 3; a++)
        if (CallStateOf(a) != "none") Problem(std::string(Name(a)) + "'s call is still " + CallStateOf(a) + " after the calm spell");
    // every message exactly once where it should be
    std::vector<std::vector<std::string>> texts;
    for (int a = 0; a < 3; a++) texts.push_back(Texts(a));
    for (auto& m : g_msgs) {
        for (int w = 0; w < 3; w++) {
            bool should = w == m.from || m.to == -1 || m.to == w;
            int n = 0;
            for (auto& t : texts[(size_t)w])
                if (t.rfind(m.token + " ", 0) == 0) n++;
            if (should && n != 1)
                Problem("message " + m.token + " (" + Name(m.from) + " to " + (m.to < 0 ? "the group" : Name(m.to)) + ") is there " +
                        std::to_string(n) + " times on " + Name(w) + "'s device");
            if (!should && n) Problem("message " + m.token + " got to " + Name(w) + ", who wasn't meant to have it");
        }
    }
    // every file whole at the other side (as many times as it was sent)
    std::map<std::pair<int, int64_t>, int> want, have;  // (receiver, size)
    for (auto& f : g_sentFiles) want[{f.to, f.size}]++;
    for (int w = 0; w < 3; w++) {
        auto count = [&](const std::string& state, bool out, int64_t size, const std::string& path) {
            if (out || state != TStateName(TState::Completed)) return;
            std::string sha;
            for (auto& [p, s] : g_sha)
                if ((int64_t)fs::file_size(p) == size) sha = s;
            if (Sha256File(path) != sha) {
                Problem(std::string("a file at ") + Name(w) + " isn't whole (" + path + ")");
                return;
            }
            have[{w, size}]++;
        };
        if (w == 2) {
            for (auto& t : g_dan.st["transfers"]) count(t.value("state", ""), t.value("out", false), t.value("size", (int64_t)0), t.value("path", ""));
        } else {
            auto l = g_p[w].e->Lock();
            for (auto& [tid, t] : g_p[w].e->transfers) count(TStateName(t->state), t->outgoing, t->size, t->localPath);
        }
    }
    if (getenv("LUNASHARE_COMPAT_VERBOSE")) {
        auto l = g_p[0].e->Lock();
        for (auto& [tid, t] : g_p[0].e->transfers)
            printf("    Ana: %s %s %lld/%lld %s\n", t->outgoing ? "out" : "in", TStateName(t->state), (long long)t->done.load(), (long long)t->size,
                   t->localPath.c_str());
    }
    for (auto& [k, n] : want)
        if (have[k] < n)
            Problem(std::to_string(n - have[k]) + " of " + std::to_string(n) + " files of " + std::to_string(k.second) + " bytes didn't get to " +
                    Name(k.first));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: lunashare_compat <peer bot> [seconds=240] [seed]\n");
        return 2;
    }
    int seconds = argc > 2 ? atoi(argv[2]) : 240;
    uint32_t seed = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 10) : (uint32_t)std::random_device{}();
    g_verbose = getenv("LUNASHARE_COMPAT_VERBOSE") != nullptr;
    g_rng.seed(seed);
    g_root = (fs::temp_directory_path() / ("lunashare_compat_" + RandomHex(4))).string();
    printf("[versions together: %d s, seed %u, Dan runs %s]\n", seconds, seed, argv[1]);
    g_t0 = SteadyMs();
    for (int s : {30 << 10, 400 << 10, 3 << 20}) {
        std::string f = JoinPath(g_root, "files/f" + std::to_string(s) + ".bin");
        WriteRandomFile(f, s, (uint32_t)s);
        g_files.push_back(f);
        g_sha[f] = Sha256File(f);
    }
    g_p[0].name = "Ana";
    g_p[0].port = 48700;
    g_p[1].name = "Ben";
    g_p[1].port = 48710;
    for (auto& p : g_p) StartEngine(p);
    g_dan.exe = fs::absolute(argv[1]).string();
    g_dan.cfg = JoinPath(g_root, "Dan/config");
    g_dan.stateFile = JoinPath(g_root, "dan-state.json");
    g_dan.port = 48720;
    if (!g_dan.Start()) {
        printf("  the bot didn't start (see %s/dan.log)\n", g_root.c_str());
        return 2;
    }
    g_p[0].e->ConnectAddress("127.0.0.1", g_p[1].e->boundPort);
    g_p[0].e->ConnectAddress("127.0.0.1", g_dan.port);
    g_p[1].e->ConnectAddress("127.0.0.1", g_dan.port);
    if (!WaitFor([] {
            g_dan.State();
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++)
                    if (a != b && !Connected(a, b)) return false;
            return true;
        }, 20000)) {
        printf("  couldn't connect everyone to start with\n");
        return 2;
    }
    g_group = g_p[0].e->CreateGroup("compat", {g_p[1].acc, g_dan.acc});
    std::this_thread::sleep_for(std::chrono::seconds(3));
    int64_t end = SteadyMs() + (int64_t)seconds * 1000;
    while (SteadyMs() < end) {
        g_round++;
        g_log.clear();
        g_msgs.clear();
        g_sentFiles.clear();
        printf("  round %d\n", g_round);
        fflush(stdout);
        int64_t chaos = SteadyMs() + 50000;
        while (SteadyMs() < chaos) {
            Act();
            std::this_thread::sleep_for(std::chrono::milliseconds(150 + R(700)));
        }
        Log("-- calm --");
        g_dan.Cmd("end");
        for (auto& p : g_p) p.e->EndCall();
        // the calm spell: long enough for reconnects, and for the files that are still going
        std::this_thread::sleep_for(std::chrono::seconds(50));
        int before = g_problems;
        CheckRules();
        if (g_problems > before) DumpLog();
        fflush(stdout);
    }
    g_dan.Stop();
    for (auto& p : g_p) p.e->Stop();
    if (g_problems) printf("\nPROBLEMS: %d (seed %u)\n", g_problems, seed);
    else printf("\nALL RULES HELD: %d rounds (seed %u)\n", g_round, seed);
    std::error_code ec;
    if (!g_problems) fs::remove_all(g_root, ec);
    return g_problems ? 1 : 0;
}
