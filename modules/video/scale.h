// Picture scaling in front of the encoder: a call can send a smaller picture than the camera takes without
// restarting the camera (a restart costs a second of black and a fresh keyframe, just when the link is struggling).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "lunashare_video.h"

namespace lsv {

// The size to encode a srcW x srcH picture at so it fits a boxW x boxH box (either orientation), keeping the aspect
// ratio; never bigger than the source. widthAlign: 2, or 16 for encoders that want macroblock-aligned widths.
inline void FitInBox(int srcW, int srcH, int boxW, int boxH, int widthAlign, int& w, int& h) {
    if (boxW <= 0 || boxH <= 0) {
        w = srcW & ~(widthAlign - 1);
        h = srcH & ~1;
        return;
    }
    int bigBox = std::max(boxW, boxH), smallBox = std::min(boxW, boxH);
    bool landscape = srcW >= srcH;
    int bw = landscape ? bigBox : smallBox, bh = landscape ? smallBox : bigBox;
    double f = std::min(1.0, std::min((double)bw / srcW, (double)bh / srcH));
    w = std::max(16, (int)(srcW * f + 0.5) & ~(widthAlign - 1));
    h = std::max(16, (int)(srcH * f + 0.5) & ~1);
}

namespace detail {
// one plane, area average (each output pixel is the mean of the source pixels it covers)
inline void ScalePlane(const uint8_t* src, int sw, int sh, int sstride, int spix, uint8_t* dst, int dw, int dh, int dstride,
                       int dpix) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * sh / dh));
        uint8_t* d = dst + (size_t)y * dstride;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * sw / dw));
            uint32_t sum = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t* s = src + (size_t)yy * sstride + (size_t)x0 * spix;
                for (int xx = x0; xx < x1; xx++, s += spix) sum += *s;
            }
            d[(size_t)x * dpix] = (uint8_t)(sum / (uint32_t)((y1 - y0) * (x1 - x0)));
        }
    }
}
}  // namespace detail

// Scales a YUV 4:2:0 picture (any layout LsvImage describes) to dw x dh, tightly packed: I420, or NV12 when nv12.
inline void ScaleYuv420(const LsvImage& s, int dw, int dh, bool nv12, std::vector<uint8_t>& out) {
    out.resize((size_t)dw * dh * 3 / 2);
    uint8_t* Y = out.data();
    uint8_t* C = Y + (size_t)dw * dh;
    detail::ScalePlane(s.y, s.width, s.height, s.y_stride, 1, Y, dw, dh, dw, 1);
    int scw = s.width / 2, sch = s.height / 2, dcw = dw / 2, dch = dh / 2;
    if (nv12) {
        detail::ScalePlane(s.u, scw, sch, s.uv_stride, s.uv_pixel_stride, C, dcw, dch, dw, 2);
        detail::ScalePlane(s.v, scw, sch, s.uv_stride, s.uv_pixel_stride, C + 1, dcw, dch, dw, 2);
    } else {
        detail::ScalePlane(s.u, scw, sch, s.uv_stride, s.uv_pixel_stride, C, dcw, dch, dcw, 1);
        detail::ScalePlane(s.v, scw, sch, s.uv_stride, s.uv_pixel_stride, C + (size_t)dcw * dch, dcw, dch, dcw, 1);
    }
}

// Lets frames through at most `fps` times a second (0 = all of them); the camera keeps its own rate.
struct FrameGate {
    int64_t next = 0;
    bool Pass(int fps, int64_t nowMs) {
        if (fps <= 0) return true;
        int64_t step = 1000 / fps;
        if (nowMs + 3 < next) return false;  // (3 ms of slack: camera timing wobbles)
        next = std::max(next + step, nowMs);  // (after a pause: from now on, no burst to catch up)
        return true;
    }
};

}  // namespace lsv
