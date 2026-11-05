// Shared library: every device offers the files in its Shared folder to its contacts. A file is identified by its
// content (SHA-256 over its 1 MB piece hashes + size), so the same file has the same id on every device. Downloads
// ask all contacts who has a file and fetch different pieces from each of them at the same time; every piece is
// checked against its hash when it arrives, and sources that send bad pieces are dropped. Finished (and even
// partly downloaded) files are offered on to others, like a torrent within your contacts.
#include <algorithm>
#include <cstring>

#include "engine.h"
#include "hash.h"
#include "platform.h"

namespace ls {

static std::string Raw(const std::string& hex) {
    std::string r(hex.size() / 2, '\0');
    if (!FromHex(hex, (uint8_t*)&r[0], r.size())) return {};
    return r;
}

static std::string ContentId(const std::string& hashes, int64_t size) {
    Sha256 h;
    h.Update(hashes.data(), hashes.size());
    uint8_t sz[8];
    PutU64(sz, (uint64_t)size);
    h.Update(sz, 8);
    return h.FinalHex();
}

static std::string Bits(const std::vector<uint8_t>& have) {
    std::string b((have.size() + 7) / 8, '\0');
    for (size_t i = 0; i < have.size(); i++)
        if (have[i]) b[i / 8] |= (char)(1 << (i % 8));
    return Base64Encode(b);
}

static std::vector<uint8_t> Unbits(const std::string& b64, size_t n) {
    std::string b;
    std::vector<uint8_t> v(n, 0);
    if (!Base64Decode(b64, b)) return v;
    for (size_t i = 0; i < n && i / 8 < b.size(); i++) v[i] = (b[i / 8] >> (i % 8)) & 1;
    return v;
}

std::string Engine::ConfigPath() { return configDir.empty() ? ConfigDir() : configDir; }

std::string Engine::SharedDir() {
    auto lk = Lock();
    return settings.sharedDir.empty() ? JoinPath(settings.downloadDir, "Shared") : settings.sharedDir;
}

// Who may see my Shared folder: my own devices, devices I trust, and people I've exchanged messages or files with.
// (A device that merely showed up on the network doesn't count.)
bool Engine::IsContact(const std::string& device) {
    auto it = devices.find(device);
    if (it == devices.end() || it->second.blocked || it->second.pubKey.empty()) return false;
    if (it->second.accountPub == MyAccount() || it->second.trusted) return true;
    auto cv = chats.find(ChatKey(device));
    if (cv != chats.end())
        for (auto& m : cv->second.messages)
            if (m.kind != MsgKind::System) return true;
    for (auto& h : history)
        if (h.peer == device) return true;
    return false;
}

bool Engine::ShareAllowedNow() {
    if (!settings.shareLibrary) return false;
    if (!settings.shareOnMobileData && Bridge().isMetered && Bridge().isMetered()) return false;
    return true;
}

bool Engine::HaveShared(const std::string& id) {
    auto lk = Lock();
    return libIndex_.count(id) > 0;
}

void Engine::RescanShared() { libRescan_ = true; }

void Engine::RefreshLibrary(const std::string& peerOrKey) {
    auto lk = Lock();
    for (auto& d : DevicesOf(peerOrKey)) {
        if (!IsConnected(d)) continue;
        peerLibraries[d].pending = true;
        QueueToPeer(d, Msg::LibGet, json::object());
    }
}

// ---- my Shared folder: index (piece hashes cached by path + size + mtime, so only new / changed files are read)

void Engine::LibraryLoop() {
    std::string cachePath;
    std::map<std::string, LibEntry> cache;  // rel -> entry
    {
        auto lk = Lock();
        cachePath = JoinPath(ConfigPath(), "shared_index.json");
    }
    json cj;
    if (LoadJsonFile(cachePath, cj) && cj.is_array())
        for (auto& e : cj) {
            LibEntry le;
            le.f.id = e.value("id", "");
            le.f.rel = e.value("rel", "");
            le.f.size = e.value("size", (int64_t)0);
            le.f.mtime = e.value("mtime", (int64_t)0);
            if (!Base64Decode(e.value("h", ""), le.hashes) || le.f.rel.empty()) continue;
            cache[le.f.rel] = le;
        }
    int64_t lastScan = 0;
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!libRescan_ && SteadyMs() - lastScan < 30000) continue;
        libRescan_ = false;
        lastScan = SteadyMs();
        std::string root = SharedDir();
        MakeDirs(root);
        std::vector<std::pair<std::string, std::string>> files;  // abs, rel
        CollectFiles(root, files);
        std::string prefix = BaseName(root) + "/";
        std::map<std::string, LibEntry> next;
        std::vector<std::pair<std::string, std::string>> todo;
        for (auto& [abs, rel0] : files) {
            std::string rel = rel0.rfind(prefix, 0) == 0 ? rel0.substr(prefix.size()) : rel0;
            if (rel.empty() || rel[0] == '.' || rel.find("/.") != std::string::npos) continue;  // hidden, partial downloads
            int64_t size = FileSize(abs), mtime = FileMTime(abs);
            if (size <= 0) continue;
            auto it = cache.find(rel);
            if (it != cache.end() && it->second.f.size == size && it->second.f.mtime == mtime) next[rel] = it->second;
            else todo.emplace_back(abs, rel);
        }
        {
            auto lk = Lock();
            libHashing = (int)todo.size();
        }
        for (auto& [abs, rel] : todo) {
            if (!running_) return;
            FILE* f = OpenFile(abs, "rb");
            if (!f) continue;
            LibEntry le;
            le.f.rel = rel;
            le.f.size = FileSize(abs);
            le.f.mtime = FileMTime(abs);
            std::vector<uint8_t> buf((size_t)kPieceSize);
            size_t n;
            int64_t total = 0;
            while ((n = fread(buf.data(), 1, buf.size(), f)) > 0 && running_) {
                Sha256 h;
                h.Update(buf.data(), n);
                uint8_t d[32];
                h.Final(d);
                le.hashes.append((const char*)d, 32);
                total += (int64_t)n;
            }
            fclose(f);
            if (total != le.f.size) continue;  // changed while reading: next round
            le.f.id = ContentId(le.hashes, le.f.size);
            next[rel] = le;
            auto lk = Lock();
            libHashing = std::max(0, libHashing - 1);
        }
        bool changed = next.size() != cache.size();
        if (!changed)
            for (auto& [rel, e] : next) {
                auto it = cache.find(rel);
                if (it == cache.end() || it->second.f.id != e.f.id) changed = true;
            }
        cache = next;
        json out = json::array();
        for (auto& [rel, e] : cache)
            out.push_back({{"id", e.f.id}, {"rel", rel}, {"size", e.f.size}, {"mtime", e.f.mtime}, {"h", Base64Encode(e.hashes)}});
        if (changed || !FileExists(cachePath)) SaveJsonFile(cachePath, out);
        auto lk = Lock();
        libHashing = 0;
        libIndex_.clear();
        myLibrary.clear();
        for (auto& [rel, e] : cache) {
            libIndex_[e.f.id] = e;
            myLibrary.push_back(e.f);
        }
        if (changed)
            for (auto& c : conns)
                if (c->ready && !c->closed && IsContact(c->peerId)) Queue(c, Msg::LibChanged, json::object());
    }
}

