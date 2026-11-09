#include "avatar.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "image_cache.h"
#include "platform.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

static void Append(void* ctx, void* data, int size) { ((std::string*)ctx)->append((const char*)data, (size_t)size); }

bool MakeSmallerJpeg(const std::string& path, int maxSide, const std::string& outPath) {
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    if (!DecodeImageFile(path, maxSide, px, w, h) || w <= 0 || h <= 0) return false;
    std::vector<unsigned char> rgb((size_t)w * h * 3);
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {  // (on white where it was see-through)
        unsigned a = px[i * 4 + 3];
        for (int c = 0; c < 3; c++) rgb[i * 3 + c] = (unsigned char)((px[i * 4 + c] * a + 255 * (255 - a)) / 255);
    }
    std::string jpeg;
    if (!stbi_write_jpg_to_func(Append, &jpeg, w, h, 3, rgb.data(), 85)) return false;
    FILE* f = ls::OpenFile(outPath, "wb");
    if (!f) return false;
    bool ok = fwrite(jpeg.data(), 1, jpeg.size(), f) == jpeg.size();
    fclose(f);
    return ok;
}

bool MakeAvatarJpeg(const std::string& path, float cx, float cy, float zoom, std::string& jpeg, std::string* err) {
    std::vector<unsigned char> src;
    int w = 0, h = 0;
    // decode at a size that keeps enough detail for the zoomed-in square
    int maxSide = (int)std::clamp(kAvatarSide * 2 * zoom * 2.0f, 512.0f, 4096.0f);
    if (!DecodeImageFile(path, maxSide, src, w, h)) {
        if (err) *err = "could not open the picture";
        return false;
    }
    float side = std::min(w, h) / std::max(1.0f, zoom);
    float x0 = std::clamp(cx * w - side * 0.5f, 0.0f, w - side), y0 = std::clamp(cy * h - side * 0.5f, 0.0f, h - side);
    // area-averaging resample of the square to kAvatarSide (sharp when shrinking, bilinear-ish when enlarging)
    const int N = kAvatarSide;
    std::vector<unsigned char> out((size_t)N * N * 3);
    float step = side / N;
    int taps = std::clamp((int)std::ceil(step), 1, 6);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float acc[3] = {0, 0, 0};
            for (int sy = 0; sy < taps; sy++)
                for (int sx = 0; sx < taps; sx++) {
                    float fx = x0 + (x + (sx + 0.5f) / taps) * step - 0.5f, fy = y0 + (y + (sy + 0.5f) / taps) * step - 0.5f;
                    int ix = std::clamp((int)std::floor(fx), 0, w - 1), iy = std::clamp((int)std::floor(fy), 0, h - 1);
                    int ix1 = std::min(ix + 1, w - 1), iy1 = std::min(iy + 1, h - 1);
                    float ax = std::clamp(fx - ix, 0.0f, 1.0f), ay = std::clamp(fy - iy, 0.0f, 1.0f);
                    for (int c = 0; c < 3; c++) {
                        auto P = [&](int px, int py) { return (float)src[((size_t)py * w + px) * 4 + c]; };
                        float top = P(ix, iy) + (P(ix1, iy) - P(ix, iy)) * ax, bot = P(ix, iy1) + (P(ix1, iy1) - P(ix, iy1)) * ax;
                        acc[c] += top + (bot - top) * ay;
                    }
                }
            for (int c = 0; c < 3; c++) out[((size_t)y * N + x) * 3 + c] = (unsigned char)std::lround(acc[c] / (taps * taps));
        }
    for (int q = 88; q >= 40; q -= 8) {  // stay well under the size limit other devices accept
        jpeg.clear();
        if (!stbi_write_jpg_to_func(Append, &jpeg, N, N, 3, out.data(), q)) break;
        if (jpeg.size() <= 60 * 1024) return true;
    }
    if (err) *err = "could not encode the picture";
    return false;
}
