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
#include <cmath>
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

// ---------------------------------------------------------------- sound

const float kEqFreq[kEqBands] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
const EqPreset kEqPresets[] = {
    {"Flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Bass boost", {7, 6, 4.5f, 2.5f, 0.5f, 0, 0, 0, 0, 0}},
    {"Treble boost", {0, 0, 0, 0, 0, 0.5f, 2, 4, 5.5f, 6.5f}},
    {"Vocal", {-2, -2, -1, 1, 3, 4, 3.5f, 2, 0, -1}},
    {"Rock", {5, 4, 2.5f, -0.5f, -1.5f, -0.5f, 2, 3.5f, 4.5f, 5}},
    {"Pop", {-1, 1, 3, 4, 3, 0.5f, -1, -1, -0.5f, 0}},
    {"Jazz", {3.5f, 2.5f, 1, 2, -1.5f, -1.5f, 0, 1.5f, 3, 3.5f}},
    {"Classical", {4, 3, 2.5f, 1.5f, -1, -1, 0, 2, 3, 3.5f}},
    {"Electronic", {5, 4.5f, 1.5f, 0, -2, 1.5f, 0.5f, 1.5f, 4.5f, 5}},
    {"Loudness", {6, 4, 1, 0, -1, -1, 0, 1, 4, 6}},
};
const int kEqPresetCount = (int)(sizeof(kEqPresets) / sizeof(kEqPresets[0]));

// ---------------------------------------------------------------- player

// a song from elsewhere: the decoder reads through this, asking again while the bytes are on their way
struct Player::ExtStream {
    LspStream s{};
    std::function<int(int64_t, uint8_t*, int)> read;
    std::atomic<bool> stop{false};
    std::atomic<bool>* quit = nullptr;
    static int Read(void* u, int64_t off, uint8_t* buf, int len) {
        auto* x = (ExtStream*)u;
        for (;;) {
            if (x->stop || *x->quit) return -1;
            int r = x->read(off, buf, len);
            if (r != -2) return r;
        }
    }
};

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
    if (exStream_) exStream_->stop = true;
    CloseLocked();
    exStream_.reset();
}

bool Player::EnsureDevice() {
    if (dev_) return true;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 2048;
    want.callback = AudioCb;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    return dev_ != 0;
}

// ---------------------------------------------------------------- songs from elsewhere

void Player::PlayExternal(const Source& s, double pos, bool playing) {
    {
        std::lock_guard<std::mutex> l(m_);
        if (!ext_) {  // (my own music rests meanwhile, where it was)
            CancelFade();
            CloseLocked();
        }
        ext_ = true;
        if (s.id != ex_.id) {
            if (exPending_) exPending_->stop = true;  // (one still being opened: given up)
            bool blend = s.fade > 0 && p_ && exOpen_ == ex_.id && !pn_;
            if (!blend && exStream_) exStream_->stop = true;  // (the old song stops reading at once - or plays on, fading out)
            exEnded_ = false;
            if (!blend) exOpen_ = 0;
        }
        ex_ = s;
        exPos_ = pos;
        exPlay_ = playing;
        exAt_ = (int64_t)SDL_GetTicks64();
        if (!s.follow) wantPlay_ = playing;
    }
    Changed();
}

void Player::Follow(double pos, bool playing) {
    std::lock_guard<std::mutex> l(m_);
    exPos_ = pos;
    exPlay_ = playing;
    exAt_ = (int64_t)SDL_GetTicks64();
}

void Player::EndExternal() {
    {
        std::lock_guard<std::mutex> l(m_);
        if (!ext_) return;
        if (exPending_) exPending_->stop = true;
        if (exStream_) exStream_->stop = true;
        if (exStreamOld_) exStreamOld_->stop = true;
        CloseLocked();
        exStream_.reset();
        exStreamOld_.reset();
        exPending_.reset();
        ext_ = false;
        ex_ = Source();
        exOpen_ = 0;
        wantPlay_ = false;
    }
    Changed();
}

bool Player::ExternalEnded(uint64_t id) {
    std::lock_guard<std::mutex> l(m_);
    return ext_ && ex_.id == id && exOpen_ == id && exEnded_;
}