// ---- requests from contacts

void Engine::HandleLibrary(const std::shared_ptr<Connection>& c, Msg type, const json& j) {
    bool allowed = IsContact(c->peerId) && ShareAllowedNow();
    switch (type) {
        case Msg::LibGet: {
            json files = json::array();
            if (allowed)
                for (auto& f : myLibrary) files.push_back({{"id", f.id}, {"rel", f.rel}, {"size", f.size}, {"mtime", f.mtime}});
            Queue(c, Msg::LibList, {{"files", files}, {"off", !allowed}});
            break;
        }
        case Msg::LibList: {
            PeerLibrary& pl = peerLibraries[c->peerId];
            pl.files.clear();
            for (auto& f : j.value("files", json::array())) {
                LibFile lf{f.value("id", ""), f.value("rel", ""), f.value("size", (int64_t)0), f.value("mtime", (int64_t)0)};
                if (lf.id.size() == 64 && !lf.rel.empty() && lf.size > 0) pl.files.push_back(lf);
            }
            pl.off = j.value("off", false);
            pl.pending = false;
            pl.fetched = NowMs();
            // new sources for running downloads
            for (auto& [id, d] : swarm) d->lastHave = 0;
            break;
        }
        case Msg::LibChanged:
            if (IsContact(c->peerId)) Queue(c, Msg::LibGet, json::object());
            break;
        case Msg::PieceHave: {
            json have = json::array();
            if (allowed)
                for (auto& x : j.value("ids", json::array())) {
                    if (!x.is_string()) continue;
                    std::string id = x.get<std::string>();
                    if (libIndex_.count(id)) {
                        have.push_back({{"id", id}, {"full", true}});
                    } else {
                        auto it = swarm.find(id);  // partly downloaded: the pieces we have
                        if (it != swarm.end() && it->second->done > 0 && !it->second->hashes.empty())
                            have.push_back({{"id", id}, {"full", false}, {"bits", Bits(it->second->have)}});
                    }
                }
            Queue(c, Msg::PieceHaveReply, {{"have", have}});
            break;
        }
        case Msg::PieceHaveReply:
            for (auto& h : j.value("have", json::array())) {
                auto it = swarm.find(h.value("id", ""));
                if (it == swarm.end()) continue;
                auto& d = *it->second;
                SwarmSource& s = d.sources[c->peerId];
                s.full = h.value("full", false);
                if (!s.full) s.bits = Unbits(h.value("bits", ""), d.Pieces());
            }
            break;
        case Msg::PieceMapGet: {
            std::string id = j.value("id", "");
            const std::string* hashes = nullptr;
            int64_t size = 0;
            if (allowed) {
                auto it = libIndex_.find(id);
                if (it != libIndex_.end()) {
                    hashes = &it->second.hashes;
                    size = it->second.f.size;
                } else if (auto sw = swarm.find(id); sw != swarm.end() && !sw->second->hashes.empty()) {
                    hashes = &sw->second->hashes;
                    size = sw->second->size;
                }
            }
            if (hashes) Queue(c, Msg::PieceMap, {{"id", id}, {"size", size}, {"ps", kPieceSize}, {"h", Base64Encode(*hashes)}});
            break;
        }
        case Msg::PieceMap: {
            auto it = swarm.find(j.value("id", ""));
            if (it == swarm.end() || !it->second->hashes.empty()) break;
            auto& d = *it->second;
            std::string h;
            int64_t size = j.value("size", (int64_t)0), ps = j.value("ps", (int64_t)0);
            // only a map that really belongs to this content id
            if (ps != kPieceSize || size != d.size || !Base64Decode(j.value("h", ""), h) || h.size() != (size_t)d.Pieces() * 32 ||
                ContentId(h, size) != d.id) {
                d.sources[c->peerId].bad++;
                break;
            }
            d.hashes = h;
            if (d.have.size() != d.Pieces()) d.have.assign(d.Pieces(), 0);
            d.state = SwarmDownload::Running;
            MarkDirty();
            break;
        }
        case Msg::PieceGet: {
            std::string id = j.value("id", "");
            uint32_t idx = j.value("idx", 0u);
            bool ok = allowed && (libIndex_.count(id) ||
                                  (swarm.count(id) && idx < swarm[id]->have.size() && swarm[id]->have[idx]));
            if (!ok) {
                Queue(c, Msg::PieceNo, {{"id", id}, {"idx", idx}});
                break;
            }
            {
                std::lock_guard<std::mutex> l(c->qm);
                if (c->pieces.size() < 64) c->pieces.push_back({id, idx});
            }
            c->qcv.notify_one();
            break;
        }
        case Msg::PieceNo: {
            auto it = swarm.find(j.value("id", ""));
            if (it == swarm.end()) break;
            auto s = it->second->sources.find(c->peerId);
            if (s != it->second->sources.end()) {
                s->second.inflight.erase(j.value("idx", 0u));
                s->second.full = false;  // ask again later what it has
                s->second.bits.clear();
            }
            break;
        }
        default:
            break;
    }
}

