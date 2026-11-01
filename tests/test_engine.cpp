// Headless end-to-end tests: two engines in one process talking over loopback TCP.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <thread>

#include "crypto.h"
#include "engine.h"
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

static std::unique_ptr<Engine> MakeEngine(const std::string& name, uint16_t port, bool autoAcceptAll, bool discovery) {
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
        j = {{"settings", ToJson(s, false)}, {"devices", json::array()}};
        SaveJsonFile(JoinPath(cfg, "settings.json"), j);
    }
    auto e = std::make_unique<Engine>();
    e->configDir = cfg;
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

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestHash();
    TestCrypto();
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
        CHECK(WaitFor([&] { return A->IsConnected(idB); }, 5000), "real Bob still connects (pinned key)");
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
    std::error_code ec;
    fs::remove_all(Root(), ec);
    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
