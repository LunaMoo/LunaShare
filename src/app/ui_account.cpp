// Settings > Your account: the account's devices, linking another device, recovery phrase and backup files.
#include <algorithm>

#include "emoji_host.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "media_host.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;

static void CenterNext(float wFrac) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (ui::M().mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.94f, 0));
    else if (wFrac > 0) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * wFrac, 0), ImGuiCond_Appearing);
}

void App::DrawAccountSettings(float fieldW, float h) {
    bool mobile = ui::M().mobile;
    float bw = mobile ? -1 : 0;
    ImGui::SeparatorText(T("Your account"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", T("Your account belongs to you, not to a server. Link your phone and computer to it: messages then reach all "
                                "of them and your contacts see one person."));
    ImGui::PopTextWrapPos();
    std::string me = eng.settings.deviceId;
    for (auto& ad : eng.settings.accountDevices) {
        ImGui::PushID(ad.id.c_str());
        std::string label;
        if (ad.id == me) {
            label = std::string(T("This device")) + "  (" + PlatformName() + ")";
        } else {
            auto it = eng.devices.find(ad.id);
            std::string plat = it != eng.devices.end() ? it->second.platform : "";
            std::string host = it != eng.devices.end() ? it->second.host : "";
            label = (host.empty() ? eng.PeerName(ad.id) : host) + (plat.empty() ? "" : "  (" + plat + ")");
            label += eng.IsConnected(ad.id) ? std::string("  - ") + T("connected") : std::string("  - ") + T("offline");
        }
        ImGui::Bullet();
        ImGui::TextUnformatted(label.c_str());
        if (ad.id != me) {
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Remove"))) {
                removeDeviceAsk = ad.id;
                openRemoveDevice = true;
            }
        }
        ImGui::PopID();
    }
    if (ImGui::Button(T("Link another device..."), ImVec2(fieldW > 0 ? fieldW : -1, h))) {
        openLinkPick = true;
        linkTarget.clear();
    }
    {
        Settings s = eng.settings;
        if (ImGui::Checkbox(T("The same playlists and settings on my devices"), &s.syncOwn)) {
            eng.ApplySettings(s);
            edit.syncOwn = s.syncOwn;
        }
        ImGui::SameLine();
        ui::HelpMarker(T("Music playlists, the sound settings (equalizer, transitions, remix, volume levelling), the movie player's "
                         "options, silent chats and the language follow you to your other linked devices. A playlist changed on two "
                         "devices: the later change counts. Songs are found by their name and tags, wherever they are on each device."));
        s = eng.settings;
        if (ImGui::Checkbox(T("Share the clipboard with my devices"), &s.clipShare)) {
            eng.ApplySettings(s);
            edit.clipShare = s.clipShare;
        }
        ImGui::SameLine();
        ui::HelpMarker(T("Text copied on one of your linked devices can be pasted on the others (while LunaShare is open on both; "
                         "on a phone, while it's in front). Off by default: a clipboard often holds passwords."));
        DrawFolderSyncSettings(h);
    }
    if (!eng.settings.accountBackedUp) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1),
                           "%s", T("Make a backup: if you lose all your devices, only the recovery phrase or a backup file can bring your account back."));
        ImGui::PopTextWrapPos();
    }
    if (ImGui::Button(T("Recovery phrase..."), ImVec2(bw, h))) {
        phraseShown = eng.RecoveryPhrase();
        openPhrase = true;
    }
    if (!mobile) ImGui::SameLine();
    if (ImGui::Button(T("Save backup file..."), ImVec2(bw, h))) {
        browseFor = BrowseFor::BackupSave;
        browser.Open(FileBrowser::Mode::SaveFile, T("Save account backup"), eng.settings.downloadDir, "lunashare-account.lunabackup", ".lunabackup");
    }
    if (!mobile) ImGui::SameLine();
    if (ImGui::Button(T("Restore account..."), ImVec2(bw, h))) {
        restoreBuf.clear();
        accountError.clear();
        openRestore = true;
    }
    ImGui::TextDisabled(T("Account fingerprint: %s"), KeyFingerprint(eng.MyAccount()).c_str());
    // username on the internet server: others can find you by it
    if (eng.settings.wanEnabled && !eng.settings.wanServer.empty() && !eng.settings.wanFromInvite) {
        ImGui::TextUnformatted(T("Username on your server (others can find you by it):"));
        if (usernameBuf.empty() && !ImGui::IsAnyItemActive() && !eng.myUsername.empty()) usernameBuf = eng.myUsername;
        ImGui::SetNextItemWidth(fieldW > 0 ? fieldW * 0.6f : -1);
        bool enter = ImGui::InputTextWithHint("##uname", T("username"), &usernameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!mobile) ImGui::SameLine();
        bool same = usernameBuf == eng.myUsername;
        ImGui::BeginDisabled(same || usernameBuf.empty() || eng.usernamePending);
        if (ImGui::Button(T("Save##uname"), ImVec2(bw, h)) || (enter && !same && !usernameBuf.empty())) eng.ClaimUsername(usernameBuf);
        ImGui::EndDisabled();
        if (!eng.myUsername.empty()) {
            if (!mobile) ImGui::SameLine();
            if (ImGui::Button(T("Remove##uname"), ImVec2(bw, h))) {
                eng.ClaimUsername("");
                usernameBuf.clear();
            }
        }
        if (!eng.myUsername.empty()) ImGui::TextDisabled(T("You are @%s on this server."), eng.myUsername.c_str());
        if (!eng.usernameError.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", UsernameError(eng.usernameError).c_str());
    }
}

