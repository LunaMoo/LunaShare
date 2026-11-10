// LunaShare - the Gallery tab (a screen on phones): the pictures and videos of the camera, Pictures, received files and
// the folders you add - by month (when they were taken: the photo's EXIF date, else the file's) or by album (folder).
// Tapping one opens the picture viewer with the rest of that list (videos play in the movie player); a long press (or
// Select) picks several to send, put in an archive or delete. Phones: the camera can be sent to one of my computers by
// itself (a folder sync). Folders are looked through on a thread; the photo dates are kept in gallery_dates.json.
#include <algorithm>
#include <atomic>
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
            if (it != dates.end() && it->is_array() && it->size() == 2 && (*it)[0] == stamp) {
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
    std::vector<Pic> pics;
    {
        std::lock_guard<std::mutex> l(sc.m);
        pics = sc.pics;
    }
    // the bar: back (from an album), by month / albums, folders, search, select
    if (!galleryAlbum.empty()) {
        if (ui::IconTextButton(T("Albums"), Icon::Back, ImVec2(0, h))) {
            galleryAlbum.clear();
            gallerySel.clear();
            gallerySelecting = false;
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
    if (ImGui::Checkbox(T("Select"), &gallerySelecting) && !gallerySelecting) gallerySel.clear();
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
    std::vector<const Pic*> list;
    for (auto& p : pics) {
        if (!galleryAlbum.empty() && p.dir != galleryAlbum) continue;
        if (!q.empty() && Low(p.name).find(q) == std::string::npos && Low(p.dir).find(q) == std::string::npos) continue;
        list.push_back(&p);
    }
    std::stable_sort(list.begin(), list.end(), [](const Pic* a, const Pic* b) { return a->date > b->date; });

    // picked: what to do with them
    if (gallerySelecting || !gallerySel.empty()) {
        ImGui::TextUnformatted(TF("%d picked", (int)gallerySel.size()).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(gallerySel.empty());
        if (ui::IconTextButton(T("Send..."), Icon::Send, ImVec2(0, h))) {
            OpenSendTo(std::vector<std::string>(gallerySel.begin(), gallerySel.end()));
            gallerySel.clear();
            gallerySelecting = false;
        }
        ImGui::SameLine();
        if (ui::IconTextButton(T("Delete..."), Icon::Trash, ImVec2(0, h))) galleryDeleteAsk = true;
        ImGui::SameLine();
        if (ui::IconTextButton(T("Pack..."), Icon::Archive, ImVec2(0, h))) {
            StartArchiveCreate(std::vector<std::string>(gallerySel.begin(), gallerySel.end()));
            gallerySel.clear();
            gallerySelecting = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("All"), ImVec2(0, h)))
            for (auto* p : list) gallerySel.insert(p->path);
        ImGui::SameLine();
        if (ImGui::Button(T("None"), ImVec2(0, h))) gallerySel.clear();
    }
    if (galleryDeleteAsk) {
        ImGui::OpenPopup("##galdel");
        galleryDeleteAsk = false;
    }
    if (ImGui::BeginPopupModal("##galdel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(TF("Delete %d file(s) from this device? This can't be undone.", (int)gallerySel.size()).c_str());
        if (ImGui::Button(T("Delete"), ImVec2(0, h))) {
            for (auto& p : gallerySel) RemoveFile(p);
            gallerySel.clear();
            gallerySelecting = false;
            GalleryRescan();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(0, h))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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
        if (gallerySel.count(p.path)) {
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
    static std::string heldDone;  // (a long press picks once, not every frame)
    auto handle = [&](const std::vector<const Pic*>& l, size_t i, bool tapped, bool held) {
        const std::string& path = l[i]->path;
        if (held && heldDone != path) {
            heldDone = path;
            gallerySelecting = true;
            if (!gallerySel.erase(path)) gallerySel.insert(path);
        } else if (tapped) {
            if (heldDone == path) heldDone.clear();
            else if (gallerySelecting) {
                if (!gallerySel.erase(path)) gallerySel.insert(path);
            } else {
                openAt(l, i);
            }
        }
    };

    if (galleryView == 1 && galleryAlbum.empty()) {
        // albums: a folder each, newest first, the newest picture on the cover
        struct Album {
            std::string dir;
            const Pic* cover = nullptr;
            int count = 0;
        };
        std::map<std::string, Album> albums;
        for (auto* p : list) {
            Album& a = albums[p->dir];
            a.dir = p->dir;
            a.count++;
            if (!a.cover) a.cover = p;  // (the list is newest first)
        }
        std::vector<Album*> al;
        for (auto& [d, a] : albums) al.push_back(&a);
        std::sort(al.begin(), al.end(), [](Album* a, Album* b) { return a->cover->date > b->cover->date; });
        int cols = std::max(2, (int)((avail + sp) / (ImGui::GetFontSize() * (mobile ? 8.0f : 10.0f) + sp)));
        float side = (avail - sp * (cols - 1)) / cols;
        for (size_t i = 0; i < al.size(); i++) {
            if (i % cols) ImGui::SameLine(0, sp);
            ImGui::PushID((int)i);
            ImGui::BeginGroup();
            bool tapped = false, held = false;
            tile(*al[i]->cover, side, &tapped, &held);
            if (tapped) {
                galleryAlbum = al[i]->dir;
                gallerySel.clear();
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
        std::string cur;
        int col = 0;
        for (size_t i = 0; i < list.size(); i++) {
            std::string month = MonthOf(list[i]->date);
            if (month != cur) {
                cur = month;
                if (col) ImGui::NewLine();
                col = 0;
                ImGui::SeparatorText(month.c_str());
            }
            if (col) ImGui::SameLine(0, sp);
            ImGui::PushID((int)i);
            bool tapped = false, held = false;
            tile(*list[i], side, &tapped, &held);
            handle(list, i, tapped, held);
            ImGui::PopID();
            if (++col >= cols) col = 0;
        }
    }
    ImGui::EndChild();
}
