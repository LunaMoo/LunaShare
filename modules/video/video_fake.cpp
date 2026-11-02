// Test backend: a synthetic camera and a trivial "codec" (downscaled luma), so the core can be tested anywhere.
// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "video_backend.h"

namespace lsv {
namespace {

class FakeBackend : public Backend {
public:
    explicit FakeBackend(Sink* s) : sink_(s) {}
    ~FakeBackend() override { StopCamera(); }

    std::vector<LsvCamera> Cameras() override {
        LsvCamera a{}, b{};
        snprintf(a.id, sizeof(a.id), "front");
        snprintf(a.name, sizeof(a.name), "Test pattern (front)");
        a.facing = LSV_FACING_FRONT;
        snprintf(b.id, sizeof(b.id), "back");
        snprintf(b.name, sizeof(b.name), "Test pattern (back)");
        b.facing = LSV_FACING_BACK;
        return {a, b};
    }

    bool StartCamera(const std::string& id, const LsvSettings& s, std::string*) override {
        StopCamera();
        s_ = s;
        bitrate_ = s.bitrate;
        front_ = id == "front";
        run_ = true;
        th_ = std::thread([this] { Loop(); });
        return true;
    }
    void StopCamera() override {
        run_ = false;
        if (th_.joinable()) th_.join();
    }
    void SetBitrate(int bps) override { bitrate_ = bps; }
    void RequestKeyframe() override { key_ = true; }
    std::vector<LsvControl> Controls() override { return {{LSV_CTL_ZOOM, 1, 4, 0.1f, zoom_}}; }
    void SetControl(int id, float v) override {
        if (id == LSV_CTL_ZOOM) zoom_ = v;
    }

    bool StartDecoder() override {
        haveKey_ = false;
        return true;
    }
    void Decode(const uint8_t* d, size_t n, bool key, int w, int h) override {
        if (n < 13 || d[4] != (key ? 0x65 : 0x41)) { sink_->OnDecoderError(); return; }
        if (key) haveKey_ = true;
        if (!haveKey_) { sink_->OnDecoderError(); return; }
        int sw = d[5] | d[6] << 8, sh = d[7] | d[8] << 8;
        if (sw <= 0 || sh <= 0 || sw > w || sh > h || n < 13 + (size_t)(sw * sh) * 3) { sink_->OnDecoderError(); return; }
        int f = w / sw;
        img_.assign((size_t)w * h, 0);
        uv_.assign((size_t)w * (h / 2), 128);
        const uint8_t *Y = d + 13, *U = Y + sw * sh, *V = U + sw * sh;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int gi = std::min(y / f, sh - 1) * sw + std::min(x / f, sw - 1);
                img_[(size_t)y * w + x] = Y[gi];
                if (!(y & 1) && !(x & 1)) {
                    uv_[(size_t)(y / 2) * w + x] = U[gi];
                    uv_[(size_t)(y / 2) * w + x + 1] = V[gi];
                }
            }
        LsvImage im{w, h, img_.data(), uv_.data(), uv_.data() + 1, w, w, 2, 0, 0};
        lastFrame = (int)(d[9] | d[10] << 8 | d[11] << 16 | (uint32_t)d[12] << 24);
        sink_->OnDecoded(im);
    }
    void StopDecoder() override {}
    std::string EncoderName() override { return "test"; }
    std::string DecoderName() override { return "test"; }

    std::atomic<int> lastFrame{-1};

private:
    void Loop() {
        int w = s_.width, h = s_.height, fps = std::max(1, s_.fps), frame = 0;
        std::vector<uint8_t> yp((size_t)w * h), uvp((size_t)w * h / 2, 128);
        auto next = std::chrono::steady_clock::now();
        while (run_) {
            // a colourful test card with a ball moving across it
            float bx = (float)((frame * 7) % (w + 200) - 100), by = h * 0.5f + h * 0.25f * std::sin(frame * 0.08f);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    float dx = x - bx, dy = y - by;
                    bool ball = dx * dx + dy * dy < (h / 8.f) * (h / 8.f);
                    yp[(size_t)y * w + x] = ball ? 230 : (uint8_t)(60 + (x * 120) / w + ((x / 40 + y / 40) % 2) * 20);
                    if (!(y & 1) && !(x & 1)) {
                        uvp[(size_t)(y / 2) * w + x] = ball ? 60 : (uint8_t)(70 + (x * 120) / w);
                        uvp[(size_t)(y / 2) * w + x + 1] = ball ? 200 : (uint8_t)(180 - (y * 110) / h);
                    }
                }
            int rot = getenv("LSV_FAKE_ROTATION") ? atoi(getenv("LSV_FAKE_ROTATION")) : 0;  // pretend to be a phone sensor
            LsvImage im{w, h, yp.data(), uvp.data(), uvp.data() + 1, w, w, 2, rot, front_ ? 1 : 0};
            sink_->OnLocalImage(im);
            bool key = key_.exchange(false) || frame % (fps * 2) == 0;
            // "quality" follows the bitrate: the coarsest grid that fits it
            int f = 4;
            while (f < 64 && (size_t)((w / f) * (h / f) * 3) * (size_t)fps * 8 > (size_t)bitrate_ * 8 / 10) f *= 2;
            int sw = std::max(1, w / f), sh = std::max(1, h / f);
            size_t raw = 13 + (size_t)(sw * sh) * 3;
            size_t target = std::max(raw, (size_t)bitrate_ / 8 / (size_t)fps * (key ? 3 : 1));
            std::vector<uint8_t> out(target, 0);
            out[3] = 1;
            out[4] = key ? 0x65 : 0x41;
            out[5] = (uint8_t)sw; out[6] = (uint8_t)(sw >> 8); out[7] = (uint8_t)sh; out[8] = (uint8_t)(sh >> 8);
            for (int i = 0; i < 4; i++) out[9 + i] = (uint8_t)(frame >> (8 * i));
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++) {
                    size_t sy = (size_t)(y * f), sx = (size_t)(x * f);
                    out[13 + y * sw + x] = yp[sy * w + sx];
                    out[13 + sw * sh + y * sw + x] = uvp[(sy / 2) * w + (sx & ~(size_t)1)];
                    out[13 + 2 * sw * sh + y * sw + x] = uvp[(sy / 2) * w + (sx & ~(size_t)1) + 1];
                }
            EncodedFrame ef;
            ef.data = out.data();
            ef.len = out.size();
            ef.key = key;
            ef.width = w;
            ef.height = h;
            ef.rotation = rot;
            ef.mirror = front_;
            sink_->OnEncoded(ef);
            frame++;
            next += std::chrono::milliseconds(1000 / fps);
            std::this_thread::sleep_until(next);
        }
    }

    Sink* sink_;
    LsvSettings s_{};
    std::atomic<int> bitrate_{0};
    std::atomic<bool> run_{false}, key_{false};
    bool front_ = true, haveKey_ = false;
    float zoom_ = 1;
    std::thread th_;
    std::vector<uint8_t> img_, uv_;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend(Sink* sink, const LsvHost*) { return std::make_unique<FakeBackend>(sink); }

}  // namespace lsv
