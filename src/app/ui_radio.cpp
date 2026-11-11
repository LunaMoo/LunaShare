// LunaShare - the Radio view of Music: stations from the Radio Browser directory (popular where I am, a search,
// genres), my favourites and the ones played lately, a station by its address. A tap plays it; the music player
// takes it from there (the notification, the lock screen and the mini player show the station and the song).
#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "image_cache.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "music.h"
#include "radio.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using ui::Icon;

namespace {
// a station's logo (or a little radio drawn instead)
void Logo(ImDrawList* dl, const std::string& path, ImVec2 a, float size) {
    ImVec2 b(a.x + size, a.y + size);
    const ImageTex* t = path.empty() ? nullptr : &Images().Get(path, ImageCache::Size::Tile);
    if (t && t->state == ImageTex::Ready && t->w > 0 && t->h > 0) {
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 235), 6);  // (logos are drawn for light backgrounds)
        float f = std::min(size / (float)t->w, size / (float)t->h) * 0.9f;
        float w = (float)t->w * f, h = (float)t->h * f;
        ImVec2 o(a.x + (size - w) * 0.5f, a.y + (size - h) * 0.5f);
        dl->AddImage(t->Id(), o, ImVec2(o.x + w, o.y + h));
        return;
    }
    dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), 6);
    ImU32 c = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    float s = size;
    // a radio: a box with a speaker and an aerial
    ImVec2 p0(a.x + s * 0.18f, a.y + s * 0.38f), p1(a.x + s * 0.82f, a.y + s * 0.80f);
    dl->AddRect(p0, p1, c, s * 0.06f, 0, std::max(1.5f, s * 0.05f));
    dl->AddCircle(ImVec2(a.x + s * 0.37f, a.y + s * 0.59f), s * 0.11f, c, 16, std::max(1.5f, s * 0.05f));
    dl->AddLine(ImVec2(a.x + s * 0.56f, a.y + s * 0.52f), ImVec2(a.x + s * 0.72f, a.y + s * 0.52f), c, std::max(1.5f, s * 0.04f));
    dl->AddLine(ImVec2(a.x + s * 0.56f, a.y + s * 0.64f), ImVec2(a.x + s * 0.72f, a.y + s * 0.64f), c, std::max(1.5f, s * 0.04f));
    dl->AddLine(ImVec2(a.x + s * 0.30f, a.y + s * 0.38f), ImVec2(a.x + s * 0.62f, a.y + s * 0.16f), c, std::max(1.5f, s * 0.05f));
}

std::string Sub(const radio::Station& s) {
    std::string t;
    auto add = [&](const std::string& x) {
        if (!x.empty()) t += (t.empty() ? "" : "  ·  ") + x;
    };
    add(s.country);
    std::string tags = s.tags;  // (the first three)
    int commas = 0;
    for (size_t i = 0; i < tags.size(); i++)
        if (tags[i] == ',' && ++commas == 3) {
            tags.resize(i);
            break;
        }
    for (auto& c : tags)
        if (c == ',') c = '/';
    add(tags);
    std::string q = s.codec;
    if (s.bitrate > 0) q += (q.empty() ? "" : " ") + std::to_string(s.bitrate) + " kbps";
    if (s.hls) q += q.empty() ? "HLS" : " HLS";
    add(q);
    return t;
}
}  // namespace