bool Player::ExtCtl(int cmd, double arg) {
    {
        std::lock_guard<std::mutex> l(m_);
        if (!ext_) return false;
    }
    return externalCtl && externalCtl(cmd, arg);
}

// (m_ held) the song from elsewhere that's open: follow the host's clock, notice its end
void Player::LoopExternal() {
    if (!p_ || exOpen_ != ex_.id) return;
    LspPlayer* cur = pn_ ? pn_ : p_;  // (blending: the new song is the one followed)
    int64_t now = (int64_t)SDL_GetTicks64();
    if (ex_.follow) {
        double target = exPos_ + (exPlay_ ? (double)(now - exAt_) / 1000.0 : 0.0);
        bool on = exPlay_ && target >= 0 && !interrupted_ && !exEnded_;
        if (on != playing_) {
            if (pn_) mod_->set_playing(pn_, on ? 1 : 0);
            mod_->set_playing(p_, on ? 1 : 0);
            playing_ = on;
            if (dev_) SDL_PauseAudioDevice(dev_, on ? 0 : 1);
        }
        if (target >= 0 && (dur_ <= 0 || target < dur_ - 1) && now - exLastSeek_ > 3000) {
            double at = mod_->position(cur);
            if (std::fabs(at - target) > (exPlay_ ? 2.0 : 0.5)) {  // (more than a moment off: a jump)
                mod_->seek(cur, target);
                exLastSeek_ = now;
                exEnded_ = false;
            }
        }
    }
    if (mod_->state(cur) == LSP_ENDED && !exEnded_) {
        exEnded_ = true;
        if (!ex_.follow) {
            playing_ = wantPlay_ = false;
            if (dev_) SDL_PauseAudioDevice(dev_, 1);
        }
        Changed();
    }
}

void Player::AudioCb(void* u, uint8_t* stream, int len) {
    auto* self = (Player*)u;
    float* out = (float*)stream;
    int frames = len / (int)(sizeof(float) * 2);
    std::lock_guard<std::mutex> l(self->pm_);
    if (self->p_) self->mod_->read_audio(self->p_, out, frames);
    else memset(stream, 0, (size_t)len);
    // a transition: the next song comes in while this one goes (equal power, so the loudness stays even)
    if (self->pn_) {
        auto& b = self->mixBuf_;
        if ((int)b.size() < frames * 2) b.resize((size_t)frames * 2);
        self->mod_->read_audio(self->pn_, b.data(), frames);
        double total = (double)std::max<int64_t>(1, self->fadeTotal_);
        for (int i = 0; i < frames; i++) {
            double t = std::min(1.0, (double)(self->fadePos_ + i) / total);
            float ga = (float)std::cos(t * 1.5707963), gb = (float)std::sin(t * 1.5707963);
            out[i * 2] = out[i * 2] * ga + b[(size_t)i * 2] * gb;
            out[i * 2 + 1] = out[i * 2 + 1] * ga + b[(size_t)i * 2 + 1] * gb;
        }
        self->fadePos_ += frames;
        if (self->fadePos_ >= self->fadeTotal_) self->fadeDone_ = true;
    }
    // equalizer: ten peaking filters (RBJ biquads, transposed direct form II), then a soft limit
    if (self->eqActive_) {
        float g = self->eqGain_;
        for (int i = 0; i < frames; i++)
            for (int ch = 0; ch < 2; ch++) {
                float x = out[i * 2 + ch] * g;
                for (int k = 0; k < kEqBands; k++) {
                    const float* c = self->eqC_[k];
                    float* z = self->eqZ_[k][ch];
                    float y = c[0] * x + z[0];
                    z[0] = c[1] * x - c[3] * y + z[1];
                    z[1] = c[2] * x - c[4] * y;
                    x = y;
                }
                // (above about -1 dBFS: bent smoothly instead of clipping)
                float a = std::fabs(x);
                if (a > 0.9f) x = (x > 0 ? 1 : -1) * (0.9f + 0.1f * std::tanh((a - 0.9f) * 10.0f));
                out[i * 2 + ch] = x;
            }
    }
    float v = self->volume_.load();
    if (v < 0.999f)
        for (int i = 0; i < frames * 2; i++) out[i] *= v;
}

