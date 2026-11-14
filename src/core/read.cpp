// LunaShare - reading a comic together: one device (the host) has the comic open; the others get its pages as they
// need them - the one on the screen first, then the next ones, then the rest - in the same 256 KB blocks as watch
// together (WatchRead / WatchData, with the page's id as "sid"), each page a picture in a folder of the cache that
// is emptied when the reading ends. Anyone turns the page; the host tells everyone, so all are on the same one.
#include <algorithm>
#include <cctype>
#include <filesystem>

#include "crypto.h"
#include "diag.h"
#include "engine.h"
#include "platform.h"

namespace ls {

struct WatchCache;  // watch.cpp: the same cache as the songs of listening together
std::shared_ptr<WatchCache> NewListenCache(uint64_t iid, int64_t size, const std::string& dir);
void ListenCacheSet(const std::shared_ptr<WatchCache>& c, const std::string& owner, const std::shared_ptr<Connection>& conn,
                    const std::string& name, const std::string& key);
bool ListenCacheInfo(const std::shared_ptr<WatchCache>& c, uint64_t& iid, bool& saving, bool& done, int64_t& readPos,
                     int64_t& lastConnect, double& loaded);
void ListenCacheConnectAt(const std::shared_ptr<WatchCache>& c, int64_t at);
void ListenCacheDrop(const std::shared_ptr<WatchCache>& c);
bool ListenCacheTake(const std::shared_ptr<WatchCache>& c, const std::string& dest);

static constexpr int kReadMaxPages = 5000;
static constexpr int kReadAtOnce = 3;  // pages coming at the same time

static std::string Cut(std::string s, size_t n) {
    if (s.size() > n) {
        s.resize(n);
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
        if (!s.empty() && ((unsigned char)s.back() & 0x80)) s.pop_back();
    }
    return s;
}

// a page's file name ending, kept short and harmless (".jpg", ".png", ".webp"...)
static std::string SafeExt(const std::string& path) {
    std::string b = BaseName(path);
    size_t dot = b.find_last_of('.');
    std::string e = dot == std::string::npos ? "" : b.substr(dot);
    if (e.size() > 6) return ".img";
    for (size_t i = 1; i < e.size(); i++)
        if (!isalnum((unsigned char)e[i])) return ".img";
    return e.size() > 1 ? e : ".img";
}

static void RemoveTree(const std::string& dir) {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(dir), ec);
}

static std::string ReadDir(const std::string& cfg, uint64_t sid) {
    return JoinPath(JoinPath(JoinPath(cfg, "cache"), "read"), ToHex((const uint8_t*)&sid, 8));
}

// ---------------------------------------------------------------- starting, joining, leaving

uint64_t Engine::StartRead(const std::string& key, const std::string& name, const std::vector<std::string>& pages, int page) {
    auto lk = Lock();
    if (key.empty() || pages.empty() || (int)pages.size() > kReadMaxPages) return 0;
    if (read.sid) LeaveRead();
    read = ReadInfo();
    read.sid = RandomU64() | 1;
    read.key = key;
    read.name = Cut(name, 255);
    read.pages = (int)pages.size();
    read.page = std::clamp(page, 0, read.pages - 1);
    read.files = pages;
    readMembers_.clear();
    readNames_.clear();
    readOwn_.clear();
    readPids_.clear();
    readSizes_.clear();
    readExts_.clear();
    {
        std::lock_guard<std::mutex> l(watchServe_.m);
        for (int i = 0; i < read.pages; i++) {
            uint64_t pid = RandomU64() | 1;
            int64_t size = FileSize(pages[(size_t)i]);
            WatchServe::File f;
            f.size = size > 0 ? size : 0;
            f.path = pages[(size_t)i];
            watchServe_.files[pid] = f;
            readOwn_[pid] = i;
            readPids_.push_back(pid);
            readSizes_.push_back(f.size);
            readExts_.push_back(SafeExt(pages[(size_t)i]));
        }
    }
    ReadPeople();
    std::vector<std::string> to = DevicesOf(key);
    for (auto& d : MyOtherDevices()) to.push_back(d);
    json inv = KeyField(key, {{"sid", read.sid}, {"name", read.name}, {"pages", read.pages}});
    for (auto& d : to) QueueToPeer(d, Msg::ReadInvite, inv);
    readInvited_ = to;
    DIAG("read: started %llx \"%s\" (%d pages) with %s - invited %d device(s)", (unsigned long long)read.sid, read.name.c_str(), read.pages,
         key.substr(0, 12).c_str(), (int)to.size());
    AddSystemMessage(key, "You started reading \"" + read.name + "\" together.");
    Emit({EventType::Info, key, ""});
    return read.sid;
}

