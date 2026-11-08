// LunaShare - listening together (the music player following a shared queue) and songs from contacts' Shared
// folders in the music player
#include <algorithm>
#include <cmath>
#include <random>

#include "emoji_host.h"
#include "i18n.h"
#include "imgui.h"
#include "music.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

static std::string Stem(const std::string& path) {
    std::string n = BaseName(path);
    size_t dot = n.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? n : n.substr(0, dot);
}

// ---------------------------------------------------------------- the player follows (every frame, engine lock)

void App::MusicExternalTick() {
    static bool ctlSet = false;
    if (!ctlSet) {  // the player's buttons (and the notification's, the media keys) while a song from elsewhere is on
        ctlSet = true;
        music::Play().externalCtl = [this](int cmd, double arg) -> bool {
            auto lk = eng.Lock();
            if (eng.listen.sid) {
                switch (cmd) {
                    case 1: eng.ListenControl(eng.listen.playing ? "pause" : "play"); break;
                    case 2: eng.ListenControl("next"); break;
                    case 3: eng.ListenControl("prev"); break;
                    case 4: case 6: eng.ListenControl("pause"); break;
                    case 5: eng.ListenControl("play"); break;
                    case 7: eng.ListenControl("seek", arg); break;
                    default: return false;
                }
                return true;
            }
            if (!musicLibList.empty()) {  // a contact's songs: next / previous in that folder
                if (cmd == 2 || cmd == 3) {
                    int n = musicLibAt + (cmd == 2 ? 1 : -1);
                    if (n >= 0 && n < (int)musicLibList.size()) {
                        musicLibAt = n;
                        MusicLibOpen();
                    }
                    return true;
                }
                if (cmd == 6) MusicLibEnd();
            }
            return false;
        };
    }
    music::Now n = music::Play().GetNow();
    // listening together: the song that's on, at the host's clock
    if (eng.listen.sid) {
        if (!musicLibList.empty()) MusicLibEnd();
        if (!listenFollowing && n.active && !n.external && n.playing) music::Play().SetPlaying(false);  // (joined: mine rests)
        listenFollowing = true;
        const ListenItem* cur = eng.ListenCurrent();
        if (cur) {
            if (!n.external || !n.follow || n.extId != cur->iid) {
                music::Source s;
                s.id = cur->iid;
                s.follow = true;
                s.size = cur->size;
                s.index = eng.listen.at;
                s.count = (int)eng.listen.items.size();
                if (eng.ListenMine(cur->iid) && !cur->path.empty()) {
                    s.path = cur->path;
                    if (!music::Lib().Find(cur->path, s.track)) s.track.path = cur->path;
                } else {
                    uint64_t iid = cur->iid;
                    s.read = [this, iid](int64_t off, uint8_t* b, int len) { return eng.ListenRead(iid, off, b, len, 200); };
                }
                s.track.title = cur->title.empty() ? Stem(cur->name) : cur->title;
                s.track.artist = cur->artist;
                s.track.album = cur->album;
                s.track.duration = cur->dur;
                music::Play().PlayExternal(s, eng.ListenPos(), eng.listen.playing);
            }
            music::Play().Follow(eng.ListenPos(), eng.listen.playing);
            if (eng.listen.host.empty() && music::Play().ExternalEnded(cur->iid)) eng.ListenEnded(cur->iid);
        } else if (n.external && n.follow) {
            music::Play().EndExternal();  // (the shared queue played out: quiet until someone adds a song)
        }
        return;
    }
    if (listenFollowing) {  // it ended (or I left): my own music again, paused
        listenFollowing = false;
        if (n.external && n.follow) music::Play().EndExternal();
    }
    // a contact's song
    if (!musicLibList.empty()) {
        if (!n.external || n.extId != musicLibSrc) MusicLibEnd();  // (something else is playing now)
        else if (music::Play().ExternalEnded(musicLibSrc)) {
            if (musicLibAt + 1 < (int)musicLibList.size()) {
                musicLibAt++;
                MusicLibOpen();
            }
        }
    }
}

