#include "media_host.h"
#include "i18n.h"

#include <SDL.h>

#include <chrono>
#include <cmath>
#include <cstring>

static constexpr double kPi = 3.14159265358979323846;

#include "bluetooth.h"
#include "gl_funcs.h"
#include "platform.h"

using namespace ls;

#if defined(LS_MEDIA_STATIC)
extern "C" const LsmModule* lunashare_media_module(void);
#endif

MediaHost& Media() {
    static MediaHost m;
    return m;
}

static Uint32 WakeEvent();

bool MediaHost::Load() {
    WakeEvent();  // registered on the UI thread
#if defined(LS_MEDIA_STATIC)
    mod_ = lunashare_media_module();
    loadInfo = "built in";
#else
    std::vector<std::string> dirs;
    if (char* base = SDL_GetBasePath()) {
        std::string b = base;
        SDL_free(base);
        dirs.push_back(b);
        dirs.push_back(JoinPath(b, "modules"));
#ifdef __APPLE__
        dirs.push_back(JoinPath(b, "../MacOS"));
        dirs.push_back(JoinPath(b, "../Frameworks"));
#endif
    }
#if defined(_WIN32)
    const char* names[] = {"lunashare_media.dll", "liblunashare_media.dll"};
#elif defined(__APPLE__)
    const char* names[] = {"liblunashare_media.dylib", "lunashare_media.dylib"};
#else
    const char* names[] = {"liblunashare_media.so", "lunashare_media.so"};
#endif
    std::vector<std::string> candidates;
#if defined(__ANDROID__)
    candidates.push_back("liblunashare_media.so");  // packaged in the APK's lib folder (calls flavor only)
#endif
    for (auto& d : dirs)
        for (auto* n : names) candidates.push_back(JoinPath(d, n));
    for (auto& c : candidates) {
#if !defined(__ANDROID__)
        if (c.find('/') != std::string::npos || c.find('\\') != std::string::npos)
            if (!FileExists(c)) continue;
#endif
        void* lib = SDL_LoadObject(c.c_str());
        if (!lib) continue;
        auto fn = (LsmGetModuleFn)SDL_LoadFunction(lib, "lunashare_media_module");
        const LsmModule* m = fn ? fn() : nullptr;
        if (!m || m->api_version != LSM_API_VERSION) {
            loadInfo = c + ": incompatible module";
            SDL_UnloadObject(lib);
            continue;
        }
        lib_ = lib;
        mod_ = m;
        loadInfo = c;
        break;
    }
    if (!mod_ && loadInfo.empty()) loadInfo = "not installed";
#endif
    if (mod_) LoadVideo();
    return mod_ != nullptr;
}

// ============================================================================ video module
void MediaHost::LoadVideo() {
#if defined(LS_MEDIA_STATIC)
    videoLoadInfo = "not available";
#else
    std::vector<std::string> candidates;
#if defined(__ANDROID__)
    candidates.push_back("liblunashare_video.so");
#endif
    if (char* base = SDL_GetBasePath()) {
        std::string b = base;
        SDL_free(base);
#if defined(_WIN32)
        const char* names[] = {"lunashare_video.dll", "liblunashare_video.dll"};
#elif defined(__APPLE__)
        const char* names[] = {"liblunashare_video.dylib", "lunashare_video.dylib"};
#else
        const char* names[] = {"liblunashare_video.so", "lunashare_video.so"};
#endif
        for (auto& d : {b, JoinPath(b, "modules")})
            for (auto* n : names) candidates.push_back(JoinPath(d, n));
    }
    for (auto& c : candidates) {
#if !defined(__ANDROID__)
        if (!FileExists(c)) continue;
#endif
        void* lib = SDL_LoadObject(c.c_str());
        if (!lib) continue;
        auto fn = (LsvGetModuleFn)SDL_LoadFunction(lib, "lunashare_video_module");
        const LsvModule* m = fn ? fn() : nullptr;
        if (!m || m->api_version != LSV_API_VERSION) {
            videoLoadInfo = c + ": incompatible module";
            SDL_UnloadObject(lib);
            continue;
        }
        vhost_ = LsvHost{LSV_API_VERSION, this, VSend, VImage, VState, VQueued, VLog, nullptr, nullptr};
        if (Bridge().javaVm) vhost_.java_vm = Bridge().javaVm();
        if (Bridge().videoClass) vhost_.java_class = Bridge().videoClass();
        if (!m->init(&vhost_)) {
            videoLoadInfo = c + ": no camera/codec support on this system";
            SDL_UnloadObject(lib);
            continue;
        }
        vlib_ = lib;
        vmod_ = m;
        videoLoadInfo = c;
        vcamAvail_ = m->vcam_available && m->vcam_available();
        break;
    }
    if (!vmod_ && videoLoadInfo.empty()) videoLoadInfo = "not installed";
#endif
}

static const int kRes[7][2] = {{640, 360}, {960, 540}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {7680, 4320}};

LsvSettings MediaHost::Preset(int res, int fps, bool adaptive) {
    res = std::max(0, std::min(kResCount - 1, res));
    fps = std::max(10, std::min(120, fps));
    int w = kRes[res][0], h = kRes[res][1];
    // H.264 needs fewer bits per pixel the larger the picture: ~0.1 at 360p, ~0.06 at 4K
    double bpp = res <= 1 ? 0.10 : res <= 3 ? 0.08 : res <= 5 ? 0.06 : 0.05;
    double bps = (double)w * h * fps * bpp;
    int bitrate = (int)std::max(250000.0, std::min(100e6, bps));
    return {w, h, fps, bitrate, adaptive ? 1 : 0};
}

const char* MediaHost::ResName(int r) {
    static const char* n[] = {"360p (640x360)", "540p (960x540)", "720p HD (1280x720)", "1080p Full HD (1920x1080)",
                              "1440p QHD (2560x1440)", "4K UHD (3840x2160)", "8K (7680x4320)"};
    return n[std::max(0, std::min(kResCount - 1, r))];
}

