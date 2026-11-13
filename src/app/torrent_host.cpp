#include "torrent_host.h"

#include <SDL.h>
#include <algorithm>

#include "crypto.h"
#include "platform.h"
#include "player_host.h"

using namespace ls;
using json = nlohmann::json;

namespace {
const int64_t kBlock = 256 * 1024;  // watching from a computer: read in blocks of this size
std::string Low(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
}  // namespace

TorrentHost& Torrents() {
    static TorrentHost t;
    return t;
}

bool TorrentHost::IsTorrentSource(const std::string& s) {
    std::string l = Low(s);
    return l.compare(0, 7, "magnet:") == 0 || (l.size() > 8 && l.compare(l.size() - 8, 8, ".torrent") == 0);
}

bool TorrentHost::Load() {
    if (IsMobilePlatform()) return false;
    std::vector<std::string> candidates;
    if (char* base = SDL_GetBasePath()) {
        std::string b = base;
        SDL_free(base);
#if defined(_WIN32)
        const char* names[] = {"lunashare_torrent.dll", "liblunashare_torrent.dll"};
#elif defined(__APPLE__)
        const char* names[] = {"liblunashare_torrent.dylib"};
#else
        const char* names[] = {"liblunashare_torrent.so"};
#endif
        for (auto& d : {b, JoinPath(b, "modules")})
            for (auto* n : names) candidates.push_back(JoinPath(d, n));
    }
    for (auto& c : candidates) {
        if (!FileExists(c)) continue;
        void* lib = SDL_LoadObject(c.c_str());
        if (!lib) {
            loadInfo = c + ": " + SDL_GetError();
            continue;
        }
        auto fn = (LstGetModuleFn)SDL_LoadFunction(lib, "lunashare_torrent_module");
        const LstModule* m = fn ? fn() : nullptr;
        if (!m || m->api_version != LST_API_VERSION) {
            SDL_UnloadObject(lib);
            continue;
        }
        lib_ = lib;
        mod_ = m;
        loadInfo = c;
        version = m->version ? m->version : "";
        return true;
    }
    return false;
}

json TorrentHost::SettingsJson(const Settings& s) const {
    return {{"down", s.torrentDown}, {"up", s.torrentUp}, {"port", s.torrentPort}, {"seed", s.torrentSeed},
            {"dht", true},           {"upnp", true},      {"agent", std::string("LunaShare/") + LUNASHARE_VERSION}};
}

std::string TorrentHost::DefaultDir() const {
    if (!eng_) return "";
    Settings s = eng_->GetSettings();
    return !s.torrentDir.empty() ? s.torrentDir : JoinPath(s.downloadDir, "Torrents");
}

void TorrentHost::Attach(Engine* e) {
    eng_ = e;
    auto lk = e->Lock();  // (the engine may be running already: a phone's screen taking it back)
    e->ownHook = [this](const std::string& from, const std::string& kind, const json& d) { Hook(from, kind, d); };
    if (mod_ && std::find(e->localCaps.begin(), e->localCaps.end(), "torrent") == e->localCaps.end()) e->localCaps.push_back("torrent");
}

void TorrentHost::Start() {
    Engine* e = eng_;
    if (!e) return;
    dir_ = JoinPath(e->ConfigPath(), "torrents");
    LoadPending();
    if (!mod_) return;
    MakeDirs(dir_);
    host_ = LstHost{LST_API_VERSION, this, [](void*, const char* s) { SDL_Log("%s", s); }};
    Settings s = e->GetSettings();
    if (!mod_->init(&host_, dir_.c_str(), SettingsJson(s).dump().c_str())) {
        loadInfo += ": could not start";
        return;
    }
    started_ = true;
    LoadMeta();
    quit_ = false;
    for (int i = 0; i < 2; i++) readers_.emplace_back([this] { ReadWorker(); });
}

void TorrentHost::Shutdown() {
    quit_ = true;
    rcv_.notify_all();
    for (auto& t : readers_)
        if (t.joinable()) t.join();
    readers_.clear();
    {
        std::lock_guard<std::mutex> l(sm_);
        stream_.reset();
    }
    if (started_ && mod_) mod_->shutdown();
    started_ = false;
}

void TorrentHost::ApplySettings(const Settings& s) {
    if (Available()) mod_->apply_settings(SettingsJson(s).dump().c_str());
}

// ---- who asked for which torrent (computer)
void TorrentHost::LoadMeta() {
    json j;
    if (!LoadJsonFile(JoinPath(dir_, "meta.json"), j) || !j.is_object()) return;
    for (auto& [id, v] : j.items())
        if (v.is_object()) meta_[id] = {v.value("from", ""), v.value("then", ""), v.value("sent", false)};
}

void TorrentHost::SaveMeta() {
    json j = json::object();
    for (auto& [id, m] : meta_) j[id] = {{"from", m.from}, {"then", m.then}, {"sent", m.sent}};
    SaveJsonFile(JoinPath(dir_, "meta.json"), j);
}

// ---- links waiting for a computer, and the computers known (phone)
void TorrentHost::LoadPending() {
    json j;
    if (!LoadJsonFile(JoinPath(eng_->ConfigPath(), "torrent_remote.json"), j) || !j.is_object()) return;
    if (j.contains("computers") && j["computers"].is_array())
        for (auto& c : j["computers"])
            if (c.is_string()) computers_.insert(c.get<std::string>());
    if (j.contains("pending") && j["pending"].is_array())
        for (auto& p : j["pending"])
            if (p.is_object() && pending_.size() < 50)
                pending_.push_back({p.value("cid", ""), p.value("device", ""), p.value("magnet", ""), p.value("torrent", ""), p.value("then", "keep"), 0, p.value("pick", false)});
}

void TorrentHost::SavePending() {
    json j = {{"computers", computers_}, {"pending", json::array()}};
    for (auto& p : pending_) j["pending"].push_back({{"cid", p.cid}, {"device", p.device}, {"magnet", p.magnet}, {"torrent", p.torrentB64}, {"then", p.then}, {"pick", p.pick}});
    SaveJsonFile(JoinPath(eng_->ConfigPath(), "torrent_remote.json"), j);
}

TorrentHost::Torrent TorrentHost::FromJson(const json& j) {
    Torrent t;
    if (!j.is_object()) return t;
    auto S = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    auto I = [&](const char* k) { return j.contains(k) && j[k].is_number() ? j[k].get<int64_t>() : (int64_t)0; };
    auto B = [&](const char* k) { return j.contains(k) && j[k].is_boolean() && j[k].get<bool>(); };
    t.id = S("id");
    t.name = S("name");
    t.dir = S("dir");
    t.path = S("path");
    t.state = S("state");
    t.error = S("error");
    t.size = I("size");
    t.done = I("done");
    t.uploaded = I("uploaded");
    t.added = I("added");
    t.downRate = (int)I("downRate");
    t.upRate = (int)I("upRate");
    t.peers = (int)I("peers");
    t.seeds = (int)I("seeds");
    t.seedsAll = (int)I("seedsAll");
    t.peersAll = (int)I("peersAll");
    t.swarmSeeds = j.contains("swarmSeeds") ? (int)I("swarmSeeds") : -1;
    t.swarmPeers = j.contains("swarmPeers") ? (int)I("swarmPeers") : -1;
    t.copies = j.contains("copies") && j["copies"].is_number() ? j["copies"].get<float>() : -1.0f;
    t.incoming = B("incoming");
    t.diskFree = j.contains("diskFree") ? I("diskFree") : -1;
    t.trackers = (int)I("trackers");
    t.trackersOk = (int)I("trackersOk");
    t.trackerMsg = S("trackerMsg");
    t.port = (int)I("port");
    t.mapped = (int)I("mapped");
    t.files = (int)I("files");
    t.paused = B("paused");
    t.single = B("single");
    t.watchFile = j.contains("watchFile") && j["watchFile"].is_number_integer() ? j["watchFile"].get<int>() : -1;
    t.watchSize = I("watchSize");
    t.watchName = S("watchName");
    t.from = S("from");
    t.then = S("then");
    t.sent = B("sent");
    return t;
}

json TorrentHost::ToJson(const Torrent& t) const {
    // (what a device of mine needs to show it; no folders of this computer)
    return {{"id", t.id},       {"name", t.name},         {"state", t.state},     {"error", t.error},       {"size", t.size},
            {"done", t.done},   {"downRate", t.downRate}, {"upRate", t.upRate},   {"peers", t.peers},       {"seeds", t.seeds},
            {"files", t.files}, {"paused", t.paused},     {"added", t.added},     {"watchFile", t.watchFile}, {"watchSize", t.watchSize},
            {"watchName", t.watchName}, {"from", t.from}, {"then", t.then},       {"sent", t.sent},
            {"seedsAll", t.seedsAll}, {"peersAll", t.peersAll}, {"swarmSeeds", t.swarmSeeds}, {"swarmPeers", t.swarmPeers},
            {"copies", t.copies}, {"incoming", t.incoming}, {"trackers", t.trackers}, {"trackersOk", t.trackersOk},
            {"trackerMsg", t.trackerMsg}, {"port", t.port}, {"mapped", t.mapped}, {"diskFree", t.diskFree}};
}

const std::vector<TorrentHost::Torrent>& TorrentHost::List() {
    if (!Available()) {
        list_.clear();
        return list_;
    }
    int64_t now = SteadyMs();
    if (listAt_ && now - listAt_ < 700) return list_;
    listAt_ = now;
    json j = json::parse(mod_->list(), nullptr, false);
    std::vector<Torrent> out;
    bool moving = false;
    if (j.is_array())
        for (auto& e : j) {
            Torrent t = FromJson(e);
            if (t.id.empty()) continue;
            // the movie to watch: the biggest one inside (looked up once the torrent says what's in it)
            Watch& w = watch_[t.id];
            if (!w.known && t.files > 0) {
                w.known = true;
                for (auto& f : Files(t.id))
                    if (PlayerHost::IsVideoPath(f.path) && f.size > w.size) w = {f.index, f.size, BaseName(f.path), true};
            }
            t.watchFile = w.file;
            t.watchSize = w.size;
            t.watchName = w.name;
            auto m = meta_.find(t.id);
            if (m != meta_.end()) {
                t.from = m->second.from;
                t.then = m->second.then;
                t.sent = m->second.sent;
            }
            if (t.state == "down" || t.state == "meta" || t.state == "checking" || t.upRate > 0) moving = true;
            if (const char* peer = (t.state == "down" || t.state == "meta") && t.peers == 0 ? getenv("LUNASHARE_TORRENT_PEER") : nullptr) {
                std::string p = peer;  // (development: "ip:port" of someone who has it)
                size_t c = p.rfind(':');
                if (c != std::string::npos) mod_->add_peer(t.id.c_str(), p.substr(0, c).c_str(), atoi(p.c_str() + c + 1));
            }
            out.push_back(std::move(t));
        }
    std::sort(out.begin(), out.end(), [](const Torrent& a, const Torrent& b) { return a.added != b.added ? a.added > b.added : a.id < b.id; });
    list_ = std::move(out);
    active_ = moving;
    return list_;
}

bool TorrentHost::Add(const std::string& source, const std::string& dir, std::string& id, std::string& err, bool pickFirst) {
    if (!Available()) {
        err = "no torrent module";
        return false;
    }
    char idb[64] = {0}, eb[512] = {0};
    json o = {{"dir", dir.empty() ? DefaultDir() : dir}, {"pickFirst", pickFirst}};
    if (!mod_->add(source.c_str(), o.dump().c_str(), idb, eb, sizeof eb)) {
        err = eb[0] ? eb : "can't add it";
        return false;
    }
    id = idb;
    listAt_ = 0;
    return true;
}

std::vector<TorrentHost::File> TorrentHost::FilesFromJson(const json& j) {
    std::vector<File> v;
    if (j.is_array())
        for (auto& e : j)
            if (e.is_object() && v.size() < 20000)
                v.push_back({e.value("i", 0), e.value("path", ""), e.value("size", (int64_t)0), e.value("done", (int64_t)0), e.value("want", true)});
    return v;
}

std::vector<TorrentHost::File> TorrentHost::Files(const std::string& id) {
    if (!Available()) return {};
    return FilesFromJson(json::parse(mod_->files(id.c_str()), nullptr, false));
}

void TorrentHost::SetWanted(const std::string& id, const std::vector<bool>& want) {
    if (!Available()) return;
    json w = json::array();
    for (bool b : want) w.push_back(b);
    mod_->set_wanted(id.c_str(), w.dump().c_str());
    watch_.erase(id);
    listAt_ = 0;
}

void TorrentHost::Pause(const std::string& id, bool paused) {
    if (!Available()) return;
    mod_->pause(id.c_str(), paused ? 1 : 0);
    listAt_ = 0;
}

void TorrentHost::Reannounce(const std::string& id) {
    if (!Available() || !mod_->reannounce) return;
    mod_->reannounce(id.c_str());
    listAt_ = 0;
}

void TorrentHost::Remove(const std::string& id, bool deleteFiles) {
    if (!Available()) return;
    mod_->remove(id.c_str(), deleteFiles ? 1 : 0);
    if (meta_.erase(id)) SaveMeta();
    watch_.erase(id);
    listAt_ = 0;
}

std::function<int(int64_t, uint8_t*, int)> TorrentHost::Reader(const std::string& id, int file) {
    if (!Available()) return nullptr;
    const LstModule* m = mod_;
    m->stream(id.c_str(), file, 1);
    return [m, id, file](int64_t off, uint8_t* buf, int len) {
        int late = 0;
        int n = m->read(id.c_str(), file, off, buf, len, 200, &late);
        if (n < 0) return -1;
        return n == 0 && late ? -2 : n;  // (-2: not here yet, asked again)
    };
}

void TorrentHost::StopWatching(const std::string& id) {
    if (Available()) mod_->stream(id.c_str(), -1, 0);
    std::lock_guard<std::mutex> l(sm_);
    if (stream_ && stream_->id == id) stream_.reset();
}

// ---- my other devices
void TorrentHost::Hook(const std::string& from, const std::string& kind, const json& d) {
    if (kind != "torrent" || !d.is_object()) return;
    std::string op = d.value("op", "");
    if (op == "data") {  // (a block of what's being watched: straight to its reader)
        std::shared_ptr<RemoteStream> st;
        {
            std::lock_guard<std::mutex> l(sm_);
            st = stream_;
        }
        std::string rid = d.value("rid", "");
        if (!st || st->device != from || rid.compare(0, st->tag.size() + 1, st->tag + ":") != 0) return;
        int64_t block = std::atoll(rid.c_str() + st->tag.size() + 1);
        std::string bin;
        bool wait = d.value("wait", false);
        if (!wait && !Base64Decode(d.value("d", ""), bin)) return;
        std::lock_guard<std::mutex> l(st->m);
        st->asked.erase(block);
        if (!wait) {
            st->blocks[block] = std::move(bin);
            st->order.push_back(block);
            while (st->order.size() > 24) {  // (about 6 MB kept)
                st->blocks.erase(st->order.front());
                st->order.pop_front();
            }
        }
        st->cv.notify_all();
        return;
    }
    if (op == "read") {
        if (!Available()) return;
        ReadReq r{from, d.value("rid", ""), d.value("id", ""), d.value("file", 0), d.value("off", (int64_t)0), d.value("len", 0), SteadyMs()};
        if (r.rid.empty() || r.len <= 0 || r.len > 1 << 20 || r.off < 0) return;
        std::lock_guard<std::mutex> l(rm_);
        if (reads_.size() < 64) reads_.push_back(std::move(r));
        rcv_.notify_one();
        return;
    }
    std::lock_guard<std::mutex> l(qm_);
    if (cmds_.size() < 200) cmds_.push_back({from, d});
}

// computer: reads for a device that watches (they wait for the pieces, so on threads of their own)
void TorrentHost::ReadWorker() {
    std::string buf;
    for (;;) {
        ReadReq r;
        {
            std::unique_lock<std::mutex> l(rm_);
            rcv_.wait(l, [&] { return quit_ || !reads_.empty(); });
            if (quit_) return;
            r = std::move(reads_.front());
            reads_.pop_front();
        }
        if (SteadyMs() - r.at > 15000) continue;  // (asked long ago: it has asked again since)
        buf.resize((size_t)r.len);
        int got = 0, late = 0;
        bool gone = false;
        while (got < r.len && !quit_) {  // (whole blocks, except at the end of the file)
            int n = mod_->read(r.id.c_str(), r.file, r.off + got, (uint8_t*)&buf[(size_t)got], r.len - got, got ? 2000 : 6000, &late);
            if (n < 0) gone = true;
            if (n <= 0) break;
            got += n;
        }
        if (quit_) return;
        json reply = {{"op", "data"}, {"rid", r.rid}};
        if (gone) reply["eof"] = true, reply["d"] = "";
        else if (got == 0 && late) reply["wait"] = true;
        else if (got < r.len && late) reply["wait"] = true;  // (a part only: the device asks for the block again)
        else reply["d"] = Base64Encode(buf.substr(0, (size_t)got));
        eng_->OwnSend(r.from, "torrent", reply);
    }
}

void TorrentHost::HandleCmd(const std::string& from, const json& d) {
    std::string op = d.value("op", "");
    if (op == "status") {  // (phone: what a computer of mine reports)
        Remote& r = remote_[from];
        r.list.clear();
        if (d.contains("list") && d["list"].is_array())
            for (auto& e : d["list"]) {
                Torrent t = FromJson(e);
                if (!t.id.empty() && r.list.size() < 500) r.list.push_back(std::move(t));
            }
        r.at = SteadyMs();
        r.off = d.value("off", false);
        if (computers_.insert(from).second) SavePending();
        bool changed = false;
        if (d.contains("acks") && d["acks"].is_array())
            for (auto& a : d["acks"])
                if (a.is_string())
                    for (auto it = pending_.begin(); it != pending_.end();)
                        if (it->cid == a.get<std::string>()) it = pending_.erase(it), changed = true;
                        else ++it;
        if (d.contains("errors") && d["errors"].is_array())
            for (auto& e : d["errors"]) {
                if (!e.is_object()) continue;
                std::string cid = e.value("cid", "");
                for (auto it = pending_.begin(); it != pending_.end();)
                    if (it->cid == cid) it = pending_.erase(it), changed = true;
                    else ++it;
                if (remoteErrors.size() < 8) remoteErrors.push_back(e.value("error", ""));
            }
        if (changed) SavePending();
        return;
    }
    if (op == "filelist") {  // (phone: the files of one torrent on a computer of mine)
        RemoteFileList& r = remoteFiles_[from + "/" + d.value("id", "")];
        r.files = FilesFromJson(d.value("files", json::array()));
        r.known = d.value("known", true);
        return;
    }
    // ---- the rest is asked of a computer with the module
    if (!mod_) return;
    if (!Available() || !eng_->GetSettings().torrentRemote) {  // (turned off here: said so, nothing done)
        json m = {{"op", "status"}, {"list", json::array()}, {"off", true}};
        if (op == "add" && !d.value("cid", "").empty())
            m["errors"] = json::array({{{"cid", d.value("cid", "")}, {"error", "that computer doesn't take torrents from your devices (its torrent settings)"}}});
        eng_->OwnSend(from, "torrent", m);
        return;
    }
    subs_[from] = SteadyMs() + 30000;
    statusAt_ = 0;  // (answer soon)
    if (op == "add") {
        std::string cid = d.value("cid", ""), magnet = d.value("magnet", ""), then = d.value("then", "keep"), err, id;
        bool ok = false;
        char idb[64] = {0}, eb[512] = {0};
        json o = {{"dir", DefaultDir()}, {"pickFirst", d.value("pick", false)}};
        if (!magnet.empty() && magnet.size() < 16384 && magnet.compare(0, 7, "magnet:") == 0) {
            ok = mod_->add(magnet.c_str(), o.dump().c_str(), idb, eb, sizeof eb) != 0;
        } else {
            std::string bin;
            if (Base64Decode(d.value("torrent", ""), bin) && !bin.empty() && bin.size() < (8u << 20))
                ok = mod_->add_data((const uint8_t*)bin.data(), (int)bin.size(), o.dump().c_str(), idb, eb, sizeof eb) != 0;
            else snprintf(eb, sizeof eb, "not a torrent");
        }
        if (ok) {
            id = idb;
            Meta& m = meta_[id];
            if (m.from.empty() || m.from == from) {  // (one that was here already keeps who asked first)
                m.from = from;
                m.then = then == "send" ? "send" : "keep";
                m.sent = false;
            }
            SaveMeta();
            if (!cid.empty()) acks_[from].push_back(cid);
        } else if (!cid.empty()) {
            errs_[from].push_back({{"cid", cid}, {"error", eb[0] ? eb : "can't add it"}});
        }
        listAt_ = 0;
    } else if (op == "pause") {
        Pause(d.value("id", ""), d.value("on", true));
    } else if (op == "files") {  // (the list is empty until a magnet link brought it)
        std::string id = d.value("id", "");
        json files = json::parse(mod_->files(id.c_str()), nullptr, false);
        bool known = files.is_array() && !files.empty();
        if (known && files.size() > 5000) files.erase(files.begin() + 5000, files.end());  // (one message stays sane)
        eng_->OwnSend(from, "torrent", {{"op", "filelist"}, {"id", id}, {"files", known ? files : json::array()}, {"known", known}});
        return;
    } else if (op == "want") {
        std::vector<bool> w;
        if (d.contains("want") && d["want"].is_array())
            for (auto& b : d["want"]) w.push_back(b.is_boolean() ? b.get<bool>() : true);
        if (!w.empty()) SetWanted(d.value("id", ""), w);
    } else if (op == "remove") {
        Remove(d.value("id", ""), d.value("files", false));
    } else if (op == "send") {
        Meta& m = meta_[d.value("id", "")];
        m.from = from;
        m.then = "send";
        m.sent = false;
        SaveMeta();
        sendCheckAt_ = 0;
    }
}

void TorrentHost::PushStatus() {
    int64_t now = SteadyMs();
    for (auto it = subs_.begin(); it != subs_.end();)
        if (it->second < now) it = subs_.erase(it);
        else ++it;
    if (subs_.empty() || now - statusAt_ < 1500) return;
    statusAt_ = now;
    listAt_ = 0;
    json list = json::array();
    for (auto& t : List()) list.push_back(ToJson(t));
    for (auto& [dev, until] : subs_) {
        json m = {{"op", "status"}, {"list", list}};
        auto a = acks_.find(dev);
        if (a != acks_.end()) m["acks"] = a->second;
        auto e = errs_.find(dev);
        if (e != errs_.end()) m["errors"] = e->second;
        if (eng_->OwnSend(dev, "torrent", m)) {
            acks_.erase(dev);
            errs_.erase(dev);
        }
    }
}

// computer: torrents a device of mine wanted sent - when they're done and it is online
void TorrentHost::SendFinished() {
    int64_t now = SteadyMs();
    if (now - sendCheckAt_ < 3000) return;
    sendCheckAt_ = now;
    bool any = false;
    for (auto& [id, m] : meta_)
        if (m.then == "send" && !m.sent && !m.from.empty()) any = true;
    if (!any) return;
    for (auto& t : List()) {
        auto it = meta_.find(t.id);
        if (it == meta_.end() || it->second.then != "send" || it->second.sent || !t.Complete()) continue;
        if (!eng_->OwnOnline(it->second.from)) continue;
        std::vector<std::pair<std::string, std::string>> files;
        for (auto& f : Files(t.id)) {
            if (!f.want || f.done < f.size) continue;
            std::string abs = JoinPath(t.dir, f.path);
            if (FileExists(abs)) files.push_back({abs, f.path});
        }
        if (files.empty()) continue;
        if (eng_->SendFileList(it->second.from, files, t.name, true)) {
            it->second.sent = true;
            SaveMeta();
        }
    }
}

void TorrentHost::Tick() {
    if (!eng_) return;
    std::deque<std::pair<std::string, json>> q;
    {
        std::lock_guard<std::mutex> l(qm_);
        q.swap(cmds_);
    }
    for (auto& [from, d] : q) HandleCmd(from, d);
    if (Available()) {
        PushStatus();
        SendFinished();
        if (SteadyMs() - listAt_ > 3000) List();  // (keeps "something is moving" true to the minute)
    }
    // links waiting for their computer
    int64_t now = SteadyMs();
    if (!pending_.empty() && now - pendingAt_ > 2000) {
        pendingAt_ = now;
        for (auto& p : pending_) {
            if (p.sentAt && now - p.sentAt < 20000) continue;
            json m = {{"op", "add"}, {"cid", p.cid}, {"then", p.then}};
            if (p.pick) m["pick"] = true;
            if (!p.magnet.empty()) m["magnet"] = p.magnet;
            else m["torrent"] = p.torrentB64;
            if (eng_->OwnSend(p.device, "torrent", m)) p.sentAt = now;
        }
    }
}

std::vector<std::string> TorrentHost::Computers() {
    std::vector<std::string> v;
    if (!eng_) return v;
    bool changed = false;
    for (auto& d : eng_->MyOtherDevices()) {
        if (eng_->PeerHasCap(d, "torrent")) {
            if (computers_.insert(d).second) changed = true;
        }
        if (computers_.count(d)) v.push_back(d);
    }
    if (changed) SavePending();
    return v;
}

bool TorrentHost::RemoteAdd(const std::string& device, const std::string& source, const std::string& then, std::string& err, bool pickFirst) {
    PendingAdd p;
    p.pick = pickFirst;
    p.cid = RandomHex(8);
    p.device = device;
    p.then = then;
    if (Low(source).compare(0, 7, "magnet:") == 0) {
        p.magnet = source;
    } else {
        std::string bin;
        int64_t size = FileSize(source);
        if (size <= 0 || size > (8 << 20) || !LoadFileBytes(source, bin)) {
            err = "can't read the file";
            return false;
        }
        p.torrentB64 = Base64Encode(bin);
    }
    if (pending_.size() >= 50) {
        err = "too many waiting";
        return false;
    }
    pending_.push_back(std::move(p));
    SavePending();
    pendingAt_ = 0;
    return true;
}

const std::vector<TorrentHost::File>* TorrentHost::RemoteFiles(const std::string& device, const std::string& id, int64_t* askedFor) {
    if (!eng_) return nullptr;
    RemoteFileList& r = remoteFiles_[device + "/" + id];
    int64_t now = SteadyMs();
    if (!r.firstAsked) r.firstAsked = now;
    if (now - r.asked > 1500) {  // (progress moves; a magnet link's list may still be coming)
        r.asked = now;
        eng_->OwnSend(device, "torrent", {{"op", "files"}, {"id", id}});
    }
    if (askedFor) *askedFor = now - r.firstAsked;
    return r.known ? &r.files : nullptr;
}

void TorrentHost::RemoteSetWanted(const std::string& device, const std::string& id, const std::vector<bool>& want) {
    if (!eng_) return;
    json w = json::array();
    for (bool b : want) w.push_back(b);
    eng_->OwnSend(device, "torrent", {{"op", "want"}, {"id", id}, {"want", w}});
    remoteFiles_.erase(device + "/" + id);
}

void TorrentHost::RemoteWatchList(const std::string& device) {
    Remote& r = remote_[device];
    int64_t now = SteadyMs();
    if (r.asked && now - r.asked < 10000) return;
    r.asked = now;
    eng_->OwnSend(device, "torrent", {{"op", "list"}});
}

const std::vector<TorrentHost::Torrent>* TorrentHost::RemoteList(const std::string& device, bool* fresh) {
    auto it = remote_.find(device);
    if (it == remote_.end() || !it->second.at) return nullptr;
    if (fresh) *fresh = SteadyMs() - it->second.at < 8000;
    return &it->second.list;
}

bool TorrentHost::RemoteOff(const std::string& device) {
    auto it = remote_.find(device);
    return it != remote_.end() && it->second.off;
}

void TorrentHost::RemotePause(const std::string& device, const std::string& id, bool on) {
    eng_->OwnSend(device, "torrent", {{"op", "pause"}, {"id", id}, {"on", on}});
}
void TorrentHost::RemoteRemove(const std::string& device, const std::string& id, bool files) {
    eng_->OwnSend(device, "torrent", {{"op", "remove"}, {"id", id}, {"files", files}});
}
void TorrentHost::RemoteSend(const std::string& device, const std::string& id) {
    eng_->OwnSend(device, "torrent", {{"op", "send"}, {"id", id}});
}

int TorrentHost::PendingCount(const std::string& device) {
    int n = 0;
    for (auto& p : pending_)
        if (p.device == device) n++;
    return n;
}

std::function<int(int64_t, uint8_t*, int)> TorrentHost::RemoteReader(const std::string& device, const std::string& id, int file, int64_t size) {
    auto st = std::make_shared<RemoteStream>();
    st->device = device;
    st->id = id;
    st->file = file;
    st->size = size;
    st->tag = std::to_string(ridSeq_++);
    {
        std::lock_guard<std::mutex> l(sm_);
        stream_ = st;
    }
    Engine* e = eng_;
    return [st, e](int64_t off, uint8_t* buf, int len) -> int {
        if (off >= st->size) return 0;
        int64_t block = off / kBlock, last = (st->size - 1) / kBlock;
        // what to ask for: this block when it isn't here, and the next few (asked outside the lock: the answers
        // come in on the engine's thread, which takes the two locks the other way round)
        std::vector<int64_t> ask;
        {
            std::lock_guard<std::mutex> l(st->m);
            int64_t now = SteadyMs();
            for (int64_t b = block; b <= std::min(last, block + 3); b++) {
                if (st->blocks.count(b)) continue;
                auto a = st->asked.find(b);
                if (a != st->asked.end() && now - a->second < 12000) continue;
                st->asked[b] = now;
                ask.push_back(b);
            }
        }
        for (int64_t b : ask) {
            int64_t o = b * kBlock;
            e->OwnSend(st->device, "torrent",
                       {{"op", "read"}, {"rid", st->tag + ":" + std::to_string(b)}, {"id", st->id}, {"file", st->file}, {"off", o},
                        {"len", (int)std::min<int64_t>(kBlock, st->size - o)}});
        }
        std::unique_lock<std::mutex> l(st->m);
        auto it = st->blocks.find(block);
        if (it == st->blocks.end()) {
            st->cv.wait_for(l, std::chrono::milliseconds(200));
            it = st->blocks.find(block);
            if (it == st->blocks.end()) return -2;  // (not here yet)
        }
        int64_t in = off - block * kBlock;
        if (in >= (int64_t)it->second.size()) return 0;
        int n = (int)std::min<int64_t>(len, (int64_t)it->second.size() - in);
        memcpy(buf, it->second.data() + in, (size_t)n);
        return n;
    };
}
