// LunaShare - the Gallery tab (a screen on phones): the pictures and videos of the camera, Pictures, received files and
// the folders you add - by month (when they were taken: the photo's EXIF date, else the file's) or by album (folder).
// Tapping one opens the picture viewer with the rest of that list (videos play in the movie player); a long press (or
// Select) picks several to send, put in an archive or delete. Phones: the camera can be sent to one of my computers by
// itself (a folder sync). Folders are looked through on a thread; the photo dates are kept in gallery_dates.json.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>

#include "emoji_host.h"
#include "i18n.h"
#include "image_cache.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "platform.h"
#include "player_host.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using json = nlohmann::json;
using ui::Icon;

namespace {
struct Pic {
    std::string path, name, dir;
    int64_t size = 0, mtime = 0, date = 0;  // date: taken (ms), else mtime
    bool video = false;
};
struct GalleryScan {
    std::mutex m;
    std::thread th;
    std::atomic<bool> busy{false};
    std::vector<Pic> pics;
    uint64_t ver = 0;
    ~GalleryScan() {
        if (th.joinable()) th.join();
    }
};
GalleryScan& Scan() {
    static GalleryScan s;
    return s;
}

std::string Low(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool IsJpeg(const std::string& n) {
    std::string l = Low(n);
    return l.size() > 4 && (l.compare(l.size() - 4, 4, ".jpg") == 0 || l.compare(l.size() - 5, 5, ".jpeg") == 0);
}

void Walk(const std::string& dir, int depth, bool videos, std::vector<Pic>& out) {
    if (depth > 8 || out.size() > 50000) return;
    for (auto& e : ListDir(dir)) {
        if (e.name.empty() || e.name[0] == '.') continue;
        std::string p = JoinPath(dir, e.name);
        if (e.isDir) {
            std::string l = Low(e.name);
            if (l == "thumbnails" || l == "cache" || l == "android") continue;  // (made by apps, not pictures of yours)
            Walk(p, depth + 1, videos, out);
        } else if (ImageCache::IsImagePath(e.name) || (videos && PlayerHost::IsVideoPath(e.name))) {
            Pic pc;
            pc.path = p;
            pc.name = e.name;
            pc.dir = dir;
            pc.size = e.size;
            pc.mtime = e.mtime;
            pc.date = e.mtime * 1000;  // (seconds -> ms)
            pc.video = PlayerHost::IsVideoPath(e.name);
            out.push_back(std::move(pc));
        }
    }
}

const char* MonthName(int m) {
    static const char* n[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    return T(n[std::clamp(m, 0, 11)]);
}
// i18n-keys: "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"

std::string MonthOf(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return std::string(MonthName(lt.tm_mon)) + " " + std::to_string(lt.tm_year + 1900);
}

#if defined(__ANDROID__)
const char* kCamera = "/storage/emulated/0/DCIM/Camera";
#endif

// what the tab shows, worked out when the pictures, the search or the album change - not every frame (a phone has
// thousands of photos). UI thread only.
struct View {
    uint64_t ver = ~0ull;
    std::string q, album;
    std::vector<Pic> pics;         // (a copy of the scan at ver)
    std::vector<const Pic*> list;  // searched / of the album, newest first
    struct Month {
        std::string label;
        size_t first = 0, count = 0;
    };
    std::vector<Month> months;
    struct Album {
        std::string dir;
        const Pic* cover = nullptr;
        int count = 0;
    };
    std::vector<Album> albums;  // newest first
};
View g_view;

void Rebuild(View& v, const std::string& q, const std::string& album) {
    v.q = q;
    v.album = album;
    v.list.clear();
    v.months.clear();
    v.albums.clear();
    for (auto& p : v.pics) {
        if (!album.empty() && p.dir != album) continue;
        if (!q.empty() && Low(p.name).find(q) == std::string::npos && Low(p.dir).find(q) == std::string::npos) continue;
        v.list.push_back(&p);
    }
    std::stable_sort(v.list.begin(), v.list.end(), [](const Pic* a, const Pic* b) { return a->date > b->date; });
    int curKey = -1;
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < v.list.size(); i++) {
        const Pic* p = v.list[i];
        time_t t = (time_t)(p->date / 1000);
        struct tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        int key = lt.tm_year * 12 + lt.tm_mon;
        if (key != curKey || v.months.empty()) {
            curKey = key;
            v.months.push_back({MonthOf(p->date), i, 0});
        }
        v.months.back().count++;
        auto it = at.find(p->dir);
        if (it == at.end()) {
            at[p->dir] = v.albums.size();
            v.albums.push_back({p->dir, p, 1});  // (the list is newest first: the newest picture is the cover)
        } else {
            v.albums[it->second].count++;
        }
    }
    std::stable_sort(v.albums.begin(), v.albums.end(), [](const View::Album& a, const View::Album& b) { return a.cover->date > b.cover->date; });
}
}  // namespace

// the folders: the camera and Pictures, received files, and mine
static std::vector<std::string> GalleryFolders(const Settings& s) {
    std::vector<std::string> v;
    auto add = [&](const std::string& d) {
        if (!d.empty() && std::find(v.begin(), v.end(), d) == v.end()) v.push_back(d);
    };
#if defined(__ANDROID__)
    add("/storage/emulated/0/DCIM");
    add("/storage/emulated/0/Pictures");
#else
    add(JoinPath(HomeDir(), "Pictures"));
#endif
    add(s.downloadDir);
    for (auto& f : s.galleryFolders) add(f);
    return v;
}

void App::GalleryRescan() {
    auto& sc = Scan();
    if (sc.busy) {
        galleryRescanWanted = true;
        return;
    }
    if (sc.th.joinable()) sc.th.join();
    std::vector<std::string> folders = GalleryFolders(eng.settings);
    bool videos = eng.settings.galleryVideos;
    std::string cfg = eng.ConfigPath();
    sc.busy = true;
    sc.th = std::thread([folders, videos, cfg] {
        std::vector<Pic> out;
        for (auto& f : folders) Walk(f, 0, videos, out);
        std::sort(out.begin(), out.end(), [](const Pic& a, const Pic& b) { return a.path < b.path; });
        out.erase(std::unique(out.begin(), out.end(), [](const Pic& a, const Pic& b) { return a.path == b.path; }), out.end());
        // when they were taken: known ones from the file of dates, the rest read (only the start of each photo)
        std::string datesPath = JoinPath(cfg, "gallery_dates.json");
        json dates;
        if (!LoadJsonFile(datesPath, dates) || !dates.is_object()) dates = json::object();
        json kept = json::object();
        bool changed = false;
        auto publish = [&] {
            auto& s = Scan();
            std::lock_guard<std::mutex> l(s.m);
            s.pics = out;
            s.ver++;
        };
        publish();  // (the list right away; dates follow)
        int64_t lastPub = SteadyMs();
        for (auto& p : out) {
            if (!IsJpeg(p.name)) continue;
            std::string stamp = std::to_string(p.size) + ":" + std::to_string(p.mtime);
            auto it = dates.find(p.path);
            int64_t d = 0;
            if (it != dates.end() && it->is_array() && it->size() == 2 && (*it)[0] == stamp && (*it)[1].is_number_integer()) {
                d = (*it)[1].get<int64_t>();
            } else {
                d = PhotoDateMs(p.path);
                changed = true;
            }
            kept[p.path] = json::array({stamp, d});
            if (d > 0) p.date = d;
            if (SteadyMs() - lastPub > 1500) {
                publish();
                lastPub = SteadyMs();
            }
        }
        if (changed || kept.size() != dates.size()) SaveJsonFile(datesPath, kept);
        publish();
        Scan().busy = false;
    });
}

void App::OpenGallery() {
    if (ui::M().mobile) {
        if (screen != MScreen::Gallery) backStack.push_back(screen);
        screen = MScreen::Gallery;
    } else {
        tab = Tab::Gallery;
        tabForce = true;
    }
}

// phones: the camera folder to one of my computers by itself (a folder sync)
void App::GalleryBackupPopup() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    if (mobile) {
        ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + vp.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + vp.y * 0.2f),
                                ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowSize(ImVec2(vp.x * 0.94f, 0));
    }
    if (!ImGui::BeginPopup("##galbackup")) return;
