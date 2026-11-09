// My other devices: the same playlists and settings everywhere.
// Playlists merge one by one (the newer change wins, deleted ones stay deleted); the synced settings go as one piece
// (the newer one wins). Each side sends again only when the other lacks something newer, so it settles.
#include <algorithm>
#include <cmath>

#include <SDL.h>

#include "i18n.h"
#include "notify.h"
#include "model.h"
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

json App::AppDataExport() {
    return {{"music", {{"playlists", music::Lib().SyncJson()}, {"folders", music::Lib().Folders()},
                       {"sound", music::SoundToJson(music::Play().GetSound())}}},
            {"moviePositions", movpos::Export()}};
}

int App::AppDataImport(const json& j) {
    if (!j.is_object()) return 0;
    json m = j.value("music", json::object());
    if (m.contains("playlists")) {
        // (an exported set has its own change times: anything newer here stays; a list deleted in the file isn't
        // a reason to delete it here)
        json pl = m["playlists"];
        pl.erase("gone");
        music::Lib().SyncMerge(pl);
    }
    std::vector<std::string> have = music::Lib().Folders();
    for (auto& f : m.value("folders", json::array()))
        if (f.is_string() && IsDirectory(f.get<std::string>()) && std::find(have.begin(), have.end(), f.get<std::string>()) == have.end())
            music::Lib().AddFolder(f.get<std::string>());
    if (m.contains("sound")) music::Play().SetSound(music::SoundFromJson(m["sound"]));
    if (j.contains("moviePositions")) {
        movpos::Import(j["moviePositions"]);
        movpos::Save(true);
    }
    return (int)music::Lib().Playlists().size();
}

void App::SyncTick() {
    // (engine lock held by the frame)
    int64_t now = SteadyMs();
    if (now < g_nextCheck) return;
    g_nextCheck = now + 500;
    if (!replyFor.empty() && editMid.empty() && replyFor == selectedPeer) DraftSet(replyFor, chatInput);  // (kept as it's typed)
    if (!eng.settings.syncOwn) {  // (the clipboard has its own switch)
        g_started = false;
        std::string k;
        json d;
        while (eng.SyncPop(k, d))
            if (k == "clip") ClipReceived(d);
        ClipTick();
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
        } else if (kind == "clip") {
            ClipReceived(data);
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
    ClipTick();
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

// ---- the clipboard, the same on my devices (an option): copied here -> theirs; copied there -> here
namespace {
std::string g_clipSeen;  // what the clipboard had when last looked at (or was set to)
int64_t g_clipTs = 0;    // the newest clipboard taken (from any device)
bool g_clipStarted = false;
}  // namespace

static std::string ClipNow() {
    if (!SDL_HasClipboardText()) return "";
    char* t = SDL_GetClipboardText();
    std::string s = t ? t : "";
    SDL_free(t);
    return s;
}

void App::ClipTick() {
    if (!eng.settings.clipShare) {
        g_clipStarted = false;
        return;
    }
    if (notify::Background()) return;  // (Android doesn't let an app in the background read it)
    std::string now = ClipNow();
    if (!g_clipStarted) {  // (what's there already isn't news)
        g_clipStarted = true;
        g_clipSeen = now;
        return;
    }
    if (now == g_clipSeen || now.empty() || now.size() > 256 * 1024) return;
    g_clipSeen = now;
    g_clipTs = std::max(NowMs(), g_clipTs + 1);
    eng.SyncPush("clip", {{"text", now}, {"ts", g_clipTs}, {"from", eng.settings.displayName}});
}

void App::ClipReceived(const json& d) {
    int64_t ts = d.value("ts", (int64_t)0);
    std::string text = d.value("text", "");
    if (!eng.settings.clipShare || ts <= g_clipTs || text.empty() || text.size() > 256 * 1024) return;
    g_clipTs = ts;
    g_clipSeen = text;
    SDL_SetClipboardText(text.c_str());
    toasts.push_back({EventType::Info, T("Clipboard"), TF("Copied on %s - paste it here.", d.value("from", std::string("?")).c_str()), "", 0, 2.5f});
}

// ---- drafts: what was typed in a chat and not sent stays there (also across restarts)
namespace {
std::map<std::string, std::string> g_drafts;
bool g_draftsLoaded = false, g_draftsDirty = false;
}  // namespace

static std::string DraftsPath(Engine& eng) { return JoinPath(eng.ConfigPath(), "drafts.json"); }

static void DraftsLoad(Engine& eng) {
    if (g_draftsLoaded) return;
    g_draftsLoaded = true;
    json j;
    if (LoadJsonFile(DraftsPath(eng), j) && j.is_object())
        for (auto& [k, v] : j.items())
            if (v.is_string()) g_drafts[k] = v.get<std::string>();
}

std::string App::DraftGet(const std::string& peer) {
    DraftsLoad(eng);
    auto it = g_drafts.find(peer);
    return it == g_drafts.end() ? std::string() : it->second;
}

void App::DraftSet(const std::string& peer, const std::string& text) {
    DraftsLoad(eng);
    bool blank = text.find_first_not_of(" \t\r\n") == std::string::npos;
    auto it = g_drafts.find(peer);
    if (blank) {
        if (it == g_drafts.end()) return;
        g_drafts.erase(it);
    } else {
        if (it != g_drafts.end() && it->second == text) return;
        g_drafts[peer] = text.substr(0, 64 * 1024);
    }
    g_draftsDirty = true;
    DraftsSave();
}

void App::DraftsSave() {
    if (!g_draftsDirty) return;
    g_draftsDirty = false;
    SaveJsonFile(DraftsPath(eng), json(g_drafts));
}
