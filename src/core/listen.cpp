// LunaShare - listening together: one device (the host) keeps a shared queue and its clock, everyone adds songs from
// their own music, and every song streams from the device it's on - straight to each listener, in the same 256 KB
// blocks as watch together (WatchRead / WatchData, with the song's id as "sid"). A song is small, so it's fetched
// whole, and the next one comes ahead of time. Everybody's music player follows the host's clock; a song change
// starts a moment later so the next one can arrive everywhere first.
#include <algorithm>
#include <chrono>
#include <thread>

#include "engine.h"
#include "platform.h"

namespace ls {

struct WatchCache;  // watch.cpp: the same cache, through these helpers
std::shared_ptr<WatchCache> NewListenCache(uint64_t iid, int64_t size, const std::string& dir);
void ListenCacheSet(const std::shared_ptr<WatchCache>& c, const std::string& owner, const std::shared_ptr<Connection>& conn,
                    const std::string& name, const std::string& key);
bool ListenCacheInfo(const std::shared_ptr<WatchCache>& c, uint64_t& iid, bool& saving, bool& done, int64_t& readPos,
                     int64_t& lastConnect, double& loaded);
void ListenCacheConnectAt(const std::shared_ptr<WatchCache>& c, int64_t at);
void ListenCacheDrop(const std::shared_ptr<WatchCache>& c);

static std::string Trim(std::string s, size_t n) {
    if (s.size() > n) {
        s.resize(n);
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
        if (!s.empty() && ((unsigned char)s.back() & 0x80)) s.pop_back();
    }
    return s;
}

static json ItemJson(const ListenItem& i) {
    return {{"iid", i.iid}, {"owner", i.owner}, {"by", i.by},     {"name", i.name}, {"title", i.title},
            {"artist", i.artist}, {"album", i.album}, {"dur", i.dur}, {"size", i.size}};
}

static bool ItemFrom(const json& j, ListenItem& i) {
    if (!j.is_object()) return false;
    i.iid = j.value("iid", (uint64_t)0);
    i.owner = Trim(j.value("owner", ""), 128);
    i.by = Trim(j.value("by", ""), 100);
    i.name = BaseName(Trim(j.value("name", ""), 255));
    i.title = Trim(j.value("title", ""), 255);
    i.artist = Trim(j.value("artist", ""), 255);
    i.album = Trim(j.value("album", ""), 255);
    i.dur = std::clamp(j.value("dur", 0.0), 0.0, 24 * 3600.0);
    i.size = j.value("size", (int64_t)0);
    return i.iid && i.size > 0 && i.size < (int64_t)4 << 30;
}

// ---------------------------------------------------------------- starting, joining, leaving

uint64_t Engine::StartListen(const std::string& key) {
    auto lk = Lock();
    if (key.empty()) return 0;
    if (listen.sid) LeaveListen();
    listen = ListenInfo();
    listen.sid = RandomU64() | 1;
    listen.key = key;
    listen.people = {settings.displayName};
    listen.devs = {settings.deviceId};
    listenMembers_.clear();
    listenNames_.clear();
    std::vector<std::string> to = DevicesOf(key);
    for (auto& d : MyOtherDevices()) to.push_back(d);
    json inv = KeyField(key, {{"sid", listen.sid}});
    for (auto& d : to) QueueToPeer(d, Msg::ListenInvite, inv);
    AddSystemMessage(key, "You started listening to music together.");
    Emit({EventType::Info, key, ""});
    return listen.sid;
}

bool Engine::JoinListen(uint64_t sid) {
    auto lk = Lock();
    if (listen.sid == sid) return true;
    auto it = std::find_if(listenInvites.begin(), listenInvites.end(), [&](const ListenInviteInfo& i) { return i.sid == sid; });
    if (it == listenInvites.end()) return false;
    ListenInviteInfo inv = *it;
    if (!ReadyConn(inv.host)) {
        ConnectPeer(inv.host);
        return false;
    }
    if (listen.sid) LeaveListen();
    listenInvites.erase(it);
    listen = ListenInfo();
    listen.sid = sid;
    listen.key = inv.key;
    listen.host = inv.host;
    listenHostSeen_ = SteadyMs();
    QueueToPeer(inv.host, Msg::ListenJoin, {{"sid", sid}, {"name", settings.displayName}});
    Emit({EventType::Info, inv.key, ""});
    return true;
}

void Engine::LeaveListen() {
    auto lk = Lock();
    if (!listen.sid) return;
    if (!listen.host.empty()) QueueToPeer(listen.host, Msg::ListenLeave, {{"sid", listen.sid}});
    EndListenLocked(listen.host.empty() ? "You ended listening together." : "");
}

void Engine::EndListenLocked(const std::string& why) {
    ListenInfo l = listen;
    if (l.host.empty())
        for (auto& d : listenMembers_) QueueToPeer(d, Msg::ListenLeave, {{"sid", l.sid}, {"end", true}});
    for (auto& o : listenOwn_) WatchServeClose(o.first);
    listenOwn_.clear();
    std::vector<std::shared_ptr<WatchCache>> caches;
    {
        std::lock_guard<std::mutex> g(watchCacheMtx_);
        caches.swap(listenCaches_);
    }
    for (auto& c : caches) ListenCacheDrop(c);
    listenMembers_.clear();
    listenNames_.clear();
    listen = ListenInfo();
    if (!why.empty() && !l.key.empty()) AddSystemMessage(l.key, why);
    Emit({EventType::Info, l.key, ""});
}

// ---------------------------------------------------------------- the queue and the clock

double Engine::ListenPos() {
    return listen.pos + (listen.playing ? (double)(SteadyMs() - listen.posAt) / 1000.0 : 0.0);
}

const ListenItem* Engine::ListenCurrent() {
    if (!listen.sid || listen.at < 0 || listen.at >= (int)listen.items.size()) return nullptr;
    return &listen.items[(size_t)listen.at];
}

void Engine::ListenSetPos(double pos) {
    listen.pos = pos;
    listen.posAt = SteadyMs();
}

void Engine::ListenGo(int at, double delay) {
    listen.at = std::clamp(at, 0, (int)listen.items.size());
    listen.playing = listen.at < (int)listen.items.size();
    ListenSetPos(listen.playing ? -delay : 0);
    listen.ver++;
    ListenBroadcast();
}

json Engine::ListenStateJson() {
    json items = json::array();
    for (auto& i : listen.items) items.push_back(ItemJson(i));
    return {{"sid", listen.sid}, {"items", items}, {"at", listen.at}, {"playing", listen.playing}, {"pos", ListenPos()},
            {"people", listen.people}, {"devs", listen.devs}, {"ver", listen.ver}};
}

void Engine::ListenBroadcast() {
    if (!listen.sid || !listen.host.empty()) return;
    listenLastBroadcast_ = SteadyMs();
    json st = ListenStateJson();
    for (auto& d : listenMembers_) QueueToPeer(d, Msg::ListenState, st);
    Emit({EventType::Info, listen.key, ""});
}

void Engine::ListenControl(const std::string& op, double pos, uint64_t iid) {
    auto lk = Lock();
    if (!listen.sid) return;
    if (!listen.host.empty()) {
        QueueToPeer(listen.host, Msg::ListenCtl, {{"sid", listen.sid}, {"op", op}, {"pos", pos}, {"iid", iid}});
        // (shown right away here; the host's answer follows)
        if (op == "play" || op == "pause") {
            double p = ListenPos();
            listen.playing = op == "play";
            ListenSetPos(std::max(0.0, p));
        } else if (op == "seek") {
            ListenSetPos(std::max(0.0, pos));
        }
        return;
    }
    ListenApplyCtl(op, pos, iid, settings.deviceId, true);
}

// the host applies a control (its own, or a listener's)
void Engine::ListenApplyCtl(const std::string& op, double pos, uint64_t iid, const std::string& who, bool host) {
    ListenInfo& l = listen;
    int n = (int)l.items.size();
    if (op == "play") {
        if (l.at >= n) return;
        double p = ListenPos();
        l.playing = true;
        ListenSetPos(p);
    } else if (op == "pause") {
        double p = ListenPos();
        l.playing = false;
        ListenSetPos(std::max(0.0, p));
    } else if (op == "seek") {
        if (l.at >= n) return;
        double d = l.items[(size_t)l.at].dur;
        ListenSetPos(std::clamp(pos, 0.0, d > 0 ? d : 1e9));
    } else if (op == "next") {
        ListenGo(l.at + 1, 1.0);
        return;
    } else if (op == "prev") {
        if (ListenPos() > 3 || l.at == 0) ListenSetPos(0);
        else {
            ListenGo(l.at - 1, 1.0);
            return;
        }
    } else if (op == "jump") {
        for (int i = 0; i < n; i++)
            if (l.items[(size_t)i].iid == iid) {
                ListenGo(i, 1.0);
                return;
            }
        return;
    } else if (op == "remove") {
        for (int i = 0; i < n; i++) {
            if (l.items[(size_t)i].iid != iid) continue;
            if (!host && l.items[(size_t)i].owner != who) return;  // (a listener: only their own songs)
            l.items.erase(l.items.begin() + i);
            if (i < l.at) l.at--;
            else if (i == l.at) {
                ListenGo(l.at, 1.0);
                return;
            }
            break;
        }
    } else {
        return;
    }
    l.ver++;
    ListenBroadcast();
}

void Engine::ListenEnded(uint64_t iid) {
    auto lk = Lock();
    const ListenItem* cur = ListenCurrent();
    if (!cur || cur->iid != iid || !listen.host.empty() || !listen.playing || ListenPos() < 1) return;
    ListenGo(listen.at + 1, 1.0);
}

void Engine::ListenAdd(std::vector<ListenItem> items) {
    auto lk = Lock();
    if (!listen.sid) return;
    json out = json::array();
    std::vector<ListenItem> added;
    for (auto& i : items) {
        if (i.path.empty()) continue;
        i.size = FileSize(i.path);
        FILE* f = i.size > 0 ? OpenFile(i.path, "rb") : nullptr;
        if (!f) continue;
        i.iid = RandomU64() | 1;
        i.owner = settings.deviceId;
        i.by = settings.displayName;
        if (i.name.empty()) i.name = BaseName(i.path);
        {
            std::lock_guard<std::mutex> g(watchServe_.m);
            watchServe_.files[i.iid] = {f, i.size};
        }
        listenOwn_[i.iid] = i.path;
        out.push_back(ItemJson(i));
        added.push_back(i);
    }
    if (added.empty()) return;
    if (!listen.host.empty()) {
        QueueToPeer(listen.host, Msg::ListenAdd, {{"sid", listen.sid}, {"items", out}});
        return;
    }
    bool ended = listen.at >= (int)listen.items.size();
    for (auto& i : added) listen.items.push_back(i);
    if (ended) ListenGo(listen.at, 1.5);  // (the queue had played out - or was empty: this one starts)
    else {
        listen.ver++;
        ListenBroadcast();
    }
}

// ---------------------------------------------------------------- others' songs coming here

void Engine::ListenCaches() {
    std::vector<const ListenItem*> want;
    for (int k = 0; k < 2; k++) {
        int i = listen.at + k;
        if (i >= 0 && i < (int)listen.items.size() && listen.items[(size_t)i].owner != settings.deviceId)
            want.push_back(&listen.items[(size_t)i]);
    }
    std::vector<std::shared_ptr<WatchCache>> keep, drop, all;
    {
        std::lock_guard<std::mutex> g(watchCacheMtx_);
        all = listenCaches_;
    }
    int64_t now = SteadyMs();
    for (auto& c : all) {
        uint64_t iid = 0;
        bool saving = false, done = false;
        int64_t readPos = 0, lastConnect = 0;
        double loaded = 0;
        ListenCacheInfo(c, iid, saving, done, readPos, lastConnect, loaded);
        bool wanted = std::any_of(want.begin(), want.end(), [&](const ListenItem* i) { return i->iid == iid; });
        bool inQueue = std::any_of(listen.items.begin(), listen.items.end(), [&](const ListenItem& i) { return i.iid == iid; });
        if (wanted || (saving && !done && inQueue)) keep.push_back(c);
        else drop.push_back(c);
    }
    for (auto* i : want) {
        bool have = false;
        for (auto& c : keep) {
            uint64_t iid = 0;
            bool s, d;
            int64_t rp, lc;
            double ld;
            ListenCacheInfo(c, iid, s, d, rp, lc, ld);
            if (iid == i->iid) have = true;
        }
        if (have) continue;
        auto c = NewListenCache(i->iid, i->size, JoinPath(ConfigPath(), "watch"));
        if (!c) continue;
        ListenCacheSet(c, i->owner, ReadyConn(i->owner), i->name, listen.key);
        keep.push_back(c);
    }
    {
        std::lock_guard<std::mutex> g(watchCacheMtx_);
        listenCaches_ = keep;
    }
    for (auto& c : drop) ListenCacheDrop(c);
    // keep them coming: the connection to each song's device, and the next blocks
    for (auto& c : keep) {
        uint64_t iid = 0;
        bool saving = false, done = false;
        int64_t readPos = 0, lastConnect = 0;
        double loaded = 0;
        ListenCacheInfo(c, iid, saving, done, readPos, lastConnect, loaded);
        if (done) continue;
        std::string owner;
        for (auto& i : listen.items)
            if (i.iid == iid) owner = i.owner;
        auto conn = ReadyConn(owner);
        ListenCacheSet(c, owner, conn, "", "");
        if (!conn) {
            if (now - lastConnect > 15000) {
                ListenCacheConnectAt(c, now);
                ConnectPeer(owner);
            }
            continue;
        }
        WatchRequestOn(c, readPos);
    }
}

int Engine::ListenRead(uint64_t iid, int64_t off, uint8_t* buf, int len, int waitMs) {
    auto wc = WatchCacheFor(iid);
    if (!wc) {  // (not asked for yet: the next tick does)
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, std::min(waitMs, 100))));
        return -2;
    }
    return WatchReadOn(wc, off, buf, len, waitMs);
}

