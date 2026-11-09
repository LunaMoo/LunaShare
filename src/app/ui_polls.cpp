// Polls: a question with answers; everyone in the chat answers and sees how the others did (and who).
#include <algorithm>

#include "emoji_host.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;

void App::DrawPollCard(const std::string& peer, const ChatMessage& m, float width) {
    bool mobile = ui::M().mobile;
    std::string mine = eng.MyVoterKey();
    std::vector<int> count(m.pollOpts.size(), 0);
    int people = 0;
    std::vector<int> my;
    for (auto& [who, v] : m.votes) {
        if (!v.opts.empty()) people++;
        for (int o : v.opts)
            if (o >= 0 && o < (int)count.size()) count[(size_t)o]++;
        if (who == mine) my = v.opts;
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextUnformatted(Emoji().Display(m.pollQ).c_str());
    ImGui::PopTextWrapPos();
    ImGui::TextDisabled("%s", m.pollMulti ? T("Poll - choose one or more") : T("Poll - choose one"));
    float rowH = mobile ? ui::M().rowH * 0.8f : ImGui::GetFrameHeight() * 1.15f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < m.pollOpts.size(); i++) {
        ImGui::PushID((int)i);
        bool on = std::find(my.begin(), my.end(), (int)i) != my.end();
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##opt", ImVec2(width, rowH)) && !selectionMode) {
            std::vector<int> v = m.pollMulti ? my : std::vector<int>();
            if (on) v.erase(std::remove(v.begin(), v.end(), (int)i), v.end());  // (tapped again: taken back)
            else v.push_back((int)i);
            eng.Vote(peer, m.mid, v);
        }
        bool hov = ImGui::IsItemHovered();
        ImVec2 q(p.x + width, p.y + rowH);
        dl->AddRectFilled(p, q, ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 6);
        if (people > 0 && count[i] > 0)  // the share of the people who answered
            dl->AddRectFilled(p, ImVec2(p.x + width * (float)count[i] / (float)people, q.y), (ui::ColAccent() & 0x00FFFFFF) | 0x50000000, 6);
        dl->AddRect(p, q, on ? ui::ColAccent() : ImGui::GetColorU32(ImGuiCol_Border), 6, 0, on ? 2.0f : 1.0f);
        float r = rowH * 0.2f;  // a round / square box, ticked when it's mine
        ImVec2 c(p.x + rowH * 0.45f, p.y + rowH * 0.5f);
        if (m.pollMulti) {
            dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImGui::GetColorU32(ImGuiCol_Text), 2, 0, 1.5f);
            if (on) ui::DrawIcon(dl, ui::Icon::Check, c, r * 2.2f, ui::ColAccent());
        } else {
            dl->AddCircle(c, r, ImGui::GetColorU32(ImGuiCol_Text), 0, 1.5f);
            if (on) dl->AddCircleFilled(c, r * 0.55f, ui::ColAccent());
        }
        std::string n = std::to_string(count[i]);
        ImVec2 ns = ImGui::CalcTextSize(n.c_str());
        float ty = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        ImVec4 clip(p.x, p.y, q.x - ns.x - 14, q.y);
        std::string label = Emoji().Display(m.pollOpts[i]);
        dl->AddText(nullptr, 0, ImVec2(p.x + rowH * 0.9f, ty), ImGui::GetColorU32(ImGuiCol_Text), label.c_str(), nullptr, 0, &clip);
        dl->AddText(ImVec2(q.x - ns.x - 8, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), n.c_str());
        if (hov && count[i] > 0 && !mobile) {  // who chose it
            std::string who;
            for (auto& [k, v] : m.votes)
                if (std::find(v.opts.begin(), v.opts.end(), (int)i) != v.opts.end())
                    who += (who.empty() ? "" : ", ") + (k == mine ? std::string(T("You")) : Emoji().Display(eng.PeerName(k)));
            ImGui::SetTooltip("%s", who.c_str());
        }
        ImGui::PopID();
    }
    ImGui::TextDisabled("%s", (people == 1 ? std::string(T("1 person answered")) : TF("%d people answered", people)).c_str());
    if (mobile && people > 0) {  // (phones: who chose what, behind a tap)
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Who?"))) {
            std::string s;
            for (size_t i = 0; i < m.pollOpts.size(); i++) {
                std::string who;
                for (auto& [k, v] : m.votes)
                    if (std::find(v.opts.begin(), v.opts.end(), (int)i) != v.opts.end())
                        who += (who.empty() ? "" : ", ") + (k == mine ? std::string(T("You")) : Emoji().Display(eng.PeerName(k)));
                s += Emoji().Display(m.pollOpts[i]) + ": " + (who.empty() ? "-" : who) + "\n";
            }
            infoMessage = s;
            openInfo = true;
        }
    }
}

void App::OpenPollNew(const std::string& peer) {
    pollPeer = peer;
    pollQ.clear();
    pollOpts = {"", ""};
    pollMulti = false;
    pollOpen = true;
}

void App::DrawPollNewPopup() {
    if (pollOpen) {
        ImGui::OpenPopup("##pollnew");
        pollOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * (mobile ? 0.35f : 0.5f)), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(mobile ? vp->WorkSize.x * 0.94f : ImGui::GetFontSize() * 24, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, vp->WorkSize.y * 0.9f));
    if (!ImGui::BeginPopupModal("##pollnew", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(T("New poll"));
    ImGui::Separator();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##q", T("Question"), &pollQ);
    int remove = -1;
    float xw = mobile ? ui::M().rowH * 0.7f : ImGui::GetFrameHeight();
    for (size_t i = 0; i < pollOpts.size(); i++) {
        ImGui::PushID((int)i);
        ImGui::SetNextItemWidth(pollOpts.size() > 2 ? ImGui::GetContentRegionAvail().x - xw - ImGui::GetStyle().ItemSpacing.x : -1);
        ImGui::InputTextWithHint("##o", TF("Answer %d", (int)i + 1).c_str(), &pollOpts[i]);
        if (pollOpts.size() > 2) {
            ImGui::SameLine();
            if (ui::IconButton("x", ui::Icon::Cancel, T("Remove"), xw)) remove = (int)i;
        }
        ImGui::PopID();
    }
    if (remove >= 0) pollOpts.erase(pollOpts.begin() + remove);
    if (pollOpts.size() < 12 && ui::IconTextButton(T("Another answer"), ui::Icon::Plus, ImVec2(mobile ? -1 : 0, h))) pollOpts.push_back("");
    ImGui::Checkbox(T("Several answers allowed"), &pollMulti);
    ImGui::Separator();
    std::vector<std::string> opts;
    for (auto& o : pollOpts)
        if (o.find_first_not_of(" \t") != std::string::npos) opts.push_back(Emoji().Restore(o));
    bool ok = pollQ.find_first_not_of(" \t") != std::string::npos && opts.size() >= 2;
    ImGui::BeginDisabled(!ok);
    if (ImGui::Button(T("Send the poll"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h))) {
        eng.SendPoll(pollPeer, Emoji().Restore(pollQ), opts, pollMulti);
        scrollToBottom = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
