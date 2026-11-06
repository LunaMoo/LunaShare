#include "ui.h"
#include "i18n.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "crypto.h"
#include "hash.h"
#include "avatar.h"
#include "notify.h"
#include "image_cache.h"
#include "media_host.h"
#include "player_host.h"
#include "emoji_host.h"
#include "wan.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "platform.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

static std::string Eta(double remaining, double speed) {
    if (speed < 1) return "";
    int s = (int)(remaining / speed);
    char b[32];
    if (s >= 3600) snprintf(b, sizeof(b), "%dh %02dm", s / 3600, (s / 60) % 60);
    else if (s >= 60) snprintf(b, sizeof(b), "%dm %02ds", s / 60, s % 60);
    else snprintf(b, sizeof(b), "%ds", s);
    return b;
}

static ImU32 StateColor(TState s) {
    switch (s) {
        case TState::Completed: return ui::ColOk();
        case TState::Failed: case TState::Cancelled: case TState::Rejected: return ui::ColErr();
        case TState::Paused: case TState::Interrupted: case TState::Pending: return ui::ColWarn();
        default: return ui::ColAccent();
    }
}

static void OpenPath(const std::string& p) {
    if (Bridge().openFile) Bridge().openFile(p);
    else OpenWithDefaultApp(p);
}

static void RevealPath(const std::string& p) {
    if (Bridge().openFolder) Bridge().openFolder(p);
    else OpenInFileManager(p);
}

App::App(Engine& e) : eng(e) {
    auto lk = eng.Lock();
    edit = eng.settings;
    browser.lastDir = eng.settings.lastBrowseDir;
    browser.prefs.grid = eng.settings.browserGrid;
    browser.prefs.picturesOnly = eng.settings.browserPicturesOnly;
    browser.prefs.sort = eng.settings.browserSort;
    browser.prefs.tile = eng.settings.browserTile;
    SetLanguage(eng.settings.language);
    Emoji().recent = eng.settings.emojiRecent;
    Emoji().tone = eng.settings.emojiTone;
}

bool App::IsMobileLayout() {
    switch (eng.settings.layout) {
        case UiLayout::Desktop: return false;
        case UiLayout::Mobile: return true;
        default: break;
    }
    if (IsMobilePlatform()) return true;
    ImVec2 sz = ImGui::GetMainViewport()->Size;
    return sz.x < sz.y && sz.x < 620 * ui::M().scale;  // narrow portrait window
}

// ============================================================================ frame

void App::Frame() {
    auto lk = eng.Lock();
    PullEvents();
    // found by username: open their chat as soon as the connection is there
    if (!findUserAcc.empty() && eng.IsConnected("@" + findUserAcc)) {
        SelectPeer("@" + findUserAcc);
        if (IsMobileLayout()) { backStack = {MScreen::Home}; screen = MScreen::Chat; }
        else { tab = Tab::Chat; tabForce = true; }
        findUserAcc.clear();
    }
    // taps on notifications
    for (notify::Action a; notify::Pop(a);) {
        if (a.what == "open" && !a.key.empty()) {
            SelectPeer(a.key);
            if (IsMobileLayout()) { backStack = {MScreen::Home}; screen = MScreen::Chat; }
            else { tab = Tab::Chat; tabForce = true; }
        } else if (a.what == "call" && eng.call.state == CallState::IncomingRinging) {
            openIncomingCall = true;
        } else if (a.what == "answer" && eng.call.state == CallState::IncomingRinging) {
            Media().WantCameraAtStart(false);
            eng.AcceptCall();
            SelectPeer(eng.call.peer);
            if (IsMobileLayout()) { backStack = {MScreen::Home}; screen = MScreen::Chat; }
            else { tab = Tab::Chat; tabForce = true; }
        }
    }
    ui::Metrics& m = ui::M();
    m.mobile = IsMobileLayout();
    m.rowH = ImGui::GetFrameHeight() * (m.mobile ? 1.9f : 1.0f);

    // keep the settings copy in sync with what the engine has (imports, other changes)
    if (!editDirty && !ImGui::IsAnyItemActive()) edit = eng.settings;

    // developer hooks used for automated screenshots
    static const char* autoSel = getenv("LUNASHARE_SCREEN") ? nullptr : getenv("LUNASHARE_AUTOSELECT");
    if (autoSel && selectedPeer.empty()) {
        for (auto& c : eng.conns)
            if (c->ready && !c->closed) {
                SelectPeer(c->peerId);
                if (m.mobile) { backStack = {MScreen::Home}; screen = MScreen::Chat; }
                break;
            }
    }
    static const char* autoWatch = getenv("LUNASHARE_AUTOWATCH");  // dev: join the first movie invitation (screenshots)
    if (autoWatch && !eng.watch.sid && !eng.watchInvites.empty() && eng.JoinWatch(eng.watchInvites.back().sid)) watchShown = *autoWatch != 'b';
    static const char* autoGroup = getenv("LUNASHARE_AUTOGROUP");  // dev: open the first group (screenshots)
    if (autoGroup && !Engine::IsGroupKey(selectedPeer) && !eng.groups.empty()) {
        SelectPeer("#" + eng.groups.begin()->first);
        if (m.mobile) { backStack = {MScreen::Home}; screen = MScreen::Chat; }
        if (*autoGroup == 'm') OpenGroupMembers(selectedPeer);
    }
    static const char* autoScreen = getenv("LUNASHARE_SCREEN");
    static bool autoScreenDone = false;
    if (autoScreen && !autoScreenDone) {
        autoScreenDone = true;
        std::string a = autoScreen;
        if (a == "transfers") { tab = Tab::Transfers; screen = MScreen::Transfers; }
        if (a == "history") { tab = Tab::History; screen = MScreen::History; }
        if (a == "devices") { tab = Tab::Devices; screen = MScreen::Devices; }
        if (a == "nearby") { tab = Tab::Nearby; screen = MScreen::Nearby; }
        if (a == "settings") { tab = Tab::Settings; screen = MScreen::Settings; }
        if (a == "library") { tab = Tab::Library; screen = MScreen::Library; }
        if (a == "home") screen = MScreen::Home;
        tabForce = true;
        autoSel = nullptr;
    }
    static const char* autoBrowse = getenv("LUNASHARE_BROWSE");
    static bool autoBrowseDone = false;
    if (autoBrowse && !autoBrowseDone) {
        autoBrowseDone = true;
        browseFor = BrowseFor::SendFiles;
        browser.Open(FileBrowser::Mode::OpenMulti, T("Send to ..."), autoBrowse);
        if (const char* n = getenv("LUNASHARE_BROWSE_SELECT")) browser.SelectFirstFiles(atoi(n));
    }
    static const char* autoAnswer = getenv("LUNASHARE_AUTOANSWER");
    static int64_t ringSince = 0;
    if (autoAnswer && eng.call.state == CallState::IncomingRinging) {
        if (!ringSince) ringSince = SteadyMs();
        if (SteadyMs() - ringSince > 1500) {
            Media().WantCameraAtStart(std::string(autoAnswer) == "video");
            eng.AcceptCall();
            ringSince = 0;
        }
    }
    static const char* autoView = getenv("LUNASHARE_AUTOVIEW");
    static bool autoViewDone = false;
    if (autoView && !autoViewDone && !selectedPeer.empty()) {
        for (auto& [id, t] : eng.transfers)
            if (t->state == TState::Completed && (ImageCache::IsImagePath(t->localPath) || PlayerHost::IsVideoPath(t->localPath)) && t->localPath.find(autoView) != std::string::npos) {
                OpenViewer(selectedPeer, t->localPath, false);
                autoViewDone = true;
                break;
            }
    }

    static bool emojiPanelEnv = getenv("LUNASHARE_EMOJIPANEL") != nullptr;  // dev: start with the emoji panel open
    if (emojiPanelEnv) emojiOpen = true, emojiPanelEnv = false;
    // dev: LUNASHARE_CONNECT=host:port connects once; LUNASHARE_AUTOCALL=video|voice calls the selected device once
    static const char* autoConnect = getenv("LUNASHARE_CONNECT");
    if (autoConnect) {
        std::string hp = autoConnect;
        size_t c = hp.rfind(':');
        if (c != std::string::npos) eng.ConnectAddress(hp.substr(0, c), (uint16_t)atoi(hp.c_str() + c + 1));
        autoConnect = nullptr;
    }
    static const char* autoCall = getenv("LUNASHARE_AUTOCALL");
    static int64_t autoCallAt = 0;
    if (autoCall && !selectedPeer.empty() && eng.IsConnected(selectedPeer) && eng.call.state == CallState::None) {
        if (!autoCallAt) autoCallAt = SteadyMs() + 1500;
        if (SteadyMs() > autoCallAt) {
            Media().WantCameraAtStart(std::string(autoCall) == "video");
            eng.StartCall(selectedPeer, autoCall);
            autoCall = nullptr;
        }
    }
    static const char* autoReact = getenv("LUNASHARE_AUTOREACT");  // dev: react to the first two messages
    if (autoReact && !selectedPeer.empty()) {
        auto it = eng.chats.find(selectedPeer);
        std::vector<std::string> mids;
        if (it != eng.chats.end())
            for (auto& m : it->second.messages)
                if (m.kind == MsgKind::Text && !m.outgoing) mids.push_back(m.mid);
        if (mids.size() >= 2) {
            eng.React(selectedPeer, mids[0], "\xE2\x9D\xA4\xEF\xB8\x8F");
            eng.React(selectedPeer, mids[1], "\xF0\x9F\x98\x82");
            reactMid = mids[0];
            if (std::string(autoReact) == "pick") openReactPick = true;
            if (std::string(autoReact) == "more") openReactMore = true;
            autoReact = nullptr;
        }
    }
    static const char* autoRec = getenv("LUNASHARE_AUTOREC");  // dev: start a voice ("1") or video ("video") message
    static bool autoRecDone = false;
    if (autoRec && !autoRecDone && !selectedPeer.empty() && Media().CanRecordMessages()) {
        recPeer = selectedPeer;
        if (std::string(autoRec) == "video") {
            recPath = JoinPath(ConfigDir(), "autorec.mp4");
            vmsgCams = (int)Media().Cameras().size();
            bool o = Media().OpenVideoMessage(recPath);
            bool r = o && getenv("LUNASHARE_AUTOREC_START") && Media().RecordVideoMessage();
            SDL_Log("autorec video: open %d record %d at %u ms", o, r, SDL_GetTicks());
        } else {
            recPath = JoinPath(ConfigDir(), "autorec.ogg");
            Media().StartRecording(recPath);
        }
        autoRecDone = true;
    }

    WatchSync();
    busy = false;
    int unread = 0;
    for (auto& [id, t] : eng.transfers)
        if (t->state == TState::Active || t->state == TState::Hashing || t->state == TState::Verifying) busy = true;
    for (auto& [peer, cv] : eng.chats) unread += cv.unread;
    if (Media().Recording() || Media().VideoMessageOpen() || Player().Playing() || eng.watch.sid) busy = true;  // keep the timer / seek bar moving
    windowTitle = unread ? "LunaShare (" + std::to_string(unread) + ")" : "LunaShare";

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos = vp->WorkPos, size = vp->WorkSize;
    size.y -= keyboardInset;
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    DrawCallBar();
    if (m.mobile) DrawMobile();
    else DrawDesktop();
    DrawPopups();
    DrawViewer();
    ImGui::End();
    if (eng.call.state != CallState::Active) {
        videoFull = videoAutoOpened = false;
        camPanel = false;
    } else if (m.mobile && VideoVisible() && !videoAutoOpened) {
        videoFull = videoAutoOpened = true;  // a video call opens full screen on phones
    }
    static bool devAvatar = getenv("LUNASHARE_AVATAR") != nullptr;  // dev hook: crop dialog for this picture
    if (devAvatar && ImGui::GetFrameCount() > 5) {
        devAvatar = false;
        avatarSrc = getenv("LUNASHARE_AVATAR");
        avatarZoom = 1.4f;
        avatarCx = avatarCy = 0.5f;
        openAvatarEditor = true;
    }
    static bool devPanel = getenv("LUNASHARE_CAMPANEL") != nullptr;  // dev hook for screenshots
    if (devPanel && eng.call.state == CallState::Active && (Media().CameraOn() || !strcmp(getenv("LUNASHARE_CAMPANEL"), "audio"))) { camPanel = true; devPanel = false; }
    if (m.mobile && videoFull && VideoVisible()) DrawVideoOverlay();
    if (watchShown) DrawWatch();
    if (camPanel) DrawCameraPanel();  // call settings: sound always, camera while it is on
    DrawToasts();
    if (Media().MicTesting() && ImGui::GetFrameCount() - settingsShownFrame > 2) Media().StopMicTest();
    // Touch scrolling everywhere (phone layout or a touch screen on desktop); the picture viewer handles its own drags.
    ui::TouchScrollUpdate((m.mobile || ImGui::GetIO().MouseSource == ImGuiMouseSource_TouchScreen) && !viewer.open);
    Images().Update();

    // Ctrl+C copies selected chat messages (when no text field has focus)
    if (!selMsgs.empty() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) CopySelectedMessages(selectedPeer);
    if (!selMsgs.empty() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))
        AskDelete(selectedPeer, std::vector<std::string>(selMsgs.begin(), selMsgs.end()));
    if (!selMsgs.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        selMsgs.clear();
        selectionMode = false;
    }
}

void App::PullEvents() {
    auto lk = eng.Lock();
    for (auto& e : eng.PopEvents()) {
        Toast t;
        t.type = e.type;
        t.peer = e.peer;
        t.tid = e.tid;
        std::string who = e.peer.empty() ? "" : eng.PeerName(e.peer);
        switch (e.type) {
            case EventType::ChatReceived:
                if (e.peer == selectedPeer && (tab == Tab::Chat || screen == MScreen::Chat)) continue;
                t.title = who;
                t.text = e.text.size() > 120 ? e.text.substr(0, 117) + "..." : e.text;
                wantAttention = true;
                break;
            case EventType::ChatDeleted:
                continue;  // the chat view simply redraws without it
            case EventType::ChatReaction:
                if (e.peer == selectedPeer && (tab == Tab::Chat || screen == MScreen::Chat)) continue;
                t.title = TF("%s reacted %s", who.c_str(), e.text.c_str());
                t.text = e.a;
                break;
            case EventType::IncomingOffer:
                t.title = TF("%s wants to send", who.c_str());
                t.text = e.text;
                t.ttl = 20.0f;
                wantAttention = true;
                break;
            case EventType::TransferDone:
                if (eng.ChatKey(e.peer) == selectedPeer && (tab == Tab::Chat || screen == MScreen::Chat) && !ui::M().mobile) continue;
                t.title = T("Transfer complete (verified)");
                t.text = e.text + (who.empty() ? "" : "  -  " + who);
                break;
            case EventType::TransferFailed:
                t.title = T("Transfer problem");
                t.text = e.text;
                t.ttl = 10.0f;
                break;
            case EventType::PeerOnline:
                continue;  // visible in the device list
            case EventType::WifiCreds:
                wifiOffers.push_back({e.peer, e.a, e.b});
                openWifiOffer = true;
                wantAttention = true;
                continue;
            case EventType::Info:
                t.title = T("Info");
                t.text = e.text;
                break;
            case EventType::Error:
                t.title = T("Error");
                t.text = e.text;
                t.ttl = 8.0f;
                break;
            case EventType::LinkRequest:
                linkAskPeer = e.peer;
                linkAskCode = e.text;
                linkAskName = e.a;
                openLinkAsk = true;
                wantAttention = true;
                continue;
            case EventType::LinkDone:
                linkTarget.clear();
                infoMessage = TDynamic(e.text);
                openInfo = true;
                continue;
            case EventType::CallIncoming:
                openIncomingCall = true;
                wantAttention = true;
                continue;
            case EventType::CallStarted:
                continue;
            case EventType::CallEnded:
                if (e.a == "hangup" || e.a == "declined") continue;  // the user did it or sees it in the chat
                t.title = who;
                t.text = e.text;
                t.ttl = 8.0f;
                if (e.text.rfind("Missed", 0) == 0) wantAttention = true;
                break;
        }
        // merge consecutive chat messages from the same device into one toast
        bool merged = false;
        if (t.type == EventType::ChatReceived)
            for (auto& o : toasts)
                if (o.type == EventType::ChatReceived && o.peer == t.peer) {
                    o.text = t.text;
                    o.ttl = 6.0f;
                    merged = true;
                }
        if (!merged) toasts.push_back(t);
        size_t maxToasts = ui::M().mobile ? 2 : 5;
        while (toasts.size() > maxToasts) toasts.erase(toasts.begin());
    }
}

// people: one entry per person (their devices share a conversation); otherwise every device (Devices tab)
std::vector<std::string> App::SortedPeers(bool people) {
    std::set<std::string> ids;
    if (people) {
        for (auto& k : eng.Conversations()) ids.insert(k);
    } else {
        for (auto& [id, d] : eng.devices) ids.insert(id);
        for (auto& [id, p] : eng.online) ids.insert(id);
    }
    for (auto& c : eng.conns)
        if (c->ready && !c->closed) ids.insert(people ? eng.ChatKey(c->peerId) : c->peerId);
    ids.erase(eng.settings.deviceId);
    std::vector<std::string> v(ids.begin(), ids.end());
    auto rank = [&](const std::string& id) {
        const KnownDevice* d = eng.DeviceFor(id);
        if (d && d->blocked) return 3;
        if (eng.IsConnected(id)) return 0;
        if (eng.IsOnline(id)) return 1;
        return 2;
    };
    auto lastSeen = [&](const std::string& id) {
        const KnownDevice* d = eng.DeviceFor(id);
        return d ? d->lastSeen : 0;
    };
    std::sort(v.begin(), v.end(), [&](const std::string& a, const std::string& b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        if (lastSeen(a) != lastSeen(b)) return lastSeen(a) > lastSeen(b);
        return eng.PeerName(a) < eng.PeerName(b);
    });
    return v;
}

void App::SelectPeer(const std::string& peer) {
    if (selectedPeer != peer && selectedPeer != eng.ChatKey(peer)) {
        selMsgs.clear();
        selectionMode = false;
        lastSelIndex = -1;
    }
    selectedPeer = eng.IsContactKey(peer) || Engine::IsGroupKey(peer) ? peer : eng.ChatKey(peer);  // a device's messages are in its person's chat
    notify::ClearChat(selectedPeer);
    eng.MarkRead(selectedPeer);
    scrollToBottom = true;
    if (!eng.IsConnected(selectedPeer)) eng.ConnectPeer(selectedPeer);
}

void App::OnDrop(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    auto lk = eng.Lock();
    bool chatVisible = !selectedPeer.empty() && !Engine::IsGroupKey(selectedPeer) &&
                       ((!ui::M().mobile && tab == Tab::Chat) || (ui::M().mobile && screen == MScreen::Chat));
    if (chatVisible) {
        eng.SendPaths(selectedPeer, paths);
        scrollToBottom = true;
    } else {
        OpenSendTo(paths);
    }
}

void App::OpenSendTo(const std::vector<std::string>& paths) {
    pendingSend = paths;
    openSendTo = true;
}

bool App::RequestQuit() {
    auto lk = eng.Lock();
    if (quitConfirmed) return true;
    bool active = false;
    for (auto& [id, t] : eng.transfers)
        if (t->state == TState::Active || t->state == TState::Hashing || t->state == TState::Verifying) active = true;
    if (!active) return true;
    openQuit = true;
    return false;
}

// ============================================================================ profile pictures

void App::DrawAvatar(ImDrawList* dl, ImVec2 c, float d, const std::string& peer, const std::string& name) {
    std::string path = eng.AvatarPath(peer);
    if (!path.empty()) {
        const ImageTex& t = Images().Get(path, ImageCache::Size::Thumb);
        if (t.state == ImageTex::Ready) {
            dl->AddImageRounded(t.Id(), ImVec2(c.x - d * 0.5f, c.y - d * 0.5f), ImVec2(c.x + d * 0.5f, c.y + d * 0.5f), ImVec2(0, 0),
                                ImVec2(1, 1), IM_COL32_WHITE, d * 0.5f);
            return;
        }
    }
    std::string key = peer.empty() ? eng.settings.deviceId : peer;
    ImU32 col = (ImGui::GetColorU32(ImHashStr(key.c_str()) | 0xFF000000) & 0xFF9F9F9F) | 0xFF404040;
    dl->AddCircleFilled(c, d * 0.5f, col);
    if (Engine::IsGroupKey(peer)) {  // a group: people, not an initial
        ui::DrawIcon(dl, Icon::Users, c, d * 0.55f, IM_COL32_WHITE);
        return;
    }
    char initial[8] = {};
    int len = 1;  // first UTF-8 character of the name
    unsigned char c0 = name.empty() ? '?' : (unsigned char)name[0];
    if (c0 >= 0xF0) len = 4; else if (c0 >= 0xE0) len = 3; else if (c0 >= 0xC0) len = 2;
    memcpy(initial, name.empty() ? "?" : name.c_str(), std::min<size_t>((size_t)len, name.empty() ? 1 : name.size()));
    if (len == 1) initial[0] = (char)toupper((unsigned char)initial[0]);
    float fs = std::min(ImGui::GetFontSize() * std::max(1.0f, d / (ImGui::GetFontSize() * 2.2f)), d * 0.6f);
    ImVec2 its = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, initial);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(c.x - its.x * 0.5f, c.y - its.y * 0.5f), IM_COL32_WHITE, initial);
}

// Settings > Profile: the picture with "Choose" / "Remove"
void App::DrawProfilePicture(float h) {
    float d = ImGui::GetFontSize() * 4.5f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##mypic", ImVec2(d, d));
    bool click = ImGui::IsItemClicked();
    DrawAvatar(ImGui::GetWindowDrawList(), ImVec2(p.x + d * 0.5f, p.y + d * 0.5f), d, "", eng.settings.displayName);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, std::max(0.0f, (d - (h > 0 ? h : ImGui::GetFrameHeight()) * 2 - ImGui::GetStyle().ItemSpacing.y) * 0.5f)));
    if (ImGui::Button(T("Choose picture..."), ImVec2(0, h)) || click) {
        browseFor = BrowseFor::Avatar;
        browser.Open(FileBrowser::Mode::OpenFile, T("Profile picture"), browser.lastDir);
    }
    bool has = !eng.settings.avatarHash.empty();
    ImGui::BeginDisabled(!has);
    if (ImGui::Button(T("Remove picture"), ImVec2(0, h))) eng.SetAvatar("");
    ImGui::EndDisabled();
    ImGui::EndGroup();
}

