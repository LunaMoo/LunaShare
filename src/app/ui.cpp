#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "crypto.h"
#include "hash.h"
#include "image_cache.h"
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
        if (a == "home") screen = MScreen::Home;
        tabForce = true;
        autoSel = nullptr;
    }
    static const char* autoBrowse = getenv("LUNASHARE_BROWSE");
    static bool autoBrowseDone = false;
    if (autoBrowse && !autoBrowseDone) {
        autoBrowseDone = true;
        browseFor = BrowseFor::SendFiles;
        browser.Open(FileBrowser::Mode::OpenMulti, "Send to ...", autoBrowse);
        if (const char* n = getenv("LUNASHARE_BROWSE_SELECT")) browser.SelectFirstFiles(atoi(n));
    }
    static const char* autoView = getenv("LUNASHARE_AUTOVIEW");
    static bool autoViewDone = false;
    if (autoView && !autoViewDone && !selectedPeer.empty()) {
        for (auto& [id, t] : eng.transfers)
            if (t->state == TState::Completed && ImageCache::IsImagePath(t->localPath) && t->localPath.find(autoView) != std::string::npos) {
                OpenViewer(selectedPeer, t->localPath, false);
                autoViewDone = true;
                break;
            }
    }

    busy = false;
    int unread = 0;
    for (auto& [id, t] : eng.transfers)
        if (t->state == TState::Active || t->state == TState::Hashing || t->state == TState::Verifying) busy = true;
    for (auto& [peer, cv] : eng.chats) unread += cv.unread;
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
    if (m.mobile) DrawMobile();
    else DrawDesktop();
    DrawPopups();
    DrawViewer();
    ImGui::End();
    DrawToasts();
    Images().Update();

    // Ctrl+C copies selected chat messages (when no text field has focus)
    if (!selMsgs.empty() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) CopySelectedMessages(selectedPeer);
    if (!selMsgs.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        selMsgs.clear();
        selectionMode = false;
    }
}

