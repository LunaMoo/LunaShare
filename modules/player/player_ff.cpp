// Player backend on FFmpeg (Windows; also Linux for development). Libraries are loaded at run time from the
// module's folder: avformat-61, avcodec-61, avutil-59 (the same trimmed LGPL build the video module uses).
// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
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

#include "player_backend.h"

namespace lsp {
namespace {

struct Api {
    // avformat
    int (*open_input)(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**);
    int (*find_stream_info)(AVFormatContext*, AVDictionary**);
    int (*find_best_stream)(AVFormatContext*, enum AVMediaType, int, int, const AVCodec**, int);
    int (*read_frame)(AVFormatContext*, AVPacket*);
    int (*seek_file)(AVFormatContext*, int, int64_t, int64_t, int64_t, int);
    void (*close_input)(AVFormatContext**);
    // avcodec
    AVCodecContext* (*alloc_context3)(const AVCodec*);
    int (*params_to_context)(AVCodecContext*, const AVCodecParameters*);
    int (*open2)(AVCodecContext*, const AVCodec*, AVDictionary**);
    void (*free_context)(AVCodecContext**);
    int (*send_packet)(AVCodecContext*, const AVPacket*);
    int (*receive_frame)(AVCodecContext*, AVFrame*);
    void (*flush_buffers)(AVCodecContext*);
    const AVCodecHWConfig* (*get_hw_config)(const AVCodec*, int);
    AVPacket* (*packet_alloc)(void);
    void (*packet_free)(AVPacket**);
    void (*packet_unref)(AVPacket*);
    const AVPacketSideData* (*side_data_get)(const AVPacketSideData*, int, enum AVPacketSideDataType);
    const char* (*codec_get_name)(enum AVCodecID);
    // avutil
    AVFrame* (*frame_alloc)(void);
    void (*frame_free)(AVFrame**);
    void (*frame_unref)(AVFrame*);
    int (*hwdevice_ctx_create)(AVBufferRef**, enum AVHWDeviceType, const char*, AVDictionary*, int);
    AVBufferRef* (*buffer_ref)(const AVBufferRef*);
    void (*buffer_unref)(AVBufferRef**);
    int (*hwframe_transfer_data)(AVFrame*, const AVFrame*, int);
    double (*display_rotation_get)(const int32_t*);
    void (*log_set_level)(int);
    // custom input (streams)
    AVFormatContext* (*format_alloc_context)(void);
    AVIOContext* (*avio_alloc_context)(unsigned char*, int, int, void*, int (*)(void*, uint8_t*, int), int (*)(void*, const uint8_t*, int),
                                       int64_t (*)(void*, int64_t, int));
    void (*avio_context_free)(AVIOContext**);
    void* (*malloc_)(size_t);
    void (*freep)(void*);
};
Api A;
std::mutex g_m;
bool g_ok = false;

std::string ModuleDir() {
#ifdef _WIN32
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&ModuleDir, &self);
    wchar_t p[MAX_PATH] = {0};
    GetModuleFileNameW(self, p, MAX_PATH);
    int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, p, -1, &s[0], n, nullptr, nullptr);
    size_t k = s.find_last_of("\\/");
    return k == std::string::npos ? "." : s.substr(0, k);
#else
    Dl_info info{};
    if (dladdr((void*)&ModuleDir, &info) && info.dli_fname) {
        std::string s = info.dli_fname;
        size_t k = s.find_last_of('/');
        return k == std::string::npos ? "." : s.substr(0, k);
    }
    return ".";
#endif
}

