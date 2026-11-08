// Music player: library (folders -> tracks with tags and covers), playlists, and the player that keeps going in the
// background. See music.h.
#include "music.h"

#include <SDL.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>

#include "lunashare_player.h"
#include "model.h"
#include "platform.h"

using json = nlohmann::json;
using namespace ls;

namespace music {

Library& Lib() {
    static Library l;
    return l;
}
Player& Play() {
    static Player p;
    return p;
}

static std::string Lower(std::string s) {
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

bool Library::IsAudioPath(const std::string& path) {
    std::string p = Lower(path);
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = p.substr(dot + 1);
    return e == "mp3" || e == "m4a" || e == "aac" || e == "flac" || e == "ogg" || e == "oga" || e == "opus" || e == "wav" || e == "wma" ||
           e == "alac" || e == "aiff" || e == "aif" || e == "ape" || e == "wv" || e == "mka";
}

std::string Library::DefaultMusicDir() {
#if defined(__ANDROID__)
    return "/storage/emulated/0/Music";
#else
    if (const char* x = getenv("XDG_MUSIC_DIR")) return x;
    return JoinPath(HomeDir(), "Music");
#endif
}

// ---------------------------------------------------------------- library

void Library::Init(const std::string& configDir, const LspModule* mod) {
    mod_ = mod && mod->api_version >= 4 && mod->tags ? mod : nullptr;
    dir_ = configDir;
    file_ = JoinPath(configDir, "music.json");
    statePath_ = JoinPath(configDir, "music_state.json");
    coverDir_ = JoinPath(configDir, "music_covers");
    json j;
    if (LoadJsonFile(file_, j) && j.is_object()) {
        for (auto& f : j.value("folders", json::array()))
            if (f.is_string()) folders_.push_back(f.get<std::string>());
        for (auto& t : j.value("tracks", json::array())) {
            if (!t.is_object()) continue;
            Track x;
            x.path = t.value("p", "");
            x.title = t.value("t", "");
            x.artist = t.value("a", "");
            x.album = t.value("al", "");
            x.albumArtist = t.value("aa", "");
            x.genre = t.value("g", "");
            x.year = t.value("y", "");
            x.cover = t.value("c", "");
            x.number = t.value("n", 0);
            x.disc = t.value("d", 0);
            x.duration = t.value("du", 0.0);
            x.size = t.value("s", (int64_t)0);
            x.mtime = t.value("m", (int64_t)0);
            x.added = t.value("ad", (int64_t)0);
            if (!x.path.empty()) tracks_[x.path] = x;
        }
        for (auto& p : j.value("playlists", json::array())) {
            if (!p.is_object()) continue;
            Playlist pl;
            pl.id = p.value("id", "");
            pl.name = p.value("name", "");
            for (auto& x : p.value("paths", json::array()))
                if (x.is_string()) pl.paths.push_back(x.get<std::string>());
            if (!pl.id.empty()) playlists_.push_back(pl);
        }
    }
    th_ = std::thread([this] { ScanLoop(); });
}

void Library::Shutdown() {
    {
        std::lock_guard<std::mutex> l(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    Save();
}

void Library::Save() {
    json j;
    {
        std::lock_guard<std::mutex> l(m_);
        if (!dirty_) return;
        dirty_ = false;
        json tr = json::array();
        for (auto& [p, t] : tracks_)
            tr.push_back({{"p", t.path}, {"t", t.title}, {"a", t.artist}, {"al", t.album}, {"aa", t.albumArtist}, {"g", t.genre}, {"y", t.year},
                          {"c", t.cover}, {"n", t.number}, {"d", t.disc}, {"du", t.duration}, {"s", t.size}, {"m", t.mtime}, {"ad", t.added}});
        json pls = json::array();
        for (auto& p : playlists_) pls.push_back({{"id", p.id}, {"name", p.name}, {"paths", p.paths}});
        j = {{"v", 1}, {"folders", folders_}, {"tracks", tr}, {"playlists", pls}};
    }
    MakeDirs(dir_);
    SaveJsonFile(file_, j);
}

std::vector<std::string> Library::Folders() {
    std::lock_guard<std::mutex> l(m_);
    return folders_;
}

void Library::AddFolder(const std::string& dir) {
    {
        std::lock_guard<std::mutex> l(m_);
        if (dir.empty() || std::find(folders_.begin(), folders_.end(), dir) != folders_.end()) return;
        folders_.push_back(dir);
        dirty_ = true;
    }
    Save();
    Rescan();
}

void Library::RemoveFolder(const std::string& dir) {
    {
        std::lock_guard<std::mutex> l(m_);
        folders_.erase(std::remove(folders_.begin(), folders_.end(), dir), folders_.end());
        dirty_ = true;
    }
    Save();
    Rescan();
}

void Library::SetImplicitFolders(const std::vector<std::string>& dirs) {
    std::lock_guard<std::mutex> l(m_);
    if (dirs == implicit_) return;
    implicit_ = dirs;
    want_ = true;
    cv_.notify_all();
}

void Library::Rescan() {
    std::lock_guard<std::mutex> l(m_);
    want_ = true;
    cv_.notify_all();
}

std::vector<Track> Library::Tracks() {
    std::lock_guard<std::mutex> l(m_);
    std::vector<Track> v;
    v.reserve(tracks_.size());
    for (auto& [p, t] : tracks_) v.push_back(t);
    return v;
}

bool Library::Find(const std::string& path, Track& out) {
    std::lock_guard<std::mutex> l(m_);
    auto it = tracks_.find(path);
    if (it == tracks_.end()) return false;
    out = it->second;
    return true;
}

std::vector<Playlist> Library::Playlists() {
    std::lock_guard<std::mutex> l(m_);
    return playlists_;
}

std::string Library::CreatePlaylist(const std::string& name) {
    Playlist p;
    char id[24];
    snprintf(id, sizeof(id), "%016llx", (unsigned long long)RandomU64());
    p.id = id;
    p.name = name.empty() ? "Playlist" : name;
    {
        std::lock_guard<std::mutex> l(m_);
        playlists_.push_back(p);
        dirty_ = true;
        version_++;
    }
    Save();
    return p.id;
}

void Library::RenamePlaylist(const std::string& id, const std::string& name) {
    {
        std::lock_guard<std::mutex> l(m_);
        for (auto& p : playlists_)
            if (p.id == id) p.name = name;
        dirty_ = true;
        version_++;
    }
    Save();
}

void Library::DeletePlaylist(const std::string& id) {
    {
        std::lock_guard<std::mutex> l(m_);
        playlists_.erase(std::remove_if(playlists_.begin(), playlists_.end(), [&](const Playlist& p) { return p.id == id; }), playlists_.end());
        dirty_ = true;
        version_++;
    }
    Save();
}

void Library::AddToPlaylist(const std::string& id, const std::vector<std::string>& paths) {
    {
        std::lock_guard<std::mutex> l(m_);
        for (auto& p : playlists_)
            if (p.id == id) p.paths.insert(p.paths.end(), paths.begin(), paths.end());
        dirty_ = true;
        version_++;
    }
    Save();
}

void Library::SetPlaylistPaths(const std::string& id, const std::vector<std::string>& paths) {
    {
        std::lock_guard<std::mutex> l(m_);
        for (auto& p : playlists_)
            if (p.id == id) p.paths = paths;
        dirty_ = true;
        version_++;
    }
    Save();
}

// an embedded cover is kept once per picture (an album's tracks usually carry the same one)
std::string Library::CoverFor(const std::string& path, const uint8_t* data, int size) {
    if (data && size > 16) {
        uint64_t h = 1469598103934665603ULL;
        for (int i = 0; i < size; i++) h = (h ^ data[i]) * 1099511628211ULL;
        bool png = size > 4 && data[0] == 0x89 && data[1] == 'P';
        char name[40];
        snprintf(name, sizeof(name), "%016llx.%s", (unsigned long long)h, png ? "png" : "jpg");
        std::string p = JoinPath(coverDir_, name);
        if (!FileExists(p)) {
            MakeDirs(coverDir_);
            if (FILE* f = OpenFile(p, "wb")) {
                fwrite(data, 1, (size_t)size, f);
                fclose(f);
            }
        }
        return p;
    }
    // a picture lying next to the music
    std::string dir = DirName(path);
    static const char* names[] = {"cover.jpg", "cover.png", "folder.jpg", "folder.png", "front.jpg", "front.png", "album.jpg", "albumart.jpg"};
    for (auto& e : ListDir(dir)) {
        if (e.isDir) continue;
        std::string n = Lower(e.name);
        for (auto* c : names)
            if (n == c) return JoinPath(dir, e.name);
    }
    return "";
}

void Library::ReadTags(Track& t) {
    LspTags tg{};
    struct Cov {
        std::vector<uint8_t> data;
    } cov;
    bool ok = mod_ && mod_->tags(t.path.c_str(), &tg, [](void* u, const uint8_t* d, int n) {
        ((Cov*)u)->data.assign(d, d + n);
    }, &cov) != 0;
    std::string base = BaseName(t.path);
    size_t dot = base.find_last_of('.');
    std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
    t.title = ok && tg.title[0] ? tg.title : stem;
    t.artist = ok ? tg.artist : "";
    t.album = ok && tg.album[0] ? tg.album : BaseName(DirName(t.path));
    t.albumArtist = ok ? tg.album_artist : "";
    t.genre = ok ? tg.genre : "";
    t.year = ok ? std::string(tg.date).substr(0, 4) : "";
    t.number = ok ? tg.track : 0;
    t.disc = ok ? tg.disc : 0;
    t.duration = ok ? tg.duration : 0;
    if (!ok) {  // "01 - Artist - Title": the number, at least
        int n = atoi(stem.c_str());
        if (n > 0 && n < 1000) t.number = n;
    }
    t.cover = CoverFor(t.path, cov.data.empty() ? nullptr : cov.data.data(), (int)cov.data.size());
}

void Library::ScanLoop() {
    for (;;) {
        std::vector<std::string> roots;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return quit_ || want_; });
            if (quit_) return;
            want_ = false;
            roots = folders_;
            for (auto& d : implicit_)
                if (std::find(roots.begin(), roots.end(), d) == roots.end()) roots.push_back(d);
        }
        scanning_ = true;
        scanDone_ = 0;
        // every audio file under the folders (not into hidden folders: .lunashare-partial and the like)
        std::map<std::string, std::pair<int64_t, int64_t>> found;  // path -> size, mtime
        std::vector<std::string> stack = roots;
        int dirs = 0;
        while (!stack.empty() && dirs < 20000) {
            std::string d = stack.back();
            stack.pop_back();
            dirs++;
            for (auto& e : ListDir(d)) {
                if (e.name.empty() || e.name[0] == '.') continue;
                std::string p = JoinPath(d, e.name);
                if (e.isDir) stack.push_back(p);
                else if (IsAudioPath(e.name) && e.size > 0) found[p] = {e.size, e.mtime};
            }
            if (quit_) return;
        }
        // drop what's gone, read what's new or changed
        std::vector<std::string> todo;
        {
            std::lock_guard<std::mutex> l(m_);
            for (auto it = tracks_.begin(); it != tracks_.end();) {
                if (!found.count(it->first)) {
                    it = tracks_.erase(it);
                    dirty_ = true;
                } else {
                    ++it;
                }
            }
            for (auto& [p, sm] : found) {
                auto it = tracks_.find(p);
                if (it == tracks_.end() || it->second.size != sm.first || it->second.mtime != sm.second) todo.push_back(p);
            }
            if (dirty_) version_++;
        }
        int64_t now = NowMs();
        for (size_t i = 0; i < todo.size(); i++) {
            Track t;
            t.path = todo[i];
            t.size = found[t.path].first;
            t.mtime = found[t.path].second;
            ReadTags(t);
            {
                std::lock_guard<std::mutex> l(m_);
                if (quit_) return;
                auto it = tracks_.find(t.path);
                t.added = it != tracks_.end() && it->second.added ? it->second.added : now;
                tracks_[t.path] = t;
                dirty_ = true;
                if (i % 25 == 24 || i + 1 == todo.size()) version_++;  // (the list shows them as they come)
            }
            scanDone_ = (int)i + 1;
        }
        scanning_ = false;
        version_++;
        Save();
    }
}

// ---------------------------------------------------------------- player

void Player::Init(const LspModule* mod, Library* lib) {
    mod_ = mod;
    lib_ = lib;
    if (!mod_) return;
    th_ = std::thread([this] { Loop(); });
}

void Player::Shutdown() {
    SaveState();
    quit_ = true;
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> l(m_);
    CloseLocked();
}

void Player::AudioCb(void* u, uint8_t* stream, int len) {
    auto* self = (Player*)u;
    float* out = (float*)stream;
    int frames = len / (int)(sizeof(float) * 2);
    {
        std::lock_guard<std::mutex> l(self->pm_);
        if (self->p_) self->mod_->read_audio(self->p_, out, frames);
        else memset(stream, 0, (size_t)len);
    }
    float v = self->volume_.load();
    if (v < 0.999f)
        for (int i = 0; i < frames * 2; i++) out[i] *= v;
}

void Player::CloseLocked() {
    LspPlayer* p;
    {
        std::lock_guard<std::mutex> l(pm_);
        p = p_;
        p_ = nullptr;
    }
    if (p) mod_->close(p);
    if (dev_) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
    playing_ = false;
}

bool Player::OpenAt(int index, double pos) {
    if (index < 0 || index >= (int)order_.size()) return false;
    at_ = index;
    const std::string& path = queue_[(size_t)order_[(size_t)index]];
    LspInfo info{};
    LspPlayer* p = mod_->open(path.c_str(), 48000, 2, &info);
    {
        std::lock_guard<std::mutex> l(pm_);
        LspPlayer* old = p_;
        p_ = p;
        if (old) mod_->close(old);
    }
    dur_ = info.duration;
    if (!p) return false;
    if (!dev_) {
        if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = 2048;
        want.callback = AudioCb;
        want.userdata = this;
        dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    }
    if (pos > 0) mod_->seek(p, pos);
    mod_->set_playing(p, wantPlay_ ? 1 : 0);
    if (dev_) SDL_PauseAudioDevice(dev_, wantPlay_ ? 0 : 1);
    playing_ = wantPlay_;
    return true;
}

void Player::Changed() {
    if (onChange) onChange();
}

void Player::PlayList(const std::vector<std::string>& paths, int start, bool shuffle) {
    if (!mod_ || paths.empty()) return;
    {
        std::lock_guard<std::mutex> l(m_);
        queue_ = paths;
        order_.resize(paths.size());
        for (size_t i = 0; i < paths.size(); i++) order_[i] = (int)i;
        start = std::clamp(start, 0, (int)paths.size() - 1);
        if (shuffle) shuffle_ = true;
        if (shuffle_) {  // the chosen one first, the rest shuffled
            std::mt19937 rng((uint32_t)SDL_GetPerformanceCounter());
            std::swap(order_[0], order_[(size_t)start]);
            std::shuffle(order_.begin() + 1, order_.end(), rng);
            if (shuffle && start == 0) std::shuffle(order_.begin(), order_.end(), rng);  // ("shuffle" button: any first)
            start = 0;
        }
        wantPlay_ = true;
        pausedByInterrupt_ = false;
        if (!OpenAt(start, 0)) {  // (a file that can't be played: the next one)
            for (int k = start + 1; k < (int)order_.size(); k++)
                if (OpenAt(k, 0)) break;
        }
    }
    Changed();
    SaveState();
}

void Player::PlayNext(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    {
        std::lock_guard<std::mutex> l(m_);
        if (order_.empty()) {
            queue_.clear();
            at_ = -1;
        }
        for (size_t i = 0; i < paths.size(); i++) {
            queue_.push_back(paths[i]);
            order_.insert(order_.begin() + std::min<size_t>(order_.size(), (size_t)(at_ + 1) + i), (int)queue_.size() - 1);
        }
    }
    if (!GetNow().active) {
        std::lock_guard<std::mutex> l(m_);
        wantPlay_ = true;
        OpenAt(0, 0);
    }
    Changed();
}

void Player::AddToQueue(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    bool start;
    {
        std::lock_guard<std::mutex> l(m_);
        start = order_.empty() || !p_;
        for (auto& p : paths) {
            queue_.push_back(p);
            order_.push_back((int)queue_.size() - 1);
        }
        if (start) {
            wantPlay_ = true;
            OpenAt(std::max(0, (int)order_.size() - (int)paths.size()), 0);
        }
    }
    Changed();
}

void Player::SetPlaying(bool on) {
    {
        std::lock_guard<std::mutex> l(m_);
        wantPlay_ = on;
        pausedByInterrupt_ = false;
        if (!p_ && on && at_ >= 0) OpenAt(at_, savedPos_);  // (remembered from the last run)
        if (p_) {
            mod_->set_playing(p_, on ? 1 : 0);
            playing_ = on;
            if (dev_) SDL_PauseAudioDevice(dev_, on ? 0 : 1);  // (paused: the device rests too - battery)
        }
    }
    Changed();
    SaveState();
}

void Player::Toggle() { SetPlaying(!GetNow().playing); }

void Player::Next() {
    {
        std::lock_guard<std::mutex> l(m_);
        if (order_.empty()) return;
        int n = at_ + 1;
        if (n >= (int)order_.size()) n = 0;
        wantPlay_ = wantPlay_ || playing_;
        OpenAt(n, 0);
    }
    Changed();
}

void Player::Prev() {
    {
        std::lock_guard<std::mutex> l(m_);
        if (order_.empty()) return;
        if (p_ && mod_->position(p_) > 3) {  // like every player: back to the start of this one first
            mod_->seek(p_, 0);
        } else {
            int n = at_ - 1;
            if (n < 0) n = repeat_ == RepeatAll ? (int)order_.size() - 1 : 0;
            OpenAt(n, 0);
        }
    }
    Changed();
}

void Player::Seek(double s) {
    std::lock_guard<std::mutex> l(m_);
    if (p_) mod_->seek(p_, std::max(0.0, s));
    else savedPos_ = std::max(0.0, s);
}

void Player::Stop() {
    {
        std::lock_guard<std::mutex> l(m_);
        CloseLocked();
        queue_.clear();
        order_.clear();
        at_ = -1;
        wantPlay_ = false;
    }
    Changed();
    SaveState();
}

void Player::SetRepeat(int r) {
    {
        std::lock_guard<std::mutex> l(m_);
        repeat_ = std::clamp(r, 0, 2);
    }
    Changed();
    SaveState();
}

void Player::SetShuffle(bool on) {
    {
        std::lock_guard<std::mutex> l(m_);
        if (on == shuffle_) return;
        shuffle_ = on;
        int cur = at_ >= 0 && at_ < (int)order_.size() ? order_[(size_t)at_] : -1;
        order_.resize(queue_.size());
        for (size_t i = 0; i < queue_.size(); i++) order_[i] = (int)i;
        if (on) {  // the current one stays first, the rest in random order
            std::mt19937 rng((uint32_t)SDL_GetPerformanceCounter());
            if (cur >= 0) std::swap(order_[0], order_[(size_t)cur]);
            std::shuffle(order_.begin() + (cur >= 0 ? 1 : 0), order_.end(), rng);
            at_ = cur >= 0 ? 0 : -1;
        } else {
            at_ = cur;
        }
    }
    Changed();
    SaveState();
}

void Player::SetVolume(float v) { volume_ = std::clamp(v, 0.0f, 1.0f); }

void Player::SetInterrupted(bool on) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> l(m_);
        if (on == interrupted_) return;
        interrupted_ = on;
        if (on && p_ && playing_) {
            mod_->set_playing(p_, 0);
            playing_ = false;
            pausedByInterrupt_ = true;
            if (dev_) SDL_PauseAudioDevice(dev_, 1);
            changed = true;
        } else if (!on && p_ && pausedByInterrupt_) {
            mod_->set_playing(p_, 1);
            playing_ = true;
            pausedByInterrupt_ = false;
            if (dev_) SDL_PauseAudioDevice(dev_, 0);
            changed = true;
        }
    }
    if (changed) Changed();
}

