// FFmpeg decoding, libraries loaded at run time. See ffdec.h.
// SPDX-License-Identifier: GPL-3.0-only
#include "ffdec.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace lsv {

namespace {

struct Api {
    const AVCodec* (*find_decoder)(enum AVCodecID);
    AVCodecContext* (*alloc_context3)(const AVCodec*);
    int (*open2)(AVCodecContext*, const AVCodec*, AVDictionary**);
    void (*free_context)(AVCodecContext**);
    int (*send_packet)(AVCodecContext*, const AVPacket*);
    int (*receive_frame)(AVCodecContext*, AVFrame*);
    const AVCodecHWConfig* (*get_hw_config)(const AVCodec*, int);
    AVPacket* (*packet_alloc)(void);
    void (*packet_free)(AVPacket**);
    AVFrame* (*frame_alloc)(void);
    void (*frame_free)(AVFrame**);
    void (*frame_unref)(AVFrame*);
    int (*hwdevice_ctx_create)(AVBufferRef**, enum AVHWDeviceType, const char*, AVDictionary*, int);
    AVBufferRef* (*buffer_ref)(const AVBufferRef*);
    void (*buffer_unref)(AVBufferRef**);
    int (*hwframe_transfer_data)(AVFrame*, const AVFrame*, int);
    void (*log_set_level)(int);
};
Api g_api;
bool g_loaded = false;
std::mutex g_loadM;

void* OpenLib(const std::string& path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    // look for avcodec's own dependency (avutil) next to it, not in the app folder
    return (void*)LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* Sym(void* lib, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

template <class T>
bool Get(void* lib, T& fn, const char* name) {
    fn = (T)Sym(lib, name);
    return fn != nullptr;
}

enum AVPixelFormat PickFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    bool wantHw = ctx->hw_device_ctx != nullptr;
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (wantHw && *p == AV_PIX_FMT_D3D11) return *p;
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_YUV420P || *p == AV_PIX_FMT_YUVJ420P || *p == AV_PIX_FMT_NV12) return *p;
    return fmts[0];
}

}  // namespace

bool FfDecoder::Load(const std::vector<std::string>& dirs, std::string* err) {
    std::lock_guard<std::mutex> l(g_loadM);
    if (g_loaded) return true;
#ifdef _WIN32
    const char* utilName = "avutil-59.dll";
    const char* codecName = "avcodec-61.dll";
    const char* sep = "\\";
#else
    const char* utilName = "libavutil.so.59";
    const char* codecName = "libavcodec.so.61";
    const char* sep = "/";
#endif
    for (auto& d : dirs) {
        std::string base = d.empty() ? "" : d + sep;
        void* util = OpenLib(base + utilName);
        if (!util) continue;
        void* codec = OpenLib(base + codecName);
        if (!codec) continue;
        Api a{};
        bool ok = Get(codec, a.find_decoder, "avcodec_find_decoder") && Get(codec, a.alloc_context3, "avcodec_alloc_context3") &&
                  Get(codec, a.open2, "avcodec_open2") && Get(codec, a.free_context, "avcodec_free_context") &&
                  Get(codec, a.send_packet, "avcodec_send_packet") && Get(codec, a.receive_frame, "avcodec_receive_frame") &&
                  Get(codec, a.get_hw_config, "avcodec_get_hw_config") && Get(codec, a.packet_alloc, "av_packet_alloc") &&
                  Get(codec, a.packet_free, "av_packet_free") && Get(util, a.frame_alloc, "av_frame_alloc") &&
                  Get(util, a.frame_free, "av_frame_free") && Get(util, a.frame_unref, "av_frame_unref") &&
                  Get(util, a.hwdevice_ctx_create, "av_hwdevice_ctx_create") && Get(util, a.buffer_ref, "av_buffer_ref") &&
                  Get(util, a.buffer_unref, "av_buffer_unref") && Get(util, a.hwframe_transfer_data, "av_hwframe_transfer_data") &&
                  Get(util, a.log_set_level, "av_log_set_level");
        if (!ok) continue;
        g_api = a;
        g_api.log_set_level(AV_LOG_ERROR);
        g_loaded = true;
        return true;
    }
    if (err) *err = std::string(codecName) + " not found";
    return false;
}

bool FfDecoder::Loaded() { return g_loaded; }

