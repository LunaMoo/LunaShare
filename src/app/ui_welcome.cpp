// LunaShare - the welcome of a first start (name and picture, adding the first person, a few hints) and "Add
// someone": the friendly way to get two devices together -
//   nearby      the devices found on the same network, one tap to open the chat
//   QR code     mine to show, theirs to scan with the camera: connects by address and verifies the keys in one go
//   Internet    an invite code used or (with a rendezvous server set up) made, to copy, show as a QR code or share
//   the app     where the other person gets LunaShare (a link and its QR code)
// The technical ways (address, Bluetooth, hotspot, rooms) stay on the Connect page.
#include <algorithm>

#include "emoji_host.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "media_host.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"
#include "wan.h"

using namespace ls;
using ui::Icon;

namespace {
const char* kGetUrl = "https://github.com/LunaMoo/LunaShare/releases/latest";

void Wrapped(const char* s, bool dim = true) {
    ImGui::PushTextWrapPos(0);
    if (dim) ImGui::TextDisabled("%s", s);
    else ImGui::TextUnformatted(s);
    ImGui::PopTextWrapPos();
}

// text in the middle of the line
void Centered(const char* s, float scale = 1.0f, bool dim = false) {
    if (scale != 1.0f) ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
    float w = ImGui::GetContentRegionAvail().x, tw = ImGui::CalcTextSize(s, nullptr, false, w).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (w - tw) * 0.5f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(w, tw + 1));
    if (dim) ImGui::TextDisabled("%s", s);
    else ImGui::TextUnformatted(s);
    ImGui::PopTextWrapPos();
    if (scale != 1.0f) ImGui::PopFont();
}

// a line with an icon in front (the hints of the welcome)
void IconLine(Icon ic, const char* title, const char* text) {
    float fs = ImGui::GetFontSize();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(p.x + fs * 1.1f, p.y + fs * 1.1f), fs * 1.1f, ImGui::GetColorU32(ImGuiCol_FrameBg));
    ui::DrawIcon(dl, ic, ImVec2(p.x + fs * 1.1f, p.y + fs * 1.1f), fs * 1.1f, ui::ColAccent());
    ImGui::Dummy(ImVec2(fs * 2.2f, fs * 2.2f));
    ImGui::SameLine(0, fs * 0.7f);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(title);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, fs * 0.25f));
}
}  // namespace

// ---- invite codes (also used by Settings > Internet)

