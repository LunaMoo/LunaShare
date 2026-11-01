// Headless end-to-end tests: two engines in one process talking over loopback TCP.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <atomic>
#include <thread>

#include "crypto.h"
#include "engine.h"
#include "mdns.h"
#include "rendezvous_server.h"
#include "wan.h"
#include "hash.h"
#include "platform.h"

using namespace ls;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            printf("       " __VA_ARGS__);                  \
            printf("\n");                                   \
            g_fail++;                                       \
        }                                                   \
    } while (0)

static bool WaitFor(std::function<bool()> f, int ms) {
    int64_t end = SteadyMs() + ms;
    while (SteadyMs() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

static std::string Root() {
    static std::string r = (fs::temp_directory_path() / ("lunashare_test_" + RandomHex(4))).string();
    return r;
}

static void WriteRandomFile(const std::string& path, int64_t size, uint32_t seed) {
    MakeDirs(DirName(path));
    FILE* f = fopen(path.c_str(), "wb");
    std::mt19937 rng(seed);
    std::vector<uint32_t> buf(1 << 16);
    int64_t left = size;
    while (left > 0) {
        for (auto& v : buf) v = rng();
        size_t n = (size_t)std::min<int64_t>(left, (int64_t)(buf.size() * 4));
        fwrite(buf.data(), 1, n, f);
        left -= (int64_t)n;
    }
    fclose(f);
}

static std::unique_ptr<Engine> MakeEngine(const std::string& name, uint16_t port, bool autoAcceptAll, bool discovery,
                                          bool broadcast = true, bool mdnsOn = true, const std::string& wanServer = "",
                                          const std::string& wanRoom = "", std::vector<std::string> caps = {}) {
    std::string cfg = JoinPath(Root(), name + "/config");
    MakeDirs(cfg);
    json j;
    if (!LoadJsonFile(JoinPath(cfg, "settings.json"), j)) {
        Settings s;
        s.displayName = name;
        s.downloadDir = JoinPath(Root(), name + "/downloads");
        s.tcpPort = port;
        s.autoAcceptAll = autoAcceptAll;
        s.discovery = discovery;
        s.autoConnect = discovery;
        s.broadcast = broadcast;
        s.mdns = mdnsOn;
        s.wanEnabled = !wanServer.empty();
        s.wanServer = wanServer;
        s.wanRoom = wanRoom;
        s.wanPassword = "secret " + wanRoom;
        if (!wanServer.empty()) s.autoConnect = true;
        j = {{"settings", ToJson(s, false)}, {"devices", json::array()}};
        SaveJsonFile(JoinPath(cfg, "settings.json"), j);
    }
    auto e = std::make_unique<Engine>();
    e->configDir = cfg;
    e->localCaps = caps;
    e->Start();
    return e;
}

static std::shared_ptr<Transfer> GetT(Engine& e, uint64_t tid) {
    auto lk = e.Lock();
    auto it = e.transfers.find(tid);
    return it == e.transfers.end() ? nullptr : it->second;
}

static TState StateOf(Engine& e, uint64_t tid) {
    auto lk = e.Lock();
    auto t = GetT(e, tid);
    return t ? t->state : (TState)-1;
}

static uint64_t FirstTid(Engine& e, uint64_t batch) {
    auto v = e.BatchTransfers(batch);
    return v.empty() ? 0 : v[0]->tid;
}

static void TestHash() {
    printf("[hash]\n");
    CHECK(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    CHECK(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(Sha256Hex(m, strlen(m)) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "448-bit");
    std::string mil(1000000, 'a');
    Sha256 s;
    for (size_t i = 0; i < mil.size(); i += 777) s.Update(mil.data() + i, std::min<size_t>(777, mil.size() - i));
    CHECK(s.FinalHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "million a, chunked");
    CHECK(Crc32("123456789", 9) == 0xCBF43926u, "crc32");
    CHECK(SanitizeRelativePath("../../etc/passwd") == "etc/passwd", "sanitize ..");
    CHECK(SanitizeRelativePath("C:\\Windows\\x?.txt") == "C_/Windows/x_.txt", "sanitize drive: %s", SanitizeRelativePath("C:\\Windows\\x?.txt").c_str());
}

static void TestCrypto() {
    printf("[crypto]\n");
    NetInit();
    SocketHandle sp[2];
    CHECK(LocalSocketPair(sp), "socketpair");
    SecureChannel a, b;
    bool okA = false, okB = false;
    std::thread t([&] { okB = b.Handshake(sp[1], false, 3000, nullptr); });
    okA = a.Handshake(sp[0], true, 3000, nullptr);
    t.join();
    CHECK(okA && okB, "handshake");
    Identity ia, ib;
    ia.Generate();
    ib.Generate();
    std::string sigA = a.Sign(ia);
    CHECK(b.VerifyPeer(ia.PubHex(), sigA), "b verifies a");
    CHECK(!b.VerifyPeer(ib.PubHex(), sigA), "wrong key rejected");
    CHECK(!a.VerifyPeer(ia.PubHex(), sigA), "reflected signature rejected");
    std::string big(300000, 'x');
    for (size_t i = 0; i < big.size(); i++) big[i] = (char)(i * 31);
    std::thread w([&] { a.Send(sp[0], Msg::Chat, "hello"); a.Send(sp[0], Msg::FileData, big); });
    Frame f1, f2;
    CHECK(b.Recv(sp[1], f1) && f1.type == Msg::Chat && f1.Text() == "hello", "small frame");
    CHECK(b.Recv(sp[1], f2) && f2.type == Msg::FileData && f2.Text() == big, "large frame");
    w.join();
    // tampering: flip one ciphertext bit on the wire
    {
        // build a valid frame into a buffer by sending through a second pair, then corrupt and forward it
        SocketHandle q[2];
        LocalSocketPair(q);
        a.Send(q[0], Msg::Chat, "secret");
        uint8_t buf[27];  // 4 len + 1 type + 6 text + 16 mac
        CHECK(RecvAll(q[1], buf, sizeof(buf)), "read frame");
        buf[7] ^= 1;
        SendAll(sp[0], buf, sizeof(buf));
        Frame f3;
        CHECK(!b.Recv(sp[1], f3), "tampered frame rejected");
        CloseSocket(q[0]);
        CloseSocket(q[1]);
    }
    CloseSocket(sp[0]);
    CloseSocket(sp[1]);
    std::string sn1 = SafetyNumber(ia.PubHex(), ib.PubHex()), sn2 = SafetyNumber(ib.PubHex(), ia.PubHex());
    CHECK(sn1 == sn2 && sn1.size() == 35, "safety number symmetric: %s", sn1.c_str());
    CHECK(KeyFingerprint(ia.PubHex()).size() == 19, "fingerprint");
    Identity re;
    CHECK(re.LoadSeed(ia.seedHex) && re.PubHex() == ia.PubHex(), "identity reload");
}

static void TestMdnsPackets() {
    printf("[mdns packets]\n");
    using namespace mdns;
    Service sv;
    sv.instance = InstanceLabel(u8"Kasia's Pixel 8 (żółw).x", "d8875e8b-67f4") + "." + kServiceType;
    sv.host = "ls-d8875e8b.local";
    sv.port = 47820;
    sv.txt = {{"id", "d8875e8b-67f4-4cd4"}, {"v", "2"}, {"name", u8"Kasia's Pixel 8 (żółw).x"}, {"pf", "Android"}};
    auto b = BuildResponse(sv, {"192.168.1.20", "10.0.0.5"}, 120);
    Packet pk;
    CHECK(Parse(b.data(), b.size(), pk) && pk.IsResponse(), "parse response");
    CHECK(pk.records.size() == 5, "5 records (%d)", (int)pk.records.size());
    auto peers = ExtractPeers(pk, "10.0.0.5");
    CHECK(peers.size() == 1 && peers[0].id == "d8875e8b-67f4-4cd4" && peers[0].port == 47820 && peers[0].ip == "10.0.0.5" &&
              peers[0].name == u8"Kasia's Pixel 8 (żółw).x" && peers[0].platform == "Android" && !peers[0].goodbye,
          "extract peer");
    CHECK(ExtractPeers(pk, "172.16.0.9").at(0).ip == "192.168.1.20", "use A record when source is not one of them");
    auto bye = BuildResponse(sv, {"10.0.0.5"}, 0);
    Packet pb;
    Parse(bye.data(), bye.size(), pb);
    auto gp = ExtractPeers(pb, "10.0.0.5");
    CHECK(gp.size() == 1 && gp[0].goodbye, "goodbye");
    // question handling
    auto q = BuildQuery(kServiceType, PTR);
    Packet pq;
    CHECK(Parse(q.data(), q.size(), pq) && !pq.IsResponse() && pq.questions.size() == 1, "parse query");
    CHECK(ShouldAnswer(pq.questions[0], sv), "answer PTR query");
    CHECK(ShouldAnswer({"_services._dns-sd._udp.local", PTR, 1}, sv), "answer service enumeration");
    CHECK(ShouldAnswer({sv.instance, SRV, 1}, sv) && ShouldAnswer({"LS-D8875E8B.LOCAL", A, 1}, sv), "answer SRV / A (case-insensitive)");
    CHECK(!ShouldAnswer({"_http._tcp.local", PTR, 1}, sv), "ignore other services");
    // legacy unicast reply echoes the question and the id
    std::vector<Question> echo{pq.questions[0]};
    auto lu = BuildResponse(sv, {"10.0.0.5"}, 120, 0x1234, &echo);
    Packet pl;
    CHECK(Parse(lu.data(), lu.size(), pl) && pl.id == 0x1234 && pl.questions.size() == 1 && pl.records[1].cls == 1 && pl.records[1].ttl == 10,
          "legacy unicast reply");
    // hand-made packet using name compression pointers (as real stacks send)
    std::vector<uint8_t> c = {0, 0, 0x84, 0, 0, 0, 0, 2, 0, 0, 0, 0};
    auto lbl = [&](const char* t) { c.push_back((uint8_t)strlen(t)); c.insert(c.end(), t, t + strlen(t)); };
    size_t svcOff = c.size();
    lbl("_lunashare"); lbl("_tcp"); lbl("local"); c.push_back(0);
    c.insert(c.end(), {0, 12, 0, 1, 0, 0, 0, 120});
    size_t instOff = c.size() + 2;
    c.insert(c.end(), {0, 6});  // rdlength: label(4) + pointer(2)
    lbl("abc");
    c.insert(c.end(), {(uint8_t)(0xC0 | (svcOff >> 8)), (uint8_t)svcOff});
    c.insert(c.end(), {(uint8_t)(0xC0 | (instOff >> 8)), (uint8_t)instOff, 0, 33, 0x80, 1, 0, 0, 0, 120, 0, 10, 0, 0, 0, 0, 0x1F, 0x90});  // rdlength 6 + label(2) + pointer(2)
    lbl("h"); c.insert(c.end(), {0xC0, (uint8_t)(svcOff + 16)});
    Packet pc;
    CHECK(Parse(c.data(), c.size(), pc) && pc.records.size() == 2 && pc.records[0].target == "abc._lunashare._tcp.local" &&
              pc.records[1].name == "abc._lunashare._tcp.local" && pc.records[1].port == 8080 && pc.records[1].target == "h.local",
          "compressed names");
    // malformed / hostile input must not crash or loop
    std::vector<uint8_t> loop = {0, 0, 0x84, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 1, 0, 1};
    Packet bad;
    CHECK(!Parse(loop.data(), loop.size(), bad), "pointer loop rejected");
    for (size_t cut = 0; cut < b.size(); cut += 7) Parse(b.data(), cut, bad);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestHash();
    TestCrypto();
    TestMdnsPackets();
    printf("root: %s\n", Root().c_str());

    auto A = MakeEngine("alice", 48110, false, true);
    auto B = MakeEngine("bob", 48120, true, true);
    std::string idA = A->settings.deviceId, idB = B->settings.deviceId;

    printf("[discovery]\n");
    bool found = WaitFor([&] { auto l = A->Lock(); return A->online.count(idB) > 0; }, 6000);
    printf("  broadcast discovery %s\n", found ? "works" : "not available in this environment (ok)");

    printf("[connect + chat]\n");
    if (!WaitFor([&] { return A->IsConnected(idB) && B->IsConnected(idA); }, 1500)) A->ConnectAddress("127.0.0.1", B->boundPort);
    CHECK(WaitFor([&] { return A->IsConnected(idB) && B->IsConnected(idA); }, 5000), "connect");
    {
        auto lk = A->Lock();
        CHECK(A->conns.size() >= 1, "conns");
    }
    A->SendChat(idB, u8"cześć Bob! zażółć gęślą jaźń");
    CHECK(WaitFor([&] { auto l = B->Lock(); return !B->chats[idA].messages.empty(); }, 3000), "chat arrives");
    CHECK(WaitFor([&] { auto l = A->Lock(); return A->chats[idB].messages.back().delivered; }, 3000), "chat delivered ack");
    {
        auto lk = B->Lock();
        CHECK(B->chats[idA].messages.back().text == u8"cześć Bob! zażółć gęślą jaźń", "utf8 text");
        CHECK(B->devices[idA].name == "alice", "device name from hello");
    }

    // ------------------------------------------------------------ pause / resume
    printf("[pause/resume]\n");
    const int64_t SZ = 24 << 20;
    std::string f1 = JoinPath(Root(), "src/big1.bin");
    WriteRandomFile(f1, SZ, 1);
    std::string sha1 = Sha256File(f1);
    {
        Settings s = A->GetSettings();
        s.speedLimitKBs = 6000;
        A->ApplySettings(s);
    }
    uint64_t b1 = A->SendPaths(idB, {f1});
    uint64_t t1 = FirstTid(*A, b1);
    CHECK(t1 != 0, "transfer created");
    CHECK(WaitFor([&] { auto t = GetT(*B, t1); return t && t->done.load() > SZ / 4; }, 15000), "progress on receiver");
    B->Pause(t1);  // receiver pauses
    CHECK(WaitFor([&] { return StateOf(*A, t1) == TState::Paused; }, 3000), "sender sees pause, state=%s", TStateName(StateOf(*A, t1)));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    int64_t d0 = GetT(*B, t1)->done.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK(GetT(*B, t1)->done.load() == d0, "no progress while paused");
    printf("  paused at %.1f%%\n", 100.0 * (double)d0 / (double)SZ);
    A->Resume(t1);  // sender resumes
    CHECK(WaitFor([&] { return StateOf(*A, t1) == TState::Completed && StateOf(*B, t1) == TState::Completed; }, 20000),
          "completed after resume: A=%s B=%s", TStateName(StateOf(*A, t1)), TStateName(StateOf(*B, t1)));
    {
        auto tb = GetT(*B, t1);
        CHECK(tb && Sha256File(tb->localPath) == sha1, "received file hash matches");
        CHECK(tb && tb->recvSha == sha1 && GetT(*A, t1)->recvSha == sha1, "both sides recorded verified hash");
        CHECK(!FileExists(tb->partPath), "partial removed");
        auto lk = B->Lock();
        CHECK(!B->history.empty() && B->history.back().verified, "history entry verified");
    }

    // ------------------------------------------------------------ restart of the receiver mid-transfer
    printf("[receiver restart resume]\n");
    std::string f2 = JoinPath(Root(), "src/big2.bin");
    WriteRandomFile(f2, SZ, 2);
    std::string sha2 = Sha256File(f2);
    uint64_t b2 = A->SendPaths(idB, {f2});
    uint64_t t2 = FirstTid(*A, b2);
    CHECK(WaitFor([&] { auto t = GetT(*B, t2); return t && t->done.load() > SZ / 3; }, 15000), "progress before restart");
    B->Stop();
    int64_t before = 0;
    B.reset();
    CHECK(WaitFor([&] { return StateOf(*A, t2) == TState::Interrupted; }, 3000), "sender interrupted: %s", TStateName(StateOf(*A, t2)));
    B = MakeEngine("bob", 48120, true, true);
    before = GetT(*B, t2) ? GetT(*B, t2)->done.load() : -1;
    printf("  resumed from %.1f%% after restart\n", 100.0 * (double)before / (double)SZ);
    CHECK(before > 0, "partial progress persisted");
    if (!WaitFor([&] { return A->IsConnected(idB); }, 2500)) A->ConnectAddress("127.0.0.1", B->boundPort);
    CHECK(WaitFor([&] { return StateOf(*A, t2) == TState::Completed && StateOf(*B, t2) == TState::Completed; }, 25000),
          "completed after restart: A=%s B=%s", TStateName(StateOf(*A, t2)), TStateName(StateOf(*B, t2)));
    CHECK(B->totalDown.load() < SZ - before / 2, "resumed instead of restarting (downloaded %lld)", (long long)B->totalDown.load());
    {
        auto tb = GetT(*B, t2);
        CHECK(tb && Sha256File(tb->localPath) == sha2, "restart: hash matches");
    }

    // ------------------------------------------------------------ corruption is detected by the final hash
    printf("[corruption detection]\n");
    std::string f3 = JoinPath(Root(), "src/big3.bin");
    WriteRandomFile(f3, SZ, 3);
    uint64_t t3 = FirstTid(*A, A->SendPaths(idB, {f3}));
    CHECK(WaitFor([&] { auto t = GetT(*B, t3); return t && t->done.load() > SZ / 3; }, 15000), "progress");
    A->Pause(t3);
    CHECK(WaitFor([&] { return StateOf(*B, t3) == TState::Paused; }, 3000), "receiver sees pause");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        auto tb = GetT(*B, t3);
        FILE* f = fopen(tb->partPath.c_str(), "r+b");
        CHECK(f != nullptr, "open part");
        if (f) { fseek(f, 1000, SEEK_SET); unsigned char c = 0x5A; fwrite(&c, 1, 1, f); fclose(f); }
    }
    B->Resume(t3);
    CHECK(WaitFor([&] { return StateOf(*A, t3) == TState::Failed && StateOf(*B, t3) == TState::Failed; }, 20000),
          "mismatch detected: A=%s B=%s", TStateName(StateOf(*A, t3)), TStateName(StateOf(*B, t3)));
    A->Retry(t3);
    {
        Settings s = A->GetSettings();
        s.speedLimitKBs = 0;
        A->ApplySettings(s);
    }

    // ------------------------------------------------------------ folder
    printf("[folder + retry]\n");
    std::string dir = JoinPath(Root(), "src/Photos");
    WriteRandomFile(JoinPath(dir, "a.jpg"), 100000, 4);
    WriteRandomFile(JoinPath(dir, "sub/b.jpg"), 200000, 5);
    WriteRandomFile(JoinPath(dir, "sub/deeper/c.txt"), 0, 6);
    uint64_t bf = A->SendPaths(idB, {dir});
    CHECK(A->BatchTransfers(bf).size() == 3, "3 files in batch");
    CHECK(WaitFor([&] {
        for (auto& t : A->BatchTransfers(bf)) if (StateOf(*B, t->tid) != TState::Completed) return false;
        return true;
    }, 15000), "folder complete");
    std::string dl = B->GetSettings().downloadDir;
    CHECK(FileExists(JoinPath(dl, "Photos/sub/deeper/c.txt")) && FileSize(JoinPath(dl, "Photos/sub/b.jpg")) == 200000, "folder structure kept");
    // retried corrupted file should now complete
    bool retryOk = WaitFor([&] {
        auto lk = A->Lock();
        for (auto& [id, t] : A->transfers)
            if (t->localPath == f3 && t->state == TState::Completed) return true;
        return false;
    }, 20000);
    CHECK(retryOk, "retry after failure completes");

    // ------------------------------------------------------------ impostor with the same device id
    printf("[key pinning]\n");
    {
        std::string cfg = JoinPath(Root(), "mallory/config");
        MakeDirs(cfg);
        Settings s;
        s.deviceId = idB;  // pretends to be Bob, but has its own key
        s.displayName = "bob";
        s.downloadDir = JoinPath(Root(), "mallory/dl");
        s.tcpPort = 48140;
        s.discovery = false;
        SaveJsonFile(JoinPath(cfg, "settings.json"), {{"settings", ToJson(s, true)}});
        B->Stop();
        B.reset();
        Engine M;
        M.configDir = cfg;
        M.Start();
        A->ConnectAddress("127.0.0.1", M.boundPort);
        bool warned = WaitFor([&] { auto l = A->Lock(); return !A->devices[idB].pendingKey.empty(); }, 5000);
        CHECK(warned, "key change detected");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(!A->IsConnected(idB), "impostor not connected");
        M.Stop();
        B = MakeEngine("bob", 48120, true, true);
        A->ConnectAddress("127.0.0.1", B->boundPort);
        bool ok = WaitFor([&] { return A->IsConnected(idB); }, 2500);
        if (!ok) {
            printf("    (first attempt did not connect, retrying)\n");
            A->ConnectAddress("127.0.0.1", B->boundPort);
            ok = WaitFor([&] { return A->IsConnected(idB); }, 4000);
        }
        if (!ok) {
            for (auto& e : A->PopEvents()) printf("    A event %d: %s\n", (int)e.type, e.text.c_str());
            for (auto& e : B->PopEvents()) printf("    B event %d: %s\n", (int)e.type, e.text.c_str());
            auto l2 = A->Lock();
            for (auto& c : A->conns) printf("    A conn %s ready=%d closed=%d peer=%s\n", c->addr.c_str(), (int)c->ready, (int)c->closed.load(), c->peerId.c_str());
            printf("    pending=%s blocked=%d\n", A->devices[idB].pendingKey.c_str(), (int)A->devices[idB].blocked);
        }
        CHECK(ok, "real Bob still connects (pinned key)");
        auto l = A->Lock();
        A->devices[idB].pendingKey.clear();
    }
    printf("[plaintext peer rejected]\n");
    {
        SocketHandle s = TcpConnect("127.0.0.1", A->boundPort, 2000);
        const char junk[] = "\x20\x00\x00\x00\x01{\"app\":\"LunaShare\",\"id\":\"x\"}...........................................";
        SendAll(s, junk, sizeof(junk));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto l = A->Lock();
        bool any = false;
        for (auto& c : A->conns) if (c->ready && c->peerId == "x") any = true;
        CHECK(!any, "plaintext hello ignored");
        CloseSocket(s);
    }
    printf("[throughput]\n");
    {
        std::string fb = JoinPath(Root(), "src/speed.bin");
        WriteRandomFile(fb, 96 << 20, 9);
        int64_t t0 = SteadyMs();
        uint64_t tb = FirstTid(*A, A->SendPaths(idB, {fb}));
        CHECK(WaitFor([&] { return StateOf(*A, tb) == TState::Completed; }, 60000), "speed transfer");
        double secs = (double)(SteadyMs() - t0) / 1000.0;
        printf("  96 MB encrypted + hashed twice over loopback: %.2f s (%.0f MB/s end to end)\n", secs, 96.0 / secs);
    }

    // ------------------------------------------------------------ export / import
    printf("[settings export/import]\n");
    A->SetCustomName(idB, "Bob's phone");
    A->SetTrusted(idB, true);
    std::string exp = JoinPath(Root(), "export/lunashare-settings.json");
    std::string err;
    CHECK(A->ExportSettings(exp, true, false, &err), "export: %s", err.c_str());
    auto C = MakeEngine("carol", 48130, false, false);
    CHECK(C->ImportSettings(exp, false, &err), "import: %s", err.c_str());
    {
        auto lk = C->Lock();
        CHECK(C->devices.count(idB) && C->devices[idB].customName == "Bob's phone" && C->devices[idB].trusted, "custom name imported");
        CHECK(C->settings.deviceId != idA, "identity not cloned");
        CHECK(C->settings.displayName == "alice", "settings imported");
        CHECK(!C->history.empty(), "history imported");
    }

    C->Stop();
    B->Stop();
    A->Stop();

    printf("[mdns discovery only]\n");
    {
        auto D = MakeEngine("dave", 48150, false, true, false, true);
        auto E = MakeEngine("erin", 48160, false, true, false, true);
        std::string idD = D->settings.deviceId, idE = E->settings.deviceId;
        bool found = WaitFor([&] {
            auto l1 = D->Lock();
            auto l2 = E->Lock();
            return D->online.count(idE) && D->online[idE].lastMdns && E->online.count(idD) && E->online[idD].lastMdns;
        }, 8000);
        printf("  mDNS: %s / %s\n", D->MdnsStatus().c_str(), E->MdnsStatus().c_str());
        CHECK(found, "found each other via mDNS only");
        {
            auto l = D->Lock();
            CHECK(D->online[idE].lastBeacon == 0 && D->online[idE].port == E->boundPort, "no broadcast used, correct port");
        }
        CHECK(WaitFor([&] { return D->IsConnected(idE) && E->IsConnected(idD); }, 8000), "auto-connected via mDNS");
        E->Stop();  // sends goodbye
        bool gone = WaitFor([&] { auto l = D->Lock(); return !D->online.count(idE) || D->online[idE].lastMdns == 0; }, 4000);
        CHECK(gone, "goodbye removes the device");
        D->Stop();
    }
    printf("[internet rendezvous]\n");
    {
        RendezvousServer srv;
        RendezvousServer::Options o;
        o.bind = "127.0.0.1";
        o.port = 48190;
        std::string err;
        CHECK(srv.Start(o, &err), "server start: %s", err.c_str());
        auto F = MakeEngine("frank", 48170, true, false, false, false, "127.0.0.1:48190", "family");
        auto G = MakeEngine("gina", 48180, true, false, false, false, "127.0.0.1:48190", "family");
        auto H = MakeEngine("hank", 48185, true, false, false, false, "127.0.0.1:48190", "strangers");
        std::string idF = F->settings.deviceId, idG = G->settings.deviceId, idH = H->settings.deviceId;
        bool listed = WaitFor([&] {
            auto l1 = F->Lock();
            auto l2 = G->Lock();
            return F->online.count(idG) && F->online[idG].lastWan && G->online.count(idF) && G->online[idF].lastWan;
        }, 6000);
        CHECK(listed, "devices in the same room see each other (F: %s)", F->WanStatus().c_str());
        CHECK(WaitFor([&] { return srv.DeviceCount() == 3 && srv.RoomCount() == 2; }, 5000), "server state %d devices %d rooms",
              (int)srv.DeviceCount(), (int)srv.RoomCount());
        {
            auto l = F->Lock();
            CHECK(!F->online.count(idH), "other room is invisible");
        }
        bool p2p = WaitFor([&] {
            auto c = F->ReadyConn(idG);
            return c && c->transport == Transport::Internet && G->IsConnected(idF);
        }, 10000);
        CHECK(p2p, "hole-punched peer-to-peer connection");
        F->SendChat(idG, "hello over the internet");
        CHECK(WaitFor([&] { auto l = G->Lock(); return !G->chats[idF].messages.empty(); }, 5000), "chat over internet");
        std::string fw = JoinPath(Root(), "src/wan.bin");
        WriteRandomFile(fw, 24 << 20, 11);
        std::string shaw = Sha256File(fw);
        int64_t t0 = SteadyMs();
        uint64_t tw = FirstTid(*F, F->SendPaths(idG, {fw}));
        CHECK(WaitFor([&] { return StateOf(*F, tw) == TState::Completed && StateOf(*G, tw) == TState::Completed; }, 60000),
              "file over internet: F=%s G=%s", TStateName(StateOf(*F, tw)), TStateName(StateOf(*G, tw)));
        auto tg = GetT(*G, tw);
        CHECK(tg && Sha256File(tg->localPath) == shaw, "internet transfer verified");
        printf("  24 MB over KCP/UDP loopback: %.2f s\n", (double)(SteadyMs() - t0) / 1000.0);
        // allow-list: a server that only accepts another room rejects us
        RendezvousServer locked;
        RendezvousServer::Options lo;
        lo.bind = "127.0.0.1";
        lo.port = 48191;
        lo.allowedRooms.insert(RoomHash("other", "x"));
        CHECK(locked.Start(lo, &err), "locked server");
        auto K = MakeEngine("kate", 48195, true, false, false, false, "127.0.0.1:48191", "family");
        CHECK(WaitFor([&] { return K->WanStatus().find("not allowed") != std::string::npos; }, 5000), "room allow-list: %s", K->WanStatus().c_str());
        K->Stop();
        locked.Stop();
        H->Stop();
        G->Stop();
        F->Stop();
        CHECK(WaitFor([&] { return srv.DeviceCount() == 0; }, 7000), "devices unregister on exit (%d left)", (int)srv.DeviceCount());
        srv.Stop();
    }

    printf("[calls: signalling + media]\n");
    {
        auto P = MakeEngine("paula", 48210, false, false, false, false, "", "", {"voice"});
        auto Q = MakeEngine("quinn", 48220, false, false, false, false, "", "", {"voice"});
        auto N = MakeEngine("nocalls", 48230, false, false, false, false);  // no voice module
        std::string idP = P->settings.deviceId, idQ = Q->settings.deviceId, idN = N->settings.deviceId;
        P->ConnectAddress("127.0.0.1", Q->boundPort);
        P->ConnectAddress("127.0.0.1", N->boundPort);
        CHECK(WaitFor([&] { return P->IsConnected(idQ) && P->IsConnected(idN); }, 5000), "connected");
        CHECK(P->PeerHasCap(idQ, "voice") && !P->PeerHasCap(idN, "voice"), "capabilities exchanged");
        CHECK(P->StartCall(idN) == 0, "cannot call a device without the module");
        std::atomic<int> gotP{0}, gotQ{0};
        P->onMedia = [&](uint64_t, const uint8_t* d, size_t n) { if (n == 3 && d[0] == 'q') gotP++; };
        Q->onMedia = [&](uint64_t, const uint8_t* d, size_t n) { if (n == 3 && d[0] == 'p') gotQ++; };
        uint64_t cid = P->StartCall(idQ);
        CHECK(cid != 0, "call started");
        CHECK(WaitFor([&] { auto l = Q->Lock(); return Q->call.state == CallState::IncomingRinging && Q->call.id == cid; }, 3000), "ringing");
        Q->SendMedia("qqq", 3);  // not active yet: must be ignored
        Q->AcceptCall();
        CHECK(WaitFor([&] { auto l = P->Lock(); return P->call.state == CallState::Active; }, 3000), "active on caller");
        for (int i = 0; i < 50; i++) {
            P->SendMedia("ppp", 3);
            Q->SendMedia("qqq", 3);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(WaitFor([&] { return gotP >= 45 && gotQ >= 45; }, 3000), "media both ways (%d / %d)", gotP.load(), gotQ.load());
        // a third device calling now gets "busy"
        N->localCaps = {"voice"};
        P->EndCall();
        CHECK(WaitFor([&] { auto l = Q->Lock(); return Q->call.state == CallState::None; }, 3000), "hang up reaches the other side");
        {
            auto l = Q->Lock();
            auto& msgs = Q->chats[idP].messages;
            CHECK(!msgs.empty() && msgs.back().text.find("Incoming call - 0:0") == 0, "call logged: %s", msgs.empty() ? "" : msgs.back().text.c_str());
        }
        // declined
        cid = P->StartCall(idQ);
        CHECK(WaitFor([&] { auto l = Q->Lock(); return Q->call.state == CallState::IncomingRinging; }, 3000), "ringing again");
        Q->EndCall();
        CHECK(WaitFor([&] { auto l = P->Lock(); return P->call.state == CallState::None; }, 3000), "declined");
        {
            auto l = P->Lock();
            CHECK(P->chats[idQ].messages.back().text == "Call declined", "declined logged: %s", P->chats[idQ].messages.back().text.c_str());
        }
        // connection loss ends an active call
        P->StartCall(idQ);
        CHECK(WaitFor([&] { auto l = Q->Lock(); return Q->call.state == CallState::IncomingRinging; }, 3000), "ringing 3");
        Q->AcceptCall();
        CHECK(WaitFor([&] { auto l = P->Lock(); return P->call.state == CallState::Active; }, 3000), "active 3");
        Q->Stop();
        CHECK(WaitFor([&] { auto l = P->Lock(); return P->call.state == CallState::None; }, 5000), "connection loss ends the call");
        N->Stop();
        P->Stop();
    }

    std::error_code ec;
    fs::remove_all(Root(), ec);
    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