void App::DrawAccountPopups() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    float bw = mobile ? -1 : 0;
    auto wrapped = [](const char* t) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * (ui::M().mobile ? 0 : 30));
        ImGui::TextUnformatted(t);
        ImGui::PopTextWrapPos();
    };

    // ---- link another device: pick it, then wait for its answer
    if (openLinkPick) { ImGui::OpenPopup(T("Link a device")); openLinkPick = false; }
    CenterNext(0);
    if (ImGui::BeginPopupModal(T("Link a device"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (linkTarget.empty()) {
            wrapped(T("Open LunaShare on the other device and connect both (same Wi-Fi, phone hotspot, Bluetooth or your internet "
                      "server). Then choose it here:"));
            int n = 0;
            for (auto& c : eng.conns) {
                if (!c->ready || c->closed) continue;
                auto it = eng.devices.find(c->peerId);
                if (it == eng.devices.end() || it->second.accountPub == eng.MyAccount()) continue;
                n++;
                std::string label = it->second.DisplayName() + "  (" + it->second.platform + ")";
                if (ImGui::Selectable((label + "##" + c->peerId).c_str(), false, 0, ImVec2(0, h)) && eng.LinkDevice(c->peerId))
                    linkTarget = c->peerId;
            }
            if (!n) ImGui::TextDisabled("%s", T("No connected device yet."));
            if (ImGui::Button(T("Cancel"), ImVec2(bw, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        } else if (!eng.LinkPending(linkTarget)) {
            ImGui::CloseCurrentPopup();  // answered (the result comes as a message) or timed out
            linkTarget.clear();
        } else {
            ImGui::Text(T("Waiting for %s to accept..."), eng.PeerName(linkTarget).c_str());
            wrapped(T("Make sure it shows the same code:"));
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.2f);
            ImGui::TextUnformatted(eng.LinkCode(linkTarget).c_str());
            ImGui::PopFont();
            if (ImGui::Button(T("Cancel"), ImVec2(bw, h))) {
                linkTarget.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // ---- another device asks this one to join its account
    if (openLinkAsk) { ImGui::OpenPopup(T("Link this device?")); openLinkAsk = false; }
    CenterNext(0);
    if (ImGui::BeginPopupModal(T("Link this device?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * (mobile ? 0 : 30));
        ImGui::Text(T("%s wants to add this device to its account. It must show the same code:"), linkAskName.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.2f);
        ImGui::TextUnformatted(linkAskCode.c_str());
        ImGui::PopFont();
        wrapped(T("This device then uses that account's name and picture and gets its contacts and conversations. The chats on "
                  "this device are kept."));
        if (ImGui::Button(T("Link"), ImVec2(bw, h))) {
            eng.AnswerLink(linkAskPeer, true);
            ImGui::CloseCurrentPopup();
        }
        if (!mobile) ImGui::SameLine();
        if (ImGui::Button(T("Decline"), ImVec2(bw, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            eng.AnswerLink(linkAskPeer, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- recovery phrase
    if (openPhrase) { ImGui::OpenPopup(T("Recovery phrase")); openPhrase = false; }
    CenterNext(0);
    if (ImGui::BeginPopupModal(T("Recovery phrase"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        wrapped(T("These 24 words bring your account back on a new device. Write them down and keep them somewhere safe. Anyone "
                  "with them can become you - never send them to anyone."));
        std::vector<std::string> words;
        size_t a = 0;
        while (a < phraseShown.size()) {
            size_t b = phraseShown.find(' ', a);
            if (b == std::string::npos) b = phraseShown.size();
            words.push_back(phraseShown.substr(a, b - a));
            a = b + 1;
        }
        int cols = mobile ? 2 : 3;
        if (ImGui::BeginTable("##words", cols, ImGuiTableFlags_SizingStretchSame)) {
            int rows = ((int)words.size() + cols - 1) / cols;
            for (int r = 0; r < rows; r++)
                for (int c = 0; c < cols; c++) {
                    ImGui::TableNextColumn();
                    int i = c * rows + r;
                    if (i < (int)words.size()) ImGui::Text("%2d. %s", i + 1, words[(size_t)i].c_str());
                }
            ImGui::EndTable();
        }
        if (ImGui::Button(T("I wrote them down"), ImVec2(bw, h))) {
            eng.MarkBackedUp();
            phraseShown.clear();
            ImGui::CloseCurrentPopup();
        }
        if (!mobile) ImGui::SameLine();
        if (ImGui::Button(T("Close"), ImVec2(bw, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            phraseShown.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- restore: from the phrase, or open a backup file
    if (openRestore) { ImGui::OpenPopup(T("Restore account")); openRestore = false; }
    CenterNext(0);
    if (ImGui::BeginPopupModal(T("Restore account"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        wrapped(T("This device joins the restored account; the account it has now is replaced. Your other devices of that account "
                  "find it again when they connect."));
        ImGui::TextUnformatted(T("Recovery phrase (24 words):"));
        ImGui::InputTextMultiline("##phrase", &restoreBuf, ImVec2(mobile ? -1 : ImGui::GetFontSize() * 30, ImGui::GetTextLineHeight() * 4));
        if (!accountError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", accountError.c_str());
        if (ImGui::Button(T("Restore from the phrase"), ImVec2(bw, h))) {
            std::string err;
            if (eng.RestoreFromPhrase(restoreBuf, &err)) {
                restoreBuf.clear();
                infoMessage = T("Account restored. Contacts and conversations come back from your other devices when they connect.");
                openInfo = true;
                ImGui::CloseCurrentPopup();
            } else {
                accountError = TDynamic(err);
            }
        }
        if (!mobile) ImGui::SameLine();
        if (ImGui::Button(T("Open a backup file..."), ImVec2(bw, h))) {
            browseFor = BrowseFor::BackupOpen;
            browser.Open(FileBrowser::Mode::OpenFile, T("Open account backup"), eng.settings.downloadDir, "", ".lunabackup");
            ImGui::CloseCurrentPopup();
        }
        if (!mobile) ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(bw, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- backup file password (save or open)
    if (openBackupPass) { ImGui::OpenPopup(T("Backup password")); openBackupPass = false; }
    CenterNext(0);
    if (ImGui::BeginPopupModal(T("Backup password"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        float fw = mobile ? -1 : ImGui::GetFontSize() * 18;
        wrapped(backupSaving ? T("The backup holds your account key, contacts and conversations, encrypted with this password. "
                                 "Without the password it can't be opened - not even by you.")
                             : T("Password of the backup:"));
        ImGui::SetNextItemWidth(fw);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputTextWithHint("##bp1", T("password"), &backupPass, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        if (backupSaving) {
            ImGui::SetNextItemWidth(fw);
            enter = ImGui::InputTextWithHint("##bp2", T("repeat the password"), &backupPass2, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        }
        if (!accountError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", accountError.c_str());
        if (ImGui::Button(backupSaving ? T("Save backup") : T("Restore"), ImVec2(bw, h)) || enter) {
            std::string err;
            if (backupSaving && backupPass != backupPass2) {
                accountError = T("The passwords are different.");
            } else if (backupSaving ? eng.SaveBackup(backupPath, backupPass, &err) : eng.RestoreBackup(backupPath, backupPass, &err)) {
                infoMessage = backupSaving ? T("Backup saved to\n") + backupPath : std::string(T("Account restored from the backup."));
                openInfo = true;
                backupPass.clear();
                backupPass2.clear();
                ImGui::CloseCurrentPopup();
            } else {
                accountError = TDynamic(err);
            }
        }
        if (!mobile) ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(bw, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            backupPass.clear();
            backupPass2.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- remove one of my devices
    if (openRemoveDevice) { ImGui::OpenPopup(T("Remove device from account?")); openRemoveDevice = false; }
    CenterNext(0);
    if (ImGui::BeginPopupModal(T("Remove device from account?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * (mobile ? 0 : 28));
        ImGui::Text(T("%s will no longer get your messages. When it connects again it gets an account of its own."),
                    eng.PeerName(removeDeviceAsk).c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button(T("Remove"), ImVec2(bw, h))) {
            eng.RemoveMyDevice(removeDeviceAsk);
            ImGui::CloseCurrentPopup();
        }
        if (!mobile) ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(bw, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---- folders that go to my other devices by themselves (photos from the phone to the computer...)
void App::DrawFolderSyncSettings(float h) {
    bool mobile = ui::M().mobile;
    auto mine = eng.MyOtherDevices();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Folders sent to my devices by themselves"));
    ImGui::SameLine();
    ui::HelpMarker(T("New and changed files of these folders go to the chosen device of yours by themselves - checked every "
                     "minute and when it comes online - into its download folder, in a folder of the same name. Nothing is "
                     "deleted there. A file changed later arrives again as a new copy."));
    Settings s = eng.settings;
    bool changed = false;
    int remove = -1;
    for (size_t i = 0; i < s.folderSyncs.size(); i++) {
        auto& f = s.folderSyncs[i];
        ImGui::PushID((int)i);
        changed |= ImGui::Checkbox("##on", &f.on);
        ImGui::SameLine();
        ImGui::TextUnformatted(f.path.c_str());
        std::string to = f.to.empty() ? std::string(T("(choose a device)")) : Emoji().Display(eng.PeerName(f.to));
        ImGui::SetNextItemWidth(mobile ? ImGui::GetContentRegionAvail().x * 0.6f : ImGui::GetFontSize() * 12);
        if (ImGui::BeginCombo("##to", (std::string("-> ") + to).c_str())) {
            for (auto& d : mine)
                if (ImGui::Selectable(Emoji().Display(eng.PeerName(d)).c_str(), d == f.to, 0, ImVec2(0, h))) {
                    f.to = d;
                    changed = true;
                }
            if (mine.empty()) ImGui::TextDisabled("%s", T("No other devices linked to your account yet."));
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        std::string st = eng.FolderSyncStatus(f.id);
        int sent = std::atoi(st.c_str()), way = std::atoi(st.c_str() + st.find('|') + 1);
        if (way) ImGui::TextDisabled("%s", TF("%d sent, %d on the way", sent, way).c_str());
        else ImGui::TextDisabled("%s", TF("%d sent", sent).c_str());
        ImGui::SameLine();
        if (ui::IconButton("rm", ui::Icon::Trash, T("Stop sending this folder"), mobile ? ui::M().rowH * 0.7f : ImGui::GetFrameHeight())) remove = (int)i;
        ImGui::PopID();
    }
    if (remove >= 0) {
        s.folderSyncs.erase(s.folderSyncs.begin() + remove);
        changed = true;
    }
    if (changed) {
        eng.ApplySettings(s);
        edit.folderSyncs = s.folderSyncs;
        eng.FolderSyncSoon();
    }
    if (ImGui::Button(T("Add a folder..."), ImVec2(mobile ? -1 : 0, h))) {
        browseFor = BrowseFor::SyncFolder;
        browser.Open(FileBrowser::Mode::SelectDir, T("A folder to send to my devices"), eng.settings.downloadDir);
    }
}

// ---- verifying a contact by QR code (Device details > Verify with QR codes...): my code for them to scan, and the
// camera to scan theirs. A match marks that device and every device of their account verified.
void App::DrawVerifyQrPopup() {
    if (verifyQrOpen) {
        ImGui::OpenPopup("##verifyqr");
        verifyQrOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.94f, 0));
    if (!ImGui::BeginPopupModal("##verifyqr", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (Media().Scanning()) Media().CloseScanner();  // (closed some other way)
        return;
    }
    float side = mobile ? ImGui::GetContentRegionAvail().x : ImGui::GetFontSize() * 18;
    std::string who = Emoji().Display(eng.PeerName(verifyQrPeer));
    ImGui::TextUnformatted(TF("Verify %s", who.c_str()).c_str());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + side);
    // the two halves: my code / the camera
    if (ImGui::RadioButton(T("My code"), !verifyQrScan) && verifyQrScan) {
        verifyQrScan = false;
        Media().CloseScanner();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!Media().CanScan() && !Media().Scanning());
    if (ImGui::RadioButton(T("Scan their code"), verifyQrScan) && !verifyQrScan) {
        verifyQrScan = true;
        verifyQrResult = 0;
        Media().audioError.clear();
        if (!Media().OpenScanner()) verifyQrMsg = Media().audioError.empty() ? T("camera could not be started") : Media().audioError;
        else verifyQrMsg.clear();
    }
    ImGui::EndDisabled();
    if (!verifyQrScan) {
        ImGui::TextDisabled("%s", T("On their device: Devices, you, Verify with QR codes..., Scan their code - and point it here."));
        ImGui::Spacing();
        float s = std::min(side, ImGui::GetFontSize() * 16);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (side - s) * 0.5f);
        ui::QrCode(eng.VerifyCode().c_str(), s);
    } else {
        if (Media().Scanning()) {
            std::string code = Media().ScannedText();
            if (!code.empty()) {
                int r = eng.VerifyByCode(verifyQrPeer, code);
                if (r == 0) {
                    verifyQrResult = 2;  // (some other QR code: keep looking)
                } else {
                    verifyQrResult = r;
                    Media().CloseScanner();
                }
            }
        }
        if (Media().Scanning()) {
            ImGui::TextDisabled("%s", T("Point the camera at the code on their screen."));
            ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + side, p0.y + side * 0.75f);
            ImGui::Dummy(ImVec2(side, side * 0.75f));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
            Media().UploadVideoNow();
            if (Media().LocalVideo().valid) Media().DrawVideo(dl, Media().LocalVideo(), p0, p1, true);
            if (verifyQrResult == 2) ImGui::TextDisabled("%s", T("That's not a LunaShare code."));
        } else if (verifyQrResult == 1) {
            ImGui::TextColored(ImVec4(0.35f, 0.8f, 0.45f, 1), "%s", TF("Verified: the keys are the same. %s is who LunaShare says.", who.c_str()).c_str());
            ImGui::TextDisabled("%s", T("Now let them scan your code too."));
        } else if (verifyQrResult == -1) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s",
                               T("The keys are different! Either this is another person's code, or someone is in between. Not verified."));
        } else if (!verifyQrMsg.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", verifyQrMsg.c_str());
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::Button(T("Close"), ImVec2(mobile ? -1 : side, h))) {
        Media().CloseScanner();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
