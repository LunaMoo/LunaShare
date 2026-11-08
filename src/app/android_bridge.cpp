// Android glue: connects the engine's HostBridge hooks to Java (org.lunamoo.lunashare.LunaShareActivity)
// for Bluetooth RFCOMM, LocalOnlyHotspot, joining Wi-Fi networks, multicast lock and opening files.
// On other platforms these functions are no-ops.
#include <string>

#include "engine.h"
#include "imgui.h"
#include "music.h"
#include "i18n.h"
#include "notify.h"
#include "platform.h"

#if defined(__ANDROID__)
#include <SDL.h>
#include <SDL_system.h>
#include <android/log.h>
#include <jni.h>
#include <cstdarg>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <cstring>

#include "imgui_internal.h"

using namespace ls;

static ls::Engine* g_engine = nullptr;
static jclass g_cls = nullptr;
static jclass g_notify = nullptr;
static jclass g_musicSvc = nullptr;  // org.lunamoo.lunashare.MusicService  // org.lunamoo.lunashare.Notify (looked up once: other threads can't FindClass app classes)
static JNIEnv* Env();

// static void method of Notify with string / bool arguments; s = signature
static void NotifyV(const char* name, const char* sig, ...) {
    if (!g_notify) return;
    JNIEnv* env = Env();
    jmethodID m = env->GetStaticMethodID(g_notify, name, sig);
    if (!m) { env->ExceptionClear(); return; }
    va_list ap;
    va_start(ap, sig);
    env->CallStaticVoidMethodV(g_notify, m, ap);
    va_end(ap);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static JNIEnv* Env() { return (JNIEnv*)SDL_AndroidGetJNIEnv(); }

static jmethodID M(const char* name, const char* sig) {
    JNIEnv* env = Env();
    return env->GetStaticMethodID(g_cls, name, sig);
}

static std::string JStr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    env->ReleaseStringUTFChars(s, c);
    return r;
}

static bool CallZ(const char* name, const char* sig, ...) {
    JNIEnv* env = Env();
    jmethodID m = M(name, sig);
    if (!m) return false;
    va_list ap;
    va_start(ap, sig);
    jboolean r = env->CallStaticBooleanMethodV(g_cls, m, ap);
    va_end(ap);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return r;
}

