// Video backend for Android: forwards to org.lunamoo.lunashare.VideoEngine (Camera2 + MediaCodec, Java side in
// android/app/src/calls/java). Java calls back through natives registered here.
// SPDX-License-Identifier: GPL-3.0-only
#include <android/log.h>
#include <jni.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include "video_backend.h"

namespace lsv {
namespace {

JavaVM* g_vm = nullptr;
jclass g_cls = nullptr;
Sink* g_sink = nullptr;

// JNIEnv for the current thread; attaches threads the JVM doesn't know (detached again when the scope ends)
struct Env {
    JNIEnv* env = nullptr;
    bool attached = false;
    Env() {
        if (!g_vm) return;
        if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
            else env = nullptr;
        }
    }
    ~Env() {
        if (attached) g_vm->DetachCurrentThread();
    }
    JNIEnv* operator->() { return env; }
    explicit operator bool() const { return env != nullptr; }
};

jmethodID M(JNIEnv* e, const char* name, const char* sig) { return e->GetStaticMethodID(g_cls, name, sig); }

void Clear(JNIEnv* e) {
    if (e->ExceptionCheck()) {
        e->ExceptionDescribe();
        e->ExceptionClear();
    }
}

std::string CallStr(const char* name) {
    Env e;
    if (!e || !g_cls) return {};
    jmethodID m = M(e.env, name, "()Ljava/lang/String;");
    if (!m) { Clear(e.env); return {}; }
    auto s = (jstring)e->CallStaticObjectMethod(g_cls, m);
    Clear(e.env);
    if (!s) return {};
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    e->ReleaseStringUTFChars(s, c);
    e->DeleteLocalRef(s);
    return r;
}

// ------------------------------------------------------------------------ natives called by Java
void JNICALL NEncoded(JNIEnv* env, jclass, jobject buf, jint off, jint size, jboolean key, jint w, jint h, jint rot, jint codec) {
    auto* p = (const uint8_t*)env->GetDirectBufferAddress(buf);
    if (!p || !g_sink) return;
    EncodedFrame f;
    f.data = p + off;
    f.len = (size_t)size;
    f.key = key;
    f.width = w;
    f.height = h;
    f.rotation = rot;
    f.codec = codec;
    g_sink->OnEncoded(f);
}

void JNICALL NImage(JNIEnv* env, jclass, jint source, jobject y, jobject u, jobject v, jint ys, jint uvs, jint uvps, jint w,
                    jint h, jint rot, jboolean mirror) {
    if (!g_sink) return;
    LsvImage img{};
    img.width = w;
    img.height = h;
    img.y = (const uint8_t*)env->GetDirectBufferAddress(y);
    img.u = (const uint8_t*)env->GetDirectBufferAddress(u);
    img.v = (const uint8_t*)env->GetDirectBufferAddress(v);
    if (!img.y || !img.u || !img.v) return;
    img.y_stride = ys;
    img.uv_stride = uvs;
    img.uv_pixel_stride = uvps;
    img.rotation = rot;
    img.mirror = mirror ? 1 : 0;
    if (source == 0) g_sink->OnLocalImage(img);
    else g_sink->OnDecoded(img);
}

void JNICALL NDecoderError(JNIEnv*, jclass) {
    if (g_sink) g_sink->OnDecoderError();
}
void JNICALL NCameraError(JNIEnv* env, jclass, jstring msg) {
    const char* c = env->GetStringUTFChars(msg, nullptr);
    if (g_sink) g_sink->OnCameraError(c ? c : "");
    env->ReleaseStringUTFChars(msg, c);
}
void JNICALL NLog(JNIEnv* env, jclass, jstring msg) {
    const char* c = env->GetStringUTFChars(msg, nullptr);
    __android_log_print(ANDROID_LOG_INFO, "LunaShare", "video: %s", c ? c : "");
    if (g_sink) g_sink->Log(c ? c : "");
    env->ReleaseStringUTFChars(msg, c);
}

class AndroidBackend : public Backend {
public:
    explicit AndroidBackend(Sink* s) : sink_(s) {}

    std::vector<LsvCamera> Cameras() override {
        std::vector<LsvCamera> out;
        std::istringstream in(CallStr("cameras"));
        std::string line;
        while (std::getline(in, line)) {
            size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos) continue;
            LsvCamera c{};
            snprintf(c.id, sizeof(c.id), "%s", line.substr(0, a).c_str());
            snprintf(c.name, sizeof(c.name), "%s", line.substr(a + 1, b - a - 1).c_str());
            c.facing = atoi(line.substr(b + 1).c_str());
            out.push_back(c);
        }
        return out;
    }

    unsigned EncodeCodecs() override { return (unsigned)CallI("encoderCodecs"); }
    unsigned DecodeCodecs() override { return (unsigned)CallI("decoderCodecs"); }

