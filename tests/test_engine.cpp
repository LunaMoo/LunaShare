// Headless end-to-end tests: two engines in one process talking over loopback TCP.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <atomic>
#include <fstream>
#include <thread>

#include "crypto.h"
#include "engine.h"
#include "mdns.h"
#include "rendezvous_server.h"
#include "wan.h"
#include "upnp.h"
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

static void TestSeal() {
    printf("[sealed messages]\n");
    Identity a, b;
    a.Generate();
    b.Generate();
    std::string msg = u8"wiadomość dla Ciebie, nawet offline";
    std::string box = SealTo(b.PubHex(), msg), out;
    CHECK(box.size() == msg.size() + 73, "envelope size");
    CHECK(OpenSealed(b, box, out) && out == msg, "recipient opens it");
    CHECK(!OpenSealed(a, box, out), "nobody else can");
    box[80] ^= 1;
    CHECK(!OpenSealed(b, box, out), "tampering is detected");
    CHECK(SealTo(b.PubHex(), msg) != SealTo(b.PubHex(), msg), "fresh keys every time");
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
    SetMachineId("");  // engines on one test computer would all look like the same device to the server
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestHash();
    TestCrypto();
    TestMdnsPackets();
    TestSeal();
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
    CHECK(WaitFor([&] { auto l = B->Lock(); return !B->chats[B->ChatKey(idA)].messages.empty(); }, 3000), "chat arrives");
    CHECK(WaitFor([&] { auto l = A->Lock(); return A->chats[A->ChatKey(idB)].messages.back().delivered; }, 3000), "chat delivered ack");
    {
        auto lk = B->Lock();
        CHECK(B->chats[B->ChatKey(idA)].messages.back().text == u8"cześć Bob! zażółć gęślą jaźń", "utf8 text");
        CHECK(B->devices[idA].name == "alice", "device name from hello");
    }

    // ------------------------------------------------------------ delete messages
    printf("[delete messages]\n");
    A->SendChat(idB, "oops, wrong chat");
    A->SendChat(idB, "keep this one");
    CHECK(WaitFor([&] { auto l = B->Lock(); return B->chats[B->ChatKey(idA)].messages.size() >= 3; }, 3000), "two more chats arrive");
    CHECK(WaitFor([&] { auto l = A->Lock(); auto& m = A->chats[A->ChatKey(idB)].messages; return m.size() >= 3 && m[m.size() - 2].delivered; }, 3000), "delivered");
    std::string oops, keep;
    {
        auto l = A->Lock();
        auto& m = A->chats[A->ChatKey(idB)].messages;
        oops = m[m.size() - 2].mid;
        keep = m.back().mid;
    }
    A->DeleteMessages(idB, {oops}, true);
    {
        auto l = A->Lock();
        for (auto& m : A->chats[A->ChatKey(idB)].messages) CHECK(m.mid != oops, "deleted locally");
    }
    CHECK(WaitFor([&] {
        auto l = B->Lock();
        for (auto& m : B->chats[B->ChatKey(idA)].messages) if (m.mid == oops) return false;
        return true;
    }, 3000), "deleted on the other device");
    CHECK(WaitFor([&] { auto l = A->Lock(); return A->chats[A->ChatKey(idB)].pendingDeletes.empty(); }, 3000), "delete acknowledged");
    {
        auto l = B->Lock();
        bool kept = false;
        for (auto& m : B->chats[B->ChatKey(idA)].messages) kept |= m.mid == keep;
        CHECK(kept, "other message kept");
    }
    // "for me" only: stays on the other side
    A->DeleteMessages(idB, {keep}, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        auto l = B->Lock();
        bool kept = false;
        for (auto& m : B->chats[B->ChatKey(idA)].messages) kept |= m.mid == keep;
        CHECK(kept, "delete for me stays local");
    }

    // ------------------------------------------------------------ reactions
    printf("[reactions]\n");
    A->SendChat(idB, "react to this");
    {
        auto l = A->Lock();
        keep = A->chats[A->ChatKey(idB)].messages.back().mid;
    }
    CHECK(WaitFor([&] {
        auto l = B->Lock();
        for (auto& m : B->chats[B->ChatKey(idA)].messages) if (m.mid == keep) return true;
        return false;
    }, 3000), "message to react to arrives");
    B->React(idA, keep, "\xE2\x9D\xA4\xEF\xB8\x8F");  // B reacts to A's message
    auto theirOn = [&](Engine& e, const std::string& peer, const std::string& want) {
        auto l = e.Lock();
        for (auto& m : e.chats[e.ChatKey(peer)].messages)
            if (m.mid == keep) return m.theirReaction == want;
        return false;
    };
    CHECK(WaitFor([&] { return theirOn(*A, idB, "\xE2\x9D\xA4\xEF\xB8\x8F"); }, 3000), "reaction arrives");
    CHECK(WaitFor([&] {
        auto l = B->Lock();
        for (auto& m : B->chats[B->ChatKey(idA)].messages)
            if (m.mid == keep) return m.reactionSent && m.myReaction == "\xE2\x9D\xA4\xEF\xB8\x8F";
        return false;
    }, 3000), "reaction acknowledged");
    A->React(idB, keep, "\xF0\x9F\x91\x8D");  // the sender reacts to its own message too
    CHECK(WaitFor([&] { return theirOn(*B, idA, "\xF0\x9F\x91\x8D"); }, 3000), "reaction on own message arrives");
    B->React(idA, keep, "");
    CHECK(WaitFor([&] { return theirOn(*A, idB, ""); }, 3000), "reaction removed");
    {
        bool ev = false;
        for (auto& e : A->PopEvents()) ev |= e.type == EventType::ChatReaction;
        CHECK(ev, "reaction event");
    }

    // ------------------------------------------------------------ profile pictures
    printf("[profile picture]\n");
    {
        auto jpeg = [](uint8_t fill) {
            std::string j = "\xFF\xD8\xFF\xE0";
            j.append(2000, (char)fill);
            return j + "\xFF\xD9";
        };
        std::string pic1 = jpeg(1), pic2 = jpeg(2);
        CHECK(!A->SetAvatar("not a jpeg"), "rejects non-JPEG");
        CHECK(!A->SetAvatar(std::string("\xFF\xD8\xFF", 3) + std::string(Engine::kMaxAvatarBytes, 'x')), "rejects too large");
        CHECK(A->SetAvatar(pic1), "set picture");
        auto cached = [&](const std::string& want) {
            std::string p = B->AvatarPath(idA), got;
            if (p.empty()) return false;
            FILE* f = OpenFile(p, "rb");
            if (!f) return false;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) got.append(buf, n);
            fclose(f);
            return got == want;
        };
        CHECK(WaitFor([&] { return cached(pic1); }, 3000), "picture pushed to the connected device");
        std::string p1 = B->AvatarPath(idA);
        CHECK(A->SetAvatar(pic2), "change picture");
        CHECK(WaitFor([&] { return cached(pic2); }, 3000), "changed picture arrives");
        CHECK(B->AvatarPath(idA) != p1 && !FileExists(p1), "new file name, old one removed");
        CHECK(!A->AvatarPath("").empty(), "own picture path");
        // a device that missed the change catches up on the next connection
        A->Disconnect(idB);
        CHECK(WaitFor([&] { return !A->IsConnected(idB) && !B->IsConnected(idA); }, 3000), "disconnected");
        CHECK(A->SetAvatar(pic1), "change while offline");
        A->ConnectAddress("127.0.0.1", B->boundPort);
        CHECK(WaitFor([&] { return A->IsConnected(idB) && B->IsConnected(idA); }, 5000), "reconnect");
        CHECK(WaitFor([&] { return cached(pic1); }, 3000), "picture fetched after reconnecting");
        CHECK(A->SetAvatar(""), "remove picture");
        CHECK(WaitFor([&] { return B->AvatarPath(idA).empty(); }, 3000), "removal arrives");
    }

    // ------------------------------------------------------------ accounts
    printf("[accounts: linking a second device]\n");
    {
        CHECK(A->MyAccount() != B->MyAccount() && !A->MyAccount().empty(), "every device starts with its own account");
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->ChatKey(idA) == "@" + A->MyAccount(); }, 3000), "the other side sees a person");
        auto A2 = MakeEngine("alice-phone", 48130, false, false);
        std::string idA2 = A2->settings.deviceId;
        A2->ConnectAddress("127.0.0.1", A->boundPort);
        CHECK(WaitFor([&] { return A->IsConnected(idA2) && A2->IsConnected(idA); }, 5000), "new device connects");
        CHECK(A->LinkDevice(idA2), "link offered");
        std::string code;
        CHECK(WaitFor([&] {
            for (auto& e : A2->PopEvents())
                if (e.type == EventType::LinkRequest && e.peer == idA) code = e.text;
            return !code.empty();
        }, 3000), "link request arrives");
        CHECK(code.size() == 7 && code == A->LinkCode(idA2), "both show the same code (%s / %s)", code.c_str(), A->LinkCode(idA2).c_str());
        A2->AnswerLink(idA, true);
        CHECK(WaitFor([&] { return A2->MyAccount() == A->MyAccount(); }, 3000), "joined the account");
        CHECK(A2->GetSettings().displayName == "alice", "took the person's name");
        {
            auto l = A2->Lock();
            CHECK(A2->devices.count(idB) == 1, "contacts copied");
            auto it = A2->chats.find("@" + B->MyAccount());
            size_t onA = 0;
            {
                auto la = A->Lock();
                onA = A->chats["@" + B->MyAccount()].messages.size();
            }
            CHECK(it != A2->chats.end() && it->second.messages.size() >= onA && onA > 0, "conversations copied (%zu of %zu)",
                  it == A2->chats.end() ? (size_t)0 : it->second.messages.size(), onA);
        }
        CHECK(WaitFor([&] { return A->IsConnected(idA2) && A2->IsConnected(idA); }, 5000), "reconnected after linking");
        {
            auto l = A->Lock();
            CHECK(A->devices[idA2].accountPub == A->MyAccount() && A->ChatKey(idA2) == idA2, "own devices stay separate");
        }
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->devices.count(idA2) && B->ChatKey(idA2) == B->ChatKey(idA); }, 3000),
              "the contact learns about the new device");
        A2->ConnectAddress("127.0.0.1", B->boundPort);
        CHECK(WaitFor([&] { return A2->IsConnected(idB) && B->IsConnected(idA2); }, 5000), "new device reaches the contact");
        auto has = [](Engine& e, const std::string& key, const std::string& text, bool out) {
            auto l = e.Lock();
            auto it = e.chats.find(key);
            if (it == e.chats.end()) return false;
            for (auto& m : it->second.messages)
                if (m.text == text && m.outgoing == out) return true;
            return false;
        };
        B->SendChat("@" + A->MyAccount(), "to both of your devices");
        std::string bKey = "@" + B->MyAccount();
        CHECK(WaitFor([&] { return has(*A, bKey, "to both of your devices", false) && has(*A2, bKey, "to both of your devices", false); }, 3000),
              "a message to the person reaches all their devices");
        {
            auto l = A->Lock();
            int n = 0;
            for (auto& m : A->chats[bKey].messages) n += m.text == "to both of your devices";
            CHECK(n == 1, "no duplicates (%d)", n);
        }
        A->SendChat(idB, "sent from the computer");
        CHECK(WaitFor([&] { return has(*A2, bKey, "sent from the computer", true); }, 3000), "my sent message appears on my other device");
        CHECK(WaitFor([&] { return has(*B, "@" + A->MyAccount(), "sent from the computer", false); }, 3000), "and reaches the contact");
        A2->Stop();
    }
    printf("[group conversations]\n");
    {
        auto C = MakeEngine("carol", 48140, false, false);
        std::string idC = C->settings.deviceId;
        C->ConnectAddress("127.0.0.1", A->boundPort);
        CHECK(WaitFor([&] { auto l = C->Lock(); return C->ChatKey(idA) == "@" + A->MyAccount() && A->IsConnected(idC); }, 5000), "carol knows alice");
        {
            auto l = B->Lock();
            CHECK(B->devices.count(idC) == 0, "bob has never met carol");
        }
        std::string gk = A->CreateGroup("Family", {"@" + B->MyAccount(), "@" + C->MyAccount()});
        CHECK(Engine::IsGroupKey(gk), "group created (%s)", gk.c_str());
        auto inGroup = [&](Engine& e, size_t n) {
            auto l = e.Lock();
            auto g = e.GroupOf(gk);
            return g && !g->left && g->members.size() == n && g->name == "Family";
        };
        CHECK(WaitFor([&] { return inGroup(*B, 3) && inGroup(*C, 3); }, 3000), "both members get the group");
        {
            auto l = B->Lock();
            CHECK(B->devices.count(idC) == 1 && B->devices[idC].accountPub == C->MyAccount() && B->devices[idC].viaGroup,
                  "bob learns carol's device from the group (vouched by her account)");
            auto cs = B->Conversations();
            CHECK(std::find(cs.begin(), cs.end(), gk) != cs.end() && std::find(cs.begin(), cs.end(), "@" + C->MyAccount()) == cs.end(),
                  "the group is listed, carol is not a contact of bob");
        }
        auto gHas = [&](Engine& e, const std::string& text, const std::string& from) {
            auto l = e.Lock();
            auto it = e.chats.find(gk);
            if (it == e.chats.end()) return false;
            for (auto& m : it->second.messages)
                if (m.text == text && m.from == from && !m.outgoing) return true;
            return false;
        };
        A->SendChat(gk, "hello everyone");
        std::string aKey = "@" + A->MyAccount();
        CHECK(WaitFor([&] { return gHas(*B, "hello everyone", aKey) && gHas(*C, "hello everyone", aKey); }, 3000), "a group message reaches every member");
        {
            auto l = B->Lock();
            CHECK(B->chats["@" + A->MyAccount()].messages.end() ==
                      std::find_if(B->chats[aKey].messages.begin(), B->chats[aKey].messages.end(), [](auto& m) { return m.text == "hello everyone"; }),
                  "and stays out of the direct conversation");
        }
        C->SendChat(gk, "hi from carol");
        CHECK(WaitFor([&] { return gHas(*A, "hi from carol", "@" + C->MyAccount()); }, 3000), "carol's message reaches alice");
        C->ConnectAddress("127.0.0.1", B->boundPort);  // (on the internet: the server would put them in touch)
        CHECK(WaitFor([&] { return gHas(*B, "hi from carol", "@" + C->MyAccount()); }, 5000), "and bob, once they connect, though they never met");
        B->React(gk, [&] {
            auto l = B->Lock();
            for (auto& m : B->chats[gk].messages)
                if (m.text == "hi from carol") return m.mid;
            return std::string();
        }(), "\xe2\x9d\xa4");
        CHECK(WaitFor([&] {
            auto l = C->Lock();
            for (auto& m : C->chats[gk].messages)
                if (m.text == "hi from carol" && m.theirReaction == "\xe2\x9d\xa4") return true;
            return false;
        }, 3000), "reactions in the group");
        // ---- group call (directly between the devices)
        printf("[group calls: directly]\n");
        {
            for (Engine* e : {A.get(), B.get(), C.get()}) e->localCaps = {"voice", "video"};
            std::string idA2 = A->settings.deviceId;
            std::atomic<int> audioB{0}, audioC{0}, audioA{0}, frameB{0}, frameC{0}, keyAskA{0}, endedA{0};
            std::set<int> streamsB;
            std::mutex sm;
            A->onMedia = [&](uint64_t, int s, const uint8_t* d, size_t n) {
                if (n >= 2 && d[0] == 2 && d[1] == 2) keyAskA++;
                if (n && d[0] == 1) audioA++;
                (void)s;
            };
            B->onMedia = [&](uint64_t, int s, const uint8_t* d, size_t n) {
                if (n && d[0] == 1) {
                    audioB++;
                    std::lock_guard<std::mutex> l(sm);
                    streamsB.insert(s);
                }
                if (n >= 2 && d[0] == 2 && d[1] == 1) frameB++;
            };
            C->onMedia = [&](uint64_t, int, const uint8_t* d, size_t n) {
                if (n && d[0] == 1) audioC++;
                if (n >= 2 && d[0] == 2 && d[1] == 1) frameC++;
            };
            A->onMediaStreamEnd = [&](int) { endedA++; };
            uint64_t cid = A->StartCall(gk, "video");
            CHECK(cid != 0, "group call started");
            auto ringing = [&](Engine& e) { auto l = e.Lock(); return e.call.state == CallState::IncomingRinging && e.call.group == gk && e.call.id == cid; };
            CHECK(WaitFor([&] { return ringing(*B) && ringing(*C); }, 3000), "everyone in the group is called");
            B->AcceptCall();
            C->AcceptCall();
            auto peersOf = [&](Engine& e) { auto l = e.Lock(); return e.call.state == CallState::Active ? (int)e.call.peers.size() : -1; };
            CHECK(WaitFor([&] { return peersOf(*A) == 2 && peersOf(*B) == 2 && peersOf(*C) == 2; }, 5000), "everyone is in the call with everyone (%d %d %d)",
                  peersOf(*A), peersOf(*B), peersOf(*C));
            const uint8_t voice[10] = {1, 0, 0, 0, 0, 0, 0, 9, 9, 9};
            for (int i = 0; i < 5; i++) {
                A->SendMedia(voice, sizeof(voice));
                C->SendMedia(voice, sizeof(voice));
            }
            CHECK(WaitFor([&] { return audioB >= 10 && audioC >= 5 && audioA >= 5; }, 3000), "everyone hears everyone (%d %d %d)", (int)audioB, (int)audioC, (int)audioA);
            {
                std::lock_guard<std::mutex> l(sm);
                CHECK(streamsB.size() == 2 && !streamsB.count(0), "a voice stream per person");
            }
            const uint8_t frame[16] = {2, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1};
            const uint8_t camOn[3] = {2, 3, 1};
            A->SendMedia(camOn, sizeof(camOn));
            CHECK(WaitFor([&] { auto l = B->Lock(); return B->call.spotlight == idA2; }, 3000), "the one with the camera on is shown");
            CHECK(WaitFor([&] { return keyAskA > 0; }, 3000), "the viewer asks for a keyframe");
            CHECK(WaitFor([&] { auto l = C->Lock(); return C->call.spotlight == idA2; }, 3000), "on everyone's screen");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));  // (A hears that both watch it)
            int b0 = frameB, c0 = frameC;
            for (int i = 0; i < 3; i++) A->SendMedia(frame, sizeof(frame));
            CHECK(WaitFor([&] { return frameB >= b0 + 3 && frameC >= c0 + 3; }, 3000), "video goes to whoever watches it");
            C->SetSpotlight(B->settings.deviceId);  // C would rather watch B (camera off: stays chosen)
            std::this_thread::sleep_for(std::chrono::milliseconds(300));  // (A hears it doesn't want A's video any more)
            c0 = frameC;
            b0 = frameB;
            for (int i = 0; i < 3; i++) A->SendMedia(frame, sizeof(frame));
            CHECK(WaitFor([&] { return frameB >= b0 + 3; }, 3000), "B still does");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            CHECK(frameC == c0, "C, watching B now, doesn't get A's video (%d)", (int)frameC - c0);
            C->EndCall();
            CHECK(WaitFor([&] { return peersOf(*A) == 1 && peersOf(*B) == 1 && endedA == 1; }, 3000), "C leaves the call");
            B->EndCall();
            CHECK(WaitFor([&] { return peersOf(*A) == 0; }, 3000), "B too");
            A->EndCall();
            CHECK(peersOf(*A) == -1, "the call is over");
            {
                auto l = A->Lock();
                bool logged = false;
                for (auto& m : A->chats[gk].messages) logged |= m.kind == MsgKind::System && m.text.rfind("Group call - ", 0) == 0;
                CHECK(logged, "noted in the group");
            }
            for (Engine* e : {A.get(), B.get(), C.get()}) {
                e->onMedia = nullptr;
                e->onMediaStreamEnd = nullptr;
            }
        }
        // a stranger can't post into the group, members can't remove each other
        C->LeaveGroup(gk);
        CHECK(WaitFor([&] { return inGroup(*A, 2) && inGroup(*B, 2); }, 3000), "carol leaves: the others see it");
        {
            auto l = C->Lock();
            CHECK(C->GroupOf(gk) && C->GroupOf(gk)->left, "carol is out");
        }
        A->SendChat(gk, "after carol left");
        CHECK(WaitFor([&] { return gHas(*B, "after carol left", aKey); }, 3000), "the group goes on");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(!gHas(*C, "after carol left", aKey), "carol no longer gets the messages");
        B->RemoveFromGroup(gk, A->MyAccount());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(inGroup(*A, 2) && inGroup(*B, 2), "only the creator removes people");
        A->AddToGroup(gk, {"@" + C->MyAccount()});
        CHECK(WaitFor([&] { return inGroup(*C, 3); }, 3000), "carol added back");
        A->RemoveFromGroup(gk, C->MyAccount());
        CHECK(WaitFor([&] { auto l = C->Lock(); return C->GroupOf(gk) && C->GroupOf(gk)->left; }, 3000) && WaitFor([&] { return inGroup(*B, 2); }, 3000),
              "the creator removes carol");
        C->Stop();
    }

    printf("[watch together]\n");
    {
        std::string movie = JoinPath(Root(), "src/movie.bin");
        WriteRandomFile(movie, 20 << 20, 77);
        std::string bob = "@" + B->MyAccount();
        uint64_t sid = A->StartWatch(bob, movie);
        CHECK(sid != 0, "watching started");
        CHECK(WaitFor([&] { auto l = B->Lock(); return !B->watchInvites.empty() && B->watchInvites.back().sid == sid; }, 3000), "the other side is invited");
        CHECK(B->JoinWatch(sid), "joined");
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->watch.hold && A->watch.people.size() == 2; }, 3000), "everyone waits while the new viewer loads");
        // the viewer's player reads here and there (like a player looking at the start, the end, then playing)
        FILE* mf = OpenFile(movie, "rb");
        std::mt19937 rng(5);
        bool same = true;
        int64_t t0 = SteadyMs();
        for (int i = 0; i < 40 && same; i++) {
            int64_t off = i == 0 ? 0 : i == 1 ? (20 << 20) - 1000 : (int64_t)(rng() % ((20 << 20) - 70000));
            std::vector<uint8_t> got(65536), want(65536);
            int n = B->WatchRead(off, got.data(), (int)got.size());
            FileSeek(mf, off);
            size_t w = fread(want.data(), 1, (size_t)n > 0 ? (size_t)n : 0, mf);
            same = n > 0 && (size_t)n == w && memcmp(got.data(), want.data(), (size_t)n) == 0;
        }
        fclose(mf);
        CHECK(same, "what the player reads is the file");
        printf("  40 reads anywhere in the movie: %.2f s\n", (double)(SteadyMs() - t0) / 1000.0);
        std::vector<uint8_t> tmp(65536);
        B->WatchRead(0, tmp.data(), 1000);  // (back to the start: playing from there)
        CHECK(WaitFor([&] { auto l = A->Lock(); return !A->watch.hold; }, 5000), "once loaded, nobody waits");
        B->WatchControl(true, 12.5);  // the viewer presses play at 12.5 s
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->watch.playing && std::abs(A->watch.Target(SteadyMs()) - 12.5) < 1.0; }, 3000),
              "a viewer's play / seek reaches the host");
        A->WatchControl(false, 30);  // the host pauses at 30 s
        CHECK(WaitFor([&] { auto l = B->Lock(); return !B->watch.playing && std::abs(B->watch.Target(SteadyMs()) - 30) < 0.01; }, 3000),
              "and the host's reaches everyone");
        CHECK(B->WatchLoaded() > 0.1, "loaded %.0f%%", B->WatchLoaded() * 100);
        A->LeaveWatch();
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->watch.sid == 0; }, 3000), "the host ends it for everyone");
        CHECK(B->WatchRead(0, tmp.data(), 100) < 0, "the player stops reading");
        CHECK(!FileExists(JoinPath(B->configDir, "watch/" + ToHex((const uint8_t*)&sid, 8) + ".part")), "the temporary copy is gone");
    }

    printf("[accounts: recovery phrase, backup, removing a device]\n");
    {
        std::string phrase = A->RecoveryPhrase(), seed, err;
        int words = 1;
        for (char ch : phrase) words += ch == ' ';
        CHECK(words == 24, "24 words (%d)", words);
        CHECK(PhraseToSeed(phrase, seed, &err) && SeedToPhrase(seed) == phrase, "phrase round trip");
        std::string typo = phrase.substr(0, phrase.rfind(' ')) + (phrase.substr(phrase.rfind(' ') + 1) == "zoo" ? " abandon" : " zoo");
        CHECK(!PhraseToSeed(typo, seed, &err), "a wrong word is noticed (%s)", err.c_str());
        CHECK(!PhraseToSeed("abandon ability", seed, &err), "too few words");

        std::string bk = JoinPath(Root(), "alice.lunabackup");
        CHECK(!A->SaveBackup(bk, "123", &err), "short password refused");
        CHECK(A->SaveBackup(bk, "correct horse", &err), "backup saved: %s", err.c_str());
        auto A3 = MakeEngine("alice-new-phone", 48140, false, false);
        std::string idA3 = A3->settings.deviceId;
        CHECK(!A3->RestoreBackup(bk, "wrong horse", &err), "wrong password refused (%s)", err.c_str());
        CHECK(A3->RestoreBackup(bk, "correct horse", &err), "backup restored: %s", err.c_str());
        CHECK(A3->MyAccount() == A->MyAccount() && A3->GetSettings().displayName == "alice", "same account and name");
        {
            auto l = A3->Lock();
            auto it = A3->chats.find("@" + B->MyAccount());
            CHECK(A3->devices.count(idB) && it != A3->chats.end() && !it->second.messages.empty(), "contacts and chats restored");
        }
        A3->ConnectAddress("127.0.0.1", A->boundPort);
        auto listed = [&](Engine& e, const std::string& id) {
            auto l = e.Lock();
            for (auto& d : e.settings.accountDevices)
                if (d.id == id) return true;
            return false;
        };
        CHECK(WaitFor([&] { return listed(*A, idA3) && listed(*A3, idA); }, 6000), "restored device and old device find each other in the account");
        A->RemoveMyDevice(idA3);
        CHECK(WaitFor([&] { return A3->MyAccount() != A->MyAccount(); }, 3000), "removed device leaves the account");
        CHECK(!listed(*A, idA3), "no longer listed");
        A3->Stop();

        auto A4 = MakeEngine("alice-tablet", 48150, false, false);
        CHECK(!A4->RestoreFromPhrase(typo, &err), "phrase with a typo refused");
        CHECK(A4->RestoreFromPhrase(phrase, &err) && A4->MyAccount() == A->MyAccount(), "account restored from the phrase");
        A4->Stop();
    }

    // ------------------------------------------------------------ shared library
    printf("[shared library: one file from several contacts at once]\n");
    {
        auto L1 = MakeEngine("lib-anna", 48160, false, false), L2 = MakeEngine("lib-bart", 48162, false, false);
        auto L3 = MakeEngine("lib-cleo", 48164, false, false), L4 = MakeEngine("lib-stranger", 48166, false, false);
        std::string i1 = L1->settings.deviceId, i2 = L2->settings.deviceId, i3 = L3->settings.deviceId;
        const int64_t BIG = 20 << 20;
        std::string big = JoinPath(L1->SharedDir(), "films/holiday.bin");
        WriteRandomFile(big, BIG, 77);
        WriteRandomFile(JoinPath(L2->SharedDir(), "holiday copy.bin"), BIG, 77);  // the same content elsewhere
        WriteRandomFile(JoinPath(L1->SharedDir(), "notes.txt"), 1000, 5);
        std::string want = Sha256File(big);
        L1->RescanShared();
        L2->RescanShared();
        auto libSize = [](Engine& e) { auto l = e.Lock(); return e.myLibrary.size(); };
        CHECK(WaitFor([&] { return libSize(*L1) == 2 && libSize(*L2) == 1; }, 15000), "Shared folders indexed (%zu, %zu)", libSize(*L1), libSize(*L2));
        L3->ConnectAddress("127.0.0.1", L1->boundPort);
        L3->ConnectAddress("127.0.0.1", L2->boundPort);
        L4->ConnectAddress("127.0.0.1", L1->boundPort);
        CHECK(WaitFor([&] { return L3->IsConnected(i1) && L3->IsConnected(i2) && L4->IsConnected(i1); }, 5000), "connected");
        // contacts: we've exchanged messages; the stranger hasn't
        L3->SendChat(i1, "hi Anna");
        L3->SendChat(i2, "hi Bart");
        CHECK(WaitFor([&] { auto l1 = L1->Lock(); auto l2 = L2->Lock(); return L1->IsContact(i3) && L2->IsContact(i3); }, 3000), "now contacts");
        L3->RefreshLibrary(i1);
        L3->RefreshLibrary(i2);
        L4->RefreshLibrary(i1);
        CHECK(WaitFor([&] { auto l = L3->Lock(); return L3->peerLibraries[i1].files.size() == 2 && L3->peerLibraries[i2].files.size() == 1; }, 5000),
              "contacts see the lists");
        CHECK(WaitFor([&] { auto l = L4->Lock(); return L4->peerLibraries[i1].fetched && L4->peerLibraries[i1].off && L4->peerLibraries[i1].files.empty(); }, 5000),
              "a stranger sees nothing");
        LibFile f;
        {
            auto l = L3->Lock();
            for (auto& x : L3->peerLibraries[i1].files)
                if (x.rel == "films/holiday.bin") f = x;
            CHECK(!f.id.empty() && L3->peerLibraries[i2].files[0].id == f.id, "the same content has the same id on both");
        }
        for (auto* e : {L1.get(), L2.get()}) {  // each source alone is slow: together they're twice as fast
            Settings s = e->GetSettings();
            s.speedLimitKBs = 4096;
            e->ApplySettings(s);
        }
        int64_t t0 = SteadyMs();
        CHECK(L3->DownloadShared(f), "download started");
        auto state = [&] { auto l = L3->Lock(); auto it = L3->swarm.find(f.id); return it == L3->swarm.end() ? -1 : (int)it->second->state; };
        CHECK(WaitFor([&] { return state() == SwarmDownload::Done; }, 30000), "downloaded (state %d)", state());
        double secs = (double)(SteadyMs() - t0) / 1000.0;
        int64_t b1 = 0, b2 = 0;
        std::string got;
        {
            auto l = L3->Lock();
            auto& d = *L3->swarm[f.id];
            b1 = d.sources.count(i1) ? d.sources[i1].bytes : 0;
            b2 = d.sources.count(i2) ? d.sources[i2].bytes : 0;
            got = d.finalPath;
        }
        printf("  20 MB in %.1f s: %.1f MB from one, %.1f MB from the other (each limited to 4 MB/s)\n", secs, b1 / 1048576.0, b2 / 1048576.0);
        CHECK(b1 > BIG / 5 && b2 > BIG / 5, "both sources sent pieces");
        CHECK(secs < 4.5, "faster than one source alone could (%.1f s)", secs);
        CHECK(Sha256File(got) == want, "file verified");
        CHECK(got.find("films") != std::string::npos && L3->HaveShared(f.id), "kept in my Shared folder (and offered on)");
        L4->Stop();
        L3->Stop();
        L2->Stop();
        L1->Stop();
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
    printf("[upnp description]\n");
    {
        std::string xml = "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
                          "<controlURL>/l3f</controlURL></service></serviceList><deviceList><device><deviceList><device><serviceList>"
                          "<service>\n<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>\n"
                          "<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId><controlURL>ctl/IPConn</controlURL></service>"
                          "</serviceList></device></deviceList></device></deviceList></device></root>";
        std::string ctl, svc;
        CHECK(UpnpMapper::ParseControlUrl(xml, "http://192.168.1.1:5000/rootDesc.xml", &ctl, &svc), "finds the WAN service");
        CHECK(ctl == "http://192.168.1.1:5000/ctl/IPConn", "control URL made absolute (%s)", ctl.c_str());
        CHECK(svc == "urn:schemas-upnp-org:service:WANIPConnection:1", "service type (%s)", svc.c_str());
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
        CHECK(WaitFor([&] { auto l = G->Lock(); return !G->chats[G->ChatKey(idF)].messages.empty(); }, 5000), "chat over internet");
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
        printf("[usernames on the server]\n");
        {
            F->ClaimUsername("Frank.F");
            CHECK(WaitFor([&] { auto l = F->Lock(); return F->myUsername == "frank.f" && !F->usernamePending; }, 5000), "claimed (%s)", F->usernameError.c_str());
            G->ClaimUsername("frank.f");
            CHECK(WaitFor([&] { auto l = G->Lock(); return G->usernameError == "taken"; }, 5000), "someone else's name is taken (%s)", G->usernameError.c_str());
            G->LookupUsername("FRANK.F");
            CHECK(WaitFor([&] {
                auto l = G->Lock();
                if (G->lookup.pending || G->lookup.acc != F->MyAccount()) return false;
                for (auto& d : G->lookup.devices) if (d.first == idF) return true;
                return false;
            }, 5000), "lookup finds the account and its devices (%s)", G->lookup.error.c_str());
            F->ClaimUsername("no spaces!");
            CHECK(WaitFor([&] { auto l = F->Lock(); return F->usernameError == "invalid name"; }, 5000), "invalid name refused");
            F->ClaimUsername("");
            CHECK(WaitFor([&] { auto l = F->Lock(); return F->myUsername.empty() && !F->usernamePending; }, 5000), "released");
            G->LookupUsername("frank.f");
            CHECK(WaitFor([&] { auto l = G->Lock(); return !G->lookup.pending && G->lookup.error == "not found"; }, 5000), "gone after release");
            G->ClaimUsername("frank.f");
            CHECK(WaitFor([&] { auto l = G->Lock(); return G->myUsername == "frank.f"; }, 5000), "free again");
        }
        printf("[private contacts (invite links)]\n");
        {
            // L invites A and B. A and B are hidden in the same room "pub" with C (not hidden):
            // L sees A and B, each of them sees L, but A and B never see each other; C sees neither.
            RendezvousServer ps;
            RendezvousServer::Options po;
            po.bind = "127.0.0.1";
            po.port = 48196;
            po.dataDir = JoinPath(Root(), "privserver");
            CHECK(ps.Start(po, &err), "server for private links");
            const std::string srvAddr = "127.0.0.1:48196";
            auto L = MakeEngine("lina", 48181, true, false, false, false, srvAddr, "lroom");
            auto A = MakeEngine("adam", 48182, true, false, false, false, srvAddr, "pub");
            auto B = MakeEngine("bea", 48183, true, false, false, false, srvAddr, "pub");
            std::string idL = L->settings.deviceId, idA = A->settings.deviceId, idB = B->settings.deviceId;
            std::string kA(64, 'a'), kB(64, 'b');
            std::string code = MakeInviteCode({srvAddr, kA, "lina", "Family"});
            InviteInfo ii;
            CHECK(ParseInviteCode(" " + code.substr(0, 10) + "\n" + code.substr(10), ii) && ii.server == srvAddr && ii.secret == kA &&
                      ii.name == "lina" && ii.room == "Family",
                  "invite code round trip");
            CHECK(!ParseInviteCode("LSI1-xyz", ii), "bad invite code refused");
            auto setLinks = [](Engine& e, bool hidden, std::vector<std::string> keys, bool mine, const std::string& room) {
                auto lk = e.Lock();
                Settings s = e.settings;
                s.wanHidden = hidden;
                s.wanLinks.clear();
                for (auto& k : keys) {
                    WanLink w;
                    w.secret = k;
                    w.label = "x";
                    w.created = NowMs();
                    w.mine = mine;
                    s.wanLinks.push_back(w);
                }
                s.wanRoom = room;
                e.ApplySettings(s);
            };
            setLinks(*L, false, {kA, kB}, true, "lroom");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));  // (the inviter registers its codes first)
            {
                auto lk = A->Lock();
                A->machineId = "adams-phone";
            }
            setLinks(*A, true, {kA}, false, "pub");
            setLinks(*B, true, {kB}, false, "pub");
            auto sees = [](Engine& e, const std::string& id) {
                auto l = e.Lock();
                auto it = e.online.find(id);
                return it != e.online.end() && it->second.lastWan != 0;
            };
            CHECK(WaitFor([&] { return sees(*L, idA) && sees(*L, idB) && sees(*A, idL) && sees(*B, idL); }, 8000),
                  "linked devices find each other across rooms (L: %s)", L->WanStatus().c_str());
            auto C = MakeEngine("cyd", 48184, true, false, false, false, srvAddr, "pub");  // (after A and B hid)
            std::string idC = C->settings.deviceId;
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            CHECK(!sees(*A, idC) && !sees(*B, idC), "invited devices see no one but their links");
            CHECK(!sees(*A, idB) && !sees(*B, idA), "people invited separately don't see each other");
            CHECK(!sees(*C, idA) && !sees(*C, idB), "invited devices aren't listed in the room");
            CHECK(WaitFor([&] {
                auto c = L->ReadyConn(idA);
                return c && c->transport == Transport::Internet && A->IsConnected(idL);
            }, 10000), "connection through a link");
            L->SendChat(idA, "private hello");
            CHECK(WaitFor([&] { auto l = A->Lock(); return !A->chats[A->ChatKey(idL)].messages.empty(); }, 5000), "chat through a link");
            // an invite room: D and E, invited into "Family", see each other (and L), but not A
            {
                std::string kD(64, 'd'), kE(64, 'e'), roomId(64, 'f');
                {
                    auto lk = L->Lock();
                    Settings s = L->settings;
                    WanLink r;
                    r.secret = roomId;
                    r.label = "Family";
                    s.wanInviteRooms = {r};
                    for (auto& k : {kD, kE}) {
                        WanLink w;
                        w.secret = k;
                        w.room = roomId;
                        s.wanLinks.push_back(w);
                    }
                    L->ApplySettings(s);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto D = MakeEngine("dora", 48187, true, false, false, false, srvAddr, "d-own");
                auto E = MakeEngine("emil", 48188, true, false, false, false, srvAddr, "e-own");
                std::string idD = D->settings.deviceId, idE = E->settings.deviceId;
                setLinks(*D, true, {kD}, false, "d-own");
                setLinks(*E, true, {kE}, false, "e-own");
                CHECK(WaitFor([&] { return sees(*D, idE) && sees(*E, idD) && sees(*D, idL) && sees(*L, idE); }, 8000),
                      "people in one invite room see each other");
                CHECK(!sees(*D, idA) && !sees(*A, idD) && !sees(*D, idB), "...but not people outside it");
                json ij;
                bool roomShown = false;
                if (LoadJsonFile(JoinPath(po.dataDir, "invited.json"), ij))
                    for (auto& [pub, v] : ij["devices"].items())
                        if (v.value("id", "") == idD) roomShown = v.value("room", "") == "Family" && v.value("by", "") == "lina" && v.value("byKey", "").size() == 64;
                CHECK(roomShown, "the server lists who invited them and into which room (%s)", ij.dump().c_str());
                D->Stop();
                E->Stop();
            }
            // A drops the invite and enters the server by hand, in C's room, not hidden: the server still remembers A
            setLinks(*A, false, {}, true, "pub");
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            CHECK(!sees(*A, idC) && !sees(*C, idA), "remembered as invited after entering the server by hand");
            // a fresh install with a new account on the same phone: still limited
            auto A2 = std::make_unique<Engine>();
            {
                std::string cfg2 = JoinPath(Root(), "adam2/config");
                MakeDirs(cfg2);
                Settings s2;
                s2.displayName = "adam again";
                s2.downloadDir = JoinPath(Root(), "adam2/downloads");
                s2.tcpPort = 48186;
                s2.autoConnect = true;
                s2.wanEnabled = true;
                s2.wanServer = srvAddr;
                s2.wanRoom = "pub";
                s2.wanPassword = "secret pub";
                SaveJsonFile(JoinPath(cfg2, "settings.json"), json{{"settings", ToJson(s2, false)}, {"devices", json::array()}});
                A2->configDir = cfg2;
                A2->machineId = "adams-phone";
                A2->Start();
            }
            std::string idA2 = A2->settings.deviceId;
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            CHECK(!sees(*A2, idC) && !sees(*C, idA2), "a new install on the same phone is still limited");
            A2->Stop();
            json inv;
            CHECK(LoadJsonFile(JoinPath(po.dataDir, "invited.json"), inv) && inv["devices"].size() == 5, "invited devices saved (%s)",
                  inv.dump().c_str());
            // the admin releases A: an ordinary device again
            std::string pubA;
            for (auto& [pub, v] : inv["devices"].items())
                if (v.value("id", "") == idA) pubA = pub;
            std::ofstream(JoinPath(po.dataDir, "invited-cmd")) << "release " << pubA.substr(0, 12) << "\n";
            CHECK(WaitFor([&] { return sees(*A, idC) && sees(*C, idA); }, 25000), "released by the admin");
            L->Stop();
            A->Stop();
            B->Stop();
            C->Stop();
            ps.Stop();
        }
        // relay: when no direct path works, the server carries the (encrypted) packets
#ifdef _WIN32
        _putenv_s("LUNASHARE_TEST_NO_DIRECT", "1");
#else
        setenv("LUNASHARE_TEST_NO_DIRECT", "1", 1);
#endif
        auto R1 = MakeEngine("rita", 48172, true, false, false, false, "127.0.0.1:48190", "relayroom");
        auto R2 = MakeEngine("rob", 48174, true, false, false, false, "127.0.0.1:48190", "relayroom");
        std::string idR1 = R1->settings.deviceId, idR2 = R2->settings.deviceId;
        bool relayed = WaitFor([&] {
            auto c = R1->ReadyConn(idR2);
            return c && c->transport == Transport::Internet && c->addr == "relay" && R2->IsConnected(idR1);
        }, 20000);
        CHECK(relayed, "relayed connection when punching fails (R1: %s)", R1->WanStatus().c_str());
        std::string fr = JoinPath(Root(), "src/relay.bin");
        WriteRandomFile(fr, 4 << 20, 12);
        uint64_t trl = FirstTid(*R1, R1->SendPaths(idR2, {fr}));
        CHECK(WaitFor([&] { return StateOf(*R2, trl) == TState::Completed; }, 30000), "file over the relay: %s", TStateName(StateOf(*R2, trl)));
        auto trr = GetT(*R2, trl);
        CHECK(trr && Sha256File(trr->localPath) == Sha256File(fr), "relayed transfer verified");
        R1->Stop();
        R2->Stop();
#ifdef _WIN32
        _putenv_s("LUNASHARE_TEST_NO_DIRECT", "");
#else
        unsetenv("LUNASHARE_TEST_NO_DIRECT");
#endif
        // offline messages: left in the server mailbox, kept across a server restart, acknowledged and read by mail
        printf("[offline messages (server mailbox)]\n");
        {
            RendezvousServer ms;
            RendezvousServer::Options mo;
            mo.bind = "127.0.0.1";
            mo.port = 48192;
            mo.dataDir = JoinPath(Root(), "mailserver");
            CHECK(ms.Start(mo, &err), "mailbox server");
            auto M1 = MakeEngine("mira", 48176, true, false, false, false, "127.0.0.1:48192", "mailroom");
            auto M2 = MakeEngine("milo", 48178, true, false, false, false, "127.0.0.1:48192", "mailroom");
            std::string idM1 = M1->settings.deviceId, idM2 = M2->settings.deviceId;
            std::string k1 = "@" + M1->MyAccount(), k2 = "@" + M2->MyAccount();
            CHECK(WaitFor([&] { return M1->IsConnected(idM2) && M2->IsConnected(idM1); }, 15000), "met once");
            M2->Stop();
            CHECK(WaitFor([&] { return !M1->IsConnected(idM2); }, 5000), "milo went away");
            M1->SendChat(k2, u8"zostawione na serwerze - left in the mailbox");
            CHECK(WaitFor([&] { return ms.MailCount() == 1; }, 25000), "sealed message waits on the server (%d)", (int)ms.MailCount());
            M1->Stop();
            ms.Stop();
            CHECK(ms.Start(mo, &err), "server restarted");
            CHECK(WaitFor([&] { return ms.MailCount() == 1; }, 7000), "mailbox kept on disk (%d)", (int)ms.MailCount());
            M2 = MakeEngine("milo", 48178, true, false, false, false, "127.0.0.1:48192", "mailroom");
            auto got = [&](Engine& e, const std::string& key, const std::string& text, bool out) -> ChatMessage* {
                auto it = e.chats.find(key);
                if (it == e.chats.end()) return nullptr;
                for (auto& m : it->second.messages)
                    if (m.text == text && m.outgoing == out) return &m;
                return nullptr;
            };
            CHECK(WaitFor([&] { auto l = M2->Lock(); return got(*M2, k1, u8"zostawione na serwerze - left in the mailbox", false) != nullptr; }, 10000),
                  "delivered from the mailbox while the sender is offline");
            M2->MarkRead(k1);
            CHECK(WaitFor([&] { return ms.MailCount() >= 2; }, 25000), "delivery + read receipts wait for the sender (%d)", (int)ms.MailCount());
            M2->Stop();
            M1 = MakeEngine("mira", 48176, true, false, false, false, "127.0.0.1:48192", "mailroom");
            CHECK(WaitFor([&] {
                auto l = M1->Lock();
                ChatMessage* m = got(*M1, k2, u8"zostawione na serwerze - left in the mailbox", true);
                return m && m->delivered && m->read;
            }, 15000), "sender sees delivered + read");
            CHECK(WaitFor([&] { return ms.MailCount() == 0; }, 10000), "mailboxes emptied (%d)", (int)ms.MailCount());
            M1->Stop();
            ms.Stop();
        }

        // group calls through the server: one upload per person, the server passes it on (sealed with the call key)
        printf("[group calls: through the server]\n");
        {
            RendezvousServer hs;
            RendezvousServer::Options ho;
            ho.bind = "127.0.0.1";
            ho.port = 48193;
            CHECK(hs.Start(ho, &err), "hub server");
            auto X = MakeEngine("xena", 48171, true, false, false, false, "127.0.0.1:48193", "hubroom");
            auto Y = MakeEngine("yuri", 48173, true, false, false, false, "127.0.0.1:48193", "hubroom");
            auto Z = MakeEngine("zoe", 48175, true, false, false, false, "127.0.0.1:48193", "hubroom");
            std::string idX = X->settings.deviceId;
            CHECK(WaitFor([&] { return X->IsConnected(Y->settings.deviceId) && X->IsConnected(Z->settings.deviceId); }, 15000), "met");
            for (Engine* e : {X.get(), Y.get(), Z.get()}) {
                auto l = e->Lock();
                e->settings.groupsViaServer = true;
                e->localCaps = {"voice", "video"};
            }
            CHECK(WaitFor([&] { return X->WanStatus().rfind("connected", 0) == 0; }, 5000), "registered");
            std::string hk = X->CreateGroup("hub", {"@" + Y->MyAccount(), "@" + Z->MyAccount()});
            CHECK(WaitFor([&] { auto l = Y->Lock(); auto l2 = Z->Lock(); return Y->GroupOf(hk) && Z->GroupOf(hk); }, 3000), "group");
            std::atomic<int> audioY{0}, audioZ{0}, bigY{0};
            Y->onMedia = [&](uint64_t, int, const uint8_t* d, size_t n) {
                if (n && d[0] == 1) audioY++;
                if (n == 50000 && d[0] == 2 && d[1] == 1 && d[49999] == 0x5A) bigY++;
            };
            Z->onMedia = [&](uint64_t, int, const uint8_t* d, size_t n) { if (n && d[0] == 1) audioZ++; };
            uint64_t cid = X->StartCall(hk, "video");
            {
                auto l = X->Lock();
                CHECK(cid && X->call.hub && X->call.key.size() == 32, "the call goes through the server");
            }
            auto ringing = [&](Engine& e) { auto l = e.Lock(); return e.call.state == CallState::IncomingRinging && e.call.hub; };
            CHECK(WaitFor([&] { return ringing(*Y) && ringing(*Z); }, 3000), "both are called");
            Y->AcceptCall();
            Z->AcceptCall();
            auto peersOf = [&](Engine& e) { auto l = e.Lock(); return e.call.state == CallState::Active ? (int)e.call.peers.size() : -1; };
            CHECK(WaitFor([&] { return peersOf(*X) == 2 && peersOf(*Y) == 2 && peersOf(*Z) == 2; }, 8000), "all three in the hub (%d %d %d)",
                  peersOf(*X), peersOf(*Y), peersOf(*Z));
            const uint8_t voice[10] = {1, 0, 0, 0, 0, 0, 0, 9, 9, 9};
            for (int i = 0; i < 10; i++) {
                X->SendMedia(voice, sizeof(voice));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            CHECK(WaitFor([&] { return audioY >= 9 && audioZ >= 9; }, 3000), "voice through the server (%d %d)", (int)audioY, (int)audioZ);
            const uint8_t camOn[3] = {2, 3, 1};
            X->SendMedia(camOn, sizeof(camOn));
            CHECK(WaitFor([&] { auto l = Y->Lock(); return Y->call.spotlight == idX; }, 4000), "X's picture is chosen");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));  // (the request to send it reaches X)
            std::vector<uint8_t> big(50000, 0x5A);
            big[0] = 2;
            big[1] = 1;
            CHECK(WaitFor([&] {
                X->SendMedia(big.data(), big.size());
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return bigY >= 3;
            }, 4000), "a 50 KB picture arrives in pieces and whole (%d)", (int)bigY);
            Z->EndCall();
            CHECK(WaitFor([&] { return peersOf(*X) == 1 && peersOf(*Y) == 1; }, 5000), "someone leaves");
            Y->EndCall();
            X->EndCall();
            Y->onMedia = nullptr;
            Z->onMedia = nullptr;
            Z->Stop();
            Y->Stop();
            X->Stop();
            hs.Stop();
        }

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
        P->onMedia = [&](uint64_t, int, const uint8_t* d, size_t n) { if (n == 3 && d[0] == 'q') gotP++; };
        Q->onMedia = [&](uint64_t, int, const uint8_t* d, size_t n) { if (n == 3 && d[0] == 'p') gotQ++; };
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
            auto& msgs = Q->chats[Q->ChatKey(idP)].messages;
            CHECK(!msgs.empty() && msgs.back().text.find("Incoming call - 0:0") == 0, "call logged: %s", msgs.empty() ? "" : msgs.back().text.c_str());
        }
        // declined
        cid = P->StartCall(idQ);
        CHECK(WaitFor([&] { auto l = Q->Lock(); return Q->call.state == CallState::IncomingRinging; }, 3000), "ringing again");
        Q->EndCall();
        CHECK(WaitFor([&] { auto l = P->Lock(); return P->call.state == CallState::None; }, 3000), "declined");
        {
            auto l = P->Lock();
            CHECK(P->chats[P->ChatKey(idQ)].messages.back().text == "Call declined", "declined logged: %s", P->chats[P->ChatKey(idQ)].messages.back().text.c_str());
        }
        // a person with two devices: both ring, the first to answer takes the call, the other stops
        {
            auto Q2 = MakeEngine("quinn-tablet", 48225, false, false, false, false, "", "", {"voice"});
            std::string idQ2 = Q2->settings.deviceId;
            Q2->ConnectAddress("127.0.0.1", Q->boundPort);
            CHECK(WaitFor([&] { return Q->IsConnected(idQ2); }, 5000), "second device connected");
            CHECK(Q->LinkDevice(idQ2), "link");
            CHECK(WaitFor([&] { for (auto& e : Q2->PopEvents()) if (e.type == EventType::LinkRequest) return true; return false; }, 3000), "link asked");
            Q2->AnswerLink(idQ, true);
            CHECK(WaitFor([&] { return Q2->MyAccount() == Q->MyAccount(); }, 3000), "linked");
            Q2->ConnectAddress("127.0.0.1", P->boundPort);
            CHECK(WaitFor([&] { auto l = P->Lock(); return P->IsConnected(idQ2) && P->ChatKey(idQ2) == P->ChatKey(idQ) && P->IsConnected(idQ); }, 8000),
                  "caller sees one person with two devices");
            std::string person = "@" + Q->MyAccount();
            std::atomic<int> rings{0};
            Q2->onEvent = [&](const Event& e) { if (e.type == EventType::CallIncoming) rings++; };
            P->StartCall(person);
            CHECK(WaitFor([&] {
                auto l1 = Q->Lock();
                auto l2 = Q2->Lock();
                return Q->call.state == CallState::IncomingRinging && Q2->call.state == CallState::IncomingRinging;
            }, 3000), "both devices ring");
            CHECK(rings == 1, "event hook sees the call (%d)", rings.load());
            Q2->AcceptCall();
            CHECK(WaitFor([&] { auto l = P->Lock(); return P->call.state == CallState::Active && P->call.peer == idQ2; }, 3000), "answered on the tablet");
            CHECK(WaitFor([&] { auto l = Q->Lock(); return Q->call.state == CallState::None; }, 3000), "the phone stops ringing");
            P->EndCall();
            CHECK(WaitFor([&] { auto l = Q2->Lock(); return Q2->call.state == CallState::None; }, 3000), "hung up");
            // a decline on one device ends it everywhere
            P->StartCall(person);
            CHECK(WaitFor([&] { auto l1 = Q->Lock(); auto l2 = Q2->Lock(); return Q->call.state == CallState::IncomingRinging && Q2->call.state == CallState::IncomingRinging; }, 3000), "ringing both");
            Q->EndCall();
            CHECK(WaitFor([&] { auto l1 = P->Lock(); auto l2 = Q2->Lock(); return P->call.state == CallState::None && Q2->call.state == CallState::None; }, 3000), "declined everywhere");
            Q2->onEvent = nullptr;
            Q2->Stop();
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