// ---------------------------------------------------------------- a contact's Shared folder

void App::MusicPlayLib(const std::vector<LibFile>& list, int at, const std::string& who) {
    if (list.empty()) return;
    if (eng.listen.sid) {
        toasts.push_back({EventType::Info, T("Music"), T("Leave listening together to play this."), "", 0, 3.0f});
        return;
    }
    musicLibList = list;
    musicLibAt = std::clamp(at, 0, (int)list.size() - 1);
    musicLibWho = who;
    MusicLibOpen();
    if (ui::M().mobile) {
        if (screen != MScreen::Music) backStack.push_back(screen);
        screen = MScreen::Music;
        musicNowShown = true;
    } else {
        tab = Tab::Music;
        tabForce = true;
    }
}

void App::MusicLibOpen() {
    if (musicLibAt < 0 || musicLibAt >= (int)musicLibList.size()) return;
    const LibFile& f = musicLibList[(size_t)musicLibAt];
    if (!musicLibId.empty()) eng.LibStreamEnd(musicLibId);
    musicLibId.clear();
    std::string local;
    std::string id = eng.LibStreamStart(f, local);
    if (id.empty() && local.empty()) {
        toasts.push_back({EventType::Error, T("Music"), T("Can't play this song now."), "", 0, 3.0f});
        return;
    }
    music::Source s;
    static uint64_t counter = 0;
    s.id = (std::hash<std::string>()(f.id) << 8) ^ ++counter;
    if (!s.id) s.id = 1;
    musicLibSrc = s.id;
    s.size = f.size;
    s.index = musicLibAt;
    s.count = (int)musicLibList.size();
    if (!local.empty()) {
        s.path = local;
        if (!music::Lib().Find(local, s.track)) s.track.title = Stem(local);
    } else {
        s.read = eng.LibStreamReader(id);
        musicLibId = id;
        s.track.title = Stem(f.rel);
        s.track.artist = musicLibWho;
    }
    music::Play().PlayExternal(s, 0, true);
}

void App::MusicLibEnd() {
    if (!musicLibId.empty()) eng.LibStreamEnd(musicLibId);
    musicLibId.clear();
    musicLibList.clear();
    musicLibAt = -1;
    auto n = music::Play().GetNow();
    if (n.external && n.extId == musicLibSrc) music::Play().EndExternal();
    musicLibSrc = 0;
}

// ---------------------------------------------------------------- listening together: the UI

void App::MusicListenAdd(const std::vector<std::string>& paths, double startAt, bool quiet) {
    if (paths.empty()) return;
    std::vector<ListenItem> items;
    for (auto& p : paths) {
        ListenItem i;
        i.path = p;
        i.name = BaseName(p);
        if (const music::Track* t = MusicTrack(p)) {
            i.title = t->title;
            i.artist = t->artist;
            i.album = t->album;
            i.dur = t->duration;
        } else {
            i.title = Stem(p);
        }
        items.push_back(i);
    }
    eng.ListenAdd(items, startAt);
    if (!quiet) toasts.push_back({EventType::Info, T("Listening together"),
                      paths.size() == 1 ? TF("Added \"%s\".", items[0].title.c_str()) : TF("Added %d songs.", (int)paths.size()), "", 0, 2.0f});
}

