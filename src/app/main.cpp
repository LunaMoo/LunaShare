// LunaShare - file sharing chat for LAN, phone hotspots and Bluetooth.
// SDL2 + OpenGL3/GLES3 + Dear ImGui.
#include "notify.h"
#include "stb_image.h"
#include "tray.h"
#include <algorithm>
#include <SDL.h>
#include <ctime>
#include <filesystem>

#include <cstdio>
#include <string>
#include <vector>

#include "engine.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "imgui_internal.h"  // (ClearActiveID: Back closes the keyboard and the text field lets go)
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"
#include "media_host.h"
#include "player_host.h"
#include "music.h"
#include "radio.h"
#include "emoji_host.h"
#include "gl_funcs.h"
#include "image_cache.h"
#include "file_assoc.h"
#include "instance.h"
#include "torrent_host.h"
#include "core.h"

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

void AndroidBridgeInit(ls::Engine* engine);
void AndroidAddSystemTextFont(ImFontAtlas* atlas);  // android_bridge.cpp: characters our fonts lack  // android_bridge.cpp (no-op elsewhere)
void AndroidBridgeShutdown();
std::string AndroidDeviceName();

#if defined(__ANDROID__)
// The phone's fallback fonts for other scripts, from its own list (/system/etc/fonts.xml, font_fallback.xml on
// newer Android): file names differ between makers and versions (NotoSansCJK-Regular.ttc, NotoSansCJKjp..., ...).
// Returns (path, index in a .ttc) for Japanese, Chinese, Korean, Thai, Hindi, Hebrew, Arabic and a few more.
static std::vector<std::pair<std::string, int>> AndroidFallbackFonts() {
    std::vector<std::pair<std::string, int>> out;
    std::string xml;
    for (const char* f : {"/system/etc/font_fallback.xml", "/system/etc/fonts.xml"}) {
        FILE* fp = fopen(f, "rb");
        if (!fp) continue;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) xml.append(buf, n);
        fclose(fp);
        if (!xml.empty()) break;
    }
    // language tag | script tag (fonts.xml uses either: lang="ja" or lang="und-Thai")
    const char* wanted[][2] = {{"ja", "Jpan"}, {"zh-Hans", "Hans"}, {"zh-Hant", "Hant"}, {"ko", "Kore"}, {"th", "Thai"}, {"hi", "Deva"},
                               {"he", "Hebr"}, {"ar", "Arab"}, {"bn", "Beng"}, {"ta", "Taml"}, {"ka", "Geor"}, {"hy", "Armn"},
                               {"am", "Ethi"}, {"km", "Khmr"}, {"my", "Mymr"}, {nullptr, nullptr}};
    bool used[16] = {};
    std::vector<std::pair<int, std::pair<std::string, int>>> found;
    size_t at = 0;
    while ((at = xml.find("<family", at)) != std::string::npos) {
        size_t end = xml.find("</family>", at);
        if (end == std::string::npos) break;
        std::string fam = xml.substr(at, end - at);
        at = end;
        size_t lp = fam.find("lang=\"");
        if (lp == std::string::npos || fam.find("variant=\"compact\"") != std::string::npos) continue;
        std::string lang = fam.substr(lp + 6, fam.find('"', lp + 6) - lp - 6);
        int which = -1;
        for (int i = 0; wanted[i][0] && which < 0; i++) {
            std::string w = wanted[i][0];  // "ja", "zh-Hans,zh-Bopo", "und-Thai"...
            if (lang == w || lang.rfind(w + ",", 0) == 0 || lang.find("," + w) != std::string::npos || lang.find(wanted[i][1]) != std::string::npos)
                which = i;
        }
        if (which < 0 || used[which]) continue;
        // the regular-weight font of the family: <font weight="400" ... index="N">File.ttc ...</font>
        size_t fp = 0;
        std::string file;
        int index = 0;
        while ((fp = fam.find("<font", fp)) != std::string::npos) {
            size_t close = fam.find('>', fp);
            if (close == std::string::npos) break;
            std::string tag = fam.substr(fp, close - fp);
            size_t nameEnd = fam.find_first_of("<", close + 1);
            std::string name = fam.substr(close + 1, nameEnd - close - 1);
            size_t a = name.find_first_not_of(" \t\r\n"), b = name.find_last_not_of(" \t\r\n");
            name = a == std::string::npos ? "" : name.substr(a, b - a + 1);
            fp = close;
            if (name.empty() || tag.find("style=\"italic\"") != std::string::npos || tag.find("fallbackFor=") != std::string::npos) continue;
            bool regular = tag.find("weight=\"400\"") != std::string::npos;
            if (file.empty() || regular) {
                file = name;
                size_t ip = tag.find("index=\"");
                index = ip == std::string::npos ? 0 : atoi(tag.c_str() + ip + 7);
            }
            if (regular) break;
        }
        if (file.empty()) continue;
        std::string path = file[0] == '/' ? file : "/system/fonts/" + file;
        if (!ls::FileExists(path)) continue;
        used[which] = true;
        found.push_back({which, {path, index}});
    }
    // Japanese first (Han characters then take Japanese shapes), then the others in the order above
    std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& f : found) out.push_back(f.second);
    return out;
}
#endif

// How tall a font's line is compared with its em (hhea ascent - descent over unitsPerEm). ImGui sizes every font by
// that line height, so a font with a tall line (Noto CJK: ~1.45 em, against ~1.17 for the main fonts) comes out
// smaller - Japanese, Chinese and Korean text looked a size too small next to Latin. Returns 0 when it can't tell.
static float FontLineToEm(const char* path, int fontNo) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    auto rd = [&](long at, int n) -> uint32_t {
        unsigned char b[4] = {};
        if (fseek(f, at, SEEK_SET) != 0 || fread(b, 1, (size_t)n, f) != (size_t)n) return 0;
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = (v << 8) | b[i];
        return v;
    };
    long base = 0;
    if (rd(0, 4) == 0x74746366) base = (long)rd(12 + 4 * fontNo, 4);  // 'ttcf': the font's own offset table
    int tables = (int)rd(base + 4, 2);
    long head = 0, hhea = 0;
    for (int i = 0; i < tables && i < 64; i++) {
        uint32_t tag = rd(base + 12 + 16 * i, 4), off = rd(base + 12 + 16 * i + 8, 4);
        if (tag == 0x68656164) head = (long)off;  // 'head'
        if (tag == 0x68686561) hhea = (long)off;  // 'hhea'
    }
    float r = 0;
    if (head && hhea) {
        uint32_t upem = rd(head + 18, 2);
        int asc = (int16_t)rd(hhea + 4, 2), desc = (int16_t)rd(hhea + 6, 2);
        if (upem > 0 && asc - desc > 0) r = (float)(asc - desc) / (float)upem;
    }
    fclose(f);
    return r;
}

