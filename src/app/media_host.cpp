#include "media_host.h"

#include <SDL.h>

#include <cmath>
#include <cstring>

static constexpr double kPi = 3.14159265358979323846;

#include "bluetooth.h"
#include "platform.h"

using namespace ls;

#if defined(LS_MEDIA_STATIC)
extern "C" const LsmModule* lunashare_media_module(void);
#endif

MediaHost& Media() {
    static MediaHost m;
    return m;
}

bool MediaHost::Load() {
#if defined(LS_MEDIA_STATIC)
    mod_ = lunashare_media_module();
    loadInfo = "built in";
#else
    std::vector<std::string> dirs;
    if (char* base = SDL_GetBasePath()) {
        std::string b = base;
        SDL_free(base);
        dirs.push_back(b);
        dirs.push_back(JoinPath(b, "modules"));
#ifdef __APPLE__
        dirs.push_back(JoinPath(b, "../MacOS"));
        dirs.push_back(JoinPath(b, "../Frameworks"));
#endif
    }
#if defined(_WIN32)
    const char* names[] = {"lunashare_media.dll", "liblunashare_media.dll"};
#elif defined(__APPLE__)
    const char* names[] = {"liblunashare_media.dylib", "lunashare_media.dylib"};
#else
    const char* names[] = {"liblunashare_media.so", "lunashare_media.so"};
#endif
    std::vector<std::string> candidates;
#if defined(__ANDROID__)
    candidates.push_back("liblunashare_media.so");  // packaged in the APK's lib folder (calls flavor only)
#endif
    for (auto& d : dirs)
        for (auto* n : names) candidates.push_back(JoinPath(d, n));
    for (auto& c : candidates) {
#if !defined(__ANDROID__)
        if (c.find('/') != std::string::npos || c.find('\\') != std::string::npos)
            if (!FileExists(c)) continue;
#endif
        void* lib = SDL_LoadObject(c.c_str());
        if (!lib) continue;
        auto fn = (LsmGetModuleFn)SDL_LoadFunction(lib, "lunashare_media_module");
        const LsmModule* m = fn ? fn() : nullptr;
        if (!m || m->api_version != LSM_API_VERSION) {
            loadInfo = c + ": incompatible module";
            SDL_UnloadObject(lib);
            continue;
        }
        lib_ = lib;
        mod_ = m;
        loadInfo = c;
        break;
    }
    if (!mod_ && loadInfo.empty()) loadInfo = "not installed";
#endif
    return mod_ != nullptr;
}

std::string MediaHost::ModuleName() const {
    return mod_ ? std::string(mod_->name) + " " + mod_->version : std::string();
}

std::vector<std::string> MediaHost::Caps() const {
    std::vector<std::string> out;
    if (!mod_) return out;
    std::string c = mod_->caps, cur;
    for (char ch : c + ",") {
        if (ch == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else if (ch != ' ') cur += ch;
    }
    return out;
}

void MediaHost::Attach(Engine* e) {
    eng_ = e;
    if (!mod_) return;
    e->localCaps = Caps();
    const LsmModule* m = mod_;
    e->onMedia = [this, m](uint64_t, const uint8_t* d, size_t n) {
        if (mode_ == Mode::Call) m->receive(d, n);
    };
}

std::vector<std::string> MediaHost::Devices(bool capture) {
    std::vector<std::string> out;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    int n = SDL_GetNumAudioDevices(capture ? 1 : 0);
    for (int i = 0; i < n; i++)
        if (const char* name = SDL_GetAudioDeviceName(i, capture ? 1 : 0)) out.push_back(name);
    return out;
}

void MediaHost::SendCb(void* u, const uint8_t* data, size_t len) {
    auto* self = (MediaHost*)u;
    if (self->eng_) self->eng_->SendMedia(data, len);
}

void MediaHost::LogCb(void*, const char* msg) { SDL_Log("media: %s", msg); }

void MediaHost::CaptureCb(void* u, uint8_t* stream, int len) {
    auto* self = (MediaHost*)u;
    int n = len / 2;
    auto* pcm = (const int16_t*)stream;
    if (self->mode_ == Mode::Call) {
        self->mod_->capture(pcm, n);
    } else if (self->mode_ == Mode::MicTest) {
        double s = 0;
        for (int i = 0; i < n; i++) s += (double)pcm[i] * pcm[i];
        double rms = std::sqrt(s / std::max(1, n)) / 32768.0;
        double db = rms > 1e-6 ? 20 * std::log10(rms) : -100;
        self->micTestLevel_ = (float)std::max(0.0, std::min(1.0, (db + 60) / 60));
    }
}

void MediaHost::PlaybackCb(void* u, uint8_t* stream, int len) {
    auto* self = (MediaHost*)u;
    auto* out = (int16_t*)stream;
    int n = len / 2;
    Mode m = self->mode_;
    if (m == Mode::Call) {
        self->mod_->playback(out, n);
        return;
    }
    // ring (incoming): two-tone 0.4 s bursts; ringback (outgoing): 425 Hz, 1 s on / 4 s off
    for (int i = 0; i < n; i++) {
        uint64_t t = self->toneSample_++;
        double sec = (double)t / LSM_SAMPLE_RATE, v = 0;
        if (m == Mode::Ring) {
            double cyc = std::fmod(sec, 3.0);
            if (cyc < 0.4 || (cyc > 0.6 && cyc < 1.0)) v = 0.25 * (std::sin(2 * kPi * 660 * sec) + std::sin(2 * kPi * 880 * sec));
        } else if (m == Mode::Ringback) {
            if (std::fmod(sec, 5.0) < 1.0) v = 0.18 * std::sin(2 * kPi * 425 * sec);
        }
        out[i] = (int16_t)(v * 32767);
    }
}

bool MediaHost::OpenDevices(bool capture, bool playback) {
    CloseDevices();
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        audioError = std::string("audio unavailable: ") + SDL_GetError();
        return false;
    }
    Settings s = eng_ ? eng_->GetSettings() : Settings();
    SDL_AudioSpec want{}, have{};
    want.freq = LSM_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 256;  // ~16 ms
    want.userdata = this;
    audioError.clear();
    if (playback) {
        want.callback = PlaybackCb;
        playDev_ = SDL_OpenAudioDevice(s.callOutputDevice.empty() ? nullptr : s.callOutputDevice.c_str(), 0, &want, &have, 0);
        if (!playDev_ && !s.callOutputDevice.empty()) playDev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!playDev_) audioError = std::string("speaker: ") + SDL_GetError();
    }
    if (capture) {
#if defined(__ANDROID__)
        SDL_AndroidRequestPermission("android.permission.RECORD_AUDIO");
#endif
        want.callback = CaptureCb;
        capDev_ = SDL_OpenAudioDevice(s.callInputDevice.empty() ? nullptr : s.callInputDevice.c_str(), 1, &want, &have, 0);
        if (!capDev_ && !s.callInputDevice.empty()) capDev_ = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
        if (!capDev_) audioError += std::string(audioError.empty() ? "" : "; ") + "microphone: " + SDL_GetError();
    }
    if (playDev_) SDL_PauseAudioDevice(playDev_, 0);
    if (capDev_) SDL_PauseAudioDevice(capDev_, 0);
    return (!playback || playDev_) && (!capture || capDev_);
}

