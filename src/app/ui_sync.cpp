// My other devices: the same playlists and settings everywhere.
// Playlists merge one by one (the newer change wins, deleted ones stay deleted); the synced settings go as one piece
// (the newer one wins). Each side sends again only when the other lacks something newer, so it settles.
#include <cmath>

#include "i18n.h"
#include "music.h"
#include "platform.h"
#include "ui.h"

using namespace ls;
using json = nlohmann::json;

namespace {
uint64_t g_plSent = 0, g_libSeen = 0;
std::string g_prefsSeen;  // the synced settings as last seen (to notice a change made here)
int64_t g_nextCheck = 0;
bool g_started = false;
}  // namespace

// the settings that follow me to my other devices (not how this screen looks, not where things are on the disk)
static json PrefsOf(const Settings& s) {
    return {{"playerLoop", s.playerLoop},     {"playerAutoplay", s.playerAutoplay}, {"playerAutoNext", s.playerAutoNext},
            {"playerHideUi", s.playerHideUi}, {"playerOrientByAspect", s.playerOrientByAspect}, {"playerPip", s.playerPip},
            {"subtitleScale", std::round(s.subtitleScale * 100) / 100}, {"language", s.language}, {"mutedChats", s.mutedChats},
            {"sound", music::SoundToJson(music::Play().GetSound())}};
}

static void PrefsApply(Engine& eng, const json& p) {
    Settings s = eng.settings;
    s.playerLoop = p.value("playerLoop", s.playerLoop);
    s.playerAutoplay = p.value("playerAutoplay", s.playerAutoplay);
    s.playerAutoNext = p.value("playerAutoNext", s.playerAutoNext);
    s.playerHideUi = p.value("playerHideUi", s.playerHideUi);
    s.playerOrientByAspect = p.value("playerOrientByAspect", s.playerOrientByAspect);
    s.playerPip = p.value("playerPip", s.playerPip);
    s.subtitleScale = std::clamp(p.value("subtitleScale", s.subtitleScale), 0.5f, 2.5f);
    s.language = p.value("language", s.language);
    bool lang = s.language != eng.settings.language;
    eng.ApplySettings(s);
    if (lang) SetLanguage(s.language);
    if (p.contains("mutedChats") && p["mutedChats"].is_object()) {
        std::map<std::string, int64_t> m;
        for (auto& [k, v] : p["mutedChats"].items())
            if (v.is_number_integer() && m.size() < 1000) m[k] = v.get<int64_t>();
        eng.SetMutedChats(m);
    }
    if (p.contains("sound")) music::Play().SetSound(music::SoundFromJson(p["sound"]));
}

static json PlaylistsMsg() { return music::Lib().SyncJson(); }

void App::SyncTick() {
    // (engine lock held by the frame)
    int64_t now = SteadyMs();
    if (now < g_nextCheck) return;
    g_nextCheck = now + 500;
    if (!eng.settings.syncOwn) {
        g_started = false;
        return;
    }
    if (!g_started) {  // (on start, or turned on: what's here goes to them; theirs comes when they connect)
        g_started = true;
        g_prefsSeen = PrefsOf(eng.settings).dump();
        g_plSent = music::Lib().PlaylistsVersion();
        eng.SyncPush("playlists", PlaylistsMsg());
        eng.SyncPush("prefs", {{"ver", eng.settings.prefsVer}, {"p", json::parse(g_prefsSeen)}});
    }
    // theirs
    std::string kind;
    json data;
    while (eng.SyncPop(kind, data)) {
        if (kind == "playlists") {
            auto r = music::Lib().SyncMerge(data);
            if (r.changed || r.behind) {
                g_plSent = music::Lib().PlaylistsVersion();
                eng.SyncPush("playlists", PlaylistsMsg());
            }
        } else if (kind == "prefs") {
            int64_t ver = data.value("ver", (int64_t)0);
            if (ver > eng.settings.prefsVer && data.contains("p") && data["p"].is_object()) {
                PrefsApply(eng, data["p"]);
                eng.SetPrefsVer(ver);
                g_prefsSeen = PrefsOf(eng.settings).dump();
                if (!editDirty) edit = eng.settings;
                eng.SyncPush("prefs", {{"ver", ver}, {"p", json::parse(g_prefsSeen)}});  // (what's kept for the next device)
            } else if (ver < eng.settings.prefsVer) {
                eng.SyncPush("prefs", {{"ver", eng.settings.prefsVer}, {"p", json::parse(g_prefsSeen)}});  // they're behind
            }
        }
    }
    // mine, changed here
    if (music::Lib().Version() != g_libSeen) {  // (a scan: songs from another device's playlists may be here now)
        g_libSeen = music::Lib().Version();
        music::Lib().ResolveSynced();
        g_libSeen = music::Lib().Version();
    }
    if (music::Lib().PlaylistsVersion() != g_plSent) {
        g_plSent = music::Lib().PlaylistsVersion();
        eng.SyncPush("playlists", PlaylistsMsg());
    }
    std::string p = PrefsOf(eng.settings).dump();
    if (p != g_prefsSeen) {
        g_prefsSeen = p;
        eng.SetPrefsVer(std::max(NowMs(), eng.settings.prefsVer + 1));
        eng.SyncPush("prefs", {{"ver", eng.settings.prefsVer}, {"p", json::parse(p)}});
    }
}
