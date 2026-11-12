// LunaShare - the Archives tab (a screen on phones): the archives in your folders (Downloads, Documents, received
// files), opening one to see inside (folders, sizes, a password when it needs one), unpacking all or some of it,
// opening or sending a file from inside, and packing files and folders into a zip / 7z / tar.gz / tar.xz / tar.zst.
// The work (listing big archives, unpacking, packing) runs on a thread; a bar shows how far it is and can stop it.
// Comics (cbz / cbr / cb7 / cbt) have their own view here, see ui_comics.cpp.
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "archives.h"
#include "i18n.h"
#include "image_cache.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "music.h"
#include "platform.h"
#include "player_host.h"
#include "torrent_host.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

namespace {
// ---- one piece of work at a time, on a thread
struct Job {
    std::thread th;
    std::atomic<bool> running{false}, cancel{false};
    std::atomic<int64_t> done{0}, total{0};
    std::mutex m;
    std::string title, current, error;
    bool finished = false, ok = false;
    std::function<void(App&, bool ok, const std::string& err)> after;  // (on the UI thread)
    ~Job() {
        cancel = true;
        if (th.joinable()) th.join();
    }
};
Job& TheJob() {
    static Job j;
    return j;
}

void Run(const std::string& title, std::function<bool(const arc::Progress&, std::string&)> work,
         std::function<void(App&, bool, const std::string&)> after) {
    Job& j = TheJob();
    if (j.running) return;
    if (j.th.joinable()) j.th.join();
    j.cancel = false;
    j.done = 0;
    j.total = 0;
    {
        std::lock_guard<std::mutex> l(j.m);
        j.title = title;
        j.current.clear();
        j.error.clear();
        j.finished = false;
        j.after = after;
    }
    j.running = true;
    j.th = std::thread([work] {
        Job& j = TheJob();
        arc::Progress p = [](int64_t d, int64_t t, const std::string& cur) {
            Job& j = TheJob();
            j.done = d;
            j.total = t;
            {
                std::lock_guard<std::mutex> l(j.m);
                j.current = cur;
            }
            return !j.cancel.load();
        };
        std::string err;
        bool ok = work(p, err);
        std::lock_guard<std::mutex> l(j.m);
        j.ok = ok;
        j.error = err;
        j.finished = true;
        j.running = false;
    });
}

// ---- the archive that's open
struct Open {
    std::string path, password, dir;  // dir: the folder inside shown ("" = the top)
    arc::Listing listing;
    bool loaded = false;
    std::set<std::string> picked;
    std::string pwBuf;
    bool entered = false;  // (one folder at the top: shown open)
};
Open& Cur() {
    static Open o;
    return o;
}

// ---- the archives found in the usual folders
struct Found {
    std::string path, name, dir;
    int64_t size = 0, mtime = 0;
};
struct FoundScan {
    std::mutex m;
    std::thread th;
    std::atomic<bool> busy{false};
    std::vector<Found> list;
    ~FoundScan() {
        if (th.joinable()) th.join();
    }
};
FoundScan& Scan() {
    static FoundScan s;
    return s;
}
void Walk(const std::string& dir, int depth, std::vector<Found>& out) {
    if (depth > 4 || out.size() > 5000) return;
    for (auto& e : ListDir(dir)) {
        if (e.name.empty() || e.name[0] == '.') continue;
        std::string p = JoinPath(dir, e.name);
        if (e.isDir) Walk(p, depth + 1, out);
        else if (arc::IsArchivePath(e.name)) out.push_back({p, e.name, dir, e.size, e.mtime});
    }
}

std::string Low(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// a folder name not taken yet: "name", "name (2)"...
std::string FreeName(const std::string& dir, const std::string& name) {
    std::string p = JoinPath(dir, name);
    for (int n = 2; FileExists(p) || IsDirectory(p); n++) p = JoinPath(dir, name + " (" + std::to_string(n) + ")");
    return p;
}

// the archive's name without .zip / .tar.gz ...
std::string StemOf(const std::string& path) {
    std::string b = BaseName(path), l = Low(b);
    for (const char* e : {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz"})
        if (l.size() > strlen(e) && l.compare(l.size() - strlen(e), strlen(e), e) == 0) return b.substr(0, b.size() - strlen(e));
    size_t dot = b.find_last_of('.');
    return dot != std::string::npos && dot > 0 ? b.substr(0, dot) : b;
}

// where "unpack" offers to put things: next to the archive - unless that is a hidden place of the app's (an archive
// opened from another app on a phone is a copy in the cache; one opened from inside another archive too): then the
// folder for received files
std::string UnpackDefault(Engine& eng, const std::string& archive) {
    std::string d = DirName(archive), cfg = eng.ConfigPath();
    if (d.compare(0, cfg.size(), cfg) == 0 || d.find("/cache/incoming") != std::string::npos) return eng.settings.downloadDir;
    return d;
}

const char* kFormats[] = {"zip", "7z", "tar.gz", "tar.xz", "tar.zst"};
}  // namespace

std::vector<std::string> App::ArchiveFolders() {
    std::vector<std::string> v;
    auto add = [&](const std::string& d) {
        if (!d.empty() && std::find(v.begin(), v.end(), d) == v.end()) v.push_back(d);
    };
#if defined(__ANDROID__)
    add("/storage/emulated/0/Download");
    add("/storage/emulated/0/Documents");
#else
    add(JoinPath(HomeDir(), "Downloads"));
    add(JoinPath(HomeDir(), "Documents"));
#endif
    add(eng.settings.downloadDir);
    for (auto& f : eng.settings.archiveFolders) add(f);
    return v;
}

void App::ArchivesRescan() {
    auto& sc = Scan();
    if (sc.busy) {
        archivesRescanWanted = true;
        return;
    }
    if (sc.th.joinable()) sc.th.join();
    std::vector<std::string> folders = ArchiveFolders();
    sc.busy = true;
    sc.th = std::thread([folders] {
        std::vector<Found> out;
        for (auto& f : folders) Walk(f, 0, out);
        std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.path < b.path; });
        out.erase(std::unique(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.path == b.path; }), out.end());
        auto& s = Scan();
        std::lock_guard<std::mutex> l(s.m);
        s.list = std::move(out);
        s.busy = false;
    });
}

