// Watch together: the player follows the shared clock; a full-window screen with the movie and its controls; the
// invitation / "watching" strip in the chat.
#include <algorithm>
#include <cfloat>
#include <cmath>

#include "i18n.h"
#include "imgui.h"
#include "emoji_host.h"
#include "media_host.h"
#include "platform.h"
#include "player_host.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

static std::string Hms(double s) {
    if (!(s >= 0)) s = 0;
    int t = (int)s, h = t / 3600, m = t / 60 % 60, sec = t % 60;
    char b[32];
    if (h) snprintf(b, sizeof(b), "%d:%02d:%02d", h, m, sec);
    else snprintf(b, sizeof(b), "%d:%02d", m, sec);
    return b;
}

// every frame: open the movie for the session I'm in, and keep it on the shared clock
void App::WatchSync() {
    const WatchInfo& w = eng.watch;
    std::string id = w.sid ? "watch:" + std::to_string(w.sid) : "";
    if (!w.sid) {
        if (!watchOpened.empty()) {
            if (Player().IsCurrent(watchOpened) || Player().OpenState() == PlayerHost::Opening::Busy) Player().Stop();
            watchOpened.clear();
            watchShown = false;
        }
        return;
    }
    bool host = w.host.empty();
    std::string cur = host ? w.path : id;
    if (watchOpened != cur) {  // a new session: open its movie (paused)
        watchOpened = cur;
        watchFailed = false;
        watchLastSeek = 0;
        if (host) {
            if (Player().Play(w.path)) Player().Pause();
            else watchFailed = true;
        } else {
            Engine* e = &eng;
            Player().OpenStream(id, w.size, [e](int64_t off, uint8_t* buf, int len) { return e->WatchRead(off, buf, len); });
        }
    }
    if (!host) {
        auto st = Player().OpenState();
        if (st == PlayerHost::Opening::Busy) return;
        if (st == PlayerHost::Opening::Failed && !Player().IsCurrent(cur)) {
            watchFailed = true;
            return;
        }
    }
    if (!Player().IsCurrent(cur)) return;  // (something else is playing just now)
    int64_t now = SteadyMs();
    double target = w.Target(now);
    double dur = Player().Duration();
    if (dur > 0) target = std::min(target, dur - 0.05);
    bool play = w.playing && !w.hold && !(dur > 0 && target >= dur - 0.1);
    if (play != Player().Playing()) Player().SetPlaying(play);
    double pos = Player().Position();
    if (std::fabs(pos - target) > (play ? 1.0 : 0.2) && now - watchLastSeek > 1500) {
        Player().Seek(target);
        watchLastSeek = now;
    }
}

void App::OpenWatchPicker(const std::string& peer) {
    selectedPeer = peer;
    browseFor = BrowseFor::Watch;
    browser.Open(FileBrowser::Mode::OpenFile, T("Watch a movie together"), browser.lastDir);
}

