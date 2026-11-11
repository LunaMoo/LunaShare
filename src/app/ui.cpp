#include "ui.h"
#include "i18n.h"

#include <SDL.h>

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstring>

#include "crypto.h"
#include "hash.h"
#include "avatar.h"
#include "notify.h"
#include "image_cache.h"
#include "media_host.h"
#include "music.h"
#include "player_host.h"
#include "emoji_host.h"
#include "archives.h"
#include "file_assoc.h"
#include "torrent_host.h"
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
        } else if (a.what == "view" || a.what == "send") {  // (Android: opened / shared with LunaShare; one path a line)
            std::vector<std::string> ps;
            size_t s = 0;
            while (s <= a.key.size()) {
                size_t e = a.key.find('\n', s);
                if (e == std::string::npos) e = a.key.size();
                if (e > s) ps.push_back(a.key.substr(s, e - s));
                s = e + 1;
            }
            if (!ps.empty()) OpenPaths(ps, a.what == "send");
        } else if (a.what == "sendtext" && !a.key.empty()) {  // (text shared with LunaShare: to a chat)
            pendingSend.clear();
            pendingForward = {a.key};
            pendingShareText = true;
            openSendTo = true;
        } else if (a.what == "unlock" && !eng.HistoryLocked()) {  // (the fingerprint; the encrypted history needs the PIN)
            locked = false;
            lockWrong = 0;
        }
    }
    LockTick();
    ui::Metrics& m = ui::M();
    m.mobile = IsMobileLayout();
    m.rowH = ImGui::GetFrameHeight() * (m.mobile ? 1.9f : 1.0f);
    if (!openPathsPending.empty() && !locked) HandleOpenPaths();

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
        if (selectedPeer.empty() && std::string(autoSel) != "1")  // =<name>: that chat, also offline
            for (auto& [id, cv] : eng.chats)
                if (eng.PeerName(id) == autoSel) {
                    SelectPeer(id);
                    if (m.mobile) { backStack = {MScreen::Home}; screen = MScreen::Chat; }
                    break;
                }
    }
    static bool devSleep = getenv("LUNASHARE_SLEEPTEST") != nullptr;  // dev: the sleep timer's question (screenshots)
    if (devSleep && ImGui::GetFrameCount() > 30) {
        devSleep = false;
        SleepSet(1, 0);
        sleepAt = SteadyMs() + 45000;
    }
    static const char* devSearch = getenv("LUNASHARE_SEARCH");  // dev: search (screenshots)
    if (devSearch && !selectedPeer.empty() && ImGui::GetFrameCount() > 200) {
        searchOn = true;
        searchText = devSearch;
        searchAll = true;
        devSearch = nullptr;
    }
    static bool devHist = getenv("LUNASHARE_OPENHIST") != nullptr;  // dev: the chat history window (screenshots)
    if (devHist && !selectedPeer.empty() && ImGui::GetFrameCount() > 3) {
        devHist = false;
        if (std::string(getenv("LUNASHARE_OPENHIST")) == "poll") {
            OpenPollNew(selectedPeer);
            pollQ = "Sunday lunch?";
            pollOpts = {"12:00", "14:00", ""};
        } else if (std::string(getenv("LUNASHARE_OPENHIST")) == "verify") {
            verifyQrOpen = true;
            verifyQrPeer = selectedPeer;
        } else if (std::string(getenv("LUNASHARE_OPENHIST")) == "starred") {
            starredOpen = true;
        } else if (std::string(getenv("LUNASHARE_OPENHIST")) == "mute") {
            muteKey = selectedPeer;
            muteOpen = true;
        } else {
            OpenChatHistory(selectedPeer);
        }
    }
    if (static const char* dc = getenv("LUNASHARE_DEVCHAT"); dc && ImGui::GetFrameCount() == 3) {  // dev: a long chat, open
        auto lk = eng.Lock();
        bool self = getenv("LUNASHARE_DEVCHATSELF") != nullptr;  // (or my own chat, written on two devices)
        auto& cv = eng.chats[self ? eng.SelfKey() : "devchatpeer00000"];
        for (int i = 0; i < atoi(dc); i++) {
            ChatMessage m;
            m.mid = "dev" + std::to_string(i);
            m.ts = NowMs() - (atoi(dc) - i) * 60000LL;
            m.kind = i % 3 ? MsgKind::Text : MsgKind::System;
            m.outgoing = i % 2;
            m.text = "message " + std::to_string(i) + (i % 5 ? "" : " with a longer text that wraps over a couple of lines in the chat window, like a real one");
            if (self && !m.outgoing) m.fromDev = "phone-of-mine-0001";
            cv.messages.push_back(m);
        }
        selectedPeer = self ? eng.SelfKey() : "devchatpeer00000";
        scrollToBottom = true;
    }
    static const char* devLink = getenv("LUNASHARE_SHARELINK");  // dev: "a link for anyone" for this file (screenshots)
    if (devLink && ImGui::GetFrameCount() == 20) {
        std::string p = devLink;
        if (p.find("|send") != std::string::npos) OpenSendTo({p.substr(0, p.find('|'))});
        else OpenShareLink(std::vector<std::string>{p.substr(0, p.find('|'))});
    }
    static const char* devMovie = getenv("LUNASHARE_MOVIE");  // dev: open a movie in the movie player (screenshots)
    if (devMovie && ImGui::GetFrameCount() == 20 && eng.StartWatch("", devMovie)) {
        watchShown = true;
        if (const char* sp = getenv("LUNASHARE_SPEED")) eng.WatchSetSpeed(atof(sp));  // (dev)
    }
    static const char* autoWatch = getenv("LUNASHARE_AUTOWATCH");  // dev: join the first movie invitation (screenshots)
    if (autoWatch && !eng.watch.sid && !eng.watchInvites.empty() && eng.JoinWatch(eng.watchInvites.back().sid)) watchShown = *autoWatch != 'b';
    static const char* autoGroup = getenv("LUNASHARE_AUTOGROUP");  // dev: open the first group (screenshots)
    if (autoGroup && !Engine::IsGroupKey(selectedPeer) && !eng.groups.empty()) {
        SelectPeer("#" + eng.groups.begin()->first);
        if (m.mobile) { backStack = {MScreen::Home}; screen = MScreen::Chat; }
        if (*autoGroup == 'm') OpenGroupMembers(selectedPeer);
    }
    static const char* devMusic = getenv("LUNASHARE_MUSICPLAY");  // dev: play the library's first song (screenshots)
    if (devMusic && ImGui::GetFrameCount() > 60 && !music::Play().GetNow().active) {
        MusicRefresh();
        std::vector<std::string> p;
        for (auto& t : musicTracks) p.push_back(t.path);
        std::sort(p.begin(), p.end());
        if (!p.empty()) music::Play().PlayList(p, 0);
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
        if (a == "gallery") { tab = Tab::Gallery; screen = MScreen::Gallery; tabForce = true; if (getenv("LUNASHARE_GALLERYVIEW")) galleryView = atoi(getenv("LUNASHARE_GALLERYVIEW")); }
        if (a == "torrents") { tab = Tab::Torrents; screen = MScreen::Torrents; tabForce = true; }
        if (a == "archives" || a == "comics") { tab = Tab::Archives; screen = MScreen::Archives; tabForce = true; archivesComics = a == "comics"; }
        if (getenv("LUNASHARE_OPENSOUND")) musicSoundOpen = true;
        if (getenv("LUNASHARE_OPENLISTEN")) musicListenOpen = true;
        if (a == "movies") { tab = Tab::Movies; screen = MScreen::Movies; }
        if (a == "music" || a == "musicnow") { tab = Tab::Music; screen = MScreen::Music; musicNowShown = a == "musicnow"; }
        if (const char* mv = getenv("LUNASHARE_MUSICVIEW")) musicView = std::clamp(atoi(mv), 0, 5);  // dev: songs .. playlists
        if (const char* mg = getenv("LUNASHARE_MUSICGROUP")) musicGroup = mg;
        if (a == "home") screen = MScreen::Home;
        tabForce = true;
        autoSel = nullptr;
    }
    static const char* autoBrowse = getenv("LUNASHARE_BROWSE");
    static bool autoBrowseDone = false;
    if (autoBrowse && !autoBrowseDone) {
        autoBrowseDone = true;
        browseFor = BrowseFor::SendFiles;
        if (getenv("LUNASHARE_BROWSE_DIR")) {  // (the folder picker: adding a music folder)
            browseFor = BrowseFor::MusicFolder;
            browser.Open(FileBrowser::Mode::SelectDir, T("Add a music folder"), autoBrowse);
        } else {
            browser.Open(FileBrowser::Mode::OpenMulti, T("Send to ..."), autoBrowse);
        }
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
    for (auto& [peer, cv] : eng.chats)
        if (cv.unread && !eng.ChatMuted(peer)) unread += cv.unread;
    if (Media().Recording() || Media().VideoMessageOpen() || Player().Playing() || eng.watch.sid) busy = true;  // keep the timer / seek bar moving
    MusicTick();
    SyncTick();
    UpdateTick();
    if (music::Play().GetNow().playing) busy = true;
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
    if (locked) {  // nothing else until the PIN
        DrawLockScreen();
        ImGui::End();
        return;
    }
    if (!welcomeInit) {
        welcomeInit = true;
        WelcomeInit();
    }
    AddPersonTick();
    DrawCallBar();
    if (welcomeStep >= 0 && eng.call.state == CallState::None) DrawWelcome();  // (a first start; a call that comes in is still answered)
    else if (m.mobile) DrawMobile();
    else DrawDesktop();
    DrawAddPersonPopup();
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
    DrawChatHistoryPopup();
    DrawMutePopup();
    DrawStarredPopup();
    DrawPollNewPopup();
    DrawUpdatePopup();
    DrawLockSetPopup();
    DrawVerifyQrPopup();
    DrawLockEncPopup();
    ArchiveTick();
    TorrentPopups();
    Torrents().Tick();
    SleepPopup();
    DrawSleepAsk();
    DrawToasts();
    DrawTextMirror();
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

// music: received files count as music too; it pauses for a movie, a call, a recording or chat audio, and goes on
// afterwards. (Also called while the window is minimized - the phone in the background - when no frames are drawn.)
void App::MusicTick() {
    auto lk = eng.Lock();
    music::Lib().SetImplicitFolders({eng.settings.downloadDir});
    bool interrupt = Player().Playing() || eng.call.state != CallState::None || Media().Recording() || Media().VideoMessageOpen();
    music::Play().SetInterrupted(interrupt);
    MusicExternalTick();
    SleepTick();
    {  // a play counts once at least half of it (or a minute) was heard
        auto now = music::Play().GetNow();
        static std::string playing;
        static double heard = 0, lastPos = -1;
        static bool counted = false;
        if (!now.active || now.external || now.path != playing) {
            playing = now.active && !now.external ? now.path : "";
            heard = 0;
            lastPos = now.pos;
            counted = false;
        } else if (now.playing) {
            double d = now.pos - lastPos;
            if (d > 0 && d < 2) heard += d;  // (a jump forward doesn't count as heard)
            lastPos = now.pos;
            if (!counted && heard >= std::min(60.0, std::max(5.0, now.dur * 0.5))) {
                counted = true;
                music::Lib().CountPlay(playing);
            }
            if (now.pos < 1 && heard > 5) {  // (started again: repeat one)
                heard = 0;
                counted = false;
            }
        } else {
            lastPos = now.pos;
        }
    }
    music::MediaKeys(music::Play().GetNow().active);
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
                if (eng.ChatMuted(e.peer) && e.a != "mention") continue;  // (silent: only the unread count; an @mention still shows)
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
                if (music::Library::IsAudioPath(e.text)) music::Lib().Rescan();  // (received music joins the library)
                if (PlayerHost::IsVideoPath(e.text)) movieRescanWanted = true;  // (and movies the Movies tab)
                if (PlayerHost::IsVideoPath(e.text) || ImageCache::IsImagePath(e.text)) galleryRescanWanted = true;  // (pictures the Gallery)
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
                if (e.text.empty()) continue;  // (only wakes the window up: something changed)
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
            case EventType::ShareAsk:  // (its window opens by itself)
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
    eng.selfName = T("Me");
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
    if (people) {  // my own chat: always first
        auto s = std::find(v.begin(), v.end(), eng.SelfKey());
        if (s != v.end()) std::rotate(v.begin(), s, s + 1);
    }
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

void App::SendFilesUI(const std::string& peer, const std::vector<std::string>& paths) {
    if (!eng.settings.photosSmaller) {
        eng.SendPaths(peer, paths);
        return;
    }
    auto isPhoto = [](const std::string& p) {
        std::string e = p.size() > 5 ? p.substr(p.find_last_of('.') == std::string::npos ? p.size() : p.find_last_of('.')) : "";
        for (auto& c : e) c = (char)std::tolower((unsigned char)c);
        return (e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".bmp") && !IsDirectory(p) && FileSize(p) > 700 * 1024;
    };
    if (std::none_of(paths.begin(), paths.end(), isPhoto)) {
        eng.SendPaths(peer, paths);
        return;
    }
    toasts.push_back({EventType::Info, T("Photos"), T("Making the photos smaller..."), "", 0, 2.0f});
    std::string dir = JoinPath(eng.ConfigPath(), "smaller");
    Engine* e = &eng;
    std::thread([e, peer, paths, dir, isPhoto] {
        std::vector<std::string> out;
        {  // (made-smaller copies from over a month ago: long sent)
            namespace fs = std::filesystem;
            std::error_code ec;
            for (auto& d : fs::directory_iterator(fs::u8path(dir), ec))
                if (d.is_directory(ec) && fs::file_time_type::clock::now() - d.last_write_time(ec) > std::chrono::hours(24 * 30)) fs::remove_all(d.path(), ec);
        }
        std::string sub = JoinPath(dir, RandomHex(6));  // (a folder per sending: the names stay the same)
        for (auto& p : paths) {
            if (isPhoto(p)) {
                MakeDirs(sub);
                std::string base = BaseName(p);
                std::string q = JoinPath(sub, base.substr(0, base.find_last_of('.')) + ".jpg");
                if (MakeSmallerJpeg(p, 2048, q) && FileSize(q) > 0 && FileSize(q) < FileSize(p)) {
                    out.push_back(q);
                    continue;
                }
                RemoveFile(q);
            }
            out.push_back(p);  // (not smaller after all, or not a photo: as it is)
        }
        e->SendPaths(peer, out);
    }).detach();
}

void App::OnDrop(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    auto lk = eng.Lock();
    bool chatVisible = !selectedPeer.empty() && !Engine::IsGroupKey(selectedPeer) &&
                       ((!ui::M().mobile && tab == Tab::Chat) || (ui::M().mobile && screen == MScreen::Chat));
    if (chatVisible) {
        SendFilesUI(selectedPeer, paths);
        scrollToBottom = true;
    } else {
        OpenSendTo(paths);
    }
}

void App::OpenPaths(const std::vector<std::string>& paths, bool send) {
    openPathsPending = paths;
    openPathsSend = send;
}

void App::HandleOpenPaths() {
    std::vector<std::string> paths;
    paths.swap(openPathsPending);
    // a magnet link, or a .torrent file: to the torrents (here, or on one of my computers)
    if (!paths.empty() && !openPathsSend && TorrentHost::IsTorrentSource(paths[0]) &&
        (paths[0].compare(0, 7, "magnet:") == 0 || FileExists(paths[0]))) {
        if (watchShown) watchShown = false;
        if (viewer.open) viewer.closeRequest = true;
        musicNowShown = false;
        TorrentOpenSource(paths[0]);
        return;
    }
    std::vector<std::string> files;
    for (auto& p : paths)
        if (FileExists(p) || IsDirectory(p)) files.push_back(p);
    if (files.empty()) return;
    bool mobile = IsMobileLayout();
    // what was on top steps aside for it: a movie (paused when nobody else watches), the picture viewer, the song
    // screen - else it would only show once those are closed
    const std::string& first = files[0];
    bool movie = !openPathsSend && files.size() == 1 && PlayerHost::IsVideoPath(first) && Player().Available();
    bool picture = !openPathsSend && files.size() == 1 && ImageCache::IsImagePath(first);
    if (watchShown && !movie) {
        if (watchFromViewer && eng.watchKeys.empty() && eng.watch.host.empty()) eng.LeaveWatch();
        else if (eng.watch.playing && eng.watch.host.empty() && eng.watchKeys.empty()) eng.WatchControl(false, -1);
        watchFromViewer = false;
        watchShown = false;
    }
    if (viewer.open && !picture) viewer.closeRequest = true;
    if (!music::Library::IsAudioPath(first)) musicNowShown = false;
    auto show = [&](Tab t, MScreen s) {
        if (mobile) {
            backStack = {MScreen::Home};
            screen = s;
        } else {
            tab = t;
            tabForce = true;
        }
    };
    if (openPathsSend || files.size() > 1 && !std::all_of(files.begin(), files.end(), [](const std::string& f) { return music::Library::IsAudioPath(f); })) {
        OpenSendTo(files);  // (several different things, or shared: to a chat)
        return;
    }
    const std::string& f = files[0];
    if (music::Library::IsAudioPath(f) && Player().Available()) {
        music::Play().PlayList(files, 0);
        show(Tab::Music, MScreen::Music);
        musicNowShown = true;
    } else if (PlayerHost::IsVideoPath(f) && Player().Available()) {
        watchFailed = false;
        watchLibList.clear();
        if (eng.StartWatch("", f)) watchShown = true;
    } else if (arc::IsComicPath(f)) {
        OpenComic(f);
        OpenArchives();  // (the Comics view: how far the opening is)
        archivesComics = true;
    } else if (arc::IsArchivePath(f) && !IsDirectory(f)) {
        OpenArchive(f);
    } else if (ImageCache::IsImagePath(f)) {
        // the other pictures of its folder come along (arrows / swipes)
        std::vector<std::string> list;
        std::string dir = DirName(f);
        for (auto& e : ListDir(dir))
            if (!e.isDir && ImageCache::IsImagePath(e.name)) list.push_back(JoinPath(dir, e.name));
        std::sort(list.begin(), list.end(), [](const std::string& a, const std::string& b) { return NaturalLess(BaseName(a), BaseName(b)); });
        if (std::find(list.begin(), list.end(), f) == list.end()) list = {f};
        OpenViewerList(list, (int)(std::find(list.begin(), list.end(), f) - list.begin()));
    } else {
        OpenSendTo(files);  // (nothing here shows it: it can still go to someone)
    }
}

void App::OpenSendTo(const std::vector<std::string>& paths) {
    pendingSend = paths;
    pendingSendList.clear();
    pendingForward.clear();
    pendingShareText = false;
    openSendTo = true;
}

// passing messages on to another chat: their text, and the files they brought (when they're here)
void App::ForwardMessages(const std::string& peer, const std::vector<std::string>& mids) {
    std::vector<std::string> texts, paths;
    auto it = eng.chats.find(peer);
    if (it == eng.chats.end()) return;
    for (auto& m : it->second.messages) {
        if (std::find(mids.begin(), mids.end(), m.mid) == mids.end() || m.kind == MsgKind::System) continue;
        if (m.kind == MsgKind::Text && !m.text.empty()) texts.push_back(m.text);
        if (m.kind == MsgKind::Files)
            for (auto tid : m.tids) {
                auto t = eng.transfers.find(tid);
                if (t != eng.transfers.end() && t->second->state == TState::Completed && FileExists(t->second->localPath))
                    paths.push_back(t->second->localPath);
            }
    }
    if (texts.empty() && paths.empty()) {
        toasts.push_back({EventType::Error, T("Forward"), T("Nothing to forward (the files aren't on this device)."), "", 0, 3.0f});
        return;
    }
    pendingSend = paths;
    pendingForward = texts;
    pendingShareText = false;
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
    bool self = eng.IsSelfKey(peer);
    if (eng.IsContactKey(peer)) {
        size_t n = eng.DevicesOf(peer).size();
        if (n > 1) sub = TF("%d devices", (int)n);
        if (self) sub = n ? TF("notes, and your other devices (%d)", (int)n) : std::string(T("notes for yourself"));
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
    } else if (self && eng.DevicesOf(peer).empty()) {
        // (nothing to connect to)
    } else if (connected && conn) sub += std::string(sub.empty() ? "" : "  ") + TF("via %s", T(TransportName(conn->transport))) + (conn->addr == "relay" ? T(" (relay)") : "");
    else if (online) sub += "  " + TF("online (%s)", TS(eng.online[dev].Via()).c_str());
    else sub += sub.empty() ? T("offline") : std::string("  ") + T("offline");

    bool muted = eng.ChatMuted(peer);
    if (muted) sub = std::string(T("silent")) + (sub.empty() ? "" : "  ") + sub;
    if (peer != selectedPeer) {
        std::string dr = DraftGet(peer);
        if (!dr.empty()) {
            if (dr.size() > 40) {
                dr.resize(40);
                while (!dr.empty() && ((unsigned char)dr.back() & 0xC0) == 0x80) dr.pop_back();  // (whole characters)
                if (!dr.empty() && ((unsigned char)dr.back() & 0x80)) dr.pop_back();
                dr += "...";
            }
            for (auto& c : dr)
                if (c == '\n' || c == '\r') c = ' ';
            sub = std::string(T("Draft")) + ": " + Emoji().Display(dr);
        }
    }
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
    DrawAvatar(dl, ac, av, self ? std::string() : peer, self ? eng.settings.displayName : name);  // (mine: my picture)
    if (!self || !eng.DevicesOf(peer).empty()) dl->AddCircleFilled(ImVec2(ac.x + av * 0.35f, ac.y + av * 0.35f), r, dot);
    float tx = p0.x + pad * 2 + av;
    float textH = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(tx, cy - textH - 1), ImGui::GetColorU32(blocked ? ImGuiCol_TextDisabled : ImGuiCol_Text), name.c_str());
    dl->AddText(ImVec2(tx, cy + 1), ui::ColMuted(), sub.c_str());
    if (tSize > 0) {
        float w = (p1.x - tx - pad) * (float)((double)tDone / (double)tSize);
        dl->AddRectFilled(ImVec2(tx, p1.y - 3), ImVec2(tx + w, p1.y - 1), ui::ColAccent());
    }
    bool mentioned = cv != eng.chats.end() && cv->second.mentioned && unread > 0;
    if (unread > 0) {
        std::string u = (mentioned ? "@ " : "") + std::to_string(unread);  // (an @mention: red even when silent)
        ImVec2 us = ImGui::CalcTextSize(u.c_str());
        float bw = std::max(us.x + 10, textH);
        ImVec2 b0(p1.x - pad - bw, cy - textH * 0.5f);
        dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + textH), muted && !mentioned ? IM_COL32(110, 110, 120, 255) : ui::ColErr(), textH * 0.5f);
        dl->AddText(ImVec2(b0.x + (bw - us.x) * 0.5f, b0.y), IM_COL32_WHITE, u.c_str());
    }
    if (rightClick) ImGui::OpenPopup(grp ? "groupctx" : "peerctx");
    if (ImGui::BeginPopup("groupctx")) {
        float h = ui::M().mobile ? ui::M().rowH * 0.8f : 0;
        if (ImGui::Selectable(T("People in this group"), false, 0, ImVec2(0, h))) OpenGroupMembers(peer);
        if (MuteItem(peer, h)) ImGui::CloseCurrentPopup();
        if (grp && !grp->left && ImGui::Selectable(T("Add people"), false, 0, ImVec2(0, h))) OpenAddToGroup(peer);
        if (grp && !grp->left && ImGui::Selectable(T("Leave the group"), false, 0, ImVec2(0, h))) eng.LeaveGroup(peer);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("peerctx")) {
        float h = ui::M().mobile ? ui::M().rowH * 0.8f : 0;
        bool others = !self || !eng.DevicesOf(peer).empty();
        if (others && ImGui::Selectable(connected ? T("Disconnect") : T("Connect"), false, 0, ImVec2(0, h))) connected ? eng.Disconnect(peer) : eng.ConnectPeer(peer);
        if (MuteItem(peer, h)) ImGui::CloseCurrentPopup();
        if (!self && ImGui::Selectable(T("Rename..."), false, 0, ImVec2(0, h))) {
            renamePeer = peer;
            renameBuf = d ? d->customName : "";
            openRename = true;
        }
        bool trusted = d && d->trusted;
        if (!self && ImGui::Selectable(trusted ? T("Untrust") : T("Trust (auto-accept files)"), false, 0, ImVec2(0, h))) eng.SetTrusted(peer, !trusted);
        if (ImGui::Selectable(T("Send files..."), false, 0, ImVec2(0, h))) {
            SelectPeer(peer);
            browseFor = BrowseFor::SendFiles;
            browser.Open(FileBrowser::Mode::OpenMulti, TF("Send to %s", name.c_str()), browser.lastDir);
        }
        if (d && !self && ImGui::Selectable(T("Device details"), false, 0, ImVec2(0, h))) {
            devicesSel = dev;
            if (ui::M().mobile) { backStack.push_back(screen); screen = MScreen::Devices; }
            else { tab = Tab::Devices; tabForce = true; }
        }
        if (!self && ImGui::Selectable(T("Forget..."), false, 0, ImVec2(0, h))) {
            forgetPeer = peer;
            openForget = true;
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
    // ---- where to: a few sections across the top, their pages (when a section has more than one) in a row under it
    {
        struct Page {
            Tab t;
            const char* label;
        };
        struct Sec {
            const char* label;
            Icon icon;
            std::vector<Page> pages;
        };
        bool torrents = Torrents().Available() || !Torrents().Computers().empty();
        std::vector<Sec> secs = {
            {T("Chat"), Icon::Send, {{Tab::Chat, T("Chat")}}},
            {T("Files"), Icon::Transfers, {{Tab::Transfers, T("Transfers")}, {Tab::History, T("History")}, {Tab::Library, T("Shared folder")}}},
            {T("Multimedia"), Icon::Image, {{Tab::Gallery, T("Gallery")}, {Tab::Movies, T("Movies")}, {Tab::Music, T("Music")}}},
            {T("Tools"), Icon::Archive, {{Tab::Archives, T("Archives")}}},
            {T("Devices"), Icon::Users, {{Tab::Devices, T("Devices")}, {Tab::Nearby, T("Connect")}}},
            {T("Settings"), Icon::Gear, {{Tab::Settings, T("Settings")}}},
        };
        if (torrents) secs[3].pages.push_back({Tab::Torrents, T("Torrents")});
        else if (tab == Tab::Torrents) tab = Tab::Archives;
        int cur = 0;
        for (size_t i = 0; i < secs.size(); i++)
            for (auto& p : secs[i].pages)
                if (p.t == tab) cur = (int)i;
        navLast[cur] = tab;
        int active = 0;
        for (auto& [id, t] : eng.transfers)
            if (!TStateFinal(t->state)) active++;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float fs = ImGui::GetFontSize(), barH = fs * 2.3f, pad = fs * 0.9f, icon = fs * 1.05f;
        ImU32 accent = ui::ColAccentText(), text = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(fs * 0.25f, ImGui::GetStyle().ItemSpacing.y));
        for (size_t i = 0; i < secs.size(); i++) {
            Sec& sc = secs[i];
            bool on = (int)i == cur;
            float tw = ImGui::CalcTextSize(sc.label).x, w = pad + icon + fs * 0.45f + tw + pad;
            if (i) ImGui::SameLine();
            ImGui::PushID((int)i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool click = ImGui::InvisibleButton("##sec", ImVec2(w, barH));
            bool hov = ImGui::IsItemHovered();
            if (on) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + barH), ImGui::GetColorU32(ImGuiCol_Header), fs * 0.5f);
            else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + barH), ImGui::GetColorU32(ImGuiCol_FrameBg), fs * 0.5f);
            ImU32 col = on ? accent : hov ? text : muted;
            ui::DrawIcon(dl, sc.icon, ImVec2(p.x + pad + icon * 0.5f, p.y + barH * 0.5f), icon, col);
            dl->AddText(ImVec2(p.x + pad + icon + fs * 0.45f, p.y + (barH - fs) * 0.5f), on ? text : col, sc.label);
            if (i == 1 && active > 0) {  // (transfers going on: how many)
                std::string n = std::to_string(active);
                ImVec2 ts = ImGui::CalcTextSize(n.c_str());
                float r = std::max(ts.x, ts.y) * 0.5f + 2;
                ImVec2 c(p.x + w - r - 1, p.y + r + 1);
                dl->AddCircleFilled(c, r, ui::ColAccent());
                dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32_WHITE, n.c_str());
            }
            if (click && !on) {
                Tab to = navLast[i];
                bool ok = false;
                for (auto& pg : sc.pages) ok |= pg.t == to;
                tab = ok ? to : sc.pages[0].t;
                cur = (int)i;
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        Sec& sc = secs[(size_t)cur];
        if (sc.pages.size() > 1) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, fs);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(fs * 0.8f, fs * 0.22f));
            for (size_t i = 0; i < sc.pages.size(); i++) {
                if (i) ImGui::SameLine();
                bool on = sc.pages[i].t == tab;
                std::string label = sc.pages[i].label;
                if (sc.pages[i].t == Tab::Transfers && active > 0) label = TF("Transfers (%d)", active);
                ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::ColAccent() : IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, on ? ui::ColOnAccent() : muted);
                if (ImGui::Button((label + "###pg" + std::to_string(i)).c_str())) tab = sc.pages[i].t;
                ImGui::PopStyleColor(2);
            }
            ImGui::PopStyleVar(2);
        }
        ImGui::Dummy(ImVec2(0, fs * 0.15f));
        tabForce = false;
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
        case Tab::Music: DrawMusic(); break;
        case Tab::Movies: DrawMovies(); break;
        case Tab::Gallery: DrawGallery(); break;
        case Tab::Archives: DrawArchives(); break;
        case Tab::Torrents: DrawTorrents(); break;
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
    bool openInv = ImGui::Selectable((eng.settings.displayName + "  " + T("- invites...") + "##me").c_str(), false, 0,
                                     ImVec2(0, ui::M().mobile ? ui::M().rowH * 0.8f : 0));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered() && !ui::M().mobile) {
        ImGui::SetTooltip(T("Invite someone, or use an invite code you got\n\nThis device\nID: %s\nKey fingerprint: %s\nPort: %u"),
                          eng.settings.deviceId.c_str(), KeyFingerprint(eng.MyPublicKey()).c_str(), eng.boundPort);
    }
    if (openInv) openInvites = true;
    DrawInvitesPopup();
    if (eng.watch.sid && (eng.watch.key.empty() || eng.watch.key != selectedPeer)) DrawWatchResume();
    ImGui::Separator();
    auto peers = SortedPeers();
    bool mini = music::Play().GetNow().active;
    float footer = ImGui::GetFrameHeightWithSpacing() * 3.2f + (mini ? ImGui::GetFrameHeight() * 1.9f + ImGui::GetStyle().ItemSpacing.y * 2 : 0);
    ImGui::BeginChild("peers", ImVec2(0, -footer));
    if (peers.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("Nobody here yet.\n\nDevices running LunaShare on your network show up by themselves. Anyone else: Add someone."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ui::PushAccent();
        if (ui::IconTextButton(T("Add someone"), Icon::Plus, ImVec2(-1, ImGui::GetFrameHeight() * 1.2f))) OpenAddPerson(0);
        ui::PopAccent();
    }
    for (auto& p : peers) {
        if (DevicePeerRow(p, p == selectedPeer && tab == Tab::Chat, 0)) {
            SelectPeer(p);
            tab = Tab::Chat;
            tabForce = true;
        }
    }
    ImGui::EndChild();
    if (mini) DrawMusicMini();
    ImGui::Separator();
    {
        if (ui::IconTextButton(T("Add someone"), Icon::Plus, ImVec2(-1, 0))) OpenAddPerson(-1);
        ImGui::SetItemTooltip("%s", T("Nearby devices, a QR code, an invite code, or the link to get LunaShare"));
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
            else if (ClearPicks()) {
            } else if (screen == MScreen::Gallery && !galleryAlbum.empty()) {
                galleryAlbum.clear();
            } else if (screen == MScreen::Archives && ArchiveBack()) {
            } else if (!backStack.empty()) { screen = backStack.back(); backStack.pop_back(); }
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
        case MScreen::Music: title = T("Music"); break;
        case MScreen::Movies: title = T("Movies"); break;
        case MScreen::Gallery: title = T("Gallery"); break;
        case MScreen::Archives: title = T("Archives"); break;
        case MScreen::Torrents: title = T("Torrents"); break;
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
    int nb = chatScreen ? 2 + (callable ? 1 : 0) + (videoable ? 1 : 0) + (webcam ? 1 : 0) + (watchable ? 1 : 0) : 0;
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
        if (ui::IconButton("chathist", Icon::History, nullptr, hb)) OpenChatHistory(selectedPeer);
        ImGui::SameLine();
        if (groupChat) GroupButtons(selectedPeer, hb);
        else if (ui::IconButton("attach", Icon::Attach, nullptr, hb)) {
            browseFor = BrowseFor::SendFiles;
            browser.Open(FileBrowser::Mode::OpenMulti, TF("Send to %s", peerTitle.c_str()), browser.lastDir);
        }
    }
    ImGui::Separator();

    float navH = (screen == MScreen::Chat || keyboardInset > 0) ? 0 : rowH * 1.25f + ImGui::GetStyle().ItemSpacing.y;
    // the music strip above the navigation (not in a chat - its text box is there - nor on the music screen itself)
    bool mini = navH > 0 && screen != MScreen::Music && music::Play().GetNow().active;
    float miniH = mini ? rowH * 0.95f + ImGui::GetStyle().ItemSpacing.y * 2 : 0;
    ImGui::BeginChild("mcontent", ImVec2(0, -navH - miniH));
    switch (screen) {
        case MScreen::Home: DrawMobileHome(); break;
        case MScreen::Chat: DrawChat(selectedPeer); break;
        case MScreen::Transfers: DrawTransfers(); break;
        case MScreen::History: DrawHistory(); break;
        case MScreen::Library: DrawLibrary(); break;
        case MScreen::Music: DrawMusic(); break;
        case MScreen::Movies: DrawMovies(); break;
        case MScreen::Gallery: DrawGallery(); break;
        case MScreen::Archives: DrawArchives(); break;
        case MScreen::Torrents: DrawTorrents(); break;
        case MScreen::Nearby: DrawNearby(); break;
        case MScreen::Settings: DrawSettings(); break;
        case MScreen::Devices:
            if (devicesSel.empty()) DrawDevices();
            else DrawDeviceDetails(devicesSel);
            break;
    }
    ImGui::EndChild();
    if (mini) DrawMusicMini();
    if (navH > 0) DrawMobileNav();
}