void Player::UpdateEq() {
    std::lock_guard<std::mutex> l(pm_);
    eqActive_ = snd_.eqOn;
    if (!eqActive_) return;
    eqGain_ = std::pow(10.0f, snd_.preamp / 20.0f);
    const double fs = 48000, q = 1.2;
    for (int k = 0; k < kEqBands; k++) {
        double A = std::pow(10.0, snd_.eq[k] / 40.0), w0 = 2 * 3.14159265358979 * kEqFreq[k] / fs;
        double alpha = std::sin(w0) / (2 * q), cw = std::cos(w0);
        double b0 = 1 + alpha * A, b1 = -2 * cw, b2 = 1 - alpha * A, a0 = 1 + alpha / A, a1 = -2 * cw, a2 = 1 - alpha / A;
        eqC_[k][0] = (float)(b0 / a0);
        eqC_[k][1] = (float)(b1 / a0);
        eqC_[k][2] = (float)(b2 / a0);
        eqC_[k][3] = (float)(a1 / a0);
        eqC_[k][4] = (float)(a2 / a0);
    }
}

Sound Player::GetSound() {
    std::lock_guard<std::mutex> l(m_);
    return snd_;
}

void Player::SetSound(const Sound& s) {
    {
        std::lock_guard<std::mutex> l(m_);
        bool fadeChanged = s.remix != snd_.remix;
        snd_ = s;
        snd_.crossfade = std::clamp(snd_.crossfade, 0, 12);
        snd_.remixLen = std::clamp(snd_.remixLen, 15, 180);
        snd_.remixFade = std::clamp(snd_.remixFade, 1, 12);
        UpdateEq();
        if (fadeChanged) CancelFade();
    }
    Changed();  // (saved by the caller when done - sliders change it every frame)
}

// remix: which part of a song plays - a random place, but always past the intro and with the whole part fitting
// before the outro (and the quiet at the end): at least 8 s / 8 % from the start, 12 s / 10 % from the end
static double FragHead(double dur) { return std::max(8.0, dur * 0.08); }
static double FragTail(double dur) { return std::max(12.0, dur * 0.10); }
double Player::PickFrag(double dur) const {
    if (!snd_.remix || dur <= 0) return 0;
    double lo = FragHead(dur), hi = dur - FragTail(dur) - snd_.remixLen;
    if (hi <= lo) return std::min(lo, dur * 0.2);  // (a short song: its middle)
    static std::mt19937 rng((uint32_t)SDL_GetPerformanceCounter());
    return std::uniform_real_distribution<double>(lo, hi)(rng);
}
double Player::FragStart(double dur) const { return snd_.remix && dur > 0 ? fragA_ : 0; }
double Player::FragEnd(double dur) const {
    if (!snd_.remix || dur <= 0) return dur;
    double end = std::min(dur - FragTail(dur), fragA_ + snd_.remixLen);
    return std::max(end, std::min(dur, fragA_ + 5));
}

// the next song in playing order, by the repeat mode; -1 = the list ended
int Player::NextIndex() {
    if (order_.empty()) return -1;
    if (repeat_ == RepeatOne) return at_;
    int n = at_ + 1;
    if (n >= (int)order_.size()) n = repeat_ == RepeatAll ? 0 : -1;
    return n;
}

void Player::StartFade(int next, double seconds) {
    const std::string& path = queue_[(size_t)order_[(size_t)next]];
    LspInfo info{};
    LspPlayer* q = mod_->open(path.c_str(), 48000, 2, &info);
    if (!q) {  // (can't be played: skipped - the plain end-of-song handling goes on to the next)
        nextAt_ = -1;
        return;
    }
    fragB_ = PickFrag(info.duration);
    if (fragB_ > 0) mod_->seek(q, fragB_);
    mod_->set_playing(q, playing_ ? 1 : 0);
    std::lock_guard<std::mutex> l(pm_);
    pn_ = q;
    nextAt_ = next;
    nextDur_ = info.duration;
    fadeTotal_ = std::max<int64_t>(1, (int64_t)(seconds * 48000));
    fadePos_ = 0;
    fadeDone_ = false;
}