bool FfDecoder::Open(int codec, bool hw, std::string* err) {
    Close();
    if (!g_loaded) {
        if (err) *err = "FFmpeg not loaded";
        return false;
    }
    AVCodecID id = codec == kHEVC ? AV_CODEC_ID_HEVC : codec == kAV1 ? AV_CODEC_ID_AV1 : AV_CODEC_ID_H264;
    const AVCodec* c = g_api.find_decoder(id);
    if (!c) {
        if (err) *err = std::string("no ") + CodecName(codec) + " decoder";
        return false;
    }
    for (int attempt = hw ? 0 : 1; attempt < 2; attempt++) {
        AVCodecContext* ctx = g_api.alloc_context3(c);
        if (!ctx) return false;
        ctx->get_format = PickFormat;
        AVBufferRef* dev = nullptr;
        if (attempt == 0) {
#ifdef _WIN32
            bool canHw = false;
            for (int i = 0;; i++) {
                const AVCodecHWConfig* hc = g_api.get_hw_config(c, i);
                if (!hc) break;
                if (hc->device_type == AV_HWDEVICE_TYPE_D3D11VA && (hc->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) canHw = true;
            }
            if (!canHw || g_api.hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0) {
                g_api.free_context(&ctx);
                continue;
            }
            ctx->hw_device_ctx = g_api.buffer_ref(dev);
            ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
#else
            g_api.free_context(&ctx);
            continue;
#endif
        } else {
            if (codec == kAV1) {  // FFmpeg's own AV1 decoder only works with a GPU
                g_api.free_context(&ctx);
                if (err) *err = "AV1 needs GPU decoding";
                return false;
            }
            // frame threads: more throughput for big pictures at the cost of a few frames of delay
            ctx->thread_count = (int)std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
            ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        if (g_api.open2(ctx, c, nullptr) < 0) {
            g_api.free_context(&ctx);
            if (dev) g_api.buffer_unref(&dev);
            continue;
        }
        ctx_ = ctx;
        hwdev_ = dev;
        hw_ = attempt == 0;
        codec_ = codec;
        pkt_ = g_api.packet_alloc();
        frame_ = g_api.frame_alloc();
        sw_ = g_api.frame_alloc();
        return true;
    }
    if (err && err->empty()) *err = std::string("cannot open the ") + CodecName(codec) + " decoder";
    return false;
}

void FfDecoder::Close() {
    if (!g_loaded) return;
    if (ctx_) {
        AVCodecContext* c = (AVCodecContext*)ctx_;
        g_api.free_context(&c);
    }
    if (pkt_) {
        AVPacket* p = (AVPacket*)pkt_;
        g_api.packet_free(&p);
    }
    if (frame_) {
        AVFrame* f = (AVFrame*)frame_;
        g_api.frame_free(&f);
    }
    if (sw_) {
        AVFrame* f = (AVFrame*)sw_;
        g_api.frame_free(&f);
    }
    if (hwdev_) {
        AVBufferRef* b = (AVBufferRef*)hwdev_;
        g_api.buffer_unref(&b);
    }
    ctx_ = pkt_ = frame_ = sw_ = hwdev_ = nullptr;
    codec_ = -1;
    hw_ = false;
}

std::string FfDecoder::Name() const {
    if (!ctx_) return "";
    return std::string("FFmpeg ") + CodecName(codec_) + (hw_ ? " (GPU)" : " (CPU)");
}

bool FfDecoder::Decode(const uint8_t* d, size_t n, const std::function<void(const LsvImage&)>& out) {
    if (!ctx_) return false;
    AVCodecContext* ctx = (AVCodecContext*)ctx_;
    AVPacket* pkt = (AVPacket*)pkt_;
    AVFrame* f = (AVFrame*)frame_;
    AVFrame* sw = (AVFrame*)sw_;
    pkt->data = (uint8_t*)d;
    pkt->size = (int)n;
    int r = g_api.send_packet(ctx, pkt);
    pkt->data = nullptr;
    pkt->size = 0;
    if (r < 0 && r != AVERROR(EAGAIN)) return false;
    for (;;) {
        r = g_api.receive_frame(ctx, f);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) return false;
        AVFrame* pic = f;
        if (f->format == AV_PIX_FMT_D3D11) {
            g_api.frame_unref(sw);
            if (g_api.hwframe_transfer_data(sw, f, 0) < 0) {  // GPU -> memory (NV12)
                g_api.frame_unref(f);
                return false;
            }
            pic = sw;
        }
        LsvImage img{};
        img.width = pic->width;
        img.height = pic->height;
        img.y = pic->data[0];
        img.y_stride = pic->linesize[0];
        if (pic->format == AV_PIX_FMT_NV12) {
            img.u = pic->data[1];
            img.v = pic->data[1] + 1;
            img.uv_stride = pic->linesize[1];
            img.uv_pixel_stride = 2;
        } else if (pic->format == AV_PIX_FMT_YUV420P || pic->format == AV_PIX_FMT_YUVJ420P) {
            img.u = pic->data[1];
            img.v = pic->data[2];
            img.uv_stride = pic->linesize[1];
            img.uv_pixel_stride = 1;
        } else {
            img.y = nullptr;  // 10-bit and friends aren't sent by our encoders
        }
        if (img.y) out(img);
        g_api.frame_unref(f);
        if (pic == sw) g_api.frame_unref(sw);
    }
    return true;
}

}  // namespace lsv