// who to listen with
void App::MusicListenPopup() {
    if (musicListenOpen) {
        ImGui::OpenPopup("##listenwho");
        musicListenOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSizeConstraints(ImVec2(mobile ? vs.x * 0.8f : ImGui::GetFontSize() * 16, 0), ImVec2(vs.x * 0.95f, vs.y * 0.7f));
    if (!ImGui::BeginPopup("##listenwho")) return;
    // what to start with
    music::Now now = music::Play().GetNow();
    bool haveNow = now.active && !now.external;
    auto pls = music::Lib().Playlists();
    if (!haveNow && musicListenStart == 0) musicListenStart = pls.empty() ? 2 : 1;
    if (pls.empty() && musicListenStart == 1) musicListenStart = haveNow ? 0 : 2;
    ImGui::TextUnformatted(T("Start with:"));
    if (haveNow && ImGui::RadioButton(TF("what's playing now: %s", now.track.title.c_str()).c_str(), musicListenStart == 0)) musicListenStart = 0;
    if (!pls.empty()) {
        if (ImGui::RadioButton(T("a playlist:"), musicListenStart == 1)) musicListenStart = 1;
        ImGui::SameLine();
        std::string cur;
        for (auto& p : pls)
            if (p.id == musicListenPl) cur = p.name;
        if (cur.empty()) {
            musicListenPl = pls[0].id;
            cur = pls[0].name;
        }
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 10);
        if (ImGui::BeginCombo("##lpl", cur.c_str())) {
            for (auto& p : pls)
                if (ImGui::Selectable((p.name + "##" + p.id).c_str(), p.id == musicListenPl, 0, ImVec2(0, h))) {
                    musicListenPl = p.id;
                    musicListenStart = 1;
                }
            ImGui::EndCombo();
        }
    }
    if (ImGui::RadioButton(T("nothing yet - songs are added afterwards"), musicListenStart == 2)) musicListenStart = 2;
    ImGui::SeparatorText(T("Listen together with..."));
    int shown = 0;
    for (auto& k : SortedPeers()) {
        const KnownDevice* d = eng.DeviceFor(k);
        if (d && d->blocked) continue;
        shown++;
        std::string name = Emoji().Display(eng.PeerName(k));
        if (eng.IsConnected(k)) name += "  ●";
        if (ImGui::Selectable((name + "##" + k).c_str(), false, 0, ImVec2(0, h))) {
            if (!musicLibList.empty()) MusicLibEnd();
            eng.StartListen(k);
            if (musicListenStart == 0 && haveNow) {  // the song that's on goes on (for everyone), then the rest of my queue
                auto q = music::Play().Queue();
                std::vector<std::string> paths;
                for (int i = std::max(0, now.index); i < (int)q.size(); i++) paths.push_back(q[(size_t)i]);
                if (paths.empty()) paths.push_back(now.path);
                MusicListenAdd(paths, now.pos, true);
            } else if (musicListenStart == 1) {
                for (auto& p : pls)
                    if (p.id == musicListenPl) MusicListenAdd(p.paths, -1, true);
            }
        }
    }
    if (!shown) ImGui::TextDisabled("%s", T("No contacts yet."));
    ImGui::EndPopup();
}

