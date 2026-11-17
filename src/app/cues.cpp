#include "cues.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "audio_out.h"

namespace cues {
namespace {
constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

struct Voice {
    const std::vector<float>* pcm;
    double pos;  // in kRate samples
    float gain;
};
std::mutex g_m;
std::vector<Voice> g_voices;
SDL_AudioDeviceID g_dev = 0;
uint64_t g_lastSound = 0;  // SDL ticks: the last time a voice played (its own output closes a while after)
uint64_t g_lastPlay[4] = {};

// a tone that glides from f0 to f1, with a soft start and a decay - the parts every cue is made of
void Tone(std::vector<float>& out, double at, double len, double f0, double f1, double vol, double decay) {
    size_t start = (size_t)(at * kRate), n = (size_t)(len * kRate);
    if (out.size() < start + n) out.resize(start + n, 0.0f);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        double t = (double)i / kRate, x = (double)i / (double)n;
        double f = f0 + (f1 - f0) * std::min(1.0, x * 2.5);  // (the glide: in the first part of the tone)
        ph += 2 * kPi * f / kRate;
        double env = std::min(1.0, t / 0.004) * std::exp(-t * decay);  // 4 ms in, then dying away
        env *= std::min(1.0, (double)(n - i) / (0.006 * kRate));       // (and no click at the end)
        // a little of the octave above: rounder, like a soft bell, not a test tone
        out[start + i] += (float)(vol * env * (std::sin(ph) + 0.18 * std::sin(2 * ph)));
    }
}

const std::vector<float>& Pcm(Cue c) {
    static std::vector<float> sent, recv, typing;
    static std::once_flag once;
    std::call_once(once, [] {
        Tone(sent, 0.0, 0.11, 620, 980, 0.30, 26);  // a quick rising blip
        Tone(recv, 0.0, 0.09, 880, 1175, 0.34, 30);  // "pop-
        Tone(recv, 0.085, 0.22, 1318, 1318, 0.30, 15);  // ding"
        Tone(typing, 0.0, 0.035, 520, 470, 0.22, 60);  // two soft taps, like a pen on a table
        Tone(typing, 0.11, 0.035, 560, 500, 0.18, 60);
    });
    switch (c) {
        case Cue::Sent: return sent;
        case Cue::Typing: return typing;
        default: return recv;
    }
}

// what plays now, added to a buffer (lock held)
template <class Out>
void Render(int frames, int rate, Out put) {
    double step = (double)kRate / rate;
    for (auto& v : g_voices) {
        for (int i = 0; i < frames; i++) {
            size_t k = (size_t)v.pos;
            if (k >= v.pcm->size()) break;
            put(i, (*v.pcm)[k] * v.gain);
            v.pos += step;
        }
    }
    for (size_t i = 0; i < g_voices.size();)
        if ((size_t)g_voices[i].pos >= g_voices[i].pcm->size()) g_voices.erase(g_voices.begin() + (long)i);
        else i++;
    if (!g_voices.empty()) g_lastSound = SDL_GetTicks64();
}

void OwnCb(void*, uint8_t* stream, int len) {
    auto* out = (float*)stream;
    int frames = len / (int)sizeof(float);
    for (int i = 0; i < frames; i++) out[i] = 0;
    std::lock_guard<std::mutex> l(g_m);
    Render(frames, kRate, [&](int i, float s) { out[i] += s; });
}
}  // namespace

void Play(Cue c) {
    uint64_t now = SDL_GetTicks64();
    {
        std::lock_guard<std::mutex> l(g_m);
        // (a burst - twenty messages that waited while offline - is one sound, not twenty)
        uint64_t& last = g_lastPlay[(int)c];
        if (now - last < (c == Cue::Typing ? 2500u : 700u)) return;
        last = now;
        g_voices.push_back({&Pcm(c), 0.0, c == Cue::ReceivedOpen ? 0.5f : 1.0f});
        g_lastSound = now;
    }
    // the phone's one output, while someone else has it: they mix it in (MixS16 / MixF32)
    if (audio_out::Single() && std::string(audio_out::Holder()) != "" && std::string(audio_out::Holder()) != "cues") return;
    if (g_dev) return;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
    SDL_AudioSpec want{}, have{};
    want.freq = kRate;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = OwnCb;
    g_dev = audio_out::Open("cues", nullptr, &want, &have);
    if (g_dev) SDL_PauseAudioDevice(g_dev, 0);
}

void Tick() {
    if (!g_dev) return;
    bool idle;
    {
        std::lock_guard<std::mutex> l(g_m);
        idle = g_voices.empty() && SDL_GetTicks64() - g_lastSound > 1500;
    }
    // (a phone's one output: let go at once, music or a call may want it; a computer keeps it a little longer)
    if (idle) {
        audio_out::Close(g_dev);
        g_dev = 0;
    }
}

void MixS16(int16_t* mono, int frames, int rate) {
    std::lock_guard<std::mutex> l(g_m);
    if (g_voices.empty()) return;
    Render(frames, rate, [&](int i, float s) {
        int v = mono[i] + (int)std::lround(s * 32767.0f);
        mono[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    });
}

void MixF32(float* out, int frames, int channels, int rate) {
    std::lock_guard<std::mutex> l(g_m);
    if (g_voices.empty()) return;
    Render(frames, rate, [&](int i, float s) {
        for (int ch = 0; ch < channels; ch++) out[(size_t)i * channels + ch] += s;
    });
}
}  // namespace cues