// Crop dialog: drag to move, zoom with the slider / mouse wheel; the circle is what others will see.
void App::DrawAvatarEditor() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    if (openAvatarEditor) {
        ImGui::OpenPopup(T("Profile picture##crop"));
        openAvatarEditor = false;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(T("Profile picture##crop"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    float box = std::min(mobile ? vp->WorkSize.x * 0.85f : ImGui::GetFontSize() * 20, vp->WorkSize.y * 0.55f);
    const ImageTex& t = Images().Get(avatarSrc, true);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##crop", ImVec2(box, box));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + box, p0.y + box), IM_COL32(20, 20, 20, 255));
    if (t.state == ImageTex::Ready && t.w > 0 && t.h > 0) {
        // the visible square (side = shorter image side / zoom) fills the box
        float side = std::min(t.w, t.h) / avatarZoom;
        float u = side / t.w, v = side / t.h;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0)) {
            ImVec2 dd = ImGui::GetIO().MouseDelta;
            avatarCx -= dd.x / box * u;
            avatarCy -= dd.y / box * v;
        }
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0)
            avatarZoom = std::clamp(avatarZoom * (ImGui::GetIO().MouseWheel > 0 ? 1.1f : 1 / 1.1f), 1.0f, 5.0f);
        side = std::min(t.w, t.h) / avatarZoom;
        u = side / t.w;
        v = side / t.h;
        avatarCx = std::clamp(avatarCx, u * 0.5f, 1 - u * 0.5f);
        avatarCy = std::clamp(avatarCy, v * 0.5f, 1 - v * 0.5f);
        dl->AddImage(t.Id(), p0, ImVec2(p0.x + box, p0.y + box), ImVec2(avatarCx - u * 0.5f, avatarCy - v * 0.5f),
                     ImVec2(avatarCx + u * 0.5f, avatarCy + v * 0.5f));
        // darken outside the circle: a thick ring from the circle to past the corners, clipped to the box
        ImVec2 c(p0.x + box * 0.5f, p0.y + box * 0.5f);
        float r = box * 0.5f;
        for (int q = 0; q < 4; q++) {  // each corner: from the box corner along two edges, back along a quarter arc
            float a0 = q * 1.5707963f;
            ImVec2 pts[40];
            int n = 0;
            ImVec2 dirA(std::cos(a0), std::sin(a0)), dirB(std::cos(a0 + 1.5707963f), std::sin(a0 + 1.5707963f));
            pts[n++] = ImVec2(c.x + dirA.x * r, c.y + dirA.y * r);
            pts[n++] = ImVec2(c.x + (dirA.x + dirB.x) * r, c.y + (dirA.y + dirB.y) * r);
            pts[n++] = ImVec2(c.x + dirB.x * r, c.y + dirB.y * r);
            for (int i = 1; i < 32; i++) {
                float a = a0 + 1.5707963f * (1.0f - i / 32.0f);
                pts[n++] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
            }
            dl->AddConcavePolyFilled(pts, n, IM_COL32(0, 0, 0, 150));
        }
        dl->AddCircle(c, r, IM_COL32(255, 255, 255, 200), 128, 2.0f);
    } else if (t.state == ImageTex::Failed) {
        ImVec2 ts = ImGui::CalcTextSize(T("This picture can't be opened."));
        dl->AddText(ImVec2(p0.x + (box - ts.x) * 0.5f, p0.y + box * 0.5f), ui::ColErr(), T("This picture can't be opened."));
    }
    ImGui::SetNextItemWidth(box);
    ImGui::SliderFloat("##zoom", &avatarZoom, 1.0f, 5.0f, T("Zoom %.1fx"));
    bool ok = t.state == ImageTex::Ready;
    ImGui::BeginDisabled(!ok);
    if (ImGui::Button(T("Use this picture"), ImVec2(mobile ? box : 0, h))) {
        std::string jpeg, err;
        if (MakeAvatarJpeg(avatarSrc, avatarCx, avatarCy, avatarZoom, jpeg, &err) && eng.SetAvatar(jpeg, &err)) {
            ImGui::CloseCurrentPopup();
        } else {
            infoMessage = T("The picture could not be used: ") + TS(err);
            openInfo = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    if (!mobile) ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(mobile ? box : 0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ============================================================================ peer rows

bool App::DevicePeerRow(const std::string& peer, bool selected, float height) {
    ImGui::PushID(peer.c_str());
    const KnownDevice* d = eng.DeviceFor(peer);  // (a person: their best device)
    std::string dev = eng.Resolve(peer);
    bool connected = eng.IsConnected(peer);
    bool online = eng.IsOnline(peer);
    bool blocked = d && (d->blocked || !d->pendingKey.empty());
    std::string name = Emoji().Display(eng.PeerName(peer));
    std::string sub;
    if (d) sub = d->platform;
    else if (online) sub = eng.online[dev].platform;
    if (eng.IsContactKey(peer)) {
        size_t n = eng.DevicesOf(peer).size();
        if (n > 1) sub = TF("%d devices", (int)n);
    }
    const Group* grp = eng.GroupOf(peer);
    auto conn = eng.ReadyConn(peer);
    if (grp) {
        sub = TF("%d people", (int)grp->members.size());
        int on = 0;
        for (auto& m : grp->members)
            if (m.acc != eng.MyAccount() && (eng.IsConnected("@" + m.acc) || eng.IsOnline("@" + m.acc))) on++;
        if (on) sub += "  " + TF("%d online", on);
        connected = false;
        online = on > 0;
    } else if (connected && conn) sub += std::string(sub.empty() ? "" : "  ") + TF("via %s", T(TransportName(conn->transport))) + (conn->addr == "relay" ? T(" (relay)") : "");
    else if (online) sub += "  " + TF("online (%s)", TS(eng.online[dev].Via()).c_str());
    else sub += sub.empty() ? T("offline") : std::string("  ") + T("offline");

    int unread = 0;
    auto cv = eng.chats.find(peer);
    if (cv != eng.chats.end()) unread = cv->second.unread;
    // aggregated live transfer progress
    int64_t tDone = 0, tSize = 0;
    for (auto& [id, t] : eng.transfers)
        if ((t->peer == peer || eng.ChatKey(t->peer) == peer) && t->state == TState::Active) { tDone += t->done.load(); tSize += t->size; }

    float lh = ImGui::GetTextLineHeightWithSpacing();
    if (height <= 0) height = lh * 2.2f;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, height));
    bool rightClick = ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                      (ui::M().mobile && ImGui::IsItemActive() && ImGui::GetIO().MouseDownDuration[0] > 0.6f && ImGui::GetMouseDragDelta(0).y == 0);
    ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = ImGui::GetFontSize() * 0.32f;
    ImU32 dot = blocked ? ui::ColErr() : connected ? ui::ColOk() : online ? ui::ColWarn() : ui::ColMuted();
    float pad = ImGui::GetStyle().FramePadding.x;
    float cy = p0.y + height * 0.5f;
    // profile picture, or a circle with the initial
    float av = std::min(height * 0.8f, ImGui::GetFontSize() * 2.2f);
    ImVec2 ac(p0.x + pad + av * 0.5f, cy);
    DrawAvatar(dl, ac, av, peer, name);
    dl->AddCircleFilled(ImVec2(ac.x + av * 0.35f, ac.y + av * 0.35f), r, dot);
    float tx = p0.x + pad * 2 + av;
    float textH = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(tx, cy - textH - 1), ImGui::GetColorU32(blocked ? ImGuiCol_TextDisabled : ImGuiCol_Text), name.c_str());
    dl->AddText(ImVec2(tx, cy + 1), ui::ColMuted(), sub.c_str());
    if (tSize > 0) {
        float w = (p1.x - tx - pad) * (float)((double)tDone / (double)tSize);
        dl->AddRectFilled(ImVec2(tx, p1.y - 3), ImVec2(tx + w, p1.y - 1), ui::ColAccent());
    }
    if (unread > 0) {
        std::string u = std::to_string(unread);
        ImVec2 us = ImGui::CalcTextSize(u.c_str());
        float bw = std::max(us.x + 10, textH);
        ImVec2 b0(p1.x - pad - bw, cy - textH * 0.5f);
        dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + textH), ui::ColErr(), textH * 0.5f);
        dl->AddText(ImVec2(b0.x + (bw - us.x) * 0.5f, b0.y), IM_COL32_WHITE, u.c_str());
    }
    if (rightClick) ImGui::OpenPopup(grp ? "groupctx" : "peerctx");
    if (ImGui::BeginPopup("groupctx")) {
        float h = ui::M().mobile ? ui::M().rowH * 0.8f : 0;
        if (ImGui::Selectable(T("People in this group"), false, 0, ImVec2(0, h))) OpenGroupMembers(peer);
        if (grp && !grp->left && ImGui::Selectable(T("Add people"), false, 0, ImVec2(0, h))) OpenAddToGroup(peer);
        if (grp && !grp->left && ImGui::Selectable(T("Leave the group"), false, 0, ImVec2(0, h))) eng.LeaveGroup(peer);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("peerctx")) {
        float h = ui::M().mobile ? ui::M().rowH * 0.8f : 0;
        if (ImGui::Selectable(connected ? T("Disconnect") : T("Connect"), false, 0, ImVec2(0, h))) connected ? eng.Disconnect(peer) : eng.ConnectPeer(peer);
        if (ImGui::Selectable(T("Rename..."), false, 0, ImVec2(0, h))) {
            renamePeer = peer;
            renameBuf = d ? d->customName : "";
            openRename = true;
        }
        bool trusted = d && d->trusted;
        if (ImGui::Selectable(trusted ? T("Untrust") : T("Trust (auto-accept files)"), false, 0, ImVec2(0, h))) eng.SetTrusted(peer, !trusted);
        if (ImGui::Selectable(T("Send files..."), false, 0, ImVec2(0, h))) {
            SelectPeer(peer);
            browseFor = BrowseFor::SendFiles;
            browser.Open(FileBrowser::Mode::OpenMulti, TF("Send to %s", name.c_str()), browser.lastDir);
        }
        if (ImGui::Selectable(T("Device details"), false, 0, ImVec2(0, h))) {
            devicesSel = dev;
            if (ui::M().mobile) { backStack.push_back(screen); screen = MScreen::Devices; }
            else { tab = Tab::Devices; tabForce = true; }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return clicked;
}

// ============================================================================ desktop

void App::DrawDesktop() {
    float sideW = ImGui::GetFontSize() * 17;
    ImGui::BeginChild("sidebar", ImVec2(sideW, 0), ImGuiChildFlags_Borders);
    DrawSidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_None);
    if (ImGui::BeginTabBar("tabs")) {
        auto item = [&](const char* label, Tab t) {
            ImGuiTabItemFlags f = (tabForce && tab == t) ? ImGuiTabItemFlags_SetSelected : 0;
            bool open = ImGui::BeginTabItem(label, nullptr, f);
            if (open) {
                if (!tabForce) tab = t;
                ImGui::EndTabItem();
            }
        };
        int active = 0;
        for (auto& [id, t] : eng.transfers)
            if (!TStateFinal(t->state)) active++;
        std::string tl = active ? TF("Transfers (%d)", active) + "###transfers" : std::string(T("Transfers")) + "###transfers";
        item(T("Chat"), Tab::Chat);
        item(tl.c_str(), Tab::Transfers);
        item(T("History"), Tab::History);
        item(T("Library"), Tab::Library);
        item(T("Devices"), Tab::Devices);
        item(T("Connect"), Tab::Nearby);
        item(T("Settings"), Tab::Settings);
        tabForce = false;
        ImGui::EndTabBar();
    }
    switch (tab) {
        case Tab::Chat:
            if (selectedPeer.empty()) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                const char* t1 = T("Select a device on the left to chat and share files.");
                const char* t2 = T("Drop files anywhere on this window to send them.");
                ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - ImGui::CalcTextSize(t1).x) * 0.5f, ImGui::GetCursorPosY() + avail.y * 0.4f));
                ImGui::TextDisabled("%s", t1);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - ImGui::CalcTextSize(t2).x) * 0.5f);
                ImGui::TextDisabled("%s", t2);
            } else {
                DrawChat(selectedPeer);
            }
            break;
        case Tab::Transfers: DrawTransfers(); break;
        case Tab::History: DrawHistory(); break;
        case Tab::Library: DrawLibrary(); break;
        case Tab::Devices: DrawDevices(); break;
        case Tab::Nearby: DrawNearby(); break;
        case Tab::Settings: DrawSettings(); break;
    }
    ImGui::EndChild();
}

void App::DrawSidebar() {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted(T("LunaShare"));
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    bool openInvites = ImGui::Selectable((eng.settings.displayName + "  " + T("- invites...") + "##me").c_str(), false, 0,
                                         ImVec2(0, ui::M().mobile ? ui::M().rowH * 0.8f : 0));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered() && !ui::M().mobile) {
        ImGui::SetTooltip(T("Invite someone, or use an invite code you got\n\nThis device\nID: %s\nKey fingerprint: %s\nPort: %u"),
                          eng.settings.deviceId.c_str(), KeyFingerprint(eng.MyPublicKey()).c_str(), eng.boundPort);
    }
    if (openInvites) ImGui::OpenPopup("##invites");
    DrawInvitesPopup();
    ImGui::Separator();
    auto peers = SortedPeers();
    float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("peers", ImVec2(0, -footer));
    if (peers.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("Looking for devices on your network...\n\nStart LunaShare on another device in the same Wi-Fi/LAN (or phone hotspot). It appears here automatically."));
        ImGui::PopTextWrapPos();
    }
    for (auto& p : peers) {
        if (DevicePeerRow(p, p == selectedPeer && tab == Tab::Chat, 0)) {
            SelectPeer(p);
            tab = Tab::Chat;
            tabForce = true;
        }
    }
    ImGui::EndChild();
    ImGui::Separator();
    {
        float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ui::IconTextButton(T("Connect..."), Icon::Link, ImVec2(half, 0))) {
            tab = Tab::Nearby;
            tabForce = true;
        }
        ImGui::SameLine();
        if (ui::IconTextButton(T("New group"), Icon::Users, ImVec2(-1, 0))) OpenNewGroup();
    }
    double up = 0, down = 0;
    for (auto& [id, t] : eng.transfers)
        if (t->state == TState::Active) (t->outgoing ? up : down) += t->speed;
    ImGui::TextDisabled(T("Up %s/s   Down %s/s"), FormatBytes(up).c_str(), FormatBytes(down).c_str());
}

// ============================================================================ mobile

void App::DrawMobile() {
    float rowH = ui::M().rowH;
    // ---- top bar
    bool root = screen == MScreen::Home || screen == MScreen::Transfers || screen == MScreen::History ||
                screen == MScreen::Nearby || screen == MScreen::Settings;
    if (!root || !backStack.empty()) {
        if (ui::IconButton("back", Icon::Back, nullptr, rowH) || ImGui::IsKeyPressed(ImGuiKey_AppBack)) {
            if (!selMsgs.empty()) { selMsgs.clear(); selectionMode = false; }
            else if (!backStack.empty()) { screen = backStack.back(); backStack.pop_back(); }
            else screen = MScreen::Home;
        }
        ImGui::SameLine();
    } else if (ImGui::IsKeyPressed(ImGuiKey_AppBack) && screen != MScreen::Home) {
        screen = MScreen::Home;
    } else if (ImGui::IsKeyPressed(ImGuiKey_AppBack) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
               Bridge().moveToBack) {
        Bridge().moveToBack();  // like other messengers: back on the home screen keeps LunaShare running in the background
    }
    const char* title = "LunaShare";
    std::string peerTitle;
    switch (screen) {
        case MScreen::Home: title = "LunaShare"; break;
        case MScreen::Chat: peerTitle = Emoji().Display(eng.PeerName(selectedPeer)); title = peerTitle.c_str(); break;
        case MScreen::Transfers: title = T("Transfers"); break;
        case MScreen::History: title = T("Shared files"); break;
        case MScreen::Library: title = T("Library"); break;
        case MScreen::Nearby: title = T("Connect"); break;
        case MScreen::Settings: title = T("Settings"); break;
        case MScreen::Devices: title = T("Device"); break;
    }
    // chat: buttons on the right; the title gets what's left (shortened with "..."), buttons shrink on tiny screens
    bool chatScreen = screen == MScreen::Chat;
    bool groupChat = chatScreen && Engine::IsGroupKey(selectedPeer);
    bool callable = chatScreen && Media().Available();
    bool videoable = callable && Media().VideoAvailable();
    bool webcam = chatScreen && !groupChat && WebcamButtonShown(selectedPeer);
    bool watchable = chatScreen && Player().Available();
    int nb = chatScreen ? 1 + (callable ? 1 : 0) + (videoable ? 1 : 0) + (webcam ? 1 : 0) + (watchable ? 1 : 0) : 0;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    float rowEnd = ImGui::GetContentRegionMax().x;
    float titleX = ImGui::GetCursorPosX();
    float hb = nb ? std::min(rowH, ((rowEnd - titleX) * 0.7f - sp * (float)(nb - 1)) / (float)nb) : 0;
    float buttonsW = nb ? hb * (float)nb + sp * (float)nb : 0;
    float rowY = ImGui::GetCursorPosY();
    float adW = rowH * 0.72f;
    if (chatScreen && rowEnd - titleX - buttonsW - adW - sp > ImGui::GetFontSize() * 6) {  // their picture, if the name still fits
        float ad = adW;
        ImVec2 sp0 = ImGui::GetCursorScreenPos();
        DrawAvatar(ImGui::GetWindowDrawList(), ImVec2(sp0.x + ad * 0.5f, sp0.y + rowH * 0.5f), ad, selectedPeer, peerTitle);
        titleX += ad + sp;
        ImGui::SetCursorPosX(titleX);
    }
    ImGui::SetCursorPosY(rowY + (rowH - ImGui::GetFontSize() * 1.3f) * 0.5f);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    {
        std::string t = title;
        float maxW = rowEnd - titleX - buttonsW;
        if (ImGui::CalcTextSize(t.c_str()).x > maxW) {
            while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > maxW) {
                t.pop_back();
                while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();  // whole UTF-8 characters
                if (!t.empty() && (unsigned char)t.back() >= 0xC0) t.pop_back();
            }
            t += "...";
        }
        ImGui::TextUnformatted(t.c_str());
    }
    ImGui::PopFont();
    if (chatScreen) {
        ImGui::SameLine(rowEnd - hb * (float)nb - sp * (float)(nb - 1));
        ImGui::SetCursorPosY(rowY + (rowH - hb) * 0.5f);
        if (webcam) {
            WebcamButton(selectedPeer, hb);
            ImGui::SameLine();
        }
        if (callable) {
            CallButton(selectedPeer, hb);
            ImGui::SameLine();
        }
        if (videoable) {
            CallButton(selectedPeer, hb, true);
            ImGui::SameLine();
        }
        if (watchable) {
            if (ui::IconButton("watch", Icon::Play, nullptr, hb)) OpenWatchPicker(selectedPeer);
            ImGui::SameLine();
        }
        if (groupChat) GroupButtons(selectedPeer, hb);
        else if (ui::IconButton("attach", Icon::Attach, nullptr, hb)) {
            browseFor = BrowseFor::SendFiles;
            browser.Open(FileBrowser::Mode::OpenMulti, TF("Send to %s", peerTitle.c_str()), browser.lastDir);
        }
    }
    ImGui::Separator();

    float navH = (screen == MScreen::Chat || keyboardInset > 0) ? 0 : rowH * 1.25f + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("mcontent", ImVec2(0, -navH));
    switch (screen) {
        case MScreen::Home: DrawMobileHome(); break;
        case MScreen::Chat: DrawChat(selectedPeer); break;
        case MScreen::Transfers: DrawTransfers(); break;
        case MScreen::History: DrawHistory(); break;
        case MScreen::Library: DrawLibrary(); break;
        case MScreen::Nearby: DrawNearby(); break;
        case MScreen::Settings: DrawSettings(); break;
        case MScreen::Devices:
            if (devicesSel.empty()) DrawDevices();
            else DrawDeviceDetails(devicesSel);
            break;
    }
    ImGui::EndChild();
    if (navH > 0) DrawMobileNav();
}

void App::DrawMobileHome() {
    ui::TouchScroll();
    auto peers = SortedPeers();
    if (peers.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("Looking for devices...\n\nOpen LunaShare on another phone or computer connected to the same Wi-Fi. "
                            "No Wi-Fi? Use Connect > Hotspot or Bluetooth."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ui::BigButton(T("Connect / Hotspot / Bluetooth"), Icon::Link)) screen = MScreen::Nearby;
        if (ui::BigButton(T("Invite someone / use an invite code"), Icon::Users)) ImGui::OpenPopup("##invites");
    }
    for (auto& p : peers) {
        if (DevicePeerRow(p, false, ui::M().rowH * 1.35f)) {
            SelectPeer(p);
            backStack.clear();
            backStack.push_back(MScreen::Home);
            screen = MScreen::Chat;
        }
    }
    if (!peers.empty()) {
        ImGui::Spacing();
        if (ui::BigButton(T("New group"), Icon::Users)) OpenNewGroup();
        if (ui::BigButton(T("Invite someone / use an invite code"), Icon::Link)) ImGui::OpenPopup("##invites");
    }
    DrawInvitesPopup();
}

void App::DrawMobileNav() {
    ImGui::Separator();
    struct Item { const char* label; Icon icon; MScreen s; };
    Item items[] = {{T("Devices"), Icon::Users, MScreen::Home}, {T("Transfers"), Icon::Transfers, MScreen::Transfers},
                    {T("Files"), Icon::History, MScreen::History}, {T("Library"), Icon::Library, MScreen::Library},
                    {T("Connect"), Icon::Link, MScreen::Nearby}, {T("Settings"), Icon::Gear, MScreen::Settings}};
    const int kItems = 6;
    float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (kItems - 1)) / (float)kItems;
    float h = ui::M().rowH * 1.25f;
    for (int i = 0; i < kItems; i++) {
        if (i) ImGui::SameLine();
        bool active = screen == items[i].s;
        ImGui::PushID(i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("##nav", ImVec2(w, h))) {
            screen = items[i].s;
            backStack.clear();
        }
        if (active) ImGui::PopStyleColor();
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cx = (mn.x + mx.x) * 0.5f;
        ui::DrawIcon(dl, items[i].icon, ImVec2(cx, mn.y + h * 0.36f), h * 0.36f, ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
        ImVec2 ts = ImGui::CalcTextSize(items[i].label);
        float fs = ImGui::GetFontSize();
        if (ts.x > w * 0.96f) {  // (six items on a narrow phone: smaller labels)
            fs *= w * 0.96f / ts.x;
            ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, items[i].label);
        }
        dl->AddText(ImGui::GetFont(), fs, ImVec2(cx - ts.x * 0.5f, mx.y - ts.y - h * 0.08f), ImGui::GetColorU32(ImGuiCol_Text), items[i].label);
        ImGui::PopFont();
        if (items[i].s == MScreen::Transfers) {
            int active = 0;
            for (auto& [id, t] : eng.transfers)
                if (!TStateFinal(t->state)) active++;
            if (active) dl->AddCircleFilled(ImVec2(cx + h * 0.28f, mn.y + h * 0.2f), h * 0.08f, ui::ColAccent());
        }
        ImGui::PopID();
    }
}

// ============================================================================ chat

