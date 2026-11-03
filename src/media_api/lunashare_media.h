/*
 * LunaShare media module interface (C ABI).
 *
 * Calls are optional: the main app only does call signalling, opens the microphone/speaker and moves packets over
 * its existing encrypted connection. Everything codec / DSP related lives in a separately built module
 * (lunashare_media.dll / liblunashare_media.so / liblunashare_media.dylib) that the app loads if it is present.
 * Without the module the app stays a pure chat + file transfer tool and advertises no call capability.
 *
 * Threading: capture() and playback() are called from audio threads, receive() from a network thread, the rest from
 * the UI thread. The module must be thread-safe accordingly. Only one call is active at a time.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LUNASHARE_MEDIA_H
#define LUNASHARE_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSM_API_VERSION 4
#define LSM_SAMPLE_RATE 48000 /* audio exchanged with the host: 48 kHz, mono, signed 16-bit (v4; was 16 kHz) */

#if defined(_WIN32)
#define LSM_EXPORT __declspec(dllexport)
#else
#define LSM_EXPORT __attribute__((visibility("default")))
#endif

typedef struct LsmHost {
    int api_version;
    void* user;
    /* send one media packet to the other side of the active call (any thread) */
    void (*send)(void* user, const uint8_t* data, size_t len);
    void (*log)(void* user, const char* message);
} LsmHost;

typedef struct LsmSettings {
    int echo_cancel;    /* 1 = acoustic echo cancellation (needed when using speakers) */
    int noise_suppress; /* 1 = background noise suppression */
    int auto_gain;      /* 1 = automatic microphone gain */
    int bitrate;        /* bits per second, e.g. 24000 */
} LsmSettings;

typedef struct LsmStats {
    float mic_level;     /* 0..1, after processing */
    float speaker_level; /* 0..1 */
    float loss_percent;  /* packets lost (before concealment) over the last seconds */
    int jitter_ms;       /* arrival jitter estimate */
    int buffered_ms;     /* current playout delay */
    int bitrate_kbps;    /* outgoing */
    int packets_in, packets_out;
} LsmStats;

typedef struct LsmModule {
    int api_version;
    const char* name;    /* "LunaShare Voice" */
    const char* version; /* "1.0" */
    const char* caps;    /* comma separated capabilities, e.g. "voice" */

    int (*start)(const LsmHost* host, const LsmSettings* settings); /* a call became active; returns 0 on success */
    void (*stop)(void);
    void (*capture)(const int16_t* pcm, int samples);   /* microphone samples */
    void (*playback)(int16_t* out, int samples);        /* fill speaker samples */
    void (*receive)(const uint8_t* data, size_t len);   /* packet from the other side */
    void (*set_mute)(int mute);
    void (*set_settings)(const LsmSettings* settings);
    void (*get_stats)(LsmStats* out);
    /* --- since version 2: voice messages, written as Ogg Opus files (independent of calls) --- */
    int (*rec_start)(const char* path_utf8, int bitrate);  /* 0 on success */
    void (*rec_push)(const int16_t* pcm, int samples);      /* 48 kHz mono microphone samples */
    float (*rec_level)(void);                               /* 0..1, for a level meter */
    double (*rec_stop)(int keep);                           /* finishes (or deletes) the file; returns seconds */
    /* --- since version 3: the same recorder, but the Opus packets (20 ms each, 48 kHz timestamps, mono) go to a
     * callback instead of a file (video messages: the host puts them into an MP4). pre_skip receives the decoder
     * delay in 48 kHz samples. Ended with rec_stop(). The callback runs on the thread that calls rec_push. */
    int (*rec_start_stream)(int bitrate, void (*packet)(void* user, const uint8_t* data, int len), void* user, int* pre_skip);
} LsmModule;

/* The only exported symbol. */
typedef const LsmModule* (*LsmGetModuleFn)(void);
LSM_EXPORT const LsmModule* lunashare_media_module(void);

#ifdef __cplusplus
}
#endif

#endif
