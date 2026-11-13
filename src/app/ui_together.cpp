// LunaShare - doing things together, from one place: during a call (or in a chat) the Together button offers watching
// a movie, listening to music and reading a comic with that person - and joining what they've started. Reading a
// comic together is here too: the comic's pages in the picture viewer, the page anyone turns turned for everyone.
#include <algorithm>

#include "emoji_host.h"
#include "i18n.h"
#include "imgui.h"
#include "music.h"
#include "platform.h"
#include "player_host.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

// the conversation of the call that's on ("" = none)
std::string App::CallChatKey() {
    const CallInfo& c = eng.call;
    if (c.state == CallState::None) return {};
    return c.group.empty() ? eng.ChatKey(c.peer) : c.group;
}

// something they invited me to (not joined yet): to show on the Together button
bool App::TogetherInvited(const std::string& key) {
    for (auto& i : eng.watchInvites)
        if (i.key == key && i.sid != eng.watch.sid && SteadyMs() - i.at < 6 * 3600 * 1000LL) return true;
    for (auto& i : eng.listenInvites)
        if (i.key == key && i.sid != eng.listen.sid) return true;
    for (auto& i : eng.readInvites)
        if (i.key == key && i.sid != eng.read.sid) return true;
    return false;
}

void App::OpenTogether(const std::string& key) {
    if (key.empty()) return;
    togetherKey = key;
    togetherOpen = true;
}

// listening together with them: what's playing goes on for both (then the rest of my queue); with nothing on, the
// music list opens - songs tapped there go to the shared queue
void App::ListenWith(const std::string& key) {
    if (!musicLibList.empty()) MusicLibEnd();
    music::Now now = music::Play().GetNow();
    bool haveNow = now.active && !now.external;
    if (!eng.StartListen(key)) return;
    if (haveNow) {
        auto q = music::Play().Queue();
        std::vector<std::string> paths;
        for (int i = std::max(0, now.index); i < (int)q.size(); i++) paths.push_back(q[(size_t)i]);
        if (paths.empty()) paths.push_back(now.path);
        MusicListenAdd(paths, now.pos, true);
    } else {
        toasts.push_back({EventType::Info, T("Listening together"), T("Tap songs to add them - they play for both of you."), "", 0, 5.0f});
    }
    if (ui::M().mobile) {
        if (screen != MScreen::Music) backStack.push_back(screen);
        screen = MScreen::Music;
        musicNowShown = haveNow;
        musicView = 0;
    } else {
        tab = Tab::Music;
        tabForce = true;
    }
}

// reading together with them: the comics open, and the one picked is shared
void App::ReadWithPick(const std::string& key) {
    readWith = key;
    archivesComics = true;
    archivePick.Clear();
    OpenArchives();
    toasts.push_back({EventType::Info, T("Read together"), T("Pick a comic - it opens for both of you."), "", 0, 5.0f});
}