void* OpenLib(const std::string& path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    return (void*)LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    return h;
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
bool G(void* lib, T& fn, const char* name) {
    fn = (T)Sym(lib, name);
    return fn != nullptr;
}

bool LoadFf(std::string* err) {
    std::lock_guard<std::mutex> l(g_m);
    if (g_ok) return true;
#ifdef _WIN32
    const char *u = "avutil-59.dll", *c = "avcodec-61.dll", *f = "avformat-61.dll", *r = "swresample-5.dll";
    const char* sep = "\\";
#else
    const char *u = "libavutil.so.59", *c = "libavcodec.so.61", *f = "libavformat.so.61", *r = "libswresample.so.5";
    const char* sep = "/";
#endif
    std::vector<std::string> dirs = {ModuleDir()};
    if (const char* d = getenv("LUNASHARE_FFMPEG_LIB")) dirs.insert(dirs.begin(), d);
    for (auto& d : dirs) {
        void* lu = OpenLib(d + sep + u);
        if (lu) OpenLib(d + sep + r);  // avcodec needs it (Opus); load it from here first
        void* lc = lu ? OpenLib(d + sep + c) : nullptr;
        void* lf = lc ? OpenLib(d + sep + f) : nullptr;
        if (!lf) continue;
        Api a{};
        bool ok = G(lf, a.open_input, "avformat_open_input") && G(lf, a.find_stream_info, "avformat_find_stream_info") &&
                  G(lf, a.find_best_stream, "av_find_best_stream") && G(lf, a.read_frame, "av_read_frame") &&
                  G(lf, a.seek_file, "avformat_seek_file") && G(lf, a.close_input, "avformat_close_input") &&
                  G(lc, a.alloc_context3, "avcodec_alloc_context3") && G(lc, a.params_to_context, "avcodec_parameters_to_context") &&
                  G(lc, a.open2, "avcodec_open2") && G(lc, a.free_context, "avcodec_free_context") &&
                  G(lc, a.send_packet, "avcodec_send_packet") && G(lc, a.receive_frame, "avcodec_receive_frame") &&
                  G(lc, a.flush_buffers, "avcodec_flush_buffers") && G(lc, a.get_hw_config, "avcodec_get_hw_config") &&
                  G(lc, a.packet_alloc, "av_packet_alloc") && G(lc, a.packet_free, "av_packet_free") &&
                  G(lc, a.packet_unref, "av_packet_unref") && G(lc, a.side_data_get, "av_packet_side_data_get") &&
                  G(lc, a.codec_get_name, "avcodec_get_name") && G(lu, a.frame_alloc, "av_frame_alloc") &&
                  G(lu, a.frame_free, "av_frame_free") && G(lu, a.frame_unref, "av_frame_unref") &&
                  G(lu, a.hwdevice_ctx_create, "av_hwdevice_ctx_create") && G(lu, a.buffer_ref, "av_buffer_ref") &&
                  G(lu, a.buffer_unref, "av_buffer_unref") && G(lu, a.hwframe_transfer_data, "av_hwframe_transfer_data") &&
                  G(lu, a.display_rotation_get, "av_display_rotation_get") && G(lu, a.log_set_level, "av_log_set_level") &&
                  G(lf, a.format_alloc_context, "avformat_alloc_context") && G(lf, a.avio_alloc_context, "avio_alloc_context") &&
                  G(lf, a.avio_context_free, "avio_context_free") && G(lu, a.malloc_, "av_malloc") && G(lu, a.freep, "av_freep");
        if (!ok) continue;
        A = a;
        A.log_set_level(AV_LOG_ERROR);
        g_ok = true;
        return true;
    }
    if (err) *err = std::string(f) + " not found";
    return false;
}

enum AVPixelFormat PickFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (ctx->hw_device_ctx && *p == AV_PIX_FMT_D3D11) return *p;
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_YUV420P || *p == AV_PIX_FMT_YUVJ420P || *p == AV_PIX_FMT_NV12) return *p;
    return fmts[0];
}

class FfSource : public Source {
public:
    ~FfSource() override {
        if (vctx_) A.free_context(&vctx_);
        if (actx_) A.free_context(&actx_);
        if (fmt_) A.close_input(&fmt_);
        if (io_) {
            A.freep(&io_->buffer);
            A.avio_context_free(&io_);
        }
        if (pkt_) A.packet_free(&pkt_);
        if (frame_) A.frame_free(&frame_);
        if (sw_) A.frame_free(&sw_);
        if (hw_) A.buffer_unref(&hw_);
    }