    bool StartCamera(const std::string& id, const LsvSettings& s, int codec, std::string* err) override {
        Env e;
        if (!e || !g_cls) { if (err) *err = "no Java VM"; return false; }
        jmethodID m = M(e.env, "startCamera", "(Ljava/lang/String;IIIII)Z");
        if (!m) { Clear(e.env); if (err) *err = "VideoEngine.startCamera missing"; return false; }
        jstring js = e->NewStringUTF(id.c_str());
        jboolean ok = e->CallStaticBooleanMethod(g_cls, m, js, s.width, s.height, s.fps, s.bitrate, codec);
        Clear(e.env);
        e->DeleteLocalRef(js);
        if (!ok && err) *err = "camera could not be opened (permission?)";
        return ok;
    }

    void StopCamera() override { CallV("stopCamera"); }

    void SetBitrate(int bps) override {
        Env e;
        if (!e || !g_cls) return;
        if (jmethodID m = M(e.env, "setBitrate", "(I)V")) e->CallStaticVoidMethod(g_cls, m, bps);
        Clear(e.env);
    }

    void RequestKeyframe() override { CallV("requestKeyframe"); }

    std::vector<LsvControl> Controls() override {
        std::vector<LsvControl> out;
        std::istringstream in(CallStr("controls"));
        std::string line;
        while (std::getline(in, line)) {
            LsvControl c{};
            if (sscanf(line.c_str(), "%d\t%f\t%f\t%f\t%f", &c.id, &c.min, &c.max, &c.step, &c.value) == 5) out.push_back(c);
        }
        return out;
    }

    void SetControl(int id, float v) override {
        Env e;
        if (!e || !g_cls) return;
        if (jmethodID m = M(e.env, "setControl", "(IF)V")) e->CallStaticVoidMethod(g_cls, m, id, v);
        Clear(e.env);
    }

    // the decode thread stays attached to the JVM for its whole life
    bool StartDecoder() override {
        if (!g_vm || !g_cls) return false;
        if (g_vm->AttachCurrentThread(&decEnv_, nullptr) != JNI_OK) return false;
        decode_ = M(decEnv_, "decode", "(Ljava/nio/ByteBuffer;IZIII)V");
        Clear(decEnv_);
        return decode_ != nullptr;
    }

    void Decode(const uint8_t* d, size_t n, bool key, int w, int h, int codec) override {
        if (!decEnv_ || !decode_) return;
        jobject buf = decEnv_->NewDirectByteBuffer((void*)d, (jlong)n);
        decEnv_->CallStaticVoidMethod(g_cls, decode_, buf, (jint)n, (jboolean)key, w, h, (jint)codec);
        Clear(decEnv_);
        decEnv_->DeleteLocalRef(buf);
    }

    void StopDecoder() override {
        if (!decEnv_) return;
        if (jmethodID m = M(decEnv_, "stopDecoder", "()V")) decEnv_->CallStaticVoidMethod(g_cls, m);
        Clear(decEnv_);
        decEnv_ = nullptr;
        g_vm->DetachCurrentThread();
    }

    std::string EncoderName() override { return CallStr("encoderName"); }
    std::string DecoderName() override { return CallStr("decoderName"); }

private:
    int CallI(const char* name) {
        Env e;
        if (!e || !g_cls) return 1;
        jmethodID m = M(e.env, name, "()I");
        if (!m) { Clear(e.env); return 1; }
        jint r = e->CallStaticIntMethod(g_cls, m);
        Clear(e.env);
        return (int)r;
    }
    void CallV(const char* name) {
        Env e;
        if (!e || !g_cls) return;
        if (jmethodID m = M(e.env, name, "()V")) e->CallStaticVoidMethod(g_cls, m);
        Clear(e.env);
    }

    Sink* sink_;
    JNIEnv* decEnv_ = nullptr;
    jmethodID decode_ = nullptr;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend(Sink* sink, const LsvHost* host) {
    if (!host || !host->java_vm || !host->java_class) return nullptr;  // the lite build has no VideoEngine
    g_vm = (JavaVM*)host->java_vm;
    Env e;
    if (!e) return nullptr;
    g_cls = (jclass)e->NewGlobalRef((jobject)host->java_class);
    static const JNINativeMethod natives[] = {
        {(char*)"nativeEncoded", (char*)"(Ljava/nio/ByteBuffer;IIZIIII)V", (void*)NEncoded},
        {(char*)"nativeImage", (char*)"(ILjava/nio/ByteBuffer;Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;IIIIIIZ)V", (void*)NImage},
        {(char*)"nativeDecoderError", (char*)"()V", (void*)NDecoderError},
        {(char*)"nativeCameraError", (char*)"(Ljava/lang/String;)V", (void*)NCameraError},
        {(char*)"nativeLog", (char*)"(Ljava/lang/String;)V", (void*)NLog},
    };
    if (e->RegisterNatives(g_cls, natives, sizeof(natives) / sizeof(natives[0])) != 0) {
        Clear(e.env);
        return nullptr;
    }
    g_sink = sink;
    return std::make_unique<AndroidBackend>(sink);
}

}  // namespace lsv
