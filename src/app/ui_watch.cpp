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

static int64_t now0() { return SteadyMs(); }

// every frame: open the movie for the session I'm in, and keep it on the shared clock
void App::WatchSync() {
    const WatchInfo& w = eng.watch;
    std::string id = w.sid ? "watch:" + std::to_string(w.sid) : "";
    // phones: sideways and without the system bars while the movie screen shows; computers: leave full screen with it
    bool movie = watchShown && w.sid;
    if (movie != watchMovie) {
        watchMovie = movie;
        if (ui::M().mobile && Bridge().movieMode) Bridge().movieMode(movie);
        if (!movie && watchFullscreen && Bridge().setFullscreen) {
            watchFullscreen = false;
            Bridge().setFullscreen(false, false);
        }
        watchActive = SteadyMs();
    }
    bool awake = w.sid && w.playing;
    if (awake != watchAwake) {
        watchAwake = awake;
        if (Bridge().keepAwake) Bridge().keepAwake(awake);
    }
    if (!w.sid) {
        if (!watchOpened.empty()) {
            if (Player().IsCurrent(watchOpened) || Player().OpenState() == PlayerHost::Opening::Busy) Player().Stop();
            watchOpened.clear();
            watchShown = false;
        }
        return;
    }
    bool host = w.host.empty(), lib = !w.libId.empty();
    bool stream = !host || lib;  // read through the network (a host's movie, or a contact's Shared folder)
    std::string cur = lib ? "lib:" + w.libId : host ? w.path : id;
    if (watchOpened != cur) {  // a new session: open its movie (paused)
        watchOpened = cur;
        watchFailed = false;
        watchLastSeek = 0;
        watchSubsAdded.clear();
        watchSubFollow = true;
        watchSubApplied = "-";
        if (lib) {
            Player().OpenStream(cur, w.size, eng.LibStreamReader(w.libId));
        } else if (host) {
            if (Player().Play(w.path)) Player().Pause();
            else watchFailed = true;
        } else {
            Engine* e = &eng;
            Player().OpenStream(id, w.size, [e](int64_t off, uint8_t* buf, int len) { return e->WatchRead(off, buf, len, 200); });
        }
    }
    if (stream) {
        auto st = Player().OpenState();
        if (st == PlayerHost::Opening::Busy) return;
        if (st == PlayerHost::Opening::Failed && !Player().IsCurrent(cur)) {
            watchFailed = true;
            return;
        }
    }
    if (!Player().IsCurrent(cur)) return;  // (something else is playing just now)
    // subtitle files: next to the movie (here), or the copies the host sent
    if (host && !lib && watchSubsAdded.empty()) {
        watchSubsAdded.insert(w.path);
        Player().AddSidecarSubtitles(w.path);
    }
    for (auto& f : w.subFiles)
        if (!host && watchSubsAdded.insert(f).second) Player().AddSubtitles(f);
    // the subtitles: the host tells which ones it has on; viewers follow that unless they picked their own
    if (now0() - watchSubCheck > 300) {
        watchSubCheck = now0();
        auto tracks = Player().Tracks();
        if (host) {
            std::string desc;
            int n = 0;
            for (auto& t : tracks) {
                if (t.kind != LSP_TRACK_SUBTITLE) continue;
                if (!t.external) n++;
                if (t.selected) desc = t.external ? std::string("file:") + t.title : "track:" + std::to_string(n) + ":" + t.lang;
            }
            eng.WatchSetHostSub(desc);
        } else if (watchSubFollow && w.hostSub != watchSubApplied) {
            int id = -2, n = 0;
            if (w.hostSub.empty()) id = -1;
            for (auto& t : tracks) {
                if (t.kind != LSP_TRACK_SUBTITLE) continue;
                if (!t.external) n++;
                if (w.hostSub.rfind("file:", 0) == 0 && t.external && w.hostSub.substr(5) == t.title) id = t.id;
                if (w.hostSub.rfind("track:", 0) == 0 && !t.external && atoi(w.hostSub.c_str() + 6) == n) id = t.id;
            }
            if (id != -2) {  // (a file may still be on its way)
                Player().SelectTrack(LSP_TRACK_SUBTITLE, id);
                watchSubApplied = w.hostSub;
            }
        }
    }
    static const char* devSub = getenv("LUNASHARE_WATCHSUB");  // dev: subtitles on (screenshots)
    if (devSub && !watchDevSub) {
        for (auto& t : Player().Tracks())
            if (t.kind == LSP_TRACK_SUBTITLE && !watchDevSub) {
                Player().SelectTrack(LSP_TRACK_SUBTITLE, t.id);
                watchDevSub = true;
            }
    }
    int64_t now = SteadyMs();
    double target = w.Target(now);
    double dur = Player().Duration();
    // loop: the host starts everyone again from the beginning
    if (host && w.loop && w.playing && !w.hold && dur > 0 && target >= dur - 0.15 && now - watchLastSeek > 1500) {
        eng.WatchControl(true, 0);
        Player().Seek(0);  // right away here (the follow-the-clock seek below waits 1.5 s after any seek)
        Player().SetPlaying(true);
        watchLastSeek = now;
        return;
    }
    if (dur > 0) target = std::min(target, dur - 0.05);
    bool play = w.playing && !w.hold && !(dur > 0 && target >= dur - 0.1);
    if (play != Player().Playing()) Player().SetPlaying(play);
    double pos = Player().Position();
    if (std::fabs(pos - target) > (play ? 1.0 : 0.2) && now - watchLastSeek > 1500) {
        Player().Seek(target);
        watchLastSeek = now;
    }
}

