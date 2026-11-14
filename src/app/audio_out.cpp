#include "audio_out.h"

#include <mutex>
#include <string>

namespace audio_out {
namespace {
std::mutex g_m;
int g_open = 0;
std::string g_holder;
bool Emulated() {
    static const bool on = SDL_getenv("LUNASHARE_ONE_OUTPUT") != nullptr;
    return on;
}
}  // namespace

bool Single() {
#if defined(__ANDROID__)
    return true;
#else
    return Emulated();
#endif
}

SDL_AudioDeviceID Open(const char* who, const char* device, const SDL_AudioSpec* want, SDL_AudioSpec* have) {
    std::lock_guard<std::mutex> l(g_m);
    SDL_AudioDeviceID dev = 0;
#if defined(__ANDROID__)
    // only a call plays on Android's call stream (our SDL patch reads this when a device opens): music, radio and
    // movies on the media stream - its volume buttons, and Bluetooth headphones' music channel, not the call one
    if (std::string(who) != "call") SDL_SetHint("SDL_ANDROID_VOICE_AUDIO", "0");
#endif
    if (Emulated() && g_open > 0) SDL_SetError("Audio device already open");  // (as a phone would)
    else dev = SDL_OpenAudioDevice(device, 0, want, have, 0);
    if (dev) {
        g_open++;
        g_holder = who;
#if defined(LUNASHARE_DIAG)
        SDL_Log("sound output: %s opened it (%d Hz, %d ch)", who, have ? have->freq : 0, have ? (int)have->channels : 0);
#else
        if (Emulated()) SDL_Log("sound output: %s opened it", who);
#endif
    } else {
        SDL_Log("sound output: %s couldn't open it: %s%s%s", who, SDL_GetError(), g_open > 0 ? " - held by " : "", g_open > 0 ? g_holder.c_str() : "");
    }
    return dev;
}

void Close(SDL_AudioDeviceID dev) {
    if (!dev) return;
    SDL_CloseAudioDevice(dev);
    std::lock_guard<std::mutex> l(g_m);
#if defined(LUNASHARE_DIAG)
    SDL_Log("sound output: %s let go of it", g_holder.c_str());
#else
    if (Emulated()) SDL_Log("sound output: %s let go of it", g_holder.c_str());
#endif
    if (g_open > 0) g_open--;
    if (g_open == 0) g_holder.clear();
}

const char* Holder() {
    static thread_local std::string s;
    std::lock_guard<std::mutex> l(g_m);
    s = g_holder;
    return s.c_str();
}
}  // namespace audio_out