// someone's invite code: -1 = not valid / can't be used (msg says why), 0 = had it already, 1 = added (edit changed:
// the caller applies the settings)
int App::UseInviteCode(const std::string& code, std::string& msg) {
    InviteInfo ii;
    std::string c = code;
    while (!c.empty() && isspace((unsigned char)c.back())) c.pop_back();
    while (!c.empty() && isspace((unsigned char)c.front())) c.erase(c.begin());
    if (!ParseInviteCode(c, ii)) {
        msg = T("That isn't a valid invite code.");
        return -1;
    }
    if (edit.wanEnabled && !edit.wanServer.empty() && edit.wanServer != ii.server) {
        msg = T("That code is for a different server than the one you use.");  // (no addresses: they may be private)
        return -1;
    }
    bool have = false;
    for (auto& l : edit.wanLinks) have |= l.secret == ii.secret;
    if (!have) {
        if (!edit.wanEnabled || edit.wanServer.empty()) {
            // joined only for this: stay out of the server's shared room
            edit.wanServer = ii.server;
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
        l.secret = ii.secret;
        l.label = ii.name;
        l.created = NowMs();
        l.mine = false;
        l.room = ii.room;  // (its name, shown next to the inviter)
        edit.wanLinks.push_back(l);
    }
    msg = ii.name.empty() ? std::string(T("Added. You'll see each other in the device list when you're both online."))
                          : TF("Added %s. You'll see each other in the device list when you're both online.", ii.name.c_str());
    return have ? 0 : 1;
}

// a new invite of mine (no room: that person sees only me); "" when there's no server of my own to invite to
std::string App::CreateInvite(const std::string& forWhom) {
    if (edit.wanServer.empty() || edit.wanFromInvite) return {};
    uint8_t k[32];
    SecureRandom(k, sizeof(k));
    WanLink l;
    l.secret = ToHex(k, sizeof(k));
    l.label = forWhom;
    l.created = NowMs();
    edit.wanLinks.push_back(l);
    InviteInfo i;
    i.server = edit.wanServer;
    i.secret = l.secret;
    i.name = edit.displayName;
    return MakeInviteCode(i);
}

// ---- add someone

void App::OpenAddPerson(int section) {
    addPersonOpen = true;
    if (section >= 0) addSection = section;
    addMsg.clear();
}

// a code that was scanned or pasted: a connect code (QR), or an invite
void App::HandleAddCode(const std::string& text) {
    std::string pub, verify, msg;
    addMsgErr = false;
    if (eng.UseConnectCode(text, pub, verify)) {
        connectPendingPub = pub;
        connectPendingVerify = verify;
        connectPendingUntil = SteadyMs() + 15000;
        addMsg = T("Connecting...");
        return;
    }
    if (text.compare(0, 5, "LSV1:") == 0) {
        addMsg = T("That code only verifies someone you already have (Devices > the person > Verify with QR codes).");
        addMsgErr = true;
        return;
    }
    if (text.compare(0, 5, "LSC1:") == 0) {
        addMsg = T("That's your own code - scan the one on the other device.");
        addMsgErr = true;
        return;
    }
    int r = UseInviteCode(text, msg);
    addMsg = msg;
    addMsgErr = r < 0;
    if (r > 0) {
        eng.ApplySettings(edit);
        editDirty = false;
    }
}

// every frame: someone whose code was scanned connects -> verified, and their chat opens
void App::AddPersonTick() {
    if (connectPendingPub.empty()) return;
    busy = true;
    std::string id = eng.ConnectedDeviceByKey(connectPendingPub);
    if (!id.empty()) {
        bool ok = eng.VerifyByCode(id, connectPendingVerify) == 1;
        std::string who = Emoji().Display(eng.PeerName(id));
        toasts.push_back({EventType::Info, T("Add someone"), ok ? TF("%s added and verified.", who.c_str()) : TF("%s added.", who.c_str()), "", 0, 5.0f});
        connectPendingPub.clear();
        addMsg.clear();
        addPersonClose = true;
        if (welcomeStep >= 0) welcomeStep = 2;  // (the welcome: on to its last page)
        else {
            SelectPeer(eng.ChatKey(id));
            if (ui::M().mobile) {
                backStack = {MScreen::Home};
                screen = MScreen::Chat;
            } else {
                tab = Tab::Chat;
                tabForce = true;
            }
        }
    } else if (SteadyMs() > connectPendingUntil) {
        connectPendingPub.clear();
        addMsg = T("Couldn't reach them. Both devices must be on the same network (the same Wi-Fi, or one on the other's hotspot).");
        addMsgErr = true;
    }
}

// the four ways; returns true when a chat was opened (the caller closes)
bool App::DrawAddPersonBody(float width) {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.85f : 0;
    float fs = ImGui::GetFontSize();
    bool picked = false;
    // the sections, as a row of buttons (two rows on phones)
    struct Sec {
        const char* label;
        Icon icon;
    };
    Sec secs[] = {{T("Nearby"), Icon::Wifi}, {T("QR code"), Icon::Image}, {T("Internet"), Icon::Link}, {T("Get the app"), Icon::Down}};
    int perRow = mobile ? 2 : 4;
    float sp = ImGui::GetStyle().ItemSpacing.x, bw = (width - sp * (perRow - 1)) / perRow;
    for (int i = 0; i < 4; i++) {
        if (i % perRow) ImGui::SameLine();
        bool on = addSection == i;
        ui::PushAccent(on);
        if (ui::IconTextButton(secs[i].label, secs[i].icon, ImVec2(bw, h)) && !on) {
            if (addSection == 1 && Media().Scanning()) Media().CloseScanner();
            if (i != 1) addScan = false;  // (left the scanner: coming back shows my code, and "Scan" starts it afresh)
            addSection = i;
            addMsg.clear();
        }
        ui::PopAccent();
    }
    ImGui::Dummy(ImVec2(0, fs * 0.3f));
    auto message = [&] {
        if (addMsg.empty()) return;
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(addMsgErr ? ui::ColWarn() : ui::ColOk()), "%s", addMsg.c_str());
        ImGui::PopTextWrapPos();
    };
    auto share = [&](const std::string& text) {
        if (ImGui::Button(T("Copy"), ImVec2(0, h))) {
            ImGui::SetClipboardText(text.c_str());
            toasts.push_back({EventType::Info, "", T("Copied."), "", 0, 2.0f});
        }
        if (Bridge().shareText) {
            ImGui::SameLine();
            if (ui::IconTextButton(T("Share..."), Icon::Send, ImVec2(0, h))) Bridge().shareText(text);
        }
    };

    if (addSection == 0) {
        // ---- nearby: who's on the network
        Wrapped(T("Open LunaShare on the other phone or computer, on the same Wi-Fi as this one. It shows up here by itself:"));
        auto peers = SortedPeers();
        if (peers.empty()) {
            ImGui::Dummy(ImVec2(0, fs * 0.5f));
            static const char* dots[] = {".", "..", "..."};
            ImGui::TextDisabled("%s%s", T("Looking for devices"), dots[(int)(ImGui::GetTime() * 2) % 3]);
            busy = true;
        }
        for (auto& p : peers)
            if (DevicePeerRow(p, false, mobile ? ui::M().rowH * 1.3f : fs * 2.6f)) {
                SelectPeer(p);
                picked = true;
            }
        ImGui::Dummy(ImVec2(0, fs * 0.3f));
        if (ui::IconTextButton(T("Search again"), Icon::Retry, ImVec2(0, h))) eng.RefreshDiscovery();
        ImGui::SameLine();
        if (ImGui::Button(T("More ways..."), ImVec2(0, h))) {
            addPersonClose = true;
            addGoConnect = true;
        }
        ImGui::SetItemTooltip("%s", T("By address, Bluetooth, a phone's hotspot"));
        Wrapped(T("No shared Wi-Fi? One phone can make a hotspot for the other (More ways), or use the QR code."));
    } else if (addSection == 1) {
        // ---- QR: mine / scan theirs
        if (ImGui::RadioButton(T("Show my code"), !addScan) && addScan) {
            addScan = false;
            Media().CloseScanner();
        }
        ImGui::SameLine();
        // (the scanner can also have been stopped from elsewhere - then "Scan" must start it again, not look chosen)
        if (addScan && !Media().Scanning() && addMsg.empty()) addScan = false;
        ImGui::BeginDisabled(!Media().CanScan() && !Media().Scanning());
        if (ImGui::RadioButton(T("Scan their code"), addScan) && !Media().Scanning()) {
            addScan = true;
            addMsg.clear();
            Media().audioError.clear();
            if (!Media().OpenScanner()) {
                addMsg = Media().audioError.empty() ? T("camera could not be started") : Media().audioError;
                addMsgErr = true;
            }
        }
        ImGui::EndDisabled();
        if (!addScan) {
            Wrapped(T("On the other device: Add someone > QR code > Scan their code - and point its camera here. Both must be on the same network."));
            float s = std::min(width, fs * 20);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - s) * 0.5f);
            static std::string code;
            static int64_t codeAt = 0;
            if (code.empty() || SteadyMs() - codeAt > 5000) {  // (the addresses change with the network)
                code = eng.ConnectCode();
                codeAt = SteadyMs();
            }
            ui::QrCode(code.c_str(), s);
            if (LocalIPv4Addresses().empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ColWarn()), "%s", T("No network connection."));
        } else if (Media().Scanning()) {
            std::string code = Media().ScannedText();
            if (!code.empty()) {
                HandleAddCode(code);
                if (!addMsgErr) {
                    Media().CloseScanner();
                    addScan = false;
                }
            }
            if (Media().Scanning()) {
                Wrapped(T("Point the camera at the code on their screen."));
                ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + width, p0.y + width * 0.75f);
                ImGui::Dummy(ImVec2(width, width * 0.75f));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
                Media().UploadVideoNow();
                if (Media().LocalVideo().valid) Media().DrawVideo(dl, Media().LocalVideo(), p0, p1, true);
                busy = true;
            }
        }
        message();
        Wrapped(T("A scanned code also verifies them: you know the device is really theirs."));
    } else if (addSection == 2) {
        // ---- far away: invite codes
        ImGui::TextUnformatted(T("Got an invite code?"));
        ImGui::SetNextItemWidth(mobile ? -1 : width * 0.7f);
        bool enter = ImGui::InputTextWithHint("##addcode", "LSI1-...", &addCodeBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!mobile) ImGui::SameLine();
        if ((ImGui::Button(T("Use code"), ImVec2(0, h)) || enter) && !addCodeBuf.empty()) {
            HandleAddCode(addCodeBuf);
            if (!addMsgErr) addCodeBuf.clear();
        }
        // ...or scan it off their screen (the QR section's scanner reads invite codes too)
        ImGui::BeginDisabled(!Media().CanScan() && !Media().Scanning());
        ImGui::SameLine();
        if (ImGui::Button(T("Scan it"), ImVec2(0, h))) {
            addSection = 1;
            addScan = true;
            addMsg.clear();
            Media().audioError.clear();
            if (!Media().OpenScanner()) {
                addMsg = Media().audioError.empty() ? T("camera could not be started") : Media().audioError;
                addMsgErr = true;
            }
        }
        ImGui::EndDisabled();
        message();
        ImGui::Dummy(ImVec2(0, fs * 0.4f));
        ImGui::Separator();
        bool own = edit.wanEnabled && !edit.wanServer.empty() && !edit.wanFromInvite;
        if (own) {
            ImGui::TextUnformatted(T("Invite someone"));
            ImGui::SetNextItemWidth(mobile ? -1 : width * 0.7f);
            ImGui::InputTextWithHint("##addfor", T("for whom (only you see it)"), &addInviteFor);
            if (!mobile) ImGui::SameLine();
            if (ImGui::Button(T("Create invite code"), ImVec2(0, h))) {
                addInviteCode = CreateInvite(addInviteFor);
                addInviteFor.clear();
                if (!addInviteCode.empty()) {
                    eng.ApplySettings(edit);
                    editDirty = false;
                }
            }
            if (!addInviteCode.empty()) {
                float s = std::min(width, fs * 24);  // (an invite is a long code: a dense QR - big enough to scan)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - s) * 0.5f);
                ui::QrCode(addInviteCode.c_str(), s);
                ImGui::SetNextItemWidth(mobile ? -1 : width);
                ImGui::InputText("##addmade", &addInviteCode, ImGuiInputTextFlags_ReadOnly);
                share(TF("Join me on LunaShare. Get it here: %s - then Add someone > Internet, and paste this invite code: %s", kGetUrl, addInviteCode.c_str()));
                Wrapped(T("Send it to that person only - anyone with the code can reach you. They paste it under Add someone > Internet (or scan it)."));
            }
            if (ImGui::Button(T("All invites and rooms..."), ImVec2(0, h))) {
                addPersonClose = true;
                addGoInvites = true;
            }
        } else {
            Wrapped(edit.wanFromInvite
                        ? T("You reach people over the Internet through the server of someone who invited you. To invite people yourself you "
                            "need a server of your own (below).")
                        : T("Over the Internet two devices need a meeting point: a small rendezvous server (yours or a friend's) that only "
                            "introduces them - messages and files stay encrypted between you."));
            Wrapped(T("Someone who has one can send you an invite code - paste it above. To set up your own: Settings > Connections > Advanced "
                      "(docs/INTERNET.md)."));
        }
    } else {
        // ---- they don't have LunaShare yet
        Wrapped(T("They don't have LunaShare yet? It's free, for Android, Windows and Linux. Let them scan this, or send them the link:"), false);
        float s = std::min(width, fs * 13);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - s) * 0.5f);
        ui::QrCode(kGetUrl, s);
        Centered(kGetUrl, 1.0f, true);
        share(TF("Get LunaShare (free; Android, Windows, Linux): %s", kGetUrl));
    }
    return picked;
}