void App::OpenMoviePlayer() {
    browseFor = BrowseFor::Movie;
    browser.Open(FileBrowser::Mode::OpenFile, T("Movie player"), browser.lastDir);
}

void App::OpenMovies() {
    if (ui::M().mobile) {
        if (screen != MScreen::Movies) backStack.push_back(screen);
        screen = MScreen::Movies;
    } else {
        tab = Tab::Movies;
        tabForce = true;
    }
}

void App::ToggleLoop() {
    Settings s = eng.GetSettings();
    s.playerLoop = !s.playerLoop;
    eng.ApplySettings(s);
    edit.playerLoop = s.playerLoop;
}

// outside the movie's chat (the movie player, a stream from a Shared folder): a strip to get back to it
void App::DrawWatchResume() {
    const WatchInfo& w = eng.watch;
    if (!w.sid || watchShown) return;
    bool mobile = ui::M().mobile;
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 60, 110, 255));
    std::string label = TF("Movie: %s", w.name.c_str());
    if (ui::IconTextButton((label + "##wresume").c_str(), Icon::Play, ImVec2(mobile ? -1 : ImGui::GetContentRegionAvail().x, mobile ? ui::M().rowH * 0.8f : 0)))
        watchShown = true;
    ImGui::PopStyleColor();
}

// add a person or a group to the movie that's on (the host)
void App::WatchAddPopup() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSizeConstraints(ImVec2(mobile ? vs.x * 0.8f : ImGui::GetFontSize() * 16, 0), ImVec2(vs.x * 0.95f, vs.y * 0.7f));
    if (!ImGui::BeginPopup("##watchadd")) return;
    ImGui::TextUnformatted(T("Watch together with..."));
    ImGui::Separator();
    int shown = 0;
    for (auto& k : SortedPeers()) {
        if (std::find(eng.watchKeys.begin(), eng.watchKeys.end(), k) != eng.watchKeys.end()) continue;
        const KnownDevice* d = eng.DeviceFor(k);
        if (d && d->blocked) continue;
        shown++;
        std::string name = Emoji().Display(eng.PeerName(k));
        if (eng.IsConnected(k)) name += "  ●";
        if (ImGui::Selectable((name + "##" + k).c_str(), false, 0, ImVec2(0, h))) {
            eng.WatchInviteTo(k);
            toasts.push_back({EventType::Info, T("Watch together"), TF("Invited %s.", eng.PeerName(k).c_str()), "", 0, 3.0f});
        }
    }
    if (!shown) ImGui::TextDisabled("%s", T("No one else to invite."));
    ImGui::EndPopup();
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

// "eng" -> "English" (the common ones; anything else is shown as it is)
static std::string LangName(const std::string& l) {
    static const char* t[][2] = {{"eng", "English"}, {"en", "English"}, {"pol", "Polski"}, {"pl", "Polski"}, {"ger", "Deutsch"},
                                 {"deu", "Deutsch"}, {"de", "Deutsch"}, {"fre", "Français"}, {"fra", "Français"}, {"fr", "Français"},
                                 {"spa", "Español"}, {"es", "Español"}, {"ita", "Italiano"}, {"it", "Italiano"}, {"rus", "Русский"},
                                 {"ru", "Русский"}, {"ukr", "Українська"}, {"uk", "Українська"}, {"cze", "Čeština"}, {"ces", "Čeština"},
                                 {"jpn", "日本語"}, {"ja", "日本語"}, {"por", "Português"}, {"pt", "Português"}, {"dut", "Nederlands"},
                                 {"nld", "Nederlands"}, {"swe", "Svenska"}, {"chi", "中文"}, {"zho", "中文"}, {"kor", "한국어"},
                                 {"hun", "Magyar"}, {"slo", "Slovenčina"}, {"slk", "Slovenčina"}, {"tur", "Türkçe"}};
    for (auto& r : t)
        if (l == r[0]) return r[1];
    return l == "und" ? "" : l;
}

