// LunaShare - watch together: one device streams a movie to a person or a group, and everyone's player plays the
// same moment. Nothing is transferred first: each viewer's player reads the file through WatchRead(), which asks
// the host for the parts it needs (256 KB blocks, up to 32 MB ahead of where the player reads) and keeps them in a
// temporary file that is deleted afterwards. The picture is the original - no re-encoding.
//
// The host keeps the clock: playing or paused, and the position at a moment. Anyone can play, pause or seek (a
// viewer's request goes to the host, which tells everyone). When someone is still loading - just joined, seeked,
// or their connection is slow - the host holds the clock and everyone waits, then all go on together. Players
// follow the clock and jump when they are more than about a second off.
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>

#include "diag.h"
#include "engine.h"
#include "crypto.h"
#include "platform.h"

namespace ls {

static constexpr int64_t kWatchBlock = 256 * 1024;
static constexpr int64_t kWatchAhead = 128;        // blocks asked for ahead of the player (32 MB)
static constexpr size_t kWatchInflight = 12;       // blocks on the way at once
static constexpr int64_t kWatchReady = 4 << 20;    // after loading: go on once this much is here ahead

struct WatchCache {
    std::mutex m;
    std::condition_variable cv;
    uint64_t sid = 0;
    std::string hostId;                 // the device with the movie
    std::weak_ptr<Connection> host;     // its connection (kept current by WatchTick)
    std::string path;
    FILE* f = nullptr;
    int64_t size = 0;
    std::vector<uint8_t> have;
    int64_t haveCount = 0;
    std::map<int64_t, int64_t> inflight;  // block -> asked at (steady ms)
    int64_t readPos = 0;
    int waiting = 0;
    bool cancelled = false;
    bool buffering = true;
    bool save = false, saved = false;   // keeping a copy: everything is asked for, then copied to Downloads
    bool bg = false;                    // the movie ended: only the copy is still coming
    bool all = false;                   // fetch the whole file (a song of listening together: small, and soon needed)
    int64_t lastConnect = 0;
    std::string saveName, saveDir, key;
    ~WatchCache() {
        if (f) fclose(f);
    }
    int64_t Blocks() const { return (size + kWatchBlock - 1) / kWatchBlock; }
    int64_t AheadLocked(int64_t from) const {  // bytes here without a gap from `from`
        int64_t b = from / kWatchBlock, n = 0;
        while (b < (int64_t)have.size() && have[(size_t)b]) {
            n += std::min(kWatchBlock, size - b * kWatchBlock);
            b++;
        }
        return std::max<int64_t>(0, n - (from - (from / kWatchBlock) * kWatchBlock));
    }
};

// ---------------------------------------------------------------- the same cache for listening together (listen.cpp)

std::shared_ptr<WatchCache> NewListenCache(uint64_t iid, int64_t size, const std::string& dir) {
    auto c = std::make_shared<WatchCache>();
    c->sid = iid;
    c->size = size;
    c->all = true;
    c->have.assign((size_t)c->Blocks(), 0);
    MakeDirs(dir);
    c->path = JoinPath(dir, "l" + ToHex((const uint8_t*)&iid, 8) + ".part");
    RemoveFile(c->path);
    c->f = OpenFile(c->path, "w+b");
    if (!c->f) return nullptr;
    return c;
}

void ListenCacheSet(const std::shared_ptr<WatchCache>& c, const std::string& owner, const std::shared_ptr<Connection>& conn,
                    const std::string& name, const std::string& key) {
    std::lock_guard<std::mutex> l(c->m);
    c->hostId = owner;
    c->host = conn;
    if (!name.empty()) c->saveName = name;
    if (!key.empty()) c->key = key;
}

bool ListenCacheInfo(const std::shared_ptr<WatchCache>& c, uint64_t& iid, bool& saving, bool& done, int64_t& readPos,
                     int64_t& lastConnect, double& loaded) {
    std::lock_guard<std::mutex> l(c->m);
    iid = c->sid;
    saving = c->save;
    done = c->haveCount == c->Blocks();
    readPos = c->readPos;
    lastConnect = c->lastConnect;
    loaded = c->Blocks() ? (double)c->haveCount / (double)c->Blocks() : 1.0;
    return true;
}

void ListenCacheConnectAt(const std::shared_ptr<WatchCache>& c, int64_t at) {
    std::lock_guard<std::mutex> l(c->m);
    c->lastConnect = at;
}

// a page of a comic read together that has all come: its file becomes `dest`
bool ListenCacheTake(const std::shared_ptr<WatchCache>& c, const std::string& dest) {
    std::lock_guard<std::mutex> l(c->m);
    if (!c->f || c->haveCount != c->Blocks()) return false;
    fclose(c->f);
    c->f = nullptr;
    c->cancelled = true;
    c->cv.notify_all();
    RemoveFile(dest);
    if (RenameFile(c->path, dest)) return true;
    RemoveFile(c->path);
    return false;
}

void ListenCacheDrop(const std::shared_ptr<WatchCache>& c) {
    std::lock_guard<std::mutex> l(c->m);
    c->cancelled = true;  // (the player's reads return)
    c->cv.notify_all();
    if (c->f) fclose(c->f);
    c->f = nullptr;
    RemoveFile(c->path);
}

// ---------------------------------------------------------------- starting, joining, leaving

uint64_t Engine::StartWatch(const std::string& key, const std::string& path) {
    auto lk = Lock();
    int64_t size = FileSize(path);
    DIAG("watch: start %s (%lld bytes) with %s", BaseName(path).c_str(), (long long)size, key.substr(0, 12).c_str());
    if (size <= 0) return 0;
    if (watch.sid) LeaveWatch();
    watch = WatchInfo();
    watch.sid = watchForceSid_ ? watchForceSid_ : RandomU64() | 1;
    watchForceSid_ = 0;
    watch.key = key;
    watch.name = BaseName(path);
    watch.path = path;
    watch.size = size;
    watch.at = SteadyMs();
    watch.people = {settings.displayName};
    watchViewers_.clear();
    watchBuffering_.clear();
    watch.allowSave = settings.watchAllowSave;
    watch.playing = settings.playerAutoplay;  // (autoplay: no need to press play)
    {
        std::lock_guard<std::mutex> l(watchServe_.m);
        watchServe_.files[watch.sid] = {OpenFile(path, "rb"), size};
    }
    watchServed_[watch.sid].allowSave = watch.allowSave;
    {  // subtitles lying next to it (movie.srt, movie.pl.srt...) go to the viewers too
        std::string dir = DirName(path), stem = watch.name;
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        auto lower = [](std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; };
        for (auto& e : ListDir(dir)) {
            std::string n = lower(e.name);
            bool sub = !e.isDir && e.size < (4 << 20) && n.size() > 4 &&
                       (n.compare(n.size() - 4, 4, ".srt") == 0 || n.compare(n.size() - 4, 4, ".ass") == 0 ||
                        n.compare(n.size() - 4, 4, ".ssa") == 0 || n.compare(n.size() - 4, 4, ".vtt") == 0);
            if (sub && n.rfind(lower(stem), 0) == 0) watch.subFiles.push_back(JoinPath(dir, e.name));
        }
        std::sort(watch.subFiles.begin(), watch.subFiles.end());
    }
    watchKeys.clear();
    if (key.empty()) {  // the movie player: alone until someone is added
        watch.loop = settings.playerLoop;
        return watch.sid;
    }
    watchKeys.push_back(key);
    json inv = KeyField(key, {{"sid", watch.sid}, {"name", watch.name}, {"size", size}});
    std::vector<std::string> to = DevicesOf(key);
    for (auto& d : MyOtherDevices()) to.push_back(d);  // (also my own: the TV computer, the tablet...)
    for (auto& d : to) QueueToPeer(d, Msg::WatchInvite, inv);
    AddSystemMessage(key, "You started watching \"" + watch.name + "\" together.");
    return watch.sid;
}

uint64_t Engine::StartWatchExt(const std::string& ext, const std::string& name, int64_t size) {
    auto lk = Lock();
    if (ext.empty() || size <= 0) return 0;
    if (watch.sid) LeaveWatch();
    watch = WatchInfo();
    watch.sid = RandomU64() | 1;
    watch.name = name;
    watch.size = size;
    watch.libId = "ext:" + ext;
    watch.at = SteadyMs();
    watch.playing = settings.playerAutoplay;
    watch.people = {settings.displayName};
    watchKeys.clear();
    watchViewers_.clear();
    watchBuffering_.clear();
    watchSelfBuffering_ = false;
    MarkDirty();
    return watch.sid;
}

void Engine::WatchSetExt(bool buffering, double loaded) {
    auto lk = Lock();
    if (!watch.sid || watch.libId.compare(0, 4, "ext:") != 0) return;
    watch.extLoaded = loaded;
    if (buffering != watchSelfBuffering_) {
        watchSelfBuffering_ = buffering;
        WatchUpdateHold();
    }
}

uint64_t Engine::WatchSwitch(const std::string& path) {
    auto lk = Lock();
    if (!watch.sid || !watch.host.empty() || !watch.libId.empty() || FileSize(path) <= 0) return 0;
    std::vector<std::string> keys = watchKeys;
    bool loop = watch.loop;
    uint64_t next = RandomU64() | 1;
    // those still on their way in from the previous switch hear "next" too, or they'd wait on a movie that's gone
    std::vector<std::string> coming = watchViewers_;
    for (auto& d : watchComing_)
        if (std::find(coming.begin(), coming.end(), d) == coming.end()) coming.push_back(d);
    watchViewers_ = coming;
    DIAG("watch: switching to %llx, %d viewer(s) come along", (unsigned long long)next, (int)coming.size());
    watchEndNext_ = next;
    LeaveWatch();  // (the viewers hear "next")
    watchEndNext_ = 0;
    watchForceSid_ = next;
    uint64_t sid = StartWatch(keys.empty() ? "" : keys[0], path);
    for (size_t i = 1; i < keys.size(); i++) WatchInviteTo(keys[i]);
    watch.loop = loop;
    if (sid) watchComing_ = coming;
    return sid;
}

void Engine::WatchInviteTo(const std::string& key) {
    auto lk = Lock();
    if (!watch.sid || !watch.host.empty() || !watch.libId.empty() || key.empty()) return;
    if (std::find(watchKeys.begin(), watchKeys.end(), key) != watchKeys.end()) return;
    watchKeys.push_back(key);
    if (watch.key.empty()) watch.key = key;  // (its chat shows the "watching" strip)
    json inv = KeyField(key, {{"sid", watch.sid}, {"name", watch.name}, {"size", watch.size}});
    for (auto& d : DevicesOf(key)) QueueToPeer(d, Msg::WatchInvite, inv);
    AddSystemMessage(key, "You invited them to watch \"" + watch.name + "\" together.");
}

// a file from a contact's Shared folder, played while its pieces come (a library download that isn't kept unless
// "Save" is pressed). One I have already is simply played from my Shared folder.
uint64_t Engine::StartLibraryWatch(const LibFile& f) {
    auto lk = Lock();
    if (f.id.size() != 64 || f.size <= 0) return 0;
    auto li = libIndex_.find(f.id);
    if (li != libIndex_.end())
        return StartWatch("", JoinPath(settings.sharedDir.empty() ? JoinPath(settings.downloadDir, "Shared") : settings.sharedDir, li->second.f.rel));
    bool existed = swarm.count(f.id) > 0;
    if (!DownloadShared(f)) return 0;
    auto d = swarm[f.id];
    if (!existed) d->keep = false;
    d->streamStop = false;
    d->streamPos = 0;
    if (watch.sid) LeaveWatch();
    watch = WatchInfo();
    watch.sid = RandomU64() | 1;
    watch.name = BaseName(f.rel);
    watch.size = f.size;
    watch.libId = f.id;
    watch.saving = d->keep;
    watch.at = SteadyMs();
    watch.playing = settings.playerAutoplay;
    watch.people = {settings.displayName};
    watchKeys.clear();
    watchViewers_.clear();
    watchBuffering_.clear();
    watchSelfBuffering_ = false;
    SwarmTick();
    return watch.sid;
}

void Engine::WatchSetLoop(bool on) {
    auto lk = Lock();
    if (!watch.sid) return;
    watch.loop = on;
    if (watch.host.empty()) WatchBroadcast();
    else QueueToPeer(watch.host, Msg::WatchCtl, {{"sid", watch.sid}, {"playing", watch.playing}, {"pos", watch.Target(SteadyMs())}, {"loop", on}});
}

void Engine::WatchSetSpeed(double speed) {
    auto lk = Lock();
    if (!watch.sid) return;
    int64_t now = SteadyMs();
    watch.pos = watch.Target(now);  // (the clock so far at the old speed)
    watch.at = now;
    watch.speed = std::clamp(speed, 0.25, 4.0);
    if (watch.host.empty()) WatchBroadcast();
    else
        QueueToPeer(watch.host, Msg::WatchCtl,
                    {{"sid", watch.sid}, {"playing", watch.playing}, {"pos", watch.pos}, {"loop", watch.loop}, {"speed", watch.speed}});
}

void Engine::WatchSave() {
    auto lk = Lock();
    if (!watch.sid || watch.saving) return;
    if (!watch.libId.empty()) {  // the library download is simply kept: it lands in the Shared folder
        auto it = swarm.find(watch.libId);
        if (it == swarm.end()) return;
        it->second->keep = true;
        watch.saving = true;
        MarkDirty();
        if (it->second->state == SwarmDownload::Done && FileExists(it->second->partPath)) FinishSwarm(it->second);
        return;
    }
    if (watch.host.empty() || !watch.allowSave) return;  // (it's mine already / the host doesn't let)
    auto wc = WatchCacheNow();
    if (!wc) return;
    {
        std::lock_guard<std::mutex> l(wc->m);
        wc->save = true;
        wc->saveName = watch.name;
        wc->saveDir = settings.downloadDir;
        wc->key = watch.key;
        if (!wc->saved && wc->haveCount == wc->Blocks()) {  // (all of it is here already)
            wc->saved = true;
            SaveWatchCopy(wc);
        }
    }
    watch.saving = true;
    QueueToPeer(watch.host, Msg::WatchReport, {{"sid", watch.sid}, {"buffering", watchReportedBuffering_}, {"saving", true}});
    WatchRequestOn(wc, 0);
}

void Engine::WatchSetAllowSave(bool on) {
    auto lk = Lock();
    settings.watchAllowSave = on;
    MarkDirty();
    if (!on)  // also no more copies of movies that already ended
        for (auto& [sid, sv] : watchServed_) sv.allowSave = sid == watch.sid ? sv.allowSave : false;
    if (!watch.sid || !watch.host.empty() || !watch.libId.empty()) return;
    watch.allowSave = on;
    auto& sv = watchServed_[watch.sid];
    sv.allowSave = on;
    if (!on) sv.savers.clear();
    WatchBroadcast();
}

void Engine::WatchServeClose(uint64_t sid) {
    watchServed_.erase(sid);
    std::lock_guard<std::mutex> l(watchServe_.m);
    auto it = watchServe_.files.find(sid);
    if (it == watchServe_.files.end()) return;
    if (it->second.f) fclose(it->second.f);
    watchServe_.files.erase(it);
}

bool Engine::ListenCacheSaveNow(const std::shared_ptr<WatchCache>& c, const std::string& dir) {
    std::lock_guard<std::mutex> l(c->m);
    if (c->save) return c->saved;
    c->save = true;
    c->saveDir = dir;
    if (c->haveCount == c->Blocks() && !c->saved) {
        c->saved = true;
        SaveWatchCopy(c);
    }
    return c->saved;
}

std::shared_ptr<WatchCache> Engine::WatchCacheFor(uint64_t sid) {
    std::lock_guard<std::mutex> l(watchCacheMtx_);
    if (watchCache_ && watchCache_->sid == sid) return watchCache_;
    for (auto& c : watchSaves_)
        if (c->sid == sid) return c;
    for (auto& c : listenCaches_)
        if (c->sid == sid) return c;
    for (auto& c : readCaches_)
        if (c.second->sid == sid) return c.second;
    return nullptr;
}

// every 250 ms (engine lock): copies still coming after their movie ended; ended movies nobody saves any more
void Engine::WatchSavesTick() {
    int64_t now = SteadyMs();
    for (auto it = watchServed_.begin(); it != watchServed_.end();) {
        auto cur = it++;
        if (cur->second.ended && (now - cur->second.lastUse > 3600 * 1000LL || !cur->second.allowSave)) WatchServeClose(cur->first);
    }
    std::vector<std::shared_ptr<WatchCache>> list;
    {
        std::lock_guard<std::mutex> l(watchCacheMtx_);
        watchSaves_.erase(std::remove_if(watchSaves_.begin(), watchSaves_.end(), [](const std::shared_ptr<WatchCache>& c) {
                              std::lock_guard<std::mutex> l2(c->m);
                              return c->saved;  // (copied out by now, or on its way)
                          }),
                          watchSaves_.end());
        list = watchSaves_;
    }
    for (auto& c : list) {
        auto conn = ReadyConn(c->hostId);
        bool ask = false;
        {
            std::lock_guard<std::mutex> l(c->m);
            c->host = conn;
            if (!conn && now - c->lastConnect > 30000) {
                c->lastConnect = now;
                ask = true;
            }
        }
        if (ask) ConnectPeer(c->hostId);
        if (conn) WatchRequestOn(c, 0);
    }
}

std::vector<Engine::WatchSaveInfo> Engine::WatchSaves() {
    std::vector<WatchSaveInfo> v;
    std::lock_guard<std::mutex> l(watchCacheMtx_);
    for (auto& c : watchSaves_) {
        std::lock_guard<std::mutex> l2(c->m);
        WatchSaveInfo i;
        i.sid = c->sid;
        i.name = c->saveName;
        i.key = c->key;
        i.frac = c->Blocks() ? (double)c->haveCount / (double)c->Blocks() : 1;
        i.connected = !c->host.expired();
        v.push_back(i);
    }
    return v;
}

void Engine::CancelWatchSave(uint64_t sid) {
    std::shared_ptr<WatchCache> c;
    {
        std::lock_guard<std::mutex> l(watchCacheMtx_);
        for (auto it = watchSaves_.begin(); it != watchSaves_.end(); ++it)
            if ((*it)->sid == sid) {
                c = *it;
                watchSaves_.erase(it);
                break;
            }
    }
    if (!c) return;
    std::lock_guard<std::mutex> l(c->m);
    c->cancelled = true;
    if (c->f) fclose(c->f);
    c->f = nullptr;
    RemoveFile(c->path);
}

bool Engine::JoinWatch(uint64_t sid) {
    auto lk = Lock();
    if (watch.sid == sid) return true;
    auto it = std::find_if(watchInvites.begin(), watchInvites.end(), [&](const WatchInviteInfo& w) { return w.sid == sid; });
    if (it == watchInvites.end()) return false;
    WatchInviteInfo inv = *it;
    if (!ReadyConn(inv.host)) {
        DIAG("watch: join %llx - not connected to the host %s", (unsigned long long)sid, inv.host.substr(0, 8).c_str());
        ConnectPeer(inv.host);
        return false;
    }
    DIAG("watch: joined %llx \"%s\" (%lld bytes, host %s)", (unsigned long long)sid, inv.name.c_str(), (long long)inv.size, inv.host.substr(0, 8).c_str());
    if (watch.sid) LeaveWatch();
    watchInvites.erase(it);
    watch = WatchInfo();
    watch.sid = sid;
    watch.key = inv.key;
    watch.host = inv.host;
    watch.name = inv.name;
    watch.size = inv.size;
    watch.at = SteadyMs();
    watch.hold = true;  // until the host's clock arrives
    auto c = std::make_shared<WatchCache>();
    c->sid = sid;
    c->size = inv.size;
    c->have.assign((size_t)c->Blocks(), 0);
    std::string dir = JoinPath(ConfigPath(), "watch");
    MakeDirs(dir);
    c->path = JoinPath(dir, ToHex((const uint8_t*)&sid, 8) + ".part");
    RemoveFile(c->path);
    c->f = OpenFile(c->path, "w+b");
    if (!c->f) {
        watch = WatchInfo();
        Emit({EventType::Error, "", "Watch together: could not create a temporary file"});
        return false;
    }
    c->hostId = inv.host;
    c->host = ReadyConn(inv.host);
    {
        std::lock_guard<std::mutex> l(watchCacheMtx_);
        watchCache_ = c;
    }
    watchHostSeen_ = SteadyMs();
    watchReportedBuffering_ = true;
    QueueToPeer(inv.host, Msg::WatchJoin, {{"sid", sid}});
    return true;
}

void Engine::LeaveWatch() {
    auto lk = Lock();
    if (!watch.sid) return;
    if (!watch.host.empty()) QueueToPeer(watch.host, Msg::WatchLeave, {{"sid", watch.sid}});
    EndWatchLocked("");
}

void Engine::EndWatchLocked(const std::string& why) {
    WatchInfo w = watch;
    if (!w.libId.empty()) {  // a library stream: dropped unless it was saved
        auto it = swarm.find(w.libId);
        if (it != swarm.end()) {
            auto d = it->second;
            d->streamPos = -1;
            if (!d->keep) {
                d->streamStop = true;
                d->scv.notify_all();
                d->state = SwarmDownload::Running;  // (so CancelShared deletes the part file even if it was complete)
                CancelShared(w.libId);
            }
        }
    } else if (!w.host.empty()) {
        std::shared_ptr<WatchCache> c;
        {
            std::lock_guard<std::mutex> l(watchCacheMtx_);
            c.swap(watchCache_);
        }
        bool keepSaving = false;
        if (c) {
            std::lock_guard<std::mutex> l(c->m);
            keepSaving = c->save && !c->saved && w.allowSave;
        }
        if (keepSaving) {  // the copy goes on coming (the player's reads stop: it isn't the current movie any more)
            {
                std::lock_guard<std::mutex> l(c->m);
                c->bg = true;
                c->cv.notify_all();
            }
            std::lock_guard<std::mutex> l(watchCacheMtx_);
            watchSaves_.push_back(c);
            AddSystemMessage(w.key, "Saving \"" + w.name + "\" goes on in the background.");
            c.reset();
        }
        if (c) {
            std::lock_guard<std::mutex> l(c->m);
            c->cancelled = true;  // the player's reads return; the file goes when the player lets go of it
            c->cv.notify_all();
            if (c->f) fclose(c->f);
            c->f = nullptr;
            RemoveFile(c->path);
        }
        for (auto& f : w.subFiles) RemoveFile(f);  // (the copies)
        if (!w.subFiles.empty()) RemoveFile(DirName(w.subFiles[0]));  // (their folder, where that works for folders)
    } else {
        json bye = {{"sid", w.sid}, {"end", true}};
        if (watchEndNext_) bye["next"] = watchEndNext_;  // (switching movies: they come along)
        for (auto& d : watchViewers_) QueueToPeer(d, Msg::WatchLeave, bye);
        auto sv = watchServed_.find(w.sid);
        if (sv != watchServed_.end() && sv->second.allowSave && !sv->second.savers.empty()) {
            sv->second.ended = true;  // someone is still saving it: the file stays open for them
            sv->second.lastUse = SteadyMs();
        } else {
            WatchServeClose(w.sid);
        }
    }
    watchViewers_.clear();
    watchComing_.clear();
    watchBuffering_.clear();
    watchKeys.clear();
    watchSelfBuffering_ = false;
    watch = WatchInfo();
    if (!why.empty() && !w.key.empty()) AddSystemMessage(w.key, why);
    Emit({EventType::Info, w.key, ""});
}

void Engine::SendWatchSubs(const std::shared_ptr<Connection>& c, const std::string& path) {
    std::string data;
    FILE* fp = OpenFile(path, "rb");
    if (!fp) return;
    char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), fp)) > 0 && data.size() < (4u << 20)) data.append(b, n);
    fclose(fp);
    Queue(c, Msg::WatchSubs, {{"sid", watch.sid}, {"name", BaseName(path)}, {"data", Base64Encode(data)}});
}