// on top of the music screen: invitations, and who's listening
void App::DrawListenStrip() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    const ListenInviteInfo* inv = nullptr;
    for (auto& i : eng.listenInvites)
        if (i.sid != eng.listen.sid) inv = &i;
    if (!inv && !eng.listen.sid) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(50, 60, 110, 255));
    ImGui::BeginChild("listenstrip", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    if (eng.listen.sid) {
        std::string who;  // (the host lists itself first)
        for (size_t i = 1; i < eng.listen.people.size(); i++) who += (who.empty() ? "" : ", ") + eng.listen.people[i];
        if (!eng.listen.host.empty()) who = Emoji().Display(eng.PeerName(eng.ChatKey(eng.listen.host)));
        ImGui::PushTextWrapPos(0);
        if (eng.listen.host.empty() && who.empty())
            ImGui::TextUnformatted(TF("Listening together - waiting for %s to join", Emoji().Display(eng.PeerName(eng.listen.key)).c_str()).c_str());
        else
            ImGui::TextUnformatted(TF("Listening together with %s", Emoji().Display(who).c_str()).c_str());
        ImGui::TextDisabled("%s", T("Tap songs to add them to the shared queue; each one plays from the device it's on."));
        ImGui::PopTextWrapPos();
        float third = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3 : 0;
        if (mobile && !musicNowShown) {
            if (ImGui::Button(T("Shared queue"), ImVec2(third, h))) musicNowShown = true;
            ImGui::SameLine();
        }
        if (!music::Lib().Playlists().empty()) {
            if (ImGui::Button(T("Add a playlist..."), ImVec2(third, h))) musicListenPlOpen = true;
            ImGui::SameLine();
        }
        if (ImGui::Button(eng.listen.host.empty() ? T("End for everyone") : T("Leave"), ImVec2(mobile ? -1 : 0, h))) eng.LeaveListen();
    } else {
        uint64_t sid = inv->sid;
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(TF("%s invites you to listen to music together", Emoji().Display(eng.PeerName(eng.ChatKey(inv->host))).c_str()).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 140, 85, 255));
        bool go = ImGui::Button(T("Join"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(T("Not now"), ImVec2(mobile ? -1 : 0, h))) {
            auto& v = eng.listenInvites;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const ListenInviteInfo& x) { return x.sid == sid; }), v.end());
        } else if (go) {
            if (!musicLibList.empty()) MusicLibEnd();
            if (!eng.JoinListen(sid))
                toasts.push_back({EventType::Error, T("Listening together"), T("Not connected to them - trying again..."), "", 0, 4.0f});
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// listening together: a whole playlist of mine to the shared queue
void App::MusicListenPlaylistPopup() {
    if (musicListenPlOpen) {
        ImGui::OpenPopup("##listenpl");
        musicListenPlOpen = false;
    }
    float h = ui::M().mobile ? ui::M().rowH * 0.8f : 0;
    if (!ImGui::BeginPopup("##listenpl")) return;
    ImGui::TextUnformatted(T("Add a playlist to the shared queue"));
    ImGui::Separator();
    for (auto& p : music::Lib().Playlists())
        if (ImGui::Selectable((p.name + " (" + std::to_string(p.paths.size()) + ")##" + p.id).c_str(), false, 0, ImVec2(0, h))) MusicListenAdd(p.paths);
    ImGui::EndPopup();
}

// the shared queue: who added what; tap to play it, remove your own (the host: any)
void App::DrawListenQueue() {
    bool mobile = ui::M().mobile;
    float rowH = mobile ? ui::M().rowH * 1.1f : ImGui::GetFontSize() * 2.6f;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGui::SeparatorText(TF("Shared queue (%d)", (int)eng.listen.items.size()).c_str());
    if (eng.listen.items.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("Empty - add songs from your music: tap them in the list."));
        ImGui::PopTextWrapPos();
    }
    bool host = eng.listen.host.empty();
    uint64_t remove = 0, jump = 0;
    for (int i = 0; i < (int)eng.listen.items.size(); i++) {
        const ListenItem& it = eng.listen.items[(size_t)i];
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool cur = i == eng.listen.at;
        bool mine = it.owner == eng.settings.deviceId;
        float xw = (host || mine) ? (mobile ? h : ImGui::GetFrameHeight()) + ImGui::GetStyle().ItemSpacing.x : 0;
        if (ImGui::Selectable("##lq", cur, ImGuiSelectableFlags_AllowOverlap, ImVec2(ImGui::GetContentRegionAvail().x - xw, rowH)) && !cur) jump = it.iid;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float x = p.x + 6, lh = ImGui::GetTextLineHeight();
        float right = p.x + ImGui::GetContentRegionAvail().x - xw;
        char db[16] = "";
        if (it.dur > 0) snprintf(db, sizeof db, "%d:%02d", (int)it.dur / 60, (int)it.dur % 60);
        std::string d = db;
        float dw = ImGui::CalcTextSize(d.c_str()).x;
        ImVec4 clipR(x, p.y, right - dw - 12, p.y + rowH);
        ImU32 tc = cur ? ImGui::GetColorU32(ImGuiCol_SliderGrabActive) : i < eng.listen.at ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                                                                                         : ImGui::GetColorU32(ImGuiCol_Text);
        std::string title = it.title.empty() ? Stem(it.name) : it.title;
        dl->AddText(nullptr, 0, ImVec2(x, p.y + rowH * 0.5f - lh - 1), tc, title.c_str(), nullptr, 0, &clipR);
        std::string sub = it.artist;
        sub += (sub.empty() ? "" : "  ·  ") + TF("from %s", Emoji().Display(mine ? std::string(T("you")) : it.by).c_str());
        if (!mine && (cur || i == eng.listen.at + 1)) {
            double ld = eng.ListenLoaded(it.iid);
            if (ld < 1) sub += "  ·  " + TF("%d%% here", (int)(ld * 100));
        }
        dl->AddText(nullptr, 0, ImVec2(x, p.y + rowH * 0.5f + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str(), nullptr, 0, &clipR);
        dl->AddText(ImVec2(right - dw - 4, p.y + (rowH - lh) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), d.c_str());
        if (host || mine) {
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - (mobile ? h : ImGui::GetFrameHeight())) * 0.5f);
            if (ui::IconButton("lqx", Icon::Cancel, T("Remove from the shared queue"), mobile ? h : 0)) remove = it.iid;
        }
        ImGui::PopID();
    }
    if (jump) eng.ListenControl("jump", 0, jump);
    if (remove) eng.ListenControl("remove", 0, remove);
}