static std::string TrackLabel(const LspTrack& t, int n) {
    std::string s = LangName(t.lang);
    if (t.title[0] && t.title != s) s += s.empty() ? t.title : std::string(" - ") + t.title;
    if (s.empty()) s = TF(t.kind == LSP_TRACK_AUDIO ? "Sound %d" : "Subtitles %d", n);
    std::string extra = t.codec;
    if (t.kind == LSP_TRACK_AUDIO && t.channels)
        extra += t.channels == 1 ? " mono" : t.channels == 2 ? " stereo" : t.channels == 6 ? " 5.1" : t.channels == 8 ? " 7.1" : " " + std::to_string(t.channels) + "ch";
    if (!extra.empty()) s += "  (" + extra + ")";
    return s;
}

// sound and subtitle choice (a popup, opened by the button)
void App::WatchTracksPopup() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    if (!ImGui::BeginPopup("##watchtracks")) return;
    {  // remembered for every movie
        bool ap = eng.settings.playerAutoplay;
        if (ImGui::Checkbox(T("Autoplay (every movie starts on its own)"), &ap)) {
            Settings s = eng.GetSettings();
            s.playerAutoplay = ap;
            eng.ApplySettings(s);
            edit.playerAutoplay = ap;
        }
    }
    auto tracks = Player().Tracks();
    ImGui::SeparatorText(T("Sound"));
    int n = 0;
    bool anySound = false;
    for (auto& t : tracks) {
        if (t.kind != LSP_TRACK_AUDIO) continue;
        anySound = true;
        std::string label = TrackLabel(t, ++n) + (t.playable ? "" : std::string("  - ") + T("can't play here"));
        ImGui::BeginDisabled(!t.playable);
        if (ImGui::Selectable((label + "##a" + std::to_string(t.id)).c_str(), t.selected != 0, ImGuiSelectableFlags_DontClosePopups, ImVec2(0, h)))
            Player().SelectTrack(LSP_TRACK_AUDIO, t.id);
        ImGui::EndDisabled();
    }
    if (!anySound) ImGui::TextDisabled("%s", T("no sound"));
    ImGui::SeparatorText(T("Subtitles"));
    const WatchInfo& w = eng.watch;
    bool viewer = w.sid && !w.host.empty();
    bool follow = viewer && watchSubFollow;
    if (viewer) {  // what the host has on (the default)
        std::string hs = w.hostSub.empty() ? std::string(T("off")) : w.hostSub.rfind("file:", 0) == 0 ? w.hostSub.substr(5) : "";
        if (hs.empty()) {  // "track:n:lang"
            int k = atoi(w.hostSub.c_str() + 6);
            size_t c = w.hostSub.find(':', 6);
            std::string lang = c == std::string::npos ? "" : LangName(w.hostSub.substr(c + 1));
            hs = lang.empty() ? TF("Subtitles %d", k) : lang;
        }
        if (ImGui::Selectable((TF("Host's subtitles (%s)", hs.c_str()) + "##hostsub").c_str(), follow, ImGuiSelectableFlags_DontClosePopups, ImVec2(0, h))) {
            watchSubFollow = true;
            watchSubApplied = "-";
            watchSubCheck = 0;
        }
    }
    bool anyOn = false;
    for (auto& t : tracks) anyOn |= t.kind == LSP_TRACK_SUBTITLE && t.selected;
    if (ImGui::Selectable(T("Off"), !anyOn && !follow, ImGuiSelectableFlags_DontClosePopups, ImVec2(0, h))) {
        Player().SelectTrack(LSP_TRACK_SUBTITLE, -1);
        watchSubFollow = false;
    }
    n = 0;
    for (auto& t : tracks) {
        if (t.kind != LSP_TRACK_SUBTITLE) continue;
        std::string label = TrackLabel(t, ++n) + (t.external ? std::string("  - ") + T("file") : "");
        ImGui::BeginDisabled(!t.playable);
        if (ImGui::Selectable((label + "##s" + std::to_string(t.id)).c_str(), t.selected && !follow, ImGuiSelectableFlags_DontClosePopups, ImVec2(0, h))) {
            Player().SelectTrack(LSP_TRACK_SUBTITLE, t.id);
            watchSubFollow = false;
        }
        ImGui::EndDisabled();
    }
    {  // the picture: brightness, contrast, colour... (this device only, remembered)
        ImGui::SeparatorText(T("Picture"));
        Settings st = eng.GetSettings();
        bool ch = false;
        float sw = mobile ? ImGui::GetContentRegionAvail().x : ImGui::GetFontSize() * 14;
        auto slider = [&](const char* id, const char* label, float* v, float lo, float hi, float def) {
            ImGui::SetNextItemWidth(sw);
            ch |= ImGui::SliderFloat(id, v, lo, hi, (std::string(label) + ": %.2f").c_str());
            if (ImGui::IsItemClicked(1) || (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))) {
                *v = def;
                ch = true;
            }
        };
        slider("##vbright", T("Brightness"), &st.videoBrightness, -0.5f, 0.5f, 0);
        slider("##vcontrast", T("Contrast"), &st.videoContrast, 0.5f, 2.0f, 1);
        slider("##vgamma", T("Gamma"), &st.videoGamma, 0.4f, 2.5f, 1);
        slider("##vsat", T("Saturation"), &st.videoSaturation, 0.0f, 2.0f, 1);
        slider("##vtemp", T("Colour temperature (cool - warm)"), &st.videoTemperature, -1.0f, 1.0f, 0);
        slider("##vtint", T("Tint (green - magenta)"), &st.videoTint, -1.0f, 1.0f, 0);
        bool def = st.videoBrightness == 0 && st.videoContrast == 1 && st.videoGamma == 1 && st.videoSaturation == 1 && st.videoTemperature == 0 &&
                   st.videoTint == 0;
        ImGui::BeginDisabled(def);
        if (ImGui::Button(T("Reset picture"), ImVec2(mobile ? -1 : 0, h))) {
            st.videoBrightness = st.videoTemperature = st.videoTint = 0;
            st.videoContrast = st.videoGamma = st.videoSaturation = 1;
            ch = true;
        }
        ImGui::EndDisabled();
        if (ch) eng.ApplySettings(st);
    }
    if (w.sid && w.host.empty() && w.libId.empty()) {  // my movie: may the others keep a copy?
        ImGui::Separator();
        bool b = w.allowSave;
        if (ImGui::Checkbox(T("Let the others save a copy"), &b)) eng.WatchSetAllowSave(b);
        if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", T("Remembered for the next movies too."));
    }