void App::DrawAddPersonPopup() {
    if (addPersonOpen) {
        ImGui::OpenPopup("##addperson");
        addPersonOpen = false;
        addPersonClose = false;
    }
    bool mobile = ui::M().mobile;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float w = mobile ? vp->WorkSize.x * 0.96f : ImGui::GetFontSize() * 32;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * (mobile ? 0.04f : 0.5f)), ImGuiCond_Appearing,
                            ImVec2(0.5f, mobile ? 0.0f : 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, vp->WorkSize.y * 0.94f));
    if (!ImGui::BeginPopupModal("##addperson", nullptr, ImGuiWindowFlags_NoTitleBar)) {
        if (Media().Scanning() && welcomeStep < 0 && addScan) {  // (closed some other way)
            Media().CloseScanner();
            addScan = false;
        }
        return;
    }
    if (mobile) ui::TouchScroll();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextUnformatted(T("Add someone"));
    ImGui::PopFont();
    bool picked = DrawAddPersonBody(ImGui::GetContentRegionAvail().x);
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
    bool close = ImGui::Button(T("Close"), ImVec2(mobile ? -1 : 0, mobile ? ui::M().rowH * 0.85f : 0)) || (!mobile && ImGui::IsKeyPressed(ImGuiKey_Escape));
    if (picked || close || addPersonClose) {
        if (Media().Scanning()) Media().CloseScanner();
        addScan = false;
        addPersonClose = false;
        connectPendingPub.clear();
        ImGui::CloseCurrentPopup();
        if (picked) {
            if (mobile) {
                backStack = {MScreen::Home};
                screen = MScreen::Chat;
            } else {
                tab = Tab::Chat;
                tabForce = true;
            }
        }
        if (addGoConnect) {
            if (mobile) {
                if (screen != MScreen::Nearby) backStack.push_back(screen);
                screen = MScreen::Nearby;
            } else {
                tab = Tab::Nearby;
                tabForce = true;
            }
        }
        if (addGoInvites) openInvites = true;
        addGoConnect = addGoInvites = false;
    }
    ImGui::EndPopup();
}