// i18n-keys: "Pop", "Rock", "Dance", "Jazz", "Classical", "News", "Talk", "Chillout", "Hip hop", "Metal", "Lofi", "80s", "Christmas"
void App::DrawRadio() {
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    radio::Radio& rd = radio::R();
    if (radioMode < 0) {  // the first time: my favourites if there are any, else what's popular here
        radioMode = rd.Favourites().empty() ? 0 : 1;
        if (radioMode == 0) rd.Popular();
    }
    // search
    ImGui::SetNextItemWidth(mobile ? ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(T("Search")).x - ImGui::GetStyle().FramePadding.x * 2 -
                                         ImGui::GetStyle().ItemSpacing.x
                                   : ImGui::GetFontSize() * 18);
    bool go = ImGui::InputTextWithHint("##rsearch", T("a station's name"), &radioSearch, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(T("Search")) || go) {  // (as tall as the text box next to it)
        if (!radioSearch.empty()) {
            radioMode = 3;
            rd.Search(radioSearch);
        }
    }
    if (!mobile) {
        ImGui::SameLine();
        if (ImGui::Button(T("Add by address..."), ImVec2(0, h))) radioAddOpen = true;
    }
    // what to show: favourites / popular here / lately / genres
    std::string cc = rd.CountryCode();
    std::string here = cc.empty() ? std::string(T("Popular everywhere")) : TF("Popular in %s", cc.c_str());
    struct Chip {
        int mode;
        const char* label;
        const char* tag;
    };
    static const Chip chips[] = {{1, "Favourites", nullptr}, {0, nullptr, nullptr},   {2, "Played lately", nullptr}, {4, "Pop", "pop"},
                                 {4, "Rock", "rock"},         {4, "Dance", "dance"},  {4, "Jazz", "jazz"},           {4, "Classical", "classical"},
                                 {4, "News", "news"},         {4, "Talk", "talk"},    {4, "Chillout", "chillout"},   {4, "Hip hop", "hip hop"},
                                 {4, "Metal", "metal"},       {4, "Lofi", "lofi"},    {4, "80s", "80s"},             {4, "Christmas", "christmas"}};
    float avail = ImGui::GetContentRegionAvail().x, x = 0, sp = ImGui::GetStyle().ItemSpacing.x;
    bool genreOpen = false;
    for (auto& c : chips) {
        if (mobile && c.mode == 4) {  // phones: the genres in a list (as chips they'd take half the screen)
            std::string label = radioMode == 4 ? std::string(T(chips[0].label)) : std::string();
            for (auto& g : chips)
                if (g.mode == 4 && radioMode == 4 && radioGenre == g.tag) label = T(g.label);
            if (radioMode != 4) label = T("Genres...");
            float w = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
            if (x > 0 && x + w <= avail) ImGui::SameLine();
            else x = 0;
            x += w + sp;
            ui::PushAccent(radioMode == 4);
            if (ImGui::Button((label + "##rgen").c_str(), ImVec2(0, h))) genreOpen = true;
            ui::PopAccent();
            break;
        }
        std::string label = c.mode == 0 ? here : T(c.label);
        float w = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        if (x > 0 && x + w <= avail) ImGui::SameLine();
        else x = 0;
        x += w + sp;
        bool on = radioMode == c.mode && (c.mode != 4 || radioGenre == c.tag);
        ui::PushAccent(on);
        if (ImGui::Button(label.c_str(), ImVec2(0, h))) {
            radioMode = c.mode;
            if (c.mode == 0) rd.Popular();
            if (c.mode == 4) {
                radioGenre = c.tag;
                rd.Genre(c.tag);
            }
        }
        ui::PopAccent();
    }
    if (genreOpen) ImGui::OpenPopup("##rgenres");
    if (ImGui::BeginPopup("##rgenres")) {
        for (auto& g : chips) {
            if (g.mode != 4) continue;
            if (ImGui::Selectable(T(g.label), radioMode == 4 && radioGenre == g.tag, 0, ImVec2(ImGui::GetFontSize() * 9, h))) {
                radioMode = 4;
                radioGenre = g.tag;
                rd.Genre(g.tag);
            }
        }
        ImGui::EndPopup();
    }
    {
        std::string cl = std::string(T("Country...")) + "##rc";
        float w = ImGui::CalcTextSize(T("Country...")).x + ImGui::GetStyle().FramePadding.x * 2;
        if (x > 0 && x + w <= avail) ImGui::SameLine();
        else x = 0;
        x += w + sp;
    }
    if (ImGui::Button((std::string(T("Country...")) + "##rc").c_str(), ImVec2(0, h))) {
        radioCountryOpen = true;
        radioCountryFilter.clear();
    }
    if (mobile) {
        float w = ImGui::CalcTextSize(T("Add by address...")).x + ImGui::GetStyle().FramePadding.x * 2;
        if (x > 0 && x + w <= avail) ImGui::SameLine();
        if (ImGui::Button(T("Add by address..."), ImVec2(0, h))) radioAddOpen = true;
    }
    ImGui::Separator();

    // the list
    std::vector<radio::Station> list;
    bool remote = radioMode == 0 || radioMode >= 3;
    if (radioMode == 1) list = rd.Favourites();
    else if (radioMode == 2) list = rd.Recent();
    else list = rd.Results();
    if (rd.Version() != radioSeen) radioSeen = rd.Version();

    ImGui::BeginChild("rlist");
    if (mobile) ui::TouchScroll();
    if (remote && rd.Busy()) {
        ImGui::TextDisabled("%s", T("Looking for stations..."));
        busy = true;
    } else if (remote && !rd.Error().empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1), "%s", T("Can't reach the radio directory - is the device online?"));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(T("Try again"), ImVec2(0, h))) {
            if (radioMode == 0) rd.Popular();
            else if (radioMode == 3) rd.Search(radioSearch);
            else rd.Genre(radioGenre);
        }
    } else if (list.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", radioMode == 1   ? T("No favourites yet: tap the star next to a station.")
                                  : radioMode == 2 ? T("Nothing played yet.")
                                                   : T("No stations found."));
        ImGui::PopTextWrapPos();
    }
    float rowH = mobile ? ui::M().rowH * 1.05f : ImGui::GetTextLineHeight() * 2.6f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < list.size() && !rd.Busy(); i++) {
        const radio::Station& s = list[i];
        ImGui::PushID((int)i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, star = rowH * 0.8f;
        bool on = rd.Playing(s.id);
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::Selectable("##st", on, ImGuiSelectableFlags_AllowOverlap, ImVec2(w - star - sp, rowH))) {
            if (on) music::Play().Toggle();
            else rd.Play(s);
        }
        if (ImGui::IsItemHovered() && !mobile && !s.homepage.empty()) ImGui::SetTooltip("%s", s.homepage.c_str());
        float lg = rowH - 8;
        Logo(dl, rd.LogoFile(s), ImVec2(p.x + 4, p.y + 4), lg);
        float tx = p.x + lg + 14, lh = ImGui::GetTextLineHeight();
        ImVec4 clip(tx, p.y, p.x + w - star - sp * 2, p.y + rowH);
        dl->AddText(nullptr, 0, ImVec2(tx, p.y + rowH * 0.5f - lh - 1), ImGui::GetColorU32(ImGuiCol_Text), s.name.c_str(), nullptr, 0, &clip);
        std::string sub;
        ImU32 subCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        if (on) {  // the one on: what's playing on it
            music::Now n = music::Play().GetNow();
            std::string t = rd.Title();
            sub = n.loading ? std::string(n.retries > 0 ? T("can't reach the station - trying again...") : T("connecting..."))
                : !n.playing ? std::string(T("paused"))
                : t.empty()  ? std::string(T("on air"))
                             : "\xE2\x99\xAA " + t;
            subCol = ui::ColAccentText();
            busy = true;
        } else {
            sub = Sub(s);
        }
        dl->AddText(nullptr, 0, ImVec2(tx, p.y + rowH * 0.5f + 1), subCol, sub.c_str(), nullptr, 0, &clip);
        // the star: a favourite
        bool fav = rd.IsFavourite(s.id);
        ImGui::SameLine(w - star);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - star, p.y + (rowH - star) * 0.5f));
        ImVec2 sp0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##fav", ImVec2(star, star))) rd.SetFavourite(s, !fav);
        if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", fav ? T("Remove from favourites") : T("Add to favourites"));
        ui::DrawIcon(dl, Icon::Star, ImVec2(sp0.x + star * 0.5f, sp0.y + star * 0.5f), star * 0.55f,
                     fav ? IM_COL32(240, 200, 60, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    }
    if (!list.empty() && remote) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("Stations from radio-browser.info"));
    }
    ImGui::EndChild();

    // ---- the country popular stations come from
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (radioCountryOpen) {
        ImGui::OpenPopup("##rcountry");
        radioCountryOpen = false;
    }
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(mobile ? vp->WorkSize.x * 0.94f : ImGui::GetFontSize() * 22, vp->WorkSize.y * 0.7f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##rcountry", nullptr, ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("Popular stations from"));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##rcf", T("search"), &radioCountryFilter);
        auto countries = rd.Countries();
        ImGui::BeginChild("rcl", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - h));
        if (mobile) ui::TouchScroll();
        if (ImGui::Selectable(T("Everywhere"), cc.empty(), 0, ImVec2(0, h))) {
            rd.SetCountry("");
            radioMode = 0;
            rd.Popular();
            ImGui::CloseCurrentPopup();
        }
        if (countries.empty()) ImGui::TextDisabled("%s", T("Loading..."));
        std::string f = radioCountryFilter;
        for (auto& ch : f) ch = (char)tolower((unsigned char)ch);
        for (auto& c : countries) {
            std::string n = c.name;
            for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
            if (!f.empty() && n.find(f) == std::string::npos && c.code.size() == 2 && f != std::string{(char)tolower(c.code[0]), (char)tolower(c.code[1])}) continue;
            std::string label = c.name + "  (" + std::to_string(c.stations) + ")##" + c.code;
            if (ImGui::Selectable(label.c_str(), c.code == cc, 0, ImVec2(0, h))) {
                rd.SetCountry(c.code);
                radioMode = 0;
                rd.Popular();
                ImGui::CloseCurrentPopup();
            }
        }
        if (countries.empty()) busy = true;
        ImGui::EndChild();
        if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- a station by its address
    if (radioAddOpen) {
        ImGui::OpenPopup("##radd");
        radioAddOpen = false;
        radioUrlBuf.clear();
        radioNameBuf.clear();
    }
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (mobile) ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.94f, 0));
    if (ImGui::BeginPopupModal("##radd", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T("A station by its address"));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (mobile ? ImGui::GetContentRegionAvail().x : ImGui::GetFontSize() * 24));
        ImGui::TextDisabled("%s", T("A stream (http / https), a playlist (.m3u, .pls) or an HLS address (.m3u8). It's kept with your favourites."));
        ImGui::PopTextWrapPos();
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 24);
        ImGui::InputTextWithHint("##rurl", "https://...", &radioUrlBuf);
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetFontSize() * 24);
        ImGui::InputTextWithHint("##rname", T("name (optional)"), &radioNameBuf);
        bool okUrl = radioUrlBuf.compare(0, 7, "http://") == 0 || radioUrlBuf.compare(0, 8, "https://") == 0;
        ImGui::BeginDisabled(!okUrl);
        if (ImGui::Button(T("Add and play"), ImVec2(mobile ? ImGui::GetContentRegionAvail().x * 0.5f : 0, h))) {
            radio::Station s = rd.AddByUrl(radioUrlBuf, radioNameBuf);
            rd.Play(s);
            radioMode = 1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(mobile ? -1 : 0, h))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