void MediaHost::CloseDevices() {
    if (capDev_) SDL_CloseAudioDevice(capDev_);
    if (playDev_) SDL_CloseAudioDevice(playDev_);
    capDev_ = playDev_ = 0;
}

void MediaHost::SetMode(Mode m) {
    Mode old = mode_;
    if (old == m) return;
    CloseDevices();  // stops the audio callbacks first
    if (old == Mode::Call && mod_) mod_->stop();
    mode_ = Mode::Off;
    toneSample_ = 0;
    if (Bridge().callAudioMode) Bridge().callAudioMode(m == Mode::Call || m == Mode::Ringback);
    if (m == Mode::Call && mod_) {
        Settings s = eng_->GetSettings();
        LsmHost h{LSM_API_VERSION, this, SendCb, LogCb};
        LsmSettings ls{s.callEchoCancel ? 1 : 0, s.callNoiseSuppress ? 1 : 0, s.callAutoGain ? 1 : 0, s.callBitrate};
        mod_->start(&h, &ls);
        mod_->set_mute(mute_ ? 1 : 0);
        mode_ = Mode::Call;
        OpenDevices(true, true);
    } else if (m == Mode::Ring || m == Mode::Ringback) {
        mode_ = m;
        OpenDevices(false, true);
    } else if (m == Mode::MicTest) {
        mode_ = m;
        OpenDevices(true, false);
    }
}

void MediaHost::Update() {
    if (!eng_) return;
    CallState st;
    {
        auto lk = eng_->Lock();
        st = eng_->call.state;
    }
    Mode want = Mode::Off;
    if (st == CallState::Active && mod_) want = Mode::Call;
    else if (st == CallState::IncomingRinging) want = Mode::Ring;
    else if (st == CallState::OutgoingRinging) want = Mode::Ringback;
    else if (micTest_) want = Mode::MicTest;
    if (want != Mode::Call && mode_ == Mode::Call) mute_ = false;
    SetMode(want);
}

void MediaHost::Shutdown() {
    SetMode(Mode::Off);
    if (eng_) eng_->onMedia = nullptr;
}

void MediaHost::SetMute(bool m) {
    mute_ = m;
    if (mod_) mod_->set_mute(m ? 1 : 0);
}

void MediaHost::SetSpeaker(bool on) {
    speaker_ = on;
    if (Bridge().setSpeakerphone) Bridge().setSpeakerphone(on);
}

LsmStats MediaHost::Stats() {
    LsmStats s{};
    if (mod_ && mode_ == Mode::Call) mod_->get_stats(&s);
    return s;
}

void MediaHost::StartMicTest() { micTest_ = true; }
void MediaHost::StopMicTest() {
    micTest_ = false;
    micTestLevel_ = 0;
}
