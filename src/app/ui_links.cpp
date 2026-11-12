// "A link for anyone": a file someone without LunaShare downloads in a browser (share_links.h). From the "Send to"
// window: one entry there, a small window with the link (copy, share, QR code) and its few options folded away; the
// links stay listed under Files > Transfers while there are any; "ask me first" asks in a small window.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "archives.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "media_host.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

struct App::ShareLinkUi {
    std::vector<std::string> paths;
    std::string id, err;
    int valid = 1, people = 0;  // 1 hour / 1 day / 1 week; one person / 5 people / anyone with the link
    std::string pin;
    bool ask = false, opened = false;
    // several files (or a folder) go as one zip, packed first
    struct Zip {
        std::atomic<bool> done{false}, cancel{false};
        std::atomic<int64_t> at{0}, total{0};
        std::string out, err;
        bool ok = false;
        std::thread t;
        ~Zip() {
            cancel = true;
            if (t.joinable()) t.join();
        }
    };
    std::shared_ptr<Zip> zip;
};

static std::string LinksDir(Engine& eng) { return JoinPath(eng.ConfigPath(), "links"); }

static std::string LeftText(int64_t ms) {
    if (ms <= 0) return T("ended");
    int64_t m = ms / 60000;
    if (m < 60) return TF("%d min left", (int)std::max<int64_t>(1, m));
    if (m < 48 * 60) return TF("%d h left", (int)(m / 60));
    return TF("%d days left", (int)(m / 1440));
}

void App::OpenSendToList(const std::vector<std::pair<std::string, std::string>>& files, const std::string& name) {
    std::vector<std::string> paths;
    for (auto& f : files) paths.push_back(f.first);
    OpenSendTo(paths);
    pendingSendList = files;
    pendingSendName = name;
}

void App::OpenShareLink(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    if (!shareUi) shareUi = std::make_shared<ShareLinkUi>();
    auto keep = *shareUi;  // (the options chosen last time stay)
    *shareUi = ShareLinkUi();
    shareUi->valid = keep.valid;
    shareUi->people = keep.people;
    shareUi->ask = keep.ask;
    shareUi->paths = paths;
    shareUi->opened = true;
}