// the Together menu (a call's, or a chat's)
void App::DrawTogetherPopup() {
    if (togetherOpen) {
        ImGui::OpenPopup("##together");
        togetherOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.85f : ImGui::GetFrameHeight() * 1.2f;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.88f, 0));
    // (a modal: drawn outside the app's window, an ordinary popup would close when that window takes the focus)
    if (!ImGui::BeginPopupModal("##together", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)))
        togetherClose = true;  // (a tap outside closes it, like a menu)
    const std::string& key = togetherKey;
    std::string who = Emoji().Display(eng.PeerName(key));
    ImGui::TextUnformatted(TF("Together with %s", who.c_str()).c_str());
    ImGui::Separator();
    float w = mobile ? -1 : ImGui::GetFontSize() * 17;
    bool done = false;
    auto item = [&](const char* id, Icon icon, const std::string& label, bool accent = false) {
        if (accent) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 140, 85, 255));
        bool r = ui::IconTextButton((label + "##" + id).c_str(), icon, ImVec2(w, h));
        if (accent) ImGui::PopStyleColor();
        return r;
    };
    // what they've started: join it
    for (auto& i : eng.watchInvites) {
        if (i.key != key || i.sid == eng.watch.sid || SteadyMs() - i.at >= 6 * 3600 * 1000LL) continue;
        if (item(("jw" + std::to_string(i.sid)).c_str(), Icon::Play, TF("Join: watching \"%s\"", i.name.c_str()), true)) {
            if (eng.JoinWatch(i.sid)) watchShown = true;
            else toasts.push_back({EventType::Error, T("Watch together"), T("Not connected to them - trying again..."), "", 0, 4.0f});
            done = true;
        }
        break;
    }
    for (auto& i : eng.listenInvites) {
        if (i.key != key || i.sid == eng.listen.sid) continue;
        if (item(("jl" + std::to_string(i.sid)).c_str(), Icon::Speaker, T("Join: listening to music"), true)) {
            if (!musicLibList.empty()) MusicLibEnd();
            if (eng.JoinListen(i.sid)) {
                if (mobile) {
                    if (screen != MScreen::Music) backStack.push_back(screen);
                    screen = MScreen::Music;
                    musicNowShown = true;
                } else {
                    tab = Tab::Music;
                    tabForce = true;
                }
            } else {
                toasts.push_back({EventType::Error, T("Listening together"), T("Not connected to them - trying again..."), "", 0, 4.0f});
            }
            done = true;
        }
        break;
    }
    for (auto& i : eng.readInvites) {
        if (i.key != key || i.sid == eng.read.sid) continue;
        if (item(("jr" + std::to_string(i.sid)).c_str(), Icon::Book, TF("Join: reading \"%s\"", i.name.c_str()), true)) {
            JoinReadNow(i.sid);
            done = true;
        }
        break;
    }
    // what's on with them already: back to it
    if (eng.watch.sid && (eng.watch.key == key || std::find(eng.watchKeys.begin(), eng.watchKeys.end(), key) != eng.watchKeys.end())) {
        if (item("ow", Icon::Play, T("Back to the movie"))) {
            watchShown = true;
            done = true;
        }
    }
    if (eng.read.sid && eng.read.key == key && !viewer.open) {
        if (item("or", Icon::Book, T("Back to the comic"))) {
            readOpenPending = true;
            done = true;
        }
    }
    // start something
    bool media = Player().Available();
    if (media && item("w", Icon::Play, T("Watch a movie together"))) {
        OpenWatchPicker(key);
        done = true;
    }
    if (eng.listen.sid && eng.listen.key == key) {
        if (item("l", Icon::Speaker, T("Back to the music"))) {
            if (mobile) {
                if (screen != MScreen::Music) backStack.push_back(screen);
                screen = MScreen::Music;
                musicNowShown = true;
            } else {
                tab = Tab::Music;
                tabForce = true;
            }
            done = true;
        }
    } else if (item("l", Icon::Speaker, T("Listen to music together"))) {
        ListenWith(key);
        done = true;
    }
    if (item("r", Icon::Book, T("Read a comic together"))) {
        ReadWithPick(key);
        done = true;
    }
    ImGui::Spacing();
    if (ImGui::Button(T("Close"), ImVec2(w, mobile ? h * 0.8f : 0))) done = true;
    if (done || togetherClose) ImGui::CloseCurrentPopup();
    togetherClose = false;
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- reading a comic together

void App::JoinReadNow(uint64_t sid) {
    if (eng.JoinRead(sid)) {
        readOpenPending = true;
    } else {
        toasts.push_back({EventType::Error, T("Read together"), T("Not connected to them - trying again..."), "", 0, 4.0f});
    }
}

// every frame: the viewer follows the shared page (and tells it when I turn one); others' pages show as they come
void App::ReadFrame() {
    ReadInfo& r = eng.read;
    if (!r.sid) {
        if (viewer.together) {  // (it ended - the host closed it, or went away)
            viewer.together = false;
            if (viewer.open && readJoined) viewer.closeRequest = true;
        }
        readJoined = false;
        readOpenPending = false;
        return;
    }
    if (readOpenPending && r.pages > 0 && (int)r.files.size() == r.pages) {  // the pages (here, or still coming) in the viewer
        readOpenPending = false;
        std::vector<std::string> caps(r.files.size(), r.name);
        std::vector<std::string> paths = r.files;
        if (paths.empty()) return;
        // (OpenViewerList needs something at every place: pages still on their way are empty for now)
        OpenViewerList(paths, r.page, caps);
        viewer.paths = paths;
        viewer.together = true;
        readGot = -1;
        readJoined = !r.host.empty();
        readLocal = r.page;
    }
    if (!viewer.open || !viewer.together) {
        if (viewer.together && !viewer.open) {  // the viewer was closed: I stop reading (the host: for everyone)
            viewer.together = false;
            eng.LeaveRead();
        }
        return;
    }
    // pages that have come since
    if (!r.host.empty() && r.got != readGot) {
        readGot = r.got;
        for (size_t i = 0; i < r.files.size() && i < viewer.paths.size(); i++)
            if (viewer.paths[i].empty() && !r.files[i].empty()) viewer.paths[i] = r.files[i];
    }
    if (viewer.index != readLocal) {  // I turned the page
        readLocal = viewer.index;
        eng.ReadTurn(viewer.index);
    } else if (r.page != readLocal && r.page >= 0 && r.page < (int)viewer.paths.size()) {  // someone else did
        int d = r.page - viewer.index;
        readLocal = r.page;
        ViewerGo(d);
    }
}