void App::DrawChat(const std::string& peer) {
    bool mobile = ui::M().mobile;
    bool connected = eng.IsConnected(peer);
    const KnownDevice* d = eng.DeviceFor(peer);
    if (!mobile) {
        float ad = ImGui::GetFontSize() * 1.7f;
        ImVec2 ap = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(ad, ad * 0.75f));
        DrawAvatar(ImGui::GetWindowDrawList(), ImVec2(ap.x + ad * 0.5f, ap.y + ad * 0.4f), ad, peer, eng.PeerName(peer));
        ImGui::SameLine();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
        ImGui::TextUnformatted(Emoji().Display(eng.PeerName(peer)).c_str());
        ImGui::PopFont();
        ImGui::SameLine();
    }
    const Group* grp = eng.GroupOf(peer);
    auto conn = eng.ReadyConn(peer);
    if (grp) {
        int on = 0;
        std::string names;
        for (auto& m : grp->members) {
            if (m.acc == eng.MyAccount()) continue;
            if (eng.IsConnected("@" + m.acc) || eng.IsOnline("@" + m.acc)) on++;
            std::string n = eng.DevicesOf("@" + m.acc).empty() && !m.name.empty() ? m.name : eng.PeerName("@" + m.acc);
            names += (names.empty() ? "" : ", ") + n;
        }
        if (grp->left) ImGui::TextDisabled("%s", T("You are no longer in this group."));
        else if (on) ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.45f, 1), "%s", TF("%d online", on).c_str());
        else ImGui::TextDisabled("%s", T("nobody online - messages wait on your server"));
        if (ImGui::IsItemHovered() && !names.empty()) ImGui::SetTooltip("%s", names.c_str());
    } else if (connected) ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.45f, 1), T("connected via %s%s"), conn ? T(TransportName(conn->transport)) : "?",
                                      conn && conn->addr == "relay" ? T(" (relay through the server)") : "");
    else if (eng.IsOnline(peer)) ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1), "%s", T("online - connecting..."));
    else if (eng.WanStatus().rfind("connected", 0) == 0) ImGui::TextDisabled("%s", T("offline - messages wait on your server, files are queued"));
    else ImGui::TextDisabled("%s", T("offline - messages and files are queued"));
    if (grp) {
    } else if (d && !d->pendingKey.empty()) {
        ImGui::SameLine();
        ui::Badge(T("KEY CHANGED"), ui::ColErr());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("This device presented a different identity key. Review it in the device details."));
    } else if (connected) {
        ImGui::SameLine();
        bool verified = d && d->verified;
        ui::Badge(verified ? T("encrypted - verified") : T("encrypted"), verified ? IM_COL32(40, 150, 90, 255) : IM_COL32(70, 90, 140, 255));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(T("End-to-end encrypted (X25519 + XChaCha20-Poly1305).\nSafety number: %s\n%s"), eng.SafetyNumberWith(peer).c_str(),
                              verified ? T("You verified this device.") : T("Compare the safety number on both devices (Device details) to verify."));
    }
    if (d && d->trusted) {
        ImGui::SameLine();
        ui::Badge("trusted", IM_COL32(60, 120, 200, 255));
    }
    if (!mobile) {
        float bw = ImGui::GetFrameHeight();
        bool callable = Media().Available() && !(grp && grp->left);
        bool videoable = callable && Media().VideoAvailable();
        bool webcam = !grp && WebcamButtonShown(peer);
        bool watchable = Player().Available() && !(grp && grp->left);
        int nb = (grp ? 1 : 3) + (callable ? 1 : 0) + (videoable ? 1 : 0) + (webcam ? 1 : 0) + (watchable ? 1 : 0);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - bw * (float)nb - ImGui::GetStyle().ItemSpacing.x * (float)(nb - 1));
        if (webcam) {
            WebcamButton(peer, bw);
            ImGui::SameLine();
        }
        if (callable) {
            CallButton(peer, bw);
            ImGui::SameLine();
        }
        if (videoable) {
            CallButton(peer, bw, true);
            ImGui::SameLine();
        }
        if (watchable) {
            if (ui::IconButton("watch", Icon::Play, T("Watch a movie together"), bw)) OpenWatchPicker(peer);
            ImGui::SameLine();
        }
        if (grp) {
            GroupButtons(peer, bw);
        } else {
        if (ui::IconButton("sendfiles", Icon::Attach, T("Send files or folders"))) {
            browseFor = BrowseFor::SendFiles;
            browser.Open(FileBrowser::Mode::OpenMulti, TF("Send to %s", eng.PeerName(peer).c_str()), browser.lastDir);
        }
        ImGui::SameLine();
        if (connected ? ui::IconButton("disc", Icon::Cancel, T("Disconnect")) : ui::IconButton("conn", Icon::Link, T("Connect now")))
            connected ? eng.Disconnect(peer) : eng.ConnectPeer(peer);
        ImGui::SameLine();
        if (ui::IconButton("details", Icon::Gear, T("Device details / rename"))) {
            devicesSel = peer;
            tab = Tab::Devices;
            tabForce = true;
        }
        }
    }

    DrawWatchBanner(peer);
    // selection toolbar
    if (!selMsgs.empty()) {
        float h = mobile ? ui::M().rowH : 0;
        ImGui::AlignTextToFramePadding();
        ImGui::Text(T("%d selected"), (int)selMsgs.size());
        ImGui::SameLine();
        if (ImGui::Button(T("Copy"), ImVec2(0, h))) CopySelectedMessages(peer);
        ImGui::SameLine();
        if (ImGui::Button(T("Delete"), ImVec2(0, h))) AskDelete(peer, std::vector<std::string>(selMsgs.begin(), selMsgs.end()));
        ImGui::SameLine();
        if (ImGui::Button(T("Select all"), ImVec2(0, h))) {
            auto it = eng.chats.find(peer);
            if (it != eng.chats.end())
                for (auto& m : it->second.messages) selMsgs.insert(m.mid);
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Clear"), ImVec2(0, h))) {
            selMsgs.clear();
            selectionMode = false;
        }
    }
    ImGui::Separator();

    float inputH = mobile ? ui::M().rowH : ImGui::GetFrameHeight() * 2.2f;
    if (!mobile && peer == eng.call.peer && VideoVisible()) {
        float availH = ImGui::GetContentRegionAvail().y - inputH - ImGui::GetStyle().ItemSpacing.y * 3;
        float vh = videoBig ? availH - ImGui::GetFrameHeight() * 3 : std::max(ImGui::GetFontSize() * 10, availH * 0.5f);
        DrawVideoStage(ImVec2(ImGui::GetContentRegionAvail().x, vh), false);
    }
    if (Media().VideoMessageOpen() && recPeer == peer) {  // camera view instead of the messages
        DrawVideoMessage(peer, inputH);
        return;
    }
    if (!Emoji().Available()) emojiOpen = false;
    float pickerH = 0;
    if (emojiOpen) {
        float availY = ImGui::GetContentRegionAvail().y;
        pickerH = mobile ? availY * 0.5f : std::min(ImGui::GetFontSize() * 17, availY * 0.5f);
    }
    ImGui::BeginChild("messages", ImVec2(0, -(inputH + pickerH + ImGui::GetStyle().ItemSpacing.y * (emojiOpen ? 3 : 2))), ImGuiChildFlags_None);
    if (mobile) ui::TouchScroll();
    DrawMessages(peer);
    DrawReactionPopups(peer);
    ImGui::EndChild();
    ImGui::Separator();
    if (emojiOpen) {
        std::string picked;
        if (Emoji().DrawPicker(ImVec2(-1, pickerH), &picked)) {
            // insert where the cursor was (the box isn't active while the picker is used)
            size_t at = Emoji().InputCursor() >= 0 ? std::min((size_t)Emoji().InputCursor(), chatInput.size()) : chatInput.size();
            chatInput.insert(at, picked);
            Emoji().SetInputCursor((int)(at + picked.size()));  // the next one goes after it
        }
    }

    // input row
    float sendW = mobile ? ui::M().rowH * 1.4f : ImGui::GetFontSize() * 6;
    // ---- voice message being recorded: timer, level, cancel / send instead of the text box
    if (Media().Recording() && recPeer == peer) {
        double secs = Media().RecordingSeconds();
        float bw = inputH;
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 50, 50, 255));
        bool cancel = ui::IconButton("reccancel", Icon::Trash, T("Discard"), bw);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x - sendW - ImGui::GetStyle().ItemSpacing.x;
        ImGui::Dummy(ImVec2(w, inputH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float blink = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 5.0f);
        float r = ImGui::GetFontSize() * 0.35f;
        dl->AddCircleFilled(ImVec2(p.x + r * 2, p.y + inputH * 0.5f), r, IM_COL32(230, 60, 60, (int)(120 + 135 * blink)));
        char t[64];
        snprintf(t, sizeof(t), "%d:%02d", (int)secs / 60, (int)secs % 60);
        dl->AddText(ImVec2(p.x + r * 4, p.y + (inputH - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), t);
        float lx = p.x + r * 4 + ImGui::CalcTextSize("00:00").x + r * 2, lw = std::max(10.0f, p.x + w - lx - r);
        float lvl = Media().RecordLevel();
        dl->AddRectFilled(ImVec2(lx, p.y + inputH * 0.42f), ImVec2(lx + lw, p.y + inputH * 0.58f), ImGui::GetColorU32(ImGuiCol_FrameBg), 4);
        dl->AddRectFilled(ImVec2(lx, p.y + inputH * 0.42f), ImVec2(lx + lw * lvl, p.y + inputH * 0.58f), ui::ColOk(), 4);
        ImGui::SameLine();
        bool send = ui::IconTextButton(T("Send"), Icon::Send, ImVec2(sendW, inputH)) || secs > 600;  // 10 minutes max
        if (cancel) Media().StopRecording(false);
        if (send) {
            double len = Media().StopRecording(true);
            if (len >= 0.5) {
                eng.SendPaths(peer, {recPath});
                scrollToBottom = true;
            } else {
                RemoveFile(recPath);  // too short to be meant
            }
        }
        DrawDeleteConfirm(peer);
        return;
    }
    float emojiW = 0;
    if (Emoji().Available()) {  // emoji panel toggle, left of the text box
        ImVec2 p = ImGui::GetCursorScreenPos();
        float bw = mobile ? inputH * 0.9f : inputH * 0.8f;
        if (emojiOpen) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
        if (ImGui::Button("##emojibtn", ImVec2(bw, inputH))) emojiOpen = !emojiOpen;
        if (emojiOpen) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", T("Emoji"));
        std::string g = Emoji().Glyph("\xF0\x9F\x99\x82");
        float es = std::min(bw, inputH) * 0.55f;
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(es, FLT_MAX, 0, g.c_str());
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), es, ImVec2(p.x + (bw - ts.x) * 0.5f, p.y + (inputH - ts.y) * 0.5f), IM_COL32_WHITE, g.c_str());
        ImGui::SameLine();
        emojiW = bw + ImGui::GetStyle().ItemSpacing.x;
    }
    if (focusInput) {
        ImGui::SetKeyboardFocusHere();
        focusInput = false;
    }
    bool groupChat = Engine::IsGroupKey(peer);
    const Group* gInfo = eng.GroupOf(peer);
    if (gInfo && gInfo->left) {
        ImGui::TextDisabled("%s", T("You are no longer in this group."));
        DrawDeleteConfirm(peer);
        return;
    }
    bool showVmsg = !groupChat && chatInput.empty() && Media().CanRecordVideoMessages() && !Media().Recording();
    float vmsgW = showVmsg ? inputH + ImGui::GetStyle().ItemSpacing.x : 0;
    (void)emojiW;
    bool send = ImGui::InputTextMultiline("##input", &chatInput, ImVec2(ImGui::GetContentRegionAvail().x - sendW - vmsgW - ImGui::GetStyle().ItemSpacing.x, inputH),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_WordWrap |
                                              ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackAlways,
                                          EmojiHost::InputCallback);
    if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", T("Enter = send, Ctrl+Enter = new line, drop files to send"));
    ImGui::SameLine();
    if (showVmsg) {
        if (ui::IconButton("vmsg", Icon::Video, T("Record a video message"), inputH)) {
            std::string dir = JoinPath(ConfigDir(), "voice");
            MakeDirs(dir);
            time_t now = time(nullptr);
            char name[96];
            strftime(name, sizeof(name), "Video message %Y-%m-%d %H-%M-%S.mp4", localtime(&now));
            recPath = JoinPath(dir, name);
            recPeer = peer;
            vmsgCams = (int)Media().Cameras().size();
            if (!Media().OpenVideoMessage(recPath))
                toasts.push_back({EventType::Error, T("Camera"), T("Could not start the camera: ") + Media().audioError, "", 0, 4.0f});
        }
        ImGui::SameLine();
    }
    if (!groupChat && chatInput.empty() && Media().CanRecordMessages() && !Media().Recording()) {
        // nothing typed: the button records a voice message
        if (ui::IconTextButton(mobile ? T("Voice") : T("Voice"), Icon::Mic, ImVec2(sendW, inputH))) {
            std::string dir = JoinPath(ConfigDir(), "voice");
            MakeDirs(dir);
            time_t now = time(nullptr);
            char name[96];
            strftime(name, sizeof(name), "Voice message %Y-%m-%d %H-%M-%S.ogg", localtime(&now));
            recPath = JoinPath(dir, name);
            recPeer = peer;
            if (!Media().StartRecording(recPath))
                toasts.push_back({EventType::Error, T("Microphone"), T("Could not start recording (microphone permission?)"), "", 0, 4.0f});
        }
        if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", T("Record a voice message"));
    } else if (ui::IconTextButton(T("Send"), Icon::Send, ImVec2(sendW, inputH))) send = true;
    if (send) {
        std::string t = chatInput;
        while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
        if (!t.empty()) {
            eng.SendChat(peer, Emoji().Restore(t));
            scrollToBottom = true;
        }
        chatInput.clear();
        Emoji().SetInputCursor(-1);
        if (!mobile) focusInput = true;
    }
    DrawDeleteConfirm(peer);
}

// Video message: camera preview filling the chat, then recording with a timer. Discard / flip / record / send.
void App::DrawVideoMessage(const std::string& peer, float inputH) {
    bool mobile = ui::M().mobile;
    float sp = ImGui::GetStyle().ItemSpacing.y;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1(p0.x + avail.x, p0.y + avail.y - inputH - sp * 2);
    ImGui::Dummy(ImVec2(avail.x, p1.y - p0.y));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255), 6);
    const auto& t = Media().LocalVideo();
    if (t.valid) Media().DrawVideo(dl, t, p0, p1, false);
    else {
        const char* s = T("Starting camera...");
        ImVec2 ts = ImGui::CalcTextSize(s);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), IM_COL32(200, 200, 200, 255), s);
    }
    bool rec = Media().VideoMessageRecording();
    double secs = Media().RecordingSeconds();
    if (rec) {  // timer on the picture
        char b[32];
        snprintf(b, sizeof(b), "%d:%02d", (int)secs / 60, (int)secs % 60);
        float fs = ImGui::GetFontSize(), r = fs * 0.35f;
        ImVec2 tp(p0.x + fs, p0.y + fs * 0.8f), ts = ImGui::CalcTextSize(b);
        dl->AddRectFilled(ImVec2(tp.x - fs * 0.4f, tp.y - fs * 0.3f), ImVec2(tp.x + r * 3 + ts.x + fs * 0.4f, tp.y + ts.y + fs * 0.3f),
                          IM_COL32(0, 0, 0, 150), fs);
        float blink = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 5.0f);
        dl->AddCircleFilled(ImVec2(tp.x + r, tp.y + ts.y * 0.5f), r, IM_COL32(230, 60, 60, (int)(120 + 135 * blink)));
        dl->AddText(ImVec2(tp.x + r * 3, tp.y), IM_COL32(255, 255, 255, 255), b);
    }
    ImGui::Separator();
    float sendW = mobile ? ui::M().rowH * 1.4f : ImGui::GetFontSize() * 6;
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 50, 50, 255));
    bool cancel = ui::IconButton("vmcancel", Icon::Trash, T("Discard"), inputH);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    bool canFlip = !rec && vmsgCams > 1;
    if (canFlip) {
        if (ui::IconButton("vmflip", Icon::FlipCamera, T("Switch camera"), inputH)) Media().NextCamera();
        ImGui::SameLine();
    }
    // middle: hint or level meter
    ImVec2 m0 = ImGui::GetCursorScreenPos();
    float mw = ImGui::GetContentRegionAvail().x - sendW - ImGui::GetStyle().ItemSpacing.x;
    ImGui::Dummy(ImVec2(std::max(1.0f, mw), inputH));
    if (rec) {
        float lvl = Media().RecordLevel(), pad = ImGui::GetFontSize() * 0.5f;
        dl->AddRectFilled(ImVec2(m0.x + pad, m0.y + inputH * 0.42f), ImVec2(m0.x + mw - pad, m0.y + inputH * 0.58f), ImGui::GetColorU32(ImGuiCol_FrameBg), 4);
        dl->AddRectFilled(ImVec2(m0.x + pad, m0.y + inputH * 0.42f), ImVec2(m0.x + pad + (mw - 2 * pad) * lvl, m0.y + inputH * 0.58f), ui::ColOk(), 4);
    } else {
        Settings vs = eng.GetSettings();
        LsvSettings vp = MediaHost::Preset(vs.callVideoRes, vs.callVideoFps, false);
        std::string hintS = mobile ? std::string(T("Up to 3 minutes")) : TF("Video message - up to 3 minutes, %dp%d", vp.height, vp.fps);
        const char* hint = hintS.c_str();
        ImVec2 ts = ImGui::CalcTextSize(hint);
        if (ts.x < mw) dl->AddText(ImVec2(m0.x + (mw - ts.x) * 0.5f, m0.y + (inputH - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    }
    ImGui::SameLine();
    bool send = false;
    if (!rec) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(190, 60, 60, 255));
        if (ui::IconTextButton(T("Record"), Icon::Video, ImVec2(sendW, inputH)) && !Media().RecordVideoMessage())
            toasts.push_back({EventType::Error, T("Microphone"), T("Could not start recording (microphone permission?)"), "", 0, 4.0f});
        ImGui::PopStyleColor();
    } else {
        send = ui::IconTextButton(T("Send"), Icon::Send, ImVec2(sendW, inputH)) || secs > 180;
    }
    if (cancel || (!mobile && ImGui::IsKeyPressed(ImGuiKey_Escape))) Media().CloseVideoMessage(false);
    if (send) {
        double len = Media().CloseVideoMessage(true);
        if (len >= 0.5) {
            eng.SendPaths(peer, {recPath});
            scrollToBottom = true;
        } else {
            RemoveFile(recPath);
            toasts.push_back({EventType::Error, T("Video message"), T("Nothing was recorded"), "", 0, 4.0f});
        }
    }
}

void App::AskDelete(const std::string& peer, const std::vector<std::string>& mids) {
    deleteMids = mids;
    deleteEveryoneOk = !mids.empty();
    auto it = eng.chats.find(peer);
    if (it != eng.chats.end())
        for (auto& m : it->second.messages)
            if (std::find(mids.begin(), mids.end(), m.mid) != mids.end() && (!m.outgoing || m.kind == MsgKind::System)) deleteEveryoneOk = false;
    openDeleteConfirm = true;
}

void App::DrawDeleteConfirm(const std::string& peer) {
    if (openDeleteConfirm) {
        ImGui::OpenPopup(T("Delete messages?"));
        openDeleteConfirm = false;
    }
    bool mobile = ui::M().mobile;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(T("Delete messages?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    float h = mobile ? ui::M().rowH : 0;
    float w = mobile ? ImGui::GetMainViewport()->Size.x * 0.8f : ImGui::GetFontSize() * 16;
    size_t n = deleteMids.size();
    ImGui::TextUnformatted(n == 1 ? T("Delete this message?") : TF("Delete %d messages?", (int)n).c_str());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    if (deleteEveryoneOk)
        ImGui::TextDisabled(T("\"For everyone\" also removes it from %s (as soon as that device is reachable)."), eng.PeerName(peer).c_str());
    else
        ImGui::TextDisabled("%s", T("Messages you received can only be deleted on this device."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    auto done = [&] {
        for (auto& mid : deleteMids) selMsgs.erase(mid);
        if (selMsgs.empty()) selectionMode = false;
        deleteMids.clear();
        ImGui::CloseCurrentPopup();
    };
    if (deleteEveryoneOk) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(190, 60, 60, 255));
        if (ImGui::Button(T("Delete for everyone"), ImVec2(w, h))) { eng.DeleteMessages(peer, deleteMids, true); done(); }
        ImGui::PopStyleColor();
    }
    if (ImGui::Button(T("Delete for me"), ImVec2(w, h))) { eng.DeleteMessages(peer, deleteMids, false); done(); }
    if (ImGui::Button(T("Cancel"), ImVec2(w, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { deleteMids.clear(); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void App::DrawMessages(const std::string& peer) {
    auto it = eng.chats.find(peer);
    if (it == eng.chats.end() || it->second.messages.empty()) {
        ImGui::TextDisabled("%s", T("No messages yet. Say hi or drop some files here."));
        return;
    }
    Conversation& cv = it->second;
    if (cv.unread || cv.readPending) eng.MarkRead(peer);  // (sends read receipts)
    if (lastMsgPeer != peer || lastMsgCount != cv.messages.size()) {
        bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40;
        if (atBottom || lastMsgPeer != peer) scrollToBottom = true;
        lastMsgPeer = peer;
        lastMsgCount = cv.messages.size();
    }
    ImGuiIO& io = ImGui::GetIO();
    bool mobile = ui::M().mobile;
    float avail = ImGui::GetContentRegionAvail().x;
    float pad = ImGui::GetFontSize() * 0.6f;
    float maxW = std::max(avail * (mobile ? 0.86f : 0.68f), ImGui::GetFontSize() * 10);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string myName = "Me";
    std::string theirName = eng.PeerName(peer);
    int64_t lastDay = -1;

    for (int i = 0; i < (int)cv.messages.size(); i++) {
        ChatMessage& m = cv.messages[(size_t)i];
        ImGui::PushID(m.mid.c_str());
        // day separator
        int64_t day = m.ts / 86400000;
        if (day != lastDay) {
            lastDay = day;
            std::string ds = FormatTime(m.ts, true).substr(0, 10);
            ImVec2 ts = ImGui::CalcTextSize(ds.c_str());
            ImGui::SetCursorPosX((avail - ts.x) * 0.5f);
            ImGui::TextDisabled("%s", ds.c_str());
        }
        if (m.kind == MsgKind::System) {
            ImGui::PushTextWrapPos(avail);
            ImGui::TextDisabled("%s", Emoji().Display(TDynamic(m.text)).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopID();
            continue;
        }
        bool sel = selMsgs.count(m.mid) > 0;
        if (Engine::IsGroupKey(peer) && !m.outgoing && !m.from.empty()) {  // who wrote it (once for a run of messages)
            const ChatMessage* prev = i > 0 ? &cv.messages[(size_t)i - 1] : nullptr;
            if (!prev || prev->from != m.from || prev->outgoing || prev->kind == MsgKind::System || m.ts - prev->ts > 600000) {
                std::string n = eng.PeerName(m.from);
                if (const Group* g = eng.GroupOf(peer))
                    for (auto& gm : g->members)
                        if ("@" + gm.acc == m.from && !gm.name.empty() && eng.DevicesOf(m.from).empty()) n = gm.name;
                ImU32 col = (ImHashStr(m.from.c_str()) & 0x007F7F7F) | 0xFF808080;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad * 0.5f);
                ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", n.c_str());
                ImGui::PopFont();
            }
        }
        float x0 = ImGui::GetCursorPosX();
        if (m.kind == MsgKind::Files) {
            float w = mobile ? avail * 0.97f : std::min(maxW, ImGui::GetFontSize() * 26.0f);
            ImGui::SetCursorPosX(m.outgoing ? x0 + avail - w : x0);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(w, pad * 0.3f));
            ImGui::Indent(pad);
            DrawBatchCard(peer, m, w - pad * 2);
            ImGui::Unindent(pad);
            ImGui::Dummy(ImVec2(w, pad * 0.3f));
            ImGui::EndGroup();
            ImVec2 p1(p0.x + w, ImGui::GetItemRectMax().y);
            dl->ChannelsSetCurrent(0);
            ImU32 bg = ImGui::GetColorU32(sel ? ImGuiCol_HeaderActive : (m.outgoing ? ImGuiCol_FrameBgActive : ImGuiCol_FrameBg));
            dl->AddRectFilled(p0, p1, bg, pad);
            dl->ChannelsMerge();
            ReactHoverButton(m, p0, p1);
            DrawReactions(peer, m, p0, p1);
            // menu: right-click anywhere on the card (also over thumbnails / buttons); selection: click the background
            bool overCard = ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            if (overCard) {
                if (ImGui::IsMouseClicked(1)) { contextMid = m.mid; ImGui::OpenPopup("msgctx"); }
                if (ImGui::IsMouseClicked(0) && (io.KeyCtrl || selectionMode) && !ImGui::IsAnyItemHovered()) { sel ? (void)selMsgs.erase(m.mid) : (void)selMsgs.insert(m.mid); }
            }
            // phones: long-press anywhere on the card (the finger must not have moved: that's scrolling)
            ImVec2 cp = io.MouseClickedPos[0];
            if (mobile && overCard && ImGui::IsMouseDown(0) && io.MouseDownDuration[0] >= 0.55f && io.MouseDownDuration[0] - io.DeltaTime < 0.55f &&
                cp.x >= p0.x && cp.x <= p1.x && cp.y >= p0.y && cp.y <= p1.y && io.MouseDragMaxDistanceSqr[0] < ImGui::GetFontSize() * ImGui::GetFontSize() * 0.25f) {
                contextMid = m.mid;
                selectionMode = true;
                selMsgs.insert(m.mid);
                ImGui::ClearActiveID();  // the thumbnail / button under the finger must not fire on release
                ImGui::OpenPopup("msgctx");
            }
            ImGui::Dummy(ImVec2(0, pad * 0.3f));
        } else {
            float wrap = maxW - pad * 2;
            const std::string& shown = Emoji().Display(m.text);
            int bigEmoji = Emoji().EmojiOnly(m.text);  // only 1-3 emoji: show them big
            float textSize = ImGui::GetFontSize() * (bigEmoji == 1 ? 2.6f : bigEmoji ? 2.0f : 1.0f);
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(textSize, FLT_MAX, wrap, shown.c_str());
            const char* state = m.read ? T("read") : m.delivered ? T("delivered") : !m.mailedTo.empty() ? T("waiting on the server") : T("queued");
            std::string meta = FormatTime(m.ts, false) + (m.outgoing ? std::string("  ") + state : std::string());
            ImVec2 ms = ImGui::CalcTextSize(meta.c_str());
            float smallScale = 0.8f;
            float bw = std::max(ts.x, ms.x * smallScale) + pad * 2;
            float bh = ts.y + ms.y * smallScale + pad * 1.6f;
            ImGui::SetCursorPosX(m.outgoing ? x0 + avail - bw : x0);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##msg", ImVec2(bw, bh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            bool hovered = ImGui::IsItemHovered();
            bool longPress = mobile && ImGui::IsItemActive() && io.MouseDownDuration[0] > 0.55f && ImGui::GetMouseDragDelta(0).y == 0 &&
                             ImGui::GetMouseDragDelta(0).x == 0;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || (ImGui::IsItemDeactivated() && false)) {}
            if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(0) && !longPress) {
                if (io.KeyShift && lastSelIndex >= 0) {
                    int a = std::min(lastSelIndex, i), b = std::max(lastSelIndex, i);
                    for (int k = a; k <= b; k++) selMsgs.insert(cv.messages[(size_t)k].mid);
                } else if (io.KeyCtrl || selectionMode) {
                    if (sel) selMsgs.erase(m.mid); else selMsgs.insert(m.mid);
                    if (selMsgs.empty()) selectionMode = false;
                    lastSelIndex = i;
                } else {
                    bool wasOnly = sel && selMsgs.size() == 1;
                    selMsgs.clear();
                    if (!wasOnly) selMsgs.insert(m.mid);
                    lastSelIndex = i;
                }
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                textViewBuf = Emoji().Display(m.text);
                openTextView = true;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || longPress) {
                contextMid = m.mid;
                if (longPress) {
                    selectionMode = true;
                    selMsgs.insert(m.mid);
                    ImGui::ClearActiveID();
                }
                ImGui::OpenPopup("msgctx");
            }
            sel = selMsgs.count(m.mid) > 0;
            ImU32 bg = ImGui::GetColorU32(sel ? ImGuiCol_HeaderActive : (m.outgoing ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg),
                                          m.outgoing && !sel ? 0.55f : 1.0f);
            if (hovered && !sel) bg = ImGui::GetColorU32(m.outgoing ? ImGuiCol_ButtonActive : ImGuiCol_FrameBgHovered, m.outgoing ? 0.7f : 1.0f);
            dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh), bg, pad);
            dl->AddText(ImGui::GetFont(), textSize, ImVec2(p0.x + pad, p0.y + pad * 0.7f), ImGui::GetColorU32(ImGuiCol_Text), shown.c_str(), nullptr, wrap);
            ImU32 metaCol = m.outgoing && !m.delivered && m.mailedTo.empty() ? ui::ColWarn()
                          : m.outgoing && m.read ? IM_COL32(90, 170, 255, 255) : ui::ColMuted();
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * smallScale,
                        ImVec2(m.outgoing ? p0.x + bw - pad - ms.x * smallScale : p0.x + pad, p0.y + pad * 0.7f + ts.y + pad * 0.2f), metaCol, meta.c_str());
            ReactHoverButton(m, p0, ImVec2(p0.x + bw, p0.y + bh));
            DrawReactions(peer, m, p0, ImVec2(p0.x + bw, p0.y + bh));
            ImGui::Dummy(ImVec2(0, pad * 0.15f));
        }

        if (contextMid == m.mid && ImGui::BeginPopup("msgctx")) {
            float h = mobile ? ui::M().rowH * 0.85f : 0;
            if (QuickReactionRow(peer, m.mid, m.myReaction)) ImGui::CloseCurrentPopup();
            ImGui::Separator();
            if (ImGui::Selectable(T("Copy text"), false, 0, ImVec2(0, h))) {
                std::string text = m.text;
                if (m.kind == MsgKind::Files) {
                    text.clear();
                    for (auto tid : m.tids) {
                        auto t = eng.transfers.find(tid);
                        if (t != eng.transfers.end()) text += t->second->rel + "  " + t->second->sha256 + "\n";
                    }
                }
                ImGui::SetClipboardText(text.c_str());
            }
            if (m.kind == MsgKind::Text && ImGui::Selectable(T("Select text..."), false, 0, ImVec2(0, h))) {
                textViewBuf = Emoji().Display(m.text);
                openTextView = true;
            }
            bool s2 = selMsgs.count(m.mid) > 0;
            if (ImGui::Selectable(s2 ? T("Unselect") : T("Select"), false, 0, ImVec2(0, h))) {
                if (s2) selMsgs.erase(m.mid); else selMsgs.insert(m.mid);
                selectionMode = mobile && !selMsgs.empty();
            }
            if (!selMsgs.empty() && ImGui::Selectable(TF("Copy %d selected", (int)selMsgs.size()).c_str(), false, 0, ImVec2(0, h)))
                CopySelectedMessages(peer);
            if (ImGui::Selectable(T("Delete..."), false, 0, ImVec2(0, h))) AskDelete(peer, {m.mid});
            if (selMsgs.size() > 1 && ImGui::Selectable(TF("Delete %d selected...", (int)selMsgs.size()).c_str(), false, 0, ImVec2(0, h)))
                AskDelete(peer, std::vector<std::string>(selMsgs.begin(), selMsgs.end()));
            if (m.kind == MsgKind::Files) {
                if (ImGui::Selectable(T("Share these files with..."), false, 0, ImVec2(0, h))) {
                    std::vector<std::string> paths;
                    for (auto tid : m.tids) {
                        auto t = eng.transfers.find(tid);
                        if (t != eng.transfers.end() && FileExists(t->second->localPath) && t->second->state == TState::Completed) paths.push_back(t->second->localPath);
                    }
                    if (!paths.empty()) OpenSendTo(paths);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (scrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        scrollToBottom = false;
    }
    (void)myName;
    (void)theirName;
}

// ---------------------------------------------------------------- reactions
static const char* kQuickReactions[] = {"\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x98\x82",
                                        "\xF0\x9F\x98\xAE", "\xF0\x9F\x98\xA2", "\xF0\x9F\x99\x8F"};

// draws an emoji (real text) centered in a box
static void EmojiAt(ImDrawList* dl, const std::string& emoji, ImVec2 p0, ImVec2 p1, float size) {
    std::string g = Emoji().Glyph(emoji);
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, g.c_str());
    dl->AddText(ImGui::GetFont(), size, ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), g.c_str());
}

bool App::QuickReactionRow(const std::string& peer, const std::string& mid, const std::string& current) {
    bool mobile = ui::M().mobile;
    float s = mobile ? ui::M().rowH * 0.9f : ImGui::GetFrameHeight() * 1.5f;
    if (mobile)  // 6 + "more" must fit across the phone screen
        s = std::min(s, (ImGui::GetMainViewport()->WorkSize.x * 0.94f - ImGui::GetStyle().WindowPadding.x * 2) / 7.6f);
    bool done = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 6; i++) {
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##qr", ImVec2(s, s))) {
            eng.React(peer, mid, current == kQuickReactions[i] ? "" : kQuickReactions[i]);  // same one again removes it
            done = true;
        }
        bool on = current == kQuickReactions[i];
        if (on || ImGui::IsItemHovered())
            dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), ImGui::GetColorU32(on ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered), s * 0.5f);
        EmojiAt(dl, kQuickReactions[i], p, ImVec2(p.x + s, p.y + s), s * 0.6f);
        ImGui::PopID();
        ImGui::SameLine(0, s * 0.08f);
    }
    if (Emoji().Available()) {  // any emoji
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##qrmore", ImVec2(s, s))) {
            reactMid = mid;
            openReactMore = true;
            done = true;
        }
        if (ImGui::IsItemHovered()) {
            dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), ImGui::GetColorU32(ImGuiCol_HeaderHovered), s * 0.5f);
            if (!mobile) ImGui::SetTooltip("%s", T("More emoji"));
        }
        ui::DrawIcon(dl, Icon::Plus, ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.45f, ImGui::GetColorU32(ImGuiCol_Text));
    } else {
        ImGui::NewLine();
    }
    return done;
}

