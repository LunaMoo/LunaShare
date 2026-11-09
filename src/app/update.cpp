// Is there a newer LunaShare? Once a day (an option) the latest release on GitHub is looked up; a newer one is shown
// once, with a button to its page (the downloads are there). Nothing is downloaded or installed by itself.
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include <SDL.h>

#include "bluetooth.h"
#include "i18n.h"
#include "imgui.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using json = nlohmann::json;

static const char* kLatestUrl = "https://api.github.com/repos/LunaMoo/LunaShare/releases/latest";
static const char* kReleasesUrl = "https://github.com/LunaMoo/LunaShare/releases";

namespace {
std::mutex g_m;
bool g_busy = false, g_done = false, g_ok = false;
std::string g_tag, g_url, g_notes;
std::thread g_th;
}  // namespace

static bool HttpsGet(const std::string& url, std::string& body) {
    if (Bridge().httpsGet) return Bridge().httpsGet(url, body);
#ifdef _WIN32
    // https://host/path
    std::string rest = url.substr(8);
    size_t slash = rest.find('/');
    std::wstring host(rest.begin(), rest.begin() + (long)slash), path(rest.begin() + (long)slash, rest.end());
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"LunaShare", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"LunaShare", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 15000);
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        if (code == 200) {
            ok = true;
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
                std::string buf(avail, '\0');
                if (!WinHttpReadData(r, buf.data(), avail, &got) || got == 0) break;
                body.append(buf.data(), got);
                if (body.size() > (1u << 20)) break;
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
#elif defined(__ANDROID__)
    return false;
#else
    // Linux / macOS: curl is nearly always there
    std::string cmd = "curl -fsSL --max-time 20 -H 'User-Agent: LunaShare' -H 'Accept: application/vnd.github+json' '" + url + "' 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0 && body.size() < (1u << 20)) body.append(buf, n);
    return pclose(p) == 0 && !body.empty();
#endif
}

// the release notes are Markdown, wrapped by hand: plain text with the wrapped lines joined again
static std::string PlainNotes(const std::string& md) {
    std::string out, line;
    auto flush = [&](std::string l) {
        size_t a = l.find_first_not_of(" \t");
        bool cont = a != std::string::npos && !out.empty() && out.back() != '\n' && l.compare(a, 2, "- ") != 0 && l[a] != '#' &&
                    l.compare(a, 2, "* ") != 0;
        if (a == std::string::npos) {  // an empty line
            if (!out.empty() && out.back() != '\n') out += '\n';
            out += '\n';
            return;
        }
        if (cont) {
            out += ' ';
            out += l.substr(a);
        } else {
            if (!out.empty() && out.back() != '\n') out += '\n';
            if (l[a] == '#') l = l.substr(l.find_first_not_of("# ", a));
            out += l;
        }
    };
    for (size_t i = 0; i <= md.size(); i++) {
        char c = i < md.size() ? md[i] : '\n';
        if (c == '\r') continue;
        if (c == '\n') {
            flush(line);
            line.clear();
        } else {
            line += c;
        }
    }
    std::string r;  // (no ** or `)
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == '`') continue;
        if (out[i] == '*' && i + 1 < out.size() && out[i + 1] == '*') {
            i++;
            continue;
        }
        r += out[i];
    }
    while (!r.empty() && (r.back() == '\n' || r.back() == ' ')) r.pop_back();
    size_t s = r.find_first_not_of("\n ");
    return s == std::string::npos ? std::string() : r.substr(s);
}

// "v1.5.10" -> {1, 5, 10}; a newer one has a bigger number at the first place they differ
static std::vector<int> VersionParts(const std::string& v) {
    std::vector<int> r;
    int cur = -1;
    for (char c : v) {
        if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
        else if (c == '.') {
            r.push_back(std::max(cur, 0));
            cur = -1;
        } else if (c == '-' || c == '+') {
            break;
        }
    }
    if (cur >= 0) r.push_back(cur);
    return r;
}