const int* MediaHost::FpsList(int* n) {
    static const int f[] = {15, 24, 30, 60, 120};
    *n = 5;
    return f;
}

std::string MediaHost::RateText(int bps) {
    char b[32];
    if (bps >= 1000000) snprintf(b, sizeof(b), "%.1f Mbit/s", bps / 1e6);
    else snprintf(b, sizeof(b), "%d kbit/s", bps / 1000);
    return b;
}

// The UI sleeps between events to save battery. A new video picture is such an event: it wakes the UI right away
// (once per drawn picture), so video is shown at its own frame rate even when nobody touches the screen.
static std::atomic<bool> g_wakePending{false};
static Uint32 WakeEvent() {
    static Uint32 t = SDL_RegisterEvents(1);
    return t;
}
static void WakeUi() {
    if (g_wakePending.exchange(true)) return;
    SDL_Event e{};
    e.type = WakeEvent();
    if (e.type != (Uint32)-1) SDL_PushEvent(&e);
}

int MediaHost::VSend(void* u, const uint8_t* d, size_t n) {
    auto* self = (MediaHost*)u;
    if (self->vmsgOpen_) {
        // video message: frames go into the file (once recording), nothing goes to the network
        if (!self->vmsgRec_ || n <= 13 || d[0] != 2 || d[1] != 1 || d[12] != 0) return 1;
        bool key = d[6] & 1;
        int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> l(self->vmsgM_);
        if (!self->vmsgStarted_) {
            if (!key) return 1;
            // the picture's turn is fixed for the whole message, from its first frame (like camera apps)
            if (!self->vmsgWriter_.Open(self->vmsgPath_, (d[7] & 3) * 90, self->vmsgPreSkip_)) return 1;
            self->vmsgT0_ = now;
            self->vmsgStarted_ = true;
        }
        self->vmsgWriter_.AddVideo(d + 13, n - 13, key, now - self->vmsgT0_, (int)(d[8] | d[9] << 8), (int)(d[10] | d[11] << 8));
        return 1;
    }
    return self->eng_ && self->eng_->SendMedia(d, n) ? 1 : 0;
}
size_t MediaHost::VQueued(void* u) {
    auto* self = (MediaHost*)u;
    return self->eng_ ? self->eng_->MediaQueuedBytes() : 0;
}
void MediaHost::VState(void* u, int on) { ((MediaHost*)u)->remoteCam_ = on != 0; }
void MediaHost::VLog(void*, const char* m) { SDL_Log("%s", m); }

void MediaHost::VImage(void* u, int src, const LsvImage* img) {
    auto* self = (MediaHost*)u;
    if (src != LSV_LOCAL && src != LSV_REMOTE) return;
    if (src == LSV_REMOTE && self->vcamOn_) {  // to "LunaShare Camera" as well, turned the way the user chose
        LsvImage v = *img;
        v.rotation = (v.rotation + self->remoteRotAdj_) % 360;
        self->vmod_->vcam_push(&v);
    }
    Slot& s = self->slots_[src];
    {
        std::lock_guard<std::mutex> l(s.m);
        if (s.fresh) return;  // the UI hasn't shown the previous picture yet: don't convert one it would never show
    }
    if (self->gpuYuv_) CopyPlanes(*img, s);
    else Convert(*img, s);
    WakeUi();
}

// GPU path: only copy the planes (tight Y, interleaved U/V); colour conversion, rotation and mirroring happen when drawing
void MediaHost::CopyPlanes(const LsvImage& im, Slot& s) {
    int W = im.width, H = im.height;
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192) return;
    int cw = (W + 1) / 2, ch = (H + 1) / 2;
    std::vector<uint8_t> y((size_t)W * H), uv((size_t)cw * ch * 2);
    for (int r = 0; r < H; r++) memcpy(&y[(size_t)r * W], im.y + (size_t)r * im.y_stride, (size_t)W);
    int ps = im.uv_pixel_stride;
    if (ps == 2 && im.v == im.u + 1) {  // NV12: already U,V pairs
        for (int r = 0; r < ch; r++) memcpy(&uv[(size_t)r * cw * 2], im.u + (size_t)r * im.uv_stride, (size_t)cw * 2);
    } else {
        for (int r = 0; r < ch; r++) {
            const uint8_t* u = im.u + (size_t)r * im.uv_stride;
            const uint8_t* v = im.v + (size_t)r * im.uv_stride;
            uint8_t* d = &uv[(size_t)r * cw * 2];
            for (int c = 0; c < cw; c++) {
                d[c * 2] = u[(size_t)c * ps];
                d[c * 2 + 1] = v[(size_t)c * ps];
            }
        }
    }
    std::lock_guard<std::mutex> l(s.m);
    s.y.swap(y);
    s.uv.swap(uv);
    s.sw = W;
    s.sh = H;
    s.rotation = ((im.rotation % 360) + 360) % 360;
    s.mirror = im.mirror != 0;
    s.gpu = true;
    s.fresh = true;
}