#ifdef _WIN32
    {  // full screen that Windows / the graphics driver don't treat as a game (their HDR for games stays off)
        ImGui::Separator();
        bool b = eng.settings.fullscreenBorderless;
        if (ImGui::Checkbox(T("Full screen without HDR (borderless window)"), &b)) {
            Settings st = eng.GetSettings();
            st.fullscreenBorderless = b;
            eng.ApplySettings(st);
            if (watchFullscreen && Bridge().setFullscreen) Bridge().setFullscreen(true, b);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("If the picture gets too bright or washed out in full screen (Auto HDR / RTX HDR):\nthe window then leaves one row of pixels at the bottom uncovered, so it isn't taken for a game."));
    }
#endif
    if (!mobile) {
        ImGui::Separator();
        if (ImGui::Selectable(T("Subtitles from a file..."), false, 0, ImVec2(0, h))) {
            browseFor = BrowseFor::WatchSubs;
            browser.Open(FileBrowser::Mode::OpenFile, T("Subtitles from a file"), browser.lastDir);
        }
    } else if (ImGui::Selectable(T("Subtitles from a file..."), false, 0, ImVec2(0, h))) {
        browseFor = BrowseFor::WatchSubs;
        browser.Open(FileBrowser::Mode::OpenFile, T("Subtitles from a file"), browser.lastDir);
    }
    ImGui::EndPopup();
}

