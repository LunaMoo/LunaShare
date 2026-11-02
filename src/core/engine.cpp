#include "engine.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "hash.h"
#include "platform.h"

namespace ls {

static std::string TidHex(uint64_t tid) {
    char b[24];
    snprintf(b, sizeof(b), "%016llx", (unsigned long long)tid);
    return b;
}

static std::string NewUuid() {
    std::string h = RandomHex(16);
    h[12] = '4';
    const char* v = "89ab";
    h[16] = v[strtoul(h.substr(16, 1).c_str(), nullptr, 16) & 3];
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}

// iOS apps may not send/receive broadcast or raw multicast without Apple's multicast entitlement;
// Bonjour (mDNS via the system API) is the supported discovery path there.
static bool BroadcastAllowed() {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    return false;
#else
    return true;
#endif
}

Engine::Engine() {}
Engine::~Engine() { Stop(); }

void Engine::Spawn(std::function<void()> fn) {
    liveThreads_++;
    std::thread([this, fn = std::move(fn)] {
        fn();
        liveThreads_--;
    }).detach();
}

// ============================================================================ lifecycle

bool Engine::Start() {
    if (running_) return true;
    NetInit();
    Load();
    {
        auto lk = Lock();
        if (settings.deviceId.empty()) {
            settings.deviceId = NewUuid();
            MarkDirty();
        }
        if (settings.identitySecret.empty() || !identity_.LoadSeed(settings.identitySecret)) {
            identity_.Generate();
            settings.identitySecret = identity_.seedHex;
            MarkDirty();
        }
        if (settings.displayName.empty()) {
            settings.displayName = UserName() + "@" + HostName();
            MarkDirty();
        }
        if (settings.downloadDir.empty()) settings.downloadDir = DefaultDownloadDir();
        MakeDirs(settings.downloadDir);
    }
    running_ = true;

    uint16_t want = settings.tcpPort ? settings.tcpPort : kDefaultTcpPort;
    for (int i = 0; i < 20 && listener_ == kInvalidSocket; i++) listener_ = TcpListen((uint16_t)(want + i), &boundPort);
    if (listener_ == kInvalidSocket) listener_ = TcpListen(0, &boundPort);
    if (listener_ == kInvalidSocket) listenError = "Could not open a TCP port";

    udp_ = UdpOpen(kDiscoveryPort);
    if (udp_ != kInvalidSocket) UdpJoinMulticast(udp_, kMulticastGroup);

    btSupported = bt_.Supported();
    StartBtServer();
    StartMdns();
    StartWan();

    listenThread_ = std::thread([this] { ListenLoop(); });
    discoveryThread_ = std::thread([this] { DiscoveryLoop(); });
    maintThread_ = std::thread([this] { MaintenanceLoop(); });
    hashThread_ = std::thread([this] { HashLoop(); });
    return listener_ != kInvalidSocket;
}

void Engine::Stop() {
    if (!running_.exchange(false)) return;
    {
        auto lk = Lock();
        for (auto& [id, t] : transfers) t->hashCancel = true;
    }
    hashCv_.notify_all();
    bt_.StopServer();
    mdns_.Stop();
    wan_.Stop();
    std::vector<std::shared_ptr<Connection>> cs;
    {
        auto lk = Lock();
        cs = conns;
    }
    for (auto& c : cs) {
        // polite goodbye so the other side can mark transfers as interrupted immediately
        Queue(c, Msg::Bye, json::object());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (auto& c : cs) CloseConn(c, "shutdown");
    if (listenThread_.joinable()) listenThread_.join();
    if (discoveryThread_.joinable()) discoveryThread_.join();
    if (maintThread_.joinable()) maintThread_.join();
    if (hashThread_.joinable()) hashThread_.join();
    for (int i = 0; i < 100 && liveThreads_ > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CloseSocket(listener_);
    listener_ = kInvalidSocket;
    CloseSocket(udp_);
    udp_ = kInvalidSocket;
    {
        auto lk = Lock();
        for (auto& [id, t] : transfers) {
            CloseFile(*t);
            if (t->state == TState::Active || t->state == TState::Queued) t->state = TState::Interrupted;
        }
        conns.clear();
        online.clear();
    }
    Save();
}

// ============================================================================ persistence

void Engine::Load() {
    auto lk = Lock();
    std::string dir = configDir.empty() ? ConfigDir() : configDir;
    MakeDirs(dir);
    json j;
    if (LoadJsonFile(JoinPath(dir, "settings.json"), j)) {
        FromJson(j.value("settings", json::object()), settings, true);
        if (j.contains("devices") && j["devices"].is_array())
            for (auto& d : j["devices"]) {
                KnownDevice kd = DeviceFromJson(d);
                if (!kd.id.empty()) devices[kd.id] = kd;
            }
    }
    if (LoadJsonFile(JoinPath(dir, "history.json"), j)) {
        if (j.contains("chats") && j["chats"].is_object())
            for (auto& [peer, arr] : j["chats"].items()) {
                Conversation& cv = chats[peer];
                cv.peer = peer;
                for (auto& m : arr) cv.messages.push_back(MessageFromJson(m));
            }
        if (j.contains("pendingDeletes") && j["pendingDeletes"].is_object())
            for (auto& [peer, arr] : j["pendingDeletes"].items())
                for (auto& mid : arr)
                    if (mid.is_string()) chats[peer].pendingDeletes.push_back(mid.get<std::string>());
        if (j.contains("files") && j["files"].is_array())
            for (auto& h : j["files"]) history.push_back(HistoryFromJson(h));
    }
    if (LoadJsonFile(JoinPath(dir, "transfers.json"), j) && j.is_array()) {
        for (auto& tj : j) {
            auto t = std::make_shared<Transfer>();
            TransferFromJson(tj, *t);
            if (!t->tid) continue;
            switch (t->state) {
                case TState::Active:
                case TState::Queued:
                case TState::Offered:
                    t->state = TState::Interrupted;
                    break;
                case TState::Verifying:
                    t->state = TState::Interrupted;  // receiver re-verifies / sender re-asks for the verdict
                    break;
                case TState::Hashing:
                    hashQueue_.push_back(t);
                    break;
                default: break;
            }
            if (!t->outgoing && !t->partPath.empty() && !TStateFinal(t->state)) {
                int64_t sz = FileSize(t->partPath);
                t->done = sz > 0 ? sz : 0;
            }
            transfers[t->tid] = t;
        }
    }
}

void Engine::Save() {
    json js, jh, jt;
    {
        auto lk = Lock();
        dirty_ = false;
        js["settings"] = ToJson(settings, true);
        js["devices"] = json::array();
        for (auto& [id, d] : devices) js["devices"].push_back(ToJson(d));
        jh["chats"] = json::object();
        if (settings.keepHistory) {
            for (auto& [peer, cv] : chats) {
                json arr = json::array();
                for (auto& m : cv.messages) arr.push_back(ToJson(m));
                jh["chats"][peer] = arr;
            }
        }
        jh["pendingDeletes"] = json::object();
        for (auto& [peer, cv] : chats)
            if (!cv.pendingDeletes.empty()) jh["pendingDeletes"][peer] = cv.pendingDeletes;
        jh["files"] = json::array();
        for (auto& h : history) jh["files"].push_back(ToJson(h));
        jt = json::array();
        for (auto& [id, t] : transfers) jt.push_back(ToJson(*t));
    }
    std::string dir = configDir.empty() ? ConfigDir() : configDir;
    MakeDirs(dir);
    SaveJsonFile(JoinPath(dir, "settings.json"), js);
    SaveJsonFile(JoinPath(dir, "history.json"), jh);
    SaveJsonFile(JoinPath(dir, "transfers.json"), jt);
}

// ============================================================================ helpers

KnownDevice& Engine::Device(const std::string& id) {
    auto it = devices.find(id);
    if (it != devices.end()) return it->second;
    KnownDevice& d = devices[id];
    d.id = id;
    d.firstSeen = NowMs();
    return d;
}

std::string Engine::PeerName(const std::string& peer) {
    auto lk = Lock();
    auto it = devices.find(peer);
    if (it != devices.end()) return it->second.DisplayName();
    auto o = online.find(peer);
    if (o != online.end()) return o->second.name;
    return peer.substr(0, 8);
}

std::shared_ptr<Connection> Engine::ReadyConn(const std::string& peer) {
    auto lk = Lock();
    std::shared_ptr<Connection> best;
    for (auto& c : conns) {
        if (!c->ready || c->closed || c->peerId != peer) continue;
        if (!best || TransportRank(c->transport) < TransportRank(best->transport)) best = c;
    }
    return best;
}

std::vector<std::shared_ptr<Transfer>> Engine::BatchTransfers(uint64_t batch) {
    auto lk = Lock();
    std::vector<std::shared_ptr<Transfer>> out;
    for (auto& [id, t] : transfers)
        if (t->batch == batch) out.push_back(t);
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a->created < b->created || (a->created == b->created && a->rel < b->rel); });
    return out;
}

void Engine::Emit(Event e) {
    auto lk = Lock();
    events_.push_back(std::move(e));
    if (events_.size() > 500) events_.erase(events_.begin());
}

std::vector<Event> Engine::PopEvents() {
    auto lk = Lock();
    std::vector<Event> e;
    e.swap(events_);
    return e;
}

std::string Engine::PartDir() {
    std::string d = JoinPath(settings.downloadDir, ".lunashare-partial");
    MakeDirs(d);
    return d;
}

void Engine::CloseFile(Transfer& t) {
    std::lock_guard<std::mutex> l(t.io);
    if (t.fp) {
        fclose(t.fp);
        t.fp = nullptr;
    }
}

void Engine::AddSystemMessage(const std::string& peer, const std::string& text) {
    auto lk = Lock();
    ChatMessage m;
    m.mid = RandomHex(8);
    m.kind = MsgKind::System;
    m.ts = NowMs();
    m.text = text;
    m.delivered = true;
    Conversation& cv = chats[peer];
    cv.peer = peer;
    cv.messages.push_back(m);
    MarkDirty();
}

ChatMessage* Engine::FindBatchMessage(const std::string& peer, uint64_t batch) {
    auto it = chats.find(peer);
    if (it == chats.end()) return nullptr;
    for (auto& m : it->second.messages)
        if (m.kind == MsgKind::Files && m.batch == batch) return &m;
    return nullptr;
}

void Engine::AddHistory(const Transfer& t) {
    if (!settings.keepHistory) return;
    for (auto& h : history)
        if (h.tid == t.tid) return;
    HistoryEntry h;
    h.tid = t.tid;
    h.peer = t.peer;
    h.peerName = PeerName(t.peer);
    h.outgoing = t.outgoing;
    h.name = t.rel.empty() ? t.name : t.rel;
    h.path = t.localPath;
    h.size = t.size;
    h.sha256 = t.outgoing ? t.sha256 : t.recvSha;
    h.verified = !t.recvSha.empty() && t.recvSha == t.sha256;
    h.ts = NowMs();
    history.push_back(h);
    MarkDirty();
}

bool Engine::BatchFinished(uint64_t batch, std::string* name, int* count, int* ok) {
    int n = 0, good = 0;
    for (auto& [id, x] : transfers) {
        if (x->batch != batch) continue;
        if (!TStateFinal(x->state)) return false;
        n++;
        if (x->state == TState::Completed) good++;
        if (name) *name = x->batchName;
    }
    if (count) *count = n;
    if (ok) *ok = good;
    return n > 0;
}

void Engine::EmitIfBatchDone(const std::shared_ptr<Transfer>& t) {
    std::string name;
    int n = 0, good = 0;
    if (!BatchFinished(t->batch, &name, &n, &good)) return;
    if (good == n) Emit({EventType::TransferDone, t->peer, n > 1 ? name : t->name, t->tid});
    else if (good > 0) Emit({EventType::TransferFailed, t->peer, name + ": " + std::to_string(good) + " of " + std::to_string(n) + " files delivered", t->tid});
}

void Engine::SetFinal(const std::shared_ptr<Transfer>& t, TState st, const std::string& err) {
    auto lk = Lock();
    t->state = st;
    t->error = err;
    t->finished = NowMs();
    t->speed = 0;
    t->hashCancel = true;
    CloseFile(*t);
    if (st == TState::Completed) AddHistory(*t);
    if (!t->outgoing && st != TState::Completed && !t->partPath.empty()) RemoveFile(t->partPath);
    MarkDirty();
    if (t->outgoing) PumpQueued(t->peer);
}

void Engine::Queue(const std::shared_ptr<Connection>& c, Msg m, const json& j) {
    if (!c || c->closed) return;
    {
        std::lock_guard<std::mutex> l(c->qm);
        c->ctrl.emplace_back(m, j.dump(-1, ' ', false, json::error_handler_t::replace));
    }
    c->qcv.notify_one();
}

bool Engine::QueueToPeer(const std::string& peer, Msg m, const json& j) {
    auto c = ReadyConn(peer);
    if (!c) return false;
    Queue(c, m, j);
    return true;
}

// ============================================================================ connections

void Engine::ListenLoop() {
    while (running_) {
        if (restartListener_.exchange(false)) {
            uint16_t newPort = 0;
            SocketHandle n = kInvalidSocket;
            uint16_t want;
            {
                auto lk = Lock();
                want = settings.tcpPort ? settings.tcpPort : kDefaultTcpPort;
            }
            n = TcpListen(want, &newPort);
            if (n != kInvalidSocket) {
                CloseSocket(listener_);
                listener_ = n;
                auto lk = Lock();
                boundPort = newPort;
                listenError.clear();
                mdns_.Update(settings.displayName, boundPort);
            } else {
                auto lk = Lock();
                listenError = "Port " + std::to_string(want) + " is busy, still using " + std::to_string(boundPort);
            }
        }
        if (listener_ == kInvalidSocket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }
        std::string ip;
        SocketHandle s = TcpAccept(listener_, &ip, 300);
        if (s != kInvalidSocket) AdoptSocket(s, Transport::LAN, ip, false);
    }
}

void Engine::AdoptSocket(SocketHandle s, Transport t, const std::string& addr, bool initiator) {
    if (!running_) { CloseSocket(s); return; }
    auto c = std::make_shared<Connection>();
    c->cid = nextCid_++;
    c->sock = s;
    c->transport = t;
    c->addr = addr;
    c->initiator = initiator;
    c->connectedAt = SteadyMs();
    c->lastRecv = SteadyMs();
    // Key exchange first (blocking, so on its own thread); only then does the connection exist for the engine.
    Spawn([this, c] {
        c->sec = std::make_unique<SecureChannel>();
        std::string err;
        int timeout = c->transport == Transport::LAN ? 8000 : 20000;
        if (!running_ || !c->sec->Handshake(c->sock, c->initiator, timeout, &err, &running_)) {
            if (c->initiator && running_) Emit({EventType::Error, "", "Secure handshake with " + c->addr + " failed: " + err});
            CloseSocket(c->sock);
            return;
        }
        c->lastRecv = SteadyMs();
        c->connectedAt = SteadyMs();
        json hello;
        {
            auto lk = Lock();
            conns.push_back(c);
            hello = {{"app", kAppId}, {"proto", kProtocolVersion}, {"id", settings.deviceId}, {"name", settings.displayName},
                     {"user", UserName()}, {"host", HostName()}, {"platform", PlatformName()}, {"macs", MacAddresses()},
                     {"tcpPort", boundPort}, {"pub", identity_.PubHex()}, {"sig", c->sec->Sign(identity_)}, {"caps", localCaps}};
        }
        Queue(c, Msg::Hello, hello);
        Spawn([this, c] { ReaderLoop(c); });
        Spawn([this, c] { WriterLoop(c); });
    });
}

void Engine::CloseConn(const std::shared_ptr<Connection>& c, const char* why) {
    (void)why;
    if (c->closed.exchange(true)) return;
    ShutdownSocket(c->sock);
    c->qcv.notify_all();
    auto lk = Lock();
    if (c->ready) {
        bool other = false;
        for (auto& o : conns)
            if (o != c && o->ready && !o->closed && o->peerId == c->peerId) other = true;
        if (!other) {
            if (call.state != CallState::None && call.peer == c->peerId) FinishCall("connection lost", false);
            InterruptPeerTransfers(c->peerId);
            Device(c->peerId).lastSeen = NowMs();
        } else {
            // transfers that were streaming on this connection move to the surviving one via re-offer
            for (auto& [id, t] : transfers) {
                if (t->peer != c->peerId || !t->outgoing) continue;
                if (t->state == TState::Active || t->state == TState::Queued) {
                    t->state = TState::Interrupted;
                    CloseFile(*t);
                    OfferTransfer(t, false);
                }
            }
        }
    }
    MarkDirty();
}

void Engine::InterruptPeerTransfers(const std::string& peer) {
    for (auto& [id, t] : transfers) {
        if (t->peer != peer) continue;
        bool interrupt = t->state == TState::Active || t->state == TState::Queued ||
                         (t->outgoing && (t->state == TState::Offered || t->state == TState::Verifying));
        if (interrupt) {
            t->state = TState::Interrupted;
            t->speed = 0;
            CloseFile(*t);
        }
    }
}

void Engine::ReaderLoop(std::shared_ptr<Connection> c) {
    Frame f;
    while (!c->closed && c->sec->Recv(c->sock, f)) {
        c->lastRecv = SteadyMs();
        c->bytesIn += (int64_t)f.payload.size() + 5;
        if (!c->ready && f.type != Msg::Hello) break;
        if (f.type == Msg::FileData) {
            HandleData(c, f);
            continue;
        }
        if (f.type == Msg::Media) {
            if (f.payload.size() > 8 && c->peerId == activeCallPeerSnapshot()) {
                uint64_t id = GetU64(f.payload.data());
                if (id && id == activeCallId_.load() && onMedia) onMedia(id, f.payload.data() + 8, f.payload.size() - 8);
            }
            continue;
        }
        if (f.type == Msg::Bye) break;
        HandleFrame(c, f);
    }
    CloseConn(c, "reader");
    if (--c->alive == 0) CloseSocket(c->sock);
}

void Engine::WriterLoop(std::shared_ptr<Connection> c) {
    std::vector<uint8_t> buf;
    while (!c->closed) {
        std::pair<Msg, std::string> msg;
        bool haveCtrl = false, haveData = false;
        {
            std::unique_lock<std::mutex> l(c->qm);
            c->qcv.wait_for(l, std::chrono::milliseconds(200), [&] { return c->closed || !c->ctrl.empty() || !c->sending.empty(); });
            if (c->closed) break;
            if (!c->ctrl.empty()) {
                msg = std::move(c->ctrl.front());
                c->ctrl.pop_front();
                haveCtrl = true;
            } else {
                haveData = !c->sending.empty();
            }
        }
        if (haveCtrl) {
            if (!c->sec->Send(c->sock, msg.first, msg.second)) break;
            c->bytesOut += (int64_t)msg.second.size() + 5;
            continue;
        }
        if (!haveData) continue;
        int64_t wait = 0;
        if (ProduceChunk(c, buf, wait)) {
            if (!c->sec->Send(c->sock, Msg::FileData, buf.data(), buf.size())) break;
            c->bytesOut += (int64_t)buf.size() + 5;
            totalUp += (int64_t)(buf.size() - kDataHeader);
        } else if (wait > 0) {
            std::unique_lock<std::mutex> l(c->qm);
            c->qcv.wait_for(l, std::chrono::milliseconds(std::min<int64_t>(wait, 50)), [&] { return c->closed || !c->ctrl.empty(); });
        }
    }
    CloseConn(c, "writer");
    if (--c->alive == 0) CloseSocket(c->sock);
}

bool Engine::ProduceChunk(const std::shared_ptr<Connection>& c, std::vector<uint8_t>& buf, int64_t& wait) {
    uint64_t tid;
    {
        std::lock_guard<std::mutex> l(c->qm);
        if (c->sending.empty()) return false;
        c->rr %= c->sending.size();
        tid = c->sending[c->rr++];
    }
    auto removeTid = [&] {
        std::lock_guard<std::mutex> l(c->qm);
        c->sending.erase(std::remove(c->sending.begin(), c->sending.end(), tid), c->sending.end());
    };
    std::shared_ptr<Transfer> t;
    size_t chunk;
    int64_t limit;
    {
        auto lk = Lock();
        auto it = transfers.find(tid);
        if (it == transfers.end() || it->second->state != TState::Active || it->second->peer != c->peerId) {
            removeTid();
            return false;
        }
        t = it->second;
        chunk = (size_t)settings.chunkKB * 1024;
        limit = settings.speedLimitKBs;
    }
    if (c->transport == Transport::Bluetooth && chunk > 32 * 1024) chunk = 32 * 1024;  // keep BT latency low
    if (c->callPriority) {
        // Voice frames share this connection: small chunks so they never wait behind a big one, and over
        // internet/Bluetooth a gentle rate cap (~200 KB/s) so queues stay short.
        chunk = std::min<size_t>(chunk, c->transport == Transport::LAN ? 64 * 1024 : 16 * 1024);
        if (c->transport != Transport::LAN) {
            int64_t since = SteadyMs() - c->lastChunkMs;
            if (since < 80) {
                wait = 80 - since;
                return false;
            }
        }
        c->lastChunkMs = SteadyMs();
    }
    if (limit > 0) {
        std::lock_guard<std::mutex> l(throttleMtx_);
        double rate = (double)limit * 1024.0;
        int64_t now = SteadyMs();
        if (tokenTs_ == 0) tokenTs_ = now;
        tokens_ += rate * (double)(now - tokenTs_) / 1000.0;
        tokenTs_ = now;
        double cap = std::max(rate / 4.0, (double)chunk);
        if (tokens_ > cap) tokens_ = cap;
        if (chunk > rate / 4 && rate / 4 >= 4096) chunk = (size_t)(rate / 4);
        if (tokens_ < (double)chunk) {
            wait = (int64_t)(((double)chunk - tokens_) / rate * 1000.0) + 1;
            return false;
        }
        tokens_ -= (double)chunk;
    }
    bool eof = false, ioError = false;
    {
        std::lock_guard<std::mutex> l(t->io);
        if (!t->fp) {
            ioError = true;
        } else if (t->sendOffset >= t->size) {
            eof = true;
        } else {
            int64_t remain = t->size - t->sendOffset;
            size_t want = (size_t)std::min<int64_t>((int64_t)chunk, remain);
            buf.resize(kDataHeader + want);
            size_t n = fread(buf.data() + kDataHeader, 1, want, t->fp);
            if (n != want) {
                ioError = true;
            } else {
                PutU64(buf.data(), tid);
                PutU64(buf.data() + 8, (uint64_t)t->sendOffset);
                PutU32(buf.data() + 16, Crc32(buf.data() + kDataHeader, n));
                t->sendOffset += (int64_t)n;
                t->done = t->sendOffset;
            }
        }
    }
    if (ioError) {
        removeTid();
        SetFinal(t, TState::Failed, "Cannot read source file");
        Queue(c, Msg::FileCancel, {{"tid", tid}, {"reason", "sender could not read the file"}});
        return false;
    }
    if (eof) {
        removeTid();
        auto lk = Lock();
        if (t->state == TState::Active) {
            t->state = TState::Verifying;
            t->speed = 0;
            CloseFile(*t);
            Queue(c, Msg::FileDone, {{"tid", tid}});
            MarkDirty();
            PumpQueued(t->peer);
        }
        return false;
    }
    return true;
}

// ============================================================================ frames

void Engine::HandleFrame(const std::shared_ptr<Connection>& c, Frame& f) {
    json j = json::parse(f.Text(), nullptr, false);
    if (j.is_discarded()) j = json::object();
    auto lk = Lock();
    switch (f.type) {
        case Msg::Hello: {
            std::string id = j.value("id", "");
            if (j.value("app", "") != kAppId || id.empty() || id == settings.deviceId) {
                CloseConn(c, "bad hello");
                return;
            }
            // Authenticate: the peer must prove it owns `pub` and is the other end of THIS key exchange.
            std::string pub = j.value("pub", ""), sig = j.value("sig", "");
            if (!c->sec->VerifyPeer(pub, sig)) {
                Emit({EventType::Error, "", "Rejected " + c->addr + ": invalid identity signature"});
                CloseConn(c, "bad signature");
                return;
            }
            bool isNew = devices.find(id) == devices.end();
            KnownDevice& d = Device(id);
            if (d.blocked) {
                CloseConn(c, "blocked");
                return;
            }
            if (!d.pubKey.empty() && d.pubKey != pub) {
                // Same device id, different key: reinstalled app, or someone impersonating it. Never connect silently.
                bool firstWarning = d.pendingKey != pub;
                d.pendingKey = pub;
                MarkDirty();
                if (firstWarning) {
                    Emit({EventType::Error, id, "SECURITY: \"" + d.DisplayName() + "\" presented a different identity key. Connection refused - open the device details to review."});
                    AddSystemMessage(id, "Identity key changed (new fingerprint " + KeyFingerprint(pub) + "). Connection refused until you accept the new key in Devices.");
                }
                CloseConn(c, "key mismatch");
                return;
            }
            if (d.pubKey.empty()) {
                d.pubKey = pub;  // trust on first use
                d.verified = false;
                AddSystemMessage(id, "End-to-end encrypted. Safety number: " + SafetyNumber(identity_.PubHex(), pub) +
                                         " - compare it on both devices to make sure nobody is in between.");
            }
            d.pendingKey.clear();
            d.name = j.value("name", d.name);
            d.user = j.value("user", d.user);
            d.host = j.value("host", d.host);
            d.platform = j.value("platform", d.platform);
            if (j.contains("macs") && j["macs"].is_array()) {
                std::vector<std::string> macs;
                for (auto& m : j["macs"]) if (m.is_string()) macs.push_back(m.get<std::string>());
                if (!macs.empty()) d.macs = macs;
            }
            if (c->transport == Transport::LAN) {
                d.lastIp = c->addr;
                d.lastPort = (uint16_t)j.value("tcpPort", (int)kDefaultTcpPort);
            } else if (c->transport == Transport::Bluetooth) {
                d.btAddr = c->addr;
            }
            d.lastSeen = NowMs();
            if (isNew && !d.macs.empty()) {
                // Same hardware, new identity (app reinstalled / config wiped)? Carry the nickname over.
                for (auto& [oid, od] : devices) {
                    if (oid == id || od.customName.empty()) continue;
                    bool match = false;
                    for (auto& m : d.macs)
                        if (std::find(od.macs.begin(), od.macs.end(), m) != od.macs.end()) match = true;
                    if (match) {
                        d.customName = od.customName;
                        AddSystemMessage(id, "Recognised hardware of \"" + od.DisplayName() + "\" (new app identity) - nickname copied.");
                        break;
                    }
                }
            }
            c->caps.clear();
            for (auto& cp : j.value("caps", json::array()))
                if (cp.is_string()) c->caps.push_back(cp.get<std::string>());
            c->peerId = id;
            c->ready = true;
            MarkDirty();
            // De-duplicate parallel connections to the same device.
            for (auto& o : conns) {
                if (o == c || !o->ready || o->closed || o->peerId != id) continue;
                std::shared_ptr<Connection> loser;
                if (o->transport != c->transport) {
                    loser = TransportRank(c->transport) > TransportRank(o->transport) ? c : o;
                } else {
                    auto initId = [&](const std::shared_ptr<Connection>& x) { return x->initiator ? settings.deviceId : id; };
                    std::string want = std::min(settings.deviceId, id);
                    bool cGood = initId(c) == want, oGood = initId(o) == want;
                    loser = (cGood && !oGood) ? o : c;
                }
                if (loser == o && o->transport == c->transport) {
                    CloseConn(o, "duplicate");
                } else if (loser == c && c->transport == o->transport) {
                    c->ready = false;
                    CloseConn(c, "duplicate");
                    return;
                }
                // different transports: keep both, LAN preferred by ReadyConn()
            }
            OnReady(c);
            break;
        }
        case Msg::Ping:
            Queue(c, Msg::Pong, json::object());
            break;
        case Msg::Pong:
            break;
        case Msg::Chat: {
            std::string mid = j.value("mid", "");
            Conversation& cv = chats[c->peerId];
            cv.peer = c->peerId;
            bool dup = false;
            for (auto& m : cv.messages)
                if (m.mid == mid && !m.outgoing) dup = true;
            if (!dup && !mid.empty()) {
                ChatMessage m;
                m.mid = mid;
                m.ts = NowMs();
                m.text = j.value("text", "");
                m.delivered = true;
                cv.messages.push_back(m);
                cv.unread++;
                Emit({EventType::ChatReceived, c->peerId, m.text});
                MarkDirty();
            }
            Queue(c, Msg::ChatAck, {{"mid", mid}});
            break;
        }
        case Msg::ChatDelete: {
            // only messages this device sent us can be removed this way
            json mids = j.value("mids", json::array());
            auto it = chats.find(c->peerId);
            if (it != chats.end() && mids.is_array()) {
                auto& msgs = it->second.messages;
                size_t before = msgs.size();
                msgs.erase(std::remove_if(msgs.begin(), msgs.end(), [&](const ChatMessage& m) {
                               if (m.outgoing || m.kind == MsgKind::System) return false;
                               for (auto& x : mids)
                                   if (x.is_string() && x.get<std::string>() == m.mid) return true;
                               return false;
                           }),
                           msgs.end());
                if (msgs.size() != before) {
                    it->second.unread = std::max(0, it->second.unread - (int)(before - msgs.size()));
                    Emit({EventType::ChatDeleted, c->peerId, ""});
                    MarkDirty();
                }
            }
            Queue(c, Msg::ChatDeleteAck, {{"mids", mids}});
            break;
        }
        case Msg::ChatDeleteAck: {
            json mids = j.value("mids", json::array());
            auto it = chats.find(c->peerId);
            if (it != chats.end() && mids.is_array()) {
                auto& pd = it->second.pendingDeletes;
                for (auto& x : mids)
                    if (x.is_string()) pd.erase(std::remove(pd.begin(), pd.end(), x.get<std::string>()), pd.end());
                MarkDirty();
            }
            break;
        }
        case Msg::ChatReact: {
            std::string mid = j.value("mid", ""), emoji = j.value("emoji", "");
            if (emoji.size() > 64) break;  // one emoji, not a message
            auto it = chats.find(c->peerId);
            if (it != chats.end())
                for (auto& m : it->second.messages)
                    if (m.mid == mid && m.kind != MsgKind::System) {
                        if (m.theirReaction != emoji) {
                            m.theirReaction = emoji;
                            MarkDirty();
                            if (!emoji.empty()) {
                                std::string about = m.kind == MsgKind::Files ? "[files] " + m.text : m.text;
                                if (about.size() > 60) about = about.substr(0, 57) + "...";
                                Event ev{EventType::ChatReaction, c->peerId, emoji};
                                ev.a = about;
                                Emit(ev);
                            }
                        }
                        break;
                    }
            Queue(c, Msg::ChatReactAck, {{"mid", mid}, {"emoji", emoji}});
            break;
        }
        case Msg::ChatReactAck: {
            std::string mid = j.value("mid", ""), emoji = j.value("emoji", "");
            auto it = chats.find(c->peerId);
            if (it != chats.end())
                for (auto& m : it->second.messages)
                    if (m.mid == mid && m.myReaction == emoji && !m.reactionSent) {
                        m.reactionSent = true;
                        MarkDirty();
                    }
            break;
        }
        case Msg::ChatAck: {
            std::string mid = j.value("mid", "");
            auto it = chats.find(c->peerId);
            if (it != chats.end())
                for (auto& m : it->second.messages)
                    if (m.outgoing && m.mid == mid) { m.delivered = true; MarkDirty(); }
            break;
        }
        case Msg::FileOffer: {
            uint64_t tid = j.value("tid", (uint64_t)0);
            if (!tid) break;
            bool resume = j.value("resume", false);
            bool peerPaused = j.value("paused", false);
            auto it = transfers.find(tid);
            if (it != transfers.end()) {
                auto t = it->second;
                if (t->outgoing || t->peer != c->peerId) break;
                switch (t->state) {
                    case TState::Completed: Queue(c, Msg::FileVerify, {{"tid", tid}, {"ok", true}, {"sha256", t->recvSha}}); break;
                    case TState::Failed: Queue(c, Msg::FileVerify, {{"tid", tid}, {"ok", false}, {"sha256", t->recvSha}}); break;
                    case TState::Rejected: Queue(c, Msg::FileReject, {{"tid", tid}, {"reason", "declined"}}); break;
                    case TState::Cancelled: Queue(c, Msg::FileCancel, {{"tid", tid}, {"reason", "cancelled"}}); break;
                    case TState::Pending: break;
                    case TState::Verifying: break;  // hashing in progress, verdict follows
                    case TState::Paused:
                        if (resume || (t->pausedByPeer && !peerPaused)) SendAcceptFor(t);
                        else if (!peerPaused) Queue(c, Msg::FilePause, {{"tid", tid}});
                        break;
                    default:  // Active / Queued / Interrupted
                        if (peerPaused) {
                            t->state = TState::Paused;
                            t->pausedByPeer = true;
                            CloseFile(*t);
                        } else if (resume || settings.autoResume || t->wantResume) {
                            SendAcceptFor(t);
                        } else {
                            t->state = TState::Interrupted;
                        }
                        break;
                }
                MarkDirty();
                break;
            }
            auto t = std::make_shared<Transfer>();
            t->tid = tid;
            t->peer = c->peerId;
            t->outgoing = false;
            t->rel = SanitizeRelativePath(j.value("rel", j.value("name", "file")));
            t->name = BaseName(t->rel);
            t->size = j.value("size", (int64_t)0);
            t->mtime = j.value("mtime", (int64_t)0);
            t->sha256 = j.value("sha256", "");
            t->batch = j.value("batch", (uint64_t)tid);
            t->batchName = j.value("batchName", t->name);
            t->batchCount = j.value("batchCount", 1);
            t->created = NowMs();
            t->partPath = JoinPath(PartDir(), TidHex(tid) + ".part");
            t->state = peerPaused ? TState::Paused : TState::Pending;
            t->pausedByPeer = peerPaused;
            if (t->size < 0 || t->sha256.size() != 64) {
                Queue(c, Msg::FileReject, {{"tid", tid}, {"reason", "invalid offer"}});
                break;
            }
            int64_t existing = FileSize(t->partPath);
            t->done = existing > 0 ? existing : 0;
            transfers[tid] = t;

            ChatMessage* bm = FindBatchMessage(c->peerId, t->batch);
            bool firstOfBatch = bm == nullptr;
            if (!bm) {
                ChatMessage m;
                m.mid = "b" + TidHex(t->batch);
                m.kind = MsgKind::Files;
                m.ts = NowMs();
                m.batch = t->batch;
                m.text = t->batchName;
                m.delivered = true;
                Conversation& cv = chats[c->peerId];
                cv.peer = c->peerId;
                cv.messages.push_back(m);
                cv.unread++;
                bm = &cv.messages.back();
            }
            if (std::find(bm->tids.begin(), bm->tids.end(), tid) == bm->tids.end()) bm->tids.push_back(tid);

            std::error_code ec;
            auto space = std::filesystem::space(std::filesystem::u8path(settings.downloadDir), ec);
            if (!ec && (int64_t)space.available < t->size - t->done.load()) {
                t->state = TState::Rejected;
                t->error = "Not enough free space";
                Queue(c, Msg::FileReject, {{"tid", tid}, {"reason", "not enough free space on the receiving device"}});
                MarkDirty();
                break;
            }
            KnownDevice& d = Device(c->peerId);
            bool autoAccept = settings.autoAcceptAll || (d.trusted && settings.autoAcceptTrusted);
            if (t->state == TState::Pending && autoAccept) SendAcceptFor(t);
            else if (firstOfBatch) Emit({EventType::IncomingOffer, c->peerId, t->batchName, tid});
            MarkDirty();
            break;
        }
        case Msg::FileAccept: {
            uint64_t tid = j.value("tid", (uint64_t)0);
            int64_t offset = j.value("offset", (int64_t)0);
            auto it = transfers.find(tid);
            if (it == transfers.end() || !it->second->outgoing || it->second->peer != c->peerId) break;
            auto t = it->second;
            if (TStateFinal(t->state)) {
                if (t->state == TState::Cancelled) Queue(c, Msg::FileCancel, {{"tid", tid}, {"reason", "cancelled by sender"}});
                break;
            }
            if (t->state == TState::Paused && !t->pausedByPeer && !j.value("resume", false)) {  // paused by our user: keep the peer in sync
                Queue(c, Msg::FilePause, {{"tid", tid}});
                break;
            }
            if (t->state == TState::Hashing) break;
            if (offset < 0 || offset > t->size) offset = 0;
            if (FileSize(t->localPath) != t->size || FileMTime(t->localPath) != t->mtime) {
                SetFinal(t, TState::Failed, "Source file changed or was removed since it was shared");
                Queue(c, Msg::FileCancel, {{"tid", tid}, {"reason", "source file changed"}});
                break;
            }
            t->pausedByPeer = false;
            t->wantResume = false;
            bool streaming;
            int active = 0;
            {
                std::lock_guard<std::mutex> l(c->qm);
                streaming = std::find(c->sending.begin(), c->sending.end(), tid) != c->sending.end();
                active = (int)c->sending.size();
            }
            if (streaming || active < settings.maxParallel) {
                StartStreaming(t, offset);
            } else {
                t->state = TState::Queued;
                t->sendOffset = offset;
                t->done = offset;
            }
            MarkDirty();
            break;
        }
        case Msg::FileReject: {
            auto it = transfers.find(j.value("tid", (uint64_t)0));
            if (it == transfers.end() || it->second->peer != c->peerId || TStateFinal(it->second->state)) break;
            SetFinal(it->second, TState::Rejected, j.value("reason", "declined"));
            Emit({EventType::TransferFailed, c->peerId, it->second->name + " was declined", it->second->tid});
            break;
        }
        case Msg::FileCancel: {
            auto it = transfers.find(j.value("tid", (uint64_t)0));
            if (it == transfers.end() || it->second->peer != c->peerId || TStateFinal(it->second->state)) break;
            SetFinal(it->second, TState::Cancelled, "Cancelled by the other device: " + j.value("reason", std::string("")));
            break;
        }
        case Msg::FilePause: {
            auto it = transfers.find(j.value("tid", (uint64_t)0));
            if (it == transfers.end() || it->second->peer != c->peerId) break;
            auto t = it->second;
            if (TStateFinal(t->state) || t->state == TState::Verifying || t->state == TState::Hashing) break;
            t->state = TState::Paused;
            t->pausedByPeer = true;
            t->speed = 0;
            CloseFile(*t);
            if (t->outgoing) PumpQueued(t->peer);
            MarkDirty();
            break;
        }
        case Msg::FileResume: {
            auto it = transfers.find(j.value("tid", (uint64_t)0));
            if (it == transfers.end() || it->second->peer != c->peerId) break;
            if (!it->second->outgoing && it->second->state == TState::Paused) SendAcceptFor(it->second);
            break;
        }
        case Msg::FileDone: {
            auto it = transfers.find(j.value("tid", (uint64_t)0));
            if (it == transfers.end() || it->second->outgoing || it->second->peer != c->peerId) break;
            auto t = it->second;
            if (t->state != TState::Active) break;
            if (t->done.load() != t->size) {
                SendAcceptFor(t);  // something went missing - ask again from what we have
                break;
            }
            t->state = TState::Verifying;
            t->speed = 0;
            t->hashDone = 0;
            CloseFile(*t);
            if (t->size == 0 && !FileExists(t->partPath)) {
                MakeDirs(DirName(t->partPath));
                if (FILE* ef = OpenFile(t->partPath, "wb")) fclose(ef);
            }
            MarkDirty();
            Spawn([this, t] { FinishIncoming(t); });
            break;
        }
        case Msg::FileVerify: {
            auto it = transfers.find(j.value("tid", (uint64_t)0));
            if (it == transfers.end() || !it->second->outgoing || it->second->peer != c->peerId) break;
            auto t = it->second;
            if (TStateFinal(t->state)) break;
            t->recvSha = j.value("sha256", "");
            bool ok = j.value("ok", false) && t->recvSha == t->sha256;
            t->done = t->size;
            if (ok) {
                SetFinal(t, TState::Completed, "");
                EmitIfBatchDone(t);
            } else {
                SetFinal(t, TState::Failed, "The receiver's SHA-256 did not match - file was corrupted in transit");
                Emit({EventType::TransferFailed, c->peerId, t->name + ": hash mismatch", t->tid});
            }
            break;
        }
        case Msg::CallInvite: {
            uint64_t id = j.value("call", (uint64_t)0);
            if (!id) break;
            std::string media = j.value("media", "voice");
            bool supported = std::find(localCaps.begin(), localCaps.end(), CapForMedia(media)) != localCaps.end();
            if (!supported) {
                Queue(c, Msg::CallEnd, {{"call", id}, {"reason", "unsupported"}});
                AddSystemMessage(c->peerId, "Missed call - calls are not installed on this device (voice module missing)");
                break;
            }
            if (call.state != CallState::None) {
                if (call.id == id) break;  // duplicate
                Queue(c, Msg::CallEnd, {{"call", id}, {"reason", "busy"}});
                AddSystemMessage(c->peerId, "Missed call (you were in another call)");
                break;
            }
            call = CallInfo();
            call.id = id;
            call.peer = c->peerId;
            call.outgoing = false;
            call.state = CallState::IncomingRinging;
            call.started = SteadyMs();
            call.media = media;
            Emit({EventType::CallIncoming, c->peerId, "Incoming call", id});
            break;
        }
        case Msg::CallAccept: {
            uint64_t id = j.value("call", (uint64_t)0);
            if (call.id != id || call.state != CallState::OutgoingRinging || call.peer != c->peerId) break;
            call.state = CallState::Active;
            call.answered = SteadyMs();
            {
                std::lock_guard<std::mutex> l(mediaMtx_);
                activeCallPeer_ = call.peer;
                activeCallConn_ = ReadyConn(call.peer);
            }
            activeCallId_ = id;
            c->callPriority = true;
            Emit({EventType::CallStarted, c->peerId, "Call started", id});
            break;
        }
        case Msg::CallEnd: {
            uint64_t id = j.value("call", (uint64_t)0);
            if (call.id != id || call.peer != c->peerId) break;
            FinishCall(j.value("reason", "hangup"), false);
            break;
        }
        case Msg::WifiCreds:
            Emit({EventType::WifiCreds, c->peerId, "Wi-Fi hotspot offered", 0, j.value("ssid", ""), j.value("pass", "")});
            break;
        default:
            break;
    }
}

void Engine::HandleData(const std::shared_ptr<Connection>& c, const Frame& f) {
    if (f.payload.size() < kDataHeader) return;
    const uint8_t* p = f.payload.data();
    uint64_t tid = GetU64(p);
    int64_t off = (int64_t)GetU64(p + 8);
    uint32_t crc = GetU32(p + 16);
    const uint8_t* data = p + kDataHeader;
    size_t n = f.payload.size() - kDataHeader;
    std::shared_ptr<Transfer> t;
    {
        auto lk = Lock();
        auto it = transfers.find(tid);
        if (it == transfers.end() || it->second->outgoing || it->second->peer != c->peerId) return;
        t = it->second;
        if (t->state != TState::Active) return;
    }
    bool resync = false;
    std::string writeError;
    {
        std::lock_guard<std::mutex> l(t->io);
        if (off != t->done.load() || Crc32(data, n) != crc || off + (int64_t)n > t->size) {
            if (!t->resyncRequested) {
                t->resyncRequested = true;
                resync = true;
            }
        } else {
            if (!t->fp) {
                MakeDirs(DirName(t->partPath));
                t->fp = OpenFile(t->partPath, FileExists(t->partPath) ? "r+b" : "wb");
                if (t->fp && !FileSeek(t->fp, off)) { fclose(t->fp); t->fp = nullptr; }
            }
            if (!t->fp) {
                writeError = "Cannot write to " + t->partPath;
            } else if (fwrite(data, 1, n, t->fp) != n) {
                writeError = "Disk write failed (disk full?)";
            } else {
                int64_t before = t->done.load();
                t->done = off + (int64_t)n;
                t->resyncRequested = false;
                if ((before >> 25) != (t->done.load() >> 25)) fflush(t->fp);  // flush every 32 MiB for crash safety
            }
        }
    }
    totalDown += (int64_t)n;
    if (!writeError.empty()) {
        Queue(c, Msg::FileCancel, {{"tid", tid}, {"reason", "receiver: " + writeError}});
        SetFinal(t, TState::Failed, writeError);
        Emit({EventType::TransferFailed, c->peerId, t->name + ": " + writeError, tid});
        return;
    }
    if (resync) {
        auto lk = Lock();
        Queue(c, Msg::FileAccept, {{"tid", tid}, {"offset", t->done.load()}});
    }
}

void Engine::OnReady(const std::shared_ptr<Connection>& c) {
    const std::string& peer = c->peerId;
    Emit({EventType::PeerOnline, peer, PeerName(peer)});
    auto it = chats.find(peer);
    if (it != chats.end()) {
        for (auto& m : it->second.messages)
            if (m.outgoing && !m.delivered && m.kind == MsgKind::Text)
                Queue(c, Msg::Chat, {{"mid", m.mid}, {"ts", m.ts}, {"text", m.text}});
        if (!it->second.pendingDeletes.empty()) Queue(c, Msg::ChatDelete, {{"mids", it->second.pendingDeletes}});
        for (auto& m : it->second.messages)
            if (!m.reactionSent) Queue(c, Msg::ChatReact, {{"mid", m.mid}, {"emoji", m.myReaction}});
    }
    std::vector<std::shared_ptr<Transfer>> outs;
    for (auto& [id, t] : transfers)
        if (t->peer == peer && t->outgoing && !TStateFinal(t->state) && t->state != TState::Hashing) outs.push_back(t);
    std::sort(outs.begin(), outs.end(), [](auto& a, auto& b) { return a->created < b->created; });
    for (auto& t : outs) {
        if (t->state != TState::Paused) t->state = TState::Offered;
        OfferTransfer(t, t->wantResume);
    }
}

void Engine::OfferTransfer(const std::shared_ptr<Transfer>& t, bool resumeRequest) {
    json j = {{"tid", t->tid}, {"name", t->name}, {"rel", t->rel}, {"size", t->size}, {"sha256", t->sha256},
              {"mtime", t->mtime}, {"batch", t->batch}, {"batchName", t->batchName}, {"batchCount", t->batchCount},
              {"paused", t->state == TState::Paused}, {"resume", resumeRequest}};
    if (!QueueToPeer(t->peer, Msg::FileOffer, j) && t->state == TState::Offered) t->state = TState::Interrupted;
}

void Engine::StartStreaming(const std::shared_ptr<Transfer>& t, int64_t offset) {
    auto c = ReadyConn(t->peer);
    if (!c) {
        t->state = TState::Interrupted;
        return;
    }
    bool ok;
    {
        std::lock_guard<std::mutex> l(t->io);
        if (!t->fp) t->fp = OpenFile(t->localPath, "rb");
        ok = t->fp && FileSeek(t->fp, offset);
        t->sendOffset = offset;
        t->done = offset;
    }
    if (!ok) {
        SetFinal(t, TState::Failed, "Cannot open source file");
        Queue(c, Msg::FileCancel, {{"tid", t->tid}, {"reason", "sender could not open the file"}});
        return;
    }
    t->state = TState::Active;
    t->lastDone = offset;
    t->lastTick = SteadyMs();
    {
        std::lock_guard<std::mutex> l(c->qm);
        if (std::find(c->sending.begin(), c->sending.end(), t->tid) == c->sending.end()) c->sending.push_back(t->tid);
    }
    c->qcv.notify_one();
}

void Engine::PumpQueued(const std::string& peer) {
    auto c = ReadyConn(peer);
    if (!c) return;
    int active;
    {
        std::lock_guard<std::mutex> l(c->qm);
        active = (int)c->sending.size();
    }
    std::vector<std::shared_ptr<Transfer>> q;
    for (auto& [id, t] : transfers)
        if (t->peer == peer && t->outgoing && t->state == TState::Queued) q.push_back(t);
    std::sort(q.begin(), q.end(), [](auto& a, auto& b) { return a->created < b->created; });
    for (auto& t : q) {
        if (active >= settings.maxParallel) break;
        StartStreaming(t, t->sendOffset);
        active++;
    }
}

void Engine::SendAcceptFor(const std::shared_ptr<Transfer>& t, bool userResume) {
    auto c = ReadyConn(t->peer);
    int64_t offset = FileSize(t->partPath);
    if (offset < 0) offset = 0;
    if (offset > t->size) {
        RemoveFile(t->partPath);
        offset = 0;
    }
    {
        std::lock_guard<std::mutex> l(t->io);
        if (t->fp) { fclose(t->fp); t->fp = nullptr; }
        t->done = offset;
        t->resyncRequested = false;
    }
    t->pausedByPeer = false;
    t->wantResume = false;
    t->lastDone = offset;
    t->lastTick = SteadyMs();
    if (!c) {
        t->state = TState::Interrupted;
        t->wantResume = true;
        return;
    }
    t->state = TState::Active;
    Queue(c, Msg::FileAccept, {{"tid", t->tid}, {"offset", offset}, {"resume", userResume}});
    MarkDirty();
}

void Engine::FinishIncoming(std::shared_ptr<Transfer> t) {
    t->hashCancel = false;
    std::string sha = Sha256File(t->partPath, &t->hashDone, &t->hashCancel);
    auto lk = Lock();
    if (t->state != TState::Verifying || !running_) return;
    if (sha.empty()) {
        t->state = TState::Interrupted;  // could not read (cancelled / shutting down) - will retry on re-offer
        return;
    }
    t->recvSha = sha;
    if (sha == t->sha256) {
        std::string dest = UniquePath(JoinPath(settings.downloadDir, t->rel));
        MakeDirs(DirName(dest));
        if (!RenameFile(t->partPath, dest)) {
            std::error_code ec;
            std::filesystem::copy_file(std::filesystem::u8path(t->partPath), std::filesystem::u8path(dest), ec);
            if (ec) {
                SetFinal(t, TState::Failed, "Verified, but could not move file to " + dest);
                QueueToPeer(t->peer, Msg::FileVerify, {{"tid", t->tid}, {"ok", false}, {"sha256", sha}});
                return;
            }
            RemoveFile(t->partPath);
        }
        t->localPath = dest;
        t->done = t->size;
        SetFinal(t, TState::Completed, "");
        QueueToPeer(t->peer, Msg::FileVerify, {{"tid", t->tid}, {"ok", true}, {"sha256", sha}});
        EmitIfBatchDone(t);
    } else {
        SetFinal(t, TState::Failed, "SHA-256 mismatch - received data is corrupted (partial file discarded)");
        QueueToPeer(t->peer, Msg::FileVerify, {{"tid", t->tid}, {"ok", false}, {"sha256", sha}});
        Emit({EventType::TransferFailed, t->peer, t->name + ": hash mismatch", t->tid});
    }
}

// ============================================================================ background loops

void Engine::HashLoop() {
    while (running_) {
        std::shared_ptr<Transfer> t;
        {
            std::unique_lock<std::mutex> l(hashMtx_);
            hashCv_.wait_for(l, std::chrono::milliseconds(300), [&] { return !hashQueue_.empty() || !running_; });
            if (!running_) break;
            if (hashQueue_.empty()) continue;
            t = hashQueue_.front();
            hashQueue_.pop_front();
        }
        {
            auto lk = Lock();
            if (t->state != TState::Hashing) continue;
        }
        t->hashDone = 0;
        t->hashCancel = false;
        int64_t size = FileSize(t->localPath);
        int64_t mtime = FileMTime(t->localPath);
        std::string sha = size >= 0 ? Sha256File(t->localPath, &t->hashDone, &t->hashCancel) : "";
        auto lk = Lock();
        if (t->state != TState::Hashing) continue;
        if (!running_) break;
        if (sha.empty()) {
            SetFinal(t, TState::Failed, "Cannot read file");
            continue;
        }
        if (FileSize(t->localPath) != size || FileMTime(t->localPath) != mtime) {
            t->hashCancel = false;
            std::lock_guard<std::mutex> l(hashMtx_);
            hashQueue_.push_back(t);  // modified while hashing, try again
            continue;
        }
        t->sha256 = sha;
        t->size = size;
        t->mtime = mtime;
        t->state = TState::Offered;
        OfferTransfer(t, false);
        MarkDirty();
    }
}

void Engine::SendBeacon(const std::string* unicastIp) {
    if (udp_ == kInvalidSocket) return;
    std::string msg;
    {
        auto lk = Lock();
        json b = {{"app", kAppId}, {"v", kProtocolVersion}, {"id", settings.deviceId}, {"name", settings.displayName},
                  {"platform", PlatformName()}, {"port", boundPort}};
        msg = b.dump();
    }
    if (unicastIp) {
        UdpSendTo(udp_, *unicastIp, kDiscoveryPort, msg.data(), msg.size());
        return;
    }
    UdpSendTo(udp_, "255.255.255.255", kDiscoveryPort, msg.data(), msg.size());
    UdpSendTo(udp_, kMulticastGroup, kDiscoveryPort, msg.data(), msg.size());
    for (auto& a : LocalIPv4Addresses())
        if (!a.broadcast.empty()) UdpSendTo(udp_, a.broadcast, kDiscoveryPort, msg.data(), msg.size());
}

void Engine::DiscoveryLoop() {
    int64_t lastSend = 0;
    char buf[4096];
    while (running_) {
        bool enabled;
        {
            auto lk = Lock();
            enabled = settings.discovery && settings.broadcast && BroadcastAllowed();
        }
        if (enabled && SteadyMs() - lastSend >= 2000) {
            SendBeacon(nullptr);
            lastSend = SteadyMs();
        }
        if (udp_ == kInvalidSocket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        std::string ip;
        int n = UdpRecvFrom(udp_, buf, sizeof(buf) - 1, &ip, 250);
        if (n <= 0 || !enabled) continue;
        json b = json::parse(std::string(buf, (size_t)n), nullptr, false);
        if (b.is_discarded() || b.value("app", "") != kAppId) continue;
        std::string id = b.value("id", "");
        uint16_t port = (uint16_t)b.value("port", (int)kDefaultTcpPort);
        if (OnDiscovered(id, b.value("name", ""), b.value("platform", ""), ip, port, false)) SendBeacon(&ip);
    }
}

bool Engine::OnDiscovered(const std::string& id, const std::string& name, const std::string& platform, const std::string& ip,
                          uint16_t port, bool viaMdns) {
    bool isNew = false, connect = false;
    {
        auto lk = Lock();
        if (id.empty() || id == settings.deviceId || !running_ || port == 0) return false;
        isNew = online.find(id) == online.end();
        OnlinePeer& p = online[id];
        p.id = id;
        if (!name.empty()) p.name = name;
        if (!platform.empty()) p.platform = platform;
        p.ip = ip;
        p.port = port;
        (viaMdns ? p.lastMdns : p.lastBeacon) = SteadyMs();
        auto dit = devices.find(id);
        bool blocked = dit != devices.end() && dit->second.blocked;
        if (dit != devices.end() && !blocked) {
            dit->second.lastIp = ip;
            dit->second.lastPort = port;
        }
        bool busy = connecting_.count(id) || IsConnected(id);
        if (!blocked && !busy && settings.autoConnect && settings.deviceId < id && SteadyMs() - lastAutoConnect_[id] > 5000) {
            lastAutoConnect_[id] = SteadyMs();
            connecting_.insert(id);
            connect = true;
        }
    }
    if (connect) {
        Spawn([this, ip, port, id] {
            SocketHandle s = TcpConnect(ip, port, 3000);
            {
                auto lk = Lock();
                connecting_.erase(id);
            }
            if (s != kInvalidSocket) AdoptSocket(s, Transport::LAN, ip, true);
        });
    }
    return isNew;
}

void Engine::StartWan() {
    WanTransport::Config cfg;
    bool on;
    {
        auto lk = Lock();
        on = settings.wanEnabled && !settings.wanServer.empty() && running_;  // no room name: the server's shared room
        cfg.server = settings.wanServer;
        cfg.roomHash = RoomHash(settings.wanRoom, settings.wanPassword);
        cfg.id = settings.deviceId;
        cfg.name = settings.displayName;
        cfg.platform = PlatformName();
        cfg.pub = identity_.PubHex();
        cfg.natpmp = settings.wanNatPmp;
        cfg.relay = settings.wanRelay;
        cfg.localPort = (uint16_t)std::clamp(settings.wanLocalPort, 0, 65535);
        cfg.forwardedPort = (uint16_t)std::clamp(settings.wanForwardedPort, 0, 65535);
        for (auto& [id, p] : online) p.lastWan = 0;
    }
    wan_.Stop();
    if (!on) return;
    wan_.Start(
        cfg,
        [this](const std::vector<WanPeer>& peers) {
            std::vector<std::string> toConnect;
            {
                auto lk = Lock();
                int64_t now = SteadyMs();
                for (auto& wp : peers) {
                    if (wp.id == settings.deviceId) continue;
                    OnlinePeer& p = online[wp.id];
                    p.id = wp.id;
                    if (!wp.name.empty() && !p.OnLan()) p.name = wp.name;
                    if (!wp.platform.empty()) p.platform = wp.platform;
                    p.lastWan = now;
                    auto d = devices.find(wp.id);
                    bool blocked = d != devices.end() && d->second.blocked;
                    // Same rule as on the LAN: the device with the smaller id dials, unless we owe it something.
                    if (!blocked && !p.OnLan() && !IsConnected(wp.id) && settings.autoConnect && settings.deviceId < wp.id &&
                        now - lastWanConnect_[wp.id] > 20000) {
                        lastWanConnect_[wp.id] = now;
                        toConnect.push_back(wp.id);
                    }
                }
            }
            for (auto& id : toConnect) wan_.Connect(id);
        },
        [this](SocketHandle s, const std::string& peerId, const std::string& addr, bool initiator) {
            (void)peerId;
            AdoptSocket(s, Transport::Internet, addr, initiator);
        });
}

void Engine::StartMdns() {
    std::string id, name;
    uint16_t port;
    bool on;
    {
        auto lk = Lock();
        on = settings.discovery && settings.mdns && running_;
        id = settings.deviceId;
        name = settings.displayName;
        port = boundPort;
    }
    if (!on || port == 0) {
        mdns_.Stop();
        return;
    }
    if (mdns_.Running()) {
        mdns_.Update(name, port);
        return;
    }
    mdns_.Start(id, name, PlatformName(), port, [this](const MdnsPeer& p) {
        if (p.goodbye) {
            auto lk = Lock();
            auto it = online.find(p.id);
            if (it != online.end()) {
                it->second.lastMdns = 0;
                if (!it->second.lastBeacon && !IsConnected(p.id)) online.erase(it);
            }
            return;
        }
        OnDiscovered(p.id, p.name, p.platform, p.ip, p.port, true);
    });
}

void Engine::MaintenanceLoop() {
    int64_t lastPing = SteadyMs(), lastSave = SteadyMs(), lastReconnect = SteadyMs();
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        int64_t now = SteadyMs();
        std::vector<std::shared_ptr<Connection>> toClose;
        std::vector<std::pair<std::string, uint16_t>> reconnect;
        {
            auto lk = Lock();
            for (auto& [id, t] : transfers) {
                if (t->state != TState::Active) { t->speed = 0; continue; }
                int64_t dt = now - t->lastTick;
                if (dt < 500) continue;
                double inst = (double)(t->done.load() - t->lastDone) * 1000.0 / (double)dt;
                if (inst < 0) inst = 0;
                t->speed = t->speed == 0 ? inst : t->speed * 0.6 + inst * 0.4;
                t->lastDone = t->done.load();
                t->lastTick = now;
            }
            bool ping = now - lastPing > 5000;
            if (ping) lastPing = now;
            for (auto& c : conns) {
                if (c->closed) continue;
                if (!c->ready && now - c->connectedAt > 10000) toClose.push_back(c);
                else if (c->ready && now - c->lastRecv.load() > 25000) toClose.push_back(c);
                else if (ping && c->ready) Queue(c, Msg::Ping, json::object());
            }
            conns.erase(std::remove_if(conns.begin(), conns.end(), [](auto& c) { return c->closed && c->alive == 0; }), conns.end());
            for (auto it = online.begin(); it != online.end();) {
                OnlinePeer& op = it->second;
                if (op.lastBeacon && now - op.lastBeacon > 8000) op.lastBeacon = 0;  // beacons every 2 s
                if (op.lastMdns && now - op.lastMdns > 100000) op.lastMdns = 0;      // mDNS re-query every 30 s
                if (op.lastWan && now - op.lastWan > 50000) op.lastWan = 0;          // server re-lists every 15 s
                if (!op.lastBeacon && !op.lastMdns && !op.lastWan && !IsConnected(it->first)) it = online.erase(it);
                else ++it;
            }
            // Reconnect to known devices we still owe something to (unfinished transfers / undelivered chat)
            // even if their beacons do not reach us (e.g. blocked broadcast on the network).
            if (now - lastReconnect > 15000) {
                lastReconnect = now;
                std::set<std::string> owe;
                for (auto& [id, t] : transfers)
                    if (!TStateFinal(t->state) && t->state != TState::Paused && t->state != TState::Pending) owe.insert(t->peer);
                for (auto& [peer, cv] : chats)
                    for (auto& m : cv.messages)
                        if (m.outgoing && !m.delivered) owe.insert(peer);
                for (auto& peer : owe) {
                    auto ow = online.find(peer);
                    if (ow != online.end() && !ow->second.OnLan() && ow->second.lastWan && !IsConnected(peer) && wan_.Running()) {
                        lastWanConnect_[peer] = now;
                        wan_.Connect(peer);
                        continue;
                    }
                    auto d = devices.find(peer);
                    if (d == devices.end() || d->second.blocked || d->second.lastIp.empty() || IsConnected(peer) || connecting_.count(peer)) continue;
                    auto o = online.find(peer);
                    if (o != online.end()) reconnect.emplace_back(o->second.ip, o->second.port);
                    else reconnect.emplace_back(d->second.lastIp, d->second.lastPort ? d->second.lastPort : kDefaultTcpPort);
                }
            }
        }
        {
            auto lk = Lock();
            if ((call.state == CallState::OutgoingRinging || call.state == CallState::IncomingRinging) && now - call.started > 45000)
                FinishCall(call.outgoing ? "no answer" : "missed", call.outgoing);
        }
        if (restartMdns_.exchange(false)) StartMdns();
        if (restartWan_.exchange(false)) StartWan();
        for (auto& c : toClose) CloseConn(c, "timeout");
        for (auto& r : reconnect) ConnectAddress(r.first, r.second);
        if (dirty_ && now - lastSave > 1000) {
            Save();
            lastSave = now;
        }
    }
}

void Engine::StartBtServer() {
    if (!settings.bluetooth) {
        bt_.StopServer();
        btStatus = btSupported ? "Off" : "Not available on this device";
        return;
    }
    std::string err;
    if (bt_.StartServer(settings.btChannel, [this](SocketHandle s, const std::string& addr) { AdoptSocket(s, Transport::Bluetooth, addr, false); }, &err))
        btStatus = "Listening for Bluetooth connections";
    else
        btStatus = err;
}

// ============================================================================ public actions

void Engine::SendChat(const std::string& peer, const std::string& text) {
    if (text.empty()) return;
    auto lk = Lock();
    ChatMessage m;
    m.mid = RandomHex(8);
    m.outgoing = true;
    m.ts = NowMs();
    m.text = text;
    Conversation& cv = chats[peer];
    cv.peer = peer;
    cv.messages.push_back(m);
    QueueToPeer(peer, Msg::Chat, {{"mid", m.mid}, {"ts", m.ts}, {"text", m.text}});
    MarkDirty();
}

uint64_t Engine::SendPaths(const std::string& peer, const std::vector<std::string>& paths) {
    std::vector<std::pair<std::string, std::string>> files;
    for (auto& p : paths) {
        if (IsDirectory(p)) CollectFiles(p, files);
        else if (FileExists(p)) files.emplace_back(p, BaseName(p));
    }
    if (files.empty()) return 0;
    uint64_t batch = RandomU64();
    std::string batchName;
    if (paths.size() == 1) batchName = BaseName(paths[0]);
    if (batchName.empty() || files.size() > 1)
        batchName = (paths.size() == 1 ? batchName + " - " : std::string()) + std::to_string(files.size()) + " files";
    if (files.size() == 1) batchName = files[0].second;
    auto lk = Lock();
    ChatMessage m;
    m.mid = "b" + TidHex(batch);
    m.outgoing = true;
    m.kind = MsgKind::Files;
    m.ts = NowMs();
    m.batch = batch;
    m.text = batchName;
    m.delivered = true;
    int64_t now = NowMs();
    int i = 0;
    for (auto& [abs, rel] : files) {
        auto t = std::make_shared<Transfer>();
        t->tid = RandomU64();
        t->batch = batch;
        t->batchName = batchName;
        t->batchCount = (int)files.size();
        t->peer = peer;
        t->outgoing = true;
        t->localPath = abs;
        t->rel = SanitizeRelativePath(rel);
        t->name = BaseName(t->rel);
        t->size = FileSize(abs);
        t->created = now + i++;  // stable order
        t->state = TState::Hashing;
        transfers[t->tid] = t;
        m.tids.push_back(t->tid);
        std::lock_guard<std::mutex> l(hashMtx_);
        hashQueue_.push_back(t);
    }
    Conversation& cv = chats[peer];
    cv.peer = peer;
    cv.messages.push_back(m);
    hashCv_.notify_all();
    MarkDirty();
    return batch;
}

void Engine::Accept(uint64_t tid) {
    auto lk = Lock();
    auto it = transfers.find(tid);
    if (it == transfers.end() || it->second->outgoing || it->second->state != TState::Pending) return;
    SendAcceptFor(it->second);
}

void Engine::AcceptBatch(uint64_t batch) {
    auto lk = Lock();
    for (auto& t : BatchTransfers(batch))
        if (!t->outgoing && t->state == TState::Pending) SendAcceptFor(t);
}

void Engine::Reject(uint64_t tid) {
    auto lk = Lock();
    auto it = transfers.find(tid);
    if (it == transfers.end() || it->second->outgoing || it->second->state != TState::Pending) return;
    QueueToPeer(it->second->peer, Msg::FileReject, {{"tid", tid}, {"reason", "declined"}});
    SetFinal(it->second, TState::Rejected, "Declined");
}

void Engine::Pause(uint64_t tid) {
    auto lk = Lock();
    auto it = transfers.find(tid);
    if (it == transfers.end()) return;
    auto t = it->second;
    switch (t->state) {
        case TState::Active: case TState::Queued: case TState::Offered: case TState::Interrupted: break;
        default: return;
    }
    t->state = TState::Paused;
    t->pausedByPeer = false;
    t->wantResume = false;
    t->speed = 0;
    CloseFile(*t);
    QueueToPeer(t->peer, Msg::FilePause, {{"tid", tid}});
    if (t->outgoing) PumpQueued(t->peer);
    MarkDirty();
}

void Engine::Resume(uint64_t tid) {
    auto lk = Lock();
    auto it = transfers.find(tid);
    if (it == transfers.end()) return;
    auto t = it->second;
    if (t->state != TState::Paused && t->state != TState::Interrupted) return;
    t->wantResume = true;
    t->pausedByPeer = false;
    if (t->outgoing) {
        t->state = TState::Offered;
        OfferTransfer(t, true);
    } else {
        SendAcceptFor(t, true);
    }
    if (t->state == TState::Interrupted) ConnectPeer(t->peer);
    MarkDirty();
}

void Engine::Cancel(uint64_t tid) {
    auto lk = Lock();
    auto it = transfers.find(tid);
    if (it == transfers.end() || TStateFinal(it->second->state)) return;
    QueueToPeer(it->second->peer, Msg::FileCancel, {{"tid", tid}, {"reason", "cancelled"}});
    SetFinal(it->second, TState::Cancelled, "Cancelled");
}

void Engine::Retry(uint64_t tid) {
    auto lk = Lock();
    auto it = transfers.find(tid);
    if (it == transfers.end()) return;
    auto old = it->second;
    if (!old->outgoing || !TStateFinal(old->state) || old->state == TState::Completed) return;
    auto t = std::make_shared<Transfer>();
    t->tid = RandomU64();
    t->batch = old->batch;
    t->batchName = old->batchName;
    t->batchCount = old->batchCount;
    t->peer = old->peer;
    t->outgoing = true;
    t->localPath = old->localPath;
    t->rel = old->rel;
    t->name = old->name;
    t->size = FileSize(old->localPath);
    t->created = old->created;
    t->state = TState::Hashing;
    transfers.erase(it);
    transfers[t->tid] = t;
    if (ChatMessage* m = FindBatchMessage(t->peer, t->batch))
        std::replace(m->tids.begin(), m->tids.end(), tid, t->tid);
    {
        std::lock_guard<std::mutex> l(hashMtx_);
        hashQueue_.push_back(t);
    }
    hashCv_.notify_all();
    MarkDirty();
}

void Engine::RemoveTransfer(uint64_t tid) {
    auto lk = Lock();
    auto it = transfers.find(tid);
    if (it == transfers.end()) return;
    if (!TStateFinal(it->second->state)) Cancel(tid);
    transfers.erase(tid);
    MarkDirty();
}

void Engine::ClearFinishedTransfers() {
    auto lk = Lock();
    for (auto it = transfers.begin(); it != transfers.end();) {
        if (TStateFinal(it->second->state)) it = transfers.erase(it);
        else ++it;
    }
    MarkDirty();
}

void Engine::PauseAll(bool pause) {
    auto lk = Lock();
    std::vector<uint64_t> ids;
    for (auto& [id, t] : transfers) ids.push_back(id);
    for (auto id : ids) pause ? Pause(id) : Resume(id);
}

void Engine::ConnectAddress(const std::string& host, uint16_t port) {
    std::string key = host + ":" + std::to_string(port);
    {
        auto lk = Lock();
        if (connecting_.count(key)) return;
        connecting_.insert(key);
    }
    Spawn([this, host, port, key] {
        SocketHandle s = TcpConnect(host, port ? port : kDefaultTcpPort, 3000);
        {
            auto lk = Lock();
            connecting_.erase(key);
        }
        if (s != kInvalidSocket) AdoptSocket(s, Transport::LAN, host, true);
        else Emit({EventType::Error, "", "Could not connect to " + key});
    });
}

void Engine::ConnectPeer(const std::string& peer) {
    auto lk = Lock();
    if (IsConnected(peer)) return;
    auto o = online.find(peer);
    if (o != online.end() && o->second.OnLan()) {
        ConnectAddress(o->second.ip, o->second.port);
        return;
    }
    if (o != online.end() && o->second.lastWan && wan_.Running()) {
        lastWanConnect_[peer] = SteadyMs();
        wan_.Connect(peer);
        return;
    }
    auto d = devices.find(peer);
    if (d == devices.end()) return;
    if (!d->second.lastIp.empty()) ConnectAddress(d->second.lastIp, d->second.lastPort);
    if (!d->second.btAddr.empty() && settings.bluetooth) ConnectBluetooth(d->second.btAddr);
}

void Engine::ConnectBluetooth(const std::string& addr) {
    int channel;
    {
        auto lk = Lock();
        channel = settings.btChannel;
    }
    Spawn([this, addr, channel] {
        std::string err;
        bool async = false;
        SocketHandle s = bt_.Connect(addr, channel, &err, &async);
        if (s != kInvalidSocket) AdoptSocket(s, Transport::Bluetooth, addr, true);
        else if (!async) Emit({EventType::Error, "", "Bluetooth: " + err});
    });
}

void Engine::Disconnect(const std::string& peer) {
    std::vector<std::shared_ptr<Connection>> cs;
    {
        auto lk = Lock();
        for (auto& c : conns)
            if (c->peerId == peer) cs.push_back(c);
    }
    for (auto& c : cs) {
        Queue(c, Msg::Bye, json::object());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    for (auto& c : cs) CloseConn(c, "user");
}

void Engine::SetCustomName(const std::string& peer, const std::string& name) {
    auto lk = Lock();
    Device(peer).customName = name;
    MarkDirty();
}

void Engine::SetTrusted(const std::string& peer, bool v) {
    auto lk = Lock();
    Device(peer).trusted = v;
    MarkDirty();
}

void Engine::SetBlocked(const std::string& peer, bool v) {
    {
        auto lk = Lock();
        Device(peer).blocked = v;
        if (v) Device(peer).trusted = false;
        MarkDirty();
    }
    if (v) Disconnect(peer);
}

void Engine::SetVerified(const std::string& peer, bool v) {
    auto lk = Lock();
    Device(peer).verified = v;
    MarkDirty();
}

void Engine::AcceptNewKey(const std::string& peer) {
    {
        auto lk = Lock();
        KnownDevice& d = Device(peer);
        if (d.pendingKey.empty()) return;
        d.pubKey = d.pendingKey;
        d.pendingKey.clear();
        d.verified = false;
        AddSystemMessage(peer, "New identity key accepted. Safety number: " + SafetyNumber(identity_.PubHex(), d.pubKey));
        MarkDirty();
    }
    ConnectPeer(peer);
}

std::string Engine::SafetyNumberWith(const std::string& peer) {
    auto lk = Lock();
    auto it = devices.find(peer);
    if (it == devices.end() || it->second.pubKey.empty()) return "";
    return SafetyNumber(identity_.PubHex(), it->second.pubKey);
}

void Engine::ForgetDevice(const std::string& peer) {
    Disconnect(peer);
    auto lk = Lock();
    devices.erase(peer);
    chats.erase(peer);
    MarkDirty();
}

void Engine::ClearChat(const std::string& peer) {
    auto lk = Lock();
    auto it = chats.find(peer);
    if (it != chats.end()) it->second.messages.clear();
    MarkDirty();
}

void Engine::DeleteMessages(const std::string& peer, const std::vector<std::string>& mids, bool forEveryone) {
    auto lk = Lock();
    auto it = chats.find(peer);
    if (it == chats.end() || mids.empty()) return;
    std::vector<std::string> remote;
    auto& msgs = it->second.messages;
    msgs.erase(std::remove_if(msgs.begin(), msgs.end(), [&](const ChatMessage& m) {
                   if (std::find(mids.begin(), mids.end(), m.mid) == mids.end()) return false;
                   // an undelivered text never reached the other side - just drop it
                   if (forEveryone && m.outgoing && m.kind != MsgKind::System && (m.delivered || m.kind == MsgKind::Files)) remote.push_back(m.mid);
                   return true;
               }),
               msgs.end());
    if (!remote.empty()) {
        auto& pd = it->second.pendingDeletes;
        pd.insert(pd.end(), remote.begin(), remote.end());
        if (pd.size() > 500) pd.erase(pd.begin(), pd.begin() + (ptrdiff_t)(pd.size() - 500));
        QueueToPeer(peer, Msg::ChatDelete, {{"mids", remote}});
    }
    MarkDirty();
}

void Engine::React(const std::string& peer, const std::string& mid, const std::string& emoji) {
    auto lk = Lock();
    auto it = chats.find(peer);
    if (it == chats.end()) return;
    for (auto& m : it->second.messages)
        if (m.mid == mid && m.kind != MsgKind::System) {
            if (m.myReaction == emoji) return;
            m.myReaction = emoji;
            m.reactionSent = false;
            QueueToPeer(peer, Msg::ChatReact, {{"mid", mid}, {"emoji", emoji}});
            MarkDirty();
            return;
        }
}

void Engine::MarkRead(const std::string& peer) {
    auto lk = Lock();
    auto it = chats.find(peer);
    if (it != chats.end()) it->second.unread = 0;
}

void Engine::RemoveHistory(size_t index) {
    auto lk = Lock();
    if (index < history.size()) history.erase(history.begin() + (ptrdiff_t)index);
    MarkDirty();
}

void Engine::ClearHistory() {
    auto lk = Lock();
    history.clear();
    MarkDirty();
}

Settings Engine::GetSettings() {
    auto lk = Lock();
    return settings;
}

void Engine::ApplySettings(const Settings& s) {
    auto lk = Lock();
    Settings old = settings;
    settings = s;
    settings.deviceId = old.deviceId;
    settings.identitySecret = old.identitySecret;
    if (settings.downloadDir.empty()) settings.downloadDir = DefaultDownloadDir();
    MakeDirs(settings.downloadDir);
    if (settings.tcpPort != old.tcpPort && running_) restartListener_ = true;
    if ((settings.bluetooth != old.bluetooth || settings.btChannel != old.btChannel) && running_) StartBtServer();
    // Restarting these joins their threads, which may be waiting for our lock: do it from the maintenance thread.
    if (running_ && (settings.discovery != old.discovery || settings.mdns != old.mdns || settings.displayName != old.displayName)) restartMdns_ = true;
    if (running_ && (settings.wanEnabled != old.wanEnabled || settings.wanServer != old.wanServer || settings.wanRoom != old.wanRoom ||
                     settings.wanPassword != old.wanPassword || settings.wanNatPmp != old.wanNatPmp || settings.wanRelay != old.wanRelay ||
                     settings.wanLocalPort != old.wanLocalPort || settings.wanForwardedPort != old.wanForwardedPort))
        restartWan_ = true;
    else if (running_ && settings.displayName != old.displayName)
        wan_.UpdateName(settings.displayName);
    MarkDirty();
}

bool Engine::ExportSettings(const std::string& path, bool includeHistory, bool includeIdentity, std::string* err) {
    json j;
    {
        auto lk = Lock();
        j = {{"app", kAppId}, {"type", "settings-export"}, {"version", 1}, {"exported", NowMs()},
             {"from", settings.displayName}, {"settings", ToJson(settings, includeIdentity)}};
        j["devices"] = json::array();
        for (auto& [id, d] : devices) j["devices"].push_back(ToJson(d));
        if (includeHistory) {
            j["files"] = json::array();
            for (auto& h : history) j["files"].push_back(ToJson(h));
            j["chats"] = json::object();
            for (auto& [peer, cv] : chats) {
                json arr = json::array();
                for (auto& m : cv.messages) arr.push_back(ToJson(m));
                j["chats"][peer] = arr;
            }
        }
    }
    MakeDirs(DirName(path));
    if (!SaveJsonFile(path, j)) {
        if (err) *err = "Cannot write " + path;
        return false;
    }
    return true;
}

bool Engine::ImportSettings(const std::string& path, bool importIdentity, std::string* err) {
    json j;
    if (!LoadJsonFile(path, j) || !j.is_object() || j.value("app", "") != kAppId) {
        if (err) *err = "Not a LunaShare settings file";
        return false;
    }
    Settings s = GetSettings();
    std::string keepDl = s.downloadDir;
    FromJson(j.value("settings", json::object()), s, importIdentity && j["settings"].contains("deviceId"));
    if (!MakeDirs(s.downloadDir)) s.downloadDir = keepDl;  // folder from another machine may not exist here
    {
        auto lk = Lock();
        std::string id = s.deviceId;
        ApplySettings(s);
        if (importIdentity && !id.empty()) {
            settings.deviceId = id;
            if (!s.identitySecret.empty() && identity_.LoadSeed(s.identitySecret)) settings.identitySecret = s.identitySecret;
        }
        if (j.contains("devices") && j["devices"].is_array()) {
            for (auto& dj : j["devices"]) {
                KnownDevice d = DeviceFromJson(dj);
                if (d.id.empty() || d.id == settings.deviceId) continue;
                auto it = devices.find(d.id);
                if (it == devices.end()) {
                    devices[d.id] = d;
                } else {
                    if (!d.customName.empty()) it->second.customName = d.customName;
                    it->second.trusted = d.trusted;
                    it->second.blocked = d.blocked;
                    if (!d.notes.empty()) it->second.notes = d.notes;
                    if (it->second.pubKey.empty()) { it->second.pubKey = d.pubKey; it->second.verified = d.verified; }
                    else if (it->second.pubKey == d.pubKey) it->second.verified = it->second.verified || d.verified;
                    if (it->second.lastIp.empty()) { it->second.lastIp = d.lastIp; it->second.lastPort = d.lastPort; }
                    if (it->second.btAddr.empty()) it->second.btAddr = d.btAddr;
                }
            }
        }
        if (j.contains("files") && j["files"].is_array()) {
            for (auto& hj : j["files"]) {
                HistoryEntry h = HistoryFromJson(hj);
                bool dup = false;
                for (auto& e : history) if (e.tid == h.tid && e.outgoing == h.outgoing) dup = true;
                if (!dup) history.push_back(h);
            }
            std::sort(history.begin(), history.end(), [](auto& a, auto& b) { return a.ts < b.ts; });
        }
        if (j.contains("chats") && j["chats"].is_object()) {
            for (auto& [peer, arr] : j["chats"].items()) {
                Conversation& cv = chats[peer];
                cv.peer = peer;
                for (auto& mj : arr) {
                    ChatMessage m = MessageFromJson(mj);
                    bool dup = false;
                    for (auto& e : cv.messages) if (e.mid == m.mid) dup = true;
                    if (!dup) cv.messages.push_back(m);
                }
                std::sort(cv.messages.begin(), cv.messages.end(), [](auto& a, auto& b) { return a.ts < b.ts; });
            }
        }
        MarkDirty();
    }
    return true;
}

void Engine::BtRefresh(bool inquiry) {
    {
        auto lk = Lock();
        if (btScanning) return;
        btScanning = true;
    }
    Spawn([this, inquiry] {
        auto devs = bt_.Devices(inquiry);
        auto lk = Lock();
        btDevices = devs;
        btScanning = false;
    });
}

void Engine::SendWifiCreds(const std::string& peer, const std::string& ssid, const std::string& pass) {
    QueueToPeer(peer, Msg::WifiCreds, {{"ssid", ssid}, {"pass", pass}});
}

// ============================================================================ calls

std::string CapForMedia(const std::string& media) {
    // "webcam": the callee sends its camera (a PC asking a phone); "webcam-offer": the caller sends its camera
    return media == "webcam" || media == "webcam-offer" ? "video" : media;
}

bool Engine::PeerHasCap(const std::string& peer, const std::string& cap) {
    auto c = ReadyConn(peer);
    return c && std::find(c->caps.begin(), c->caps.end(), cap) != c->caps.end();
}

uint64_t Engine::StartCall(const std::string& peer, const std::string& media) {
    auto lk = Lock();
    if (call.state != CallState::None || !PeerHasCap(peer, CapForMedia(media)) ||
        std::find(localCaps.begin(), localCaps.end(), CapForMedia(media)) == localCaps.end())
        return 0;
    call = CallInfo();
    call.id = RandomU64();
    call.peer = peer;
    call.outgoing = true;
    call.state = CallState::OutgoingRinging;
    call.started = SteadyMs();
    call.media = media;
    QueueToPeer(peer, Msg::CallInvite, {{"call", call.id}, {"media", media}});
    return call.id;
}

void Engine::AcceptCall() {
    auto lk = Lock();
    if (call.state != CallState::IncomingRinging) return;
    auto c = ReadyConn(call.peer);
    if (!c) {
        FinishCall("connection lost", false);
        return;
    }
    call.state = CallState::Active;
    call.answered = SteadyMs();
    {
        std::lock_guard<std::mutex> l(mediaMtx_);
        activeCallPeer_ = call.peer;
        activeCallConn_ = c;
    }
    activeCallId_ = call.id;
    c->callPriority = true;
    Queue(c, Msg::CallAccept, {{"call", call.id}});
    Emit({EventType::CallStarted, call.peer, "Call started", call.id});
}

void Engine::EndCall(const std::string& reason) {
    auto lk = Lock();
    if (call.state == CallState::None) return;
    std::string r = reason;
    if (call.state == CallState::IncomingRinging && reason == "hangup") r = "declined";
    FinishCall(r, true);
}

void Engine::FinishCall(const std::string& reason, bool notifyPeer) {
    // called with the engine lock held
    if (call.state == CallState::None) return;
    CallInfo c = call;
    if (notifyPeer) QueueToPeer(c.peer, Msg::CallEnd, {{"call", c.id}, {"reason", reason}});
    activeCallId_ = 0;
    {
        std::lock_guard<std::mutex> l(mediaMtx_);
        activeCallPeer_.clear();
        activeCallConn_.reset();
    }
    for (auto& cn : conns)
        if (cn->peerId == c.peer) cn->callPriority = false;
    call = CallInfo();
    std::string text;
    if (c.state == CallState::Active) {
        int secs = (int)((SteadyMs() - c.answered) / 1000);
        char b[64];
        snprintf(b, sizeof(b), "%d:%02d", secs / 60, secs % 60);
        text = std::string(c.outgoing ? "Outgoing" : "Incoming") + " call - " + b;
        if (reason == "connection lost") text += " (connection lost)";
    } else if (c.outgoing) {
        text = reason == "busy" ? "Call not answered - busy" : reason == "declined" ? "Call declined"
             : reason == "unsupported" ? "Call failed - the other device has no voice module" : reason == "no answer" ? "No answer"
             : "Call cancelled";
    } else {
        text = reason == "declined" ? "Declined call" : "Missed call";
    }
    AddSystemMessage(c.peer, text);
    Emit({EventType::CallEnded, c.peer, text, c.id, reason});
}

std::shared_ptr<Connection> Engine::CallConn() {
    std::shared_ptr<Connection> c;
    std::string peer;
    {
        std::lock_guard<std::mutex> l(mediaMtx_);
        peer = activeCallPeer_;
        c = activeCallConn_.lock();
    }
    if (!c || c->closed) {
        // the connection was replaced (e.g. LAN came back): look it up once, under the engine lock
        auto lk = Lock();
        c = ReadyConn(peer);
        if (!c) return nullptr;
        c->callPriority = true;
        std::lock_guard<std::mutex> l(mediaMtx_);
        activeCallConn_ = c;
    }
    return c;
}

bool Engine::SendMedia(const void* data, size_t len) {
    uint64_t id = activeCallId_.load();
    if (!id || len < 1 || len > (8u << 20)) return false;  // 4K keyframes can be several hundred KB
    bool video = ((const uint8_t*)data)[0] == 2;
    if (!video && len > 4000) return false;
    auto c = CallConn();
    if (!c) return false;
    std::string payload(8 + len, '\0');
    PutU64((uint8_t*)payload.data(), id);
    memcpy(&payload[8], data, len);
    {
        std::lock_guard<std::mutex> l(c->qm);
        size_t audio = 0, videoBytes = 0;
        for (auto& q : c->ctrl) {
            if (q.first != Msg::Media) continue;
            if (q.second.size() > 8 && (uint8_t)q.second[8] == 2) videoBytes += q.second.size();
            else audio++;
        }
        if (video) {
            // keep latency bounded: the module adapts its bitrate from MediaQueuedBytes() and skips frames on refusal
            if (videoBytes > 8u * 1024 * 1024) return false;  // hard stop; normally the module adapts long before
            c->ctrl.emplace_back(Msg::Media, std::move(payload));
        } else {
            // never let voice pile up: if the link stalls, drop the oldest audio instead of growing latency
            if (audio > 25) {
                for (auto it = c->ctrl.begin(); it != c->ctrl.end(); ++it)
                    if (it->first == Msg::Media && !(it->second.size() > 8 && (uint8_t)it->second[8] == 2)) { c->ctrl.erase(it); break; }
            }
            // audio overtakes queued video
            auto pos = c->ctrl.end();
            for (auto it = c->ctrl.begin(); it != c->ctrl.end(); ++it)
                if (it->first == Msg::Media && it->second.size() > 8 && (uint8_t)it->second[8] == 2) { pos = it; break; }
            c->ctrl.insert(pos, {Msg::Media, std::move(payload)});
        }
    }
    c->qcv.notify_one();
    return true;
}

size_t Engine::MediaQueuedBytes() {
    if (!activeCallId_.load()) return 0;
    std::shared_ptr<Connection> c;
    {
        std::lock_guard<std::mutex> l(mediaMtx_);
        c = activeCallConn_.lock();
    }
    if (!c) return 0;
    std::lock_guard<std::mutex> l(c->qm);
    size_t n = 0;
    for (auto& q : c->ctrl)
        if (q.first == Msg::Media) n += q.second.size();
    return n;
}

std::string Engine::activeCallPeerSnapshot() {
    std::lock_guard<std::mutex> l(mediaMtx_);
    return activeCallPeer_;
}

void Engine::SetHotspotInfo(const HotspotInfo& h) {
    auto lk = Lock();
    hotspot = h;
    if (!h.error.empty()) Emit({EventType::Error, "", "Hotspot: " + h.error});
}

}  // namespace ls
