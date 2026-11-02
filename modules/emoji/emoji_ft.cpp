// Emoji backend for Linux/macOS: a colour emoji font (Noto Color Emoji, Apple Color Emoji, ...) through FreeType.
// Sequences (ZWJ, skin tones, flags, keycaps) come out of the font's GSUB ligatures, applied here without a shaper.
// SPDX-License-Identifier: GPL-3.0-only
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

#include "emoji_backend.h"

namespace lse {
namespace {

FT_Library g_ft = nullptr;
FT_Face g_face = nullptr;
std::vector<uint8_t> g_gsub;
// ligature lookups: first glyph -> list of (rest of the components, result glyph), longest first
std::map<uint32_t, std::vector<std::pair<std::vector<uint32_t>, uint32_t>>> g_ligs;

uint32_t U16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
uint32_t U32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

std::vector<uint32_t> Coverage(const uint8_t* base, size_t avail) {
    std::vector<uint32_t> g;
    if (avail < 4) return g;
    uint32_t fmt = U16(base), n = U16(base + 2);
    if (fmt == 1) {
        for (uint32_t i = 0; i < n && 4 + i * 2 + 2 <= avail; i++) g.push_back(U16(base + 4 + i * 2));
    } else if (fmt == 2) {
        for (uint32_t i = 0; i < n && 4 + i * 6 + 6 <= avail; i++) {
            uint32_t s = U16(base + 4 + i * 6), e = U16(base + 4 + i * 6 + 2);
            for (uint32_t x = s; x <= e && g.size() < 200000; x++) g.push_back(x);
        }
    }
    return g;
}

void ReadLigatureSubst(size_t off) {
    const uint8_t* d = g_gsub.data();
    size_t n = g_gsub.size();
    if (off + 6 > n || U16(d + off) != 1) return;
    std::vector<uint32_t> cov = Coverage(d + off + U16(d + off + 2), n - off - U16(d + off + 2));
    uint32_t sets = U16(d + off + 4);
    for (uint32_t s = 0; s < sets && s < cov.size() && off + 6 + s * 2 + 2 <= n; s++) {
        size_t so = off + U16(d + off + 6 + s * 2);
        if (so + 2 > n) continue;
        uint32_t ligs = U16(d + so);
        auto& list = g_ligs[cov[s]];
        for (uint32_t l = 0; l < ligs && so + 2 + l * 2 + 2 <= n; l++) {
            size_t lo = so + U16(d + so + 2 + l * 2);
            if (lo + 4 > n) continue;
            uint32_t glyph = U16(d + lo), comps = U16(d + lo + 2);
            if (comps == 0 || lo + 4 + (comps - 1) * 2 > n) continue;
            std::vector<uint32_t> rest;
            for (uint32_t c = 1; c < comps; c++) rest.push_back(U16(d + lo + 4 + (c - 1) * 2));
            list.push_back({rest, glyph});
        }
    }
}

void LoadGsub() {
    FT_ULong len = 0;
    if (FT_Load_Sfnt_Table(g_face, TTAG_GSUB, 0, nullptr, &len) || len < 10) return;
    g_gsub.resize(len);
    if (FT_Load_Sfnt_Table(g_face, TTAG_GSUB, 0, g_gsub.data(), &len)) return;
    const uint8_t* d = g_gsub.data();
    size_t lookupList = U16(d + 8);
    if (lookupList + 2 > len) return;
    uint32_t count = U16(d + lookupList);
    for (uint32_t i = 0; i < count && lookupList + 2 + i * 2 + 2 <= len; i++) {
        size_t lo = lookupList + U16(d + lookupList + 2 + i * 2);
        if (lo + 6 > len) continue;
        uint32_t type = U16(d + lo), subs = U16(d + lo + 4);
        for (uint32_t s = 0; s < subs && lo + 6 + s * 2 + 2 <= len; s++) {
            size_t so = lo + U16(d + lo + 6 + s * 2);
            if (type == 4) ReadLigatureSubst(so);
            else if (type == 7 && so + 8 <= len && U16(d + so + 2) == 4) ReadLigatureSubst(so + U32(d + so + 4));
        }
    }
    for (auto& [g, list] : g_ligs)
        std::stable_sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
}

std::vector<uint32_t> Decode(const char* s) {
    std::vector<uint32_t> cps;
    const auto* p = (const unsigned char*)s;
    while (*p) {
        uint32_t c = *p;
        int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        c = n == 0 ? c : n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07;
        p++;
        for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++, p++) c = c << 6 | (*p & 0x3F);
        cps.push_back(c);
    }
    return cps;
}

uint32_t Shape(const std::vector<uint32_t>& cps) {
    std::vector<uint32_t> g;
    for (uint32_t c : cps) {
        uint32_t gi = FT_Get_Char_Index(g_face, c);
        if (gi == 0 && (c == 0xFE0F || c == 0x200D)) continue;  // not in the font: ligatures then don't contain it
        g.push_back(gi);
    }
    if (g.empty() || g[0] == 0) return 0;
    for (size_t i = 0; i < g.size(); i++) {
        auto it = g_ligs.find(g[i]);
        if (it == g_ligs.end()) continue;
        for (auto& [rest, out] : it->second) {
            if (i + 1 + rest.size() > g.size()) continue;
            if (std::equal(rest.begin(), rest.end(), g.begin() + (ptrdiff_t)i + 1)) {
                g.erase(g.begin() + (ptrdiff_t)i + 1, g.begin() + (ptrdiff_t)(i + 1 + rest.size()));
                g[i] = out;
                break;
            }
        }
    }
    return g[0];  // whole sequence if it formed a ligature, else its first emoji
}

}  // namespace

