#include "player_host.h"

#include <SDL.h>

#include <algorithm>
#include <cstring>

#include "bluetooth.h"
#include "gl_funcs.h"
#include "media_host.h"
#include "platform.h"

using namespace ls;

PlayerHost& Player() {
    static PlayerHost p;
    return p;
}

static std::string Ext(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

bool PlayerHost::IsVideoPath(const std::string& path) {
    std::string e = Ext(path);
    return e == "mp4" || e == "m4v" || e == "mov" || e == "mkv" || e == "webm" || e == "avi" || e == "3gp";
}

bool PlayerHost::IsAudioPath(const std::string& path) {
    std::string e = Ext(path);
    return e == "mp3" || e == "m4a" || e == "aac" || e == "ogg" || e == "oga" || e == "opus" || e == "flac" || e == "wav" ||
           e == "weba";
}

bool PlayerHost::Load() {
    std::vector<std::string> candidates;
#if defined(__ANDROID__)
    candidates.push_back("liblunashare_player.so");
#endif
    if (char* base = SDL_GetBasePath()) {
        std::string b = base;
        SDL_free(base);
#if defined(_WIN32)
        const char* names[] = {"lunashare_player.dll", "liblunashare_player.dll"};
#elif defined(__APPLE__)
        const char* names[] = {"liblunashare_player.dylib"};
#else
        const char* names[] = {"liblunashare_player.so"};
#endif
        for (auto& d : {b, JoinPath(b, "modules")})
            for (auto* n : names) candidates.push_back(JoinPath(d, n));
    }
    for (auto& c : candidates) {
#if !defined(__ANDROID__)
        if (!FileExists(c)) continue;
#endif
        void* lib = SDL_LoadObject(c.c_str());
        if (!lib) continue;
        auto fn = (LspGetModuleFn)SDL_LoadFunction(lib, "lunashare_player_module");
        const LspModule* m = fn ? fn() : nullptr;
        if (!m || m->api_version != LSP_API_VERSION) {
            SDL_UnloadObject(lib);
            continue;
        }
        host_ = LspHost{LSP_API_VERSION, this, [](void*, const char* s) { SDL_Log("%s", s); }, nullptr, nullptr};
        if (Bridge().javaVm) host_.java_vm = Bridge().javaVm();
        if (Bridge().playerClass) host_.java_class = Bridge().playerClass();
        if (!m->init(&host_)) {
            loadInfo = c + ": decoders not available";
            SDL_UnloadObject(lib);
            continue;
        }
        lib_ = lib;
        mod_ = m;
        loadInfo = c;
        worker_ = std::thread([this] { Worker(); });
        return true;
    }
    if (loadInfo.empty()) loadInfo = "not installed";
    return false;
}

void PlayerHost::Shutdown() {
    Stop();
    {
        std::lock_guard<std::mutex> l(mm_);
        quit_ = true;
    }
    mcv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

// ---------------------------------------------------------------- thumbnails / info
namespace {
// small upright RGBA thumbnail from a YUV picture (nearest neighbour; it's ~320 px)
void Thumb(const LsvImage& im, std::vector<uint8_t>& out, int& ow, int& oh) {
    int rot = ((im.rotation % 360) + 360) % 360;
    bool swap = rot == 90 || rot == 270;
    int uw = swap ? im.height : im.width, uh = swap ? im.width : im.height;
    float f = std::min(1.0f, 360.0f / (float)std::max(uw, uh));
    ow = std::max(1, (int)(uw * f));
    oh = std::max(1, (int)(uh * f));
    out.resize((size_t)ow * oh * 4);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int ux = (int)((int64_t)x * uw / ow), uy = (int)((int64_t)y * uh / oh), sx, sy;
            switch (rot) {
                case 90: sx = uy; sy = im.height - 1 - ux; break;
                case 180: sx = im.width - 1 - ux; sy = im.height - 1 - uy; break;
                case 270: sx = im.width - 1 - uy; sy = ux; break;
                default: sx = ux; sy = uy; break;
            }
            int Y = im.y[(size_t)sy * im.y_stride + sx];
            size_t ci = (size_t)(sy >> 1) * im.uv_stride + (size_t)(sx >> 1) * im.uv_pixel_stride;
            int U = im.u[ci] - 128, V = im.v[ci] - 128, C = (Y - 16) * 298;
            int r = (C + 409 * V + 128) >> 8, g = (C - 100 * U - 208 * V + 128) >> 8, b = (C + 516 * U + 128) >> 8;
            uint8_t* d = &out[((size_t)y * ow + x) * 4];
            d[0] = (uint8_t)std::clamp(r, 0, 255);
            d[1] = (uint8_t)std::clamp(g, 0, 255);
            d[2] = (uint8_t)std::clamp(b, 0, 255);
            d[3] = 255;
        }
}
}  // namespace

void PlayerHost::Worker() {
    for (;;) {
        std::string path;
        {
            std::unique_lock<std::mutex> l(mm_);
            mcv_.wait(l, [&] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            path = queue_.front();
            queue_.pop_front();
        }
        Pending pd;
        pd.path = path;
        struct Ctx {
            Pending* pd;
        } ctx{&pd};
        pd.ok = mod_->probe(path.c_str(), &pd.info, [](void* u, const LsvImage* im) {
            auto* c = (Ctx*)u;
            Thumb(*im, c->pd->rgba, c->pd->w, c->pd->h);
        }, &ctx) != 0;
        std::lock_guard<std::mutex> l(mm_);
        done_.push_back(std::move(pd));
    }
}

const PlayerHost::Meta& PlayerHost::GetMeta(const std::string& path) {
    std::lock_guard<std::mutex> l(mm_);
    auto it = meta_.find(path);
    if (it != meta_.end()) return it->second;
    Meta& m = meta_[path];
    if (mod_) {
        queue_.push_back(path);
        mcv_.notify_one();
    } else {
        m.done = true;
    }
    return m;
}

void PlayerHost::Update() {
    if (!mod_) return;
    // finished probes -> thumbnails (GL on the UI thread)
    std::vector<Pending> done;
    {
        std::lock_guard<std::mutex> l(mm_);
        done.swap(done_);
    }
    for (auto& pd : done) {
        std::lock_guard<std::mutex> l(mm_);
        Meta& m = meta_[pd.path];
        m.done = true;
        m.ok = pd.ok;
        m.info = pd.info;
        if (!pd.rgba.empty()) {
            glf::GenTextures(1, &m.tex);
            glf::BindTexture(glf::TEXTURE_2D, m.tex);
            glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_MIN_FILTER, glf::LINEAR);
            glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_MAG_FILTER, glf::LINEAR);
            glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_WRAP_S, glf::CLAMP_TO_EDGE);
            glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_WRAP_T, glf::CLAMP_TO_EDGE);
            glf::PixelStorei(glf::UNPACK_ALIGNMENT, 4);
            glf::TexImage2D(glf::TEXTURE_2D, 0, glf::RGBA, pd.w, pd.h, 0, glf::RGBA, glf::UNSIGNED_BYTE, pd.rgba.data());
            m.tw = pd.w;
            m.th = pd.h;
        }
    }
    UpdateSubtitle();
    // current picture of the playing video
    if (p_ && info_.has_video)
        mod_->video_frame(p_, [](void*, const LsvImage* im) { Media().PushPlayerFrame(*im); }, nullptr);
    // finished: release the audio device (keeps the last picture)
    if (p_ && dev_ && mod_->state(p_) == LSP_ENDED) CloseAudio();
}

