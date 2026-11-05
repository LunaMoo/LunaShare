// Groups: "New group" (a name and the people), the people in a group (add, remove, rename, leave).
#include <algorithm>

#include "i18n.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

void App::OpenNewGroup() {
    groupDlgAdd = false;
    groupNameBuf.clear();
    groupPick.clear();
    openGroupDlg = true;
}

void App::OpenAddToGroup(const std::string& key) {
    groupDlgAdd = true;
    groupDlgKey = key;
    groupPick.clear();
    openGroupDlg = true;
}

void App::OpenGroupMembers(const std::string& key) {
    groupDlgKey = key;
    const Group* g = eng.GroupOf(key);
    groupNameBuf = g ? g->name : "";
    openGroupMembers = true;
}

// "people" button in a group's chat header
void App::GroupButtons(const std::string& key, float size) {
    if (ui::IconButton("gmembers", Icon::Users, T("People in this group"), size)) OpenGroupMembers(key);
}

void App::DrawGroupPopups() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.85f : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    auto centerNext = [&]() {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.94f, 0));
        else ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24, 0), ImGuiCond_Appearing);
    };
    float listH = std::min(vp->WorkSize.y * 0.45f, ImGui::GetFontSize() * 18);

    // ---- new group / add people: a checklist of the people I know
    if (openGroupDlg) {
        ImGui::OpenPopup("##groupdlg");
        openGroupDlg = false;
    }
    centerNext();
    if (ImGui::BeginPopupModal("##groupdlg", nullptr, ImGuiWindowFlags_NoTitleBar)) {
        const Group* g = groupDlgAdd ? eng.GroupOf(groupDlgKey) : nullptr;
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
        ImGui::TextUnformatted(groupDlgAdd ? T("Add people") : T("New group"));
        ImGui::PopFont();
        if (!groupDlgAdd) {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##gname", T("Group name"), &groupNameBuf);
        }
        std::vector<std::string> people;
        for (auto& k : SortedPeers())
            if (Engine::IsContactKey(k) && !(g && g->Has(k.substr(1)))) people.push_back(k);
        ImGui::BeginChild("gpeople", ImVec2(0, listH), ImGuiChildFlags_Borders);
        if (mobile) ui::TouchScroll();
        if (people.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", groupDlgAdd ? T("Everyone you know is already in this group.")
                                                  : T("No people yet: connect to someone first (their devices need LunaShare 1.1 or newer)."));
            ImGui::PopTextWrapPos();
        }
        float rowH = mobile ? ui::M().rowH : ImGui::GetFrameHeight() * 1.4f;
        for (auto& p : people) {
            ImGui::PushID(p.c_str());
            bool on = groupPick.count(p) > 0;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##p", on, ImGuiSelectableFlags_DontClosePopups, ImVec2(0, rowH))) on ? (void)groupPick.erase(p) : (void)groupPick.insert(p);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float av = rowH * 0.75f;
            DrawAvatar(dl, ImVec2(p0.x + av * 0.6f, p0.y + rowH * 0.5f), av, p, eng.PeerName(p));
            dl->AddText(ImVec2(p0.x + av * 1.4f, p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), eng.PeerName(p).c_str());
            float bx = ImGui::GetItemRectMax().x - rowH * 0.7f, by = p0.y + rowH * 0.5f, r = rowH * 0.22f;
            dl->AddRect(ImVec2(bx - r, by - r), ImVec2(bx + r, by + r), ImGui::GetColorU32(ImGuiCol_Text), 3, 0, 1.5f);
            if (on) ui::DrawIcon(dl, Icon::Check, ImVec2(bx, by), r * 1.8f, ui::ColOk());
            ImGui::PopID();
        }
        ImGui::EndChild();
        bool ok = !groupPick.empty() && (groupDlgAdd || !groupNameBuf.empty());
        ImGui::BeginDisabled(!ok);
        const char* go = groupDlgAdd ? T("Add") : T("Create");
        if (ImGui::Button(go, ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h))) {
            std::vector<std::string> v(groupPick.begin(), groupPick.end());
            if (groupDlgAdd) eng.AddToGroup(groupDlgKey, v);
            else {
                std::string key = eng.CreateGroup(groupNameBuf, v);
                if (!key.empty()) {
                    SelectPeer(key);
                    if (mobile) {
                        backStack.clear();
                        backStack.push_back(MScreen::Home);
                        screen = MScreen::Chat;
                    } else {
                        tab = Tab::Chat;
                        tabForce = true;
                    }
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- the people in a group
    if (openGroupMembers) {
        ImGui::OpenPopup("##groupmembers");
        openGroupMembers = false;
    }
    centerNext();
    if (ImGui::BeginPopupModal("##groupmembers", nullptr, ImGuiWindowFlags_NoTitleBar)) {
        const Group* g = eng.GroupOf(groupDlgKey);
        if (!g) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        bool creator = g->creator == eng.MyAccount();
        if (!g->left) {
            ImGui::SetNextItemWidth(mobile ? -1 : -ImGui::CalcTextSize(T("Rename")).x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
            bool enter = ImGui::InputTextWithHint("##gn", T("Group name"), &groupNameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
            if (!mobile) ImGui::SameLine();
            ImGui::BeginDisabled(groupNameBuf.empty() || groupNameBuf == g->name);
            if (ImGui::Button(T("Rename"), ImVec2(mobile ? -1 : 0, h)) || (enter && !groupNameBuf.empty())) eng.RenameGroup(groupDlgKey, groupNameBuf);
            ImGui::EndDisabled();
        } else {
            ImGui::TextUnformatted(g->name.c_str());
            ImGui::TextDisabled("%s", T("You are no longer in this group."));
        }
        ImGui::SeparatorText(TF("%d people", (int)g->members.size()).c_str());
        ImGui::BeginChild("gmembers", ImVec2(0, listH), ImGuiChildFlags_Borders);
        if (mobile) ui::TouchScroll();
        float rowH = mobile ? ui::M().rowH : ImGui::GetFrameHeight() * 1.4f;
        std::string removeAcc;
        for (auto& m : g->members) {
            ImGui::PushID(m.acc.c_str());
            bool me = m.acc == eng.MyAccount();
            std::string key = "@" + m.acc;
            std::string name = me ? T("You") : eng.DevicesOf(key).empty() && !m.name.empty() ? m.name : eng.PeerName(key);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, rowH));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float av = rowH * 0.75f;
            DrawAvatar(dl, ImVec2(p0.x + av * 0.6f, p0.y + rowH * 0.5f), av, me ? "" : key, name);
            if (!me) {
                ImU32 dot = eng.IsConnected(key) ? ui::ColOk() : eng.IsOnline(key) ? ui::ColWarn() : ui::ColMuted();
                dl->AddCircleFilled(ImVec2(p0.x + av * 0.95f, p0.y + rowH * 0.5f + av * 0.35f), av * 0.13f, dot);
            }
            std::string label = name + (m.acc == g->creator ? std::string("  (") + T("created the group") + ")" : std::string());
            dl->AddText(ImVec2(p0.x + av * 1.4f, p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
            if (creator && !me && !g->left) {
                float bw = ImGui::CalcTextSize(T("Remove")).x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMax().x - bw, p0.y + (rowH - (h > 0 ? h : ImGui::GetFrameHeight())) * 0.5f));
                if (ImGui::Button(T("Remove"), ImVec2(bw, h))) removeAcc = m.acc;
                ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rowH + ImGui::GetStyle().ItemSpacing.y));
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (!removeAcc.empty()) eng.RemoveFromGroup(groupDlgKey, removeAcc);
        float bw = mobile ? -1 : 0;
        if (!g->left) {
            if (ui::IconTextButton(T("Add people"), Icon::Plus, ImVec2(bw, h))) {
                ImGui::CloseCurrentPopup();
                OpenAddToGroup(groupDlgKey);
            }
            if (!mobile) ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 50, 50, 255));
            if (ImGui::Button(T("Leave the group"), ImVec2(bw, h))) {
                eng.LeaveGroup(groupDlgKey);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            if (!mobile) ImGui::SameLine();
        }
        if (ImGui::Button(T("Close"), ImVec2(bw, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