bool Engine::JoinRead(uint64_t sid) {
    auto lk = Lock();
    if (read.sid == sid) return true;
    auto it = std::find_if(readInvites.begin(), readInvites.end(), [&](const ReadInviteInfo& i) { return i.sid == sid; });
    if (it == readInvites.end()) return false;
    ReadInviteInfo inv = *it;
    if (!ReadyConn(inv.host)) {
        DIAG("read: join %llx - not connected to the host %s", (unsigned long long)sid, inv.host.substr(0, 8).c_str());
        ConnectPeer(inv.host);
        return false;
    }
    DIAG("read: joined %llx \"%s\" (%d pages, host %s)", (unsigned long long)sid, inv.name.c_str(), inv.pages, inv.host.substr(0, 8).c_str());
    if (read.sid) LeaveRead();
    readInvites.erase(it);
    read = ReadInfo();
    read.sid = sid;
    read.key = inv.key;
    read.host = inv.host;
    read.name = inv.name;
    readPids_.clear();
    readSizes_.clear();
    readExts_.clear();
    readHostSeen_ = SteadyMs();
    readTurnAt_ = 0;
    {  // (pages of an earlier one that didn't end tidily)
        std::string all = JoinPath(JoinPath(ConfigPath(), "cache"), "read");
        RemoveTree(all);
        MakeDirs(ReadDir(ConfigPath(), sid));
    }
    QueueToPeer(inv.host, Msg::ReadJoin, {{"sid", sid}, {"name", settings.displayName}});
    Emit({EventType::Info, inv.key, ""});
    return true;
}

void Engine::LeaveRead() {
    auto lk = Lock();
    if (!read.sid) return;
    if (!read.host.empty()) QueueToPeer(read.host, Msg::ReadLeave, {{"sid", read.sid}});
    EndReadLocked(read.host.empty() ? "You stopped reading together." : "");
}

void Engine::EndReadLocked(const std::string& why) {
    ReadInfo r = read;
    if (r.host.empty()) {
        std::vector<std::string> tell = readInvited_;  // (those who haven't joined forget the invite)
        for (auto& d : readMembers_)
            if (std::find(tell.begin(), tell.end(), d) == tell.end()) tell.push_back(d);
        for (auto& d : tell) QueueToPeer(d, Msg::ReadLeave, {{"sid", r.sid}, {"end", true}});
        for (auto& o : readOwn_) WatchServeClose(o.first);
    }
    std::vector<std::pair<int, std::shared_ptr<WatchCache>>> caches;
    {
        std::lock_guard<std::mutex> g(watchCacheMtx_);
        caches.swap(readCaches_);
    }
    for (auto& c : caches) ListenCacheDrop(c.second);
    if (!r.host.empty() && r.sid) RemoveTree(ReadDir(ConfigPath(), r.sid));  // (the pages that came here)
    readOwn_.clear();
    readPids_.clear();
    readSizes_.clear();
    readExts_.clear();
    readMembers_.clear();
    readInvited_.clear();
    readNames_.clear();
    read = ReadInfo();
    if (!why.empty() && !r.key.empty()) AddSystemMessage(r.key, why);
    Emit({EventType::Info, r.key, ""});
}

// ---------------------------------------------------------------- turning pages