static void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const char* main[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__ANDROID__)
        "/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc", "/Library/Fonts/Arial Unicode.ttf",
#else
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
        nullptr};
    // Fallback fonts merged in for symbols and CJK (glyphs load on demand, so this is cheap).
    const char* extra[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\seguisym.ttf", "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\malgun.ttf", "C:\\Windows\\Fonts\\LeelawUI.ttf", "C:\\Windows\\Fonts\\Nirmala.ttc",
#elif defined(__ANDROID__)
        "/system/fonts/NotoSansSymbols-Regular-Subsetted.ttf", "/system/fonts/NotoSansCJK-Regular.ttc",
#elif defined(__APPLE__)
        "/System/Library/Fonts/PingFang.ttc",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJKjp-Regular.otf", "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
#endif
        nullptr};
    ImFont* f = nullptr;
    float mainLine = 0;
    for (int i = 0; main[i] && !f; i++)
        if (ls::FileExists(main[i])) {
            f = io.Fonts->AddFontFromFileTTF(main[i]);
            if (f) mainLine = FontLineToEm(main[i], 0);
        }
    if (!f) {
        io.Fonts->AddFontDefaultVector();
        return;
    }
    // a merged font's letters drawn as big as the main font's (see FontLineToEm)
    auto sizeLike = [&](ImFontConfig& cfg, const char* path, int no) {
        float line = FontLineToEm(path, no);
        if (mainLine > 0 && line > 0) cfg.ExtraSizeScale = std::clamp(line / mainLine, 0.8f, 1.4f);
    };
    for (int i = 0; extra[i]; i++) {
        if (!ls::FileExists(extra[i])) continue;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        sizeLike(cfg, extra[i], 0);
        io.Fonts->AddFontFromFileTTF(extra[i], 0.0f, &cfg);
    }
#if defined(__ANDROID__)
    for (auto& [path, index] : AndroidFallbackFonts()) {
        if (path == "/system/fonts/NotoSansCJK-Regular.ttc" && index == 0) continue;  // (already in)
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.FontNo = index;
        sizeLike(cfg, path.c_str(), index);
        io.Fonts->AddFontFromFileTTF(path.c_str(), 0.0f, &cfg);
    }
#endif
}

static void ApplyStyle(int theme, float scale, bool mobile) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    switch (theme) {
        case 1: ImGui::StyleColorsLight(); break;
        case 2: ImGui::StyleColorsClassic(); break;
        default: ImGui::StyleColorsDark(); break;
    }
    // soft shapes, room to breathe, no hard frames
    style.WindowRounding = 10;
    style.ChildRounding = 10;
    style.FrameRounding = 8;
    style.PopupRounding = 10;
    style.GrabRounding = 8;
    style.TabRounding = 8;
    style.ScrollbarRounding = 8;
    style.ScrollbarSize = 11;
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(8, 7);
    style.ItemInnerSpacing = ImVec2(6, 5);
    style.WindowPadding = ImVec2(12, 10);
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.SeparatorTextBorderSize = 1;
    style.SeparatorTextPadding = ImVec2(12, 6);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    auto rgb = [](int hex, float a = 1.0f) { return ImVec4(((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a); };
    if (theme == 0 || theme == 1 || theme == 3 || theme == 4) {
        // one palette for all three: a background, two raised steps, text, and one accent (indigo; Twilight: rose)
        bool dark = theme != 1;
        ImVec4 bg = rgb(dark ? 0x15171C : 0xF4F5F8), raised = rgb(dark ? 0x1E2128 : 0xFFFFFF), frame = rgb(dark ? 0x272B34 : 0xE7E9EF),
               frameHi = rgb(dark ? 0x313642 : 0xDCDFE8), frameAct = rgb(dark ? 0x3A4050 : 0xCDD2E0), text = rgb(dark ? 0xE9EBF0 : 0x1B1D23),
               muted = rgb(dark ? 0x8E94A3 : 0x6A7080), line = rgb(dark ? 0x2C303A : 0xDADDE5), accent = rgb(dark ? 0x6474F0 : 0x4F5FE0),
               accentHi = rgb(dark ? 0x7685F5 : 0x6170EA), accentSoft = rgb(dark ? 0x6474F0 : 0x4F5FE0, dark ? 0.22f : 0.16f),
               accentText = rgb(dark ? 0x93A0FF : 0x4353D6);
        if (theme == 3) {
            // Twilight: a night sky going plum, sakura-rose accent. Text stays as bright as on Dark; the accent is deep
            // enough for white text on it (outgoing messages), with a light pink for accent-coloured text.
            bg = rgb(0x17142A);
            raised = rgb(0x231D38);
            frame = rgb(0x2B2442, 0.92f);
            frameHi = rgb(0x372E54, 0.95f);
            frameAct = rgb(0x433864);
            text = rgb(0xF2ECF8);
            muted = rgb(0xA79CC0);
            line = rgb(0x3A3152);
            accent = rgb(0xB8467C);
            accentHi = rgb(0xCB5A90);
            accentSoft = rgb(0xE07AA8, 0.24f);
            accentText = rgb(0xFF9EC8);
        }
        if (theme == 4) {
            // Moonlight: the same sky, colder - night blues, a moonlit blue accent (white text still reads on it)
            bg = rgb(0x0F1628);
            raised = rgb(0x172036);
            frame = rgb(0x1E2A44, 0.92f);
            frameHi = rgb(0x283754, 0.95f);
            frameAct = rgb(0x324466);
            text = rgb(0xEAF1FB);
            muted = rgb(0x93A3BF);
            line = rgb(0x2A3856);
            accent = rgb(0x3A6FC4);
            accentHi = rgb(0x4B82D6);
            accentSoft = rgb(0x6FA8F0, 0.22f);
            accentText = rgb(0x8EC5FF);
        }
        ImVec4* c = style.Colors;
        c[ImGuiCol_Text] = text;
        c[ImGuiCol_TextDisabled] = muted;
        c[ImGuiCol_WindowBg] = bg;
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = raised;
        c[ImGuiCol_Border] = line;
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = frame;
        c[ImGuiCol_FrameBgHovered] = frameHi;
        c[ImGuiCol_FrameBgActive] = frameAct;
        c[ImGuiCol_TitleBg] = raised;
        c[ImGuiCol_TitleBgActive] = raised;
        c[ImGuiCol_TitleBgCollapsed] = raised;
        c[ImGuiCol_MenuBarBg] = raised;
        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = frameHi;
        c[ImGuiCol_ScrollbarGrabHovered] = frameAct;
        c[ImGuiCol_ScrollbarGrabActive] = muted;
        c[ImGuiCol_CheckMark] = accentText;
        c[ImGuiCol_SliderGrab] = accent;
        c[ImGuiCol_SliderGrabActive] = accentHi;
        c[ImGuiCol_Button] = frame;
        c[ImGuiCol_ButtonHovered] = frameHi;
        c[ImGuiCol_ButtonActive] = accent;
        c[ImGuiCol_Header] = accentSoft;
        c[ImGuiCol_HeaderHovered] = frameHi;
        c[ImGuiCol_HeaderActive] = accent;
        c[ImGuiCol_Separator] = line;
        c[ImGuiCol_SeparatorHovered] = accent;
        c[ImGuiCol_SeparatorActive] = accentHi;
        c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered] = accentSoft;
        c[ImGuiCol_ResizeGripActive] = accent;
        c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TabHovered] = frameHi;
        c[ImGuiCol_TabSelected] = accentSoft;
        c[ImGuiCol_TabSelectedOverline] = accent;
        c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TabDimmedSelected] = accentSoft;
        c[ImGuiCol_PlotLines] = accent;
        c[ImGuiCol_PlotHistogram] = accent;
        c[ImGuiCol_TableHeaderBg] = frame;
        c[ImGuiCol_TableBorderStrong] = line;
        c[ImGuiCol_TableBorderLight] = line;
        c[ImGuiCol_TableRowBgAlt] = rgb(dark ? 0xFFFFFF : 0x000000, 0.03f);
        c[ImGuiCol_TextSelectedBg] = theme == 3 ? rgb(0xE07AA8, 0.40f) : theme == 4 ? rgb(0x6FA8F0, 0.40f) : rgb(dark ? 0x6474F0 : 0x4F5FE0, 0.40f);
        c[ImGuiCol_DragDropTarget] = accentHi;
        c[ImGuiCol_NavCursor] = accent;
        c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, dark ? 0.55f : 0.35f);
    }
    if (mobile) {
        style.FramePadding = ImVec2(13, 10);
        style.ItemSpacing = ImVec2(10, 10);
        style.FrameRounding = 12;
        style.ScrollbarSize = 6;  // drag to scroll instead
        style.TouchExtraPadding = ImVec2(4, 4);
        style.GrabMinSize = 28;
        style.WindowPadding = ImVec2(10, 10);
    }
    if (theme == 3 || theme == 4) {  // Twilight, Moonlight: softer, rounder
        style.FrameRounding += 3;
        style.ChildRounding = 14;
        style.PopupRounding = 14;
        style.WindowRounding = 14;
        style.GrabRounding = style.FrameRounding;
        style.PopupBorderSize = 1;
    }
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontSizeBase = mobile ? 17.0f : 16.0f;
}