// writer thread: read one requested piece from the file (complete one in my Shared folder, or a partial download)
bool Engine::ProducePiece(const std::shared_ptr<Connection>& c, std::vector<uint8_t>& buf, int64_t& wait) {
    Connection::PieceOut p;
    {
        std::lock_guard<std::mutex> l(c->qm);
        if (c->pieces.empty()) return false;
        p = c->pieces.front();
    }
    std::string path;
    int64_t size = 0;
    std::shared_ptr<SwarmDownload> partial;
    {
        auto lk = Lock();
        auto it = libIndex_.find(p.id);
        if (it != libIndex_.end()) {
            path = JoinPath(settings.sharedDir.empty() ? JoinPath(settings.downloadDir, "Shared") : settings.sharedDir, it->second.f.rel);
            size = it->second.f.size;
        } else if (auto sw = swarm.find(p.id); sw != swarm.end() && p.idx < sw->second->have.size() && sw->second->have[p.idx]) {
            partial = sw->second;
            path = partial->partPath;
            size = partial->size;
        }
    }
    int64_t off = (int64_t)p.idx * kPieceSize;
    size_t len = path.empty() || off >= size ? 0 : (size_t)std::min<int64_t>(kPieceSize, size - off);
    if (len > 0 && !ThrottleTake(len, wait)) return false;  // (upload limit: try again shortly)
    {
        std::lock_guard<std::mutex> l(c->qm);
        if (!c->pieces.empty()) c->pieces.pop_front();
    }
    if (len == 0) return false;
    buf.resize(36 + len);
    std::string raw = Raw(p.id);
    if (raw.size() != 32) return false;
    memcpy(buf.data(), raw.data(), 32);
    PutU32(buf.data() + 32, p.idx);
    FILE* f = OpenFile(path, "rb");
    bool ok = f && FileSeek(f, off) && fread(buf.data() + 36, 1, len, f) == len;
    if (f) fclose(f);
    return ok;
}

