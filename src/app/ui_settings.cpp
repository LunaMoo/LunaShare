// Settings, in categories: a column of them on the left (desktop) or a list that opens a page each (phone), and a
// search over every option. The options themselves are the same as before; only where they live changed.
// SPDX-License-Identifier: GPL-3.0-only
#include <SDL.h>

#include <algorithm>
#include <cstring>

#include "crypto.h"
#include "emoji_host.h"
#include "file_assoc.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "media_host.h"
#include "platform.h"
#include "protocol.h"
#include "ui.h"
#include "ui_widgets.h"
#include "wan.h"

using namespace ls;
using ui::Icon;

namespace {

enum Cat { kAccount, kPrivacy, kFiles, kCalls, kSync, kConnections, kAppearance, kBackup, kAbout, kCatCount };

struct CatInfo {
    const char* title;
    Icon icon;
};
const CatInfo kCats[kCatCount] = {
    {"Profile & account", Icon::Users},     {"Privacy & security", Icon::Lock}, {"Chats & files", Icon::Folder},
    {"Calls", Icon::Phone},                 {"My devices", Icon::Link},         {"Connections", Icon::Wifi},
    {"Appearance & language", Icon::Palette}, {"Backup", Icon::Archive},        {"About & updates", Icon::Info},
};

// What the search finds: an option (or a heading), the category it is in and the heading it is under (scrolled to).
struct Entry {
    int cat;
    const char* section;
    const char* label;
};
const Entry kIndex[] = {
    {kAccount, "Profile", "Profile picture"},
    {kAccount, "Profile", "Name shown to other devices"},
    {kAccount, "Your account", "Your account"},
    {kAccount, "Your account", "Link another device..."},
    {kAccount, "Your account", "Account fingerprint: %s"},
    {kAccount, "Your account", "Username on your server (others can find you by it):"},
    {kPrivacy, "App lock", "App lock"},
    {kPrivacy, "App lock", "Set a PIN..."},
    {kPrivacy, "App lock", "Unlock with the fingerprint / face too"},
    {kPrivacy, "App lock", "Encrypt the chat history with the PIN"},
    {kPrivacy, "History & privacy", "Keep chat and shared-file history"},
    {kPrivacy, "History & privacy", "Clear shared files history"},
    {kFiles, "Received files", "Save received files to"},
    {kFiles, "Accept files without asking", "Automatically accept files from trusted devices"},
    {kFiles, "Accept files without asking", "Automatically accept files from everyone"},
    {kFiles, "Accept files without asking", "On mobile data, always ask first"},
    {kFiles, "Sending", "Send photos smaller"},
    {kFiles, "Sending", "Resume interrupted transfers automatically"},
    {kFiles, "Sending", "Upload speed: unlimited"},
    {kFiles, "Opening files", "Offer LunaShare for pictures, movies, songs, archives and comics"},
    {kCalls, "Calls", "Echo cancellation"},
    {kCalls, "Calls", "Noise suppression"},
    {kCalls, "Calls", "Microphone"},
    {kCalls, "Calls", "Speaker"},
    {kCalls, "Calls", "Test microphone"},
    {kCalls, "Calls", "Lower video quality automatically when the connection is slow"},
    {kCalls, "Calls", "Camera"},
    {kSync, "My devices", "The same playlists and settings on my devices"},
    {kSync, "My devices", "Share the clipboard with my devices"},
    {kSync, "My devices", "Folders sent to my devices by themselves"},
    {kConnections, "In the background", "Keep running in the background"},
    {kConnections, "In the background", "Keep running when the window is closed"},
    {kConnections, "Internet", "Rendezvous server (optional)"},
    {kConnections, "Invites", "Invites"},
    {kConnections, "Invites", "Create invite code"},
    {kConnections, "Bluetooth", "Accept Bluetooth connections"},
    {kConnections, "Advanced", "Room name"},
    {kConnections, "Advanced", "Relay through the server when a direct connection fails"},
    {kConnections, "Advanced", "Group chats and calls through the server"},
    {kConnections, "Advanced", "Ask the router for a port mapping (UPnP / NAT-PMP)"},
    {kConnections, "Advanced", "Private mode: don't list me in the room"},
    {kConnections, "Advanced", "Find devices on the local network"},
    {kConnections, "Advanced", "Connect to found devices automatically"},
    {kConnections, "Advanced", "TCP port"},
    {kConnections, "Advanced", "Files at once per device: %d"},
    {kAppearance, "Appearance", "Layout: automatic"},
    {kAppearance, "Appearance", "UI scale: automatic"},
    {kAppearance, "Appearance", "Theme: dark"},
    {kAppearance, "Appearance", "Language: "},
    {kBackup, "Your account", "Recovery phrase..."},
    {kBackup, "Your account", "Save backup file..."},
    {kBackup, "Your account", "Restore account..."},
    {kBackup, "Backup / move to another device", "Export settings..."},
    {kBackup, "Backup / move to another device", "Import settings..."},
    {kAbout, "About", "Check for a new version once a day"},
    {kAbout, "About", "Settings folder: %s"},
};

// lower case, for the search: ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic (enough for the UI's languages)
std::string Fold(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp;
        int n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F), n = 2;
        else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F), n = 3;
        else { out += s.substr(i, 1); i++; continue; }
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 32;
        else if (cp >= 0x100 && cp <= 0x17F && cp % 2 == 0 && cp != 0x130 && cp != 0x138) cp += 1;
        else if (cp >= 0x391 && cp <= 0x3AB) cp += 32;
        else if (cp >= 0x410 && cp <= 0x42F) cp += 32;
        else if (cp >= 0x400 && cp <= 0x40F) cp += 80;
        if (n == 3 || cp >= 0x800) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp >= 0x80) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)cp;
        }
        i += (size_t)n;
    }
    return out;
}