static float DetectScale(SDL_Window* window) {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    float ddpi = 160, hdpi, vdpi;
    if (SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(window), &ddpi, &hdpi, &vdpi) != 0 || ddpi <= 0) ddpi = 320;
    float s = ddpi / 160.0f;
    // the phone layout needs ~540 logical pixels across: high-density phones (DPI/160 = 3 on a 1280 px wide screen)
    // would otherwise cut off buttons and text, so cap the scale by the screen's short side
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);  // pixels on Android
    int shortSide = std::min(ww, wh);
    if (shortSide > 0) s = std::min(s, (float)shortSide / 540.0f);
    return std::max(1.0f, s);
#else
    (void)window;
    float s = ImGui_ImplSDL2_GetContentScaleForDisplay(0);
    return s > 0 ? s : 1.0f;
#endif
}

#ifdef __ANDROID__
static bool g_kbOn = false;         // the on-screen keyboard was asked for
static Uint64 g_kbOffSince = 0;     // ... and no text field has wanted it since then (0 = one does)
#endif

#ifdef __ANDROID__
// The on-screen keyboard works on a copy of the active text field (LunaIme.java) and sends it back whole: text +
// selection. Here the field is made to match it - through ImGui's own input (the selection set, then the changed part
// typed over it / a Backspace), so the field's filters and callbacks (emoji glyphs) see it as typing. Whatever the field
// then holds that the copy doesn't (the app sent / cleared it, an emoji became a glyph, a tap moved the cursor,
// another field) goes back to the keyboard as a new generation; what the keyboard made on an older one is dropped.
static int Utf8Back(const std::string& s, int i) {
    while (i > 0 && i < (int)s.size() && ((unsigned char)s[(size_t)i] & 0xC0) == 0x80) i--;
    return i;
}