// YUV 4:2:0 -> RGBA, turning the picture upright (and mirroring the self-view) on the way
void MediaHost::Convert(const LsvImage& im, Slot& s) {
    int rot = ((im.rotation % 360) + 360) % 360;
    bool swap = rot == 90 || rot == 270;
    int ow = swap ? im.height : im.width, oh = swap ? im.width : im.height;
    if (ow <= 0 || oh <= 0 || ow > 4096 || oh > 4096) return;
    std::vector<uint8_t> out((size_t)ow * oh * 4);
    const int W = im.width, H = im.height, ps = im.uv_pixel_stride;
    for (int oy = 0; oy < oh; oy++) {
        uint8_t* dst = &out[(size_t)oy * ow * 4];
        for (int ox0 = 0; ox0 < ow; ox0++) {
            int ox = im.mirror ? ow - 1 - ox0 : ox0;
            int sx, sy;
            switch (rot) {
                case 90: sx = oy; sy = H - 1 - ox; break;
                case 180: sx = W - 1 - ox; sy = H - 1 - oy; break;
                case 270: sx = W - 1 - oy; sy = ox; break;
                default: sx = ox; sy = oy; break;
            }
            int Y = im.y[(size_t)sy * im.y_stride + sx];
            size_t ci = (size_t)(sy >> 1) * im.uv_stride + (size_t)(sx >> 1) * ps;
            int U = im.u[ci] - 128, V = im.v[ci] - 128;
            int C = (Y - 16) * 298;
            int r = (C + 409 * V + 128) >> 8, g = (C - 100 * U - 208 * V + 128) >> 8, b = (C + 516 * U + 128) >> 8;
            dst[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            dst[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            dst[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            dst[3] = 255;
            dst += 4;
        }
    }
    std::lock_guard<std::mutex> l(s.m);
    s.rgba.swap(out);
    s.w = ow;
    s.h = oh;
    s.gpu = false;
    s.fresh = true;
}

static void MakeTex(unsigned& t) {
    if (t) return;
    glf::GenTextures(1, &t);
    glf::BindTexture(glf::TEXTURE_2D, t);
    glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_MIN_FILTER, glf::LINEAR);
    glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_MAG_FILTER, glf::LINEAR);
    glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_WRAP_S, glf::CLAMP_TO_EDGE);
    glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_WRAP_T, glf::CLAMP_TO_EDGE);
}

static void Upload(unsigned tex, bool resize, int internalFmt, unsigned fmt, int w, int h, const void* data) {
    glf::BindTexture(glf::TEXTURE_2D, tex);
    if (resize) glf::TexImage2D(glf::TEXTURE_2D, 0, internalFmt, w, h, 0, fmt, glf::UNSIGNED_BYTE, data);
    else glf::TexSubImage2D(glf::TEXTURE_2D, 0, 0, 0, w, h, fmt, glf::UNSIGNED_BYTE, data);
}

void MediaHost::PushPlayerFrame(const LsvImage& img) {
    Slot& s = slots_[2];
    {
        std::lock_guard<std::mutex> l(s.m);
        if (s.fresh) return;
    }
    if (gpuYuv_) CopyPlanes(img, s);
    else Convert(img, s);
}

void MediaHost::UploadVideo() {
    g_wakePending = false;  // the next picture may wake the UI again
    for (int i = 0; i < 3; i++) {
        Slot& s = slots_[i];
        VideoTex& t = tex_[i];
        std::lock_guard<std::mutex> l(s.m);
        if (!s.fresh) continue;
        s.fresh = false;
        int prevAlign = 4;
        glf::GetIntegerv(glf::UNPACK_ALIGNMENT, &prevAlign);
        glf::PixelStorei(glf::UNPACK_ALIGNMENT, 1);
        if (s.gpu) {
            bool resize = !t.gpu || t.sw != s.sw || t.sh != s.sh || !t.uvTex;
            MakeTex(t.tex);
            MakeTex(t.uvTex);
            Upload(t.tex, resize, glf::R8, glf::RED, s.sw, s.sh, s.y.data());
            Upload(t.uvTex, resize, glf::RG8, glf::RG, (s.sw + 1) / 2, (s.sh + 1) / 2, s.uv.data());
            t.gpu = true;
            t.sw = s.sw;
            t.sh = s.sh;
            t.rotation = s.rotation;
            t.mirror = s.mirror;
            bool swap = s.rotation == 90 || s.rotation == 270;
            t.w = swap ? s.sh : s.sw;
            t.h = swap ? s.sw : s.sh;
        } else {
            MakeTex(t.tex);
            bool resize = t.gpu || t.w != s.w || t.h != s.h;
            Upload(t.tex, resize, glf::RGBA, glf::RGBA, s.w, s.h, s.rgba.data());
            t.gpu = false;
            t.w = t.sw = s.w;
            t.h = t.sh = s.h;
            t.rotation = 0;
            t.mirror = false;
        }
        glf::PixelStorei(glf::UNPACK_ALIGNMENT, prevAlign);
        t.valid = true;
        t.lastMs = (int64_t)SDL_GetTicks64();
    }
}

