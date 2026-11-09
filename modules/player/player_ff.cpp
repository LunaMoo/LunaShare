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
    // tracks / subtitles
    const AVCodec* (*find_decoder)(enum AVCodecID);
    int (*decode_subtitle2)(AVCodecContext*, AVSubtitle*, int*, const AVPacket*);
    void (*subtitle_free)(AVSubtitle*);
    AVDictionaryEntry* (*dict_get)(const AVDictionary*, const char*, const AVDictionaryEntry*, int);
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

#ifdef LSP_FF_STATIC
// FFmpeg linked in (Android: the sound and subtitle decoders the phone lacks)
bool LoadFf(std::string*) {
    std::lock_guard<std::mutex> l(g_m);
    if (g_ok) return true;
    A.open_input = reinterpret_cast<decltype(A.open_input)>(&::avformat_open_input);
    A.find_stream_info = reinterpret_cast<decltype(A.find_stream_info)>(&::avformat_find_stream_info);
    A.find_best_stream = reinterpret_cast<decltype(A.find_best_stream)>(&::av_find_best_stream);
    A.read_frame = reinterpret_cast<decltype(A.read_frame)>(&::av_read_frame);
    A.seek_file = reinterpret_cast<decltype(A.seek_file)>(&::avformat_seek_file);
    A.close_input = reinterpret_cast<decltype(A.close_input)>(&::avformat_close_input);
    A.alloc_context3 = reinterpret_cast<decltype(A.alloc_context3)>(&::avcodec_alloc_context3);
    A.params_to_context = reinterpret_cast<decltype(A.params_to_context)>(&::avcodec_parameters_to_context);
    A.open2 = reinterpret_cast<decltype(A.open2)>(&::avcodec_open2);
    A.free_context = reinterpret_cast<decltype(A.free_context)>(&::avcodec_free_context);
    A.send_packet = reinterpret_cast<decltype(A.send_packet)>(&::avcodec_send_packet);
    A.receive_frame = reinterpret_cast<decltype(A.receive_frame)>(&::avcodec_receive_frame);
    A.flush_buffers = reinterpret_cast<decltype(A.flush_buffers)>(&::avcodec_flush_buffers);
    A.get_hw_config = reinterpret_cast<decltype(A.get_hw_config)>(&::avcodec_get_hw_config);
    A.packet_alloc = reinterpret_cast<decltype(A.packet_alloc)>(&::av_packet_alloc);
    A.packet_free = reinterpret_cast<decltype(A.packet_free)>(&::av_packet_free);
    A.packet_unref = reinterpret_cast<decltype(A.packet_unref)>(&::av_packet_unref);
    A.side_data_get = reinterpret_cast<decltype(A.side_data_get)>(&::av_packet_side_data_get);
    A.codec_get_name = reinterpret_cast<decltype(A.codec_get_name)>(&::avcodec_get_name);
    A.frame_alloc = reinterpret_cast<decltype(A.frame_alloc)>(&::av_frame_alloc);
    A.frame_free = reinterpret_cast<decltype(A.frame_free)>(&::av_frame_free);
    A.frame_unref = reinterpret_cast<decltype(A.frame_unref)>(&::av_frame_unref);
    A.hwdevice_ctx_create = reinterpret_cast<decltype(A.hwdevice_ctx_create)>(&::av_hwdevice_ctx_create);
    A.buffer_ref = reinterpret_cast<decltype(A.buffer_ref)>(&::av_buffer_ref);
    A.buffer_unref = reinterpret_cast<decltype(A.buffer_unref)>(&::av_buffer_unref);
    A.hwframe_transfer_data = reinterpret_cast<decltype(A.hwframe_transfer_data)>(&::av_hwframe_transfer_data);
    A.display_rotation_get = reinterpret_cast<decltype(A.display_rotation_get)>(&::av_display_rotation_get);
    A.log_set_level = reinterpret_cast<decltype(A.log_set_level)>(&::av_log_set_level);
    A.format_alloc_context = reinterpret_cast<decltype(A.format_alloc_context)>(&::avformat_alloc_context);
    A.avio_alloc_context = reinterpret_cast<decltype(A.avio_alloc_context)>(&::avio_alloc_context);
    A.avio_context_free = reinterpret_cast<decltype(A.avio_context_free)>(&::avio_context_free);
    A.malloc_ = reinterpret_cast<decltype(A.malloc_)>(&::av_malloc);
    A.freep = reinterpret_cast<decltype(A.freep)>(&::av_freep);
    A.find_decoder = reinterpret_cast<decltype(A.find_decoder)>(&::avcodec_find_decoder);
    A.decode_subtitle2 = reinterpret_cast<decltype(A.decode_subtitle2)>(&::avcodec_decode_subtitle2);
    A.subtitle_free = reinterpret_cast<decltype(A.subtitle_free)>(&::avsubtitle_free);
    A.dict_get = reinterpret_cast<decltype(A.dict_get)>(&::av_dict_get);
    A.log_set_level(AV_LOG_ERROR);
    g_ok = true;
    return true;
}
#else
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
                  G(lf, a.avio_context_free, "avio_context_free") && G(lu, a.malloc_, "av_malloc") && G(lu, a.freep, "av_freep") &&
                  G(lc, a.find_decoder, "avcodec_find_decoder") && G(lc, a.decode_subtitle2, "avcodec_decode_subtitle2") &&
                  G(lc, a.subtitle_free, "avsubtitle_free") && G(lu, a.dict_get, "av_dict_get");
        if (!ok) continue;
        A = a;
        A.log_set_level(AV_LOG_ERROR);
        g_ok = true;
        return true;
    }
    if (err) *err = std::string(f) + " not found";
    return false;
}
#endif