// subtitles over the picture (fit: where the picture is drawn)
static void DrawSubtitles(ImDrawList* dl, ImVec2 fit0, ImVec2 fit1, float bottomLimit) {
    const auto& s = Player().Subtitle();
    float vh = fit1.y - fit0.y, vw = fit1.x - fit0.x;
    if (s.tex && s.sw > 0) {
        ImVec2 a(fit0.x + s.x * vw, fit0.y + s.y * vh), b(a.x + s.sw * vw, a.y + s.sh * vh);
        if (b.y > bottomLimit) {  // (above the controls when they show)
            float d = b.y - bottomLimit;
            a.y -= d;
            b.y -= d;
        }
        dl->AddImage((ImTextureID)(intptr_t)s.tex, a, b);
        return;
    }
    if (s.text.empty()) return;
    ImFont* f = ImGui::GetFont();
    float fs = std::clamp(vh / 15.0f, ImGui::GetFontSize() * 1.15f, ImGui::GetFontSize() * 3.2f);
    float wrap = vw * 0.9f;
    // line by line, centred, bottom up
    std::vector<std::string> lines;
    size_t a = 0;
    for (size_t i = 0; i <= s.text.size(); i++)
        if (i == s.text.size() || s.text[i] == '\n') {
            lines.push_back(s.text.substr(a, i - a));
            a = i + 1;
        }
    float y = std::min(fit1.y, bottomLimit) - fs * 0.6f;
    float outline = std::max(1.5f, fs / 14);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const char* txt = it->c_str();
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, wrap, txt);
        y -= ts.y;
        ImVec2 p((fit0.x + fit1.x - ts.x) * 0.5f, y);
        for (int k = 0; k < 8; k++) {
            float ang = k * 0.785398f;
            dl->AddText(f, fs, ImVec2(p.x + std::cos(ang) * outline, p.y + std::sin(ang) * outline), IM_COL32(0, 0, 0, 230), txt, nullptr, wrap);
        }
        dl->AddText(f, fs, p, IM_COL32(255, 255, 255, 255), txt, nullptr, wrap);
        y -= fs * 0.1f;
    }
}

// file names in a natural order ("Episode 2" before "Episode 10")
bool NaturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && a[i2] == '0') i2++;
            while (j2 < b.size() && b[j2] == '0') j2++;
            size_t ie = i2, je = j2;
            while (ie < a.size() && isdigit((unsigned char)a[ie])) ie++;
            while (je < b.size() && isdigit((unsigned char)b[je])) je++;
            if (ie - i2 != je - j2) return ie - i2 < je - j2;
            int c = a.compare(i2, ie - i2, b, j2, je - j2);
            if (c) return c < 0;
            i = ie;
            j = je;
            continue;
        }
        int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[j]);
        if (ca != cb) return ca < cb;
        i++;
        j++;
    }
    return a.size() - i < b.size() - j;
}

int App::WatchNeighbor(int dir) {
    const WatchInfo& w = eng.watch;
    if (!w.libId.empty()) {  // from a contact's Shared folder: the list it was started from
        int at = -1;
        for (int i = 0; i < (int)watchLibList.size(); i++)
            if (watchLibList[(size_t)i].id == w.libId) at = i;
        int n = at + dir;
        return at >= 0 && n >= 0 && n < (int)watchLibList.size() ? n : -1;
    }
    if (!w.host.empty() || w.path.empty()) return -1;  // (a viewer: the host chooses)
    std::string dir0 = DirName(w.path);
    if (dir0 != watchDirOf) {  // (the folder, once)
        watchDirOf = dir0;
        watchDirList.clear();
        for (auto& e : ListDir(dir0))
            if (!e.isDir && PlayerHost::IsVideoPath(e.name)) watchDirList.push_back(e.name);
        std::sort(watchDirList.begin(), watchDirList.end(), NaturalLess);
    }
    std::string me = BaseName(w.path);
    for (int i = 0; i < (int)watchDirList.size(); i++)
        if (watchDirList[(size_t)i] == me) {
            int n = i + dir;
            return n >= 0 && n < (int)watchDirList.size() ? n : -1;
        }
    return -1;
}

void App::WatchStep(int dir) {
    int n = WatchNeighbor(dir);
    if (n < 0) return;
    watchFailed = false;
    if (!eng.watch.libId.empty()) {
        eng.StartLibraryWatch(watchLibList[(size_t)n]);
        return;
    }
    eng.WatchSwitch(JoinPath(watchDirOf, watchDirList[(size_t)n]));
}