// ---------------------------------------------------------------- YUV -> RGB shader
// Swapped in for one draw command through an ImDrawList callback; uses the same vertex layout and projection as the
// ImGui OpenGL backend (read from its program), then ImDrawCallback_ResetRenderState switches back.
namespace {
struct YuvGl {
    unsigned prog = 0;
    int proj = -1, texY = -1, texUV = -1;
    bool failed = false;
};
YuvGl g_yuv;
std::string g_glsl;

unsigned Compile(unsigned type, const std::string& src) {
    unsigned sh = glf::CreateShader(type);
    const char* p = src.c_str();
    glf::ShaderSource(sh, 1, &p, nullptr);
    glf::CompileShader(sh);
    int ok = 0;
    glf::GetShaderiv(sh, glf::COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glf::GetShaderInfoLog(sh, sizeof(log), nullptr, log);
        SDL_Log("video shader: %s", log);
        glf::DeleteShader(sh);
        return 0;
    }
    return sh;
}

bool BuildYuv(unsigned backendProg) {
    bool es = g_glsl.find(" es") != std::string::npos;
    std::string vs = g_glsl + "\n" + (es ? "precision highp float;\n" : "") +
                     "uniform mat4 ProjMtx;\nin vec2 Position;\nin vec2 UV;\nin vec4 Color;\nout vec2 Frag_UV;\nout vec4 Frag_Color;\n"
                     "void main() { Frag_UV = UV; Frag_Color = Color; gl_Position = ProjMtx * vec4(Position.xy, 0, 1); }\n";
    std::string fs = g_glsl + "\n" + (es ? "precision mediump float;\n" : "") +
                     "uniform sampler2D TexY;\nuniform sampler2D TexUV;\nin vec2 Frag_UV;\nin vec4 Frag_Color;\nout vec4 Out_Color;\n"
                     "void main() {\n"
                     "  float y = 1.16438 * (texture(TexY, Frag_UV).r - 0.0625);\n"
                     "  vec2 c = texture(TexUV, Frag_UV).rg - vec2(0.5);\n"
                     "  vec3 rgb = vec3(y + 1.59603 * c.y, y - 0.39176 * c.x - 0.81297 * c.y, y + 2.01723 * c.x);\n"
                     "  Out_Color = vec4(clamp(rgb, 0.0, 1.0), 1.0) * Frag_Color;\n"
                     "}\n";
    unsigned v = Compile(glf::VERTEX_SHADER, vs), f = Compile(glf::FRAGMENT_SHADER, fs);
    if (!v || !f) return false;
    unsigned p = glf::CreateProgram();
    glf::AttachShader(p, v);
    glf::AttachShader(p, f);
    // same attribute slots as the ImGui backend, so its vertex array setup works for us too
    const char* attrs[] = {"Position", "UV", "Color"};
    for (auto* a : attrs) {
        int loc = glf::GetAttribLocation(backendProg, a);
        if (loc >= 0) glf::BindAttribLocation(p, (unsigned)loc, a);
    }
    glf::LinkProgram(p);
    glf::DeleteShader(v);
    glf::DeleteShader(f);
    int ok = 0;
    glf::GetProgramiv(p, glf::LINK_STATUS, &ok);
    if (!ok) return false;
    SDL_Log("video: converting YUV on the GPU");
    g_yuv.prog = p;
    g_yuv.proj = glf::GetUniformLocation(p, "ProjMtx");
    g_yuv.texY = glf::GetUniformLocation(p, "TexY");
    g_yuv.texUV = glf::GetUniformLocation(p, "TexUV");
    return true;
}

void YuvBegin(const ImDrawList*, const ImDrawCmd* cmd) {
    unsigned uvTex = *(const unsigned*)cmd->UserCallbackData;
    int backend = 0;
    glf::GetIntegerv(glf::CURRENT_PROGRAM, &backend);
    if (!g_yuv.prog && !g_yuv.failed && !BuildYuv((unsigned)backend)) {
        g_yuv.failed = true;
        Media().SetGlsl(nullptr);  // fall back to converting on the CPU from now on
    }
    if (!g_yuv.prog) return;
    float m[16];
    glf::GetUniformfv((unsigned)backend, glf::GetUniformLocation((unsigned)backend, "ProjMtx"), m);
    glf::UseProgram(g_yuv.prog);
    glf::UniformMatrix4fv(g_yuv.proj, 1, 0, m);
    glf::Uniform1i(g_yuv.texY, 0);
    glf::Uniform1i(g_yuv.texUV, 1);
    glf::ActiveTexture(glf::TEXTURE1);
    glf::BindTexture(glf::TEXTURE_2D, uvTex);
    glf::ActiveTexture(glf::TEXTURE0);
}
}  // namespace

void MediaHost::SetGlsl(const char* glsl) {
    if (!glsl) {
        gpuYuv_ = false;
        return;
    }
    glsl_ = glsl;
    g_glsl = glsl;
    // GLES 2 has no single/two-channel textures to sample like this: keep the CPU conversion there
    gpuYuv_ = std::string(glsl) != "#version 100" && glf::LoadShaders();
    if (!gpuYuv_) SDL_Log("video: converting YUV on the CPU (%s)", glsl);
}

void MediaHost::DrawVideo(ImDrawList* dl, const VideoTex& t, ImVec2 p0, ImVec2 p1, bool cover, int extraRotation) {
    float bw = p1.x - p0.x, bh = p1.y - p0.y;
    if (!t.valid || t.w <= 0 || t.h <= 0 || bw <= 0 || bh <= 0) return;
    int extra = ((extraRotation % 360) + 360) % 360;
    int rot = (t.rotation + extra) % 360;
    float tw = (float)t.w, th = (float)t.h;
    if (extra == 90 || extra == 270) std::swap(tw, th);
    float s = cover ? std::max(bw / tw, bh / th) : std::min(bw / tw, bh / th);
    float w = tw * s, h = th * s;
    ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
    // the part of the upright picture that is visible (cover crops it to the box)
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (cover) {
        if (a.x < p0.x) { float f = (p0.x - a.x) / w; u0 = f; u1 = 1 - f; a.x = p0.x; b.x = p1.x; }
        if (a.y < p0.y) { float f = (p0.y - a.y) / h; v0 = f; v1 = 1 - f; a.y = p0.y; b.y = p1.y; }
    }
    // upright (display) coordinates -> texture coordinates: mirror first, then undo the rotation
    auto tc = [&](float u, float v) {
        if (t.mirror) u = 1 - u;
        switch (rot) {
            case 90: return ImVec2(v, 1 - u);
            case 180: return ImVec2(1 - u, 1 - v);
            case 270: return ImVec2(1 - v, u);
            default: return ImVec2(u, v);
        }
    };
    ImTextureID id = (ImTextureID)(intptr_t)t.tex;
    if (t.gpu) {
        unsigned uv = t.uvTex;
        dl->AddCallback(YuvBegin, &uv, sizeof(uv));
    }
    dl->AddImageQuad(id, a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y), tc(u0, v0), tc(u1, v0), tc(u1, v1), tc(u0, v1));
    if (t.gpu) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