void App::ReactHoverButton(const ChatMessage& m, ImVec2 b0, ImVec2 b1) {
    if (ui::M().mobile || m.kind == MsgKind::System) return;  // phones: long-press menu
    float s = ImGui::GetFrameHeight();
    ImVec2 wmin = ImGui::GetWindowPos(), wmax(wmin.x + ImGui::GetWindowWidth(), wmin.y + ImGui::GetWindowHeight());
    // the whole row counts as hovering, so the button can be reached
    bool rowHover = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                    ImGui::IsMouseHoveringRect(ImVec2(wmin.x, b0.y), ImVec2(wmax.x, b1.y), false);
    if (!rowHover && !(openReactPick && reactMid == m.mid)) return;
    float x = m.outgoing ? b0.x - s * 1.25f : b1.x + s * 0.25f;
    ImVec2 p(x, (b0.y + b1.y - s) * 0.5f);
    ImVec2 back = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID("reacthover");
    if (ImGui::InvisibleButton("##rh", ImVec2(s, s))) {
        reactMid = m.mid;
        openReactPick = true;
    }
    bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetTooltip("%s", T("React"));
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.5f, ImGui::GetColorU32(hov ? ImGuiCol_ButtonHovered : ImGuiCol_Button));
    EmojiAt(dl, "\xF0\x9F\x99\x82", p, ImVec2(p.x + s, p.y + s), s * 0.55f);
    ImGui::SetCursorScreenPos(back);
}

void App::DrawReactions(const std::string& peer, ChatMessage& m, ImVec2 b0, ImVec2 b1) {
    if (m.myReaction.empty() && m.theirReaction.empty()) return;
    float fs = ImGui::GetFontSize();
    float ph = fs * 1.6f, es = fs * 1.05f;
    struct Pill {
        std::string emoji;
        int count;
        bool mine;
    };
    std::vector<Pill> pills;
    if (!m.myReaction.empty() && m.myReaction == m.theirReaction) pills.push_back({m.myReaction, 2, true});
    else {
        if (!m.theirReaction.empty()) pills.push_back({m.theirReaction, 1, false});
        if (!m.myReaction.empty()) pills.push_back({m.myReaction, 1, true});
    }
    float total = 0;
    std::vector<float> widths;
    for (auto& p : pills) {
        float w = ph * 1.1f + (p.count > 1 ? ImGui::CalcTextSize("2").x + fs * 0.35f : 0);
        widths.push_back(w);
        total += w + fs * 0.3f;
    }
    float x = m.outgoing ? b1.x - total - fs * 0.3f : b0.x + fs * 0.5f;
    float y = b1.y - ph * 0.3f;
    ImVec2 back = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < pills.size(); i++) {
        auto& p = pills[i];
        ImVec2 a(x, y), b(x + widths[i], y + ph);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID((int)i + 1000);
        bool click = ImGui::InvisibleButton("##pill", ImVec2(widths[i], ph));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov && !ui::M().mobile)
            ImGui::SetTooltip("%s", p.count > 1 ? T("You both reacted - click to remove yours") : p.mine ? T("Your reaction - click to remove") : T("Click to react the same"));
        if (click) eng.React(peer, m.mid, p.mine ? "" : p.emoji);
        ImU32 bg = ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
        dl->AddRectFilled(a, b, bg, ph * 0.5f);
        dl->AddRect(a, b, p.mine ? ImGui::GetColorU32(ImGuiCol_ButtonActive) : ImGui::GetColorU32(ImGuiCol_WindowBg), ph * 0.5f, 0, fs * 0.12f);
        EmojiAt(dl, p.emoji, a, ImVec2(a.x + ph * 1.1f, b.y), es);
        if (p.count > 1) dl->AddText(ImVec2(a.x + ph * 1.05f, a.y + (ph - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), "2");
        if (p.mine && !m.reactionSent) dl->AddCircleFilled(ImVec2(b.x - fs * 0.15f, a.y + fs * 0.15f), fs * 0.13f, ui::ColWarn());  // not delivered yet
        x += widths[i] + fs * 0.3f;
    }
    // make room below the bubble for the part of the pills that hangs out
    ImGui::SetCursorScreenPos(ImVec2(back.x, std::max(back.y, y + ph + fs * 0.15f)));
    ImGui::Dummy(ImVec2(0, 0));
}

void App::DrawReactionPopups(const std::string& peer) {
    if (openReactPick) {
        ImGui::OpenPopup("reactpick");
        openReactPick = false;
    }
    if (ImGui::BeginPopup("reactpick")) {
        std::string cur;
        auto it = eng.chats.find(peer);
        if (it != eng.chats.end())
            for (auto& m : it->second.messages)
                if (m.mid == reactMid) cur = m.myReaction;
        if (QuickReactionRow(peer, reactMid, cur)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (openReactMore) {
        ImGui::OpenPopup("reactmore");
        openReactMore = false;
    }
    bool mobile = ui::M().mobile;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 sz = mobile ? ImVec2(vp->WorkSize.x * 0.96f, vp->WorkSize.y * 0.6f) : ImVec2(ImGui::GetFontSize() * 26, ImGui::GetFontSize() * 20);
    ImGui::SetNextWindowSize(sz);
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (mobile) ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopup("reactmore")) {
        std::string picked;
        if (Emoji().DrawPicker(ImVec2(-1, -1), &picked)) {
            eng.React(peer, reactMid, Emoji().Restore(picked));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::CopySelectedMessages(const std::string& peer) {
    auto it = eng.chats.find(peer);
    if (it == eng.chats.end()) return;
    std::string out;
    int count = 0;
    for (auto& m : it->second.messages)
        if (selMsgs.count(m.mid)) count++;
    std::string them = eng.PeerName(peer);
    for (auto& m : it->second.messages) {
        if (!selMsgs.count(m.mid)) continue;
        std::string text = m.text;
        if (m.kind == MsgKind::Files) text = "[files] " + m.text;
        if (count == 1) out = text;
        else out += "[" + FormatTime(m.ts, true) + "] " + (m.outgoing ? std::string("Me") : them) + ": " + text + "\n";
    }
    ImGui::SetClipboardText(out.c_str());
    toasts.push_back({EventType::Info, T("Copied"), TF("%d message(s) copied to clipboard", count), "", 0, 2.5f});
}

void App::DrawBatchCard(const std::string& peer, const ChatMessage& m, float width) {
    auto ts = eng.BatchTransfers(m.batch);
    std::vector<std::shared_ptr<Transfer>> items;
    for (auto tid : m.tids) {
        auto it = eng.transfers.find(tid);
        if (it != eng.transfers.end()) items.push_back(it->second);
    }
    if (items.empty()) items = ts;
    int64_t total = 0, done = 0;
    int nDone = 0, nPending = 0, nFail = 0, nActive = 0, nPaused = 0, nHash = 0;
    double speed = 0;
    bool allVerified = !items.empty();
    for (auto& t : items) {
        total += t->size;
        int64_t d = t->state == TState::Completed ? t->size : t->done.load();
        if (t->state == TState::Hashing) d = 0;
        done += d;
        speed += t->speed;
        switch (t->state) {
            case TState::Completed: nDone++; break;
            case TState::Pending: nPending++; break;
            case TState::Failed: case TState::Cancelled: case TState::Rejected: nFail++; break;
            case TState::Paused: case TState::Interrupted: nPaused++; break;
            case TState::Hashing: nHash++; break;
            default: nActive++; break;
        }
        if (t->state != TState::Completed || t->recvSha != t->sha256) allVerified = false;
    }
    bool mobile = ui::M().mobile;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float ih = ImGui::GetTextLineHeight();
    ImVec2 ip = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(ih * 1.2f, ih));
    ui::DrawIcon(dl, items.size() > 1 ? Icon::Folder : Icon::File, ImVec2(ip.x + ih * 0.5f, ip.y + ih * 0.5f), ih, ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::SameLine();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - ih * 1.5f);
    ImGui::TextUnformatted(Emoji().Display(m.text).c_str());
    ImGui::PopTextWrapPos();
    if (items.empty()) {
        ImGui::TextDisabled("%s", T("(transfer details removed)"));
        return;
    }
    ImGui::TextDisabled("%s  -  %s", TF(m.outgoing ? "Sending %s" : "Receiving %s", FormatBytes((double)total).c_str()).c_str(),
                        items.size() > 1 ? TF("%d files", (int)items.size()).c_str() : T(TStateName(items[0]->state)));
    DrawThumbnails(peer, items, width);
    DrawAudioItems(items, width);
    // progress
    std::string overlay;
    char buf[160];
    if (nHash && !nActive) {
        int64_t hd = 0, hs = 0;
        for (auto& t : items) if (t->state == TState::Hashing) { hd += t->hashDone.load(); hs += t->size; }
        snprintf(buf, sizeof(buf), "Computing SHA-256... %.0f%%", hs ? 100.0 * (double)hd / (double)hs : 100.0);
        overlay = buf;
        ui::ProgressBarText(hs ? (float)((double)hd / (double)hs) : 1.f, overlay.c_str(), ImVec2(width, 0), ui::ColWarn());
    } else if (nDone == (int)items.size()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(allVerified ? ui::ColOk() : ui::ColWarn()));
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(ih, ih));
        ui::DrawIcon(dl, Icon::Check, ImVec2(cp.x + ih * 0.5f, cp.y + ih * 0.5f), ih, allVerified ? ui::ColOk() : ui::ColWarn());
        ImGui::SameLine();
        ImGui::TextUnformatted(allVerified ? T("Delivered - SHA-256 verified on both sides") : T("Delivered"));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && items.size() == 1) ImGui::SetTooltip(T("SHA-256 before: %s\nSHA-256 after:  %s"), items[0]->sha256.c_str(), items[0]->recvSha.c_str());
    } else {
        double frac = total ? (double)done / (double)total : (nDone ? 1.0 : 0.0);
        if (speed > 0) snprintf(buf, sizeof(buf), "%.0f%%  %s/s  %s left", frac * 100, FormatBytes(speed).c_str(), Eta((double)(total - done), speed).c_str());
        else snprintf(buf, sizeof(buf), "%.0f%%  %s", frac * 100, nPaused ? "paused" : nPending ? "waiting for accept" : nFail ? "problem" : "");
        ui::ProgressBarText((float)frac, buf, ImVec2(width, mobile ? ImGui::GetFrameHeight() : 0), nFail ? ui::ColErr() : nPaused ? ui::ColWarn() : 0);
    }
    // batch actions
    float h = mobile ? ui::M().rowH * 0.85f : 0;
    bool any = false;
    auto sl = [&]() { if (any) ImGui::SameLine(); any = true; };
    if (!m.outgoing && nPending) {
        sl();
        if (ui::IconTextButton(T("Accept"), Icon::Check, ImVec2(0, h))) eng.AcceptBatch(m.batch);
        sl();
        if (ui::IconTextButton(T("Decline"), Icon::Cancel, ImVec2(0, h)))
            for (auto& t : items) if (t->state == TState::Pending) eng.Reject(t->tid);
    }
    if (nActive || nHash) {
        sl();
        if (ui::IconTextButton(T("Pause"), Icon::Pause, ImVec2(0, h)))
            for (auto& t : items) eng.Pause(t->tid);
    }
    if (nPaused) {
        sl();
        if (ui::IconTextButton(T("Resume"), Icon::Play, ImVec2(0, h)))
            for (auto& t : items) eng.Resume(t->tid);
    }
    if (nActive || nPaused || nHash || (m.outgoing && nPending)) {
        sl();
        if (ui::IconTextButton(T("Cancel"), Icon::Cancel, ImVec2(0, h)))
            for (auto& t : items) eng.Cancel(t->tid);
    }
    if (m.outgoing && nFail) {
        sl();
        if (ui::IconTextButton(T("Retry"), Icon::Retry, ImVec2(0, h)))
            for (auto& t : items) if (t->state == TState::Failed || t->state == TState::Cancelled) eng.Retry(t->tid);
    }
    if (nDone && items.size() == 1) {
        sl();
        if (ui::IconTextButton(T("Open"), Icon::Open, ImVec2(0, h))) OpenPath(items[0]->localPath);
        sl();
        if (ui::IconTextButton(T("Folder"), Icon::Folder, ImVec2(0, h))) RevealPath(items[0]->localPath);
    } else if (nDone && !m.outgoing) {
        sl();
        std::string dir = JoinPath(eng.settings.downloadDir, items[0]->rel.substr(0, items[0]->rel.find('/')));
        if (ui::IconTextButton(T("Folder"), Icon::Folder, ImVec2(0, h))) RevealPath(items[0]->localPath.empty() ? eng.settings.downloadDir : DirName(items[0]->localPath));
    }
    if (nDone) {
        sl();
        if (ui::IconTextButton(T("Share"), Icon::Share, ImVec2(0, h))) {
            std::vector<std::string> paths;
            for (auto& t : items) if (t->state == TState::Completed && FileExists(t->localPath)) paths.push_back(t->localPath);
            OpenSendTo(paths);
        }
    }
    // single file error
    for (auto& t : items)
        if (!t->error.empty() && (t->state == TState::Failed || t->state == TState::Rejected) && items.size() == 1) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ColErr()), "%s", t->error.c_str());
            ImGui::PopTextWrapPos();
        }
    if (items.size() > 1) {
        bool open = expandedBatches.count(m.batch) > 0;
        if (ImGui::SmallButton(open ? T("Hide files") : T("Show files"))) {
            if (open) expandedBatches.erase(m.batch); else expandedBatches.insert(m.batch);
        }
        if (open) {
            int shown = 0;
            for (auto& t : items) {
                if (shown++ > 300) { ImGui::TextDisabled("..."); break; }
                ImGui::PushID((int)(t->tid & 0x7FFFFFFF));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
                ImGui::TextUnformatted(t->rel.c_str());
                ImGui::PopTextWrapPos();
                float frac = t->size ? (float)((double)(t->state == TState::Completed ? t->size : t->done.load()) / (double)t->size) : (t->state == TState::Completed ? 1.f : 0.f);
                snprintf(buf, sizeof(buf), "%s  %s", T(TStateName(t->state)), FormatBytes((double)t->size).c_str());
                ui::ProgressBarText(frac, buf, ImVec2(width - (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) * 2, 0), StateColor(t->state));
                ImGui::SameLine();
                DrawTransferActions(t);
                ImGui::PopID();
            }
        }
    }
    (void)peer;
}

// ============================================================================ transfers

void App::DrawTransferActions(const std::shared_ptr<Transfer>& t) {
    bool any = false;
    auto sl = [&]() { if (any) ImGui::SameLine(); any = true; };
    float sz = ImGui::GetFrameHeight();
    switch (t->state) {
        case TState::Pending:
            sl(); if (ui::IconButton("acc", Icon::Check, T("Accept"), sz)) eng.Accept(t->tid);
            sl(); if (ui::IconButton("rej", Icon::Cancel, T("Decline"), sz, true)) eng.Reject(t->tid);
            break;
        case TState::Active: case TState::Queued: case TState::Offered:
            sl(); if (ui::IconButton("pause", Icon::Pause, T("Pause"), sz)) eng.Pause(t->tid);
            sl(); if (ui::IconButton("cancel", Icon::Cancel, T("Cancel"), sz, true)) eng.Cancel(t->tid);
            break;
        case TState::Paused: case TState::Interrupted:
            sl(); if (ui::IconButton("resume", Icon::Play, T("Resume"), sz)) eng.Resume(t->tid);
            sl(); if (ui::IconButton("cancel", Icon::Cancel, T("Cancel"), sz, true)) eng.Cancel(t->tid);
            break;
        case TState::Hashing: case TState::Verifying:
            sl(); if (ui::IconButton("cancel", Icon::Cancel, T("Cancel"), sz, true)) eng.Cancel(t->tid);
            sl(); ImGui::Dummy(ImVec2(sz, sz));
            break;
        case TState::Completed:
            sl(); if (ui::IconButton("open", Icon::Open, T("Open"), sz)) OpenPath(t->localPath);
            sl(); if (ui::IconButton("folder", Icon::Folder, T("Show in folder"), sz)) RevealPath(t->localPath);
            break;
        default:
            if (t->outgoing) { sl(); if (ui::IconButton("retry", Icon::Retry, T("Retry"), sz)) eng.Retry(t->tid); }
            sl(); if (ui::IconButton("remove", Icon::Trash, T("Remove from list"), sz)) eng.RemoveTransfer(t->tid);
            break;
    }
}

void App::DrawTransferRow(const std::shared_ptr<Transfer>& t, bool compact) {
    char buf[200];
    int64_t done = t->state == TState::Completed ? t->size : t->done.load();
    float frac = t->size ? (float)((double)done / (double)t->size) : (t->state == TState::Completed ? 1.f : 0.f);
    if (t->state == TState::Hashing) frac = t->size ? (float)((double)t->hashDone.load() / (double)t->size) : 1.f;
    if (t->state == TState::Verifying && !t->outgoing) frac = t->size ? (float)((double)t->hashDone.load() / (double)t->size) : 1.f;
    if (t->state == TState::Active && t->speed > 0)
        snprintf(buf, sizeof(buf), "%.1f%%  %s / %s  %s/s  %s", frac * 100, FormatBytes((double)done).c_str(), FormatBytes((double)t->size).c_str(),
                 FormatBytes(t->speed).c_str(), Eta((double)(t->size - done), t->speed).c_str());
    else if (t->state == TState::Hashing || (t->state == TState::Verifying && !t->outgoing))
        snprintf(buf, sizeof(buf), "%s SHA-256 %.0f%%", t->state == TState::Hashing ? "Hashing" : "Verifying", frac * 100);
    else
        snprintf(buf, sizeof(buf), "%s  %.1f%%  %s", T(TStateName(t->state)), frac * 100, FormatBytes((double)t->size).c_str());
    float actionsW = (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) * 2;
    if (compact) {
        ImGui::TextUnformatted(t->rel.c_str());
        ImGui::TextDisabled("%s %s", t->outgoing ? "to" : "from", eng.PeerName(t->peer).c_str());
        ui::ProgressBarText(frac, buf, ImVec2(ImGui::GetContentRegionAvail().x - actionsW, ImGui::GetFrameHeight()), StateColor(t->state));
        ImGui::SameLine();
        DrawTransferActions(t);
    } else {
        ui::ProgressBarText(frac, buf, ImVec2(-1, 0), StateColor(t->state));
    }
    if (!t->error.empty() && TStateFinal(t->state)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ColErr()), "%s", t->error.c_str());
        ImGui::PopTextWrapPos();
    }
}