// ---- the welcome (a first start): instead of the app's pages until it's done or skipped

void App::WelcomeInit() {
    static const char* dev = getenv("LUNASHARE_WELCOME");  // dev: "0".."2" shows that page, "add" the Add someone popup
    bool fresh;
    {
        auto lk = eng.Lock();
        fresh = !eng.settings.welcomed && eng.devices.empty() && eng.chats.empty();
    }
    if (!eng.settings.welcomed && !fresh) WelcomeDone();  // (an install from before the welcome existed)
    if (fresh) {
        welcomeStep = 0;
        welcomeName = eng.settings.displayName;
    }
    if (dev && !strcmp(dev, "add")) OpenAddPerson(getenv("LUNASHARE_ADDSECTION") ? atoi(getenv("LUNASHARE_ADDSECTION")) : 0);
    else if (dev) {
        welcomeStep = std::clamp(atoi(dev), 0, 2);
        welcomeName = eng.settings.displayName;
        if (getenv("LUNASHARE_ADDSECTION")) addSection = atoi(getenv("LUNASHARE_ADDSECTION"));
    }
}

void App::WelcomeDone() {
    Settings s = eng.GetSettings();
    s.welcomed = true;
    eng.ApplySettings(s);
    edit.welcomed = true;
    welcomeStep = -1;
    if (Media().Scanning()) Media().CloseScanner();
    addScan = false;
    connectPendingPub.clear();
    addMsg.clear();
}

