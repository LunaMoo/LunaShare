// Platform part of the video module: camera + H.264 encoder/decoder.
// The portable core (video_core.cpp) does packets, keyframe recovery and bitrate adaptation on top of it.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "lunashare_video.h"

namespace lsv {

struct EncodedFrame {
    const uint8_t* data = nullptr;  // H.264 Annex B; keyframes carry SPS/PPS
    size_t len = 0;
    bool key = false;
    int width = 0, height = 0;      // coded size
    int rotation = 0;               // clockwise degrees to show it upright
    bool mirror = false;            // front camera (only used for the local preview)
};

// Implemented by the core; backends call it from any thread.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void OnEncoded(const EncodedFrame& f) = 0;
    virtual void OnLocalImage(const LsvImage& img) = 0;
    virtual void OnDecoded(const LsvImage& img) = 0;  // rotation/mirror are filled in by the core
    virtual void OnDecoderError() = 0;
    virtual void OnCameraError(const std::string& msg) = 0;
    virtual void Log(const std::string& msg) = 0;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::vector<LsvCamera> Cameras() = 0;
    // starts capture + encoding; encoded frames and preview images go to the sink
    virtual bool StartCamera(const std::string& id, const LsvSettings& s, std::string* err) = 0;
    virtual void StopCamera() = 0;
    virtual void SetBitrate(int bps) = 0;
    virtual void RequestKeyframe() = 0;
    virtual std::vector<LsvControl> Controls() = 0;
    virtual void SetControl(int id, float value) = 0;
    // decoding: called from the core's decode thread only
    virtual bool StartDecoder() = 0;
    virtual void Decode(const uint8_t* annexb, size_t len, bool key, int width, int height) = 0;
    virtual void StopDecoder() = 0;
    virtual std::string EncoderName() = 0;
    virtual std::string DecoderName() = 0;
};

std::unique_ptr<Backend> CreateBackend(Sink* sink, const LsvHost* host);

// Small helpers shared by backends
bool H264HasNal(const uint8_t* p, size_t n, int type);  // any NAL of this type (5 = IDR, 7 = SPS)

}  // namespace lsv
