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
        // every device has an account; older installs get one now, with this device as its only member
        if (settings.accountSecret.empty() || !account_.LoadSeed(settings.accountSecret)) {
            account_.Generate();
            settings.accountSecret = account_.seedHex;
            settings.accountDevices.clear();
            settings.accountVer = 0;
            MarkDirty();
        }
        {
            bool self = false;
            for (auto& d : settings.accountDevices) self |= d.id == settings.deviceId && d.pub == identity_.PubHex();
            if (!self) {
                settings.accountDevices.push_back({settings.deviceId, identity_.PubHex()});
                settings.accountVer = std::max(NowMs(), settings.accountVer + 1);
                SignAccountDevices();
                MarkDirty();
            }
        }
        accountCert_ = SignData(account_, "device", settings.deviceId + "\n" + identity_.PubHex());
        std::vector<std::string> ids;
        for (auto& [id, d] : devices) ids.push_back(id);
        for (auto& id : ids) MergeChats(id);
        for (auto& [k, cv] : chats)
            for (auto& m : cv.messages)
                if (!m.outgoing && !m.readSent) cv.readPending = true;
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
    libThread_ = std::thread([this] { LibraryLoop(); });
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
    if (libThread_.joinable()) libThread_.join();
    for (auto& t : saveThreads_)
        if (t.joinable()) t.join();
    saveThreads_.clear();
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

static std::vector<std::string> JsonStrings(const json& a);
static std::vector<AccountDevice> JsonDevices(const json& a);

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
        for (auto& a : j.value("accounts", json::array())) {
            if (!a.is_object()) continue;
            AccountInfo ai;
            ai.pub = a.value("pub", "");
            ai.ver = a.value("ver", (int64_t)0);
            ai.devices = JsonDevices(a.value("devices", json::array()));
            ai.removed = JsonStrings(a.value("removed", json::array()));
            ai.sig = a.value("sig", "");
            if (!ai.pub.empty()) accounts[ai.pub] = ai;
        }
        for (auto& gj : j.value("groups", json::array())) {
            if (!gj.is_object()) continue;
            Group g;
            g.id = gj.value("gid", "");
            g.name = gj.value("name", "");
            g.creator = gj.value("creator", "");
            g.ver = gj.value("ver", (int64_t)0);
            g.left = gj.value("left", false);
            for (auto& m : gj.value("members", json::array()))
                if (m.is_object()) g.members.push_back({m.value("acc", ""), m.value("name", "")});
            if (!g.id.empty()) groups[g.id] = g;
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
    if (LoadJsonFile(JoinPath(dir, "swarm.json"), j)) LoadSwarm(j);
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
        js["accounts"] = json::array();
        for (auto& [pub, a] : accounts) {
            json ds = json::array();
            for (auto& d : a.devices) ds.push_back({{"id", d.id}, {"pub", d.pub}});
            js["accounts"].push_back({{"pub", pub}, {"ver", a.ver}, {"devices", ds}, {"removed", a.removed}, {"sig", a.sig}});
        }
        js["groups"] = json::array();
        for (auto& [id, g] : groups) {
            json gj = GroupJson(g);
            gj["left"] = g.left;
            js["groups"].push_back(gj);
        }
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
    SaveJsonFile(JoinPath(dir, "swarm.json"), SwarmJson());
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

void Engine::MuteChat(const std::string& key, int minutes) {
    auto lk = Lock();
    if (minutes < 0) settings.mutedChats.erase(key);
    else settings.mutedChats[key] = minutes == 0 ? 0 : NowMs() + (int64_t)minutes * 60000;
    MarkDirty();
}

int64_t Engine::ChatMutedUntil(const std::string& key) {
    auto lk = Lock();
    auto it = settings.mutedChats.find(key);
    if (it == settings.mutedChats.end()) return -1;
    if (it->second != 0 && it->second <= NowMs()) {  // (the time is up)
        settings.mutedChats.erase(it);
        MarkDirty();
        return -1;
    }
    return it->second;
}

bool Engine::ChatMuted(const std::string& key) { return ChatMutedUntil(key) >= 0; }

std::string Engine::PeerName(const std::string& peerOrKey) {
    auto lk = Lock();
    std::string peer = peerOrKey;
    if (IsGroupKey(peerOrKey)) {
        auto g = groups.find(peerOrKey.substr(1));
        return g == groups.end() || g->second.name.empty() ? "Group" : g->second.name;
    }
    if (IsContactKey(peerOrKey)) {
        // a person: a nickname given to any of their devices, else the name of the device seen last
        const KnownDevice* best = nullptr;
        for (auto& id : DevicesOf(peerOrKey)) {
            auto it = devices.find(id);
            if (it == devices.end()) continue;
            if (!it->second.customName.empty()) return it->second.customName;
            if (!best || it->second.lastSeen > best->lastSeen) best = &it->second;
        }
        if (!best) return peerOrKey.substr(1, 8);
        peer = best->id;
    }
    auto it = devices.find(peer);
    if (it != devices.end()) return it->second.DisplayName();
    auto o = online.find(peer);
    if (o != online.end()) return o->second.name;
    return peer.substr(0, 8);
}

std::shared_ptr<Connection> Engine::ReadyConn(const std::string& peer) {
    auto lk = Lock();
    std::shared_ptr<Connection> best;
    bool person = IsContactKey(peer);
    for (auto& c : conns) {
        if (!c->ready || c->closed) continue;
        if (person ? ChatKey(c->peerId) != peer : c->peerId != peer) continue;
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
    if (onEvent) onEvent(e);
    events_.push_back(std::move(e));
    if (events_.size() > 500) events_.erase(events_.begin());
}

std::vector<Event> Engine::PopEvents() {
    auto lk = Lock();
    std::vector<Event> e;
    e.swap(events_);
    return e;
}

// ============================================================================ accounts

static bool WriteWholeFile(const std::string& path, const std::string& data);
static bool ReadWholeFile(const std::string& path, std::string& out, size_t maxBytes);
static bool LooksLikeJpeg(const std::string& d);

std::string Engine::ChatKey(const std::string& device) {
    auto it = devices.find(device);
    if (it == devices.end() || it->second.accountPub.empty() || it->second.accountPub == MyAccount()) return device;
    return "@" + it->second.accountPub;
}

std::vector<std::string> Engine::DevicesOf(const std::string& key) {
    if (IsGroupKey(key)) {  // every device of every other member
        std::vector<std::string> out;
        auto g = groups.find(key.substr(1));
        if (g == groups.end() || g->second.left) return out;
        for (auto& m : g->second.members)
            if (m.acc != MyAccount())
                for (auto& d : DevicesOf("@" + m.acc)) out.push_back(d);
        return out;
    }
    if (!IsContactKey(key)) return {key};
    std::vector<std::string> out;
    std::string acc = key.substr(1);
    for (auto& [id, d] : devices)
        if (d.accountPub == acc && !d.blocked) out.push_back(id);
    return out;
}

std::string Engine::Resolve(const std::string& key) {
    if (!IsContactKey(key)) return key;
    if (auto c = ReadyConn(key)) return c->peerId;
    std::string best;
    int64_t bestSeen = -1;
    for (auto& id : DevicesOf(key)) {
        auto o = online.find(id);
        int64_t seen = o != online.end() ? INT64_MAX : devices[id].lastSeen;
        if (seen > bestSeen) {
            best = id;
            bestSeen = seen;
        }
    }
    return best.empty() ? key : best;
}

bool Engine::IsOnline(const std::string& key) {
    for (auto& id : DevicesOf(key))
        if (online.count(id)) return true;
    return false;
}

const KnownDevice* Engine::DeviceFor(const std::string& key) {
    auto it = devices.find(Resolve(key));
    return it == devices.end() ? nullptr : &it->second;
}

std::vector<std::string> Engine::MyOtherDevices() {
    std::vector<std::string> out;
    for (auto& [id, d] : devices)
        if (d.accountPub == MyAccount() && id != settings.deviceId && !d.blocked) out.push_back(id);
    return out;
}

std::vector<std::string> Engine::Conversations() {
    std::set<std::string> keys;
    for (auto& [id, d] : devices)
        if (id != settings.deviceId && !d.viaGroup) keys.insert(ChatKey(id));
    for (auto& [id, o] : online) {
        auto d = devices.find(id);
        if (id != settings.deviceId && (d == devices.end() || !d->second.viaGroup)) keys.insert(ChatKey(id));
    }
    for (auto& [id, g] : groups)
        if (!g.left) keys.insert("#" + id);
    for (auto& [k, cv] : chats)
        if (!cv.messages.empty()) keys.insert(k);
    return std::vector<std::string>(keys.begin(), keys.end());
}

// What an account signs about its devices. The text is canonical (sorted), so any device of the account produces
// the same bytes for the same list.
static std::string DeviceListText(int64_t ver, std::vector<AccountDevice> list, std::vector<std::string> removed) {
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.id < b.id; });
    std::sort(removed.begin(), removed.end());
    std::string t = std::to_string(ver) + "\n";
    for (auto& d : list) t += d.id + " " + d.pub + "\n";
    for (auto& r : removed) t += "removed " + r + "\n";
    return t;
}