// ---------------------------------------------------------------- playback
void PlayerHost::AudioCb(void* u, uint8_t* stream, int len) {
    auto* self = (PlayerHost*)u;
    std::lock_guard<std::mutex> l(self->pm_);
    if (self->p_) self->mod_->read_audio(self->p_, (float*)stream, len / (int)(sizeof(float) * 2));
    else memset(stream, 0, (size_t)len);
}

bool PlayerHost::OpenAudio() {
    if (dev_) return true;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = AudioCb;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev_) return false;
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void PlayerHost::CloseAudio() {
    if (dev_) SDL_CloseAudioDevice(dev_);
    dev_ = 0;
}

bool PlayerHost::Play(const std::string& path) {
    if (!mod_) return false;
    if (cur_ != path) {
        Stop();
        LspInfo info{};
        LspPlayer* p = mod_->open(path.c_str(), 48000, 2, &info);
        if (!p) return false;
        {
            std::lock_guard<std::mutex> l(pm_);
            p_ = p;
        }
        cur_ = path;
        info_ = info;
        Media().ClearPlayerVideo();
    }
    // the clock runs on the audio device even for silent videos, so it stays open while playing
    if (!OpenAudio()) SDL_Log("player: no audio device");
    mod_->set_playing(p_, 1);
    return true;
}

void PlayerHost::OpenStream(const std::string& id, int64_t size, std::function<int(int64_t, uint8_t*, int)> read) {
    if (!mod_ || mod_->api_version < 2 || !mod_->open_stream) {
        openState_ = (int)Opening::Failed;
        return;
    }
    Stop();
    streamRead_ = std::move(read);
    stream_.user = this;
    stream_.size = size;
    stream_.read = [](void* u, int64_t off, uint8_t* buf, int len) { return ((PlayerHost*)u)->streamRead_(off, buf, len); };
    openingId_ = id;
    openState_ = (int)Opening::Busy;
    opener_ = std::thread([this] {
        LspInfo info{};
        LspPlayer* p = mod_->open_stream(&stream_, 48000, 2, &info);
        opened_ = p;
        openedInfo_ = info;
        openState_ = p ? (int)Opening::Ready : (int)Opening::Failed;
    });
}

