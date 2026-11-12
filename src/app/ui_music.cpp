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
#include "radio.h"
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
    // the same "Send to" as everything else (a chat, or a link for anyone); an album goes in its folder, with its picture
    std::vector<std::pair<std::string, std::string>> files;
    std::set<std::string> extra;
    for (auto& p : paths) {
        files.push_back({p, folder.empty() ? BaseName(p) : folder + "/" + BaseName(p)});
        if (!folder.empty()) {
            const music::Track* t = MusicTrack(p);
            if (t && !t->cover.empty() && DirName(t->cover) == DirName(p)) extra.insert(t->cover);
        }
    }
    for (auto& c : extra) files.push_back({c, folder + "/" + BaseName(c)});
    OpenSendToList(files, folder.empty() ? (paths.size() == 1 ? BaseName(paths[0]) : TF("%d songs", (int)paths.size())) : folder);
    musicPick.Clear();
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
            musicPick.Clear();
        }
    ImGui::Separator();
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##newpl", T("new playlist"), &musicNewName);
    if (!mobile) ImGui::SameLine();
    ImGui::BeginDisabled(musicNewName.empty());
    if (ImGui::Button(T("Create and add"), ImVec2(mobile ? -1 : 0, h))) {
        std::string id = music::Lib().CreatePlaylist(musicNewName);
        music::Lib().AddToPlaylist(id, musicAddPaths);
        musicPick.Clear();
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
    if (ImGui::Selectable(T("Select"), false, 0, ImVec2(0, h))) {
        musicPick.selecting = true;
        musicPick.sel.insert(path);
    }
    if (const music::Track* t = MusicTrack(path)) {
        if (ImGui::Selectable(t->fav ? T("Remove from favourites") : T("Add to favourites"), false, 0, ImVec2(0, h)))
            music::Lib().SetFavourite(path, !t->fav);
        if (t->plays) ImGui::TextDisabled("%s", TF("Played %d times", t->plays).c_str());
    }
    if (!playlistId.empty() && playlistId[0] != '#') {  // its place in the playlist
        ImGui::Separator();
        int n = (int)musicListShown.size();
        if (playlistIndex > 0 && ImGui::Selectable(T("Move up"), false, 0, ImVec2(0, h))) MusicPlaylistMove(playlistId, playlistIndex, playlistIndex - 1);
        if (playlistIndex + 1 < n && ImGui::Selectable(T("Move down"), false, 0, ImVec2(0, h)))
            MusicPlaylistMove(playlistId, playlistIndex, playlistIndex + 1);
        if (playlistIndex > 0 && ImGui::Selectable(T("Move to the top"), false, 0, ImVec2(0, h))) MusicPlaylistMove(playlistId, playlistIndex, 0);
        if (playlistIndex + 1 < n && ImGui::Selectable(T("Move to the bottom"), false, 0, ImVec2(0, h)))
            MusicPlaylistMove(playlistId, playlistIndex, n - 1);
        if (ImGui::Selectable(T("Remove from this playlist"), false, 0, ImVec2(0, h))) MusicPlaylistMove(playlistId, playlistIndex, -1);
    }
}

// a song of a playlist (by its place in the list shown: songs that aren't found any more aren't shown) to another
// place; to = -1: out of the playlist
void App::MusicPlaylistMove(const std::string& id, int from, int to) {
    for (auto& p : music::Lib().Playlists()) {
        if (p.id != id) continue;
        // where the shown songs are in the playlist (in order; the same song twice is fine)
        std::vector<int> at;
        size_t k = 0;
        for (auto& path : musicListShown) {
            while (k < p.paths.size() && p.paths[k] != path) k++;
            if (k >= p.paths.size()) return;  // (the list changed meanwhile)
            at.push_back((int)k++);
        }
        if (from < 0 || from >= (int)at.size()) return;
        auto v = p.paths;
        std::string moved = v[(size_t)at[(size_t)from]];
        v.erase(v.begin() + at[(size_t)from]);
        if (to >= 0 && to < (int)at.size()) {
            int dest = at[(size_t)to];
            if (at[(size_t)to] > at[(size_t)from]) dest--;  // (one fewer before it now)
            if (to > from) dest++;                          // (after the one it was dropped on)
            v.insert(v.begin() + std::clamp(dest, 0, (int)v.size()), moved);
        }
        music::Lib().SetPlaylistPaths(p.id, v);
        return;
    }
}

