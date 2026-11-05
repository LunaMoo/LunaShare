/*
 * LunaShare player module interface (C ABI): plays audio and video files shared in chat.
 *
 * Optional like the call modules: lunashare_player.dll / liblunashare_player.so is loaded when present. It opens a
 * file, decodes it (FFmpeg on desktop, the platform decoders on Android) and hands the host audio in the host's
 * format plus YUV pictures with their time stamps. The host owns the audio device and the clock: it pulls audio
 * from its audio callback and asks for the picture that belongs to the current position.
 *
 * Threading: lsp_* calls on one player may come from the UI thread and the host's audio thread.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LUNASHARE_PLAYER_H
#define LUNASHARE_PLAYER_H

#include <stddef.h>
#include <stdint.h>

#include "lunashare_video.h" /* LsvImage */

#ifdef __cplusplus
extern "C" {
#endif

#define LSP_API_VERSION 2

#if defined(_WIN32)
#define LSP_EXPORT __declspec(dllexport)
#else
#define LSP_EXPORT __attribute__((visibility("default")))
#endif

typedef struct LspHost {
    int api_version;
    void* user;
    void (*log)(void* user, const char* message);
    void* java_vm;    /* Android: JavaVM* */
    void* java_class; /* Android: global reference to org.lunamoo.lunashare.PlayerEngine */
} LspHost;

typedef struct LspInfo {
    double duration;   /* seconds, 0 if unknown */
    int has_video, has_audio;
    int width, height; /* upright display size */
    int rotation;      /* clockwise degrees the pictures need (phone videos) */
    char video_codec[32], audio_codec[32];
} LspInfo;

enum { LSP_PLAYING = 0, LSP_PAUSED = 1, LSP_ENDED = 2, LSP_ERROR = 3 };

typedef struct LspPlayer LspPlayer;

typedef void (*LspImageFn)(void* user, const LsvImage* img);

/* since version 2: a file that is not (all) here yet - "watch together" streams it from another device. read()
 * fills up to len bytes at offset, waiting until they have arrived; returns the bytes read, 0 at the end, < 0 when
 * the stream was cancelled (the player then stops). It is called from the player's decode thread. */
typedef struct LspStream {
    void* user;
    int64_t size;
    int (*read)(void* user, int64_t offset, uint8_t* buf, int len);
} LspStream;

typedef struct LspModule {
    int api_version;
    const char* name;
    const char* version;
    int (*init)(const LspHost* host);
    /* duration, size... and optionally a picture from near the start (for chat thumbnails); 0 = can't play it */
    int (*probe)(const char* path_utf8, LspInfo* info, LspImageFn thumb, void* user);
    /* audio comes out as interleaved float at sample_rate / channels (the host's device format) */
    LspPlayer* (*open)(const char* path_utf8, int sample_rate, int channels, LspInfo* info);
    void (*close)(LspPlayer* p);
    void (*set_playing)(LspPlayer* p, int playing);
    void (*seek)(LspPlayer* p, double seconds);
    /* audio thread: fills `frames` frames (silence when paused/ended); this also advances the clock */
    void (*read_audio)(LspPlayer* p, float* out, int frames);
    /* UI thread: delivers the picture for the current position if it changed since the last call */
    int (*video_frame)(LspPlayer* p, LspImageFn fn, void* user);
    double (*position)(LspPlayer* p);
    int (*state)(LspPlayer* p);
    /* --- since version 2: like open(), but reading through `stream` (which must stay valid until close) */
    LspPlayer* (*open_stream)(const LspStream* stream, int sample_rate, int channels, LspInfo* info);
} LspModule;

typedef const LspModule* (*LspGetModuleFn)(void);
LSP_EXPORT const LspModule* lunashare_player_module(void);

#ifdef __cplusplus
}
#endif
#endif