enum AVPixelFormat PickFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (ctx->hw_device_ctx && *p == AV_PIX_FMT_D3D11) return *p;
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_YUV420P || *p == AV_PIX_FMT_YUVJ420P || *p == AV_PIX_FMT_NV12 || *p == AV_PIX_FMT_YUV420P10LE ||
            *p == AV_PIX_FMT_P010LE)
            return *p;
    return fmts[0];
}

class FfSource : public Source {
public:
    explicit FfSource(bool noVideo = false) : noVideo_(noVideo) {}
    ~FfSource() override {
        if (vctx_) A.free_context(&vctx_);
        if (actx_) A.free_context(&actx_);
        if (sctx_) A.free_context(&sctx_);
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
        if (noVideo_) vs_ = -1;  // (the picture comes from elsewhere)
        if (vs_ >= 0 && !(vc && OpenVideo(vc))) vs_ = -1;
        if (as_ >= 0 && !(ac && OpenAudio(ac))) as_ = -1;
        if (as_ < 0)  // the main sound track can't be decoded here (DTS, TrueHD...): any other one that can
            for (unsigned i = 0; i < fmt_->nb_streams && as_ < 0; i++) {
                if (fmt_->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
                const AVCodec* c = A.find_decoder(fmt_->streams[i]->codecpar->codec_id);
                as_ = (int)i;
                if (!c || !OpenAudio(c)) as_ = -1;
            }
        UpdateDiscard();
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

    std::vector<TrackInfo> Tracks() override {
        std::vector<TrackInfo> out;
        for (unsigned i = 0; fmt_ && i < fmt_->nb_streams; i++) {
            AVStream* st = fmt_->streams[i];
            auto type = st->codecpar->codec_type;
            if (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_SUBTITLE) continue;
            TrackInfo t;
            t.kind = type == AVMEDIA_TYPE_AUDIO ? LSP_TRACK_AUDIO : LSP_TRACK_SUBTITLE;
            t.id = (int)i;
            t.selected = (int)i == (type == AVMEDIA_TYPE_AUDIO ? as_ : ss_);
            t.playable = A.find_decoder(st->codecpar->codec_id) != nullptr;
            t.channels = type == AVMEDIA_TYPE_AUDIO ? st->codecpar->ch_layout.nb_channels : 0;
            if (auto* e = A.dict_get(st->metadata, "language", nullptr, 0)) t.lang = e->value;
            if (auto* e = A.dict_get(st->metadata, "title", nullptr, 0)) t.title = e->value;
            t.codec = A.codec_get_name(st->codecpar->codec_id);
            out.push_back(t);
        }
        return out;
    }

    std::vector<std::pair<double, std::string>> Chapters() override {
        std::vector<std::pair<double, std::string>> out;
        for (unsigned i = 0; fmt_ && i < fmt_->nb_chapters && i < 500; i++) {
            const AVChapter* c = fmt_->chapters[i];
            double start = (double)c->start * c->time_base.num / std::max(1, c->time_base.den);
            std::string title;
            if (auto* e = A.dict_get(c->metadata, "title", nullptr, 0)) title = e->value;
            out.push_back({std::max(0.0, start), title});
        }
        std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.first < b.first; });
        return out;
    }