// rows of songs; tapping one plays the list from there. Returns nothing - it acts right away.
void App::MusicTrackList(const std::vector<const music::Track*>& list, const std::string& playlistId, bool showAlbum) {
    bool mobile = ui::M().mobile;
    bool ordered = !playlistId.empty() && playlistId[0] != '#';  // a playlist: its order can change (drag a song, or its menu)
    musicPickAll.clear();
    for (auto* t : list) musicPickAll.push_back(t->path);
    if (ordered) {
        musicListShown.clear();
        for (auto* t : list) musicListShown.push_back(t->path);
    }
    float rowH = mobile ? ui::M().rowH * 1.1f : ImGui::GetFontSize() * 2.6f;
    float cov = rowH - 8;
    std::string cur = music::Play().GetNow().path;
    bool selecting = musicPick.Active();
    ImGuiListClipper clip;
    clip.Begin((int)list.size(), rowH + ImGui::GetStyle().ItemSpacing.y);
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const music::Track* t = list[(size_t)i];
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool sel = musicPick.sel.count(t->path) > 0;
            bool clicked = ImGui::Selectable("##row", sel || t->path == cur, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowH));
            if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(1)) ImGui::OpenPopup("##trk");
            if (PickItem(musicPick, t->path, clicked)) {
                if (eng.listen.sid && playlistId != "#queue") {  // listening together: to the shared queue
                    MusicListenAdd({t->path});
                } else {
                    std::vector<std::string> paths;
                    for (auto* x : list) paths.push_back(x->path);
                    music::Play().PlayList(paths, i);
                }
            }
            if (!sel && musicPick.sel.count(t->path)) {  // (picked now: its menu from the bar)
                musicPickPl = playlistId;
                musicPickIdx = i;
            }
            if (ordered && !mobile) {  // drag and drop within the playlist
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    ImGui::SetDragDropPayload("LS_PLITEM", &i, sizeof(int));
                    ImGui::TextUnformatted(t->title.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("LS_PLITEM")) MusicPlaylistMove(playlistId, *(const int*)pl->Data, i);
                    ImGui::EndDragDropTarget();
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
            if (t->fav) ui::DrawIcon(dl, Icon::Star, ImVec2(right - dw - 4 - lh * 0.7f, p.y + rowH * 0.5f), lh * 0.75f, IM_COL32(240, 200, 60, 255));
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

// ---------------------------------------------------------------- lyrics: song.lrc next to song.mp3

static void LoadLyrics(const std::string& song, std::vector<std::pair<double, std::string>>& out, bool& synced) {
    out.clear();
    synced = false;
    std::string base = song.substr(0, song.find_last_of('.'));
    std::string path;
    for (const char* ext : {".lrc", ".LRC", ".Lrc"})
        if (FileExists(base + ext)) path = base + ext;
    if (path.empty()) return;
    FILE* f = OpenFile(path, "rb");
    if (!f) return;
    std::string all;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && all.size() < (1 << 20)) all.append(buf, n);
    fclose(f);
    if (all.size() >= 3 && (unsigned char)all[0] == 0xEF && (unsigned char)all[1] == 0xBB && (unsigned char)all[2] == 0xBF) all.erase(0, 3);
    double offset = 0;
    std::vector<std::string> plain;
    size_t a = 0;
    while (a <= all.size()) {
        size_t e = all.find('\n', a);
        if (e == std::string::npos) e = all.size();
        std::string line = all.substr(a, e - a);
        a = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::vector<double> times;
        size_t i = 0;
        while (i < line.size() && line[i] == '[') {  // [mm:ss.xx] (several on one line), or [ar:...] and friends
            size_t c = line.find(']', i);
            if (c == std::string::npos) break;
            std::string tag = line.substr(i + 1, c - i - 1);
            int mm = 0;
            double ss = 0;
            if (sscanf(tag.c_str(), "%d:%lf", &mm, &ss) == 2 && isdigit((unsigned char)tag[0])) times.push_back(mm * 60 + ss);
            else if (tag.rfind("offset:", 0) == 0) offset = atof(tag.c_str() + 7) / 1000.0;
            i = c + 1;
        }
        std::string text = line.substr(i);
        for (double t : times) out.push_back({t, text});
        if (times.empty() && i == 0 && !line.empty()) plain.push_back(line);
    }
    if (!out.empty()) {
        synced = true;
        for (auto& l : out) l.first = std::max(0.0, l.first - offset);  // (positive offset: the lyrics come sooner)
        std::stable_sort(out.begin(), out.end(), [](auto& x, auto& y) { return x.first < y.first; });
    } else {
        for (auto& p : plain) out.push_back({-1, p});
    }
}