// the movie, full window: the picture fills it, the controls lie on top and hide after a few seconds
void App::DrawWatch() {
    const WatchInfo& w = eng.watch;
    if (!w.sid) {
        if (eng.WatchFollowing()) return;  // (the host went on to the next movie: it comes in a moment)
        watchShown = false;
        return;
    }
    bool mobile = ui::M().mobile;
    float bh = mobile ? ui::M().rowH : ImGui::GetFrameHeight() * 1.5f;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::Begin("##watch", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float sp = ImGui::GetStyle().ItemSpacing.x, pad = ImGui::GetStyle().WindowPadding.x + sp;
    int64_t now = SteadyMs();
    ImVec2 a = vp->Pos, b(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);

    // ---- show the controls on any movement / touch / key; hide them a few seconds later while it plays
    bool popupOpen = ImGui::IsPopupOpen("##watchtracks") || ImGui::IsPopupOpen("##watchadd");
    // were the controls already showing when the finger came down? (a touch moves the pointer in one frame and
    // clicks in the next: the move must not count as "visible" for the tap that follows)
    bool wereShown = now - watchActive < 3000 && now - watchShownSince > 350;
    if (ImGui::IsMouseClicked(0)) watchTapHides = wereShown;
    bool moved = !mobile && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0);  // (phones: only touches count)
    if (moved || ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || io.InputQueueCharacters.Size ||
        ImGui::IsKeyPressed(ImGuiKey_Space) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow) || popupOpen ||
        watchDrag >= 0 || !w.playing || w.hold)
        watchActive = now;
    bool controls = now - watchActive < 3000;
    if (controls && !watchCtlShown) watchShownSince = now;
    watchCtlShown = controls;
    bool tapToShow = !watchTapHides;  // (a tap on a hidden screen only brings the controls back)
    if (!controls && !mobile) ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    // ---- the picture
    ImGui::SetCursorScreenPos(a);
    ImGui::SetNextItemAllowOverlap();  // (the controls lie on top of it and take their own clicks)
    ImGui::InvisibleButton("##screen", ImVec2(b.x - a.x, b.y - a.y));
    bool tapped = ImGui::IsItemClicked() && controls && !tapToShow && wereShown;
    // the middle of the screen (with the controls showing): play / pause
    ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    float midR = std::max(bh * 1.1f, std::min(b.x - a.x, b.y - a.y) * 0.13f);
    ImVec2 mp = io.MousePos;
    bool tapMid = tapped && (mp.x - mid.x) * (mp.x - mid.x) + (mp.y - mid.y) * (mp.y - mid.y) < midR * midR;
    bool dbl = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
    std::string cur = !w.libId.empty() ? "lib:" + w.libId : w.host.empty() ? w.path : "watch:" + std::to_string(w.sid);
    bool open = Player().IsCurrent(cur);
    bool host = w.host.empty(), lib = !w.libId.empty();
    const auto& vt = Media().PlayerVideo();
    const LspInfo& inf = Player().Info();
    ImVec2 fit0 = a, fit1 = b;
    if (inf.width > 0 && inf.height > 0) {  // where the picture lands (kept in proportion)
        float sc = std::min((b.x - a.x) / inf.width, (b.y - a.y) / inf.height);
        float pw = inf.width * sc, ph = inf.height * sc;
        fit0 = ImVec2((a.x + b.x - pw) * 0.5f, (a.y + b.y - ph) * 0.5f);
        fit1 = ImVec2(fit0.x + pw, fit0.y + ph);
    }
    if (open && vt.valid && inf.has_video) {
        const Settings& st = eng.settings;
        float adj[8] = {st.videoBrightness, st.videoContrast, st.videoSaturation, st.videoGamma, st.videoTemperature, st.videoTint, 0, 0};
        Media().DrawVideo(dl, vt, a, b, false, 0, adj);
    } else {
        const char* msg = watchFailed ? T("This movie can't be played on this device.")
                        : !open ? T("Loading the movie...") : inf.has_video ? T("Loading...") : "";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), IM_COL32(220, 220, 220, 255), msg);
    }
    float barH = bh + ImGui::GetTextLineHeight() + sp * 3;
    if (open) DrawSubtitles(dl, fit0, fit1, controls ? b.y - barH - sp : b.y);
    if (open && w.hold) {  // a spinner while someone loads
        ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        float r = ImGui::GetFontSize() * 1.2f, t = (float)ImGui::GetTime() * 6;
        dl->AddCircleFilled(c, r * 1.5f, IM_COL32(0, 0, 0, 120));
        dl->PathArcTo(c, r, t, t + 4.2f, 24);
        dl->PathStroke(IM_COL32(255, 255, 255, 220), ImDrawFlags_None, 3.0f);
        if (!w.waitFor.empty()) {
            std::string m = TF("Waiting for %s...", w.waitFor.c_str());
            ImVec2 ts = ImGui::CalcTextSize(m.c_str());
            dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + r * 1.8f), IM_COL32(255, 220, 140, 255), m.c_str());
        }
    }
    bool fullscreenKey = !mobile && (ImGui::IsKeyPressed(ImGuiKey_F, false) || dbl);
    if (fullscreenKey && Bridge().setFullscreen) {
        watchFullscreen = !watchFullscreen;
        Bridge().setFullscreen(watchFullscreen, eng.settings.fullscreenBorderless);
    }

    double dur = open ? Player().Duration() : 0;
    double pos = w.Target(now);
    if (dur > 0) pos = std::min(pos, dur);
    bool playing = w.playing;
    bool toggle = (tapped && (!mobile || tapMid) && !dbl) || ImGui::IsKeyPressed(ImGuiKey_Space, false);
    if (controls && open && !w.hold) {  // the big play / pause in the middle
        dl->AddCircleFilled(mid, midR * 0.62f, IM_COL32(0, 0, 0, 110));
        ui::DrawIcon(dl, w.playing ? Icon::Pause : Icon::Play, mid, midR * 0.9f, IM_COL32(255, 255, 255, 230));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) eng.WatchControl(playing, std::max(0.0, pos - 10));
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) eng.WatchControl(playing, dur > 0 ? std::min(dur - 1, pos + 10) : pos + 10);

    if (controls) {  // ---- < and >: the previous / next movie in the folder
        float sz = bh * 1.3f;
        for (int d : {-1, 1}) {
            if (WatchNeighbor(d) < 0) continue;
            ImVec2 c(d < 0 ? a.x + pad + sz * 0.5f : b.x - pad - sz * 0.5f, (a.y + b.y) * 0.5f);
            ImGui::SetCursorScreenPos(ImVec2(c.x - sz * 0.5f, c.y - sz * 0.5f));
            ImGui::PushID(d);
            bool hit = ImGui::InvisibleButton("##wstep", ImVec2(sz, sz));
            bool hov = ImGui::IsItemHovered();
            if (hov) ImGui::SetTooltip("%s", d < 0 ? T("Previous movie in the folder") : T("Next movie in the folder"));
            ImGui::PopID();
            dl->AddCircleFilled(c, sz * 0.5f, IM_COL32(0, 0, 0, hov ? 170 : 125));
            float s2 = sz * 0.17f;
            ImVec2 pts[3] = {ImVec2(c.x + s2 * 0.5f * d * -1, c.y - s2), ImVec2(c.x + s2 * 0.5f * d, c.y), ImVec2(c.x + s2 * 0.5f * d * -1, c.y + s2)};
            dl->AddPolyline(pts, 3, IM_COL32(255, 255, 255, 230), 0, std::max(2.5f, sz * 0.06f));
            if (hit) WatchStep(d);
        }
    }
    if (controls) {
        // ---- top: back, title, who's watching, leave
        dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + bh * 2.2f), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + pad));
        bool back = ui::IconButton("wback", Icon::Back, T("Back to the chat (the movie goes on)"), bh);
        ImGui::SameLine();
        float leaveW = ImGui::CalcTextSize(w.host.empty() ? T("Stop") : T("Leave")).x + ImGui::GetStyle().FramePadding.x * 2 + (mobile ? bh * 0.4f : 0);
        ImGui::BeginGroup();
        ImGui::TextUnformatted(w.name.c_str());
        std::string who;
        for (auto& p : w.people) who += (who.empty() ? "" : ", ") + Emoji().Display(p);
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "%s", who.c_str());
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(b.x - pad - leaveW, a.y + pad));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 50, 50, 255));
        bool leave = ImGui::Button(w.host.empty() ? T("Stop") : T("Leave"), ImVec2(leaveW, bh));
        ImGui::PopStyleColor();
        // a video opened from the picture viewer (nobody added): back = done with it, and the viewer comes back
        bool fromViewer = watchFromViewer && eng.watchKeys.empty() && host;
        if (back && !fromViewer) watchShown = false;
        if (leave || (back && fromViewer)) {
            eng.LeaveWatch();
            watchShown = false;
        }

        // ---- bottom: play/pause, seek bar, time; sound & subtitles, full screen, call
        dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - barH * 1.6f), b, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190));
        float y0 = b.y - barH;
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y0));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 255, 255, 30));
        if (ui::IconButton("wplay", playing ? Icon::Pause : Icon::Play, playing ? T("Pause for everyone") : T("Play for everyone"), bh)) toggle = true;
        bool inCall = eng.call.state == CallState::Active;
        bool canAdd = host && !lib, canSave = ((!host && w.allowSave) || lib) && !w.saving;
        int right = 2 + (canAdd ? 1 : 0) + (canSave ? 1 : 0) + (Bridge().setFullscreen && !mobile ? 1 : 0) + (inCall ? 2 : 0);
        float rightW = (bh + sp) * (float)right;
        ImGui::SameLine();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float barW = std::max(40.0f, b.x - pad - rightW - p.x - sp);
        ImGui::InvisibleButton("wseek", ImVec2(barW, bh));
        float frac = dur > 0 ? (float)std::clamp(pos / dur, 0.0, 1.0) : 0;
        if (ImGui::IsItemActive() && dur > 0) {
            watchDrag = std::clamp((io.MousePos.x - p.x) / barW, 0.0f, 1.0f);
            frac = watchDrag;
        }
        if (ImGui::IsItemDeactivated() && dur > 0 && watchDrag >= 0) {
            eng.WatchControl(playing, watchDrag * dur);
            watchDrag = -1;
        }
        float cy = p.y + bh * 0.5f;
        dl->AddRectFilled(ImVec2(p.x, cy - 4), ImVec2(p.x + barW, cy + 4), IM_COL32(255, 255, 255, 60), 4);
        if (!host || lib) {  // how much of the movie is here
            float lf = (float)std::clamp(eng.WatchLoaded(), 0.0, 1.0);
            dl->AddRectFilled(ImVec2(p.x, cy + 2), ImVec2(p.x + barW * lf, cy + 4), IM_COL32(255, 255, 255, 110), 2);
        }
        dl->AddRectFilled(ImVec2(p.x, cy - 4), ImVec2(p.x + barW * frac, cy + 4), ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 4);
        dl->AddCircleFilled(ImVec2(p.x + barW * frac, cy), mobile ? 12.0f : 8.0f, ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
        ImGui::SameLine();
        bool lp = w.loop;
        if (lp) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 110, 200, 255));
        if (ui::IconButton("wloop", Icon::Loop, lp ? T("Loop: on (for everyone)") : T("Loop: off"), bh)) {
            eng.WatchSetLoop(!lp);
            if (host && eng.watchKeys.empty() && eng.settings.playerLoop == lp) ToggleLoop();  // (alone: remembered)
        }
        if (lp) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ui::IconButton("wtracks", Icon::Sliders, T("Sound, subtitles and picture"), bh)) ImGui::OpenPopup("##watchtracks");
        WatchTracksPopup();
        if (canSave) {
            ImGui::SameLine();
            if (ui::IconButton("wsave", Icon::Down, lib ? T("Save a copy (to your Shared folder)") : T("Save a copy (to Downloads)"), bh)) eng.WatchSave();
        }
        if (canAdd) {
            ImGui::SameLine();
            if (ui::IconButton("wadd", Icon::Plus, T("Add a person"), bh)) ImGui::OpenPopup("##watchadd");
            WatchAddPopup();
        }
        if (Bridge().setFullscreen && !mobile) {
            ImGui::SameLine();
            if (ui::IconButton("wfull", Icon::Expand, watchFullscreen ? T("Leave full screen (F)") : T("Full screen (F)"), bh)) {
                watchFullscreen = !watchFullscreen;
                Bridge().setFullscreen(watchFullscreen, eng.settings.fullscreenBorderless);
            }
        }
        if (inCall) {  // talking while watching: mute / hang up right here
            ImGui::SameLine();
            bool muted = Media().Muted();
            if (ui::IconButton("wmute", muted ? Icon::MicOff : Icon::Mic, muted ? T("Unmute") : T("Mute"), bh)) Media().SetMute(!muted);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(190, 50, 50, 255));
            if (ui::IconButton("whang", Icon::HangUp, T("Hang up"), bh)) eng.EndCall();
            ImGui::PopStyleColor();
        }
        ImGui::PopStyleColor();
        std::string time = Hms(watchDrag >= 0 ? watchDrag * dur : pos) + " / " + (dur > 0 ? Hms(dur) : std::string("--:--"));
        if (w.saving && eng.WatchLoaded() < 1) time += "     " + TF("saving: %d%%", (int)(eng.WatchLoaded() * 100));
        else if (w.saving) time += "     " + std::string(T("saved"));
        else if (!host || lib) time += "     " + TF("%d%% here", (int)(eng.WatchLoaded() * 100));
        dl->AddText(ImVec2(p.x, p.y + bh + sp * 0.5f), IM_COL32(235, 235, 235, 255), time.c_str());
    }
    if (toggle) eng.WatchControl(!playing, -1);
    if (tapped && mobile && !tapMid) watchActive = 0;  // phones: a tap on the picture (not its middle) hides the controls again
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && watchFullscreen && Bridge().setFullscreen) {
        watchFullscreen = false;
        Bridge().setFullscreen(false, false);
    } else if (ImGui::IsKeyPressed(ImGuiKey_AppBack) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !popupOpen)) {
        if (watchFromViewer && eng.watchKeys.empty() && w.host.empty()) eng.LeaveWatch();
        watchShown = false;
    }
    if (!watchShown && watchFromViewer) {  // back to the pictures
        watchFromViewer = false;
        if (!viewer.paths.empty()) {
            viewer.open = true;
            viewer.requestOpen = true;
        }
    }
    ImGui::End();
}