void Engine::WatchShareSubtitles(const std::string& path) {
    auto lk = Lock();
    if (!watch.sid || !watch.host.empty() || FileSize(path) <= 0 || FileSize(path) > (4 << 20)) return;
    if (std::find(watch.subFiles.begin(), watch.subFiles.end(), path) != watch.subFiles.end()) return;
    watch.subFiles.push_back(path);
    for (auto& d : watchViewers_)
        if (auto c = ReadyConn(d)) SendWatchSubs(c, path);
}

void Engine::WatchSetHostSub(const std::string& desc) {
    auto lk = Lock();
    if (!watch.sid || !watch.host.empty() || watch.hostSub == desc) return;
    watch.hostSub = desc;
    WatchBroadcast();
}

// ---------------------------------------------------------------- the clock

json Engine::WatchStateJson() {
    int64_t now = SteadyMs();
    json people = json::array({settings.displayName});
    for (auto& d : watchViewers_) people.push_back(PeerName(ChatKey(d)));
    return {{"sid", watch.sid}, {"playing", watch.playing}, {"pos", watch.Target(now)}, {"hold", watch.hold},
            {"wait", watch.waitFor}, {"people", people}, {"hsub", watch.hostSub}, {"loop", watch.loop}, {"save", watch.allowSave}, {"speed", watch.speed}, {"ver", ++watchVer_}};
}