    bool OpenStream(const LspStream* s, LspInfo* info, bool wantAudio, std::string* err) override {
        stream_ = *s;
        const int kBuf = 256 * 1024;
        auto* buf = (unsigned char*)A.malloc_(kBuf);
        io_ = A.avio_alloc_context(buf, kBuf, 0, this, &FfSource::IoRead, nullptr, &FfSource::IoSeek);
        fmt_ = A.format_alloc_context();
        if (!io_ || !fmt_) {
            *err = "out of memory";
            return false;
        }
        fmt_->pb = io_;
        fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;
        return OpenCommon("", info, wantAudio, err);
    }

    bool Open(const std::string& path, LspInfo* info, bool wantAudio, std::string* err) override { return OpenCommon(path, info, wantAudio, err); }

    static int IoRead(void* o, uint8_t* buf, int size) {
        auto* self = (FfSource*)o;
        int n = self->stream_.read(self->stream_.user, self->pos_, buf, size);
        if (n < 0) return AVERROR(EIO);
        if (n == 0) return AVERROR_EOF;
        self->pos_ += n;
        return n;
    }
    static int64_t IoSeek(void* o, int64_t off, int whence) {
        auto* self = (FfSource*)o;
        int64_t size = self->stream_.size;
        if (whence & AVSEEK_SIZE) return size;
        whence &= ~AVSEEK_FORCE;
        int64_t to = whence == SEEK_SET ? off : whence == SEEK_CUR ? self->pos_ + off : whence == SEEK_END ? size + off : -1;
        if (to < 0) return -1;
        self->pos_ = to;
        return to;
    }

    bool OpenCommon(const std::string& path, LspInfo* info, bool wantAudio, std::string* err) {
        if (A.open_input(&fmt_, io_ ? nullptr : path.c_str(), nullptr, nullptr) < 0) {
            *err = "cannot open file";
            return false;
        }
        if (A.find_stream_info(fmt_, nullptr) < 0) {
            *err = "unknown format";
            return false;
        }
        const AVCodec* vc = nullptr;
        const AVCodec* ac = nullptr;
        vs_ = A.find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &vc, 0);
        as_ = A.find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, vs_ >= 0 ? vs_ : -1, &ac, 0);
        // pictures attached to music files (cover art) are not a video
        if (vs_ >= 0 && (fmt_->streams[vs_]->disposition & AV_DISPOSITION_ATTACHED_PIC)) vs_ = -1;
        if (vs_ >= 0 && !(vc && OpenVideo(vc))) vs_ = -1;
        if (as_ >= 0 && !(ac && OpenAudio(ac))) as_ = -1;
        if (!wantAudio && as_ >= 0) {
            A.free_context(&actx_);
            infoAudioOnly_ = true;
        }
        if (vs_ < 0 && as_ < 0 && !infoAudioOnly_) {
            *err = "no playable audio or video (codec not included)";
            return false;
        }
        memset(info, 0, sizeof(*info));
        info->duration = fmt_->duration > 0 ? fmt_->duration / (double)AV_TIME_BASE : 0;
        info->has_audio = as_ >= 0;
        info->has_video = vs_ >= 0;
        if (vs_ >= 0) {
            AVCodecParameters* p = fmt_->streams[vs_]->codecpar;
            const AVPacketSideData* sd = A.side_data_get(p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
            if (sd && sd->size >= 9 * 4) {
                double ccw = A.display_rotation_get((const int32_t*)sd->data);  // counter-clockwise degrees
                rotation_ = ((int)std::lround(-ccw / 90.0) * 90 % 360 + 360) % 360;
            }
            bool swap = rotation_ == 90 || rotation_ == 270;
            info->width = swap ? p->height : p->width;
            info->height = swap ? p->width : p->height;
            snprintf(info->video_codec, sizeof(info->video_codec), "%s%s", A.codec_get_name(p->codec_id), hwOn_ ? " (GPU)" : "");
        }
        if (as_ >= 0 || infoAudioOnly_) {
            int s = as_ >= 0 ? as_ : A.find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (s >= 0) snprintf(info->audio_codec, sizeof(info->audio_codec), "%s", A.codec_get_name(fmt_->streams[s]->codecpar->codec_id));
            if (infoAudioOnly_ && s >= 0) info->has_audio = 1;
        }
        pkt_ = A.packet_alloc();
        frame_ = A.frame_alloc();
        sw_ = A.frame_alloc();
        return true;
    }

    int Step(Sink& sink) override {
        int r = A.read_frame(fmt_, pkt_);
        if (r < 0) {
            // end: drain the decoders once
            if (!drained_) {
                drained_ = true;
                if (vctx_) Decode(vctx_, nullptr, sink, true);
                if (actx_) Decode(actx_, nullptr, sink, false);
            }
            return 0;
        }
        if (pkt_->stream_index == vs_ && vctx_) Decode(vctx_, pkt_, sink, true);
        else if (pkt_->stream_index == as_ && actx_) Decode(actx_, pkt_, sink, false);
        A.packet_unref(pkt_);
        return 1;
    }

    bool Seek(double t) override {
        int64_t ts = (int64_t)(t * AV_TIME_BASE);
        int r = A.seek_file(fmt_, -1, INT64_MIN, ts, ts, 0);
        if (vctx_) A.flush_buffers(vctx_);
        if (actx_) A.flush_buffers(actx_);
        drained_ = false;
        skipBefore_ = t;  // decode up to the target quietly (seeking lands on the keyframe before it)
        return r >= 0;
    }

