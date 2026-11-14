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
#include <deque>
#include <mutex>

#include "crypto.h"
#include "engine.h"
#include "mdns.h"
#include "rendezvous_server.h"
#include "wan.h"
#include "upnp.h"
#include "hash.h"
#include "link_crypto.h"
#include "link_server.h"
#include "platform.h"
#include "rtmedia.h"

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
    FILE* f = OpenFile(path, "wb");  // (UTF-8 names on Windows too)
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

// the chat history encrypted with the app-lock PIN: saved while locked, merged once opened
static void TestHistoryCrypt() {
    printf("[history encrypted with the PIN]\n");
    std::string cfg = JoinPath(Root(), "histcrypt/config");
    auto add = [](Engine& e, const std::string& mid, const std::string& text) {
        auto lk = e.Lock();
        ChatMessage m;
        m.mid = mid;
        m.text = text;
        m.ts = NowMs();
        e.chats["@x"].peer = "@x";
        e.chats["@x"].messages.push_back(m);
    };
    auto texts = [](Engine& e) {
        auto lk = e.Lock();
        std::string r;
        for (auto& m : e.chats["@x"].messages) r += m.text + ";";
        return r;
    };
    auto has = [&](const std::string& f) { return FileSize(JoinPath(cfg, f)) >= 0; };
    auto fileHas = [&](const std::string& f, const std::string& what) {
        std::string b;
        return LoadFileBytes(JoinPath(cfg, f), b) && b.find(what) != std::string::npos;
    };
    auto E = MakeEngine("histcrypt", 48331, false, false, false, false);
    add(*E, "m1", "secret one");
    E->SetHistoryEncryption("1234");
    E->Stop();
    CHECK(!has("history.json") && has("history.enc") && !fileHas("history.enc", "secret one"), "encrypted on disk, plain file gone");
    E = MakeEngine("histcrypt", 48331, false, false, false, false);
    CHECK(E->HistoryLocked() && texts(*E).empty(), "locked after a start");
    add(*E, "m2", "secret two");
    E->Stop();
    CHECK(has("history-new.enc") && !fileHas("history-new.enc", "secret two"), "while locked: saved apart, encrypted");
    E = MakeEngine("histcrypt", 48331, false, false, false, false);
    add(*E, "m3", "secret three");
    CHECK(!E->UnlockHistory("0000") && E->HistoryLocked(), "a wrong PIN doesn't open it");
    CHECK(E->UnlockHistory("1234") && !E->HistoryLocked(), "the PIN opens it");
    CHECK(texts(*E) == "secret one;secret two;secret three;", "merged in order (%s)", texts(*E).c_str());
    E->SetLock("h", "s", 4, "5678");  // a new PIN: the key is wrapped again
    E->Stop();
    CHECK(!has("history-new.enc"), "merged files gone");
    E = MakeEngine("histcrypt", 48331, false, false, false, false);
    CHECK(!E->UnlockHistory("1234") && E->UnlockHistory("5678") && texts(*E) == "secret one;secret two;secret three;", "the new PIN opens it");
    E->SetHistoryEncryption("");
    E->Stop();
    CHECK(has("history.json") && !has("history.enc") && fileHas("history.json", "secret one"), "back to a plain file");
    E = MakeEngine("histcrypt", 48331, false, false, false, false);
    CHECK(!E->HistoryLocked() && texts(*E).size() > 10, "plain again after a start");
    E->SetHistoryEncryption("1111");
    E->Stop();
    E = MakeEngine("histcrypt", 48331, false, false, false, false);
    add(*E, "m4", "after");
    E->SetLock("", "", 0);  // the PIN forgotten (the recovery phrase): the old history is lost, the lock and the encryption off
    CHECK(!E->HistoryLocked() && !E->HistoryEncrypted() && texts(*E) == "after;", "forgotten PIN: a fresh history");
    E->Stop();
    CHECK(!has("history.enc") && has("history.json"), "forgotten PIN: plain file");
}

struct HttpRes {
    int status = 0;
    std::string head, body;
};
static HttpRes HttpGet(uint16_t port, const std::string& path, const std::string& extra = "") {
    HttpRes r;
    SocketHandle s = TcpConnect("127.0.0.1", port, 3000);
    if (s == kInvalidSocket) return r;
    std::string q = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n" + extra + "\r\n";
    SendAll(s, q.data(), q.size());
    std::string all;
    char b[65536];
    while (WaitReadable(s, 15000) == 1) {
        int n = RecvSome(s, b, sizeof(b));
        if (n <= 0) break;
        all.append(b, (size_t)n);
    }
    CloseSocket(s);
    size_t e = all.find("\r\n\r\n");
    if (e == std::string::npos) return r;
    r.head = all.substr(0, e);
    r.body = all.substr(e + 4);
    sscanf(r.head.c_str(), "HTTP/1.1 %d", &r.status);
    return r;
}
static std::string ReadAll(const std::string& p) {
    std::string out;
    FILE* f = OpenFile(p, "rb");
    if (!f) return out;
    char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n);
    fclose(f);
    return out;
}

