// LunaShare - the sleep timer: in some minutes, at the end of this song or at the end of this movie, the music and
// the movie stop. A minute before, a window asks whether to give it longer (or stop right away); a minute it isn't
// answered, it stops.
#include <algorithm>

#include "i18n.h"
#include "imgui.h"
#include "music.h"
#include "platform.h"
#include "player_host.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

static constexpr int64_t kWarn = 60 * 1000;  // the question comes this long before

void App::SleepSet(int minutes, int mode) {
    sleepMode = mode;
    sleepAt = mode == 0 && minutes > 0 ? SteadyMs() + (int64_t)minutes * 60000 : 0;
    sleepAsked = false;
    music::Play().SetStopAfterCurrent(mode == 1);
    if (mode < 0) sleepMode = -1;
}

void App::SleepStopAll() {
    music::Play().SetStopAfterCurrent(false);
    if (music::Play().GetNow().playing) music::Play().SetPlaying(false);
    if (eng.watch.sid && eng.watch.playing) eng.WatchControl(false, -1);
    if (Player().Playing()) Player().Pause();
    sleepMode = -1;
    sleepAt = 0;
    sleepAsked = false;
    toasts.push_back({EventType::Info, T("Sleep timer"), T("Stopped. Good night!"), "", 0, 4.0f});
}

// every frame, and while the app is in the background too (MusicTick)
void App::SleepTick() {
    if (sleepMode < 0) return;
    int64_t now = SteadyMs();
    if (sleepMode == 0) {
        if (now >= sleepAt) SleepStopAll();
        else if (!sleepAsked && sleepAt - now <= kWarn) {
            sleepAsked = true;
            sleepAskOpen = true;
        }
    } else if (sleepMode == 1) {  // the end of this song (the player stops by itself)
        if (!music::Play().StopAfterCurrent()) {
            sleepMode = -1;
            toasts.push_back({EventType::Info, T("Sleep timer"), T("Stopped. Good night!"), "", 0, 4.0f});
        }
    } else if (sleepMode == 2) {  // the end of this movie
        const WatchInfo& w = eng.watch;
        double dur = Player().Duration();
        if (!w.sid) sleepMode = -1;
        else if (dur > 0 && w.Target(now) >= dur - 0.4) SleepStopAll();
    }
}

// what the timer is set to, in a few words (for the buttons)
std::string App::SleepLabel() {
    if (sleepMode == 0) {
        int64_t left = std::max<int64_t>(0, sleepAt - SteadyMs()) / 1000;
        char b[32];
        snprintf(b, sizeof b, "%d:%02d", (int)(left / 60), (int)(left % 60));
        return TF("Sleep in %s", b);
    }
    if (sleepMode == 1) return T("Sleep after this song");
    if (sleepMode == 2) return T("Sleep after this movie");
    return T("Sleep timer");
}

// setting it (from the music player or the movie player)
void App::SleepPopup() {
    if (sleepOpen) {
        ImGui::OpenPopup("##sleep");
        sleepOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.9f, 0));
    if (!ImGui::BeginPopup("##sleep")) return;
    ImGui::TextUnformatted(T("Sleep timer"));
    ImGui::TextDisabled("%s", T("The music and the movie stop; a minute before, you're asked."));
    ImGui::Separator();
    const int mins[] = {5, 15, 30, 45, 60, 90, 120};
    for (int m : mins)
        if (ImGui::Selectable(TF("in %d minutes", m).c_str(), false, 0, ImVec2(0, h))) SleepSet(m, 0);
    if (music::Play().GetNow().active && !music::Play().GetNow().external &&
        ImGui::Selectable(T("at the end of this song"), sleepMode == 1, 0, ImVec2(0, h)))
        SleepSet(0, 1);
    if (eng.watch.sid && ImGui::Selectable(T("at the end of this movie"), sleepMode == 2, 0, ImVec2(0, h))) SleepSet(0, 2);
    if (sleepMode >= 0) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", SleepLabel().c_str());
        if (ImGui::Selectable(T("Turn the timer off"), false, 0, ImVec2(0, h))) SleepSet(0, -1);
    }
    ImGui::EndPopup();
}

// a minute before: longer, or stop now?
void App::DrawSleepAsk() {
    if (sleepAskOpen) {
        ImGui::OpenPopup("##sleepask");
        sleepAskOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.85f : ImGui::GetFrameHeight() * 1.2f;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(mobile ? vp->WorkSize.x * 0.9f : ImGui::GetFontSize() * 24, 0));
    if (!ImGui::BeginPopupModal("##sleepask", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) return;
    if (sleepMode != 0) {  // (turned off meanwhile)
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    int64_t left = std::max<int64_t>(0, sleepAt - SteadyMs()) / 1000;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted(TF("Stopping in %d s", (int)left).c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s", T("The sleep timer. Longer?"));
    ImGui::Spacing();
    float w3 = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
    auto more = [&](int m) {
        sleepAt = std::max(sleepAt, SteadyMs()) + (int64_t)m * 60000;
        sleepAsked = false;
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button("+5 min", ImVec2(w3, h))) more(5);
    ImGui::SameLine();
    if (ImGui::Button("+15 min", ImVec2(w3, h))) more(15);
    ImGui::SameLine();
    if (ImGui::Button("+30 min", ImVec2(w3, h))) more(30);
    float w2 = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    if (ImGui::Button(T("Stop now"), ImVec2(w2, h))) {
        ImGui::CloseCurrentPopup();
        SleepStopAll();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Turn the timer off"), ImVec2(w2, h))) {
        SleepSet(0, -1);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