bool Engine::ThrottleTake(size_t& chunk, int64_t& wait) {
    int64_t limit;
    {
        auto lk = Lock();
        limit = settings.speedLimitKBs;
    }
    if (limit <= 0) return true;
    std::lock_guard<std::mutex> l(throttleMtx_);
    double rate = (double)limit * 1024.0;
    int64_t now = SteadyMs();
    if (tokenTs_ == 0) tokenTs_ = now;
    tokens_ += rate * (double)(now - tokenTs_) / 1000.0;
    tokenTs_ = now;
    double cap = std::max(rate, (double)chunk);
    if (tokens_ > cap) tokens_ = cap;
    if (tokens_ < (double)chunk) {
        wait = (int64_t)(((double)chunk - tokens_) / rate * 1000.0) + 1;
        return false;
    }
    tokens_ -= (double)chunk;
    return true;
}

// ---- downloading

bool Engine::DownloadShared(const LibFile& f) {
    auto lk = Lock();
    if (f.id.size() != 64 || f.size <= 0 || libIndex_.count(f.id)) return false;
    auto it = swarm.find(f.id);
    if (it != swarm.end()) {
        if (it->second->state == SwarmDownload::Paused || it->second->state == SwarmDownload::Failed)
            it->second->state = it->second->hashes.empty() ? SwarmDownload::Searching : SwarmDownload::Running;
        return true;
    }
    auto d = std::make_shared<SwarmDownload>();
    d->id = f.id;
    d->rel = SanitizeRelativePath(f.rel);
    if (d->rel.empty()) d->rel = f.id.substr(0, 12);
    d->size = f.size;
    d->created = NowMs();
    std::string part = JoinPath(settings.sharedDir.empty() ? JoinPath(settings.downloadDir, "Shared") : settings.sharedDir, ".lunashare-partial");
    MakeDirs(part);
    d->partPath = JoinPath(part, f.id.substr(0, 32) + ".part");
    swarm[f.id] = d;
    MarkDirty();
    return true;
}

void Engine::PauseShared(const std::string& id, bool pause) {
    auto lk = Lock();
    auto it = swarm.find(id);
    if (it == swarm.end()) return;
    auto& d = *it->second;
    if (pause && (d.state == SwarmDownload::Running || d.state == SwarmDownload::Searching)) {
        d.state = SwarmDownload::Paused;
        for (auto& [dev, s] : d.sources) s.inflight.clear();
    } else if (!pause && d.state == SwarmDownload::Paused) {
        d.state = d.hashes.empty() ? SwarmDownload::Searching : SwarmDownload::Running;
        d.lastHave = 0;
    }
    MarkDirty();
}

void Engine::CancelShared(const std::string& id) {
    auto lk = Lock();
    auto it = swarm.find(id);
    if (it == swarm.end()) return;
    if (it->second->state != SwarmDownload::Done) RemoveFile(it->second->partPath);
    swarm.erase(it);
    MarkDirty();
}

