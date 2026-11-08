// Music player screen: the library (songs, artists, albums, genres, folders, playlists), the "now playing" panel,
// the mini player at the bottom, sending songs / albums to people.
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>

#include "emoji_host.h"
#include "i18n.h"
#include "image_cache.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "music.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

static std::string Clk(double s) {
    if (!(s >= 0)) s = 0;
    int t = (int)s;
    char b[32];
    if (t >= 3600) snprintf(b, sizeof(b), "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(b, sizeof(b), "%d:%02d", t / 60, t % 60);
    return b;
}

static std::string Low(std::string s) {
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

// a cover (or a note sign when there's none)
static void Cover(ImDrawList* dl, const std::string& path, ImVec2 a, float size, float rounding = 4) {
    ImVec2 b(a.x + size, a.y + size);
    const ImageTex* t = path.empty() ? nullptr : &Images().Get(path, size > 200 ? ImageCache::Size::Thumb : ImageCache::Size::Tile);
    if (t && t->state == ImageTex::Ready && t->w > 0 && t->h > 0) {
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (t->w > t->h) { float f = (float)t->h / (float)t->w; uv0.x = (1 - f) * 0.5f; uv1.x = 1 - uv0.x; }
        else { float f = (float)t->w / (float)t->h; uv0.y = (1 - f) * 0.5f; uv1.y = 1 - uv0.y; }
        dl->AddImageRounded(t->Id(), a, b, uv0, uv1, IM_COL32_WHITE, rounding);
        return;
    }
    dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    // a note
    float s = size;
    ImU32 c = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddCircleFilled(ImVec2(a.x + s * 0.4f, a.y + s * 0.68f), s * 0.11f, c);
    dl->AddLine(ImVec2(a.x + s * 0.5f, a.y + s * 0.68f), ImVec2(a.x + s * 0.5f, a.y + s * 0.25f), c, std::max(1.5f, s * 0.05f));
    dl->AddLine(ImVec2(a.x + s * 0.5f, a.y + s * 0.25f), ImVec2(a.x + s * 0.68f, a.y + s * 0.33f), c, std::max(1.5f, s * 0.05f));
}

// ---------------------------------------------------------------- data for the views

void App::MusicRefresh() {
    uint64_t v = music::Lib().Version();
    if (v == musicVer) return;
    musicVer = v;
    musicTracks = music::Lib().Tracks();
    musicByPath.clear();
    for (size_t i = 0; i < musicTracks.size(); i++) musicByPath[musicTracks[i].path] = (int)i;
}

const music::Track* App::MusicTrack(const std::string& path) {
    auto it = musicByPath.find(path);
    return it == musicByPath.end() ? nullptr : &musicTracks[(size_t)it->second];
}

static bool AlbumOrder(const music::Track* a, const music::Track* b) {
    if (a->disc != b->disc) return a->disc < b->disc;
    if (a->number != b->number) return a->number < b->number;
    return Low(a->title) < Low(b->title);
}

// ---------------------------------------------------------------- sending

void App::MusicSend(const std::vector<std::string>& paths, const std::string& folder) {
    musicSendFiles.clear();
    std::set<std::string> extra;
    for (auto& p : paths) {
        musicSendFiles.push_back({p, folder.empty() ? BaseName(p) : folder + "/" + BaseName(p)});
        if (!folder.empty()) {  // the album's picture goes with it
            const music::Track* t = MusicTrack(p);
            if (t && !t->cover.empty() && DirName(t->cover) == DirName(p)) extra.insert(t->cover);
        }
    }
    for (auto& c : extra) musicSendFiles.push_back({c, folder + "/" + BaseName(c)});
    musicSendName = folder.empty() ? (paths.size() == 1 ? BaseName(paths[0]) : TF("%d songs", (int)paths.size())) : folder;
    musicSendOpen = true;
}

void App::MusicSendPopup() {
    if (musicSendOpen) {
        ImGui::OpenPopup("##musicsend");
        musicSendOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSizeConstraints(ImVec2(mobile ? vs.x * 0.8f : ImGui::GetFontSize() * 16, 0), ImVec2(vs.x * 0.95f, vs.y * 0.7f));
    if (!ImGui::BeginPopup("##musicsend")) return;
    ImGui::TextUnformatted(TF("Send \"%s\" to...", musicSendName.c_str()).c_str());
    ImGui::Separator();
    int shown = 0;
    for (auto& k : SortedPeers()) {
        const KnownDevice* d = eng.DeviceFor(k);
        if (d && d->blocked) continue;
        shown++;
        std::string name = Emoji().Display(eng.PeerName(k));
        if (eng.IsConnected(k)) name += "  ●";
        if (ImGui::Selectable((name + "##" + k).c_str(), false, 0, ImVec2(0, h))) {
            if (eng.SendFileList(k, musicSendFiles, musicSendName))
                toasts.push_back({EventType::Info, T("Music"), TF("Sending \"%s\" to %s.", musicSendName.c_str(), eng.PeerName(k).c_str()), "", 0, 3.0f});
            musicSel.clear();
        }
    }
    if (!shown) ImGui::TextDisabled("%s", T("No contacts yet."));
    ImGui::EndPopup();
}

// adding songs to a playlist (or a new one)
void App::MusicPlaylistPopup() {
    if (musicAddOpen) {
        ImGui::OpenPopup("##musicadd");
        musicAddOpen = false;
        musicNewName.clear();
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    if (!ImGui::BeginPopup("##musicadd")) return;
    ImGui::TextUnformatted(TF("Add %d song(s) to...", (int)musicAddPaths.size()).c_str());
    ImGui::Separator();
    for (auto& p : music::Lib().Playlists())
        if (ImGui::Selectable((p.name + "##" + p.id).c_str(), false, 0, ImVec2(0, h))) {
            music::Lib().AddToPlaylist(p.id, musicAddPaths);
            toasts.push_back({EventType::Info, T("Music"), TF("Added to \"%s\".", p.name.c_str()), "", 0, 2.5f});
            musicSel.clear();
        }
    ImGui::Separator();
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##newpl", T("new playlist"), &musicNewName);
    if (!mobile) ImGui::SameLine();
    ImGui::BeginDisabled(musicNewName.empty());
    if (ImGui::Button(T("Create and add"), ImVec2(mobile ? -1 : 0, h))) {
        std::string id = music::Lib().CreatePlaylist(musicNewName);
        music::Lib().AddToPlaylist(id, musicAddPaths);
        musicSel.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

// a song's menu: play next, queue, playlist, send, (remove from the playlist)
void App::MusicTrackMenu(const std::string& path, const std::string& playlistId, int playlistIndex) {
    float h = ui::M().mobile ? ui::M().rowH * 0.8f : 0;
    if (eng.listen.sid && ImGui::Selectable(T("Add to listening together"), false, 0, ImVec2(0, h))) MusicListenAdd({path});
    if (ImGui::Selectable(T("Play next"), false, 0, ImVec2(0, h))) music::Play().PlayNext({path});
    if (ImGui::Selectable(T("Add to the queue"), false, 0, ImVec2(0, h))) music::Play().AddToQueue({path});
    if (ImGui::Selectable(T("Add to a playlist..."), false, 0, ImVec2(0, h))) {
        musicAddPaths = {path};
        musicAddOpen = true;
    }
    if (ImGui::Selectable(T("Send to..."), false, 0, ImVec2(0, h))) MusicSend({path}, "");
    if (ImGui::Selectable(T("Select"), false, 0, ImVec2(0, h))) musicSel.insert(path);
    if (!playlistId.empty() && playlistId[0] != '#' && ImGui::Selectable(T("Remove from this playlist"), false, 0, ImVec2(0, h))) {
        for (auto& p : music::Lib().Playlists())
            if (p.id == playlistId && playlistIndex >= 0 && playlistIndex < (int)p.paths.size()) {
                auto v = p.paths;
                v.erase(v.begin() + playlistIndex);
                music::Lib().SetPlaylistPaths(p.id, v);
            }
    }
}

// rows of songs; tapping one plays the list from there. Returns nothing - it acts right away.
void App::MusicTrackList(const std::vector<const music::Track*>& list, const std::string& playlistId, bool showAlbum) {
    bool mobile = ui::M().mobile;
    float rowH = mobile ? ui::M().rowH * 1.1f : ImGui::GetFontSize() * 2.6f;
    float cov = rowH - 8;
    std::string cur = music::Play().GetNow().path;
    bool selecting = !musicSel.empty();
    ImGuiListClipper clip;
    clip.Begin((int)list.size(), rowH + ImGui::GetStyle().ItemSpacing.y);
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const music::Track* t = list[(size_t)i];
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool sel = musicSel.count(t->path) > 0;
            bool clicked = ImGui::Selectable("##row", sel || t->path == cur, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowH));
            bool longPress = mobile && ImGui::IsItemActive() && ImGui::GetIO().MouseDownDuration[0] > 0.55f && ImGui::GetMouseDragDelta(0).y == 0;
            if (longPress || (ImGui::IsItemHovered() && ImGui::IsMouseReleased(1))) ImGui::OpenPopup("##trk");
            if (clicked && !longPress) {
                if (selecting || ImGui::GetIO().KeyCtrl) {
                    if (sel) musicSel.erase(t->path);
                    else musicSel.insert(t->path);
                } else if (eng.listen.sid && playlistId != "#queue") {  // listening together: to the shared queue
                    MusicListenAdd({t->path});
                } else {
                    std::vector<std::string> paths;
                    for (auto* x : list) paths.push_back(x->path);
                    music::Play().PlayList(paths, i);
                }
            }
            if (ImGui::BeginPopup("##trk")) {
                MusicTrackMenu(t->path, playlistId, i);
                ImGui::EndPopup();
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float x = p.x + 4;
            if (selecting) {  // a tick
                float r = rowH * 0.18f;
                ImVec2 cc(x + r, p.y + rowH * 0.5f);
                if (sel) {
                    dl->AddCircleFilled(cc, r, ui::ColAccent());
                    ui::DrawIcon(dl, Icon::Check, cc, r * 1.2f, IM_COL32_WHITE);
                } else {
                    dl->AddCircle(cc, r, ImGui::GetColorU32(ImGuiCol_Text), 0, 2);
                }
                x += r * 2 + 8;
            }
            Cover(dl, t->cover, ImVec2(x, p.y + 4), cov);
            x += cov + 10;
            float right = p.x + ImGui::GetContentRegionAvail().x;
            std::string d = t->duration > 0 ? Clk(t->duration) : "";
            float dw = ImGui::CalcTextSize(d.c_str()).x;
            ImU32 tc = t->path == cur ? ImGui::GetColorU32(ImGuiCol_SliderGrabActive) : ImGui::GetColorU32(ImGuiCol_Text);
            float lh = ImGui::GetTextLineHeight();
            ImVec4 clipR(x, p.y, right - dw - 12, p.y + rowH);
            dl->AddText(nullptr, 0, ImVec2(x, p.y + rowH * 0.5f - lh - 1), tc, t->title.c_str(), nullptr, 0, &clipR);
            std::string sub = t->artist;
            if (showAlbum && !t->album.empty()) sub += (sub.empty() ? "" : "  ·  ") + t->album;
            dl->AddText(nullptr, 0, ImVec2(x, p.y + rowH * 0.5f + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str(), nullptr, 0, &clipR);
            dl->AddText(ImVec2(right - dw - 4, p.y + (rowH - lh) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), d.c_str());
            ImGui::PopID();
        }
}

// buttons above a group of songs (album / artist / genre / folder / playlist)
void App::MusicGroupButtons(const std::vector<const music::Track*>& list, const std::string& sendFolder) {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    std::vector<std::string> paths;
    for (auto* t : list) paths.push_back(t->path);
    bool together = eng.listen.sid != 0;  // listening together: they go to the shared queue
    if (ui::IconTextButton(together ? T("Add all") : T("Play"), together ? Icon::Plus : Icon::Play, ImVec2(0, h)) && !paths.empty()) {
        if (together) MusicListenAdd(paths);
        else music::Play().PlayList(paths, 0, false);
    }
    ImGui::SameLine();
    if (ui::IconTextButton(T("Shuffle"), Icon::Shuffle, ImVec2(0, h)) && !paths.empty()) {
        if (together) {
            std::vector<std::string> v = paths;
            std::mt19937 rng((uint32_t)SteadyMs());
            std::shuffle(v.begin(), v.end(), rng);
            MusicListenAdd(v);
        } else {
            music::Play().PlayList(paths, 0, true);
        }
    }
    ImGui::SameLine();
    if (ui::IconTextButton(T("Send..."), Icon::Share, ImVec2(0, h)) && !paths.empty()) MusicSend(paths, sendFolder);
    if (!mobile) ImGui::SameLine();
    if (ui::IconTextButton(T("To a playlist..."), Icon::Plus, ImVec2(0, h)) && !paths.empty()) {
        musicAddPaths = paths;
        musicAddOpen = true;
    }
}

// ---------------------------------------------------------------- now playing

void App::DrawMusicNow(bool big) {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH : ImGui::GetFrameHeight() * 1.4f;
    music::Now n = music::Play().GetNow();
    if (!n.active) {
        if (eng.listen.sid) {
            ImGui::TextDisabled("%s", T("Nothing playing yet."));
            DrawListenQueue();
            return;
        }
        ImGui::TextDisabled("%s", T("Nothing playing. Pick a song."));
        if (ImGui::Button(T("Listen together..."), ImVec2(big ? -1 : 0, mobile ? h * 0.85f : 0))) musicListenOpen = true;
        return;
    }
    float w = ImGui::GetContentRegionAvail().x;
    float cs = big ? std::min(w, ImGui::GetContentRegionAvail().y * 0.5f) : std::min(w, ImGui::GetFontSize() * 14);
    ImVec2 p = ImGui::GetCursorScreenPos();
    Cover(ImGui::GetWindowDrawList(), n.track.cover, ImVec2(p.x + (w - cs) * 0.5f, p.y), cs, 8);
    ImGui::Dummy(ImVec2(w, cs + 6));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(n.track.title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    std::string sub = n.track.artist + (n.track.album.empty() ? "" : (n.track.artist.empty() ? "" : "  ·  ") + n.track.album);
    if (n.follow && eng.ListenCurrent()) {  // listening together: whose song
        const ListenItem* li = eng.ListenCurrent();
        bool mine = li->owner == eng.settings.deviceId;
        sub += (sub.empty() ? "" : "  ·  ") + TF("from %s", Emoji().Display(mine ? std::string(T("you")) : li->by).c_str());
    } else if (n.external && !musicLibWho.empty()) {
        sub += (sub.empty() ? "" : "  ·  ") + TF("from %s's Shared folder", Emoji().Display(musicLibWho).c_str());
    }
    ImGui::TextDisabled("%s", sub.c_str());
    if (n.loading) ImGui::TextDisabled("%s", T("on its way..."));
    // seek bar
    ImVec2 bp = ImGui::GetCursorScreenPos();
    float bw = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##mseek", ImVec2(bw, h * 0.6f));
    double frac = n.dur > 0 ? std::clamp(n.pos / n.dur, 0.0, 1.0) : 0;
    if (ImGui::IsItemActive() && n.dur > 0) {
        musicDrag = std::clamp((ImGui::GetIO().MousePos.x - bp.x) / bw, 0.0f, 1.0f);
        frac = musicDrag;
    }
    if (ImGui::IsItemDeactivated() && musicDrag >= 0) {
        music::Play().Seek(musicDrag * n.dur);
        musicDrag = -1;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cy = bp.y + h * 0.3f;
    dl->AddRectFilled(ImVec2(bp.x, cy - 3), ImVec2(bp.x + bw, cy + 3), ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
    dl->AddRectFilled(ImVec2(bp.x, cy - 3), ImVec2(bp.x + bw * (float)frac, cy + 3), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 3);
    dl->AddCircleFilled(ImVec2(bp.x + bw * (float)frac, cy), mobile ? 10.0f : 7.0f, ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
    ImGui::TextDisabled("%s / %s", Clk(musicDrag >= 0 ? musicDrag * n.dur : n.pos).c_str(), Clk(n.dur).c_str());
    // the buttons
    float sp = ImGui::GetStyle().ItemSpacing.x;
    float bs = std::min(h * 1.1f, (bw - sp * 4) / 5);
    float total = bs * 5 + sp * 4;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (bw - total) * 0.5f));
    bool sh = n.shuffle;
    ImGui::BeginDisabled(n.external);  // (shuffle and repeat are for my own queue)
    if (sh) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ui::IconButton("mshuf", Icon::Shuffle, sh ? T("Shuffle: on") : T("Shuffle: off"), bs)) music::Play().SetShuffle(!sh);
    if (sh) ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::IconButton("mprev", Icon::Back, T("Previous"), bs)) music::Play().Prev();
    ImGui::SameLine();
    if (ui::IconButton("mplay", n.playing ? Icon::Pause : Icon::Play, n.playing ? T("Pause") : T("Play"), bs)) music::Play().Toggle();
    ImGui::SameLine();
    // "next": the back arrow turned around
    ImVec2 np = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##mnext", ImVec2(bs, bs))) music::Play().Next();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Next"));
    {
        ImVec2 c(np.x + bs * 0.5f, np.y + bs * 0.5f);
        float s = bs * 0.22f;
        ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        dl->AddLine(ImVec2(c.x - s * 0.6f, c.y - s), ImVec2(c.x + s * 0.4f, c.y), col, 2.5f);
        dl->AddLine(ImVec2(c.x + s * 0.4f, c.y), ImVec2(c.x - s * 0.6f, c.y + s), col, 2.5f);
    }
    ImGui::SameLine();
    int rp = n.repeat;
    if (rp) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const char* rt = rp == music::RepeatOne ? T("Repeat: this song") : rp == music::RepeatAll ? T("Repeat: the list") : T("Repeat: off");
    ImVec2 rpp = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(n.external);
    if (ui::IconButton("mrep", Icon::Loop, rt, bs)) music::Play().SetRepeat((rp + 1) % 3);
    ImGui::EndDisabled();
    if (rp == music::RepeatOne) dl->AddText(ImVec2(rpp.x + bs * 0.68f, rpp.y + bs * 0.05f), ImGui::GetColorU32(ImGuiCol_Text), "1");
    if (rp) ImGui::PopStyleColor();
    if (n.external) {  // a song from elsewhere: keep it / leave
        ImGui::Spacing();
        float aw = (ImGui::GetContentRegionAvail().x - sp * 2) / 3, ah = mobile ? h * 0.85f : ImGui::GetFrameHeight() * 1.2f;
        bool canSave = false, saved = false;
        if (n.follow) {
            const ListenItem* li = eng.ListenCurrent();
            if (li && !eng.ListenMine(li->iid)) {
                canSave = true;
                saved = eng.ListenSaving(li->iid);
                ImGui::BeginDisabled(saved);
                if (ImGui::Button(saved ? T("Saving") : T("Save"), ImVec2(aw, ah))) eng.ListenSave(li->iid);
                ImGui::EndDisabled();
            }
        } else if (!musicLibId.empty()) {
            canSave = true;
            saved = eng.LibStreamSaved(musicLibId);
            ImGui::BeginDisabled(saved);
            if (ImGui::Button(saved ? T("Saving") : T("Save"), ImVec2(aw, ah))) eng.LibStreamSave(musicLibId);
            ImGui::EndDisabled();
        }
        if (canSave) ImGui::SameLine();
        if (n.follow) {
            if (ImGui::Button(eng.listen.host.empty() ? T("End for everyone") : T("Leave"), ImVec2(aw, ah))) eng.LeaveListen();
        } else if (ImGui::Button(T("Stop"), ImVec2(aw, ah))) {
            music::Play().Stop();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Sound..."), ImVec2(aw, ah))) musicSoundOpen = true;
        if (eng.listen.sid) DrawListenQueue();
        return;
    }
    if (eng.listen.sid) {  // (listening together, nothing of it on yet)
        DrawListenQueue();
        return;
    }
    if (big) {
        ImGui::Spacing();
        // finger-sized: three across
        float aw = (ImGui::GetContentRegionAvail().x - sp * 2) / 3, ah = mobile ? h * 0.85f : ImGui::GetFrameHeight() * 1.2f;
        if (ImGui::Button(T("Send..."), ImVec2(aw, ah))) MusicSend({n.path}, "");
        ImGui::SameLine();
        if (ImGui::Button(T("Stop"), ImVec2(aw, ah))) music::Play().Stop();
        ImGui::SameLine();
        if (ImGui::Button(T("Sound..."), ImVec2(aw, ah))) musicSoundOpen = true;
        if (ImGui::Button(T("Listen together..."), ImVec2(-1, ah))) musicListenOpen = true;
        // the queue
        ImGui::SeparatorText(TF("Queue (%d)", n.count).c_str());
        auto q = music::Play().Queue();
        std::vector<const music::Track*> list;
        static std::vector<music::Track> tmp;
        tmp.clear();
        tmp.reserve(q.size());
        for (auto& p : q) {
            const music::Track* t = MusicTrack(p);
            if (t) tmp.push_back(*t);
            else {
                music::Track x;
                x.path = p;
                x.title = BaseName(p);
                tmp.push_back(x);
            }
        }
        for (auto& t : tmp) list.push_back(&t);
        MusicTrackList(list, "#queue", true);
    }
}

// the strip at the bottom: cover, title, play / pause, next; tapping it opens the music screen
void App::DrawMusicMini() {
    music::Now n = music::Play().GetNow();
    if (!n.active) return;
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.95f : ImGui::GetFrameHeight() * 1.9f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 6);
    float bs = h * 0.8f;
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("##mini", ImVec2(w - bs * 2 - 12, h))) {
        if (mobile) {
            if (screen != MScreen::Music) backStack.push_back(screen);
            screen = MScreen::Music;
            musicNowShown = true;
        } else {
            tab = Tab::Music;
            tabForce = true;
        }
    }
    Cover(dl, n.track.cover, ImVec2(p.x + 4, p.y + 4), h - 8);
    float tx = p.x + h + 4;
    ImVec4 clipR(tx, p.y, p.x + w - bs * 2 - 16, p.y + h);
    float lh = ImGui::GetTextLineHeight();
    dl->AddText(nullptr, 0, ImVec2(tx, p.y + h * 0.5f - lh - 1), ImGui::GetColorU32(ImGuiCol_Text), n.track.title.c_str(), nullptr, 0, &clipR);
    dl->AddText(nullptr, 0, ImVec2(tx, p.y + h * 0.5f + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), n.track.artist.c_str(), nullptr, 0, &clipR);
    if (n.dur > 0)  // a thin progress line
        dl->AddRectFilled(ImVec2(p.x + 6, p.y + h - 3), ImVec2(p.x + 6 + (w - 12) * (float)std::clamp(n.pos / n.dur, 0.0, 1.0), p.y + h - 1),
                          ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - bs * 2 - 8, p.y + (h - bs) * 0.5f));
    if (ui::IconButton("miniplay", n.playing ? Icon::Pause : Icon::Play, nullptr, bs)) music::Play().Toggle();
    ImGui::SameLine();
    ImVec2 np = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##mininext", ImVec2(bs, bs))) music::Play().Next();
    ImVec2 c(np.x + bs * 0.5f, np.y + bs * 0.5f);
    float s = bs * 0.22f;
    dl->AddLine(ImVec2(c.x - s * 0.6f, c.y - s), ImVec2(c.x + s * 0.4f, c.y), ImGui::GetColorU32(ImGuiCol_Text), 2.5f);
    dl->AddLine(ImVec2(c.x + s * 0.4f, c.y), ImVec2(c.x - s * 0.6f, c.y + s), ImGui::GetColorU32(ImGuiCol_Text), 2.5f);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(0, 0));
}

// ---------------------------------------------------------------- the music screen

// sound: equalizer, smooth transitions, remix
// i18n-keys: "Flat", "Bass boost", "Treble boost", "Vocal", "Rock", "Pop", "Jazz", "Classical", "Electronic", "Loudness"
void App::MusicSoundPopup() {
    if (musicSoundOpen) {
        ImGui::OpenPopup("##msound");
        musicSoundOpen = false;
    }
    bool mobile = ui::M().mobile;
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (mobile) ImGui::SetNextWindowSize(ImVec2(vp.x * 0.96f, 0));
    if (!ImGui::BeginPopup("##msound")) {
        if (musicSoundDirty) music::Play().SaveState();
        musicSoundDirty = false;
        return;
    }
    music::Sound s = music::Play().GetSound();
    bool ch = false;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGui::SeparatorText(T("Equalizer"));
    ch |= ImGui::Checkbox(T("On"), &s.eqOn);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    std::string cur = s.preset.empty() ? std::string(T("Custom")) : std::string(T(s.preset.c_str()));
    if (ImGui::BeginCombo("##eqp", cur.c_str())) {
        for (int i = 0; i < music::kEqPresetCount; i++)
            if (ImGui::Selectable(T(music::kEqPresets[i].name), s.preset == music::kEqPresets[i].name, 0, ImVec2(0, h))) {
                s.preset = music::kEqPresets[i].name;
                for (int k = 0; k < music::kEqBands; k++) s.eq[k] = music::kEqPresets[i].g[k];
                s.eqOn = true;
                ch = true;
            }
        ImGui::EndCombo();
    }
    if (!s.eqOn) ImGui::BeginDisabled();
    float avail = mobile ? vp.x * 0.96f - ImGui::GetStyle().WindowPadding.x * 2 : ImGui::GetFontSize() * 26;
    float sp = ImGui::GetStyle().ItemSpacing.x * 0.5f;
    float bw = std::max(ImGui::GetFontSize() * 1.4f, (avail - sp * (music::kEqBands - 1)) / music::kEqBands);
    float bh = ImGui::GetFontSize() * (mobile ? 9 : 8);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp, ImGui::GetStyle().ItemSpacing.y));
    for (int k = 0; k < music::kEqBands; k++) {
        if (k) ImGui::SameLine();
        ImGui::PushID(k);
        if (ImGui::VSliderFloat("##b", ImVec2(bw, bh), &s.eq[k], -12, 12, "")) {
            s.preset.clear();
            ch = true;
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetTooltip("%+.1f dB", s.eq[k]);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {  // (double click: back to 0)
            s.eq[k] = 0;
            s.preset.clear();
            ch = true;
        }
        ImGui::PopID();
    }
    for (int k = 0; k < music::kEqBands; k++) {  // the frequencies under the sliders
        if (k) ImGui::SameLine();
        float f = music::kEqFreq[k];
        char lb[16];
        if (f >= 1000) snprintf(lb, sizeof lb, "%gk", f / 1000);
        else snprintf(lb, sizeof lb, "%g", f);
        float tw = ImGui::CalcTextSize(lb).x;
        ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(c.x + (bw - tw) * 0.5f, c.y));
        ImGui::TextDisabled("%s", lb);
        ImGui::SameLine(0, 0);
        ImGui::SetCursorScreenPos(ImVec2(c.x + bw, c.y));
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
    }
    ImGui::PopStyleVar();
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 16);
    ch |= ImGui::SliderFloat("##pre", &s.preamp, -12, 6, T("preamp %+.1f dB"));
    if (!s.eqOn) ImGui::EndDisabled();

    ImGui::SeparatorText(T("Transitions"));
    if (s.remix) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 16);
    ch |= ImGui::SliderInt("##xf", &s.crossfade, 0, 12, s.crossfade ? T("smooth transition: %d s") : T("smooth transition: off"));
    if (s.remix) ImGui::EndDisabled();
    ch |= ImGui::Checkbox(T("Remix: only a part of every song"), &s.remix);
    ImGui::TextDisabled("%s", T("skips the beginnings and ends, flowing from part to part"));
    if (s.remix) {
        const int lens[] = {30, 45, 60, 90};
        ImGui::TextUnformatted(T("Part length"));
        for (int i = 0; i < 4; i++) {
            ImGui::SameLine();
            char b[16];
            snprintf(b, sizeof b, "%d s", lens[i]);
            if (ImGui::RadioButton(b, s.remixLen == lens[i])) {
                s.remixLen = lens[i];
                ch = true;
            }
        }
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 16);
        ch |= ImGui::SliderInt("##rf", &s.remixFade, 1, 12, T("blend: %d s"));
    }
    ImGui::Spacing();
    if (ImGui::Button(T("Close"), ImVec2(mobile ? -1 : 0, h))) ImGui::CloseCurrentPopup();
    if (ch) {
        music::Play().SetSound(s);
        musicSoundDirty = true;
    }
    ImGui::EndPopup();
}

