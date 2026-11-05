// LunaShare player module - portable core: a decode thread keeps ~1 s of audio and a few pictures ahead; the
// host's audio callback pulls audio (which also drives the clock) and the UI asks for the picture that belongs to
// the current position. Files without audio run on a wall clock.
// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "player_backend.h"

namespace lsp {
namespace {

double NowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

LspHost g_host{};
bool g_init = false;

void Log(const std::string& m) {
    if (g_host.log) g_host.log(g_host.user, ("player: " + m).c_str());
}

// linear resampler + channel mapping into the device format. Positions are in input frames; index -1 is the
// last frame of the previous chunk, so consecutive chunks join without clicks.
class Resampler {
public:
    void Reset() {
        phase_ = 0;
        have_ = false;
    }
    void Run(const float* in, int frames, int inRate, int inCh, int outRate, int outCh, std::vector<float>& out) {
        if (frames <= 0 || inRate <= 0 || inCh <= 0) return;
        if (inRate != inRate_ || inCh != inCh_ || outCh != outCh_) {
            inRate_ = inRate;
            inCh_ = inCh;
            outCh_ = outCh;
            Reset();
        }
        auto sample = [&](int i, int c) -> float {  // output channel c of input frame i (-1 = previous chunk)
            if (i < 0) return last_[c];
            const float* f = in + (size_t)i * inCh;
            if (inCh == 1) return f[0];
            if (outCh == 1) return (f[0] + f[1]) * 0.5f;
            return f[std::min(c, inCh - 1)];
        };
        if (!have_) phase_ = 0;
        double step = (double)inRate / outRate;
        double pos = phase_;
        while (pos < frames - 1) {
            int i = pos < 0 ? -1 : (int)pos;
            double t = pos - i;
            for (int c = 0; c < outCh; c++) {
                float a = sample(i, c), b = sample(i + 1, c);
                out.push_back((float)(a + (b - a) * t));
            }
            pos += step;
        }
        phase_ = pos - frames;  // >= -1
        last_.resize((size_t)outCh);
        for (int c = 0; c < outCh; c++) last_[c] = sample(frames - 1, c);
        have_ = true;
    }

private:
    double phase_ = 0;
    bool have_ = false;
    int inRate_ = 0, inCh_ = 0, outCh_ = 0;
    std::vector<float> last_;
};

struct Frame {
    std::vector<uint8_t> y, uv;  // tight Y + interleaved U/V
    int w = 0, h = 0, rotation = 0;
    double pts = 0;
};

}  // namespace
}  // namespace lsp

struct LspPlayer {
    std::unique_ptr<lsp::Source> src;
    LspInfo info{};
    int rate = 48000, channels = 2;
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool quit = false;
    // audio ring (device format) and the time of its first sample
    std::deque<float> audio;
    double audioPts = 0;
    bool audioPtsSet = false;
    lsp::Resampler rs;
    std::vector<float> tmp;
    // pictures
    std::deque<lsp::Frame> frames;
    lsp::Frame shown;
    bool newShown = false;
    // state
    std::atomic<int> state{LSP_PAUSED};
    bool eof = false, error = false;
    double seekTo = -1;
    // wall clock (no audio)
    double clockBase = 0, clockStart = 0;
    bool clockRunning = false;

    double PositionLocked() {
        if (info.has_audio && audioPtsSet) return audioPts;
        if (clockRunning) return clockBase + (lsp::NowSec() - clockStart);
        return clockBase;
    }