void Engine::SwarmTick() {
    int64_t now = SteadyMs();
    for (auto& [id, dp] : swarm) {
        SwarmDownload& d = *dp;
        if (d.state != SwarmDownload::Searching && d.state != SwarmDownload::Running) continue;
        // who has it? (all connected contacts, every 10 s or when something changed)
        if (now - d.lastHave > 10000) {
            d.lastHave = now;
            for (auto& c : conns)
                if (c->ready && !c->closed && IsContact(c->peerId)) Queue(c, Msg::PieceHave, {{"ids", json::array({id})}});
        }
        // sources that went away or keep sending garbage
        for (auto it = d.sources.begin(); it != d.sources.end();) {
            if (!ReadyConn(it->first) || it->second.bad >= 2) it = d.sources.erase(it);
            else ++it;
        }
        if (d.hashes.empty()) {
            if (now - d.lastMapAsk > 5000)
                for (auto& [dev, s] : d.sources)
                    if (s.full || !s.bits.empty()) {
                        d.lastMapAsk = now;
                        QueueToPeer(dev, Msg::PieceMapGet, {{"id", id}});
                        break;
                    }
            continue;
        }
        uint32_t n = d.Pieces();
        std::vector<uint8_t> inflight(n, 0);
        for (auto& [dev, s] : d.sources)
            for (auto it = s.inflight.begin(); it != s.inflight.end();) {
                if (now - it->second > 30000) it = s.inflight.erase(it);  // too slow: someone else gets it
                else inflight[(it++)->first] = 1;
            }
        uint32_t missing = 0;
        for (uint32_t i = 0; i < n; i++) missing += !d.have[i];
        // keep up to 8 pieces in flight per source; every source starts at a different place so they don't overlap
        int si = 0;
        for (auto& [dev, s] : d.sources) {
            uint32_t start = (uint32_t)(((uint64_t)n * (uint64_t)si++) / std::max<size_t>(1, d.sources.size()));
            while (s.inflight.size() < 8) {
                int64_t pick = -1;
                for (uint32_t k = 0; k < n && pick < 0; k++) {
                    uint32_t i = (start + k) % n;
                    bool avail = s.full || (i < s.bits.size() && s.bits[i]);
                    if (!d.have[i] && !inflight[i] && avail) pick = i;
                }
                // end game: the last pieces are asked from more than one source, whoever is faster wins
                if (pick < 0 && missing <= 8)
                    for (uint32_t i = 0; i < n && pick < 0; i++) {
                        bool avail = s.full || (i < s.bits.size() && s.bits[i]);
                        if (!d.have[i] && avail && !s.inflight.count(i)) pick = i;
                    }
                if (pick < 0) break;
                s.inflight[(uint32_t)pick] = now;
                inflight[(size_t)pick] = 1;
                QueueToPeer(dev, Msg::PieceGet, {{"id", id}, {"idx", (uint32_t)pick}});
            }
        }
        if (now - d.speedTs >= 1000) {
            double inst = d.speedTs ? (double)d.speedBytes * 1000.0 / (double)(now - d.speedTs) : 0;
            d.speed = d.speed == 0 ? inst : d.speed * 0.6 + inst * 0.4;
            d.speedBytes = 0;
            d.speedTs = now;
        }
    }
}

// reader thread: a piece arrived
void Engine::HandlePieceData(const std::shared_ptr<Connection>& c, const Frame& f) {
    if (f.payload.size() < 36) return;
    std::string id = ToHex(f.payload.data(), 32);
    uint32_t idx = GetU32(f.payload.data() + 32);
    const uint8_t* data = f.payload.data() + 36;
    size_t len = f.payload.size() - 36;
    std::shared_ptr<SwarmDownload> d;
    std::string expect;
    {
        auto lk = Lock();
        auto it = swarm.find(id);
        if (it == swarm.end() || it->second->hashes.empty() || idx >= it->second->Pieces()) return;
        d = it->second;
        if (d->have[idx]) {  // (end game duplicate)
            d->sources[c->peerId].inflight.erase(idx);
            return;
        }
        expect = d->hashes.substr((size_t)idx * 32, 32);
    }
    int64_t off = (int64_t)idx * d->pieceSize;
    size_t want = (size_t)std::min<int64_t>(d->pieceSize, d->size - off);
    Sha256 h;
    h.Update(data, len);
    uint8_t got[32];
    h.Final(got);
    bool good = len == want && memcmp(got, expect.data(), 32) == 0;
    bool written = false;
    if (good) {
        // (two sources' threads must not both create the file: the second "w+b" would wipe the first one's piece)
        std::lock_guard<std::mutex> io(d->io);
        if (!FileExists(d->partPath)) MakeDirs(DirName(d->partPath));  // (the folder may have been deleted meanwhile)
        FILE* fp = OpenFile(d->partPath, FileExists(d->partPath) ? "r+b" : "w+b");
        written = fp && FileSeek(fp, off) && fwrite(data, 1, len, fp) == len;
        if (fp) written = fclose(fp) == 0 && written;
    }
    auto lk = Lock();
    if (swarm.find(id) == swarm.end()) return;  // cancelled meanwhile
    SwarmSource& s = d->sources[c->peerId];
    s.inflight.erase(idx);
    if (!good) {
        s.bad++;  // two bad pieces and this source is dropped
        return;
    }
    if (!written) {
        d->state = SwarmDownload::Failed;
        d->error = "cannot write " + d->partPath;
        return;
    }
    totalDown += (int64_t)len;
    s.bytes += (int64_t)len;
    if (!d->have[idx]) {
        d->have[idx] = 1;
        d->done += (int64_t)len;
        d->speedBytes += (int64_t)len;
    }
    for (auto& [dev, o] : d->sources) o.inflight.erase(idx);
    if (d->done >= d->size) FinishSwarm(d);
    else SwarmTick();  // ask for the next piece right away (not only on the next maintenance tick)
}