void App::DrawMusic() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    MusicRefresh();
    MusicSendPopup();
    MusicPlaylistPopup();
    MusicSoundPopup();
    MusicListenPopup();
    MusicListenPlaylistPopup();
    DrawListenStrip();
    if (mobile && musicNowShown) {  // phones: "now playing" fills the screen
        if (ui::IconTextButton(T("Library"), Icon::List, ImVec2(0, h))) musicNowShown = false;
        ImGui::BeginChild("mnow");
        ui::TouchScroll();
        DrawMusicNow(true);
        ImGui::EndChild();
        return;
    }
    // the view: songs / artists / albums / genres / folders / playlists
    const char* views[] = {T("Songs"), T("Artists"), T("Albums"), T("Genres"), T("Folders"), T("Playlists")};
    if (!musicGroup.empty()) {
        if (ui::IconTextButton(T("Back"), Icon::Back, ImVec2(0, h))) musicGroup.clear();
        ImGui::SameLine();
    }
    for (int i = 0; i < 6 && musicGroup.empty(); i++) {
        if (i) ImGui::SameLine();
        bool on = musicView == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(views[i], ImVec2(0, h))) {
            musicView = i;
            musicGroup.clear();
        }
        if (on) ImGui::PopStyleColor();
        if (mobile && i == 2) ImGui::NewLine();  // (two rows on phones)
    }
    // search, sort, folders
    ImGui::SetNextItemWidth(mobile ? ImGui::GetContentRegionAvail().x * 0.55f : ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##msearch", T("search"), &musicSearch);
    if (musicView == 0 && musicGroup.empty()) {
        ImGui::SameLine();
        const char* sorts[] = {T("Title"), T("Artist"), T("Album"), T("Recently added")};
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 9);
        if (ImGui::BeginCombo("##msort", sorts[musicSort])) {
            for (int i = 0; i < 4; i++)
                if (ImGui::Selectable(sorts[i], musicSort == i, 0, ImVec2(0, h))) musicSort = i;
            ImGui::EndCombo();
        }
    }
    if (!mobile) {
        ImGui::SameLine();
        if (ui::IconTextButton(T("Folders..."), Icon::Folder)) ImGui::OpenPopup("##mfolders");
        ImGui::SameLine();
        if (ImGui::Button(T("Sound..."))) musicSoundOpen = true;
        if (!eng.listen.sid) {
            ImGui::SameLine();
            if (ImGui::Button(T("Listen together..."))) musicListenOpen = true;
        }
    } else {
        if (ImGui::Button(T("Music folders..."), ImVec2(eng.listen.sid ? -1 : ImGui::GetContentRegionAvail().x * 0.5f, h)))
            ImGui::OpenPopup("##mfolders");
        if (!eng.listen.sid) {
            ImGui::SameLine();
            if (ImGui::Button(T("Listen together..."), ImVec2(-1, h))) musicListenOpen = true;
        }
    }
    if (music::Lib().Scanning()) {
        ImGui::SameLine();
        ImGui::TextDisabled(T("reading new music... %d"), music::Lib().ScanDone());
    }
    static bool devFolders = getenv("LUNASHARE_OPENFOLDERS") != nullptr;  // (dev: screenshots)
    if (devFolders && ImGui::GetFrameCount() > 30) {
        devFolders = false;
        ImGui::OpenPopup("##mfolders");
    }
    {  // (on a phone: across the screen, never taller than it - the buttons first, the list scrolls)
        ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
        if (mobile) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + vp.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + vp.y * 0.12f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0));
            ImGui::SetNextWindowSize(ImVec2(vp.x * 0.94f, 0));
        }
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(vp.x, vp.y * 0.75f));
    }
    if (ImGui::BeginPopup("##mfolders")) {
        ImGui::TextUnformatted(T("Music folders"));
        float half = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f : 0;
        if (ui::IconTextButton(T("Add a folder..."), Icon::Plus, ImVec2(half, h))) {
            browseFor = BrowseFor::MusicFolder;
            browser.Open(FileBrowser::Mode::SelectDir, T("Add a music folder"), music::Library::DefaultMusicDir());
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::IconTextButton(T("Look again"), Icon::Retry, ImVec2(half, h))) music::Lib().Rescan();
        ImGui::Separator();
        auto folders = music::Lib().Folders();
        float rowH = (mobile ? h : ImGui::GetFrameHeight()) + ImGui::GetStyle().ItemSpacing.y;
        float listH = std::min(rowH * (float)folders.size() + ImGui::GetTextLineHeightWithSpacing() * 2,
                               ImGui::GetMainViewport()->WorkSize.y * 0.45f);
        ImGui::BeginChild("##mfl", ImVec2(mobile ? 0 : ImGui::GetFontSize() * 30, listH));
        if (mobile) ui::TouchScroll();
        for (auto& f : folders) {
            ImGui::PushID(f.c_str());
            float rw = ImGui::CalcTextSize(T("Remove")).x + ImGui::GetStyle().FramePadding.x * 2 + (mobile ? h * 0.3f : 0);
            ImGui::AlignTextToFramePadding();
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - rw - ImGui::GetStyle().ItemSpacing.x);
            ImGui::TextUnformatted(f.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - rw);
            if (ImGui::Button(T("Remove"), ImVec2(rw, mobile ? h * 0.8f : 0))) music::Lib().RemoveFolder(f);
            ImGui::PopID();
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled(T("+ received files: %s"), eng.settings.downloadDir.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        if (mobile && ImGui::Button(T("Close"), ImVec2(-1, h))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // selection: send / add to a playlist
    if (!musicSel.empty()) {
        ImGui::TextUnformatted(TF("%d selected", (int)musicSel.size()).c_str());
        ImGui::SameLine();
        std::vector<std::string> sel(musicSel.begin(), musicSel.end());
        if (ImGui::SmallButton(T("Send..."))) MusicSend(sel, "");
        ImGui::SameLine();
        if (eng.listen.sid) {
            if (ImGui::SmallButton(T("Add to listening together"))) {
                MusicListenAdd(sel);
                musicSel.clear();
            }
            ImGui::SameLine();
        }
        if (ImGui::SmallButton(T("To a playlist..."))) {
            musicAddPaths = sel;
            musicAddOpen = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Clear"))) musicSel.clear();
    }

    // the list area (desktop: "now playing" on the right)
    float nowW = mobile ? 0 : std::min(ImGui::GetFontSize() * 18, ImGui::GetContentRegionAvail().x * 0.38f);
    ImGui::BeginChild("mlist", ImVec2(mobile ? 0 : -nowW - ImGui::GetStyle().ItemSpacing.x, 0));
    if (mobile) ui::TouchScroll();
    std::string q = Low(musicSearch);
    auto match = [&](const music::Track& t) {
        return q.empty() || Low(t.title).find(q) != std::string::npos || Low(t.artist).find(q) != std::string::npos ||
               Low(t.album).find(q) != std::string::npos || Low(t.genre).find(q) != std::string::npos;
    };
    if (musicTracks.empty() && !music::Lib().Scanning()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("No music yet. Add the folders with your music - and what people send you lands here by itself."));
        ImGui::PopTextWrapPos();
        std::string def = music::Library::DefaultMusicDir();
        if (IsDirectory(def) && ui::IconTextButton(TF("Add %s", def.c_str()).c_str(), Icon::Plus, ImVec2(mobile ? -1 : 0, h)))
            music::Lib().AddFolder(def);
        if (ui::IconTextButton(T("Add a folder..."), Icon::Folder, ImVec2(mobile ? -1 : 0, h))) {
            browseFor = BrowseFor::MusicFolder;
            browser.Open(FileBrowser::Mode::SelectDir, T("Add a music folder"), music::Library::DefaultMusicDir());
        }
    }
    float rowH = mobile ? ui::M().rowH * 1.1f : ImGui::GetFontSize() * 2.6f;
    // a list of groups (artists, albums...): name, cover, count; tapping opens it
    auto groups = [&](const std::map<std::string, std::vector<const music::Track*>>& g, const char* prefix) {
        ImGuiListClipper clip;
        std::vector<const std::pair<const std::string, std::vector<const music::Track*>>*> items;
        for (auto& kv : g) items.push_back(&kv);
        clip.Begin((int)items.size(), rowH + ImGui::GetStyle().ItemSpacing.y);
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                auto& [name, list] = *items[(size_t)i];
                ImGui::PushID(i);
                ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##g", false, 0, ImVec2(0, rowH))) musicGroup = std::string(prefix) + name;
                if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(1)) ImGui::OpenPopup("##gm");
                if (ImGui::BeginPopup("##gm")) {
                    std::vector<std::string> paths;
                    for (auto* t : list) paths.push_back(t->path);
                    if (ImGui::Selectable(T("Play"))) music::Play().PlayList(paths, 0);
                    if (ImGui::Selectable(T("Send..."))) MusicSend(paths, prefix[0] == 'l' || prefix[0] == 'a' ? name : "");
                    if (ImGui::Selectable(T("To a playlist..."))) {
                        musicAddPaths = paths;
                        musicAddOpen = true;
                    }
                    ImGui::EndPopup();
                }
                std::string cov;
                for (auto* t : list)
                    if (!t->cover.empty()) { cov = t->cover; break; }
                ImDrawList* dl = ImGui::GetWindowDrawList();
                Cover(dl, cov, ImVec2(p.x + 4, p.y + 4), rowH - 8);
                float x = p.x + rowH + 6, lh = ImGui::GetTextLineHeight();
                dl->AddText(ImVec2(x, p.y + rowH * 0.5f - lh - 1), ImGui::GetColorU32(ImGuiCol_Text), name.empty() ? T("(unknown)") : name.c_str());
                std::string sub = TF("%d songs", (int)list.size());
                if (prefix[0] == 'l' && !list.empty() && !list[0]->AlbumArtist().empty()) sub = list[0]->AlbumArtist() + "  ·  " + sub;
                dl->AddText(ImVec2(x, p.y + rowH * 0.5f + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
                ImGui::PopID();
            }
    };
    std::vector<const music::Track*> list;
    if (!musicGroup.empty()) {  // one artist / album / genre / folder / playlist
        char kind = musicGroup[0];
        std::string key = musicGroup.substr(2);
        std::string playlistId, sendFolder;
        if (kind == 'p') {
            for (auto& pl : music::Lib().Playlists())
                if (pl.id == key) {
                    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
                    ImGui::TextUnformatted(pl.name.c_str());
                    ImGui::PopFont();
                    for (auto& p : pl.paths)
                        if (const music::Track* t = MusicTrack(p)) list.push_back(t);
                    playlistId = pl.id;
                    sendFolder = pl.name;
                }
        } else {
            for (auto& t : musicTracks) {
                bool in = kind == 'a' ? t.AlbumArtist() == key : kind == 'l' ? t.album + "\x1f" + t.AlbumArtist() == key
                        : kind == 'g' ? t.genre == key : DirName(t.path) == key;
                if (in && match(t)) list.push_back(&t);
            }
            std::string title = kind == 'l' ? key.substr(0, key.find("\x1f")) : kind == 'f' ? BaseName(key) : key;
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
            ImGui::TextUnformatted(title.empty() ? T("(unknown)") : title.c_str());
            ImGui::PopFont();
            if (kind == 'l' || kind == 'f') {
                std::sort(list.begin(), list.end(), AlbumOrder);
                sendFolder = title;
            } else if (kind == 'a') {
                std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->album != b->album ? Low(a->album) < Low(b->album) : AlbumOrder(a, b); });
                sendFolder = title;
            } else {
                std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return Low(a->title) < Low(b->title); });
            }
        }
        MusicGroupButtons(list, sendFolder);
        if (kind == 'p') {
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Delete playlist"))) {
                music::Lib().DeletePlaylist(playlistId);
                musicGroup.clear();
            }
        }
        MusicTrackList(list, playlistId, kind != 'l');
    } else if (musicView == 0) {
        for (auto& t : musicTracks)
            if (match(t)) list.push_back(&t);
        std::sort(list.begin(), list.end(), [&](const music::Track* a, const music::Track* b) {
            switch (musicSort) {
                case 1: if (Low(a->artist) != Low(b->artist)) return Low(a->artist) < Low(b->artist); break;
                case 2: if (Low(a->album) != Low(b->album)) return Low(a->album) < Low(b->album); return AlbumOrder(a, b);
                case 3: if (a->added != b->added) return a->added > b->added; break;
                default: break;
            }
            return Low(a->title) < Low(b->title);
        });
        if (!list.empty()) MusicGroupButtons(list, "");
        MusicTrackList(list, "", true);
    } else if (musicView == 5) {  // playlists
        if (ImGui::Button(T("New playlist"), ImVec2(mobile ? -1 : 0, h))) {
            musicAddPaths.clear();
            std::string id = music::Lib().CreatePlaylist(TF("Playlist %d", (int)music::Lib().Playlists().size() + 1));
            musicGroup = "p:" + id;
        }
        std::map<std::string, std::vector<const music::Track*>> g;
        std::map<std::string, std::string> ids;
        for (auto& pl : music::Lib().Playlists()) {
            auto& v = g[pl.name];
            for (auto& p : pl.paths)
                if (const music::Track* t = MusicTrack(p)) v.push_back(t);
            ids[pl.name] = pl.id;
        }
        // (groups() opens by name; playlists open by id)
        for (auto& [name, v] : g) {
            ImGui::PushID(name.c_str());
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##pl", false, 0, ImVec2(0, rowH))) musicGroup = "p:" + ids[name];
            std::string cov;
            for (auto* t : v)
                if (!t->cover.empty()) { cov = t->cover; break; }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            Cover(dl, cov, ImVec2(p.x + 4, p.y + 4), rowH - 8);
            float lh = ImGui::GetTextLineHeight();
            dl->AddText(ImVec2(p.x + rowH + 6, p.y + rowH * 0.5f - lh - 1), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
            dl->AddText(ImVec2(p.x + rowH + 6, p.y + rowH * 0.5f + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), TF("%d songs", (int)v.size()).c_str());
            ImGui::PopID();
        }
    } else {
        std::map<std::string, std::vector<const music::Track*>> g;
        for (auto& t : musicTracks) {
            if (!match(t)) continue;
            switch (musicView) {
                case 1: g[t.AlbumArtist()].push_back(&t); break;
                case 2: g[t.album + "\x1f" + t.AlbumArtist()].push_back(&t); break;
                case 3: g[t.genre].push_back(&t); break;
                case 4: g[DirName(t.path)].push_back(&t); break;
            }
        }
        if (musicView == 2) {  // albums: "name  ·  artist" shown, keyed with the artist (two albums can share a name)
            ImGuiListClipper clip;
            std::vector<const std::pair<const std::string, std::vector<const music::Track*>>*> items;
            for (auto& kv : g) items.push_back(&kv);
            clip.Begin((int)items.size(), rowH + ImGui::GetStyle().ItemSpacing.y);
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                    auto& [k, v] = *items[(size_t)i];
                    ImGui::PushID(i);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable("##al", false, 0, ImVec2(0, rowH))) musicGroup = "l:" + k;
                    std::string cov;
                    for (auto* t : v)
                        if (!t->cover.empty()) { cov = t->cover; break; }
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    Cover(dl, cov, ImVec2(p.x + 4, p.y + 4), rowH - 8);
                    float lh = ImGui::GetTextLineHeight();
                    std::string name = k.substr(0, k.find('\x1f')), artist = k.substr(k.find('\x1f') + 1);
                    dl->AddText(ImVec2(p.x + rowH + 6, p.y + rowH * 0.5f - lh - 1), ImGui::GetColorU32(ImGuiCol_Text), name.empty() ? T("(unknown)") : name.c_str());
                    std::string sub = (artist.empty() ? std::string() : artist + "  ·  ") + TF("%d songs", (int)v.size());
                    dl->AddText(ImVec2(p.x + rowH + 6, p.y + rowH * 0.5f + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
                    ImGui::PopID();
                }
        } else {
            groups(g, musicView == 1 ? "a:" : musicView == 3 ? "g:" : "f:");
        }
    }
    ImGui::Dummy(ImVec2(0, h));
    ImGui::EndChild();
    if (!mobile) {
        ImGui::SameLine();
        ImGui::BeginChild("mnowd", ImVec2(0, 0), ImGuiChildFlags_Borders);
        DrawMusicNow(false);
        ImGui::EndChild();
    }
}
