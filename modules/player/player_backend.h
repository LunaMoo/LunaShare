// Platform part of the player module: open a file and decode it step by step.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "lunashare_player.h"

namespace lsp {

struct Sink {
    // interleaved float samples at the file's own rate / channel count
    std::function<void(const float* pcm, int frames, int rate, int channels, double pts)> audio;
    // a decoded picture (rotation filled in), valid only during the call
    std::function<void(const LsvImage& img, double pts)> video;
};

class Source {
public:
    virtual ~Source() = default;
    virtual bool Open(const std::string& path, LspInfo* info, bool wantAudio, std::string* err) = 0;
    virtual bool OpenStream(const LspStream* s, LspInfo* info, bool wantAudio, std::string* err) = 0;
    // decode a little; returns 1 = go on, 0 = end of file, -1 = error
    virtual int Step(Sink& sink) = 0;
    virtual bool Seek(double seconds) = 0;
};

std::unique_ptr<Source> CreateSource(const LspHost* host);
bool InitBackend(const LspHost* host, std::string* err);

}  // namespace lsp
