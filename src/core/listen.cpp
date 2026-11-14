// LunaShare - listening together: one device (the host) keeps a shared queue and its clock, everyone adds songs from
// their own music, and every song streams from the device it's on - straight to each listener, in the same 256 KB
// blocks as watch together (WatchRead / WatchData, with the song's id as "sid"). A song is small, so it's fetched
// whole, and the next one comes ahead of time. Everybody's music player follows the host's clock; a song change
// starts a moment later so the next one can arrive everywhere first.
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <thread>

#include "diag.h"
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
    listenHistory_.clear();
    listenNames_.clear();
    std::vector<std::string> to = DevicesOf(key);
    for (auto& d : MyOtherDevices()) to.push_back(d);
    json inv = KeyField(key, {{"sid", listen.sid}});
    for (auto& d : to) QueueToPeer(d, Msg::ListenInvite, inv);
    DIAG("listen: started %llx with %s - invited %d device(s)", (unsigned long long)listen.sid, key.substr(0, 12).c_str(), (int)to.size());
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
        DIAG("listen: join %llx - not connected to the host %s", (unsigned long long)sid, inv.host.substr(0, 8).c_str());
        ConnectPeer(inv.host);
        return false;
    }
    DIAG("listen: joined %llx (host %s)", (unsigned long long)sid, inv.host.substr(0, 8).c_str());
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
    listenHistory_.clear();
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

// remix: the part of a song - a random place past the intro, the whole part ending before the outro and the quiet
static double FragHead(double dur) { return std::max(8.0, dur * 0.08); }
static double FragTail(double dur) { return std::max(12.0, dur * 0.10); }
static double PickFrag(double dur, int len) {
    if (dur <= 0) return 0;
    double lo = FragHead(dur), hi = dur - FragTail(dur) - len;
    if (hi <= lo) return std::min(lo, dur * 0.2);
    return lo + (hi - lo) * (double)(RandomU64() % 1000000) / 1000000.0;
}

double ListenInfo::FragEnd() const {
    if (at < 0 || at >= (int)items.size()) return 0;
    double dur = items[(size_t)at].dur;
    if (!remix || dur <= 0) return dur;
    double end = std::min(dur - FragTail(dur), frag + remixLen);
    return std::max(end, std::min(dur, frag + 5));
}

void Engine::ListenGo(int at, double delay, double fade) {
    listen.at = std::clamp(at, 0, (int)listen.items.size());
    listen.playing = listen.at < (int)listen.items.size();
    listen.seq++;
    listen.fade = listen.playing ? fade : 0;
    listen.frag = listen.playing && listen.remix ? PickFrag(listen.items[(size_t)listen.at].dur, listen.remixLen) : 0;
    if (listen.playing) {
        uint64_t iid = listen.items[(size_t)listen.at].iid;
        if (std::find(listen.played.begin(), listen.played.end(), iid) == listen.played.end()) listen.played.push_back(iid);
        if (listenHistory_.empty() || listenHistory_.back() != iid) listenHistory_.push_back(iid);
        if (listenHistory_.size() > 500) listenHistory_.erase(listenHistory_.begin());
    }
    listen.nextIid = 0;
    if (listen.shuffle && listen.playing) {  // (picked now, so everyone can fetch it ahead)
        int nx = ListenNextIndex(true);
        if (nx < (int)listen.items.size() && nx != listen.at) listen.nextIid = listen.items[(size_t)nx].iid;
    }
    // (a blend: right away - the next song came ahead; otherwise a moment for it to arrive everywhere)
    ListenSetPos(!listen.playing ? 0 : fade > 0 ? listen.frag : listen.frag > 0 ? std::max(0.0, listen.frag - delay) : -delay);
    DIAG("listen: song %d/%d%s%s", listen.at + 1, (int)listen.items.size(),
            listen.playing ? " - " : " (the end)", listen.playing ? listen.items[(size_t)listen.at].name.c_str() : "");
    listen.ver++;
    ListenBroadcast();
}

