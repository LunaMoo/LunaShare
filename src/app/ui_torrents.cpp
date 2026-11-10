// LunaShare - the Torrents tab (a screen on phones). With the torrent module (desktops) it adds .torrent files and
// magnet links, shows how far each one is, picks the files wanted, watches a movie while it still downloads, and
// sends what's done to a chat. Without the module (phones) the same list shows the torrents of one of MY computers:
// a link opened here is handed to that computer, and when it's done the files are sent here - or watched from there.
#include <algorithm>
#include <atomic>

#include "emoji_host.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "platform.h"
#include "player_host.h"
#include "torrent_host.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;
using Torrent = TorrentHost::Torrent;

namespace {
std::string Rate(int bps) { return FormatBytes((double)bps) + "/s"; }

// the files popup: what's ticked, until OK
struct FilesState {
    std::string id;
    std::vector<TorrentHost::File> files;
    std::vector<bool> want;
    int64_t at = 0;
} g_files;
}  // namespace

void App::OpenTorrents() {
    if (ui::M().mobile) {
        if (screen != MScreen::Torrents) backStack.push_back(screen);
        screen = MScreen::Torrents;
    } else {
        tab = Tab::Torrents;
        tabForce = true;
    }
}

void App::TorrentOpenSource(const std::string& source) {
    torrentAddSource = source;
    torrentAddErr.clear();
    torrentAddDir = Torrents().Available() ? Torrents().DefaultDir() : "";
    torrentAddOpen = true;
    OpenTorrents();
}

void App::WatchTorrent(const std::string& device, const std::string& id, int file, int64_t size, const std::string& name) {
    if (!Player().Available() || file < 0 || size <= 0) return;
    if (device.empty()) {
        // all of it here already: played like any file
        for (auto& t : Torrents().List())
            if (t.id == id)
                for (auto& f : Torrents().Files(id))
                    if (f.index == file && f.done >= f.size && FileExists(JoinPath(t.dir, f.path))) {
                        watchFailed = false;
                        watchLibList.clear();
                        if (eng.StartWatch("", JoinPath(t.dir, f.path))) watchShown = true;
                        return;
                    }
        Torrents().Pause(id, false);  // (a stopped one would never bring the pieces)
    } else {
        Torrents().RemotePause(device, id, false);
    }
    auto read = device.empty() ? Torrents().Reader(id, file) : Torrents().RemoteReader(device, id, file, size);
    if (!read) return;
    auto starved = std::make_shared<std::atomic<bool>>(false);
    watchExtStarved = starved;
    watchExtReader = [read, starved](int64_t off, uint8_t* buf, int len) {
        int n = read(off, buf, len);
        if (n == -2) starved->store(true);
        else if (n > 0) starved->store(false);
        return n;
    };
    std::string ext = (device.empty() ? "" : device + ":") + id + ":" + std::to_string(file);
    watchExtId = "ext:" + ext;
    watchExtTorrent = id;
    watchExtDevice = device;
    watchFailed = false;
    watchFromViewer = false;
    watchLibList.clear();
    if (eng.StartWatchExt(ext, name, size)) watchShown = true;
}

// every frame a torrent is being watched: the clock waits while the pieces come; the bar shows how much is there
void App::WatchExtTick() {
    double loaded = 0;
    const std::vector<Torrent>* list = watchExtDevice.empty() ? &Torrents().List() : Torrents().RemoteList(watchExtDevice);
    if (!watchExtDevice.empty()) Torrents().RemoteWatchList(watchExtDevice);
    if (list)
        for (auto& t : *list)
            if (t.id == watchExtTorrent && t.size > 0) loaded = (double)t.done / (double)t.size;
    eng.WatchSetExt(watchExtStarved && watchExtStarved->load() && Player().OpenState() != PlayerHost::Opening::Busy, loaded);
}

void App::WatchExtEnd() {
    if (watchExtDevice.empty() && !watchExtTorrent.empty()) Torrents().StopWatching(watchExtTorrent);
    else if (!watchExtTorrent.empty()) Torrents().StopWatching(watchExtTorrent);  // (drops the blocks kept)
    watchExtReader = nullptr;
    watchExtStarved.reset();
    watchExtId.clear();
    watchExtTorrent.clear();
    watchExtDevice.clear();
}