void MediaHost::SetVirtualCamera(bool on) {
    if (!vcamAvail_ || vcamBusy_ || on == vcamOn_) return;
    if (vcamThread_.joinable()) vcamThread_.join();
    vcamBusy_ = true;
    {
        std::lock_guard<std::mutex> l(vcamM_);
        vcamStatus_ = on ? T("Setting up LunaShare Camera...") : "";
    }
    vcamThread_ = std::thread([this, on] {
        char msg[256] = {0};
        bool ok = vmod_->vcam_enable(on ? 1 : 0, msg, sizeof(msg)) != 0;
        if (on) vcamOn_ = ok;
        else vcamOn_ = false;
        {
            std::lock_guard<std::mutex> l(vcamM_);
            vcamStatus_ = ok ? (on ? T("LunaShare Camera is available to other apps") : "") : std::string(T("Virtual camera: ")) + msg;
        }
        vcamBusy_ = false;
    });
}

std::string MediaHost::VcamStatus() {
    std::lock_guard<std::mutex> l(vcamM_);
    return vcamStatus_;
}

void MediaHost::StartVideo() {
    if (!vmod_ || videoRunning_ || !eng_) return;
    std::string peer;
    {
        auto lk = eng_->Lock();
        peer = eng_->call.peer;
    }
    if (!eng_->PeerHasCap(peer, "video")) return;
    Settings s = eng_->GetSettings();
    LsvSettings vs = Preset(s.callVideoRes, s.callVideoFps, s.callVideoAdaptive);
    vs.codec_pref = s.callVideoH264Only ? 1 : 0;
    if (!vmod_->start(&vs)) return;
    videoRunning_ = true;
    remoteCam_ = false;
    if (!s.callCamera.empty()) {
        camId_ = s.callCamera;
        vmod_->select_camera(camId_.c_str());
    }
    if (wantCamera_) SetCameraOn(true);
}

void MediaHost::StopVideo() {
    wantCamera_ = false;
    if (!videoRunning_) return;
    vmod_->stop();
    videoRunning_ = false;
    camOn_ = false;
    remoteCam_ = false;
    for (int i = 0; i < 2; i++) {
        tex_[i].valid = false;
        std::lock_guard<std::mutex> l(slots_[i].m);
        slots_[i].fresh = false;
    }
}

bool MediaHost::SetCameraOn(bool on) {
    if (!videoRunning_) return false;
    if (on) {
#if defined(__ANDROID__)
        if (!SDL_AndroidRequestPermission("android.permission.CAMERA")) {
            audioError = T("camera permission denied");
            return false;
        }
#endif
        if (!vmod_->set_camera_on(1)) {
            audioError = T("camera could not be started");
            return false;
        }
        camOn_ = true;
    } else {
        vmod_->set_camera_on(0);
        camOn_ = false;
        tex_[LSV_LOCAL].valid = false;
    }
    return true;
}

std::vector<LsvCamera> MediaHost::Cameras() {
    std::vector<LsvCamera> v(16);
    int n = vmod_ ? vmod_->list_cameras(v.data(), 16) : 0;
    v.resize((size_t)std::max(0, n));
    return v;
}

void MediaHost::SelectCamera(const std::string& id) {
    camId_ = id;
    if (vmod_) vmod_->select_camera(id.c_str());
}

std::string MediaHost::CurrentCameraName() {
    auto cams = Cameras();
    for (auto& c : cams)
        if (camId_ == c.id) return c.name;
    for (auto& c : cams)
        if (c.facing == LSV_FACING_FRONT) return c.name;  // the default
    return cams.empty() ? "Camera" : cams[0].name;
}

void MediaHost::NextCamera() {
    auto cams = Cameras();
    if (cams.size() < 2) return;
    // prefer flipping between the main front and back cameras
    int cur = -1;
    for (size_t i = 0; i < cams.size(); i++)
        if (camId_ == cams[i].id) cur = (int)i;
    int curFacing = cur >= 0 ? cams[(size_t)cur].facing : LSV_FACING_FRONT;
    for (auto& c : cams)
        if (c.facing != curFacing) {
            SelectCamera(c.id);
            return;
        }
    SelectCamera(cams[(size_t)((cur + 1) % (int)cams.size())].id);
}

std::vector<LsvControl> MediaHost::Controls() {
    std::vector<LsvControl> v(LSV_CTL_COUNT + 4);
    int n = vmod_ && camOn_ ? vmod_->get_controls(v.data(), (int)v.size()) : 0;
    v.resize((size_t)std::max(0, n));
    return v;
}

void MediaHost::SetControl(int id, float val) {
    if (vmod_ && camOn_) vmod_->set_control(id, val);
}

LsvStats MediaHost::VideoStats() {
    LsvStats s{};
    if (vmod_ && videoRunning_) vmod_->get_stats(&s);
    return s;
}

void MediaHost::ApplyVideoQuality() {
    if (!vmod_ || !videoRunning_ || !eng_) return;
    Settings s = eng_->GetSettings();
    LsvSettings vs = Preset(s.callVideoRes, s.callVideoFps, s.callVideoAdaptive);
    vs.codec_pref = s.callVideoH264Only ? 1 : 0;
    vmod_->set_settings(&vs);
}

std::string MediaHost::ModuleName() const {
    return mod_ ? std::string(mod_->name) + " " + mod_->version : std::string();
}