    bool SelectAudio(int id) override {
        if (!fmt_ || id < 0 || id >= (int)fmt_->nb_streams || fmt_->streams[id]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) return false;
        const AVCodec* c = A.find_decoder(fmt_->streams[id]->codecpar->codec_id);
        if (!c) return false;
        int old = as_;
        if (actx_) A.free_context(&actx_);
        as_ = id;
        if (!OpenAudio(c)) {
            as_ = old;  // (back to the one that worked)
            const AVCodec* oc = old >= 0 ? A.find_decoder(fmt_->streams[old]->codecpar->codec_id) : nullptr;
            if (!oc || !OpenAudio(oc)) as_ = -1;
        }
        UpdateDiscard();
        return as_ == id;
    }

    bool SelectSubtitle(int id) override {
        if (sctx_) A.free_context(&sctx_);
        ss_ = -1;
        if (fmt_ && id >= 0 && id < (int)fmt_->nb_streams && fmt_->streams[id]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            const AVCodec* c = A.find_decoder(fmt_->streams[id]->codecpar->codec_id);
            if (c) {
                sctx_ = A.alloc_context3(c);
                A.params_to_context(sctx_, fmt_->streams[id]->codecpar);
                sctx_->pkt_timebase = fmt_->streams[id]->time_base;
                if (A.open2(sctx_, c, nullptr) >= 0) ss_ = id;
                else A.free_context(&sctx_);
            }
        }
        UpdateDiscard();
        return ss_ == id;
    }

    // only the streams being played are read (others are skipped by the demuxer)
    void UpdateDiscard() {
        for (unsigned i = 0; fmt_ && i < fmt_->nb_streams; i++)
            fmt_->streams[i]->discard = ((int)i == vs_ || (int)i == as_ || (int)i == ss_) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }

