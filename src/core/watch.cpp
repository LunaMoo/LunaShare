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
#include <condition_variable>
#include <cstring>

#include "engine.h"
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

// ---------------------------------------------------------------- starting, joining, leaving

uint64_t Engine::StartWatch(const std::string& key, const std::string& path) {
    auto lk = Lock();
    int64_t size = FileSize(path);
    if (size <= 0) return 0;
    if (watch.sid) LeaveWatch();
    watch = WatchInfo();
    watch.sid = RandomU64() | 1;
    watch.key = key;
    watch.name = BaseName(path);
    watch.path = path;
    watch.size = size;
    watch.at = SteadyMs();
    watch.people = {settings.displayName};
    watchViewers_.clear();
    watchBuffering_.clear();
    {
        std::lock_guard<std::mutex> l(watchServe_.m);
        if (watchServe_.f) fclose(watchServe_.f);
        watchServe_.f = OpenFile(path, "rb");
        watchServe_.sid = watch.sid;
        watchServe_.size = size;
    }
    json inv = KeyField(key, {{"sid", watch.sid}, {"name", watch.name}, {"size", size}});
    std::vector<std::string> to = DevicesOf(key);
    for (auto& d : MyOtherDevices()) to.push_back(d);  // (also my own: the TV computer, the tablet...)
    for (auto& d : to) QueueToPeer(d, Msg::WatchInvite, inv);
    AddSystemMessage(key, "You started watching \"" + watch.name + "\" together.");
    return watch.sid;
}

bool Engine::JoinWatch(uint64_t sid) {
    auto lk = Lock();
    if (watch.sid == sid) return true;
    auto it = std::find_if(watchInvites.begin(), watchInvites.end(), [&](const WatchInviteInfo& w) { return w.sid == sid; });
    if (it == watchInvites.end()) return false;
    WatchInviteInfo inv = *it;
    if (!ReadyConn(inv.host)) {
        ConnectPeer(inv.host);
        return false;
    }
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
    watchCache_ = c;
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
    if (!w.host.empty()) {
        std::shared_ptr<WatchCache> c = watchCache_;
        watchCache_.reset();
        if (c) {
            std::lock_guard<std::mutex> l(c->m);
            c->cancelled = true;  // the player's reads return; the file goes when the player lets go of it
            c->cv.notify_all();
            if (c->f) fclose(c->f);
            c->f = nullptr;
            RemoveFile(c->path);
        }
    } else {
        for (auto& d : watchViewers_) QueueToPeer(d, Msg::WatchLeave, {{"sid", w.sid}, {"end", true}});
        std::lock_guard<std::mutex> l(watchServe_.m);
        if (watchServe_.f) fclose(watchServe_.f);
        watchServe_.f = nullptr;
        watchServe_.sid = 0;
    }
    watchViewers_.clear();
    watchBuffering_.clear();
    watch = WatchInfo();
    if (!why.empty()) AddSystemMessage(w.key, why);
    Emit({EventType::Info, w.key, ""});
}

// ---------------------------------------------------------------- the clock

