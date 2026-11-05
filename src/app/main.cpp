// LunaShare - file sharing chat for LAN, phone hotspots and Bluetooth.
// SDL2 + OpenGL3/GLES3 + Dear ImGui.
#include "notify.h"
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
#include "emoji_host.h"
#include "gl_funcs.h"
#include "image_cache.h"

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

void AndroidBridgeInit(ls::Engine* engine);  // android_bridge.cpp (no-op elsewhere)
void AndroidBridgeShutdown();
std::string AndroidDeviceName();

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
        "C:\\Windows\\Fonts\\seguisym.ttf", "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\malgun.ttf",
#elif defined(__ANDROID__)
        "/system/fonts/NotoSansSymbols-Regular-Subsetted.ttf", "/system/fonts/NotoSansCJK-Regular.ttc",
#elif defined(__APPLE__)
        "/System/Library/Fonts/PingFang.ttc",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
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
    Emoji().InstallClipboardHook();
    Player().Load();         // optional player module (audio/video files in chat)
    Media().Load();          // optional calls module; must happen before Start() so the capability is announced
    Media().Attach(&engine);
    engine.Start();
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
            if (ev.type != SDL_DROPFILE) ImGui_ImplSDL2_ProcessEvent(&ev);
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
            SDL_Delay(50);
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
        app.keyboardInset = (ls::IsMobilePlatform() && SDL_IsScreenKeyboardShown(window)) ? (float)dh * 0.42f : 0.0f;

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
                if (got >= 3 && stage == 0 && now >= ms) {
                    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
                    io.AddMousePosEvent((float)x, (float)y);
                    io.AddMouseButtonEvent(0, true);
                    stage = 1;
                    upAt = now + hold;
                } else if (stage == 1 && now >= upAt) {
                    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
                    io.AddMouseButtonEvent(0, false);
                    SDL_Log("tap %d,%d done", x, y);
                    stage = 0;
                    taps += n;
                    while (*taps == ';' || *taps == ' ') taps++;
                }
            }
        }
        ImGui::NewFrame();
        app.Frame();
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
