// App lock: a PIN (and on phones the fingerprint / face) before LunaShare shows anything - when it starts, and when it
// comes back after a while in the background. Messages keep arriving meanwhile; their notifications don't show the
// text. It keeps out someone holding the unlocked device. Optionally the chat history is encrypted with the PIN too
// (Engine::SetHistoryEncryption): after a start the PIN (not the fingerprint) opens it.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "bluetooth.h"
#include "emoji_host.h"
#include "hash.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "media_host.h"
#include "notify.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;

static std::string HashPin(const std::string& pin, const std::string& salt) {
    std::string h = salt;
    for (int i = 0; i < 20000; i++) {  // (slow on purpose: guessing takes longer)
        std::string in = salt + h + pin;
        h = Sha256Hex(in.data(), in.size());
    }
    return h;
}

bool LockOn(const Settings& s) { return !s.lockHash.empty(); }

static bool BioAvailable() { return Bridge().biometricAvailable && Bridge().biometricAvailable(); }

void App::LockNow() {
    if (!LockOn(eng.settings)) return;
    locked = true;
    lockEntry.clear();
    lockBioAsked = false;
    lockForgot = false;
}

void App::LockTick() {
    bool on = LockOn(eng.settings);
    if (!lockInit) {
        lockInit = true;
        if (on) LockNow();
        if (Bridge().secureWindow) Bridge().secureWindow(on);
        lockSecure = on;
    }
    if (on != lockSecure) {  // (turned on / off in the settings)
        lockSecure = on;
        if (Bridge().secureWindow) Bridge().secureWindow(on);
    }
    bool bg = notify::Background();
    if (bg && !lockWasBg) lockBgSince = SteadyMs();
    if (!bg && lockWasBg && on && !locked && SteadyMs() - lockBgSince >= (int64_t)eng.settings.lockAfter * 60000) LockNow();
    lockWasBg = bg;
    if (!on) locked = false;
}

void App::UnlockWith(const std::string& pin) {
    if (SteadyMs() < lockWaitUntil) return;
    if (HashPin(pin, eng.settings.lockSalt) == eng.settings.lockHash) {
        locked = false;
        lockWrong = 0;
        lockEntry.clear();
        if (eng.HistoryLocked() && !eng.UnlockHistory(pin)) {
            infoMessage = T("The encrypted chat history couldn't be opened (the file is damaged?).");
            openInfo = true;
        }
        return;
    }
    lockWrong++;
    lockEntry.clear();
    lockShake = 0.4f;
    if (lockWrong >= 5) lockWaitUntil = SteadyMs() + 30000LL * (1LL << std::min(lockWrong - 5, 6));  // 30 s, 1 min, 2 min...
}