// the lyrics in the cover's place: the line being sung in the middle, bright; the others around it, dimmer
void App::DrawLyrics(ImDrawList* dl, ImVec2 a, ImVec2 b, double pos) {
    dl->AddRectFilled(a, b, IM_COL32(25, 27, 34, 255), 8);
    ImFont* f = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * 1.05f, lh = fs * 1.55f, wrap = (b.x - a.x) * 0.9f;
    int cur = -1;
    if (musicLyricsSynced)
        for (int i = 0; i < (int)musicLyrics.size(); i++)
            if (musicLyrics[(size_t)i].first <= pos + 0.15) cur = i;
    // heights (lines can wrap) and where the current one is
    std::vector<float> hs;
    float total = 0, curY = 0;
    for (int i = 0; i < (int)musicLyrics.size(); i++) {
        const std::string& t = musicLyrics[(size_t)i].second;
        float hh = std::max(lh, f->CalcTextSizeA(fs, FLT_MAX, wrap, t.empty() ? " " : t.c_str()).y + lh - fs);
        if (i == cur) curY = total + hh * 0.5f;
        hs.push_back(hh);
        total += hh;
    }
    float target = musicLyricsSynced ? curY - (b.y - a.y) * 0.5f : musicLyricsScroll;
    if (!musicLyricsSynced) {  // (not timed: scrolled by hand)
        if (ImGui::IsItemHovered()) target -= ImGui::GetIO().MouseWheel * lh * 2;
        if (ImGui::IsItemActive()) target -= ImGui::GetIO().MouseDelta.y;
        target = std::clamp(target, 0.0f, std::max(0.0f, total - (b.y - a.y)));
        musicLyricsScroll = target;
    }
    musicLyricsY += (target - musicLyricsY) * std::min(1.0f, ImGui::GetIO().DeltaTime * 8);  // (slides along)
    dl->PushClipRect(a, b, true);
    float y = a.y - musicLyricsY;
    for (int i = 0; i < (int)musicLyrics.size(); i++) {
        float hh = hs[(size_t)i];
        if (y + hh > a.y && y < b.y) {
            const std::string& t = musicLyrics[(size_t)i].second;
            ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, wrap, t.c_str());
            ImU32 col = !musicLyricsSynced ? IM_COL32(225, 225, 230, 255) : i == cur ? IM_COL32(255, 255, 255, 255)
                                                                         : IM_COL32(150, 152, 165, i < cur ? 150 : 210);
            dl->AddText(f, fs, ImVec2((a.x + b.x - ts.x) * 0.5f, y + (hh - ts.y) * 0.5f), col, t.c_str(), nullptr, wrap);
        }
        y += hh;
    }
    dl->PopClipRect();
}

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
    if (musicLyricsFor != n.path) {  // (the lyrics of this song, if it has them)
        musicLyricsFor = n.path;
        LoadLyrics(n.path, musicLyrics, musicLyricsSynced);
        musicLyricsY = musicLyricsScroll = 0;
    }
    ImVec2 c0(p.x + (w - cs) * 0.5f, p.y), c1(c0.x + cs, c0.y + cs);
    ImGui::SetCursorScreenPos(c0);
    ImGui::SetNextItemAllowOverlap();  // (the star on it is a button of its own)
    ImGui::InvisibleButton("##mcover", ImVec2(cs, cs));  // (a tap: the lyrics / the cover)
    if (ImGui::IsItemDeactivated() && !musicLyrics.empty() && std::fabs(ImGui::GetMouseDragDelta(0, 0).y) < 6) musicLyricsOn = !musicLyricsOn;
    if (!musicLyrics.empty() && musicLyricsOn) {
        DrawLyrics(ImGui::GetWindowDrawList(), c0, c1, n.pos);
    } else {
        Cover(ImGui::GetWindowDrawList(), n.track.cover, c0, cs, 8);
        if (!musicLyrics.empty()) {  // (there are lyrics: a tag in the corner)
            const char* lt = T("Lyrics");
            ImVec2 ts = ImGui::CalcTextSize(lt);
            ImVec2 q(c1.x - ts.x - 14, c1.y - ts.y - 12);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(q.x - 6, q.y - 3), ImVec2(c1.x - 6, c1.y - 6), IM_COL32(0, 0, 0, 170), 6);
            ImGui::GetWindowDrawList()->AddText(q, IM_COL32(255, 255, 255, 235), lt);
        }
    }
    if (const music::Track* ft = n.external ? nullptr : MusicTrack(n.path)) {  // a favourite: the star in the corner
        float s = std::max(ImGui::GetFontSize() * 1.6f, cs * 0.12f);
        ImVec2 sp(c0.x + 6, c0.y + 6);
        ImGui::SetCursorScreenPos(sp);
        if (ImGui::InvisibleButton("##mfav", ImVec2(s, s))) music::Lib().SetFavourite(ft->path, !ft->fav);
        if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", ft->fav ? T("Remove from favourites") : T("Add to favourites"));
        ImDrawList* fdl = ImGui::GetWindowDrawList();
        fdl->AddCircleFilled(ImVec2(sp.x + s * 0.5f, sp.y + s * 0.5f), s * 0.5f, IM_COL32(0, 0, 0, 140));
        ui::DrawIcon(fdl, Icon::Star, ImVec2(sp.x + s * 0.5f, sp.y + s * 0.5f), s * 0.6f, ft->fav ? IM_COL32(240, 200, 60, 255) : IM_COL32(255, 255, 255, 120));
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y));
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
    if (n.live && n.loading) ImGui::TextDisabled("%s", n.retries > 0 ? T("can't reach the station - trying again...") : T("connecting..."));
    else if (n.loading) ImGui::TextDisabled("%s", T("on its way..."));
    ImVec2 bp = ImGui::GetCursorScreenPos();
    float bw = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (n.live) {  // internet radio: live, nothing to seek
        ui::Badge(T("LIVE"), n.loading ? ImGui::GetColorU32(ImGuiCol_FrameBgActive) : IM_COL32(200, 60, 80, 255));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Clk(n.pos).c_str());
    } else {
    // seek bar
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
    float cy = bp.y + h * 0.3f;
    dl->AddRectFilled(ImVec2(bp.x, cy - 3), ImVec2(bp.x + bw, cy + 3), ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
    dl->AddRectFilled(ImVec2(bp.x, cy - 3), ImVec2(bp.x + bw * (float)frac, cy + 3), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 3);
    dl->AddCircleFilled(ImVec2(bp.x + bw * (float)frac, cy), mobile ? 10.0f : 7.0f, ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
    ImGui::TextDisabled("%s / %s", Clk(musicDrag >= 0 ? musicDrag * n.dur : n.pos).c_str(), Clk(n.dur).c_str());
    }
    // the buttons (radio: previous / next station; no shuffle or repeat)
    float sp = ImGui::GetStyle().ItemSpacing.x;
    float bs = std::min(h * 1.1f, (bw - sp * 4) / 5);
    float total = bs * 5 + sp * 4;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (bw - total) * 0.5f));
    bool together = n.follow && eng.listen.sid;  // listening together: shuffle and repeat are everyone's
    bool sh = together ? eng.listen.shuffle : n.shuffle;
    ImGui::BeginDisabled(n.live);
    ui::PushAccent(sh);
    if (ui::IconButton("mshuf", Icon::Shuffle, sh ? T("Shuffle: on") : T("Shuffle: off"), bs)) {
        if (together) eng.ListenControl("shuffle", sh ? 0 : 1);
        else music::Play().SetShuffle(!sh);
    }
    ui::PopAccent();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::IconButton("mprev", Icon::Back, n.live ? T("Previous station") : T("Previous"), bs)) music::Play().Prev();
    ImGui::SameLine();
    if (ui::IconButton("mplay", n.playing ? Icon::Pause : Icon::Play, n.playing ? T("Pause") : T("Play"), bs)) music::Play().Toggle();
    ImGui::SameLine();
    // "next": the back arrow turned around
    ImVec2 np = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##mnext", ImVec2(bs, bs))) music::Play().Next();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", n.live ? T("Next station") : T("Next"));
    {
        ImVec2 c(np.x + bs * 0.5f, np.y + bs * 0.5f);
        float s = bs * 0.22f;
        ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        dl->AddLine(ImVec2(c.x - s * 0.6f, c.y - s), ImVec2(c.x + s * 0.4f, c.y), col, 2.5f);
        dl->AddLine(ImVec2(c.x + s * 0.4f, c.y), ImVec2(c.x - s * 0.6f, c.y + s), col, 2.5f);
    }
    ImGui::SameLine();
    int rp = together ? eng.listen.repeat : n.repeat;
    ui::PushAccent(rp);
    const char* rt = rp == music::RepeatOne ? T("Repeat: this song") : rp == music::RepeatAll ? T("Repeat: the list") : T("Repeat: off");
    ImVec2 rpp = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(n.live);
    if (ui::IconButton("mrep", Icon::Loop, rt, bs)) {
        if (together) eng.ListenControl("repeat", (rp + 1) % 3);
        else music::Play().SetRepeat((rp + 1) % 3);
    }
    if (rp == music::RepeatOne) dl->AddText(ImVec2(rpp.x + bs * 0.68f, rpp.y + bs * 0.05f), ImGui::GetColorU32(ImGuiCol_Text), "1");
    ui::PopAccent();
    ImGui::EndDisabled();
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
        } else if (n.live && radio::R().Active()) {  // radio: the station as a favourite
            canSave = true;
            radio::Station st = radio::R().Current();
            bool fav = radio::R().IsFavourite(st.id);
            ui::PushAccent(fav);
            if (ImGui::Button(fav ? T("Saved") : T("Save"), ImVec2(aw, ah))) radio::R().SetFavourite(st, !fav);
            ui::PopAccent();
            if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", fav ? T("Remove from favourites") : T("Add to favourites"));
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
        if (ImGui::Button(T("Listen together..."), ImVec2((ImGui::GetContentRegionAvail().x - sp) / 2, ah))) musicListenOpen = true;
        ImGui::SameLine();
        if (ImGui::Button(SleepLabel().c_str(), ImVec2(-1, ah))) sleepOpen = true;
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
    std::string under = n.track.artist;
    if (n.live) under = n.loading ? std::string(n.retries > 0 ? T("trying again...") : T("connecting...")) : under.empty() ? std::string(T("live radio")) : under;
    dl->AddText(nullptr, 0, ImVec2(tx, p.y + h * 0.5f + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), under.c_str(), nullptr, 0, &clipR);
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
    ch |= ui::Checkbox(T("On"), &s.eqOn);
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

    ImGui::SeparatorText(T("Volume"));
    ch |= ui::Checkbox(T("Even out the volume between songs"), &s.level);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", T("Uses the songs' ReplayGain; songs without it are measured the first time they play through."));
    ImGui::PopTextWrapPos();
    ImGui::SeparatorText(T("Transitions"));
    if (eng.listen.sid) {  // listening together: the transitions are everyone's
        ImGui::TextDisabled("%s", T("listening together: for everyone"));
        ListenInfo& l = eng.listen;
        int xf = l.crossfade, rl = l.remixLen, rf = l.remixFade;
        bool rx = l.remix;
        if (rx) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 16);
        if (ImGui::SliderInt("##lxf", &xf, 0, 12, xf ? T("smooth transition: %d s") : T("smooth transition: off")))
            l.crossfade = xf;  // (shown at once; sent when let go)
        if (ImGui::IsItemDeactivatedAfterEdit()) eng.ListenControl("crossfade", l.crossfade);
        if (rx) ImGui::EndDisabled();
        if (ui::Checkbox(T("Remix: only a part of every song"), &rx)) eng.ListenControl("remix", rx ? 1 : 0);
        ImGui::TextDisabled("%s", T("skips the beginnings and ends, flowing from part to part"));
        if (l.remix) {
            const int lens[] = {30, 45, 60, 90};
            ImGui::TextUnformatted(T("Part length"));
            for (int i = 0; i < 4; i++) {
                ImGui::SameLine();
                char b[16];
                snprintf(b, sizeof b, "%d s##l", lens[i]);
                if (ImGui::RadioButton(b, rl == lens[i])) eng.ListenControl("remixlen", lens[i]);
            }
            ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 16);
            if (ImGui::SliderInt("##lrf", &rf, 1, 12, T("blend: %d s"))) l.remixFade = rf;
            if (ImGui::IsItemDeactivatedAfterEdit()) eng.ListenControl("remixfade", l.remixFade);
        }
        ImGui::Spacing();
        if (ImGui::Button(T("Close"), ImVec2(mobile ? -1 : 0, h))) ImGui::CloseCurrentPopup();
        if (ch) {
            music::Play().SetSound(s);
            musicSoundDirty = true;
        }
        ImGui::EndPopup();
        return;
    }
    if (s.remix) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 16);
    ch |= ImGui::SliderInt("##xf", &s.crossfade, 0, 12, s.crossfade ? T("smooth transition: %d s") : T("smooth transition: off"));
    if (s.crossfade == 0) {
        ch |= ui::Checkbox(T("No gap between songs"), &s.gapless);
        ImGui::SameLine();
        ui::HelpMarker(T("The next song is ready a few seconds early and starts the very moment this one ends - albums recorded "
                         "as one piece (live, mixes) play on without a click."));
    }
    if (s.remix) ImGui::EndDisabled();
    ch |= ui::Checkbox(T("Remix: only a part of every song"), &s.remix);
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
    // the view: songs / artists / albums / genres / folders / playlists / internet radio
    const char* views[] = {T("Songs"), T("Artists"), T("Albums"), T("Genres"), T("Folders"), T("Playlists"), T("Radio")};
    if (!musicGroup.empty()) {
        if (ui::IconTextButton(T("Back"), Icon::Back, ImVec2(0, h))) musicGroup.clear();
        ImGui::SameLine();
    }
    for (int i = 0; i < 7 && musicGroup.empty(); i++) {
        if (i) ImGui::SameLine();
        bool on = musicView == i;
        ui::PushAccent(on);
        if (ImGui::Button(views[i], ImVec2(0, h))) {
            musicView = i;
            musicGroup.clear();
        }
        ui::PopAccent();
        if (mobile && i == 3) ImGui::NewLine();  // (two rows on phones)
    }
    if (musicView == 6 && musicGroup.empty()) {
        DrawRadio();
        return;
    }
    // search, sort, folders
    ImGui::SetNextItemWidth(mobile ? ImGui::GetContentRegionAvail().x * 0.55f : ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##msearch", T("search"), &musicSearch);
    if (musicView == 0 && musicGroup.empty()) {
        ImGui::SameLine();
        const char* sorts[] = {T("Title"), T("Artist"), T("Album"), T("Recently added"), T("Most played")};
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 9);
        if (ImGui::BeginCombo("##msort", sorts[musicSort])) {
            for (int i = 0; i < 5; i++)
                if (ImGui::Selectable(sorts[i], musicSort == i, 0, ImVec2(0, h))) musicSort = i;
            ImGui::EndCombo();
        }
    }
    if (!mobile) {
        ImGui::SameLine();
        if (ui::IconTextButton(T("Folders..."), Icon::Folder)) ImGui::OpenPopup("##mfolders");
        ImGui::SameLine();
        if (ImGui::Button(T("Sound..."))) musicSoundOpen = true;
        ImGui::SameLine();
        if (ImGui::Button(SleepLabel().c_str())) sleepOpen = true;
        ImGui::SameLine();
        if (ui::Checkbox(T("Select"), &musicPick.selecting) && !musicPick.selecting) musicPick.sel.clear();
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
    // picked songs: send (to a chat or as a link), pack, delete; to a playlist / listening together; one: its menu
    if (musicPick.Active()) {
        std::vector<std::string> sel(musicPick.sel.begin(), musicPick.sel.end());
        float h2 = mobile ? ui::M().rowH * 0.8f : 0;
        DrawPickBar(musicPick, musicPickAll, [&] {
            ImGui::BeginDisabled(sel.empty());
            if (ui::IconTextButton(T("To a playlist..."), Icon::List, ImVec2(0, h2))) {
                musicAddPaths = sel;
                musicAddOpen = true;
            }
            if (eng.listen.sid) {
                ImGui::SameLine();
                if (ImGui::Button(T("Add to listening together"), ImVec2(0, h2))) {
                    MusicListenAdd(sel);
                    musicPick.Clear();
                }
            }
            if (sel.size() == 1) {
                ImGui::SameLine();
                if (ImGui::Button(T("More..."), ImVec2(0, h2))) ImGui::OpenPopup("##pickmenu");
                if (ImGui::BeginPopup("##pickmenu")) {
                    MusicTrackMenu(sel[0], musicPickPl, musicPickIdx);
                    ImGui::EndPopup();
                }
            }
            ImGui::EndDisabled();
        }, [] { music::Lib().Rescan(); });
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
        if (kind == 's') {  // the lists made by themselves
            std::string title;
            for (auto& t : musicTracks)
                if (match(t) && (key == "fav" ? t.fav : key == "top" ? t.plays > 0 : key == "recent" ? t.lastPlayed > 0 : true)) list.push_back(&t);
            if (key == "fav") {
                title = T("Favourites");
                std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return Low(a->title) < Low(b->title); });
            } else if (key == "top") {
                title = T("Most played");
                std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->plays > b->plays; });
                if (list.size() > 100) list.resize(100);
            } else if (key == "recent") {
                title = T("Played lately");
                std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->lastPlayed > b->lastPlayed; });
                if (list.size() > 100) list.resize(100);
            } else {
                title = T("Recently added");
                std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->added > b->added; });
                if (list.size() > 100) list.resize(100);
            }
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
            ImGui::TextUnformatted(title.c_str());
            ImGui::PopFont();
            sendFolder = title;
        } else if (kind == 'p') {
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
                case 4: if (a->plays != b->plays) return a->plays > b->plays; break;
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
        {  // the lists made by themselves: favourites, most played, played lately, recently added
            int fav = 0, top = 0, recent = 0;
            for (auto& t : musicTracks) {
                fav += t.fav;
                top += t.plays > 0;
                recent += t.lastPlayed > 0;
            }
            struct Smart {
                const char* key;
                std::string name;
                int n;
            } smart[] = {{"fav", T("Favourites"), fav}, {"top", T("Most played"), std::min(top, 100)},
                         {"recent", T("Played lately"), std::min(recent, 100)}, {"new", T("Recently added"), std::min((int)musicTracks.size(), 100)}};
            for (auto& s : smart) {
                if (s.n == 0) continue;
                ImGui::PushID(s.key);
                ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##sm", false, 0, ImVec2(0, rowH))) musicGroup = std::string("s:") + s.key;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(ImVec2(p.x + 4, p.y + 4), ImVec2(p.x + rowH - 4, p.y + rowH - 4), ImGui::GetColorU32(ImGuiCol_FrameBgActive), 4);
                ui::DrawIcon(dl, std::string(s.key) == "fav" ? Icon::Star : std::string(s.key) == "new" ? Icon::Plus : Icon::Play,
                             ImVec2(p.x + rowH * 0.5f, p.y + rowH * 0.5f), rowH * 0.4f,
                             std::string(s.key) == "fav" ? IM_COL32(240, 200, 60, 255) : ImGui::GetColorU32(ImGuiCol_Text));
                float lh = ImGui::GetTextLineHeight();
                dl->AddText(ImVec2(p.x + rowH + 6, p.y + rowH * 0.5f - lh - 1), ImGui::GetColorU32(ImGuiCol_Text), s.name.c_str());
                dl->AddText(ImVec2(p.x + rowH + 6, p.y + rowH * 0.5f + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), TF("%d songs", s.n).c_str());
                ImGui::PopID();
            }
            ImGui::Separator();
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
