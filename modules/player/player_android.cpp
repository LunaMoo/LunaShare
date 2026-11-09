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

void JNICALL NSubtitle(JNIEnv* env, jclass, jlong target, jstring text, jlong pts) {
    auto* sink = (Sink*)(intptr_t)target;
    if (!sink || !sink->subtitle || !text) return;
    const char* c = env->GetStringUTFChars(text, nullptr);
    std::string t = c ? c : "";
    if (c) env->ReleaseStringUTFChars(text, c);
    static const std::vector<uint8_t> none;
    sink->subtitle(pts / 1e6, -1, t, none, 0, 0, 0, 0, 0, 0);
}

jint JNICALL NRead(JNIEnv* env, jclass, jlong stream, jlong pos, jbyteArray buf, jint off, jint size) {
    auto* s = (const LspStream*)(intptr_t)stream;
    if (!s || size <= 0) return -1;
    thread_local std::vector<uint8_t> tmp;
    tmp.resize((size_t)size);
    int n = s->read(s->user, (int64_t)pos, tmp.data(), size);
    if (n > 0) env->SetByteArrayRegion(buf, off, n, (const jbyte*)tmp.data());
    return n;
}

void JNICALL NVideo(JNIEnv* env, jclass, jlong target, jobject y, jobject u, jobject v, jint ys, jint uvs, jint ps, jint yps, jint cx, jint cy,
                    jint w, jint h, jint rot, jlong pts) {
    auto* sink = (Sink*)(intptr_t)target;
    if (!sink || !sink->video || w <= 0 || h <= 0) return;
    const uint8_t* py = (const uint8_t*)env->GetDirectBufferAddress(y);
    const uint8_t* pu = (const uint8_t*)env->GetDirectBufferAddress(u);
    const uint8_t* pv = (const uint8_t*)env->GetDirectBufferAddress(v);
    if (!py || !pu || !pv) return;
    cx &= ~1;  // (the crop's corner, on a whole chroma sample)
    cy &= ~1;
    LsvImage im{};
    im.width = w;
    im.height = h;
    if (yps >= 2) {
        // 10-bit (P010 and the like): 16 bits per sample, the value in the high bits - keep the high byte. Without
        // this the picture comes out green (the low bytes read as colour).
        int cw = (w + 1) / 2, ch = (h + 1) / 2;
        thread_local std::vector<uint8_t> y8, u8, v8;
        y8.resize((size_t)w * h);
        u8.resize((size_t)cw * ch);
        v8.resize((size_t)cw * ch);
        for (int r = 0; r < h; r++) {
            const uint8_t* s = py + (size_t)(r + cy) * ys + (size_t)cx * yps + 1;
            uint8_t* d = &y8[(size_t)r * w];
            for (int c = 0; c < w; c++) d[c] = s[(size_t)c * yps];
        }
        for (int r = 0; r < ch; r++) {
            const uint8_t* su = pu + (size_t)(r + cy / 2) * uvs + (size_t)(cx / 2) * ps + 1;
            const uint8_t* sv = pv + (size_t)(r + cy / 2) * uvs + (size_t)(cx / 2) * ps + 1;
            for (int c = 0; c < cw; c++) {
                u8[(size_t)r * cw + c] = su[(size_t)c * ps];
                v8[(size_t)r * cw + c] = sv[(size_t)c * ps];
            }
        }
        im.y = y8.data();
        im.u = u8.data();
        im.v = v8.data();
        im.y_stride = w;
        im.uv_stride = cw;
        im.uv_pixel_stride = 1;
    } else {
        im.y = py + (size_t)cy * ys + cx;
        im.u = pu + (size_t)(cy / 2) * uvs + (size_t)(cx / 2) * ps;
        im.v = pv + (size_t)(cy / 2) * uvs + (size_t)(cx / 2) * ps;
        im.y_stride = ys;
        im.uv_stride = uvs;
        im.uv_pixel_stride = ps;
    }
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

    bool OpenStream(const LspStream* s, LspInfo* info, bool wantAudio, std::string* err) override {
        stream_ = *s;
        return OpenWith(nullptr, info, wantAudio, err);
    }

    bool Open(const std::string& path, LspInfo* info, bool wantAudio, std::string* err) override { return OpenWith(&path, info, wantAudio, err); }

    bool OpenWith(const std::string* path, LspInfo* info, bool wantAudio, std::string* err) {
        JNIEnv* e = Env();
        if (!e || !g_cls) {
            *err = "no Java VM";
            return false;
        }
        jobject o = nullptr;
        if (path) {
            jmethodID open = e->GetStaticMethodID(g_cls, "open", "(Ljava/lang/String;Z)Lorg/lunamoo/lunashare/PlayerEngine;");
            if (!open) {
                Clear(e);
                *err = "PlayerEngine missing";
                return false;
            }
            jstring js = e->NewStringUTF(path->c_str());
            o = e->CallStaticObjectMethod(g_cls, open, js, (jboolean)wantAudio);
            Clear(e);
            e->DeleteLocalRef(js);
        } else {
            jmethodID open = e->GetStaticMethodID(g_cls, "openStream", "(JJZ)Lorg/lunamoo/lunashare/PlayerEngine;");
            if (!open) {
                Clear(e);
                *err = "PlayerEngine missing";
                return false;
            }
            o = e->CallStaticObjectMethod(g_cls, open, (jlong)(intptr_t)&stream_, (jlong)stream_.size, (jboolean)wantAudio);
            Clear(e);
        }
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

    std::vector<TrackInfo> Tracks() override {
        std::vector<TrackInfo> out;
        JNIEnv* e = Env();
        if (!e || !obj_) return out;
        jmethodID m = e->GetMethodID(g_cls, "tracks", "()Ljava/lang/String;");
        if (!m) { Clear(e); return out; }
        auto s = (jstring)e->CallObjectMethod(obj_, m);
        Clear(e);
        if (!s) return out;
        const char* c = e->GetStringUTFChars(s, nullptr);
        std::string txt = c ? c : "";
        if (c) e->ReleaseStringUTFChars(s, c);
        e->DeleteLocalRef(s);
        size_t a = 0;
        while (a < txt.size()) {
            size_t nl = txt.find('\n', a);
            std::string line = txt.substr(a, nl == std::string::npos ? std::string::npos : nl - a);
            a = nl == std::string::npos ? txt.size() : nl + 1;
            std::vector<std::string> f;
            size_t b = 0;
            for (;;) {
                size_t t = line.find('\t', b);
                f.push_back(line.substr(b, t == std::string::npos ? std::string::npos : t - b));
                if (t == std::string::npos) break;
                b = t + 1;
            }
            if (f.size() < 8) continue;
            TrackInfo ti;
            ti.kind = atoi(f[0].c_str());
            ti.id = atoi(f[1].c_str());
            ti.lang = f[2];
            ti.title = f[3];
            ti.codec = f[4];
            ti.channels = atoi(f[5].c_str());
            ti.playable = f[6] == "1";
            ti.selected = f[7] == "1";
            out.push_back(ti);
        }
        return out;
    }

    bool SelectAudio(int id) override { return CallBool("selectAudio", id); }
    bool SelectSubtitle(int id) override { return CallBool("selectSubtitle", id); }

    bool CallBool(const char* name, int arg) {
        JNIEnv* e = Env();
        if (!e || !obj_) return false;
        jmethodID m = e->GetMethodID(g_cls, name, "(I)Z");
        if (!m) { Clear(e); return false; }
        jboolean r = e->CallBooleanMethod(obj_, m, (jint)arg);
        Clear(e);
        return r;
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
    LspStream stream_{};
};


#ifdef LSP_FF_SIDE
// The phone's decoder for the picture, FFmpeg for sound and subtitles: phones often can't decode AC-3 / DTS
// (licensing), and Android reads no ASS or Blu-ray subtitles from MKV files at all.
class HybridSource : public Source {
public:
    bool Open(const std::string& path, LspInfo* info, bool wantAudio, std::string* err) override {
        if (wantAudio) OpenSide([&](Source& s, LspInfo* i, std::string* e) { return s.Open(path, i, true, e); });
        return OpenMain([&](Source& s, LspInfo* i, bool a, std::string* e) { return s.Open(path, i, a, e); }, info, wantAudio, err);
    }
    bool OpenStream(const LspStream* st, LspInfo* info, bool wantAudio, std::string* err) override {
        if (wantAudio) OpenSide([&](Source& s, LspInfo* i, std::string* e) { return s.OpenStream(st, i, true, e); });
        return OpenMain([&](Source& s, LspInfo* i, bool a, std::string* e) { return s.OpenStream(st, i, a, e); }, info, wantAudio, err);
    }
    int Step(Sink& sink) override { return StepNeeds(sink, true, true); }
    int StepNeeds(Sink& sink, bool needVideo, bool needAudio) override {
        if (!sideOk_) return mainOk_ ? main_.Step(sink) : 0;
        if (!mainDone_ && (needVideo || sideDone_)) {
            int r = main_.Step(sink);
            if (r <= 0) mainDone_ = true;
        }
        if (!sideDone_ && (needAudio || !sideAudio_ || mainDone_)) {
            int r = side_->Step(sink);
            if (r <= 0) sideDone_ = true;
        }
        return mainDone_ && sideDone_ ? 0 : 1;
    }
    bool Seek(double t) override {
        mainDone_ = !mainOk_ || (sideAudio_ && !mainVideo_);
        sideDone_ = false;
        bool ok = mainOk_ ? main_.Seek(t) : true;
        if (sideOk_) side_->Seek(t);
        return ok;
    }
    std::vector<TrackInfo> Tracks() override { return sideOk_ ? side_->Tracks() : main_.Tracks(); }
    std::vector<std::pair<double, std::string>> Chapters() override { return sideOk_ ? side_->Chapters() : main_.Chapters(); }
    bool SelectAudio(int id) override { return sideAudio_ ? side_->SelectAudio(id) : main_.SelectAudio(id); }
    bool SelectSubtitle(int id) override { return sideOk_ ? side_->SelectSubtitle(id) : main_.SelectSubtitle(id); }

private:
    template <class F>
    void OpenSide(F open) {
        std::string e;
        if (!InitFf(&e)) return;
        side_ = CreateFfSource();
        LspInfo si{};
        sideOk_ = side_ && open(*side_, &si, &e);
        if (!sideOk_) {
            side_.reset();
            return;
        }
        sideAudio_ = si.has_audio;
        sideInfo_ = si;
    }
    template <class F>
    bool OpenMain(F open, LspInfo* info, bool wantAudio, std::string* err) {
        // the phone's own sound only when FFmpeg has none to give
        bool ok = open(main_, info, wantAudio && !sideAudio_, err);
        if (!ok && !sideAudio_) return false;
        if (!ok) memset(info, 0, sizeof(*info));
        if (sideAudio_) {
            info->has_audio = 1;
            memcpy(info->audio_codec, sideInfo_.audio_codec, sizeof(info->audio_codec));
            if (info->duration <= 0) info->duration = sideInfo_.duration;
        }
        mainOk_ = ok;
        mainVideo_ = ok && info->has_video;
        mainDone_ = !ok || (sideAudio_ && !mainVideo_);  // (sound-only file: FFmpeg does it all)
        return true;
    }
    AndroidSource main_;
    std::unique_ptr<Source> side_;
    LspInfo sideInfo_{};
    bool sideOk_ = false, sideAudio_ = false, mainOk_ = false, mainVideo_ = false, mainDone_ = false, sideDone_ = false;
};
#endif
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
        {(char*)"nativeVideo", (char*)"(JLjava/nio/ByteBuffer;Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;IIIIIIIIIJ)V", (void*)NVideo},
        {(char*)"nativeRead", (char*)"(JJ[BII)I", (void*)NRead},
        {(char*)"nativeSubtitle", (char*)"(JLjava/lang/String;J)V", (void*)NSubtitle},
    };
    if (e->RegisterNatives(g_cls, natives, 4) != 0) {
        Clear(e);
        *err = "cannot register natives";
        return false;
    }
    return true;
}

std::unique_ptr<Source> CreateSource(const LspHost*) {
#ifdef LSP_FF_SIDE
    return std::make_unique<HybridSource>();
#else
    return std::make_unique<AndroidSource>();
#endif
}

}  // namespace lsp