void Engine::WatchBroadcast() {
    if (!watch.sid || !watch.host.empty()) return;
    json st = WatchStateJson();
    watch.people.clear();
    for (auto& p : st["people"]) watch.people.push_back(p.get<std::string>());
    for (auto& d : watchViewers_) QueueToPeer(d, Msg::WatchState, st);
    watchLastState_ = SteadyMs();
}

void Engine::WatchUpdateHold() {
    std::string wait;
    for (auto& [d, b] : watchBuffering_)
        if (b) wait += (wait.empty() ? "" : ", ") + PeerName(ChatKey(d));
    bool hold = !wait.empty() || watchSelfBuffering_;
    if (hold == watch.hold && wait == watch.waitFor) return;
    DIAG("watch: %s%s%s", hold ? "holding for " : "going on (no one loading)", wait.c_str(), watchSelfBuffering_ ? " (me)" : "");
    int64_t now = SteadyMs();
    watch.pos = watch.Target(now);  // (frozen while holding, goes on from there)
    watch.at = now;
    watch.hold = hold;
    watch.waitFor = wait;
    WatchBroadcast();
}

void Engine::WatchControl(bool playing, double pos) {
    auto lk = Lock();
    if (!watch.sid) return;
    int64_t now = SteadyMs();
    double p = pos >= 0 ? pos : watch.Target(now);
    DIAG("watch: I %s at %.1f s", playing ? "play" : "pause", p);
    if (watch.host.empty()) {
        watch.pos = p;
        watch.playing = playing;
        watch.at = now;
        WatchBroadcast();
    } else {
        watch.pos = p;  // (shown right away; the host's answer follows)
        watch.playing = playing;
        watch.at = now;
        QueueToPeer(watch.host, Msg::WatchCtl, {{"sid", watch.sid}, {"playing", playing}, {"pos", p}});
    }
}