// the strip on top of a chat: someone invites me to listen / we're listening
void App::DrawListenBanner(const std::string& peer) {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    const ListenInviteInfo* inv = nullptr;
    for (auto& i : eng.listenInvites)
        if (i.key == peer && i.sid != eng.listen.sid) inv = &i;
    bool on = eng.listen.sid && eng.listen.key == peer;
    if (!inv && !on) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(50, 60, 110, 255));
    float rowH = (mobile ? h : ImGui::GetFrameHeight()) + ImGui::GetStyle().WindowPadding.y * 2;
    ImGui::BeginChild("listenbanner", ImVec2(0, rowH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::AlignTextToFramePadding();
    std::string text = on ? std::string(T("Listening to music together"))
                          : TF("%s invites you to listen to music together", Emoji().Display(eng.PeerName(eng.ChatKey(inv->host))).c_str());
    const char* bl = on ? T("Open") : T("Join");
    float bw = ImGui::CalcTextSize(bl).x + ImGui::GetStyle().FramePadding.x * 2 + (mobile ? h * 0.5f : 0);
    float closeW = inv && !on ? (h > 0 ? h : ImGui::GetFrameHeight()) + ImGui::GetStyle().ItemSpacing.x : 0;
    float avail = ImGui::GetContentRegionAvail().x - bw - closeW - ImGui::GetStyle().ItemSpacing.x;
    std::string t = text;
    while (t.size() > 4 && ImGui::CalcTextSize(t.c_str()).x > avail) {
        t.pop_back();
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();
    }
    if (t != text) t += "...";
    ImGui::TextUnformatted(t.c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - bw - closeW);
    bool open = false;
    if (on) {
        open = ImGui::Button(bl, ImVec2(bw, h));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 140, 85, 255));
        bool go = ImGui::Button(bl, ImVec2(bw, h));
        ImGui::PopStyleColor();
        uint64_t sid = inv->sid;
        ImGui::SameLine();
        if (ui::IconButton("ldismiss", Icon::Cancel, T("Not now"), h)) {
            auto& v = eng.listenInvites;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const ListenInviteInfo& x) { return x.sid == sid; }), v.end());
        } else if (go) {
            if (!musicLibList.empty()) MusicLibEnd();
            if (eng.JoinListen(sid)) open = true;
            else toasts.push_back({EventType::Error, T("Listening together"), T("Not connected to them - trying again..."), "", 0, 4.0f});
        }
    }
    if (open) {
        if (mobile) {
            if (screen != MScreen::Music) backStack.push_back(screen);
            screen = MScreen::Music;
            musicNowShown = true;
        } else {
            tab = Tab::Music;
            tabForce = true;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}