// the strip on top of a chat: someone invites me / we're watching
void App::DrawWatchBanner(const std::string& peer) {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    const WatchInfo& w = eng.watch;
    const WatchInviteInfo* inv = nullptr;
    for (auto& i : eng.watchInvites)
        if (i.key == peer && i.sid != w.sid && SteadyMs() - i.at < 6 * 3600 * 1000LL) inv = &i;
    bool watching = w.sid && w.key == peer;
    if (!inv && !watching) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(50, 60, 110, 255));
    float rowH = (mobile ? ui::M().rowH * 0.8f : ImGui::GetFrameHeight()) + ImGui::GetStyle().WindowPadding.y * 2;
    ImGui::BeginChild("watchbanner", ImVec2(0, rowH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::AlignTextToFramePadding();
    std::string text = watching ? TF("Watching \"%s\" together", w.name.c_str())
                                : TF("%s invites you to watch \"%s\"", Emoji().Display(eng.PeerName(eng.ChatKey(inv->host))).c_str(), inv->name.c_str());
    float bw = ImGui::CalcTextSize(watching ? T("Open") : T("Watch")).x + ImGui::GetStyle().FramePadding.x * 2 + (mobile ? h : 0);
    float closeW = inv && !watching ? (h > 0 ? h : ImGui::GetFrameHeight()) + ImGui::GetStyle().ItemSpacing.x : 0;
    float avail = ImGui::GetContentRegionAvail().x - bw - closeW - ImGui::GetStyle().ItemSpacing.x;
    std::string t = text;
    while (t.size() > 4 && ImGui::CalcTextSize(t.c_str()).x > avail) {
        t.pop_back();
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();
    }
    if (t != text) t += "...";
    ImGui::TextUnformatted(t.c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - bw - closeW);
    if (watching) {
        if (ui::IconTextButton(T("Open"), Icon::Play, ImVec2(bw, h))) watchShown = true;
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 140, 85, 255));
        bool go = ui::IconTextButton(T("Watch"), Icon::Play, ImVec2(bw, h));
        ImGui::PopStyleColor();
        uint64_t sid = inv->sid;
        ImGui::SameLine();
        if (ui::IconButton("wdismiss", Icon::Cancel, T("Not now"), h)) {
            auto& v = eng.watchInvites;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const WatchInviteInfo& x) { return x.sid == sid; }), v.end());
        } else if (go) {
            if (eng.JoinWatch(sid)) watchShown = true;
            else toasts.push_back({EventType::Error, T("Watch together"), T("Not connected to the device with the movie - trying again..."), "", 0, 4.0f});
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// the movie, full window
void App::DrawWatch() {
    const WatchInfo& w = eng.watch;
    if (!w.sid) {
        watchShown = false;
        return;
    }
    bool mobile = ui::M().mobile;
    float bh = mobile ? ui::M().rowH : ImGui::GetFrameHeight() * 1.5f;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(8, 8, 10, 255));
    ImGui::Begin("##watch", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float sp = ImGui::GetStyle().ItemSpacing.x;
    // ---- top: back, title, who's watching, leave
    if (ui::IconButton("wback", Icon::Back, T("Back to the chat (the movie goes on)"), bh) || ImGui::IsKeyPressed(ImGuiKey_AppBack) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape))
        watchShown = false;
    ImGui::SameLine();
    float leaveW = ImGui::CalcTextSize(T("Leave")).x + ImGui::GetStyle().FramePadding.x * 2 + (mobile ? bh * 0.4f : 0);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(w.name.c_str());
    std::string who;
    for (auto& p : w.people) who += (who.empty() ? "" : ", ") + Emoji().Display(p);
    if (w.hold && !w.waitFor.empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s", TF("Waiting for %s...", w.waitFor.c_str()).c_str());
    else ImGui::TextDisabled("%s", who.c_str());
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - leaveW);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 50, 50, 255));
    if (ImGui::Button(w.host.empty() ? T("Stop") : T("Leave"), ImVec2(leaveW, bh))) {
        eng.LeaveWatch();
        ImGui::PopStyleColor();
        ImGui::End();
        watchShown = false;
        return;
    }
    ImGui::PopStyleColor();

    // ---- the picture
    float controlsH = bh + ImGui::GetTextLineHeight() * 1.6f + sp * 3;
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 b(a.x + avail.x, a.y + avail.y - controlsH);
    ImGui::InvisibleButton("##screen", ImVec2(avail.x, std::max(1.0f, b.y - a.y)));
    bool tapped = ImGui::IsItemClicked();
    std::string cur = w.host.empty() ? w.path : "watch:" + std::to_string(w.sid);
    bool open = Player().IsCurrent(cur);
    const auto& vt = Media().PlayerVideo();
    if (open && vt.valid && Player().Info().has_video) {
        Media().DrawVideo(dl, vt, a, b, false);
    } else {
        const char* msg = watchFailed ? T("This movie can't be played on this device.")
                        : !open ? T("Loading the movie...") : Player().Info().has_video ? T("Loading...") : "";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), IM_COL32(220, 220, 220, 255), msg);
    }
    if (open && w.hold) {  // a spinner while someone loads
        ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f + (vt.valid ? 0 : ImGui::GetFontSize() * 2));
        float r = ImGui::GetFontSize() * 1.2f, t = (float)ImGui::GetTime() * 6;
        dl->AddCircleFilled(c, r * 1.5f, IM_COL32(0, 0, 0, 120));
        dl->PathArcTo(c, r, t, t + 4.2f, 24);
        dl->PathStroke(IM_COL32(255, 255, 255, 220), ImDrawFlags_None, 3.0f);
    }

    // ---- controls
    int64_t now = SteadyMs();
    double dur = open ? Player().Duration() : 0;
    double pos = w.Target(now);
    if (dur > 0) pos = std::min(pos, dur);
    bool playing = w.playing;
    bool toggle = tapped || ImGui::IsKeyPressed(ImGuiKey_Space, false);
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + sp));
    if (ui::IconButton("wplay", playing ? Icon::Pause : Icon::Play, playing ? T("Pause for everyone") : T("Play for everyone"), bh)) toggle = true;
    if (toggle) eng.WatchControl(!playing, -1);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) eng.WatchControl(playing, std::max(0.0, pos - 10));
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) eng.WatchControl(playing, dur > 0 ? std::min(dur - 1, pos + 10) : pos + 10);
    ImGui::SameLine();
    // seek bar: drag to look for a moment, let go to take everyone there
    ImVec2 p = ImGui::GetCursorScreenPos();
    float callW = 0;
    bool inCall = eng.call.state == CallState::Active;
    if (inCall) callW = (bh + sp) * 2;
    float barW = std::max(40.0f, ImGui::GetContentRegionAvail().x - callW);
    ImGui::InvisibleButton("wseek", ImVec2(barW, bh));
    float frac = dur > 0 ? (float)std::clamp(pos / dur, 0.0, 1.0) : 0;
    if (ImGui::IsItemActive() && dur > 0) {
        watchDrag = std::clamp((ImGui::GetIO().MousePos.x - p.x) / barW, 0.0f, 1.0f);
        frac = watchDrag;
    }
    if (ImGui::IsItemDeactivated() && dur > 0 && watchDrag >= 0) {
        eng.WatchControl(playing, watchDrag * dur);
        watchDrag = -1;
    }
    float cy = p.y + bh * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, cy - 4), ImVec2(p.x + barW, cy + 4), IM_COL32(255, 255, 255, 50), 4);
    if (!w.host.empty()) {  // how much of the movie is here
        float lf = (float)std::clamp(eng.WatchLoaded(), 0.0, 1.0);
        dl->AddRectFilled(ImVec2(p.x, cy + 2), ImVec2(p.x + barW * lf, cy + 4), IM_COL32(255, 255, 255, 90), 2);
    }
    dl->AddRectFilled(ImVec2(p.x, cy - 4), ImVec2(p.x + barW * frac, cy + 4), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 4);
    dl->AddCircleFilled(ImVec2(p.x + barW * frac, cy), 8, ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
    if (inCall) {  // talking while watching: mute / hang up right here
        ImGui::SameLine();
        bool muted = Media().Muted();
        if (ui::IconButton("wmute", muted ? Icon::MicOff : Icon::Mic, muted ? T("Unmute") : T("Mute"), bh)) Media().SetMute(!muted);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(190, 50, 50, 255));
        if (ui::IconButton("whang", Icon::HangUp, T("Hang up"), bh)) eng.EndCall();
        ImGui::PopStyleColor();
    }
    std::string time = Hms(watchDrag >= 0 ? watchDrag * dur : pos) + " / " + (dur > 0 ? Hms(dur) : std::string("--:--"));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + bh));
    ImGui::TextUnformatted(time.c_str());
    if (!w.host.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", TF("%d%% here", (int)(eng.WatchLoaded() * 100)).c_str());
    }
    ImGui::End();
}