void App::DrawTransfers() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH : 0;
    // phones: three equal buttons sharing the width, with short labels, so the row always fits
    float bw = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3 : 0;
    if (ui::IconTextButton(mobile ? T("Pause") : T("Pause all"), Icon::Pause, ImVec2(bw, h))) eng.PauseAll(true);
    ImGui::SameLine();
    if (ui::IconTextButton(mobile ? T("Resume") : T("Resume all"), Icon::Play, ImVec2(bw, h))) eng.PauseAll(false);
    ImGui::SameLine();
    if (ui::IconTextButton(mobile ? T("Clear") : T("Clear finished"), Icon::Trash, ImVec2(bw, h))) eng.ClearFinishedTransfers();
    ImGui::Separator();
    std::vector<std::shared_ptr<Transfer>> list;
    for (auto& [id, t] : eng.transfers) list.push_back(t);
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) {
        bool fa = TStateFinal(a->state), fb = TStateFinal(b->state);
        if (fa != fb) return !fa;
        return a->created > b->created;
    });
    if (list.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("No transfers. Drop files on the window or use the + button in a chat."));
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::BeginChild("tlist");
    if (mobile) ui::TouchScroll();
    if (mobile) {
        for (auto& t : list) {
            ImGui::PushID((int)(t->tid & 0x7FFFFFFF));
            DrawTransferRow(t, true);
            ImGui::Separator();
            ImGui::PopID();
        }
    } else if (ImGui::BeginTable("transfers", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize());
        ImGui::TableSetupColumn(T("File"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(T("Device"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(T("Progress"), ImGuiTableColumnFlags_WidthStretch, 2.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) * 2);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)list.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                auto& t = list[(size_t)i];
                ImGui::PushID((int)(t->tid & 0x7FFFFFFF));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImVec2 p = ImGui::GetCursorScreenPos();
                float ih = ImGui::GetFrameHeight();
                ImGui::Dummy(ImVec2(ImGui::GetFontSize(), ih));
                ui::DrawIcon(ImGui::GetWindowDrawList(), t->outgoing ? Icon::Up : Icon::Down, ImVec2(p.x + ImGui::GetFontSize() * 0.5f, p.y + ih * 0.5f),
                             ImGui::GetFontSize() * 0.9f, t->outgoing ? ui::ColAccent() : ui::ColOk());
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(t->rel.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(T("%s\nSHA-256 (sender): %s\nSHA-256 (receiver): %s%s%s"), t->localPath.empty() ? t->rel.c_str() : t->localPath.c_str(),
                                      t->sha256.empty() ? "-" : t->sha256.c_str(), t->recvSha.empty() ? "-" : t->recvSha.c_str(),
                                      t->error.empty() ? "" : "\n", t->error.c_str());
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(eng.PeerName(t->peer).c_str());
                ImGui::TableNextColumn();
                DrawTransferRow(t, false);
                ImGui::TableNextColumn();
                DrawTransferActions(t);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

// ============================================================================ history

void App::DrawHistory() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH : 0;
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 18);
    ImGui::InputTextWithHint("##hfilter", T("Search name, device or hash"), &historyFilter);
    if (!mobile) ImGui::SameLine();
    ImGui::BeginDisabled(historySel.empty());
    std::string shareLabel = TF("Share selected (%d)", (int)historySel.size());
    if (ui::IconTextButton(shareLabel.c_str(), Icon::Share, ImVec2(0, h))) {
        std::vector<std::string> paths;
        for (auto& e : eng.history)
            if (historySel.count(e.tid) && FileExists(e.path)) paths.push_back(e.path);
        if (paths.empty()) toasts.push_back({EventType::Error, T("Nothing to share"), T("Selected files no longer exist on this device"), "", 0, 5});
        else OpenSendTo(paths);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::IconTextButton(T("Clear"), Icon::Trash, ImVec2(0, h))) ImGui::OpenPopup("clearhist");
    if (ImGui::BeginPopup("clearhist")) {
        ImGui::TextUnformatted(T("Remove all entries from the shared files history?\n(The files themselves are not deleted.)"));
        if (ImGui::Button(T("Clear history"))) { eng.ClearHistory(); historySel.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Separator();
    std::string f = historyFilter;
    std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    std::vector<int> rows;
    for (int i = (int)eng.history.size() - 1; i >= 0; i--) {
        auto& e = eng.history[(size_t)i];
        if (!f.empty()) {
            std::string hay = e.name + " " + e.peerName + " " + eng.PeerName(e.peer) + " " + e.sha256 + " " + e.path;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(f) == std::string::npos) continue;
        }
        rows.push_back(i);
    }
    if (rows.empty()) {
        ImGui::TextDisabled("%s", eng.history.empty() ? T("Files you send or receive will be listed here so you can share them again.") : T("No matches."));
        return;
    }
    ImGui::BeginChild("hlist");
    if (mobile) ui::TouchScroll();
    int removeIdx = -1;
    auto actions = [&](HistoryEntry& e, int idx) {
        bool exists = FileExists(e.path);
        float sz = mobile ? ui::M().rowH * 0.85f : ImGui::GetFrameHeight();
        ImGui::BeginDisabled(!exists);
        if (ImageCache::IsImagePath(e.path)) {
            if (ui::IconButton("view", Icon::Star, T("View picture"), sz)) OpenViewer(e.peer, e.path, true);
            if (exists && ImGui::IsItemHovered() && !ui::M().mobile) {
                const ImageTex& t = Images().Get(e.path, false);
                if (t.state == ImageTex::Ready && ImGui::BeginTooltip()) {
                    float s = ImGui::GetFontSize() * 12 / (float)std::max(t.w, t.h);
                    ImGui::Image(ImTextureRef(t.Id()), ImVec2((float)t.w * s, (float)t.h * s));
                    ImGui::EndTooltip();
                }
            }
            ImGui::SameLine();
        }
        if (ui::IconButton("share", Icon::Share, T("Share again with..."), sz)) OpenSendTo({e.path});
        ImGui::SameLine();
        if (ui::IconButton("open", Icon::Open, T("Open"), sz)) OpenPath(e.path);
        ImGui::SameLine();
        if (ui::IconButton("folder", Icon::Folder, T("Show in folder"), sz)) RevealPath(e.path);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ui::IconButton("hash", Icon::Copy, T("Copy SHA-256"), sz)) ImGui::SetClipboardText(e.sha256.c_str());
        ImGui::SameLine();
        if (ui::IconButton("rm", Icon::Trash, T("Remove from history"), sz, true)) removeIdx = idx;
    };
    if (mobile) {
        for (int idx : rows) {
            auto& e = eng.history[(size_t)idx];
            ImGui::PushID(idx);
            bool sel = historySel.count(e.tid) > 0;
            if (ImGui::Checkbox("##sel", &sel)) { if (sel) historySel.insert(e.tid); else historySel.erase(e.tid); }
            ImGui::SameLine();
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TextDisabled("%s %s  -  %s  -  %s %s", e.outgoing ? T("Sent to") : T("From"), eng.PeerName(e.peer).c_str(), FormatBytes((double)e.size).c_str(),
                                FormatTime(e.ts).c_str(), e.verified ? T("- verified") : "");
            if (!FileExists(e.path)) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ColWarn()), "%s", T("File moved or deleted"));
            else if (ImageCache::IsImagePath(e.path)) {
                const ImageTex& t = Images().Get(e.path, false);
                if (t.state == ImageTex::Ready) {
                    float th = ui::M().rowH * 2.2f;
                    float tw = std::min(ImGui::GetContentRegionAvail().x, th * (float)t.w / (float)t.h);
                    if (ImGui::ImageButton("##hthumb", ImTextureRef(t.Id()), ImVec2(tw, tw * (float)t.h / (float)t.w))) OpenViewer(e.peer, e.path, true);
                }
            }
            actions(e, idx);
            ImGui::Separator();
            ImGui::PopID();
        }
    } else if (ImGui::BeginTable("history", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn(T("File"), ImGuiTableColumnFlags_WidthStretch, 2.5f);
        ImGui::TableSetupColumn(T("Device"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(T("Size"), ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn(T("Date"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) * 6);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)rows.size());
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                int idx = rows[(size_t)r];
                auto& e = eng.history[(size_t)idx];
                ImGui::PushID(idx);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool sel = historySel.count(e.tid) > 0;
                if (ImGui::Checkbox("##sel", &sel)) { if (sel) historySel.insert(e.tid); else historySel.erase(e.tid); }
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                bool exists = FileExists(e.path);
                if (!exists) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextUnformatted(e.name.c_str());
                if (!exists) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(T("%s%s\nSHA-256: %s\n%s"), e.path.c_str(), exists ? "" : T("  (missing)"), e.sha256.c_str(), e.verified ? T("Verified on both sides") : "");
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s %s", e.outgoing ? "to" : "from", eng.PeerName(e.peer).c_str());
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(FormatBytes((double)e.size).c_str());
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(FormatTime(e.ts).c_str());
                ImGui::TableNextColumn();
                actions(e, idx);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    if (removeIdx >= 0) {
        historySel.erase(eng.history[(size_t)removeIdx].tid);
        eng.RemoveHistory((size_t)removeIdx);
    }
}

// ============================================================================ devices

void App::DrawDevices() {
    bool mobile = ui::M().mobile;
    auto peers = SortedPeers(false);
    if (peers.empty()) {
        ImGui::TextDisabled("%s", T("No devices yet."));
        return;
    }
    if (mobile) {
        ui::TouchScroll();
        for (auto& p : peers)
            if (DevicePeerRow(p, false, ui::M().rowH * 1.3f)) devicesSel = p;
        return;
    }
    ImGui::BeginChild("devlist", ImVec2(ImGui::GetFontSize() * 16, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    for (auto& p : peers)
        if (DevicePeerRow(p, p == devicesSel, 0)) devicesSel = p;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("devdetails");
    if (devicesSel.empty() || std::find(peers.begin(), peers.end(), devicesSel) == peers.end()) devicesSel = peers[0];
    DrawDeviceDetails(devicesSel);
    ImGui::EndChild();
}

void App::DrawDeviceDetails(const std::string& peer) {
    bool mobile = ui::M().mobile;
    if (mobile) ui::TouchScroll();
    float h = mobile ? ui::M().rowH : 0;
    auto it = eng.devices.find(peer);
    if (it == eng.devices.end()) {
        auto o = eng.online.find(peer);
        ImGui::Text("%s", o != eng.online.end() ? o->second.name.c_str() : peer.c_str());
        ImGui::TextDisabled("%s", T("Seen on the network but never connected."));
        if (ui::IconTextButton(T("Connect"), Icon::Link, ImVec2(0, h))) eng.ConnectPeer(peer);
        return;
    }
    KnownDevice& d = it->second;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextUnformatted(d.DisplayName().c_str());
    ImGui::PopFont();
    ImGui::TextDisabled(T("Announces itself as \"%s\""), d.name.c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted(T("Nickname (only on this device)"));
    static std::string nickBuf, nickPeer;
    if (nickPeer != peer || !ImGui::IsItemActive()) {
        if (nickPeer != peer) { nickPeer = peer; nickBuf = d.customName; }
    }
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 18);
    if (ImGui::InputTextWithHint("##nick", d.name.c_str(), &nickBuf)) eng.SetCustomName(peer, nickBuf);
    ImGui::Spacing();
    if (!d.pendingKey.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(110, 30, 30, 200));
        ImGui::BeginChild("keywarn", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(T("Identity key changed - connection refused"));
        ImGui::Text(T("Known key:  %s\nNew key:      %s"), KeyFingerprint(d.pubKey).c_str(), KeyFingerprint(d.pendingKey).c_str());
        ImGui::TextUnformatted(T("This happens when LunaShare was reinstalled or its settings were reset on that device. "
                               "It can also mean another device is pretending to be it. Only accept if you know why it changed, "
                               "then compare the new safety number on both screens."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(T("Accept new key"), ImVec2(0, h))) eng.AcceptNewKey(peer);
        ImGui::SameLine();
        if (ImGui::Button(T("Block device"), ImVec2(0, h))) eng.SetBlocked(peer, true);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    ImGui::SeparatorText(T("Security"));
    if (d.pubKey.empty()) {
        ImGui::TextDisabled("%s", T("No identity key yet - it is exchanged on the first connection."));
    } else {
        std::string safety = SafetyNumber(eng.MyPublicKey(), d.pubKey);
        ImGui::TextDisabled("%s", T("Safety number (identical on both devices when nobody is in between):"));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextUnformatted(safety.substr(0, 17).c_str());
        ImGui::TextUnformatted(safety.size() > 18 ? safety.substr(18).c_str() : "");
        ImGui::PopFont();
        bool ver = d.verified;
        if (ImGui::Checkbox(T("I compared it - mark as verified"), &ver)) eng.SetVerified(peer, ver);
        ImGui::TextDisabled(T("Their key: %s    Yours: %s"), KeyFingerprint(d.pubKey).c_str(), KeyFingerprint(eng.MyPublicKey()).c_str());
    }
    ImGui::Spacing();
    bool trusted = d.trusted, blocked = d.blocked;
    if (ImGui::Checkbox(T("Trusted - accept files automatically"), &trusted)) eng.SetTrusted(peer, trusted);
    if (ImGui::Checkbox(T("Blocked"), &blocked)) eng.SetBlocked(peer, blocked);
    ImGui::Spacing();
    if (ImGui::BeginTable("devinfo", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.5f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        auto row = [](const char* k, const std::string& v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.empty() ? "-" : v.c_str());
            if (ImGui::IsItemClicked()) ImGui::SetClipboardText(v.c_str());
        };
        std::string macs;
        for (auto& m : d.macs) macs += (macs.empty() ? "" : ", ") + m;
        row(T("User @ host"), d.user + " @ " + d.host);
        row(T("Platform"), d.platform);
        row(T("Fingerprint"), d.Fingerprint());
        row(T("Device ID"), d.id);
        row(T("MAC address"), macs);
        row(T("Last address"), d.lastIp.empty() ? "" : d.lastIp + ":" + std::to_string(d.lastPort));
        row(T("Bluetooth"), d.btAddr);
        row(T("First seen"), d.firstSeen ? FormatTime(d.firstSeen) : "");
        row(T("Last seen"), eng.IsConnected(peer) ? T("now") : (d.lastSeen ? FormatTime(d.lastSeen) : ""));
        int sent = 0, recv = 0;
        int64_t bytes = 0;
        for (auto& e : eng.history)
            if (e.peer == peer) { (e.outgoing ? sent : recv)++; bytes += e.size; }
        row(T("Shared"), TF("%d sent, %d received (%s)", (int)sent, (int)recv, FormatBytes((double)bytes).c_str()));
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", T("Click a value to copy it."));
    ImGui::Spacing();
    ImGui::TextUnformatted(T("Notes"));
    std::string notes = d.notes;
    if (ImGui::InputTextMultiline("##notes", &notes, ImVec2(mobile ? -1 : ImGui::GetFontSize() * 24, ImGui::GetTextLineHeight() * 3))) {
        d.notes = notes;
        eng.SetCustomName(peer, d.customName);  // marks dirty
    }
    ImGui::Spacing();
    bool connected = eng.IsConnected(peer);
    if (ui::IconTextButton(T("Chat"), Icon::Send, ImVec2(0, h))) {
        SelectPeer(peer);
        if (mobile) { backStack.push_back(screen); screen = MScreen::Chat; }
        else { tab = Tab::Chat; tabForce = true; }
    }
    ImGui::SameLine();
    if (ui::IconTextButton(connected ? T("Disconnect") : T("Connect"), Icon::Link, ImVec2(0, h))) connected ? eng.Disconnect(peer) : eng.ConnectPeer(peer);
    if (!d.btAddr.empty()) {
        ImGui::SameLine();
        if (ui::IconTextButton(T("Via Bluetooth"), Icon::Bluetooth, ImVec2(0, h))) eng.ConnectBluetooth(d.btAddr);
    }
    if (mobile) ImGui::Spacing(); else ImGui::SameLine();
    if (ui::IconTextButton(T("Clear chat"), Icon::Trash, ImVec2(0, h))) ImGui::OpenPopup("clearchat");
    ImGui::SameLine();
    if (ui::IconTextButton(T("Forget device"), Icon::Trash, ImVec2(0, h))) ImGui::OpenPopup("forget");
    if (ImGui::BeginPopup("clearchat")) {
        ImGui::TextUnformatted(T("Delete the chat history with this device (on this device only)?"));
        if (ImGui::Button(T("Delete"))) { eng.ClearChat(peer); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("forget")) {
        ImGui::TextUnformatted(T("Forget this device, its nickname and chat history?"));
        if (ImGui::Button(T("Forget"))) {
            eng.ForgetDevice(peer);
            if (selectedPeer == peer) selectedPeer.clear();
            devicesSel.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ============================================================================ connect / nearby

std::string App::UsernameError(const std::string& e) {
    if (e == "taken") return T("That username is already taken on this server.");
    if (e == "invalid name") return T("3 to 32 characters: letters, digits, dot, dash or underscore, starting with a letter or digit.");
    if (e == "not found") return T("Nobody uses this username on your server.");
    if (e == "that is you") return T("That's your own username.");
    return TDynamic(e);
}

void App::DrawNearby() {
    bool mobile = ui::M().mobile;
    if (mobile) ui::TouchScroll();
    float h = mobile ? ui::M().rowH : 0;
    ImGui::BeginChild("nearby");
    if (mobile) ui::TouchScroll();

    // ---- this device
    ImGui::SeparatorText(T("This device"));
    ImGui::Text("%s", eng.settings.displayName.c_str());
    ImGui::TextDisabled(T("Key fingerprint %s  -  port %u"), KeyFingerprint(eng.MyPublicKey()).c_str(), eng.boundPort);
    if (!eng.listenError.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ColErr()), "%s", eng.listenError.c_str());
    auto addrs = LocalIPv4Addresses();
    for (auto& a : addrs) {
        ImGui::BulletText("%s:%u", a.ip.c_str(), eng.boundPort);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", a.iface.c_str());
    }
    if (addrs.empty()) ImGui::TextDisabled("%s", T("No network connection."));
    ImGui::TextDisabled("%s", T("Devices on the same network find each other automatically:"));
    ImGui::BulletText(T("Bonjour / mDNS (_lunashare._tcp): %s"), eng.settings.discovery && eng.settings.mdns ? eng.MdnsStatus().c_str() : "off");
    ImGui::BulletText(T("UDP broadcast beacons (port %u): %s"), kDiscoveryPort, eng.settings.discovery && eng.settings.broadcast ? "on" : "off");
    ImGui::BulletText(T("Internet rendezvous: %s"), eng.settings.wanEnabled ? TDynamic(eng.WanStatus()).c_str() : T("off (Settings > Advanced > Internet)"));
    if (ui::IconTextButton(T("Search again"), Icon::Retry, ImVec2(0, h))) eng.RefreshDiscovery();
    if (!eng.online.empty()) {
        ImGui::TextDisabled("%s", T("Found on the network:"));
        for (auto& [id, p] : eng.online) {
            if (p.OnLan()) ImGui::BulletText("%s  %s:%u", eng.PeerName(id).c_str(), p.ip.c_str(), p.port);
            else ImGui::BulletText("%s", eng.PeerName(id).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%s%s)", TS(p.Via()).c_str(), eng.IsConnected(id) ? T(", connected") : "");
        }
    }

    // ---- manual
    // ---- find someone by username (internet server)
    if (eng.settings.wanEnabled && !eng.settings.wanServer.empty() && !eng.settings.wanFromInvite) {
        ImGui::SeparatorText(T("Find someone by username"));
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 14);
        bool enter = ImGui::InputTextWithHint("##finduser", T("username"), &findUserBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!mobile) ImGui::SameLine();
        if ((ImGui::Button(T("Find"), ImVec2(mobile ? -1 : 0, h)) || enter) && !findUserBuf.empty()) eng.LookupUsername(findUserBuf);
        const auto& lu = eng.lookup;
        if (lu.pending) {
            ImGui::TextDisabled("%s", T("Searching..."));
        } else if (!lu.error.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ColWarn()), "%s", UsernameError(lu.error).c_str());
        } else if (!lu.acc.empty()) {
            std::string key = "@" + lu.acc;
            ImGui::Text("@%s", lu.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(lu.devices.empty() ? T("- not online right now") : T("- %d device(s) online"), (int)lu.devices.size());
            bool connected = eng.IsConnected(key);
            if (connected) {
                if (ImGui::Button(T("Open chat"), ImVec2(mobile ? -1 : 0, h))) {
                    SelectPeer(key);
                    if (mobile) { backStack = {MScreen::Home}; screen = MScreen::Chat; }
                    else { tab = Tab::Chat; tabForce = true; }
                }
            } else if (!lu.devices.empty() && ImGui::Button(T("Connect"), ImVec2(mobile ? -1 : 0, h))) {
                eng.ConnectLookedUp();
                findUserAcc = lu.acc;
            }
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", T("After connecting, compare the safety number with them (device details) to be sure it's really them."));
            ImGui::PopTextWrapPos();
        }
    }

    ImGui::SeparatorText(T("Connect by address"));
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##host", "192.168.1.20", &manualHost, mobile ? 0 : ImGuiInputTextFlags_None);
    if (!mobile) ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    ImGui::InputInt("##port", &manualPort, 0);
    ImGui::SameLine();
    std::string host = manualHost;
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        manualPort = atoi(host.c_str() + colon + 1);
        host = host.substr(0, colon);
    }
    ImGui::BeginDisabled(host.empty());
    if (ui::IconTextButton(T("Connect"), Icon::Link, ImVec2(0, h))) eng.ConnectAddress(host, (uint16_t)manualPort);
    ImGui::EndDisabled();

    // ---- bluetooth
    ImGui::SeparatorText(T("Bluetooth"));
    if (!eng.btSupported) {
        ImGui::TextDisabled(T("Bluetooth is not available here (no adapter or not supported on %s yet)."), PlatformName());
    } else {
        bool bt = eng.settings.bluetooth;
        if (ImGui::Checkbox(T("Accept Bluetooth connections"), &bt)) {
            Settings s = eng.settings;
            s.bluetooth = bt;
            eng.ApplySettings(s);
            if (bt && Bridge().requestPermissions) Bridge().requestPermissions();
        }
        ImGui::TextDisabled("%s", eng.btStatus.c_str());
        ImGui::TextDisabled("%s", T("Pair the devices in the system Bluetooth settings first. Bluetooth is slow (~100-300 KB/s) - good for chat, small files, or handing over hotspot credentials."));
        if (ui::IconTextButton(eng.btScanning ? T("Searching...") : T("Paired devices"), Icon::Bluetooth, ImVec2(0, h)) && !eng.btScanning) eng.BtRefresh(false);
        ImGui::SameLine();
        if (ui::IconTextButton(T("Scan nearby"), Icon::Retry, ImVec2(0, h)) && !eng.btScanning) eng.BtRefresh(true);
        for (auto& d : eng.btDevices) {
            ImGui::PushID(d.addr.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", d.name.empty() ? d.addr.c_str() : d.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s%s", d.addr.c_str(), d.paired ? T(" (paired)") : "");
            ImGui::SameLine();
            if (ImGui::Button(T("Connect"), ImVec2(0, mobile ? h * 0.8f : 0))) eng.ConnectBluetooth(d.addr);
            ImGui::PopID();
        }
    }

    // ---- hotspot
    ImGui::SeparatorText(T("Phone-to-phone (Wi-Fi hotspot)"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", T("No shared Wi-Fi? One phone starts a hotspot, the other joins it - then they find each other automatically "
                        "at full Wi-Fi speed. The other phone can join by scanning the QR code with its camera, or automatically "
                        "when both are connected over Bluetooth."));
    ImGui::PopTextWrapPos();
    if (Bridge().hotspotStart) {
        if (!eng.hotspot.active) {
            if (ui::BigButton(T("Start hotspot on this phone"), Icon::Wifi, mobile ? -1 : ImGui::GetFontSize() * 18)) Bridge().hotspotStart();
        } else {
            ImGui::Text(T("Hotspot running: %s"), eng.hotspot.ssid.c_str());
            ImGui::Text(T("Password: %s"), eng.hotspot.pass.c_str());
            std::string wifiQr = "WIFI:T:WPA;S:" + eng.hotspot.ssid + ";P:" + eng.hotspot.pass + ";;";
            ui::QrCode(wifiQr.c_str(), std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 16));
            for (auto& c : eng.conns) {
                if (!c->ready || c->closed) continue;
                ImGui::PushID((int)c->cid);
                std::string l = TF("Send Wi-Fi login to %s", eng.PeerName(c->peerId).c_str());
                if (ui::IconTextButton(l.c_str(), Icon::Share, ImVec2(0, h))) eng.SendWifiCreds(c->peerId, eng.hotspot.ssid, eng.hotspot.pass);
                ImGui::PopID();
            }
            if (ui::IconTextButton(T("Stop hotspot"), Icon::Cancel, ImVec2(0, h)) && Bridge().hotspotStop) Bridge().hotspotStop();
        }
    }
    if (Bridge().wifiJoin) {
        ImGui::TextUnformatted(T("Join a hotspot"));
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 14);
        ImGui::InputTextWithHint("##jssid", T("Network name (SSID)"), &joinSsid);
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 14);
        ImGui::InputTextWithHint("##jpass", T("Password"), &joinPass, ImGuiInputTextFlags_Password);
        ImGui::BeginDisabled(joinSsid.empty());
        if (ui::IconTextButton(T("Join"), Icon::Wifi, ImVec2(0, h))) Bridge().wifiJoin(joinSsid, joinPass);
        ImGui::EndDisabled();
    }
    if (!IsMobilePlatform()) {
#ifdef _WIN32
        if (ImGui::Button(T("Open Windows Mobile hotspot settings"))) OpenWithDefaultApp("ms-settings:network-mobilehotspot");
#endif
        ImGui::TextDisabled("%s", T("Sharing this computer's hotspot? Enter its name and password to show a QR code phones can scan:"));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::InputTextWithHint("##qssid", T("SSID"), &qrSsid);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::InputTextWithHint("##qpass", T("Password"), &qrPass);
        if (!qrSsid.empty()) {
            std::string wifiQr = "WIFI:T:WPA;S:" + qrSsid + ";P:" + qrPass + ";;";
            ui::QrCode(wifiQr.c_str(), ImGui::GetFontSize() * 12);
            for (auto& c : eng.conns) {
                if (!c->ready || c->closed || c->transport != Transport::Bluetooth) continue;
                ImGui::PushID((int)c->cid);
                if (ImGui::Button(TF("Send to %s over Bluetooth", eng.PeerName(c->peerId).c_str()).c_str())) eng.SendWifiCreds(c->peerId, qrSsid, qrPass);
                ImGui::PopID();
            }
        }
    }
    ImGui::Dummy(ImVec2(0, h));
    ImGui::EndChild();
}

// ============================================================================ settings

void App::DrawSettings() {
    settingsShownFrame = ImGui::GetFrameCount();
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH : 0;
    ImGui::BeginChild("settings");
    if (mobile) ui::TouchScroll();
    bool changed = false;
    float fieldW = mobile ? -1 : ImGui::GetFontSize() * 22;

    DrawPrivateContacts(fieldW, h, changed);  // first: using / making an invite code is what new users need

    ImGui::SeparatorText(T("Profile"));
    DrawProfilePicture(h);
    ImGui::TextUnformatted(T("Name shown to other devices"));
    ImGui::SetNextItemWidth(fieldW);
    ImGui::InputText("##dname", &edit.displayName);
    if (ImGui::IsItemActive()) editDirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;

    DrawAccountSettings(fieldW, h);

    ImGui::SeparatorText(T("Internet"));
    if (edit.wanFromInvite) {
        // joined with someone's invite: their server's address isn't shown (nor its rooms and usernames)
        std::string who = edit.wanLinks.empty() ? std::string() : edit.wanLinks[0].label;
        ImGui::PushTextWrapPos(0);
        if (who.empty()) ImGui::TextUnformatted(T("Connected to the internet through an invite."));
        else ImGui::TextUnformatted(TF("Connected to the internet through %s's invite.", who.c_str()).c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button(T("Use my own server instead"), ImVec2(0, h))) {
            edit.wanServer.clear();
            edit.wanEnabled = false;
            edit.wanHidden = false;
            edit.wanLinks.clear();
            edit.wanFromInvite = false;
            changed = true;
        }
        ui::HelpMarker(T("Removes the invite (and any other invites to the same server): you lose touch with those people "
                       "over the internet, and can enter a server of your own."));
    } else {
    ImGui::TextUnformatted(T("Rendezvous server (optional)"));
    ImGui::SetNextItemWidth(fieldW);
    ImGui::InputTextWithHint("##wanserver", "home.example.com", &edit.wanServer);
    if (ImGui::IsItemActive()) editDirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        edit.wanEnabled = !edit.wanServer.empty();  // a server set = use it
        changed = true;
    }
    }
    if (!edit.wanFromInvite) ui::HelpMarker(T("Lets your devices find each other outside the local network. The server only introduces them; chat and "
                   "files are end-to-end encrypted and go directly between the devices (through the server only when no direct "
                   "path is possible). Leave empty for local network, hotspot and Bluetooth only."));
    if (eng.settings.wanEnabled && !eng.settings.wanServer.empty()) {
        ImGui::TextDisabled(T("Status: %s"), TDynamic(eng.WanStatus()).c_str());
        std::string mp = eng.WanMappedEndpoint();
        if (!mp.empty()) ImGui::TextDisabled(T("Reachable through the router at %s"), mp.c_str());
    }

    // running in the background: phones keep a connection (foreground service), computers can close to the tray
    if (IsMobilePlatform()) {
        if (ImGui::Checkbox(T("Keep running in the background"), &edit.backgroundRun)) {
            changed = true;
            if (Bridge().backgroundService) Bridge().backgroundService(edit.backgroundRun);
        }
        ui::HelpMarker(T("Messages and calls arrive while LunaShare is in the background (a small notification shows that it is connected). Uses a little battery."));
        static double lastCheck = -10;
        static bool restricted = false;
        if (ImGui::GetTime() - lastCheck > 2) {
            lastCheck = ImGui::GetTime();
            restricted = Bridge().batteryRestricted && Bridge().batteryRestricted();
        }
        if (edit.backgroundRun && restricted) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "%s", T("Battery optimisation may cut the connection while the screen is off."));
            ImGui::PopTextWrapPos();
            if (ImGui::Button(T("Allow running in the background..."), ImVec2(mobile ? -1 : 0, h)) && Bridge().requestBatteryExemption)
                Bridge().requestBatteryExemption();
        }
    } else {
        changed |= ImGui::Checkbox(T("Keep running when the window is closed"), &edit.closeToTray);
#if defined(_WIN32)
        ui::HelpMarker(T("Closing the window keeps LunaShare connected for messages and calls (in the tray next to the clock). Quit it from the tray icon."));
#else
        ui::HelpMarker(T("Closing the window only minimises it, so LunaShare stays connected for messages and calls."));
#endif
    }

    ImGui::SeparatorText(T("Received files"));
    ImGui::TextUnformatted(T("Save received files to"));
    ImGui::SetNextItemWidth(mobile ? -1 : fieldW);
    ImGui::InputText("##dldir", &edit.downloadDir);
    if (ImGui::IsItemActive()) editDirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    if (!mobile) ImGui::SameLine();
    if (ImGui::Button(T("Browse..."), ImVec2(0, h))) {
        browseFor = BrowseFor::DownloadDir;
        browser.Open(FileBrowser::Mode::SelectDir, T("Folder for received files"), edit.downloadDir);
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Open folder"), ImVec2(0, h))) RevealPath(edit.downloadDir);
    ImGui::SeparatorText(T("Appearance"));
    int layout = (int)edit.layout;
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::Combo("##layout", &layout, TItems("Layout: automatic\0Layout: desktop\0Layout: phone (vertical, big buttons)\0"))) {
        edit.layout = (UiLayout)layout;
        changed = true;
        styleGeneration++;
    }
    ImGui::SetNextItemWidth(fieldW);
    float sc = edit.uiScale <= 0 ? 0.0f : edit.uiScale;
    if (ImGui::SliderFloat("##scale", &sc, 0.0f, 3.0f, sc < 0.5f ? T("UI scale: automatic") : T("UI scale: %.2fx"))) {
        edit.uiScale = sc < 0.5f ? 0.0f : sc;
        editDirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { changed = true; styleGeneration++; }
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::Combo("##theme", &edit.theme, TItems("Theme: dark\0Theme: light\0Theme: classic\0"))) { changed = true; styleGeneration++; }
    {
        // language: automatic (the system's) or one of the translations
        const auto& langs = Languages();
        std::string cur = T("Language: automatic");
        for (auto& l : langs)
            if (edit.language == l.code) cur = std::string(T("Language: ")) + l.name;
        ImGui::SetNextItemWidth(fieldW);
        if (ImGui::BeginCombo("##lang", cur.c_str())) {
            auto pick = [&](const std::string& code) {
                edit.language = code;
                SetLanguage(code);
                changed = true;
            };
            if (ImGui::Selectable(T("Automatic (system language)"), edit.language.empty(), 0, ImVec2(0, h))) pick("");
            for (auto& l : langs)
                if (ImGui::Selectable(l.name, edit.language == l.code, 0, ImVec2(0, h))) pick(l.code);
            ImGui::EndCombo();
        }
    }
    ImGui::TextDisabled("%s", Emoji().Available() ? T("Colour emoji: on") : T("Colour emoji: off (module not installed)"));

    ImGui::Spacing();
    // everything below works automatically: hidden until asked for
    static bool advEnv = getenv("LUNASHARE_ADVANCED") != nullptr;  // dev: start with Advanced open (screenshots)
    if (advEnv) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::CollapsingHeader(T("Advanced"), ImGuiTreeNodeFlags_None)) {
        ImGui::SeparatorText(T("Receiving"));
        changed |= ImGui::Checkbox(T("Automatically accept files from trusted devices"), &edit.autoAcceptTrusted);
        changed |= ImGui::Checkbox(T("Automatically accept files from everyone"), &edit.autoAcceptAll);
        ui::HelpMarker(T("Anyone on your network could send you files without asking. Only enable on networks you trust."));
        changed |= ImGui::Checkbox(T("Resume interrupted transfers automatically"), &edit.autoResume);

        ImGui::SeparatorText(T("Network"));
        changed |= ImGui::Checkbox(T("Find devices on the local network"), &edit.discovery);
        ImGui::BeginDisabled(!edit.discovery);
        ImGui::Indent();
        changed |= ImGui::Checkbox(T("Bonjour / mDNS"), &edit.mdns);
        ui::HelpMarker(T("Standard service discovery (_lunashare._tcp). Works on most routers and is the only method iPhones/iPads can use."));
        changed |= ImGui::Checkbox(T("UDP broadcast beacons"), &edit.broadcast);
        ui::HelpMarker(T("Fast discovery that also works on networks without multicast. Not available on iOS."));
        ImGui::Unindent();
        ImGui::EndDisabled();
        changed |= ImGui::Checkbox(T("Connect to found devices automatically"), &edit.autoConnect);
        int port = edit.tcpPort;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::InputInt(T("TCP port"), &port, 0)) { edit.tcpPort = (uint16_t)std::clamp(port, 1, 65535); editDirty = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        ImGui::SameLine();
        ImGui::TextDisabled(T("(listening on %u)"), eng.boundPort);

        ImGui::SeparatorText(T("Transfers"));
        int limit = (int)(edit.speedLimitKBs / 1024);
        ImGui::SetNextItemWidth(fieldW);
        if (ImGui::SliderInt("##limit", &limit, 0, 200, limit == 0 ? T("Upload speed: unlimited") : T("Upload limit: %d MB/s"))) { edit.speedLimitKBs = (int64_t)limit * 1024; changed = true; }
        ImGui::SetNextItemWidth(fieldW);
        changed |= ImGui::SliderInt("##par", &edit.maxParallel, 1, 8, T("Files at once per device: %d"));
        ImGui::SetNextItemWidth(fieldW);
        changed |= ImGui::SliderInt("##chunk", &edit.chunkKB, 16, 2048, T("Chunk size: %d KB"), ImGuiSliderFlags_Logarithmic);

        DrawCallSettings(fieldW, h, changed);

        ImGui::SeparatorText(T("Internet"));
        bool invited = edit.wanFromInvite;
        if (!invited) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("Devices that use the same server, room name and password see each other. Without a room name, every "
                            "device on the server is in one shared room - set a name and password to keep your devices to yourself."));
        ImGui::PopTextWrapPos();
        }
        changed |= ImGui::Checkbox(T("Use the rendezvous server"), &edit.wanEnabled);
        ImGui::BeginDisabled(!edit.wanEnabled);
        auto wanField = [&](const char* label, const char* hint, std::string* v, ImGuiInputTextFlags f) {
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(fieldW);
            ImGui::InputTextWithHint((std::string("##") + label).c_str(), hint, v, f);
            if (ImGui::IsItemActive()) editDirty = true;
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        };
        if (!invited) {
            wanField(T("Room name"), T("family"), &edit.wanRoom, 0);
            wanField(T("Room password"), T("shared by your devices"), &edit.wanPassword, ImGuiInputTextFlags_Password);
        }
        changed |= ImGui::Checkbox(T("Relay through the server when a direct connection fails"), &edit.wanRelay);
        ui::HelpMarker(T("Some networks can't be reached directly at all (mobile data and other carrier-grade NAT on both sides, "
                       "strict firewalls). The server then passes the data along. It stays end-to-end encrypted, the server can't read it."));
        changed |= ImGui::Checkbox(T("Group chats and calls through the server"), &edit.groupsViaServer);
        ui::HelpMarker(T("People in your groups whom you don't know otherwise don't connect to your device directly, so they don't "
                       "learn your internet address. Messages go through the server's mailbox and calls through the server, "
                       "end-to-end encrypted. In a group call everyone also sends their voice and video only once, to the "
                       "server, instead of to every person - easier on phones."));
        changed |= ImGui::Checkbox(T("Ask the router for a port mapping (UPnP / NAT-PMP)"), &edit.wanNatPmp);
        ui::HelpMarker(T("Most home routers allow this automatically. It makes direct connections work behind routers that change the "
                       "port for every destination."));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::InputInt(T("Fixed local UDP port"), &edit.wanLocalPort, 0)) { edit.wanLocalPort = std::clamp(edit.wanLocalPort, 0, 65535); editDirty = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::InputInt(T("Port forwarded on the router"), &edit.wanForwardedPort, 0)) { edit.wanForwardedPort = std::clamp(edit.wanForwardedPort, 0, 65535); editDirty = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        ui::HelpMarker(T("Only if you forward a UDP port on your router to this device by hand: the local port above, the external port here."));
        ImGui::EndDisabled();
        if (eng.settings.wanEnabled) {
            std::string ep = eng.WanPublicEndpoint();
            if (!ep.empty()) ImGui::TextDisabled(T("Seen by the server as %s"), ep.c_str());
            if (!invited && ImGui::Button(T("Copy room hash (for the server's allow-list)"), ImVec2(0, h)))
                ImGui::SetClipboardText(RoomHash(eng.settings.wanRoom, eng.settings.wanPassword).c_str());
        }
        if (!edit.wanFromInvite) {
            changed |= ImGui::Checkbox(T("Private mode: don't list me in the room"), &edit.wanHidden);
            ui::HelpMarker(T("Others in the room don't see this device. You still see them and can contact them; your own devices "
                           "and people with one of your invite codes still find you."));
        }
        if (eng.WanServerLacksLinks())
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", T("The server is too old for private contacts: update it."));


        ImGui::SeparatorText(T("Bluetooth"));
        changed |= ImGui::Checkbox(T("Accept Bluetooth connections##s"), &edit.bluetooth);
    #if defined(__linux__) && !defined(__ANDROID__)
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        changed |= ImGui::SliderInt(T("RFCOMM channel"), &edit.btChannel, 1, 30);
        ui::HelpMarker(T("Linux uses a fixed RFCOMM channel. Use the same channel on both Linux machines."));
    #endif

        ImGui::SeparatorText(T("History & privacy"));
        changed |= ImGui::Checkbox(T("Keep chat and shared-file history"), &edit.keepHistory);
        if (ImGui::Button(T("Clear shared files history"), ImVec2(0, h))) eng.ClearHistory();

        ImGui::SeparatorText(T("Backup / move to another device"));
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("Exports all settings plus your list of known devices with their nicknames, trust and block state."));
        ImGui::PopTextWrapPos();
        ImGui::Checkbox(T("Include chat & shared files history"), &exportHistory);
        ImGui::Checkbox(T("Include this device's identity (only for restoring THIS device)"), &exportIdentity);
        if (ui::IconTextButton(T("Export settings..."), Icon::Up, ImVec2(0, h))) {
            browseFor = BrowseFor::Export;
            std::string name = "lunashare-settings-" + FormatTime(NowMs(), true).substr(0, 10) + ".json";
            browser.Open(FileBrowser::Mode::SaveFile, T("Export settings"), eng.settings.downloadDir, name, ".json");
        }
        ImGui::SameLine();
        if (ui::IconTextButton(T("Import settings..."), Icon::Down, ImVec2(0, h))) {
            browseFor = BrowseFor::Import;
            browser.Open(FileBrowser::Mode::OpenFile, T("Import settings"), eng.settings.downloadDir, "", ".json");
        }
        ImGui::Checkbox(T("When importing, also take over the device identity"), &importIdentity);

    }

    ImGui::SeparatorText(T("About"));
    ImGui::TextDisabled(T("LunaShare %s  -  %s  -  protocol v%d"), LUNASHARE_VERSION, PlatformName(), kProtocolVersion);
    ImGui::TextDisabled(T("Settings folder: %s"), (eng.configDir.empty() ? ConfigDir() : eng.configDir).c_str());
    ImGui::TextDisabled("%s", T("Every file is hashed with SHA-256 before sending and again after receiving; transfers can be paused, resumed, and survive restarts."));
    ImGui::TextDisabled("%s", T("All connections are end-to-end encrypted (X25519 key exchange, XChaCha20-Poly1305, EdDSA device keys pinned on first contact)."));
    ImGui::TextDisabled(T("This device's key fingerprint: %s"), KeyFingerprint(eng.MyPublicKey()).c_str());
    ImGui::Dummy(ImVec2(0, h));
    ImGui::EndChild();

    if (changed) {
        eng.ApplySettings(edit);
        editDirty = false;
    }
}