void App::OpenArchives() {
    if (ui::M().mobile) {
        if (screen != MScreen::Archives) backStack.push_back(screen);
        screen = MScreen::Archives;
    } else {
        tab = Tab::Archives;
        tabForce = true;
    }
}

void App::OpenArchive(const std::string& path, const std::string& password) {
    Open& o = Cur();
    o.path = path;
    o.password = password;
    o.dir.clear();
    o.picked.clear();
    o.loaded = false;
    o.entered = false;
    o.listing = arc::Listing();
    Run(TF("Opening %s", BaseName(path).c_str()),
        [path, password](const arc::Progress&, std::string& err) {
            arc::Listing l;
            bool ok = arc::List(path, l, password);
            err = l.error;
            Open& o = Cur();
            if (o.path == path) {
                o.listing = std::move(l);
                o.loaded = true;
            }
            return ok;
        },
        nullptr);
    OpenArchives();
}

// a file from inside, unpacked to the cache, then shown like any opened file
void App::ArchiveOpenEntry(const std::string& entry) {
    Open& o = Cur();
    std::string tmp = JoinPath(eng.ConfigPath(), "cache/open");
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(tmp), ec);
    std::string path = o.path, pw = o.password, parent = entry.find('/') == std::string::npos ? "" : entry.substr(0, entry.rfind('/'));
    Run(TF("Opening %s", BaseName(entry).c_str()),
        [path, pw, tmp, entry, parent](const arc::Progress& p, std::string& err) {
            return arc::Extract(path, tmp, {entry}, pw, p, err, nullptr, parent);
        },
        [tmp, entry](App& app, bool ok, const std::string&) {
            if (!ok) return;
            std::string p = JoinPath(tmp, BaseName(entry));
            // what LunaShare shows itself opens here; anything else (a text, a PDF...) with the system's app for it
            if (ImageCache::IsImagePath(p) || PlayerHost::IsVideoPath(p) || music::Library::IsAudioPath(p) || arc::IsArchivePath(p) ||
                TorrentHost::IsTorrentSource(p))
                app.OpenPaths({p}, false);
            else if (Bridge().openFile) Bridge().openFile(p);
            else OpenWithDefaultApp(p);
        });
}

