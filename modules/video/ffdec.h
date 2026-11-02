// H.264 / HEVC / AV1 decoding through FFmpeg's libavcodec, loaded at run time (avcodec-61 + avutil-59).
// On Windows it decodes on the GPU through D3D11VA when the graphics card supports the codec, otherwise on the CPU
// (HEVC/H.264; FFmpeg's AV1 decoder needs the GPU). Frames come out as NV12 or I420 LsvImages.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "lunashare_video.h"

namespace lsv {

enum Codec { kH264 = 0, kHEVC = 1, kAV1 = 2, kMJPEG = 3 /* cameras only, never negotiated */ };
inline unsigned CodecBit(int c) { return 1u << c; }
inline const char* CodecName(int c) { return c == kHEVC ? "HEVC" : c == kAV1 ? "AV1" : "H.264"; }

class FfDecoder {
public:
    // Loads the libraries from one of the directories (first that works). Safe to call again.
    static bool Load(const std::vector<std::string>& dirs, std::string* err);
    static bool Loaded();

    ~FfDecoder() { Close(); }
    static bool HasSoftwareAv1();  // libdav1d in this FFmpeg build (Linux)
    // hw: try the GPU first (Windows D3D11VA); falls back to the CPU where FFmpeg can
    bool Open(int codec, bool hw, std::string* err);
    void Close();
    bool IsOpen() const { return ctx_ != nullptr; }
    int OpenCodec() const { return codec_; }
    bool Hardware() const { return hw_; }
    std::string Name() const;
    // one access unit / temporal unit; out() gets every picture that becomes ready. false = decoding error.
    bool Decode(const uint8_t* data, size_t len, const std::function<void(const LsvImage&)>& out);

private:
    void* ctx_ = nullptr;     // AVCodecContext*
    void* pkt_ = nullptr;     // AVPacket*
    void* frame_ = nullptr;   // AVFrame*
    void* sw_ = nullptr;      // AVFrame* (GPU frames copied to memory)
    void* hwdev_ = nullptr;   // AVBufferRef*
    int codec_ = -1;
    bool hw_ = false;
    std::vector<uint8_t> conv_;  // 4:2:2 pictures (MJPEG cameras) turned into 4:2:0
};

// H.264 encoding through FFmpeg (libx264 where the build has it), Annex B with SPS/PPS on every keyframe.
class FfEncoder {
public:
    ~FfEncoder() { Close(); }
    bool Open(int width, int height, int fps, int bitrate, std::string* err);
    void Close();
    bool IsOpen() const { return ctx_ != nullptr; }
    std::string Name() const { return name_; }
    void SetBitrate(int bps);
    // I420 picture; out() gets the encoded frames (zero or more)
    bool Encode(const LsvImage& img, bool forceKey, const std::function<void(const uint8_t*, size_t, bool key)>& out);

private:
    void* ctx_ = nullptr;
    void* frame_ = nullptr;
    void* pkt_ = nullptr;
    int64_t pts_ = 0;
    int bitrate_ = 0, fps_ = 30;
    std::string name_;
};

}  // namespace lsv
