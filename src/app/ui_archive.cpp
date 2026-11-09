// LunaShare - a chat's history: archive it (compressed; files stay where they are, the messages keep links to them),
// clear it (keeping yesterday's and today's messages if wished), restore an archive, or ask the other side to send
// the history back when both the chat and its archive are gone.
#include <algorithm>

#include "emoji_host.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_stdlib.h"
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

void DrawPin(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    // a round head, the pin under it going down-left
    ImVec2 head(c.x + s * 0.15f, c.y - s * 0.15f);
    dl->AddLine(ImVec2(head.x - s * 0.1f, head.y + s * 0.1f), ImVec2(c.x - s * 0.4f, c.y + s * 0.4f), col, std::max(1.0f, s * 0.12f));
    dl->AddCircleFilled(head, s * 0.28f, col);
}

static std::string OneLine(std::string s, size_t max) {
    for (auto& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    if (s.size() > max) {
        s.resize(max);
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
        if (!s.empty() && ((unsigned char)s.back() & 0x80)) s.pop_back();
        s += "...";
    }
    return s;
}

void App::DrawPinnedBar(const std::string& peer) {
    auto it = eng.chats.find(peer);
    if (it == eng.chats.end()) return;
    std::vector<const ChatMessage*> pins;
    for (auto& m : it->second.messages)
        if (m.pinned) pins.push_back(&m);
    if (pins.empty()) return;
    std::sort(pins.begin(), pins.end(), [](auto* a, auto* b) { return a->pinTs > b->pinTs; });  // (the newest first)
    if (pinIdx >= (int)pins.size()) pinIdx = 0;
    const ChatMessage& m = *pins[(size_t)pinIdx];
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.75f : ImGui::GetFrameHeight();
    float xw = h;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x - xw - ImGui::GetStyle().ItemSpacing.x;
    std::string what = m.kind == MsgKind::Files ? std::string(T("files")) : Emoji().Display(m.text);
    std::string line = OneLine(what, 120) + (pins.size() > 1 ? "   (" + std::to_string(pinIdx + 1) + "/" + std::to_string(pins.size()) + ")" : "");
    if (ImGui::InvisibleButton("##pinbar", ImVec2(w, h))) {
        searchJump = m.mid;  // there, and the next one next time
        pinIdx = (pinIdx + 1) % (int)pins.size();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 4);
    DrawPin(dl, ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), ImGui::GetFontSize() * 0.8f, ui::ColAccent());
    ImVec4 clip(p.x, p.y, p.x + w - 4, p.y + h);
    dl->AddText(nullptr, 0, ImVec2(p.x + h, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), line.c_str(), nullptr, 0, &clip);
    ImGui::SameLine();
    if (ui::IconButton("unpin", ui::Icon::Cancel, T("Unpin"), xw)) eng.PinMessage(peer, m.mid, false);
}