void App::DrawMobileHome() {
    ui::TouchScroll();
    auto peers = SortedPeers();
    if (peers.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("Nobody here yet.\n\nDevices running LunaShare on your network show up by themselves. Anyone else: Add someone."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ui::PushAccent();
        if (ui::BigButton(T("Add someone"), Icon::Plus)) OpenAddPerson(0);
        ui::PopAccent();
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
        if (ui::BigButton(T("Add someone"), Icon::Plus)) OpenAddPerson(-1);
        if (ui::BigButton(T("New group"), Icon::Users)) OpenNewGroup();
    }
    DrawWatchResume();
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
        ui::PushAccent(active);
        if (ImGui::Button("##nav", ImVec2(w, h))) {
            screen = items[i].s;
            backStack.clear();
        }
        ui::PopAccent();
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
        DrawAvatar(ImGui::GetWindowDrawList(), ImVec2(ap.x + ad * 0.5f, ap.y + ad * 0.4f), ad, eng.IsSelfKey(peer) ? std::string() : peer,
                   eng.IsSelfKey(peer) ? eng.settings.displayName : eng.PeerName(peer));
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
    } else if (eng.IsSelfKey(peer) && eng.DevicesOf(peer).empty()) {
        ImGui::TextDisabled("%s", T("notes for yourself - files you add stay on this device"));
    } else if (connected) ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.45f, 1), T("connected via %s%s"), conn ? T(TransportName(conn->transport)) : "?",
                                      conn && conn->addr == "relay" ? T(" (relay through the server)") : "");
    else if (eng.IsOnline(peer)) ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1), "%s", T("online - connecting..."));
    else if (eng.WanStatus().rfind("connected", 0) == 0) ImGui::TextDisabled("%s", T("offline - messages wait on your server, files are queued"));
    else ImGui::TextDisabled("%s", T("offline - messages and files are queued"));
    if (auto cvt = eng.chats.find(peer); cvt != eng.chats.end() && cvt->second.ttl > 0) {  // disappearing
        ImGui::SameLine();
        int t = cvt->second.ttl;
        ui::Badge(t == 3600 ? T("disappear: 1 hour") : t == 86400 ? T("disappear: 1 day") : T("disappear: 1 week"), ui::ColWarn());
    }
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
        bool selfChat = eng.IsSelfKey(peer);  // (my own chat: no connect / device details)
        int nb = (grp || selfChat ? 3 : 5) + (callable ? 1 : 0) + (videoable ? 1 : 0) + (webcam ? 1 : 0) + (watchable ? 1 : 0);
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
        if (ui::IconButton("chatsearch", Icon::ZoomIn, T("Search messages"), bw)) OpenChatSearch();
        ImGui::SameLine();
        if (ui::IconButton("chathist", Icon::History, T("Chat history: archive, clear, restore"), bw)) OpenChatHistory(peer);
        ImGui::SameLine();
        if (grp) {
            GroupButtons(peer, bw);
        } else {
        if (ui::IconButton("sendfiles", Icon::Attach, T("Send files or folders"))) {
            browseFor = BrowseFor::SendFiles;
            browser.Open(FileBrowser::Mode::OpenMulti, TF("Send to %s", eng.PeerName(peer).c_str()), browser.lastDir);
        }
        if (!selfChat) {
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
    }

    DrawWatchBanner(peer);
    DrawListenBanner(peer);
    DrawHistoryAskBanner(peer);
    DrawPinnedBar(peer);
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
        if (ImGui::Button(T("Forward"), ImVec2(0, h))) ForwardMessages(peer, std::vector<std::string>(selMsgs.begin(), selMsgs.end()));
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
    DrawChatSearch(peer);
    if (replyFor != peer) {  // (another chat: no answer / change in progress; its own unsent text)
        if (!replyFor.empty() && editMid.empty()) DraftSet(replyFor, chatInput);
        replyFor = peer;
        chatInput = DraftGet(peer);
        replyMid.clear();
        editMid.clear();
        if (const char* r = getenv("LUNASHARE_REPLYTO")) replyMid = r;  // dev: answering this message (screenshots)
    }
    float stripH = (!replyMid.empty() || !editMid.empty()) ? ImGui::GetFrameHeightWithSpacing() + (mobile ? ui::M().rowH * 0.2f : 0) : 0;
    // a group: "@" + the start of a name suggests the people in it
    std::vector<std::pair<std::string, std::string>> menMatch;  // account, name
    size_t menAt = std::string::npos;
    if (Engine::IsGroupKey(peer)) {
        size_t at = chatInput.rfind('@');
        if (at != std::string::npos && (at == 0 || chatInput[at - 1] == ' ' || chatInput[at - 1] == '\n') &&
            chatInput.find_first_of(" \n", at) == std::string::npos && chatInput.size() - at <= 32) {
            std::string pre = chatInput.substr(at + 1);
            for (auto& c : pre) c = (char)std::tolower((unsigned char)c);
            if (const Group* g = eng.GroupOf(peer))
                for (auto& mbr : g->members) {
                    if (mbr.acc == eng.MyAccount() || menMatch.size() >= 6) continue;
                    std::string nm = Emoji().Display(eng.PeerName("@" + mbr.acc)), low = nm;
                    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
                    if (low.rfind(pre, 0) == 0) menMatch.push_back({mbr.acc, nm});
                }
            if (!menMatch.empty()) menAt = at;
        }
    }
    float menH = menMatch.empty() ? 0 : (mobile ? ui::M().rowH * 0.8f : ImGui::GetFrameHeight()) + ImGui::GetStyle().ItemSpacing.y;
    stripH += menH;
    ImGui::BeginChild("messages", ImVec2(0, -(inputH + pickerH + stripH + ImGui::GetStyle().ItemSpacing.y * (emojiOpen ? 3 : 2))), ImGuiChildFlags_None);
    if (mobile) ui::TouchScroll();
    DrawMessages(peer);
    DrawReactionPopups(peer);
    ImGui::EndChild();
    ImGui::Separator();
    if (menH > 0) {  // @ + a name: the people it could be
        for (size_t i = 0; i < menMatch.size(); i++) {
            if (i) ImGui::SameLine();
            ImGui::PushID((int)i);
            if (ImGui::Button(("@" + menMatch[i].second).c_str(), ImVec2(0, menH - ImGui::GetStyle().ItemSpacing.y))) {
                chatInput = chatInput.substr(0, menAt) + "@" + menMatch[i].second + " ";
                focusInput = true;
                Emoji().CursorToEnd();
            }
            ImGui::PopID();
        }
        stripH -= menH;
    }
    if (stripH > 0) {  // answering / changing a message: which one, and x to stop
        const ChatMessage* q = nullptr;
        auto ch = eng.chats.find(peer);
        if (ch != eng.chats.end())
            for (auto& m : ch->second.messages)
                if (m.mid == (editMid.empty() ? replyMid : editMid)) q = &m;
        std::string what = !q ? std::string() : q->kind == MsgKind::Files ? std::string(T("files")) : Emoji().Display(q->text);
        for (auto& c : what)
            if (c == '\n') c = ' ';
        std::string who = !q ? "" : q->outgoing ? T("You") : Emoji().Display(eng.PeerName(q->from.empty() ? peer : q->from));
        std::string head = editMid.empty() ? TF("Answering %s:", who.c_str()) : std::string(T("Changing your message:"));
        float xw = mobile ? ui::M().rowH * 0.75f : ImGui::GetFrameHeight();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x - xw - ImGui::GetStyle().ItemSpacing.x;
        ImDrawList* sdl = ImGui::GetWindowDrawList();
        sdl->AddRectFilled(p, ImVec2(p.x + 3, p.y + stripH - ImGui::GetStyle().ItemSpacing.y), ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
        ImVec4 clip(p.x, p.y, p.x + w, p.y + stripH);
        std::string line = head + " " + what;
        sdl->AddText(nullptr, 0, ImVec2(p.x + 10, p.y + (stripH - ImGui::GetStyle().ItemSpacing.y - ImGui::GetTextLineHeight()) * 0.5f),
                     ImGui::GetColorU32(ImGuiCol_TextDisabled), line.c_str(), nullptr, 0, &clip);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w + ImGui::GetStyle().ItemSpacing.x, p.y));
        if (ui::IconButton("rpx", Icon::Cancel, T("Cancel"), xw)) {
            if (!editMid.empty()) chatInput.clear();
            replyMid.clear();
            editMid.clear();
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + stripH));
    }
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
    if (Engine::IsGroupKey(peer) && chatInput.empty() && editMid.empty()) {  // groups: a poll (while nothing is typed)
        float bw = mobile ? inputH * 0.9f : inputH * 0.8f;
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##pollbtn", ImVec2(bw, inputH))) OpenPollNew(peer);
        if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", T("New poll..."));
        // three bars of a chart
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        float s = std::min(bw, inputH) * 0.42f, cx = p.x + bw * 0.5f, by = p.y + inputH * 0.5f + s * 0.5f;
        ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        float bwid = s * 0.24f;
        pdl->AddRectFilled(ImVec2(cx - s * 0.5f, by - s * 0.55f), ImVec2(cx - s * 0.5f + bwid, by), col);
        pdl->AddRectFilled(ImVec2(cx - bwid * 0.5f, by - s), ImVec2(cx + bwid * 0.5f, by), col);
        pdl->AddRectFilled(ImVec2(cx + s * 0.5f - bwid, by - s * 0.75f), ImVec2(cx + s * 0.5f, by), col);
        ImGui::SameLine();
        emojiW += bw + ImGui::GetStyle().ItemSpacing.x;
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
            if (!editMid.empty()) {
                eng.EditMessage(peer, editMid, Emoji().Restore(t));
            } else {
                std::vector<std::string> men;  // (the people still @named in what's sent)
                if (const Group* g = eng.GroupOf(peer))
                    for (auto& mbr : g->members)
                        if (mbr.acc != eng.MyAccount() && t.find("@" + Emoji().Display(eng.PeerName("@" + mbr.acc))) != std::string::npos)
                            men.push_back(mbr.acc);
                eng.SendChat(peer, Emoji().Restore(t), replyMid, men);
                scrollToBottom = true;
            }
        }
        replyMid.clear();
        editMid.clear();
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