// ============================================================================ popups & toasts

void App::DrawPopups() {
    DrawGroupPopups();
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 center(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f);
    auto centerNext = [&](float wFrac) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.94f, 0));
        else if (wFrac > 0) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * wFrac, 0), ImGuiCond_Appearing);
    };

    // ---- file browser results
    bool browserDone = browser.Draw();
    if (browser.prefsChanged) {
        browser.prefsChanged = false;
        Settings s = eng.settings;
        s.browserGrid = browser.prefs.grid;
        s.browserPicturesOnly = browser.prefs.picturesOnly;
        s.browserSort = browser.prefs.sort;
        s.browserTile = browser.prefs.tile;
        eng.ApplySettings(s);
        edit = eng.settings;
    }
    if (Emoji().settingsChanged) {  // picker: recent emoji / skin tone
        Emoji().settingsChanged = false;
        Settings s = eng.settings;
        s.emojiRecent = edit.emojiRecent = Emoji().recent;
        s.emojiTone = edit.emojiTone = Emoji().tone;
        eng.ApplySettings(s);
    }
    if (browserDone) {
        auto res = browser.result;
        Settings s = eng.settings;
        s.lastBrowseDir = browser.lastDir;
        std::string err;
        switch (browseFor) {
            case BrowseFor::SendFiles:
                if (!selectedPeer.empty()) { eng.SendPaths(selectedPeer, res); scrollToBottom = true; }
                else OpenSendTo(res);
                break;
            case BrowseFor::WatchSubs: {
                int id = res.empty() ? -1 : Player().AddSubtitles(res[0]);
                if (id >= 0) {
                    Player().SelectTrack(LSP_TRACK_SUBTITLE, id);
                    watchSubFollow = false;  // (a viewer's own choice)
                    if (eng.watch.sid && eng.watch.host.empty()) {
                        eng.WatchShareSubtitles(res[0]);  // the others get the file too
                        watchSubsAdded.insert(res[0]);
                    }
                }
                else toasts.push_back({EventType::Error, T("Subtitles"), T("Could not read the subtitles (.srt, .ass or .vtt)."), "", 0, 4.0f});
                break;
            }
            case BrowseFor::Watch:
                if (!selectedPeer.empty() && !res.empty() && eng.StartWatch(selectedPeer, res[0])) watchShown = true;
                break;
            case BrowseFor::DownloadDir:
                edit.downloadDir = res[0];
                s.downloadDir = res[0];
                break;
            case BrowseFor::Export:
                if (eng.ExportSettings(res[0], exportHistory, exportIdentity, &err)) infoMessage = T("Settings exported to\n") + res[0];
                else infoMessage = T("Export failed: ") + err;
                openInfo = true;
                break;
            case BrowseFor::BackupSave:
            case BrowseFor::BackupOpen:
                if (!res.empty()) {
                    backupPath = res[0];
                    backupSaving = browseFor == BrowseFor::BackupSave;
                    backupPass.clear();
                    backupPass2.clear();
                    accountError.clear();
                    openBackupPass = true;
                }
                break;
            case BrowseFor::Avatar:
                if (!res.empty()) {
                    avatarSrc = res[0];
                    avatarZoom = 1;
                    avatarCx = avatarCy = 0.5f;
                    openAvatarEditor = true;
                }
                break;
            case BrowseFor::Import:
                if (eng.ImportSettings(res[0], importIdentity, &err)) { infoMessage = T("Settings imported from\n") + res[0]; styleGeneration++; s = eng.settings; s.lastBrowseDir = browser.lastDir; }
                else infoMessage = T("Import failed: ") + err;
                openInfo = true;
                break;
            default: break;
        }
        eng.ApplySettings(s);
        edit = eng.settings;
        browseFor = BrowseFor::None;
    }

    DrawIncomingCall();

    // ---- send to device picker
    if (openSendTo) { ImGui::OpenPopup(T("Send to device")); openSendTo = false; }
    centerNext(0.4f);
    if (ImGui::BeginPopupModal(T("Send to device"), nullptr, ImGuiWindowFlags_AlwaysAutoResize * !mobile)) {
        int64_t total = 0;
        for (auto& p : pendingSend) total += IsDirectory(p) ? 0 : std::max<int64_t>(0, FileSize(p));
        ImGui::Text(T("%d item(s)%s"), (int)pendingSend.size(), total ? ("  -  " + FormatBytes((double)total)).c_str() : "");
        for (size_t i = 0; i < pendingSend.size() && i < 5; i++) ImGui::BulletText("%s", BaseName(pendingSend[i]).c_str());
        if (pendingSend.size() > 5) ImGui::TextDisabled(T("... and %d more"), (int)pendingSend.size() - 5);
        ImGui::Separator();
        ImGui::TextUnformatted(T("Choose a device:"));
        auto peers = SortedPeers();
        ImGui::BeginChild("pick", ImVec2(mobile ? 0 : ImGui::GetFontSize() * 24, std::min(ImGui::GetFontSize() * 16, vp->WorkSize.y * 0.5f)), ImGuiChildFlags_Borders);
        if (mobile) ui::TouchScroll();
        for (auto& p : peers) {
            const KnownDevice* d = eng.DeviceFor(p);
            if (d && d->blocked) continue;
            if (DevicePeerRow(p, false, mobile ? ui::M().rowH * 1.2f : 0)) {
                eng.SendPaths(p, pendingSend);
                SelectPeer(p);
                if (mobile) { backStack.clear(); backStack.push_back(MScreen::Home); screen = MScreen::Chat; }
                else { tab = Tab::Chat; tabForce = true; }
                pendingSend.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        if (peers.empty()) ImGui::TextDisabled("%s", T("No devices known yet."));
        ImGui::EndChild();
        if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pendingSend.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- select text view
    if (openTextView) { ImGui::OpenPopup(T("Message text")); openTextView = false; }
    centerNext(0.5f);
    if (ImGui::BeginPopupModal(T("Message text"), nullptr, 0)) {
        ImGui::TextDisabled("%s", T("Select any part of the text and press Ctrl+C, or copy everything:"));
        ImGui::InputTextMultiline("##tv", &textViewBuf, ImVec2(-1, ImGui::GetTextLineHeight() * (mobile ? 12 : 10)), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
        if (ImGui::Button(T("Copy all"), ImVec2(0, h))) ImGui::SetClipboardText(textViewBuf.c_str());
        ImGui::SameLine();
        if (ImGui::Button(T("Close"), ImVec2(0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- rename
    if (openRename) { ImGui::OpenPopup(T("Rename device")); openRename = false; }
    centerNext(0.3f);
    if (ImGui::BeginPopupModal(T("Rename device"), nullptr, ImGuiWindowFlags_AlwaysAutoResize * !mobile)) {
        const KnownDevice* d = eng.DeviceFor(renamePeer);
        ImGui::Text(T("Nickname for %s"), d ? d->name.c_str() : renamePeer.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 18);
        bool enter = ImGui::InputTextWithHint("##rn", T("leave empty to use the announced name"), &renameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(T("Save"), ImVec2(0, h)) || enter) { eng.SetCustomName(renamePeer, renameBuf); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- wifi offer
    if (openWifiOffer && !wifiOffers.empty()) { ImGui::OpenPopup(T("Wi-Fi hotspot offered")); openWifiOffer = false; }
    centerNext(0.35f);
    if (ImGui::BeginPopupModal(T("Wi-Fi hotspot offered"), nullptr, ImGuiWindowFlags_AlwaysAutoResize * !mobile)) {
        auto& o = wifiOffers.front();
        ImGui::Text(T("%s shares its hotspot for a faster connection:"), eng.PeerName(o.peer).c_str());
        ImGui::Text(T("Network: %s"), o.ssid.c_str());
        ImGui::Text(T("Password: %s"), o.pass.c_str());
        std::string qr = "WIFI:T:WPA;S:" + o.ssid + ";P:" + o.pass + ";;";
        ui::QrCode(qr.c_str(), ImGui::GetFontSize() * 10);
        if (Bridge().wifiJoin) {
            if (ImGui::Button(T("Join now"), ImVec2(0, h))) { Bridge().wifiJoin(o.ssid, o.pass); wifiOffers.erase(wifiOffers.begin()); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
        } else {
            ImGui::TextDisabled("%s", T("Join this network in your system Wi-Fi settings."));
        }
        if (ImGui::Button(T("Copy password"), ImVec2(0, h))) ImGui::SetClipboardText(o.pass.c_str());
        ImGui::SameLine();
        if (ImGui::Button(T("Close"), ImVec2(0, h))) { wifiOffers.erase(wifiOffers.begin()); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // ---- info
    if (openInfo) { ImGui::OpenPopup(T("LunaShare##info")); openInfo = false; }
    centerNext(0);
    if (ImGui::BeginPopupModal(T("LunaShare##info"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(infoMessage.c_str());
        if (ImGui::Button(T("OK"), ImVec2(mobile ? -1 : ImGui::GetFontSize() * 6, h)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    DrawAvatarEditor();
    DrawAccountPopups();

    // ---- quit
    if (openQuit) { ImGui::OpenPopup(T("Quit LunaShare?")); openQuit = false; }
    centerNext(0);
    if (ImGui::BeginPopupModal(T("Quit LunaShare?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T("Transfers are still running.\nThey will be paused and continue automatically the next time both devices are online."));
        if (ImGui::Button(T("Quit"), ImVec2(0, h))) { quitConfirmed = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(T("Keep running"), ImVec2(0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::DrawToasts() {
    if (toasts.empty()) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    bool mobile = ui::M().mobile;
    float w = mobile ? vp->WorkSize.x * 0.94f : ImGui::GetFontSize() * 22;
    float y = vp->WorkPos.y + vp->WorkSize.y - keyboardInset - (mobile ? ui::M().rowH * 1.6f : ImGui::GetFontSize());
    float dt = ImGui::GetIO().DeltaTime;
    for (int i = (int)toasts.size() - 1; i >= 0; i--) {
        Toast& t = toasts[(size_t)i];
        t.ttl -= dt;
        if (t.ttl <= 0) { toasts.erase(toasts.begin() + i); continue; }
        char id[32];
        snprintf(id, sizeof(id), "##toast%d", i);
        ImGui::SetNextWindowPos(ImVec2(mobile ? vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f : vp->WorkPos.x + vp->WorkSize.x - w - ImGui::GetFontSize(), y), 0, ImVec2(0, 1));
        ImGui::SetNextWindowSize(ImVec2(w, 0));
        ImGui::SetNextWindowBgAlpha(0.95f);
        ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImU32 col = t.type == EventType::Error || t.type == EventType::TransferFailed ? ui::ColErr()
                  : t.type == EventType::TransferDone ? ui::ColOk() : ui::ColAccent();
        ImVec2 p = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 4, p.y + ImGui::GetWindowHeight()), col);
        ImGui::TextUnformatted(Emoji().Display(TDynamic(t.title)).c_str());
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", Emoji().Display(TDynamic(t.text)).c_str());
        ImGui::PopTextWrapPos();
        float bh = mobile ? ui::M().rowH * 0.8f : 0;
        if (t.type == EventType::IncomingOffer) {
            auto it = eng.transfers.find(t.tid);
            if (it != eng.transfers.end() && it->second->state == TState::Pending) {
                if (ImGui::Button(T("Accept"), ImVec2(0, bh))) { eng.AcceptBatch(it->second->batch); t.ttl = 0; }
                ImGui::SameLine();
                if (ImGui::Button(T("Decline"), ImVec2(0, bh))) {
                    for (auto& x : eng.BatchTransfers(it->second->batch)) if (x->state == TState::Pending) eng.Reject(x->tid);
                    t.ttl = 0;
                }
                ImGui::SameLine();
            } else {
                t.ttl = std::min(t.ttl, 1.0f);
            }
        }
        if (mobile && t.type != EventType::IncomingOffer) {
            if (ImGui::IsWindowHovered() && ImGui::IsMouseReleased(0) && ImGui::GetMouseDragDelta(0).y == 0) {
                if (!t.peer.empty()) {
                    SelectPeer(t.peer);
                    backStack.clear();
                    backStack.push_back(MScreen::Home);
                    screen = MScreen::Chat;
                }
                t.ttl = 0;
            }
        } else if (!t.peer.empty() && t.type != EventType::Error) {
            if (ImGui::Button(T("Open chat"), ImVec2(0, bh))) {
                SelectPeer(t.peer);
                if (mobile) { backStack.clear(); backStack.push_back(MScreen::Home); screen = MScreen::Chat; }
                else { tab = Tab::Chat; tabForce = true; }
                t.ttl = 0;
            }
            ImGui::SameLine();
        }
        if ((!mobile || t.type == EventType::IncomingOffer) && ImGui::Button(T("Dismiss"), ImVec2(0, bh))) t.ttl = 0;
        if (ImGui::IsWindowHovered()) t.ttl = std::max(t.ttl, 1.5f);
        y -= ImGui::GetWindowHeight() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::End();
    }
}

// ============================================================================ calls

bool App::CallButton(const std::string& peer, float size, bool video) {
    const Group* grp = eng.GroupOf(peer);
    bool connected = grp ? !grp->left : eng.IsConnected(peer);
    bool capable = grp ? connected : connected && eng.PeerHasCap(peer, video ? "video" : "voice");
    bool busy = eng.call.state != CallState::None;
    const char* tip = !connected ? (video ? T("Video call (device is offline)") : T("Voice call (device is offline)"))
                    : !capable ? (video ? T("Video call - the other device does not have the video module")
                                        : T("Voice call - the other device does not have the voice module"))
                    : busy ? T("Already in a call") : grp ? (video ? T("Group video call") : T("Group voice call"))
                    : video ? T("Video call") : T("Voice call");
    ImGui::BeginDisabled(!capable || busy);
    // green when a call can be made: the usual colour for "call"
    bool green = capable && !busy;
    if (green) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 140, 85, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(55, 170, 105, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(35, 120, 75, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_WHITE);
    }
    bool clicked = ui::IconButton(video ? "vcall" : "call", video ? Icon::Video : Icon::Phone, tip, size);
    if (green) ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    if (clicked) {
        Media().WantCameraAtStart(video);
        eng.StartCall(peer, video ? "video" : "voice");
    }
    return clicked;
}

bool App::WebcamButtonShown(const std::string& peer) const {
    if (!Media().VideoAvailable()) return false;
    if (Media().VcamOn()) return true;  // this PC has LunaShare Camera: ask the other device for its camera
    // a phone offering its camera to a Windows PC
    const KnownDevice* d = const_cast<ls::Engine&>(eng).DeviceFor(peer);
    return IsMobilePlatform() && d && (d->platform.find("Windows") != std::string::npos || d->platform.find("Linux") != std::string::npos);
}

bool App::WebcamButton(const std::string& peer, float size) {
    bool connected = eng.IsConnected(peer);
    bool capable = connected && eng.PeerHasCap(peer, "video");
    bool busy = eng.call.state != CallState::None;
    bool pc = Media().VcamOn();
    const char* tip = !connected ? T("Webcam (device is offline)")
                    : !capable ? T("Webcam - the other device does not have the video module")
                    : busy ? T("Already in a call")
                    : pc ? T("Use this device's camera as a webcam (LunaShare Camera)") : T("Send my camera to this PC as a webcam");
    ImGui::BeginDisabled(!capable || busy);
    bool clicked = ui::IconButton("webcam", Icon::Webcam, tip, size);
    ImGui::EndDisabled();
    if (clicked) {
        Media().WantCameraAtStart(!pc);  // the side without the virtual camera sends its picture
        eng.StartCall(peer, pc ? "webcam" : "webcam-offer");
    }
    return clicked;
}

bool App::VideoVisible() const {
    if (!Media().VideoRunning()) return false;
    const auto& r = Media().RemoteVideo();
    return Media().CameraOn() || (Media().RemoteCameraOn() && r.valid);
}

int App::CallButtonCount(bool withVideo) const {
    int n = 2;  // mute, hang up
    if (eng.call.state != CallState::Active) return 1;
    if (IsMobilePlatform()) n++;  // audio output
    if (withVideo && Media().VideoRunning()) {
        n++;                                   // camera on/off
        if (Media().CameraOn()) n++;           // flip
    }
    if (Media().Available()) n++;              // call settings (sound, camera)
    return n;
}

void App::DrawCallButtons(float bw, float h, bool withVideo) {
    const CallInfo& c = eng.call;
    if (c.state == CallState::Active) {
        bool muted = Media().Muted();
        if (muted) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(170, 60, 60, 255));
        const bool cap = ui::M().mobile;  // phones: a word under each call button
        if (ui::IconButton("mute", muted ? Icon::MicOff : Icon::Mic, muted ? T("Unmute") : T("Mute"), bw, false, cap ? (muted ? T("Unmute##cap") : T("Mute##cap")) : nullptr))
            Media().SetMute(!muted);
        if (muted) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (withVideo && Media().VideoRunning()) {
            bool on = Media().CameraOn();
            if (!on) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(170, 60, 60, 255));
            if (ui::IconButton("cam", on ? Icon::Video : Icon::VideoOff, on ? T("Turn camera off") : T("Turn camera on"), bw, false,
                               cap ? T("Camera##cap") : nullptr)) {
                Media().SetCameraOn(!on);
                if (!on && ui::M().mobile) videoFull = true;
            }
            if (!on) ImGui::PopStyleColor();
            ImGui::SameLine();
            if (on) {
                if (ui::IconButton("flip", Icon::FlipCamera, T("Switch camera"), bw, false, cap ? T("Switch##cap") : nullptr)) Media().NextCamera();
                ImGui::SameLine();
            }
        }
        if (Media().Available()) {
            if (camPanel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            bool wasOpen = camPanel;
            if (ui::IconButton("camset", Icon::Sliders, T("Call settings (sound, camera, quality...)"), bw, false, cap ? T("Settings##cap") : nullptr))
                camPanel = !camPanel;
            if (wasOpen) ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        if (IsMobilePlatform()) {
            // Audio output: with only earpiece + loudspeaker it's a toggle; with a headset connected it opens a list.
            const MediaHost::Route* cur = Media().Current();
            bool spk = cur && cur->kind == "speaker";
            bool hl = cur && cur->kind != "earpiece";
            if (hl) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ui::IconButton("speaker", Icon::Speaker, cur ? MediaHost::RouteLabel(*cur).c_str() : T("Audio output"), bw, false,
                               cap ? (spk ? T("Speaker##cap") : T("Sound##cap")) : nullptr)) {
                Media().RefreshRoutes();
                auto& rs = Media().Routes();
                if (rs.size() > 2) {
                    ImGui::OpenPopup("##callroute");
                } else {
                    for (auto& r : rs)
                        if ((r.kind == "speaker") != spk) { Media().SetRoute(r.id); break; }
                }
            }
            if (hl) ImGui::PopStyleColor();
            if (ImGui::BeginPopup("##callroute")) {
                for (auto& r : Media().Routes()) {
                    if (ImGui::Selectable(MediaHost::RouteLabel(r).c_str(), r.id == Media().CurrentRoute(), 0, ImVec2(ImGui::GetFontSize() * 12, h * 0.9f)))
                        Media().SetRoute(r.id);
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 50, 50, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(230, 70, 70, 255));
    if (ui::IconButton("hangup", Icon::HangUp, T("Hang up"), bw, false, ui::M().mobile ? T("End##cap") : nullptr)) eng.EndCall();
    ImGui::PopStyleColor(2);
}

void App::DrawCallBar() {
    const CallInfo& c = eng.call;
    if (c.state == CallState::None || c.state == CallState::IncomingRinging) return;
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH : ImGui::GetFrameHeight() * 1.2f;
    bool phoneVideo = mobile && VideoVisible();
    int nb = CallButtonCount(true) + (phoneVideo && !videoFull ? 1 : 0);
    // phones: the buttons get their own row under the status once there are more than two, so they always fit
    bool twoRows = mobile && nb >= 2;
    float barH = twoRows ? h * 2 + ImGui::GetStyle().ItemSpacing.y : h;
    bool groupCall = !c.group.empty();
    bool strip = groupCall && c.state == CallState::Active && c.media != "video";  // (video: in the picture)
    float stripTile = mobile ? ui::M().rowH * 1.1f : ImGui::GetFontSize() * 2.8f;
    float stripH = strip ? stripTile + ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y * 2 : 0;
    barH += stripH;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c.state == CallState::Active ? IM_COL32(30, 110, 70, 255) : IM_COL32(60, 70, 120, 255));
    ImGui::BeginChild("callbar", ImVec2(0, barH + ImGui::GetStyle().WindowPadding.y * 2), ImGuiChildFlags_AlwaysUseWindowPadding);
    std::string who = Emoji().Display(eng.PeerName(c.peer));
    char status[160];
    if (c.state == CallState::Active && groupCall) {
        int secs = (int)((SteadyMs() - c.answered) / 1000);
        snprintf(status, sizeof(status), "%s  %d:%02d%s", who.c_str(), secs / 60, secs % 60, c.hub ? T("  through the server") : "");
    } else if (c.state == CallState::Active) {
        int secs = (int)((SteadyMs() - c.answered) / 1000);
        auto conn = eng.ReadyConn(c.peer);
        snprintf(status, sizeof(status), "%s  %d:%02d%s", who.c_str(), secs / 60, secs % 60, conn ? ("  " + TF("via %s", T(TransportName(conn->transport)))).c_str() : "");
    } else {
        snprintf(status, sizeof(status), "Calling %s...", who.c_str());
    }
    ImGui::AlignTextToFramePadding();
    float ih = ImGui::GetTextLineHeight();
    ImVec2 ip = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(ih, h));
    ui::DrawIcon(ImGui::GetWindowDrawList(), Icon::Phone, ImVec2(ip.x + ih * 0.5f, ip.y + h * 0.5f), ih, IM_COL32_WHITE);
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextUnformatted(status);
    if (c.state == CallState::Active) {
        LsmStats st = Media().Stats();
        if (!mobile) {
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextColored(ImVec4(0.85f, 0.95f, 0.9f, 1), "mic");
            ImGui::SameLine();
            ui::ProgressBarText(Media().Muted() ? 0.f : st.mic_level, "", ImVec2(ImGui::GetFontSize() * 4, ImGui::GetTextLineHeight() * 0.5f), IM_COL32(150, 240, 170, 255));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.85f, 0.95f, 0.9f, 1), "spk");
            ImGui::SameLine();
            ui::ProgressBarText(st.speaker_level, "", ImVec2(ImGui::GetFontSize() * 4, ImGui::GetTextLineHeight() * 0.5f), IM_COL32(150, 240, 170, 255));
        }
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !mobile) {
            std::string vid;
            if (Media().VideoRunning()) {
                LsvStats vs = Media().VideoStats();
                char b[320];
                snprintf(b, sizeof(b), "\nVideo out: %s%dx%d %d fps %d kbps  in: %dx%d %d fps %d kbps\n%s / %s", Media().CameraOn() ? "" : "(camera off) ",
                         vs.send_width, vs.send_height, vs.send_fps, vs.send_kbps, vs.recv_width, vs.recv_height, vs.recv_fps, vs.recv_kbps,
                         vs.encoder[0] ? vs.encoder : "-", vs.decoder[0] ? vs.decoder : "-");
                vid = b;
            }
            ImGui::SetTooltip(T("Packet loss %.1f%%  jitter %d ms  delay %d ms  %d kbps\nsent %d / received %d packets%s%s%s"), st.loss_percent,
                              st.jitter_ms, st.buffered_ms, st.bitrate_kbps, st.packets_out, st.packets_in, vid.c_str(),
                              Media().audioError.empty() ? "" : "\n", Media().audioError.c_str());
        }
    }
    // buttons on the right, shrunk if needed so all of them fit the width
    float sp = ImGui::GetStyle().ItemSpacing.x;
    float rowW = ImGui::GetContentRegionMax().x - ImGui::GetStyle().WindowPadding.x;
    float bw = std::min(h, (rowW - sp * (float)(nb - 1)) / (float)nb);
    if (twoRows) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - bw * (float)nb - sp * (float)(nb - 1),
                                   ImGui::GetStyle().WindowPadding.y + h + ImGui::GetStyle().ItemSpacing.y + (h - bw) * 0.5f));
    } else {
        ImGui::SameLine(ImGui::GetContentRegionMax().x - bw * (float)nb - sp * (float)(nb - 1));
        ImGui::SetCursorPosY(ImGui::GetStyle().WindowPadding.y + (h - bw) * 0.5f);
    }
    if (phoneVideo && !videoFull) {
        if (ui::IconButton("vfull", Icon::Expand, T("Show video"), bw)) videoFull = true;
        ImGui::SameLine();
    }
    DrawCallButtons(bw, h, true);
    if (strip) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, ImGui::GetStyle().WindowPadding.y + barH - stripH + ImGui::GetStyle().ItemSpacing.y));
        GroupCallStrip(stripTile, false);
    }
    if (!Media().audioError.empty() && c.state == CallState::Active) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.6f, 1), "%s", Media().audioError.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- video
void App::DrawVideoStage(ImVec2 size, bool phone) {
    // Desktop: a child window inside the chat. Phone: drawn straight into the full-screen call window, because the
    // call buttons are placed on top of it in that window, and a child window would take their touches.
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (!phone) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(12, 12, 14, 255));
        ImGui::BeginChild("videostage", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        p0 = ImGui::GetWindowPos();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const auto& rv = Media().RemoteVideo();
    const auto& lv = Media().LocalVideo();
    int64_t nowMs = (int64_t)SDL_GetTicks64();
    bool remote = Media().RemoteCameraOn() && rv.valid;
    bool local = Media().CameraOn() && lv.valid && nowMs - lv.lastMs < 1500;  // stale = no self-view stream (120 fps)
    std::string who = Emoji().Display(eng.PeerName(eng.call.peer));
    bool groupCall = !eng.call.group.empty();
    if (groupCall) {  // the person whose video is shown
        who.clear();
        for (auto& p : eng.call.peers)
            if (p.dev == eng.call.spotlight) who = CallPeerName(p);
        if (eng.call.peers.empty()) remote = false;
    }
    if (remote) {
        Media().DrawVideo(dl, rv, p0, p1, false, Media().RemoteRotation());
    } else if (local) {
        Media().DrawVideo(dl, lv, p0, p1, false);  // nothing from them: show yourself big
    }
    if (!remote && !(groupCall && who.empty())) {
        // their camera is off: initial + hint
        ImVec2 c((p0.x + p1.x) * 0.5f, p0.y + size.y * (local ? 0.15f : 0.42f));
        float r = std::min(size.x, size.y) * (local ? 0.07f : 0.13f);
        dl->AddCircleFilled(c, r, IM_COL32(60, 90, 140, local ? 200 : 255));
        std::string ini = who.empty() ? "?" : who.substr(0, 1);
        ImFont* f = ImGui::GetFont();
        float fs = r * 1.1f;
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, ini.c_str());
        dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32_WHITE, ini.c_str());
        std::string msg = Media().RemoteCameraOn() ? TF("Waiting for %s's video...", who.c_str()) : TF("%s's camera is off", who.c_str());
        ImVec2 ms = ImGui::CalcTextSize(msg.c_str());
        dl->AddText(ImVec2(c.x - ms.x * 0.5f, c.y + r + ImGui::GetStyle().ItemSpacing.y), IM_COL32(220, 220, 220, 255), msg.c_str());
    }
    if (remote && !local && Media().CameraOn()) {
        const char* t = eng.settings.callVideoFps > 60 ? T("No self-view at 120 fps") : T("Starting camera...");
        ImVec2 ts = ImGui::CalcTextSize(t);
        float m = ImGui::GetStyle().ItemSpacing.x * 1.5f;
        ImVec2 a(p1.x - ts.x - m * 3, p1.y - ts.y - m * 3 - (phone ? ui::M().rowH * 1.6f : 0));
        dl->AddRectFilled(a, ImVec2(a.x + ts.x + m * 2, a.y + ts.y + m * 2), IM_COL32(0, 0, 0, 150), 6);
        dl->AddText(ImVec2(a.x + m, a.y + m), IM_COL32(230, 230, 230, 255), t);
    }
    if (remote && local) {
        // self-view in the corner
        float pw = size.x * (phone ? 0.3f : 0.22f);
        float ph = pw * (float)lv.h / std::max(1, lv.w);
        if (ph > size.y * 0.4f) { ph = size.y * 0.4f; pw = ph * (float)lv.w / std::max(1, lv.h); }
        float m = ImGui::GetStyle().ItemSpacing.x * 1.5f;
        ImVec2 a(p1.x - pw - m, p1.y - ph - m - (phone ? ui::M().rowH * 1.6f : 0)), b(a.x + pw, a.y + ph);
        dl->AddRectFilled(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), IM_COL32(255, 255, 255, 90), 6);
        Media().DrawVideo(dl, lv, a, b, true);
    }
    if (groupCall) {  // everyone in the call along the top: tap to watch someone else
        float tile = phone ? ui::M().rowH * 1.1f : ImGui::GetFontSize() * 2.6f;
        float m = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorScreenPos(ImVec2(p0.x + m, p0.y + m + (phone ? ui::M().rowH + m : 0)));
        GroupCallStrip(tile, true);
    }
    {
        // corner buttons: turn their picture (phones lying sideways...) and, on desktop, bigger / smaller
        float bw = phone ? ui::M().rowH : ImGui::GetFrameHeight();
        float sp = ImGui::GetStyle().ItemSpacing.x;
        int n = (remote ? 2 : 0) + (phone ? 0 : 1);
        ImGui::SetCursorScreenPos(ImVec2(p1.x - (bw + sp) * (float)n, p0.y + ImGui::GetStyle().ItemSpacing.y + (phone ? bw + sp : 0)));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 110));
        if (remote) {
            if (ui::IconButton("rotl", Icon::RotateLeft, T("Turn their picture left"), bw)) Media().RotateRemote(-1);
            ImGui::SameLine();
            if (ui::IconButton("rotr", Icon::RotateRight, T("Turn their picture right"), bw)) Media().RotateRemote(1);
            if (!phone) ImGui::SameLine();
        }
        if (!phone && ui::IconButton("vbig", Icon::Expand, videoBig ? T("Smaller video") : T("Bigger video"), bw)) videoBig = !videoBig;
        ImGui::PopStyleColor();
    }
    if (!phone) {
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

void App::DrawVideoOverlay() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 12, 14, 255));
    ImGui::Begin("##videocall", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                             ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    // keep the call above the app - but never above its own dropdowns / popups (they'd end up hidden behind it)
    bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!camPanel && !popupOpen) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImVec2 full = ImGui::GetContentRegionAvail();
    ImVec2 top = ImGui::GetCursorPos();
    DrawVideoStage(full, true);
    // top: back to chat + name + timer
    float bw = ui::M().rowH;
    float pad = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPos(ImVec2(top.x + pad, top.y + pad));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 110));
    if (ui::IconButton("vback", Icon::Back, T("Back to chat"), bw)) videoFull = false;
    ImGui::SameLine();
    int secs = (int)((SteadyMs() - eng.call.answered) / 1000);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (bw - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::Text("%s  %d:%02d", Emoji().Display(eng.PeerName(eng.call.peer)).c_str(), secs / 60, secs % 60);
    // bottom: call buttons, centered
    int nb = CallButtonCount(true);
    float bb = std::min(bw * 1.05f, (full.x - pad * (float)(nb + 1)) / (float)nb);  // always fits the screen width
    float rowW = bb * (float)nb + pad * (float)(nb - 1);
    ImGui::SetCursorPos(ImVec2(top.x + (full.x - rowW) * 0.5f, top.y + full.y - bb - pad * 2));
    DrawCallButtons(bb, bb, true);
    ImGui::PopStyleColor();
    ImGui::End();
}

bool App::VideoQualityControls(Settings& s, float w, float h) {
    bool changed = false;
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##vres", (std::string(T("Video: ")) + MediaHost::ResName(s.callVideoRes)).c_str())) {
        for (int r = 0; r < MediaHost::kResCount; r++)
            if (ImGui::Selectable(MediaHost::ResName(r), r == s.callVideoRes, 0, ImVec2(0, h))) {
                s.callVideoRes = r;
                changed = true;
            }
        ImGui::EndCombo();
    }
    int nf = 0;
    const int* fl = MediaHost::FpsList(&nf);
    char cur[32];
    snprintf(cur, sizeof(cur), "Frame rate: %d fps", s.callVideoFps);
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##vfps", cur)) {
        for (int i = 0; i < nf; i++) {
            char l[16];
            snprintf(l, sizeof(l), "%d fps", fl[i]);
            if (ImGui::Selectable(l, fl[i] == s.callVideoFps, 0, ImVec2(0, h))) {
                s.callVideoFps = fl[i];
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    LsvSettings p = MediaHost::Preset(s.callVideoRes, s.callVideoFps, true);
    ImGui::TextDisabled(T("Up to about %s.%s"), MediaHost::RateText(p.bitrate).c_str(),
                        p.bitrate > 20000000 ? T(" Needs fast Wi-Fi or a cable.") : p.height >= 1080 || p.fps >= 60 ? T(" Best on Wi-Fi.") : "");
    if (s.callVideoFps > 60 || s.callVideoRes >= 4)
        ImGui::TextDisabled("%s", T("If a camera can't do this, the closest it can do is sent."));
    if (s.callVideoRes >= 6)
        ImGui::TextDisabled("%s", T("8K in H.264: if the receiving device can't decode it, it asks for 4K instead."));
    return changed;
}

void App::DrawCameraPanel() {
    bool mobile = ui::M().mobile;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (mobile) {
        // leave the call buttons at the bottom of the video view visible (hang up!)
        float buttons = ui::M().rowH * 1.05f + ImGui::GetStyle().ItemSpacing.x * 3;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y * 0.3f));
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y * 0.7f - buttons));
        ImGui::SetNextWindowBgAlpha(0.92f);
    } else {
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - ImGui::GetFontSize() * 25, vp->WorkPos.y + ImGui::GetFontSize() * 5),
                                ImGuiCond_Appearing);
    }
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoCollapse | (mobile ? ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize : ImGuiWindowFlags_AlwaysAutoResize);
    if (!ImGui::Begin((std::string(T("Call settings")) + "###callpanel").c_str(), &camPanel, fl)) {
        ImGui::End();
        return;
    }
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());  // not over its own dropdowns (white balance, size...)
    if (mobile) ui::TouchScroll();
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    float w = ImGui::GetContentRegionAvail().x;
    // sound: the same settings as in Settings > Calls, applied right away
    ImGui::SeparatorText(T("Sound"));
    bool audioChanged = false;
    AudioCallControls(w - ImGui::GetFontSize() * 2, h, audioChanged);
    if (audioChanged) {
        eng.ApplySettings(edit);
        editDirty = false;
    }
    LsmStats ast = Media().Stats();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(T("Voice: %d kbps, %.1f%% lost, %d ms delay"), ast.bitrate_kbps, ast.loss_percent, ast.buffered_ms);
    ImGui::PopTextWrapPos();
    if (!Media().CameraOn()) {
        if (mobile && ImGui::Button(T("Close"), ImVec2(w, h))) camPanel = false;
        ImGui::End();
        return;
    }
    ImGui::SeparatorText(T("Video"));
    double now = ImGui::GetTime();
    if (now - camCtlTime > 0.5) {  // re-read now and then (the camera may change values itself)
        camCtl = Media().Controls();
        camCtlTime = now;
    }
    auto find = [&](int id) -> LsvControl* {
        for (auto& c : camCtl) if (c.id == id) return &c;
        return nullptr;
    };
    auto set = [&](LsvControl* c, float v) {
        c->value = v;
        Media().SetControl(c->id, v);
        camCtlTime = now;  // our value stands for a moment
    };
    // camera + quality
    auto cams = Media().Cameras();
    if (cams.size() > 1) {
        ImGui::SetNextItemWidth(w);
        std::string cur = Media().CurrentCameraName();
        if (ImGui::BeginCombo("##camsel", cur.c_str())) {
            for (auto& c : cams)
                if (ImGui::Selectable(c.name, false, 0, ImVec2(0, h))) {
                    Media().SelectCamera(c.id);
                    camCtlTime = -1;
                }
            ImGui::EndCombo();
        }
    }
    Settings s = eng.GetSettings();
    if (VideoQualityControls(s, w, h)) {
        eng.ApplySettings(s);
        edit.callVideoRes = s.callVideoRes;
        edit.callVideoFps = s.callVideoFps;
        Media().ApplyVideoQuality();
        camCtlTime = -1;
    }
    if (ImGui::Checkbox(T("Lower quality when slow"), &s.callVideoAdaptive)) {
        eng.ApplySettings(s);
        edit.callVideoAdaptive = s.callVideoAdaptive;
        Media().ApplyVideoQuality();
    }
    if (ImGui::Checkbox(T("My picture in full quality"), &s.callFullPreview)) {
        eng.ApplySettings(s);
        edit.callFullPreview = s.callFullPreview;
        Media().RestartCamera();  // the self-view size is chosen when the camera starts
        camCtlTime = -1;
    }
    ImGui::Separator();
    if (camCtl.empty()) ImGui::TextDisabled("%s", T("This camera has no adjustable settings."));
    if (LsvControl* z = find(LSV_CTL_ZOOM)) {
        float v = z->value;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("##zoom", &v, z->min, z->max, T("Zoom %.1fx"), ImGuiSliderFlags_Logarithmic)) set(z, v);
        // quick zoom steps that the camera supports (0.6x = ultra-wide on many phones)
        const float steps[] = {0.6f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f};
        bool first = true;
        for (float st : steps) {
            if (st < z->min - 0.01f || st > z->max + 0.01f) continue;
            if (!first) ImGui::SameLine();
            first = false;
            char lbl[16];
            snprintf(lbl, sizeof(lbl), st < 1 ? "%.1fx" : "%.0fx", st);
            if (ImGui::Button(lbl, ImVec2(0, h))) set(z, st);
        }
    }
    LsvControl* ae = find(LSV_CTL_AUTO_EXPOSURE);
    bool aeAuto = !ae || ae->value > 0.5f;
    if (ae) {
        bool v = aeAuto;
        if (ImGui::Checkbox(T("Automatic exposure"), &v)) set(ae, v ? 1.f : 0.f);
        aeAuto = v;
    }
    if (LsvControl* ev = find(LSV_CTL_EXPOSURE_COMP); ev && aeAuto) {
        float v = ev->value;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("##ev", &v, ev->min, ev->max, T("Exposure %+.1f EV"))) set(ev, v);
    }
    if (!aeAuto) {
        if (LsvControl* iso = find(LSV_CTL_ISO)) {
            float v = iso->value;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##iso", &v, iso->min, iso->max, T("ISO %.0f"), ImGuiSliderFlags_Logarithmic)) set(iso, v);
        }
        if (LsvControl* sh = find(LSV_CTL_SHUTTER)) {
            float v = sh->value;
            char fmt[48];
            snprintf(fmt, sizeof(fmt), v > 0 ? "Shutter 1/%.0f s (%%.2f ms)" : "Shutter %%.2f ms", v > 0 ? 1000.0 / v : 0.0);
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##shutter", &v, std::max(0.01f, sh->min), sh->max, fmt, ImGuiSliderFlags_Logarithmic)) set(sh, v);
        }
    }
    LsvControl* af = find(LSV_CTL_AUTO_FOCUS);
    bool afAuto = !af || af->value > 0.5f;
    if (af) {
        bool v = afAuto;
        if (ImGui::Checkbox(T("Automatic focus"), &v)) set(af, v ? 1.f : 0.f);
        afAuto = v;
    }
    if (LsvControl* fo = find(LSV_CTL_FOCUS); fo && !afAuto) {
        float v = fo->value;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("##focus", &v, fo->min, fo->max, T("Focus %.1f"))) set(fo, v);
        if (mobile) ImGui::TextDisabled("%s", T("Left = far away, right = close up"));
    }
    if (LsvControl* wb = find(LSV_CTL_WHITE_BALANCE)) {
        static const char* names[] = {"Auto", "Daylight", "Cloudy", "Shade", "Tungsten (indoor lamps)", "Fluorescent"};
        int v = std::clamp((int)std::lround(wb->value), 0, 5);
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("##wb", (std::string(T("White balance: ")) + T(names[v])).c_str())) {
            for (int i = 0; i <= std::min(5, (int)wb->max); i++)
                if (ImGui::Selectable(T(names[i]), i == v, 0, ImVec2(0, h))) set(wb, (float)i);
            ImGui::EndCombo();
        }
    }
    if (LsvControl* br = find(LSV_CTL_BRIGHTNESS)) {
        float v = br->value;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("##bright", &v, br->min, br->max, T("Brightness %.0f"))) set(br, v);
    }
    if (LsvControl* t = find(LSV_CTL_TORCH)) {
        bool v = t->value > 0.5f;
        if (ImGui::Checkbox(T("Flashlight"), &v)) set(t, v ? 1.f : 0.f);
    }
    if (LsvControl* st = find(LSV_CTL_STABILIZATION)) {
        bool v = st->value > 0.5f;
        if (ImGui::Checkbox(T("Stabilization"), &v)) set(st, v ? 1.f : 0.f);
    }
    ImGui::Separator();
    LsvStats vs = Media().VideoStats();
    ImGui::TextDisabled(T("Sending %dx%d, %d fps, %d kbps"), vs.send_width, vs.send_height, vs.send_fps, vs.send_kbps);
    if (vs.encoder[0]) ImGui::TextDisabled(T("Encoder: %s"), vs.encoder);
    if (vs.recv_width) ImGui::TextDisabled(T("Receiving %dx%d, %d fps, %d kbps"), vs.recv_width, vs.recv_height, vs.recv_fps, vs.recv_kbps);
    if (mobile && ImGui::Button(T("Close"), ImVec2(w, h))) camPanel = false;
    ImGui::End();
}

