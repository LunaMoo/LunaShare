#include "hash.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "platform.h"

namespace ls {

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void Sha256::Reset() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h_, init, sizeof(h_));
    len_ = 0;
    bufLen_ = 0;
}

void Sha256::Block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::Update(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    len_ += len;
    if (bufLen_) {
        size_t take = 64 - bufLen_;
        if (take > len) take = len;
        memcpy(buf_ + bufLen_, p, take);
        bufLen_ += take; p += take; len -= take;
        if (bufLen_ == 64) { Block(buf_); bufLen_ = 0; }
    }
    while (len >= 64) { Block(p); p += 64; len -= 64; }
    if (len) { memcpy(buf_, p, len); bufLen_ = len; }
}

void Sha256::Final(uint8_t out[32]) {
    uint64_t bits = len_ * 8;
    uint8_t pad = 0x80;
    Update(&pad, 1);
    uint8_t zero = 0;
    while (bufLen_ != 56) Update(&zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    Update(lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(h_[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h_[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h_[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h_[i];
    }
}

std::string Sha256::FinalHex() {
    uint8_t d[32];
    Final(d);
    static const char* hx = "0123456789abcdef";
    std::string s(64, '0');
    for (int i = 0; i < 32; i++) { s[i * 2] = hx[d[i] >> 4]; s[i * 2 + 1] = hx[d[i] & 15]; }
    return s;
}

std::string Sha256Hex(const void* data, size_t len) {
    Sha256 s;
    s.Update(data, len);
    return s.FinalHex();
}

std::string Sha256File(const std::string& path, std::atomic<int64_t>* progress, const std::atomic<bool>* cancel) {
    FILE* f = OpenFile(path, "rb");
    if (!f) return {};
    std::vector<uint8_t> buf(1 << 20);
    Sha256 s;
    int64_t done = 0;
    for (;;) {
        if (cancel && cancel->load()) { fclose(f); return {}; }
        size_t n = fread(buf.data(), 1, buf.size(), f);
        if (n) { s.Update(buf.data(), n); done += (int64_t)n; if (progress) progress->store(done); }
        if (n < buf.size()) break;
    }
    bool err = ferror(f) != 0;
    fclose(f);
    if (err) return {};
    return s.FinalHex();
}

static uint32_t g_crcTable[8][256];
static bool g_crcInit = [] {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crcTable[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++) g_crcTable[t][i] = (g_crcTable[t - 1][i] >> 8) ^ g_crcTable[0][g_crcTable[t - 1][i] & 0xFF];
    return true;
}();

uint32_t Crc32(const void* data, size_t len, uint32_t crc) {
    (void)g_crcInit;
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    while (len >= 8) {  // slicing-by-8
        uint32_t a = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        crc = g_crcTable[7][a & 0xFF] ^ g_crcTable[6][(a >> 8) & 0xFF] ^ g_crcTable[5][(a >> 16) & 0xFF] ^ g_crcTable[4][a >> 24] ^
              g_crcTable[3][p[4]] ^ g_crcTable[2][p[5]] ^ g_crcTable[1][p[6]] ^ g_crcTable[0][p[7]];
        p += 8; len -= 8;
    }
    while (len--) crc = g_crcTable[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

}  // namespace ls