    void DecodeLoop() {
        lsp::Sink sink;
        sink.audio = [this](const float* pcm, int n, int r, int ch, double pts) {
            std::lock_guard<std::mutex> l(m);
            if (seekTo >= 0) return;
            if (!audioPtsSet || audio.empty()) {
                audioPts = pts;
                audioPtsSet = true;
            }
            tmp.clear();
            rs.Run(pcm, n, r, ch, rate, channels, tmp);
            audio.insert(audio.end(), tmp.begin(), tmp.end());
        };
        sink.video = [this](const LsvImage& im, double pts) {
            lsp::Frame f;
            f.w = im.width;
            f.h = im.height;
            f.rotation = im.rotation;
            f.pts = pts;
            int cw = (im.width + 1) / 2, ch = (im.height + 1) / 2;
            f.y.resize((size_t)im.width * im.height);
            f.uv.resize((size_t)cw * ch * 2);
            for (int r = 0; r < im.height; r++) memcpy(&f.y[(size_t)r * im.width], im.y + (size_t)r * im.y_stride, (size_t)im.width);
            for (int r = 0; r < ch; r++) {
                const uint8_t* u = im.u + (size_t)r * im.uv_stride;
                const uint8_t* v = im.v + (size_t)r * im.uv_stride;
                uint8_t* d = &f.uv[(size_t)r * cw * 2];
                if (im.uv_pixel_stride == 2 && im.v == im.u + 1) memcpy(d, u, (size_t)cw * 2);
                else
                    for (int c = 0; c < cw; c++) {
                        d[c * 2] = u[(size_t)c * im.uv_pixel_stride];
                        d[c * 2 + 1] = v[(size_t)c * im.uv_pixel_stride];
                    }
            }
            std::lock_guard<std::mutex> l(m);
            if (seekTo >= 0) return;
            frames.push_back(std::move(f));
        };
        for (;;) {
            {
                std::unique_lock<std::mutex> l(m);
                // stay ~1 s ahead on audio and a few pictures ahead on video
                cv.wait_for(l, std::chrono::milliseconds(10), [&] {
                    if (quit || seekTo >= 0) return true;
                    if (eof || error) return false;
                    bool audioFull = !info.has_audio || audio.size() >= (size_t)rate * channels;
                    bool videoFull = !info.has_video || frames.size() >= 6;
                    return !(audioFull && videoFull);
                });
                if (quit) return;
                if (seekTo >= 0) {
                    double t = seekTo;
                    seekTo = -1;
                    audio.clear();
                    frames.clear();
                    audioPtsSet = false;
                    audioPts = t;
                    clockBase = t;
                    clockStart = lsp::NowSec();
                    eof = error = false;
                    rs.Reset();
                    l.unlock();
                    src->Seek(t);
                    continue;
                }
                if (eof || error) continue;
                bool audioFull = !info.has_audio || audio.size() >= (size_t)rate * channels;
                bool videoFull = !info.has_video || frames.size() >= 6;
                if (audioFull && videoFull) continue;
            }
            int r = src->Step(sink);
            if (r <= 0) {
                std::lock_guard<std::mutex> l(m);
                if (r == 0) eof = true;
                else error = true;
            }
        }
    }
};

