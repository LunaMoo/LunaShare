// A user simulator: three people (Ana, Ben, Cleo - contacts, and a group together; Ana has her phone as well, a second
// device on her account) do random things in random order -
// chat, files, calls, group calls, watching / listening / reading together, settings, blocking - while the network drops,
// stalls and the apps restart. Rounds: a minute of that, then a calm spell, then rules that must hold whatever happened
// (nothing hangs, calls agree on both ends, nothing stuck, every message there exactly once, sessions consistent).
//
//   lunashare_fuzz [seconds=300] [seed=random]     LUNASHARE_FUZZ_VERBOSE=1: every action as it's done
//                                                  LUNASHARE_FUZZ_PORT=n: ports from n (several runs at once)
//                                                  LUNASHARE_FUZZ_WORLD=lan (default) | internet | relay | mixed:
//   internet: everyone through a server run here (no LAN discovery); relay: the same, every connection relayed by it;
//   mixed: Ana and Ben on one Wi-Fi, Cleo on "mobile data" (only through the server)
//
// Everything done is logged with the seed; a broken rule prints the round's actions, so it can be replayed and cut down.
#include <algorithm>
#include <atomic>
#include <cinttypes>
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
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include "crypto.h"
#include "engine.h"
#include "hash.h"
#include "platform.h"
#include "rendezvous_server.h"
using namespace ls;
namespace fs = std::filesystem;

static std::string g_root;
static std::mt19937 g_rng;
static bool g_verbose = false;
static int g_problems = 0;
static std::deque<std::string> g_log;  // this round's actions
static std::mutex g_logM;
static int64_t g_t0 = 0;

static int R(int n) { return n <= 1 ? 0 : (int)(g_rng() % (uint32_t)n); }
static bool Chance(int pct) { return R(100) < pct; }

static void Log(const std::string& s) {
    char t[32];
    snprintf(t, sizeof(t), "%7.1f ", (SteadyMs() - g_t0) / 1000.0);
    std::lock_guard<std::mutex> l(g_logM);
    g_log.push_back(t + s);
    if (g_verbose) printf("  %s%s\n", t, s.c_str());
}

static void Problem(const std::string& what) {
    g_problems++;
    printf("\n  PROBLEM: %s\n", what.c_str());
}

// where every thread is (gdb, when there is one): what a hang or deadlock is waiting on
static void DumpThreads() {
#ifdef _WIN32
    return;  // (gdb attaching: Linux)
#endif
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "gdb -p %d -batch -ex 'set pagination off' -ex 'thread apply all bt 25' 2>/dev/null | grep -E '^Thread|^#' | grep -v 'in ?? ()' | head -400",
             (int)getpid());
    printf("  -- threads --\n");
    fflush(stdout);
    if (system(cmd) != 0) printf("  (no gdb)\n");
    fflush(stdout);
}

static void DumpLog() {
    std::lock_guard<std::mutex> l(g_logM);
    printf("  -- this round's actions (%zu) --\n", g_log.size());
    for (auto& s : g_log) printf("    %s\n", s.c_str());
    fflush(stdout);
}

