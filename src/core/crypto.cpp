#include "crypto.h"

#include <cstring>
#include <stdexcept>

#include "monocypher.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/random.h>
#endif
#endif

namespace ls {

void SecureRandom(void* buf, size_t n) {
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return;
#elif defined(__APPLE__)
    arc4random_buf(buf, n);
    return;
#else
#if defined(__linux__) && !defined(__ANDROID__)
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom((char*)buf + got, n - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got == n) return;
#endif
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t got2 = 0;
        while (got2 < n) {
            ssize_t r = read(fd, (char*)buf + got2, n - got2);
            if (r <= 0) break;
            got2 += (size_t)r;
        }
        close(fd);
        if (got2 == n) return;
    }
#endif
    // No usable CSPRNG: refuse to continue rather than produce weak keys.
    abort();
}

std::string ToHex(const uint8_t* p, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; i++) { s[i * 2] = hx[p[i] >> 4]; s[i * 2 + 1] = hx[p[i] & 15]; }
    return s;
}

bool FromHex(const std::string& h, uint8_t* out, size_t n) {
    if (h.size() != n * 2) return false;
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; i++) {
        int a = v(h[i * 2]), b = v(h[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

Identity::~Identity() { crypto_wipe(sk, sizeof(sk)); }

void Identity::Generate() {
    uint8_t seed[32];
    SecureRandom(seed, 32);
    seedHex = ToHex(seed, 32);
    crypto_eddsa_key_pair(sk, pk, seed);  // wipes seed
    valid = true;
}

bool Identity::LoadSeed(const std::string& hex) {
    uint8_t seed[32];
    if (!FromHex(hex, seed, 32)) return false;
    seedHex = hex;
    crypto_eddsa_key_pair(sk, pk, seed);
    valid = true;
    return true;
}

std::string KeyFingerprint(const std::string& pubHex) {
    uint8_t pk[32], h[16];
    if (!FromHex(pubHex, pk, 32)) return "";
    crypto_blake2b(h, sizeof(h), pk, 32);
    static const char* alpha = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";  // no 0/O/1/I
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    size_t bi = 0;
    while (out.size() < 19) {
        if (bits < 5) { acc = (acc << 8) | h[bi++]; bits += 8; }
        out += alpha[(acc >> (bits - 5)) & 31];
        bits -= 5;
        if (out.size() == 4 || out.size() == 9 || out.size() == 14) out += '-';
    }
    return out;
}

std::string SafetyNumber(const std::string& a, const std::string& b) {
    uint8_t pa[32], pb[32];
    if (!FromHex(a, pa, 32) || !FromHex(b, pb, 32)) return "";
    bool swap = memcmp(pa, pb, 32) > 0;
    uint8_t msg[64 + 16];
    memcpy(msg, "LunaShare safety", 16);
    memcpy(msg + 16, swap ? pb : pa, 32);
    memcpy(msg + 48, swap ? pa : pb, 32);
    uint8_t h[30];
    crypto_blake2b(h, sizeof(h), msg, sizeof(msg));
    std::string out;
    for (int g = 0; g < 6; g++) {
        uint64_t v = 0;
        for (int i = 0; i < 5; i++) v = (v << 8) | h[g * 5 + i];
        char buf[8];
        snprintf(buf, sizeof(buf), "%05u", (unsigned)(v % 100000));
        if (g) out += ' ';
        out += buf;
    }
    return out;
}

// ---------------------------------------------------------------- channel

static constexpr char kMagic[4] = {'L', 'S', 'H', '2'};
static constexpr size_t kHsSize = 4 + 32 + 32;

SecureChannel::~SecureChannel() {
    crypto_wipe(tx_, sizeof(tx_));
    crypto_wipe(rx_, sizeof(rx_));
}

bool SecureChannel::Handshake(SocketHandle s, bool initiator, int timeoutMs, std::string* err, const std::atomic<bool>* running) {
    initiator_ = initiator;
    uint8_t ephSk[32], ephPk[32], nonce[32];
    SecureRandom(ephSk, 32);
    SecureRandom(nonce, 32);
    crypto_x25519_public_key(ephPk, ephSk);
    uint8_t mine[kHsSize], theirs[kHsSize];
    memcpy(mine, kMagic, 4);
    memcpy(mine + 4, ephPk, 32);
    memcpy(mine + 36, nonce, 32);
    auto fail = [&](const char* why) {
        crypto_wipe(ephSk, 32);
        if (err) *err = why;
        return false;
    };
    if (!SendAll(s, mine, kHsSize)) return fail("send failed");
    int waited = 0, r = 0;
    while ((r = WaitReadable(s, 200)) == 0 && (waited += 200) < timeoutMs)
        if (running && !running->load()) return fail("cancelled");
    if (r != 1) return fail("handshake timeout");
    if (!RecvAll(s, theirs, kHsSize)) return fail("connection closed during handshake");
    if (memcmp(theirs, kMagic, 4) != 0) return fail("not a LunaShare v2 peer (old version?)");

    uint8_t shared[32];
    crypto_x25519(shared, ephSk, theirs + 4);
    crypto_wipe(ephSk, 32);
    uint8_t zero[32] = {};
    if (crypto_verify32(shared, zero) == 0) return fail("invalid key share");

    const uint8_t* ei = initiator ? mine + 4 : theirs + 4;
    const uint8_t* er = initiator ? theirs + 4 : mine + 4;
    const uint8_t* ni = initiator ? mine + 36 : theirs + 36;
    const uint8_t* nr = initiator ? theirs + 36 : mine + 36;
    uint8_t t[22 + 128];
    memcpy(t, "LunaShare v2 handshake", 22);
    memcpy(t + 22, ei, 32);
    memcpy(t + 54, er, 32);
    memcpy(t + 86, ni, 32);
    memcpy(t + 118, nr, 32);
    crypto_blake2b(th_, 64, t, sizeof(t));
    uint8_t keys[64];
    crypto_blake2b_keyed(keys, 64, shared, 32, th_, 64);
    crypto_wipe(shared, 32);
    memcpy(initiator ? tx_ : rx_, keys, 32);
    memcpy(initiator ? rx_ : tx_, keys + 32, 32);
    crypto_wipe(keys, 64);
    return true;
}

void SecureChannel::AuthMessage(bool initiatorRole, uint8_t* msg, size_t* len) const {
    memcpy(msg, "LunaShare auth", 14);
    msg[14] = initiatorRole ? 'I' : 'R';
    memcpy(msg + 15, th_, 64);
    *len = 15 + 64;
}

static std::string SignedMessage(const std::string& context, const std::string& data) {
    std::string m = "LunaShare sig v1\n" + context;
    m.push_back('\0');
    return m + data;
}

std::string SignData(const Identity& id, const std::string& context, const std::string& data) {
    uint8_t sig[64];
    std::string m = SignedMessage(context, data);
    crypto_eddsa_sign(sig, id.sk, (const uint8_t*)m.data(), m.size());
    return ToHex(sig, 64);
}

bool VerifyData(const std::string& pubHex, const std::string& context, const std::string& data, const std::string& sigHex) {
    uint8_t pk[32], sig[64];
    if (!FromHex(pubHex, pk, 32) || !FromHex(sigHex, sig, 64)) return false;
    std::string m = SignedMessage(context, data);
    return crypto_eddsa_check(sig, pk, (const uint8_t*)m.data(), m.size()) == 0;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::string& bin) {
    std::string out;
    out.reserve((bin.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bin.size(); i += 3) {
        uint32_t v = (uint8_t)bin[i] << 16 | (uint8_t)bin[i + 1] << 8 | (uint8_t)bin[i + 2];
        for (int k = 3; k >= 0; k--) out.push_back(kB64[(v >> (k * 6)) & 63]);
    }
    if (i < bin.size()) {
        uint32_t v = (uint8_t)bin[i] << 16 | (i + 1 < bin.size() ? (uint8_t)bin[i + 1] << 8 : 0);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(i + 1 < bin.size() ? kB64[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

bool Base64Decode(const std::string& text, std::string& out) {
    out.clear();
    uint32_t v = 0;
    int bits = 0;
    for (char ch : text) {
        int d;
        if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') d = ch - '0' + 52;
        else if (ch == '+' || ch == '-') d = 62;
        else if (ch == '/' || ch == '_') d = 63;
        else if (ch == '=' || ch == '\n' || ch == '\r') continue;
        else return false;
        v = v << 6 | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((v >> bits) & 0xFF));
        }
    }
    return true;
}

std::string SecureChannel::Sign(const Identity& id) const {
    uint8_t msg[80], sig[64];
    size_t len;
    AuthMessage(initiator_, msg, &len);
    crypto_eddsa_sign(sig, id.sk, msg, len);
    return ToHex(sig, 64);
}

bool SecureChannel::VerifyPeer(const std::string& pubHex, const std::string& sigHex) const {
    uint8_t pk[32], sig[64], msg[80];
    if (!FromHex(pubHex, pk, 32) || !FromHex(sigHex, sig, 64)) return false;
    size_t len;
    AuthMessage(!initiator_, msg, &len);  // the peer signed with the opposite role
    return crypto_eddsa_check(sig, pk, msg, len) == 0;
}

static void Nonce(uint64_t ctr, uint8_t n[24]) {
    memset(n, 0, 24);
    for (int i = 0; i < 8; i++) n[i] = (uint8_t)(ctr >> (8 * i));
}

bool SecureChannel::Send(SocketHandle s, Msg type, const void* payload, size_t len) {
    size_t body = 1 + len;
    if (body + 16 > kMaxFrame) return false;
    sendBuf_.resize(4 + body + 16);
    uint8_t* b = sendBuf_.data();
    PutU32(b, (uint32_t)(body + 16));
    b[4] = (uint8_t)type;
    if (len) memcpy(b + 5, payload, len);
    uint8_t n[24];
    Nonce(txCtr_++, n);
    crypto_aead_lock(b + 4, b + 4 + body, tx_, n, b, 4, b + 4, body);  // in place, length is authenticated
    return SendAll(s, b, sendBuf_.size());
}

bool SecureChannel::Recv(SocketHandle s, Frame& out) {
    uint8_t hdr[4];
    if (!RecvAll(s, hdr, 4)) return false;
    uint32_t len = GetU32(hdr);
    if (len < 17 || len > kMaxFrame) return false;
    std::vector<uint8_t> buf(len);
    if (!RecvAll(s, buf.data(), len)) return false;
    size_t body = len - 16;
    uint8_t n[24];
    Nonce(rxCtr_++, n);
    if (crypto_aead_unlock(buf.data(), buf.data() + body, rx_, n, hdr, 4, buf.data(), body) != 0) return false;  // tampered
    out.type = (Msg)buf[0];
    out.payload.assign(buf.begin() + 1, buf.begin() + (ptrdiff_t)body);
    return true;
}

}  // namespace ls