    void DecodeSubtitle(Sink& sink) {
        if (!sink.subtitle) return;
        AVSubtitle sub{};
        int got = 0;
        if (A.decode_subtitle2(sctx_, &sub, &got, pkt_) < 0 || !got) return;
        AVStream* st = fmt_->streams[ss_];
        double tb = av_q2d(st->time_base);
        int64_t ts = pkt_->pts != AV_NOPTS_VALUE ? pkt_->pts : pkt_->dts;
        double base = ts != AV_NOPTS_VALUE ? ts * tb - Start() : 0;
        double start = base + sub.start_display_time / 1000.0;
        double end = -1;
        if (sub.end_display_time && sub.end_display_time != UINT32_MAX) end = base + sub.end_display_time / 1000.0;
        else if (pkt_->duration > 0) end = base + pkt_->duration * tb;
        std::string text;
        std::vector<uint8_t> rgba;
        int bx0 = 1 << 30, by0 = 1 << 30, bx1 = 0, by1 = 0;
        for (unsigned r = 0; r < sub.num_rects; r++) {
            AVSubtitleRect* rc = sub.rects[r];
            if (rc->type == SUBTITLE_ASS && rc->ass) {
                // "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text"
                std::string a = rc->ass;
                size_t at = 0;
                for (int k = 0; k < 8 && at != std::string::npos; k++) at = a.find(',', at == 0 && k == 0 ? 0 : at + 1);
                text += (text.empty() ? "" : "\n") + (at == std::string::npos ? a : a.substr(at + 1));
            } else if (rc->type == SUBTITLE_TEXT && rc->text) {
                text += (text.empty() ? "" : "\n") + std::string(rc->text);
            } else if (rc->type == SUBTITLE_BITMAP && rc->w > 0 && rc->h > 0) {
                bx0 = std::min(bx0, rc->x);
                by0 = std::min(by0, rc->y);
                bx1 = std::max(bx1, rc->x + rc->w);
                by1 = std::max(by1, rc->y + rc->h);
            }
        }
        int bw = bx1 - bx0, bh = by1 - by0;
        float fx = 0, fy = 0, fw = 0, fh = 0;
        if (bw > 0 && bh > 0 && bw <= 8192 && bh <= 8192) {  // picture subtitles: all the pieces into one RGBA image
            rgba.assign((size_t)bw * bh * 4, 0);
            for (unsigned r = 0; r < sub.num_rects; r++) {
                AVSubtitleRect* rc = sub.rects[r];
                if (rc->type != SUBTITLE_BITMAP || !rc->data[0] || !rc->data[1]) continue;
                const uint32_t* pal = (const uint32_t*)rc->data[1];  // ARGB (native endian)
                for (int y = 0; y < rc->h; y++)
                    for (int x = 0; x < rc->w; x++) {
                        uint32_t c = pal[rc->data[0][(size_t)y * rc->linesize[0] + x]];
                        uint8_t* d = &rgba[(((size_t)(rc->y - by0 + y)) * bw + (rc->x - bx0 + x)) * 4];
                        d[0] = (uint8_t)(c >> 16);
                        d[1] = (uint8_t)(c >> 8);
                        d[2] = (uint8_t)c;
                        d[3] = (uint8_t)(c >> 24);
                    }
            }
            // positions are in the subtitle's own frame (the video size for Blu-ray / DVD)
            int fwid = sctx_->width > 0 ? sctx_->width : vctx_ ? vctx_->width : bw;
            int fhei = sctx_->height > 0 ? sctx_->height : vctx_ ? vctx_->height : bh;
            fx = (float)bx0 / (float)fwid;
            fy = (float)by0 / (float)fhei;
            fw = (float)bw / (float)fwid;
            fh = (float)bh / (float)fhei;
        } else {
            bw = bh = 0;
        }
        sink.subtitle(start, end, text, rgba, bw, bh, fx, fy, fw, fh);
        A.subtitle_free(&sub);
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
        else if (pkt_->stream_index == ss_ && sctx_) DecodeSubtitle(sink);
        A.packet_unref(pkt_);
        return 1;
    }

    // where the file's clock starts (all streams count from here; a stream's own start - for subtitles the first
    // line - would shift it)
    double Start() const { return fmt_ && fmt_->start_time != AV_NOPTS_VALUE ? fmt_->start_time / (double)AV_TIME_BASE : 0; }