// ---------------------------------------------------------------- links in messages

struct TextLink {
    size_t a, b;      // bytes in the text
    std::string url;  // (www. gets https://)
};

// http://..., https://... and www.... (to the next space; a full stop or a bracket at the very end isn't part of it)
static std::vector<TextLink> FindLinks(const std::string& s) {
    std::vector<TextLink> out;
    auto low = [&](size_t i) { return (char)tolower((unsigned char)s[i]); };
    auto at = [&](size_t i, const char* w) {
        for (size_t k = 0; w[k]; k++)
            if (i + k >= s.size() || low(i + k) != w[k]) return false;
        return true;
    };
    for (size_t i = 0; i < s.size(); i++) {
        if (i > 0 && !isspace((unsigned char)s[i - 1]) && s[i - 1] != '(' && s[i - 1] != '<' && s[i - 1] != '"' && s[i - 1] != '\'') continue;
        bool http = at(i, "http://") || at(i, "https://");
        bool www = !http && at(i, "www.");
        if (!http && !www) continue;
        size_t e = i;
        while (e < s.size() && !isspace((unsigned char)s[e]) && s[e] != '"' && s[e] != '<' && s[e] != '>') e++;
        while (e > i && strchr(".,;:!?)'", s[e - 1])) {
            if (s[e - 1] == ')' && std::count(s.begin() + (long)i, s.begin() + (long)e, '(') >= std::count(s.begin() + (long)i, s.begin() + (long)e, ')'))
                break;  // (a bracket that belongs to it: wikipedia/Foo_(bar))
            e--;
        }
        size_t minLen = http ? (at(i, "https://") ? 9 : 8) : 5;
        if (e - i < minLen) continue;
        std::string u = s.substr(i, e - i);
        out.push_back({i, e, www ? "https://" + u : u});
        i = e;
    }
    return out;
}