void App::PullEvents() {
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
            case EventType::IncomingOffer:
                t.title = who + " wants to send";
                t.text = e.text;
                t.ttl = 20.0f;
                wantAttention = true;
                break;
            case EventType::TransferDone:
                if (e.peer == selectedPeer && (tab == Tab::Chat || screen == MScreen::Chat) && !ui::M().mobile) continue;
                t.title = "Transfer complete (verified)";
                t.text = e.text + (who.empty() ? "" : "  -  " + who);
                break;
            case EventType::TransferFailed:
                t.title = "Transfer problem";
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
                t.title = "Info";
                t.text = e.text;
                break;
            case EventType::Error:
                t.title = "Error";
                t.text = e.text;
                t.ttl = 8.0f;
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

std::vector<std::string> App::SortedPeers() {
    std::set<std::string> ids;
    for (auto& [id, d] : eng.devices) ids.insert(id);
    for (auto& [id, p] : eng.online) ids.insert(id);
    for (auto& c : eng.conns)
        if (c->ready && !c->closed) ids.insert(c->peerId);
    std::vector<std::string> v(ids.begin(), ids.end());
    auto rank = [&](const std::string& id) {
        auto d = eng.devices.find(id);
        if (d != eng.devices.end() && d->second.blocked) return 3;
        if (eng.IsConnected(id)) return 0;
        if (eng.online.count(id)) return 1;
        return 2;
    };
    auto lastSeen = [&](const std::string& id) {
        auto d = eng.devices.find(id);
        return d == eng.devices.end() ? 0 : d->second.lastSeen;
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
    if (selectedPeer != peer) {
        selMsgs.clear();
        selectionMode = false;
        lastSelIndex = -1;
    }
    selectedPeer = peer;
    eng.MarkRead(peer);
    scrollToBottom = true;
    if (!eng.IsConnected(peer)) eng.ConnectPeer(peer);
}

void App::OnDrop(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    auto lk = eng.Lock();
    bool chatVisible = !selectedPeer.empty() && ((!ui::M().mobile && tab == Tab::Chat) || (ui::M().mobile && screen == MScreen::Chat));
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

// ============================================================================ peer rows

bool App::DevicePeerRow(const std::string& peer, bool selected, float height) {
    ImGui::PushID(peer.c_str());
    auto d = eng.devices.find(peer);
    bool connected = eng.IsConnected(peer);
    bool online = eng.online.count(peer) > 0;
    bool blocked = d != eng.devices.end() && (d->second.blocked || !d->second.pendingKey.empty());
    std::string name = eng.PeerName(peer);
    std::string sub;
    if (d != eng.devices.end()) sub = d->second.platform;
    else if (online) sub = eng.online[peer].platform;
    auto conn = eng.ReadyConn(peer);
    if (connected && conn) sub += std::string(sub.empty() ? "" : "  ") + "via " + TransportName(conn->transport);
    else if (online) sub += "  online (" + eng.online[peer].Via() + ")";
    else sub += sub.empty() ? "offline" : "  offline";

    int unread = 0;
    auto cv = eng.chats.find(peer);
    if (cv != eng.chats.end()) unread = cv->second.unread;
    // aggregated live transfer progress
    int64_t tDone = 0, tSize = 0;
    for (auto& [id, t] : eng.transfers)
        if (t->peer == peer && t->state == TState::Active) { tDone += t->done.load(); tSize += t->size; }

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
    // avatar circle with initial
    float av = std::min(height * 0.8f, ImGui::GetFontSize() * 2.2f);
    ImVec2 ac(p0.x + pad + av * 0.5f, cy);
    ImU32 avCol = ImGui::GetColorU32(ImHashStr(peer.c_str()) | 0xFF000000) & 0xFF9F9F9F;
    avCol |= 0xFF404040;
    dl->AddCircleFilled(ac, av * 0.5f, avCol);
    char initial[8] = {};
    {
        // first UTF-8 character of the name
        int len = 1;
        unsigned char c0 = name.empty() ? '?' : (unsigned char)name[0];
        if (c0 >= 0xF0) len = 4; else if (c0 >= 0xE0) len = 3; else if (c0 >= 0xC0) len = 2;
        memcpy(initial, name.empty() ? "?" : name.c_str(), std::min<size_t>((size_t)len, name.empty() ? 1 : name.size()));
        if (len == 1) initial[0] = (char)toupper((unsigned char)initial[0]);
    }
    ImVec2 its = ImGui::CalcTextSize(initial);
    dl->AddText(ImVec2(ac.x - its.x * 0.5f, ac.y - its.y * 0.5f), IM_COL32_WHITE, initial);
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
    if (rightClick) ImGui::OpenPopup("peerctx");
    if (ImGui::BeginPopup("peerctx")) {
        float h = ui::M().mobile ? ui::M().rowH * 0.8f : 0;
        if (ImGui::Selectable(connected ? "Disconnect" : "Connect", false, 0, ImVec2(0, h))) connected ? eng.Disconnect(peer) : eng.ConnectPeer(peer);
        if (ImGui::Selectable("Rename...", false, 0, ImVec2(0, h))) {
            renamePeer = peer;
            renameBuf = d != eng.devices.end() ? d->second.customName : "";
            openRename = true;
        }
        bool trusted = d != eng.devices.end() && d->second.trusted;
        if (ImGui::Selectable(trusted ? "Untrust" : "Trust (auto-accept files)", false, 0, ImVec2(0, h))) eng.SetTrusted(peer, !trusted);
        if (ImGui::Selectable("Send files...", false, 0, ImVec2(0, h))) {
            SelectPeer(peer);
            browseFor = BrowseFor::SendFiles;
            browser.Open(FileBrowser::Mode::OpenMulti, "Send to " + name, browser.lastDir);
        }
        if (ImGui::Selectable("Device details", false, 0, ImVec2(0, h))) {
            devicesSel = peer;
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
        std::string tl = active ? "Transfers (" + std::to_string(active) + ")###transfers" : "Transfers###transfers";
        item("Chat", Tab::Chat);
        item(tl.c_str(), Tab::Transfers);
        item("History", Tab::History);
        item("Devices", Tab::Devices);
        item("Connect", Tab::Nearby);
        item("Settings", Tab::Settings);
        tabForce = false;
        ImGui::EndTabBar();
    }
    switch (tab) {
        case Tab::Chat:
            if (selectedPeer.empty()) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                const char* t1 = "Select a device on the left to chat and share files.";
                const char* t2 = "Drop files anywhere on this window to send them.";
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
        case Tab::Devices: DrawDevices(); break;
        case Tab::Nearby: DrawNearby(); break;
        case Tab::Settings: DrawSettings(); break;
    }
    ImGui::EndChild();
}

void App::DrawSidebar() {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted("LunaShare");
    ImGui::PopFont();
    ImGui::TextDisabled("%s", eng.settings.displayName.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("This device\nID: %s\nKey fingerprint: %s\nPort: %u", eng.settings.deviceId.c_str(), KeyFingerprint(eng.MyPublicKey()).c_str(), eng.boundPort);
    }
    ImGui::Separator();
    auto peers = SortedPeers();
    float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("peers", ImVec2(0, -footer));
    if (peers.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Looking for devices on your network...\n\nStart LunaShare on another device in the same Wi-Fi/LAN (or phone hotspot). It appears here automatically.");
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
    if (ui::IconTextButton("Connect...", Icon::Link, ImVec2(-1, 0))) {
        tab = Tab::Nearby;
        tabForce = true;
    }
    double up = 0, down = 0;
    for (auto& [id, t] : eng.transfers)
        if (t->state == TState::Active) (t->outgoing ? up : down) += t->speed;
    ImGui::TextDisabled("Up %s/s   Down %s/s", FormatBytes(up).c_str(), FormatBytes(down).c_str());
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
    }
    const char* title = "LunaShare";
    std::string peerTitle;
    switch (screen) {
        case MScreen::Home: title = "LunaShare"; break;
        case MScreen::Chat: peerTitle = eng.PeerName(selectedPeer); title = peerTitle.c_str(); break;
        case MScreen::Transfers: title = "Transfers"; break;
        case MScreen::History: title = "Shared files"; break;
        case MScreen::Nearby: title = "Connect"; break;
        case MScreen::Settings: title = "Settings"; break;
        case MScreen::Devices: title = "Device"; break;
    }
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - ImGui::GetFontSize() * 1.3f) * 0.5f);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (screen == MScreen::Chat) {
        ImGui::SameLine(ImGui::GetContentRegionMax().x - rowH);
        if (ui::IconButton("attach", Icon::Attach, nullptr, rowH)) {
            browseFor = BrowseFor::SendFiles;
            browser.Open(FileBrowser::Mode::OpenMulti, "Send to " + peerTitle, browser.lastDir);
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
        ImGui::TextDisabled("Looking for devices...\n\nOpen LunaShare on another phone or computer connected to the same Wi-Fi. "
                            "No Wi-Fi? Use Connect > Hotspot or Bluetooth.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ui::BigButton("Connect / Hotspot / Bluetooth", Icon::Link)) screen = MScreen::Nearby;
    }
    for (auto& p : peers) {
        if (DevicePeerRow(p, false, ui::M().rowH * 1.35f)) {
            SelectPeer(p);
            backStack.clear();
            backStack.push_back(MScreen::Home);
            screen = MScreen::Chat;
        }
    }
}

void App::DrawMobileNav() {
    ImGui::Separator();
    struct Item { const char* label; Icon icon; MScreen s; };
    Item items[] = {{"Devices", Icon::Users, MScreen::Home}, {"Transfers", Icon::Transfers, MScreen::Transfers},
                    {"Files", Icon::History, MScreen::History}, {"Connect", Icon::Link, MScreen::Nearby},
                    {"Settings", Icon::Gear, MScreen::Settings}};
    float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4) / 5.0f;
    float h = ui::M().rowH * 1.25f;
    for (int i = 0; i < 5; i++) {
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
        dl->AddText(ImVec2(cx - ts.x * 0.5f, mx.y - ts.y - h * 0.08f), ImGui::GetColorU32(ImGuiCol_Text), items[i].label);
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
    auto d = eng.devices.find(peer);
    if (!mobile) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
        ImGui::TextUnformatted(eng.PeerName(peer).c_str());
        ImGui::PopFont();
        ImGui::SameLine();
    }
    auto conn = eng.ReadyConn(peer);
    if (connected) ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.45f, 1), "connected via %s", conn ? TransportName(conn->transport) : "?");
    else if (eng.online.count(peer)) ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1), "online - connecting...");
    else ImGui::TextDisabled("offline - messages and files are queued");
    if (d != eng.devices.end() && !d->second.pendingKey.empty()) {
        ImGui::SameLine();
        ui::Badge("KEY CHANGED", ui::ColErr());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This device presented a different identity key. Review it in the device details.");
    } else if (connected) {
        ImGui::SameLine();
        bool verified = d != eng.devices.end() && d->second.verified;
        ui::Badge(verified ? "encrypted - verified" : "encrypted", verified ? IM_COL32(40, 150, 90, 255) : IM_COL32(70, 90, 140, 255));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("End-to-end encrypted (X25519 + XChaCha20-Poly1305).\nSafety number: %s\n%s", eng.SafetyNumberWith(peer).c_str(),
                              verified ? "You verified this device." : "Compare the safety number on both devices (Device details) to verify.");
    }
    if (d != eng.devices.end() && d->second.trusted) {
        ImGui::SameLine();
        ui::Badge("trusted", IM_COL32(60, 120, 200, 255));
    }
    if (!mobile) {
        float bw = ImGui::GetFrameHeight();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - bw * 3 - ImGui::GetStyle().ItemSpacing.x * 2);
        if (ui::IconButton("sendfiles", Icon::Attach, "Send files or folders")) {
            browseFor = BrowseFor::SendFiles;
            browser.Open(FileBrowser::Mode::OpenMulti, "Send to " + eng.PeerName(peer), browser.lastDir);
        }
        ImGui::SameLine();
        if (connected ? ui::IconButton("disc", Icon::Cancel, "Disconnect") : ui::IconButton("conn", Icon::Link, "Connect now"))
            connected ? eng.Disconnect(peer) : eng.ConnectPeer(peer);
        ImGui::SameLine();
        if (ui::IconButton("details", Icon::Gear, "Device details / rename")) {
            devicesSel = peer;
            tab = Tab::Devices;
            tabForce = true;
        }
    }

    // selection toolbar
    if (!selMsgs.empty()) {
        float h = mobile ? ui::M().rowH : 0;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d selected", (int)selMsgs.size());
        ImGui::SameLine();
        if (ImGui::Button("Copy", ImVec2(0, h))) CopySelectedMessages(peer);
        ImGui::SameLine();
        if (ImGui::Button("Select all", ImVec2(0, h))) {
            auto it = eng.chats.find(peer);
            if (it != eng.chats.end())
                for (auto& m : it->second.messages) selMsgs.insert(m.mid);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(0, h))) {
            selMsgs.clear();
            selectionMode = false;
        }
    }
    ImGui::Separator();

    float inputH = mobile ? ui::M().rowH : ImGui::GetFrameHeight() * 2.2f;
    ImGui::BeginChild("messages", ImVec2(0, -(inputH + ImGui::GetStyle().ItemSpacing.y * 2)), ImGuiChildFlags_None);
    if (mobile) ui::TouchScroll();
    DrawMessages(peer);
    ImGui::EndChild();
    ImGui::Separator();

    // input row
    float sendW = mobile ? ui::M().rowH * 1.4f : ImGui::GetFontSize() * 6;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - sendW - ImGui::GetStyle().ItemSpacing.x);
    if (focusInput) {
        ImGui::SetKeyboardFocusHere();
        focusInput = false;
    }
    bool send = ImGui::InputTextMultiline("##input", &chatInput, ImVec2(ImGui::GetContentRegionAvail().x - sendW - ImGui::GetStyle().ItemSpacing.x, inputH),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_WordWrap);
    if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("Enter = send, Ctrl+Enter = new line, drop files to send");
    ImGui::SameLine();
    if (ui::IconTextButton("Send", Icon::Send, ImVec2(sendW, inputH))) send = true;
    if (send) {
        std::string t = chatInput;
        while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
        if (!t.empty()) {
            eng.SendChat(peer, t);
            scrollToBottom = true;
        }
        chatInput.clear();
        if (!mobile) focusInput = true;
    }
}