    bool Seek(double t) override {
        int64_t ts = (int64_t)((t + Start()) * AV_TIME_BASE);
        int r = A.seek_file(fmt_, -1, INT64_MIN, ts, ts, 0);
        if (vctx_) A.flush_buffers(vctx_);
        if (actx_) A.flush_buffers(actx_);
        if (sctx_) A.flush_buffers(sctx_);
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
            double pts = ts != AV_NOPTS_VALUE ? ts * tb - Start() : 0;
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
        } else if (pic->format == AV_PIX_FMT_YUV420P10LE || pic->format == AV_PIX_FMT_P010LE) {
            // 10-bit (HDR and anime releases): to 8 bits
            int w = pic->width, h = pic->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
            b8y_.resize((size_t)w * h);
            b8uv_.resize((size_t)cw * ch * 2);
            bool planar = pic->format == AV_PIX_FMT_YUV420P10LE;
            for (int y = 0; y < h; y++) {
                const uint16_t* s = (const uint16_t*)(pic->data[0] + (size_t)y * pic->linesize[0]);
                uint8_t* d = &b8y_[(size_t)y * w];
                for (int x = 0; x < w; x++) d[x] = (uint8_t)(planar ? std::min(255, s[x] >> 2) : s[x] >> 8);
            }
            for (int y = 0; y < ch; y++) {
                uint8_t* d = &b8uv_[(size_t)y * cw * 2];
                if (planar) {
                    const uint16_t* u = (const uint16_t*)(pic->data[1] + (size_t)y * pic->linesize[1]);
                    const uint16_t* v = (const uint16_t*)(pic->data[2] + (size_t)y * pic->linesize[2]);
                    for (int x = 0; x < cw; x++) {
                        d[x * 2] = (uint8_t)std::min(255, u[x] >> 2);
                        d[x * 2 + 1] = (uint8_t)std::min(255, v[x] >> 2);
                    }
                } else {
                    const uint16_t* uv = (const uint16_t*)(pic->data[1] + (size_t)y * pic->linesize[1]);
                    for (int x = 0; x < cw * 2; x++) d[x] = (uint8_t)(uv[x] >> 8);
                }
            }
            im.y = b8y_.data();
            im.y_stride = w;
            im.u = b8uv_.data();
            im.v = b8uv_.data() + 1;
            im.uv_stride = cw * 2;
            im.uv_pixel_stride = 2;
        } else {
            return;  // 4:2:2 / 4:4:4 etc. aren't converted (rare)
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

    bool noVideo_ = false;
    AVFormatContext* fmt_ = nullptr;
    AVIOContext* io_ = nullptr;
    LspStream stream_{};
    int64_t pos_ = 0;
    AVCodecContext* vctx_ = nullptr;
    AVCodecContext* actx_ = nullptr;
    AVCodecContext* sctx_ = nullptr;
    int ss_ = -1;
    std::vector<uint8_t> b8y_, b8uv_;
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

bool ReadTags(const std::string& path, LspTags* out, LspCoverFn cover, void* user) {
    if (!LoadFf(nullptr)) return false;
    AVFormatContext* fmt = nullptr;
    if (A.open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    A.find_stream_info(fmt, nullptr);
    memset(out, 0, sizeof(*out));
    auto get = [&](const char* key, char* dst, size_t n) {
        const AVDictionaryEntry* e = A.dict_get(fmt->metadata, key, nullptr, 0);  // AV_DICT_MATCH_CASE off: any case
        for (unsigned i = 0; !e && i < fmt->nb_streams; i++) e = A.dict_get(fmt->streams[i]->metadata, key, nullptr, 0);
        if (e && e->value && !dst[0]) snprintf(dst, n, "%s", e->value);
    };
    get("title", out->title, sizeof(out->title));
    get("artist", out->artist, sizeof(out->artist));
    get("album", out->album, sizeof(out->album));
    get("album_artist", out->album_artist, sizeof(out->album_artist));
    get("genre", out->genre, sizeof(out->genre));
    get("date", out->date, sizeof(out->date));
    char tr[32] = {0}, ds[32] = {0};
    get("track", tr, sizeof(tr));
    get("disc", ds, sizeof(ds));
    out->track = atoi(tr);  // ("3/12" -> 3)
    out->disc = atoi(ds);
    char rg[64] = {0};  // ReplayGain: "-6.54 dB"
    get("REPLAYGAIN_TRACK_GAIN", rg, sizeof(rg));
    if (rg[0]) {
        out->track_gain = atof(rg);
        out->has_track_gain = 1;
    }
    rg[0] = 0;
    get("REPLAYGAIN_ALBUM_GAIN", rg, sizeof(rg));
    if (rg[0]) {
        out->album_gain = atof(rg);
        out->has_album_gain = 1;
    }
    if (fmt->duration > 0) out->duration = (double)fmt->duration / AV_TIME_BASE;
    bool audio = false;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audio = true;
        if (cover && (st->disposition & AV_DISPOSITION_ATTACHED_PIC) && st->attached_pic.data && st->attached_pic.size > 0) {
            cover(user, st->attached_pic.data, st->attached_pic.size);
            cover = nullptr;  // (the first one)
        }
    }
    A.close_input(&fmt);
    return audio;
}

#ifdef LSP_FF_SIDE
// Android: FFmpeg reads sound and subtitles next to the platform's video decoder (player_android.cpp)
bool InitFf(std::string* err) { return LoadFf(err); }
std::unique_ptr<Source> CreateFfSource() { return std::make_unique<FfSource>(true); }
#else
bool InitBackend(const LspHost*, std::string* err) { return LoadFf(err); }
std::unique_ptr<Source> CreateSource(const LspHost*) { return std::make_unique<FfSource>(); }
#endif

}  // namespace lsp
