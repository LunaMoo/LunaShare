// LunaShare - file sharing chat for LAN, phone hotspots and Bluetooth.
// SDL2 + OpenGL3/GLES3 + Dear ImGui.
#include "notify.h"
#include "stb_image.h"
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);  // (avatar.cpp)
#include "tray.h"
#include <algorithm>
#include <SDL.h>

#include <cstdio>
#include <string>
#include <vector>

#include "engine.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"
#include "media_host.h"
#include "player_host.h"
#include "music.h"
#include "emoji_host.h"
#include "gl_funcs.h"
#include "image_cache.h"

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
    for (int i = 0; main[i] && !f; i++)
        if (ls::FileExists(main[i])) f = io.Fonts->AddFontFromFileTTF(main[i]);
    if (!f) {
        io.Fonts->AddFontDefaultVector();
        return;
    }
    for (int i = 0; extra[i]; i++) {
        if (!ls::FileExists(extra[i])) continue;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(extra[i], 0.0f, &cfg);
    }
#if defined(__ANDROID__)
    for (auto& [path, index] : AndroidFallbackFonts()) {
        if (path == "/system/fonts/NotoSansCJK-Regular.ttc" && index == 0) continue;  // (already in)
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.FontNo = index;
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
    style.WindowRounding = 6;
    style.ChildRounding = 6;
    style.FrameRounding = 5;
    style.PopupRounding = 6;
    style.GrabRounding = 5;
    style.TabRounding = 5;
    style.ScrollbarRounding = 8;
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(8, 6);
    if (theme == 0) {
        ImVec4* c = style.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
        c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.13f, 0.00f);
        c[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.14f, 0.17f, 0.98f);
        c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.19f, 0.23f, 1.00f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.23f, 0.25f, 0.30f, 1.00f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.29f, 0.36f, 1.00f);
        c[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.36f, 0.50f, 1.00f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.36f, 0.42f, 0.75f, 1.00f);
        c[ImGuiCol_Header] = ImVec4(0.24f, 0.27f, 0.36f, 1.00f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.32f, 0.44f, 1.00f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.36f, 0.42f, 0.70f, 1.00f);
        c[ImGuiCol_Tab] = ImVec4(0.16f, 0.17f, 0.21f, 1.00f);
        c[ImGuiCol_TabHovered] = ImVec4(0.36f, 0.42f, 0.70f, 1.00f);
        c[ImGuiCol_TabSelected] = ImVec4(0.30f, 0.35f, 0.58f, 1.00f);
        c[ImGuiCol_PlotHistogram] = ImVec4(0.42f, 0.50f, 0.90f, 1.00f);
        c[ImGuiCol_CheckMark] = ImVec4(0.55f, 0.65f, 1.00f, 1.00f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.55f, 0.95f, 1.00f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.17f, 0.21f, 1.00f);
        c[ImGuiCol_Separator] = ImVec4(0.25f, 0.27f, 0.32f, 1.00f);
    }
    if (mobile) {
        style.FramePadding = ImVec2(12, 10);
        style.ItemSpacing = ImVec2(10, 10);
        style.ScrollbarSize = 6;  // drag to scroll instead
        style.TouchExtraPadding = ImVec2(4, 4);
        style.GrabMinSize = 28;
        style.WindowPadding = ImVec2(10, 10);
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

int main(int argc, char** argv) {
#ifdef _WIN32
    SetProcessDPIAware();
#endif
    // --portable / --config <dir>: keep settings next to the executable or elsewhere
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) ls::SetConfigDirOverride(argv[++i]);
        else if (a == "--portable") {
            char* base = SDL_GetBasePath();
            if (base) { ls::SetConfigDirOverride(ls::JoinPath(base, "LunaShareData")); SDL_free(base); }
        }
    }
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
    if (const char* p = SDL_AndroidGetInternalStoragePath()) ls::SetConfigDirOverride(p);