private:
    bool OpenVideo(const AVCodec* c) {
        for (int attempt = 0; attempt < 2; attempt++) {
            AVCodecContext* ctx = A.alloc_context3(c);
            A.params_to_context(ctx, fmt_->streams[vs_]->codecpar);
            ctx->get_format = PickFormat;
            bool hw = false;
#ifdef _WIN32
            if (attempt == 0) {
                bool can = false;
                for (int i = 0;; i++) {
                    const AVCodecHWConfig* h = A.get_hw_config(c, i);
                    if (!h) break;
                    if (h->device_type == AV_HWDEVICE_TYPE_D3D11VA) can = true;
                }
                if (can && (hw_ || A.hwdevice_ctx_create(&hw_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0)) {
                    ctx->hw_device_ctx = A.buffer_ref(hw_);
                    hw = true;
                }
            }
#endif
            if (attempt == 0 && !hw) attempt = 1;
            if (!hw) {
                ctx->thread_count = (int)std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
                ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            }
            if (A.open2(ctx, c, nullptr) >= 0) {
                vctx_ = ctx;
                hwOn_ = hw;
                return true;
            }
            A.free_context(&ctx);
        }
        return false;
    }

    bool OpenAudio(const AVCodec* c) {
        actx_ = A.alloc_context3(c);
        A.params_to_context(actx_, fmt_->streams[as_]->codecpar);
        if (A.open2(actx_, c, nullptr) < 0) {
            A.free_context(&actx_);
            return false;
        }
        return true;
    }

    void Decode(AVCodecContext* ctx, AVPacket* pkt, Sink& sink, bool video) {
        if (A.send_packet(ctx, pkt) < 0 && pkt) return;
        AVStream* st = fmt_->streams[video ? vs_ : as_];
        double tb = av_q2d(st->time_base);
        for (;;) {
            int r = A.receive_frame(ctx, frame_);
            if (r < 0) break;
            int64_t ts = frame_->best_effort_timestamp != AV_NOPTS_VALUE ? frame_->best_effort_timestamp : frame_->pts;
            double pts = ts != AV_NOPTS_VALUE ? (ts - (st->start_time != AV_NOPTS_VALUE ? st->start_time : 0)) * tb : 0;
            if (pts + 0.001 < skipBefore_) {
                A.frame_unref(frame_);
                continue;
            }
            if (video) EmitVideo(sink, pts);
            else EmitAudio(sink, pts);
            A.frame_unref(frame_);
        }
    }

    void EmitVideo(Sink& sink, double pts) {
        AVFrame* pic = frame_;
        if (frame_->format == AV_PIX_FMT_D3D11) {
            A.frame_unref(sw_);
            if (A.hwframe_transfer_data(sw_, frame_, 0) < 0) return;
            pic = sw_;
        }
        LsvImage im{};
        im.width = pic->width;
        im.height = pic->height;
        im.y = pic->data[0];
        im.y_stride = pic->linesize[0];
        im.rotation = rotation_;
        if (pic->format == AV_PIX_FMT_NV12) {
            im.u = pic->data[1];
            im.v = pic->data[1] + 1;
            im.uv_stride = pic->linesize[1];
            im.uv_pixel_stride = 2;
        } else if (pic->format == AV_PIX_FMT_YUV420P || pic->format == AV_PIX_FMT_YUVJ420P) {
            im.u = pic->data[1];
            im.v = pic->data[2];
            im.uv_stride = pic->linesize[1];
            im.uv_pixel_stride = 1;
        } else {
            return;  // 10-bit / 4:4:4 etc. aren't converted (rare in shared clips)
        }
        if (sink.video) sink.video(im, pts);
        if (pic == sw_) A.frame_unref(sw_);
        if (skipBefore_ > 0) skipBefore_ = 0;
    }

    void EmitAudio(Sink& sink, double pts) {
        int n = frame_->nb_samples, ch = frame_->ch_layout.nb_channels, rate = frame_->sample_rate;
        if (n <= 0 || ch <= 0 || !sink.audio) return;
        pcm_.resize((size_t)n * ch);
        auto fmt = (AVSampleFormat)frame_->format;
        for (int c = 0; c < ch; c++)
            for (int i = 0; i < n; i++) {
                float v = 0;
                switch (fmt) {
                    case AV_SAMPLE_FMT_FLTP: v = ((const float*)frame_->extended_data[c])[i]; break;
                    case AV_SAMPLE_FMT_FLT: v = ((const float*)frame_->extended_data[0])[i * ch + c]; break;
                    case AV_SAMPLE_FMT_S16P: v = ((const int16_t*)frame_->extended_data[c])[i] / 32768.f; break;
                    case AV_SAMPLE_FMT_S16: v = ((const int16_t*)frame_->extended_data[0])[i * ch + c] / 32768.f; break;
                    case AV_SAMPLE_FMT_S32P: v = (float)(((const int32_t*)frame_->extended_data[c])[i] / 2147483648.0); break;
                    case AV_SAMPLE_FMT_S32: v = (float)(((const int32_t*)frame_->extended_data[0])[i * ch + c] / 2147483648.0); break;
                    case AV_SAMPLE_FMT_DBLP: v = (float)((const double*)frame_->extended_data[c])[i]; break;
                    case AV_SAMPLE_FMT_DBL: v = (float)((const double*)frame_->extended_data[0])[i * ch + c]; break;
                    case AV_SAMPLE_FMT_U8: v = (((const uint8_t*)frame_->extended_data[0])[i * ch + c] - 128) / 128.f; break;
                    default: break;
                }
                pcm_[(size_t)i * ch + c] = v;
            }
        sink.audio(pcm_.data(), n, rate, ch, pts);
        skipBefore_ = 0;
    }

    AVFormatContext* fmt_ = nullptr;
    AVIOContext* io_ = nullptr;
    LspStream stream_{};
    int64_t pos_ = 0;
    AVCodecContext* vctx_ = nullptr;
    AVCodecContext* actx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* sw_ = nullptr;
    AVBufferRef* hw_ = nullptr;
    int vs_ = -1, as_ = -1, rotation_ = 0;
    bool hwOn_ = false, drained_ = false, infoAudioOnly_ = false;
    double skipBefore_ = 0;
    std::vector<float> pcm_;
};

}  // namespace

bool InitBackend(const LspHost*, std::string* err) { return LoadFf(err); }
std::unique_ptr<Source> CreateSource(const LspHost*) { return std::make_unique<FfSource>(); }

}  // namespace lsp