void Engine::FinishSwarm(const std::shared_ptr<SwarmDownload>& d) {
    std::string root = settings.sharedDir.empty() ? JoinPath(settings.downloadDir, "Shared") : settings.sharedDir;
    std::string dest = UniquePath(JoinPath(root, d->rel));
    MakeDirs(DirName(dest));
    if (!RenameFile(d->partPath, dest)) {
        d->state = SwarmDownload::Failed;
        d->error = "cannot move the file to " + dest;
        return;
    }
    d->state = SwarmDownload::Done;
    d->finalPath = dest;
    for (auto& [dev, s] : d->sources) s.inflight.clear();
    // offered on to others right away (the piece hashes are already known)
    LibEntry le;
    le.f.id = d->id;
    std::string rel = dest.size() > root.size() + 1 ? dest.substr(root.size() + 1) : BaseName(dest);
    std::replace(rel.begin(), rel.end(), '\\', '/');
    le.f.rel = rel;
    le.f.size = d->size;
    le.f.mtime = FileMTime(dest);
    le.hashes = d->hashes;
    libIndex_[d->id] = le;
    myLibrary.push_back(le.f);
    libRescan_ = true;  // (updates the cache file)
    HistoryEntry h;
    h.tid = 0;
    h.name = BaseName(dest);
    h.path = dest;
    h.size = d->size;
    h.ts = NowMs();
    h.outgoing = false;
    h.verified = true;  // every piece was checked
    std::string from;
    int64_t most = -1;
    for (auto& [dev, s] : d->sources)
        if (s.bytes > most) { most = s.bytes; from = dev; }
    h.peer = from;
    h.peerName = from.empty() ? std::string() : PeerName(from);
    history.push_back(h);
    MarkDirty();
    Emit({EventType::TransferDone, "", BaseName(dest), 0});
}

json Engine::SwarmJson() {
    json a = json::array();
    for (auto& [id, d] : swarm) {
        if (d->state == SwarmDownload::Done) continue;
        a.push_back({{"id", d->id}, {"rel", d->rel}, {"size", d->size}, {"part", d->partPath}, {"h", Base64Encode(d->hashes)},
                     {"have", Bits(d->have)}, {"paused", d->state == SwarmDownload::Paused}, {"created", d->created}});
    }
    return a;
}

void Engine::LoadSwarm(const json& a) {
    if (!a.is_array()) return;
    for (auto& j : a) {
        auto d = std::make_shared<SwarmDownload>();
        d->id = j.value("id", "");
        d->rel = j.value("rel", "");
        d->size = j.value("size", (int64_t)0);
        d->partPath = j.value("part", "");
        d->created = j.value("created", (int64_t)0);
        Base64Decode(j.value("h", ""), d->hashes);
        if (d->id.size() != 64 || d->size <= 0 || d->partPath.empty()) continue;
        if (!d->hashes.empty() && (d->hashes.size() != (size_t)d->Pieces() * 32 || ContentId(d->hashes, d->size) != d->id)) d->hashes.clear();
        if (!d->hashes.empty()) {
            d->have = Unbits(j.value("have", ""), d->Pieces());
            // what's really on disk (a piece counts only if the part file reaches it)
            int64_t onDisk = FileSize(d->partPath);
            for (uint32_t i = 0; i < d->Pieces(); i++) {
                int64_t end = std::min<int64_t>((int64_t)(i + 1) * d->pieceSize, d->size);
                if (d->have[i] && end > onDisk) d->have[i] = 0;
                if (d->have[i]) d->done += end - (int64_t)i * d->pieceSize;
            }
        }
        d->state = j.value("paused", false) ? SwarmDownload::Paused : d->hashes.empty() ? SwarmDownload::Searching : SwarmDownload::Running;
        swarm[d->id] = d;
    }
}

}  // namespace ls