void App::DrawShareLinkPopups() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    auto center = [&] {
        ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.94f, 0));
        else ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(vp->WorkSize.x * 0.9f, ImGui::GetFontSize() * 28), 0), ImVec2(vp->WorkSize.x * 0.9f, vp->WorkSize.y * 0.95f));
    };
    auto wrapped = [](const std::string& s, bool dim = true) {
        ImGui::PushTextWrapPos(0);
        if (dim) ImGui::TextDisabled("%s", s.c_str());
        else ImGui::TextUnformatted(s.c_str());
        ImGui::PopTextWrapPos();
    };

    // ---- someone waits for my yes ("ask me first")
    auto asks = eng.shareLinks.Asks();
    const char* askTitle = T("Someone wants your file");
    if (!asks.empty() && !ImGui::IsPopupOpen(askTitle)) ImGui::OpenPopup(askTitle);
    center();
    if (ImGui::BeginPopupModal(askTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize * !mobile)) {
        if (asks.empty()) {
            ImGui::CloseCurrentPopup();
        } else {
            auto& a = asks.front();
            wrapped(a.via == "lan" ? TF("Someone on this network (%s) opened your link to \"%s\". Let them download it?", a.ip.c_str(), a.name.c_str())
                                   : TF("Someone on the Internet (%s) opened your link to \"%s\". Let them download it?", a.ip.c_str(), a.name.c_str()),
                    false);
            if (asks.size() > 1) ImGui::TextDisabled(T("... and %d more"), (int)asks.size() - 1);
            ImGui::Spacing();
            float bw = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2 : 0;
            if (ui::IconTextButton(T("Allow"), Icon::Check, ImVec2(bw, h))) eng.shareLinks.Answer(a.id, true);
            ImGui::SameLine();
            if (ImGui::Button(T("Don't allow"), ImVec2(bw, h))) eng.shareLinks.Answer(a.id, false);
        }
        ImGui::EndPopup();
    }

    // ---- making a link
    if (!shareUi) return;
    auto& s = *shareUi;
    const char* title = T("Link for anyone");
    if (s.opened) {
        ImGui::OpenPopup(title);
        s.opened = false;
    }
    center();
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize * !mobile)) return;
    bool close = false;
    auto createLink = [&] {
        ShareLinks::Options o;
        static const int64_t validMs[] = {3600 * 1000LL, 24 * 3600 * 1000LL, 7 * 24 * 3600 * 1000LL};
        static const int people[] = {1, 5, 0};
        o.validMs = validMs[std::clamp(s.valid, 0, 2)];
        o.maxPeople = people[std::clamp(s.people, 0, 2)];
        o.pin = s.pin;
        o.ask = s.ask;
        std::string err;
        s.id = eng.shareLinks.Create(s.paths[0], o, err);
        if (s.id.empty()) s.err = err;
    };
    static const bool devCreate = getenv("LUNASHARE_SHARELINK") && strstr(getenv("LUNASHARE_SHARELINK"), "|create");  // (dev)
    if (devCreate && s.id.empty() && !s.zip && ImGui::GetFrameCount() > 25) createLink();
    if (s.zip && s.zip->done) {
        if (s.zip->t.joinable()) s.zip->t.join();
        if (s.zip->ok) {
            s.paths = {s.zip->out};
        } else {
            s.err = s.zip->err.empty() ? T("Could not pack the files.") : s.zip->err;
            RemoveFile(s.zip->out);
        }
        s.zip.reset();
        if (s.err.empty()) createLink();
    }
    if (s.zip) {
        // packing several things into one file first
        busy = true;
        auto& z = *s.zip;
        ImGui::TextUnformatted(TF("Packing %s", BaseName(z.out).c_str()).c_str());
        ImGui::ProgressBar(z.total > 0 ? (float)((double)z.at / (double)z.total) : 0.0f, ImVec2(mobile ? -1 : ImGui::GetFontSize() * 20, 0));
        if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h))) {
            z.cancel = true;
            close = true;
        }
    } else if (s.id.empty()) {
        {
            int64_t total = 0;
            for (auto& p : s.paths) total += IsDirectory(p) ? 0 : std::max<int64_t>(0, FileSize(p));
            bool one = s.paths.size() == 1 && !IsDirectory(s.paths[0]);
            ImGui::TextUnformatted(one ? BaseName(s.paths[0]).c_str() : TF("%d item(s), sent as one .zip file", (int)s.paths.size()).c_str());
            if (total) ImGui::TextDisabled("%s", FormatBytes((double)total).c_str());
        }
        ImGui::Spacing();
        wrapped(T("Anyone who has the link can download it in a web browser, without LunaShare. It comes straight from this device, "
                  "so it works while LunaShare runs here."));
        bool web = eng.shareLinks.WebAvailable(), lan = !eng.shareLinks.LanBase().empty();
        if (!web && lan) wrapped(T("Your server doesn't offer links for anyone, so this one works only on this network (the same Wi-Fi)."));
        if (!web && !lan) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::ColWarn());
            wrapped(T("No link can be made right now: this device isn't on a network, and your server doesn't offer links for anyone."), false);
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        if (ImGui::TreeNode(T("Options"))) {
            const char* valid[] = {T("1 hour"), T("1 day"), T("1 week")};
            const char* people[] = {T("one person"), T("5 people"), T("anyone with the link")};
            ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 12);
            ImGui::Combo(T("Valid for"), &s.valid, valid, 3);
            ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 12);
            ImGui::Combo(T("For"), &s.people, people, 3);
            ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 12);
            ImGui::InputTextWithHint(T("PIN"), T("none"), &s.pin, ImGuiInputTextFlags_CharsDecimal);
            ui::Checkbox(T("Ask me before each download"), &s.ask);
            ImGui::TreePop();
        }
        if (!s.err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::ColWarn());
            wrapped(s.err, false);
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        float bw = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2 : 0;
        if (!web && !lan) ImGui::BeginDisabled();
        bool make = ui::IconTextButton(T("Create link"), Icon::Link, ImVec2(bw, h));
        if (!web && !lan) ImGui::EndDisabled();
        if (make) {
            s.err.clear();
            if (s.paths.size() > 1 || IsDirectory(s.paths[0])) {
                // pack first (on a thread)
                MakeDirs(LinksDir(eng));
                std::string name = s.paths.size() == 1 ? BaseName(s.paths[0]) : "LunaShare " + FormatTime(NowMs(), true);
                for (auto& c : name)
                    if (c == ':' || c == '/' || c == '\\') c = '-';
                auto z = std::make_shared<ShareLinkUi::Zip>();
                z->out = UniquePath(JoinPath(LinksDir(eng), name + ".zip"));
                std::vector<std::string> in = s.paths;
                ShareLinkUi::Zip* zp = z.get();
                z->t = std::thread([zp, in] {
                    zp->ok = arc::Create(zp->out, arc::Format::Zip, in, [zp](int64_t d, int64_t t, const std::string&) {
                        zp->at = d;
                        zp->total = t;
                        return !zp->cancel;
                    }, zp->err);
                    zp->done = true;
                });
                s.zip = z;
            } else {
                createLink();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(bw, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
    } else {
        ShareLinkInfo info;
        bool found = false;
        for (auto& l : eng.shareLinks.List())
            if (l.id == s.id) info = l, found = true;
        if (!found) {
            close = true;
        } else {
            std::string url = info.webUrl.empty() ? info.lanUrl : info.webUrl;
            ImGui::TextUnformatted(info.name.c_str());
            ImGui::TextDisabled("%s  -  %s", FormatBytes((double)info.size).c_str(), LeftText(info.expires - NowMs()).c_str());
            if (url.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::ColWarn());
                wrapped(T("No link can be made right now: this device isn't on a network, and your server doesn't offer links for anyone."), false);
                ImGui::PopStyleColor();
            } else {
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##url", &url, ImGuiInputTextFlags_ReadOnly);
                if (info.webUrl.empty()) wrapped(T("Works only on this network (the same Wi-Fi)."));
                if (info.pin && !s.pin.empty()) wrapped(TF("Tell the PIN separately (not in the same message): %s", s.pin.c_str()), false);
                float bw = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (Bridge().shareText ? 2 : 1)) / (Bridge().shareText ? 3 : 2) : 0;
                if (ui::IconTextButton(T("Copy"), Icon::Copy, ImVec2(bw, h))) {
                    ImGui::SetClipboardText(url.c_str());
                    toasts.push_back({EventType::Info, "", T("Copied."), "", 0, 2.0f});
                }
                if (Bridge().shareText) {
                    ImGui::SameLine();
                    if (ui::IconTextButton(T("Share..."), Icon::Share, ImVec2(bw, h))) Bridge().shareText(url);
                }
                ImGui::SameLine();
                if (ImGui::Button(T("Done"), ImVec2(bw, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
                ImGui::Spacing();
                float q = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 10);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - q) * 0.5f);
                ui::QrCode(url.c_str(), q);
                if (!info.ready) {
                    busy = true;
                    ImGui::TextDisabled("%s", T("Checking the file... (the link already works)"));
                }
            }
            if (url.empty() && (ImGui::Button(T("Close"), ImVec2(mobile ? -1 : 0, h)) || ImGui::IsKeyPressed(ImGuiKey_Escape))) {
                eng.shareLinks.Remove(s.id);
                close = true;
            }
        }
    }
    if (close) {
        shareUi->paths.clear();
        shareUi->id.clear();
        shareUi->zip.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::DrawShareLinksList() {
    auto links = eng.shareLinks.List();
    if (links.empty()) return;
    bool mobile = ui::M().mobile;
    int64_t now = NowMs();
    ImGui::SeparatorText(T("Links for anyone"));
    float bs = ImGui::GetFrameHeight();
    for (auto& l : links) {
        ImGui::PushID(l.id.c_str());
        bool live = !l.revoked && now < l.expires;
        int got = 0;
        for (auto& v : l.visits) got += v.done;
        ImGui::AlignTextToFramePadding();
        if (!live) ImGui::BeginDisabled();
        ImGui::TextUnformatted(l.name.c_str());
        if (!live) ImGui::EndDisabled();
        if (ImGui::IsItemHovered() || (mobile && ImGui::IsItemClicked())) {
            std::string tip = TF("Valid until %s", FormatTime(l.expires).c_str());
            if (l.maxPeople) tip += "\n" + TF("For %d of %d people", l.people, l.maxPeople);
            for (auto& v : l.visits)
                tip += "\n" + FormatTime(v.at) + "  " + v.ip + "  " + (v.via == "lan" ? T("on this network") : T("through the server")) + "  " +
                       (v.state == 2 ? T("not allowed") : v.state == 1 ? T("waiting") : v.done ? T("downloaded") : FormatBytes((double)v.bytes));
            ImGui::SetTooltip("%s", tip.c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", (live ? LeftText(l.expires - now) + (got ? "  -  " + TF("downloaded %d time(s)", got) : std::string())
                                        : std::string(l.revoked ? T("turned off") : T("ended")))
                                      .c_str());
        float right = ImGui::GetWindowContentRegionMax().x;
        int n = live ? 2 : 1;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - n * bs - (n - 1) * ImGui::GetStyle().ItemSpacing.x));
        if (live) {
            if (ui::IconButton("copy", Icon::Copy, T("Copy the link"), bs)) {
                std::string url = l.webUrl.empty() ? l.lanUrl : l.webUrl;
                ImGui::SetClipboardText(url.c_str());
                toasts.push_back({EventType::Info, "", T("Copied."), "", 0, 2.0f});
            }
            ImGui::SameLine();
            if (ui::IconButton("off", Icon::Cancel, T("Turn the link off"), bs)) eng.shareLinks.Revoke(l.id);
        } else if (ui::IconButton("rm", Icon::Trash, T("Remove"), bs)) {
            if (DirName(l.path) == LinksDir(eng)) RemoveFile(l.path);  // a .zip packed for it
            eng.shareLinks.Remove(l.id);
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
}

// ---- picking files in a list (gallery, movies, music, archives) ------------------------------------------------------

bool App::PickItem(PickState& st, const std::string& path, bool tapped) {
    bool held = ImGui::IsItemActive() && ImGui::GetIO().MouseDownDuration[0] >= 0.45f && !ImGui::IsMouseDragging(0, 8.0f);
    if (ImGui::IsItemClicked() && ImGui::GetIO().KeyCtrl) held = true;  // (desktops: Ctrl+click picks)
    if (held && st.heldDone != path) {
        st.heldDone = path;
        st.selecting = true;
        if (!st.sel.erase(path)) st.sel.insert(path);
        return false;
    }
    if (!tapped) return false;
    if (st.heldDone == path) {  // (the end of that long press)
        st.heldDone.clear();
        return false;
    }
    if (st.selecting) {
        if (!st.sel.erase(path)) st.sel.insert(path);
        return false;
    }
    return true;
}

void App::PickMark(ImDrawList* dl, ImVec2 a, ImVec2 b, bool picked, float rounding) {
    if (!picked) return;
    dl->AddRectFilled(a, b, IM_COL32(60, 110, 220, 90), rounding);
    dl->AddRect(a, b, IM_COL32(90, 150, 255, 255), rounding, 0, 3.0f);
    float r = std::min(b.x - a.x, b.y - a.y) * 0.11f;
    ImVec2 c(a.x + r + 4, a.y + r + 4);
    dl->AddCircleFilled(c, r, IM_COL32(60, 120, 230, 255));
    ui::DrawIcon(dl, Icon::Check, c, r * 1.5f, IM_COL32_WHITE);
}

void App::DrawPickBar(PickState& st, const std::vector<std::string>& all, const std::function<void()>& extra,
                      const std::function<void()>& deleted) {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGui::PushID(&st);
    std::vector<std::string> sel(st.sel.begin(), st.sel.end());
    auto allNone = [&] {
        if (ImGui::Button(T("All"), ImVec2(0, h)))
            for (auto& p : all) st.sel.insert(p);
        ImGui::SameLine();
        if (ImGui::Button(T("None"), ImVec2(0, h))) st.Clear();
    };
    // phones: "N picked  All  None", then the three actions sharing the width
    float bw = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3 : 0;
    if (mobile) {
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, (h - ImGui::GetTextLineHeight()) * 0.5f - ImGui::GetStyle().ItemSpacing.y));
        ImGui::TextUnformatted(TF("%d picked", (int)sel.size()).c_str());
        ImGui::EndGroup();
        ImGui::SameLine();
        allNone();
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TF("%d picked", (int)sel.size()).c_str());
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(sel.empty());
    if (ui::IconTextButton(T("Send..."), Icon::Send, ImVec2(bw, h))) {
        OpenSendTo(sel);  // (to a chat, or as a link for anyone)
        st.Clear();
    }
    ImGui::SameLine();
    if (ui::IconTextButton(T("Pack..."), Icon::Archive, ImVec2(bw, h))) {
        StartArchiveCreate(sel);
        st.Clear();
    }
    ImGui::SameLine();
    if (ui::IconTextButton(T("Delete..."), Icon::Trash, ImVec2(bw, h))) st.deleteAsk = true;
    ImGui::EndDisabled();
    if (extra) {
        if (!mobile) ImGui::SameLine();
        extra();
    }
    if (!mobile) {
        ImGui::SameLine();
        allNone();
    }
    if (st.deleteAsk) {
        ImGui::OpenPopup("##pickdel");
        st.deleteAsk = false;
    }
    if (ImGui::BeginPopupModal("##pickdel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(TF("Delete %d file(s) from this device? This can't be undone.", (int)st.sel.size()).c_str());
        if (ImGui::Button(T("Delete"), ImVec2(0, h))) {
            for (auto& p : st.sel) RemoveFile(p);
            st.Clear();
            if (deleted) deleted();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(0, h))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

bool App::ClearPicks() {
    PickState* st = screen == MScreen::Gallery ? &galleryPick : screen == MScreen::Movies ? &moviePick
                  : screen == MScreen::Music  ? &musicPick   : screen == MScreen::Archives ? &archivePick : nullptr;
    if (!st || !st->Active()) return false;
    st->Clear();
    return true;
}