void Engine::WatchTick() {
    WatchSavesTick();
    if (watchFollow_ && SteadyMs() - watchFollowAt_ > 15000) watchFollow_ = 0;  // (the next movie never came)
    if (!watch.sid) return;
    int64_t now = SteadyMs();
    if (watch.host.empty()) {
        if (watch.libId.compare(0, 4, "ext:") == 0) {
            // (the app's own stream: it says when it waits - WatchSetExt)
        } else if (!watch.libId.empty()) {  // a library stream: the clock waits while the pieces come
            auto it = swarm.find(watch.libId);
            bool b = it != swarm.end() && LibStreamBuffering(it->second);
            if (b != watchSelfBuffering_) {
                watchSelfBuffering_ = b;
                WatchUpdateHold();
            }
            if (it != swarm.end()) watch.saving = it->second->keep;
        }
        if (now - watchLastState_ > 1000) WatchBroadcast();
        return;
    }
    // viewer: tell the host whether I'm ready, keep loading ahead, notice when the host is gone
    bool b = WatchBuffering();
    if (b != watchReportedBuffering_ || now - watchLastReport_ > 1000) {
        watchReportedBuffering_ = b;
        watchLastReport_ = now;
        QueueToPeer(watch.host, Msg::WatchReport, {{"sid", watch.sid}, {"buffering", b}, {"saving", watch.saving}});
    }
    if (ReadyConn(watch.host)) watchHostSeen_ = now;
    else if (now - watchHostSeen_ > 20000) {
        EndWatchLocked("Watching together ended (connection lost).");
        return;
    }
    if (auto c = WatchCacheNow()) {
        int64_t from;
        {
            std::lock_guard<std::mutex> l(c->m);
            from = c->readPos;
            c->host = ReadyConn(watch.host);  // (a new connection after a network change)
        }
        WatchRequestOn(c, from);
    }
}