void App::DrawIncomingCall() {
    bool ringing = eng.call.state == CallState::IncomingRinging;
    if (openIncomingCall && ringing) ImGui::OpenPopup(T("Incoming call"));
    openIncomingCall = false;
    bool mobile = ui::M().mobile;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (mobile) {
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
    } else {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.4f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 22, 0));
    }
    if (!ImGui::BeginPopupModal(T("Incoming call"), nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                              (mobile ? 0 : ImGuiWindowFlags_AlwaysAutoResize)))
        return;
    if (!ringing) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    std::string who = Emoji().Display(eng.PeerName(eng.call.peer));
    float avail = ImGui::GetContentRegionAvail().x;
    if (mobile) ImGui::Dummy(ImVec2(0, vp->WorkSize.y * 0.2f));
    // pulsing phone icon
    float sz = ImGui::GetFontSize() * (mobile ? 5.0f : 3.0f);
    float pulse = 0.85f + 0.15f * sinf((float)ImGui::GetTime() * 6.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(avail, sz * 1.3f));
    ImVec2 cc(p.x + avail * 0.5f, p.y + sz * 0.65f);
    if (!eng.AvatarPath(eng.call.peer).empty()) {  // their picture with a pulsing ring and a small phone badge
        ImGui::GetWindowDrawList()->AddCircle(cc, sz * 0.64f * pulse, IM_COL32(40, 150, 90, 255), 64, sz * 0.06f);
        DrawAvatar(ImGui::GetWindowDrawList(), cc, sz * 1.1f, eng.call.peer, who);
        ImVec2 bc(cc.x + sz * 0.42f, cc.y + sz * 0.42f);
        ImGui::GetWindowDrawList()->AddCircleFilled(bc, sz * 0.2f, IM_COL32(40, 150, 90, 255));
        ui::DrawIcon(ImGui::GetWindowDrawList(), Icon::Phone, bc, sz * 0.2f, IM_COL32_WHITE);
    } else {
        ImGui::GetWindowDrawList()->AddCircleFilled(cc, sz * 0.6f * pulse, IM_COL32(40, 150, 90, 255));
        ui::DrawIcon(ImGui::GetWindowDrawList(), Icon::Phone, cc, sz * 0.6f, IM_COL32_WHITE);
    }
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    ImVec2 ts = ImGui::CalcTextSize(who.c_str());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - ts.x) * 0.5f));
    ImGui::TextUnformatted(who.c_str());
    ImGui::PopFont();
    bool videoCall = eng.call.media == "video" && Media().VideoAvailable();
    bool webcamAsk = eng.call.media == "webcam", webcamOffer = eng.call.media == "webcam-offer";
    const char* sub = webcamAsk ? T("wants to use your camera as a webcam") : webcamOffer ? T("offers its camera as a webcam")
                    : videoCall ? T("is video calling you") : T("is calling you");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - ImGui::CalcTextSize(sub).x) * 0.5f));
    ImGui::TextDisabled("%s", sub);
    ImGui::Spacing();
    if (mobile) ImGui::Dummy(ImVec2(0, vp->WorkSize.y * 0.15f));
    float bh = mobile ? ui::M().rowH * 1.3f : ImGui::GetFrameHeight() * 1.6f;
    int nbtn = videoCall && !webcamAsk && !webcamOffer ? 3 : 2;
    float bw = (avail - ImGui::GetStyle().ItemSpacing.x * (float)(nbtn - 1)) / (float)nbtn;
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(190, 50, 50, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220, 70, 70, 255));
    if (ui::IconTextButton(T("Decline"), Icon::HangUp, ImVec2(bw, bh)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) eng.EndCall();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 150, 90, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 180, 110, 255));
    bool accept = false;
    if (webcamAsk || webcamOffer) {
        if (ui::IconTextButton(webcamAsk ? T("Allow") : T("Accept"), Icon::Webcam, ImVec2(bw, bh))) {
            Media().WantCameraAtStart(webcamAsk);  // asked: we send our camera; offered: we only receive
            accept = true;
        }
    } else if (videoCall) {
        if (ui::IconTextButton(mobile ? T("Voice") : T("Voice only"), Icon::Phone, ImVec2(bw, bh))) {
            Media().WantCameraAtStart(false);
            accept = true;
        }
        ImGui::SameLine();
        if (ui::IconTextButton(T("Video"), Icon::Video, ImVec2(bw, bh))) {
            Media().WantCameraAtStart(true);
            accept = true;
        }
    } else if (ui::IconTextButton(T("Answer"), Icon::Phone, ImVec2(bw, bh))) {
        Media().WantCameraAtStart(false);
        accept = true;
    }
    if (accept) {
        eng.AcceptCall();
        SelectPeer(eng.call.peer);
        if (mobile) { backStack = {MScreen::Home}; screen = MScreen::Chat; }
        else { tab = Tab::Chat; tabForce = true; }
    }
    ImGui::PopStyleColor(2);
    ImGui::EndPopup();
}

