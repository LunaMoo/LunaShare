/*
 * LunaShare emoji module interface (C ABI).
 *
 * Optional, like the voice and video modules: lunashare_emoji.dll / liblunashare_emoji.so is loaded when present.
 * It draws colour emoji with the system's own emoji font (Windows: Segoe UI Emoji through DirectWrite, Android: the
 * system font through android.graphics, Linux/macOS: a colour emoji font through FreeType) and carries the list the
 * emoji picker shows. Without it the app works as before; emoji then show as plain glyphs or boxes.
 *
 * Threading: everything is called from the UI thread.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LUNASHARE_EMOJI_H
#define LUNASHARE_EMOJI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSE_API_VERSION 1

#if defined(_WIN32)
#define LSE_EXPORT __declspec(dllexport)
#else
#define LSE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct LseHost {
    int api_version;
    void* user;
    void (*log)(void* user, const char* message);
    void* java_vm;    /* Android: JavaVM* */
    void* java_class; /* Android: global reference to org.lunamoo.lunashare.EmojiRenderer */
} LseHost;

/* One catalog entry (the picker's list, grouped). */
typedef struct LseEntry {
    const char* emoji; /* UTF-8, fully qualified */
    const char* name;  /* lower case English CLDR name, e.g. "grinning face" */
    int group;         /* index into the group names */
    int skin_tones;    /* 1: a skin tone modifier (U+1F3FB..U+1F3FF) can follow the first code point */
} LseEntry;

typedef struct LseModule {
    int api_version;
    const char* name;
    const char* version;
    /* once after loading; returns 0 if there is nothing to draw emoji with (no emoji font) */
    int (*init)(const LseHost* host);
    /* Draws one emoji (UTF-8, possibly a sequence: ZWJ, skin tone, flag, keycap) centered in a size x size square.
     * Returns a pointer to size*size*4 bytes of RGBA (not premultiplied), valid until the next call, or NULL if the
     * font has no picture for it. */
    const uint8_t* (*render)(const char* utf8, int size);
    /* the picker list */
    int (*group_count)(void);
    const char* (*group_name)(int group);
    int (*entry_count)(void);
    const LseEntry* (*entry)(int index);
} LseModule;

typedef const LseModule* (*LseGetModuleFn)(void);
LSE_EXPORT const LseModule* lunashare_emoji_module(void);

#ifdef __cplusplus
}
#endif
#endif