void Player::FinishFade() {
    LspPlayer* old;
    {
        std::lock_guard<std::mutex> l(pm_);
        if (!pn_) return;
        old = p_;
        p_ = pn_;
        pn_ = nullptr;
        fadeDone_ = false;
    }
    if (old) mod_->close(old);
    at_ = nextAt_;
    dur_ = nextDur_;
    fragA_ = fragB_;
    nextAt_ = -1;
}

void Player::CancelFade() {
    LspPlayer* q;
    {
        std::lock_guard<std::mutex> l(pm_);
        q = pn_;
        pn_ = nullptr;
        fadeDone_ = false;
    }
    if (q) mod_->close(q);
    nextAt_ = -1;
}


void Player::CloseLocked() {
    CancelFade();
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
    CancelFade();
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
    fragA_ = pos > 0 ? pos : PickFrag(info.duration);  // (remix: a part of it; resumed: from where it was)
    if (pos <= 0) pos = fragA_;
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
        if (ext_) {  // (my own music again)
            if (exStream_) exStream_->stop = true;
            if (exStreamOld_) exStreamOld_->stop = true;
            CloseLocked();
            exStream_.reset();
            exStreamOld_.reset();
            ext_ = false;
            ex_ = Source();
            exOpen_ = 0;
        }
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
    if (ExtCtl(on ? 5 : 4, 0)) return;
    {
        std::lock_guard<std::mutex> l(m_);
        wantPlay_ = on;
        pausedByInterrupt_ = false;
        if (!p_ && on && at_ >= 0) OpenAt(at_, savedPos_);  // (remembered from the last run)
        if (pn_) mod_->set_playing(pn_, on ? 1 : 0);
        if (p_) {
            mod_->set_playing(p_, on ? 1 : 0);
            playing_ = on;
            if (dev_) SDL_PauseAudioDevice(dev_, on ? 0 : 1);  // (paused: the device rests too - battery)
        }
    }
    Changed();
    SaveState();
}

void Player::Toggle() {
    if (ExtCtl(1, 0)) return;
    SetPlaying(!GetNow().playing);
}

