// Android glue: connects the engine's HostBridge hooks to Java (org.lunamoo.lunashare.LunaShareActivity)
// for Bluetooth RFCOMM, LocalOnlyHotspot, joining Wi-Fi networks, multicast lock and opening files.
// On other platforms these functions are no-ops.
#include <string>

#include "engine.h"
#include "i18n.h"
#include "notify.h"

#if defined(__ANDROID__)
#include <SDL.h>
#include <SDL_system.h>
#include <android/log.h>
#include <jni.h>
#include <cstdarg>
#include <unistd.h>

using namespace ls;

static ls::Engine* g_engine = nullptr;
static jclass g_cls = nullptr;
static jclass g_notify = nullptr;  // org.lunamoo.lunashare.Notify (looked up once: other threads can't FindClass app classes)
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

#else

void AndroidBridgeInit(ls::Engine*) {}
void AndroidBridgeShutdown() {}
std::string AndroidDeviceName() { return {}; }

#endif
