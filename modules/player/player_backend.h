// Platform part of the player module: open a file and decode it step by step.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lunashare_player.h"

namespace lsp {

struct Sink {
    // interleaved float samples at the file's own rate / channel count
    std::function<void(const float* pcm, int frames, int rate, int channels, double pts)> audio;
    // a decoded picture (rotation filled in), valid only during the call
    std::function<void(const LsvImage& img, double pts)> video;
    // a subtitle: text, or a picture (straight RGBA) placed at x/y/w/h (fractions of the video). end <= start: until
    // the next one
    std::function<void(double start, double end, const std::string& text, const std::vector<uint8_t>& rgba, int w, int h, float x,
                       float y, float sw, float sh)>
        subtitle;
};

struct TrackInfo {
    int kind = 0, id = 0, channels = 0;
    bool playable = true, selected = false;
    std::string lang, title, codec;
};

class Source {
public:
    virtual ~Source() = default;
    virtual bool Open(const std::string& path, LspInfo* info, bool wantAudio, std::string* err) = 0;
    virtual bool OpenStream(const LspStream* s, LspInfo* info, bool wantAudio, std::string* err) = 0;
    // a URL fetched through the host (live: no seeking); sources that can't: false
    virtual bool OpenUrl(const std::string& url, const char* format, const LspUrlIo* io, LspInfo* info, std::string* err) {
        (void)url;
        (void)format;
        (void)io;
        (void)info;
        *err = "not supported";
        return false;
    }
    // decode a little; returns 1 = go on, 0 = end of file, -1 = error
    virtual int Step(Sink& sink) = 0;
    // the same, knowing what the player is short of (sources made of two decoders step only the one needed)
    virtual int StepNeeds(Sink& sink, bool needVideo, bool needAudio) {
        (void)needVideo;
        (void)needAudio;
        return Step(sink);
    }
    virtual bool Seek(double seconds) = 0;
    virtual std::vector<TrackInfo> Tracks() { return {}; }
    virtual std::vector<std::pair<double, std::string>> Chapters() { return {}; }  // start (s), title
    // decode thread, between Step()s; the core seeks to the current position afterwards
    virtual bool SelectAudio(int id) { (void)id; return false; }
    virtual bool SelectSubtitle(int id) { (void)id; return false; }  // -1 = none
};

std::unique_ptr<Source> CreateSource(const LspHost* host);
#ifdef LSP_FF_SIDE
bool InitFf(std::string* err);
std::unique_ptr<Source> CreateFfSource();  // sound and subtitles only
#endif
bool InitBackend(const LspHost* host, std::string* err);
// music tags + embedded cover through FFmpeg (where it's there: computers, Android with the FFmpeg side)
bool ReadTags(const std::string& path, LspTags* out, LspCoverFn cover, void* user);

}  // namespace lsp