#endif

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
    ImGui_ImplOpenGL3_Init(glsl);
    Media().SetGlsl(glsl);  // video: YUV -> RGB in a shader
    LoadFonts();

    ls::Engine engine;
    AndroidBridgeInit(&engine);
    Emoji().Load();          // optional colour emoji module (needs the Android bridge for the system font)
    Emoji().AddToFont(io.Fonts);
    AndroidAddSystemTextFont(io.Fonts);  // last: only characters no other font has
    Emoji().InstallClipboardHook();
    Player().Load();         // optional player module (audio/video files in chat)
    Media().Load();          // optional calls module; must happen before Start() so the capability is announced
    Media().Attach(&engine);
    engine.Start();
    // the music player (its own audio device and thread: it plays on in the background)
    // chat archives are kept compressed (zlib, from stb - already here for pictures)
    engine.compress = [](const std::string& in) -> std::string {
        int n = 0;
        unsigned char* z = stbi_zlib_compress((unsigned char*)in.data(), (int)in.size(), &n, 8);
        std::string out = z && n > 0 ? std::string((const char*)z, (size_t)n) : std::string();
        free(z);
        return out;
    };
    engine.decompress = [](const std::string& in) -> std::string {
        int n = 0;
        char* d = stbi_zlib_decode_malloc(in.data(), (int)in.size(), &n);
        std::string out = d && n > 0 ? std::string(d, (size_t)n) : std::string();
        free(d);
        return out;
    };
    music::Lib().Init(engine.ConfigPath(), Player().Module());
    movpos::Init(engine.ConfigPath());
    music::Play().Init(Player().Module(), &music::Lib());
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
    notify::Attach(&engine);  // system notifications while in the background
    if (tray::Available()) notify::SetTrayHook([](const std::string& t, const std::string& x) { tray::Balloon(t, x); });
    if (ls::Bridge().backgroundService && engine.GetSettings().backgroundRun) ls::Bridge().backgroundService(true);
    App app(engine);
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
                ev.type == SDL_TEXTINPUT || ev.type == SDL_FINGERMOTION || ev.type == SDL_FINGERDOWN || ev.type == SDL_WINDOWEVENT)
                lastInput = ls::SteadyMs();
            got = SDL_PollEvent(&ev) != 0;
        }
        if (!drops.empty()) {  // some platforms send DROPFILE without BEGIN/COMPLETE
            app.OnDrop(drops);
            drops.clear();
        }
        if (app.quitConfirmed) running = false;
        if (tray::TakeQuit()) running = false;
        if (tray::TakeShow() || (hiddenToTray && notify::Peek())) {
            SDL_ShowWindow(window);
            SDL_RestoreWindow(window);
            SDL_RaiseWindow(window);
            hiddenToTray = false;
            notify::SetBackground(false);
        }
        if (SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) {
            SDL_Delay(ls::IsMobilePlatform() ? 200 : 50);  // (phones in the background: nothing drawn, few wake-ups)
            app.MusicTick();  // (a call coming in pauses the music also now)
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
                if (got >= 3 && stage == 0 && now >= ms) {
                    push(SDL_MOUSEMOTION);
                    push(SDL_MOUSEBUTTONDOWN);
                    stage = 1;
                    upAt = now + hold;
                } else if (stage == 1 && now >= upAt) {
                    push(SDL_MOUSEBUTTONUP);
                    SDL_Log("tap %d,%d done", x, y);
                    if (const char* typed = SDL_getenv("LUNASHARE_TYPE")) {  // dev: then type this (once)
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
        ImGui::NewFrame();
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

        int fbw, fbh;
        SDL_GL_GetDrawableSize(window, &fbw, &fbh);
        glf::Viewport(0, 0, fbw, fbh);
        ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        glf::ClearColor(bg.x, bg.y, bg.z, 1.0f);
        glf::Clear(glf::COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // optional screenshot for docs/CI (read before swapping)
        static int64_t startMs = ls::SteadyMs();
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
        SDL_GL_SwapWindow(window);

    }

    tray::Hide();
    if (ls::Bridge().backgroundService) ls::Bridge().backgroundService(false);
    AndroidBridgeShutdown();
    engine.onEvent = nullptr;
    engine.LeaveWatch();  // (unblocks a movie still being read from another device)
    music::Play().onChange = nullptr;
    music::Play().Shutdown();
    music::Lib().Shutdown();
    movpos::Save(true);
    UpdateShutdown();
    Player().Shutdown();
    Media().Shutdown();
    engine.Stop();
    Images().Shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