void App::TorrentPopups() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    auto place = [&] {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * (mobile ? 0.25f : 0.5f)), ImGuiCond_Appearing,
                                ImVec2(0.5f, mobile ? 0.0f : 0.5f));
        if (mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.94f, 0));
    };
    float wide = mobile ? -1 : ImGui::GetFontSize() * 26;

    // ---- add: a link or a file; where to (here), or on which computer
    if (torrentAddOpen) {
        ImGui::OpenPopup("##tadd");
        torrentAddOpen = false;
    }
    place();
    if (ImGui::BeginPopupModal("##tadd", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        bool here = Torrents().Available();
        std::vector<std::string> comps = here ? std::vector<std::string>() : Torrents().Computers();
        ImGui::TextUnformatted(T("Add a torrent"));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (mobile ? ImGui::GetContentRegionAvail().x : ImGui::GetFontSize() * 26));
        if (torrentAddSource.empty()) {
            ImGui::TextDisabled("%s", T("A magnet link:"));
            ImGui::SetNextItemWidth(wide);
            ImGui::InputTextWithHint("##tmagnet", "magnet:?xt=urn:btih:...", &torrentMagnetBuf);
            if (ImGui::Button(T("Paste"), ImVec2(0, h))) {
                if (const char* c = ImGui::GetClipboardText()) torrentMagnetBuf = c;
            }
            ImGui::SameLine();
            if (ImGui::Button(T("Open a .torrent file..."), ImVec2(0, h))) {
                browseFor = BrowseFor::TorrentFile;
                browser.Open(FileBrowser::Mode::OpenFile, T("Open a .torrent file"), browser.lastDir);
            }
            // (spaces and line ends around a pasted link don't matter)
            std::string m = torrentMagnetBuf;
            while (!m.empty() && isspace((unsigned char)m.back())) m.pop_back();
            while (!m.empty() && isspace((unsigned char)m.front())) m.erase(m.begin());
            if (!m.empty() && !TorrentHost::IsTorrentSource(m)) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", T("That's not a magnet link."));
            ImGui::BeginDisabled(m.compare(0, 7, "magnet:") != 0);
            if (ImGui::Button(T("Next"), ImVec2(0, h))) {
                torrentAddSource = m;
                torrentMagnetBuf.clear();
                torrentAddDir = here ? Torrents().DefaultDir() : "";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
        } else {
            bool magnet = torrentAddSource.compare(0, 7, "magnet:") == 0;
            std::string shown = magnet ? torrentAddSource.substr(0, 80) + (torrentAddSource.size() > 80 ? "..." : "") : BaseName(torrentAddSource);
            // (a magnet link's name, when it carries one)
            size_t dn = magnet ? torrentAddSource.find("dn=") : std::string::npos;
            if (dn != std::string::npos) {
                std::string n = torrentAddSource.substr(dn + 3, torrentAddSource.find('&', dn) - dn - 3), out;
                for (size_t i = 0; i < n.size(); i++) {
                    if (n[i] == '+') out += ' ';
                    else if (n[i] == '%' && i + 2 < n.size() && isxdigit((unsigned char)n[i + 1]) && isxdigit((unsigned char)n[i + 2])) {
                        out += (char)std::stoi(n.substr(i + 1, 2), nullptr, 16);
                        i += 2;
                    } else out += n[i];
                }
                if (!out.empty()) shown = out;
            }
            ImGui::TextDisabled("%s", shown.c_str());
            if (here) {
                ImGui::TextDisabled("%s", T("Save to:"));
                ImGui::SetNextItemWidth(wide);
                ImGui::InputText("##tdir", &torrentAddDir);
                if (ImGui::Button(T("Browse..."), ImVec2(0, h))) {
                    browseFor = BrowseFor::TorrentDir;
                    browser.Open(FileBrowser::Mode::SelectDir, T("Save the torrent to"), torrentAddDir);
                }
            } else if (comps.empty()) {
                ImGui::TextUnformatted(T("Torrents are downloaded by LunaShare on a computer (Windows or Linux). Link one to your account "
                                         "(Settings > Account there) - then links opened here are downloaded on it and sent to you."));
            } else {
                if (std::find(comps.begin(), comps.end(), torrentAddDevice) == comps.end()) torrentAddDevice = comps[0];
                ImGui::TextUnformatted(T("Download it on:"));
                for (auto& c : comps) {
                    std::string label = Emoji().Display(eng.PeerName(c)) + (eng.OwnOnline(c) ? "" : std::string("  ") + T("(offline)")) + "##" + c;
                    if (ImGui::RadioButton(label.c_str(), torrentAddDevice == c)) torrentAddDevice = c;
                }
                ImGui::TextUnformatted(T("When it's done:"));
                ImGui::RadioButton(T("Send it to this device"), &torrentAddThen, 0);
                ImGui::RadioButton(T("Keep it on the computer"), &torrentAddThen, 1);
                if (!eng.OwnOnline(torrentAddDevice)) ImGui::TextDisabled("%s", T("It's offline: the link is kept and handed over when it comes online."));
            }
            if (!torrentAddErr.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", torrentAddErr.c_str());
            ImGui::BeginDisabled(!here && comps.empty());
            if (ImGui::Button(T("Download"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h))) {
                std::string id, err;
                bool ok = here ? Torrents().Add(torrentAddSource, torrentAddDir, id, err)
                               : Torrents().RemoteAdd(torrentAddDevice, torrentAddSource, torrentAddThen == 0 ? "send" : "keep", err);
                if (ok) {
                    if (!here) torrentDevice = torrentAddDevice;
                    torrentAddSource.clear();
                    ImGui::CloseCurrentPopup();
                } else {
                    torrentAddErr = TF("Can't add it: %s", err.c_str());
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
        }
        if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h))) {
            torrentAddSource.clear();
            torrentMagnetBuf.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }

    // ---- the files of one (here): which ones to download
    if (torrentFilesOpen) {
        ImGui::OpenPopup("##tfiles");
        torrentFilesOpen = false;
        g_files.id = torrentFilesId;
        g_files.files = Torrents().Files(torrentFilesId);
        g_files.want.clear();
        for (auto& f : g_files.files) g_files.want.push_back(f.want);
        g_files.at = SteadyMs();
    }
    place();
    ImGui::SetNextWindowSizeConstraints(ImVec2(mobile ? vp->WorkSize.x * 0.94f : ImGui::GetFontSize() * 30, 0), ImVec2(vp->WorkSize.x * 0.94f, vp->WorkSize.y * 0.8f));
    if (ImGui::BeginPopupModal("##tfiles", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T("Files to download"));
        if (g_files.files.empty()) {
            ImGui::TextDisabled("%s", T("Not known yet (a magnet link first fetches the list from other people)."));
            if (SteadyMs() - g_files.at > 1500) {  // (looked at again while the popup is open)
                g_files.files = Torrents().Files(g_files.id);
                g_files.want.clear();
                for (auto& f : g_files.files) g_files.want.push_back(f.want);
                g_files.at = SteadyMs();
            }
        } else if (SteadyMs() - g_files.at > 1500) {  // (the progress moves; the ticks stay as they are)
            auto now = Torrents().Files(g_files.id);
            if (now.size() == g_files.files.size())
                for (size_t i = 0; i < now.size(); i++) g_files.files[i].done = now[i].done;
            g_files.at = SteadyMs();
        }
        if (ImGui::Button(T("All"), ImVec2(0, h))) std::fill(g_files.want.begin(), g_files.want.end(), true);
        ImGui::SameLine();
        if (ImGui::Button(T("None"), ImVec2(0, h))) std::fill(g_files.want.begin(), g_files.want.end(), false);
        ImGui::BeginChild("tfl", ImVec2(mobile ? 0 : ImGui::GetFontSize() * 36, std::min(vp->WorkSize.y * 0.5f, ImGui::GetFrameHeightWithSpacing() * (mobile ? 2.0f : 1.0f) * ((float)g_files.files.size() + 0.5f))));
        if (mobile) ui::TouchScroll();
        for (size_t i = 0; i < g_files.files.size(); i++) {
            auto& f = g_files.files[i];
            ImGui::PushID((int)i);
            bool w = g_files.want[i];
            if (ImGui::Checkbox("##w", &w)) g_files.want[i] = w;
            ImGui::SameLine();
            ImGui::TextUnformatted(f.path.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s  %d%%", FormatBytes((double)f.size).c_str(), f.size > 0 ? (int)(f.done * 100 / f.size) : 100);
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (ImGui::Button(T("OK"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h))) {
            if (!g_files.files.empty()) {
                // (the list leaves out padding files: back to one tick per file of the torrent)
                int n = 0;
                for (auto& f : g_files.files) n = std::max(n, f.index + 1);
                std::vector<bool> all((size_t)n, false);
                for (size_t i = 0; i < g_files.files.size(); i++) all[(size_t)g_files.files[i].index] = g_files.want[i];
                Torrents().SetWanted(g_files.id, all);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- remove
    if (torrentRemoveOpen) {
        ImGui::OpenPopup("##tremove");
        torrentRemoveOpen = false;
        torrentRemoveFiles = false;
    }
    place();
    if (ImGui::BeginPopupModal("##tremove", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (mobile ? ImGui::GetContentRegionAvail().x : ImGui::GetFontSize() * 24));
        ImGui::TextUnformatted(TF("Remove \"%s\" from the list?", torrentRemoveName.c_str()).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Checkbox(torrentRemoveDevice.empty() ? T("Also delete its files") : T("Also delete its files on the computer"), &torrentRemoveFiles);
        if (ImGui::Button(T("Remove"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h))) {
            if (watchExtTorrent == torrentRemoveId && eng.watch.sid) eng.LeaveWatch();
            if (torrentRemoveDevice.empty()) Torrents().Remove(torrentRemoveId, torrentRemoveFiles);
            else Torrents().RemoteRemove(torrentRemoveDevice, torrentRemoveId, torrentRemoveFiles);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- settings (here)
    if (torrentSettingsOpen) {
        ImGui::OpenPopup("##tsettings");
        torrentSettingsOpen = false;
    }
    place();
    if (ImGui::BeginPopupModal("##tsettings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        Settings s = eng.GetSettings();
        bool ch = false;
        ImGui::TextUnformatted(T("Torrent settings"));
        ImGui::TextDisabled("%s", T("Save to:"));
        std::string dir = Torrents().DefaultDir();
        ImGui::SetNextItemWidth(wide);
        ImGui::InputText("##tsdir", &dir, ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button(T("Browse..."), ImVec2(0, h))) {
            browseFor = BrowseFor::TorrentSaveDir;
            browser.Open(FileBrowser::Mode::SelectDir, T("Save torrents to"), dir);
        }
        float iw = ImGui::GetFontSize() * 8;
        ImGui::SetNextItemWidth(iw);
        if (ImGui::InputInt(T("Download limit (KiB/s, 0 = none)"), &s.torrentDown, 0)) ch = true;
        ImGui::SetNextItemWidth(iw);
        if (ImGui::InputInt(T("Upload limit (KiB/s, 0 = none)"), &s.torrentUp, 0)) ch = true;
        const int seedVals[] = {0, 100, 200, -1};
        const char* seedNames[] = {T("Stop when it's downloaded"), T("Until as much was uploaded as downloaded"), T("Until twice as much was uploaded"),
                                   T("Keep sharing until I stop it")};
        int cur = 1;
        for (int i = 0; i < 4; i++)
            if (seedVals[i] == s.torrentSeed) cur = i;
        ImGui::TextDisabled("%s", T("Sharing a finished torrent with others:"));
        ImGui::SetNextItemWidth(wide);
        if (ImGui::BeginCombo("##tseed", seedNames[cur])) {
            for (int i = 0; i < 4; i++)
                if (ImGui::Selectable(seedNames[i], cur == i, 0, ImVec2(0, h))) {
                    s.torrentSeed = seedVals[i];
                    ch = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(iw);
        if (ImGui::InputInt(T("Port (0 = any)"), &s.torrentPort, 0)) ch = true;
        if (ImGui::Checkbox(T("My other devices can add torrents here"), &s.torrentRemote)) ch = true;
        ImGui::SameLine();
        ui::HelpMarker(T("A phone linked to your account can hand a magnet link or .torrent file to this computer, see how far it is, get the "
                         "files sent when it's done, or watch them from here."));
        if (ch) {
            s.torrentDown = std::clamp(s.torrentDown, 0, 1000000);
            s.torrentUp = std::clamp(s.torrentUp, 0, 1000000);
            s.torrentPort = std::clamp(s.torrentPort, 0, 65535);
            eng.ApplySettings(s);
            edit.torrentDown = s.torrentDown;
            edit.torrentUp = s.torrentUp;
            edit.torrentSeed = s.torrentSeed;
            edit.torrentPort = s.torrentPort;
            edit.torrentRemote = s.torrentRemote;
            Torrents().ApplySettings(s);
        }
        ImGui::TextDisabled("libtorrent %s", Torrents().version.c_str());
        if (ImGui::Button(T("Close"), ImVec2(mobile ? -1 : 0, h))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::DrawTorrents() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    bool here = Torrents().Available();
    std::vector<std::string> comps = here ? std::vector<std::string>() : Torrents().Computers();
    // errors a computer reported for links handed over
    for (auto& e : Torrents().remoteErrors) toasts.push_back({EventType::Error, T("Torrents"), TF("Can't add it: %s", e.c_str()), "", 0, 6.0f});
    Torrents().remoteErrors.clear();

    if (ui::IconTextButton(T("Add a torrent..."), Icon::Plus, ImVec2(0, h))) {
        torrentAddSource.clear();
        torrentAddErr.clear();
        torrentAddOpen = true;
    }
    if (here) {
        ImGui::SameLine();
        if (ui::IconTextButton(T("Settings..."), Icon::Gear, ImVec2(0, h))) torrentSettingsOpen = true;
    }
    const std::vector<Torrent>* list = nullptr;
    std::string device;
    bool fresh = true;
    if (here) {
        list = &Torrents().List();
    } else if (!comps.empty()) {
        if (std::find(comps.begin(), comps.end(), torrentDevice) == comps.end()) {
            torrentDevice = comps[0];
            for (auto& c : comps)
                if (eng.OwnOnline(c)) torrentDevice = c;
        }
        device = torrentDevice;
        if (comps.size() > 1) {
            if (!mobile) ImGui::SameLine();
            ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 14);
            if (ImGui::BeginCombo("##tdev", Emoji().Display(eng.PeerName(device)).c_str())) {
                for (auto& c : comps)
                    if (ImGui::Selectable((Emoji().Display(eng.PeerName(c)) + "##" + c).c_str(), c == device, 0, ImVec2(0, h))) torrentDevice = c;
                ImGui::EndCombo();
            }
        }
        Torrents().RemoteWatchList(device);
        list = Torrents().RemoteList(device, &fresh);
        bool online = eng.OwnOnline(device);
        std::string who = Emoji().Display(eng.PeerName(device));
        ImGui::TextDisabled("%s", online ? TF("On %s", who.c_str()).c_str() : TF("%s is offline", who.c_str()).c_str());
        int waiting = Torrents().PendingCount(device);
        if (waiting) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", TF("- %d link(s) waiting for it", waiting).c_str());
        }
        if (online && (!list || !fresh)) busy = true;  // (the list is on its way)
    }
    if (Torrents().Active()) busy = true;
    // totals
    if (list && !list->empty()) {
        int down = 0, up = 0;
        for (auto& t : *list) down += t.downRate, up += t.upRate;
        if (down || up) {
            ImGui::SameLine();
            ImGui::TextDisabled("   %s %s   %s %s", T("down"), Rate(down).c_str(), T("up"), Rate(up).c_str());
        }
    }
    ImGui::Separator();
    ImGui::BeginChild("torrents");
    if (mobile) ui::TouchScroll();
    if (!here && comps.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", IsMobilePlatform()
                                      ? T("Torrents are downloaded by LunaShare on a computer (Windows or Linux). Link one to your account "
                                          "(Settings > Account there) - then links opened here are downloaded on it and sent to you.")
                                      : T("The torrent module isn't installed here (modules/lunashare_torrent)."));
        ImGui::PopTextWrapPos();
    } else if (Torrents().RemoteOff(device)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("That computer doesn't take torrents from your devices: turn it on there (Torrents > Settings...)."));
        ImGui::PopTextWrapPos();
    } else if (!list) {
        ImGui::TextDisabled("%s", eng.OwnOnline(device) ? T("Loading...") : T("Its torrents show when it's online."));
    } else if (list->empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("No torrents. Add a magnet link or a .torrent file - or open one with LunaShare."));
        ImGui::PopTextWrapPos();
    }
    if (list) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (size_t i = 0; i < list->size(); i++) {
            const Torrent& t = (*list)[i];
            ImGui::PushID(t.id.c_str());
            ImGui::TextUnformatted(t.name.c_str());
            // the bar, with what's going on written in it
            float frac = t.size > 0 ? (float)((double)t.done / (double)t.size) : 0.0f;
            const char* st = t.state == "meta"       ? T("getting the file list...")
                             : t.state == "checking" ? T("checking")
                             : t.state == "down"     ? T("downloading")
                             : t.state == "seed"     ? T("done - sharing")
                             : t.state == "done"     ? T("done")
                             : t.state == "paused"   ? T("paused")
                                                     : T("error");
            std::string text = t.size > 0 ? TF("%d%% of %s", (int)(frac * 100), FormatBytes((double)t.size).c_str()) : std::string();
            text += (text.empty() ? "" : "  -  ") + std::string(st);
            if (t.state == "down" && t.downRate > 0) {
                text += "  " + Rate(t.downRate);
                int64_t left = (t.size - t.done) / std::max(t.downRate, 1);
                if (left < 100 * 3600) text += "  " + (left >= 3600 ? TF("%dh %dmin left", (int)(left / 3600), (int)(left % 3600 / 60)) : left >= 60 ? TF("%d min left", (int)(left / 60)) : TF("%d s left", (int)left));
            }
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x, bh = ImGui::GetFrameHeight();
            ImGui::Dummy(ImVec2(w, bh));
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + bh), ImGui::GetColorU32(ImGuiCol_FrameBg), 4);
            ImU32 col = t.state == "error" ? IM_COL32(170, 60, 60, 255) : t.Complete() ? IM_COL32(50, 130, 80, 255) : ImGui::GetColorU32(ImGuiCol_PlotHistogram);
            if (frac > 0) dl->AddRectFilled(p, ImVec2(p.x + w * std::min(frac, 1.0f), p.y + bh), col, 4);
            ImVec4 clip(p.x + 4, p.y, p.x + w - 4, p.y + bh);
            dl->AddText(nullptr, 0, ImVec2(p.x + 8, p.y + (bh - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(240, 240, 240, 255), text.c_str(), nullptr, 0, &clip);
            if (t.state == "error" && !t.error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", t.error.c_str());
            std::string sub = TF("%d peers", t.peers);
            if (t.upRate > 0) sub += std::string("   ") + T("up") + " " + Rate(t.upRate);
            if (t.uploaded > 0) sub += "   " + TF("%s uploaded", FormatBytes((double)t.uploaded).c_str());
            if (!t.from.empty() && here) sub += "   " + TF("for %s", Emoji().Display(eng.PeerName(t.from)).c_str()) + (t.then == "send" ? std::string(" ") + (t.sent ? T("(sent)") : T("(sent when done)")) : "");
            if (!here && t.then == "send") sub += std::string("   ") + (t.sent ? T("sent here") : T("sent here when done"));
            ImGui::TextDisabled("%s", sub.c_str());
            // buttons
            float bw = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f : 0;
            int nb = 0;
            auto next = [&] {
                if (nb++ && (!mobile || nb % 2 == 0)) ImGui::SameLine();
            };
            bool stopped = t.paused;
            next();
            if (ui::IconTextButton(stopped ? (t.Complete() ? T("Share again") : T("Resume")) : (t.Complete() ? T("Stop sharing") : T("Pause")), stopped ? Icon::Play : Icon::Pause, ImVec2(bw, h))) {
                if (here) Torrents().Pause(t.id, !stopped);
                else Torrents().RemotePause(device, t.id, !stopped);
            }
            if (t.watchFile >= 0 && Player().Available()) {
                next();
                if (ui::IconTextButton(T("Watch"), Icon::Video, ImVec2(bw, h))) WatchTorrent(device, t.id, t.watchFile, t.watchSize, t.watchName);
                if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", t.watchName.c_str());
            }
            if (here) {
                next();
                if (ui::IconTextButton(T("Files..."), Icon::List, ImVec2(bw, h))) {
                    torrentFilesId = t.id;
                    torrentFilesOpen = true;
                }
                next();
                if (ui::IconTextButton(T("Open folder"), Icon::Folder, ImVec2(bw, h))) {
                    std::string d = t.single || !IsDirectory(t.path) ? t.dir : t.path;
                    if (Bridge().openFolder) Bridge().openFolder(d);
                    else OpenInFileManager(d);
                }
                if (t.Complete()) {
                    next();
                    if (ui::IconTextButton(T("Send..."), Icon::Send, ImVec2(bw, h))) OpenSendTo({t.path});
                }
            } else if (t.Complete() && !(t.then == "send" && !t.sent)) {
                next();
                if (ui::IconTextButton(t.sent ? T("Send here again") : T("Send here"), Icon::Down, ImVec2(bw, h))) Torrents().RemoteSend(device, t.id);
            }
            next();
            if (ui::IconTextButton(T("Remove..."), Icon::Trash, ImVec2(bw, h))) {
                torrentRemoveId = t.id;
                torrentRemoveName = t.name;
                torrentRemoveDevice = device;
                torrentRemoveOpen = true;
            }
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}