// the comic viewer's Read together button: with whom (the person in the call first)
void App::ReadWhoPopup() {
    if (readWhoOpen) {
        ImGui::OpenPopup("##readwho");
        readWhoOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSizeConstraints(ImVec2(mobile ? vs.x * 0.8f : ImGui::GetFontSize() * 16, 0), ImVec2(vs.x * 0.95f, vs.y * 0.7f));
    if (!ImGui::BeginPopup("##readwho")) return;
    ImGui::SeparatorText(T("Read together with..."));
    std::vector<std::string> list;
    std::string inCall = CallChatKey();
    if (!inCall.empty()) list.push_back(inCall);
    for (auto& k : SortedPeers())
        if (k != inCall) list.push_back(k);
    int shown = 0;
    for (auto& k : list) {
        const KnownDevice* d = eng.DeviceFor(k);
        if (d && d->blocked) continue;
        shown++;
        std::string name = Emoji().Display(eng.PeerName(k));
        if (k == inCall) name += "  (" + std::string(T("in the call")) + ")";
        else if (eng.IsConnected(k)) name += "  \xE2\x97\x8F";
        if (ImGui::Selectable((name + "##" + k).c_str(), false, 0, ImVec2(0, h))) {
            std::string title = viewer.captions.empty() ? std::string() : viewer.captions[0];
            if (eng.StartRead(k, title, viewer.paths, viewer.index)) {
                viewer.together = true;
                readJoined = false;
                readLocal = viewer.index;
            }
            ImGui::CloseCurrentPopup();
        }
    }
    if (!shown) ImGui::TextDisabled("%s", T("No contacts yet."));
    ImGui::EndPopup();
}

// a comic that has just opened (ComicFrame): shared, when it was picked for reading together
void App::ReadComicOpened(const std::string& name) {
    if (readWith.empty()) return;
    std::string key = readWith;
    readWith.clear();
    if (eng.StartRead(key, name, viewer.paths, viewer.index)) {
        viewer.together = true;
        readJoined = false;
        readLocal = viewer.index;
    }
}

// the strip on top of a chat: they invite me to read / we're reading
void App::DrawReadBanner(const std::string& peer) {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    const ReadInviteInfo* inv = nullptr;
    for (auto& i : eng.readInvites)
        if (i.key == peer && i.sid != eng.read.sid) inv = &i;
    bool on = eng.read.sid && eng.read.key == peer;
    if (!inv && !on) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(50, 60, 110, 255));
    float rowH = (mobile ? h : ImGui::GetFrameHeight()) + ImGui::GetStyle().WindowPadding.y * 2;
    ImGui::BeginChild("readbanner", ImVec2(0, rowH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::AlignTextToFramePadding();
    std::string text = on ? TF("Reading \"%s\" together", eng.read.name.c_str())
                          : TF("%s invites you to read \"%s\" together", Emoji().Display(eng.PeerName(eng.ChatKey(inv->host))).c_str(), inv->name.c_str());
    const char* bl = on ? T("Open") : T("Join");
    float bw = ImGui::CalcTextSize(bl).x + ImGui::GetStyle().FramePadding.x * 2 + (mobile ? h * 0.5f : 0);
    float closeW = (h > 0 ? h : ImGui::GetFrameHeight()) + ImGui::GetStyle().ItemSpacing.x;
    float avail = ImGui::GetContentRegionAvail().x - bw - closeW - ImGui::GetStyle().ItemSpacing.x;
    std::string t = text;
    while (t.size() > 4 && ImGui::CalcTextSize(t.c_str()).x > avail) {
        t.pop_back();
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();
    }
    if (t != text) t += "...";
    ImGui::TextUnformatted(t.c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - bw - closeW);
    if (on) {
        if (ImGui::Button(bl, ImVec2(bw, h)) && !viewer.open) readOpenPending = true;
        ImGui::SameLine();
        if (ui::IconButton("rstop", Icon::Cancel, eng.read.host.empty() ? T("End for everyone") : T("Leave"), h)) {
            viewer.together = false;
            eng.LeaveRead();
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 140, 85, 255));
        bool go = ImGui::Button(bl, ImVec2(bw, h));
        ImGui::PopStyleColor();
        uint64_t sid = inv->sid;
        ImGui::SameLine();
        if (ui::IconButton("rdismiss", Icon::Cancel, T("Not now"), h)) {
            auto& v = eng.readInvites;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const ReadInviteInfo& x) { return x.sid == sid; }), v.end());
        } else if (go) {
            JoinReadNow(sid);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}