std::vector<std::string> MediaHost::Caps() const {
    std::vector<std::string> out;
    if (!mod_) return out;
    std::string c = mod_->caps, cur;
    for (char ch : c + ",") {
        if (ch == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else if (ch != ' ') cur += ch;
    }
    return out;
}

void MediaHost::Attach(Engine* e) {
    eng_ = e;
    if (!mod_) return;
    e->localCaps = Caps();
    if (vmod_) e->localCaps.push_back("video");
    const LsmModule* m = mod_;
    e->onMedia = [this, m](uint64_t, const uint8_t* d, size_t n) {
        if (mode_ != Mode::Call || n < 1) return;
        if (d[0] == 2) {
            if (videoRunning_ && vmod_) vmod_->receive(d, n);
        } else {
            m->receive(d, n);
        }
    };
}

std::vector<std::string> MediaHost::Devices(bool capture) {
    std::vector<std::string> out;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    int n = SDL_GetNumAudioDevices(capture ? 1 : 0);
    for (int i = 0; i < n; i++)
        if (const char* name = SDL_GetAudioDeviceName(i, capture ? 1 : 0)) out.push_back(name);
    return out;
}

void MediaHost::SendCb(void* u, const uint8_t* data, size_t len) {
    auto* self = (MediaHost*)u;
    if (self->eng_) self->eng_->SendMedia(data, len);
}

void MediaHost::LogCb(void*, const char* msg) { SDL_Log("media: %s", msg); }

void MediaHost::ApplyMicGain(int16_t* pcm, int n) {
    float g = micGain_.load();
    if (g <= 1.001f) return;
    for (int i = 0; i < n; i++) {
        float v = pcm[i] * g;
        pcm[i] = (int16_t)std::max(-32768.f, std::min(32767.f, v));
    }
}

void MediaHost::CaptureCb(void* u, uint8_t* stream, int len) {
    auto* self = (MediaHost*)u;
    int n = len / 2;
    auto* pcm = (int16_t*)stream;
    self->ApplyMicGain(pcm, n);
    if (self->mode_ == Mode::Call) {
        self->mod_->capture(pcm, n);
    } else if (self->mode_ == Mode::Record) {
        self->mod_->rec_push(pcm, n);
    } else if (self->mode_ == Mode::MicTest) {
        double s = 0;
        for (int i = 0; i < n; i++) s += (double)pcm[i] * pcm[i];
        double rms = std::sqrt(s / std::max(1, n)) / 32768.0;
        double db = rms > 1e-6 ? 20 * std::log10(rms) : -100;
        self->micTestLevel_ = (float)std::max(0.0, std::min(1.0, (db + 60) / 60));
    }
}

void MediaHost::PlaybackCb(void* u, uint8_t* stream, int len) {
    auto* self = (MediaHost*)u;
    auto* out = (int16_t*)stream;
    int n = len / 2;
    Mode m = self->mode_;
    if (m == Mode::Call) {
        self->mod_->playback(out, n);
        return;
    }
    // ring (incoming): two-tone 0.4 s bursts; ringback (outgoing): 425 Hz, 1 s on / 4 s off
    for (int i = 0; i < n; i++) {
        uint64_t t = self->toneSample_++;
        double sec = (double)t / LSM_SAMPLE_RATE, v = 0;
        if (m == Mode::Ring) {
            double cyc = std::fmod(sec, 3.0);
            if (cyc < 0.4 || (cyc > 0.6 && cyc < 1.0)) v = 0.25 * (std::sin(2 * kPi * 660 * sec) + std::sin(2 * kPi * 880 * sec));
        } else if (m == Mode::Ringback) {
            if (std::fmod(sec, 5.0) < 1.0) v = 0.18 * std::sin(2 * kPi * 425 * sec);
        }
        out[i] = (int16_t)(v * 32767);
    }
}

bool MediaHost::OpenDevices(bool capture, bool playback) {
    CloseDevices();
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        audioError = std::string("audio unavailable: ") + SDL_GetError();
        return false;
    }
    Settings s = eng_ ? eng_->GetSettings() : Settings();
    SDL_AudioSpec want{}, have{};
    want.freq = LSM_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;  // ~10.7 ms at 48 kHz
    want.userdata = this;
    audioError.clear();
    if (playback) {
        want.callback = PlaybackCb;
        playDev_ = SDL_OpenAudioDevice(s.callOutputDevice.empty() ? nullptr : s.callOutputDevice.c_str(), 0, &want, &have, 0);
        if (!playDev_ && !s.callOutputDevice.empty()) playDev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!playDev_) audioError = std::string("speaker: ") + SDL_GetError();
    }
    if (capture) {
#if defined(__ANDROID__)
        SDL_AndroidRequestPermission("android.permission.RECORD_AUDIO");
#endif
        want.callback = CaptureCb;
        capDev_ = SDL_OpenAudioDevice(s.callInputDevice.empty() ? nullptr : s.callInputDevice.c_str(), 1, &want, &have, 0);
        if (!capDev_ && !s.callInputDevice.empty()) capDev_ = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
        if (!capDev_) audioError += std::string(audioError.empty() ? "" : "; ") + "microphone: " + SDL_GetError();
    }
    if (playDev_) SDL_PauseAudioDevice(playDev_, 0);
    if (capDev_) SDL_PauseAudioDevice(capDev_, 0);
    return (!playback || playDev_) && (!capture || capDev_);
}

void MediaHost::CloseDevices() {
    if (capDev_) SDL_CloseAudioDevice(capDev_);
    if (playDev_) SDL_CloseAudioDevice(playDev_);
    capDev_ = playDev_ = 0;
}

void MediaHost::SetMode(Mode m) {
    Mode old = mode_;
    if (old == m) return;
    CloseDevices();  // stops the audio callbacks first
    if (old == Mode::Call) StopVideo();
    if (old == Mode::Call && mod_) mod_->stop();
    mode_ = Mode::Off;
    toneSample_ = 0;
    if (Bridge().callAudioMode) Bridge().callAudioMode(m == Mode::Call || m == Mode::Ringback);
    if (m == Mode::Call && mod_) {
        Settings s = eng_->GetSettings();
        LsmHost h{LSM_API_VERSION, this, SendCb, LogCb};
        LsmSettings ls{s.callEchoCancel ? 1 : 0, s.callNoiseSuppress ? 1 : 0, s.callAutoGain ? 1 : 0, s.callBitrate};
        mod_->start(&h, &ls);
        {
            auto lk = eng_->Lock();
            if (eng_->call.media.rfind("webcam", 0) == 0) mute_ = true;  // camera only: microphones start muted
        }
        remoteRotAdj_ = 0;
        mod_->set_mute(mute_ ? 1 : 0);
        mode_ = Mode::Call;
        OpenDevices(true, true);
        liveLs_ = ls;
        liveIn_ = s.callInputDevice;
        liveOut_ = s.callOutputDevice;
        liveDefaultOut_ = DefaultOutputName();
        liveRoute_ = -2;  // first route reading is not a change
        StartVideo();
    } else if (m == Mode::Ring || m == Mode::Ringback) {
        mode_ = m;
        OpenDevices(false, true);
    } else if (m == Mode::MicTest || m == Mode::Record) {
        mode_ = m;
        OpenDevices(true, false);
    }
}