void Engine::ReadTurn(int page) {
    auto lk = Lock();
    if (!read.sid || read.pages <= 0) return;
    page = std::clamp(page, 0, read.pages - 1);
    if (page == read.page) return;
    DIAG("read: I turn to page %d", page + 1);
    read.page = page;
    if (!read.host.empty()) {  // (here at once; the host tells everyone)
        readTurnAt_ = SteadyMs();
        readTurnSeq_ = read.seq;
        QueueToPeer(read.host, Msg::ReadTurn, {{"sid", read.sid}, {"page", page}});
        Emit({EventType::Info, read.key, ""});
        return;
    }
    read.seq++;
    ReadBroadcast();
}

void Engine::ReadPeople() {
    read.people = {settings.displayName};
    read.devs = {settings.deviceId};
    for (auto& d : readMembers_) {
        read.people.push_back(readNames_[d]);
        read.devs.push_back(d);
    }
}

void Engine::ReadBroadcast() {
    if (!read.sid || !read.host.empty()) return;
    readLastBroadcast_ = SteadyMs();
    json st = {{"sid", read.sid}, {"name", read.name}, {"page", read.page}, {"seq", read.seq}, {"pids", readPids_},
               {"sizes", readSizes_}, {"exts", readExts_}, {"people", read.people}, {"devs", read.devs}};
    for (auto& d : readMembers_) QueueToPeer(d, Msg::ReadState, st);
    Emit({EventType::Info, read.key, ""});
}

void Engine::ReadPeerGone(const std::string& dev) {
    if (!read.sid || !read.host.empty()) return;
    if (std::find(readMembers_.begin(), readMembers_.end(), dev) == readMembers_.end()) return;
    readMembers_.erase(std::remove(readMembers_.begin(), readMembers_.end(), dev), readMembers_.end());
    ReadPeople();
    ReadBroadcast();
}

// ---------------------------------------------------------------- the pages coming here

void Engine::ReadCaches() {
    if (!read.sid || read.host.empty() || readPids_.empty()) return;
    int n = (int)readPids_.size();
    // the order they're wanted in: this page, the next two, the one before, then onwards, then back to the start
    std::vector<int> order;
    for (int i : {read.page, read.page + 1, read.page + 2, read.page - 1})
        if (i >= 0 && i < n) order.push_back(i);
    for (int i = read.page + 3; i < n; i++) order.push_back(i);
    for (int i = read.page - 2; i >= 0; i--) order.push_back(i);
    std::vector<int> want;
    for (int i : order)
        if (read.files[(size_t)i].empty() && readSizes_[(size_t)i] > 0 && (int)want.size() < kReadAtOnce) want.push_back(i);
    std::vector<std::pair<int, std::shared_ptr<WatchCache>>> all, keep, drop;
    {
        std::lock_guard<std::mutex> g(watchCacheMtx_);
        all = readCaches_;
    }
    std::string dir = ReadDir(ConfigPath(), read.sid);
    for (auto& c : all) {
        uint64_t iid = 0;
        bool saving = false, done = false;
        int64_t rp = 0, lc = 0;
        double ld = 0;
        ListenCacheInfo(c.second, iid, saving, done, rp, lc, ld);
        int pg = c.first;
        if (done) {  // a page has come: it's a picture in the folder now
            char nm[32];
            snprintf(nm, sizeof(nm), "p%05d", pg);
            std::string dest = JoinPath(dir, nm + readExts_[(size_t)pg]);
            if (ListenCacheTake(c.second, dest)) {
                DIAG("read: page %d came", pg + 1);
                read.files[(size_t)pg] = dest;
                read.got++;
                Emit({EventType::Info, read.key, ""});
            } else {
                DIAG("read: page %d came but COULDN'T BE SAVED as %s", pg + 1, dest.c_str());
                drop.push_back(c);
            }
            continue;
        }
        if (std::find(want.begin(), want.end(), pg) != want.end()) keep.push_back(c);
        else drop.push_back(c);  // (turned elsewhere: other pages first)
    }
    auto host = ReadyConn(read.host);
    for (int pg : want) {
        if (std::any_of(keep.begin(), keep.end(), [&](auto& c) { return c.first == pg; })) continue;
        auto c = NewListenCache(readPids_[(size_t)pg], readSizes_[(size_t)pg], dir);
        DIAG("read: fetching page %d (%lld bytes)%s%s", pg + 1, (long long)readSizes_[(size_t)pg], host ? "" : " - not connected to the host",
             c ? "" : " - CAN'T MAKE ITS CACHE FILE");
        if (!c) continue;
        ListenCacheSet(c, read.host, host, "", read.key);
        keep.push_back({pg, c});
    }
    {
        std::lock_guard<std::mutex> g(watchCacheMtx_);
        readCaches_ = keep;
    }
    for (auto& c : drop) ListenCacheDrop(c.second);
    int64_t now = SteadyMs();
    for (auto& c : keep) {
        uint64_t iid = 0;
        bool saving = false, done = false;
        int64_t rp = 0, lc = 0;
        double ld = 0;
        ListenCacheInfo(c.second, iid, saving, done, rp, lc, ld);
        if (done) continue;
        ListenCacheSet(c.second, read.host, host, "", "");
        if (!host) {
            if (now - lc > 15000) {
                ListenCacheConnectAt(c.second, now);
                ConnectPeer(read.host);
            }
            continue;
        }
        WatchRequestOn(c.second, rp);
    }
}