static std::vector<std::string> JsonStrings(const json& a) {
    std::vector<std::string> out;
    if (a.is_array())
        for (auto& x : a)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

static std::vector<AccountDevice> JsonDevices(const json& a) {
    std::vector<AccountDevice> out;
    if (a.is_array())
        for (auto& d : a) {
            if (!d.is_object()) continue;
            AccountDevice ad{d.value("id", ""), d.value("pub", "")};
            if (!ad.id.empty() && ad.pub.size() == 64) out.push_back(ad);
        }
    return out;
}

void Engine::SignAccountDevices() {
    settings.accountSig = SignData(account_, "devices", DeviceListText(settings.accountVer, settings.accountDevices, settings.accountRemoved));
}

json Engine::AccountJson() {
    json ds = json::array();
    for (auto& d : settings.accountDevices) ds.push_back({{"id", d.id}, {"pub", d.pub}});
    return {{"pub", MyAccount()}, {"ver", settings.accountVer}, {"devices", ds}, {"removed", settings.accountRemoved}, {"sig", settings.accountSig}};
}

void Engine::PushAccount() {
    json aj = AccountJson();
    for (auto& o : conns)
        if (o->ready && !o->closed) Queue(o, Msg::AccountData, aj);
}

void Engine::HandleAccount(const std::shared_ptr<Connection>& c, const json& j) {
    auto dv = devices.find(c->peerId);
    if (dv == devices.end()) return;
    if (j.value("pub", "") != dv->second.accountPub) return;  // only the account the sending device belongs to
    ApplyAccountList(j, false);
}

bool Engine::ApplyAccountList(const json& j, bool fromGroup) {
    if (!j.is_object()) return false;
    std::string pub = j.value("pub", ""), sig = j.value("sig", "");
    int64_t ver = j.value("ver", (int64_t)0);
    if (pub.size() != 64) return false;
    std::vector<AccountDevice> list = JsonDevices(j.value("devices", json::array()));
    std::vector<std::string> removed = JsonStrings(j.value("removed", json::array()));
    if (list.empty() || list.size() > 64 || removed.size() > 256 || !VerifyData(pub, "devices", DeviceListText(ver, list, removed), sig)) return false;
    bool mine = pub == MyAccount();
    if (mine && fromGroup) return false;  // my own devices tell me about my account
    std::vector<AccountDevice> old = mine ? settings.accountDevices : accounts[pub].devices;
    if (ver <= (mine ? settings.accountVer : accounts[pub].ver)) return false;
    if (mine) {
        bool self = false;
        for (auto& d : list) self |= d.id == settings.deviceId;
        if (!self && std::find(removed.begin(), removed.end(), settings.deviceId) != removed.end()) {
            LeaveAccount();  // another of my devices removed this one
            return true;
        }
        settings.accountDevices = list;
        settings.accountRemoved = removed;
        settings.accountVer = ver;
        settings.accountSig = sig;
        if (!self) {
            // a newer list without me, but I was not removed (restored from the phrase elsewhere): rejoin
            settings.accountDevices.push_back({settings.deviceId, identity_.PubHex()});
            settings.accountVer = std::max(NowMs(), settings.accountVer + 1);
            SignAccountDevices();
            PushAccount();
        }
    } else {
        AccountInfo& a = accounts[pub];
        a.pub = pub;
        a.ver = ver;
        a.devices = list;
        a.removed = removed;
        a.sig = sig;
    }
    // devices of the list we have never met: known now (their key is vouched for by the account), so messages can
    // be queued for them and the internet server can find them
    for (auto& ad : list) {
        if (ad.id == settings.deviceId) continue;
        auto it = devices.find(ad.id);
        if (it == devices.end()) {
            bool known = !DevicesOf("@" + pub).empty();
            KnownDevice& nd = Device(ad.id);
            nd.pubKey = ad.pub;
            nd.accountPub = pub;
            nd.viaGroup = fromGroup && !known && !mine;
            nd.name = mine ? settings.displayName : PeerName("@" + pub);
        } else if (it->second.pubKey == ad.pub && it->second.accountPub != pub) {
            it->second.accountPub = pub;
            MergeChats(ad.id);
        }
    }
    // devices removed from the account no longer speak for it
    for (auto& od : old)
        if (std::find(list.begin(), list.end(), od) == list.end()) {
            auto it = devices.find(od.id);
            if (it != devices.end() && it->second.accountPub == pub) it->second.accountPub.clear();
        }
    MarkDirty();
    return true;
}

void Engine::MergeChats(const std::string& device) {
    std::string key = ChatKey(device);
    if (key != device) MergeChatInto(device, key);
}

void Engine::MergeChatInto(const std::string& fromKey, const std::string& key) {
    if (fromKey == key) return;
    auto it = chats.find(fromKey);
    if (it == chats.end()) return;
    Conversation& to = chats[key];
    to.peer = key;
    for (auto& m : it->second.messages) {
        bool dup = false;
        for (auto& e : to.messages) dup |= e.mid == m.mid;
        if (!dup) to.messages.push_back(m);
    }
    std::stable_sort(to.messages.begin(), to.messages.end(), [](auto& a, auto& b) { return a.ts < b.ts; });
    to.unread += it->second.unread;
    to.pendingDeletes.insert(to.pendingDeletes.end(), it->second.pendingDeletes.begin(), it->second.pendingDeletes.end());
    chats.erase(fromKey);  // (`it` is invalid from here on)
    MarkDirty();
}

// ---- linking

std::string Engine::LinkCode(const std::string& device) {
    auto c = ReadyConn(device);
    return c && c->sec ? c->sec->Code() : std::string();
}

bool Engine::LinkPending(const std::string& device) {
    auto lk = Lock();
    auto it = linkOffered_.find(device);
    return it != linkOffered_.end() && SteadyMs() - it->second < 10 * 60000;
}

bool Engine::LinkDevice(const std::string& device) {
    auto lk = Lock();
    auto dv = devices.find(device);
    auto c = ReadyConn(device);
    if (dv == devices.end() || dv->second.accountPub == MyAccount() || !c) return false;
    linkOffered_[device] = SteadyMs();
    Queue(c, Msg::LinkOffer, {{"name", settings.displayName}});
    return true;
}

void Engine::AnswerLink(const std::string& device, bool accept) {
    auto lk = Lock();
    auto it = linkAsked_.find(device);
    if (it == linkAsked_.end()) return;
    linkAsked_.erase(it);
    if (accept) linkJoining_[device] = SteadyMs();
    QueueToPeer(device, Msg::LinkAnswer, {{"accept", accept}});
}

// Everything another device needs to become "me": the account key and device list, my profile, contacts and
// conversations (the newest 2000 messages of each). Sent to a device being linked, and saved in backups.
json Engine::AccountPayload(const std::string& exceptDevice) {
    json devs = json::array();
    for (auto& [id, kd] : devices)
        if (id != exceptDevice) devs.push_back(ToJson(kd));
    KnownDevice self;
    self.id = settings.deviceId;
    self.name = settings.displayName;
    self.platform = PlatformName();
    self.host = HostName();
    self.pubKey = identity_.PubHex();
    self.accountPub = MyAccount();
    self.trusted = true;
    self.lastIp = "";
    self.lastPort = boundPort;
    devs.push_back(ToJson(self));
    json accs = json::array();
    for (auto& [pub, a] : accounts) {
        if (a.sig.empty()) continue;
        json ds = json::array();
        for (auto& ad : a.devices) ds.push_back({{"id", ad.id}, {"pub", ad.pub}});
        accs.push_back({{"pub", a.pub}, {"ver", a.ver}, {"devices", ds}, {"removed", a.removed}, {"sig", a.sig}});
    }
    json cj = json::object();
    for (auto& [key, cv] : chats) {
        if (key == exceptDevice || cv.messages.empty()) continue;
        json arr = json::array();
        size_t from = cv.messages.size() > 2000 ? cv.messages.size() - 2000 : 0;
        for (size_t i = from; i < cv.messages.size(); i++) arr.push_back(ToJson(cv.messages[i]));
        cj[key] = arr;
    }
    json pj = ProfileJson();
    return {{"secret", settings.accountSecret}, {"account", AccountJson()}, {"name", settings.displayName},
            {"avh", settings.avatarHash}, {"img", pj["img"]}, {"pv", settings.profileVer},
            {"devices", devs}, {"accounts", accs}, {"chats", cj}};
}

// Become a device of the account in `p` (link grant or restored backup / phrase). requireSelf: the signed device
// list must already contain this device (linking); otherwise this device adds itself (restoring).
bool Engine::AdoptAccount(const json& p, const std::string& fromDevice, bool requireSelf, std::string* err) {
    Identity acc;
    json a = p.value("account", json::object());
    std::string pub = a.value("pub", ""), sig = a.value("sig", "");
    int64_t ver = a.value("ver", (int64_t)0);
    if (!acc.LoadSeed(p.value("secret", "")) || (!pub.empty() && acc.PubHex() != pub)) {
        if (err) *err = "the account key is not valid";
        return false;
    }
    pub = acc.PubHex();
    std::vector<AccountDevice> list = JsonDevices(a.value("devices", json::array()));
    std::vector<std::string> removed = JsonStrings(a.value("removed", json::array()));
    bool self = false;
    for (auto& d : list) self |= d.id == settings.deviceId && d.pub == identity_.PubHex();
    bool listOk = !list.empty() && VerifyData(pub, "devices", DeviceListText(ver, list, removed), sig);
    if (requireSelf && (!self || !listOk)) {
        if (err) *err = "the account's device list is not valid";
        return false;
    }
    if (!listOk) {
        list.clear();
        removed.clear();
        ver = 0;
    }
    // this device: never "removed" when the owner restores it on purpose
    removed.erase(std::remove(removed.begin(), removed.end(), settings.deviceId), removed.end());
    account_.LoadSeed(p.value("secret", ""));
    settings.accountSecret = account_.seedHex;
    settings.accountDevices = list;
    settings.accountRemoved = removed;
    settings.accountVer = ver;
    settings.accountSig = sig;
    if (!self) {
        settings.accountDevices.push_back({settings.deviceId, identity_.PubHex()});
        settings.accountVer = std::max(NowMs(), settings.accountVer + 1);
    }
    SignAccountDevices();
    accountCert_ = SignData(account_, "device", settings.deviceId + "\n" + identity_.PubHex());
    wan_.SetAccount(MyAccount(), accountCert_);
    // the person's profile: name and picture
    std::string name = p.value("name", ""), avh = p.value("avh", ""), img;
    if (!name.empty()) settings.displayName = name;
    if (!avh.empty() && Base64Decode(p.value("img", ""), img) && img.size() <= kMaxAvatarBytes && LooksLikeJpeg(img) &&
        Sha256Hex(img.data(), img.size()) == avh && WriteWholeFile(AvatarFile("", avh), img)) {
        if (!settings.avatarHash.empty() && settings.avatarHash != avh) RemoveFile(AvatarFile("", settings.avatarHash));
        settings.avatarHash = avh;
    }
    settings.profileVer = std::max(NowMs(), std::max(settings.profileVer + 1, p.value("pv", (int64_t)0)));
    // contacts
    for (auto& dj : p.value("devices", json::array())) {
        KnownDevice kd = DeviceFromJson(dj);
        if (kd.id.empty() || kd.id == settings.deviceId) continue;
        auto it = devices.find(kd.id);
        if (it == devices.end()) {
            kd.avatarHash.clear();  // their picture comes with their own profile
            kd.profileVer = 0;
            devices[kd.id] = kd;
        } else {
            KnownDevice& e = it->second;
            if (e.pubKey.empty()) e.pubKey = kd.pubKey;
            if (e.pubKey == kd.pubKey && !kd.accountPub.empty()) e.accountPub = kd.accountPub;
            if (e.customName.empty()) e.customName = kd.customName;
            e.trusted |= kd.trusted;
            e.verified |= kd.verified && e.pubKey == kd.pubKey;
        }
    }
    if (!fromDevice.empty()) devices[fromDevice].accountPub = pub;
    for (auto& aj : p.value("accounts", json::array())) {
        AccountInfo ai;
        ai.pub = aj.value("pub", "");
        ai.ver = aj.value("ver", (int64_t)0);
        ai.sig = aj.value("sig", "");
        ai.devices = JsonDevices(aj.value("devices", json::array()));
        ai.removed = JsonStrings(aj.value("removed", json::array()));
        if (ai.pub.empty() || ai.pub == pub || !VerifyData(ai.pub, "devices", DeviceListText(ai.ver, ai.devices, ai.removed), ai.sig)) continue;
        if (ai.ver > accounts[ai.pub].ver) accounts[ai.pub] = ai;
    }
    // conversations: merged with what this device already had
    json chatsJ = p.value("chats", json::object());  // (kept alive: items() of a temporary would dangle)
    for (auto& [key, arr] : chatsJ.items()) {
        if (key == settings.deviceId || !arr.is_array()) continue;
        Conversation& cv = chats[key];
        cv.peer = key;
        for (auto& mj : arr) {
            ChatMessage m = MessageFromJson(mj);
            m.pendingTo.clear();
            bool dup = false;
            for (auto& e : cv.messages) dup |= e.mid == m.mid;
            if (!dup) cv.messages.push_back(m);
        }
        std::stable_sort(cv.messages.begin(), cv.messages.end(), [](auto& x, auto& y) { return x.ts < y.ts; });
    }
    if (!fromDevice.empty()) MergeChatInto("@" + pub, fromDevice);  // our old chat with that device (another account then)
    std::vector<std::string> ids;
    for (auto& [id, d] : devices) ids.push_back(id);
    for (auto& id : ids) MergeChats(id);
    MarkDirty();
    // reconnect everywhere: the other devices learn the new account from the next Hello
    std::vector<std::shared_ptr<Connection>> cs = conns;
    Spawn([this, cs] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::vector<std::string> peers;
        for (auto& o : cs) {
            peers.push_back(o->peerId);
            CloseConn(o, "account changed");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        for (auto& pr : peers) ConnectPeer(pr);
    });
    return true;
}

void Engine::HandleLinkGrant(const std::shared_ptr<Connection>& c, const json& j) {
    auto lj = linkJoining_.find(c->peerId);
    if (lj == linkJoining_.end() || SteadyMs() - lj->second > 10 * 60000) return;  // only what we agreed to
    linkJoining_.erase(lj);
    if (AdoptAccount(j, c->peerId, true, nullptr))
        Emit({EventType::LinkDone, c->peerId, "This device is now linked to the account of " + settings.displayName + "."});
}

void Engine::LeaveAccount() {
    account_.Generate();
    settings.accountSecret = account_.seedHex;
    settings.accountDevices = {{settings.deviceId, identity_.PubHex()}};
    settings.accountRemoved.clear();
    settings.accountVer = NowMs();
    settings.accountBackedUp = false;
    SignAccountDevices();
    accountCert_ = SignData(account_, "device", settings.deviceId + "\n" + identity_.PubHex());
    wan_.SetAccount(MyAccount(), accountCert_);
    MarkDirty();
    Emit({EventType::LinkDone, "", "This device was removed from your account and now has an account of its own."});
    std::vector<std::shared_ptr<Connection>> cs = conns;
    Spawn([this, cs] {
        std::vector<std::string> peers;
        for (auto& o : cs) {
            peers.push_back(o->peerId);
            CloseConn(o, "account changed");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        for (auto& pr : peers) ConnectPeer(pr);
    });
}

void Engine::RemoveMyDevice(const std::string& device) {
    auto lk = Lock();
    if (device == settings.deviceId) return;
    auto& list = settings.accountDevices;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const AccountDevice& a) { return a.id == device; }), list.end());
    if (std::find(settings.accountRemoved.begin(), settings.accountRemoved.end(), device) == settings.accountRemoved.end())
        settings.accountRemoved.push_back(device);
    settings.accountVer = std::max(NowMs(), settings.accountVer + 1);
    SignAccountDevices();
    auto it = devices.find(device);
    if (it != devices.end() && it->second.accountPub == MyAccount()) it->second.accountPub.clear();
    PushAccount();
    MarkDirty();
}