json Engine::WatchStateJson() {
    int64_t now = SteadyMs();
    json people = json::array({settings.displayName});
    for (auto& d : watchViewers_) people.push_back(PeerName(ChatKey(d)));
    return {{"sid", watch.sid}, {"playing", watch.playing}, {"pos", watch.Target(now)}, {"hold", watch.hold},
            {"wait", watch.waitFor}, {"people", people}, {"ver", ++watchVer_}};
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
    bool hold = !wait.empty();
    if (hold == watch.hold && wait == watch.waitFor) return;
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
    if (!watch.sid) return;
    int64_t now = SteadyMs();
    if (watch.host.empty()) {
        if (now - watchLastState_ > 1000) WatchBroadcast();
        return;
    }
    // viewer: tell the host whether I'm ready, keep loading ahead, notice when the host is gone
    bool b = WatchBuffering();
    if (b != watchReportedBuffering_ || now - watchLastReport_ > 1000) {
        watchReportedBuffering_ = b;
        watchLastReport_ = now;
        QueueToPeer(watch.host, Msg::WatchReport, {{"sid", watch.sid}, {"buffering", b}});
    }
    if (ReadyConn(watch.host)) watchHostSeen_ = now;
    else if (now - watchHostSeen_ > 20000) {
        EndWatchLocked("Watching together ended (connection lost).");
        return;
    }
    if (auto c = watchCache_) {
        int64_t from;
        {
            std::lock_guard<std::mutex> l(c->m);
            from = c->readPos;
        }
        WatchRequest(from);
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
            if (w.size <= 0) return;
            watchInvites.push_back(w);
            if (watchInvites.size() > 10) watchInvites.erase(watchInvites.begin());
            std::string who = PeerName(ChatKey(dev));
            std::string text = who + " invites you to watch \"" + w.name + "\" together.";
            AddSystemMessage(key, text);
            Emit({EventType::ChatReceived, key, text});
            break;
        }
        case Msg::WatchJoin:
            if (sid != watch.sid || !watch.host.empty()) return;
            if (std::find(watchViewers_.begin(), watchViewers_.end(), dev) == watchViewers_.end()) watchViewers_.push_back(dev);
            watchBuffering_[dev] = true;
            WatchUpdateHold();
            WatchBroadcast();
            break;
        case Msg::WatchLeave:
            if (j.value("end", false)) {
                watchInvites.erase(std::remove_if(watchInvites.begin(), watchInvites.end(), [&](const WatchInviteInfo& w) { return w.sid == sid; }),
                                   watchInvites.end());
                if (sid == watch.sid && dev == watch.host) EndWatchLocked(PeerName(ChatKey(dev)) + " stopped the movie.");
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
            WatchBroadcast();
            break;
        }
        case Msg::WatchState: {
            if (sid != watch.sid || dev != watch.host) return;
            watch.playing = j.value("playing", false);
            watch.pos = j.value("pos", 0.0);
            watch.at = SteadyMs();
            watch.hold = j.value("hold", false);
            watch.waitFor = j.value("wait", "");
            watch.people.clear();
            for (auto& p : j.value("people", json::array()))
                if (p.is_string()) watch.people.push_back(p.get<std::string>());
            watchHostSeen_ = SteadyMs();
            break;
        }
        case Msg::WatchReport:
            if (sid != watch.sid || !watch.host.empty() || !watchBuffering_.count(dev)) return;
            watchBuffering_[dev] = j.value("buffering", false);
            WatchUpdateHold();
            break;
        case Msg::WatchRead: {
            if (sid != watch.sid || !watch.host.empty()) return;
            if (std::find(watchViewers_.begin(), watchViewers_.end(), dev) == watchViewers_.end()) return;
            Connection::WatchOut w;
            w.sid = sid;
            w.off = j.value("off", (int64_t)0);
            w.len = (int)std::clamp<int64_t>(j.value("len", (int64_t)0), 0, kWatchBlock);
            if (w.off < 0 || w.off >= watch.size || w.len <= 0) return;
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
    if (w.sid != watchServe_.sid || !watchServe_.f) return false;
    size_t len = (size_t)std::min<int64_t>(w.len, watchServe_.size - w.off);
    buf.resize(16 + len);
    PutU64(buf.data(), w.sid);
    PutU64(buf.data() + 8, (uint64_t)w.off);
    if (!FileSeek(watchServe_.f, w.off) || fread(buf.data() + 16, 1, len, watchServe_.f) != len) return false;
    return true;
}

// reader thread (viewer): a block arrived
void Engine::HandleWatchData(const std::shared_ptr<Connection>& c, const Frame& f) {
    if (f.payload.size() <= 16) return;
    uint64_t sid = GetU64(f.payload.data());
    int64_t off = (int64_t)GetU64(f.payload.data() + 8);
    const uint8_t* data = f.payload.data() + 16;
    int64_t len = (int64_t)f.payload.size() - 16;
    std::shared_ptr<WatchCache> wc;
    {
        auto lk = Lock();
        if (sid != watch.sid || c->peerId != watch.host) return;
        wc = watchCache_;
    }
    if (!wc) return;
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
    }
    if (more) WatchRequest(from);
}

void Engine::WatchRequest(int64_t from) {
    auto lk = Lock();
    auto wc = watchCache_;
    if (!wc || !watch.sid || watch.host.empty()) return;
    auto c = ReadyConn(watch.host);
    if (!c) return;
    std::vector<int64_t> ask;
    {
        std::lock_guard<std::mutex> l(wc->m);
        int64_t now = SteadyMs();
        for (auto it = wc->inflight.begin(); it != wc->inflight.end();)
            it = now - it->second > 10000 ? wc->inflight.erase(it) : std::next(it);  // (lost: ask again)
        int64_t b0 = std::max<int64_t>(0, from) / kWatchBlock, end = std::min(wc->Blocks(), b0 + kWatchAhead);
        for (int64_t b = b0; b < end && wc->inflight.size() < kWatchInflight; b++)
            if (!wc->have[(size_t)b] && !wc->inflight.count(b)) {
                wc->inflight[b] = now;
                ask.push_back(b);
            }
    }
    for (auto b : ask) {
        int64_t off = b * kWatchBlock;
        Queue(c, Msg::WatchRead, {{"sid", watch.sid}, {"off", off}, {"len", std::min(kWatchBlock, watch.size - off)}});
    }
}

int Engine::WatchRead(int64_t off, uint8_t* buf, int len) {
    std::shared_ptr<WatchCache> wc;
    {
        auto lk = Lock();
        wc = watchCache_;
    }
    if (!wc || len <= 0) return -1;
    for (;;) {
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
                if (more) WatchRequest(off + n);
                return n;
            }
            wc->readPos = off;
            wc->buffering = true;
            wc->waiting++;
        }
        WatchRequest(off);
        {
            std::unique_lock<std::mutex> l(wc->m);
            int64_t b = off / kWatchBlock;
            wc->cv.wait_for(l, std::chrono::milliseconds(250), [&] { return wc->cancelled || wc->have[(size_t)b]; });
            wc->waiting--;
        }
    }
}

bool Engine::WatchBuffering() {
    auto lk = Lock();
    auto wc = watchCache_;
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
    auto lk = Lock();
    auto wc = watchCache_;
    if (!wc) return watch.sid ? 1.0 : 0.0;
    std::lock_guard<std::mutex> l(wc->m);
    return wc->Blocks() ? (double)wc->haveCount / (double)wc->Blocks() : 1.0;
}

}  // namespace ls