// the Extract... popup: where to (next to the archive, in a folder of its name)
void App::ArchiveExtractPopup() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    Open& o = Cur();
    if (archiveExtractOpen) {
        ImGui::OpenPopup("##arcx");
        archiveExtractOpen = false;
    }
    if (mobile) ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->WorkSize.x * 0.94f, 0));
    if (!ImGui::BeginPopupModal("##arcx", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) return;
    bool some = !archiveExtractOnly.empty();
    ImGui::TextUnformatted(some ? TF("Unpack %d picked", (int)archiveExtractOnly.size()).c_str() : T("Unpack everything"));
    ImGui::TextDisabled("%s", T("To:"));
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 24);
    ImGui::InputText("##arcxto", &archiveExtractTo);
    if (!mobile) ImGui::SameLine();
    if (ImGui::Button(T("Browse..."), ImVec2(0, h))) {
        browseFor = BrowseFor::ArchiveDest;
        browser.Open(FileBrowser::Mode::SelectDir, T("Unpack to"), archiveExtractTo);
    }
    ui::Checkbox(T("Into a folder named after the archive"), &archiveExtractSub);
    if (ImGui::Button(T("Unpack"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h))) {
        std::string dest = archiveExtractSub ? FreeName(archiveExtractTo, StemOf(o.path)) : archiveExtractTo;
        std::string path = o.path, pw = o.password, strip = some ? o.dir : "";
        std::vector<std::string> only = archiveExtractOnly;
        Run(TF("Unpacking %s", BaseName(path).c_str()),
            [path, pw, dest, only, strip](const arc::Progress& p, std::string& err) { return arc::Extract(path, dest, only, pw, p, err, nullptr, strip); },
            [dest](App& app, bool ok, const std::string& err) {
                if (ok) {
                    app.archiveDoneMsg = TF("Unpacked to %s", dest.c_str());
                    app.archiveDoneDir = dest;
                    app.galleryRescanWanted = true;
                    app.movieRescanWanted = true;
                } else {
                    app.archiveDoneMsg = TF("Couldn't unpack: %s", err.c_str());
                    app.archiveDoneDir.clear();
                }
            });
        o.picked.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Pack...: files and folders into a new archive
void App::StartArchiveCreate(const std::vector<std::string>& inputs) {
    if (inputs.empty()) return;
    archiveCreateInputs = inputs;
    archiveCreateName = inputs.size() == 1 ? StemOf(inputs[0]) : BaseName(DirName(inputs[0]));
    if (archiveCreateName.empty()) archiveCreateName = "Archive";
    archiveCreateTo = DirName(inputs[0]);
    archiveCreateOpen = true;
}

void App::ArchiveCreatePopup() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    if (archiveCreateOpen) {
        ImGui::OpenPopup("##arcc");
        archiveCreateOpen = false;
    }
    if (mobile) ImGui::SetNextWindowSize(ImVec2(ImGui::GetMainViewport()->WorkSize.x * 0.94f, 0));
    if (!ImGui::BeginPopupModal("##arcc", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) return;
    int64_t total = 0;
    for (auto& p : archiveCreateInputs) total += IsDirectory(p) ? 0 : std::max<int64_t>(0, FileSize(p));
    ImGui::TextUnformatted(TF("Pack %d item(s)", (int)archiveCreateInputs.size()).c_str());
    for (size_t i = 0; i < archiveCreateInputs.size() && i < 4; i++) ImGui::BulletText("%s", BaseName(archiveCreateInputs[i]).c_str());
    if (archiveCreateInputs.size() > 4) ImGui::TextDisabled(T("... and %d more"), (int)archiveCreateInputs.size() - 4);
    ImGui::TextDisabled("%s", T("Name:"));
    ImGui::SetNextItemWidth(mobile ? ImGui::GetContentRegionAvail().x * 0.6f : ImGui::GetFontSize() * 16);
    ImGui::InputText("##arccname", &archiveCreateName);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 6);
    if (ImGui::BeginCombo("##arccfmt", kFormats[std::clamp(archiveCreateFormat, 0, 4)])) {
        for (int i = 0; i < 5; i++)
            if (ImGui::Selectable(kFormats[i], archiveCreateFormat == i, 0, ImVec2(0, h))) archiveCreateFormat = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ui::HelpMarker(T("zip: opens everywhere. 7z: smaller (slower to make). tar.gz / tar.xz / tar.zst: for Linux; zst is the fastest, xz the smallest."));
    ImGui::TextDisabled("%s", T("In:"));
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 24);
    ImGui::InputText("##arccto", &archiveCreateTo);
    if (!mobile) ImGui::SameLine();
    if (ImGui::Button(T("Browse...") , ImVec2(0, h))) {
        browseFor = BrowseFor::ArchiveCreateDest;
        browser.Open(FileBrowser::Mode::SelectDir, T("Put the archive in"), archiveCreateTo);
    }
    if (ImGui::Button(T("Pack"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h)) && !archiveCreateName.empty()) {
        arc::Format f = (arc::Format)std::clamp(archiveCreateFormat, 0, 4);
        std::string base = arc::SafeRelPath(archiveCreateName);
        for (auto& c : base)
            if (c == '/') c = '_';
        std::string out = JoinPath(archiveCreateTo, base + arc::FormatExt(f));
        for (int n = 2; FileExists(out); n++) out = JoinPath(archiveCreateTo, base + " (" + std::to_string(n) + ")" + arc::FormatExt(f));
        std::vector<std::string> in = archiveCreateInputs;
        Run(TF("Packing %s", BaseName(out).c_str()), [out, f, in](const arc::Progress& p, std::string& err) { return arc::Create(out, f, in, p, err); },
            [out](App& app, bool ok, const std::string& err) {
                if (ok) {
                    app.archiveDoneMsg = TF("Made %s", out.c_str());
                    app.archiveDoneDir = DirName(out);
                    app.archivesRescanWanted = true;
                } else {
                    app.archiveDoneMsg = TF("Couldn't pack: %s", err.c_str());
                    app.archiveDoneDir.clear();
                }
            });
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    (void)total;
}

// every frame: work that finished gets its "after" (open the file, the send-to list...), and a note when it was
// started elsewhere (the gallery)
void App::ArchiveTick() {
    ComicFrame();
    ArchiveExtractPopup();
    ArchiveCreatePopup();
    Job& j = TheJob();
    if (j.running) busy = true;  // (keep drawing while it moves)
    {
        std::unique_lock<std::mutex> l(j.m);
        if (j.finished) {
            j.finished = false;
            auto after = j.after;
            bool ok = j.ok;
            std::string err = j.error;
            l.unlock();
            if (j.th.joinable()) j.th.join();
            std::string before = archiveDoneMsg;
            if (after) after(*this, ok, err);
            if (!archiveDoneMsg.empty() && archiveDoneMsg != before && tab != Tab::Archives && screen != MScreen::Archives)
                toasts.push_back({EventType::Info, T("Archives"), archiveDoneMsg, "", 0, 5.0f});
            else if (!ok && !err.empty() && err != "stopped" && !after)
                toasts.push_back({EventType::Error, T("Archives"), err, "", 0, 5.0f});
        }
    }
}

// the bar of the work going on / just done (on top of the tab)
void App::DrawArchiveJob() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    Job& j = TheJob();
    if (j.running) {
        std::string title, cur;
        {
            std::lock_guard<std::mutex> l(j.m);
            title = j.title;
            cur = j.current;
        }
        int64_t d = j.done, t = j.total;
        ImGui::TextUnformatted(title.c_str());
        if (!cur.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", BaseName(cur).c_str());
        }
        float fr = t > 0 ? (float)((double)d / (double)t) : 0.0f;
        ImGui::ProgressBar(t > 0 ? fr : -1.0f * (float)ImGui::GetTime(), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.7f : ImGui::GetFontSize() * 20, 0),
                           t > 0 ? (FormatBytes((double)d) + " / " + FormatBytes((double)t)).c_str() : "");
        ImGui::SameLine();
        if (ImGui::Button(T("Stop"), ImVec2(0, h))) j.cancel = true;
    } else if (!archiveDoneMsg.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(archiveDoneMsg.c_str());
        ImGui::PopTextWrapPos();
        if (!archiveDoneDir.empty()) {
            if (ImGui::Button(T("Open folder"), ImVec2(0, h))) {
                if (Bridge().openFolder) Bridge().openFolder(archiveDoneDir);
                else OpenInFileManager(archiveDoneDir);
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(T("OK"), ImVec2(0, h))) archiveDoneMsg.clear();
    }
}