// ---- usernames

void Engine::ClaimUsername(const std::string& nameIn) {
    auto lk = Lock();
    std::string name = nameIn;
    for (auto& ch : name) ch = (char)tolower((unsigned char)ch);
    std::string room = RoomHash(settings.wanRoom, settings.wanPassword);
    usernameError.clear();
    usernamePending = true;
    json j = name.empty() ? json{{"t", "urel"}, {"sig", SignData(account_, "username", room + "\nrelease")}}
                          : json{{"t", "uclaim"}, {"name", name}, {"sig", SignData(account_, "username", room + "\n" + name)}};
    wan_.SendServer(j.dump());
}

void Engine::LookupUsername(const std::string& name) {
    auto lk = Lock();
    lookup = UserLookup();
    lookup.name = name;
    lookup.pending = true;
    wan_.SendServer(json{{"t", "ulook"}, {"name", name}}.dump());
}

void Engine::ConnectLookedUp() {
    auto lk = Lock();
    for (auto& [id, n] : lookup.devices) {
        if (id == settings.deviceId || IsConnected(id)) continue;
        lastWanConnect_[id] = SteadyMs();
        wan_.Connect(id);
    }
}

void Engine::HandleServerReply(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    auto lk = Lock();
    std::string t = j.value("t", ""), name = j.value("name", "");
    if (t == "uname") {
        myUsername = name;
    } else if (t == "uok") {
        myUsername = name;
        usernamePending = false;
    } else if (t == "uerr") {
        if (lookup.pending && name == lookup.name) {
            lookup.pending = false;
            lookup.error = j.value("msg", "error");
        } else {
            usernamePending = false;
            usernameError = j.value("msg", "error");
        }
    } else if (t == "uinfo") {
        lookup.pending = false;
        lookup.name = name;
        lookup.acc = j.value("acc", "");
        lookup.devices.clear();
        for (auto& d : j.value("devs", json::array()))
            if (d.is_object()) lookup.devices.push_back({d.value("id", ""), d.value("name", "")});
        if (lookup.acc == MyAccount()) lookup.error = "that is you";
    }
}

// ---- recovery

std::string Engine::RecoveryPhrase() {
    auto lk = Lock();
    return SeedToPhrase(settings.accountSecret);
}

void Engine::MarkBackedUp() {
    auto lk = Lock();
    settings.accountBackedUp = true;
    MarkDirty();
}

bool Engine::RestoreFromPhrase(const std::string& phrase, std::string* err) {
    std::string seed;
    if (!PhraseToSeed(phrase, seed, err)) return false;
    auto lk = Lock();
    if (seed == settings.accountSecret) return true;  // already this account
    // only the key: the device list, contacts and chats come back from the account's other devices
    json p = {{"secret", seed}, {"name", settings.displayName}};
    if (!AdoptAccount(p, "", false, err)) return false;
    settings.accountBackedUp = true;
    return true;
}

bool Engine::SaveBackup(const std::string& path, const std::string& password, std::string* err) {
    if (password.size() < 6) {
        if (err) *err = "use a password of at least 6 characters";
        return false;
    }
    std::string plain;
    {
        auto lk = Lock();
        plain = AccountPayload("").dump();
    }
    std::string blob = PasswordEncrypt(plain, password);
    json f = {{"app", kAppId}, {"type", "account-backup"}, {"version", 1}, {"data", Base64Encode(blob)}};
    if (!WriteWholeFile(path, f.dump(1))) {
        if (err) *err = "could not write the file";
        return false;
    }
    MarkBackedUp();
    return true;
}

bool Engine::RestoreBackup(const std::string& path, const std::string& password, std::string* err) {
    std::string text, blob, plain;
    json f;
    if (!ReadWholeFile(path, text, 256u << 20) || (f = json::parse(text, nullptr, false)).is_discarded() ||
        f.value("type", "") != "account-backup" || !Base64Decode(f.value("data", ""), blob)) {
        if (err) *err = "not a LunaShare account backup";
        return false;
    }
    if (!PasswordDecrypt(blob, password, plain)) {
        if (err) *err = "wrong password (or the file is damaged)";
        return false;
    }
    json p = json::parse(plain, nullptr, false);
    if (p.is_discarded()) {
        if (err) *err = "the backup is damaged";
        return false;
    }
    auto lk = Lock();
    if (!AdoptAccount(p, "", false, err)) return false;
    settings.accountBackedUp = true;
    return true;
}

// ============================================================================ profile pictures

static std::string ProfileSigned(int64_t ver, const std::string& name, const std::string& avh) {
    return std::to_string(ver) + "\n" + name + "\n" + avh;
}

std::string Engine::DataDir() { return configDir.empty() ? ConfigDir() : configDir; }

std::string Engine::AvatarFile(const std::string& peer, const std::string& hash) {
    if (hash.size() < 16) return {};
    // the name contains the hash: a new picture is a new file (image caches key on the path)
    if (peer.empty()) return JoinPath(DataDir(), "avatar_" + hash.substr(0, 16) + ".jpg");
    std::string safe;
    for (char ch : peer) safe.push_back(isalnum((unsigned char)ch) || ch == '-' ? ch : '_');
    return JoinPath(JoinPath(DataDir(), "avatars"), safe + "_" + hash.substr(0, 16) + ".jpg");
}

std::string Engine::AvatarPath(const std::string& peer) {
    auto lk = Lock();
    std::string h, dev = peer;
    if (peer.empty()) h = settings.avatarHash;
    else if (IsContactKey(peer)) {
        int64_t best = -1;
        for (auto& id : DevicesOf(peer)) {
            auto& d = devices[id];
            if (d.profileVer > best) { best = d.profileVer; h = d.avatarHash; dev = id; }
        }
    } else if (auto it = devices.find(peer); it != devices.end()) h = it->second.avatarHash;
    return h.empty() ? std::string() : AvatarFile(dev, h);  // called every frame: no file system check
}

static bool ReadWholeFile(const std::string& path, std::string& out, size_t maxBytes) {
    FILE* f = OpenFile(path, "rb");
    if (!f) return false;
    out.clear();
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out.size() <= maxBytes) out.append(buf, n);
    fclose(f);
    return out.size() <= maxBytes;
}

static bool WriteWholeFile(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    FILE* f = OpenFile(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) { RemoveFile(tmp); return false; }
    RemoveFile(path);
    return RenameFile(tmp, path);
}

static bool LooksLikeJpeg(const std::string& d) {
    return d.size() > 4 && (uint8_t)d[0] == 0xFF && (uint8_t)d[1] == 0xD8 && (uint8_t)d[2] == 0xFF;
}

json Engine::ProfileJson() {
    std::string img;
    if (!settings.avatarHash.empty()) ReadWholeFile(AvatarFile("", settings.avatarHash), img, kMaxAvatarBytes);
    return {{"ver", settings.profileVer}, {"name", settings.displayName}, {"avh", settings.avatarHash},
            {"img", Base64Encode(img)},
            {"sig", SignData(identity_, "profile", ProfileSigned(settings.profileVer, settings.displayName, settings.avatarHash))}};
}

bool Engine::SetAvatar(const std::string& jpeg, std::string* err) {
    if (!jpeg.empty() && (jpeg.size() > kMaxAvatarBytes || !LooksLikeJpeg(jpeg))) {
        if (err) *err = "not a JPEG picture or too large";
        return false;
    }
    std::string hash = jpeg.empty() ? std::string() : Sha256Hex(jpeg.data(), jpeg.size());
    auto lk = Lock();
    std::string old = settings.avatarHash;
    if (!hash.empty() && !WriteWholeFile(AvatarFile("", hash), jpeg)) {
        if (err) *err = "could not save the picture";
        return false;
    }
    if (!old.empty() && old != hash) RemoveFile(AvatarFile("", old));
    settings.avatarHash = hash;
    settings.profileVer = std::max(NowMs(), settings.profileVer + 1);
    MarkDirty();
    json pj = ProfileJson();
    for (auto& c : conns)
        if (c->ready && !c->closed) Queue(c, Msg::ProfileData, pj);
    return true;
}

void Engine::HandleProfile(const std::shared_ptr<Connection>& c, const json& j) {
    auto it = devices.find(c->peerId);
    if (it == devices.end()) return;
    KnownDevice& d = it->second;
    int64_t ver = j.value("ver", (int64_t)0);
    std::string name = j.value("name", ""), avh = j.value("avh", ""), img64 = j.value("img", ""), sig = j.value("sig", "");
    if (ver < d.profileVer) return;  // older than what we have (out-of-order push)
    if (!VerifyData(d.pubKey, "profile", ProfileSigned(ver, name, avh), sig)) {
        Emit({EventType::Error, c->peerId, "Ignored a profile with an invalid signature from " + d.DisplayName()});
        return;
    }
    std::string img;
    if (!avh.empty()) {
        if (img64.size() > kMaxAvatarBytes * 4 / 3 + 8 || !Base64Decode(img64, img) || img.size() > kMaxAvatarBytes ||
            !LooksLikeJpeg(img) || Sha256Hex(img.data(), img.size()) != avh)
            return;
        MakeDirs(JoinPath(DataDir(), "avatars"));
        if (!WriteWholeFile(AvatarFile(c->peerId, avh), img)) return;
    }
    if (!d.avatarHash.empty() && d.avatarHash != avh) RemoveFile(AvatarFile(c->peerId, d.avatarHash));
    d.avatarHash = avh;
    d.profileVer = ver;
    if (!name.empty()) d.name = name;
    MarkDirty();
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
    std::string key = ChatKey(peer);
    Conversation& cv = chats[key];
    cv.peer = key;
    cv.messages.push_back(m);
    MarkDirty();
}