static bool ImeSync(App& app) {
    static ImGuiID imeId = 0;
    static int gen = 0;
    static std::string last;
    static int lastS = 0, lastE = 0;
    static bool applying = false;  // (an edit is queued in ImGui's input: wait for it to go through)
    static std::string apText;
    static int apS = 0, apE = 0, apEnters = 0;
    static bool apSelOnly = false;   // what's being applied changes only the cursor ...
    static int apFromS = 0, apFromE = 0;  // ... from where it was then
    static bool pend = false;
    static std::string pText;
    static int pS = 0, pE = 0, pEnters = 0;
    auto& br = ls::Bridge();
    int jg = 0, js = 0, je = 0, jn = 0;
    std::string jt;
    if (br.imeTake && br.imeTake(jg, jt, js, je, jn) && jg == gen) {
        pend = true;
        pText = std::move(jt);
        pS = js;
        pE = je;
        pEnters += jn;
    }
    ImGuiID id = ImGui::GetActiveID();
    ImGuiInputTextState* st = id ? ImGui::GetInputTextState(id) : nullptr;
    if (st && (st->Flags & ImGuiInputTextFlags_ReadOnly)) st = nullptr;
    if (!g_kbOn || !st) {  // (no field, even for a moment: the next one - the same one again too - starts over)
        imeId = 0;
        applying = pend = false;
        pEnters = 0;
        return false;
    }
    std::string cur(st->GetText(), (size_t)std::max(0, st->TextLen));
    int len = (int)cur.size();
    int cs = std::clamp(std::min(st->GetSelectionStart(), st->GetSelectionEnd()), 0, len);
    int ce = std::clamp(std::max(st->GetSelectionStart(), st->GetSelectionEnd()), 0, len);
    if (cs == ce) cs = ce = std::clamp(st->GetCursorPos(), 0, len);
    auto push = [&](bool restart) {
        gen++;
        last = cur;
        lastS = cs;
        lastE = ce;
        applying = pend = false;
        pEnters = apEnters = 0;
        if (br.imeSetState) br.imeSetState(cur, cs, ce, gen, restart);
    };
    ImGuiIO& io = ImGui::GetIO();
    if (id != imeId || app.imeReset) {
        imeId = id;
        app.imeReset = false;
        push(true);
        return false;
    }
    if (applying) {
        if (GImGui->InputEventsQueue.Size > 0) return true;
        applying = false;
        if (cur != apText) {  // (the field made it something else - or it was Enter, and the message went)
            push(true);
            return false;
        }
        if (apSelOnly && apEnters == 0 && (cs != apFromS || ce != apFromE)) {
            // the cursor was moved in the field meanwhile (a tap): that's newer than the keyboard's - it stands
            push(false);
            return false;
        }
        st->SetSelection(std::min(Utf8Back(cur, apS), len), std::min(Utf8Back(cur, apE), len));
        st->CursorFollow = true;
        st->CursorAnimReset();
        last = cur;
        lastS = std::min(apS, len);
        lastE = std::min(apE, len);
        if (apEnters > 0) {  // (Enter after the text it came with)
            for (; apEnters > 0; apEnters--) {
                io.AddKeyEvent(ImGuiKey_Enter, true);
                io.AddKeyEvent(ImGuiKey_Enter, false);
            }
            applying = true;
        }
        return true;
    }
    // The cursor was moved in the field itself (a tap, a drag) since the keyboard was last told, and what the keyboard
    // sent changes no text: it was sent before the keyboard knew of the move (finishing the word being typed, its
    // suggestions) and carries the old cursor - applied, the cursor jumped back after every tap. The field's place
    // stands, and the keyboard is told it. (Enter pressed with it still goes through.)
    if (pend && cur == last && (cs != lastS || ce != lastE) && pText == last && pEnters == 0) {
        pend = false;
        push(false);
        return false;
    }
    if (pend) {
        pend = false;
        apText = pText;
        apS = std::clamp(std::min(pS, pE), 0, (int)apText.size());
        apE = std::clamp(std::max(pS, pE), 0, (int)apText.size());
        apEnters = pEnters;
        pEnters = 0;
        apSelOnly = cur == apText;
        apFromS = cs;
        apFromE = ce;
        if (cur != apText) {
            int n = (int)apText.size(), p = 0, q = 0;
            while (p < len && p < n && cur[(size_t)p] == apText[(size_t)p]) p++;
            while (q < len - p && q < n - p && cur[(size_t)(len - 1 - q)] == apText[(size_t)(n - 1 - q)]) q++;
            p = std::min(Utf8Back(cur, p), Utf8Back(apText, p));
            while (q > 0 && ((len - q < len && ((unsigned char)cur[(size_t)(len - q)] & 0xC0) == 0x80) ||
                             (n - q < n && ((unsigned char)apText[(size_t)(n - q)] & 0xC0) == 0x80)))
                q--;
            st->SetSelection(p, len - q);
            std::string mid = apText.substr((size_t)p, (size_t)(n - p - q));
            if (mid.empty()) {
                io.AddKeyEvent(ImGuiKey_Backspace, true);
                io.AddKeyEvent(ImGuiKey_Backspace, false);
            } else {
                io.AddInputCharactersUTF8(mid.c_str());
            }
            applying = true;
            return true;
        }
        applying = true;  // (only the selection / Enter: set as the queue is empty)
        return true;
    }
    if (cur != last || cs != lastS || ce != lastE) push(cur != last);
    return false;
}
#endif

// One screen: from the window (a phone's screen) opening to its end. On a phone it runs again each time Android
// creates the screen anew, in the same process - the engine may then already be running (core.h).
static int RunScreen(int argc, char** argv) {
#ifdef _WIN32
    SetProcessDPIAware();
#endif
    // --portable / --config <dir>: keep settings next to the executable or elsewhere; other words: files to open
    // (--send: to share them instead)
    std::vector<std::string> openPaths;
    bool openSend = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) ls::SetConfigDirOverride(argv[++i]);
        else if (a == "--portable") {
            char* base = SDL_GetBasePath();
            if (base) { ls::SetConfigDirOverride(ls::JoinPath(base, "LunaShareData")); SDL_free(base); }
        } else if (a == "--send") openSend = true;
        else if (a.size() > 2 && a.compare(0, 2, "--") == 0) continue;
        else if (a.compare(0, 7, "file://") == 0) {  // (desktops hand files over as addresses: back to a path)
            std::string p = a.substr(7), out;
            if (p.compare(0, 9, "localhost") == 0) p = p.substr(9);
            for (size_t k = 0; k < p.size(); k++) {
                if (p[k] == '%' && k + 2 < p.size() && isxdigit((unsigned char)p[k + 1]) && isxdigit((unsigned char)p[k + 2])) {
                    out += (char)std::stoi(p.substr(k + 1, 2), nullptr, 16);
                    k += 2;
                } else out += p[k];
            }
            openPaths.push_back(out);
        } else if (a.compare(0, 4, "-psn") != 0) openPaths.push_back(a);
    }
    // one copy per settings folder: a second start hands its files over and ends
    static bool owner = false;  // (dev rounds below: this process has it already)
    if (!owner && !ls::IsMobilePlatform() && !instance::Acquire(ls::ConfigDir())) {
        instance::SendRequest(ls::ConfigDir(), openPaths, openSend);
        return 0;
    }
    owner = true;
    if (!ls::IsMobilePlatform()) instance::ClearRequests(ls::ConfigDir());
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait PortraitUpsideDown LandscapeLeft LandscapeRight");
    // Android: sound goes on in the background (music, calls); a movie or a chat video is paused on its own
    // (with the default "block on pause", SDL pauses every audio device whatever the hint above says - and the app's
    // loop doesn't need to stop in the background: it only waits, without drawing, while the window is minimized)
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
#if defined(__ANDROID__)
    if (const char* p = SDL_AndroidGetInternalStoragePath()) {
        ls::SetConfigDirOverride(p);
        // (libarchive makes its temporary files there when packing a 7z; Android has no /tmp)
        std::string tmp = ls::JoinPath(p, "cache/tmp");
        ls::MakeDirs(tmp);
        setenv("TMPDIR", tmp.c_str(), 1);
    }
#endif
    {
        // files unpacked from archives to look at or to send: what was looked at goes now; what was sent stays two
        // weeks (a transfer to someone offline may still need it)
        std::string cache = ls::JoinPath(ls::ConfigDir(), "cache");
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::u8path(ls::JoinPath(cache, "open")), ec);
        int64_t now = (int64_t)time(nullptr);
        for (auto& e : ls::ListDir(cache))
            if (e.isDir && e.name.compare(0, 5, "send-") == 0 && now - e.mtime > 14 * 86400)
                std::filesystem::remove_all(std::filesystem::u8path(ls::JoinPath(cache, e.name)), ec);
    }

    const char* glsl = nullptr;