static void TestShareLinks() {
    printf("[links for anyone]\n");
    std::string err;
    // ---- on this network
    std::string dir = JoinPath(Root(), "links");
    MakeDirs(dir);
    std::string f1 = JoinPath(dir, u8"zdjęcie 1.bin");
    WriteRandomFile(f1, 3 * 1024 * 1024 + 123, 77);
    ShareLinks sl;
    std::vector<ShareAsk> fired;
    std::mutex fm;
    sl.onAsk = [&](const ShareAsk& a) { std::lock_guard<std::mutex> l(fm); fired.push_back(a); };
    sl.Start(dir, 0);
    uint16_t lp = sl.LanPort();
    CHECK(lp != 0, "LAN link port");
    ShareLinks::Options o;
    std::string id = sl.Create(f1, o, err);
    CHECK(!id.empty(), "link created: %s", err.c_str());
    CHECK(WaitFor([&] { auto l = sl.List(); return !l.empty() && l[0].ready; }, 10000), "hashed");
    if (sl.List().empty()) return;
    auto info = sl.List()[0];
    CHECK(info.sha256 == Sha256File(f1), "its SHA-256");
    CHECK(info.webUrl.empty(), "no server: no Internet link");
    std::string tok;
    {
        // the LAN address may be missing on a test machine without a private address: take the token from the state
        json st;
        LoadJsonFile(JoinPath(dir, "share_links.json"), st);
        tok = st["links"][0].value("tok", "");
    }
    std::string base = "/f/" + tok;
    auto page = HttpGet(lp, base);
    CHECK(page.status == 200 && page.body.find("LunaShare") != std::string::npos && page.head.find("Content-Security-Policy") != std::string::npos, "the page (%d)", page.status);
    CHECK(page.body.find(u8"zdjęcie 1.bin") != std::string::npos, "with the file's name");
    CHECK(HttpGet(lp, "/f/nope").status == 404, "an unknown link");
    auto st1 = HttpGet(lp, base + "/start?sid=browserone1");
    CHECK(st1.status == 200 && st1.body.find("\"ok\"") != std::string::npos, "started (%d %s)", st1.status, st1.body.c_str());
    auto fileR = HttpGet(lp, base + "/file?sid=browserone1");
    CHECK(fileR.status == 200 && fileR.body == ReadAll(f1), "the file (%d, %d bytes)", fileR.status, (int)fileR.body.size());
    CHECK(fileR.head.find("filename*=UTF-8''zdj%C4%99cie%201.bin") != std::string::npos, "its name for saving");
    auto part = HttpGet(lp, base + "/file?sid=browserone1", "Range: bytes=100-199\r\n");
    CHECK(part.status == 206 && part.body == ReadAll(f1).substr(100, 100), "resumes (range %d)", part.status);
    CHECK(HttpGet(lp, base + "/file?sid=otherbrowser").status == 403, "the file only after the checks");
    auto st2 = HttpGet(lp, base + "/start?sid=otherbrowser");
    CHECK(st2.status == 410 && st2.body.find("used") != std::string::npos, "one person: the next one is told it's used (%s)", st2.body.c_str());
    CHECK(WaitFor([&] { auto l = sl.List(); return !l.empty() && l[0].visits.size() == 1 && l[0].visits[0].done && l[0].people == 1; }, 3000), "who got it is listed");

    // PIN: 5 wrong tries end the link
    o.pin = "4321";
    o.maxPeople = 0;
    std::string idp = sl.Create(f1, o, err);
    CHECK(WaitFor([&] { for (auto& l : sl.List()) if (l.id == idp) return l.ready; return false; }, 10000), "PIN link ready");
    json st;
    LoadJsonFile(JoinPath(dir, "share_links.json"), st);
    std::string tokp;
    for (auto& l : st["links"]) if (l.value("id", "") == idp) tokp = l.value("tok", "");
    auto np = HttpGet(lp, "/f/" + tokp + "/start?sid=pinbrowser1");
    CHECK(np.status == 401 && np.body.find("\"pin\"") != std::string::npos, "asks for the PIN");
    auto wp = HttpGet(lp, "/f/" + tokp + "/start?sid=pinbrowser1&pin=1111");
    CHECK(wp.status == 401 && wp.body.find("\"left\":4") != std::string::npos, "wrong PIN: 4 tries left (%s)", wp.body.c_str());
    auto gp = HttpGet(lp, "/f/" + tokp + "/start?sid=pinbrowser1&pin=4321");
    CHECK(gp.status == 200, "the right PIN");
    for (int i = 0; i < 4; i++) HttpGet(lp, "/f/" + tokp + "/start?sid=guesser0" + std::to_string(i) + "&pin=000" + std::to_string(i));
    CHECK(HttpGet(lp, "/f/" + tokp + "/start?sid=guesser99&pin=4321").status == 410, "5 wrong PINs ended the link");
    CHECK(HttpGet(lp, "/f/" + tokp + "/file?sid=pinbrowser1").status == 410, "for everyone");

    // ask me first
    ShareLinks::Options oa;
    oa.ask = true;
    std::string ida = sl.Create(f1, oa, err);
    CHECK(WaitFor([&] { for (auto& l : sl.List()) if (l.id == ida) return l.ready; return false; }, 10000), "ask link ready");
    LoadJsonFile(JoinPath(dir, "share_links.json"), st);
    std::string toka;
    for (auto& l : st["links"]) if (l.value("id", "") == ida) toka = l.value("tok", "");
    auto w1 = HttpGet(lp, "/f/" + toka + "/start?sid=askbrowser1");
    CHECK(w1.status == 202 && w1.body.find("wait") != std::string::npos, "waits for me");
    CHECK(sl.Asks().size() == 1 && fired.size() == 1, "I'm asked");
    sl.Answer(sl.Asks()[0].id, true);
    CHECK(HttpGet(lp, "/f/" + toka + "/start?sid=askbrowser1").status == 200, "allowed");
    sl.Revoke(ida);
    CHECK(HttpGet(lp, "/f/" + toka + "/start?sid=askbrowser1").status == 410, "turned off");

    // kept across a restart
    sl.Stop();
    ShareLinks sl2;
    sl2.Start(dir, lp);
    CHECK(sl2.List().size() == 3, "links kept (%d)", (int)sl2.List().size());
    CHECK(HttpGet(sl2.LanPort(), base + "/file?sid=browserone1", "Range: bytes=0-9\r\n").status == 206, "the person who got it may resume after a restart");
    sl2.Stop();

    // ---- over the Internet: page on the server, file from the device through its tunnel, end-to-end encrypted
    RendezvousServer rs;
    RendezvousServer::Options ro;
    ro.bind = "127.0.0.1";
    ro.port = 48194;
    ro.linkPort = 48195;
    CHECK(rs.Start(ro, &err), "server");
    LinkServer lsv;
    LinkServer::Options lo;
    lo.bind = "127.0.0.1";
    lo.httpPort = 48195;
    lo.tunnelPort = 48194;
    lo.allowed = [&](const std::string& p) { return rs.LinkAllowed(p); };
    CHECK(lsv.Start(lo, &err), "link server: %s", err.c_str());
    auto W = MakeEngine("wanda", 48180, true, false, false, false, "127.0.0.1:48194", "linkroom");
    std::string f2 = JoinPath(dir, "film.bin");
    WriteRandomFile(f2, 2 * 1024 * 1024 + 5, 78);
    ShareLinks::Options ow;
    ow.maxPeople = 2;
    std::string idw = W->shareLinks.Create(f2, ow, err);
    CHECK(WaitFor([&] { auto l = W->shareLinks.List(); return !l.empty() && l[0].ready && !l[0].webUrl.empty(); }, 15000), "an Internet link once the server offers them");
    CHECK(WaitFor([&] { return W->shareLinks.TunnelStatus() == "online"; }, 10000), "tunnel (%s)", W->shareLinks.TunnelStatus().c_str());
    if (W->shareLinks.List().empty()) return;
    std::string url = W->shareLinks.List()[0].webUrl;
    CHECK(url.rfind("http://127.0.0.1:48195/s/" + idw + "#", 0) == 0 && url.size() < 70, "a short link: %s", url.c_str());
    std::string secret;
    FromB64Url(url.substr(url.find('#') + 1), secret);
    LinkKeys k = DeriveLinkKeys((const uint8_t*)secret.data(), secret.size());
    auto wpage = HttpGet(48195, "/s/" + idw);
    CHECK(wpage.status == 200 && wpage.body.find("\"mode\":\"web\"") != std::string::npos && wpage.body.find("film.bin") == std::string::npos,
          "the server's page doesn't know the file");
    auto meta = HttpGet(48195, "/api/" + idw + "/meta?sid=webbrowser1");
    std::string plain;
    CHECK(meta.status == 200 && LinkOpen(k, kLinkMetaIndex, true, meta.body, plain), "sealed details (%d)", meta.status);
    json mj = json::parse(plain, nullptr, false);
    CHECK(mj.is_object() && mj.value("n", "") == "film.bin" && mj.value("s", (int64_t)0) == FileSize(f2) && mj.value("h", "") == Sha256File(f2), "name, size, SHA-256");
    std::string got;
    int64_t parts = (FileSize(f2) + kLinkChunk - 1) / kLinkChunk;
    bool allOk = true;
    for (int64_t i = 0; i < parts; i++) {
        auto pr = HttpGet(48195, "/api/" + idw + "/p/" + std::to_string(i) + "?sid=webbrowser1");
        std::string pp;
        bool ok = pr.status == 200 && LinkOpen(k, (uint64_t)i, i == parts - 1, pr.body, pp);
        allOk &= ok;
        if (i == 0) {
            std::string bad = pr.body;
            bad[10] ^= 1;
            std::string x;
            CHECK(!LinkOpen(k, 0, false, bad, x), "a changed part is caught");
            CHECK(!LinkOpen(k, 1, false, pr.body, x), "a part in the wrong place is caught");
            CHECK(!LinkOpen(k, 0, true, pr.body, x), "a cut-short file is caught");
        }
        got += pp;
    }
    CHECK(allOk && got == ReadAll(f2), "the file through the server (%d bytes)", (int)got.size());
    CHECK(HttpGet(48195, "/api/" + idw + "/p/0?sid=strangerxx").status == 403, "parts only for whoever passed the checks");
    CHECK(HttpGet(48195, "/api/AAAAAAAAAAAA/meta?sid=webbrowser1").status == 503, "an unknown link: offline");
    CHECK(HttpGet(48195, "/sw.js").body.find("ReadableStream") != std::string::npos, "service worker");
    CHECK(WaitFor([&] { auto l = W->shareLinks.List(); return !l.empty() && !l[0].visits.empty() && l[0].visits[0].done && l[0].visits[0].via == "web"; }, 3000), "listed as got through the server");
    W->shareLinks.Revoke(idw);
    auto gone = HttpGet(48195, "/api/" + idw + "/meta?sid=webbrowser1");
    CHECK(gone.status == 410 || gone.status == 503, "turned off (%d)", gone.status);
    W->Stop();
    CHECK(WaitFor([&] { return lsv.TunnelCount() == 0; }, 5000), "tunnel closed");
    lsv.Stop();
    rs.Stop();
}