// a label without its format part ("Upload limit: %d MB/s" -> "Upload limit:"), for showing as a result
std::string Plain(const char* t) {
    std::string s = t;
    size_t p = s.find('%');
    if (p != std::string::npos) s = s.substr(0, p);
    while (!s.empty() && (s.back() == ' ' || s.back() == ':')) s.pop_back();
    return s;
}

}  // namespace

bool App::SettingsSection(const char* en, const char* help) {
    ImGui::Spacing();
    if (!settingsGoto.empty() && settingsGoto == en) {
        ImGui::SetScrollHereY(0.1f);
        settingsGoto.clear();
    }
    // (a phone page already has the category's name on top: the same heading again isn't needed)
    if (ui::M().mobile && !help && settingsCat >= 0 && settingsCat < kCatCount && !strcmp(en, kCats[settingsCat].title)) return true;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    ImGui::TextUnformatted(T(en));
    ImGui::PopStyleColor();
    if (help) ui::HelpMarker(help);
    ImGui::Separator();
    return true;
}

const char* App::SettingsTitle(int cat) { return cat >= 0 && cat < kCatCount ? T(kCats[cat].title) : T("Settings"); }

std::string App::SettingsSummary(int cat) {
    const Settings& s = eng.settings;
    switch (cat) {
        case kAccount: return Emoji().Display(s.displayName);
        case kPrivacy: return LockOn(s) ? T("App lock: on") : T("App lock: off");
        case kFiles: return s.downloadDir;
        case kCalls: return Media().Available() ? T("Sound, camera, quality") : T("Not installed");
        case kSync: {
            size_t n = s.accountDevices.size() > 0 ? s.accountDevices.size() - 1 : 0;
            return n ? TF("%d device(s) linked", (int)n) : std::string(T("No other devices linked to your account yet."));
        }
        case kConnections:
            if (s.wanFromInvite) return T("Internet through an invite");
            return s.wanEnabled && !s.wanServer.empty() ? s.wanServer : std::string(T("Local network, hotspot, Bluetooth"));
        case kAppearance: {
            std::string lang = T("Language: automatic");
            for (auto& l : Languages())
                if (s.language == l.code) lang = l.name;
            return lang;
        }
        case kBackup: return s.accountBackedUp ? T("Recovery phrase, backup file, settings") : T("No backup yet");
        case kAbout: return std::string("LunaShare ") + LUNASHARE_VERSION;
    }
    return {};
}

