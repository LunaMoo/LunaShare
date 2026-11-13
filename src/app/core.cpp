#include "core.h"

#include <SDL.h>

#include <cstdlib>
#include <mutex>

#include "engine.h"
#include "i18n.h"
#include <nlohmann/json.hpp>
#include "model.h"
#include "notify.h"
#include "platform.h"
#include "stb_image.h"

extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);  // (avatar.cpp)
void AndroidBridgeInit(ls::Engine* engine);
void AndroidBridgeShutdown();

namespace core {
namespace {
ls::Engine* g_engine = nullptr;
std::recursive_mutex g_m;  // (the UI's thread and the service's start it)
}  // namespace

bool Emulated() {
    static const bool on = getenv("LUNASHARE_KEEP_CORE") != nullptr;
    return on;
}

ls::Engine* Running() {
    std::lock_guard<std::recursive_mutex> l(g_m);
    return g_engine;
}

ls::Engine& Make() {
    std::lock_guard<std::recursive_mutex> l(g_m);
    if (g_engine) return *g_engine;
    g_engine = new ls::Engine();
    ls::Engine& e = *g_engine;
    // chat archives are kept compressed (zlib, from stb - already here for pictures)
    e.compress = [](const std::string& in) -> std::string {
        int n = 0;
        unsigned char* z = stbi_zlib_compress((unsigned char*)in.data(), (int)in.size(), &n, 8);
        std::string out = z && n > 0 ? std::string((const char*)z, (size_t)n) : std::string();
        free(z);
        return out;
    };
    e.decompress = [](const std::string& in) -> std::string {
        int n = 0;
        char* d = stbi_zlib_decode_malloc(in.data(), (int)in.size(), &n);
        std::string out = d && n > 0 ? std::string(d, (size_t)n) : std::string();
        free(d);
        return out;
    };
    notify::Attach(&e);  // system notifications (before Start: set once, never while it runs)
    return e;
}

static std::string CapsPath() { return ls::JoinPath(ls::ConfigDir(), "caps.json"); }

void SaveCaps(ls::Engine& e) {
    std::vector<std::string> caps;
    {
        auto lk = e.Lock();
        caps = e.localCaps;
    }
    nlohmann::json j = caps;
    std::string old;
    if (ls::LoadFileBytes(CapsPath(), old) && old == j.dump()) return;
    ls::SaveFileAtomic(CapsPath(), j.dump());
}

void LoadCaps(ls::Engine& e) {
    std::string s;
    if (!ls::LoadFileBytes(CapsPath(), s)) return;
    try {
        auto j = nlohmann::json::parse(s);
        if (!j.is_array()) return;
        auto lk = e.Lock();
        e.localCaps = j.get<std::vector<std::string>>();
    } catch (...) {
    }
}

bool KeepWithoutScreen(ls::Engine& e) {
    if (!ls::IsMobilePlatform() && !Emulated()) return false;
    return e.GetSettings().backgroundRun;
}

void Destroy() {
    std::lock_guard<std::recursive_mutex> l(g_m);
    if (!g_engine) return;
    g_engine->onEvent = nullptr;
    g_engine->Stop();
    delete g_engine;
    g_engine = nullptr;
}

bool StartWithoutScreen(const std::string& filesDir) {
    std::lock_guard<std::recursive_mutex> l(g_m);
    if (g_engine) return true;  // (already running: the screen's, or a start before)
    if (!filesDir.empty()) {
        ls::SetConfigDirOverride(filesDir);
        std::string tmp = ls::JoinPath(filesDir, "cache/tmp");
        ls::MakeDirs(tmp);
#if !defined(_WIN32)
        setenv("TMPDIR", tmp.c_str(), 1);
#endif
    }
    {
        // Never a new identity from here: with no settings in that folder (the app never set up, or the folder isn't
        // the one the app uses) the engine would make up a new device - another duplicate contact for everyone.
        // The app itself does the first start.
        std::string sp = ls::JoinPath(ls::ConfigDir(), "settings.json");
        if (!ls::FileExists(sp) && !ls::FileExists(sp + ".bak")) {
            SDL_Log("core: no settings in %s - not started without the screen", ls::ConfigDir().c_str());
            return false;
        }
        // (background running switched off: not even started for a moment)
        nlohmann::json j;
        if ((ls::LoadJsonFile(sp, j) || ls::LoadJsonFile(sp + ".bak", j)) && j.is_object() && j.contains("settings") &&
            j["settings"].is_object() && j["settings"].value("backgroundRun", true) == false) {
            SDL_Log("core: background running is off - not started without the screen");
            return false;
        }
    }
    ls::Engine& e = Make();
#if defined(__ANDROID__)
    AndroidBridgeInit(&e);
#endif
    LoadCaps(e);
    notify::SetBackground(true);  // (no screen: everything goes to the notifications)
    auto fail = [] {
        Destroy();
#if defined(__ANDROID__)
        AndroidBridgeShutdown();  // (the app isn't running after all: notification buttons don't call into it)
#endif
        return false;
    };
    if (!e.Start()) {
        SDL_Log("core: the engine couldn't start without the screen");
        return fail();
    }
    if (!e.GetSettings().backgroundRun) return fail();  // (switched off: nothing should run while the app isn't open)
    SetLanguage(e.GetSettings().language);  // (notifications are in the person's language)
    SDL_Log("core: running without the screen");
    return true;
}
}  // namespace core