// ---------------------------------------------------------------- signalling

void Engine::HandleWatch(const std::shared_ptr<Connection>& c, Msg type, const json& j) {
    uint64_t sid = j.value("sid", (uint64_t)0);
    if (!sid) return;
    const std::string& dev = c->peerId;
    switch (type) {
        case Msg::WatchInvite: {
            std::string key = FrameKey(c, j);
            auto dv = devices.find(dev);
            if (key.empty() || dv == devices.end()) return;
            if (dv->second.accountPub == MyAccount()) key = j.contains("gid") ? key : ChatKey(dev);
            watchInvites.erase(std::remove_if(watchInvites.begin(), watchInvites.end(), [&](const WatchInviteInfo& w) { return w.sid == sid; }),
                               watchInvites.end());
            WatchInviteInfo w;
            w.sid = sid;
            w.key = key;
            w.host = dev;
            w.name = j.value("name", "");
            if (w.name.size() > 200) w.name = w.name.substr(0, 200);
            w.size = j.value("size", (int64_t)0);
            w.at = SteadyMs();
            DIAG("watch: invited to %llx \"%s\" by %s (chat %s)", (unsigned long long)sid, w.name.c_str(), dev.substr(0, 8).c_str(), key.substr(0, 12).c_str());
            if (w.size <= 0) return;
            watchInvites.push_back(w);
            if (watchInvites.size() > 10) watchInvites.erase(watchInvites.begin());
            if (watchFollow_ == sid) {  // (the next movie of the one I was watching: straight in)
                watchFollow_ = 0;
                JoinWatch(sid);
                break;
            }
            std::string who = PeerName(ChatKey(dev));
            std::string text = who + " invites you to watch \"" + w.name + "\" together.";
            AddSystemMessage(key, text);
            Emit({EventType::ChatReceived, key, text});
            break;
        }
        case Msg::WatchJoin:
            if (sid != watch.sid || !watch.host.empty()) return;
            watchComing_.erase(std::remove(watchComing_.begin(), watchComing_.end(), dev), watchComing_.end());
            if (std::find(watchViewers_.begin(), watchViewers_.end(), dev) == watchViewers_.end()) {
                watchViewers_.push_back(dev);
                for (auto& f : watch.subFiles) SendWatchSubs(c, f);
            }
            watchBuffering_[dev] = true;
            DIAG("watch: %s joined", dev.substr(0, 8).c_str());
            WatchUpdateHold();
            WatchBroadcast();
            break;
        case Msg::WatchLeave:
            if (j.value("nosave", false)) {  // a copy I'm still getting can't be had any more
                std::shared_ptr<WatchCache> gone;
                {
                    std::lock_guard<std::mutex> l(watchCacheMtx_);
                    for (auto it = watchSaves_.begin(); it != watchSaves_.end(); ++it)
                        if ((*it)->sid == sid && (*it)->hostId == dev) {
                            gone = *it;
                            watchSaves_.erase(it);
                            break;
                        }
                }
                if (gone) {
                    std::lock_guard<std::mutex> l(gone->m);
                    gone->cancelled = true;
                    if (gone->f) fclose(gone->f);
                    gone->f = nullptr;
                    RemoveFile(gone->path);
                    AddSystemMessage(gone->key, "Saving \"" + gone->saveName + "\" stopped: " + PeerName(ChatKey(dev)) + " no longer offers the movie.");
                    Emit({EventType::Info, gone->key, ""});
                }
                return;
            }
            if (j.value("end", false)) {
                watchInvites.erase(std::remove_if(watchInvites.begin(), watchInvites.end(), [&](const WatchInviteInfo& w) { return w.sid == sid; }),
                                   watchInvites.end());
                if (watchFollow_ == sid) {  // (the movie I was about to come along to is already gone: on to its next)
                    uint64_t next = j.value("next", (uint64_t)0);
                    watchFollow_ = next;
                    watchFollowAt_ = SteadyMs();
                    if (next && std::any_of(watchInvites.begin(), watchInvites.end(), [&](const WatchInviteInfo& w) { return w.sid == next; })) {
                        watchFollow_ = 0;
                        JoinWatch(next);
                    }
                    return;
                }
                if (sid == watch.sid && dev == watch.host) {
                    uint64_t next = j.value("next", (uint64_t)0);
                    EndWatchLocked(next ? "" : PeerName(ChatKey(dev)) + " stopped the movie.");
                    if (next) {  // the host went on to another movie: along to it
                        bool have = std::any_of(watchInvites.begin(), watchInvites.end(), [&](const WatchInviteInfo& w) { return w.sid == next; });
                        if (have) JoinWatch(next);
                        else {
                            watchFollow_ = next;
                            watchFollowAt_ = SteadyMs();
                        }
                    }
                }
                return;
            }
            if (sid != watch.sid || !watch.host.empty()) return;
            watchViewers_.erase(std::remove(watchViewers_.begin(), watchViewers_.end(), dev), watchViewers_.end());
            watchBuffering_.erase(dev);
            WatchUpdateHold();
            WatchBroadcast();
            break;
        case Msg::WatchCtl: {
            if (sid != watch.sid || !watch.host.empty()) return;
            if (std::find(watchViewers_.begin(), watchViewers_.end(), dev) == watchViewers_.end()) return;
            double pos = j.value("pos", -1.0);
            int64_t now = SteadyMs();
            watch.pos = pos >= 0 ? std::min(pos, 1e7) : watch.Target(now);
            watch.playing = j.value("playing", false);
            watch.at = now;
            if (j.contains("loop")) watch.loop = j.value("loop", false);
            if (j.contains("speed")) watch.speed = std::clamp(j.value("speed", 1.0), 0.25, 4.0);
            WatchBroadcast();
            break;
        }
        case Msg::WatchSubs: {
            if (sid != watch.sid || dev != watch.host) return;
            std::string name = BaseName(j.value("name", "")), data;
            if (name.empty() || name.size() > 200 || !Base64Decode(j.value("data", ""), data) || data.size() > (4u << 20)) return;
            std::string dir = JoinPath(JoinPath(ConfigPath(), "watch"), ToHex((const uint8_t*)&sid, 8));
            MakeDirs(dir);
            std::string p = JoinPath(dir, name);
            FILE* fp = OpenFile(p, "wb");
            if (!fp) return;
            fwrite(data.data(), 1, data.size(), fp);
            fclose(fp);
            if (std::find(watch.subFiles.begin(), watch.subFiles.end(), p) == watch.subFiles.end()) watch.subFiles.push_back(p);
            break;
        }
        case Msg::WatchState: {
            if (sid != watch.sid || dev != watch.host) return;
            if (j.value("playing", false) != watch.playing || j.value("hold", false) != watch.hold)
                DIAG("watch: host says %s at %.1f s%s%s", j.value("playing", false) ? "playing" : "paused", j.value("pos", 0.0),
                     j.value("hold", false) ? ", holding for " : "", j.value("wait", "").c_str());
            watch.playing = j.value("playing", false);
            watch.pos = j.value("pos", 0.0);
            watch.at = SteadyMs();
            watch.hold = j.value("hold", false);
            watch.waitFor = j.value("wait", "");
            watch.hostSub = j.value("hsub", "");
            watch.loop = j.value("loop", false);
            watch.speed = std::clamp(j.value("speed", 1.0), 0.25, 4.0);
            watch.allowSave = j.value("save", true);
            if (!watch.allowSave && watch.saving) {  // the host turned saving off
                if (auto wc = WatchCacheNow()) {
                    std::lock_guard<std::mutex> l(wc->m);
                    wc->save = false;
                }
                watch.saving = false;
                AddSystemMessage(watch.key, PeerName(ChatKey(dev)) + " turned off saving copies of the movie.");
            }
            watch.people.clear();
            for (auto& p : j.value("people", json::array()))
                if (p.is_string()) watch.people.push_back(p.get<std::string>());
            watchHostSeen_ = SteadyMs();
            break;
        }
        case Msg::WatchReport:
            if (sid == watch.sid && watch.host.empty() && watch.allowSave && j.value("saving", false) &&
                std::find(watchViewers_.begin(), watchViewers_.end(), dev) != watchViewers_.end())
                watchServed_[sid].savers.insert(dev);
            if (sid != watch.sid || !watch.host.empty() || !watchBuffering_.count(dev)) return;
            if (watchBuffering_[dev] != j.value("buffering", false)) DIAG("watch: %s %s", dev.substr(0, 8).c_str(), j.value("buffering", false) ? "is loading" : "is ready");
            watchBuffering_[dev] = j.value("buffering", false);
            WatchUpdateHold();
            break;
        case Msg::WatchRead: {
            // the movie that's on, for those watching it; or one that ended, for those still saving a copy
            bool viewer = sid == watch.sid && watch.host.empty() &&
                          std::find(watchViewers_.begin(), watchViewers_.end(), dev) != watchViewers_.end();
            auto sv = watchServed_.find(sid);
            bool saver = sv != watchServed_.end() && sv->second.allowSave && sv->second.savers.count(dev);
            // (a song of mine while listening together, for anyone listening)
            bool listener = listen.sid && listenOwn_.count(sid) &&
                            std::find(listen.devs.begin(), listen.devs.end(), dev) != listen.devs.end();
            // (a page of the comic I'm reading with them)
            bool reader = read.sid && read.host.empty() && readOwn_.count(sid) &&
                          std::find(readMembers_.begin(), readMembers_.end(), dev) != readMembers_.end();
            if ((listener || reader) && j.value("off", (int64_t)0) == 0)
                DIAG("serve: %s asks for %s %llx from the start", dev.substr(0, 8).c_str(), listener ? "song" : "page", (unsigned long long)sid);
            if (viewer && j.value("off", (int64_t)0) == 0) DIAG("serve: %s asks for the movie from the start", dev.substr(0, 8).c_str());
            if (!viewer && !saver && !listener && !reader) {
                if (listenOwn_.count(sid) || readOwn_.count(sid))
                    DIAG("serve: %s asked for %llx - REFUSED (not listening / reading with us)", dev.substr(0, 8).c_str(), (unsigned long long)sid);
                if (listenOwn_.count(sid) || readOwn_.count(sid)) return;
                if (!(sid == watch.sid && watch.host.empty())) Queue(c, Msg::WatchLeave, {{"sid", sid}, {"end", true}, {"nosave", true}});
                return;
            }
            if (sv != watchServed_.end()) sv->second.lastUse = SteadyMs();
            int64_t size = 0;
            {
                std::lock_guard<std::mutex> l(watchServe_.m);
                auto f = watchServe_.files.find(sid);
                if (f != watchServe_.files.end()) size = f->second.size;
            }
            Connection::WatchOut w;
            w.sid = sid;
            w.off = j.value("off", (int64_t)0);
            w.len = (int)std::clamp<int64_t>(j.value("len", (int64_t)0), 0, kWatchBlock);
            if (w.off < 0 || w.off >= size || w.len <= 0) return;
            {
                std::lock_guard<std::mutex> l(c->qm);
                if (c->watchOut.size() < 64) c->watchOut.push_back(w);
            }
            c->qcv.notify_one();
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------- the data

// writer thread (host): the next block a viewer asked for
bool Engine::ProduceWatch(const std::shared_ptr<Connection>& c, std::vector<uint8_t>& buf) {
    Connection::WatchOut w;
    {
        std::lock_guard<std::mutex> l(c->qm);
        if (c->watchOut.empty()) return false;
        w = c->watchOut.front();
        c->watchOut.pop_front();
    }
    std::lock_guard<std::mutex> l(watchServe_.m);
    auto it = watchServe_.files.find(w.sid);
    if (it == watchServe_.files.end() || (!it->second.f && it->second.path.empty())) return false;
    auto& sf = it->second;
    size_t len = (size_t)std::min<int64_t>(w.len, sf.size - w.off);
    buf.resize(16 + len);
    PutU64(buf.data(), w.sid);
    PutU64(buf.data() + 8, (uint64_t)w.off);
    FILE* f = sf.f ? sf.f : OpenFile(sf.path, "rb");  // (a page: opened for the moment)
    bool ok = f && FileSeek(f, w.off) && fread(buf.data() + 16, 1, len, f) == len;
    if (f && !sf.f) fclose(f);
    return ok;
}

// reader thread (viewer): a block arrived
void Engine::HandleWatchData(const std::shared_ptr<Connection>& c, const Frame& f) {
    if (f.payload.size() <= 16) return;
    uint64_t sid = GetU64(f.payload.data());
    int64_t off = (int64_t)GetU64(f.payload.data() + 8);
    const uint8_t* data = f.payload.data() + 16;
    int64_t len = (int64_t)f.payload.size() - 16;
    std::shared_ptr<WatchCache> wc = WatchCacheFor(sid);
    if (!wc || c->peerId != wc->hostId) return;
    bool more;
    int64_t from;
    {
        std::lock_guard<std::mutex> l(wc->m);
        if (wc->cancelled || !wc->f || off < 0 || off % kWatchBlock || off + len > wc->size) return;
        int64_t b = off / kWatchBlock;
        if (len != std::min(kWatchBlock, wc->size - off)) return;
        wc->inflight.erase(b);
        if (wc->have[(size_t)b]) return;
        if (!FileSeek(wc->f, off) || fwrite(data, 1, (size_t)len, wc->f) != (size_t)len) return;
        fflush(wc->f);
        wc->have[(size_t)b] = 1;
        wc->haveCount++;
        wc->cv.notify_all();
        more = wc->inflight.size() < kWatchInflight / 2;
        from = wc->readPos;
        if (wc->save && !wc->saved && wc->haveCount == wc->Blocks()) {
            wc->saved = true;
            SaveWatchCopy(wc);  // (a background one is removed from the list by WatchSavesTick)
        }
    }
    if (more) WatchRequestOn(wc, from);
}

std::shared_ptr<WatchCache> Engine::WatchCacheNow() {
    std::lock_guard<std::mutex> l(watchCacheMtx_);
    return watchCache_;
}

void Engine::WatchRequest(int64_t from) {
    if (auto wc = WatchCacheNow()) WatchRequestOn(wc, from);
}

// asks the host for what's missing ahead of `from` (any thread; no engine lock: Queue() only needs the connection's)
void Engine::WatchRequestOn(const std::shared_ptr<WatchCache>& wc, int64_t from) {
    std::shared_ptr<Connection> c;
    std::vector<int64_t> ask;
    uint64_t sid;
    int64_t size;
    {
        std::lock_guard<std::mutex> l(wc->m);
        c = wc->host.lock();
        if (!c || c->closed || wc->cancelled) return;
        sid = wc->sid;
        size = wc->size;
        int64_t now = SteadyMs();
        for (auto it = wc->inflight.begin(); it != wc->inflight.end();)
            it = now - it->second > 10000 ? wc->inflight.erase(it) : std::next(it);  // (lost: ask again)
        int64_t b0 = std::max<int64_t>(0, from) / kWatchBlock, end = std::min(wc->Blocks(), b0 + kWatchAhead);
        for (int64_t b = b0; b < end && wc->inflight.size() < kWatchInflight; b++)
            if (!wc->have[(size_t)b] && !wc->inflight.count(b)) {
                wc->inflight[b] = now;
                ask.push_back(b);
            }
        if (wc->save || wc->all)  // keeping a copy: after what the player needs, everything else (from where it reads, then the start)
            for (int64_t k = 0, n = wc->Blocks(); k < n && wc->inflight.size() < kWatchInflight; k++) {
                int64_t b = (end + k) % n;
                if (!wc->have[(size_t)b] && !wc->inflight.count(b)) {
                    wc->inflight[b] = now;
                    ask.push_back(b);
                }
            }
    }
    for (auto b : ask) {
        int64_t off = b * kWatchBlock;
        Queue(c, Msg::WatchRead, {{"sid", sid}, {"off", off}, {"len", std::min(kWatchBlock, size - off)}});
    }
}

int Engine::WatchRead(int64_t off, uint8_t* buf, int len, int waitMs) {
    std::shared_ptr<WatchCache> wc = WatchCacheNow();  // (the player's thread: never the engine lock)
    return WatchReadOn(wc, off, buf, len, waitMs);
}

int Engine::WatchReadOn(const std::shared_ptr<WatchCache>& wc, int64_t off, uint8_t* buf, int len, int waitMs) {
    if (!wc || len <= 0) return -1;
    int64_t t0 = SteadyMs();
    for (;;) {
        if (waitMs >= 0 && SteadyMs() - t0 > waitMs) return -2;
        {
            std::unique_lock<std::mutex> l(wc->m);
            if (wc->cancelled) return -1;
            if (off >= wc->size) return 0;
            int64_t b = off / kWatchBlock;
            if (wc->have[(size_t)b]) {
                int64_t blockEnd = std::min(wc->size, (b + 1) * kWatchBlock);
                int n = (int)std::min<int64_t>(len, blockEnd - off);
                if (!FileSeek(wc->f, off) || fread(buf, 1, (size_t)n, wc->f) != (size_t)n) return -1;
                wc->readPos = off + n;
                bool more = wc->inflight.size() < kWatchInflight / 2;
                l.unlock();
                if (more) WatchRequestOn(wc, off + n);
                return n;
            }
            wc->readPos = off;
            wc->buffering = true;
            wc->waiting++;
        }
        WatchRequestOn(wc, off);
        {
            std::unique_lock<std::mutex> l(wc->m);
            int64_t b = off / kWatchBlock;
            wc->cv.wait_for(l, std::chrono::milliseconds(250), [&] { return wc->cancelled || wc->have[(size_t)b]; });
            wc->waiting--;
        }
    }
}

bool Engine::WatchBuffering() {
    auto wc = WatchCacheNow();
    if (!wc) return false;
    std::lock_guard<std::mutex> l(wc->m);
    if (wc->waiting > 0) return wc->buffering = true;
    if (wc->buffering) {
        int64_t ahead = wc->AheadLocked(wc->readPos);
        if (ahead >= kWatchReady || wc->readPos + ahead >= wc->size) wc->buffering = false;
    }
    return wc->buffering;
}

double Engine::WatchLoaded() {
    if (watch.libId.compare(0, 4, "ext:") == 0) return watch.extLoaded;
    if (!watch.libId.empty()) {  // (UI thread, under the engine lock)
        auto it = swarm.find(watch.libId);
        return it == swarm.end() || !it->second->size ? 0.0 : (double)it->second->done / (double)it->second->size;
    }
    auto wc = WatchCacheNow();
    if (!wc) return watch.sid ? 1.0 : 0.0;
    std::lock_guard<std::mutex> l(wc->m);
    return wc->Blocks() ? (double)wc->haveCount / (double)wc->Blocks() : 1.0;
}

// the whole movie is here: copied out of the cache into Downloads (on its own thread - it can be big)
void Engine::SaveWatchCopy(const std::shared_ptr<WatchCache>& wc) {  // (wc->m is held)
    std::string src = wc->path, name = wc->saveName.empty() ? "movie" : BaseName(wc->saveName), dir = wc->saveDir, key = wc->key;
    int64_t size = wc->size;
    bool bg = wc->bg;
    if (bg && wc->f) {  // (nobody plays it any more: closed, and deleted once copied)
        fflush(wc->f);
    }
    // (never joined here: this runs under the engine lock, and the copying thread takes it at the end)
    std::weak_ptr<WatchCache> weak = wc;
    saveThreads_.emplace_back([this, src, name, dir, key, size, bg, weak] {
        MakeDirs(dir);
        std::string dest = UniquePath(JoinPath(dir, name));
        bool ok = false;
        FILE* in = OpenFile(src, "rb");
        FILE* out = in ? OpenFile(dest, "wb") : nullptr;
        if (in && out) {
            std::vector<char> b(1 << 20);
            int64_t left = size;
            ok = true;
            while (left > 0 && ok) {
                size_t n = fread(b.data(), 1, (size_t)std::min<int64_t>(left, (int64_t)b.size()), in);
                ok = n > 0 && fwrite(b.data(), 1, n, out) == n;
                left -= (int64_t)n;
            }
        }
        if (in) fclose(in);
        if (out) ok = fclose(out) == 0 && ok;
        if (!ok) RemoveFile(dest);
        if (bg) {  // the temporary copy isn't needed any more
            if (auto c = weak.lock()) {
                std::lock_guard<std::mutex> l(c->m);
                if (c->f) fclose(c->f);
                c->f = nullptr;
            }
            RemoveFile(src);
        }
        auto lk = Lock();
        if (ok) {
            HistoryEntry h;
            h.name = BaseName(dest);
            h.path = dest;
            h.size = size;
            h.ts = NowMs();
            h.outgoing = false;
            h.peer = watch.host;
            h.peerName = watch.host.empty() ? std::string() : PeerName(watch.host);
            history.push_back(h);
            MarkDirty();
            if (!key.empty()) AddSystemMessage(key, "Saved \"" + name + "\" to " + dest);
            Emit({EventType::TransferDone, key, BaseName(dest), 0});
        } else {
            Emit({EventType::Error, key, "Could not save \"" + name + "\" to " + dir});
        }
    });
}

// ---------------------------------------------------------------- library streams (the player reads pieces)

// the music player: a song from a contact's Shared folder, played while its pieces come (kept only when saved)
std::string Engine::LibStreamStart(const LibFile& f, std::string& localPath) {
    auto lk = Lock();
    localPath.clear();
    if (f.id.size() != 64 || f.size <= 0) return "";
    auto li = libIndex_.find(f.id);
    if (li != libIndex_.end()) {
        localPath = JoinPath(settings.sharedDir.empty() ? JoinPath(settings.downloadDir, "Shared") : settings.sharedDir, li->second.f.rel);
        return "";
    }
    bool existed = swarm.count(f.id) > 0;
    if (!DownloadShared(f)) return "";
    auto d = swarm[f.id];
    if (!existed) d->keep = false;
    d->streamStop = false;
    d->streamPos = 0;
    return f.id;
}

void Engine::LibStreamEnd(const std::string& id) {
    auto lk = Lock();
    if (watch.sid && watch.libId == id) return;  // (the movie player has it too)
    auto it = swarm.find(id);
    if (it == swarm.end()) return;
    auto d = it->second;
    d->streamPos = -1;
    if (!d->keep) {
        d->streamStop = true;
        d->scv.notify_all();
        d->state = SwarmDownload::Running;  // (so CancelShared deletes the part file even if it was complete)
        CancelShared(id);
    }
}

void Engine::LibStreamSave(const std::string& id) {
    auto lk = Lock();
    auto it = swarm.find(id);
    if (it == swarm.end() || it->second->keep) return;
    it->second->keep = true;
    MarkDirty();
    if (it->second->state == SwarmDownload::Done && FileExists(it->second->partPath)) FinishSwarm(it->second);
}

bool Engine::LibStreamSaved(const std::string& id) {
    auto it = swarm.find(id);
    return it == swarm.end() || it->second->keep;
}

double Engine::LibStreamLoaded(const std::string& id) {
    auto it = swarm.find(id);
    return it == swarm.end() || !it->second->size ? 1.0 : (double)it->second->done / (double)it->second->size;
}

std::function<int(int64_t, uint8_t*, int)> Engine::LibStreamReader(const std::string& id) {
    std::shared_ptr<SwarmDownload> d;
    {
        auto lk = Lock();
        auto it = swarm.find(id);
        if (it != swarm.end()) d = it->second;
    }
    return [d](int64_t off, uint8_t* buf, int len) -> int {
        if (!d || len <= 0) return -1;
        int64_t t0 = SteadyMs();
        for (;;) {
            std::string path;
            int n = 0;
            {
                std::unique_lock<std::mutex> l(d->sm);
                if (d->streamStop) return -1;
                if (off >= d->size) return 0;
                d->streamPos = off;
                uint32_t idx = (uint32_t)(off / d->pieceSize);
                if (idx < d->have.size() && d->have[idx]) {
                    path = d->state == SwarmDownload::Done && !d->finalPath.empty() ? d->finalPath : d->partPath;
                    n = (int)std::min<int64_t>(len, std::min(d->size, (int64_t)(idx + 1) * d->pieceSize) - off);
                } else {
                    if (SteadyMs() - t0 > 200) return -2;  // (the player asks again; it can stop meanwhile)
                    d->streamWaitAt = SteadyMs();
                    d->scv.wait_for(l, std::chrono::milliseconds(100));
                    continue;
                }
            }
            FILE* f = OpenFile(path, "rb");  // (opened per read: the file may move into the Shared folder meanwhile)
            bool ok = f && FileSeek(f, off) && fread(buf, 1, (size_t)n, f) == (size_t)n;
            if (f) fclose(f);
            if (ok) return n;
            if (SteadyMs() - t0 > 2000) return -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // (renamed just now: once more)
        }
    };
}

bool Engine::LibStreamBuffering(const std::shared_ptr<SwarmDownload>& d) {
    // the player waited for a piece just now (it asks again every 200 ms while it waits)
    return SteadyMs() - d->streamWaitAt.load() < 500;
}

}  // namespace ls