static bool WaitFor(std::function<bool()> f, int ms) {
    int64_t end = SteadyMs() + ms;
    while (SteadyMs() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return f();
}

static void WriteRandomFile(const std::string& path, int64_t size, uint32_t seed) {
    MakeDirs(DirName(path));
    FILE* f = OpenFile(path, "wb");
    std::mt19937 rng(seed);
    std::vector<uint32_t> buf(1 << 14);
    int64_t left = size;
    while (left > 0) {
        for (auto& v : buf) v = rng();
        size_t n = (size_t)std::min<int64_t>(left, (int64_t)(buf.size() * 4));
        fwrite(buf.data(), 1, n, f);
        left -= (int64_t)n;
    }
    fclose(f);
}

// ---------------------------------------------------------------- the people
static const int kN = 4;  // devices: Ana, Ben, Cleo, and Ana's phone (on Ana's account)
struct Person {
    int owner = 0;  // whose account (the device that is that person first): Ana's phone is Ana's
    std::string name;
    uint16_t port;
    bool autoAccept;
    std::unique_ptr<Engine> e;
    std::string dev, acc;  // device id, account key ("@...")
    std::atomic<int> media{0};
};
static Person g_p[kN];
static std::string g_group;  // the three of them
static std::vector<std::string> g_files;  // to send
static std::string g_movie, g_song;
static std::string g_world = "lan";
static std::unique_ptr<RendezvousServer> g_srv;
static uint16_t g_srvPort = 0;
static bool OnServer(int) { return g_world != "lan"; }  // (in "mixed", Ana and Ben too: they reach Cleo there)
static bool OnLan(int who) { return g_world == "lan" || (g_world == "mixed" && (who == 0 || who == 1)); }  // (mixed:
// Ana's PC and Ben on one Wi-Fi; Cleo and Ana's phone on mobile data)

static bool StartServer() {
    g_srv = std::make_unique<RendezvousServer>();
    RendezvousServer::Options o;
    o.bind = "127.0.0.1";
    o.port = g_srvPort;
    o.dataDir = JoinPath(g_root, "server");
    std::string err;
    if (!g_srv->Start(o, &err)) {
        printf("  server: %s\n", err.c_str());
        return false;
    }
    return true;
}
static std::vector<std::string> g_pages;

static void StartEngine(Person& p) {
    std::string cfg = JoinPath(g_root, p.name + "/config");
    MakeDirs(cfg);
    json j;
    if (!LoadJsonFile(JoinPath(cfg, "settings.json"), j)) {
        Settings s;
        s.displayName = p.name;
        s.downloadDir = JoinPath(g_root, p.name + "/downloads");
        s.tcpPort = p.port;
        s.autoAcceptAll = p.autoAccept;
        int who = (int)(&p - g_p);
        s.discovery = s.broadcast = s.mdns = OnLan(who);
        s.autoConnect = true;
        if (OnServer(who)) {
            s.wanEnabled = true;
            s.wanServer = "127.0.0.1:" + std::to_string(g_srvPort);
            s.wanRoom = "sim";
        }
        j = {{"settings", ToJson(s, false)}, {"devices", json::array()}};
        SaveJsonFile(JoinPath(cfg, "settings.json"), j);
    }
    p.e = std::make_unique<Engine>();
    p.e->configDir = cfg;
    p.e->localCaps = {"voice", "video"};
    p.e->Start();
    Person* pp = &p;
    p.e->onMedia = [pp](uint64_t, int, const uint8_t*, size_t) { pp->media++; };
    p.dev = p.e->settings.deviceId;
    p.acc = "@" + p.e->MyAccount();
}

// an action must come back: a call into the engine that doesn't within 15 s is a hang (a deadlock, most likely)
static void Do(const std::string& what, std::function<void()> f) {
    Log(what);
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t([f, done] {
        f();
        *done = true;
    });
    int64_t end = SteadyMs() + 15000;
    while (!*done && SteadyMs() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!*done) {
        Problem("HANG: \"" + what + "\" didn't come back in 15 s");
        DumpLog();
        DumpThreads();
        fflush(stdout);
        std::_Exit(3);  // (the engines are stuck: nothing more to learn)
    }
    t.join();
}

// every engine's lock can be had: a watchdog that a deadlock can't hide from (also between actions)
static std::atomic<bool> g_stopDog{false};
static std::atomic<int> g_restarting{-1};
static void LockDog() {
    while (!g_stopDog) {
        for (int i = 0; i < kN && !g_stopDog; i++) {
            if (g_restarting == i) continue;
            Engine* e = g_p[i].e.get();
            auto got = std::make_shared<std::atomic<bool>>(false);
            std::thread t([e, got] {
                auto l = e->Lock();
                *got = true;
            });
            int64_t end = SteadyMs() + 12000;
            while (!*got && SteadyMs() < end && g_restarting != i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!*got && g_restarting != i) {
                Problem("DEADLOCK: " + g_p[i].name + "'s engine lock not free for 12 s");
                DumpLog();
                DumpThreads();
                std::_Exit(3);
            }
            t.join();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// ---------------------------------------------------------------- what's been sent (for "every message exactly once")
struct Sent {
    int from;
    std::string to;    // a person's device, or the group key
    std::string token;
    int round;
};
static std::vector<Sent> g_sent;
static std::set<std::string> g_unsure;  // tokens that may rightly be gone (deleted for everyone)
static std::set<std::string> g_unsureChats;  // "from>to" chats where things may rightly be gone (blocked, cleared, ttl)
static int g_round = 0, g_msgN = 0;

// (by person: "account>key"; a chat may lose messages rightly both ways once one side clears it, blocks, etc.)
static std::string ChatId(int from, const std::string& to) { return g_p[from].acc + ">" + to; }
static void UnsureBetween(int i, const std::string& key) {
    if (key == g_group) {
        for (int a = 0; a < kN; a++) g_unsureChats.insert(ChatId(a, g_group));
        return;
    }
    g_unsureChats.insert(g_p[i].acc + ">" + key);
    g_unsureChats.insert(key + ">" + g_p[i].acc);
}

// ---------------------------------------------------------------- the actions
static bool SamePerson(int a, int b) { return g_p[a].owner == g_p[b].owner; }
static int Other(int i) {  // a device of someone else
    int j;
    do j = R(kN);
    while (SamePerson(i, j));
    return j;
}
static std::string Target(int i, int* person) {  // a person (their account: every device of theirs) or the group
    if (Chance(30)) {
        *person = -1;
        return g_group;
    }
    *person = Other(i);
    return g_p[*person].acc;
}

static std::string LastMid(Engine& e, const std::string& key, bool mine) {
    auto l = e.Lock();
    auto it = e.chats.find(key);
    if (it == e.chats.end()) return "";
    auto& m = it->second.messages;
    for (int k = (int)m.size() - 1, seen = 0; k >= 0 && seen < 8; k--, seen++)
        if (m[(size_t)k].outgoing == mine && !m[(size_t)k].mid.empty()) return m[(size_t)k].mid;
    return "";
}

static std::vector<uint64_t> Tids(Engine& e) {
    std::vector<uint64_t> v;
    auto l = e.Lock();
    for (auto& [tid, t] : e.transfers) v.push_back(tid);
    return v;
}

static void Act() {
    int i = R(kN);
    Person& p = g_p[i];
    Engine& e = *p.e;
    int k = R(100);
    int who;
    if (k < 16) {  // chat
        std::string to = Target(i, &who);
        std::string tok = "m" + std::to_string(g_round) + "_" + std::to_string(++g_msgN);
        g_sent.push_back({i, to, tok, g_round});
        Do(p.name + " writes " + tok + " to " + (who < 0 ? "the group" : g_p[who].name), [&] { e.SendChat(to, tok + " hello"); });
    } else if (k < 20) {  // edit / delete / react / pin / poll on a recent message
        std::string to = Target(i, &who);
        std::string key = to;
        std::string mid = LastMid(e, key, Chance(70));
        if (mid.empty()) return;
        int what = R(6);
        if (what == 0) {
            Do(p.name + " edits a message in " + key.substr(0, 8), [&] {
                std::string text;
                {
                    auto l = e.Lock();
                    for (auto& m : e.chats[key].messages)
                        if (m.mid == mid) text = m.text;
                }
                if (!text.empty()) e.EditMessage(key, mid, text + " (edited)");
            });
        } else if (what == 1) {
            {
                auto l = e.Lock();
                for (auto& m : e.chats[key].messages)
                    if (m.mid == mid && m.text.size() > 1) g_unsure.insert(m.text.substr(0, m.text.find(' ')));
            }
            Do(p.name + " deletes a message for everyone in " + key.substr(0, 8), [&] { e.DeleteMessages(key, {mid}, true); });
        } else if (what == 2) {
            Do(p.name + " reacts in " + key.substr(0, 8), [&] { e.React(key, mid, Chance(20) ? "" : "\xF0\x9F\x91\x8D"); });
        } else if (what == 3) {
            Do(p.name + " pins in " + key.substr(0, 8), [&] { e.PinMessage(key, mid, Chance(70)); });
        } else if (what == 4) {
            Do(p.name + " starts a poll in " + key.substr(0, 8), [&] { e.SendPoll(key, "pizza?", {"yes", "no", "maybe"}, Chance(50)); });
        } else {
            Do(p.name + " votes in " + key.substr(0, 8), [&] { e.Vote(key, mid, {R(3)}); });
        }
    } else if (k < 22) {  // disappearing messages / clearing / muting / read
        std::string to = Target(i, &who);
        std::string key = to;
        int what = R(4);
        if (what == 0) {
            UnsureBetween(i, to);
            int ttl = Chance(50) ? 0 : 3600;
            Do(p.name + " sets disappearing messages " + std::to_string(ttl) + " in " + key.substr(0, 8), [&] { e.SetChatTtl(key, ttl); });
        } else if (what == 1) {
            UnsureBetween(i, to);
            Do(p.name + " clears the chat " + key.substr(0, 8), [&] { e.ClearChat(key); });
        } else if (what == 2) {
            Do(p.name + " mutes " + key.substr(0, 8), [&] { e.MuteChat(key, Chance(50) ? 0 : 60); });
        } else {
            Do(p.name + " reads " + key.substr(0, 8), [&] { e.MarkRead(key); });
        }
    } else if (k < 30) {  // files
        int what = R(10);
        if (what < 4) {
            std::string to = Target(i, &who);
            std::string f = g_files[(size_t)R((int)g_files.size())];
            Do(p.name + " sends " + BaseName(f) + " to " + (who < 0 ? "the group" : g_p[who].name), [&] {
                uint64_t b = e.SendPaths(to, {f});
                std::string ids;
                for (auto& t : e.BatchTransfers(b)) ids += " " + std::to_string(t->tid % 10000);
                Log("  (transfer" + ids + ")");
            });
        } else {
            auto tids = Tids(e);
            if (tids.empty()) return;
            uint64_t tid = tids[(size_t)R((int)tids.size())];
            static const char* ops[] = {"accepts", "rejects", "pauses", "resumes", "cancels", "retries", "removes", "pauses all", "resumes all", "clears finished"};
            int op = R(10);
            Do(p.name + " " + ops[op] + " transfer " + std::to_string(tid % 10000), [&] {
                switch (op) {
                case 0: e.Accept(tid); break;
                case 1: e.Reject(tid); break;
                case 2: e.Pause(tid); break;
                case 3: e.Resume(tid); break;
                case 4: e.Cancel(tid); break;
                case 5: e.Retry(tid); break;
                case 6: e.RemoveTransfer(tid); break;
                case 7: e.PauseAll(true); break;
                case 8: e.PauseAll(false); break;
                default: e.ClearFinishedTransfers(); break;
                }
            });
        }
    } else if (k < 44) {  // calls
        int what = R(10);
        CallState st;
        {
            auto l = e.Lock();
            st = e.call.state;
        }
        if (what < 3 && st == CallState::None) {
            std::string to = Target(i, &who);
            std::string media = Chance(50) ? "video" : "voice";
            Do(p.name + " calls " + (who < 0 ? "the group" : g_p[who].name) + " (" + media + ")", [&] { e.StartCall(to, media); });
        } else if (what < 6 && st == CallState::IncomingRinging) {
            Do(p.name + " answers", [&] { e.AcceptCall(); });
        } else if (what == 6 && st != CallState::None) {
            Do(p.name + (st == CallState::IncomingRinging ? " declines" : " hangs up"), [&] { e.EndCall(); });
        } else if (what < 9 && st == CallState::Active) {
            Do(p.name + " talks (media)", [&] {
                for (int n = 0; n < 20; n++) {
                    uint8_t pkt[40] = {1};
                    e.SendMedia(pkt, sizeof(pkt));
                }
            });
        } else if (st == CallState::Active) {
            Do(p.name + " spotlights someone", [&] { e.SetSpotlight(g_p[Other(i)].dev); });
        }
    } else if (k < 56) {  // watch / listen / read together
        int what = R(12);
        if (what == 0) {
            std::string to = Target(i, &who);
            Do(p.name + " starts watching with " + (who < 0 ? "the group" : g_p[who].name), [&] { e.StartWatch(who < 0 ? to : g_p[who].acc, g_movie); });
        } else if (what == 1) {
            uint64_t sid = 0;
            {
                auto l = e.Lock();
                if (!e.watchInvites.empty()) sid = e.watchInvites[(size_t)R((int)e.watchInvites.size())].sid;
            }
            if (sid) Do(p.name + " joins a watch invite", [&] { e.JoinWatch(sid); });
        } else if (what == 2) {
            Do(p.name + " plays / pauses / seeks the movie", [&] { e.WatchControl(Chance(50), Chance(50) ? -1 : R(100)); });
        } else if (what == 3) {
            Do(p.name + " changes speed / loop", [&] {
                if (Chance(50)) e.WatchSetSpeed(Chance(50) ? 1.0 : 1.5);
                else e.WatchSetLoop(Chance(50));
            });
        } else if (what == 4) {
            Do(p.name + " leaves watching", [&] { e.LeaveWatch(); });
        } else if (what == 5) {
            std::string to = Target(i, &who);
            Do(p.name + " starts listening with " + (who < 0 ? "the group" : g_p[who].name), [&] { e.StartListen(who < 0 ? to : g_p[who].acc); });
        } else if (what == 6) {
            uint64_t sid = 0;
            {
                auto l = e.Lock();
                if (!e.listenInvites.empty()) sid = e.listenInvites[(size_t)R((int)e.listenInvites.size())].sid;
            }
            if (sid) Do(p.name + " joins a listen invite", [&] { e.JoinListen(sid); });
        } else if (what == 7) {
            Do(p.name + " adds a song / plays / skips", [&] {
                if (Chance(50)) {
                    ListenItem it;
                    it.name = it.title = "song";
                    it.path = g_song;
                    it.size = (int64_t)FileSize(g_song);
                    it.dur = 180;
                    e.ListenAdd({it});
                } else {
                    static const char* ops[] = {"play", "pause", "seek", "next", "prev"};
                    e.ListenControl(ops[R(5)], R(100));
                }
            });
        } else if (what == 8) {
            Do(p.name + " leaves listening", [&] { e.LeaveListen(); });
        } else if (what == 9) {
            std::string to = Target(i, &who);
            Do(p.name + " starts reading with " + (who < 0 ? "the group" : g_p[who].name), [&] { e.StartRead(who < 0 ? to : g_p[who].acc, "comic.cbz", g_pages, 0); });
        } else if (what == 10) {
            uint64_t sid = 0;
            {
                auto l = e.Lock();
                if (!e.readInvites.empty()) sid = e.readInvites[(size_t)R((int)e.readInvites.size())].sid;
            }
            if (sid && Chance(70)) Do(p.name + " joins a read invite", [&] { e.JoinRead(sid); });
            else Do(p.name + " turns the page", [&] { e.ReadTurn(R((int)g_pages.size())); });
        } else {
            Do(p.name + " leaves reading", [&] { e.LeaveRead(); });
        }
    } else if (k < 64) {  // settings
        int what = R(4);
        Do(p.name + (what == 0 ? " toggles real-time calls" : what == 1 ? " changes the speed limit" : what == 2 ? " toggles auto-accept" : " toggles fps first"), [&] {
            Settings s = e.GetSettings();
            if (what == 0) s.callRealtime = !s.callRealtime;
            else if (what == 1) s.speedLimitKBs = Chance(50) ? 0 : 200 + R(2000);
            else if (what == 2) s.autoAcceptAll = !s.autoAcceptAll;
            else s.callVideoFpsFirst = !s.callVideoFpsFirst;
            e.ApplySettings(s);
        });
    } else if (k < 67) {  // blocking (undone within the round's calm spell at the latest)
        int j = Other(i);
        bool on = Chance(60);
        UnsureBetween(i, g_p[j].acc);
        UnsureBetween(i, g_group);
        Do(p.name + (on ? " blocks " : " unblocks ") + g_p[j].name, [&] { e.SetBlocked(g_p[j].acc, on); });
    } else if (k < 90) {  // the network
        int j = Other(i);
        int what = R(6);
        if (what == 0) Do(p.name + " disconnects from " + g_p[j].name, [&] { e.Disconnect(g_p[j].dev); });
        else if (what == 1) Do(p.name + "'s connection to " + g_p[j].name + " drops", [&] { e.DropConnectionTo(g_p[j].dev); });
        else if (what == 2) {
            int ms = 500 + R(4000);
            Do(p.name + "'s connection to " + g_p[j].name + " stalls " + std::to_string(ms) + " ms", [&] { e.StallConnectionTo(g_p[j].dev, ms); });
        } else if (what == 3) Do(p.name + "'s network changes", [&] { e.NetworkChanged(true); });
        else if (what == 4) Do(p.name + " connects to " + g_p[j].name, [&] { e.ConnectPeer(g_p[j].dev); });
        else {
            Do(p.name + "'s connections all drop", [&] {
                for (int a = 0; a < kN; a++)
                    if (a != i) e.DropConnectionTo(g_p[a].dev);
            });
        }
    } else {  // the app closes and opens again (rarely: it's slow) - or, through a server, the server does
        if (g_srv && Chance(15)) {
            Do("the server restarts", [&] {
                g_srv->Stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(200 + R(3000)));
                StartServer();
            });
            return;
        }
        if (!Chance(25)) return;
        Do(p.name + " closes and reopens the app", [&] {
            g_restarting = i;
            p.e->Stop();
            p.e.reset();
            StartEngine(p);
            g_restarting = -1;
        });
    }
}

// ---------------------------------------------------------------- the rules
static void DrainEvents() {
    for (auto& p : g_p)
        if (p.e) p.e->PopEvents();
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

static int IndexOfDev(const std::string& dev) {
    for (int a = 0; a < kN; a++)
        if (g_p[a].dev == dev) return a;
    return -1;
}

static void CheckRules() {
    // everyone is connected again
    for (int a = 0; a < kN; a++)
        for (int b = 0; b < kN; b++)
            if (a != b && !g_p[a].e->IsConnected(g_p[b].dev))
                Problem(g_p[a].name + " isn't connected to " + g_p[b].name + " again after the calm spell (" +
                        Engine::ConnFailText(g_p[a].e->ConnectTrouble(g_p[b].dev)) + ")");
    // the way they're connected is the world's: through the server where they can't see each other directly
    for (int a = 0; a < kN; a++)
        for (int b = 0; b < kN; b++) {
            if (a == b) continue;
            auto c = g_p[a].e->ReadyConn(g_p[b].dev);
            if (!c) continue;
            bool mustNet = g_world == "internet" || g_world == "relay" || (g_world == "mixed" && (a == 2 || b == 2));
            if (mustNet && c->transport != Transport::Internet)
                Problem(g_p[a].name + " reaches " + g_p[b].name + " directly (" + c->addr + "), though only the server connects them here");
            if (g_world == "relay" && c->transport == Transport::Internet && c->addr != "relay")
                Problem(g_p[a].name + " reaches " + g_p[b].name + " without the relay (" + c->addr + ") in the relay world");
        }
    // calls: both ends agree; nothing left ringing or reconnecting
    for (int a = 0; a < kN; a++) {
        Engine& e = *g_p[a].e;
        CallInfo c;
        {
            auto l = e.Lock();
            c = e.call;
        }
        if (c.state == CallState::None) continue;
        // (a call rings 45 s before it gives up: one placed as the calm began may still be ringing - legitimately)
        if (c.state != CallState::Active && SteadyMs() - c.started > 47000)
            Problem(g_p[a].name + "'s call is still " + CallStateName(c.state) + " after the calm spell (" +
                    std::to_string((SteadyMs() - c.started) / 1000) + " s since it began)");
        if (c.lostAt) Problem(g_p[a].name + "'s call is still reconnecting after the calm spell");
        if (c.state == CallState::Active && c.group.empty()) {
            int o = IndexOfDev(c.peer);
            if (o < 0) {
                Problem(g_p[a].name + " is in a call with an unknown device");
                continue;
            }
            CallInfo d;
            {
                auto l = g_p[o].e->Lock();
                d = g_p[o].e->call;
            }
            if (d.state != CallState::Active || d.id != c.id || d.peer != g_p[a].dev)
                Problem(g_p[a].name + " is in a call with " + g_p[o].name + ", who is " + CallStateName(d.state) +
                        (d.id == c.id ? " in it" : " in another or none"));
        }
        if (c.state == CallState::Active && !c.group.empty() && !c.hub) {
            for (auto& pr : c.peers) {
                if (pr.lostAt) Problem(g_p[a].name + "'s group call still waits for someone after the calm spell");
                int o = IndexOfDev(pr.dev);
                if (o < 0) continue;
                CallInfo d;
                {
                    auto l = g_p[o].e->Lock();
                    d = g_p[o].e->call;
                }
                bool back = false;
                for (auto& q : d.peers) back |= q.dev == g_p[a].dev;
                if (d.state != CallState::Active || d.id != c.id || !back)
                    Problem(g_p[a].name + " has " + g_p[o].name + " in the group call, but not the other way round (" + CallStateName(d.state) + ")");
            }
        }
    }
    // one person's devices: never both in the same call (the first to answer takes it, the other stops ringing)
    for (int a = 0; a < kN; a++)
        for (int b = a + 1; b < kN; b++) {
            if (!SamePerson(a, b)) continue;
            CallInfo x, y;
            {
                auto l = g_p[a].e->Lock();
                x = g_p[a].e->call;
            }
            {
                auto l = g_p[b].e->Lock();
                y = g_p[b].e->call;
            }
            if (x.id && x.id == y.id && x.state == CallState::Active && y.state == CallState::Active && x.group.empty())
                Problem(g_p[a].name + " and " + g_p[b].name + " (the same person) are both in one call");
        }
    // together: viewers and hosts agree
    for (int a = 0; a < kN; a++) {
        Engine& e = *g_p[a].e;
        uint64_t ws, ls_, rs;
        std::string wh, lh, rh;
        {
            auto l = e.Lock();
            ws = e.watch.sid, wh = e.watch.host;
            ls_ = e.listen.sid, lh = e.listen.host;
            rs = e.read.sid, rh = e.read.host;
        }
        auto hostHas = [&](const char* what, uint64_t sid, const std::string& host, auto get) {
            int o = IndexOfDev(host);
            if (o < 0) {
                Problem(g_p[a].name + " " + what + " with an unknown host");
                return;
            }
            uint64_t hs;
            std::string hh;
            {
                auto l = g_p[o].e->Lock();
                get(*g_p[o].e, hs, hh);
            }
            if (hs != sid || !hh.empty())
                Problem(g_p[a].name + " is still " + what + " with " + g_p[o].name + ", who isn't hosting it any more");
        };
        if (ws && !wh.empty()) hostHas("watching", ws, wh, [](Engine& x, uint64_t& s, std::string& h) { s = x.watch.sid, h = x.watch.host; });
        if (ls_ && !lh.empty()) hostHas("listening", ls_, lh, [](Engine& x, uint64_t& s, std::string& h) { s = x.listen.sid, h = x.listen.host; });
        if (rs && !rh.empty()) hostHas("reading", rs, rh, [](Engine& x, uint64_t& s, std::string& h) { s = x.read.sid, h = x.read.host; });
    }
    // transfers: none stuck moving (looked at twice, 3 s apart)
    struct Moving {
        int who;
        std::shared_ptr<Transfer> t;
        TState st;
        int64_t done;
    };
    std::vector<Moving> moving;
    for (int a = 0; a < kN; a++) {
        auto l = g_p[a].e->Lock();
        for (auto& [tid, t] : g_p[a].e->transfers)
            if (t->state == TState::Active || t->state == TState::Hashing) moving.push_back({a, t, t->state, t->done.load()});
    }
    if (!moving.empty()) std::this_thread::sleep_for(std::chrono::seconds(3));
    for (auto& m : moving)
        if (m.t->state == m.st && m.t->done.load() == m.done) {
            int o = IndexOfDev(m.t->peer);
            std::string there = "unknown";
            if (o >= 0) {
                auto l = g_p[o].e->Lock();
                auto it = g_p[o].e->transfers.find(m.t->tid);
                there = it == g_p[o].e->transfers.end() ? "not there" : std::string(TStateName(it->second->state)) +
                        (it->second->pausedByPeer ? " (paused by the other side)" : "") + ", " + std::to_string(it->second->done.load()) + " done";
            }
            Problem(g_p[m.who].name + "'s " + (m.t->outgoing ? "outgoing" : "incoming") + " transfer " + m.t->name + " (" +
                    std::to_string(m.t->tid % 10000) + ") is " + TStateName(m.st) + " and doesn't move (" + std::to_string(m.done) + " of " +
                    std::to_string(m.t->size) + "); at " + (o >= 0 ? g_p[o].name : "?") + ": " + there);
        }
    // only the three of them know each other (another program's devices showing up would spoil the run)
    for (int a = 0; a < kN; a++) {
        auto l = g_p[a].e->Lock();
        for (auto& [id, d] : g_p[a].e->devices)
            if (IndexOfDev(id) < 0) {
                Problem(g_p[a].name + " knows a device that isn't one of the three: " + id.substr(0, 8) + " (another run nearby?)");
                break;
            }
    }
    // messages: on every device it's meant for exactly once - each device of the person (or of the group's people), and
    // the sender's own other devices (a copy) - but those that may rightly be gone
    std::vector<std::map<std::string, int>> seen(kN);  // device -> token -> times
    for (int a = 0; a < kN; a++) {
        Engine& e = *g_p[a].e;
        auto l = e.Lock();
        for (auto& [key, chat] : e.chats)
            for (auto& m : chat.messages)
                if (m.text.size() > 2 && m.text[0] == 'm') seen[(size_t)a][m.text.substr(0, m.text.find(' '))]++;
    }
    int missing = 0, twice = 0;
    std::string firstMissing, firstTwice;
    for (auto& s : g_sent) {
        if (s.round != g_round || g_unsure.count(s.token) || g_unsureChats.count(ChatId(s.from, s.to))) continue;
        for (int b = 0; b < kN; b++) {
            if (b == s.from) continue;
            bool meant = s.to == g_group || s.to == g_p[b].acc || SamePerson(b, s.from);
            if (!meant) continue;
            int n = seen[(size_t)b][s.token];
            if (n == 0) {
                missing++;
                if (firstMissing.empty()) firstMissing = s.token + " " + g_p[s.from].name + "->" + g_p[b].name;
            }
            if (n > 1) {
                twice++;
                if (firstTwice.empty()) firstTwice = s.token + " at " + g_p[b].name;
            }
        }
    }
    if (missing) Problem(std::to_string(missing) + " messages never arrived (first: " + firstMissing + ")");
    if (twice) Problem(std::to_string(twice) + " messages arrived more than once (first: " + firstTwice + ")");
}

static int CountDir(const char* d) {
    int n = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) n++;
    return n;
}

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 300;
    uint32_t seed = argc > 2 ? (uint32_t)strtoul(argv[2], nullptr, 10) : (uint32_t)std::random_device{}();
    g_verbose = getenv("LUNASHARE_FUZZ_VERBOSE") != nullptr;
    g_rng.seed(seed);
    g_root = (fs::temp_directory_path() / ("lunashare_fuzz_" + RandomHex(4))).string();
    if (const char* w = getenv("LUNASHARE_FUZZ_WORLD")) g_world = w;
    if (g_world != "lan" && g_world != "internet" && g_world != "relay" && g_world != "mixed") {
        printf("LUNASHARE_FUZZ_WORLD: lan, internet, relay or mixed\n");
        return 2;
    }
    if (g_world == "relay") {  // (the devices can't reach each other directly: only through the server's relay)
#ifdef _WIN32
        _putenv_s("LUNASHARE_TEST_NO_PUNCH", "1");
#else
        setenv("LUNASHARE_TEST_NO_PUNCH", "1", 1);
#endif
    }
    printf("[user simulator: %d s, seed %u, %s]  (again: LUNASHARE_FUZZ_WORLD=%s lunashare_fuzz %d %u)\n", seconds, seed, g_world.c_str(),
           g_world.c_str(), seconds, seed);
    g_srvPort = (uint16_t)((getenv("LUNASHARE_FUZZ_PORT") ? atoi(getenv("LUNASHARE_FUZZ_PORT")) : 48600) + 50);
    if (g_world != "lan" && !StartServer()) return 2;
    g_t0 = SteadyMs();
    // things to share
    for (int s : {20 << 10, 300 << 10, 2 << 20, 6 << 20}) {
        std::string f = JoinPath(g_root, "files/f" + std::to_string(s) + ".bin");
        WriteRandomFile(f, s, (uint32_t)s);
        g_files.push_back(f);
    }
    g_movie = JoinPath(g_root, "files/movie.mp4");
    WriteRandomFile(g_movie, 4 << 20, 7);
    g_song = JoinPath(g_root, "files/song.mp3");
    WriteRandomFile(g_song, 600 << 10, 8);
    for (int k = 0; k < 4; k++) {
        g_pages.push_back(JoinPath(g_root, "files/page" + std::to_string(k) + ".jpg"));
        WriteRandomFile(g_pages.back(), 80 << 10, 20 + k);
    }
    // the three of them
    const char* names[] = {"Ana", "Ben", "Cleo", "Ana's phone"};
    for (int a = 0; a < kN; a++) {
        g_p[a].owner = a == 3 ? 0 : a;
        g_p[a].name = names[a];
        g_p[a].port = (uint16_t)((getenv("LUNASHARE_FUZZ_PORT") ? atoi(getenv("LUNASHARE_FUZZ_PORT")) : 48600) + a * 10);
        g_p[a].autoAccept = a != 2;  // (Cleo accepts files by hand)
        StartEngine(g_p[a]);
    }
    if (g_world == "lan") {
        for (int a = 0; a < kN; a++)
            for (int b = a + 1; b < kN; b++) g_p[a].e->ConnectAddress("127.0.0.1", g_p[b].e->boundPort);
    } else {  // (through the server: they see each other in its room and connect by themselves)
        if (g_world == "mixed") g_p[0].e->ConnectAddress("127.0.0.1", g_p[1].e->boundPort);
        for (int a = 0; a < kN; a++)
            for (int b = 0; b < kN; b++)
                if (a != b) g_p[a].e->ConnectPeer(g_p[b].dev);
    }
    if (!WaitFor([] {
            for (int a = 0; a < kN; a++)
                for (int b = 0; b < kN; b++)
                    if (a != b && !g_p[a].e->IsConnected(g_p[b].dev)) return false;
            return true;
        }, g_world == "lan" ? 15000 : 40000)) {
        printf("  couldn't connect everyone to start with\n");
        return 2;
    }
    // Ana's phone joins Ana's account
    {
        Engine& pc = *g_p[0].e;
        Engine& ph = *g_p[3].e;
        if (!pc.LinkDevice(g_p[3].dev)) {
            printf("  couldn't offer the link to Ana's phone\n");
            return 2;
        }
        bool asked = WaitFor([&] {
            for (auto& ev : ph.PopEvents())
                if (ev.type == EventType::LinkRequest && ev.peer == g_p[0].dev) return true;
            return false;
        }, 8000);
        if (asked) ph.AnswerLink(g_p[0].dev, true);
        if (!asked || !WaitFor([&] { return ph.MyAccount() == pc.MyAccount(); }, 8000)) {
            printf("  Ana's phone didn't join her account\n");
            return 2;
        }
        g_p[3].acc = g_p[0].acc;
        if (!WaitFor([] {
                for (int a = 0; a < kN; a++)
                    for (int b = 0; b < kN; b++)
                        if (a != b && !g_p[a].e->IsConnected(g_p[b].dev)) return false;
                return true;
            }, 40000)) {
            printf("  not everyone connected again after linking\n");
            return 2;
        }
    }
    g_group = g_p[0].e->CreateGroup("Friends", {g_p[1].acc, g_p[2].acc});
    WaitFor([] {
        for (int a = 1; a < kN; a++) {
            auto l = g_p[a].e->Lock();
            auto gr = g_p[a].e->GroupOf(g_group);
            if (!gr || gr->members.size() != 3) return false;
        }
        return true;
    }, 10000);
    std::thread dog(LockDog);
    int threads0 = CountDir("/proc/self/task"), fds0 = CountDir("/proc/self/fd");
    int64_t end = SteadyMs() + (int64_t)seconds * 1000;
    int actions = 0;
    while (SteadyMs() < end) {
        g_round++;
        {
            std::lock_guard<std::mutex> l(g_logM);
            g_log.clear();
        }
        g_unsure.clear();
        g_unsureChats.clear();
        int before = g_problems;
        printf("[round %d]", g_round);
        fflush(stdout);
        int64_t until = std::min(end, SteadyMs() + 60000);
        while (SteadyMs() < until) {
            Act();
            actions++;
            DrainEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(50 + R(250)));
        }
        // the calm spell: nobody blocked, no limits, everyone reachable
        Log("-- calm: unblock everyone, no speed limits, stop the stalls --");
        for (int a = 0; a < kN; a++) {
            Engine& e = *g_p[a].e;
            Do(g_p[a].name + " unblocks everyone, no limit", [&] {
                for (int b = 0; b < kN; b++)
                    if (!SamePerson(a, b)) e.SetBlocked(g_p[b].acc, false);
                Settings s = e.GetSettings();
                s.speedLimitKBs = 0;
                e.ApplySettings(s);
                e.PauseAll(false);
            });
        }
        for (int t = 0; t < 55; t++) {  // 55 s: unanswered ringing ends after 45 s; the grace times are 30 s
            DrainEvents();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        CheckRules();
        int threads = CountDir("/proc/self/task"), fds = CountDir("/proc/self/fd");
        printf(" %d actions so far, %d threads, %d open files%s\n", actions, threads, fds, g_problems > before ? "  <- problems" : "");
        if (g_problems > before) DumpLog();
        if (threads > threads0 + 200) Problem("threads keep growing: " + std::to_string(threads0) + " -> " + std::to_string(threads));
        if (fds > fds0 + 300) Problem("open files keep growing: " + std::to_string(fds0) + " -> " + std::to_string(fds));
        // a fresh start for the next round: calls and sessions ended
        for (int a = 0; a < kN; a++) {
            Engine& e = *g_p[a].e;
            Do(g_p[a].name + " ends everything", [&] {
                e.EndCall();
                e.LeaveWatch();
                e.LeaveListen();
                e.LeaveRead();
            });
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    g_stopDog = true;
    dog.join();
    for (auto& p : g_p) p.e->Stop();
    if (g_srv) g_srv->Stop();
    printf("%s: %d rounds, %d actions, %d problems (seed %u)\n", g_problems ? "PROBLEMS" : "ALL RULES HELD", g_round, actions, g_problems, seed);
    std::error_code ec;
    fs::remove_all(g_root, ec);
    return g_problems ? 1 : 0;
}
