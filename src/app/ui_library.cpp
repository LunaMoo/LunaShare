// Library: my Shared folder, downloads from contacts' Shared folders (several sources at once), and what my
// contacts share.
#include <algorithm>
#include <map>
#include <set>

#include "i18n.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "music.h"
#include "platform.h"
#include "player_host.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

static std::string Lower(std::string s) {
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

static void Reveal(const std::string& p) {
    if (Bridge().openFolder) Bridge().openFolder(p);
    else OpenInFileManager(p);
}

static void OpenIt(const std::string& p) {
    if (Bridge().openFile) Bridge().openFile(p);
    else OpenWithDefaultApp(p);
}

void App::DrawLibrary() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    float bw = mobile ? -1 : 0;
    ImGui::BeginChild("library");
    if (mobile) ui::TouchScroll();
    DrawWatchResume();
    if (Player().Available()) {
        if (ui::IconTextButton(T("Movie player..."), Icon::Play, ImVec2(mobile ? -1 : 0, h))) OpenMoviePlayer();
        if (!mobile) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", T("Play a movie here - you can add people to watch with you afterwards."));
        }
        if (ui::IconTextButton(T("Music player"), Icon::Speaker, ImVec2(mobile ? -1 : 0, h))) {
            if (mobile) {
                backStack.push_back(screen);
                screen = MScreen::Music;
                musicNowShown = false;
            } else {
                tab = Tab::Music;
                tabForce = true;
            }
        }
        if (!mobile) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", T("Your music, songs from contacts' Shared folders, listening together."));
        }
    }

    // ---- my Shared folder
    ImGui::SeparatorText(T("Your Shared folder"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", T("Your contacts can download what you put in this folder. What you download from them lands here too and "
                                "is passed on: the more of you have a file, the faster it comes - from everyone at once."));
    ImGui::PopTextWrapPos();
    std::string dir = eng.SharedDir();
    int64_t total = 0;
    for (auto& f : eng.myLibrary) total += f.size;
    ImGui::TextUnformatted(dir.c_str());
    ImGui::TextDisabled(T("%d files, %s"), (int)eng.myLibrary.size(), FormatBytes((double)total).c_str());
    if (eng.libHashing > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled(T("- reading %d new file(s)..."), eng.libHashing);
    }
    if (ui::IconTextButton(T("Open folder"), Icon::Folder, ImVec2(mobile ? -1 : 0, h))) {
        MakeDirs(dir);
        Reveal(dir);
    }
    if (!mobile) ImGui::SameLine();
    if (ui::IconTextButton(T("Look again"), Icon::Retry, ImVec2(mobile ? -1 : 0, h))) eng.RescanShared();
    bool changed = false;
    changed |= ImGui::Checkbox(T("Let my contacts download from my Shared folder"), &edit.shareLibrary);
    ui::HelpMarker(T("Contacts are your own devices, devices you trust, and people you have exchanged messages or files with."));
    if (IsMobilePlatform()) changed |= ImGui::Checkbox(T("Also on mobile data"), &edit.shareOnMobileData);
    if (changed) {
        eng.ApplySettings(edit);
        editDirty = false;
    }

    // ---- downloads
    bool anyKept = false;
    for (auto& [id, d] : eng.swarm) anyKept |= d->keep;
    auto movieSaves = eng.WatchSaves();
    if (anyKept || !movieSaves.empty()) {
        ImGui::SeparatorText(T("Downloads"));
        for (auto& s : movieSaves) {  // copies of movies watched together, still coming after the movie ended
            ImGui::PushID((int)(s.sid & 0x7fffffff));
            ImGui::TextUnformatted(s.name.c_str());
            std::string status = s.connected ? TF("%d%% - saving the movie", (int)(s.frac * 100))
                                             : TF("%d%% - waiting for %s", (int)(s.frac * 100), eng.PeerName(s.key).c_str());
            float pw = mobile ? ImGui::GetContentRegionAvail().x : ImGui::GetFontSize() * 22;
            ui::ProgressBarText((float)s.frac, status.c_str(), ImVec2(pw, ImGui::GetFrameHeight()));
            if (!mobile) ImGui::SameLine();
            if (ui::IconButton("wcancel", Icon::Cancel, T("Cancel"), h > 0 ? h : 0, true)) eng.CancelWatchSave(s.sid);
            ImGui::PopID();
        }
        std::vector<std::shared_ptr<SwarmDownload>> list;  // (shared-folder downloads)
        for (auto& [id, d] : eng.swarm)
            if (d->keep) list.push_back(d);  // (not the ones only being played)
        std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a->created > b->created; });
        std::string removeId;
        for (auto& d : list) {
            ImGui::PushID(d->id.c_str());
            ImGui::TextUnformatted(d->rel.c_str());
            float frac = d->size ? (float)((double)d->done / (double)d->size) : 0.0f;
            int sources = 0;
            for (auto& [dev, s] : d->sources) sources += s.full || !s.bits.empty();
            std::string status;
            switch (d->state) {
                case SwarmDownload::Searching: status = T("looking for devices that have it..."); break;
                case SwarmDownload::Running:
                    status = TF("%d%% - %s/s - from %d device(s)", (int)(frac * 100), FormatBytes(d->speed).c_str(), sources);
                    if (sources == 0) status = TF("%d%% - waiting for a device that has it", (int)(frac * 100));
                    break;
                case SwarmDownload::Paused: status = TF("%d%% - paused", (int)(frac * 100)); break;
                case SwarmDownload::Done: status = T("done - in your Shared folder"); break;
                case SwarmDownload::Failed: status = std::string(T("failed: ")) + d->error; break;
            }
            float pw = mobile ? ImGui::GetContentRegionAvail().x : ImGui::GetFontSize() * 22;
            ui::ProgressBarText(frac, status.c_str(), ImVec2(pw, ImGui::GetFrameHeight()),
                                d->state == SwarmDownload::Done ? ui::ColOk() : d->state == SwarmDownload::Failed ? ui::ColErr() : 0);
            if (!mobile) ImGui::SameLine();
            if (d->state == SwarmDownload::Done) {
                if (ui::IconTextButton(T("Open"), Icon::Open, ImVec2(0, h))) OpenIt(d->finalPath);
                ImGui::SameLine();
                if (ui::IconTextButton(T("Remove from list"), Icon::Cancel, ImVec2(0, h))) removeId = d->id;
            } else {
                bool paused = d->state == SwarmDownload::Paused;
                if (ui::IconButton("pause", paused ? Icon::Play : Icon::Pause, paused ? T("Resume") : T("Pause"), h > 0 ? h : 0))
                    eng.PauseShared(d->id, !paused);
                ImGui::SameLine();
                if (ui::IconButton("cancel", Icon::Cancel, T("Cancel"), h > 0 ? h : 0, true)) eng.CancelShared(d->id);
            }
            ImGui::PopID();
        }
        if (!removeId.empty()) eng.swarm.erase(removeId);
    }

    // ---- contacts' Shared folders
    ImGui::SeparatorText(T("Your contacts' Shared folders"));
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 18);
    ImGui::InputTextWithHint("##libsearch", T("search"), &librarySearch);
    std::string q = Lower(librarySearch);
    for (auto& c : eng.conns)  // contacts whose list we don't have yet (they may have become contacts after connecting)
        if (c->ready && !c->closed && eng.IsContact(c->peerId)) {
            auto& pl = eng.peerLibraries[c->peerId];
            if (!pl.fetched && !pl.pending) eng.RefreshLibrary(c->peerId);
        }
    // how many devices offer each file (more = faster)
    std::map<std::string, int> offers;
    for (auto& [dev, pl] : eng.peerLibraries)
        if (eng.IsConnected(dev))
            for (auto& f : pl.files) offers[f.id]++;
    int shown = 0;
    for (auto& key : SortedPeers(true)) {
        std::vector<LibFile> files;
        std::set<std::string> seen;
        bool any = false, pending = false, off = true;
        for (auto& dev : eng.DevicesOf(key)) {
            auto it = eng.peerLibraries.find(dev);
            if (it == eng.peerLibraries.end()) continue;
            any = true;
            pending |= it->second.pending;
            off &= it->second.off;
            for (auto& f : it->second.files)
                if (seen.insert(f.id).second && (q.empty() || Lower(f.rel).find(q) != std::string::npos)) files.push_back(f);
        }
        if (!any) continue;
        shown++;
        ImGui::PushID(key.c_str());
        std::string head = eng.PeerName(key) + (off ? std::string("  - ") + T("doesn't share with you") : "  (" + std::to_string(files.size()) + ")");
        if (ImGui::CollapsingHeader((head + "###lib").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (pending) ImGui::TextDisabled("%s", T("Updating..."));
            std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.rel < b.rel; });
            size_t limit = 500;
            for (size_t i = 0; i < files.size() && i < limit; i++) {
                const LibFile& f = files[i];
                ImGui::PushID(f.id.c_str());
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(f.rel.c_str());
                ImGui::SameLine();
                int n = offers.count(f.id) ? offers[f.id] : 0;
                std::string info = n > 1 ? TF("%s - on %d devices", FormatBytes((double)f.size).c_str(), n) : FormatBytes((double)f.size);
                ImGui::TextDisabled("%s", info.c_str());
                if (!mobile) ImGui::SameLine();
                auto sw = eng.swarm.find(f.id);
                if (Player().Available() && music::Library::IsAudioPath(f.rel)) {  // music: in the music player, while it comes
                    if (ui::IconTextButton(T("Play"), Icon::Play, ImVec2(mobile ? -1 : 0, h))) {
                        std::vector<LibFile> songs;  // (the songs of that folder after it, for "next")
                        int at = 0;
                        std::string dir = DirName(f.rel);
                        for (auto& g : files)
                            if (music::Library::IsAudioPath(g.rel) && DirName(g.rel) == dir) {
                                if (g.id == f.id) at = (int)songs.size();
                                songs.push_back(g);
                            }
                        MusicPlayLib(songs, at, eng.PeerName(key));
                    }
                    if (!mobile) ImGui::SameLine();
                } else if (Player().Available() && Player().Playable(f.rel)) {  // watch it while it comes
                    if (ui::IconTextButton(T("Play"), Icon::Play, ImVec2(mobile ? -1 : 0, h)) && eng.StartLibraryWatch(f)) watchShown = true;
                    if (!mobile) ImGui::SameLine();
                }
                if (eng.HaveShared(f.id)) {
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ColOk()), "%s", T("you have it"));
                } else if (sw != eng.swarm.end() && sw->second->keep && sw->second->state != SwarmDownload::Done) {
                    ImGui::TextDisabled(T("downloading %d%%"), (int)(sw->second->size ? sw->second->done * 100 / sw->second->size : 0));
                } else if (ui::IconTextButton(T("Download file"), Icon::Down, ImVec2(mobile ? -1 : 0, h))) {
                    eng.DownloadShared(f);
                }
                ImGui::PopID();
            }
            if (files.size() > limit) ImGui::TextDisabled(T("... and %d more (search to find them)"), (int)(files.size() - limit));
            if (files.empty() && !off) ImGui::TextDisabled("%s", q.empty() ? T("Nothing shared yet.") : T("Nothing matches."));
            if (ImGui::SmallButton(T("Refresh"))) eng.RefreshLibrary(key);
        }
        ImGui::PopID();
    }
    if (!shown) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("When your contacts are connected, what they share appears here."));
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0, h));
    ImGui::EndChild();
}