void Engine::ListenSave(uint64_t iid) {
    auto lk = Lock();
    auto wc = WatchCacheFor(iid);
    if (!wc) return;
    MakeDirs(settings.downloadDir);
    ListenCacheSaveNow(wc, settings.downloadDir);
}

bool Engine::ListenSaving(uint64_t iid) {
    auto wc = WatchCacheFor(iid);
    if (!wc) return false;
    uint64_t id;
    bool saving = false, done;
    int64_t rp, lc;
    double ld;
    ListenCacheInfo(wc, id, saving, done, rp, lc, ld);
    return saving;
}

double Engine::ListenLoaded(uint64_t iid) {
    if (listenOwn_.count(iid)) return 1.0;
    auto wc = WatchCacheFor(iid);
    if (!wc) return 0;
    uint64_t id;
    bool saving, done;
    int64_t rp, lc;
    double ld = 0;
    ListenCacheInfo(wc, id, saving, done, rp, lc, ld);
    return ld;
}

// ---------------------------------------------------------------- the tick and the messages

void Engine::ListenTick() {
    // (invites older than six hours are forgotten)
    int64_t now = SteadyMs();
    listenInvites.erase(std::remove_if(listenInvites.begin(), listenInvites.end(),
                                       [&](const ListenInviteInfo& i) { return now - i.at > 6 * 3600 * 1000LL; }),
                        listenInvites.end());
    if (!listen.sid) return;
    if (listen.host.empty()) {
        const ListenItem* cur = ListenCurrent();
        if (cur && listen.playing && cur->dur > 0 && ListenPos() >= cur->dur + 1.0) {  // (the player didn't say: the clock does)
            ListenGo(listen.at + 1, 1.0);
        } else if (now - listenLastBroadcast_ > 2000) {
            ListenBroadcast();
        }
    } else {
        if (ReadyConn(listen.host)) listenHostSeen_ = now;
        else if (now - listenHostSeen_ > 30000) {
            EndListenLocked("Listening together ended: the host is gone.");
            return;
        } else if (now - listenHostSeen_ > 3000 && (now - listenHostSeen_) % 10000 < 300) {
            ConnectPeer(listen.host);
        }
    }
    ListenCaches();
}