// Microphone / speaker and voice processing: in Settings and in the call settings panel (applied live during calls)
void App::AudioCallControls(float fieldW, float h, bool& changed) {
    auto deviceCombo = [&](const char* label, bool capture, std::string& value) {
        static std::vector<std::string> devs[2];
        static double lastScan[2] = {-100, -100};
        if (ImGui::GetTime() - lastScan[capture] > 5) {
            devs[capture] = MediaHost::Devices(capture);
            lastScan[capture] = ImGui::GetTime();
        }
        ImGui::SetNextItemWidth(fieldW);
        std::string preview = std::string(T(label)) + ": " + (value.empty() ? std::string(T("system default")) : value);
        if (ImGui::BeginCombo((std::string("##") + label).c_str(), preview.c_str())) {
            if (ImGui::Selectable(T("System default"), value.empty(), 0, ImVec2(0, h))) { value.clear(); changed = true; }
            for (auto& d : devs[capture])
                if (ImGui::Selectable(d.c_str(), value == d, 0, ImVec2(0, h))) { value = d; changed = true; }
            ImGui::EndCombo();
        }
    };
    deviceCombo("Microphone", true, edit.callInputDevice);
    deviceCombo("Speaker", false, edit.callOutputDevice);
    changed |= ImGui::Checkbox(T("Echo cancellation"), &edit.callEchoCancel);
    ui::HelpMarker(T("Removes your speaker's sound from the microphone so the other side does not hear themselves. Keep it on unless you use headphones."));
    changed |= ImGui::Checkbox(T("Noise suppression"), &edit.callNoiseSuppress);
    changed |= ImGui::Checkbox(T("Automatic microphone level"), &edit.callAutoGain);
    int kbps = edit.callBitrate / 1000;
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::SliderInt("##callbr", &kbps, 16, 128, T("Voice quality: %d kbps"))) { edit.callBitrate = kbps * 1000; editDirty = true; }
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::SliderInt("##micboost", &edit.callMicBoost, 0, 24, T("Microphone boost: +%d dB"))) editDirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    ui::HelpMarker(T("Makes a quiet microphone louder. Use the microphone test to check: normal speech should fill about two thirds of the bar."));
}

// The invites from the sidebar (clicking my own name): the same as the top of the settings
void App::DrawInvitesPopup() {
    static bool invEnv = getenv("LUNASHARE_OPENINVITES") != nullptr;  // dev: screenshots
    if (invEnv && ImGui::GetFrameCount() == 30) ImGui::OpenPopup("##invites");
    bool mobile = ui::M().mobile;
    ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(mobile ? ImVec2(vs.x * 0.96f, 0) : ImVec2(ImGui::GetFontSize() * 30, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(vs.x, vs.y * 0.9f));
    if (!ImGui::BeginPopupModal("##invites", nullptr, ImGuiWindowFlags_NoTitleBar)) return;
    float h = mobile ? ui::M().rowH : 0;
    bool changed = false;
    DrawPrivateContacts(mobile ? -1 : ImGui::GetFontSize() * 22, h, changed);
    if (changed) {
        eng.ApplySettings(edit);
        editDirty = false;
    }
    ImGui::Spacing();
    if (ImGui::Button(T("Close"), ImVec2(mobile ? -1 : 0, h)) || (!mobile && ImGui::IsKeyPressed(ImGuiKey_Escape))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Private contacts: invite codes that let one person reach me through the server without a shared room, so the
// people I invite this way never see each other (docs/INTERNET.md).
void App::DrawPrivateContacts(float fieldW, float h, bool& changed) {
    static std::string forWhom, code, msg, newRoom;
    static int roomSel = 0;  // 0: no room, 1..n: an invite room, n+1: a new one
    static bool msgErr = false;
    static int msgAt = 0;  // where the message shows: 1 under "Use code", 2 under the invites
    auto roomName = [&](const std::string& id) -> std::string {
        for (auto& r : edit.wanInviteRooms)
            if (r.secret == id) return r.label;
        return {};
    };
    auto codeFor = [&](const WanLink& l) {
        InviteInfo i;
        i.server = edit.wanServer;
        i.secret = l.secret;
        i.name = edit.displayName;
        i.room = roomName(l.room);
        return MakeInviteCode(i);
    };
    ImGui::SeparatorText(T("Invites"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", T("Invite people with a code instead of sharing the room: each of them can reach you through the "
                                "server, and they only see each other if you invite them into the same invite room."));
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(fieldW * 0.6f);
    ImGui::InputTextWithHint("##inviteCode", T("invite code you got (LSI1-...)"), &code);
    if (ImGui::GetContentRegionAvail().x - ImGui::GetItemRectSize().x > ImGui::CalcTextSize(T("Use code")).x + 40) ImGui::SameLine();
    if (ImGui::Button(T("Use code"), ImVec2(0, h))) {
        InviteInfo ii;
        bool parsed = ParseInviteCode(code, ii);
        std::string server = ii.server, secret = ii.secret, name = ii.name;
        if (!parsed) {
            msg = T("That isn't a valid invite code.");
            msgErr = true;
            msgAt = 1;
        } else if (edit.wanEnabled && !edit.wanServer.empty() && edit.wanServer != server) {
            msg = T("That code is for a different server than the one you use.");  // (no addresses: they may be private)
            msgErr = true;
            msgAt = 1;
        } else {
            bool have = false;
            for (auto& l : edit.wanLinks) have |= l.secret == secret;
            if (!have) {
                if (!edit.wanEnabled || edit.wanServer.empty()) {
                    // joined only for this: stay out of the server's shared room
                    edit.wanServer = server;
                    edit.wanEnabled = true;
                    edit.wanHidden = true;
                    edit.wanFromInvite = true;
                    // a room of my own that nobody else uses: I see no one on that server but the people who invited me
                    uint8_t rr[16];
                    SecureRandom(rr, sizeof(rr));
                    edit.wanRoom = "invite-" + ToHex(rr, sizeof(rr));
                    edit.wanPassword.clear();
                }
                WanLink l;
                l.secret = secret;
                l.label = name;
                l.created = NowMs();
                l.mine = false;
                l.room = ii.room;  // (its name, shown next to the inviter)
                edit.wanLinks.push_back(l);
                changed = true;
            }
            msg = name.empty() ? std::string(T("Added. You'll see each other in the device list when you're both online."))
                               : TF("Added %s. You'll see each other in the device list when you're both online.", name.c_str());
            msgErr = false;
            msgAt = 1;
            code.clear();
        }
    }
    if (!msg.empty() && msgAt == 1) {
        ImGui::PushTextWrapPos(0);
        if (msgErr) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", msg.c_str());
        else ImGui::TextDisabled("%s", msg.c_str());
        ImGui::PopTextWrapPos();
    }
    bool invited = edit.wanFromInvite;  // someone else's server: no codes of my own to it (they'd carry its address)
    int remove = -1;
    for (int i = 0; i < (int)edit.wanLinks.size(); i++) {
        auto& l = edit.wanLinks[(size_t)i];
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(l.label.empty() ? T("(no name)") : l.label.c_str());
        std::string rn = l.mine ? roomName(l.room) : l.room;
        if (!rn.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.55f, 0.75f, 1, 1), "[%s]", rn.c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", FormatTime(l.created, true).substr(0, 10).c_str());
        ImGui::SameLine();
        if (!invited && ImGui::SmallButton(T("Copy code"))) {
            ImGui::SetClipboardText(codeFor(l).c_str());
            msg = T("Invite code copied.");
            msgErr = false;
            msgAt = 2;
        }
        if (!invited) ImGui::SameLine();
        if (ImGui::SmallButton(T("Remove"))) remove = i;
        ImGui::PopID();
    }
    if (remove >= 0) {
        edit.wanLinks.erase(edit.wanLinks.begin() + remove);
        if (invited && edit.wanLinks.empty()) {  // the last invite gone: nothing left there for me
            edit.wanServer.clear();
            edit.wanEnabled = false;
            edit.wanHidden = false;
            edit.wanFromInvite = false;
        }
        changed = true;
    }

    if (!invited) {
    // invite rooms: everyone invited into the same room sees each other (and you); without a room, only you
    if (!edit.wanInviteRooms.empty()) {
        ImGui::TextDisabled("%s", T("Invite rooms (people in one see each other):"));
        int del = -1;
        for (int i = 0; i < (int)edit.wanInviteRooms.size(); i++) {
            auto& r = edit.wanInviteRooms[(size_t)i];
            int n = 0;
            for (auto& l : edit.wanLinks) n += l.mine && l.room == r.secret;
            ImGui::PushID(1000 + i);
            ImGui::AlignTextToFramePadding();
            ImGui::BulletText("%s", r.label.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(n == 1 ? T("1 person") : T("%d people"), n);
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Delete room"))) del = i;
            ImGui::SetItemTooltip("%s", T("Its people stay your private contacts, but no longer see each other."));
            ImGui::PopID();
        }
        if (del >= 0) {
            for (auto& l : edit.wanLinks)
                if (l.mine && l.room == edit.wanInviteRooms[(size_t)del].secret) l.room.clear();
            edit.wanInviteRooms.erase(edit.wanInviteRooms.begin() + del);
            roomSel = 0;
            changed = true;
        }
    }
    int nRooms = (int)edit.wanInviteRooms.size();
    if (roomSel > nRooms + 1) roomSel = 0;
    std::string cur = roomSel == 0 ? T("No room (only you)") : roomSel <= nRooms ? edit.wanInviteRooms[(size_t)roomSel - 1].label : T("New room...");
    ImGui::SetNextItemWidth(fieldW * 0.6f);
    if (ImGui::BeginCombo("##inviteRoom", cur.c_str())) {
        if (ImGui::Selectable(T("No room (only you)"), roomSel == 0)) roomSel = 0;
        for (int i = 0; i < nRooms; i++)
            if (ImGui::Selectable((edit.wanInviteRooms[(size_t)i].label + "##r" + std::to_string(i)).c_str(), roomSel == i + 1)) roomSel = i + 1;
        if (ImGui::Selectable(T("New room..."), roomSel == nRooms + 1)) roomSel = nRooms + 1;
        ImGui::EndCombo();
    }
    ui::HelpMarker(T("People you invite into the same room see each other in their device list (and you). People "
                   "without a room, or in another room, see only you."));
    if (roomSel == nRooms + 1) {
        ImGui::SetNextItemWidth(fieldW * 0.6f);
        ImGui::InputTextWithHint("##newRoom", T("room name (they see it)"), &newRoom);
    }
    ImGui::SetNextItemWidth(fieldW * 0.6f);
    ImGui::InputTextWithHint("##inviteFor", T("for whom (only you see it)"), &forWhom);
    // (narrow phone screens: the button goes under the field instead of off the edge)
    if (ImGui::GetContentRegionAvail().x - ImGui::GetItemRectSize().x > ImGui::CalcTextSize(T("Create invite code")).x + 40) ImGui::SameLine();
    bool needName = roomSel == nRooms + 1 && newRoom.empty();
    ImGui::BeginDisabled(edit.wanServer.empty() || needName);
    if (ImGui::Button(T("Create invite code"), ImVec2(0, h))) {
        uint8_t k[32];
        WanLink l;
        if (roomSel == nRooms + 1) {
            SecureRandom(k, sizeof(k));
            WanLink r;
            r.secret = ToHex(k, sizeof(k));
            r.label = newRoom;
            r.created = NowMs();
            edit.wanInviteRooms.push_back(r);
            roomSel = (int)edit.wanInviteRooms.size();  // the next invite goes to the same room
            newRoom.clear();
        }
        if (roomSel > 0) l.room = edit.wanInviteRooms[(size_t)roomSel - 1].secret;
        SecureRandom(k, sizeof(k));
        l.secret = ToHex(k, sizeof(k));
        l.label = forWhom;
        l.created = NowMs();
        edit.wanLinks.push_back(l);
        ImGui::SetClipboardText(codeFor(l).c_str());
        msg = T("Invite code copied. Send it to that person only - anyone with the code can reach you.");
        msgErr = false;
            msgAt = 2;
        forWhom.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    if (edit.wanServer.empty()) ImGui::TextDisabled("%s", T("Set the server above first."));
    }

    if (!msg.empty() && msgAt == 2) {
        ImGui::PushTextWrapPos(0);
        if (msgErr) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", msg.c_str());
        else ImGui::TextDisabled("%s", msg.c_str());
        ImGui::PopTextWrapPos();
    }
}

void App::DrawCallSettings(float fieldW, float h, bool& changed) {
    ImGui::SeparatorText(T("Calls"));
    if (!Media().Available()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled(T("Voice calls are an optional module and are not installed (%s). To enable them, put the "
                            "lunashare_media library next to the LunaShare program "
                            "and restart."), Media().loadInfo.c_str());
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextDisabled("%s - %s", Media().ModuleName().c_str(), Media().loadInfo.c_str());
    AudioCallControls(fieldW, h, changed);
    if (Media().VideoAvailable()) {
        changed |= VideoQualityControls(edit, fieldW, h);
        ui::HelpMarker(T("Picture size and frame rate you send in video calls. Lower uses less battery and data; you can also change it during a call."));
        changed |= ImGui::Checkbox(T("Lower video quality automatically when the connection is slow"), &edit.callVideoAdaptive);
        changed |= ImGui::Checkbox(T("Show my own picture in full quality"), &edit.callFullPreview);
        ui::HelpMarker(T("Your self-view (in calls and when recording a video message) in the camera's full size and frame rate, like "
                       "the recording, instead of a lighter preview. Uses a bit more battery."));
        ui::HelpMarker(T("Keeps the picture smooth: less bitrate first, then a lower frame rate and a smaller picture while the "
                       "connection is slow, back up when it recovers."));
        changed |= ImGui::Checkbox(T("Use only H.264 (if HEVC/AV1 video shows problems)"), &edit.callVideoH264Only);
        if (Media().VcamAvailable()) {
            bool v = edit.virtualCamera;
            ImGui::BeginDisabled(Media().VcamBusy());
            if (ImGui::Checkbox(T("Virtual camera: use received video as a webcam (\"LunaShare Camera\")"), &v)) {
                edit.virtualCamera = v;
                changed = true;
                Media().SetVirtualCamera(v);
            }
            ImGui::EndDisabled();
#if defined(_WIN32)
            ui::HelpMarker(T("Other apps (Teams, Zoom, OBS, Discord...) can pick \"LunaShare Camera\". It shows the video from the other "
                           "device during a video call - for example your phone's camera - and a dark picture otherwise. The first "
                           "time Windows asks for administrator permission to install it."));
#else
            ui::HelpMarker(T("Other apps (browsers, Zoom, OBS, Discord...) can pick \"LunaShare Camera\". It shows the video from the "
                           "other device during a video call - for example your phone's camera - and a dark picture otherwise. It uses "
                           "the v4l2loopback kernel module; LunaShare asks for your password once per boot to load it."));
#endif
            std::string st = Media().VcamStatus();
            if (!st.empty()) ImGui::TextDisabled("%s", st.c_str());
        }
#if defined(__linux__) && !defined(__ANDROID__)
        else if (Media().VideoAvailable()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", T("\"LunaShare Camera\" for other apps (for example your phone as a webcam): install the v4l2loopback "
                                        "kernel module (package v4l2loopback-dkms) and restart LunaShare."));
            ImGui::PopTextWrapPos();
        }
#endif
        auto cams = Media().Cameras();
        if (cams.size() > 1) {
            std::string cur = T("Front camera (default)");
            for (auto& c : cams) if (edit.callCamera == c.id) cur = c.name;
            ImGui::SetNextItemWidth(fieldW);
            if (ImGui::BeginCombo("##defcam", (T("Camera: ") + cur).c_str())) {
                if (ImGui::Selectable(T("Front camera (default)"), edit.callCamera.empty(), 0, ImVec2(0, h))) { edit.callCamera.clear(); changed = true; }
                for (auto& c : cams)
                    if (ImGui::Selectable(c.name, edit.callCamera == c.id, 0, ImVec2(0, h))) { edit.callCamera = c.id; changed = true; }
                ImGui::EndCombo();
            }
        }
    }
    bool testing = Media().MicTesting();
    if (eng.call.state == CallState::None) {
        if (ImGui::Button(testing ? T("Stop microphone test") : T("Test microphone"), ImVec2(0, h))) testing ? Media().StopMicTest() : Media().StartMicTest();
        if (testing) {
            ImGui::SameLine();
            ui::ProgressBarText(Media().MicTestLevel(), "", ImVec2(ImGui::GetFontSize() * 8, ImGui::GetFrameHeight() * 0.5f), ui::ColOk());
        }
    }
    if (!Media().audioError.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ColErr()), "%s", Media().audioError.c_str());
}