void App::DrawSettings() {
    settingsShownFrame = ImGui::GetFrameCount();
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH : 0;
    bool changed = false;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    if (static const char* dev = getenv("LUNASHARE_SETCAT"); dev) {  // dev: open this category (screenshots)
        settingsCat = atoi(dev);
        dev = nullptr;
    }
    if (static const char* dev = getenv("LUNASHARE_SETQ"); dev) {  // dev: search for this (screenshots)
        settingsQuery = dev;
        dev = nullptr;
    }
    if (!mobile && settingsCat < 0) settingsCat = kAccount;
    if (settingsCat >= kCatCount) settingsCat = mobile ? -1 : kAccount;

    auto searchBox = [&](float w) {
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputTextWithHint("##setsearch", T("Search settings"), &settingsQuery) && !settingsQuery.empty() && mobile)
            settingsCat = -1;
    };

    if (mobile) {
        if (settingsCat < 0) {
            // the list of categories (or what the search found)
            ImGui::BeginChild("setlist");
            searchBox(-1);
            ImGui::Spacing();
            if (!settingsQuery.empty()) {
                SettingsSearchResults(h);
            } else {
                float rowH = ui::M().rowH * 1.25f;
                for (int c = 0; c < kCatCount; c++) {
                    ImGui::PushID(c);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float w = ImGui::GetContentRegionAvail().x;
                    if (ImGui::Selectable("##cat", false, 0, ImVec2(w, rowH))) settingsCat = c;
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    float ic = rowH * 0.42f;
                    ui::DrawIcon(dl, kCats[c].icon, ImVec2(p.x + rowH * 0.45f, p.y + rowH * 0.5f), ic, ImGui::GetColorU32(ImGuiCol_Text));
                    float tx = p.x + rowH * 0.95f;
                    float lh = ImGui::GetTextLineHeight();
                    dl->AddText(ImVec2(tx, p.y + rowH * 0.5f - lh * 1.05f), ImGui::GetColorU32(ImGuiCol_Text), T(kCats[c].title));
                    std::string sum = SettingsSummary(c);
                    ImVec4 clip(tx, p.y, p.x + w - sp, p.y + rowH);
                    dl->AddText(nullptr, 0, ImVec2(tx, p.y + rowH * 0.5f + lh * 0.05f), ImGui::GetColorU32(ImGuiCol_TextDisabled), sum.c_str(),
                                nullptr, 0, &clip);
                    ImGui::PopID();
                }
            }
            ImGui::Dummy(ImVec2(0, h));
            ImGui::EndChild();
            return;
        }
        ImGui::BeginChild("settings");
        SettingsCategory(settingsCat, -1, h, changed);
        ImGui::Dummy(ImVec2(0, h));
        ImGui::EndChild();
    } else {
        // desktop: categories on the left, the chosen one (or the search results) on the right
        float listW = ImGui::GetFontSize() * 14;
        ImGui::BeginChild("setcats", ImVec2(listW, 0), ImGuiChildFlags_None);
        searchBox(-1);
        ImGui::Spacing();
        for (int c = 0; c < kCatCount; c++) {
            ImGui::PushID(c);
            bool sel = settingsQuery.empty() && settingsCat == c;
            ImVec2 p = ImGui::GetCursorScreenPos();
            float rh = ImGui::GetFrameHeight() * 1.15f;
            if (ImGui::Selectable("##cat", sel, 0, ImVec2(0, rh))) {
                settingsCat = c;
                settingsQuery.clear();
            }
            float ic = ImGui::GetFontSize() * 0.95f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ui::DrawIcon(dl, kCats[c].icon, ImVec2(p.x + ic * 0.75f, p.y + rh * 0.5f), ic, ImGui::GetColorU32(ImGuiCol_Text));
            dl->AddText(ImVec2(p.x + ic * 1.7f, p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), T(kCats[c].title));
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("settings");
        if (!settingsQuery.empty()) SettingsSearchResults(h);
        else SettingsCategory(settingsCat, ImGui::GetFontSize() * 22, h, changed);
        ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
        ImGui::EndChild();
    }

    if (changed) {
        eng.ApplySettings(edit);
        editDirty = false;
    }
}