void Engine::ListenPeerGone(const std::string& dev) {
    if (!listen.sid || !listen.host.empty()) return;
    if (std::find(listenMembers_.begin(), listenMembers_.end(), dev) == listenMembers_.end()) return;
    // (a short break isn't leaving: their songs stay while they come back; ListenLeave removes them)
    listenMembers_.erase(std::remove(listenMembers_.begin(), listenMembers_.end(), dev), listenMembers_.end());
    listen.people = {settings.displayName};
    listen.devs = {settings.deviceId};
    for (auto& d : listenMembers_) {
        listen.people.push_back(listenNames_[d]);
        listen.devs.push_back(d);
    }
    listen.ver++;
    ListenBroadcast();
}

void Engine::ListenApply(const json& j) {
    ListenInfo& l = listen;
    std::vector<ListenItem> items;
    for (auto& x : j.value("items", json::array())) {
        ListenItem i;
        if (ItemFrom(x, i)) {
            auto own = listenOwn_.find(i.iid);
            if (own != listenOwn_.end()) i.path = own->second;
            items.push_back(i);
        }
        if (items.size() >= 2000) break;
    }
    l.items = items;
    l.at = std::clamp(j.value("at", 0), 0, (int)l.items.size());
    l.playing = j.value("playing", false);
    ListenSetPos(j.value("pos", 0.0));
    l.people.clear();
    for (auto& p : j.value("people", json::array()))
        if (p.is_string() && l.people.size() < 64) l.people.push_back(Trim(p.get<std::string>(), 100));
    l.devs.clear();
    for (auto& p : j.value("devs", json::array()))
        if (p.is_string() && l.devs.size() < 64) l.devs.push_back(Trim(p.get<std::string>(), 128));
    l.ver = j.value("ver", (int64_t)0);
    Emit({EventType::Info, l.key, ""});
}