static void CallV(const char* name, const char* sig, ...) {
    JNIEnv* env = Env();
    jmethodID m = M(name, sig);
    if (!m) return;
    va_list ap;
    va_start(ap, sig);
    env->CallStaticVoidMethodV(g_cls, m, ap);
    va_end(ap);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static void CallStr1(const char* name, const std::string& a) {
    JNIEnv* env = Env();
    jstring ja = env->NewStringUTF(a.c_str());
    CallV(name, "(Ljava/lang/String;)V", ja);
    env->DeleteLocalRef(ja);
}

std::string AndroidDeviceName() {
    if (!g_cls) return {};
    JNIEnv* env = Env();
    jmethodID m = M("deviceName", "()Ljava/lang/String;");
    if (!m) return {};
    jstring s = (jstring)env->CallStaticObjectMethod(g_cls, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
    std::string r = JStr(env, s);
    if (s) env->DeleteLocalRef(s);
    return r;
}

void AndroidBridgeInit(ls::Engine* engine) {
    g_engine = engine;
    JNIEnv* env = Env();
    jclass c = env->FindClass("org/lunamoo/lunashare/LunaShareActivity");
    if (!c) { env->ExceptionClear(); return; }
    g_cls = (jclass)env->NewGlobalRef(c);
    env->DeleteLocalRef(c);
    CallV("acquireMulticastLock", "()V");
    if (jmethodID mid = M("machineId", "()Ljava/lang/String;")) {  // (before the engine starts the internet)
        jstring s = (jstring)env->CallStaticObjectMethod(g_cls, mid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else ls::SetMachineId(JStr(env, s));
        if (s) env->DeleteLocalRef(s);
    } else {
        env->ExceptionClear();
    }

    HostBridge& b = Bridge();
    b.btAvailable = [] { return CallZ("btAvailable", "()Z"); };
    b.btSetListening = [](bool on) { return CallZ("btSetListening", "(Z)Z", (jboolean)on); };
    b.btDevices = [](bool inquiry) {
        std::vector<BtDevice> out;
        JNIEnv* env = Env();
        jmethodID m = M("btDevices", "(Z)[Ljava/lang/String;");
        if (!m) return out;
        auto arr = (jobjectArray)env->CallStaticObjectMethod(g_cls, m, (jboolean)inquiry);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return out; }
        if (!arr) return out;
        jsize n = env->GetArrayLength(arr);
        for (jsize i = 0; i < n; i++) {
            auto s = (jstring)env->GetObjectArrayElement(arr, i);
            std::string v = JStr(env, s);  // "addr|name|paired"
            env->DeleteLocalRef(s);
            BtDevice d;
            size_t p1 = v.find('|'), p2 = v.rfind('|');
            d.addr = v.substr(0, p1);
            if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1) {
                d.name = v.substr(p1 + 1, p2 - p1 - 1);
                d.paired = v.substr(p2 + 1) == "1";
            }
            out.push_back(d);
        }
        env->DeleteLocalRef(arr);
        return out;
    };
    b.btConnectAsync = [](const std::string& addr) { CallStr1("btConnect", addr); };
    b.hotspotStart = [] { return CallZ("hotspotStart", "()Z"); };
    b.hotspotStop = [] { CallV("hotspotStop", "()V"); };
    b.wifiJoin = [](const std::string& ssid, const std::string& pass) {
        JNIEnv* env = Env();
        jstring a = env->NewStringUTF(ssid.c_str()), p = env->NewStringUTF(pass.c_str());
        CallV("wifiJoin", "(Ljava/lang/String;Ljava/lang/String;)V", a, p);
        env->DeleteLocalRef(a);
        env->DeleteLocalRef(p);
    };
    b.openFile = [](const std::string& path) { CallStr1("openFile", path); };
    b.openFolder = [](const std::string& path) { CallStr1("openFolder", path); };
    b.requestPermissions = [] { CallV("requestPermissions", "()V"); };
    b.callAudioMode = [](bool on) { CallV("callAudioMode", "(Z)V", (jboolean)on); };
    b.callRoutes = []() -> std::string {
        JNIEnv* env = Env();
        jmethodID m = M("callRoutes", "()Ljava/lang/String;");
        if (!m) return {};
        jstring s = (jstring)env->CallStaticObjectMethod(g_cls, m);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
        std::string r = JStr(env, s);
        if (s) env->DeleteLocalRef(s);
        return r;
    };
    b.setCallRoute = [](int id) { CallV("setCallRoute", "(I)V", (jint)id); };
    b.keyboardFraction = []() -> float {
        static jmethodID m = M("keyboardFraction", "()F");
        if (!m) return -1.0f;
        JNIEnv* env = Env();
        jfloat f = env->CallStaticFloatMethod(g_cls, m);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return -1.0f; }
        return f;
    };
    b.javaVm = []() -> void* {
        JavaVM* vm = nullptr;
        Env()->GetJavaVM(&vm);
        return vm;
    };
    b.playerClass = []() -> void* {
        JNIEnv* env = Env();
        jmethodID m = M("playerClass", "()Ljava/lang/Class;");
        if (!m) { env->ExceptionClear(); return nullptr; }
        jobject c = env->CallStaticObjectMethod(g_cls, m);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        if (!c) return nullptr;
        jobject g = env->NewGlobalRef(c);
        env->DeleteLocalRef(c);
        return g;
    };
    b.emojiClass = []() -> void* {
        JNIEnv* env = Env();
        jmethodID m = M("emojiClass", "()Ljava/lang/Class;");
        if (!m) { env->ExceptionClear(); return nullptr; }
        jobject c = env->CallStaticObjectMethod(g_cls, m);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        if (!c) return nullptr;
        jobject g = env->NewGlobalRef(c);
        env->DeleteLocalRef(c);
        return g;
    };
    b.videoClass = []() -> void* {
        JNIEnv* env = Env();
        jmethodID m = M("videoClass", "()Ljava/lang/Class;");
        if (!m) { env->ExceptionClear(); return nullptr; }
        jobject c = env->CallStaticObjectMethod(g_cls, m);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
        if (!c) return nullptr;
        jobject g = env->NewGlobalRef(c);
        env->DeleteLocalRef(c);
        return g;
    };
    // notifications + background service
    jclass nc = env->FindClass("org/lunamoo/lunashare/Notify");
    if (nc) {
        g_notify = (jclass)env->NewGlobalRef(nc);
        env->DeleteLocalRef(nc);
    } else {
        env->ExceptionClear();
    }
    NotifyV("setNativeRunning", "(Z)V", (jboolean) true);
    auto str = [](JNIEnv* e, const std::string& s) { return e->NewStringUTF(s.c_str()); };
    b.notifyMessage = [str](const std::string& key, const std::string& title, const std::string& text, const std::string& av) {
        JNIEnv* e = Env();
        jstring a = str(e, key), t = str(e, title), x = str(e, text), v = str(e, av);
        NotifyV("message", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", a, t, x, v);
        e->DeleteLocalRef(a); e->DeleteLocalRef(t); e->DeleteLocalRef(x); e->DeleteLocalRef(v);
    };
    b.clearChatNotification = [str](const std::string& key) {
        JNIEnv* e = Env();
        jstring a = str(e, key);
        NotifyV("clearChat", "(Ljava/lang/String;)V", a);
        e->DeleteLocalRef(a);
    };
    b.notifyCall = [str](const std::string& key, const std::string& title, bool video, const std::string& av) {
        JNIEnv* e = Env();
        jstring a = str(e, key), t = str(e, title), v = str(e, av), an = str(e, T("Answer")), de = str(e, T("Decline")),
                sub = str(e, video ? T("is video calling you") : T("is calling you"));
        NotifyV("call", "(Ljava/lang/String;Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", a, t,
                (jboolean)video, v, an, de, sub);
        e->DeleteLocalRef(a); e->DeleteLocalRef(t); e->DeleteLocalRef(v); e->DeleteLocalRef(an); e->DeleteLocalRef(de); e->DeleteLocalRef(sub);
    };
    b.cancelCallNotification = [] { NotifyV("cancelCall", "()V"); };
    b.backgroundService = [](bool on) { NotifyV("background", "(Z)V", (jboolean)on); };
    if (jclass mc = env->FindClass("org/lunamoo/lunashare/MusicService")) {
        g_musicSvc = (jclass)env->NewGlobalRef(mc);
        env->DeleteLocalRef(mc);
    } else {
        env->ExceptionClear();
    }
    b.musicState = [](bool active, bool playing, const std::string& title, const std::string& artist, const std::string& album,
                      const std::string& cover, double dur, double pos) {
        if (!g_musicSvc) return;
        JNIEnv* e = Env();
        jmethodID m = e->GetStaticMethodID(g_musicSvc, "update",
                                           "(ZZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJ)V");
        if (!m) {
            e->ExceptionClear();
            return;
        }
        jstring t = e->NewStringUTF(title.c_str()), a = e->NewStringUTF(artist.c_str()), al = e->NewStringUTF(album.c_str()),
                c = e->NewStringUTF(cover.c_str());
        e->CallStaticVoidMethod(g_musicSvc, m, (jboolean)active, (jboolean)playing, t, a, al, c, (jlong)(dur * 1000), (jlong)(pos * 1000));
        if (e->ExceptionCheck()) e->ExceptionClear();
        e->DeleteLocalRef(t);
        e->DeleteLocalRef(a);
        e->DeleteLocalRef(al);
        e->DeleteLocalRef(c);
    };
    b.batteryRestricted = []() -> bool {
        if (!g_notify) return false;
        JNIEnv* e = Env();
        jmethodID m = e->GetStaticMethodID(g_notify, "batteryRestricted", "()Z");
        if (!m) { e->ExceptionClear(); return false; }
        jboolean r = e->CallStaticBooleanMethod(g_notify, m);
        if (e->ExceptionCheck()) { e->ExceptionClear(); return false; }
        return r;
    };
    b.requestBatteryExemption = [] { NotifyV("requestBatteryExemption", "()V"); };
    b.moveToBack = [] { CallV("moveToBack", "()V"); };
    b.movieMode = [](bool on) { CallV("movieMode", "(Z)V", (jboolean)on); };
    b.isMetered = []() -> bool {
        if (!g_notify) return false;
        JNIEnv* e = Env();
        jmethodID m = e->GetStaticMethodID(g_notify, "isMetered", "()Z");
        if (!m) { e->ExceptionClear(); return false; }
        jboolean r = e->CallStaticBooleanMethod(g_notify, m);
        if (e->ExceptionCheck()) { e->ExceptionClear(); return false; }
        return r;
    };
    CallV("requestPermissions", "()V");
}

void AndroidBridgeShutdown() { NotifyV("setNativeRunning", "(Z)V", (jboolean) false); }

extern "C" {

// Java created a Bluetooth socket. We hand back one end of a native socketpair; Java pumps bytes between
// the BluetoothSocket streams and that fd, the engine sees an ordinary stream socket on the other end.
JNIEXPORT jint JNICALL Java_org_lunamoo_lunashare_LunaShareActivity_nativeAdoptBluetooth(JNIEnv* env, jclass, jstring jaddr, jboolean initiator) {
    if (!g_engine) return -1;
    SocketHandle sv[2];
    if (!LocalSocketPair(sv)) return -1;
    g_engine->AdoptSocket(sv[0], Transport::Bluetooth, JStr(env, jaddr), initiator);
    return (jint)sv[1];
}

JNIEXPORT void JNICALL Java_org_lunamoo_lunashare_LunaShareActivity_nativeOnHotspot(JNIEnv* env, jclass, jboolean active, jstring ssid,
                                                                                    jstring pass, jstring error) {
    if (!g_engine) return;
    HotspotInfo h;
    h.active = active;
    h.ssid = JStr(env, ssid);
    h.pass = JStr(env, pass);
    h.error = JStr(env, error);
    g_engine->SetHotspotInfo(h);
}

// a notification was tapped ("open" a chat, "answer" / show a call) or "decline" pressed
JNIEXPORT void JNICALL Java_org_lunamoo_lunashare_LunaShareActivity_nativeOnIntent(JNIEnv* env, jclass, jstring jaction, jstring jkey) {
    std::string a = JStr(env, jaction), key = JStr(env, jkey);
    size_t dot = a.rfind('.');
    std::string what = dot == std::string::npos ? a : a.substr(dot + 1);  // OPEN / ANSWER / DECLINE / CALL
    if (what == "DECLINE") {
        if (g_engine) g_engine->EndCall("declined");
        return;
    }
    notify::Push({what == "ANSWER" ? "answer" : what == "CALL" ? "call" : "open", key});
}

// the music notification, lock screen, headset buttons, audio focus (see MusicService.java)
JNIEXPORT void JNICALL Java_org_lunamoo_lunashare_LunaShareActivity_nativeMusic(JNIEnv*, jclass, jint cmd, jlong arg) {
    auto& p = music::Play();
    switch (cmd) {
        case 1: p.Toggle(); break;
        case 2: p.Next(); break;
        case 3: p.Prev(); break;
        case 4: p.SetPlaying(false); break;
        case 5: p.SetPlaying(true); break;
        case 6: p.Stop(); break;
        case 7: p.Seek((double)arg / 1000.0); break;
        case 8: p.SetVolume((float)arg / 100.0f); break;
    }
}

JNIEXPORT void JNICALL Java_org_lunamoo_lunashare_LunaShareActivity_nativeOnError(JNIEnv* env, jclass, jstring msg) {
    __android_log_print(ANDROID_LOG_WARN, "LunaShare", "%s", JStr(env, msg).c_str());
    if (!g_engine) return;
    HotspotInfo h;
    {
        auto lk = g_engine->Lock();
        h = g_engine->hotspot;
    }
    h.error = JStr(env, msg);
    g_engine->SetHotspotInfo(h);
}

}  // extern "C"

// ---------------------------------------------------------------- system text fallback
// Merged after every font: characters none of our fonts has (Japanese on phones whose CJK font stb_truetype
// can't read, rare scripts, ...) are drawn by Android's own text renderer, which has every system font.
namespace {
std::unordered_map<uint32_t, bool> g_canDraw;

jstring JChar(JNIEnv* env, uint32_t c) {
    jchar w[2];
    int n = 1;
    if (c >= 0x10000) {
        c -= 0x10000;
        w[0] = (jchar)(0xD800 + (c >> 10));
        w[1] = (jchar)(0xDC00 + (c & 0x3FF));
        n = 2;
    } else {
        w[0] = (jchar)c;
    }
    return env->NewString(w, n);
}

bool CanDraw(uint32_t c) {
    auto it = g_canDraw.find(c);
    if (it != g_canDraw.end()) return it->second;
    bool ok = false;
    JNIEnv* env = Env();
    jmethodID m = g_cls ? M("canDrawText", "(Ljava/lang/String;)Z") : nullptr;
    if (m) {
        jstring s = JChar(env, c);
        ok = env->CallStaticBooleanMethod(g_cls, m, s);
        env->DeleteLocalRef(s);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        ok = false;
    }
    g_canDraw[c] = ok;
    return ok;
}

// {w, h, left, top, advance*64, alpha...}
bool RenderChar(uint32_t c, float px, std::vector<int>& out) {
    JNIEnv* env = Env();
    static jmethodID m = nullptr;
    if (!m && g_cls) m = M("renderText", "(Ljava/lang/String;F)[I");
    if (!m) {
        env->ExceptionClear();
        return false;
    }
    jstring s = JChar(env, c);
    auto arr = (jintArray)env->CallStaticObjectMethod(g_cls, m, s, (jfloat)px);
    env->DeleteLocalRef(s);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (!arr) return false;
    jsize n = env->GetArrayLength(arr);
    out.resize((size_t)n);
    if (n > 0) env->GetIntArrayRegion(arr, 0, n, out.data());
    env->DeleteLocalRef(arr);
    return n >= 5 && (size_t)n == 5 + (size_t)out[0] * (size_t)out[1];
}

struct SystemTextLoader {
    static int GlyphIndex(ImFontAtlas*, ImFontConfig*, ImWchar ch) {
        uint32_t c = (uint32_t)ch;
        if (c < 0x80 || c >= 0xF0000 || (c >= 0xD800 && c < 0xE000)) return 0;  // ASCII; emoji module / surrogates
        return CanDraw(c) ? (int)c : 0;
    }
    static bool Contains(ImFontAtlas*, ImFontConfig*, int g) { return g > 0; }
    static bool LoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, int g, ImFontGlyph* out, float* advOnly) {
        float density = src->RasterizerDensity * baked->RasterizerDensity;
        if (density <= 0) density = 1;
        std::vector<int> r;
        if (!RenderChar((uint32_t)g, baked->Size * density, r)) return false;
        float adv = r[4] / 64.0f / density;
        if (advOnly) {
            *advOnly = adv;
            return true;
        }
        out->AdvanceX = adv;
        int w = r[0], h = r[1];
        if (w <= 0 || h <= 0) return true;  // blank (a space of some kind)
        ImFontAtlasRectId id = ImFontAtlasPackAddRect(atlas, w, h);
        if (id == ImFontAtlasRectId_Invalid) return false;
        ImTextureRect* rect = ImFontAtlasPackGetRect(atlas, id);
        std::vector<unsigned char> alpha((size_t)w * h);
        for (size_t i = 0; i < alpha.size(); i++) alpha[i] = (unsigned char)r[5 + i];
        float s = 1.0f / density, x0 = r[2] * s, y0 = r[3] * s + IM_ROUND(baked->Ascent);
        out->X0 = x0;
        out->Y0 = y0;
        out->X1 = x0 + w * s;
        out->Y1 = y0 + h * s;
        out->Visible = true;
        out->Colored = false;  // white + alpha: tinted with the text colour like any other glyph
        out->PackId = id;
        ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, rect, alpha.data(), ImTextureFormat_Alpha8, w);
        return true;
    }
    static const ImFontLoader* Get() {
        static ImFontLoader l;
        if (!l.Name) {
            l.Name = "Android system text";
            l.FontSrcInit = [](ImFontAtlas*, ImFontConfig*) { return true; };
            l.FontSrcGetGlyphIndexFromCodepoint = GlyphIndex;
            l.FontSrcContainsGlyph = Contains;
            l.FontBakedInit = [](ImFontAtlas*, ImFontConfig*, ImFontBaked*, void*) { return true; };
            l.FontBakedLoadGlyph = LoadGlyph;
        }
        return &l;
    }
};
}  // namespace

void AndroidAddSystemTextFont(ImFontAtlas* atlas) {
    if (!g_cls || atlas->Fonts.Size == 0) return;
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.FontLoader = SystemTextLoader::Get();
    strncpy(cfg.Name, "System text", sizeof(cfg.Name) - 1);
    atlas->AddFont(&cfg);
}

#else

void AndroidAddSystemTextFont(ImFontAtlas*) {}
void AndroidBridgeInit(ls::Engine*) {}
void AndroidBridgeShutdown() {}
std::string AndroidDeviceName() { return {}; }

#endif