void App::DrawMessages(const std::string& peer) {
    auto it = eng.chats.find(peer);
    if (it == eng.chats.end() || it->second.messages.empty()) {
        ImGui::TextDisabled("No messages yet. Say hi or drop some files here.");
        return;
    }
    Conversation& cv = it->second;
    cv.unread = 0;
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
            ImGui::TextDisabled("%s", m.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopID();
            continue;
        }
        bool sel = selMsgs.count(m.mid) > 0;
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
            // selection by clicking the card background
            if (ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
                if (ImGui::IsMouseClicked(1)) { contextMid = m.mid; ImGui::OpenPopup("msgctx"); }
                if (ImGui::IsMouseClicked(0) && (io.KeyCtrl || selectionMode)) { sel ? (void)selMsgs.erase(m.mid) : (void)selMsgs.insert(m.mid); }
            }
            ImGui::Dummy(ImVec2(0, pad * 0.3f));
        } else {
            float wrap = maxW - pad * 2;
            ImVec2 ts = ImGui::CalcTextSize(m.text.c_str(), nullptr, false, wrap);
            std::string meta = FormatTime(m.ts, false) + (m.outgoing ? (m.delivered ? "  delivered" : "  queued") : "");
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
                textViewBuf = m.text;
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
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p0.x + pad, p0.y + pad * 0.7f), ImGui::GetColorU32(ImGuiCol_Text), m.text.c_str(), nullptr, wrap);
            ImU32 metaCol = m.outgoing && !m.delivered ? ui::ColWarn() : ui::ColMuted();
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * smallScale,
                        ImVec2(m.outgoing ? p0.x + bw - pad - ms.x * smallScale : p0.x + pad, p0.y + pad * 0.7f + ts.y + pad * 0.2f), metaCol, meta.c_str());
            ImGui::Dummy(ImVec2(0, pad * 0.15f));
        }

        if (contextMid == m.mid && ImGui::BeginPopup("msgctx")) {
            float h = mobile ? ui::M().rowH * 0.85f : 0;
            if (ImGui::Selectable("Copy text", false, 0, ImVec2(0, h))) {
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
            if (m.kind == MsgKind::Text && ImGui::Selectable("Select text...", false, 0, ImVec2(0, h))) {
                textViewBuf = m.text;
                openTextView = true;
            }
            bool s2 = selMsgs.count(m.mid) > 0;
            if (ImGui::Selectable(s2 ? "Unselect" : "Select", false, 0, ImVec2(0, h))) {
                if (s2) selMsgs.erase(m.mid); else selMsgs.insert(m.mid);
                selectionMode = mobile && !selMsgs.empty();
            }
            if (!selMsgs.empty() && ImGui::Selectable(("Copy " + std::to_string(selMsgs.size()) + " selected").c_str(), false, 0, ImVec2(0, h)))
                CopySelectedMessages(peer);
            if (m.kind == MsgKind::Files) {
                if (ImGui::Selectable("Share these files with...", false, 0, ImVec2(0, h))) {
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
    toasts.push_back({EventType::Info, "Copied", std::to_string(count) + " message(s) copied to clipboard", "", 0, 2.5f});
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
    ImGui::TextUnformatted(m.text.c_str());
    ImGui::PopTextWrapPos();
    if (items.empty()) {
        ImGui::TextDisabled("(transfer details removed)");
        return;
    }
    ImGui::TextDisabled("%s%s  -  %s", m.outgoing ? "Sending " : "Receiving ", FormatBytes((double)total).c_str(),
                        items.size() > 1 ? (std::to_string(items.size()) + " files").c_str() : TStateName(items[0]->state));
    DrawThumbnails(peer, items, width);
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
        ImGui::TextUnformatted(allVerified ? "Delivered - SHA-256 verified on both sides" : "Delivered");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && items.size() == 1) ImGui::SetTooltip("SHA-256 before: %s\nSHA-256 after:  %s", items[0]->sha256.c_str(), items[0]->recvSha.c_str());
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
        if (ui::IconTextButton("Accept", Icon::Check, ImVec2(0, h))) eng.AcceptBatch(m.batch);
        sl();
        if (ui::IconTextButton("Decline", Icon::Cancel, ImVec2(0, h)))
            for (auto& t : items) if (t->state == TState::Pending) eng.Reject(t->tid);
    }
    if (nActive || nHash) {
        sl();
        if (ui::IconTextButton("Pause", Icon::Pause, ImVec2(0, h)))
            for (auto& t : items) eng.Pause(t->tid);
    }
    if (nPaused) {
        sl();
        if (ui::IconTextButton("Resume", Icon::Play, ImVec2(0, h)))
            for (auto& t : items) eng.Resume(t->tid);
    }
    if (nActive || nPaused || nHash || (m.outgoing && nPending)) {
        sl();
        if (ui::IconTextButton("Cancel", Icon::Cancel, ImVec2(0, h)))
            for (auto& t : items) eng.Cancel(t->tid);
    }
    if (m.outgoing && nFail) {
        sl();
        if (ui::IconTextButton("Retry", Icon::Retry, ImVec2(0, h)))
            for (auto& t : items) if (t->state == TState::Failed || t->state == TState::Cancelled) eng.Retry(t->tid);
    }
    if (nDone && items.size() == 1) {
        sl();
        if (ui::IconTextButton("Open", Icon::Open, ImVec2(0, h))) OpenPath(items[0]->localPath);
        sl();
        if (ui::IconTextButton("Folder", Icon::Folder, ImVec2(0, h))) RevealPath(items[0]->localPath);
    } else if (nDone && !m.outgoing) {
        sl();
        std::string dir = JoinPath(eng.settings.downloadDir, items[0]->rel.substr(0, items[0]->rel.find('/')));
        if (ui::IconTextButton("Folder", Icon::Folder, ImVec2(0, h))) RevealPath(items[0]->localPath.empty() ? eng.settings.downloadDir : DirName(items[0]->localPath));
    }
    if (nDone) {
        sl();
        if (ui::IconTextButton("Share", Icon::Share, ImVec2(0, h))) {
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
        if (ImGui::SmallButton(open ? "Hide files" : "Show files")) {
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
                snprintf(buf, sizeof(buf), "%s  %s", TStateName(t->state), FormatBytes((double)t->size).c_str());
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
            sl(); if (ui::IconButton("acc", Icon::Check, "Accept", sz)) eng.Accept(t->tid);
            sl(); if (ui::IconButton("rej", Icon::Cancel, "Decline", sz, true)) eng.Reject(t->tid);
            break;
        case TState::Active: case TState::Queued: case TState::Offered:
            sl(); if (ui::IconButton("pause", Icon::Pause, "Pause", sz)) eng.Pause(t->tid);
            sl(); if (ui::IconButton("cancel", Icon::Cancel, "Cancel", sz, true)) eng.Cancel(t->tid);
            break;
        case TState::Paused: case TState::Interrupted:
            sl(); if (ui::IconButton("resume", Icon::Play, "Resume", sz)) eng.Resume(t->tid);
            sl(); if (ui::IconButton("cancel", Icon::Cancel, "Cancel", sz, true)) eng.Cancel(t->tid);
            break;
        case TState::Hashing: case TState::Verifying:
            sl(); if (ui::IconButton("cancel", Icon::Cancel, "Cancel", sz, true)) eng.Cancel(t->tid);
            sl(); ImGui::Dummy(ImVec2(sz, sz));
            break;
        case TState::Completed:
            sl(); if (ui::IconButton("open", Icon::Open, "Open", sz)) OpenPath(t->localPath);
            sl(); if (ui::IconButton("folder", Icon::Folder, "Show in folder", sz)) RevealPath(t->localPath);
            break;
        default:
            if (t->outgoing) { sl(); if (ui::IconButton("retry", Icon::Retry, "Retry", sz)) eng.Retry(t->tid); }
            sl(); if (ui::IconButton("remove", Icon::Trash, "Remove from list", sz)) eng.RemoveTransfer(t->tid);
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
        snprintf(buf, sizeof(buf), "%s  %.1f%%  %s", TStateName(t->state), frac * 100, FormatBytes((double)t->size).c_str());
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
    if (ui::IconTextButton("Pause all", Icon::Pause, ImVec2(0, h))) eng.PauseAll(true);
    ImGui::SameLine();
    if (ui::IconTextButton("Resume all", Icon::Play, ImVec2(0, h))) eng.PauseAll(false);
    ImGui::SameLine();
    if (ui::IconTextButton("Clear finished", Icon::Trash, ImVec2(0, h))) eng.ClearFinishedTransfers();
    ImGui::Separator();
    std::vector<std::shared_ptr<Transfer>> list;
    for (auto& [id, t] : eng.transfers) list.push_back(t);
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) {
        bool fa = TStateFinal(a->state), fb = TStateFinal(b->state);
        if (fa != fb) return !fa;
        return a->created > b->created;
    });
    if (list.empty()) {
        ImGui::TextDisabled("No transfers. Drop files on the window or use the + button in a chat.");
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
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch, 2.6f);
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
                    ImGui::SetTooltip("%s\nSHA-256 (sender): %s\nSHA-256 (receiver): %s%s%s", t->localPath.empty() ? t->rel.c_str() : t->localPath.c_str(),
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
    ImGui::InputTextWithHint("##hfilter", "Search name, device or hash", &historyFilter);
    if (!mobile) ImGui::SameLine();
    ImGui::BeginDisabled(historySel.empty());
    std::string shareLabel = "Share selected (" + std::to_string(historySel.size()) + ")";
    if (ui::IconTextButton(shareLabel.c_str(), Icon::Share, ImVec2(0, h))) {
        std::vector<std::string> paths;
        for (auto& e : eng.history)
            if (historySel.count(e.tid) && FileExists(e.path)) paths.push_back(e.path);
        if (paths.empty()) toasts.push_back({EventType::Error, "Nothing to share", "Selected files no longer exist on this device", "", 0, 5});
        else OpenSendTo(paths);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::IconTextButton("Clear", Icon::Trash, ImVec2(0, h))) ImGui::OpenPopup("clearhist");
    if (ImGui::BeginPopup("clearhist")) {
        ImGui::TextUnformatted("Remove all entries from the shared files history?\n(The files themselves are not deleted.)");
        if (ImGui::Button("Clear history")) { eng.ClearHistory(); historySel.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
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
        ImGui::TextDisabled(eng.history.empty() ? "Files you send or receive will be listed here so you can share them again." : "No matches.");
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
            if (ui::IconButton("view", Icon::Star, "View picture", sz)) OpenViewer(e.peer, e.path, true);
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
        if (ui::IconButton("share", Icon::Share, "Share again with...", sz)) OpenSendTo({e.path});
        ImGui::SameLine();
        if (ui::IconButton("open", Icon::Open, "Open", sz)) OpenPath(e.path);
        ImGui::SameLine();
        if (ui::IconButton("folder", Icon::Folder, "Show in folder", sz)) RevealPath(e.path);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ui::IconButton("hash", Icon::Copy, "Copy SHA-256", sz)) ImGui::SetClipboardText(e.sha256.c_str());
        ImGui::SameLine();
        if (ui::IconButton("rm", Icon::Trash, "Remove from history", sz, true)) removeIdx = idx;
    };
    if (mobile) {
        for (int idx : rows) {
            auto& e = eng.history[(size_t)idx];
            ImGui::PushID(idx);
            bool sel = historySel.count(e.tid) > 0;
            if (ImGui::Checkbox("##sel", &sel)) { if (sel) historySel.insert(e.tid); else historySel.erase(e.tid); }
            ImGui::SameLine();
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TextDisabled("%s %s  -  %s  -  %s %s", e.outgoing ? "Sent to" : "From", eng.PeerName(e.peer).c_str(), FormatBytes((double)e.size).c_str(),
                                FormatTime(e.ts).c_str(), e.verified ? "- verified" : "");
            if (!FileExists(e.path)) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ColWarn()), "File moved or deleted");
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
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 2.5f);
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthStretch, 1.0f);
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
                    ImGui::SetTooltip("%s%s\nSHA-256: %s\n%s", e.path.c_str(), exists ? "" : "  (missing)", e.sha256.c_str(), e.verified ? "Verified on both sides" : "");
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
    auto peers = SortedPeers();
    if (peers.empty()) {
        ImGui::TextDisabled("No devices yet.");
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
        ImGui::TextDisabled("Seen on the network but never connected.");
        if (ui::IconTextButton("Connect", Icon::Link, ImVec2(0, h))) eng.ConnectPeer(peer);
        return;
    }
    KnownDevice& d = it->second;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextUnformatted(d.DisplayName().c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("Announces itself as \"%s\"", d.name.c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted("Nickname (only on this device)");
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
        ImGui::TextUnformatted("Identity key changed - connection refused");
        ImGui::Text("Known key:  %s\nNew key:      %s", KeyFingerprint(d.pubKey).c_str(), KeyFingerprint(d.pendingKey).c_str());
        ImGui::TextUnformatted("This happens when LunaShare was reinstalled or its settings were reset on that device. "
                               "It can also mean another device is pretending to be it. Only accept if you know why it changed, "
                               "then compare the new safety number on both screens.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Accept new key", ImVec2(0, h))) eng.AcceptNewKey(peer);
        ImGui::SameLine();
        if (ImGui::Button("Block device", ImVec2(0, h))) eng.SetBlocked(peer, true);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    ImGui::SeparatorText("Security");
    if (d.pubKey.empty()) {
        ImGui::TextDisabled("No identity key yet - it is exchanged on the first connection.");
    } else {
        std::string safety = SafetyNumber(eng.MyPublicKey(), d.pubKey);
        ImGui::TextDisabled("Safety number (identical on both devices when nobody is in between):");
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextUnformatted(safety.substr(0, 17).c_str());
        ImGui::TextUnformatted(safety.size() > 18 ? safety.substr(18).c_str() : "");
        ImGui::PopFont();
        bool ver = d.verified;
        if (ImGui::Checkbox("I compared it - mark as verified", &ver)) eng.SetVerified(peer, ver);
        ImGui::TextDisabled("Their key: %s    Yours: %s", KeyFingerprint(d.pubKey).c_str(), KeyFingerprint(eng.MyPublicKey()).c_str());
    }
    ImGui::Spacing();
    bool trusted = d.trusted, blocked = d.blocked;
    if (ImGui::Checkbox("Trusted - accept files automatically", &trusted)) eng.SetTrusted(peer, trusted);
    if (ImGui::Checkbox("Blocked", &blocked)) eng.SetBlocked(peer, blocked);
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
        row("User @ host", d.user + " @ " + d.host);
        row("Platform", d.platform);
        row("Fingerprint", d.Fingerprint());
        row("Device ID", d.id);
        row("MAC address", macs);
        row("Last address", d.lastIp.empty() ? "" : d.lastIp + ":" + std::to_string(d.lastPort));
        row("Bluetooth", d.btAddr);
        row("First seen", d.firstSeen ? FormatTime(d.firstSeen) : "");
        row("Last seen", eng.IsConnected(peer) ? "now" : (d.lastSeen ? FormatTime(d.lastSeen) : ""));
        int sent = 0, recv = 0;
        int64_t bytes = 0;
        for (auto& e : eng.history)
            if (e.peer == peer) { (e.outgoing ? sent : recv)++; bytes += e.size; }
        row("Shared", std::to_string(sent) + " sent, " + std::to_string(recv) + " received (" + FormatBytes((double)bytes) + ")");
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Click a value to copy it.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Notes");
    std::string notes = d.notes;
    if (ImGui::InputTextMultiline("##notes", &notes, ImVec2(mobile ? -1 : ImGui::GetFontSize() * 24, ImGui::GetTextLineHeight() * 3))) {
        d.notes = notes;
        eng.SetCustomName(peer, d.customName);  // marks dirty
    }
    ImGui::Spacing();
    bool connected = eng.IsConnected(peer);
    if (ui::IconTextButton("Chat", Icon::Send, ImVec2(0, h))) {
        SelectPeer(peer);
        if (mobile) { backStack.push_back(screen); screen = MScreen::Chat; }
        else { tab = Tab::Chat; tabForce = true; }
    }
    ImGui::SameLine();
    if (ui::IconTextButton(connected ? "Disconnect" : "Connect", Icon::Link, ImVec2(0, h))) connected ? eng.Disconnect(peer) : eng.ConnectPeer(peer);
    if (!d.btAddr.empty()) {
        ImGui::SameLine();
        if (ui::IconTextButton("Via Bluetooth", Icon::Bluetooth, ImVec2(0, h))) eng.ConnectBluetooth(d.btAddr);
    }
    if (mobile) ImGui::Spacing(); else ImGui::SameLine();
    if (ui::IconTextButton("Clear chat", Icon::Trash, ImVec2(0, h))) ImGui::OpenPopup("clearchat");
    ImGui::SameLine();
    if (ui::IconTextButton("Forget device", Icon::Trash, ImVec2(0, h))) ImGui::OpenPopup("forget");
    if (ImGui::BeginPopup("clearchat")) {
        ImGui::TextUnformatted("Delete the chat history with this device (on this device only)?");
        if (ImGui::Button("Delete")) { eng.ClearChat(peer); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("forget")) {
        ImGui::TextUnformatted("Forget this device, its nickname and chat history?");
        if (ImGui::Button("Forget")) {
            eng.ForgetDevice(peer);
            if (selectedPeer == peer) selectedPeer.clear();
            devicesSel.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ============================================================================ connect / nearby

void App::DrawNearby() {
    bool mobile = ui::M().mobile;
    if (mobile) ui::TouchScroll();
    float h = mobile ? ui::M().rowH : 0;
    ImGui::BeginChild("nearby");
    if (mobile) ui::TouchScroll();

    // ---- this device
    ImGui::SeparatorText("This device");
    ImGui::Text("%s", eng.settings.displayName.c_str());
    ImGui::TextDisabled("Key fingerprint %s  -  port %u", KeyFingerprint(eng.MyPublicKey()).c_str(), eng.boundPort);
    if (!eng.listenError.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ColErr()), "%s", eng.listenError.c_str());
    auto addrs = LocalIPv4Addresses();
    for (auto& a : addrs) {
        ImGui::BulletText("%s:%u", a.ip.c_str(), eng.boundPort);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", a.iface.c_str());
    }
    if (addrs.empty()) ImGui::TextDisabled("No network connection.");
    ImGui::TextDisabled("Devices on the same network find each other automatically:");
    ImGui::BulletText("Bonjour / mDNS (_lunashare._tcp): %s", eng.settings.discovery && eng.settings.mdns ? eng.MdnsStatus().c_str() : "off");
    ImGui::BulletText("UDP broadcast beacons (port %u): %s", kDiscoveryPort, eng.settings.discovery && eng.settings.broadcast ? "on" : "off");
    if (ui::IconTextButton("Search again", Icon::Retry, ImVec2(0, h))) eng.RefreshDiscovery();
    if (!eng.online.empty()) {
        ImGui::TextDisabled("Found on the network:");
        for (auto& [id, p] : eng.online) {
            ImGui::BulletText("%s  %s:%u", eng.PeerName(id).c_str(), p.ip.c_str(), p.port);
            ImGui::SameLine();
            ImGui::TextDisabled("(%s%s)", p.Via().c_str(), eng.IsConnected(id) ? ", connected" : "");
        }
    }

    // ---- manual
    ImGui::SeparatorText("Connect by address");
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
    if (ui::IconTextButton("Connect", Icon::Link, ImVec2(0, h))) eng.ConnectAddress(host, (uint16_t)manualPort);
    ImGui::EndDisabled();

    // ---- bluetooth
    ImGui::SeparatorText("Bluetooth");
    if (!eng.btSupported) {
        ImGui::TextDisabled("Bluetooth is not available here (no adapter or not supported on %s yet).", PlatformName());
    } else {
        bool bt = eng.settings.bluetooth;
        if (ImGui::Checkbox("Accept Bluetooth connections", &bt)) {
            Settings s = eng.settings;
            s.bluetooth = bt;
            eng.ApplySettings(s);
            if (bt && Bridge().requestPermissions) Bridge().requestPermissions();
        }
        ImGui::TextDisabled("%s", eng.btStatus.c_str());
        ImGui::TextDisabled("Pair the devices in the system Bluetooth settings first. Bluetooth is slow (~100-300 KB/s) - good for chat, small files, or handing over hotspot credentials.");
        if (ui::IconTextButton(eng.btScanning ? "Searching..." : "Paired devices", Icon::Bluetooth, ImVec2(0, h)) && !eng.btScanning) eng.BtRefresh(false);
        ImGui::SameLine();
        if (ui::IconTextButton("Scan nearby", Icon::Retry, ImVec2(0, h)) && !eng.btScanning) eng.BtRefresh(true);
        for (auto& d : eng.btDevices) {
            ImGui::PushID(d.addr.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", d.name.empty() ? d.addr.c_str() : d.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s%s", d.addr.c_str(), d.paired ? " (paired)" : "");
            ImGui::SameLine();
            if (ImGui::Button("Connect", ImVec2(0, mobile ? h * 0.8f : 0))) eng.ConnectBluetooth(d.addr);
            ImGui::PopID();
        }
    }

    // ---- hotspot
    ImGui::SeparatorText("Phone-to-phone (Wi-Fi hotspot)");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("No shared Wi-Fi? One phone starts a hotspot, the other joins it - then they find each other automatically "
                        "at full Wi-Fi speed. The other phone can join by scanning the QR code with its camera, or automatically "
                        "when both are connected over Bluetooth.");
    ImGui::PopTextWrapPos();
    if (Bridge().hotspotStart) {
        if (!eng.hotspot.active) {
            if (ui::BigButton("Start hotspot on this phone", Icon::Wifi, mobile ? -1 : ImGui::GetFontSize() * 18)) Bridge().hotspotStart();
        } else {
            ImGui::Text("Hotspot running: %s", eng.hotspot.ssid.c_str());
            ImGui::Text("Password: %s", eng.hotspot.pass.c_str());
            std::string wifiQr = "WIFI:T:WPA;S:" + eng.hotspot.ssid + ";P:" + eng.hotspot.pass + ";;";
            ui::QrCode(wifiQr.c_str(), std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 16));
            for (auto& c : eng.conns) {
                if (!c->ready || c->closed) continue;
                ImGui::PushID((int)c->cid);
                std::string l = "Send Wi-Fi login to " + eng.PeerName(c->peerId);
                if (ui::IconTextButton(l.c_str(), Icon::Share, ImVec2(0, h))) eng.SendWifiCreds(c->peerId, eng.hotspot.ssid, eng.hotspot.pass);
                ImGui::PopID();
            }
            if (ui::IconTextButton("Stop hotspot", Icon::Cancel, ImVec2(0, h)) && Bridge().hotspotStop) Bridge().hotspotStop();
        }
    }
    if (Bridge().wifiJoin) {
        ImGui::TextUnformatted("Join a hotspot");
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 14);
        ImGui::InputTextWithHint("##jssid", "Network name (SSID)", &joinSsid);
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 14);
        ImGui::InputTextWithHint("##jpass", "Password", &joinPass, ImGuiInputTextFlags_Password);
        ImGui::BeginDisabled(joinSsid.empty());
        if (ui::IconTextButton("Join", Icon::Wifi, ImVec2(0, h))) Bridge().wifiJoin(joinSsid, joinPass);
        ImGui::EndDisabled();
    }
    if (!IsMobilePlatform()) {
#ifdef _WIN32
        if (ImGui::Button("Open Windows Mobile hotspot settings")) OpenWithDefaultApp("ms-settings:network-mobilehotspot");
#endif
        ImGui::TextDisabled("Sharing this computer's hotspot? Enter its name and password to show a QR code phones can scan:");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::InputTextWithHint("##qssid", "SSID", &qrSsid);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::InputTextWithHint("##qpass", "Password", &qrPass);
        if (!qrSsid.empty()) {
            std::string wifiQr = "WIFI:T:WPA;S:" + qrSsid + ";P:" + qrPass + ";;";
            ui::QrCode(wifiQr.c_str(), ImGui::GetFontSize() * 12);
            for (auto& c : eng.conns) {
                if (!c->ready || c->closed || c->transport != Transport::Bluetooth) continue;
                ImGui::PushID((int)c->cid);
                if (ImGui::Button(("Send to " + eng.PeerName(c->peerId) + " over Bluetooth").c_str())) eng.SendWifiCreds(c->peerId, qrSsid, qrPass);
                ImGui::PopID();
            }
        }
    }
    ImGui::Dummy(ImVec2(0, h));
    ImGui::EndChild();
}

// ============================================================================ settings

void App::DrawSettings() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH : 0;
    ImGui::BeginChild("settings");
    if (mobile) ui::TouchScroll();
    bool changed = false;
    float fieldW = mobile ? -1 : ImGui::GetFontSize() * 22;

    ImGui::SeparatorText("Profile");
    ImGui::TextUnformatted("Name shown to other devices");
    ImGui::SetNextItemWidth(fieldW);
    ImGui::InputText("##dname", &edit.displayName);
    if (ImGui::IsItemActive()) editDirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;

    ImGui::SeparatorText("Receiving");
    ImGui::TextUnformatted("Save received files to");
    ImGui::SetNextItemWidth(mobile ? -1 : fieldW);
    ImGui::InputText("##dldir", &edit.downloadDir);
    if (ImGui::IsItemActive()) editDirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    if (!mobile) ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(0, h))) {
        browseFor = BrowseFor::DownloadDir;
        browser.Open(FileBrowser::Mode::SelectDir, "Folder for received files", edit.downloadDir);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open folder", ImVec2(0, h))) RevealPath(edit.downloadDir);
    changed |= ImGui::Checkbox("Automatically accept files from trusted devices", &edit.autoAcceptTrusted);
    changed |= ImGui::Checkbox("Automatically accept files from everyone", &edit.autoAcceptAll);
    ui::HelpMarker("Anyone on your network could send you files without asking. Only enable on networks you trust.");
    changed |= ImGui::Checkbox("Resume interrupted transfers automatically", &edit.autoResume);

    ImGui::SeparatorText("Network");
    changed |= ImGui::Checkbox("Find devices on the local network", &edit.discovery);
    ImGui::BeginDisabled(!edit.discovery);
    ImGui::Indent();
    changed |= ImGui::Checkbox("Bonjour / mDNS", &edit.mdns);
    ui::HelpMarker("Standard service discovery (_lunashare._tcp). Works on most routers and is the only method iPhones/iPads can use.");
    changed |= ImGui::Checkbox("UDP broadcast beacons", &edit.broadcast);
    ui::HelpMarker("Fast discovery that also works on networks without multicast. Not available on iOS.");
    ImGui::Unindent();
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Connect to found devices automatically", &edit.autoConnect);
    int port = edit.tcpPort;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::InputInt("TCP port", &port, 0)) { edit.tcpPort = (uint16_t)std::clamp(port, 1, 65535); editDirty = true; }
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(listening on %u)", eng.boundPort);

    ImGui::SeparatorText("Transfers");
    int limit = (int)(edit.speedLimitKBs / 1024);
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::SliderInt("##limit", &limit, 0, 200, limit == 0 ? "Upload speed: unlimited" : "Upload limit: %d MB/s")) { edit.speedLimitKBs = (int64_t)limit * 1024; changed = true; }
    ImGui::SetNextItemWidth(fieldW);
    changed |= ImGui::SliderInt("##par", &edit.maxParallel, 1, 8, "Files at once per device: %d");
    ImGui::SetNextItemWidth(fieldW);
    changed |= ImGui::SliderInt("##chunk", &edit.chunkKB, 16, 2048, "Chunk size: %d KB", ImGuiSliderFlags_Logarithmic);

    ImGui::SeparatorText("Bluetooth");
    changed |= ImGui::Checkbox("Accept Bluetooth connections##s", &edit.bluetooth);
