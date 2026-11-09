// Folder sync between my own devices: the files of a chosen folder go to one of my other devices by themselves -
// new ones and changed ones, a minute or so after they appear (and when that device comes online). Nothing is
// deleted on the other side. What was sent (size + time of each file) is kept in folder_sync.json.
#include <filesystem>

#include "engine.h"
#include "platform.h"

namespace ls {

namespace fs = std::filesystem;

void Engine::FolderSyncLoad() {
    if (fsLoaded_) return;
    fsLoaded_ = true;
    json j;
    if (!LoadJsonFile(JoinPath(ConfigPath(), "folder_sync.json"), j) || !j.is_object()) return;
    for (auto& [id, files] : j.items()) {
        if (!files.is_object()) continue;
        auto& m = fsIndex_[id];
        for (auto& [rel, sig] : files.items())
            if (sig.is_string()) m[rel] = sig.get<std::string>();
    }
}

void Engine::FolderSyncSave() {
    json j = json::object();
    for (auto& [id, m] : fsIndex_) {
        bool known = false;
        for (auto& f : settings.folderSyncs) known |= f.id == id;
        if (known) j[id] = m;  // (a removed folder is forgotten)
    }
    SaveJsonFile(JoinPath(ConfigPath(), "folder_sync.json"), j);
}

static bool SkipName(const std::string& n) {
    if (n.empty() || n[0] == '.') return true;  // hidden, and phones' ".pending-..." / ".trashed-..."
    auto ends = [&](const char* e) {
        size_t l = strlen(e);
        return n.size() >= l && n.compare(n.size() - l, l, e) == 0;
    };
    return ends(".part") || ends(".tmp") || ends(".crdownload") || ends(".download") || ends("~");
}

void Engine::FolderSyncTick(bool force) {
    int64_t now = SteadyMs();
    std::vector<Settings::FolderSync> todo;
    {
        auto lk = Lock();
        FolderSyncLoad();
        // what was on its way: arrived -> remembered; failed -> tried again next time
        bool changed = false;
        for (auto it = fsPending_.begin(); it != fsPending_.end();) {
            auto t = transfers.find(it->first);
            if (t == transfers.end() || t->second->state == TState::Failed || t->second->state == TState::Cancelled ||
                t->second->state == TState::Rejected) {
                it = fsPending_.erase(it);
            } else if (t->second->state == TState::Completed) {
                fsIndex_[it->second.id][it->second.rel] = it->second.sig;
                changed = true;
                it = fsPending_.erase(it);
            } else {
                ++it;
            }
        }
        if (changed) FolderSyncSave();
        if (!force && !fsSoon_ && now - fsLastScan_ < 60000) return;
        fsLastScan_ = now;
        fsSoon_ = false;
        for (auto& f : settings.folderSyncs) {
            if (!f.on || f.path.empty() || !ReadyConn(f.to)) continue;
            auto d = devices.find(f.to);
            if (d == devices.end() || MyAccount().empty() || d->second.accountPub != MyAccount()) continue;  // (mine only)
            todo.push_back(f);
        }
    }
    for (auto& f : todo) {
        // the folder, without the lock (it may be big)
        struct File {
            std::string rel, abs, sig;
        };
        std::vector<File> files;
        std::error_code ec;
        fs::path root = fs::u8path(f.path);
        if (!fs::is_directory(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator() && files.size() < 20000; it.increment(ec)) {
            std::string name = it->path().filename().u8string();
            if (SkipName(name)) {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::string abs = it->path().u8string();
            int64_t size = FileSize(abs), mt = FileMTime(abs);
            auto age = fs::file_time_type::clock::now() - it->last_write_time(ec);  // (the file clock's own epoch: it differs per system)
            if (size < 0 || ec || age < std::chrono::seconds(10)) continue;            // (still being written)
            std::string rel = fs::relative(it->path(), root, ec).generic_u8string();
            if (ec || rel.empty()) continue;
            files.push_back({rel, abs, std::to_string(size) + ":" + std::to_string(mt)});
        }
        auto lk = Lock();
        auto& idx = fsIndex_[f.id];
        std::set<std::string> onTheWay;
        for (auto& [tid, p] : fsPending_)
            if (p.id == f.id) onTheWay.insert(p.rel);
        std::string folder = BaseName(f.path);
        if (folder.empty()) folder = "Folder";
        std::vector<std::pair<std::string, std::string>> send;
        std::map<std::string, const File*> byAbs;
        for (auto& x : files) {
            auto i = idx.find(x.rel);
            if ((i != idx.end() && i->second == x.sig) || onTheWay.count(x.rel)) continue;
            send.push_back({x.abs, folder + "/" + x.rel});
            byAbs[x.abs] = &x;
            if (send.size() >= 500) break;  // (the rest next time)
        }
        if (send.empty()) continue;
        uint64_t batch = SendFileList(f.to, send, folder, true);
        for (auto& [tid, t] : transfers)
            if (t->batch == batch && t->outgoing) {
                auto b = byAbs.find(t->localPath);
                if (b != byAbs.end()) fsPending_[tid] = {f.id, b->second->rel, b->second->sig};
            }
    }
}

std::string Engine::FolderSyncStatus(const std::string& id) {
    auto lk = Lock();
    FolderSyncLoad();
    int sent = 0, way = 0;
    if (auto i = fsIndex_.find(id); i != fsIndex_.end()) sent = (int)i->second.size();
    for (auto& [tid, p] : fsPending_) way += p.id == id;
    return std::to_string(sent) + "|" + std::to_string(way);
}

}  // namespace ls
