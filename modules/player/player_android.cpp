// Player backend on Android: org.lunamoo.lunashare.PlayerEngine (MediaExtractor + MediaCodec) driven through JNI.
// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "player_backend.h"

namespace lsp {
namespace {

JavaVM* g_vm = nullptr;
jclass g_cls = nullptr;

// JNIEnv for this thread; threads the JVM doesn't know are attached once and detached when they end
struct ThreadEnv {
    JNIEnv* env = nullptr;
    bool attached = false;
    ~ThreadEnv() {
        if (attached && g_vm) g_vm->DetachCurrentThread();
    }
};
JNIEnv* Env() {
    thread_local ThreadEnv te;
    if (te.env) return te.env;
    if (!g_vm) return nullptr;
    if (g_vm->GetEnv((void**)&te.env, JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&te.env, nullptr) != JNI_OK) return te.env = nullptr;
        te.attached = true;
    }
    return te.env;
}

void Clear(JNIEnv* e) {
    if (e->ExceptionCheck()) e->ExceptionClear();
}

// natives: `target` is the Sink* of the Step() in progress
void JNICALL NAudio(JNIEnv* env, jclass, jlong target, jobject buf, jint off, jint size, jint rate, jint ch, jboolean isFloat, jlong pts) {
    auto* sink = (Sink*)(intptr_t)target;
    auto* p = (const uint8_t*)env->GetDirectBufferAddress(buf);
    if (!sink || !p || ch <= 0 || !sink->audio) return;
    thread_local std::vector<float> pcm;
    int frames;
    if (isFloat) {
        frames = size / (4 * ch);
        pcm.assign((const float*)(p + off), (const float*)(p + off) + (size_t)frames * ch);
    } else {
        frames = size / (2 * ch);
        pcm.resize((size_t)frames * ch);
        const int16_t* s = (const int16_t*)(p + off);
        for (size_t i = 0; i < pcm.size(); i++) pcm[i] = s[i] / 32768.f;
    }
    sink->audio(pcm.data(), frames, rate, ch, pts / 1e6);
}

void JNICALL NVideo(JNIEnv* env, jclass, jlong target, jobject y, jobject u, jobject v, jint ys, jint uvs, jint ps, jint w, jint h,
                    jint rot, jlong pts) {
    auto* sink = (Sink*)(intptr_t)target;
    if (!sink || !sink->video) return;
    LsvImage im{};
    im.width = w;
    im.height = h;
    im.y = (const uint8_t*)env->GetDirectBufferAddress(y);
    im.u = (const uint8_t*)env->GetDirectBufferAddress(u);
    im.v = (const uint8_t*)env->GetDirectBufferAddress(v);
    if (!im.y || !im.u || !im.v) return;
    im.y_stride = ys;
    im.uv_stride = uvs;
    im.uv_pixel_stride = ps;
    im.rotation = rot;
    sink->video(im, pts / 1e6);
}

class AndroidSource : public Source {
public:
    ~AndroidSource() override {
        JNIEnv* e = Env();
        if (e && obj_) {
            jmethodID m = e->GetMethodID(g_cls, "close", "()V");
            if (m) e->CallVoidMethod(obj_, m);
            Clear(e);
            e->DeleteGlobalRef(obj_);
        }
    }

    bool Open(const std::string& path, LspInfo* info, bool wantAudio, std::string* err) override {
        JNIEnv* e = Env();
        if (!e || !g_cls) {
            *err = "no Java VM";
            return false;
        }
        jmethodID open = e->GetStaticMethodID(g_cls, "open", "(Ljava/lang/String;Z)Lorg/lunamoo/lunashare/PlayerEngine;");
        if (!open) {
            Clear(e);
            *err = "PlayerEngine missing";
            return false;
        }
        jstring js = e->NewStringUTF(path.c_str());
        jobject o = e->CallStaticObjectMethod(g_cls, open, js, (jboolean)wantAudio);
        Clear(e);
        e->DeleteLocalRef(js);
        if (!o) {
            *err = "this phone can't play the file";
            return false;
        }
        obj_ = e->NewGlobalRef(o);
        e->DeleteLocalRef(o);
        jmethodID inf = e->GetMethodID(g_cls, "info", "()Ljava/lang/String;");
        auto s = (jstring)e->CallObjectMethod(obj_, inf);
        Clear(e);
        std::string txt;
        if (s) {
            const char* c = e->GetStringUTFChars(s, nullptr);
            txt = c ? c : "";
            e->ReleaseStringUTFChars(s, c);
            e->DeleteLocalRef(s);
        }
        memset(info, 0, sizeof(*info));
        char vc[64] = {0}, ac[64] = {0};
        sscanf(txt.c_str(), "%lf\t%d\t%d\t%d\t%d\t%d\t%63[^\t]\t%63[^\t]", &info->duration, &info->has_video, &info->has_audio, &info->width,
               &info->height, &info->rotation, vc, ac);
        snprintf(info->video_codec, sizeof(info->video_codec), "%s", vc);
        snprintf(info->audio_codec, sizeof(info->audio_codec), "%s", ac);
        step_ = e->GetMethodID(g_cls, "step", "(J)I");
        seek_ = e->GetMethodID(g_cls, "seek", "(D)V");
        return step_ && seek_;
    }

    int Step(Sink& sink) override {
        JNIEnv* e = Env();
        if (!e) return -1;
        jint r = e->CallIntMethod(obj_, step_, (jlong)(intptr_t)&sink);
        if (e->ExceptionCheck()) {
            e->ExceptionClear();
            return -1;
        }
        return r;
    }

    bool Seek(double t) override {
        JNIEnv* e = Env();
        if (!e) return false;
        e->CallVoidMethod(obj_, seek_, (jdouble)t);
        Clear(e);
        return true;
    }

private:
    jobject obj_ = nullptr;
    jmethodID step_ = nullptr, seek_ = nullptr;
};

}  // namespace

bool InitBackend(const LspHost* host, std::string* err) {
    if (!host->java_vm || !host->java_class) {
        *err = "no Java VM";
        return false;
    }
    g_vm = (JavaVM*)host->java_vm;
    JNIEnv* e = Env();
    if (!e) return false;
    g_cls = (jclass)e->NewGlobalRef((jobject)host->java_class);
    static const JNINativeMethod natives[] = {
        {(char*)"nativeAudio", (char*)"(JLjava/nio/ByteBuffer;IIIIZJ)V", (void*)NAudio},
        {(char*)"nativeVideo", (char*)"(JLjava/nio/ByteBuffer;Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;IIIIIIJ)V", (void*)NVideo},
    };
    if (e->RegisterNatives(g_cls, natives, 2) != 0) {
        Clear(e);
        *err = "cannot register natives";
        return false;
    }
    return true;
}

std::unique_ptr<Source> CreateSource(const LspHost*) { return std::make_unique<AndroidSource>(); }

}  // namespace lsp