void Engine::HandleListen(const std::shared_ptr<Connection>& c, Msg type, const json& j) {
    uint64_t sid = j.value("sid", (uint64_t)0);
    if (!sid) return;
    const std::string& dev = c->peerId;
    bool host = listen.sid == sid && listen.host.empty();
    bool member = host && std::find(listenMembers_.begin(), listenMembers_.end(), dev) != listenMembers_.end();
    switch (type) {
        case Msg::ListenInvite: {
            std::string key = FrameKey(c, j);
            auto dv = devices.find(dev);
            if (key.empty() || dv == devices.end() || listen.sid == sid) return;
            if (dv->second.accountPub == MyAccount()) key = j.contains("gid") ? key : ChatKey(dev);
            listenInvites.erase(std::remove_if(listenInvites.begin(), listenInvites.end(), [&](const ListenInviteInfo& i) { return i.sid == sid; }),
                                listenInvites.end());
            listenInvites.push_back({sid, key, dev, SteadyMs()});
            if (listenInvites.size() > 10) listenInvites.erase(listenInvites.begin());
            std::string text = PeerName(ChatKey(dev)) + " invites you to listen to music together.";
            AddSystemMessage(key, text);
            Emit({EventType::ChatReceived, key, text});
            break;
        }
        case Msg::ListenJoin: {
            if (!host) return;
            auto to = DevicesOf(listen.key);
            auto mine = MyOtherDevices();
            if (std::find(to.begin(), to.end(), dev) == to.end() && std::find(mine.begin(), mine.end(), dev) == mine.end()) return;
            if (!member) listenMembers_.push_back(dev);
            std::string name = Trim(j.value("name", ""), 100);
            listenNames_[dev] = name.empty() ? PeerName(dev) : name;
            ListenPeerGone("");  // (people / devices again)
            listen.people = {settings.displayName};
            listen.devs = {settings.deviceId};
            for (auto& d : listenMembers_) {
                listen.people.push_back(listenNames_[d]);
                listen.devs.push_back(d);
            }
            listen.ver++;
            ListenBroadcast();
            break;
        }
        case Msg::ListenLeave:
            if (listen.sid != sid) {  // (an invite that ended before I joined)
                if (j.value("end", false))
                    listenInvites.erase(std::remove_if(listenInvites.begin(), listenInvites.end(), [&](const ListenInviteInfo& i) { return i.sid == sid; }),
                                        listenInvites.end());
                return;
            }
            if (!host) {
                if (dev == listen.host && j.value("end", false)) EndListenLocked("Listening together ended.");
                return;
            }
            if (!member) return;
            {  // their songs that haven't played go with them
                int at = listen.at;
                bool curTheirs = at < (int)listen.items.size() && listen.items[(size_t)at].owner == dev;
                std::vector<ListenItem> keep;
                for (int i = 0; i < (int)listen.items.size(); i++)
                    if (i <= at || listen.items[(size_t)i].owner != dev) keep.push_back(listen.items[(size_t)i]);
                listen.items = keep;
                ListenPeerGone(dev);
                if (curTheirs) ListenGo(at + 1, 1.0);
            }
            break;
        case Msg::ListenCtl:
            if (!member) return;
            ListenApplyCtl(j.value("op", ""), j.value("pos", 0.0), j.value("iid", (uint64_t)0), dev, false);
            break;
        case Msg::ListenState:
            if (listen.sid != sid || dev != listen.host) return;
            listenHostSeen_ = SteadyMs();
            ListenApply(j);
            break;
        case Msg::ListenAdd: {
            if (!member) return;
            bool ended = listen.at >= (int)listen.items.size();
            int n = 0;
            for (auto& x : j.value("items", json::array())) {
                ListenItem i;
                if (!ItemFrom(x, i) || listen.items.size() >= 2000) continue;
                i.owner = dev;
                i.by = listenNames_[dev];
                listen.items.push_back(i);
                n++;
            }
            if (!n) return;
            if (ended) ListenGo(listen.at, 1.5);
            else {
                listen.ver++;
                ListenBroadcast();
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace ls