void App::DrawArchives() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    Open& o = Cur();
    DrawArchiveJob();
    if (!o.path.empty()) {
        DrawArchiveOpen();
        return;
    }
    // ---- the list: archives (or comics) in the folders
    auto& sc = Scan();
    if (!archivesScanned || (archivesRescanWanted && !sc.busy)) {
        archivesScanned = true;
        archivesRescanWanted = false;
        ArchivesRescan();
    }
    if (ImGui::RadioButton(T("Archives"), !archivesComics) && archivesComics) archivesComics = false, archivePick.Clear();
    ImGui::SameLine();
    if (ImGui::RadioButton(T("Comics"), archivesComics) && !archivesComics) archivesComics = true, archivePick.Clear();
    ImGui::SameLine();
    if (ui::Checkbox(T("Select"), &archivePick.selecting) && !archivePick.selecting) archivePick.sel.clear();
    if (!mobile) ImGui::SameLine();
    float third = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f : 0;
    if (ui::IconTextButton(T("Open..."), Icon::Folder, ImVec2(third, h))) {
        browseFor = BrowseFor::ArchiveOpen;
        browser.Open(FileBrowser::Mode::OpenFile, T("Open an archive"), browser.lastDir);
    }
    ImGui::SameLine();
    if (ui::IconTextButton(T("Pack..."), Icon::Archive, ImVec2(third, h))) {
        browseFor = BrowseFor::ArchiveInputs;
        browser.Open(FileBrowser::Mode::OpenMulti, T("Files and folders to pack"), browser.lastDir);
    }
    ImGui::SameLine();
    if (ui::IconTextButton(T("Folders..."), Icon::List, ImVec2(third, h))) ImGui::OpenPopup("##arcfolders");
    if (!mobile) ImGui::SameLine();
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##arcsearch", T("search"), &archivesSearch);
    if (sc.busy) ImGui::TextDisabled("%s", T("looking through the folders..."));
    if (mobile) {
        ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + vp.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + vp.y * 0.12f),
                                ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowSize(ImVec2(vp.x * 0.94f, 0));
    }
    if (ImGui::BeginPopup("##arcfolders")) {
        ImGui::TextUnformatted(T("Folders looked through"));
        if (ui::IconTextButton(T("Add a folder..."), Icon::Plus, ImVec2(0, h))) {
            browseFor = BrowseFor::ArchiveFolder;
            browser.Open(FileBrowser::Mode::SelectDir, T("Add a folder"), browser.lastDir);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::IconTextButton(T("Look again"), Icon::Retry, ImVec2(0, h))) ArchivesRescan();
        ImGui::Separator();
        std::string remove;
        for (auto& f : eng.settings.archiveFolders) {
            ImGui::PushID(f.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(f.c_str());
            ImGui::SameLine();
            if (ImGui::Button(T("Remove"), ImVec2(0, mobile ? h * 0.8f : 0))) remove = f;
            ImGui::PopID();
        }
        for (auto& d : ArchiveFolders())
            if (std::find(eng.settings.archiveFolders.begin(), eng.settings.archiveFolders.end(), d) == eng.settings.archiveFolders.end())
                ImGui::TextDisabled(T("+ always: %s"), d.c_str());
        if (!remove.empty()) {
            Settings s = eng.GetSettings();
            s.archiveFolders.erase(std::remove(s.archiveFolders.begin(), s.archiveFolders.end(), remove), s.archiveFolders.end());
            eng.ApplySettings(s);
            edit.archiveFolders = s.archiveFolders;
            ArchivesRescan();
        }
        if (mobile && ImGui::Button(T("Close"), ImVec2(-1, h))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    std::vector<Found> found;
    {
        std::lock_guard<std::mutex> l(sc.m);
        found = sc.list;
    }
    std::string q = Low(archivesSearch);
    std::vector<const Found*> list;
    for (auto& f : found) {
        if (arc::IsComicPath(f.name) != archivesComics) continue;
        if (!q.empty() && Low(f.name).find(q) == std::string::npos) continue;
        list.push_back(&f);
    }
    if (archivePick.Active()) {  // picked: send (to a chat or as a link), pack, delete
        std::vector<std::string> all;
        for (auto* f : list) all.push_back(f->path);
        DrawPickBar(archivePick, all, nullptr, [this] { ArchivesRescan(); });
    }
    if (archivesComics) {
        std::vector<std::string> paths;
        for (auto* f : list) paths.push_back(f->path);
        DrawComicShelf(paths, sc.busy);
        return;
    }
    std::sort(list.begin(), list.end(), [](const Found* a, const Found* b) { return a->mtime > b->mtime; });
    ImGui::BeginChild("archives");
    if (mobile) ui::TouchScroll();
    if (list.empty())
        ImGui::TextDisabled("%s", sc.busy ? T("looking through the folders...") : q.empty() ? T("No archives in these folders.") : T("Nothing matches."));
    float rowH = mobile ? ui::M().rowH * 1.1f : ImGui::GetFrameHeight() * 1.6f;
    for (size_t i = 0; i < list.size(); i++) {
        const Found& f = *list[i];
        ImGui::PushID((int)i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool picked = archivePick.sel.count(f.path) > 0;
        bool tapped = ImGui::Selectable("##a", picked, 0, ImVec2(0, rowH));
        if (PickItem(archivePick, f.path, tapped)) OpenArchive(f.path);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (archivePick.selecting || picked) {  // a tick instead of the icon
            float r = rowH * 0.2f;
            ImVec2 cc(p.x + rowH * 0.45f, p.y + rowH * 0.5f);
            if (picked) {
                dl->AddCircleFilled(cc, r, ui::ColAccent());
                ui::DrawIcon(dl, Icon::Check, cc, r * 1.2f, IM_COL32_WHITE);
            } else {
                dl->AddCircle(cc, r, ImGui::GetColorU32(ImGuiCol_Text), 0, 2);
            }
        } else
        ui::DrawIcon(dl, Icon::Archive, ImVec2(p.x + rowH * 0.45f, p.y + rowH * 0.5f), rowH * 0.5f, ImGui::GetColorU32(ImGuiCol_Text));
        float x = p.x + rowH;
        dl->AddText(ImVec2(x, p.y + rowH * 0.08f), ImGui::GetColorU32(ImGuiCol_Text), f.name.c_str());
        std::string sub = FormatBytes((double)f.size) + "   " + FormatTime(f.mtime * 1000) + "   " + f.dir;
        dl->AddText(ImVec2(x, p.y + rowH * 0.52f), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// phones' back: up a folder inside the archive, then out of it (true = done here)
bool App::ArchiveBack() {
    Open& o = Cur();
    if (o.path.empty()) return false;
    if (!o.dir.empty()) o.dir = o.dir.find('/') == std::string::npos ? "" : o.dir.substr(0, o.dir.rfind('/'));
    else o.path.clear();
    return true;
}

// inside one archive
void App::DrawArchiveOpen() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    Open& o = Cur();
    if (ui::IconTextButton(T("Archives"), Icon::Back, ImVec2(0, h))) {
        o.path.clear();
        return;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(BaseName(o.path).c_str());
    if (!o.loaded) {
        ImGui::TextDisabled("%s", T("Loading..."));
        return;
    }
    const arc::Listing& L = o.listing;
    if (!o.entered) {
        o.entered = true;
        std::string top;
        bool one = !L.entries.empty();
        for (auto& e : L.entries) {
            std::string t = e.path.substr(0, e.path.find('/'));
            if (t == e.path && !e.dir) one = false;
            if (top.empty()) top = t;
            else if (t != top) one = false;
        }
        if (one) o.dir = top;
    }
    int files = 0;
    int64_t bytes = 0;
    int pictures = 0;
    for (auto& e : L.entries)
        if (!e.dir) {
            files++;
            bytes += e.size;
            if (ImageCache::IsImagePath(e.path)) pictures++;
        }
    ImGui::TextDisabled("%s", (L.format + "   " + TF("%d files, %s", files, FormatBytes((double)bytes).c_str()) + (L.encrypted ? std::string("   ") + T("(password)") : "")).c_str());
    if (!L.error.empty() && !L.needPassword) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", L.error.c_str());
    if (L.needPassword || (L.encrypted && o.password.empty())) {
        ImGui::TextUnformatted(L.needPassword && !o.password.empty() ? T("Wrong password - try again:") : T("This archive needs a password:"));
        ImGui::SetNextItemWidth(mobile ? ImGui::GetContentRegionAvail().x * 0.6f : ImGui::GetFontSize() * 14);
        bool go = ImGui::InputText("##arcpw", &o.pwBuf, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button(T("Open"), ImVec2(0, h)) || go) && !o.pwBuf.empty()) {
            std::string pw = o.pwBuf;
            o.pwBuf.clear();
            OpenArchive(o.path, pw);
            return;
        }
        if (L.needPassword) return;
    }
    // actions
    std::vector<std::string> picked(o.picked.begin(), o.picked.end());
    // (phones: two a line)
    float half = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f : 0;
    if (ui::IconTextButton(T("Unpack all..."), Icon::Down, ImVec2(half, h))) {
        archiveExtractOnly.clear();
        archiveExtractTo = UnpackDefault(eng, o.path);
        archiveExtractSub = true;
        archiveExtractOpen = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(picked.empty());
    if (ui::IconTextButton(TF("Unpack picked (%d)...", (int)picked.size()).c_str(), Icon::Down, ImVec2(half, h))) {
        archiveExtractOnly = picked;
        archiveExtractTo = UnpackDefault(eng, o.path);
        archiveExtractSub = false;
        archiveExtractOpen = true;
    }
    if (!mobile) ImGui::SameLine();
    if (ui::IconTextButton(T("Send..."), Icon::Send, ImVec2(half, h))) {
        std::string tmp = JoinPath(eng.ConfigPath(), "cache/send-" + std::to_string(NowMs()));
        std::string path = o.path, pw = o.password, strip = o.dir;
        Run(T("Getting them out to send"),
            [path, pw, tmp, picked, strip](const arc::Progress& p, std::string& err) { return arc::Extract(path, tmp, picked, pw, p, err, nullptr, strip); },
            [tmp, picked](App& app, bool ok, const std::string&) {
                if (!ok) return;
                std::vector<std::string> top;
                for (auto& pk : picked) top.push_back(JoinPath(tmp, BaseName(pk)));
                app.OpenSendTo(top);
            });
        o.picked.clear();
    }
    ImGui::EndDisabled();
    if (pictures > 0 && pictures * 2 >= files) {  // (mostly pictures: a comic, or an album)
        ImGui::SameLine();
        if (ui::IconTextButton(T("Read"), Icon::Book, ImVec2(half, h))) OpenComic(o.path, o.password);
    }
    // where we are inside
    ImGui::Separator();
    if (ImGui::SmallButton("/")) o.dir.clear();
    if (!o.dir.empty()) {
        size_t s = 0;
        while (true) {
            size_t e = o.dir.find('/', s);
            std::string part = o.dir.substr(s, e == std::string::npos ? std::string::npos : e - s);
            ImGui::SameLine(0, 2);
            ImGui::TextDisabled(">");
            ImGui::SameLine(0, 2);
            ImGui::PushID((int)s);
            if (ImGui::SmallButton(part.c_str())) o.dir = o.dir.substr(0, e == std::string::npos ? o.dir.size() : e);
            ImGui::PopID();
            if (e == std::string::npos) break;
            s = e + 1;
        }
    }
    // what's in this folder: folders first, then by name
    std::vector<const arc::Entry*> here;
    for (auto& e : L.entries) {
        std::string parent = e.path.find('/') == std::string::npos ? "" : e.path.substr(0, e.path.rfind('/'));
        if (parent == o.dir) here.push_back(&e);
    }
    std::sort(here.begin(), here.end(), [](const arc::Entry* a, const arc::Entry* b) {
        return a->dir != b->dir ? a->dir : NaturalLess(BaseName(a->path), BaseName(b->path));
    });
    // a folder's size: what's inside
    std::map<std::string, int64_t> dirSize;
    for (auto& e : L.entries)
        if (!e.dir)
            for (size_t s = e.path.find('/'); s != std::string::npos; s = e.path.find('/', s + 1)) dirSize[e.path.substr(0, s)] += e.size;
    ImGui::BeginChild("arcfiles");
    if (mobile) ui::TouchScroll();
    float rowH = mobile ? ui::M().rowH * 0.95f : ImGui::GetFrameHeight();
    std::string enter, open;
    for (size_t i = 0; i < here.size(); i++) {
        const arc::Entry& e = *here[i];
        ImGui::PushID((int)i);
        bool pk = o.picked.count(e.path) > 0;
        ImGui::SetNextItemWidth(rowH);
        if (ui::Checkbox("##pk", &pk)) {
            if (pk) o.picked.insert(e.path);
            else o.picked.erase(e.path);
        }
        ImGui::SameLine();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##e", false, 0, ImVec2(w, rowH))) {
            if (e.dir) enter = e.path;
            else open = e.path;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        Icon ic = e.dir ? Icon::Folder : ImageCache::IsImagePath(e.path) ? Icon::Image : Icon::File;
        ui::DrawIcon(dl, ic, ImVec2(p.x + rowH * 0.5f, p.y + rowH * 0.5f), rowH * 0.55f, ImGui::GetColorU32(ImGuiCol_Text));
        float tx = p.x + rowH * 1.1f, ty = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        std::string sz = FormatBytes((double)(e.dir ? dirSize[e.path] : e.size));
        float szW = ImGui::CalcTextSize(sz.c_str()).x;
        ImVec4 clip(tx, p.y, p.x + w - szW - 12, p.y + rowH);
        dl->AddText(nullptr, 0, ImVec2(tx, ty), ImGui::GetColorU32(ImGuiCol_Text), BaseName(e.path).c_str(), nullptr, 0, &clip);
        dl->AddText(ImVec2(p.x + w - szW - 4, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), sz.c_str());
        if (e.encrypted) ImGui::SetItemTooltip("%s", T("(password)"));
        ImGui::PopID();
    }
    if (here.empty()) ImGui::TextDisabled("%s", T("Empty."));
    ImGui::EndChild();
    if (!enter.empty()) o.dir = enter;
    if (!open.empty()) ArchiveOpenEntry(open);
}