bool BackendInit(const LseHost*, std::string* why) {
    static const char* fonts[] = {
#if defined(__APPLE__)
        "/System/Library/Fonts/Apple Color Emoji.ttc",
#endif
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf", "/usr/share/fonts/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf", "/usr/share/fonts/noto-emoji/NotoColorEmoji.ttf",
        "/usr/share/fonts/TTF/NotoColorEmoji.ttf", "/usr/share/fonts/truetype/twemoji/TwitterColorEmoji-SVGinOT.ttf",
        "/usr/share/fonts/joypixels/JoyPixels.ttf", nullptr};
    if (FT_Init_FreeType(&g_ft)) {
        *why = "FreeType failed";
        return false;
    }
    const char* env = getenv("LUNASHARE_EMOJI_FONT");
    for (int i = -1; !g_face && (i < 0 || fonts[i]); i++) {
        const char* f = i < 0 ? env : fonts[i];
        if (!f) continue;
        FT_Face face = nullptr;
        if (FT_New_Face(g_ft, f, 0, &face)) continue;
        if (!FT_HAS_COLOR(face)) {
            FT_Done_Face(face);
            continue;
        }
        g_face = face;
        Log(std::string("font ") + f);
    }
    if (!g_face) {
        *why = "no colour emoji font found (install Noto Color Emoji)";
        return false;
    }
    LoadGsub();
    return true;
}

bool BackendRender(const char* utf8, int size, std::vector<uint8_t>& rgba) {
    uint32_t glyph = Shape(Decode(utf8));
    if (!glyph) return false;
    if (FT_HAS_FIXED_SIZES(g_face)) {  // bitmap font (CBDT/sbix): pick the strike closest above the wanted size
        int best = 0;
        for (int i = 0; i < g_face->num_fixed_sizes; i++) {
            int h = g_face->available_sizes[i].height, bh = g_face->available_sizes[best].height;
            if ((bh < size && h > bh) || (h >= size && h < bh)) best = i;
        }
        FT_Select_Size(g_face, best);
    } else {
        FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)size);
    }
    if (FT_Load_Glyph(g_face, glyph, FT_LOAD_COLOR) || FT_Render_Glyph(g_face->glyph, FT_RENDER_MODE_NORMAL)) return false;
    const FT_Bitmap& b = g_face->glyph->bitmap;
    if (b.pixel_mode != FT_PIXEL_MODE_BGRA || b.width == 0 || b.rows == 0) return false;
    // fit into the square (box filter), premultiplied BGRA in, straight RGBA out
    int sw = (int)b.width, sh = (int)b.rows;
    float k = std::min((size * 0.96f) / sw, (size * 0.96f) / sh);
    float dw = sw * k, dh = sh * k, ox = (size - dw) * 0.5f, oy = (size - dh) * 0.5f;
    for (int y = 0; y < size; y++) {
        float sy0 = (y - oy) / k, sy1 = (y + 1 - oy) / k;
        if (sy1 <= 0 || sy0 >= sh) continue;
        for (int x = 0; x < size; x++) {
            float sx0 = (x - ox) / k, sx1 = (x + 1 - ox) / k;
            if (sx1 <= 0 || sx0 >= sw) continue;
            double acc[4] = {0, 0, 0, 0}, wsum = 0;
            int ys = std::max(0, (int)std::floor(sy0)), ye = std::min(sh - 1, (int)std::ceil(sy1) - 1);
            int xs = std::max(0, (int)std::floor(sx0)), xe = std::min(sw - 1, (int)std::ceil(sx1) - 1);
            for (int yy = ys; yy <= ye; yy++) {
                float wy = std::min(sy1, (float)yy + 1) - std::max(sy0, (float)yy);
                if (wy <= 0) continue;
                const uint8_t* row = b.buffer + (ptrdiff_t)yy * b.pitch;
                for (int xx = xs; xx <= xe; xx++) {
                    float wx = std::min(sx1, (float)xx + 1) - std::max(sx0, (float)xx);
                    if (wx <= 0) continue;
                    const uint8_t* p = row + xx * 4;
                    double w = (double)wx * wy;
                    for (int c = 0; c < 4; c++) acc[c] += p[c] * w;
                    wsum += w;
                }
            }
            double area = (sx1 - sx0) * (sy1 - sy0);  // pixels outside the glyph count as transparent
            if (wsum <= 0) continue;
            double a = acc[3] / area;
            uint8_t* o = &rgba[((size_t)y * size + x) * 4];
            if (a < 0.5) continue;
            o[0] = (uint8_t)std::min(255.0, acc[2] / area * 255.0 / a);
            o[1] = (uint8_t)std::min(255.0, acc[1] / area * 255.0 / a);
            o[2] = (uint8_t)std::min(255.0, acc[0] / area * 255.0 / a);
            o[3] = (uint8_t)std::min(255.0, a);
        }
    }
    return true;
}

}  // namespace lse