ChatMessage* Engine::FindBatchMessage(const std::string& peer, uint64_t batch) {
    auto it = chats.find(ChatKey(peer));
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
    if (c && c->mailbox) {  // answering something that came from the mailbox: directly if we can, else by mail
        if (auto rc = ReadyConn(c->peerId)) Queue(rc, m, j);
        else MailTo(c->peerId, m, j);
        return;
    }
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
                     {"tcpPort", boundPort}, {"pub", identity_.PubHex()}, {"sig", c->sec->Sign(identity_)}, {"caps", localCaps},
                     {"pv", settings.profileVer}, {"avh", settings.avatarHash},
                     {"acc", account_.PubHex()}, {"acs", accountCert_}, {"av", settings.accountVer}};
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
            if (call.state == CallState::Active && !call.group.empty() && !call.hub) GroupPeerLeft(c->peerId);
            if (watch.sid && watch.host.empty() && std::find(watchViewers_.begin(), watchViewers_.end(), c->peerId) != watchViewers_.end()) {
                watchViewers_.erase(std::remove(watchViewers_.begin(), watchViewers_.end(), c->peerId), watchViewers_.end());
                watchBuffering_.erase(c->peerId);  // (a viewer that went away doesn't keep everyone waiting)
                WatchUpdateHold();
                WatchBroadcast();
            }
            ListenPeerGone(c->peerId);
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
        if (f.type == Msg::PieceData) {
            HandlePieceData(c, f);
            continue;
        }
        if (f.type == Msg::WatchData) {
            HandleWatchData(c, f);
            continue;
        }
        if (f.type == Msg::FileData) {
            HandleData(c, f);
            continue;
        }
        if (f.type == Msg::Media) {
            if (f.payload.size() > 8 && groupCall_.load()) {
                uint64_t id = GetU64(f.payload.data());
                if (id && id == activeCallId_.load()) GroupMediaIn(c->peerId, f.payload.data() + 8, f.payload.size() - 8);
            } else if (f.payload.size() > 8 && c->peerId == activeCallPeerSnapshot()) {
                uint64_t id = GetU64(f.payload.data());
                if (id && id == activeCallId_.load() && onMedia) onMedia(id, 0, f.payload.data() + 8, f.payload.size() - 8);
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
    bool pieceTurn = false;
    while (!c->closed) {
        std::pair<Msg, std::string> msg;
        bool haveCtrl = false, haveData = false, havePiece = false, haveWatch = false;
        {
            std::unique_lock<std::mutex> l(c->qm);
            c->qcv.wait_for(l, std::chrono::milliseconds(200),
                            [&] { return c->closed || !c->ctrl.empty() || !c->sending.empty() || !c->pieces.empty() || !c->watchOut.empty(); });
            if (c->closed) break;
            if (!c->ctrl.empty()) {
                msg = std::move(c->ctrl.front());
                c->ctrl.pop_front();
                haveCtrl = true;
            } else if (!c->watchOut.empty()) {
                haveWatch = true;
            } else {
                haveData = !c->sending.empty();
                havePiece = !c->pieces.empty();
            }
        }
        if (haveCtrl) {
            if (!c->sec->Send(c->sock, msg.first, msg.second)) break;
            c->bytesOut += (int64_t)msg.second.size() + 5;
            continue;
        }
        if (haveWatch) {  // watch together: the movie comes before anything else that's bulk
            if (ProduceWatch(c, buf)) {
                if (!c->sec->Send(c->sock, Msg::WatchData, buf.data(), buf.size())) break;
                c->bytesOut += (int64_t)buf.size() + 5;
                totalUp += (int64_t)buf.size() - 16;
            }
            continue;
        }
        // shared-library pieces take turns with ordinary transfers
        if (havePiece && (!haveData || (pieceTurn = !pieceTurn))) {
            int64_t pw = 0;
            if (ProducePiece(c, buf, pw)) {
                if (!c->sec->Send(c->sock, Msg::PieceData, buf.data(), buf.size())) break;
                c->bytesOut += (int64_t)buf.size() + 5;
                totalUp += (int64_t)buf.size() - 36;
            } else if (pw > 0 && !haveData) {
                std::unique_lock<std::mutex> l(c->qm);
                c->qcv.wait_for(l, std::chrono::milliseconds(std::min<int64_t>(pw, 50)), [&] { return c->closed || !c->ctrl.empty(); });
            }
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
            // the account it belongs to: valid only with the account key's signature over this device
            std::string helloAcc;
            {
                std::string acc = j.value("acc", ""), acs = j.value("acs", "");
                if (!acc.empty() && VerifyData(acc, "device", id + "\n" + pub, acs)) helloAcc = acc;
            }
            bool isNew = devices.find(id) == devices.end();
            KnownDevice& d = Device(id);
            if (d.blocked) {
                CloseConn(c, "blocked");
                return;
            }
            if (GroupOnlyHidden(id)) {  // a group member I don't know: through the server only
                CloseConn(c, "groups via server");
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
                bool knownPerson = !helloAcc.empty() && !DevicesOf("@" + helloAcc).empty();
                if (helloAcc == MyAccount()) {
                    // one of my own devices: nothing to verify
                } else if (knownPerson) {
                    d.accountPub = helloAcc;  // a new device of someone we know: vouched for by their account
                    for (auto& o : DevicesOf("@" + helloAcc))
                        if (o != id && devices[o].verified) d.verified = true;
                    AddSystemMessage(id, "New device: " + j.value("name", std::string("?")) + " (" + j.value("platform", std::string()) + ")");
                } else {
                    AddSystemMessage(id, "End-to-end encrypted. Safety number: " +
                                             SafetyNumber(helloAcc.empty() ? identity_.PubHex() : MyAccount(), helloAcc.empty() ? pub : helloAcc) +
                                             " - compare it on both devices to make sure nobody is in between.");
                }
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
            {
                // account: a valid certificate (account key's signature over this device) makes the device part of
                // that person; their messages then share one conversation with the person's other devices
                const std::string& newAcc = helloAcc;
                if (newAcc != d.accountPub) {
                    std::string oldAcc = d.accountPub;
                    d.accountPub = newAcc;
                    if (!oldAcc.empty() && oldAcc != MyAccount() && DevicesOf("@" + oldAcc).empty())
                        MergeChatInto("@" + oldAcc, ChatKey(id));  // the person's only device moved: keep the chat
                    MergeChats(id);
                    if (!oldAcc.empty() && !newAcc.empty() && newAcc != MyAccount())
                        AddSystemMessage(id, "This device was linked to another account.");
                }
                if (!newAcc.empty()) {
                    int64_t av = j.value("av", (int64_t)0);
                    int64_t have = newAcc == MyAccount() ? settings.accountVer : accounts[newAcc].ver;
                    if (newAcc != MyAccount() && accounts[newAcc].pub.empty()) accounts[newAcc].pub = newAcc;
                    if (newAcc == MyAccount()) {
                        bool listed = false;
                        for (auto& ad : settings.accountDevices) listed |= ad.id == id && ad.pub == pub;
                        bool removedDev = std::find(settings.accountRemoved.begin(), settings.accountRemoved.end(), id) != settings.accountRemoved.end();
                        if (!listed && !removedDev) {  // holds the account key but my list lost it (restored here): add it
                            settings.accountDevices.push_back({id, pub});
                            settings.accountVer = std::max(NowMs(), std::max(settings.accountVer, av) + 1);
                            SignAccountDevices();
                            have = settings.accountVer;
                            PushAccount();
                        }
                    }
                    if (av > have) Queue(c, Msg::AccountGet, json::object());
                    else if (newAcc == MyAccount() && av < have) Queue(c, Msg::AccountData, AccountJson());  // my device is behind
                }
            }
            {
                // profile picture: fetch when theirs is newer than ours (or the cached file went missing)
                int64_t pv = j.value("pv", (int64_t)0);
                std::string avh = j.value("avh", "");
                bool missing = !avh.empty() && !FileExists(AvatarFile(id, avh));
                if (pv > d.profileVer || (avh != d.avatarHash && pv >= d.profileVer) || missing) Queue(c, Msg::ProfileGet, json::object());
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
            std::string key = FrameKey(c, j);
            if (key.empty()) {
                Queue(c, Msg::ChatAck, KeyField("#" + j.value("gid", ""), {{"mid", mid}}));  // (so it isn't sent again)
                break;
            }
            Conversation& cv = chats[key];
            cv.peer = key;
            bool dup = false;
            for (auto& m : cv.messages)
                if (m.mid == mid && !m.outgoing) dup = true;  // (a person's devices each send their copy)
            if (!dup && !mid.empty()) {
                ChatMessage m;
                m.mid = mid;
                m.ts = c->mailbox ? std::min(NowMs(), j.value("ts", NowMs())) : NowMs();  // (from the mailbox: when it was sent)
                m.text = j.value("text", "");
                m.replyTo = j.value("re", "");
                m.replyText = j.value("ret", "").substr(0, 400);
                m.edited = j.value("edited", (int64_t)0);
                m.forwarded = j.value("fwd", false);
                if (j.contains("poll")) PollFromJson(j["poll"], m);
                m.delivered = true;
                m.readSent = false;
                bool me = false;
                if (IsGroupKey(key)) {
                    for (auto& x : j.value("men", json::array()))
                        if (x.is_string() && m.mentions.size() < 64) {
                            m.mentions.push_back(x.get<std::string>());
                            me |= !MyAccount().empty() && x.get<std::string>() == MyAccount();
                        }
                    if (me) cv.mentioned = true;
                }
                if (IsGroupKey(key)) m.from = ChatKey(c->peerId);
                else
                    for (auto& d : DevicesOf(key)) devices[d].viaGroup = false;
                m.pendingTo = MyOtherDevices();  // my other devices get it too, in case the sender didn't know them
                cv.messages.push_back(m);
                if (c->mailbox) std::stable_sort(cv.messages.begin(), cv.messages.end(), [](auto& a, auto& b) { return a.ts < b.ts; });
                cv.unread++;
                cv.readPending = true;
                for (auto& msg : cv.messages)
                    if (msg.mid == mid && !msg.outgoing) {
                        for (auto& d : std::vector<std::string>(msg.pendingTo)) QueueChatTo(d, key, msg);
                        break;
                    }
                Emit({EventType::ChatReceived, key, m.text, 0, me ? "mention" : ""});
                MarkDirty();
            }
            Queue(c, Msg::ChatAck, KeyField(key, {{"mid", mid}}));
            break;
        }
        case Msg::ChatSync: {
            // from another device of my own account: a message it sent or received
            std::string mid = j.value("mid", ""), key = j.value("chat", "");
            auto dv = devices.find(c->peerId);
            if (dv == devices.end() || dv->second.accountPub != MyAccount() || mid.empty() || key.empty() || key == settings.deviceId) break;
            if (key == c->peerId) break;  // a chat with that device itself stays separate
            if (IsGroupKey(key)) {
                auto g = groups.find(key.substr(1));
                if (g != groups.end() && g->second.left) {
                    Queue(c, Msg::ChatSyncAck, {{"mid", mid}});
                    break;
                }
                if (g == groups.end()) EnsureGroup(key.substr(1), "");
            }
            Conversation& cv = chats[key];
            cv.peer = key;
            bool out = j.value("out", false), dup = false;
            for (auto& m : cv.messages)
                if (m.mid == mid) dup = true;
            if (!dup) {
                ChatMessage m;
                m.mid = mid;
                m.outgoing = out;
                m.ts = j.value("ts", NowMs());
                m.text = j.value("text", "");
                m.from = j.value("from", "");
                m.replyTo = j.value("re", "");
                m.replyText = j.value("ret", "").substr(0, 400);
                m.edited = j.value("edited", (int64_t)0);
                m.forwarded = j.value("fwd", false);
                if (j.contains("poll")) PollFromJson(j["poll"], m);
                for (auto& x : j.value("men", json::array()))
                    if (x.is_string() && m.mentions.size() < 64) m.mentions.push_back(x.get<std::string>());
                m.delivered = true;
                cv.messages.push_back(m);
                std::stable_sort(cv.messages.begin(), cv.messages.end(), [](auto& a, auto& b) { return a.ts < b.ts; });
                if (!out) {
                    cv.unread++;
                    cv.readPending = true;
                    for (auto& x : cv.messages)
                        if (x.mid == mid) x.readSent = false;
                    Emit({EventType::ChatReceived, key, m.text});
                }
                MarkDirty();
            }
            Queue(c, Msg::ChatSyncAck, {{"mid", mid}});
            break;
        }
        case Msg::ChatRead: {
            // "seen": from the person's device (my messages were read), or from my own device (I read them there)
            auto dv = devices.find(c->peerId);
            bool mine = dv != devices.end() && dv->second.accountPub == MyAccount();
            std::string key = mine ? j.value("chat", "") : FrameKey(c, j);
            auto it = chats.find(key);
            if (it == chats.end()) break;
            std::set<std::string> mids;
            for (auto& x : j.value("mids", json::array()))
                if (x.is_string()) mids.insert(x.get<std::string>());
            for (auto& m : it->second.messages) {
                if (!mids.count(m.mid)) continue;
                if (!mine && m.outgoing && !m.read) {
                    m.read = true;
                    m.delivered = true;
                    MarkDirty();
                } else if (mine && !m.outgoing && !m.readSent) {
                    m.readSent = true;
                    MarkDirty();
                }
            }
            if (mine) it->second.unread = 0;
            break;
        }
        case Msg::ChatSyncAck: {
            std::string mid = j.value("mid", "");
            for (auto& [k, cv] : chats)
                for (auto& m : cv.messages)
                    if (m.mid == mid) {
                        auto& p = m.pendingTo;
                        p.erase(std::remove(p.begin(), p.end(), c->peerId), p.end());
                        MarkDirty();
                    }
            break;
        }
        case Msg::AccountGet:
            Queue(c, Msg::AccountData, AccountJson());
            break;
        case Msg::AccountData:
            HandleAccount(c, j);
            break;
        case Msg::LinkOffer: {
            auto dv = devices.find(c->peerId);
            if (dv == devices.end() || dv->second.blocked || dv->second.accountPub == MyAccount()) break;
            linkAsked_[c->peerId] = SteadyMs();
            Event ev{EventType::LinkRequest, c->peerId, c->sec->Code()};
            ev.a = j.value("name", PeerName(c->peerId));
            Emit(ev);
            break;
        }
        case Msg::LinkAnswer: {
            auto lo = linkOffered_.find(c->peerId);
            if (lo == linkOffered_.end() || SteadyMs() - lo->second > 10 * 60000) break;
            linkOffered_.erase(lo);
            if (!j.value("accept", false)) {
                Emit({EventType::LinkDone, c->peerId, "Linking was declined on " + PeerName(c->peerId) + "."});
                break;
            }
            KnownDevice& d = Device(c->peerId);
            auto& list = settings.accountDevices;
            list.erase(std::remove_if(list.begin(), list.end(), [&](const AccountDevice& a) { return a.id == c->peerId; }), list.end());
            list.push_back({c->peerId, d.pubKey});
            auto& rm = settings.accountRemoved;
            rm.erase(std::remove(rm.begin(), rm.end(), c->peerId), rm.end());
            settings.accountVer = std::max(NowMs(), settings.accountVer + 1);
            SignAccountDevices();
            d.accountPub = MyAccount();
            d.trusted = true;
            Queue(c, Msg::LinkGrant, AccountPayload(c->peerId));
            // tell my other devices and my contacts about the new member
            for (auto& o : conns)
                if (o != c && o->ready && !o->closed) Queue(o, Msg::AccountData, AccountJson());
            Emit({EventType::LinkDone, c->peerId, PeerName(c->peerId) + " is now linked to your account."});
            MarkDirty();
            break;
        }
        case Msg::LinkGrant:
            HandleLinkGrant(c, j);
            break;
        case Msg::ChatDelete: {
            // only messages this device sent us can be removed this way
            json mids = j.value("mids", json::array());
            std::string key = FrameKey(c, j), who = ChatKey(c->peerId);
            auto it = chats.find(key);
            if (it != chats.end() && mids.is_array()) {
                auto& msgs = it->second.messages;
                size_t before = msgs.size();
                msgs.erase(std::remove_if(msgs.begin(), msgs.end(), [&](const ChatMessage& m) {
                               if (m.outgoing || m.kind == MsgKind::System) return false;
                               if (IsGroupKey(key) && m.from != who) return false;  // only their own messages
                               for (auto& x : mids)
                                   if (x.is_string() && x.get<std::string>() == m.mid) return true;
                               return false;
                           }),
                           msgs.end());
                if (msgs.size() != before) {
                    it->second.unread = std::max(0, it->second.unread - (int)(before - msgs.size()));
                    Emit({EventType::ChatDeleted, key, ""});
                    MarkDirty();
                }
            }
            Queue(c, Msg::ChatDeleteAck, KeyField(key, {{"mids", mids}}));
            break;
        }
        case Msg::ChatVote: {
            std::string mid = j.value("mid", "");
            int64_t ts = j.value("ts", (int64_t)0);
            auto dv = devices.find(c->peerId);
            bool mine = dv != devices.end() && !MyAccount().empty() && dv->second.accountPub == MyAccount();
            std::string key = mine && j.contains("chat") ? j.value("chat", "") : FrameKey(c, j);
            std::string who = mine ? MyVoterKey() : ChatKey(c->peerId);
            auto it = chats.find(key);
            if (it == chats.end() || mid.empty()) break;
            for (auto& m : it->second.messages)
                if (m.mid == mid && !m.pollQ.empty()) {
                    auto& v = m.votes[who];
                    if (ts > v.ts && ts <= NowMs() + 86400000LL) {
                        v.opts.clear();
                        for (auto& o : j.value("o", json::array()))
                            if (o.is_number_integer() && o.get<int>() >= 0 && o.get<int>() < (int)m.pollOpts.size() && v.opts.size() < 12)
                                v.opts.push_back(o.get<int>());
                        if (!m.pollMulti && v.opts.size() > 1) v.opts.resize(1);
                        v.ts = ts;
                        Emit({EventType::Info, key, ""});
                        MarkDirty();
                    }
                    break;
                }
            break;
        }
        case Msg::ChatPin: {
            std::string mid = j.value("mid", "");
            int64_t ts = j.value("ts", (int64_t)0);
            auto dv = devices.find(c->peerId);
            bool mine = dv != devices.end() && !MyAccount().empty() && dv->second.accountPub == MyAccount();
            std::string key = mine && j.contains("chat") ? j.value("chat", "") : FrameKey(c, j);
            auto it = chats.find(key);
            if (it == chats.end() || mid.empty()) break;
            for (auto& m : it->second.messages)
                if (m.mid == mid) {
                    if (ts > m.pinTs && ts <= NowMs() + 86400000LL) {
                        m.pinned = j.value("on", false);
                        m.pinTs = ts;
                        Emit({EventType::Info, key, ""});
                        MarkDirty();
                    }
                    break;
                }
            break;
        }
        case Msg::ChatEdit: {
            // a message changed by who wrote it (or, from my other device, one of mine)
            std::string mid = j.value("mid", ""), text = j.value("text", "");
            int64_t ts = j.value("ts", (int64_t)0);
            auto dv = devices.find(c->peerId);
            bool mine = dv != devices.end() && dv->second.accountPub == MyAccount();
            std::string key = mine && j.contains("chat") ? j.value("chat", "") : FrameKey(c, j), who = ChatKey(c->peerId);
            auto it = chats.find(key);
            if (it != chats.end() && !mid.empty() && text.size() < 64 * 1024)
                for (auto& m : it->second.messages) {
                    if (m.mid != mid || m.kind != MsgKind::Text) continue;
                    bool ok = mine ? m.outgoing : (!m.outgoing && (!IsGroupKey(key) || m.from == who));
                    if (ok && ts > m.edited) {
                        m.text = text;
                        m.edited = ts;
                        Emit({EventType::Info, key, ""});
                        MarkDirty();
                    }
                    break;
                }
            json ack = mine ? json{{"chat", key}, {"mid", mid}} : KeyField(key, {{"mid", mid}});
            Queue(c, Msg::ChatEditAck, ack);
            break;
        }
        case Msg::SyncData: {
            auto dv = devices.find(c->peerId);
            if (!settings.syncOwn || dv == devices.end() || MyAccount().empty() || dv->second.accountPub != MyAccount()) break;  // (mine only)
            std::string kind = j.value("kind", "");
            if (kind.empty() || kind.size() > 32 || !j.contains("data")) break;
            for (auto it = syncIn_.begin(); it != syncIn_.end();)  // (an older one of the same kind isn't needed any more)
                it = it->first == kind ? syncIn_.erase(it) : it + 1;
            if (syncIn_.size() < 16) syncIn_.push_back({kind, j["data"]});
            MarkDirty();
            break;
        }
        case Msg::ChatEditAck: {
            std::string mid = j.value("mid", "");
            auto dv = devices.find(c->peerId);
            bool mine = dv != devices.end() && dv->second.accountPub == MyAccount();
            std::string key = mine && j.contains("chat") ? j.value("chat", "") : FrameKey(c, j);
            auto it = chats.find(key);
            if (it != chats.end() && !IsGroupKey(key) && !mine)  // (a group: kept for the others; my devices: for the contact)
                for (auto& m : it->second.messages)
                    if (m.mid == mid && m.editPending) {
                        m.editPending = false;
                        MarkDirty();
                    }
            break;
        }
        case Msg::ChatDeleteAck: {
            json mids = j.value("mids", json::array());
            auto it = chats.find(FrameKey(c, j));
            if (it != chats.end() && mids.is_array() && !IsGroupKey(it->first)) {  // (in a group: kept for the others)
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
            std::string key = FrameKey(c, j);
            auto it = chats.find(key);
            if (it != chats.end())
                for (auto& m : it->second.messages)
                    if (m.mid == mid && m.kind != MsgKind::System) {
                        if (m.theirReaction != emoji) {
                            m.theirReaction = emoji;
                            MarkDirty();
                            if (!emoji.empty()) {
                                std::string about = m.kind == MsgKind::Files ? "[files] " + m.text : m.text;
                                if (about.size() > 60) about = about.substr(0, 57) + "...";
                                Event ev{EventType::ChatReaction, key, emoji};
                                ev.a = about;
                                Emit(ev);
                            }
                        }
                        break;
                    }
            Queue(c, Msg::ChatReactAck, KeyField(key, {{"mid", mid}, {"emoji", emoji}}));
            break;
        }
        case Msg::GroupInfo:
            HandleGroupInfo(c, j);
            break;
        case Msg::ProfileGet:
            Queue(c, Msg::ProfileData, ProfileJson());
            break;
        case Msg::ProfileData:
            HandleProfile(c, j);
            break;
        case Msg::ChatReactAck: {
            std::string mid = j.value("mid", ""), emoji = j.value("emoji", "");
            auto it = chats.find(FrameKey(c, j));
            if (it != chats.end() && !IsGroupKey(it->first))  // (in a group: resent to whoever connects)
                for (auto& m : it->second.messages)
                    if (m.mid == mid && m.myReaction == emoji && !m.reactionSent) {
                        m.reactionSent = true;
                        MarkDirty();
                    }
            break;
        }
        case Msg::ChatAck: {
            std::string mid = j.value("mid", "");
            auto it = chats.find(FrameKey(c, j));
            if (it != chats.end())
                for (auto& m : it->second.messages)
                    if (m.outgoing && m.mid == mid) {
                        m.delivered = true;
                        m.pendingTo.erase(std::remove(m.pendingTo.begin(), m.pendingTo.end(), c->peerId), m.pendingTo.end());
                        MarkDirty();
                    }
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
                std::string key = ChatKey(c->peerId);
                Conversation& cv = chats[key];
                cv.peer = key;
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
        case Msg::CallJoin: case Msg::CallWant:
            HandleGroupCall(c, f.type, j);
            break;
        case Msg::WatchInvite: case Msg::WatchJoin: case Msg::WatchLeave: case Msg::WatchCtl: case Msg::WatchState:
        case Msg::WatchReport: case Msg::WatchRead: case Msg::WatchSubs:
            if (!c->mailbox) HandleWatch(c, f.type, j);
            break;
        case Msg::ListenInvite: case Msg::ListenJoin: case Msg::ListenLeave: case Msg::ListenCtl: case Msg::ListenState:
        case Msg::ListenAdd:
            if (!c->mailbox) HandleListen(c, f.type, j);
            break;
        case Msg::HistoryAsk: case Msg::HistoryData:
            if (!c->mailbox) HandleHistory(c, f.type, j);
            break;
        case Msg::CallInvite: {
            uint64_t id = j.value("call", (uint64_t)0);
            if (j.contains("gid")) {
                HandleGroupCall(c, f.type, j);
                break;
            }
            if (!id || c->mailbox) break;
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
            bool invited = call.peer == c->peerId || std::find(call.invited.begin(), call.invited.end(), c->peerId) != call.invited.end();
            if (call.id != id || call.state != CallState::OutgoingRinging || !invited) break;
            // answered here: the person's other devices stop ringing
            for (auto& d : call.invited)
                if (d != c->peerId) QueueToPeer(d, Msg::CallEnd, {{"call", id}, {"reason", "answered elsewhere"}});
            call.peer = c->peerId;
            call.invited.clear();
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
            if (j.contains("gid")) {
                HandleGroupCall(c, f.type, j);
                break;
            }
            if (call.id != id) break;
            std::string reason = j.value("reason", "hangup");
            auto& inv = call.invited;
            if (call.state == CallState::OutgoingRinging && std::find(inv.begin(), inv.end(), c->peerId) != inv.end()) {
                // one of several devices: "busy" / "no voice module" there just leaves the others ringing; a decline
                // ends the call everywhere
                inv.erase(std::remove(inv.begin(), inv.end(), c->peerId), inv.end());
                if (reason != "declined" && !inv.empty()) {
                    if (call.peer == c->peerId) call.peer = inv[0];
                    break;
                }
                call.peer = c->peerId;
                FinishCall(reason, true);  // (tells the remaining ones)
                break;
            }
            if (call.peer != c->peerId) break;
            FinishCall(reason, false);
            break;
        }
        case Msg::LibGet: case Msg::LibList: case Msg::LibChanged: case Msg::PieceHave: case Msg::PieceHaveReply:
        case Msg::PieceMapGet: case Msg::PieceMap: case Msg::PieceGet: case Msg::PieceNo:
            HandleLibrary(c, f.type, j);
            break;
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

void Engine::SyncPush(const std::string& kind, const json& data) {
    auto lk = Lock();
    if (!settings.syncOwn) return;
    syncOut_[kind] = data;
    for (auto& d : MyOtherDevices())
        if (auto c = ReadyConn(d)) Queue(c, Msg::SyncData, {{"kind", kind}, {"data", data}});
}

void Engine::SetLock(const std::string& hash, const std::string& salt, int len) {
    auto lk = Lock();
    settings.lockHash = hash;
    settings.lockSalt = salt;
    settings.lockLen = len;
    MarkDirty();
}

void Engine::SetUpdateState(int64_t last, const std::string& seen) {
    auto lk = Lock();
    settings.updateLast = last;
    settings.updateSeen = seen;
    MarkDirty();
}

void Engine::SetPrefsVer(int64_t ver) {
    auto lk = Lock();
    settings.prefsVer = ver;
    MarkDirty();
}

void Engine::SetMutedChats(const std::map<std::string, int64_t>& m) {
    auto lk = Lock();
    settings.mutedChats = m;
    MarkDirty();
}

bool Engine::SyncPop(std::string& kind, json& data) {
    auto lk = Lock();
    if (syncIn_.empty()) return false;
    kind = syncIn_.front().first;
    data = std::move(syncIn_.front().second);
    syncIn_.pop_front();
    return true;
}

void Engine::OnReady(const std::shared_ptr<Connection>& c) {
    const std::string& peer = c->peerId;
    Emit({EventType::PeerOnline, peer, PeerName(peer)});
    {  // my own device: what I keep the same everywhere
        auto dv = devices.find(peer);
        if (settings.syncOwn && dv != devices.end() && !MyAccount().empty() && dv->second.accountPub == MyAccount())
            for (auto& [kind, data] : syncOut_) Queue(c, Msg::SyncData, {{"kind", kind}, {"data", data}});
    }
    if (IsContact(peer)) {  // their Shared folder (and a new source for running downloads)
        Queue(c, Msg::LibGet, json::object());
        for (auto& [id, d] : swarm) d->lastHave = 0;
    }
    // messages waiting for this device: in its person's conversation, or (my own device) in any conversation
    for (auto& [key, cv] : chats)
        for (auto& m : cv.messages)
            if (std::find(m.pendingTo.begin(), m.pendingTo.end(), peer) != m.pendingTo.end()) QueueChatTo(peer, key, m);
    auto it = chats.find(ChatKey(peer));
    if (it != chats.end()) {
        for (auto& m : it->second.messages)
            if (m.outgoing && !m.delivered && m.kind == MsgKind::Text && m.pendingTo.empty())  // (from older versions)
                Queue(c, Msg::Chat, {{"mid", m.mid}, {"ts", m.ts}, {"text", m.text}});
        if (!it->second.pendingDeletes.empty()) Queue(c, Msg::ChatDelete, {{"mids", it->second.pendingDeletes}});
        for (auto& m : it->second.messages) {
            if (!m.reactionSent) Queue(c, Msg::ChatReact, {{"mid", m.mid}, {"emoji", m.myReaction}});
            if (m.editPending && m.outgoing) Queue(c, Msg::ChatEdit, {{"mid", m.mid}, {"text", m.text}, {"ts", m.edited}});
            if (m.pinTs) Queue(c, Msg::ChatPin, {{"mid", m.mid}, {"on", m.pinned}, {"ts", m.pinTs}});  // (whoever missed it; the later wins)
            if (auto v = m.votes.find(MyVoterKey()); v != m.votes.end() && !m.pollQ.empty())
                Queue(c, Msg::ChatVote, {{"mid", m.mid}, {"o", v->second.opts}, {"ts", v->second.ts}});
        }
    }
    {  // my own device: the pins of every chat
        auto dv = devices.find(peer);
        if (dv != devices.end() && !MyAccount().empty() && dv->second.accountPub == MyAccount())
            for (auto& [key, cv] : chats)
                for (auto& m : cv.messages)
                    if (m.pinTs) Queue(c, Msg::ChatPin, {{"chat", key}, {"mid", m.mid}, {"on", m.pinned}, {"ts", m.pinTs}});
    }
    // watching together: someone of that conversation (or one of my devices) came online - they're invited too
    if (watch.sid && watch.host.empty() && watch.libId.empty() && std::find(watchViewers_.begin(), watchViewers_.end(), peer) == watchViewers_.end()) {
        auto mine = MyOtherDevices();
        for (auto& k : watchKeys) {
            auto to = DevicesOf(k);
            if (std::find(to.begin(), to.end(), peer) != to.end() || (k == watchKeys[0] && std::find(mine.begin(), mine.end(), peer) != mine.end())) {
                Queue(c, Msg::WatchInvite, KeyField(k, {{"sid", watch.sid}, {"name", watch.name}, {"size", watch.size}}));
                break;
            }
        }
    }
    // a group call going on that this device's person is in: "I'm in"
    if (call.state == CallState::Active && !call.group.empty() && !call.hub) {
        auto dv = devices.find(peer);
        const Group* g = GroupOf(call.group);
        bool in = false;
        for (auto& p : call.peers) in |= p.dev == peer;
        if (!in && g && dv != devices.end() && dv->second.accountPub != MyAccount() && g->Has(dv->second.accountPub))
            Queue(c, Msg::CallJoin, {{"call", call.id}, {"gid", call.group.substr(1)}});
        if (in) SyncGroupMedia();  // (a new connection to someone in the call)
    }
    // groups this device is in (or one of mine): the current member list, deletions and reactions
    {
        auto dv = devices.find(peer);
        std::string acc = dv != devices.end() ? dv->second.accountPub : "";
        for (auto& [gid, g] : groups) {
            if (acc.empty() || g.ver <= 0 || (acc != MyAccount() && !g.Has(acc))) continue;
            if (g.left && acc != MyAccount()) continue;
            Queue(c, Msg::GroupInfo, GroupJson(g));
            auto gc = chats.find("#" + gid);
            if (gc == chats.end() || g.left) continue;
            if (!gc->second.pendingDeletes.empty()) Queue(c, Msg::ChatDelete, {{"mids", gc->second.pendingDeletes}, {"gid", gid}});
            for (auto& m : gc->second.messages) {
                if (!m.reactionSent) Queue(c, Msg::ChatReact, {{"mid", m.mid}, {"emoji", m.myReaction}, {"gid", gid}});
                if (m.editPending && m.outgoing) Queue(c, Msg::ChatEdit, {{"mid", m.mid}, {"text", m.text}, {"ts", m.edited}, {"gid", gid}});
                if (m.pinTs) Queue(c, Msg::ChatPin, {{"mid", m.mid}, {"on", m.pinned}, {"ts", m.pinTs}, {"gid", gid}});
                if (auto v = m.votes.find(MyVoterKey()); v != m.votes.end() && !m.pollQ.empty())
                    Queue(c, Msg::ChatVote, {{"mid", m.mid}, {"o", v->second.opts}, {"ts", v->second.ts}, {"gid", gid}});
            }
        }
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
        cfg.hidden = settings.wanHidden;
        // per server, so servers can't match devices with each other
        if (std::string mid = machineId.empty() ? MachineId() : machineId; !mid.empty()) cfg.hw = RoomHash("\x02hw\n" + settings.wanServer, mid);
        for (auto& l : settings.wanLinks) {
            cfg.links.push_back(LinkToken(l.secret));
            if (!l.mine) cfg.inv.push_back(cfg.links.back());
        }
        for (auto& r : settings.wanInviteRooms) {
            std::vector<std::string> toks;
            for (auto& l : settings.wanLinks)
                if (l.mine && l.room == r.secret) toks.push_back(LinkToken(l.secret));
            if (!toks.empty()) cfg.groups.push_back({LinkToken(r.secret), r.label, toks});
        }
        for (auto& [id, p] : online) p.lastWan = 0;
    }
    wan_.Stop();
    if (!on) return;
    wan_.signMail = [this](const std::string& challenge) { return SignData(identity_, "mailbox", challenge); };
    wan_.onServerReply = [this](const std::string& text) { HandleServerReply(text); };
    {
        auto lk = Lock();
        wan_.SetAccount(MyAccount(), accountCert_);
    }
    wan_.onMail = [this](const std::string& id, const std::string& d) { return HandleMail(id, d); };
    wan_.onHub = [this](int from, const std::string& msg) { GroupHubIn(from, msg); };
    wan_.onHubMembers = [this](int you, const std::vector<std::string>& ids) { GroupHubMembers(you, ids); };
    wan_.onHubError = [this](const std::string& msg) { Emit({EventType::Error, "", "Group call through the server: " + msg}); };
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
                    // private mode: don't make myself known to strangers in the room (only known devices and links)
                    if (settings.wanHidden && !wp.viaLink && (d == devices.end() || d->second.viaGroup)) blocked = true;
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
            SwarmTick();  // shared-library downloads: ask for pieces
            if (now - lastMailScan_ > 20000) {  // messages for offline devices: into the server mailbox
                lastMailScan_ = now;
                MailPending();
            }
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
            GroupCallTick();
            WatchTick();
            ListenTick();
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

void Engine::SendChat(const std::string& peerArg, const std::string& text, const std::string& replyTo, const std::vector<std::string>& mentions,
                      bool forwarded) {
    SendChatPoll(peerArg, text, replyTo, mentions, forwarded, json());
}

void Engine::SendChatPoll(const std::string& peerArg, const std::string& text, const std::string& replyTo,
                          const std::vector<std::string>& mentions, bool forwarded, const json& poll) {
    if (text.empty()) return;
    auto lk = Lock();
    const std::string peer = IsContactKey(peerArg) ? peerArg : ChatKey(peerArg);
    ChatMessage m;
    m.mid = RandomHex(8);
    m.outgoing = true;
    m.ts = NowMs();
    m.text = text;
    m.forwarded = forwarded;
    if (poll.is_object()) PollFromJson(poll, m);
    if (IsGroupKey(peerArg))
        for (auto& a : mentions)
            if (m.mentions.size() < 64) m.mentions.push_back(a);
    if (!replyTo.empty()) {  // an answer: which message, and the start of it (in case the other side doesn't have it)
        auto ch = chats.find(peer);
        if (ch != chats.end())
            for (auto& q : ch->second.messages)
                if (q.mid == replyTo) {
                    m.replyTo = replyTo;
                    m.replyText = q.kind == MsgKind::Files ? std::string("[files]") : q.text.substr(0, 160);
                    while (!m.replyText.empty() && ((unsigned char)m.replyText.back() & 0xC0) == 0x80) m.replyText.pop_back();
                    if (!m.replyText.empty() && ((unsigned char)m.replyText.back() & 0x80)) m.replyText.pop_back();
                }
    }
    // to every device of the person, and a copy to my other devices
    for (auto& d : DevicesOf(peer)) {
        m.pendingTo.push_back(d);
        if (!IsGroupKey(peer)) devices[d].viaGroup = false;  // a group member I now talk to directly: a contact
    }
    for (auto& d : MyOtherDevices()) m.pendingTo.push_back(d);
    Conversation& cv = chats[peer];
    cv.peer = peer;
    cv.messages.push_back(m);
    ChatMessage& sm = cv.messages.back();
    for (auto& d : std::vector<std::string>(sm.pendingTo)) QueueChatTo(d, peer, sm);
    MarkDirty();
}

void Engine::QueueChatTo(const std::string& device, const std::string& key, ChatMessage& m) {
    auto d = devices.find(device);
    bool mine = d != devices.end() && d->second.accountPub == MyAccount();
    if (!mine && !m.outgoing) return;
    Msg type = mine ? Msg::ChatSync : Msg::Chat;
    json j = mine ? json{{"chat", key}, {"mid", m.mid}, {"ts", m.ts}, {"text", m.text}, {"out", m.outgoing}}
                  : KeyField(key, {{"mid", m.mid}, {"ts", m.ts}, {"text", m.text}});
    if (mine && !m.from.empty()) j["from"] = m.from;
    if (!m.replyTo.empty()) {
        j["re"] = m.replyTo;
        j["ret"] = m.replyText;
    }
    if (!m.mentions.empty()) j["men"] = m.mentions;
    if (m.forwarded) j["fwd"] = true;
    if (!m.pollQ.empty()) j["poll"] = {{"q", m.pollQ}, {"o", m.pollOpts}, {"m", m.pollMulti}};
    if (m.edited) j["edited"] = m.edited;
    if (auto c = ReadyConn(device)) {
        Queue(c, type, j);
        return;
    }
    if (IsGroupKey(key) && wan_.Running() && SteadyMs() - lastWanConnect_[device] > 60000 && !GroupOnlyHidden(device)) {
        lastWanConnect_[device] = SteadyMs();  // a group member we may never have met: try to reach them directly
        wan_.Connect(device);
    }
    // not reachable now: leave it in the server mailbox (once), if the message isn't too old
    if (std::find(m.mailedTo.begin(), m.mailedTo.end(), device) != m.mailedTo.end()) return;
    if (NowMs() - m.ts > 9LL * 86400000) return;
    if (MailTo(device, type, j)) {
        m.mailedTo.push_back(device);
        MarkDirty();
    }
}

// ---- offline messages

// Seals a frame for one device and leaves it in the server mailbox. Inside: who sends it (device key + account
// certificate) and a signature over the frame bound to the recipient, so it can't be forged or redirected.
bool Engine::MailTo(const std::string& device, Msg type, const json& j) {
    if (!wan_.Running() || !wan_.Registered()) return false;
    auto d = devices.find(device);
    if (d == devices.end() || d->second.pubKey.size() != 64 || d->second.blocked) return false;
    json inner = {{"from", settings.deviceId}, {"fpub", identity_.PubHex()}, {"acc", MyAccount()}, {"acs", accountCert_},
                  {"to", device}, {"t", (int)type}, {"j", j}, {"ts", NowMs()}};
    std::string m = inner.dump(-1, ' ', false, json::error_handler_t::replace);
    std::string plain = json{{"m", m}, {"sig", SignData(identity_, "mail", d->second.pubKey + "\n" + m)}}.dump();
    std::string sealed = SealTo(d->second.pubKey, plain);
    if (sealed.empty() || sealed.size() > 5000) return false;  // (longer texts wait for a direct connection)
    wan_.MailPut(d->second.pubKey, Sha256Hex(sealed.data(), sealed.size()).substr(0, 32), Base64Encode(sealed));
    return true;
}

bool Engine::HandleMail(const std::string& id, const std::string& dataB64) {
    (void)id;
    std::string sealed, plain;
    if (!Base64Decode(dataB64, sealed) || !OpenSealed(identity_, sealed, plain)) return true;  // not for us / damaged
    json outer = json::parse(plain, nullptr, false);
    if (!outer.is_object()) return true;
    std::string m = outer.value("m", ""), sig = outer.value("sig", "");
    json inner = json::parse(m, nullptr, false);
    if (!inner.is_object()) return true;
    std::string from = inner.value("from", ""), fpub = inner.value("fpub", ""), acc = inner.value("acc", ""), acs = inner.value("acs", "");
    if (from.empty() || from == settings.deviceId || inner.value("to", "") != settings.deviceId) return true;
    if (!VerifyData(fpub, "mail", identity_.PubHex() + "\n" + m, sig)) return true;
    Msg type = (Msg)inner.value("t", 0);
    switch (type) {
        case Msg::Chat: case Msg::ChatAck: case Msg::ChatSync: case Msg::ChatSyncAck: case Msg::ChatDelete:
        case Msg::ChatDeleteAck: case Msg::ChatReact: case Msg::ChatReactAck: case Msg::ChatRead: case Msg::GroupInfo: break;
        case Msg::ChatEdit: case Msg::ChatEditAck: case Msg::ChatPin: case Msg::ChatVote: break;
        case Msg::CallInvite: case Msg::CallEnd: break;  // (group calls; a call with one person needs a connection)
        default: return true;  // nothing else travels by mail
    }
    {
        auto lk = Lock();
        auto dv = devices.find(from);
        bool certOk = !acc.empty() && VerifyData(acc, "device", from + "\n" + fpub, acs);
        if (dv != devices.end() && !dv->second.pubKey.empty()) {
            if (dv->second.pubKey != fpub || dv->second.blocked) return true;  // not the device we know
            if (certOk && dv->second.accountPub.empty()) {
                dv->second.accountPub = acc;
                MergeChats(from);
            }
        } else {
            // a device we never met: accepted only as a new device of someone we know (or one of mine)
            bool knownPerson = certOk && (acc == MyAccount() || !DevicesOf("@" + acc).empty());
            if (!knownPerson) return true;
            KnownDevice& nd = Device(from);
            nd.pubKey = fpub;
            nd.accountPub = acc;
            nd.name = acc == MyAccount() ? settings.displayName : PeerName("@" + acc);
        }
    }
    auto c = std::make_shared<Connection>();
    c->peerId = from;
    c->ready = true;
    c->mailbox = true;
    c->transport = Transport::Internet;
    Frame f;
    f.type = type;
    std::string payload = inner.value("j", json::object()).dump();
    f.payload.assign(payload.begin(), payload.end());
    HandleFrame(c, f);
    return true;
}

void Engine::MailPending() {
    if (!wan_.Running() || !wan_.Registered()) return;
    for (auto& [key, cv] : chats)
        for (auto& m : cv.messages)
            for (auto& d : std::vector<std::string>(m.pendingTo))
                if (!ReadyConn(d) && std::find(m.mailedTo.begin(), m.mailedTo.end(), d) == m.mailedTo.end()) QueueChatTo(d, key, m);
}

uint64_t Engine::SendPaths(const std::string& peer, const std::vector<std::string>& paths) {
    std::vector<std::pair<std::string, std::string>> files;
    for (auto& p : paths) {
        if (IsDirectory(p)) CollectFiles(p, files);
        else if (FileExists(p)) files.emplace_back(p, BaseName(p));
    }
    if (files.empty()) return 0;
    std::string batchName;
    if (paths.size() == 1) batchName = BaseName(paths[0]);
    if (batchName.empty() || files.size() > 1)
        batchName = (paths.size() == 1 ? batchName + " - " : std::string()) + std::to_string(files.size()) + " files";
    if (files.size() == 1) batchName = files[0].second;
    return SendFileList(peer, files, batchName);
}

uint64_t Engine::SendFileList(const std::string& peer, const std::vector<std::pair<std::string, std::string>>& filesIn, const std::string& batchName) {
    std::vector<std::pair<std::string, std::string>> files;
    for (auto& f : filesIn)
        if (FileExists(f.first)) files.push_back(f);
    if (files.empty()) return 0;
    uint64_t batch = RandomU64();
    auto lk = Lock();
    std::string key = IsContactKey(peer) ? peer : ChatKey(peer), device = Resolve(peer);
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
        t->peer = device;
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
    Conversation& cv = chats[key];
    cv.peer = key;
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

void Engine::ConnectPeer(const std::string& peerOrKey) {
    auto lk = Lock();
    if (IsGroupKey(peerOrKey)) {
        for (auto& d : DevicesOf(peerOrKey))
            if (!GroupOnlyHidden(d)) ConnectPeer(d);
        return;
    }
    if (IsContactKey(peerOrKey)) {
        for (auto& d : DevicesOf(peerOrKey)) ConnectPeer(d);
        return;
    }
    const std::string& peer = peerOrKey;
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
            if (c->peerId == peer || (IsContactKey(peer) && c->ready && ChatKey(c->peerId) == peer)) cs.push_back(c);
    }
    for (auto& c : cs) {
        Queue(c, Msg::Bye, json::object());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    for (auto& c : cs) CloseConn(c, "user");
}

// (for a person: applies to all of their devices)
void Engine::SetCustomName(const std::string& peer, const std::string& name) {
    auto lk = Lock();
    for (auto& d : DevicesOf(peer)) Device(d).customName = name;
    MarkDirty();
}

void Engine::SetTrusted(const std::string& peer, bool v) {
    auto lk = Lock();
    for (auto& d : DevicesOf(peer)) Device(d).trusted = v;
    MarkDirty();
}

void Engine::SetBlocked(const std::string& peer, bool v) {
    std::vector<std::string> ds;
    {
        auto lk = Lock();
        ds = DevicesOf(peer);
        for (auto& d : ds) {
            Device(d).blocked = v;
            if (v) Device(d).trusted = false;
        }
        MarkDirty();
    }
    if (v)
        for (auto& d : ds) Disconnect(d);
}

void Engine::SetVerified(const std::string& peer, bool v) {
    auto lk = Lock();
    for (auto& d : DevicesOf(peer)) Device(d).verified = v;
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
    // a person (or a device of one): compare account keys - the same number for all their devices
    if (IsContactKey(peer)) return SafetyNumber(MyAccount(), peer.substr(1));
    auto it = devices.find(peer);
    if (it != devices.end() && !it->second.accountPub.empty()) return SafetyNumber(MyAccount(), it->second.accountPub);
    if (it == devices.end() || it->second.pubKey.empty()) return "";
    return SafetyNumber(identity_.PubHex(), it->second.pubKey);
}

void Engine::ForgetDevice(const std::string& peer) {
    std::vector<std::string> ds;
    {
        auto lk = Lock();
        ds = DevicesOf(peer);
    }
    for (auto& d : ds) Disconnect(d);
    auto lk = Lock();
    for (auto& d : ds) {
        auto dv = devices.find(d);
        if (dv != devices.end() && !dv->second.avatarHash.empty()) RemoveFile(AvatarFile(d, dv->second.avatarHash));
        devices.erase(d);
        chats.erase(d);
    }
    if (IsContactKey(peer)) accounts.erase(peer.substr(1));
    chats.erase(peer);
    MarkDirty();
}

void Engine::ClearChat(const std::string& peerArg) {
    auto lk = Lock();
    const std::string peer = IsContactKey(peerArg) ? peerArg : ChatKey(peerArg);
    auto it = chats.find(peer);
    if (it != chats.end()) it->second.messages.clear();
    MarkDirty();
}

void Engine::DeleteMessages(const std::string& peerArg, const std::vector<std::string>& mids, bool forEveryone) {
    auto lk = Lock();
    const std::string peer = IsContactKey(peerArg) ? peerArg : ChatKey(peerArg);
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
        for (auto& d : DevicesOf(peer)) QueueToPeer(d, Msg::ChatDelete, KeyField(peer, {{"mids", remote}}));
    }
    MarkDirty();
}

std::string Engine::MyVoterKey() { return MyAccount().empty() ? settings.deviceId : "@" + MyAccount(); }

void Engine::SendPoll(const std::string& peerArg, const std::string& question, const std::vector<std::string>& answers, bool several) {
    // older versions see it as text
    std::string text = "\xF0\x9F\x93\x8A " + question;  // (a chart emoji)
    for (size_t i = 0; i < answers.size(); i++) text += "\n" + std::to_string(i + 1) + ". " + answers[i];
    SendChatPoll(peerArg, text, "", {}, false, {{"q", question}, {"o", answers}, {"m", several}});
}

void Engine::Vote(const std::string& peerArg, const std::string& mid, const std::vector<int>& answers) {
    auto lk = Lock();
    const std::string peer = IsContactKey(peerArg) || IsGroupKey(peerArg) ? peerArg : ChatKey(peerArg);
    auto it = chats.find(peer);
    if (it == chats.end()) return;
    for (auto& m : it->second.messages)
        if (m.mid == mid && !m.pollQ.empty()) {
            auto& v = m.votes[MyVoterKey()];
            v.opts.clear();
            for (int a : answers)
                if (a >= 0 && a < (int)m.pollOpts.size() && std::find(v.opts.begin(), v.opts.end(), a) == v.opts.end()) v.opts.push_back(a);
            if (!m.pollMulti && v.opts.size() > 1) v.opts.resize(1);
            v.ts = std::max(NowMs(), v.ts + 1);
            for (auto& d : DevicesOf(peer)) QueueToPeer(d, Msg::ChatVote, KeyField(peer, {{"mid", mid}, {"o", v.opts}, {"ts", v.ts}}));
            for (auto& d : MyOtherDevices()) QueueToPeer(d, Msg::ChatVote, {{"chat", peer}, {"mid", mid}, {"o", v.opts}, {"ts", v.ts}});
            MarkDirty();
            return;
        }
}

void Engine::PinMessage(const std::string& peerArg, const std::string& mid, bool on) {
    auto lk = Lock();
    const std::string peer = IsContactKey(peerArg) ? peerArg : ChatKey(peerArg);
    auto it = chats.find(peer);
    if (it == chats.end()) return;
    for (auto& m : it->second.messages)
        if (m.mid == mid && m.kind != MsgKind::System) {
            m.pinned = on;
            m.pinTs = std::max(NowMs(), m.pinTs + 1);
            for (auto& d : DevicesOf(peer)) QueueToPeer(d, Msg::ChatPin, KeyField(peer, {{"mid", mid}, {"on", on}, {"ts", m.pinTs}}));
            for (auto& d : MyOtherDevices()) QueueToPeer(d, Msg::ChatPin, {{"chat", peer}, {"mid", mid}, {"on", on}, {"ts", m.pinTs}});
            MarkDirty();
            return;
        }
}

void Engine::StarMessage(const std::string& peerArg, const std::string& mid, bool on) {
    auto lk = Lock();
    const std::string peer = IsContactKey(peerArg) ? peerArg : ChatKey(peerArg);
    auto it = chats.find(peer);
    if (it == chats.end()) return;
    for (auto& m : it->second.messages)
        if (m.mid == mid) {
            m.starred = on;
            MarkDirty();
        }
}

bool Engine::EditMessage(const std::string& peerArg, const std::string& mid, const std::string& text) {
    if (text.empty()) return false;
    auto lk = Lock();
    const std::string peer = IsContactKey(peerArg) ? peerArg : ChatKey(peerArg);
    auto it = chats.find(peer);
    if (it == chats.end()) return false;
    for (auto& m : it->second.messages)
        if (m.mid == mid && m.outgoing && m.kind == MsgKind::Text) {
            if (m.text == text) return true;
            m.text = text;
            m.edited = NowMs();
            if (!m.delivered && !m.pendingTo.empty()) m.mailedTo.clear();  // (still waiting: the mailbox copy goes out changed)
            // (also when not delivered yet: the old text may already be on its way)
            m.editPending = true;
            json j = KeyField(peer, {{"mid", mid}, {"text", text}, {"ts", m.edited}});
            for (auto& d : DevicesOf(peer)) QueueToPeer(d, Msg::ChatEdit, j);
            for (auto& d : MyOtherDevices()) QueueToPeer(d, Msg::ChatEdit, {{"chat", peer}, {"mid", mid}, {"text", text}, {"ts", m.edited}});
            MarkDirty();
            return true;
        }
    return false;
}

void Engine::React(const std::string& peerArg, const std::string& mid, const std::string& emoji) {
    auto lk = Lock();
    const std::string peer = IsContactKey(peerArg) ? peerArg : ChatKey(peerArg);
    auto it = chats.find(peer);
    if (it == chats.end()) return;
    for (auto& m : it->second.messages)
        if (m.mid == mid && m.kind != MsgKind::System) {
            if (m.myReaction == emoji) return;
            m.myReaction = emoji;
            m.reactionSent = false;
            for (auto& d : DevicesOf(peer)) QueueToPeer(d, Msg::ChatReact, KeyField(peer, {{"mid", mid}, {"emoji", emoji}}));
            MarkDirty();
            return;
        }
}

void Engine::MarkRead(const std::string& peerArg) {
    auto lk = Lock();
    const std::string peer = IsContactKey(peerArg) ? peerArg : ChatKey(peerArg);
    auto it = chats.find(peer);
    if (it == chats.end()) return;
    Conversation& cv = it->second;
    cv.unread = 0;
    cv.mentioned = false;
    if (!cv.readPending) return;
    cv.readPending = false;
    // read receipts: to the person's devices, and to my own so they clear the conversation too
    json mids = json::array();
    for (auto& m : cv.messages)
        if (!m.outgoing && m.kind == MsgKind::Text && !m.readSent) {
            m.readSent = true;
            mids.push_back(m.mid);
        }
    if (mids.empty()) return;
    std::vector<std::string> to = DevicesOf(peer);
    for (auto& d : MyOtherDevices()) to.push_back(d);
    for (auto& d : to) {
        json j = KeyField(peer, {{"mids", mids}, {"chat", peer}});
        if (!QueueToPeer(d, Msg::ChatRead, j)) MailTo(d, Msg::ChatRead, j);
    }
    MarkDirty();
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
    settings.avatarHash = old.avatarHash;  // only SetAvatar changes the profile picture
    settings.accountSecret = old.accountSecret;  // the account changes only through linking / recovery
    settings.accountDevices = old.accountDevices;
    settings.accountRemoved = old.accountRemoved;
    settings.accountVer = old.accountVer;
    settings.accountSig = old.accountSig;
    settings.accountBackedUp = old.accountBackedUp;
    settings.profileVer = old.profileVer;
    settings.mutedChats = old.mutedChats;  // only MuteChat changes these (the settings page keeps an older copy)
    settings.prefsVer = old.prefsVer;
    settings.updateLast = old.updateLast;
    settings.lockHash = old.lockHash;  // only SetLock changes the PIN
    settings.lockSalt = old.lockSalt;
    settings.lockLen = old.lockLen;
    settings.updateSeen = old.updateSeen;
    if (settings.downloadDir.empty()) settings.downloadDir = DefaultDownloadDir();
    MakeDirs(settings.downloadDir);
    if (settings.tcpPort != old.tcpPort && running_) restartListener_ = true;
    if ((settings.bluetooth != old.bluetooth || settings.btChannel != old.btChannel) && running_) StartBtServer();
    // Restarting these joins their threads, which may be waiting for our lock: do it from the maintenance thread.
    if (running_ && (settings.discovery != old.discovery || settings.mdns != old.mdns || settings.displayName != old.displayName)) restartMdns_ = true;
    if (running_ && (settings.wanEnabled != old.wanEnabled || settings.wanServer != old.wanServer || settings.wanRoom != old.wanRoom ||
                     settings.wanPassword != old.wanPassword || settings.wanNatPmp != old.wanNatPmp || settings.wanRelay != old.wanRelay ||
                     settings.wanLocalPort != old.wanLocalPort || settings.wanForwardedPort != old.wanForwardedPort ||
                     settings.wanHidden != old.wanHidden || !(settings.wanLinks == old.wanLinks) ||
                     !(settings.wanInviteRooms == old.wanInviteRooms)))
        restartWan_ = true;
    else if (running_ && settings.displayName != old.displayName)
        wan_.UpdateName(settings.displayName);
    MarkDirty();
}

bool Engine::ExportSettings(const std::string& path, bool includeHistory, bool includeIdentity, std::string* err, const json& appData) {
    json j;
    {
        auto lk = Lock();
        j = {{"app", kAppId}, {"type", "settings-export"}, {"version", 1}, {"exported", NowMs()},
             {"from", settings.displayName}, {"settings", ToJson(settings, includeIdentity)}};
        if (appData.is_object()) j["appData"] = appData;
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

bool Engine::ImportSettings(const std::string& path, bool importIdentity, std::string* err, json* appData) {
    json j;
    if (!LoadJsonFile(path, j) || !j.is_object() || j.value("app", "") != kAppId) {
        if (err) *err = "Not a LunaShare settings file";
        return false;
    }
    if (appData) *appData = j.value("appData", json::object());
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

uint64_t Engine::StartCall(const std::string& peerOrKey, const std::string& media) {
    auto lk = Lock();
    if (IsGroupKey(peerOrKey)) return StartGroupCall(peerOrKey, media);
    std::string peer = Resolve(peerOrKey);
    // a person: ring every connected device of theirs that can take this call; the first to answer gets it
    std::vector<std::string> ring;
    if (IsContactKey(peerOrKey))
        for (auto& d : DevicesOf(peerOrKey))
            if (PeerHasCap(d, CapForMedia(media))) ring.push_back(d);
    if (ring.empty()) ring.push_back(peer);
    else if (std::find(ring.begin(), ring.end(), peer) == ring.end()) peer = ring[0];
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
    call.invited = ring;
    for (auto& d : ring) QueueToPeer(d, Msg::CallInvite, {{"call", call.id}, {"media", media}});
    return call.id;
}

void Engine::AcceptCall() {
    auto lk = Lock();
    if (call.state != CallState::IncomingRinging) return;
    if (!call.group.empty()) {
        AcceptGroupCall();
        return;
    }
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
    if (!call.group.empty()) {
        FinishGroupCall(reason);
        return;
    }
    CallInfo c = call;
    if (notifyPeer) {
        QueueToPeer(c.peer, Msg::CallEnd, {{"call", c.id}, {"reason", reason}});
        for (auto& d : c.invited)
            if (d != c.peer) QueueToPeer(d, Msg::CallEnd, {{"call", c.id}, {"reason", reason}});
    }
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
        text = reason == "declined" ? "Declined call" : reason == "answered elsewhere" ? "Call answered on another device" : "Missed call";
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
    if (groupCall_.load()) return SendGroupMedia(id, (const uint8_t*)data, len);
    auto c = CallConn();
    if (!c) return false;
    return QueueMediaTo(c, id, (const uint8_t*)data, len);
}

bool Engine::QueueMediaTo(const std::shared_ptr<Connection>& c, uint64_t id, const uint8_t* data, size_t len) {
    bool video = data[0] == 2;
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
    if (groupCall_.load()) return GroupQueuedBytes();
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
