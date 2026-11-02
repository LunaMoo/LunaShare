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
#include <libavutil/opt.h>
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
    // optional (only needed by the encoder / dav1d)
    const AVCodec* (*find_decoder_by_name)(const char*);
    const AVCodec* (*find_encoder_by_name)(const char*);
    const AVCodec* (*find_encoder)(enum AVCodecID);
    int (*send_frame)(AVCodecContext*, const AVFrame*);
    int (*receive_packet)(AVCodecContext*, AVPacket*);
    void (*packet_unref)(AVPacket*);
    int (*frame_get_buffer)(AVFrame*, int);
    int (*frame_make_writable)(AVFrame*);
    int (*opt_set)(void*, const char*, const char*, int);
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
        Get(codec, a.find_decoder_by_name, "avcodec_find_decoder_by_name");
        Get(codec, a.find_encoder_by_name, "avcodec_find_encoder_by_name");
        Get(codec, a.find_encoder, "avcodec_find_encoder");
        Get(codec, a.send_frame, "avcodec_send_frame");
        Get(codec, a.receive_packet, "avcodec_receive_packet");
        Get(codec, a.packet_unref, "av_packet_unref");
        Get(util, a.frame_get_buffer, "av_frame_get_buffer");
        Get(util, a.frame_make_writable, "av_frame_make_writable");
        Get(util, a.opt_set, "av_opt_set");
        g_api = a;
        g_api.log_set_level(AV_LOG_ERROR);
        g_loaded = true;
        return true;
    }
    if (err) *err = std::string(codecName) + " not found";
    return false;
}

bool FfDecoder::Loaded() { return g_loaded; }

bool FfDecoder::HasSoftwareAv1() { return g_loaded && g_api.find_decoder_by_name && g_api.find_decoder_by_name("libdav1d"); }