int Engine::ListenNextIndex(bool skip) {
    int n = (int)listen.items.size();
    if (!n) return 0;
    if (listen.repeat == 2 && !skip && listen.at < n) return listen.at;  // (repeat one: when it ends by itself)
    if (listen.shuffle && listen.nextIid)  // (the one picked ahead)
        for (int i = 0; i < n; i++)
            if (listen.items[(size_t)i].iid == listen.nextIid && i != listen.at) return i;
    if (!listen.shuffle) {
        int nx = listen.at + 1;
        if (nx >= n) nx = listen.repeat == 1 ? 0 : n;
        return nx;
    }
    auto pick = [&]() -> int {
        std::vector<int> c;
        for (int i = 0; i < n; i++)
            if (i != listen.at && std::find(listen.played.begin(), listen.played.end(), listen.items[(size_t)i].iid) == listen.played.end())
                c.push_back(i);
        if (c.empty()) return -1;
        return c[(size_t)(RandomU64() % c.size())];
    };
    int r = pick();
    if (r >= 0) return r;
    if (listen.repeat != 1) return n;  // (everything played)
    listen.played.clear();             // a new round
    if (listen.at < n) listen.played.push_back(listen.items[(size_t)listen.at].iid);
    r = pick();
    return r >= 0 ? r : (listen.at < n ? listen.at : 0);
}

json Engine::ListenStateJson() {
    json items = json::array();
    for (auto& i : listen.items) items.push_back(ItemJson(i));
    return {{"sid", listen.sid}, {"items", items}, {"at", listen.at}, {"playing", listen.playing}, {"pos", ListenPos()},
            {"people", listen.people}, {"devs", listen.devs}, {"ver", listen.ver}, {"shuffle", listen.shuffle},
            {"repeat", listen.repeat}, {"played", listen.played}, {"next", listen.nextIid}, {"xf", listen.crossfade},
            {"remix", listen.remix}, {"rlen", listen.remixLen}, {"rfade", listen.remixFade}, {"frag", listen.frag},
            {"seq", listen.seq}, {"fade", listen.fade}};
}

void Engine::ListenBroadcast() {
    if (!listen.sid || !listen.host.empty()) return;
    int n = (int)listen.items.size();
    if (listen.shuffle && listen.at < n) {  // (the one picked to come next is still there - or another is picked)
        bool ok = false;
        for (int i = 0; i < n; i++)
            if (listen.items[(size_t)i].iid == listen.nextIid && i != listen.at) ok = true;
        if (!ok) {
            listen.nextIid = 0;
            int nx = ListenNextIndex(true);
            if (nx < n && nx != listen.at) listen.nextIid = listen.items[(size_t)nx].iid;
        }
    }
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
        ListenGo(ListenNextIndex(true), 1.0);
        return;
    } else if (op == "prev") {
        if (l.shuffle && ListenPos() <= 3 && listenHistory_.size() >= 2) {  // shuffled: the one that played before
            listenHistory_.pop_back();
            uint64_t back = listenHistory_.back();
            listenHistory_.pop_back();
            for (int i = 0; i < n; i++)
                if (l.items[(size_t)i].iid == back) {
                    ListenGo(i, 1.0);
                    return;
                }
            ListenSetPos(0);
        } else if (ListenPos() > 3 || l.at == 0) {
            ListenSetPos(0);
        } else {
            ListenGo(l.at - 1, 1.0);
            return;
        }
    } else if (op == "shuffle") {
        l.shuffle = pos != 0;
        l.played.clear();
        l.nextIid = 0;
        if (l.at < n) {
            l.played.push_back(l.items[(size_t)l.at].iid);
            if (l.shuffle) {
                int nx = ListenNextIndex(true);
                if (nx < n && nx != l.at) l.nextIid = l.items[(size_t)nx].iid;
            }
        }
    } else if (op == "repeat") {
        l.repeat = std::clamp((int)pos, 0, 2);
    } else if (op == "crossfade") {
        l.crossfade = std::clamp((int)pos, 0, 12);
    } else if (op == "remix") {
        l.remix = pos != 0;
        if (l.remix && l.at < n && ListenPos() < l.items[(size_t)l.at].dur) {  // (this song's part: from where it is now)
            l.frag = std::max(0.0, ListenPos());
            if (l.frag + 5 > l.items[(size_t)l.at].dur - FragTail(l.items[(size_t)l.at].dur)) l.frag = 0;
        }
    } else if (op == "remixlen") {
        l.remixLen = std::clamp((int)pos, 15, 180);
    } else if (op == "remixfade") {
        l.remixFade = std::clamp((int)pos, 1, 12);
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
    ListenGo(ListenNextIndex(false), 1.0);
}