// where each character of a wrapped text lands (the same way ImFont::RenderText wraps)
struct TextGlyph {
    size_t off, end;
    float x, y, w;
};
static std::vector<TextGlyph> LayoutText(ImFont* font, float size, const char* text, const char* text_end, float wrap) {
    std::vector<TextGlyph> g;
    const char* s = text;
    float x = 0, y = 0, lh = size;
    const char* eol = nullptr;
    while (s < text_end) {
        if (!eol) eol = ImFontCalcWordWrapPositionEx(font, size, s, text_end, wrap - x);
        if (s >= eol) {
            x = 0;
            y += lh;
            eol = nullptr;
            s = ImTextCalcWordWrapNextLineStart(s, text_end);
            continue;
        }
        unsigned int c = (unsigned int)*s;
        int n = c < 0x80 ? 1 : ImTextCharFromUtf8(&c, s, text_end);
        if (n <= 0) n = 1;
        if (c == '\n') {
            x = 0;
            y += lh;
            s += n;
            continue;
        }
        float w = font->CalcTextSizeA(size, FLT_MAX, 0, s, s + n).x;
        g.push_back({(size_t)(s - text), (size_t)(s - text + n), x, y, w});
        x += w;
        s += n;
    }
    return g;
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

    int64_t nowMs = SteadyMs();
    bool jumped = false;
    // my own chat: which of my devices wrote what (once there's more than this one)
    bool selfChat = eng.IsSelfKey(peer), devLabels = false;
    if (selfChat) {
        devLabels = !eng.MyOtherDevices().empty();
        for (auto& x : cv.messages) devLabels |= !x.fromDev.empty();
    }
    for (int i = 0; i < (int)cv.messages.size(); i++) {
        ChatMessage& m = cv.messages[(size_t)i];
        ImGui::PushID(m.mid.c_str());
        if (!searchJump.empty() && m.mid == searchJump) {  // (a search result: here, lit up for a moment)
            ImGui::SetScrollHereY(0.3f);
            searchJump.clear();
            chatFollow = false;
            jumped = true;
            searchFlash = m.mid;
            searchFlashAt = nowMs;
            scrollToBottom = false;
        }
        if (m.mid == searchFlash && nowMs - searchFlashAt < 2200) {
            float a = 1.0f - (float)(nowMs - searchFlashAt) / 2200.0f;
            ImVec2 sp0 = ImGui::GetCursorScreenPos();
            float lh = ImGui::GetTextLineHeightWithSpacing() * 2.6f;
            dl->AddRectFilled(ImVec2(sp0.x - 4, sp0.y - 2), ImVec2(sp0.x + avail + 4, sp0.y + lh), IM_COL32(255, 200, 60, (int)(70 * a)), 6);
        }
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
        static const char* devGrow = getenv("LUNASHARE_DEVGROW");  // dev: something above grows (a thumbnail loading)
        if (devGrow && i == 5) ImGui::Dummy(ImVec2(1, ImGui::GetTime() > 2.0 ? 300.0f : 10.0f));
        bool sel = selMsgs.count(m.mid) > 0;
        if (devLabels) {  // the device it was written on (once for a run of messages)
            const ChatMessage* prev = i > 0 ? &cv.messages[(size_t)i - 1] : nullptr;
            std::string who = m.outgoing ? std::string() : m.fromDev;
            if (!prev || prev->kind == MsgKind::System || (prev->outgoing ? std::string() : prev->fromDev) != who || m.ts - prev->ts > 600000) {
                std::string n = m.outgoing ? TF("%s (this device)", eng.settings.displayName.c_str()) : eng.PeerName(who);
                ImU32 col = (ImHashStr(who.empty() ? eng.settings.deviceId.c_str() : who.c_str()) & 0x007F7F7F) | 0xFF808080;
                ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
                float tw = ImGui::CalcTextSize(n.c_str()).x;
                ImGui::SetCursorPosX(m.outgoing ? ImGui::GetCursorPosX() + avail - tw - pad * 0.5f : ImGui::GetCursorPosX() + pad * 0.5f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", n.c_str());
                ImGui::PopFont();
            }
        }
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
        if (m.kind == MsgKind::Files || !m.pollQ.empty()) {
            float w = mobile ? avail * 0.97f : std::min(maxW, ImGui::GetFontSize() * 26.0f);
            ImGui::SetCursorPosX(m.outgoing ? x0 + avail - w : x0);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(w, pad * 0.3f));
            ImGui::Indent(pad);
            if (!m.pollQ.empty()) DrawPollCard(peer, m, w - pad * 2);
            else DrawBatchCard(peer, m, w - pad * 2);
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
            std::string meta = std::string(m.starred || m.pinned ? (m.starred && m.pinned ? "          " : "      ") : "") +  // (room for the marks)
                               (m.forwarded ? std::string(T("forwarded")) + "  " : std::string()) +
                               (m.edited ? std::string(T("changed")) + "  " : std::string()) + FormatTime(m.ts, false) +
                               (m.outgoing ? std::string("  ") + state : std::string());
            ImVec2 ms = ImGui::CalcTextSize(meta.c_str());
            float smallScale = 0.8f;
            // an answer: the message it answers, on top (who, and the start of it); a tap goes there
            float qh = 0, qw = 0;
            std::string qWho, qText;
            if (!m.replyTo.empty()) {
                const ChatMessage* q = nullptr;
                for (auto& x : cv.messages)
                    if (x.mid == m.replyTo) q = &x;
                qText = q ? (q->kind == MsgKind::Files ? std::string(T("files")) : q->text) : m.replyText;
                if (qText == "[files]") qText = T("files");
                for (auto& c : qText)
                    if (c == '\n') c = ' ';
                qText = Emoji().Display(qText);
                qWho = !q ? "" : q->outgoing ? T("You") : Emoji().Display(eng.PeerName(q->from.empty() ? peer : q->from));
                qh = ImGui::GetTextLineHeight() * (qWho.empty() ? 1.0f : 2.0f) + pad * 0.8f;
                qw = std::min(wrap, std::max(ImGui::CalcTextSize(qWho.c_str()).x, ImGui::CalcTextSize(qText.c_str()).x) + pad * 1.2f);
            }
            float bw = std::max(std::max(ts.x, qw), ms.x * smallScale) + pad * 2;
            float bh = ts.y + ms.y * smallScale + pad * 1.6f + qh;
            ImGui::SetCursorPosX(m.outgoing ? x0 + avail - bw : x0);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            // links: where they are (to draw them as links and open the one tapped)
            std::vector<TextLink> links = bigEmoji ? std::vector<TextLink>() : FindLinks(shown);
            std::vector<TextGlyph> glyphs;
            int linkHover = -1;
            ImVec2 tp(p0.x + pad, p0.y + pad * 0.7f + qh);
            if (!links.empty()) {
                glyphs = LayoutText(ImGui::GetFont(), textSize, shown.c_str(), shown.c_str() + shown.size(), wrap);
                ImVec2 mp = io.MousePos;
                for (auto& gl : glyphs)
                    if (mp.x >= tp.x + gl.x && mp.x < tp.x + gl.x + gl.w && mp.y >= tp.y + gl.y && mp.y < tp.y + gl.y + textSize)
                        for (size_t k = 0; k < links.size(); k++)
                            if (gl.off >= links[k].a && gl.off < links[k].b) linkHover = (int)k;
            }
            ImGui::InvisibleButton("##msg", ImVec2(bw, bh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            if (linkHover >= 0 && ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (!mobile) ImGui::SetTooltip("%s", links[(size_t)linkHover].url.c_str());
            }
            bool linkTap = linkHover >= 0 && ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(0) && !io.KeyCtrl && !io.KeyShift &&
                           !selectionMode && std::abs(ImGui::GetMouseDragDelta(0, 0).x) < 8 && std::abs(ImGui::GetMouseDragDelta(0, 0).y) < 8 &&
                           io.MouseDownDurationPrev[0] < 0.5f;
            if (linkTap) SDL_OpenURL(links[(size_t)linkHover].url.c_str());  // (the system's browser)
            bool hovered = ImGui::IsItemHovered();
            bool longPress = mobile && ImGui::IsItemActive() && io.MouseDownDuration[0] > 0.55f && ImGui::GetMouseDragDelta(0).y == 0 &&
                             ImGui::GetMouseDragDelta(0).x == 0;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || (ImGui::IsItemDeactivated() && false)) {}
            if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(0) && !longPress && !linkTap) {
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
            if (!m.outgoing && std::find(m.mentions.begin(), m.mentions.end(), eng.MyAccount()) != m.mentions.end())  // @me: an outline
                dl->AddRect(p0, ImVec2(p0.x + bw, p0.y + bh), ui::ColWarn(), pad, 0, 2.0f);
            if (qh > 0) {  // the message it answers
                ImVec2 q0(p0.x + pad * 0.6f, p0.y + pad * 0.5f), q1(p0.x + bw - pad * 0.6f, p0.y + pad * 0.3f + qh);
                bool qhov = ImGui::IsMouseHoveringRect(q0, q1);
                // (a shade of the bubble: darker on dark themes, lighter on the light one - the accent stays readable on it)
                bool lightTheme = ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x > 0.5f;
                ImU32 qbg = lightTheme ? IM_COL32(255, 255, 255, qhov ? 200 : 160) : IM_COL32(0, 0, 0, qhov ? 70 : 45);
                dl->AddRectFilled(q0, q1, qbg, pad * 0.4f);
                dl->AddRectFilled(q0, ImVec2(q0.x + 3, q1.y), ui::ColAccentText(), pad * 0.2f);
                ImVec4 qc(q0.x, q0.y, q1.x - 4, q1.y);
                float ty = q0.y + pad * 0.25f;
                if (!qWho.empty()) {
                    dl->AddText(nullptr, 0, ImVec2(q0.x + 9, ty), ui::ColAccentText(), qWho.c_str(), nullptr, 0, &qc);
                    ty += ImGui::GetTextLineHeight();
                }
                dl->AddText(nullptr, 0, ImVec2(q0.x + 9, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), qText.c_str(), nullptr, 0, &qc);
                if (qhov && ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(0) && !longPress && ImGui::GetMouseDragDelta(0, 0).y == 0 && !selectionMode)
                    searchJump = m.replyTo;  // (there)
            }
            if (glyphs.empty()) {
                dl->AddText(ImGui::GetFont(), textSize, tp, ImGui::GetColorU32(ImGuiCol_Text), shown.c_str(), nullptr, wrap);
            } else {  // with links: run by run, links coloured and underlined
                bool lightLinks = ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x > 0.5f;
                ImU32 tc = ImGui::GetColorU32(ImGuiCol_Text), lc = lightLinks ? IM_COL32(30, 90, 200, 255) : IM_COL32(110, 175, 255, 255),
                      lch = lightLinks ? IM_COL32(20, 60, 160, 255) : IM_COL32(160, 205, 255, 255);
                size_t k = 0;
                while (k < glyphs.size()) {
                    int li = -1;
                    for (size_t q = 0; q < links.size(); q++)
                        if (glyphs[k].off >= links[q].a && glyphs[k].off < links[q].b) li = (int)q;
                    size_t e = k + 1;
                    while (e < glyphs.size() && glyphs[e].y == glyphs[k].y) {
                        int lj = -1;
                        for (size_t q = 0; q < links.size(); q++)
                            if (glyphs[e].off >= links[q].a && glyphs[e].off < links[q].b) lj = (int)q;
                        if (lj != li) break;
                        e++;
                    }
                    const TextGlyph &g0 = glyphs[k], &g1 = glyphs[e - 1];
                    ImVec2 rp(tp.x + g0.x, tp.y + g0.y);
                    ImU32 col = li < 0 ? tc : li == linkHover ? lch : lc;
                    dl->AddText(ImGui::GetFont(), textSize, rp, col, shown.c_str() + g0.off, shown.c_str() + g1.end);
                    if (li >= 0) {
                        float uy = rp.y + textSize - 1;
                        dl->AddLine(ImVec2(rp.x, uy), ImVec2(tp.x + g1.x + g1.w, uy), col, 1.0f);
                    }
                    k = e;
                }
            }
            ImU32 metaCol = m.outgoing && !m.delivered && m.mailedTo.empty() ? ui::ColWarn()
                          : m.outgoing && m.read ? IM_COL32(90, 170, 255, 255) : ui::ColMuted();
            ImVec2 mp(m.outgoing ? p0.x + bw - pad - ms.x * smallScale : p0.x + pad, p0.y + pad * 0.7f + qh + ts.y + pad * 0.2f);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * smallScale, mp, metaCol, meta.c_str());
            {  // starred / pinned: small marks in front of the time
                float isz = ImGui::GetFontSize() * smallScale, ix = mp.x + isz * 0.55f, iy = mp.y + isz * 0.55f;
                if (m.starred) {
                    ui::DrawIcon(dl, ui::Icon::Star, ImVec2(ix, iy), isz, IM_COL32(240, 200, 60, 255));
                    ix += isz * 1.3f;
                }
                if (m.pinned) DrawPin(dl, ImVec2(ix, iy), isz, metaCol);
            }
            ReactHoverButton(m, p0, ImVec2(p0.x + bw, p0.y + bh));
            DrawReactions(peer, m, p0, ImVec2(p0.x + bw, p0.y + bh));
            ImGui::Dummy(ImVec2(0, pad * 0.15f));
        }

        if (contextMid == m.mid && ImGui::BeginPopup("msgctx")) {
            float h = mobile ? ui::M().rowH * 0.85f : 0;
            if (QuickReactionRow(peer, m.mid, m.myReaction)) ImGui::CloseCurrentPopup();
            ImGui::Separator();
            if (m.kind != MsgKind::System && ImGui::Selectable(T("Reply"), false, 0, ImVec2(0, h))) {
                replyMid = m.mid;
                editMid.clear();
                focusInput = true;
                selMsgs.clear();
                selectionMode = false;
            }
            if (m.outgoing && m.kind == MsgKind::Text && m.pollQ.empty() && ImGui::Selectable(T("Change..."), false, 0, ImVec2(0, h))) {
                editMid = m.mid;
                replyMid.clear();
                chatInput = Emoji().Display(m.text);
                focusInput = true;
                Emoji().CursorToEnd();
                selMsgs.clear();
                selectionMode = false;
            }
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
            if (m.kind != MsgKind::System && ImGui::Selectable(T("Forward..."), false, 0, ImVec2(0, h))) ForwardMessages(peer, {m.mid});
            if (m.kind != MsgKind::System && ImGui::Selectable(m.pinned ? T("Unpin") : T("Pin in this chat"), false, 0, ImVec2(0, h)))
                eng.PinMessage(peer, m.mid, !m.pinned);
            if (ImGui::Selectable(m.starred ? T("Remove the star") : T("Star (keep in Starred messages)"), false, 0, ImVec2(0, h)))
                eng.StarMessage(peer, m.mid, !m.starred);
            if (selMsgs.size() > 1 && ImGui::Selectable(TF("Forward %d selected...", (int)selMsgs.size()).c_str(), false, 0, ImVec2(0, h)))
                ForwardMessages(peer, std::vector<std::string>(selMsgs.begin(), selMsgs.end()));
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    // Stay at the end while it's where the reader is: pictures, video thumbnails and long texts above keep growing
    // for a while after a chat opens (the first time after starting most of all). Scrolling up lets go; back at the
    // end, it holds again.
    float sy = ImGui::GetScrollY(), maxY = ImGui::GetScrollMaxY();
    if (!jumped) {
        if (scrollToBottom || sy >= maxY - 4.0f) chatFollow = true;
        else if (sy < chatLastScrollY - 1.0f) chatFollow = false;  // the reader scrolled up
        if (chatFollow && (scrollToBottom || sy < maxY - 0.5f)) ImGui::SetScrollHereY(1.0f);
        // Where the end really is this frame (things above grew, or the chat just opened): if the view isn't there,
        // this picture would flash - it's not shown, and the next frame (at the end) comes at once.
        ImGuiWindow* win = ImGui::GetCurrentWindow();  // (its content as ImGui will measure it: the next frame's end)
        float content = IM_TRUNC(win->DC.CursorMaxPos.y - win->DC.CursorStartPos.y);
        float endY = content + win->WindowPadding.y * 2 - (win->Size.y - win->ScrollbarSizes.y - win->TitleBarHeight - win->MenuBarHeight);
        // (only for a few frames after the end moved: should the two ever disagree for good, nothing spins)
        if (scrollToBottom || fabsf(endY - chatLastEndY) > 0.5f) chatSettle = 3;
        chatLastEndY = endY;
        if (chatFollow && chatSettle > 0 && endY > 0 && fabsf(sy - endY) > 1.0f) {
            discardFrame = true;
            busy = true;
        }
        if (chatSettle > 0) chatSettle--;
        if (static const char* lg = getenv("LUNASHARE_CHATLOG"); lg)
            fprintf(stderr, "chat t=%.3f sy=%.1f max=%.1f end=%.1f follow=%d discard=%d\n", ImGui::GetTime(), sy, maxY, endY, chatFollow, discardFrame);
    }
    scrollToBottom = false;
    chatLastScrollY = sy;
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
    DrawShareLinksList();
    std::vector<std::shared_ptr<Transfer>> list;
    for (auto& [id, t] : eng.transfers)
        if (t->peer != eng.settings.deviceId) list.push_back(t);  // (not files kept in my own chat)
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
        bool seen = o != eng.online.end();
        ImGui::TextDisabled("%s", seen ? T("Seen on the network but never connected.") : T("No device of this contact is known any more."));
        if (seen && ui::IconTextButton(T("Connect"), Icon::Link, ImVec2(0, h))) eng.ConnectPeer(peer);
        if (seen) ImGui::SameLine();
        if (ui::IconTextButton(T("Forget..."), Icon::Trash, ImVec2(0, h))) {
            forgetPeer = peer;
            openForget = true;
        }
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
        // or by QR code: each scans the other's
        if (ImGui::Button(T("Verify with QR codes..."), ImVec2(0, ui::M().mobile ? ui::M().rowH * 0.8f : 0))) {
            verifyQrOpen = true;
            verifyQrPeer = peer;
            verifyQrScan = false;
            verifyQrResult = 0;
            verifyQrMsg.clear();
        }
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
    DrawLockSettings(h);

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
    if (assoc::Supported()) {  // "Open with" (desktops; Android always has it)
        ImGui::SeparatorText(T("Opening files"));
        static int assocState = -1;  // (asked once, not every frame)
        if (assocState < 0) assocState = assoc::Registered() ? 1 : 0;
        bool on = assocState == 1;
        if (ImGui::Checkbox(T("Offer LunaShare for pictures, movies, songs, archives and comics"), &on)) {
            std::string err;
            if (on) {
                if (!assoc::Register(err)) {
                    infoMessage = TF("Couldn't register LunaShare: %s", err.c_str());
                    openInfo = true;
                }
            } else {
                assoc::Unregister();
            }
            assocState = assoc::Registered() ? 1 : 0;
        }
        ImGui::SameLine();
        ui::HelpMarker(T("LunaShare shows up in the file manager's \"Open with\" for these files (for you only). Started again while it "
                         "runs, it shows them in the running one. To open them with LunaShare on a double click, pick it as the "
                         "default app for them."));
        if (on && assoc::CanOpenSystemSettings() && ImGui::Button(T("Default apps..."), ImVec2(0, h))) assoc::OpenSystemSettings();
    }
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
        ImGui::BeginDisabled(!edit.autoAcceptTrusted && !edit.autoAcceptAll);
        ImGui::Indent();
        {
            const int sizes[] = {0, 10, 50, 100, 500, 1024, 4096};
            auto name = [](int mb) {
                return mb == 0 ? std::string(T("any size")) : mb >= 1024 ? TF("up to %d GB", mb / 1024) : TF("up to %d MB", mb);
            };
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T("Without asking:"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::BeginCombo("##aamax", name(edit.autoAcceptMaxMB).c_str())) {
                for (int mb : sizes)
                    if (ImGui::Selectable(name(mb).c_str(), edit.autoAcceptMaxMB == mb)) {
                        edit.autoAcceptMaxMB = mb;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ui::HelpMarker(T("A bigger file asks first, even from a trusted device."));
            if (Bridge().isMetered) changed |= ImGui::Checkbox(T("On mobile data, always ask first"), &edit.autoAcceptNotMetered);
        }
        ImGui::Unindent();
        ImGui::EndDisabled();
        changed |= ImGui::Checkbox(T("Resume interrupted transfers automatically"), &edit.autoResume);
        ImGui::SeparatorText(T("Sending"));
        changed |= ImGui::Checkbox(T("Send photos smaller"), &edit.photosSmaller);
        ImGui::SameLine();
        ui::HelpMarker(T("Photos (JPEG, PNG) over 0.7 MB go as a JPEG at most 2048 pixels wide - a few times smaller, still sharp on "
                         "a screen. Their EXIF data (camera, location) isn't copied. Videos and other files go as they are."));

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
        ImGui::TextDisabled("%s", T("Exports all settings plus your list of known devices with their nicknames, trust and block state, the "
                                    "music playlists, folders and sound settings, and where you stopped in movies."));
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
    {
        Settings us = eng.settings;
        if (ImGui::Checkbox(T("Check for a new version once a day"), &us.updateCheck)) {
            eng.ApplySettings(us);
            edit.updateCheck = us.updateCheck;
        }
        ImGui::SameLine();
        ui::HelpMarker(T("Asks GitHub (where LunaShare is published) about the newest release - nothing else is sent, and nothing is "
                         "downloaded by itself: a newer version shows a window with a button to its download page."));
        if (ImGui::Button(T("Check now"), ImVec2(0, h))) {
            updateAsked = true;
            UpdateCheckNow();
        }
        if (!updateTag.empty() && UpdateIsNewer(updateTag, LUNASHARE_VERSION)) {
            ImGui::SameLine();
            if (ImGui::Button(TF("Get %s", updateTag.c_str()).c_str(), ImVec2(0, h))) SDL_OpenURL(updateUrl.c_str());
        }
    }
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
                if (!selectedPeer.empty()) { SendFilesUI(selectedPeer, res); scrollToBottom = true; }
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
            case BrowseFor::MusicFolder:
                if (!res.empty()) music::Lib().AddFolder(res[0]);
                break;
            case BrowseFor::SyncFolder:
                if (!res.empty()) {
                    auto mine = eng.MyOtherDevices();
                    Settings fs = eng.settings;
                    fs.folderSyncs.push_back({RandomHex(6), res[0], mine.empty() ? "" : mine[0], true});
                    eng.ApplySettings(fs);
                    edit.folderSyncs = fs.folderSyncs;
                    eng.FolderSyncSoon();
                }
                break;
            case BrowseFor::Movie:
                if (!res.empty() && eng.StartWatch("", res[0])) watchShown = true;
                break;
            case BrowseFor::TorrentFile:
                if (!res.empty()) {
                    torrentAddSource = res[0];
                    torrentAddDir = Torrents().Available() ? Torrents().DefaultDir() : "";
                }
                torrentAddOpen = true;  // (back to the popup)
                break;
            case BrowseFor::TorrentDir:
                if (!res.empty()) torrentAddDir = res[0];
                torrentAddOpen = true;
                break;
            case BrowseFor::TorrentSaveDir:
                if (!res.empty()) {
                    s.torrentDir = res[0];
                    edit.torrentDir = res[0];
                }
                torrentSettingsOpen = true;
                break;
            case BrowseFor::ArchiveOpen:
                if (!res.empty()) OpenArchive(res[0]);
                break;
            case BrowseFor::ArchiveInputs:
                if (!res.empty()) StartArchiveCreate(res);
                break;
            case BrowseFor::ArchiveDest:
                if (!res.empty()) {
                    archiveExtractTo = res[0];
                    archiveExtractOpen = true;  // (back to the popup)
                }
                break;
            case BrowseFor::ArchiveCreateDest:
                if (!res.empty()) {
                    archiveCreateTo = res[0];
                    archiveCreateOpen = true;
                }
                break;
            case BrowseFor::ArchiveFolder:
                if (!res.empty() && std::find(s.archiveFolders.begin(), s.archiveFolders.end(), res[0]) == s.archiveFolders.end()) {
                    s.archiveFolders.push_back(res[0]);
                    edit.archiveFolders = s.archiveFolders;
                    archivesRescanWanted = true;
                    archivesScanned = false;
                }
                break;
            case BrowseFor::GalleryFolder:
                if (!res.empty() && std::find(s.galleryFolders.begin(), s.galleryFolders.end(), res[0]) == s.galleryFolders.end()) {
                    s.galleryFolders.push_back(res[0]);
                    edit.galleryFolders = s.galleryFolders;
                    galleryRescanWanted = true;
                    galleryScanned = false;
                }
                break;
            case BrowseFor::MovieFolder:
                if (!res.empty() && std::find(s.movieFolders.begin(), s.movieFolders.end(), res[0]) == s.movieFolders.end()) {
                    s.movieFolders.push_back(res[0]);
                    edit.movieFolders = s.movieFolders;
                    movieRescanWanted = true;
                    movieScanned = false;
                }
                break;
            case BrowseFor::DownloadDir:
                edit.downloadDir = res[0];
                s.downloadDir = res[0];
                break;
            case BrowseFor::Export:
                if (eng.ExportSettings(res[0], exportHistory, exportIdentity, &err, AppDataExport())) infoMessage = T("Settings exported to\n") + res[0];
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
                if (json ad; eng.ImportSettings(res[0], importIdentity, &err, &ad)) {
                    infoMessage = T("Settings imported from\n") + res[0];
                    int n = AppDataImport(ad);
                    if (n > 0) infoMessage += "\n" + TF("Playlists: %d", n);
                    styleGeneration++;
                    s = eng.settings;
                    s.lastBrowseDir = browser.lastDir;
                }
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
        if (!pendingForward.empty()) {
            ImGui::TextUnformatted(pendingShareText ? T("Send this text") : TF("Forward %d message(s)", (int)pendingForward.size()).c_str());
            for (size_t i = 0; i < pendingForward.size() && i < 3; i++) {
                std::string s = Emoji().Display(pendingForward[i]);
                if (s.size() > 60) {
                    s.resize(60);
                    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
                    if (!s.empty() && ((unsigned char)s.back() & 0x80)) s.pop_back();
                    s += "...";
                }
                for (auto& c : s)
                    if (c == '\n') c = ' ';
                ImGui::BulletText("%s", s.c_str());
            }
        }
        if (!pendingSend.empty() || pendingForward.empty())
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
            if (Engine::IsGroupKey(p) && !pendingSend.empty()) continue;  // (files go to people, not to groups)
            if (const Group* g = eng.GroupOf(p); g && g->left) continue;
            if (DevicePeerRow(p, false, mobile ? ui::M().rowH * 1.2f : 0)) {
                for (auto& t : pendingForward) eng.SendChat(p, t, "", {}, !pendingShareText);
                if (!pendingSendList.empty()) eng.SendFileList(p, pendingSendList, pendingSendName);
                else if (!pendingSend.empty()) SendFilesUI(p, pendingSend);
                pendingSendList.clear();
                pendingForward.clear();
                pendingShareText = false;
                SelectPeer(p);
                if (mobile) { backStack.clear(); backStack.push_back(MScreen::Home); screen = MScreen::Chat; }
                else { tab = Tab::Chat; tabForce = true; }
                pendingSend.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        if (peers.empty()) ImGui::TextDisabled("%s", T("No devices known yet."));
        ImGui::EndChild();
        bool linkable = !pendingSend.empty() && pendingForward.empty();
        if (linkable && ui::IconTextButton(T("Link for anyone..."), Icon::Link, ImVec2(mobile ? -1 : 0, h))) {
            OpenShareLink(pendingSend);
            pendingSend.clear();
            ImGui::CloseCurrentPopup();
        }
        if (linkable && !mobile) ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pendingSend.clear();
            pendingForward.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    DrawShareLinkPopups();

    // ---- forgetting a contact (a person: all their devices, their nicknames and the chat)
    if (openForget) { ImGui::OpenPopup("##forgetpeer"); openForget = false; }
    centerNext(0.35f);
    if (ImGui::BeginPopupModal("##forgetpeer", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize * !mobile)) {
        ImGui::PushTextWrapPos(mobile ? 0 : ImGui::GetFontSize() * 26);
        ImGui::TextUnformatted(TF("Forget %s? Their devices, nicknames and the chat history are removed from this device. "
                                  "If they connect again, they show up as new.", eng.PeerName(forgetPeer).c_str()).c_str());
        ImGui::PopTextWrapPos();
        float bw = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2 : 0;
        if (ImGui::Button(T("Forget"), ImVec2(bw, h))) {
            eng.ForgetDevice(forgetPeer);
            if (selectedPeer == forgetPeer || eng.ChatKey(selectedPeer) == forgetPeer) selectedPeer.clear();
            if (devicesSel == forgetPeer) devicesSel.clear();
            forgetPeer.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(bw, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
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
            ui::PushAccent(camPanel);  // (pushes the same count on or off)
            if (ui::IconButton("camset", Icon::Sliders, T("Call settings (sound, camera, quality...)"), bw, false, cap ? T("Settings##cap") : nullptr))
                camPanel = !camPanel;
            ui::PopAccent();
            ImGui::SameLine();
        }
        if (IsMobilePlatform()) {
            // Audio output: with only earpiece + loudspeaker it's a toggle; with a headset connected it opens a list.
            const MediaHost::Route* cur = Media().Current();
            bool spk = cur && cur->kind == "speaker";
            bool hl = cur && cur->kind != "earpiece";
            ui::PushAccent(hl);
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
            ui::PopAccent();
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
    if (invEnv && ImGui::GetFrameCount() == 30) openInvites = true;
    if (openInvites) {
        ImGui::OpenPopup("##invites");
        openInvites = false;
    }
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
        int r = UseInviteCode(code, msg);
        msgErr = r < 0;
        msgAt = 1;
        if (r > 0) changed = true;
        if (r >= 0) code.clear();
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

// ---------------------------------------------------------------- typing on a phone

// The on-screen keyboard covers the bottom of the screen - often the very field being typed in (a dialog, a popup).
// So while it's up, the text (with its cursor) is shown again in a strip just above it; tapping in the strip puts the
// cursor there. (The taps never reach ImGui - that would end the typing - see main.cpp.)
void App::DrawTextMirror() {
    ImGuiContext& g = *GImGui;
    ImGuiInputTextState* st = ImGui::GetInputTextState(g.ActiveId);
    bool show = keyboardInset > 0 && ImGui::GetIO().WantTextInput && st && st->Stb;
    if (show) {  // (only when the field's cursor is out of sight - under the keyboard; a field above it is fine as it is)
        ImGuiViewport* v = ImGui::GetMainViewport();
        float kbTop = v->WorkPos.y + v->WorkSize.y - keyboardInset;
        const ImGuiPlatformImeData& ime = g.PlatformImeData;
        bool visible = ime.WantVisible && ime.InputPos.y >= v->WorkPos.y && ime.InputPos.y + ime.InputLineHeight <= kbTop;
        bool shownBefore = mirrorRect[2] > mirrorRect[0];
        if (visible && !(shownBefore && ime.InputPos.y + ime.InputLineHeight > mirrorRect[1])) show = false;
    }
    if (!show) {
        mirrorRect[0] = mirrorRect[1] = mirrorRect[2] = mirrorRect[3] = 0;
        mirrorTapX = mirrorTapY = -1;
        mirrorFirstLine = 0;
        return;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImFont* font = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * 1.1f, lh = fs * 1.3f;
    float pad = ImGui::GetStyle().WindowPadding.x;
    float x0 = vp->WorkPos.x, x1 = vp->WorkPos.x + vp->WorkSize.x;
    float bottom = vp->WorkPos.y + vp->WorkSize.y - keyboardInset;
    float textW = x1 - x0 - pad * 2;
    // the characters, wrapped to the strip's width: where each one starts
    bool password = (st->Flags & ImGuiInputTextFlags_Password) != 0;
    const char* text = st->GetText();
    int len = st->TextLen;
    struct Ch { int off; float x; int line; };
    std::vector<Ch> chars;
    chars.reserve((size_t)len + 1);
    float x = 0;
    int line = 0;
    for (int i = 0; i < len;) {
        unsigned int c = 0;
        int n = ImTextCharFromUtf8(&c, text + i, text + len);
        if (n <= 0) n = 1;
        if (c == '\n') {
            chars.push_back({i, x, line});
            x = 0;
            line++;
            i += n;
            continue;
        }
        float w = password ? font->CalcTextSizeA(fs, FLT_MAX, 0, "*").x : font->CalcTextSizeA(fs, FLT_MAX, 0, text + i, text + i + n).x;
        if (x + w > textW && x > 0) {
            x = 0;
            line++;
        }
        chars.push_back({i, x, line});
        x += w;
        i += n;
    }
    chars.push_back({len, x, line});  // (the end: the cursor may stand there)
    int lines = line + 1;
    int show_lines = std::min(lines, 4);
    float h = show_lines * lh + pad * 2;
    float y0 = bottom - h;
    // the cursor's line stays in view
    int cur = st->GetCursorPos();
    int curLine = 0;
    float curX = 0;
    for (auto& c : chars)
        if (c.off <= cur) {
            curLine = c.line;
            curX = c.x;
        }
    if (curLine < mirrorFirstLine) mirrorFirstLine = curLine;
    if (curLine >= mirrorFirstLine + show_lines) mirrorFirstLine = curLine - show_lines + 1;
    mirrorFirstLine = std::clamp(mirrorFirstLine, 0, std::max(0, lines - show_lines));
    // a tap in the strip: the cursor goes to the nearest place between two characters
    if (mirrorTapX >= 0) {
        int tl = mirrorFirstLine + (int)((mirrorTapY - y0 - pad) / lh);
        tl = std::clamp(tl, mirrorFirstLine, mirrorFirstLine + show_lines - 1);
        float tx = mirrorTapX - x0 - pad;
        int best = -1;
        float bd = 1e9f;
        for (size_t k = 0; k < chars.size(); k++) {
            if (chars[k].line != tl) continue;
            float d = std::fabs(chars[k].x - tx);
            if (d < bd) {
                bd = d;
                best = chars[k].off;
            }
            // (the end of a wrapped line: just before the first character of the next one)
        }
        if (best >= 0) {
            st->SetSelection(best, best);
            st->CursorAnimReset();
            st->CursorFollow = true;
            cur = best;
        }
        mirrorTapX = mirrorTapY = -1;
    }
    for (auto& c : chars)
        if (c.off <= cur) {
            curLine = c.line;
            curX = c.x;
        }
    // draw it (on top of everything)
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, bottom), ImGui::GetColorU32(ImGuiCol_PopupBg));
    dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y0), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 2.0f);
    int sa = std::min(st->GetSelectionStart(), st->GetSelectionEnd()), sb = std::max(st->GetSelectionStart(), st->GetSelectionEnd());
    ImU32 tc = ImGui::GetColorU32(ImGuiCol_Text), selc = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
    for (size_t k = 0; k + 1 < chars.size(); k++) {
        const Ch& c = chars[k];
        if (c.line < mirrorFirstLine || c.line >= mirrorFirstLine + show_lines) continue;
        float cx = x0 + pad + c.x, cy = y0 + pad + (c.line - mirrorFirstLine) * lh + (lh - fs) * 0.5f;
        int n = chars[k + 1].off - c.off;
        if (text[c.off] == '\n') continue;
        if (c.off >= sa && c.off < sb) {
            float w = chars[k + 1].line == c.line ? chars[k + 1].x - c.x : fs * 0.5f;
            dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + w, cy + fs), selc);
        }
        if (password) dl->AddText(font, fs, ImVec2(cx, cy), tc, "*");
        else dl->AddText(font, fs, ImVec2(cx, cy), tc, text + c.off, text + c.off + n);
    }
    if (len == 0 && !password)
        dl->AddText(font, fs, ImVec2(x0 + pad, y0 + pad + (lh - fs) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), T("typing..."));
    if (fmodf((float)ImGui::GetTime(), 1.0f) < 0.6f || st->CursorAnim < 0) {  // the cursor (blinking)
        float cx = x0 + pad + curX, cy = y0 + pad + (curLine - mirrorFirstLine) * lh + (lh - fs) * 0.5f;
        dl->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + fs), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 2.5f);
    }
    mirrorRect[0] = x0;
    mirrorRect[1] = y0;
    mirrorRect[2] = x1;
    mirrorRect[3] = bottom;
}