#if defined(IMGUI_IMPL_OPENGL_ES2)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    glsl = "#version 100";
#elif defined(IMGUI_IMPL_OPENGL_ES3)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    glsl = "#version 300 es";
#elif defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    glsl = "#version 150";
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    glsl = "#version 130";
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    float scale0 = ImGui_ImplSDL2_GetContentScaleForDisplay(0);
    if (scale0 <= 0) scale0 = 1;
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    int w = (int)(1180 * scale0), h = (int)(760 * scale0);
    if (ls::IsMobilePlatform()) flags |= SDL_WINDOW_FULLSCREEN;
    if (const char* sz = SDL_getenv("LUNASHARE_WINDOW")) sscanf(sz, "%dx%d", &w, &h);
    SDL_Window* window = SDL_CreateWindow("LunaShare", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!window) {
        printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 320, 400);
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        printf("SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);
    if (!ls::IsMobilePlatform())
        ls::Bridge().setFullscreen = [window](bool on, bool borderless) {
            static bool inBorderless = false, wasMaximized = false;
            static SDL_Rect before{};
            if (inBorderless) {  // back from the borderless kind
                SDL_SetWindowAlwaysOnTop(window, SDL_FALSE);
                SDL_SetWindowBordered(window, SDL_TRUE);
                SDL_SetWindowPosition(window, before.x, before.y);
                SDL_SetWindowSize(window, before.w, before.h);
                if (wasMaximized) SDL_MaximizeWindow(window);
                inBorderless = false;
            }
            if (!on || !borderless) {
                SDL_SetWindowFullscreen(window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                return;
            }
            // the whole screen but its last row of pixels, on top of the taskbar
            SDL_SetWindowFullscreen(window, 0);
            wasMaximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
            if (wasMaximized) SDL_RestoreWindow(window);
            SDL_GetWindowPosition(window, &before.x, &before.y);
            SDL_GetWindowSize(window, &before.w, &before.h);
            SDL_Rect r{};
            if (SDL_GetDisplayBounds(SDL_GetWindowDisplayIndex(window) > 0 ? SDL_GetWindowDisplayIndex(window) : 0, &r) != 0) return;
            SDL_SetWindowBordered(window, SDL_FALSE);
            SDL_SetWindowAlwaysOnTop(window, SDL_TRUE);
            SDL_SetWindowPosition(window, r.x, r.y);
            SDL_SetWindowSize(window, r.w, r.h - 1);
            SDL_RaiseWindow(window);
            inBorderless = true;
        };
#ifdef __ANDROID__
    ls::Bridge().imeWakeEvent = SDL_RegisterEvents(1);  // (see ImeSync)
    if (ls::Bridge().imeWakeEvent == (Uint32)-1) ls::Bridge().imeWakeEvent = 0;
#endif
    ls::Bridge().keepAwake = [](bool on) { on ? SDL_DisableScreenSaver() : SDL_EnableScreenSaver(); };
    if (!glf::Load()) printf("warning: some OpenGL functions are missing\n");
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    static std::string iniPath = ls::JoinPath(ls::ConfigDir(), "imgui.ini");
    io.IniFilename = iniPath.c_str();
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
#ifdef __ANDROID__
    // The on-screen keyboard. The backend calls SDL_StartTextInput() whenever the text cursor moves (every letter
    // typed) and SDL_StopTextInput() the moment no field has focus (one frame while the layout moves for the
    // keyboard): on Android each of those re-creates or hides the keyboard - it blinked and swallowed the first
    // letters. Here it's shown once when a field wants it, and hidden only when none has for a moment.
    ImGui::GetPlatformIO().Platform_SetImeDataFn = [](ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* data) {
        if (data->WantVisible) {
            g_kbOffSince = 0;
            if (!g_kbOn || !SDL_IsTextInputActive()) {
                SDL_Rect r{(int)data->InputPos.x, (int)data->InputPos.y, 1, (int)data->InputLineHeight};
                SDL_SetTextInputRect(&r);
                SDL_StartTextInput();
                g_kbOn = true;
            }
        } else if (g_kbOn && !g_kbOffSince) {
            g_kbOffSince = SDL_GetTicks64();
        }
    };
#endif
    ImGui_ImplOpenGL3_Init(glsl);
    Media().SetGlsl(glsl);  // video: YUV -> RGB in a shader
    LoadFonts();

    // The engine: on a phone it may already be running - it kept going while the screen was away, or the background
    // service started it (see core.h). Then it's taken as it is, not started again.
    bool adopted = core::Running() != nullptr;
    ls::Engine& engine = adopted ? *core::Running() : core::Make();
    if (adopted) SDL_Log("core: the screen takes back the running engine");
    AndroidBridgeInit(&engine);
    Emoji().Load();          // optional colour emoji module (needs the Android bridge for the system font)
    Emoji().AddToFont(io.Fonts);
    AndroidAddSystemTextFont(io.Fonts);  // last: only characters no other font has
    Emoji().InstallClipboardHook();
    Player().Load();         // optional player module (audio/video files in chat)
    Media().Load();          // optional calls module; must happen before Start() so the capability is announced
    Media().Attach(&engine);
    Torrents().Load();       // optional torrent module (desktops)
    Torrents().Attach(&engine);  // (after Media's: it adds to the capabilities announced)
    if (!adopted) engine.Start();
    core::SaveCaps(engine);  // (for a start without the screen)
    Torrents().Start();
    if (Torrents().Available() && assoc::Supported()) {
        // "Open with" was set up before torrents came: .torrent files and magnet links are added to it
        assoc::WithTorrents(false);
        bool had = assoc::Registered();
        assoc::WithTorrents(true);
        std::string err;
        if (had && !assoc::Registered()) assoc::Register(err);
    }
    // the music player (its own audio device and thread: it plays on in the background)
    music::Lib().Init(engine.ConfigPath(), Player().Module());
    movpos::Init(engine.ConfigPath());
    music::Play().Init(Player().Module(), &music::Lib());
    radio::R().Init(engine.ConfigPath());
    music::Play().LoadState(music::Lib().StatePath());
    music::Play().onChange = [] {
        static Uint32 ev = SDL_RegisterEvents(1);
        SDL_Event e{};
        e.type = ev;
        if (ev != (Uint32)-1) SDL_PushEvent(&e);  // (the UI shows it)
        if (ls::Bridge().musicState) {
            music::Now n = music::Play().GetNow();
            ls::Bridge().musicState(n.active, n.playing, n.track.title, n.track.artist, n.track.album, n.track.cover, n.dur, n.pos);
        }
    };
#if defined(__ANDROID__)
    {
        // first start: use the phone's model name instead of "user@android"
        auto lk = engine.Lock();
        std::string dn = AndroidDeviceName();
        if (!dn.empty() && (engine.settings.displayName.rfind("user@", 0) == 0)) {
            ls::Settings s = engine.settings;
            s.displayName = dn;
            engine.ApplySettings(s);
        }
    }
#endif
    // (system notifications while in the background: attached by core::Make)
    if (tray::Available()) notify::SetTrayHook([](const std::string& t, const std::string& x) { tray::Balloon(t, x); });
    if (ls::Bridge().backgroundService && engine.GetSettings().backgroundRun) ls::Bridge().backgroundService(true);
    // (the engine ends after the UI: the App - declared below, so gone first - still refers to it; on a phone with
    // background running on it doesn't end at all)
    bool keepEngine = false;
    struct EngineEnd {
        bool& keep;
        ~EngineEnd() {
            if (!keep) core::Destroy();
        }
    } engineEnd{keepEngine};
    App app(engine);
    if (!openPaths.empty()) app.OpenPaths(openPaths, openSend);
    int64_t lastRequestPoll = 0;
    bool hiddenToTray = false;
    // closing the window: quit, or (setting) keep running - in the tray on Windows, minimised elsewhere
    auto closeRequested = [&]() -> bool {
        if (!ls::IsMobilePlatform() && engine.GetSettings().closeToTray) {
            if (tray::Available() && tray::Show("LunaShare")) {
                SDL_HideWindow(window);
                hiddenToTray = true;
            } else {
                SDL_MinimizeWindow(window);
            }
            notify::SetBackground(true);
            return false;
        }
        return app.RequestQuit();
    };

    int styleKey = -1;
    std::string lastTitle;
    std::vector<std::string> drops;
    bool running = true;
    int64_t lastInput = ls::SteadyMs();
    const int64_t screenStart = ls::SteadyMs();  // (dev hooks: their times count from this screen's start)
    while (running) {
        // ---- events (sleep when idle to save battery/CPU)
        SDL_Event ev;
        bool idle = !app.busy && ls::SteadyMs() - lastInput > 1500;
        bool got = idle ? SDL_WaitEventTimeout(&ev, 250) != 0 : SDL_PollEvent(&ev) != 0;
        while (got) {
            // a tap on the typing strip above the on-screen keyboard: only moves the cursor (ImGui would end the typing)
            static bool mirrorDown = false;
            bool swallow = false;
            if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEMOTION) {
                float mx = ev.type == SDL_MOUSEMOTION ? (float)ev.motion.x : (float)ev.button.x;
                float my = ev.type == SDL_MOUSEMOTION ? (float)ev.motion.y : (float)ev.button.y;
                const float* r = app.mirrorRect;
                bool inside = r[2] > r[0] && mx >= r[0] && mx < r[2] && my >= r[1] && my < r[3];
                if (ev.type == SDL_MOUSEBUTTONDOWN && inside) {
                    mirrorDown = true;
                    app.mirrorTapX = mx;
                    app.mirrorTapY = my;
                }
                swallow = mirrorDown || (inside && ev.type != SDL_MOUSEMOTION);
                if (ev.type == SDL_MOUSEBUTTONUP) mirrorDown = false;
                if (ev.type == SDL_MOUSEMOTION && mirrorDown) {  // (sliding the finger along moves the cursor too)
                    app.mirrorTapX = mx;
                    app.mirrorTapY = my;
                }
            }
            if (swallow) lastInput = ls::SteadyMs();
            if (ev.type != SDL_DROPFILE && !swallow) ImGui_ImplSDL2_ProcessEvent(&ev);
            switch (ev.type) {
                case SDL_QUIT:
                    if (ls::IsMobilePlatform() ? app.RequestQuit() : (!hiddenToTray && closeRequested())) running = false;
                    break;
                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_CLOSE && ev.window.windowID == SDL_GetWindowID(window) && !hiddenToTray && closeRequested())
                        running = false;
                    if (ev.window.event == SDL_WINDOWEVENT_MINIMIZED || ev.window.event == SDL_WINDOWEVENT_HIDDEN) notify::SetBackground(true);
                    if (ev.window.event == SDL_WINDOWEVENT_RESTORED || ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                        (ev.window.event == SDL_WINDOWEVENT_SHOWN && !hiddenToTray))
                        notify::SetBackground(false);
                    break;
                case SDL_DROPBEGIN: drops.clear(); break;
                case SDL_DROPFILE:
                    if (ev.drop.file) {
                        drops.push_back(ev.drop.file);
                        SDL_free(ev.drop.file);
                    }
                    break;
                case SDL_DROPCOMPLETE:
                    app.OnDrop(drops);
                    drops.clear();
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    if (ev.key.keysym.sym == SDLK_AC_BACK) io.AddKeyEvent(ImGuiKey_AppBack, ev.type == SDL_KEYDOWN);
                    break;
                case SDL_MULTIGESTURE: {
                    int ww, wh;
                    SDL_GetWindowSize(window, &ww, &wh);
                    if (ev.mgesture.numFingers >= 2) app.OnPinch(ev.mgesture.dDist, ev.mgesture.x * (float)ww, ev.mgesture.y * (float)wh);
                    break;
                }
                case SDL_APP_WILLENTERBACKGROUND:
                case SDL_APP_DIDENTERBACKGROUND:
                    notify::SetBackground(true);  // engine keeps running (background service); notifications from now on
#if defined(__ANDROID__)
                    if (Player().Playing()) Player().Pause();  // (a movie / chat video; music plays on)
#endif
                    break;
                case SDL_APP_WILLENTERFOREGROUND:
                case SDL_APP_DIDENTERFOREGROUND:
                    notify::SetBackground(false);
                    break;
                default: break;
            }
            if (ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEWHEEL || ev.type == SDL_KEYDOWN ||
                ev.type == SDL_TEXTINPUT || ev.type == SDL_FINGERMOTION || ev.type == SDL_FINGERDOWN || ev.type == SDL_WINDOWEVENT ||
                (ls::Bridge().imeWakeEvent && ev.type == ls::Bridge().imeWakeEvent))
                lastInput = ls::SteadyMs();
            got = SDL_PollEvent(&ev) != 0;
        }
        if (!drops.empty()) {  // some platforms send DROPFILE without BEGIN/COMPLETE
            app.OnDrop(drops);
            drops.clear();
        }
        if (app.quitConfirmed) running = false;
        if (tray::TakeQuit()) running = false;
        bool showReq = false;
        if (!ls::IsMobilePlatform() && ls::SteadyMs() - lastRequestPoll > 300) {  // (another start handed something over)
            lastRequestPoll = ls::SteadyMs();
            instance::Request rq;
            if (instance::Poll(ls::ConfigDir(), rq)) {
                showReq = true;
                if (!rq.paths.empty()) app.OpenPaths(rq.paths, rq.send);
            }
        }
        if (tray::TakeShow() || showReq || (hiddenToTray && notify::Peek())) {
            SDL_ShowWindow(window);
            SDL_RestoreWindow(window);
            SDL_RaiseWindow(window);
            hiddenToTray = false;
            notify::SetBackground(false);
        }
        if (SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) {
            SDL_Delay(ls::IsMobilePlatform() ? 200 : 50);  // (phones in the background: nothing drawn, few wake-ups)
            app.MusicTick();  // (a call coming in pauses the music also now)
            // calls are followed here too: one that ended while the screen was off kept the microphone and the
            // phone's one sound output until the app was opened again (the radio "played" without a sound)
            Media().Update(true);
            Player().UpdateAudio();
            continue;
        }

        // ---- style (DPI / user scale / theme / layout changes)
        ls::Settings s = engine.GetSettings();
        float scale = s.uiScale > 0 ? s.uiScale : DetectScale(window);
        bool mobile = s.layout == ls::UiLayout::Mobile || (s.layout == ls::UiLayout::Auto && ls::IsMobilePlatform());
        if (s.layout == ls::UiLayout::Auto && !ls::IsMobilePlatform()) {
            int ww, wh;
            SDL_GetWindowSize(window, &ww, &wh);
            mobile = ww < wh && ww < 620 * scale;
        }
        int key = (int)(scale * 100) * 100 + s.theme * 10 + (mobile ? 1 : 0) + app.styleGeneration * 100000;
        if (key != styleKey) {
            styleKey = key;
            ApplyStyle(s.theme, scale, mobile);
            app.theme = s.theme;
            ui::M().scale = scale;
        }
        int dw, dh;
        SDL_GetWindowSize(window, &dw, &dh);
        {  // the on-screen keyboard: how much of the window it covers (Android tells; otherwise a guess)
            static const bool fakeKb = SDL_getenv("LUNASHARE_FAKEKB") != nullptr;  // (dev: the phone keyboard on a computer)
            float frac = ls::Bridge().keyboardFraction ? ls::Bridge().keyboardFraction() : -1.0f;
            bool shown = ls::IsMobilePlatform() ? (frac > 0 || SDL_IsScreenKeyboardShown(window)) : fakeKb && ImGui::GetIO().WantTextInput;
            if (frac <= 0.05f) frac = 0.42f;
            app.keyboardInset = shown ? (float)dh * (frac < 0.7f ? frac : 0.7f) : 0.0f;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        {
            // dev: LUNASHARE_FPSLOG prints how many frames the UI draws per second
            static const bool fpsLog = SDL_getenv("LUNASHARE_FPSLOG") != nullptr;
            static int frames = 0;
            static int64_t fpsT = ls::SteadyMs();
            if (fpsLog && (++frames, ls::SteadyMs() - fpsT >= 1000)) {
                SDL_Log("ui fps %d", frames);
                frames = 0;
                fpsT = ls::SteadyMs();
            }
        }
        {
            // dev: LUNASHARE_TAPS="x,y,ms[,hold_ms];..." simulates touches (pixels, ms since start) for UI tests
            static const char* taps = SDL_getenv("LUNASHARE_TAPS");
            static int64_t t0 = ls::SteadyMs();
            static int stage = 0;  // 0 idle, 1 down sent
            static int64_t upAt = 0;
            if (taps && *taps) {
                int x, y, ms, hold = 80, n = 0;
                int got = sscanf(taps, "%d,%d,%d,%d%n", &x, &y, &ms, &hold, &n);
                if (got < 4) { hold = 80; got = sscanf(taps, "%d,%d,%d%n", &x, &y, &ms, &n); }
                int64_t now = ls::SteadyMs() - t0;
                // (as SDL events, like a real finger: they go the same way - e.g. past the typing strip)
                auto push = [&](Uint32 type) {
                    SDL_Event e{};
                    e.type = type;
                    if (type == SDL_MOUSEMOTION) {
                        e.motion.which = SDL_TOUCH_MOUSEID;
                        e.motion.x = x;
                        e.motion.y = y;
                    } else {
                        e.button.which = SDL_TOUCH_MOUSEID;
                        e.button.button = SDL_BUTTON_LEFT;
                        e.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
                        e.button.clicks = 1;
                        e.button.x = x;
                        e.button.y = y;
                    }
                    e.button.windowID = SDL_GetWindowID(window);
                    SDL_PushEvent(&e);
                };
                if (got >= 3 && stage == 0 && now >= ms && x < 0) {  // (x -1: type LUNASHARE_TYPE; -2: Backspace, y times)
                    if (x == -1) {
                        SDL_Event e{};
                        e.type = SDL_TEXTINPUT;
                        e.text.windowID = SDL_GetWindowID(window);
                        SDL_strlcpy(e.text.text, SDL_getenv("LUNASHARE_TYPE") ? SDL_getenv("LUNASHARE_TYPE") : "a", sizeof(e.text.text));
                        SDL_PushEvent(&e);
                    } else {
                        for (int k = 0; k < (y > 1 ? y : 1); k++)
                            for (Uint32 t : {(Uint32)SDL_KEYDOWN, (Uint32)SDL_KEYUP}) {
                                SDL_Event e{};
                                e.type = t;
                                e.key.windowID = SDL_GetWindowID(window);
                                e.key.state = t == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
                                e.key.keysym.sym = SDLK_BACKSPACE;
                                e.key.keysym.scancode = SDL_SCANCODE_BACKSPACE;
                                SDL_PushEvent(&e);
                            }
                    }
                    taps += n;
                    while (*taps == ';' || *taps == ' ') taps++;
                } else if (got >= 3 && stage == 0 && now >= ms) {
                    push(SDL_MOUSEMOTION);
                    push(SDL_MOUSEBUTTONDOWN);
                    stage = 1;
                    upAt = now + hold;
                } else if (stage == 1 && now >= upAt) {
                    push(SDL_MOUSEBUTTONUP);
                    SDL_Log("tap %d,%d done", x, y);
                    if (const char* typed = SDL_getenv("LUNASHARE_TYPE"); typed && !strstr(SDL_getenv("LUNASHARE_TAPS"), "-1,")) {  // dev: then type this (once)
                        static bool done = false;
                        if (!done) {
                            done = true;
                            SDL_Event e{};
                            e.type = SDL_TEXTINPUT;
                            e.text.windowID = SDL_GetWindowID(window);
                            SDL_strlcpy(e.text.text, typed, sizeof(e.text.text));
                            SDL_PushEvent(&e);
                        }
                    }
                    stage = 0;
                    taps += n;
                    while (*taps == ';' || *taps == ' ') taps++;
                }
            }
        }
#ifdef __ANDROID__
        if (g_kbOn && g_kbOffSince && SDL_GetTicks64() - g_kbOffSince > 300) {  // (no field wanted it for a moment)
            SDL_StopTextInput();
            g_kbOn = false;
            g_kbOffSince = 0;
        }
#endif
        ImGui::NewFrame();
#ifdef __ANDROID__
        // Back closed the keyboard (SDL stopped text input): the text field lets go too, or it would ask for the
        // keyboard again at once (it blinked back). Tapping the field brings it back.
        if (g_kbOn && !SDL_IsTextInputActive()) {
            g_kbOn = false;
            g_kbOffSince = 0;
            ImGui::ClearActiveID();
        }
#endif
        app.discardFrame = false;
        app.Frame();
        Player().SetLoop(engine.settings.playerLoop && !engine.watch.sid);
        Player().Update();  // before Media(): hands it the current video picture to upload
        Media().Update();
        ImGui::Render();

        if (app.windowTitle != lastTitle) {
            lastTitle = app.windowTitle;
            SDL_SetWindowTitle(window, lastTitle.c_str());
        }
        if (app.wantAttention) {
            app.wantAttention = false;
#if SDL_VERSION_ATLEAST(2, 0, 16)
            if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) SDL_FlashWindow(window, SDL_FLASH_UNTIL_FOCUSED);
#endif
        }

        // a frame whose picture is off (the chat jumped back to its end) isn't shown: the next one comes at once
        static int discarded = 0;
        if (app.discardFrame && discarded < 6) {
            discarded++;
            continue;
        }
        discarded = 0;
        int fbw, fbh;
        SDL_GL_GetDrawableSize(window, &fbw, &fbh);
        glf::Viewport(0, 0, fbw, fbh);
        ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        glf::ClearColor(bg.x, bg.y, bg.z, 1.0f);
        glf::Clear(glf::COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // optional screenshot for docs/CI (read before swapping)
        const int64_t startMs = screenStart;
        if (static const char* fl = SDL_getenv("LUNASHARE_FRAMELOG"); fl) {  // dev: when each frame was drawn (smoothness)
            static FILE* ff = fopen(fl, "w");
            if (ff) fprintf(ff, "%lld\n", (long long)(ls::SteadyMs() - startMs)), fflush(ff);
        }
        if (const char* shot = SDL_getenv("LUNASHARE_SCREENSHOT")) {
            const char* delay = SDL_getenv("LUNASHARE_SCREENSHOT_DELAY");
            if (ls::SteadyMs() - startMs > (delay ? atoi(delay) : 4000)) {
                std::vector<unsigned char> px((size_t)fbw * fbh * 4);
                glf::PixelStorei(glf::PACK_ALIGNMENT, 1);
                glf::ReadPixels(0, 0, fbw, fbh, glf::RGBA, glf::UNSIGNED_BYTE, px.data());  // RGBA is the only format GLES guarantees
                if (FILE* f = fopen(shot, "wb")) {
                    fprintf(f, "P6\n%d %d\n255\n", fbw, fbh);
                    for (int y = fbh - 1; y >= 0; y--)
                        for (int x = 0; x < fbw; x++) fwrite(&px[((size_t)y * fbw + x) * 4], 1, 3, f);
                    fclose(f);
                }
                running = false;
            }
        }
#ifdef __ANDROID__
        if (ImeSync(app)) lastInput = ls::SteadyMs();  // (an edit on its way through: the next frames come at once)
#endif
        SDL_GL_SwapWindow(window);

    }

    // A phone's screen going away (Android ends the UI when it likes) leaves the engine running when "Keep running in
    // the background" is on: messages and calls still arrive, and the screen takes it back when it comes (core.h).
    // What needs the screen ends: a call (its sound and pictures are the UI's), watching together, the music.
    const bool keep = keepEngine = core::KeepWithoutScreen(engine);
    tray::Hide();
    if (keep) {
        notify::SetBackground(true);
        {
            // (a call in progress or being made needs the screen; one coming in rings on as a notification)
            auto lk = engine.Lock();
            ls::CallState cs = engine.call.state;
            engine.ownHook = nullptr;  // (the torrents' - computers only)
            lk.unlock();
            if (cs == ls::CallState::Active || cs == ls::CallState::OutgoingRinging) engine.EndCall("hangup");
        }
        SDL_Log("core: the screen goes, the engine keeps running");
    } else {
        if (ls::Bridge().backgroundService) ls::Bridge().backgroundService(false);
        AndroidBridgeShutdown();
    }
    engine.LeaveWatch();  // (unblocks a movie still being read from another device)
    engine.LeaveRead();   // (a comic read together is on the screen)
    music::Play().onChange = nullptr;
    radio::R().Shutdown();
    music::Play().Shutdown();
    music::Lib().Shutdown();
    movpos::Save(true);
    UpdateShutdown();
    Player().Shutdown();
    Media().Shutdown();
    Torrents().Shutdown();
    if (!keep) engine.Stop();  // (here, as always; the engine itself ends after the UI - EngineEnd)
    Images().Shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

int main(int argc, char** argv) {
#if !defined(__ANDROID__)
    // Dev / tests: what a phone does, on a computer (with LUNASHARE_KEEP_CORE: the window's end keeps the engine).
    //   LUNASHARE_HEADLESS_FIRST=ms  the engine starts without a screen first (the phone's background service),
    //                                and the window comes that much later
    //   LUNASHARE_SCREEN_ROUNDS=n    the window opens and closes n times (LUNASHARE_SCREEN_GAP=ms between)
    const char* hf = SDL_getenv("LUNASHARE_HEADLESS_FIRST");
    const char* rn = SDL_getenv("LUNASHARE_SCREEN_ROUNDS");
    const char* gp = SDL_getenv("LUNASHARE_SCREEN_GAP");
    if (hf || rn) {
        for (int i = 1; i + 1 < argc; i++)
            if (std::string(argv[i]) == "--config") ls::SetConfigDirOverride(argv[i + 1]);
        if (hf) {
            bool ok = core::StartWithoutScreen("");
            SDL_Log("dev: started without the screen: %s", ok ? "yes" : "no");
            SDL_Delay((Uint32)atoi(hf));
        }
        int rounds = rn && atoi(rn) > 1 ? atoi(rn) : 1, r = 0;
        for (int i = 0; i < rounds; i++) {
            SDL_Log("dev: screen %d of %d (engine %s)", i + 1, rounds, core::Running() ? "running" : "not running");
            r = RunScreen(argc, argv);
            if (i + 1 < rounds && gp) SDL_Delay((Uint32)atoi(gp));
        }
        core::Destroy();  // (the very end of the process)
        return r;
    }
#endif
    return RunScreen(argc, argv);
}