void MediaHost::Update() {
    if (!eng_) return;
    if (!vcamChecked_) {  // settings are loaded by now
        vcamChecked_ = true;
        if (vcamAvail_ && eng_->GetSettings().virtualCamera) SetVirtualCamera(true);
    }
    uint32_t now = SDL_GetTicks();
    if (now - settingsTick_ > 500) {
        settingsTick_ = now;
        micGain_ = std::pow(10.0f, (float)eng_->GetSettings().callMicBoost / 20.0f);
    }
    CallState st;
    {
        auto lk = eng_->Lock();
        st = eng_->call.state;
    }
    Mode want = Mode::Off;
    if (st == CallState::Active && mod_) want = Mode::Call;
    else if (st == CallState::IncomingRinging) want = Mode::Ring;
    else if (st == CallState::OutgoingRinging) want = Mode::Ringback;
    else if (recording_ || vmsgRec_) want = Mode::Record;
    else if (micTest_) want = Mode::MicTest;
    if (recording_ && st != CallState::None) StopRecording(false);  // a call takes the microphone
    if (vmsgOpen_ && st != CallState::None) CloseVideoMessage(false);  // ... and the camera
    if (st == CallState::None) wantCamera_ = false;
    if (want != Mode::Call && mode_ == Mode::Call) mute_ = false;
    SetMode(want);
    UploadVideo();
    if (mode_ == Mode::Call && Bridge().callRoutes && now - routesTick_ > 1500) {
        routesTick_ = now;
        RefreshRoutes();  // follows headsets being plugged in / connected during the call
    }
    if (mode_ == Mode::Call) ApplyLiveAudio();
}

std::string MediaHost::DefaultOutputName() {
#if SDL_VERSION_ATLEAST(2, 24, 0) && !defined(__ANDROID__)
    char* name = nullptr;
    SDL_AudioSpec spec;
    if (SDL_GetDefaultAudioInfo(&name, &spec, 0) == 0 && name) {
        std::string r = name;
        SDL_free(name);
        return r;
    }
#endif
    return {};
}

void MediaHost::ResetEcho() {
    if (mod_ && mod_->api_version >= 5 && mod_->reset_echo) mod_->reset_echo();
}

// Audio settings changed during a call take effect right away; a different speaker / headphones / loudspeaker makes
// the echo canceller start over (what it learned about the old sound path only gets in the way).
void MediaHost::ApplyLiveAudio() {
    Settings s = eng_->GetSettings();
    LsmSettings ls{s.callEchoCancel ? 1 : 0, s.callNoiseSuppress ? 1 : 0, s.callAutoGain ? 1 : 0, s.callBitrate};
    if (memcmp(&ls, &liveLs_, sizeof(ls)) != 0) {
        bool aecOn = ls.echo_cancel && !liveLs_.echo_cancel;
        liveLs_ = ls;
        mod_->set_settings(&ls);
        if (aecOn) ResetEcho();
    }
    bool pathChanged = false;
    if (s.callInputDevice != liveIn_ || s.callOutputDevice != liveOut_) {
        liveIn_ = s.callInputDevice;
        liveOut_ = s.callOutputDevice;
        OpenDevices(true, true);
        pathChanged = true;
    }
    if (currentRoute_ != liveRoute_) {
        pathChanged |= liveRoute_ != -2;
        liveRoute_ = currentRoute_;
    }
    static uint32_t lastDefault = 0;
    uint32_t now = SDL_GetTicks();
    if (s.callOutputDevice.empty() && now - lastDefault > 1500) {  // "system default" switched (headphones plugged in)
        lastDefault = now;
        std::string d = DefaultOutputName();
        if (d != liveDefaultOut_) {
            liveDefaultOut_ = d;
            pathChanged = true;
        }
    }
    if (pathChanged) ResetEcho();
}

void MediaHost::Shutdown() {
    CloseVideoMessage(false);
    if (recording_) StopRecording(false);
    SetMode(Mode::Off);
    if (vcamThread_.joinable()) vcamThread_.join();
    if (vcamOn_ && vmod_) vmod_->vcam_enable(0, nullptr, 0);
    vcamOn_ = false;
    if (eng_) eng_->onMedia = nullptr;
}

void MediaHost::SetMute(bool m) {
    mute_ = m;
    if (mod_) mod_->set_mute(m ? 1 : 0);
}

void MediaHost::RefreshRoutes() {
    routes_.clear();
    currentRoute_ = -1;
    if (!Bridge().callRoutes) return;
    std::string s = Bridge().callRoutes();
    size_t pos = 0;
    bool first = true;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(pos, e - pos);
        pos = e + 1;
        if (first) { currentRoute_ = atoi(line.c_str()); first = false; continue; }
        size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t1 == std::string::npos) continue;
        Route r;
        r.id = atoi(line.substr(0, t1).c_str());
        r.kind = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
        r.name = t2 == std::string::npos ? "" : line.substr(t2 + 1);
        routes_.push_back(r);
    }
}