// ---------------------------------------------------------------- the tick and the messages

void Engine::ReadTick() {
    int64_t now = SteadyMs();
    readInvites.erase(std::remove_if(readInvites.begin(), readInvites.end(),
                                     [&](const ReadInviteInfo& i) { return now - i.at > 6 * 3600 * 1000LL; }),
                      readInvites.end());
    if (!read.sid) return;
    if (read.host.empty()) {
        if (now - readLastBroadcast_ > 3000) ReadBroadcast();
        return;
    }
    if (ReadyConn(read.host)) readHostSeen_ = now;
    else if (now - readHostSeen_ > 30000) {
        DIAG("read: the host is gone");
        EndReadLocked("Reading together ended: the host is gone.");
        return;
    } else if (now - readHostSeen_ > 3000 && (now - readHostSeen_) % 10000 < 300) {
        ConnectPeer(read.host);
    }
    ReadCaches();
}

void Engine::HandleRead(const std::shared_ptr<Connection>& c, Msg type, const json& j) {
    uint64_t sid = j.value("sid", (uint64_t)0);
    if (!sid) return;
    const std::string& dev = c->peerId;
    bool host = read.sid == sid && read.host.empty();
    bool member = host && std::find(readMembers_.begin(), readMembers_.end(), dev) != readMembers_.end();
    switch (type) {
        case Msg::ReadInvite: {
            std::string key = FrameKey(c, j);
            auto dv = devices.find(dev);
            if (key.empty() || dv == devices.end() || read.sid == sid) return;
            if (dv->second.accountPub == MyAccount()) key = j.contains("gid") ? key : ChatKey(dev);
            int pages = j.value("pages", 0);
            if (pages <= 0 || pages > kReadMaxPages) return;
            std::string name = BaseName(Cut(j.value("name", ""), 255));
            readInvites.erase(std::remove_if(readInvites.begin(), readInvites.end(), [&](const ReadInviteInfo& i) { return i.sid == sid; }),
                              readInvites.end());
            readInvites.push_back({sid, key, dev, name, pages, SteadyMs()});
            DIAG("read: invited to %llx \"%s\" (%d pages) by %s (chat %s)", (unsigned long long)sid, name.c_str(), pages, dev.substr(0, 8).c_str(), key.substr(0, 12).c_str());
            if (readInvites.size() > 10) readInvites.erase(readInvites.begin());
            std::string text = PeerName(ChatKey(dev)) + " invites you to read \"" + name + "\" together.";
            AddSystemMessage(key, text);
            Emit({EventType::ChatReceived, key, text});
            break;
        }
        case Msg::ReadJoin: {
            if (!host) return;
            auto to = DevicesOf(read.key);
            auto mine = MyOtherDevices();
            if (std::find(to.begin(), to.end(), dev) == to.end() && std::find(mine.begin(), mine.end(), dev) == mine.end()) return;
            if (!member) readMembers_.push_back(dev);
            DIAG("read: %s joined", dev.substr(0, 8).c_str());
            std::string name = Cut(j.value("name", ""), 100);
            readNames_[dev] = name.empty() ? PeerName(dev) : name;
            ReadPeople();
            ReadBroadcast();
            break;
        }
        case Msg::ReadLeave:
            if (read.sid != sid) {  // (an invite that ended before I joined)
                if (j.value("end", false))
                    readInvites.erase(std::remove_if(readInvites.begin(), readInvites.end(), [&](const ReadInviteInfo& i) { return i.sid == sid; }),
                                      readInvites.end());
                return;
            }
            if (!host) {
                if (dev == read.host && j.value("end", false)) EndReadLocked("Reading together ended.");
                return;
            }
            if (member) ReadPeerGone(dev);
            break;
        case Msg::ReadTurn: {
            if (!member || read.pages <= 0) return;
            int p = std::clamp(j.value("page", 0), 0, read.pages - 1);
            if (p == read.page) return;
            DIAG("read: %s turned to page %d", dev.substr(0, 8).c_str(), p + 1);
            read.page = p;
            read.seq++;
            ReadBroadcast();
            break;
        }
        case Msg::ReadState: {
            if (read.sid != sid || dev != read.host) return;
            readHostSeen_ = SteadyMs();
            if (readPids_.empty()) {  // the first one: what the comic is
                std::vector<uint64_t> pids;
                std::vector<int64_t> sizes;
                std::vector<std::string> exts;
                for (auto& x : j.value("pids", json::array()))
                    if (x.is_number_unsigned() && pids.size() < (size_t)kReadMaxPages) pids.push_back(x.get<uint64_t>());
                for (auto& x : j.value("sizes", json::array()))
                    if (x.is_number_integer() && sizes.size() < pids.size()) sizes.push_back(std::clamp<int64_t>(x.get<int64_t>(), 0, (int64_t)1 << 30));
                for (auto& x : j.value("exts", json::array()))
                    if (x.is_string() && exts.size() < pids.size()) exts.push_back(SafeExt("p" + x.get<std::string>()));
                DIAG("read: the host's comic: %d pages", (int)pids.size());
                if (pids.empty() || sizes.size() != pids.size()) {
                    DIAG("read: ...UNUSABLE (pages %d, sizes %d)", (int)pids.size(), (int)sizes.size());
                    return;
                }
                exts.resize(pids.size(), ".img");
                readPids_ = pids;
                readSizes_ = sizes;
                readExts_ = exts;
                read.pages = (int)pids.size();
                read.files.assign(pids.size(), "");
                std::string name = BaseName(Cut(j.value("name", ""), 255));
                if (!name.empty()) read.name = name;
            }
            int p = std::clamp(j.value("page", 0), 0, read.pages - 1);
            int64_t seq = j.value("seq", (int64_t)0);
            // (a turn of mine on its way to the host: an older word of the host's doesn't turn the page back)
            bool mineOnWay = readTurnAt_ && SteadyMs() - readTurnAt_ < 3000 && seq <= readTurnSeq_;
            if (!mineOnWay) {
                read.page = p;
                readTurnAt_ = 0;
            }
            read.seq = seq;
            read.people.clear();
            for (auto& x : j.value("people", json::array()))
                if (x.is_string() && read.people.size() < 64) read.people.push_back(Cut(x.get<std::string>(), 100));
            read.devs.clear();
            for (auto& x : j.value("devs", json::array()))
                if (x.is_string() && read.devs.size() < 64) read.devs.push_back(Cut(x.get<std::string>(), 128));
            ReadCaches();
            Emit({EventType::Info, read.key, ""});
            break;
        }
        default:
            break;
    }
}

}  // namespace ls