void App::DrawLockScreen() {
    bool mobile = ui::M().mobile;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float W = mobile ? std::min(avail.x * 0.86f, ImGui::GetFontSize() * 22) : ImGui::GetFontSize() * 16;
    float keyH = mobile ? std::min(ui::M().rowH * 1.15f, avail.y * 0.11f) : ImGui::GetFrameHeight() * 1.8f;
    float x0 = ImGui::GetCursorPosX() + (avail.x - W) * 0.5f;
    float total = ImGui::GetTextLineHeightWithSpacing() * 5 + keyH * 4 + ImGui::GetStyle().ItemSpacing.y * 8;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - total) * 0.4f));
    auto centered = [&](const char* s, bool dim) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - ImGui::CalcTextSize(s).x) * 0.5f);
        dim ? ImGui::TextDisabled("%s", s) : ImGui::TextUnformatted(s);
    };
    centered("LunaShare", false);
    int64_t wait = lockWaitUntil - SteadyMs();
    // a call can still be answered
    if (eng.call.state == CallState::IncomingRinging) {
        std::string who = TF("%s is calling you", Emoji().Display(eng.PeerName(eng.call.peer)).c_str());
        centered(who.c_str(), false);
        ImGui::SetCursorPosX(x0);
        if (ImGui::Button(T("Answer"), ImVec2(W * 0.5f - 4, keyH))) {
            Media().WantCameraAtStart(false);
            eng.AcceptCall();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Decline"), ImVec2(W * 0.5f - 4, keyH))) eng.EndCall("declined");
    } else if (eng.call.state == CallState::Active) {
        ImGui::SetCursorPosX(x0);
        if (ImGui::Button(T("Hang up"), ImVec2(W, keyH * 0.8f))) eng.EndCall();
    }
    if (lockForgot) {  // the account's recovery phrase opens it too (and turns the lock off)
        ImGui::SetCursorPosX(x0);
        ImGui::PushTextWrapPos(x0 + W);
        ImGui::TextDisabled("%s", T("Forgot the PIN? Type your account's recovery phrase (the 24 words) - the lock turns off."));
        if (eng.HistoryLocked())
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.4f, 1), "%s",
                               T("The chat history is encrypted with the PIN: without it, the messages from before are lost."));
        ImGui::PopTextWrapPos();
        ImGui::SetCursorPosX(x0);
        ImGui::SetNextItemWidth(W);
        ImGui::InputTextMultiline("##phrase", &lockPhraseBuf, ImVec2(W, ImGui::GetTextLineHeight() * 4));
        ImGui::SetCursorPosX(x0);
        if (ImGui::Button(T("Unlock"), ImVec2(W * 0.5f - 4, keyH * 0.8f))) {
            auto norm = [](const std::string& s) {
                std::string r;
                bool sp = true;
                for (char c : s) {
                    if (std::isspace((unsigned char)c)) {
                        if (!sp) r += ' ';
                        sp = true;
                    } else {
                        r += (char)std::tolower((unsigned char)c);
                        sp = false;
                    }
                }
                while (!r.empty() && r.back() == ' ') r.pop_back();
                return r;
            };
            std::string mine = eng.RecoveryPhrase();
            if (!mine.empty() && norm(lockPhraseBuf) == norm(mine)) {
                eng.SetLock("", "", 0);
                locked = false;
                lockWrong = 0;
                infoMessage = T("The app lock is off. You can set a new PIN in Settings.");
                openInfo = true;
            } else {
                lockPhraseBuf.clear();
                lockShake = 0.4f;
            }
            lockPhraseBuf.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Back"), ImVec2(W * 0.5f - 4, keyH * 0.8f))) lockForgot = false;
        return;
    }
    if (wait > 0) {
        std::string w = TF("Too many tries - wait %d s", (int)((wait + 999) / 1000));
        centered(w.c_str(), true);
    } else {
        centered(lockWrong ? T("Wrong PIN - try again") : T("Enter your PIN"), lockWrong == 0);
    }
    if (eng.HistoryLocked() && eng.settings.lockBio && BioAvailable()) centered(T("(after a start: the PIN - it opens the chats)"), true);
    // the dots
    {
        float shake = lockShake > 0 ? std::sin(lockShake * 60) * lockShake * 20 : 0;
        lockShake = std::max(0.0f, lockShake - io.DeltaTime);
        int n = std::max(eng.settings.lockLen, (int)lockEntry.size());
        float r = ImGui::GetFontSize() * 0.32f, gap = r * 3.2f;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float cx = ImGui::GetWindowPos().x + ImGui::GetCursorPosX() + avail.x * 0.5f - (n - 1) * gap * 0.5f + shake;
        float cy = p.y + r * 2;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < n; i++) {
            ImVec2 c(cx + i * gap, cy);
            if (i < (int)lockEntry.size()) dl->AddCircleFilled(c, r, ui::ColAccent());
            else dl->AddCircle(c, r, ImGui::GetColorU32(ImGuiCol_TextDisabled), 0, 1.5f);
        }
        ImGui::Dummy(ImVec2(1, r * 4.5f));
    }
    // the keypad (and the keyboard)
    bool can = wait <= 0;
    auto press = [&](char c) {
        if (!can) return;
        if (c == '<') {
            if (!lockEntry.empty()) lockEntry.pop_back();
            return;
        }
        if (lockEntry.size() < 12) lockEntry += c;
        if ((int)lockEntry.size() == eng.settings.lockLen) UnlockWith(lockEntry);
    };
    if (!ImGui::IsAnyItemActive()) {
        for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
            ImWchar c = io.InputQueueCharacters[i];
            if (c >= '0' && c <= '9') press((char)c);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) press('<');
        if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && !lockEntry.empty()) UnlockWith(lockEntry);
    }
    const char* keys = "123456789B0<";
    float kw = (W - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
    bool bio = eng.settings.lockBio && BioAvailable() && !eng.HistoryLocked();  // (the encrypted history needs the PIN itself)
    ImGui::BeginDisabled(!can);
    for (int i = 0; i < 12; i++) {
        if (i % 3 == 0) ImGui::SetCursorPosX(x0);
        else ImGui::SameLine();
        char k = keys[i];
        ImGui::PushID(i);
        if (k == 'B') {
            if (bio) {
                if (ImGui::Button(T("Finger / face"), ImVec2(kw, keyH)) && Bridge().biometricAsk) Bridge().biometricAsk(T("Unlock LunaShare"), T("Use the PIN"));
            } else {
                ImGui::Dummy(ImVec2(kw, keyH));
            }
        } else if (k == '<') {
            if (ImGui::Button("##bk", ImVec2(kw, keyH))) press('<');
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax(), c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            float s = std::min(kw, keyH) * 0.16f;
            ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            ImDrawList* dl = ImGui::GetWindowDrawList();  // a "delete" key: a left arrow
            dl->AddLine(ImVec2(c.x - s * 1.2f, c.y), ImVec2(c.x + s * 1.2f, c.y), col, 2.5f);
            dl->AddLine(ImVec2(c.x - s * 1.2f, c.y), ImVec2(c.x - s * 0.2f, c.y - s), col, 2.5f);
            dl->AddLine(ImVec2(c.x - s * 1.2f, c.y), ImVec2(c.x - s * 0.2f, c.y + s), col, 2.5f);
        } else {
            char lbl[2] = {k, 0};
            ImGui::SetWindowFontScale(1.3f);
            if (ImGui::Button(lbl, ImVec2(kw, keyH))) press(k);
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::SetCursorPosX(x0);
    if (ImGui::Button(T("Forgot the PIN?"), ImVec2(W, 0))) {
        lockForgot = true;
        lockPhraseBuf.clear();
    }
    if (bio && !lockBioAsked && Bridge().biometricAsk) {  // (asked right away once; the button asks again)
        lockBioAsked = true;
        Bridge().biometricAsk(T("Unlock LunaShare"), T("Use the PIN"));
    }
}

// ---- Settings > App lock
void App::DrawLockSettings(float h) {
    bool mobile = ui::M().mobile;
    bool on = LockOn(eng.settings);
    if (!on) {
        if (ImGui::Button(T("Set a PIN..."), ImVec2(mobile ? -1 : 0, h))) {
            lockSetOpen = true;
            lockNew1.clear();
            lockNew2.clear();
            lockSetErr.clear();
        }
        return;
    }
    Settings s = eng.settings;
    bool ch = false;
    const int afters[] = {0, 1, 5, 15, 60};
    std::string cur = s.lockAfter == 0 ? std::string(T("right away")) : TF("after %d min", s.lockAfter);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Ask again when I come back"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (ImGui::BeginCombo("##lockafter", cur.c_str())) {
        for (int a : afters) {
            std::string l = a == 0 ? std::string(T("right away")) : TF("after %d min", a);
            if (ImGui::Selectable(l.c_str(), s.lockAfter == a, 0, ImVec2(0, h))) {
                s.lockAfter = a;
                ch = true;
            }
        }
        ImGui::EndCombo();
    }
    if (BioAvailable()) ch |= ui::Checkbox(T("Unlock with the fingerprint / face too"), &s.lockBio);
    if (ch) {
        eng.ApplySettings(s);
        edit.lockAfter = s.lockAfter;
        edit.lockBio = s.lockBio;
    }
    bool enc = eng.HistoryEncrypted();
    if (ui::Checkbox(T("Encrypt the chat history with the PIN"), &enc)) {
        if (enc) {
            lockEncOpen = true;
            lockEncPin.clear();
            lockEncErr.clear();
        } else {
            eng.SetHistoryEncryption("");
        }
    }
    ImGui::SameLine();
    ui::HelpMarker(T("The messages are saved encrypted (Argon2 + XChaCha20): someone who copies the files can't read them without "
                     "the PIN. After a start the PIN opens them (not the fingerprint); what arrives before is kept and added. "
                     "Drafts then aren't saved. A forgotten PIN means the history is lost. A short PIN can be guessed by a "
                     "patient computer - a longer one is safer."));
    if (ImGui::Button(T("Lock now"), ImVec2(0, h))) LockNow();
    ImGui::SameLine();
    if (ImGui::Button(T("Change the PIN..."), ImVec2(0, h))) {
        lockSetOpen = true;
        lockNew1.clear();
        lockNew2.clear();
        lockSetErr.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Turn the lock off"), ImVec2(0, h))) eng.SetLock("", "", 0);
}

void App::DrawLockEncPopup() {
    if (lockEncOpen) {
        ImGui::OpenPopup("##lockenc");
        lockEncOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * (mobile ? 0.3f : 0.5f)), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.9f, 0));
    if (!ImGui::BeginPopupModal("##lockenc", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(T("Encrypt the chat history with the PIN"));
    ImGui::TextDisabled("%s", T("Your PIN, to confirm:"));
    auto digits = [](ImGuiInputTextCallbackData* d) { return (d->EventChar >= '0' && d->EventChar <= '9') ? 0 : 1; };
    ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 12);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool go = ImGui::InputTextWithHint("##encpin", T("PIN"), &lockEncPin,
                                       ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue, digits);
    if (!lockEncErr.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", lockEncErr.c_str());
    go |= ImGui::Button(T("Encrypt"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h));
    if (go) {
        if (HashPin(lockEncPin, eng.settings.lockSalt) != eng.settings.lockHash) {
            lockEncErr = T("Wrong PIN - try again");
            lockEncPin.clear();
        } else {
            eng.SetHistoryEncryption(lockEncPin);
            lockEncPin.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h))) {
        lockEncPin.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::DrawLockSetPopup() {
    if (lockSetOpen) {
        ImGui::OpenPopup("##lockset");
        lockSetOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * (mobile ? 0.3f : 0.5f)), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.9f, 0));
    if (!ImGui::BeginPopupModal("##lockset", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(T("App lock: a new PIN"));
    ImGui::TextDisabled("%s", T("4 - 12 digits."));
    float w = mobile ? -1 : ImGui::GetFontSize() * 12;
    auto digits = [](ImGuiInputTextCallbackData* d) { return (d->EventChar >= '0' && d->EventChar <= '9') ? 0 : 1; };
    ImGui::SetNextItemWidth(w);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##pin1", T("PIN"), &lockNew1, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CallbackCharFilter, digits);
    ImGui::SetNextItemWidth(w);
    ImGui::InputTextWithHint("##pin2", T("The same PIN again"), &lockNew2, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CallbackCharFilter, digits);
    if (!lockSetErr.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", lockSetErr.c_str());
    if (ImGui::Button(T("Save"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h))) {
        if (lockNew1.size() < 4 || lockNew1.size() > 12) {
            lockSetErr = T("4 - 12 digits.");
        } else if (lockNew1 != lockNew2) {
            lockSetErr = T("The two PINs differ.");
        } else {
            std::string salt = RandomHex(16);
            eng.SetLock(HashPin(lockNew1, salt), salt, (int)lockNew1.size(), lockNew1);
            lockNew1.clear();
            lockNew2.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h))) {
        lockNew1.clear();
        lockNew2.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