void App::SettingsSearchResults(float h) {
    std::string q = Fold(settingsQuery);
    int shown = 0;
    std::vector<std::string> seen;
    for (auto& e : kIndex) {
        std::string label = Plain(T(e.label));
        if (Fold(label).find(q) == std::string::npos && Fold(Plain(e.label)).find(q) == std::string::npos &&
            Fold(T(kCats[e.cat].title)).find(q) == std::string::npos)
            continue;
        if (std::find(seen.begin(), seen.end(), label) != seen.end()) continue;
        seen.push_back(label);
        ImGui::PushID(shown);
        std::string row = label + "##r";
        float rowH = ImGui::GetTextLineHeight() * 2.3f + ImGui::GetStyle().FramePadding.y * 2;
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable(row.c_str(), false, 0, ImVec2(0, rowH))) {
            settingsCat = e.cat;
            settingsGoto = e.section;
            settingsQuery.clear();
        }
        // the category under the option
        std::string where = T(kCats[e.cat].title);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ImGui::GetStyle().FramePadding.x, p.y + rowH - ImGui::GetTextLineHeight() * 1.05f),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled), where.c_str());
        ImGui::PopID();
        shown++;
    }
    if (!shown) ImGui::TextDisabled("%s", T("Nothing found."));
}

void App::SettingsCategory(int cat, float fieldW, float h, bool& changed) {
    bool mobile = ui::M().mobile;
    float bw = mobile ? -1 : 0;
    switch (cat) {
        case kAccount: {
            SettingsSection("Profile");
            DrawProfilePicture(h);
            ImGui::TextUnformatted(T("Name shown to other devices"));
            ImGui::SetNextItemWidth(fieldW);
            ImGui::InputText("##dname", &edit.displayName);
            if (ImGui::IsItemActive()) editDirty = true;
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            SettingsSection("Your account", T("Your account belongs to you, not to a server. Link your phone and computer to it: messages then "
                                              "reach all of them and your contacts see one person."));
            DrawAccountSettings(fieldW, h, 0);
            break;
        }
        case kPrivacy: {
            SettingsSection("App lock", T("A PIN before LunaShare shows anything - when it starts and when you come back to it. Messages still "
                                          "arrive; notifications don't show their text. (It keeps out someone holding your unlocked device; the "
                                          "files are encrypted only with the option below.)"));
            DrawLockSettings(h);
            SettingsSection("History & privacy");
            changed |= ui::Checkbox(T("Keep chat and shared-file history"), &edit.keepHistory);
            if (ImGui::Button(T("Clear shared files history"), ImVec2(bw, h))) eng.ClearHistory();
            break;
        }
        case kFiles: {
            SettingsSection("Received files");
            ImGui::TextUnformatted(T("Save received files to"));
            ImGui::SetNextItemWidth(fieldW);
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

            SettingsSection("Accept files without asking");
            changed |= ui::Checkbox(T("Automatically accept files from trusted devices"), &edit.autoAcceptTrusted);
            changed |= ui::Checkbox(T("Automatically accept files from everyone"), &edit.autoAcceptAll);
            ui::HelpMarker(T("Anyone on your network could send you files without asking. Only enable on networks you trust."));
            ImGui::BeginDisabled(!edit.autoAcceptTrusted && !edit.autoAcceptAll);
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
                        if (ImGui::Selectable(name(mb).c_str(), edit.autoAcceptMaxMB == mb, 0, ImVec2(0, h))) {
                            edit.autoAcceptMaxMB = mb;
                            changed = true;
                        }
                    ImGui::EndCombo();
                }
                ui::HelpMarker(T("A bigger file asks first, even from a trusted device."));
                if (Bridge().isMetered) changed |= ui::Checkbox(T("On mobile data, always ask first"), &edit.autoAcceptNotMetered);
            }
            ImGui::EndDisabled();

            SettingsSection("Sending");
            changed |= ui::Checkbox(T("Send photos smaller"), &edit.photosSmaller);
            ui::HelpMarker(T("Photos (JPEG, PNG) over 0.7 MB go as a JPEG at most 2048 pixels wide - a few times smaller, still sharp on "
                             "a screen. Their EXIF data (camera, location) isn't copied. Videos and other files go as they are."));
            changed |= ui::Checkbox(T("Resume interrupted transfers automatically"), &edit.autoResume);
            {
                int limit = (int)(edit.speedLimitKBs / 1024);
                ImGui::SetNextItemWidth(fieldW);
                if (ImGui::SliderInt("##limit", &limit, 0, 200, limit == 0 ? T("Upload speed: unlimited") : T("Upload limit: %d MB/s"))) {
                    edit.speedLimitKBs = (int64_t)limit * 1024;
                    changed = true;
                }
            }
            if (assoc::Supported()) {  // "Open with" (desktops; Android always has it)
                SettingsSection("Opening files", T("LunaShare shows up in the file manager's \"Open with\" for these files (for you only). Started "
                                                   "again while it runs, it shows them in the running one. To open them with LunaShare on a double "
                                                   "click, pick it as the default app for them."));
                static int assocState = -1;  // (asked once, not every frame)
                if (assocState < 0) assocState = assoc::Registered() ? 1 : 0;
                bool on = assocState == 1;
                if (ui::Checkbox(T("Offer LunaShare for pictures, movies, songs, archives and comics"), &on)) {
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
                if (on && assoc::CanOpenSystemSettings() && ImGui::Button(T("Default apps..."), ImVec2(0, h))) assoc::OpenSystemSettings();
            }
            break;
        }
        case kCalls:
            SettingsSection("Calls");
            DrawCallSettings(fieldW, h, changed);
            break;
        case kSync:
            SettingsSection("My devices", T("Your phone and computer linked to your account (Profile & account > Link another device...)."));
            DrawAccountSettings(fieldW, h, 1);
            break;
        case kConnections: {
            SettingsSection("In the background");
            // running in the background: phones keep a connection (foreground service), computers can close to the tray
            if (IsMobilePlatform()) {
                if (ui::Checkbox(T("Keep running in the background"), &edit.backgroundRun)) {
                    changed = true;
                    if (Bridge().backgroundService) Bridge().backgroundService(edit.backgroundRun);
                }
                ui::HelpMarker(T("Messages and calls arrive while LunaShare is in the background (a small notification shows that it is "
                                 "connected). Uses a little battery."));
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
                    if (ImGui::Button(T("Allow running in the background..."), ImVec2(bw, h)) && Bridge().requestBatteryExemption)
                        Bridge().requestBatteryExemption();
                }
            } else {
                changed |= ui::Checkbox(T("Keep running when the window is closed"), &edit.closeToTray);
#if defined(_WIN32)
                ui::HelpMarker(T("Closing the window keeps LunaShare connected for messages and calls (in the tray next to the clock). Quit it from the tray icon."));
#else
                ui::HelpMarker(T("Closing the window only minimises it, so LunaShare stays connected for messages and calls."));
#endif
            }

            SettingsSection("Internet");
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
                ui::HelpMarker(T("Lets your devices find each other outside the local network. The server only introduces them; chat and "
                                 "files are end-to-end encrypted and go directly between the devices (through the server only when no direct "
                                 "path is possible). Leave empty for local network, hotspot and Bluetooth only."));
                ImGui::SetNextItemWidth(fieldW);
                ImGui::InputTextWithHint("##wanserver", "home.example.com", &edit.wanServer);
                if (ImGui::IsItemActive()) editDirty = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    edit.wanEnabled = !edit.wanServer.empty();  // a server set = use it
                    changed = true;
                }
            }
            if (eng.settings.wanEnabled && !eng.settings.wanServer.empty()) {
                ImGui::TextDisabled(T("Status: %s"), TDynamic(eng.WanStatus()).c_str());
                std::string mp = eng.WanMappedEndpoint();
                if (!mp.empty()) ImGui::TextDisabled(T("Reachable through the router at %s"), mp.c_str());
            }

            SettingsSection("Invites", T("Invite people with a code instead of sharing the room: each of them can reach you through the "
                                         "server, and they only see each other if you invite them into the same invite room."));
            DrawPrivateContacts(fieldW, h, changed);

            SettingsSection("Bluetooth");
            changed |= ui::Checkbox(T("Accept Bluetooth connections##s"), &edit.bluetooth);

            ImGui::Spacing();
            static bool advEnv = getenv("LUNASHARE_ADVANCED") != nullptr;  // dev: start with Advanced open (screenshots)
            if (advEnv) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (settingsGoto == "Advanced") ImGui::SetNextItemOpen(true);
            if (!settingsGoto.empty() && settingsGoto == "Advanced") {
                ImGui::SetScrollHereY(0.1f);
                settingsGoto.clear();
            }
            if (ImGui::CollapsingHeader(T("Advanced"), ImGuiTreeNodeFlags_None)) {
                bool invited = edit.wanFromInvite;
                ImGui::SeparatorText(T("Internet"));
                changed |= ui::Checkbox(T("Use the rendezvous server"), &edit.wanEnabled);
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
                    ui::HelpMarker(T("Devices that use the same server, room name and password see each other. Without a room name, every "
                                     "device on the server is in one shared room - set a name and password to keep your devices to yourself."));
                    wanField(T("Room password"), T("shared by your devices"), &edit.wanPassword, ImGuiInputTextFlags_Password);
                }
                changed |= ui::Checkbox(T("Relay through the server when a direct connection fails"), &edit.wanRelay);
                ui::HelpMarker(T("Some networks can't be reached directly at all (mobile data and other carrier-grade NAT on both sides, "
                                 "strict firewalls). The server then passes the data along. It stays end-to-end encrypted, the server can't read it."));
                changed |= ui::Checkbox(T("Group chats and calls through the server"), &edit.groupsViaServer);
                ui::HelpMarker(T("People in your groups whom you don't know otherwise don't connect to your device directly, so they don't "
                                 "learn your internet address. Messages go through the server's mailbox and calls through the server, "
                                 "end-to-end encrypted. In a group call everyone also sends their voice and video only once, to the "
                                 "server, instead of to every person - easier on phones."));
                changed |= ui::Checkbox(T("Ask the router for a port mapping (UPnP / NAT-PMP)"), &edit.wanNatPmp);
                ui::HelpMarker(T("Most home routers allow this automatically. It makes direct connections work behind routers that change the "
                                 "port for every destination."));
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
                if (ImGui::InputInt(T("Fixed local UDP port"), &edit.wanLocalPort, 0)) {
                    edit.wanLocalPort = std::clamp(edit.wanLocalPort, 0, 65535);
                    editDirty = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
                if (ImGui::InputInt(T("Port forwarded on the router"), &edit.wanForwardedPort, 0)) {
                    edit.wanForwardedPort = std::clamp(edit.wanForwardedPort, 0, 65535);
                    editDirty = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                ui::HelpMarker(T("Only if you forward a UDP port on your router to this device by hand: the local port above, the external port here."));
                ImGui::EndDisabled();
                if (eng.settings.wanEnabled) {
                    std::string ep = eng.WanPublicEndpoint();
                    if (!ep.empty()) ImGui::TextDisabled(T("Seen by the server as %s"), ep.c_str());
                    if (!invited && ImGui::Button(T("Copy room hash (for the server's allow-list)"), ImVec2(0, h)))
                        ImGui::SetClipboardText(RoomHash(eng.settings.wanRoom, eng.settings.wanPassword).c_str());
                }
                if (!invited) {
                    changed |= ui::Checkbox(T("Private mode: don't list me in the room"), &edit.wanHidden);
                    ui::HelpMarker(T("Others in the room don't see this device. You still see them and can contact them; your own devices "
                                     "and people with one of your invite codes still find you."));
                }
                if (eng.WanServerLacksLinks())
                    ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", T("The server is too old for private contacts: update it."));

                ImGui::SeparatorText(T("Network"));
                changed |= ui::Checkbox(T("Find devices on the local network"), &edit.discovery);
                ImGui::BeginDisabled(!edit.discovery);
                ImGui::Indent();
                changed |= ui::Checkbox(T("Bonjour / mDNS"), &edit.mdns);
                ui::HelpMarker(T("Standard service discovery (_lunashare._tcp). Works on most routers and is the only method iPhones/iPads can use."));
                changed |= ui::Checkbox(T("UDP broadcast beacons"), &edit.broadcast);
                ui::HelpMarker(T("Fast discovery that also works on networks without multicast. Not available on iOS."));
                ImGui::Unindent();
                ImGui::EndDisabled();
                changed |= ui::Checkbox(T("Connect to found devices automatically"), &edit.autoConnect);
                int port = edit.tcpPort;
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
                if (ImGui::InputInt(T("TCP port"), &port, 0)) {
                    edit.tcpPort = (uint16_t)std::clamp(port, 1, 65535);
                    editDirty = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                ImGui::SameLine();
                ImGui::TextDisabled(T("(listening on %u)"), eng.boundPort);

                ImGui::SeparatorText(T("Transfers"));
                ImGui::SetNextItemWidth(fieldW);
                changed |= ImGui::SliderInt("##par", &edit.maxParallel, 1, 8, T("Files at once per device: %d"));
                ImGui::SetNextItemWidth(fieldW);
                changed |= ImGui::SliderInt("##chunk", &edit.chunkKB, 16, 2048, T("Chunk size: %d KB"), ImGuiSliderFlags_Logarithmic);
#if defined(__linux__) && !defined(__ANDROID__)
                ImGui::SeparatorText(T("Bluetooth"));
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
                changed |= ImGui::SliderInt(T("RFCOMM channel"), &edit.btChannel, 1, 30);
                ui::HelpMarker(T("Linux uses a fixed RFCOMM channel. Use the same channel on both Linux machines."));
#endif
            }
            break;
        }
        case kAppearance: {
            SettingsSection("Appearance");
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
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                changed = true;
                styleGeneration++;
            }
            ImGui::SetNextItemWidth(fieldW);
            if (ImGui::Combo("##theme", &edit.theme, TItems("Theme: dark\0Theme: light\0Theme: classic\0Theme: twilight\0Theme: moonlight\0"))) {
                changed = true;
                styleGeneration++;
            }
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
            break;
        }
        case kBackup: {
            SettingsSection("Your account", T("Make a backup: if you lose all your devices, only the recovery phrase or a backup file can bring "
                                              "your account back."));
            DrawAccountSettings(fieldW, h, 2);
            SettingsSection("Backup / move to another device",
                            T("Exports all settings plus your list of known devices with their nicknames, trust and block state, the "
                              "music playlists, folders and sound settings, and where you stopped in movies."));
            ui::Checkbox(T("Include chat & shared files history"), &exportHistory);
            ui::Checkbox(T("Include this device's identity (only for restoring THIS device)"), &exportIdentity);
            if (ui::IconTextButton(T("Export settings..."), Icon::Up, ImVec2(bw, h))) {
                browseFor = BrowseFor::Export;
                std::string name = "lunashare-settings-" + FormatTime(NowMs(), true).substr(0, 10) + ".json";
                browser.Open(FileBrowser::Mode::SaveFile, T("Export settings"), eng.settings.downloadDir, name, ".json");
            }
            if (!mobile) ImGui::SameLine();
            if (ui::IconTextButton(T("Import settings..."), Icon::Down, ImVec2(bw, h))) {
                browseFor = BrowseFor::Import;
                browser.Open(FileBrowser::Mode::OpenFile, T("Import settings"), eng.settings.downloadDir, "", ".json");
            }
            ui::Checkbox(T("When importing, also take over the device identity"), &importIdentity);
            break;
        }
        case kAbout: {
            SettingsSection("About");
            ImGui::TextDisabled(T("LunaShare %s  -  %s  -  protocol v%d"), LUNASHARE_VERSION, PlatformName(), kProtocolVersion);
            {
                Settings us = eng.settings;
                if (ui::Checkbox(T("Check for a new version once a day"), &us.updateCheck)) {
                    eng.ApplySettings(us);
                    edit.updateCheck = us.updateCheck;
                }
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
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled(T("Settings folder: %s"), (eng.configDir.empty() ? ConfigDir() : eng.configDir).c_str());
            ImGui::TextDisabled("%s", T("Every file is hashed with SHA-256 before sending and again after receiving; transfers can be paused, resumed, and survive restarts."));
            ImGui::TextDisabled("%s", T("All connections are end-to-end encrypted (X25519 key exchange, XChaCha20-Poly1305, EdDSA device keys pinned on first contact)."));
            ImGui::TextDisabled(T("This device's key fingerprint: %s"), KeyFingerprint(eng.MyPublicKey()).c_str());
            ImGui::PopTextWrapPos();
            break;
        }
    }
}