// for browser tests (tools/test_links.py): a server and a device sharing a file, until the file `stop` appears
static int ServeLinks(const char* file) {
    std::string err;
    RendezvousServer rs;
    RendezvousServer::Options ro;
    ro.bind = "127.0.0.1";
    ro.port = 48294;
    ro.linkPort = 48295;
    ro.linkTls = getenv("LINK_CERT") != nullptr;
    if (!rs.Start(ro, &err)) return printf("server: %s\n", err.c_str()), 1;
    LinkServer lsv;
    LinkServer::Options lo;
    lo.bind = "127.0.0.1";
    lo.httpPort = 48295;
    lo.tunnelPort = 48294;
    if (getenv("LINK_CERT")) lo.certFile = getenv("LINK_CERT"), lo.keyFile = getenv("LINK_KEY");
    lo.allowed = [&](const std::string& p) { return rs.LinkAllowed(p); };
    if (!lsv.Start(lo, &err)) return printf("links: %s\n", err.c_str()), 1;
    auto W = MakeEngine("serve", 48280, true, false, false, false, "127.0.0.1:48294", "serveroom");
    ShareLinks::Options o;
    o.maxPeople = 0;
    std::string plain = W->shareLinks.Create(file, o, err);
    o.pin = "2468";
    std::string pin = W->shareLinks.Create(file, o, err);
    o.pin = "";
    o.ask = true;
    std::string ask = W->shareLinks.Create(file, o, err);
    WaitFor([&] { for (auto& l : W->shareLinks.List()) if (!l.ready || l.webUrl.empty()) return false; return W->shareLinks.TunnelStatus() == "online"; }, 60000);
    for (auto& l : W->shareLinks.List())
        printf("%s %s %s\n", l.id == plain ? "plain" : l.id == pin ? "pin" : "ask", l.webUrl.c_str(), l.lanUrl.c_str());
    json st;
    LoadJsonFile(JoinPath(W->ConfigPath(), "share_links.json"), st);
    for (auto& l : st["links"]) printf("tok %s %s\n", l.value("id", "").c_str(), l.value("tok", "").c_str());
    printf("lanport %d\nready\n", W->shareLinks.LanPort());
    std::string stop = JoinPath(DirName(file), "stop");
    while (!FileExists(stop)) {
        for (auto& a : W->shareLinks.Asks()) {
            printf("asked %s %s\n", a.name.c_str(), a.via.c_str());
            W->shareLinks.Answer(a.id, true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    W->Stop();
    lsv.Stop();
    rs.Stop();
    return 0;
}

// ---- real-time call media over a simulated network: loss, delay, a slow link with a deep buffer
struct SimLink {
    std::mutex m;
    struct P {
        double at;
        std::string d;
    };
    std::deque<P> q;
    int delayMs = 40, lossPct = 0, kbps = 0;  // kbps 0 = unlimited
    double busyUntil = 0;
    size_t bufBytes = 0, bufLimit = 1u << 20;
    std::mt19937 rng{7};
    std::function<void(const uint8_t*, size_t)> to;
    std::atomic<bool> run{true};
    std::thread th;
    static double NowF() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void Start() {
        th = std::thread([this] {
            while (run) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                std::vector<std::string> due;
                {
                    std::lock_guard<std::mutex> l(m);
                    double now = NowF();
                    while (!q.empty() && q.front().at <= now) {
                        bufBytes -= q.front().d.size();
                        due.push_back(std::move(q.front().d));
                        q.pop_front();
                    }
                }
                for (auto& d : due)
                    if (to) to((const uint8_t*)d.data(), d.size());
            }
        });
    }
    void Stop() {
        run = false;
        if (th.joinable()) th.join();
    }
    bool Put(const uint8_t* d, size_t n) {
        std::lock_guard<std::mutex> l(m);
        if (lossPct && (int)(rng() % 100) < lossPct) return true;
        if (bufBytes + n > bufLimit) return true;  // (the router's buffer is full: dropped)
        double now = NowF();
        double start = std::max(now, busyUntil);
        double tx = kbps ? (double)n * 8.0 / kbps : 0.0;  // ms on the slow link
        busyUntil = start + tx;
        q.push_back({busyUntil + delayMs, std::string((const char*)d, n)});
        bufBytes += n;
        return true;
    }
};

static void TestRtMedia() {
    printf("[real-time call media]\n");
    uint8_t key[32];
    SecureRandom(key, 32);
    uint64_t callId = 0x1122334455667788ull;
    SimLink ab, ba;
    std::atomic<int> voiceB{0}, framesB{0}, outOfOrder{0};
    std::atomic<uint32_t> lastFrame{0};
    auto A = std::make_unique<RtMedia>(key, callId, [&](const uint8_t* d, size_t n) { return ab.Put(d, n); },
                                       [&](const uint8_t*, size_t) {});
    auto B = std::make_unique<RtMedia>(
        key, callId, [&](const uint8_t* d, size_t n) { return ba.Put(d, n); },
        [&](const uint8_t* d, size_t n) {
            if (n >= 6 && d[0] == 2 && d[1] == 1) {
                uint32_t f = d[2] | (uint32_t)d[3] << 8 | (uint32_t)d[4] << 16 | (uint32_t)d[5] << 24;
                if (f <= lastFrame) outOfOrder++;
                lastFrame = f;
                framesB++;
            } else if (n >= 1 && d[0] == 1) {
                voiceB++;
            }
        });
    ab.to = [&](const uint8_t* d, size_t n) { B->Input(d, n); };
    ba.to = [&](const uint8_t* d, size_t n) { A->Input(d, n); };
    ab.Start();
    ba.Start();
    CHECK(A->LinkBps() == RtMedia::kStartBps, "starts low (%d)", A->LinkBps());
    CHECK(WaitFor([&] { return A->Ready() && B->Ready(); }, 2000), "both sides hear each other");
    uint32_t frameNo = 0;
    auto sendFrame = [&](size_t size) {
        std::vector<uint8_t> f(std::max<size_t>(size, 16), 0x55);
        f[0] = 2;
        f[1] = 1;
        frameNo++;
        for (int i = 0; i < 4; i++) f[2 + i] = (uint8_t)(frameNo >> (8 * i));
        return A->Send(f.data(), f.size());
    };
    auto follow = [&] { return (size_t)std::max(500.0, A->LinkBps() * 0.8 / 8 / 30); };  // an encoder following the estimate
    auto sendVoice = [&] {
        uint8_t v[60] = {1};
        A->Send(v, sizeof(v));
    };
    // clean network
    int sentF = 0;
    for (int i = 0; i < 30; i++) {
        sendVoice();
        sentF += sendFrame(follow()) ? 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    CHECK(sentF == 30, "nothing refused at the start (%d of 30)", sentF);
    CHECK(WaitFor([&] { return framesB == 30 && voiceB == 30; }, 2000), "clean: every frame and voice packet (%d / %d)", framesB.load(), voiceB.load());
    // 5% loss both ways, 40 ms each way: lost pieces are asked for again, frames still come complete and in order
    // (the delay stays the same from here on: a jump would look like a queue building up for a few seconds)
    ab.lossPct = ba.lossPct = 5;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int f0 = framesB, v0 = voiceB;
    int refusedL = 0;
    for (int i = 0; i < 150; i++) {  // 15 fps of frames twice the size: a few pieces each
        sendVoice();
        if (!sendFrame(follow() * 2)) refusedL++;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        sendVoice();
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    RtStats sa = A->Stats(), sb = B->Stats();
    int gotF = framesB - f0, gotV = voiceB - v0;
    printf("    5%% loss: %d/150 frames (%d refused), %d/300 voice, %d resent, %d late, rtt %d ms, loss in %.1f%%\n", gotF, refusedL, gotV,
           sa.resent, sb.lateFrames, sa.rttMs, sb.lossInPct10 / 10.0);
    CHECK(gotF >= (150 - refusedL) * 95 / 100 && refusedL < 15, "frames completed despite loss (%d of %d sent)", gotF, 150 - refusedL);
    CHECK(gotV >= 265 && gotV <= 300, "voice: lost ones are simply missing (%d of 300)", gotV);
    CHECK(sa.resent > 0, "lost pieces were sent again (%d)", sa.resent);
    CHECK(outOfOrder == 0, "frames in order (%d out of order)", outOfOrder.load());
    CHECK(sa.rttMs >= 70 && sa.rttMs <= 200, "round trip measured (%d ms)", sa.rttMs);
    CHECK(sb.lossInPct10 > 15 && sb.lossInPct10 < 120, "loss measured (%.1f%%)", sb.lossInPct10 / 10.0);
    // a bad mobile connection: 15% loss - voice keeps going, most frames still complete
    ab.lossPct = ba.lossPct = 15;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    f0 = framesB;
    v0 = voiceB;
    int sentH = 0;
    for (int i = 0; i < 100; i++) {
        sendVoice();
        if (sendFrame(follow() * 2)) sentH++;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        sendVoice();
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    printf("    15%% loss: %d/%d frames, %d/200 voice, estimate %d kbit/s\n", framesB - f0, sentH, voiceB - v0, A->LinkBps() / 1000);
    CHECK(framesB - f0 >= sentH * 80 / 100, "most frames still complete at 15%% loss (%d of %d)", framesB - f0, sentH);
    CHECK(voiceB - v0 >= 150, "voice keeps going (%d of 200)", voiceB - v0);
    CHECK(A->Ready() && B->Ready(), "path still confirmed at 15%% loss");
    CHECK(A->LinkBps() >= 200000, "random loss without a queue doesn't crush the bitrate (%d kbit/s)", A->LinkBps() / 1000);
    // a slow link (600 kbit/s) with a deep buffer: the estimate finds it and the delay stays low
    ab.lossPct = ba.lossPct = 0;
    ab.kbps = 600;
    int sent = 0, refused = 0;
    int64_t until = SteadyMs() + 14000;
    int fLate0 = 0;
    while (SteadyMs() < until) {
        sendVoice();
        if (sendFrame(follow())) sent++;
        else refused++;
        if (until - SteadyMs() < 3000 && !fLate0) fLate0 = framesB;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    sa = A->Stats();
    int lastFew = framesB - fLate0;
    printf("    600 kbit/s link: estimate %d kbit/s, their queue %d ms, %d frames in the last 3 s, %d refused\n", sa.estimateKbps,
           sa.queueDelayMs, lastFew, refused);
    CHECK(sa.estimateKbps >= 300 && sa.estimateKbps <= 900, "estimate near the link, at most 1.5x what gets through (%d kbit/s)", sa.estimateKbps);
    CHECK(sa.queueDelayMs < 150, "no standing queue (%d ms)", sa.queueDelayMs);
    CHECK(lastFew > 60, "video keeps flowing (%d frames in 3 s)", lastFew);
    // the link opens up: the estimate climbs again
    ab.kbps = 0;
    ab.delayMs = ba.delayMs = 10;
    int startKbps = A->LinkBps() / 1000;
    until = SteadyMs() + 9000;
    while (SteadyMs() < until) {
        sendVoice();
        sendFrame(follow());
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    CHECK(A->LinkBps() / 1000 > startKbps * 2, "estimate climbs when the link opens (%d -> %d kbit/s)", startKbps, A->LinkBps() / 1000);
    // other calls' and garbage datagrams are ignored
    int fb = framesB;
    std::string junk(200, 'x');
    B->Input((const uint8_t*)junk.data(), junk.size());
    uint8_t key2[32];
    SecureRandom(key2, 32);
    {
        RtMedia other(key2, callId + 1, [&](const uint8_t* d, size_t n) { B->Input(d, n); return true; }, [](const uint8_t*, size_t) {});
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    CHECK(framesB == fb && B->Ready(), "foreign datagrams ignored");
    ab.Stop();
    ba.Stop();
    A.reset();
    B.reset();
}

// A call between two connected engines with real-time media wanted on X (onX) and Y (onY): media goes both ways,
// over the real-time channel when both want it (and it gets through), over the connection otherwise.
static void RtCall(Engine& X, Engine& Y, const std::string& idY, bool onX, bool onY, bool expectRt, const char* what) {
    for (auto* e : {&X, &Y}) {
        auto lk = e->Lock();
        Settings s = e->settings;
        s.callRealtime = e == &X ? onX : onY;
        e->ApplySettings(s);
    }
    std::atomic<int> voiceX{0}, voiceY{0}, videoY{0};
    X.onMedia = [&](uint64_t, int, const uint8_t* d, size_t n) { if (n == 60 && d[0] == 1) voiceX++; };
    Y.onMedia = [&](uint64_t, int, const uint8_t* d, size_t n) {
        if (n == 60 && d[0] == 1) voiceY++;
        if (n == 1500 && d[0] == 2 && d[1] == 1) videoY++;
    };
    uint64_t cid = X.StartCall(idY);
    CHECK(cid != 0, "%s: call started", what);
    CHECK(WaitFor([&] { auto l = Y.Lock(); return Y.call.state == CallState::IncomingRinging && Y.call.id == cid; }, 5000), "%s: ringing", what);
    Y.AcceptCall();
    CHECK(WaitFor([&] { auto l = X.Lock(); return X.call.state == CallState::Active; }, 5000), "%s: active", what);
    RtStats st;
    if (expectRt) CHECK(WaitFor([&] { return X.CallRtStats(st) && st.ready && Y.CallRtStats(st) && st.ready; }, 4000), "%s: real-time path confirmed", what);
    else std::this_thread::sleep_for(std::chrono::milliseconds(800));
    uint8_t voice[60] = {1};
    std::vector<uint8_t> frame(1500, 7);  // (~200 kbit/s: under the real-time channel's starting rate)
    frame[0] = 2;
    frame[1] = 1;
    int framesSent = 0;
    for (int i = 0; i < 100; i++) {
        X.SendMedia(voice, sizeof(voice));
        Y.SendMedia(voice, sizeof(voice));
        if (i % 3 == 0 && X.SendMedia(frame.data(), frame.size())) framesSent++;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(WaitFor([&] { return voiceX >= 95 && voiceY >= 95 && videoY >= framesSent && framesSent >= 30; }, 4000),
          "%s: media both ways (voice %d / %d, video %d of %d)", what, voiceX.load(), voiceY.load(), videoY.load(), framesSent);
    bool rtX = X.CallRtStats(st) && st.ready, rtY = Y.CallRtStats(st) && st.ready;
    CHECK(rtX == expectRt && rtY == expectRt, "%s: %s", what, expectRt ? "over the real-time channel" : "over the connection (no real-time channel)");
    if (expectRt) CHECK(X.CallLinkBps() >= RtMedia::kMinBps, "%s: an estimate for the video (%d)", what, X.CallLinkBps());
    X.EndCall();
    CHECK(WaitFor([&] { auto l = Y.Lock(); return Y.call.state == CallState::None; }, 3000), "%s: ended", what);
    CHECK(!X.CallRtStats(st) && !Y.CallRtStats(st), "%s: channel gone with the call", what);
    X.onMedia = nullptr;
    Y.onMedia = nullptr;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (const char* f = getenv("LUNASHARE_TEST_SERVE_LINK")) return ServeLinks(f);
    SetMachineId("");  // engines on one test computer would all look like the same device to the server
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestHash();
    TestCrypto();
    TestMdnsPackets();
    TestSeal();
    TestHistoryCrypt();
    TestRtMedia();
    if (getenv("LUNASHARE_TEST_RT_ONLY")) return g_fail ? 1 : 0;
    printf("root: %s\n", Root().c_str());
    TestShareLinks();
    if (getenv("LUNASHARE_TEST_LINKS_ONLY")) return g_fail ? 1 : 0;

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
    {  // verifying by QR code (the scanned text)
        CHECK(A->VerifyByCode(idB, "https://example.com") == 0, "verify: not a LunaShare code");
        CHECK(A->VerifyByCode(idB, A->VerifyCode()) == -1, "verify: someone else's code doesn't match");
        CHECK(!A->devices[idB].verified, "verify: still not verified");
        CHECK(A->VerifyByCode(idB, B->VerifyCode()) == 1, "verify: bob's code matches");
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->devices[idB].verified; }, 10), "verify: bob verified");
        A->SetVerified(idB, false);
    }
    {  // adding someone by QR code: the code carries the keys and where to connect
        auto N = MakeEngine("qr-new", 48341, false, false);
        std::string pub, verify;
        CHECK(N->UseConnectCode("LSV1:" + A->MyAccount() + ":" + A->MyPublicKey(), pub, verify) == 0, "connect code: a verify code isn't one");
        CHECK(N->UseConnectCode(N->ConnectCode(), pub, verify) == 0, "connect code: not my own");
        CHECK(N->UseConnectCode("LSC1:" + std::string(64, 'a') + ":" + std::string(64, 'b') + ":99999:10.0.0.1", pub, verify) == 0, "connect code: a bad port");
        CHECK(N->UseConnectCode(A->ConnectCode(), pub, verify) == 1 && pub == A->MyPublicKey(), "connect code: alice's is taken");
        std::string got;
        CHECK(WaitFor([&] { got = N->ConnectedDeviceByKey(pub); return !got.empty(); }, 6000) && got == idA, "connect code: connected to alice");
        CHECK(N->VerifyByCode(got, verify) == 1, "connect code: and verified");
        {
            auto l = N->Lock();
            CHECK(N->devices[idA].verified, "connect code: alice marked verified");
        }
        std::string idN = N->settings.deviceId;
        A->SendChat(idN, "hi from the QR code");
        std::string keyN = A->ChatKey(idN);
        CHECK(WaitFor([&] { auto l = A->Lock(); auto& m = A->chats[keyN].messages; return !m.empty() && m.back().delivered; }, 3000), "chat with the new contact");
        N->Stop();
        WaitFor([&] { return !A->IsConnected(idN); }, 3000);
        A->ForgetDevice(idN);
        {
            auto l = A->Lock();
            auto conv = A->Conversations();
            CHECK(keyN[0] == '@' && std::find(conv.begin(), conv.end(), keyN) == conv.end() && !A->chats.count(keyN),
                  "forgetting a person's only device forgets the person and the chat (no nameless entry left)");
        }
    }
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

    // ------------------------------------------------------------ answer + change
    printf("[reply + edit]\n");
    A->SendChat(idB, "what time?");
    std::string q;
    {
        auto l = A->Lock();
        q = A->chats[A->ChatKey(idB)].messages.back().mid;
    }
    CHECK(WaitFor([&] { auto l = B->Lock(); return B->chats[B->ChatKey(idA)].messages.back().mid == q; }, 3000), "question arrives");
    B->SendChat(idA, "at five", q);
    CHECK(WaitFor([&] {
        auto l = A->Lock();
        auto& m = A->chats[A->ChatKey(idB)].messages.back();
        return m.text == "at five" && m.replyTo == q && m.replyText == "what time?";
    }, 3000), "answer carries what it answers");
    CHECK(A->EditMessage(idB, q, "what time tomorrow?"), "edit own message");
    CHECK(WaitFor([&] {
        auto l = B->Lock();
        for (auto& m : B->chats[B->ChatKey(idA)].messages)
            if (m.mid == q) return m.text == "what time tomorrow?" && m.edited > 0;
        return false;
    }, 3000), "edit arrives");
    CHECK(WaitFor([&] {
        auto l = A->Lock();
        for (auto& m : A->chats[A->ChatKey(idB)].messages)
            if (m.mid == q) return !m.editPending;
        return false;
    }, 3000), "edit acknowledged");
    // changed right away (the first text may already be on its way)
    A->SendChat(idB, "typo hree");
    {
        std::string t;
        {
            auto l = A->Lock();
            t = A->chats[A->ChatKey(idB)].messages.back().mid;
        }
        A->EditMessage(idB, t, "typo here");
        CHECK(WaitFor([&] {
            auto l = B->Lock();
            for (auto& m : B->chats[B->ChatKey(idA)].messages)
                if (m.mid == t) return m.text == "typo here";
            return false;
        }, 3000), "quick edit arrives");
    }
    {
        std::string theirs;
        {
            auto l = A->Lock();
            for (auto& m : A->chats[A->ChatKey(idB)].messages)
                if (m.text == "at five") theirs = m.mid;
        }
        CHECK(!A->EditMessage(idB, theirs, "hacked"), "cannot change their message");
    }

    // rules for taking files without asking: a bigger one asks first
    {
        std::string big = JoinPath(A->ConfigPath(), "big.bin");
        WriteRandomFile(big, 2 * 1024 * 1024 + 7, 77);
        B->settings.autoAcceptMaxMB = 1;
        uint64_t batch = A->SendPaths(idB, {big});
        auto stateAtB = [&]() {
            auto l = B->Lock();
            for (auto& [tid, t] : B->transfers)
                if (!t->outgoing && t->name == "big.bin") return (int)t->state;
            return -1;
        };
        CHECK(WaitFor([&] { return stateAtB() == (int)TState::Pending; }, 5000), "a file over the limit waits to be accepted");
        {
            std::vector<uint64_t> tids;
            {
                auto l = A->Lock();
                for (auto& [tid, t] : A->transfers)
                    if (t->batch == batch) tids.push_back(tid);
            }
            for (auto t : tids) A->Cancel(t);
        }
        B->settings.autoAcceptMaxMB = 0;
    }

    // forwarded: marked as such on the other side
    A->SendChat(idB, "passed on", "", {}, true);
    CHECK(WaitFor([&] {
        auto l = B->Lock();
        auto& m = B->chats[B->ChatKey(idA)].messages.back();
        return m.text == "passed on" && m.forwarded;
    }, 3000), "forwarded mark arrives");

    // pinned for both, starred only here
    {
        std::string pm;
        {
            auto l = A->Lock();
            pm = A->chats[A->ChatKey(idB)].messages.back().mid;
        }
        A->PinMessage(idB, pm, true);
        A->StarMessage(idB, pm, true);
        auto pinnedOn = [&](Engine& e, const std::string& key, bool want) {
            auto l = e.Lock();
            for (auto& m : e.chats[key].messages)
                if (m.mid == pm) return m.pinned == want;
            return false;
        };
        CHECK(WaitFor([&] { return pinnedOn(*B, B->ChatKey(idA), true); }, 3000), "pinned for the other side");
        {
            auto l = B->Lock();
            for (auto& m : B->chats[B->ChatKey(idA)].messages)
                if (m.mid == pm) CHECK(!m.starred, "a star stays mine");
        }
        B->PinMessage(idA, pm, false);
        CHECK(WaitFor([&] { return pinnedOn(*A, A->ChatKey(idB), false); }, 3000), "unpinned by the other side");
    }

    // disappearing messages: set on one side, both follow; messages go away when their time is up
    {
        A->SetChatTtl(idB, 3600);
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->chats[B->ChatKey(idA)].ttl == 3600; }, 3000), "both sides disappear after an hour");
        A->SendChat(idB, "self-destruct");
        CHECK(WaitFor([&] {
            auto l = B->Lock();
            auto& m = B->chats[B->ChatKey(idA)].messages.back();
            return m.text == "self-destruct" && m.expires > NowMs() + 3500 * 1000LL && m.expires < NowMs() + 3700 * 1000LL;
        }, 3000), "it carries its time");
        {
            auto l = B->Lock();
            B->chats[B->ChatKey(idA)].messages.back().expires = NowMs() - 1;  // (an hour later)
            B->ExpireMessages();
            CHECK(B->chats[B->ChatKey(idA)].messages.back().text != "self-destruct", "and goes away");
        }
        {
            auto l = A->Lock();
            for (auto& m : A->chats[A->ChatKey(idB)].messages)
                if (m.text == "self-destruct") m.expires = NowMs() - 1;
            A->ExpireMessages();
        }
        B->SetChatTtl(idA, 0);
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->chats[A->ChatKey(idB)].ttl == 0; }, 3000), "turned off by the other side");
    }

    // ------------------------------------------------------------ silent chats
    printf("[silent chats]\n");
    {
        std::string k = A->ChatKey(idB);
        CHECK(!A->ChatMuted(k), "not silent at first");
        A->MuteChat(k, 60);
        CHECK(A->ChatMuted(k) && A->ChatMutedUntil(k) > 0, "silent for an hour");
        A->MuteChat(k, 0);
        CHECK(A->ChatMutedUntil(k) == 0, "silent until turned back on");
        {
            auto l = A->Lock();
            A->settings.mutedChats[k] = 1;  // (a time long gone)
        }
        CHECK(!A->ChatMuted(k), "time is up");
        Settings s = A->settings;
        A->MuteChat(k, 0);
        A->ApplySettings(s);  // (an older copy from the settings page)
        CHECK(A->ChatMuted(k), "settings page keeps it");
        A->MuteChat(k, -1);
        CHECK(!A->ChatMuted(k), "back on");
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
            CHECK(A->devices[idA2].accountPub == A->MyAccount() && A->ChatKey(idA2) == A->SelfKey(), "my other device: my own chat");
            auto conv = A->Conversations();
            CHECK(std::count(conv.begin(), conv.end(), A->SelfKey()) == 1 && std::count(conv.begin(), conv.end(), idA2) == 0,
                  "one chat with myself, not one per device");
        }
        {  // my own chat: notes reach my other devices, shown as written there; files stay here
            std::string self = A->SelfKey();
            A->SendChat(self, "a note to myself");
            auto from = [](Engine& e, const std::string& key, const std::string& text, bool& out, std::string& dev) {
                auto l = e.Lock();
                for (auto& m : e.chats[key].messages)
                    if (m.text == text) { out = m.outgoing; dev = m.fromDev; return true; }
                return false;
            };
            bool out = true;
            std::string dev;
            CHECK(WaitFor([&] { return from(*A2, self, "a note to myself", out, dev); }, 3000) && !out && dev == idA,
                  "a note reaches my other device, from this one (%d %s)", (int)out, dev.c_str());
            CHECK(from(*A, self, "a note to myself", out, dev) && out && dev.empty(), "here it's mine");
            A2->SendChat(A2->SelfKey(), "answered on the other device");
            CHECK(WaitFor([&] { return from(*A, self, "answered on the other device", out, dev); }, 3000) && !out && dev == idA2,
                  "and back, from that device");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            {
                auto l = B->Lock();
                bool leaked = false;
                for (auto& [k, cv] : B->chats)
                    for (auto& m : cv.messages) leaked |= m.text == "a note to myself";
                CHECK(!leaked, "never to a contact");
            }
            std::string note = JoinPath(Root(), "note.bin");
            WriteRandomFile(note, 1000, 5);
            A->SendPaths(self, {note});
            auto l = A->Lock();
            bool kept = false;
            for (auto& [id, t] : A->transfers) kept |= t->localPath == note && t->state == TState::Completed && t->peer == A->settings.deviceId;
            CHECK(kept, "a file in my own chat is kept here, not sent");
        }
        {  // ...and my other device is told: offered there (in my own chat), saved only when I want it
            auto offered = [&](TState st) {
                auto l = A2->Lock();
                for (auto& [id, t] : A2->transfers)
                    if (!t->outgoing && t->name == "note.bin" && t->ask && t->state == st) return t->batch;
                return (uint64_t)0;
            };
            CHECK(WaitFor([&] { return offered(TState::Pending) != 0; }, 8000), "my other device is offered the file put in my own chat");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            uint64_t ob = offered(TState::Pending);
            CHECK(ob != 0, "not taken by itself");
            {
                auto l = A2->Lock();
                bool inSelf = false;
                for (auto& m : A2->chats[A2->SelfKey()].messages) inSelf |= m.kind == MsgKind::Files && m.batch == ob;
                CHECK(inSelf, "shown in my own chat there");
            }
            A2->AcceptBatch(ob);
            CHECK(WaitFor([&] { return offered(TState::Completed) != 0; }, 8000), "saved there when asked");
        }
        {  // ...but sent to one of my devices (picked by itself): it goes there, shown in my own chat on both
            std::string f = JoinPath(Root(), "tomypc.bin");
            WriteRandomFile(f, 300000, 9);
            A->SendPaths(idA2, {f});
            auto got = [&] {
                auto l = A2->Lock();
                for (auto& [id, t] : A2->transfers)
                    if (!t->outgoing && t->name == "tomypc.bin" && t->state == TState::Completed) return true;
                return false;
            };
            CHECK(WaitFor(got, 8000), "a file sent to my other device arrives there");
            auto l = A->Lock();
            bool inSelf = false;
            for (auto& m : A->chats[A->SelfKey()].messages) inSelf |= m.kind == MsgKind::Files && !m.tids.empty() && A->transfers.count(m.tids[0]) &&
                                                                    A->transfers[m.tids[0]]->name == "tomypc.bin";
            CHECK(inSelf, "shown in my own chat");
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
        // playlists / settings: to my other device only (the app merges them)
        A->SyncPush("playlists", {{"lists", json::array({{{"id", "p1"}, {"mod", 5}}})}});
        auto popKind = [](Engine& e, const std::string& want, json& out) {
            std::string k;
            json d;
            while (e.SyncPop(k, d))
                if (k == want) {
                    out = d;
                    return true;
                }
            return false;
        };
        json got;
        CHECK(WaitFor([&] { return popKind(*A2, "playlists", got); }, 3000) && got["lists"][0]["id"] == "p1", "synced to my other device");
        {
            json none;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            CHECK(!popKind(*B, "playlists", none), "not to a contact");
        }
        // the clipboard: only when the receiving device shares it too
        A->settings.clipShare = true;
        A->SyncPush("clip", {{"text", "secret"}, {"ts", NowMs()}});
        {
            json none;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(!popKind(*A2, "clip", none), "not taken while it's off there");
        }
        A2->settings.clipShare = true;
        A->SyncPush("clip", {{"text", "copied"}, {"ts", NowMs()}});
        CHECK(WaitFor([&] { return popKind(*A2, "clip", got); }, 3000) && got["text"] == "copied", "the clipboard reaches my other device");
        // something asked of one device of mine (torrents on my computer): only between my own devices
        {
            std::mutex hm;
            std::vector<std::string> got2;
            {
                auto l = A2->Lock();
                A2->ownHook = [&](const std::string& from, const std::string& kind, const json& d) {
                    std::lock_guard<std::mutex> g(hm);
                    got2.push_back(from + "|" + kind + "|" + d.value("op", ""));
                };
            }
            CHECK(A->OwnOnline(idA2) && A->OwnSend(idA2, "torrent", {{"op", "list"}}), "asked of my other device");
            CHECK(WaitFor([&] { std::lock_guard<std::mutex> g(hm); return got2.size() == 1 && got2[0] == idA + "|torrent|list"; }, 3000), "it arrives, with who asked");
            CHECK(!A->OwnSend(idB, "torrent", {{"op", "list"}}) && !B->OwnSend(idA, "torrent", {{"op", "list"}}), "never to or from a contact");
            auto l = A2->Lock();
            A2->ownHook = nullptr;
        }
        // folder sync: a folder's files go to my other device by themselves (new ones later too), without asking
        {
            namespace fs = std::filesystem;
            std::string src = JoinPath(A->ConfigPath(), "Camera");
            MakeDirs(JoinPath(src, "2026"));
            auto put = [&](const std::string& rel, const std::string& data) {
                std::string p = JoinPath(src, rel);
                std::ofstream(fs::u8path(p), std::ios::binary) << data;
                fs::last_write_time(fs::u8path(p), fs::file_time_type::clock::now() - std::chrono::minutes(5));  // (not still being written)
            };
            put("a.jpg", "photo one");
            put("2026/b.jpg", "photo two");
            put(".pending-x.jpg", "not yet");
            {
                auto l = A->Lock();
                A->settings.folderSyncs.push_back({"fs1", src, A2->settings.deviceId, true});
            }
            A->FolderSyncTick(true);
            std::string dst = JoinPath(A2->settings.downloadDir, "Camera");
            CHECK(WaitFor([&] { return FileExists(JoinPath(dst, "a.jpg")) && FileExists(JoinPath(dst, "2026/b.jpg")); }, 8000), "the folder's files arrive");
            CHECK(!FileExists(JoinPath(dst, ".pending-x.jpg")), "hidden ones don't");
            CHECK(WaitFor([&] { A->FolderSyncTick(); return A->FolderSyncStatus("fs1") == "2|0"; }, 5000), "remembered as sent");
            put("c.jpg", "photo three");
            A->FolderSyncTick(true);
            CHECK(WaitFor([&] { return FileExists(JoinPath(dst, "c.jpg")); }, 8000), "a new one follows");
            CHECK(WaitFor([&] { A->FolderSyncTick(); return A->FolderSyncStatus("fs1") == "3|0"; }, 5000), "only the new one");
        }
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
        // @mentions: the named one gets told (through a silent chat too)
        B->MarkRead(gk);
        C->MarkRead(gk);
        A->SendChat(gk, "@bob look at this", "", {B->MyAccount()});
        CHECK(WaitFor([&] {
            auto l = B->Lock();
            return B->chats[gk].mentioned && B->chats[gk].messages.back().mentions.size() == 1;
        }, 3000), "the one @named sees it");
        CHECK(WaitFor([&] { return gHas(*C, "@bob look at this", aKey); }, 3000), "the others get it too");
        {
            auto l = C->Lock();
            CHECK(!C->chats[gk].mentioned, "not as a mention");
        }
        B->MarkRead(gk);
        {
            auto l = B->Lock();
            CHECK(!B->chats[gk].mentioned, "read: no more @");
        }
        // a poll: everyone sees everyone's answers
        A->SendPoll(gk, "Sunday lunch?", {"12:00", "14:00", "16:00"}, false);
        std::string pm;
        CHECK(WaitFor([&] {
            auto l = B->Lock();
            auto& m = B->chats[gk].messages.back();
            pm = m.mid;
            return m.pollQ == "Sunday lunch?" && m.pollOpts.size() == 3;
        }, 3000), "the poll arrives");
        CHECK(WaitFor([&] {  // (carol votes too: she must have it first)
            auto l = C->Lock();
            for (auto& m : C->chats[gk].messages)
                if (m.mid == pm) return true;
            return false;
        }, 3000), "the poll reaches carol");
        B->Vote(gk, pm, {1});
        C->Vote(gk, pm, {1, 2});  // (one answer only: the first counts)
        auto votesOn = [&](Engine& e, const std::string& who, int want) {
            auto l = e.Lock();
            for (auto& m : e.chats[gk].messages)
                if (m.mid == pm) {
                    auto v = m.votes.find(who);
                    return v != m.votes.end() && v->second.opts.size() == 1 && v->second.opts[0] == want;
                }
            return false;
        };
        CHECK(WaitFor([&] { return votesOn(*A, "@" + B->MyAccount(), 1) && votesOn(*A, "@" + C->MyAccount(), 1); }, 3000), "answers reach the asker");
        CHECK(WaitFor([&] { return votesOn(*C, "@" + B->MyAccount(), 1); }, 5000), "and the others");
        B->Vote(gk, pm, {0});
        CHECK(WaitFor([&] { return votesOn(*A, "@" + B->MyAccount(), 0); }, 3000), "a changed answer");
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
        B->WatchSetLoop(true);
        CHECK(WaitFor([&] { auto l = A->Lock(); auto l2 = B->Lock(); return A->watch.loop && B->watch.loop; }, 3000), "loop on for everyone");
        // the viewer keeps a copy: everything comes, then it's in Downloads
        B->WatchSave();
        std::string saved = JoinPath(B->GetSettings().downloadDir, "movie.bin");
        std::string wantSha = Sha256File(movie);
        CHECK(WaitFor([&] { return FileSize(saved) == (20 << 20) && Sha256File(saved) == wantSha; }, 15000), "a copy saved to Downloads (%lld bytes, %.0f%% loaded, saving %d)",
              (long long)FileSize(saved), B->WatchLoaded() * 100, (int)B->watch.saving);
        A->LeaveWatch();
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->watch.sid == 0; }, 3000), "the host ends it for everyone");
        CHECK(B->WatchRead(0, tmp.data(), 100) < 0, "the player stops reading");
        CHECK(!FileExists(JoinPath(B->configDir, "watch/" + ToHex((const uint8_t*)&sid, 8) + ".part")), "the temporary copy is gone");
        // saving goes on after the host stops the movie; a host can also turn saving off
        {
            std::string movie2 = JoinPath(Root(), "src/movie2.bin");
            WriteRandomFile(movie2, 160 << 20, 78);
            std::string sha2 = Sha256File(movie2);
            uint64_t s3 = A->StartWatch(bob, movie2);
            CHECK(WaitFor([&] { auto l = B->Lock(); return !B->watchInvites.empty() && B->watchInvites.back().sid == s3; }, 3000) && B->JoinWatch(s3),
                  "second movie joined");
            std::vector<uint8_t> t2(1000);
            B->WatchRead(0, t2.data(), 1000);
            CHECK(WaitFor([&] { auto l = B->Lock(); return B->watch.allowSave; }, 3000), "saving allowed by default");
            B->WatchSave();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));  // (the host hears that B is saving)
            A->LeaveWatch();  // the host stops before the copy is complete
            CHECK(WaitFor([&] { auto l = B->Lock(); return B->watch.sid == 0; }, 3000), "the movie ended");
            auto bgs = B->WatchSaves();
            printf("  copy %s when the movie ended\n", bgs.empty() ? "already complete" : ("at " + std::to_string((int)(bgs[0].frac * 100)) + "%").c_str());
            std::string saved2 = JoinPath(B->GetSettings().downloadDir, "movie2.bin");
            CHECK(WaitFor([&] { return FileSize(saved2) == (160 << 20) && Sha256File(saved2) == sha2; }, 20000),
                  "the copy kept coming and was saved (%lld bytes)", (long long)FileSize(saved2));
            CHECK(WaitFor([&] { return B->WatchSaves().empty(); }, 3000), "nothing left in the background");
            // the host turns saving off: viewers can't keep a copy
            A->WatchSetAllowSave(false);
            uint64_t s4 = A->StartWatch(bob, movie2);
            CHECK(WaitFor([&] { auto l = B->Lock(); return !B->watchInvites.empty() && B->watchInvites.back().sid == s4; }, 3000) && B->JoinWatch(s4),
                  "third movie joined");
            CHECK(WaitFor([&] { auto l = B->Lock(); return !B->watch.allowSave && B->watch.people.size() == 2; }, 3000), "saving turned off reaches the viewer");
            B->WatchSave();
            {
                auto l = B->Lock();
                CHECK(!B->watch.saving, "the viewer can't save");
            }
            A->LeaveWatch();
            A->WatchSetAllowSave(true);
            CHECK(WaitFor([&] { auto l = B->Lock(); return B->watch.sid == 0; }, 3000), "ended");
        }
        {  // the host goes on to another movie: the viewer comes along
            std::string movie2 = JoinPath(Root(), "src/movie2.bin");
            WriteRandomFile(movie2, 3 << 20, 78);
            uint64_t s5 = A->StartWatch(bob, movie);
            CHECK(WaitFor([&] { auto l = B->Lock(); return !B->watchInvites.empty() && B->watchInvites.back().sid == s5; }, 3000) && B->JoinWatch(s5),
                  "watching");
            CHECK(WaitFor([&] { auto l = A->Lock(); return A->watch.people.size() == 2; }, 3000), "together");
            uint64_t s6 = A->WatchSwitch(movie2);
            CHECK(s6 && s6 != s5, "the next movie");
            CHECK(WaitFor([&] { auto l = B->Lock(); return B->watch.sid == s6 && B->watch.name == "movie2.bin"; }, 3000), "the viewer comes along");
            CHECK(WaitFor([&] { auto l = A->Lock(); return A->watch.people.size() == 2; }, 3000), "still together");
            // next, next, back - quicker than the viewer gets in: it still ends up on the last one
            uint64_t s7 = A->WatchSwitch(movie);
            uint64_t s8 = A->WatchSwitch(movie2);
            uint64_t s9 = A->WatchSwitch(movie);
            CHECK(s7 && s8 && s9, "quick switches");
            CHECK(WaitFor([&] { auto l = B->Lock(); return B->watch.sid == s9 && B->watch.name == "movie.bin"; }, 5000),
                  "the viewer follows quick switches (on %llx)", (unsigned long long)[&] { auto l = B->Lock(); return B->watch.sid; }());
            CHECK(WaitFor([&] { auto l = A->Lock(); return A->watch.people.size() == 2; }, 3000), "together after quick switches");
            {
                auto l = B->Lock();
                bool stale = std::any_of(B->watchInvites.begin(), B->watchInvites.end(), [&](const WatchInviteInfo& w) { return w.sid == s7 || w.sid == s8 || w.sid == s9; });
                CHECK(!stale, "no leftover invitations to join");
            }
            A->LeaveWatch();
            CHECK(WaitFor([&] { auto l = B->Lock(); return B->watch.sid == 0; }, 3000), "ended");
        }
        // the movie player: alone first, then someone is added
        uint64_t sid2 = A->StartWatch("", movie);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(sid2 != 0 && WaitFor([&] { auto l = B->Lock(); return B->watchInvites.empty() || B->watchInvites.back().sid != sid2; }, 100), "alone: nobody invited");
        A->WatchInviteTo(bob);
        CHECK(WaitFor([&] { auto l = B->Lock(); return !B->watchInvites.empty() && B->watchInvites.back().sid == sid2; }, 3000), "added afterwards: invited");
        CHECK(B->JoinWatch(sid2) && WaitFor([&] { auto l = A->Lock(); return A->watch.people.size() == 2; }, 3000), "and watching together");
        A->LeaveWatch();
        {  // autoplay: a movie starts on its own
            auto l = A->Lock();
            CHECK(!A->watch.playing, "(stopped)");
            Settings s = A->GetSettings();
            s.playerAutoplay = true;
            A->ApplySettings(s);
            A->StartWatch("", movie);
            CHECK(A->watch.playing, "autoplay: playing without pressing play");
            A->LeaveWatch();
            s.playerAutoplay = false;
            A->ApplySettings(s);
            A->StartWatch("", movie);
            CHECK(!A->watch.playing, "without autoplay: waits for play");
            A->LeaveWatch();
        }
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->watch.sid == 0; }, 3000), "ended");
    }

    printf("[listening together]\n");
    {
        std::string songA = JoinPath(Root(), "src/songA.mp3"), songB = JoinPath(Root(), "src/songB.mp3");
        WriteRandomFile(songA, 3 << 20, 11);
        WriteRandomFile(songB, (5 << 20) + 1234, 12);
        std::string bob = "@" + B->MyAccount();
        uint64_t sid = A->StartListen(bob);
        CHECK(sid != 0, "started");
        CHECK(WaitFor([&] { auto l = B->Lock(); return !B->listenInvites.empty() && B->listenInvites.back().sid == sid; }, 3000), "invited");
        CHECK(B->JoinListen(sid), "joined");
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->listen.people.size() == 2; }, 3000), "the host sees the listener");
        // each adds a song of their own
        ListenItem ia;
        ia.path = songA;
        ia.title = "Song A";
        ia.dur = 3;
        A->ListenAdd({ia});
        ListenItem ib;
        ib.path = songB;
        ib.title = "Song B";
        ib.dur = 200;
        B->ListenAdd({ib});
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->listen.items.size() == 2 && B->listen.playing && B->listen.at == 0; }, 3000),
              "the shared queue reaches everyone, the first song plays");
        uint64_t iidB;
        {
            auto l = A->Lock();
            CHECK(A->listen.items[0].owner == A->settings.deviceId && A->listen.items[1].owner == B->settings.deviceId, "whose songs");
            iidB = A->listen.items[1].iid;
        }
        uint64_t iidA;
        {
            auto l = B->Lock();
            iidA = B->listen.items[0].iid;
        }
        // the listener reads the host's song through the stream, like the player does
        auto readAll = [](Engine& e, uint64_t iid, int64_t size, const std::string& orig) {
            std::vector<uint8_t> got((size_t)size), want((size_t)size);
            FILE* f = OpenFile(orig, "rb");
            bool ok = f && fread(want.data(), 1, want.size(), f) == want.size();
            if (f) fclose(f);
            int64_t off = 0, t0 = SteadyMs();
            while (ok && off < size && SteadyMs() - t0 < 10000) {
                int n = e.ListenRead(iid, off, got.data() + off, (int)std::min<int64_t>(100000, size - off), 200);
                if (n == -2) continue;
                if (n <= 0) return false;
                off += n;
            }
            return ok && off == size && got == want;
        };
        CHECK(readAll(*B, iidA, 3 << 20, songA), "the host's song streams to the listener");
        // the song ends (by the clock: 3 s + a moment): the listener's song is next, and it streams to the host
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->listen.at == 1 && A->listen.playing; }, 8000), "next song by the clock");
        CHECK(readAll(*A, iidB, (5 << 20) + 1234, songB), "the listener's song streams to the host");
        // controls from the listener go through the host
        B->ListenControl("pause");
        CHECK(WaitFor([&] { auto l = A->Lock(); return !A->listen.playing; }, 3000), "a listener pauses for everyone");
        B->ListenControl("seek", 42);
        CHECK(WaitFor([&] { auto l = A->Lock(); return std::abs(A->ListenPos() - 42) < 0.01; }, 3000), "a listener seeks for everyone");
        B->ListenControl("prev");
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->listen.at == 1 && A->ListenPos() < 0.01; }, 3000), "back to the start of the song");
        B->ListenControl("remove", 0, iidA);  // (not theirs: refused)
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        {
            auto l = A->Lock();
            CHECK(A->listen.items.size() == 2, "a listener can't remove someone else's song");
        }
        // repeat one: the song starts again when it ends; shuffle: a random one not played yet, picked ahead
        B->ListenControl("repeat", 2);
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->listen.repeat == 2; }, 3000), "a listener turns repeat on for everyone");
        A->ListenControl("play");
        A->ListenControl("seek", 150);
        A->ListenEnded(iidB);
        {
            auto l = A->Lock();
            CHECK(A->listen.at == 1 && A->ListenPos() < 0, "repeat one: the same song again");
        }
        A->ListenControl("repeat", 0);
        A->ListenControl("shuffle", 1);
        {
            auto l = A->Lock();
            CHECK(A->listen.shuffle && A->listen.nextIid == iidA, "shuffle: the other song is picked to come next");
        }
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->listen.shuffle && B->listen.NextForFetch() == 0; }, 3000),
              "the listener knows which one comes next (to fetch it ahead)");
        A->ListenControl("next");
        {
            auto l = A->Lock();
            CHECK(A->listen.at == 0, "shuffle: next is the picked one");
        }
        A->ListenControl("next");
        {
            auto l = A->Lock();
            CHECK(A->listen.at == 2, "shuffle, no repeat: everything played - the end");
        }
        A->ListenControl("shuffle", 0);
        // remix (everyone's): a part of the song, blending into a part of the next one before it ends
        B->ListenControl("remix", 1);
        B->ListenControl("remixfade", 2);
        B->ListenControl("repeat", 1);
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->listen.remix && A->listen.remixFade == 2; }, 3000), "a listener turns remix on");
        A->ListenControl("jump", 0, iidB);
        int64_t seq0;
        {
            auto l = A->Lock();
            CHECK(A->listen.at == 1 && A->listen.frag >= 16 && A->listen.frag <= 135 && A->ListenPos() >= A->listen.frag - 1.01,
                  "remix: the part starts at a random place past the intro, ending before the outro (%.1f)", A->listen.frag);
            CHECK(std::abs(A->listen.FragEnd() - (A->listen.frag + 45)) < 0.01, "the part's end");
            seq0 = A->listen.seq;
            A->ListenControl("seek", A->listen.FragEnd() - 2.4);
        }
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->listen.seq > seq0; }, 3000), "before the part ends, the next one comes");
        {
            auto l = A->Lock();
            CHECK(A->listen.at == 0 && A->listen.fade == 2 && A->ListenPos() >= A->listen.frag, "blending in (2 s), right away");
        }
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->listen.at == 0 && B->listen.fade == 2 && B->listen.seq > seq0; }, 3000),
              "the listener blends in too");
        A->ListenControl("remix", 0);
        A->ListenControl("repeat", 0);
        A->ListenControl("jump", 0, iidB);
        // saving the listener's song on the host
        A->ListenSave(iidB);
        std::string saved = JoinPath(A->settings.downloadDir, "songB.mp3");
        CHECK(WaitFor([&] { return FileSize(saved) == (5 << 20) + 1234; }, 5000), "a song kept from listening together");
        RemoveFile(saved);
        // the listener leaves: their songs that haven't played go
        ListenItem ib2 = ib;
        B->ListenAdd({ib2});
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->listen.items.size() == 3; }, 3000), "another one added");
        A->ListenControl("jump", 0, iidA);
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->listen.at == 0; }, 3000), "jumped back to the first");
        B->LeaveListen();
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->listen.items.size() == 1 && A->listen.people.size() == 1; }, 3000),
              "the one who left takes their songs along");
        {
            auto l = B->Lock();
            CHECK(B->listen.sid == 0, "left");
        }
        A->LeaveListen();
        {
            auto l = A->Lock();
            CHECK(A->listen.sid == 0, "ended");
        }
    }

    printf("[reading a comic together]\n");
    {
        // a comic's pages (as the viewer has them: pictures in a folder) - a big one, a small one, an empty file
        std::vector<std::string> pages;
        for (int i = 0; i < 6; i++) {
            std::string p = JoinPath(Root(), "src/comic/page" + std::to_string(i) + (i == 2 ? ".png" : ".jpg"));
            MakeDirs(DirName(p));
            WriteRandomFile(p, i == 0 ? (1 << 20) + 77 : 20000 + i * 1000, 40 + i);
            pages.push_back(p);
        }
        auto slurp = [](const std::string& p, std::string& out) {
            FILE* f = OpenFile(p, "rb");
            if (!f) return false;
            char buf[65536];
            size_t n;
            out.clear();
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
            fclose(f);
            return true;
        };
        auto same = [&](const std::string& a, const std::string& b) {
            std::string x, y;
            return slurp(a, x) && slurp(b, y) && x == y;
        };
        std::string bob = "@" + B->MyAccount();
        uint64_t sid = A->StartRead(bob, "Moomins.cbz", pages, 1);
        CHECK(sid != 0, "started");
        CHECK(WaitFor([&] { auto l = B->Lock(); return !B->readInvites.empty() && B->readInvites.back().sid == sid; }, 3000), "invited");
        {
            auto l = B->Lock();
            CHECK(B->readInvites.back().name == "Moomins.cbz" && B->readInvites.back().pages == 6, "the invite says what it is");
        }
        CHECK(B->JoinRead(sid), "joined");
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->read.people.size() == 2; }, 3000), "the host sees the reader");
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->read.pages == 6 && B->read.page == 1 && !B->read.files[1].empty(); }, 5000),
              "the page that's open comes first");
        std::string got1;
        {
            auto l = B->Lock();
            got1 = B->read.files[1];
        }
        CHECK(same(got1, pages[1]) && got1.size() > 4 && got1.substr(got1.size() - 4) == ".jpg", "the page arrived whole, as a picture");
        CHECK(WaitFor([&] { auto l = B->Lock(); return std::none_of(B->read.files.begin(), B->read.files.end(), [](auto& f) { return f.empty(); }); }, 8000),
              "then all the others");
        {
            auto l = B->Lock();
            bool ok = true;
            for (int i = 0; i < 6; i++) ok = ok && same(B->read.files[(size_t)i], pages[(size_t)i]);
            CHECK(ok, "every page the same as the host's");
        }
        // turning pages: anyone, for everyone
        B->ReadTurn(4);
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->read.page == 4; }, 3000), "the reader turns the page for the host");
        A->ReadTurn(2);
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->read.page == 2; }, 3000), "the host turns it for the reader");
        B->ReadTurn(5);
        {
            auto l = B->Lock();
            CHECK(B->read.page == 5, "turned here at once (the host's answer follows)");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        {
            auto l = B->Lock();
            CHECK(B->read.page == 5, "an older word from the host doesn't turn it back");
        }
        CHECK(WaitFor([&] { auto l = A->Lock(); return A->read.page == 5; }, 3000), "and the host follows");
        // the pages are given only to those reading
        // the host ends it: the reader's copies go
        std::string dir = DirName(got1);
        A->LeaveRead();
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->read.sid == 0; }, 3000), "the host ends it for everyone");
        CHECK(!FileExists(got1) && !FileExists(dir), "the pages that came are deleted");
        {
            auto l = A->Lock();
            CHECK(A->read.sid == 0 && FileExists(pages[0]), "the host's own pages stay");
        }
        // an invite that ended before joining is forgotten
        uint64_t sid2 = A->StartRead(bob, "Second.cbz", pages, 0);
        CHECK(WaitFor([&] { auto l = B->Lock(); return !B->readInvites.empty() && B->readInvites.back().sid == sid2; }, 3000), "invited again");
        A->LeaveRead();
        CHECK(WaitFor([&] { auto l = B->Lock(); return B->readInvites.empty(); }, 3000), "the ended one is forgotten");
    }

    printf("[chat archives, history sent back]\n");
    {
        std::string bk = A->ChatKey(idB);
        auto texts = [&](Engine& e, const std::string& key) {
            auto l = e.Lock();
            std::vector<std::pair<std::string, bool>> v;
            for (auto& m : e.chats[key].messages)
                if (m.kind == MsgKind::Text) v.push_back({m.text, m.outgoing});
            return v;
        };
        auto before = texts(*A, bk);
        CHECK(!before.empty(), "a chat to archive (%d)", (int)before.size());
        std::string path = A->ArchiveChat(bk, true, false);
        CHECK(!path.empty() && FileExists(path), "archived");
        CHECK(texts(*A, bk).empty(), "and cleared");
        auto list = A->ChatArchives(bk);
        CHECK(list.size() == 1 && list[0].path == path && list[0].count >= (int)before.size(), "listed");
        CHECK(A->RestoreChatArchive(path) >= (int)before.size(), "restored");
        CHECK(texts(*A, bk) == before, "the same messages, the same sides");
        CHECK(A->RestoreChatArchive(path) == 0, "restoring again adds nothing");
        CHECK(A->DeleteChatArchive(path) && A->ChatArchives(bk).empty(), "archive deleted");
        // lost both: Bob sends it back (after agreeing)
        A->ClearChat(bk);
        A->AskHistory(bk);
        std::string ak = B->ChatKey(idA);
        CHECK(WaitFor([&] { auto l = B->Lock(); return !B->historyAsks.empty() && B->historyAsks.back().key == ak; }, 3000), "Bob is asked");
        B->AnswerHistoryAsk(idA, ak, true);
        CHECK(WaitFor([&] { return texts(*A, bk).size() >= before.size(); }, 5000), "the history comes back");
        auto back = texts(*A, bk);
        bool same = true;
        for (auto& t : before)
            if (std::find(back.begin(), back.end(), t) == back.end()) same = false;
        CHECK(same, "with the right sides (mine are mine)");
    }

    printf("[accounts: recovery phrase, backup, removing a device]\n");
    {
        std::string phrase = A->RecoveryPhrase(), seed, err;
        int words = 1;
        for (char ch : phrase) words += ch == ' ';
        CHECK(words == 24, "24 words (%d)", words);
        CHECK(PhraseToSeed(phrase, seed, &err) && SeedToPhrase(seed) == phrase, "phrase round trip");
        // a wrong last word: the checksum notices it - except by chance (1 in 256), so the first of a few that's noticed
        std::string typo;
        for (const char* w : {" zoo", " abandon", " zebra", " absurd", " youth"}) {
            std::string t = phrase.substr(0, phrase.rfind(' ')) + w;
            std::string s2, e2;
            if (t != phrase && !PhraseToSeed(t, s2, &e2)) {
                typo = t;
                break;
            }
        }
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
        std::string trailer = JoinPath(L1->SharedDir(), "films/trailer.bin");
        WriteRandomFile(trailer, 6 << 20, 33);
        std::string want = Sha256File(big);
        L1->RescanShared();
        L2->RescanShared();
        auto libSize = [](Engine& e) { auto l = e.Lock(); return e.myLibrary.size(); };
        CHECK(WaitFor([&] { return libSize(*L1) == 3 && libSize(*L2) == 1; }, 15000), "Shared folders indexed (%zu, %zu)", libSize(*L1), libSize(*L2));
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
        CHECK(WaitFor([&] { auto l = L3->Lock(); return L3->peerLibraries[i1].files.size() == 3 && L3->peerLibraries[i2].files.size() == 1; }, 5000),
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
        {  // played from their Shared folder while it comes (not kept)
            CHECK(L3->StartLibraryWatch(f) != 0, "movie player on a contact's file");
            auto read = L3->LibStreamReader(f.id);
            FILE* mf = OpenFile(big, "rb");
            std::mt19937 rng(9);
            bool same = true;
            for (int i = 0; i < 20 && same; i++) {
                int64_t off = i == 0 ? 0 : (int64_t)(rng() % (BIG - 70000));
                std::vector<uint8_t> got(65536), want(65536);
                int n = -2;
                int64_t t1 = SteadyMs();
                while (n == -2 && SteadyMs() - t1 < 10000) n = read(off, got.data(), (int)got.size());
                FileSeek(mf, off);
                size_t w = n > 0 ? fread(want.data(), 1, (size_t)n, mf) : 0;
                same = n > 0 && (size_t)n == w && memcmp(got.data(), want.data(), (size_t)n) == 0;
            }
            fclose(mf);
            CHECK(same, "what the player reads is their file");
            std::string part;
            {
                auto l = L3->Lock();
                auto it = L3->swarm.find(f.id);
                part = it == L3->swarm.end() ? "" : it->second->partPath;
                CHECK(it != L3->swarm.end() && !it->second->keep, "played, not kept");
            }
            L3->LeaveWatch();
            {
                auto l = L3->Lock();
                CHECK(!L3->swarm.count(f.id) && !FileExists(part) && !L3->HaveShared(f.id), "closing it leaves nothing behind");
            }
            // another one played, saved, closed: the download goes on and the file lands in the Shared folder
            LibFile g;
            {
                auto l = L3->Lock();
                for (auto& x : L3->peerLibraries[i1].files)
                    if (x.rel == "films/trailer.bin") g = x;
            }
            CHECK(L3->StartLibraryWatch(g) != 0, "trailer played");
            L3->WatchSave();
            L3->LeaveWatch();
            auto st = [&] { auto l = L3->Lock(); auto it = L3->swarm.find(g.id); return it == L3->swarm.end() ? -1 : (int)it->second->state; };
            CHECK(WaitFor([&] { return st() == SwarmDownload::Done && L3->HaveShared(g.id); }, 30000), "saved: the download went on after closing (%d)", st());
            std::string kept;
            {
                auto l = L3->Lock();
                kept = L3->swarm[g.id]->finalPath;
            }
            CHECK(Sha256File(kept) == Sha256File(trailer), "and it's the right file");
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
    // (paused early: on a slow runner the 4 seconds this file takes were sometimes over before the pause)
    CHECK(WaitFor([&] { auto t = GetT(*B, t3); return t && t->done.load() > SZ / 8; }, 15000), "progress");
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
    CHECK(A->ExportSettings(exp, true, false, &err, json{{"music", {{"folders", {"/m"}}}}}), "export: %s", err.c_str());
    auto C = MakeEngine("carol", 48130, false, false);
    json appData;
    CHECK(C->ImportSettings(exp, false, &err, &appData), "import: %s", err.c_str());
    CHECK(appData["music"]["folders"][0] == "/m", "the app's part comes back");
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
        auto F = MakeEngine("frank", 48170, true, false, false, false, "127.0.0.1:48190", "family", {"voice"});
        auto G = MakeEngine("gina", 48180, true, false, false, false, "127.0.0.1:48190", "family", {"voice"});
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
        RtCall(*F, *G, idG, true, true, true, "internet, direct");
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
            // ... and that new install (a new person on the phone) gets an invite of its own from L: they see each other
            {
                std::string kN(64, '9');
                {
                    auto lk = L->Lock();
                    Settings s = L->settings;
                    WanLink w;
                    w.secret = kN;
                    w.label = "dad";
                    w.created = NowMs();
                    s.wanLinks.push_back(w);
                    L->ApplySettings(s);
                }
                auto A3 = std::make_unique<Engine>();
                std::string cfg3 = JoinPath(Root(), "adam3/config");
                MakeDirs(cfg3);
                Settings s3;
                s3.displayName = "dad";
                s3.downloadDir = JoinPath(Root(), "adam3/downloads");
                s3.tcpPort = 48189;
                s3.autoConnect = true;
                s3.wanEnabled = true;
                s3.wanServer = srvAddr;
                s3.wanRoom = "invite-0123456789abcdef";  // (what using an invite sets up: a room of its own, hidden)
                s3.wanHidden = true;
                s3.wanFromInvite = true;
                WanLink got;
                got.secret = kN;
                got.label = "lina";
                got.created = NowMs();
                got.mine = false;
                s3.wanLinks.push_back(got);
                SaveJsonFile(JoinPath(cfg3, "settings.json"), json{{"settings", ToJson(s3, false)}, {"devices", json::array()}});
                A3->configDir = cfg3;
                A3->machineId = "adams-phone";
                A3->Start();  // (straight away: it may register before L has sent the new code)
                std::string idA3 = A3->settings.deviceId;
                CHECK(WaitFor([&] { return sees(*A3, idL) && sees(*L, idA3); }, 25000),
                      "a new person on a phone that was invited before reaches the one who invited them now (A3: %s)", A3->WanStatus().c_str());
                CHECK(!sees(*A3, idC), "...and is still limited to that");
                A3->Stop();
            }
            json inv;
            CHECK(LoadJsonFile(JoinPath(po.dataDir, "invited.json"), inv) && inv["devices"].size() >= 5, "invited devices saved (%s)",
                  inv.dump().c_str());
            // the admin releases A: an ordinary device again
            std::string pubA;
            for (auto& [pub, v] : inv["devices"].items())
                if (v.value("id", "") == idA) pubA = pub;
            std::ofstream(JoinPath(po.dataDir, "invited-cmd")) << "release " << pubA.substr(0, 12) << "\n";
            CHECK(WaitFor([&] { return sees(*A, idC) && sees(*C, idA); }, 25000), "released by the admin");
            {
                // the app is told: a device that joined with an invite stops hiding the server once released
                auto lk = A->Lock();
                Settings s = A->settings;
                s.wanFromInvite = true;
                A->ApplySettings(s);
            }
            CHECK(WaitFor([&] { auto l = A->Lock(); return !A->settings.wanFromInvite; }, 25000), "the released device's app is told (it shows the server now)");
            // the admin blocks A: signed out at once, and kept out
            std::ofstream(JoinPath(po.dataDir, "invited-cmd")) << "block " << pubA.substr(0, 12) << "\n";
            CHECK(WaitFor([&] { return !sees(*C, idA); }, 65000), "a blocked person disappears for the others (within a minute)");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            CHECK(!sees(*C, idA) && A->WanStatus().find("doesn't let you") != std::string::npos, "and can't come back (A: %s)", A->WanStatus().c_str());
            CHECK(LoadJsonFile(JoinPath(po.dataDir, "invited.json"), inv) && inv["devices"][pubA].value("blocked", false), "blocked, saved");
            std::ofstream(JoinPath(po.dataDir, "invited-cmd")) << "unblock " << pubA.substr(0, 12) << "\n";
            CHECK(WaitFor([&] { return sees(*A, idC) && sees(*C, idA); }, 30000), "unblocked: back as before (released)");
            // ... and erases every trace of A: the record, the invite codes it made
            A->Stop();
            std::ofstream(JoinPath(po.dataDir, "invited-cmd")) << "erase " << pubA.substr(0, 12) << "\n";
            CHECK(WaitFor([&] {
                json j;
                if (!LoadJsonFile(JoinPath(po.dataDir, "invited.json"), j) || j["devices"].contains(pubA)) return false;
                json ow = j.value("owners", json::object());
                for (auto& [tok, owner] : ow.items())
                    if (owner == pubA) return false;
                return true;
            }, 15000), "erased from the server");
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
        auto R1 = MakeEngine("rita", 48172, true, false, false, false, "127.0.0.1:48190", "relayroom", {"voice"});
        auto R2 = MakeEngine("rob", 48174, true, false, false, false, "127.0.0.1:48190", "relayroom", {"voice"});
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
        RtCall(*R1, *R2, idR2, true, true, true, "through the server's relay");
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

    printf("[calls: real-time media]\n");
    {
        auto P = MakeEngine("rtpaula", 48240, false, false, false, false, "", "", {"voice"});
        auto Q = MakeEngine("rtquinn", 48244, false, false, false, false, "", "", {"voice"});
        std::string idQ = Q->settings.deviceId;
        P->ConnectAddress("127.0.0.1", Q->boundPort);
        CHECK(WaitFor([&] { return P->IsConnected(idQ); }, 5000), "connected");
        CHECK(P->RtUdpPort() != 0 && Q->RtUdpPort() != 0 && P->RtUdpPort() != Q->RtUdpPort(), "each has a UDP port for call media (%u / %u)",
              P->RtUdpPort(), Q->RtUdpPort());
        RtCall(*P, *Q, idQ, true, true, true, "LAN, both want it");
        RtCall(*P, *Q, idQ, true, false, false, "LAN, only the caller wants it");
        RtCall(*P, *Q, idQ, false, true, false, "LAN, only the one called wants it");
        RtCall(*P, *Q, idQ, false, false, false, "LAN, neither");
        P->Stop();
        Q->Stop();
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