namespace lsp {
namespace {

int ApiInit(const LspHost* h) {
    if (!h || h->api_version != LSP_API_VERSION) return 0;
    g_host = *h;
    std::string err;
    g_init = InitBackend(&g_host, &err);
    if (!g_init) Log(err);
    return g_init ? 1 : 0;
}

int ApiProbe(const char* path, LspInfo* info, LspImageFn thumb, void* user) {
    if (!g_init) return 0;
    auto src = CreateSource(&g_host);
    LspInfo inf{};
    std::string err;
    if (!src || !src->Open(path, &inf, false, &err)) return 0;
    if (info) *info = inf;
    if (thumb && inf.has_video) {
        bool got = false;
        Sink s;
        s.video = [&](const LsvImage& im, double) {
            if (!got) thumb(user, &im);
            got = true;
        };
        s.audio = [](const float*, int, int, int, double) {};
        if (inf.duration > 4) src->Seek(std::min(1.0, inf.duration * 0.1));  // skip a black first frame
        for (int i = 0; i < 400 && !got; i++)
            if (src->Step(s) <= 0) break;
    }
    return 1;
}

LspPlayer* ApiOpen(const char* path, int rate, int channels, LspInfo* info) {
    if (!g_init) return nullptr;
    auto* p = new LspPlayer();
    p->src = CreateSource(&g_host);
    p->rate = rate > 0 ? rate : 48000;
    p->channels = channels == 1 ? 1 : 2;
    std::string err;
    if (!p->src || !p->src->Open(path, &p->info, true, &err)) {
        Log("cannot open " + std::string(path) + ": " + err);
        delete p;
        return nullptr;
    }
    if (info) *info = p->info;
    p->th = std::thread([p] { p->DecodeLoop(); });
    return p;
}

LspPlayer* ApiOpenStream(const LspStream* s, int rate, int channels, LspInfo* info) {
    if (!g_init || !s || !s->read) return nullptr;
    auto* p = new LspPlayer();
    p->src = CreateSource(&g_host);
    p->rate = rate > 0 ? rate : 48000;
    p->channels = channels == 1 ? 1 : 2;
    std::string err;
    if (!p->src || !p->src->OpenStream(s, &p->info, true, &err)) {
        Log("cannot open the stream: " + err);
        delete p;
        return nullptr;
    }
    if (info) *info = p->info;
    p->th = std::thread([p] { p->DecodeLoop(); });
    return p;
}

void ApiClose(LspPlayer* p) {
    if (!p) return;
    {
        std::lock_guard<std::mutex> l(p->m);
        p->quit = true;
    }
    p->cv.notify_all();
    if (p->th.joinable()) p->th.join();
    delete p;
}

void ApiSetPlaying(LspPlayer* p, int playing) {
    if (!p) return;
    std::lock_guard<std::mutex> l(p->m);
    int st = p->state;
    if (st == LSP_ERROR) return;
    if (playing) {
        if (st == LSP_ENDED) {  // play again from the start
            p->seekTo = 0;
            p->clockBase = 0;
            p->cv.notify_all();
        }
        if (!p->clockRunning) {
            p->clockStart = NowSec();
            p->clockRunning = true;
        }
        p->state = LSP_PLAYING;
    } else {
        if (p->clockRunning) p->clockBase += NowSec() - p->clockStart;
        p->clockRunning = false;
        if (st == LSP_PLAYING) p->state = LSP_PAUSED;
    }
}

void ApiSeek(LspPlayer* p, double t) {
    if (!p) return;
    std::lock_guard<std::mutex> l(p->m);
    t = std::max(0.0, p->info.duration > 0 ? std::min(t, p->info.duration - 0.05) : t);
    p->seekTo = t;
    p->audio.clear();
    p->frames.clear();
    p->audioPts = t;
    p->audioPtsSet = false;
    p->clockBase = t;
    p->clockStart = NowSec();
    if (p->state == LSP_ENDED) p->state = LSP_PAUSED;
    p->cv.notify_all();
}

void ApiReadAudio(LspPlayer* p, float* out, int n) {
    size_t want = (size_t)n * (size_t)(p ? p->channels : 2);
    if (!p) {
        memset(out, 0, want * sizeof(float));
        return;
    }
    std::lock_guard<std::mutex> l(p->m);
    size_t got = 0;
    if (p->state == LSP_PLAYING && p->seekTo < 0) {
        got = std::min(want, p->audio.size());
        std::copy(p->audio.begin(), p->audio.begin() + (ptrdiff_t)got, out);
        p->audio.erase(p->audio.begin(), p->audio.begin() + (ptrdiff_t)got);
        p->audioPts += (double)(got / (size_t)p->channels) / p->rate;
        if (got < want && p->eof && (p->frames.empty() || !p->info.has_video)) p->state = LSP_ENDED;
    }
    if (got < want) memset(out + got, 0, (want - got) * sizeof(float));
    p->cv.notify_all();
}

int ApiVideoFrame(LspPlayer* p, LspImageFn fn, void* user) {
    if (!p) return 0;
    std::unique_lock<std::mutex> l(p->m);
    double pos = p->PositionLocked();
    bool changed = false;
    // newest picture that is due (drop late ones), or the first one after a seek
    while (!p->frames.empty() && (p->frames.front().pts <= pos + 0.005 || !p->shown.w)) {
        p->shown = std::move(p->frames.front());
        p->frames.pop_front();
        changed = true;
    }
    if (!p->info.has_audio && p->eof && p->frames.empty() && p->state == LSP_PLAYING) p->state = LSP_ENDED;
    if (p->info.duration > 0 && !p->info.has_audio && pos >= p->info.duration && p->state == LSP_PLAYING) p->state = LSP_ENDED;
    p->cv.notify_all();
    if (!changed || !fn) return 0;
    lsp::Frame& f = p->shown;
    LsvImage im{f.w, f.h, f.y.data(), f.uv.data(), f.uv.data() + 1, f.w, ((f.w + 1) / 2) * 2, 2, f.rotation, 0};
    fn(user, &im);
    return 1;
}

double ApiPosition(LspPlayer* p) {
    if (!p) return 0;
    std::lock_guard<std::mutex> l(p->m);
    return p->PositionLocked();
}

int ApiState(LspPlayer* p) {
    if (!p) return LSP_ERROR;
    std::lock_guard<std::mutex> l(p->m);
    if (p->error && p->audio.empty() && p->frames.empty()) return LSP_ERROR;
    return p->state;
}

const LspModule kModule = {LSP_API_VERSION, "LunaShare player", "1.1", ApiInit, ApiProbe, ApiOpen, ApiClose, ApiSetPlaying,
                           ApiSeek, ApiReadAudio, ApiVideoFrame, ApiPosition, ApiState, ApiOpenStream};

}  // namespace
}  // namespace lsp

extern "C" LSP_EXPORT const LspModule* lunashare_player_module(void) { return &lsp::kModule; }
