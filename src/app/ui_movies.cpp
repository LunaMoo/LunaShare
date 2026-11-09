// LunaShare - the Movies tab (a screen on phones): the videos in your folders (received files and Videos always
// included) as pictures from the movies, by folder; tapping one opens the movie player. Folders are looked through
// on their own thread.
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "i18n.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "platform.h"
#include "player_host.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

namespace {
struct MovieFile {
    std::string path, name, dir;
    int64_t size = 0, mtime = 0;
};
struct MovieScan {
    std::mutex m;
    std::thread th;
    std::atomic<bool> busy{false};
    std::vector<MovieFile> files;
    uint64_t ver = 0;
    std::vector<std::string> folders;  // what was looked through
    ~MovieScan() {
        if (th.joinable()) th.join();
    }
};
MovieScan& Scan() {
    static MovieScan s;
    return s;
}

std::string Low(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

void Walk(const std::string& dir, int depth, std::vector<MovieFile>& out) {
    if (depth > 8 || out.size() > 20000) return;
    for (auto& e : ListDir(dir)) {
        if (e.name.empty() || e.name[0] == '.') continue;
        std::string p = JoinPath(dir, e.name);
        if (e.isDir) Walk(p, depth + 1, out);
        else if (PlayerHost::IsVideoPath(e.name)) out.push_back({p, e.name, dir, e.size, e.mtime});
    }
}
}  // namespace

// the folders: mine, plus received files and the system's Videos folder
static std::vector<std::string> MovieFolders(const Settings& s) {
    std::vector<std::string> v;
    auto add = [&](const std::string& d) {
        if (!d.empty() && std::find(v.begin(), v.end(), d) == v.end()) v.push_back(d);
    };
#if defined(__ANDROID__)
    add("/storage/emulated/0/Movies");
    add("/storage/emulated/0/DCIM");
#else
    add(JoinPath(HomeDir(), "Videos"));
#endif
    add(s.downloadDir);
    for (auto& f : s.movieFolders) add(f);
    return v;
}

void App::MovieRescan() {
    auto& sc = Scan();
    if (sc.busy) {
        movieRescanWanted = true;
        return;
    }
    if (sc.th.joinable()) sc.th.join();
    std::vector<std::string> folders = MovieFolders(eng.settings);
    sc.busy = true;
    sc.th = std::thread([folders] {
        std::vector<MovieFile> out;
        for (auto& f : folders) Walk(f, 0, out);
        // (a folder inside another one: once)
        std::sort(out.begin(), out.end(), [](const MovieFile& a, const MovieFile& b) { return a.path < b.path; });
        out.erase(std::unique(out.begin(), out.end(), [](const MovieFile& a, const MovieFile& b) { return a.path == b.path; }), out.end());
        auto& s = Scan();
        std::lock_guard<std::mutex> l(s.m);
        s.files = std::move(out);
        s.folders = folders;
        s.ver++;
        s.busy = false;
    });
}

void App::DrawMovies() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    auto& sc = Scan();
    if (!movieScanned || (movieRescanWanted && !sc.busy)) {
        movieScanned = true;
        movieRescanWanted = false;
        MovieRescan();
    }
    std::vector<MovieFile> files;
    {
        std::lock_guard<std::mutex> l(sc.m);
        files = sc.files;
    }
    DrawWatchResume();
    // the bar: open any file, folders, search, sort
    if (ui::IconTextButton(T("Open a file..."), Icon::Folder, ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h))) OpenMoviePlayer();
    ImGui::SameLine();
    if (ui::IconTextButton(T("Movie folders..."), Icon::List, ImVec2(mobile ? -1 : 0, h))) ImGui::OpenPopup("##movfolders");
    if (!mobile) ImGui::SameLine();
    ImGui::SetNextItemWidth(mobile ? ImGui::GetContentRegionAvail().x * 0.55f : ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##movsearch", T("search"), &movieSearch);
    ImGui::SameLine();
    const char* sorts[] = {T("Name"), T("Recently added"), T("Largest first")};
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 9);
    if (ImGui::BeginCombo("##movsort", sorts[std::clamp(movieSort, 0, 2)])) {
        for (int i = 0; i < 3; i++)
            if (ImGui::Selectable(sorts[i], movieSort == i, 0, ImVec2(0, h))) movieSort = i;
        ImGui::EndCombo();
    }
    if (sc.busy) {
        if (!mobile) ImGui::SameLine();
        ImGui::TextDisabled("%s", T("looking through the folders..."));
    }
    // folders popup (like music's: the buttons first, the list scrolls)
    {
        ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
        if (mobile) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + vp.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + vp.y * 0.12f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0));
            ImGui::SetNextWindowSize(ImVec2(vp.x * 0.94f, 0));
        }
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(vp.x, vp.y * 0.75f));
    }
    if (ImGui::BeginPopup("##movfolders")) {
        ImGui::TextUnformatted(T("Movie folders"));
        float half = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f : 0;
        if (ui::IconTextButton(T("Add a folder..."), Icon::Plus, ImVec2(half, h))) {
            browseFor = BrowseFor::MovieFolder;
            browser.Open(FileBrowser::Mode::SelectDir, T("Add a movie folder"), browser.lastDir);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::IconTextButton(T("Look again"), Icon::Retry, ImVec2(half, h))) MovieRescan();
        ImGui::Separator();
        ImGui::BeginChild("##mvfl", ImVec2(mobile ? 0 : ImGui::GetFontSize() * 30, ImGui::GetMainViewport()->WorkSize.y * (mobile ? 0.4f : 0.3f)));
        if (mobile) ui::TouchScroll();
        std::string remove;
        for (auto& f : eng.settings.movieFolders) {
            ImGui::PushID(f.c_str());
            float rw = ImGui::CalcTextSize(T("Remove")).x + ImGui::GetStyle().FramePadding.x * 2 + (mobile ? h * 0.3f : 0);
            ImGui::AlignTextToFramePadding();
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - rw - ImGui::GetStyle().ItemSpacing.x);
            ImGui::TextUnformatted(f.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - rw);
            if (ImGui::Button(T("Remove"), ImVec2(rw, mobile ? h * 0.8f : 0))) remove = f;
            ImGui::PopID();
        }
        ImGui::PushTextWrapPos(0);
        Settings none = eng.settings;
        none.movieFolders.clear();
        for (auto& d : MovieFolders(none)) ImGui::TextDisabled(T("+ always: %s"), d.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        if (!remove.empty()) {
            Settings s = eng.GetSettings();
            s.movieFolders.erase(std::remove(s.movieFolders.begin(), s.movieFolders.end(), remove), s.movieFolders.end());
            eng.ApplySettings(s);
            edit.movieFolders = s.movieFolders;
            MovieRescan();
        }
        if (mobile && ImGui::Button(T("Close"), ImVec2(-1, h))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // the movies: filtered, sorted, by folder (sorted by name) or all together (newest / largest)
    std::string q = Low(movieSearch);
    std::vector<const MovieFile*> list;
    for (auto& f : files)
        if (q.empty() || Low(f.name).find(q) != std::string::npos || Low(f.dir).find(q) != std::string::npos) list.push_back(&f);
    if (movieSort == 1) std::sort(list.begin(), list.end(), [](auto a, auto b) { return a->mtime > b->mtime; });
    else if (movieSort == 2) std::sort(list.begin(), list.end(), [](auto a, auto b) { return a->size > b->size; });
    else std::stable_sort(list.begin(), list.end(), [](auto a, auto b) { return a->dir != b->dir ? a->dir < b->dir : NaturalLess(a->name, b->name); });

    ImGui::BeginChild("movies");
    if (mobile) ui::TouchScroll();
    if (list.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", sc.busy ? T("looking through the folders...")
                           : q.empty() ? T("No movies in your folders yet. Add a folder, or open a file.") : T("Nothing matches."));
        ImGui::PopTextWrapPos();
    }
    float avail = ImGui::GetContentRegionAvail().x;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    int cols = std::max(2, (int)((avail + sp) / (ImGui::GetFontSize() * (mobile ? 8.5f : 11.0f) + sp)));
    float cw = (avail - sp * (cols - 1)) / cols, ch = cw * 9 / 16;
    float th = ch + ImGui::GetTextLineHeightWithSpacing() * 2;
    std::string curDir;
    int col = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < list.size(); i++) {
        const MovieFile& f = *list[i];
        if (movieSort == 0 && f.dir != curDir) {  // a header per folder
            curDir = f.dir;
            if (col) ImGui::NewLine();
            col = 0;
            ImGui::SeparatorText(BaseName(f.dir).c_str());
        }
        if (col) ImGui::SameLine(0, sp);
        ImGui::PushID((int)i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool visible = ImGui::IsRectVisible(p, ImVec2(p.x + cw, p.y + th));
        if (ImGui::InvisibleButton("##mv", ImVec2(cw, th))) {
            watchFailed = false;
            watchLibList.clear();
            if (eng.StartWatch("", f.path)) watchShown = true;
        }
        bool hov = ImGui::IsItemHovered();
        if (hov && !mobile) ImGui::SetTooltip("%s\n%s", f.path.c_str(), FormatBytes((double)f.size).c_str());
        if (visible) {  // (pictures only for those on screen: they're made in the background)
            ImVec2 a = p, b(p.x + cw, p.y + ch);
            dl->AddRectFilled(a, b, ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 6);
            const auto& m = Player().GetMeta(f.path);
            if (m.tex && m.tw > 0 && m.th > 0) {
                float s = std::min(cw / m.tw, ch / m.th);
                float w = m.tw * s, hh = m.th * s;
                ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                dl->AddImageRounded((ImTextureID)(intptr_t)m.tex, ImVec2(c.x - w * 0.5f, c.y - hh * 0.5f), ImVec2(c.x + w * 0.5f, c.y + hh * 0.5f),
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 6);
            } else {
                ui::DrawIcon(dl, Icon::Video, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), ch * 0.35f,
                             m.done && !m.ok ? ui::ColErr() : ui::ColMuted());
            }
            if (m.done && m.info.duration > 0) {  // its length, bottom right
                int d = (int)m.info.duration;
                char t[32];
                if (d >= 3600) snprintf(t, sizeof t, "%d:%02d:%02d", d / 3600, d / 60 % 60, d % 60);
                else snprintf(t, sizeof t, "%d:%02d", d / 60, d % 60);
                ImVec2 ts = ImGui::CalcTextSize(t);
                ImVec2 tp(b.x - ts.x - 6, b.y - ts.y - 4);
                dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 1), ImVec2(b.x - 3, b.y - 3), IM_COL32(0, 0, 0, 170), 4);
                dl->AddText(tp, IM_COL32(255, 255, 255, 235), t);
            }
            if (eng.watch.sid && eng.watch.host.empty() && eng.watch.path == f.path)  // (the one that's on)
                dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 6.0f, ImDrawFlags_None, 3.0f);
            ImVec4 clip(p.x, b.y, p.x + cw, p.y + th);
            dl->AddText(nullptr, 0, ImVec2(p.x + 2, b.y + 3), ImGui::GetColorU32(ImGuiCol_Text), f.name.c_str(), nullptr, cw - 4, &clip);
        }
        ImGui::PopID();
        if (++col >= cols) col = 0;
    }
    ImGui::EndChild();
}