bool FfDecoder::Open(int codec, bool hw, std::string* err) {
    Close();
    if (!g_loaded) {
        if (err) *err = "FFmpeg not loaded";
        return false;
    }
    AVCodecID id = codec == kHEVC ? AV_CODEC_ID_HEVC : codec == kAV1 ? AV_CODEC_ID_AV1 : codec == kMJPEG ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_H264;
    const AVCodec* c = g_api.find_decoder(id);
    const AVCodec* dav1d = codec == kAV1 && !hw && g_api.find_decoder_by_name ? g_api.find_decoder_by_name("libdav1d") : nullptr;
    if (dav1d) c = dav1d;  // software AV1 (Linux builds)
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
            if (codec == kAV1 && !dav1d) {  // FFmpeg's own AV1 decoder only works with a GPU
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
        } else if (pic->format == AV_PIX_FMT_YUV422P || pic->format == AV_PIX_FMT_YUVJ422P) {
            // MJPEG webcams: average the chroma of each two lines into one 4:2:0 picture
            int cw = (pic->width + 1) / 2, ch = (pic->height + 1) / 2;
            conv_.resize((size_t)cw * ch * 2);
            uint8_t* u = conv_.data();
            uint8_t* v = u + (size_t)cw * ch;
            for (int y = 0; y < ch; y++) {
                int y0 = y * 2, y1 = std::min(pic->height - 1, y0 + 1);
                const uint8_t* su0 = pic->data[1] + (size_t)y0 * pic->linesize[1];
                const uint8_t* su1 = pic->data[1] + (size_t)y1 * pic->linesize[1];
                const uint8_t* sv0 = pic->data[2] + (size_t)y0 * pic->linesize[2];
                const uint8_t* sv1 = pic->data[2] + (size_t)y1 * pic->linesize[2];
                for (int x = 0; x < cw; x++) {
                    u[(size_t)y * cw + x] = (uint8_t)((su0[x] + su1[x] + 1) >> 1);
                    v[(size_t)y * cw + x] = (uint8_t)((sv0[x] + sv1[x] + 1) >> 1);
                }
            }
            img.u = u;
            img.v = v;
            img.uv_stride = cw;
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

// ======================================================================== encoder
bool FfEncoder::Open(int width, int height, int fps, int bitrate, std::string* err) {
    Close();
    if (!g_loaded || !g_api.send_frame || !g_api.receive_packet || !g_api.opt_set || !g_api.frame_get_buffer) {
        if (err) *err = "FFmpeg encoder functions missing";
        return false;
    }
    const AVCodec* c = g_api.find_encoder_by_name ? g_api.find_encoder_by_name("libx264") : nullptr;
    if (!c && g_api.find_encoder) c = g_api.find_encoder(AV_CODEC_ID_H264);
    if (!c) {
        if (err) *err = "no H.264 encoder in FFmpeg";
        return false;
    }
    AVCodecContext* ctx = g_api.alloc_context3(c);
    if (!ctx) return false;
    fps_ = std::max(1, fps);
    ctx->width = width & ~1;
    ctx->height = height & ~1;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = AVRational{1, fps_};
    ctx->framerate = AVRational{fps_, 1};
    ctx->bit_rate = bitrate;
    ctx->rc_max_rate = bitrate;
    ctx->rc_buffer_size = std::max(bitrate / 2, 64000);  // ~0.5 s: steady enough, still low delay
    ctx->gop_size = fps_ * 4;                             // keyframes mostly on request
    ctx->max_b_frames = 0;
    ctx->thread_count = 0;                                // x264 picks (sliced threads with zerolatency)
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (ctx->priv_data) {
        int pixels = ctx->width * ctx->height * fps_;
        // fast enough for real time on ordinary desktop CPUs, faster presets for big pictures
        g_api.opt_set(ctx->priv_data, "preset", pixels > 1920 * 1080 * 60 ? "ultrafast" : pixels > 1920 * 1080 * 30 ? "superfast" : "veryfast", 0);
        g_api.opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        g_api.opt_set(ctx->priv_data, "profile", "high", 0);
    }
    if (g_api.open2(ctx, c, nullptr) < 0) {
        g_api.free_context(&ctx);
        if (err) *err = std::string("cannot open ") + c->name;
        return false;
    }
    AVFrame* f = g_api.frame_alloc();
    f->format = AV_PIX_FMT_YUV420P;
    f->width = ctx->width;
    f->height = ctx->height;
    if (g_api.frame_get_buffer(f, 0) < 0) {
        g_api.frame_free(&f);
        g_api.free_context(&ctx);
        if (err) *err = "no memory for frames";
        return false;
    }
    ctx_ = ctx;
    frame_ = f;
    pkt_ = g_api.packet_alloc();
    pts_ = 0;
    bitrate_ = bitrate;
    name_ = std::string(c->name) == "libx264" ? "x264 (software)" : std::string(c->name);
    return true;
}

void FfEncoder::Close() {
    if (!g_loaded) return;
    if (ctx_) {
        AVCodecContext* c = (AVCodecContext*)ctx_;
        g_api.free_context(&c);
    }
    if (frame_) {
        AVFrame* f = (AVFrame*)frame_;
        g_api.frame_free(&f);
    }
    if (pkt_) {
        AVPacket* p = (AVPacket*)pkt_;
        g_api.packet_free(&p);
    }
    ctx_ = frame_ = pkt_ = nullptr;
}

void FfEncoder::SetBitrate(int bps) {
    if (!ctx_ || bps == bitrate_) return;
    AVCodecContext* ctx = (AVCodecContext*)ctx_;
    bitrate_ = bps;
    ctx->bit_rate = bps;  // libx264 reconfigures itself on the next frame
    ctx->rc_max_rate = bps;
    ctx->rc_buffer_size = std::max(bps / 2, 64000);
}

bool FfEncoder::Encode(const LsvImage& img, bool forceKey, const std::function<void(const uint8_t*, size_t, bool)>& out) {
    if (!ctx_) return false;
    AVCodecContext* ctx = (AVCodecContext*)ctx_;
    AVFrame* f = (AVFrame*)frame_;
    AVPacket* pkt = (AVPacket*)pkt_;
    if (g_api.frame_make_writable && g_api.frame_make_writable(f) < 0) return false;
    int w = std::min(img.width, f->width), h = std::min(img.height, f->height);
    for (int y = 0; y < h; y++) memcpy(f->data[0] + (size_t)y * f->linesize[0], img.y + (size_t)y * img.y_stride, (size_t)w);
    for (int y = 0; y < h / 2; y++) {
        uint8_t* du = f->data[1] + (size_t)y * f->linesize[1];
        uint8_t* dv = f->data[2] + (size_t)y * f->linesize[2];
        const uint8_t* su = img.u + (size_t)y * img.uv_stride;
        const uint8_t* sv = img.v + (size_t)y * img.uv_stride;
        if (img.uv_pixel_stride == 1) {
            memcpy(du, su, (size_t)w / 2);
            memcpy(dv, sv, (size_t)w / 2);
        } else {
            for (int x = 0; x < w / 2; x++) {
                du[x] = su[x * img.uv_pixel_stride];
                dv[x] = sv[x * img.uv_pixel_stride];
            }
        }
    }
    f->pts = pts_++;
    f->pict_type = forceKey ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (forceKey) f->flags |= AV_FRAME_FLAG_KEY;
    else f->flags &= ~AV_FRAME_FLAG_KEY;
    if (g_api.send_frame(ctx, f) < 0) return false;
    for (;;) {
        int r = g_api.receive_packet(ctx, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) return false;
        out(pkt->data, (size_t)pkt->size, (pkt->flags & AV_PKT_FLAG_KEY) != 0);
        g_api.packet_unref(pkt);
    }
    return true;
}

}  // namespace lsv
