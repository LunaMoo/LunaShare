#include "link_crypto.h"

#include <cstring>

#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace ls {

// ---- AES-256 (FIPS-197), encryption only
namespace {
const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

inline uint8_t Xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
}  // namespace

Aes256::Aes256(const uint8_t key[32]) {
    memcpy(rk, key, 32);
    uint8_t rcon = 1;
    for (int i = 8; i < 60; i++) {
        uint8_t t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if (i % 8 == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(kSbox[t[1]] ^ rcon);
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[u];
            rcon = Xt(rcon);
        } else if (i % 8 == 4) {
            for (auto& b : t) b = kSbox[b];
        }
        for (int j = 0; j < 4; j++) rk[i * 4 + j] = rk[(i - 8) * 4 + j] ^ t[j];
    }
}

void Aes256::Encrypt(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= 14; r++) {
        uint8_t t[16];
        // SubBytes + ShiftRows (column-major state: s[c*4 + row])
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) t[c * 4 + row] = kSbox[s[((c + row) % 4) * 4 + row]];
        if (r < 14)  // MixColumns
            for (int c = 0; c < 4; c++) {
                uint8_t* a = t + c * 4;
                uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                a[0] ^= (uint8_t)(all ^ Xt((uint8_t)(a0 ^ a1)));
                a[1] ^= (uint8_t)(all ^ Xt((uint8_t)(a1 ^ a2)));
                a[2] ^= (uint8_t)(all ^ Xt((uint8_t)(a2 ^ a3)));
                a[3] ^= (uint8_t)(all ^ Xt((uint8_t)(a3 ^ a0)));
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[r * 16 + i];
    }
    memcpy(out, s, 16);
}

// ---- the link scheme
static void Hmac512(const uint8_t* key, size_t kn, const void* a, size_t an, const void* b, size_t bn, uint8_t out[64]) {
    crypto_sha512_hmac_ctx c;
    crypto_sha512_hmac_init(&c, key, kn);
    if (an) crypto_sha512_hmac_update(&c, (const uint8_t*)a, an);
    if (bn) crypto_sha512_hmac_update(&c, (const uint8_t*)b, bn);
    crypto_sha512_hmac_final(&c, out);
}

LinkKeys DeriveLinkKeys(const uint8_t* secret, size_t n) {
    LinkKeys k;
    uint8_t h[64];
    static const char e[] = "LunaShare link enc", m[] = "LunaShare link mac";
    Hmac512(secret, n, e, sizeof(e) - 1, nullptr, 0, h);
    memcpy(k.enc, h, 32);
    Hmac512(secret, n, m, sizeof(m) - 1, nullptr, 0, h);
    memcpy(k.mac, h, 32);
    crypto_wipe(h, sizeof(h));
    return k;
}

static void Ctr(const LinkKeys& k, uint64_t index, const uint8_t* in, uint8_t* out, size_t n) {
    Aes256 aes(k.enc);
    uint8_t block[16], ks[16];
    for (int i = 0; i < 8; i++) block[i] = (uint8_t)(index >> (56 - 8 * i));
    uint64_t ctr = 0;
    for (size_t off = 0; off < n; off += 16, ctr++) {
        for (int i = 0; i < 8; i++) block[8 + i] = (uint8_t)(ctr >> (56 - 8 * i));
        aes.Encrypt(block, ks);
        size_t m = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < m; i++) out[off + i] = in[off + i] ^ ks[i];
    }
}

static void Tag(const LinkKeys& k, uint64_t index, bool last, const uint8_t* ct, size_t n, uint8_t tag[32]) {
    uint8_t head[13] = {'L', 'S', 'L', '1'};
    for (int i = 0; i < 8; i++) head[4 + i] = (uint8_t)(index >> (56 - 8 * i));
    head[12] = last ? 1 : 0;
    uint8_t h[64];
    Hmac512(k.mac, 32, head, sizeof(head), ct, n, h);
    memcpy(tag, h, 32);
}

std::string LinkSeal(const LinkKeys& k, uint64_t index, bool last, const uint8_t* data, size_t n) {
    std::string out(n + 32, '\0');
    Ctr(k, index, data, (uint8_t*)&out[0], n);
    Tag(k, index, last, (const uint8_t*)out.data(), n, (uint8_t*)&out[n]);
    return out;
}

bool LinkOpen(const LinkKeys& k, uint64_t index, bool last, const std::string& sealed, std::string& plain) {
    if (sealed.size() < 32) return false;
    size_t n = sealed.size() - 32;
    uint8_t tag[32];
    Tag(k, index, last, (const uint8_t*)sealed.data(), n, tag);
    if (crypto_verify32(tag, (const uint8_t*)sealed.data() + n) != 0) return false;
    plain.assign(n, '\0');
    Ctr(k, index, (const uint8_t*)sealed.data(), (uint8_t*)&plain[0], n);
    return true;
}

std::string LinkPinProof(const LinkKeys& k, const std::string& pin) {
    uint8_t h[64];
    std::string m = "pin:" + pin;
    Hmac512(k.mac, 32, m.data(), m.size(), nullptr, 0, h);
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; i++) s += hx[h[i] >> 4], s += hx[h[i] & 15];
    return s;
}

std::string B64Url(const uint8_t* d, size_t n) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        s += a[v >> 18 & 63];
        s += a[v >> 12 & 63];
        if (i + 1 < n) s += a[v >> 6 & 63];
        if (i + 2 < n) s += a[v & 63];
    }
    return s;
}

bool FromB64Url(const std::string& s, std::string& out) {
    out.clear();
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        int x = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '-' ? 62 : c == '_' ? 63 : -1;
        if (x < 0) return false;
        v = v << 6 | (uint32_t)x;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)(v >> bits & 255);
        }
    }
    return true;
}

}  // namespace ls