PlayerHost::Opening PlayerHost::OpenState() {
    Opening s = (Opening)openState_.load();
    if (s != Opening::Busy && opener_.joinable()) {
        opener_.join();
        if (opened_) {  // opened: becomes the current player (paused)
            {
                std::lock_guard<std::mutex> l(pm_);
                p_ = opened_;
            }
            opened_ = nullptr;
            cur_ = openingId_;
            info_ = openedInfo_;
            Media().ClearPlayerVideo();
            if (!OpenAudio()) SDL_Log("player: no audio device");
        }
    }
    return s;
}

void PlayerHost::SetPlaying(bool on) {
    if (!p_) return;
    if (on && !OpenAudio()) SDL_Log("player: no audio device");
    mod_->set_playing(p_, on ? 1 : 0);
}

std::vector<LspTrack> PlayerHost::Tracks() const {
    std::vector<LspTrack> v;
    if (!p_ || mod_->api_version < 3 || !mod_->tracks) return v;
    v.resize(64);
    v.resize((size_t)std::max(0, mod_->tracks(p_, v.data(), (int)v.size())));
    return v;
}

void PlayerHost::SelectTrack(int kind, int id) {
    if (p_ && mod_->select_track) mod_->select_track(p_, kind, id);
}

int PlayerHost::AddSubtitles(const std::string& path) {
    if (!p_ || !mod_->add_subtitles) return -1;
    return mod_->add_subtitles(p_, path.c_str());
}

void PlayerHost::AddSidecarSubtitles(const std::string& videoPath) {
    std::string dir = DirName(videoPath), base = BaseName(videoPath);
    size_t dot = base.find_last_of('.');
    std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
    auto lower = [](std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; };
    std::vector<std::string> names;
    for (auto& e : ListDir(dir)) {
        if (e.isDir) continue;
        std::string n = lower(e.name);
        bool sub = n.size() > 4 && (n.rfind(".srt") == n.size() - 4 || n.rfind(".ass") == n.size() - 4 || n.rfind(".ssa") == n.size() - 4 ||
                                    n.rfind(".vtt") == n.size() - 4);
        if (sub && n.rfind(lower(stem), 0) == 0) names.push_back(e.name);  // movie.srt, movie.pl.srt, movie.English.srt
    }
    std::sort(names.begin(), names.end());
    for (auto& n : names) AddSubtitles(JoinPath(dir, n));
}

void PlayerHost::UpdateSubtitle() {
    if (!p_ || mod_->api_version < 3 || !mod_->subtitle) {
        if (subSerial_ != -1) {
            if (sub_.tex) glf::DeleteTextures(1, &sub_.tex);
            sub_ = Sub();
            subSerial_ = -1;
        }
        return;
    }
    LspSubtitle s{};
    int serial = mod_->subtitle(p_, &s);
    if (serial == subSerial_) return;
    subSerial_ = serial;
    if (sub_.tex) glf::DeleteTextures(1, &sub_.tex);
    sub_ = Sub();
    sub_.text = s.text ? s.text : "";
    if (s.rgba && s.w > 0 && s.h > 0) {
        glf::GenTextures(1, &sub_.tex);
        glf::BindTexture(glf::TEXTURE_2D, sub_.tex);
        glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_MIN_FILTER, glf::LINEAR);
        glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_MAG_FILTER, glf::LINEAR);
        glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_WRAP_S, glf::CLAMP_TO_EDGE);
        glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_WRAP_T, glf::CLAMP_TO_EDGE);
        glf::PixelStorei(glf::UNPACK_ALIGNMENT, 4);
        glf::TexImage2D(glf::TEXTURE_2D, 0, glf::RGBA, s.w, s.h, 0, glf::RGBA, glf::UNSIGNED_BYTE, s.rgba);
        sub_.w = s.w;
        sub_.h = s.h;
        sub_.x = s.x;
        sub_.y = s.y;
        sub_.sw = s.sw;
        sub_.sh = s.sh;
    }
}

void PlayerHost::Pause() {
    if (p_) mod_->set_playing(p_, 0);
}

void PlayerHost::Toggle(const std::string& path) {
    if (IsCurrent(path) && Playing()) Pause();
    else Play(path);
}

void PlayerHost::Seek(double t) {
    if (p_) mod_->seek(p_, t);
}

void PlayerHost::Stop() {
    if (opener_.joinable()) {  // (its reads have to return first: the caller cancels the stream before)
        opener_.join();
        if (opened_) mod_->close(opened_);
        opened_ = nullptr;
    }
    openState_ = (int)Opening::None;
    CloseAudio();
    LspPlayer* p;
    {
        std::lock_guard<std::mutex> l(pm_);
        p = p_;
        p_ = nullptr;
    }
    if (p) mod_->close(p);
    cur_.clear();
    info_ = LspInfo{};
    Media().ClearPlayerVideo();
}

bool PlayerHost::Playing() const { return p_ && mod_->state(p_) == LSP_PLAYING; }
double PlayerHost::Position() const { return p_ ? mod_->position(p_) : 0; }
int PlayerHost::State() const { return p_ ? mod_->state(p_) : LSP_PAUSED; }