bool UpdateIsNewer(const std::string& tag, const std::string& mine) {
    auto a = VersionParts(tag), b = VersionParts(mine);
    for (size_t i = 0; i < std::max(a.size(), b.size()); i++) {
        int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

void App::UpdateCheckNow() {
    std::lock_guard<std::mutex> l(g_m);
    if (g_busy) return;
    if (g_th.joinable()) g_th.join();
    g_busy = true;
    g_done = false;
    eng.SetUpdateState(NowMs(), eng.settings.updateSeen);
    g_th = std::thread([] {
        std::string body, tag, url, notes;
        bool ok = HttpsGet(kLatestUrl, body);
        if (ok) {
            json j = json::parse(body, nullptr, false);
            ok = j.is_object() && j.contains("tag_name");
            if (ok) {
                tag = j.value("tag_name", "");
                url = j.value("html_url", std::string(kReleasesUrl));
                notes = PlainNotes(j.value("body", ""));
                if (notes.size() > 3000) notes = notes.substr(0, 3000) + "...";
            }
        }
        std::lock_guard<std::mutex> l(g_m);
        g_busy = false;
        g_done = true;
        g_ok = ok;
        if (ok) {
            g_tag = tag;
            g_url = url;
            g_notes = notes;
        }
    });
}

static const char* MyVersion() {
    static const char* fake = getenv("LUNASHARE_FAKEVER");  // dev: pretend to be an older version (screenshots)
    return fake ? fake : LUNASHARE_VERSION;
}

void App::UpdateTick() {
    static bool devAsk = getenv("LUNASHARE_FAKEVER") != nullptr;
    if (devAsk && ImGui::GetFrameCount() > 5) {
        devAsk = false;
        updateAsked = true;
        UpdateCheckNow();
    }
    // a day after the last time (and not in the first seconds: the connections come first)
    if (eng.settings.updateCheck && NowMs() - eng.settings.updateLast > 24 * 3600000LL && ImGui::GetTime() > 20) UpdateCheckNow();
    std::lock_guard<std::mutex> l(g_m);
    if (!g_done) return;
    g_done = false;
    if (!g_ok) {
        if (updateAsked) {  // (asked by hand: say it didn't work)
            infoMessage = T("Couldn't check for a new version (no internet?).");
            openInfo = true;
        }
        updateAsked = false;
        return;
    }
    updateTag = g_tag;
    updateUrl = g_url;
    updateNotes = g_notes;
    bool newer = UpdateIsNewer(g_tag, MyVersion());
    if (newer && (g_tag != eng.settings.updateSeen || updateAsked)) {
        updateOpen = true;
        eng.SetUpdateState(eng.settings.updateLast, g_tag);
    } else if (!newer && updateAsked) {
        infoMessage = TF("You have the newest version (%s).", MyVersion());
        openInfo = true;
    }
    updateAsked = false;
}

void App::DrawUpdatePopup() {
    if (updateOpen) {
        ImGui::OpenPopup("##update");
        updateOpen = false;
    }
    bool mobile = ui::M().mobile;
    float h = mobile ? ui::M().rowH * 0.8f : 0;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(mobile ? vp->WorkSize.x * 0.92f : ImGui::GetFontSize() * 26, 0),
                                        ImVec2(mobile ? vp->WorkSize.x * 0.92f : ImGui::GetFontSize() * 34, vp->WorkSize.y * 0.85f));
    if (!ImGui::BeginPopupModal("##update", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) return;
    std::string v = updateTag;
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v = v.substr(1);
    ImGui::TextUnformatted(TF("LunaShare %s is available", v.c_str()).c_str());
    ImGui::TextDisabled(T("You have %s."), MyVersion());
    if (!updateNotes.empty()) {
        ImGui::Separator();
        ImGui::BeginChild("##notes", ImVec2(0, std::min(ImGui::GetTextLineHeightWithSpacing() * 12, vp->WorkSize.y * 0.45f)), ImGuiChildFlags_None);
        if (mobile) ui::TouchScroll();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(updateNotes.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    }
    ImGui::Separator();
    float bw = mobile ? -1 : 0;
    if (ui::IconTextButton(T("Open the download page"), ui::Icon::Down, ImVec2(bw, h))) {
        SDL_OpenURL(updateUrl.c_str());
        ImGui::CloseCurrentPopup();
    }
    if (!mobile) ImGui::SameLine();
    if (ImGui::Button(T("Later"), ImVec2(bw, h))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void UpdateShutdown() {
    std::thread t;
    {
        std::lock_guard<std::mutex> l(g_m);
        t = std::move(g_th);
    }
    if (t.joinable()) t.detach();  // (a slow network mustn't hold up closing)
}
