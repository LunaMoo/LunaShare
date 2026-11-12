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
    if (Emulated() && g_open > 0) SDL_SetError("Audio device already open");  // (as a phone would)
    else dev = SDL_OpenAudioDevice(device, 0, want, have, 0);
    if (dev) {
        g_open++;
        g_holder = who;
        if (Emulated()) SDL_Log("sound output: %s opened it", who);
    } else {
        SDL_Log("sound output: %s couldn't open it: %s%s%s", who, SDL_GetError(), g_open > 0 ? " - held by " : "", g_open > 0 ? g_holder.c_str() : "");
    }
    return dev;
}

void Close(SDL_AudioDeviceID dev) {
    if (!dev) return;
    SDL_CloseAudioDevice(dev);
    std::lock_guard<std::mutex> l(g_m);
    if (Emulated()) SDL_Log("sound output: %s let go of it", g_holder.c_str());
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
