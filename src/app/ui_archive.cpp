// LunaShare - a chat's history: archive it (compressed; files stay where they are, the messages keep links to them),
// clear it (keeping yesterday's and today's messages if wished), restore an archive, or ask the other side to send
// the history back when both the chat and its archive are gone.
#include <algorithm>

#include "emoji_host.h"
#include "i18n.h"
#include "imgui.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

void App::OpenChatHistory(const std::string& peer) {
    histPeer = peer;
    histOpen = true;
    histConfirmDelete.clear();
}

void App::DrawChatHistoryPopup() {
    if (histOpen) {
        ImGui::OpenPopup("##chathist");
        histOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * (mobile ? 0.04f : 0.5f)),
                            ImGuiCond_Appearing, ImVec2(0.5f, mobile ? 0.0f : 0.5f));
    float ww = mobile ? vp->WorkSize.x * 0.96f : ImGui::GetFontSize() * 34;
    ImGui::SetNextWindowSizeConstraints(ImVec2(ww, 0), ImVec2(ww, vp->WorkSize.y * 0.92f));
    bool open = true;
    if (!ImGui::BeginPopupModal("##chathist", &open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) return;
    std::string name = Emoji().Display(eng.PeerName(histPeer));
    ImGui::TextUnformatted(TF("Chat history - %s", name.c_str()).c_str());
    ImGui::Separator();
    auto it = eng.chats.find(histPeer);
    int count = it == eng.chats.end() ? 0 : (int)it->second.messages.size();

    // ---- archive (and clear)
    ImGui::SeparatorText(T("Archive"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", T("Saves this chat to a compressed archive on this device. Files aren't copied: the messages keep their links, so "
                                "files still on the disk open from a restored chat like before."));
    ImGui::PopTextWrapPos();
    ImGui::Checkbox(T("Then clear the chat"), &histClear);
    if (histClear) {
        ImGui::Indent();
        ImGui::Checkbox(T("Keep yesterday's and today's messages"), &histKeep);
        ImGui::Unindent();
    }
    ImGui::BeginDisabled(count == 0);
    if (ui::IconTextButton(histClear ? TF("Archive and clear (%d messages)", count).c_str() : TF("Archive (%d messages)", count).c_str(), Icon::Down,
                           ImVec2(mobile ? -1 : 0, h))) {
        std::string p = eng.ArchiveChat(histPeer, histClear, histKeep);
        toasts.push_back({p.empty() ? EventType::Error : EventType::Info, T("Chat history"),
                          p.empty() ? std::string(T("Could not save the archive.")) : TF("Archived to %s", BaseName(p).c_str()), "", 0, 3.0f});
    }
    ImGui::EndDisabled();

    // ---- the archives
    auto list = eng.ChatArchives(histAll ? "" : histPeer);
    ImGui::SeparatorText(TF("Archives (%d)", (int)list.size()).c_str());
    ImGui::Checkbox(T("Show the archives of all chats"), &histAll);
    float listH = std::min((mobile ? ui::M().rowH * 2.2f : ImGui::GetFrameHeight() * 2.6f) * (float)std::max<size_t>(1, list.size()),
                           vp->WorkSize.y * (mobile ? 0.36f : 0.32f));
    ImGui::BeginChild("##archlist", ImVec2(0, listH), ImGuiChildFlags_Borders);
    if (mobile) ui::TouchScroll();
    if (list.empty()) ImGui::TextDisabled("%s", T("No archives yet."));
    for (auto& a : list) {
        ImGui::PushID(a.path.c_str());
        std::string head = FormatTime(a.created) + "  -  " + TF("%d messages", a.count);
        if (histAll) head = Emoji().Display(a.name.empty() ? eng.PeerName(a.peer) : a.name) + "  -  " + head;
        ImGui::TextUnformatted(head.c_str());
        std::string span = FormatTime(a.from) + " - " + FormatTime(a.to) + "  (" + FormatBytes((double)a.bytes) + ")";
        ImGui::TextDisabled("%s", span.c_str());
        if (ImGui::Button(T("Restore"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h))) {
            int n = eng.RestoreChatArchive(a.path);
            toasts.push_back({n < 0 ? EventType::Error : EventType::Info, T("Chat history"),
                              n < 0 ? std::string(T("Could not read the archive.")) : n == 0 ? std::string(T("Nothing to restore - the chat has all of it."))
                                                                                     : TF("Restored %d messages.", n),
                              "", 0, 3.0f});
        }
        ImGui::SameLine();
        if (histConfirmDelete == a.path) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(170, 50, 50, 255));
            if (ImGui::Button(T("Delete for good"), ImVec2(mobile ? -1 : 0, h))) {
                eng.DeleteChatArchive(a.path);
                histConfirmDelete.clear();
            }
            ImGui::PopStyleColor();
        } else if (ImGui::Button(T("Delete..."), ImVec2(mobile ? -1 : 0, h))) {
            histConfirmDelete = a.path;
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- lost it all?
    ImGui::SeparatorText(T("Lost the history?"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", T("Ask the other side (and your other devices) to send this chat's history back. They're asked first; "
                                "what comes back joins the chat."));
    ImGui::PopTextWrapPos();
    if (ui::IconTextButton(TF("Ask %s to send it back", name.c_str()).c_str(), Icon::Down, ImVec2(mobile ? -1 : 0, h))) {
        eng.AskHistory(histPeer);
        toasts.push_back({EventType::Info, T("Chat history"), T("Asked - the history comes once they agree."), "", 0, 3.0f});
    }
    ImGui::Spacing();
    if (ImGui::Button(T("Close"), ImVec2(mobile ? -1 : ImGui::GetFontSize() * 8, h)) || !open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// someone asks me for our history: send it or not
void App::DrawHistoryAskBanner(const std::string& peer) {
    const Engine::HistoryAsk* ask = nullptr;
    for (auto& a : eng.historyAsks)
        if (a.key == peer) ask = &a;
    if (!ask) return;
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(50, 60, 110, 255));
    ImGui::BeginChild("histask", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(TF("%s asks for the history of this chat (%d messages) - they lost theirs.", Emoji().Display(ask->name).c_str(), ask->count).c_str());
    ImGui::PopTextWrapPos();
    std::string dev = ask->dev, key = ask->key;
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 140, 85, 255));
    bool send = ImGui::Button(T("Send it"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    bool no = ImGui::Button(T("Don't send"), ImVec2(mobile ? -1 : 0, h));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (send || no) eng.AnswerHistoryAsk(dev, key, send);
}