void Player::Next() {
    if (ExtCtl(2, 0)) return;
    {
        std::lock_guard<std::mutex> l(m_);
        if (ext_) return;
    }
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
    if (ExtCtl(3, 0)) return;
    {
        std::lock_guard<std::mutex> l(m_);
        if (ext_) {
            if (p_) mod_->seek(p_, 0);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> l(m_);
        if (order_.empty()) return;
        if (p_ && mod_->position(p_) > FragStart(dur_) + 3) {  // like every player: back to the start of this one first
            CancelFade();
            mod_->seek(p_, FragStart(dur_));
        } else {
            int n = at_ - 1;
            if (n < 0) n = repeat_ == RepeatAll ? (int)order_.size() - 1 : 0;
            OpenAt(n, 0);
        }
    }
    Changed();
}

void Player::Seek(double s) {
    if (ExtCtl(7, s)) return;
    std::lock_guard<std::mutex> l(m_);
    CancelFade();
    if (p_) mod_->seek(p_, std::max(0.0, s));
    else savedPos_ = std::max(0.0, s);
}

void Player::Stop() {
    if (ExtCtl(6, 0)) return;
    {
        std::lock_guard<std::mutex> l(m_);
        if (ext_) {
            if (exStream_) exStream_->stop = true;
            if (exStreamOld_) exStreamOld_->stop = true;
            CloseLocked();
            exStream_.reset();
            exStreamOld_.reset();
            ext_ = false;
            ex_ = Source();
            exOpen_ = 0;
        }
    }
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
            if (pn_) mod_->set_playing(pn_, 0);
            mod_->set_playing(p_, 0);
            playing_ = false;
            pausedByInterrupt_ = true;
            if (dev_) SDL_PauseAudioDevice(dev_, 1);
            changed = true;
        } else if (!on && p_ && pausedByInterrupt_) {
            if (pn_) mod_->set_playing(pn_, 1);
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
        if (ext_) {
            n.active = n.external = true;
            n.follow = ex_.follow;
            n.extId = ex_.id;
            n.track = ex_.track;
            n.path = ex_.path;
            bool open = p_ && exOpen_ == ex_.id;
            n.loading = !open;
            n.playing = open && playing_;
            n.pos = open ? mod_->position(pn_ ? pn_ : p_) : std::max(0.0, exPos_);
            n.dur = open && dur_ > 0 ? dur_ : ex_.track.duration;
            n.index = ex_.index;
            n.count = ex_.count;
            return n;
        }
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
        // a song from elsewhere to open (without the lock: its first bytes may take a moment to come)
        Source src;
        std::shared_ptr<ExtStream> st;
        bool open = false;
        {
            std::lock_guard<std::mutex> l(m_);
            if (ext_ && exOpen_ != ex_.id && !exOpening_ && ex_.id) {
                exOpening_ = open = true;
                src = ex_;
                if (src.path.empty()) {
                    st = std::make_shared<ExtStream>();
                    st->read = src.read;
                    st->quit = &quit_;
                    st->s.user = st.get();
                    st->s.size = src.size;
                    st->s.read = ExtStream::Read;
                    exPending_ = st;
                }
            }
        }
        if (open) {
            LspInfo info{};
            LspPlayer* q = nullptr;
            if (!src.path.empty()) q = mod_->open(src.path.c_str(), 48000, 2, &info);
            else if (src.read && mod_->open_stream) q = mod_->open_stream(&st->s, 48000, 2, &info);
            std::lock_guard<std::mutex> l(m_);
            exOpening_ = false;
            if (exPending_ == st) exPending_.reset();
            bool blend = ext_ && ex_.id == src.id && q && src.fade > 0 && p_ && !pn_;
            if (blend) {  // the new song comes in while the one that's on fades out (the audio callback mixes them)
                dur_ = info.duration > 0 ? info.duration : src.track.duration;
                double target = exPos_ + (exPlay_ ? (double)((int64_t)SDL_GetTicks64() - exAt_) / 1000.0 : 0.0);
                if (target > 0.5) mod_->seek(q, target);
                mod_->set_playing(q, playing_ ? 1 : 0);
                {
                    std::lock_guard<std::mutex> g(pm_);
                    pn_ = q;
                    fadeTotal_ = std::max<int64_t>(1, (int64_t)(src.fade * 48000));
                    fadePos_ = 0;
                    fadeDone_ = false;
                }
                exStreamOld_ = exStream_;
                exStream_ = st;
                exOpen_ = src.id;
                exEnded_ = false;
                exLastSeek_ = (int64_t)SDL_GetTicks64();
                changed = true;
            } else if (ext_ && ex_.id == src.id && q) {
                LspPlayer* old;
                if (pn_) CancelFade();
                {
                    std::lock_guard<std::mutex> g(pm_);
                    old = p_;
                    p_ = q;
                }
                if (exStream_) exStream_->stop = true;
                if (exStreamOld_) exStreamOld_->stop = true;
                if (old) mod_->close(old);
                exStreamOld_.reset();
                exStream_ = st;
                exOpen_ = src.id;
                exEnded_ = false;
                dur_ = info.duration > 0 ? info.duration : src.track.duration;
                EnsureDevice();
                bool on;
                if (src.follow) {
                    double target = exPos_ + (exPlay_ ? (double)((int64_t)SDL_GetTicks64() - exAt_) / 1000.0 : 0.0);
                    if (target > 0.5) mod_->seek(q, target);
                    on = exPlay_ && target >= 0 && !interrupted_;
                    exLastSeek_ = (int64_t)SDL_GetTicks64();
                } else {
                    if (exPos_ > 0) mod_->seek(q, exPos_);
                    on = wantPlay_ && !interrupted_;
                }
                mod_->set_playing(q, on ? 1 : 0);
                playing_ = on;
                if (dev_) SDL_PauseAudioDevice(dev_, on ? 0 : 1);
                changed = true;
            } else {
                if (q) mod_->close(q);
                if (ext_ && ex_.id == src.id && !q) {  // (can't be played: counts as played out)
                    exOpen_ = src.id;
                    exEnded_ = true;
                    changed = true;
                }
            }
        }
        {
            std::lock_guard<std::mutex> l(m_);
            if (ext_ && fadeDone_) {  // the blend is over: the new song is the one that's on
                LspPlayer* old;
                {
                    std::lock_guard<std::mutex> g(pm_);
                    old = p_;
                    p_ = pn_;
                    pn_ = nullptr;
                    fadeDone_ = false;
                }
                if (exStreamOld_) exStreamOld_->stop = true;
                if (old) mod_->close(old);
                exStreamOld_.reset();
            }
            if (ext_) LoopExternal();
        }
        {
            std::lock_guard<std::mutex> l(m_);
            if (ext_) goto tail;
            if (fadeDone_) {  // the next song has fully taken over
                FinishFade();
                changed = true;
            }
            // smooth transitions / remix: start the next one before this one ends
            double fade = snd_.remix ? snd_.remixFade : snd_.crossfade;
            if (p_ && playing_ && !pn_ && dur_ > 0 && fade > 0) {
                double pos = mod_->position(p_), end = FragEnd(dur_);
                if (pos >= end - fade && pos < dur_ - 0.05) {
                    int n = stopAfter_ ? -1 : NextIndex();  // (the sleep timer: this song is the last)
                    if (n >= 0) StartFade(n, fade);
                    else if (snd_.remix && pos >= end) {  // the list ended: stop at the end of the part
                        stopAfter_ = false;
                        playing_ = wantPlay_ = false;
                        mod_->set_playing(p_, 0);
                        if (dev_) SDL_PauseAudioDevice(dev_, 1);
                        changed = true;
                    }
                }
            }
            if (p_ && !pn_ && mod_->state(p_) == LSP_ENDED && playing_ && stopAfter_) {  // the sleep timer: done
                stopAfter_ = false;
                playing_ = wantPlay_ = false;
                if (dev_) SDL_PauseAudioDevice(dev_, 1);
                changed = true;
            } else if (p_ && !pn_ && mod_->state(p_) == LSP_ENDED && playing_) {
                int n = at_ + 1;
                if (repeat_ == RepeatOne) n = at_;
                else if (n >= (int)order_.size()) n = repeat_ == RepeatAll ? 0 : -1;
                if (n >= 0) {
                    if (repeat_ == RepeatOne) {
                        fragA_ = PickFrag(dur_);
                        mod_->seek(p_, fragA_);
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
    tail:
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
    json s = j.value("sound", json::object());
    if (s.is_object()) {
        snd_.eqOn = s.value("eqOn", false);
        json eq = s.value("eq", json::array());
        for (int k = 0; k < kEqBands && k < (int)eq.size(); k++)
            if (eq[(size_t)k].is_number()) snd_.eq[k] = std::clamp(eq[(size_t)k].get<float>(), -12.0f, 12.0f);
        snd_.preamp = std::clamp(s.value("preamp", 0.0f), -12.0f, 6.0f);
        snd_.preset = s.value("preset", "Flat");
        snd_.crossfade = std::clamp(s.value("crossfade", 0), 0, 12);
        snd_.remix = s.value("remix", false);
        snd_.remixLen = std::clamp(s.value("remixLen", 45), 15, 180);
        snd_.remixFade = std::clamp(s.value("remixFade", 6), 1, 12);
    }
    UpdateEq();
}

void Player::SaveState() {
    if (statePath_.empty()) return;
    json j;
    {
        std::lock_guard<std::mutex> l(m_);
        json eq = json::array();
        for (float g : snd_.eq) eq.push_back(g);
        j = {{"queue", queue_}, {"order", order_}, {"at", at_}, {"repeat", repeat_}, {"shuffle", shuffle_}, {"pos", savedPos_},
             {"sound", {{"eqOn", snd_.eqOn}, {"eq", eq}, {"preamp", snd_.preamp}, {"preset", snd_.preset}, {"crossfade", snd_.crossfade},
                        {"remix", snd_.remix}, {"remixLen", snd_.remixLen}, {"remixFade", snd_.remixFade}}}};
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