const MediaHost::Route* MediaHost::Current() const {
    for (auto& r : routes_)
        if (r.id == currentRoute_) return &r;
    return nullptr;
}

void MediaHost::SetRoute(int id) {
    if (Bridge().setCallRoute) Bridge().setCallRoute(id);
    currentRoute_ = id;
    routesTick_ = SDL_GetTicks();  // give Android a moment before re-reading the route
}

std::string MediaHost::RouteLabel(const Route& r) {
    if (r.kind == "earpiece") return T("Phone earpiece");
    if (r.kind == "speaker") return T("Loudspeaker");
    if (r.kind == "bluetooth") return r.name.empty() ? std::string(T("Bluetooth")) : std::string(T("Bluetooth")) + ": " + r.name;
    if (r.kind == "wired") return r.name.empty() ? std::string(T("Headphones")) : std::string(T("Headphones")) + ": " + r.name;
    if (r.kind == "usb") return r.name.empty() ? std::string(T("USB headset")) : "USB: " + r.name;
    return r.name.empty() ? r.kind : r.name;
}

LsmStats MediaHost::Stats() {
    LsmStats s{};
    if (mod_ && mode_ == Mode::Call) mod_->get_stats(&s);
    return s;
}

bool MediaHost::StartRecording(const std::string& path) {
    if (!mod_ || recording_ || mode_ == Mode::Call) return false;
    Settings s = eng_ ? eng_->GetSettings() : Settings();
    if (mod_->rec_start(path.c_str(), std::max(24000, s.callBitrate)) != 0) return false;
    micTest_ = false;
    recording_ = true;
    recStart_ = SDL_GetTicks();
    SetMode(Mode::Record);  // opens the microphone now (Update() keeps it that way)
    if (!capDev_) {
        mod_->rec_stop(0);
        recording_ = false;
        SetMode(Mode::Off);
        return false;
    }
    return true;
}

double MediaHost::StopRecording(bool keep) {
    if (!recording_) return 0;
    recording_ = false;
    SetMode(Mode::Off);  // closes the microphone first, so nothing is pushed while finishing
    return mod_->rec_stop(keep ? 1 : 0);
}

double MediaHost::RecordingSeconds() const {
    return recording_ || vmsgRec_ ? (SDL_GetTicks() - recStart_) / 1000.0 : 0;
}

// ---- video messages
void MediaHost::VmsgAudio(void* u, const uint8_t* d, int n) {
    auto* self = (MediaHost*)u;
    std::lock_guard<std::mutex> l(self->vmsgM_);
    if (self->vmsgStarted_) self->vmsgWriter_.AddAudio(d, (size_t)n);  // sound before the first picture is dropped
}

bool MediaHost::OpenVideoMessage(const std::string& path) {
    if (!CanRecordVideoMessages() || vmsgOpen_ || recording_) return false;
#if defined(__ANDROID__)
    if (!SDL_AndroidRequestPermission("android.permission.CAMERA")) {
        audioError = T("camera permission denied");
        return false;
    }
#endif
    // same resolution / frame rate as video calls (Advanced settings); always H.264, which the MP4 needs and every
    // device plays; no adaptation: this is recorded locally, not sent live
    Settings cs = eng_ ? eng_->GetSettings() : Settings();
    LsvSettings vs = Preset(cs.callVideoRes, cs.callVideoFps, false);
    vs.codec_pref = 1;
    if (!vmod_->start(&vs)) return false;
    vmsgPath_ = path;
    vmsgStarted_ = false;
    vmsgRec_ = false;
    vmsgOpen_ = true;
    // selfie camera unless one was picked for this before
    if (camId_.empty())
        for (auto& c : Cameras())
            if (c.facing == LSV_FACING_FRONT) {
                camId_ = c.id;
                break;
            }
    if (!camId_.empty()) vmod_->select_camera(camId_.c_str());
    if (!vmod_->set_camera_on(1)) {
        vmod_->stop();
        vmsgOpen_ = false;
        audioError = T("camera could not be started");
        return false;
    }
    camOn_ = true;
    return true;
}

bool MediaHost::RecordVideoMessage() {
    if (!vmsgOpen_ || vmsgRec_) return false;
    Settings s = eng_ ? eng_->GetSettings() : Settings();
    if (mod_->rec_start_stream(std::max(32000, s.callBitrate), VmsgAudio, this, &vmsgPreSkip_) != 0) return false;
    recStart_ = SDL_GetTicks();
    vmsgRec_ = true;
    SetMode(Mode::Record);  // microphone on
    if (!capDev_) {
        vmsgRec_ = false;
        mod_->rec_stop(0);
        SetMode(Mode::Off);
        return false;
    }
    const uint8_t askKey[2] = {2, 2};  // start the file with a fresh keyframe
    vmod_->receive(askKey, 2);
    return true;
}

double MediaHost::CloseVideoMessage(bool keep) {
    if (!vmsgOpen_) return 0;
    vmod_->set_camera_on(0);
    vmod_->stop();  // no more frames after this
    camOn_ = false;
    bool wasRec = vmsgRec_;
    vmsgRec_ = false;
    if (wasRec) {
        SetMode(Mode::Off);  // microphone off
        mod_->rec_stop(0);
    }
    double secs = 0;
    {
        std::lock_guard<std::mutex> l(vmsgM_);
        if (vmsgStarted_) {
            if (keep) secs = vmsgWriter_.Finish();
            else vmsgWriter_.Abort();
        }
        vmsgStarted_ = false;
    }
    vmsgOpen_ = false;
    tex_[LSV_LOCAL].valid = false;
    {
        std::lock_guard<std::mutex> l(slots_[LSV_LOCAL].m);
        slots_[LSV_LOCAL].fresh = false;
    }
    return secs;
}

void MediaHost::StartMicTest() { micTest_ = true; }
void MediaHost::StopMicTest() {
    micTest_ = false;
    micTestLevel_ = 0;
}