void App::DrawWelcome() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH : ImGui::GetFrameHeight() * 1.25f;
    float fs = ImGui::GetFontSize();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float w = mobile ? avail.x : std::min(avail.x, fs * 34);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - w) * 0.5f);
    ImGui::BeginChild("welcome", ImVec2(w, 0));
    if (mobile) ui::TouchScroll();
    w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0, mobile ? fs * 0.5f : std::max(fs, (avail.y - fs * 34) * 0.4f)));
    // where we are: three dots
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float r = fs * 0.22f, gap = fs * 0.9f, x0 = p.x + w * 0.5f - gap;
        for (int i = 0; i < 3; i++) dl->AddCircleFilled(ImVec2(x0 + gap * (float)i, p.y + r), r, i == welcomeStep ? ui::ColAccent() : ImGui::GetColorU32(ImGuiCol_FrameBg));
        ImGui::Dummy(ImVec2(w, r * 2 + fs * 0.6f));
    }
    float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (welcomeStep == 0) {
        Centered(T("Welcome to LunaShare"), 1.8f);
        Centered(T("Chat, call and send files between your devices and the people you know - directly, with nothing kept on a server."), 1.0f, true);
        ImGui::Dummy(ImVec2(0, fs * 0.8f));
        IconLine(Icon::Send, T("Files of any size"), T("Photos, movies, whole folders - on the same Wi-Fi at the network's full speed, and over the Internet."));
        IconLine(Icon::Users, T("Chats and calls"), T("Messages, voice and video calls, groups - encrypted from one device to the other."));
        IconLine(Icon::Library, T("Your things on all your devices"), T("Link your phone and computer: shared chats, clipboard, folders, music and movies."));
        ImGui::Dummy(ImVec2(0, fs * 0.5f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, fs * 0.3f));
        ImGui::TextUnformatted(T("What should people see you as?"));
        ImGui::SetNextItemWidth(mobile ? -1 : fs * 18);
        ImGui::InputTextWithHint("##wname", T("your name"), &welcomeName);
        DrawProfilePicture(mobile ? ui::M().rowH * 0.8f : 0);
        ImGui::PushTextWrapPos(0.0f);  // (longer in some languages)
        ImGui::TextDisabled("%s", T("(both can be changed later in Settings)"));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, fs * 0.6f));
        if (ImGui::Button(T("Skip"), ImVec2(half, h))) WelcomeDone();
        ImGui::SameLine();
        ui::PushAccent();
        bool next = ImGui::Button(T("Next"), ImVec2(half, h));
        ui::PopAccent();
        if (next) {
            std::string n = welcomeName;
            while (!n.empty() && isspace((unsigned char)n.back())) n.pop_back();
            while (!n.empty() && isspace((unsigned char)n.front())) n.erase(n.begin());
            if (!n.empty() && n != eng.settings.displayName) {
                Settings s = eng.GetSettings();
                s.displayName = n;
                eng.ApplySettings(s);
                edit.displayName = n;
            }
            welcomeStep = 1;
        }
    } else if (welcomeStep == 1) {
        Centered(T("Add your first person or device"), 1.5f);
        Centered(T("Another phone or computer of yours, or a friend's."), 1.0f, true);
        ImGui::Dummy(ImVec2(0, fs * 0.6f));
        if (DrawAddPersonBody(w)) {
            WelcomeDone();
            if (mobile) {
                backStack = {MScreen::Home};
                screen = MScreen::Chat;
            } else {
                tab = Tab::Chat;
                tabForce = true;
            }
        } else if (addGoConnect || addGoInvites) {  // (the technical pages: out of the welcome)
            bool conn = addGoConnect;
            addGoConnect = addGoInvites = addPersonClose = false;
            WelcomeDone();
            if (!conn) openInvites = true;
            else if (mobile) {
                backStack = {MScreen::Home};
                screen = MScreen::Nearby;
            } else {
                tab = Tab::Nearby;
                tabForce = true;
            }
        } else if (welcomeStep == 1) {
            addPersonClose = false;
            ImGui::Dummy(ImVec2(0, fs * 0.6f));
            if (ImGui::Button(T("Back"), ImVec2(half, h))) {
                if (Media().Scanning()) Media().CloseScanner();
                addScan = false;
                welcomeStep = 0;
            }
            ImGui::SameLine();
            if (ImGui::Button(SortedPeers().empty() ? T("Later") : T("Next"), ImVec2(half, h))) {
                if (Media().Scanning()) Media().CloseScanner();
                addScan = false;
                welcomeStep = 2;
            }
        }
    } else {
        Centered(T("You're set"), 1.5f);
        ImGui::Dummy(ImVec2(0, fs * 0.6f));
        IconLine(Icon::Attach, T("Sending"), mobile ? T("Open a chat and tap the paperclip - or share to LunaShare from any app.")
                                                     : T("Drop files on the window, or open a chat and press the paperclip."));
        IconLine(Icon::Link, T("Your other devices"), T("Settings > Profile & account links a phone and a computer into one: the same chats on both, and files go between them without asking."));
        {
            // someone connected already who isn't one of my devices: linking it is one tap away
            bool canLink = false;
            for (auto& p : SortedPeers()) {
                bool mine = false;
                for (auto& ad : eng.settings.accountDevices) mine |= ad.id == p;
                canLink |= !mine && eng.IsConnected(p);
            }
            if (canLink) {
                float iw = ImGui::GetFontSize() * 2.2f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + iw);
                if (ImGui::Button(T("Link another device..."), ImVec2(mobile ? ImGui::GetContentRegionAvail().x : 0, h))) {
                    WelcomeDone();
                    openLinkPick = true;
                    linkTarget.clear();
                }
            }
        }
        IconLine(Icon::Image, T("More than chat"), T("Gallery, movies and music, archives and comics have their own pages - also to watch or listen together."));
        IconLine(Icon::Gear, T("Private by default"), T("No account to sign up for. Settings > Privacy & security adds a PIN; people are added from the \"Add someone\" button."));
        ImGui::Dummy(ImVec2(0, fs * 0.6f));
        if (ImGui::Button(T("Back"), ImVec2(half, h))) welcomeStep = 1;
        ImGui::SameLine();
        ui::PushAccent();
        bool go = ImGui::Button(T("Start"), ImVec2(half, h));
        ui::PopAccent();
        if (go) WelcomeDone();
    }
    ImGui::EndChild();
}