Now Player::GetNow() {
    Now n;
    std::string path;
    {
        std::lock_guard<std::mutex> l(m_);
        n.repeat = repeat_;
        n.shuffle = shuffle_;
        n.count = (int)order_.size();
        n.index = at_;
        if (at_ < 0 || at_ >= (int)order_.size()) return n;
        path = queue_[(size_t)order_[(size_t)at_]];
        n.active = true;
        n.playing = playing_ && p_;
        n.pos = p_ ? mod_->position(p_) : savedPos_;
        n.dur = dur_;
    }
    n.path = path;
    if (!lib_ || !lib_->Find(path, n.track)) {
        n.track.path = path;
        n.track.title = BaseName(path);
    }
    if (n.dur <= 0) n.dur = n.track.duration;
    return n;
}

std::vector<std::string> Player::Queue() {
    std::lock_guard<std::mutex> l(m_);
    std::vector<std::string> v;
    for (int i : order_) v.push_back(queue_[(size_t)i]);
    return v;
}

// the player's own thread: the next track when one ends (also while the app is in the background)
void Player::Loop() {
    int64_t lastChange = 0;
    while (!quit_) {
        SDL_Delay(100);
        bool changed = false;
        {
            std::lock_guard<std::mutex> l(m_);
            if (p_ && mod_->state(p_) == LSP_ENDED && playing_) {
                int n = at_ + 1;
                if (repeat_ == RepeatOne) n = at_;
                else if (n >= (int)order_.size()) n = repeat_ == RepeatAll ? 0 : -1;
                if (n >= 0) {
                    if (repeat_ == RepeatOne) {
                        mod_->seek(p_, 0);
                        mod_->set_playing(p_, 1);
                    } else {
                        OpenAt(n, 0);
                    }
                } else {  // the end of the list
                    playing_ = wantPlay_ = false;
                    mod_->seek(p_, 0);
                    mod_->set_playing(p_, 0);
                    if (dev_) SDL_PauseAudioDevice(dev_, 1);
                }
                changed = true;
            }
            if (p_) savedPos_ = mod_->position(p_);
        }
        int64_t now = (int64_t)SDL_GetTicks64();
        if (changed) {
            lastChange = now;
            Changed();
            SaveState();
        } else if (now - lastChange > 1000) {  // (the position, for the notification / lock screen)
            lastChange = now;
            if (GetNow().playing) Changed();
            if (now - lastSave_ > 15000) SaveState();
        }
    }
}