void App::DrawStarredPopup() {
    if (starredOpen) {
        ImGui::OpenPopup("##starred");
        starredOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    float ww = mobile ? vp->WorkSize.x * 0.96f : ImGui::GetFontSize() * 32;
    ImGui::SetNextWindowSize(ImVec2(ww, std::min(vp->WorkSize.y * 0.85f, ImGui::GetFontSize() * 30)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##starred", nullptr, ImGuiWindowFlags_NoTitleBar)) return;
    ImGui::TextUnformatted(T("Starred messages"));
    ImGui::Separator();
    struct Item {
        std::string key;
        const ChatMessage* m;
    };
    std::vector<Item> items;
    for (auto& [key, cv] : eng.chats)
        for (auto& m : cv.messages)
            if (m.starred) items.push_back({key, &m});
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.m->ts > b.m->ts; });
    ImGui::BeginChild("##stl", ImVec2(0, -(mobile ? ui::M().rowH : ImGui::GetFrameHeightWithSpacing() * 1.2f)), ImGuiChildFlags_Borders);
    if (mobile) ui::TouchScroll();
    if (items.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("Nothing starred yet - Star in a message's menu keeps it here."));
        ImGui::PopTextWrapPos();
    }
    for (size_t i = 0; i < items.size(); i++) {
        auto& it = items[i];
        ImGui::PushID((int)i);
        std::string who = it.m->outgoing ? T("You") : Emoji().Display(eng.PeerName(it.m->from.empty() ? it.key : it.m->from));
        std::string head = Emoji().Display(eng.PeerName(it.key)) + "  -  " + who + "  -  " + FormatTime(it.m->ts);
        std::string body = it.m->kind == MsgKind::Files ? std::string(T("files")) : OneLine(Emoji().Display(it.m->text), 200);
        float rh = ImGui::GetTextLineHeightWithSpacing() * 2 + (mobile ? 8 : 2);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##st", false, 0, ImVec2(0, rh))) {  // to that chat, at the message
            SelectPeer(it.key);
            if (mobile) {
                backStack = {MScreen::Home};
                screen = MScreen::Chat;
            } else {
                tab = Tab::Chat;
                tabForce = true;
            }
            searchJump = it.m->mid;
            ImGui::CloseCurrentPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec4 clip(p.x, p.y, p.x + ImGui::GetContentRegionAvail().x, p.y + rh);
        dl->AddText(nullptr, 0, ImVec2(p.x + 4, p.y + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), head.c_str(), nullptr, 0, &clip);
        dl->AddText(nullptr, 0, ImVec2(p.x + 4, p.y + 1 + ImGui::GetTextLineHeightWithSpacing()), ImGui::GetColorU32(ImGuiCol_Text), body.c_str(), nullptr, 0, &clip);
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (ImGui::Button(T("Close"), ImVec2(mobile ? -1 : 0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

bool App::MuteItem(const std::string& key, float h, bool button) {
    auto item = [&](const std::string& l) {
        return button ? ui::IconTextButton(l.c_str(), Icon::Speaker, ImVec2(ui::M().mobile ? -1 : 0, h)) : ImGui::Selectable(l.c_str(), false, 0, ImVec2(0, h));
    };
    int64_t until = eng.ChatMutedUntil(key);
    if (until >= 0) {
        std::string l = until == 0 ? std::string(T("Turn notifications back on")) : TF("Turn notifications back on (silent until %s)", FormatTime(until, until - NowMs() > 20 * 3600000LL).c_str());
        if (!item(l)) return false;
        eng.MuteChat(key, -1);
        return true;
    }
    if (!item(T("Silence notifications..."))) return false;
    muteKey = key;
    muteOpen = true;
    return true;
}

void App::DrawMutePopup() {
    if (muteOpen) {
        ImGui::OpenPopup("##mute");
        muteOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.8f, 0));
    bool open = true;
    if (!ImGui::BeginPopupModal("##mute", &open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(TF("Silence %s", Emoji().Display(eng.PeerName(muteKey)).c_str()).c_str());
    ImGui::PushTextWrapPos(mobile ? 0 : ImGui::GetFontSize() * 22);
    ImGui::TextDisabled("%s", T("New messages don't make a sound or show a notification - they're only counted (in grey). Calls still ring."));
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    struct { const char* label; int min; } opts[] = {{T("For 1 hour"), 60}, {T("For 8 hours"), 480}, {T("For a day"), 1440}, {T("For a week"), 10080},
                                          {T("Until I turn them back on"), 0}};
    for (auto& o : opts)
        if (ImGui::Selectable(o.label, false, 0, ImVec2(0, h))) {
            eng.MuteChat(muteKey, o.min);
            ImGui::CloseCurrentPopup();
        }
    ImGui::Separator();
    if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
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
    if (ui::IconTextButton(T("Search messages"), Icon::ZoomIn, ImVec2(mobile ? -1 : 0, h))) {
        OpenChatSearch();
        ImGui::CloseCurrentPopup();
    }
    if (MuteItem(histPeer, h, true)) ImGui::CloseCurrentPopup();
    if (ui::IconTextButton(T("Starred messages"), ui::Icon::Star, ImVec2(mobile ? -1 : 0, h))) {
        starredOpen = true;
        ImGui::CloseCurrentPopup();
    }
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

// ---------------------------------------------------------------- searching messages

static std::string Fold(const std::string& s) {  // (lower case: ASCII, and Polish letters)
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            o += (char)tolower(c);
            continue;
        }
        if (i + 1 < s.size()) {
            unsigned char d = (unsigned char)s[i + 1];
            static const unsigned char up[][2] = {{0xC4, 0x84}, {0xC4, 0x86}, {0xC4, 0x98}, {0xC5, 0x81}, {0xC5, 0x83},
                                                  {0xC3, 0x93}, {0xC5, 0x9A}, {0xC5, 0xB9}, {0xC5, 0xBB}};
            bool low = false;
            for (auto& u : up)
                if (c == u[0] && d == u[1]) {
                    o += (char)c;
                    o += (char)(d + (c == 0xC3 ? 0x20 : 1));  // Ó -> ó, Ą -> ą ...
                    low = true;
                }
            if (low) {
                i++;
                continue;
            }
        }
        o += (char)c;
    }
    return o;
}

void App::DrawChatSearch(const std::string& peer) {
    if (!searchOn) return;
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.75f : 0;
    if (searchFor != peer) {  // (another chat: from the end again)
        searchFor = peer;
        searchAt = -1;
    }
    // what matches: this chat's messages (newest last)
    std::string q = Fold(searchText);
    std::vector<int> hits;
    auto it = eng.chats.find(peer);
    if (!q.empty() && it != eng.chats.end())
        for (int i = 0; i < (int)it->second.messages.size(); i++) {
            const ChatMessage& m = it->second.messages[(size_t)i];
            if (m.kind != MsgKind::System && Fold(m.text).find(q) != std::string::npos) hits.push_back(i);
        }
    float bw = mobile ? h : ImGui::GetFrameHeight();
    float right = bw * 3 + ImGui::GetStyle().ItemSpacing.x * 4 + ImGui::CalcTextSize("000 / 000").x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - right);
    if (searchFocus) {
        ImGui::SetKeyboardFocusHere();
        searchFocus = false;
    }
    bool enter = ImGui::InputTextWithHint("##csearch", T("search in this chat"), &searchText, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited()) searchAt = -1;
    auto go = [&](int k) {
        if (hits.empty()) return;
        searchAt = (k % (int)hits.size() + (int)hits.size()) % (int)hits.size();
        searchJump = it->second.messages[(size_t)hits[(size_t)searchAt]].mid;
    };
    if (enter) go(searchAt < 0 ? (int)hits.size() - 1 : searchAt - 1);  // (Enter: the next one back in time)
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (hits.empty()) ImGui::TextDisabled("%s", q.empty() ? "" : "0");
    else ImGui::TextDisabled("%d / %d", searchAt < 0 ? 0 : searchAt + 1, (int)hits.size());
    ImGui::SameLine();
    if (ui::IconButton("srup", Icon::Up, T("Earlier"), bw)) go(searchAt < 0 ? (int)hits.size() - 1 : searchAt - 1);
    ImGui::SameLine();
    if (ui::IconButton("srdn", Icon::Down, T("Later"), bw)) go(searchAt < 0 ? 0 : searchAt + 1);
    ImGui::SameLine();
    if (ui::IconButton("srx", Icon::Cancel, T("Close"), bw) || (!mobile && ImGui::IsKeyPressed(ImGuiKey_Escape))) {
        searchOn = false;
        searchText.clear();
    }
    // all chats: a list of what's found elsewhere
    ImGui::Checkbox(T("in all chats"), &searchAll);
    if (searchAll && !q.empty()) {
        struct Hit {
            std::string key, mid, text;
            int64_t ts;
        };
        std::vector<Hit> all;
        for (auto& [key, cv] : eng.chats)
            for (auto& m : cv.messages)
                if (m.kind != MsgKind::System && Fold(m.text).find(q) != std::string::npos) all.push_back({key, m.mid, m.text, m.ts});
        std::sort(all.begin(), all.end(), [](const Hit& a, const Hit& b) { return a.ts > b.ts; });
        if (all.size() > 200) all.resize(200);
        float lh = (mobile ? ui::M().rowH * 1.1f : ImGui::GetTextLineHeightWithSpacing() * 2.2f);
        ImGui::BeginChild("##allhits", ImVec2(0, std::min(lh * (float)std::max<size_t>(1, all.size()) + ImGui::GetStyle().WindowPadding.y * 2,
                                                          ImGui::GetContentRegionAvail().y * 0.45f)),
                          ImGuiChildFlags_Borders);
        if (mobile) ui::TouchScroll();
        if (all.empty()) ImGui::TextDisabled("%s", T("Nothing found."));
        for (size_t i = 0; i < all.size(); i++) {
            ImGui::PushID((int)i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##hit", false, 0, ImVec2(0, lh - ImGui::GetStyle().ItemSpacing.y))) {
                selectedPeer = all[i].key;
                if (mobile && screen != MScreen::Chat) {
                    backStack.push_back(screen);
                    screen = MScreen::Chat;
                }
                searchJump = all[i].mid;
                searchAll = false;
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            std::string head = Emoji().Display(eng.PeerName(all[i].key)) + "  -  " + FormatTime(all[i].ts);
            std::string t = all[i].text;
            for (auto& c : t)
                if (c == '\n') c = ' ';
            ImVec4 clip(p.x, p.y, p.x + ImGui::GetContentRegionAvail().x, p.y + lh);
            dl->AddText(nullptr, 0, ImVec2(p.x + 4, p.y + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), head.c_str(), nullptr, 0, &clip);
            dl->AddText(nullptr, 0, ImVec2(p.x + 4, p.y + 2 + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_Text),
                        Emoji().Display(t).c_str(), nullptr, 0, &clip);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
}
