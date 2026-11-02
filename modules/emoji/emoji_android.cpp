// Emoji backend for Android: org.lunamoo.lunashare.EmojiRenderer draws with the system emoji font (android.graphics).
// Called from the UI thread, which SDL has already attached to the JVM.
// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>

#include "emoji_backend.h"

namespace lse {
namespace {
JavaVM* g_vm = nullptr;
jclass g_cls = nullptr;
jmethodID g_render = nullptr;

JNIEnv* Env() {
    JNIEnv* e = nullptr;
    if (!g_vm) return nullptr;
    if (g_vm->GetEnv((void**)&e, JNI_VERSION_1_6) != JNI_OK && g_vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return nullptr;
    return e;
}
}  // namespace

bool BackendInit(const LseHost* host, std::string* why) {
    g_vm = (JavaVM*)host->java_vm;
    g_cls = (jclass)host->java_class;
    JNIEnv* e = Env();
    if (!e || !g_cls) {
        *why = "EmojiRenderer class missing";
        return false;
    }
    g_render = e->GetStaticMethodID(g_cls, "render", "(Ljava/lang/String;I)[I");
    if (!g_render) {
        e->ExceptionClear();
        *why = "EmojiRenderer.render missing";
        return false;
    }
    return true;
}

bool BackendRender(const char* utf8, int size, std::vector<uint8_t>& rgba) {
    JNIEnv* e = Env();
    if (!e) return false;
    // NewStringUTF wants modified UTF-8 (no 4-byte sequences), so build the string from UTF-16
    std::u16string w;
    const auto* p = (const unsigned char*)utf8;
    while (*p) {
        uint32_t c = *p;
        int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        c = n == 0 ? c : n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07;
        p++;
        for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++, p++) c = c << 6 | (*p & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            w.push_back((char16_t)(0xD800 + (c >> 10)));
            w.push_back((char16_t)(0xDC00 + (c & 0x3FF)));
        } else {
            w.push_back((char16_t)c);
        }
    }
    jstring s = e->NewString((const jchar*)w.data(), (jsize)w.size());
    auto arr = (jintArray)e->CallStaticObjectMethod(g_cls, g_render, s, (jint)size);
    e->DeleteLocalRef(s);
    if (e->ExceptionCheck()) {
        e->ExceptionClear();
        return false;
    }
    if (!arr) return false;
    bool ok = false;
    if (e->GetArrayLength(arr) == size * size) {
        jint* px = e->GetIntArrayElements(arr, nullptr);
        for (int i = 0; i < size * size; i++) {
            uint32_t v = (uint32_t)px[i];  // ARGB, not premultiplied
            uint8_t* o = &rgba[(size_t)i * 4];
            o[0] = (uint8_t)(v >> 16);
            o[1] = (uint8_t)(v >> 8);
            o[2] = (uint8_t)v;
            o[3] = (uint8_t)(v >> 24);
        }
        e->ReleaseIntArrayElements(arr, px, JNI_ABORT);
        ok = true;
    }
    e->DeleteLocalRef(arr);
    return ok;
}

}  // namespace lse