void Player::LoadState(const std::string& path) {
    statePath_ = path;
    json j;
    if (!LoadJsonFile(path, j) || !j.is_object()) return;
    std::lock_guard<std::mutex> l(m_);
    for (auto& q : j.value("queue", json::array()))
        if (q.is_string()) queue_.push_back(q.get<std::string>());
    for (auto& o : j.value("order", json::array()))
        if (o.is_number_integer() && o.get<int>() >= 0 && o.get<int>() < (int)queue_.size()) order_.push_back(o.get<int>());
    if (order_.size() != queue_.size()) {
        order_.resize(queue_.size());
        for (size_t i = 0; i < queue_.size(); i++) order_[i] = (int)i;
    }
    at_ = std::clamp(j.value("at", -1), -1, (int)order_.size() - 1);
    repeat_ = std::clamp(j.value("repeat", 0), 0, 2);
    shuffle_ = j.value("shuffle", false);
    savedPos_ = j.value("pos", 0.0);
}

void Player::SaveState() {
    if (statePath_.empty()) return;
    json j;
    {
        std::lock_guard<std::mutex> l(m_);
        j = {{"queue", queue_}, {"order", order_}, {"at", at_}, {"repeat", repeat_}, {"shuffle", shuffle_}, {"pos", savedPos_}};
        lastSave_ = (int64_t)SDL_GetTicks64();
    }
    SaveJsonFile(statePath_, j);
}

#ifdef _WIN32
static HWND g_keys = nullptr;
static LRESULT CALLBACK KeysProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_HOTKEY) {
        switch ((int)wp) {
            case 1: Play().Toggle(); break;
            case 2: Play().Next(); break;
            case 3: Play().Prev(); break;
            case 4: Play().SetPlaying(false); break;
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
void MediaKeys(bool on) {
    static bool now = false;
    if (on == now) return;
    now = on;
    if (!g_keys) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = KeysProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LunaShareMediaKeys";
        RegisterClassW(&wc);
        g_keys = CreateWindowW(L"LunaShareMediaKeys", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    }
    if (!g_keys) return;
    if (on) {
        RegisterHotKey(g_keys, 1, 0, VK_MEDIA_PLAY_PAUSE);
        RegisterHotKey(g_keys, 2, 0, VK_MEDIA_NEXT_TRACK);
        RegisterHotKey(g_keys, 3, 0, VK_MEDIA_PREV_TRACK);
        RegisterHotKey(g_keys, 4, 0, VK_MEDIA_STOP);
    } else {
        for (int i = 1; i <= 4; i++) UnregisterHotKey(g_keys, i);
    }
}
#else
void MediaKeys(bool) {}
#endif

}  // namespace music