#if defined(__linux__) && !defined(__ANDROID__)
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    changed |= ImGui::SliderInt("RFCOMM channel", &edit.btChannel, 1, 30);
    ui::HelpMarker("Linux uses a fixed RFCOMM channel. Use the same channel on both Linux machines.");
#endif

    ImGui::SeparatorText("Appearance");
    int layout = (int)edit.layout;
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::Combo("##layout", &layout, "Layout: automatic\0Layout: desktop\0Layout: phone (vertical, big buttons)\0")) {
        edit.layout = (UiLayout)layout;
        changed = true;
        styleGeneration++;
    }
    ImGui::SetNextItemWidth(fieldW);
    float sc = edit.uiScale <= 0 ? 0.0f : edit.uiScale;
    if (ImGui::SliderFloat("##scale", &sc, 0.0f, 3.0f, sc < 0.5f ? "UI scale: automatic" : "UI scale: %.2fx")) {
        edit.uiScale = sc < 0.5f ? 0.0f : sc;
        editDirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { changed = true; styleGeneration++; }
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::Combo("##theme", &edit.theme, "Theme: dark\0Theme: light\0Theme: classic\0")) { changed = true; styleGeneration++; }

    ImGui::SeparatorText("History & privacy");
    changed |= ImGui::Checkbox("Keep chat and shared-file history", &edit.keepHistory);
    if (ImGui::Button("Clear shared files history", ImVec2(0, h))) eng.ClearHistory();

    ImGui::SeparatorText("Backup / move to another device");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Exports all settings plus your list of known devices with their nicknames, trust and block state.");
    ImGui::PopTextWrapPos();
    ImGui::Checkbox("Include chat & shared files history", &exportHistory);
    ImGui::Checkbox("Include this device's identity (only for restoring THIS device)", &exportIdentity);
    if (ui::IconTextButton("Export settings...", Icon::Up, ImVec2(0, h))) {
        browseFor = BrowseFor::Export;
        std::string name = "lunashare-settings-" + FormatTime(NowMs(), true).substr(0, 10) + ".json";
        browser.Open(FileBrowser::Mode::SaveFile, "Export settings", eng.settings.downloadDir, name, ".json");
    }
    ImGui::SameLine();
    if (ui::IconTextButton("Import settings...", Icon::Down, ImVec2(0, h))) {
        browseFor = BrowseFor::Import;
        browser.Open(FileBrowser::Mode::OpenFile, "Import settings", eng.settings.downloadDir, "", ".json");
    }
    ImGui::Checkbox("When importing, also take over the device identity", &importIdentity);

    ImGui::SeparatorText("About");
    ImGui::TextDisabled("LunaShare 0.1  -  %s  -  protocol v%d", PlatformName(), kProtocolVersion);
    ImGui::TextDisabled("Settings folder: %s", (eng.configDir.empty() ? ConfigDir() : eng.configDir).c_str());
    ImGui::TextDisabled("Every file is hashed with SHA-256 before sending and again after receiving; transfers can be paused, resumed, and survive restarts.");
    ImGui::TextDisabled("All connections are end-to-end encrypted (X25519 key exchange, XChaCha20-Poly1305, EdDSA device keys pinned on first contact).");
    ImGui::TextDisabled("This device's key fingerprint: %s", KeyFingerprint(eng.MyPublicKey()).c_str());
    ImGui::Dummy(ImVec2(0, h));
    ImGui::EndChild();

    if (changed) {
        eng.ApplySettings(edit);
        editDirty = false;
    }
}

