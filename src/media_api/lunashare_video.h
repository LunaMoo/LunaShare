/*
 * LunaShare video module interface (C ABI).
 *
 * Like the voice module (lunashare_media.h) this is optional: lunashare_video.dll / liblunashare_video.so is loaded
 * when present and adds the "video" capability. The module owns the camera, the H.264 encoder and decoder (platform
 * hardware codecs where available) and the packet format; the host moves packets over the call connection and draws
 * the images it gets back.
 *
 * Packets the module sends always start with the byte 2 (voice packets start with 1), so the host can route
 * received media by its first byte.
 *
 * Threading: images and packets may be delivered from any thread; everything else is called from the UI thread.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LUNASHARE_VIDEO_H
#define LUNASHARE_VIDEO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSV_API_VERSION 1

#if defined(_WIN32)
#define LSV_EXPORT __declspec(dllexport)
#else
#define LSV_EXPORT __attribute__((visibility("default")))
#endif

/* A YUV 4:2:0 image. uv_pixel_stride 1 = planar (I420), 2 = interleaved (NV12: v = u + 1, NV21: u = v + 1). */
typedef struct LsvImage {
    int width, height;
    const uint8_t* y;
    const uint8_t* u;
    const uint8_t* v;
    int y_stride, uv_stride, uv_pixel_stride;
    int rotation; /* clockwise degrees (0/90/180/270) to turn the image upright */
    int mirror;   /* 1: flip horizontally after rotating (front camera self-view) */
} LsvImage;

enum { LSV_LOCAL = 0, LSV_REMOTE = 1 };

typedef struct LsvHost {
    int api_version;
    void* user;
    /* send one packet to the other side; returns 0 if it was refused (link congested) */
    int (*send)(void* user, const uint8_t* data, size_t len);
    /* a new picture: LSV_LOCAL = own camera preview, LSV_REMOTE = decoded from the peer. Copy before returning. */
    void (*image)(void* user, int source, const LsvImage* img);
    /* the peer turned its camera on/off */
    void (*remote_state)(void* user, int camera_on);
    /* bytes of media waiting to be sent on the call connection (congestion signal) */
    size_t (*queued_bytes)(void* user);
    void (*log)(void* user, const char* message);
    void* java_vm;    /* Android: JavaVM* */
    void* java_class; /* Android: global reference to org.lunamoo.lunashare.VideoEngine */
} LsvHost;

typedef struct LsvSettings {
    int width, height; /* capture size, landscape (the camera's own orientation) */
    int fps;
    int bitrate;      /* target, bits per second */
    int adaptive;     /* 1: lower bitrate / frame rate when the link can't keep up */
} LsvSettings;

enum { LSV_FACING_BACK = 0, LSV_FACING_FRONT = 1, LSV_FACING_EXTERNAL = 2 };

typedef struct LsvCamera {
    char id[64];
    char name[96];
    int facing;
} LsvCamera;

/* Camera controls. Values are floats; booleans are 0/1, enums are indexes into the control's labels. */
enum {
    LSV_CTL_ZOOM = 1,        /* zoom ratio (min may be < 1 on phones with an ultra-wide lens) */
    LSV_CTL_AUTO_EXPOSURE,   /* 1 auto, 0 manual (ISO + shutter) */
    LSV_CTL_EXPOSURE_COMP,   /* EV, used while auto exposure is on */
    LSV_CTL_ISO,
    LSV_CTL_SHUTTER,         /* milliseconds */
    LSV_CTL_AUTO_FOCUS,      /* 1 continuous auto focus, 0 manual */
    LSV_CTL_FOCUS,           /* 0 = infinity .. max = closest */
    LSV_CTL_TORCH,
    LSV_CTL_WHITE_BALANCE,   /* enum, labels: Auto|Daylight|Cloudy|Shade|Tungsten|Fluorescent */
    LSV_CTL_STABILIZATION,
    LSV_CTL_BRIGHTNESS,      /* webcams */
    LSV_CTL_COUNT
};

typedef struct LsvControl {
    int id;
    float min, max, step, value;
} LsvControl;

typedef struct LsvStats {
    int send_width, send_height, send_fps, send_kbps;
    int recv_width, recv_height, recv_fps, recv_kbps;
    int frames_skipped; /* not sent because the link was congested */
    char encoder[64], decoder[64];
} LsvStats;

typedef struct LsvModule {
    int api_version;
    const char* name;
    const char* version;
    /* once after loading; the host struct must stay valid until the program exits */
    int (*init)(const LsvHost* host);
    /* a call with video capability started (decoder ready, camera still off) */
    int (*start)(const LsvSettings* settings);
    void (*stop)(void);
    void (*receive)(const uint8_t* data, size_t len);
    /* turn the camera on/off; returns 0 on failure (no camera, permission denied, ...) */
    int (*set_camera_on)(int on);
    int (*list_cameras)(LsvCamera* out, int max);
    int (*select_camera)(const char* id);
    /* quality change during a call (restarts capture if the size changes) */
    void (*set_settings)(const LsvSettings* settings);
    /* controls the current camera supports (only valid while it's on) */
    int (*get_controls)(LsvControl* out, int max);
    void (*set_control)(int id, float value);
    void (*get_stats)(LsvStats* out);
} LsvModule;

typedef const LsvModule* (*LsvGetModuleFn)(void);
LSV_EXPORT const LsvModule* lunashare_video_module(void);

#ifdef __cplusplus
}
#endif
#endif