#if defined(__ANDROID__)
    std::string cam = kCamera;
#else
    std::string cam = JoinPath(HomeDir(), "Pictures");
#endif
    ImGui::TextUnformatted(T("Back up the camera"));
    ImGui::PushTextWrapPos(mobile ? 0 : ImGui::GetFontSize() * 26);
    ImGui::TextDisabled("%s", T("New photos and videos go to a device of yours by themselves (into its download folder) - "
                                "checked every minute and when it comes online. Nothing is deleted there."));
    ImGui::PopTextWrapPos();
    ImGui::TextDisabled("%s", cam.c_str());
    Settings s = eng.settings;
    int at = -1;
    for (size_t i = 0; i < s.folderSyncs.size(); i++)
        if (s.folderSyncs[i].path == cam) at = (int)i;
    auto mine = eng.MyOtherDevices();
    if (mine.empty()) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", T("No other devices linked to your account yet."));
    for (auto& d : mine) {
        bool on = at >= 0 && s.folderSyncs[(size_t)at].on && s.folderSyncs[(size_t)at].to == d;
        if (ImGui::Selectable(TF("-> %s", Emoji().Display(eng.PeerName(d)).c_str()).c_str(), on, 0, ImVec2(0, h))) {
            if (on) s.folderSyncs.erase(s.folderSyncs.begin() + at);
            else if (at >= 0) {
                s.folderSyncs[(size_t)at].to = d;
                s.folderSyncs[(size_t)at].on = true;
            } else {
                s.folderSyncs.push_back({RandomHex(6), cam, d, true});
            }
            eng.ApplySettings(s);
            edit.folderSyncs = s.folderSyncs;
            eng.FolderSyncSoon();
        }
    }
    if (at >= 0) {
        std::string st = eng.FolderSyncStatus(s.folderSyncs[(size_t)at].id);
        int sent = std::atoi(st.c_str()), way = std::atoi(st.c_str() + st.find('|') + 1);
        ImGui::TextDisabled("%s", way ? TF("%d sent, %d on the way", sent, way).c_str() : TF("%d sent", sent).c_str());
    }
    if (mobile && ImGui::Button(T("Close"), ImVec2(-1, h))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::DrawGallery() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    auto& sc = Scan();
    if (!galleryScanned || (galleryRescanWanted && !sc.busy)) {
        galleryScanned = true;
        galleryRescanWanted = false;
        GalleryRescan();
    }
    // the bar: back (from an album), by month / albums, folders, search, select
    if (!galleryAlbum.empty()) {
        if (ui::IconTextButton(T("Albums"), Icon::Back, ImVec2(0, h))) {
            galleryAlbum.clear();
            galleryPick.sel.clear();
            galleryPick.selecting = false;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(BaseName(galleryAlbum).c_str());
        if (!mobile) ImGui::SameLine();
    } else {
        const char* views[] = {T("By month"), T("Albums")};
        for (int i = 0; i < 2; i++) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(views[i], galleryView == i)) galleryView = i;
        }
        ImGui::SameLine();
    }
    if (ui::IconTextButton(T("Folders..."), Icon::Folder, ImVec2(0, h))) ImGui::OpenPopup("##galfolders");
    ImGui::SameLine();
    if (IsMobilePlatform()) {
        if (ui::IconTextButton(T("Back up..."), Icon::Transfers, ImVec2(0, h))) ImGui::OpenPopup("##galbackup");
        ImGui::SameLine();
    }
    if (mobile) ImGui::NewLine();
    ImGui::SetNextItemWidth(mobile ? ImGui::GetContentRegionAvail().x * 0.6f : ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##galsearch", T("search"), &gallerySearch);
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Select"), &galleryPick.selecting) && !galleryPick.selecting) galleryPick.sel.clear();
    if (sc.busy) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("looking through the folders..."));
    }
    GalleryBackupPopup();
    // folders popup
    if (mobile) {
        ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + vp.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + vp.y * 0.12f),
                                ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowSize(ImVec2(vp.x * 0.94f, 0));
    }
    if (ImGui::BeginPopup("##galfolders")) {
        ImGui::TextUnformatted(T("Gallery folders"));
        float half = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f : 0;
        if (ui::IconTextButton(T("Add a folder..."), Icon::Plus, ImVec2(half, h))) {
            browseFor = BrowseFor::GalleryFolder;
            browser.Open(FileBrowser::Mode::SelectDir, T("Add a gallery folder"), browser.lastDir);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::IconTextButton(T("Look again"), Icon::Retry, ImVec2(half, h))) GalleryRescan();
        bool vids = eng.settings.galleryVideos;
        if (ImGui::Checkbox(T("Videos too"), &vids)) {
            Settings s = eng.GetSettings();
            s.galleryVideos = vids;
            eng.ApplySettings(s);
            edit.galleryVideos = vids;
            GalleryRescan();
        }
        ImGui::Separator();
        std::string remove;
        for (auto& f : eng.settings.galleryFolders) {
            ImGui::PushID(f.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(f.c_str());
            ImGui::SameLine();
            if (ImGui::Button(T("Remove"), ImVec2(0, mobile ? h * 0.8f : 0))) remove = f;
            ImGui::PopID();
        }
        Settings none = eng.settings;
        none.galleryFolders.clear();
        for (auto& d : GalleryFolders(none)) ImGui::TextDisabled(T("+ always: %s"), d.c_str());
        if (!remove.empty()) {
            Settings s = eng.GetSettings();
            s.galleryFolders.erase(std::remove(s.galleryFolders.begin(), s.galleryFolders.end(), remove), s.galleryFolders.end());
            eng.ApplySettings(s);
            edit.galleryFolders = s.galleryFolders;
            GalleryRescan();
        }
        if (mobile && ImGui::Button(T("Close"), ImVec2(-1, h))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // what's shown: searched, the album (if one is open), newest first
    std::string q = Low(gallerySearch);
    {
        bool fresh = false;
        {
            std::lock_guard<std::mutex> l(sc.m);
            if (sc.ver != g_view.ver) {
                g_view.pics = sc.pics;
                g_view.ver = sc.ver;
                fresh = true;
            }
        }
        if (fresh || q != g_view.q || galleryAlbum != g_view.album) Rebuild(g_view, q, galleryAlbum);
    }
    const std::vector<const Pic*>& list = g_view.list;

    // picked: what to do with them
    if (galleryPick.Active()) {
        std::vector<std::string> all;
        for (auto* p : list) all.push_back(p->path);
        DrawPickBar(galleryPick, all, nullptr, [this] { GalleryRescan(); });
    }

    ImGui::BeginChild("gallery");
    if (mobile) ui::TouchScroll();
    if (list.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", sc.busy ? T("looking through the folders...")
                           : q.empty() ? T("No pictures in your folders yet. Add a folder with Folders...") : T("Nothing matches."));
        ImGui::PopTextWrapPos();
    }
    float avail = ImGui::GetContentRegionAvail().x;
    float sp = mobile ? 3.0f : ImGui::GetStyle().ItemSpacing.x * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto tile = [&](const Pic& p, float side, bool* tapped, bool* held) {
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + side, a.y + side);
        *tapped = ImGui::InvisibleButton("##t", ImVec2(side, side));
        bool hov = ImGui::IsItemHovered();
        *held = ImGui::IsItemActive() && ImGui::GetIO().MouseDownDuration[0] >= 0.45f && !ImGui::IsMouseDragging(0, 8.0f);
        if (ImGui::IsItemClicked() && ImGui::GetIO().KeyCtrl) *held = true;  // (desktops: Ctrl+click picks)
        if (!ImGui::IsRectVisible(a, b)) return;
        dl->AddRectFilled(a, b, ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg));
        ImTextureID id = 0;
        float tw = 0, th = 0;
        if (p.video) {
            const auto& m = Player().GetMeta(p.path);
            if (m.tex) id = (ImTextureID)(intptr_t)m.tex, tw = (float)m.tw, th = (float)m.th;
        } else {
            const ImageTex& t = Images().Get(p.path, ImageCache::Size::Tile);
            if (t.state == ImageTex::Ready) id = t.Id(), tw = (float)t.w, th = (float)t.h;
        }
        if (id && tw > 0 && th > 0) {  // (cropped to a square, like every gallery)
            float s = std::max(side / tw, side / th);
            float cw = side / (tw * s), ch = side / (th * s);
            ImVec2 uv0((1 - cw) * 0.5f, (1 - ch) * 0.5f), uv1(uv0.x + cw, uv0.y + ch);
            dl->AddImage(id, a, b, uv0, uv1);
        } else {
            ui::DrawIcon(dl, p.video ? Icon::Video : Icon::Image, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), side * 0.3f, ui::ColMuted());
        }
        if (p.video) {
            float r = side * 0.13f;
            ImVec2 c(b.x - r - 4, b.y - r - 4);
            dl->AddCircleFilled(c, r, IM_COL32(0, 0, 0, 150));
            ui::DrawIcon(dl, Icon::Play, ImVec2(c.x + r * 0.1f, c.y), r * 1.1f, IM_COL32_WHITE);
        }
        if (galleryPick.sel.count(p.path)) {
            dl->AddRectFilled(a, b, IM_COL32(60, 110, 220, 90));
            dl->AddRect(a, b, IM_COL32(90, 150, 255, 255), 0.0f, 0, 3.0f);
            float r = side * 0.11f;
            ImVec2 c(a.x + r + 4, a.y + r + 4);
            dl->AddCircleFilled(c, r, IM_COL32(60, 120, 230, 255));
            ui::DrawIcon(dl, Icon::Check, c, r * 1.5f, IM_COL32_WHITE);
        }
        if (hov && !mobile) ImGui::SetTooltip("%s\n%s  %s", p.name.c_str(), FormatTime(p.date).c_str(), FormatBytes((double)p.size).c_str());
    };
    auto openAt = [&](const std::vector<const Pic*>& l, size_t i) {
        std::vector<std::string> paths, caps;
        for (auto* p : l) {
            paths.push_back(p->path);
            caps.push_back(FormatTime(p->date) + "  -  " + BaseName(p->dir));
        }
        OpenViewerList(paths, (int)i, caps);
        viewer.canDelete = true;
    };
    auto handle = [&](const std::vector<const Pic*>& l, size_t i, bool tapped) {
        if (PickItem(galleryPick, l[i]->path, tapped)) openAt(l, i);
    };

    if (galleryView == 1 && galleryAlbum.empty()) {
        // albums: a folder each, newest first, the newest picture on the cover
        std::vector<const View::Album*> al;
        for (auto& a : g_view.albums) al.push_back(&a);
        int cols = std::max(2, (int)((avail + sp) / (ImGui::GetFontSize() * (mobile ? 8.0f : 10.0f) + sp)));
        float side = (avail - sp * (cols - 1)) / cols;
        for (size_t i = 0; i < al.size(); i++) {
            if (i % cols) ImGui::SameLine(0, sp);
            ImGui::PushID((int)i);
            ImGui::BeginGroup();
            bool tapped = false, held = false;
            tile(*al[i]->cover, side, &tapped, &held);
            if (tapped) {
                galleryAlbum = al[i]->dir;  // (the view is rebuilt at the next frame: nothing of it is used after this)
                galleryPick.sel.clear();
                ImGui::EndGroup();
                ImGui::PopID();
                break;
            }
            ImVec2 tp = ImGui::GetCursorScreenPos();
            ImVec4 clip(tp.x, tp.y, tp.x + side, tp.y + ImGui::GetTextLineHeight() * 2.2f);
            dl->AddText(nullptr, 0, tp, ImGui::GetColorU32(ImGuiCol_Text), BaseName(al[i]->dir).c_str(), nullptr, side, &clip);
            ImGui::Dummy(ImVec2(side, ImGui::GetTextLineHeight()));
            ImGui::TextDisabled("%d", al[i]->count);
            ImGui::EndGroup();
            ImGui::PopID();
        }
    } else {
        // a grid, a header per month (or the album's pictures)
        int cols = std::max(3, (int)((avail + sp) / (ImGui::GetFontSize() * (mobile ? 5.5f : 7.5f) + sp)));
        float side = (avail - sp * (cols - 1)) / cols;
        // only the rows in view are drawn (the rest is empty space of the same height)
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp, sp));
        float hH = ImGui::GetTextLineHeight() * 1.7f, rowH = side + sp;
        float top = ImGui::GetScrollY() - ImGui::GetCursorPosY(), bottom = top + ImGui::GetWindowHeight();
        float y = 0, skipped = 0;
        auto flush = [&] {
            if (skipped > 0) ImGui::Dummy(ImVec2(1, skipped - sp));
            skipped = 0;
        };
        for (auto& m : g_view.months) {
            if (y + hH + sp > top && y < bottom) {
                flush();
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(avail, hH));
                float ty = p.y + (hH - ImGui::GetTextLineHeight()) * 0.5f;
                dl->AddText(ImVec2(p.x + 2, ty), ImGui::GetColorU32(ImGuiCol_Text), m.label.c_str());
                float tw = ImGui::CalcTextSize(m.label.c_str()).x;
                dl->AddLine(ImVec2(p.x + tw + 12, p.y + hH * 0.5f), ImVec2(p.x + avail, p.y + hH * 0.5f), ImGui::GetColorU32(ImGuiCol_Separator));
            } else {
                skipped += hH + sp;
            }
            y += hH + sp;
            int nrows = (int)((m.count + (size_t)cols - 1) / (size_t)cols);
            int r0 = std::clamp((int)floorf((top - y) / rowH), 0, nrows);
            int r1 = std::clamp((int)ceilf((bottom - y) / rowH), r0, nrows);
            skipped += (float)r0 * rowH;
            for (int r = r0; r < r1; r++) {
                flush();
                for (int c = 0; c < cols; c++) {
                    size_t i = m.first + (size_t)r * (size_t)cols + (size_t)c;
                    if (i >= m.first + m.count) break;
                    if (c) ImGui::SameLine(0, sp);
                    ImGui::PushID((int)i);
                    bool tapped = false, held = false;
                    tile(*list[i], side, &tapped, &held);
                    handle(list, i, tapped);
                    ImGui::PopID();
                }
            }
            skipped += (float)(nrows - r1) * rowH;
            y += (float)nrows * rowH;
        }
        flush();
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
}