void Engine::ListenAdd(std::vector<ListenItem> items, double startAt) {
    auto lk = Lock();
    if (!listen.sid) return;
    json out = json::array();
    std::vector<ListenItem> added;
    for (auto& i : items) {
        if (i.path.empty()) continue;
        i.size = FileSize(i.path);
        FILE* f = i.size > 0 ? OpenFile(i.path, "rb") : nullptr;
        DIAG("listen: add %s (%lld bytes)%s", BaseName(i.path).c_str(), (long long)i.size, f ? "" : " - CAN'T OPEN IT");
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
        QueueToPeer(listen.host, Msg::ListenAdd, {{"sid", listen.sid}, {"items", out}, {"start", startAt}});
        return;
    }
    bool ended = listen.at >= (int)listen.items.size();
    for (auto& i : added) listen.items.push_back(i);
    if (ended && startAt >= 0) {  // (it goes on from where it was; the others catch up as it comes)
        ListenGo(listen.at, 0);
        ListenSetPos(startAt);
        listen.ver++;
        ListenBroadcast();
    } else if (ended) ListenGo(listen.at, 1.5);  // (the queue had played out - or was empty: this one starts)
    else {
        listen.ver++;
        ListenBroadcast();
    }
}

// ---------------------------------------------------------------- others' songs coming here

void Engine::ListenCaches() {
    std::vector<const ListenItem*> want;
    for (int i : {listen.at, listen.NextForFetch()})
        if (i >= 0 && i < (int)listen.items.size() && listen.items[(size_t)i].owner != settings.deviceId &&
            (want.empty() || want[0] != &listen.items[(size_t)i]))
            want.push_back(&listen.items[(size_t)i]);
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
        DIAG("listen: fetching %s (%lld bytes) from %s%s%s", i->name.c_str(), (long long)i->size, i->owner.substr(0, 8).c_str(),
                ReadyConn(i->owner) ? "" : " - not connected", c ? "" : " - CAN'T MAKE ITS CACHE FILE");
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
        double fade = listen.remix ? listen.remixFade : listen.crossfade;
        double end = listen.FragEnd();
        bool due = cur && listen.playing && fade > 0 && end > 0 && ListenPos() >= end - fade;
        int nx = due ? ListenNextIndex(false) : 0;
        if (due && nx < (int)listen.items.size()) {  // the next one blends in before this one (or its part) ends
            ListenGo(nx, 0, fade);
        } else if (cur && listen.playing && listen.remix && end > 0 && ListenPos() >= end) {  // (the last part: done)
            ListenGo(ListenNextIndex(false), 1.0);
        } else if (cur && listen.playing && cur->dur > 0 && ListenPos() >= cur->dur + 1.0) {  // (the player didn't say: the clock does)
            ListenGo(ListenNextIndex(false), 1.0);
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
    {  // (said when the song or play/pause changes)
        int at = std::clamp(j.value("at", 0), 0, (int)items.size());
        bool pl = j.value("playing", false);
        if (at != l.at || pl != l.playing || items.size() != l.items.size())
            DIAG("listen: host says song %d/%d %s at %.1f s", at + 1, (int)items.size(), pl ? "playing" : "paused", j.value("pos", 0.0));
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
    l.shuffle = j.value("shuffle", false);
    l.nextIid = j.value("next", (uint64_t)0);
    l.crossfade = std::clamp(j.value("xf", 0), 0, 12);
    l.remix = j.value("remix", false);
    l.remixLen = std::clamp(j.value("rlen", 45), 15, 180);
    l.remixFade = std::clamp(j.value("rfade", 6), 1, 12);
    l.frag = std::max(0.0, j.value("frag", 0.0));
    l.seq = j.value("seq", (int64_t)0);
    l.fade = std::clamp(j.value("fade", 0.0), 0.0, 12.0);
    l.repeat = std::clamp(j.value("repeat", 0), 0, 2);
    l.played.clear();
    for (auto& p : j.value("played", json::array()))
        if (p.is_number_unsigned() && l.played.size() < 4000) l.played.push_back(p.get<uint64_t>());
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
            DIAG("listen: invited to %llx by %s (chat %s)", (unsigned long long)sid, dev.substr(0, 8).c_str(), key.substr(0, 12).c_str());
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
                if (curTheirs) {
                    listen.at = std::min(at, (int)listen.items.size() - 1);
                    ListenGo(ListenNextIndex(true), 1.0);
                }
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
            double start = j.value("start", -1.0);
            if (ended && start >= 0) {
                ListenGo(listen.at, 0);
                ListenSetPos(start);
                listen.ver++;
                ListenBroadcast();
            } else if (ended) ListenGo(listen.at, 1.5);
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