// ============================================================================ popups & toasts

void App::DrawPopups() {
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
            case BrowseFor::DownloadDir:
                edit.downloadDir = res[0];
                s.downloadDir = res[0];
                break;
            case BrowseFor::Export:
                if (eng.ExportSettings(res[0], exportHistory, exportIdentity, &err)) infoMessage = "Settings exported to\n" + res[0];
                else infoMessage = "Export failed: " + err;
                openInfo = true;
                break;
            case BrowseFor::Import:
                if (eng.ImportSettings(res[0], importIdentity, &err)) { infoMessage = "Settings imported from\n" + res[0]; styleGeneration++; s = eng.settings; s.lastBrowseDir = browser.lastDir; }
                else infoMessage = "Import failed: " + err;
                openInfo = true;
                break;
            default: break;
        }
        eng.ApplySettings(s);
        edit = eng.settings;
        browseFor = BrowseFor::None;
    }

    // ---- send to device picker
    if (openSendTo) { ImGui::OpenPopup("Send to device"); openSendTo = false; }
    centerNext(0.4f);
    if (ImGui::BeginPopupModal("Send to device", nullptr, ImGuiWindowFlags_AlwaysAutoResize * !mobile)) {
        int64_t total = 0;
        for (auto& p : pendingSend) total += IsDirectory(p) ? 0 : std::max<int64_t>(0, FileSize(p));
        ImGui::Text("%d item(s)%s", (int)pendingSend.size(), total ? ("  -  " + FormatBytes((double)total)).c_str() : "");
        for (size_t i = 0; i < pendingSend.size() && i < 5; i++) ImGui::BulletText("%s", BaseName(pendingSend[i]).c_str());
        if (pendingSend.size() > 5) ImGui::TextDisabled("... and %d more", (int)pendingSend.size() - 5);
        ImGui::Separator();
        ImGui::TextUnformatted("Choose a device:");
        auto peers = SortedPeers();
        ImGui::BeginChild("pick", ImVec2(mobile ? 0 : ImGui::GetFontSize() * 24, std::min(ImGui::GetFontSize() * 16, vp->WorkSize.y * 0.5f)), ImGuiChildFlags_Borders);
        if (mobile) ui::TouchScroll();
        for (auto& p : peers) {
            auto d = eng.devices.find(p);
            if (d != eng.devices.end() && d->second.blocked) continue;
            if (DevicePeerRow(p, false, mobile ? ui::M().rowH * 1.2f : 0)) {
                eng.SendPaths(p, pendingSend);
                SelectPeer(p);
                if (mobile) { backStack.clear(); backStack.push_back(MScreen::Home); screen = MScreen::Chat; }
                else { tab = Tab::Chat; tabForce = true; }
                pendingSend.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        if (peers.empty()) ImGui::TextDisabled("No devices known yet.");
        ImGui::EndChild();
        if (ImGui::Button("Cancel", ImVec2(mobile ? -1 : 0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pendingSend.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- select text view
    if (openTextView) { ImGui::OpenPopup("Message text"); openTextView = false; }
    centerNext(0.5f);
    if (ImGui::BeginPopupModal("Message text", nullptr, 0)) {
        ImGui::TextDisabled("Select any part of the text and press Ctrl+C, or copy everything:");
        ImGui::InputTextMultiline("##tv", &textViewBuf, ImVec2(-1, ImGui::GetTextLineHeight() * (mobile ? 12 : 10)), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
        if (ImGui::Button("Copy all", ImVec2(0, h))) ImGui::SetClipboardText(textViewBuf.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- rename
    if (openRename) { ImGui::OpenPopup("Rename device"); openRename = false; }
    centerNext(0.3f);
    if (ImGui::BeginPopupModal("Rename device", nullptr, ImGuiWindowFlags_AlwaysAutoResize * !mobile)) {
        auto d = eng.devices.find(renamePeer);
        ImGui::Text("Nickname for %s", d != eng.devices.end() ? d->second.name.c_str() : renamePeer.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 18);
        bool enter = ImGui::InputTextWithHint("##rn", "leave empty to use the announced name", &renameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save", ImVec2(0, h)) || enter) { eng.SetCustomName(renamePeer, renameBuf); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- wifi offer
    if (openWifiOffer && !wifiOffers.empty()) { ImGui::OpenPopup("Wi-Fi hotspot offered"); openWifiOffer = false; }
    centerNext(0.35f);
    if (ImGui::BeginPopupModal("Wi-Fi hotspot offered", nullptr, ImGuiWindowFlags_AlwaysAutoResize * !mobile)) {
        auto& o = wifiOffers.front();
        ImGui::Text("%s shares its hotspot for a faster connection:", eng.PeerName(o.peer).c_str());
        ImGui::Text("Network: %s", o.ssid.c_str());
        ImGui::Text("Password: %s", o.pass.c_str());
        std::string qr = "WIFI:T:WPA;S:" + o.ssid + ";P:" + o.pass + ";;";
        ui::QrCode(qr.c_str(), ImGui::GetFontSize() * 10);
        if (Bridge().wifiJoin) {
            if (ImGui::Button("Join now", ImVec2(0, h))) { Bridge().wifiJoin(o.ssid, o.pass); wifiOffers.erase(wifiOffers.begin()); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
        } else {
            ImGui::TextDisabled("Join this network in your system Wi-Fi settings.");
        }
        if (ImGui::Button("Copy password", ImVec2(0, h))) ImGui::SetClipboardText(o.pass.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(0, h))) { wifiOffers.erase(wifiOffers.begin()); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // ---- info
    if (openInfo) { ImGui::OpenPopup("LunaShare##info"); openInfo = false; }
    centerNext(0);
    if (ImGui::BeginPopupModal("LunaShare##info", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(infoMessage.c_str());
        if (ImGui::Button("OK", ImVec2(mobile ? -1 : ImGui::GetFontSize() * 6, h)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- quit
    if (openQuit) { ImGui::OpenPopup("Quit LunaShare?"); openQuit = false; }
    centerNext(0);
    if (ImGui::BeginPopupModal("Quit LunaShare?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Transfers are still running.\nThey will be paused and continue automatically the next time both devices are online.");
        if (ImGui::Button("Quit", ImVec2(0, h))) { quitConfirmed = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Keep running", ImVec2(0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
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
        ImGui::TextUnformatted(t.title.c_str());
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", t.text.c_str());
        ImGui::PopTextWrapPos();
        float bh = mobile ? ui::M().rowH * 0.8f : 0;
        if (t.type == EventType::IncomingOffer) {
            auto it = eng.transfers.find(t.tid);
            if (it != eng.transfers.end() && it->second->state == TState::Pending) {
                if (ImGui::Button("Accept", ImVec2(0, bh))) { eng.AcceptBatch(it->second->batch); t.ttl = 0; }
                ImGui::SameLine();
                if (ImGui::Button("Decline", ImVec2(0, bh))) {
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
            if (ImGui::Button("Open chat", ImVec2(0, bh))) {
                SelectPeer(t.peer);
                if (mobile) { backStack.clear(); backStack.push_back(MScreen::Home); screen = MScreen::Chat; }
                else { tab = Tab::Chat; tabForce = true; }
                t.ttl = 0;
            }
            ImGui::SameLine();
        }
        if ((!mobile || t.type == EventType::IncomingOffer) && ImGui::Button("Dismiss", ImVec2(0, bh))) t.ttl = 0;
        if (ImGui::IsWindowHovered()) t.ttl = std::max(t.ttl, 1.5f);
        y -= ImGui::GetWindowHeight() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::End();
    }
}
