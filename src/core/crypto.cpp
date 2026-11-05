#include "crypto.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "hash.h"
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

static const char* const kWords[2048] = {
#include "wordlist_en.inc"
};

std::string SeedToPhrase(const std::string& seedHex) {
    uint8_t seed[32];
    if (!FromHex(seedHex, seed, 32)) return {};
    uint8_t h[32];
    {
        Sha256 sh;
        sh.Update(seed, 32);
        sh.Final(h);
    }
    // 256 bits of seed + 8 checksum bits = 24 x 11 bits
    std::string out;
    for (int w = 0; w < 24; w++) {
        int v = 0;
        for (int b = 0; b < 11; b++) {
            int bit = w * 11 + b;
            int byte = bit < 256 ? (seed[bit / 8] >> (7 - bit % 8)) & 1 : (h[0] >> (7 - (bit - 256))) & 1;
            v = v << 1 | byte;
        }
        if (w) out += ' ';
        out += kWords[v];
    }
    crypto_wipe(seed, sizeof(seed));
    return out;
}

bool PhraseToSeed(const std::string& phrase, std::string& seedHex, std::string* err) {
    std::vector<int> idx;
    std::string word;
    auto flush = [&]() -> bool {
        if (word.empty()) return true;
        for (auto& ch : word) ch = (char)tolower((unsigned char)ch);
        // the first four letters identify a word (as in BIP-39), so short forms work too
        int found = -1;
        for (int i = 0; i < 2048 && found < 0; i++)
            if (word == kWords[i] || (word.size() >= 4 && strncmp(kWords[i], word.c_str(), 4) == 0 && strlen(kWords[i]) >= 4)) found = i;
        if (found < 0) {
            if (err) *err = "unknown word: " + word;
            return false;
        }
        idx.push_back(found);
        word.clear();
        return true;
    };
    for (char ch : phrase) {
        if (isalpha((unsigned char)ch)) word.push_back(ch);
        else if (!flush()) return false;
    }
    if (!flush()) return false;
    if (idx.size() != 24) {
        if (err) *err = "the recovery phrase has 24 words (got " + std::to_string(idx.size()) + ")";
        return false;
    }
    uint8_t seed[32] = {}, check = 0;
    for (int w = 0; w < 24; w++)
        for (int b = 0; b < 11; b++) {
            int bit = w * 11 + b, v = (idx[(size_t)w] >> (10 - b)) & 1;
            if (bit < 256) seed[bit / 8] |= (uint8_t)(v << (7 - bit % 8));
            else check |= (uint8_t)(v << (7 - (bit - 256)));
        }
    uint8_t h[32];
    Sha256 sh;
    sh.Update(seed, 32);
    sh.Final(h);
    if (h[0] != check) {
        if (err) *err = "the words don't match (a typo or a word missing?)";
        return false;
    }
    seedHex = ToHex(seed, 32);
    crypto_wipe(seed, sizeof(seed));
    return true;
}

static void PasswordKey(const std::string& password, const uint8_t salt[16], uint8_t key[32]) {
    crypto_argon2_config cfg = {CRYPTO_ARGON2_ID, 32768, 3, 1};  // 32 MiB
    std::vector<uint8_t> work((size_t)cfg.nb_blocks * 1024);
    crypto_argon2_inputs in = {(const uint8_t*)password.data(), salt, (uint32_t)password.size(), 16};
    crypto_argon2(key, 32, work.data(), cfg, in, crypto_argon2_no_extras);
    crypto_wipe(work.data(), work.size());
}

std::string PasswordEncrypt(const std::string& plain, const std::string& password) {
    uint8_t salt[16], nonce[24], key[32], mac[16];
    SecureRandom(salt, 16);
    SecureRandom(nonce, 24);
    PasswordKey(password, salt, key);
    std::string out = "LSB1";
    out.append((const char*)salt, 16);
    out.append((const char*)nonce, 24);
    std::string ct(plain.size(), '\0');
    crypto_aead_lock((uint8_t*)&ct[0], mac, key, nonce, (const uint8_t*)"LSB1", 4, (const uint8_t*)plain.data(), plain.size());
    crypto_wipe(key, 32);
    out.append((const char*)mac, 16);
    return out + ct;
}

bool PasswordDecrypt(const std::string& blob, const std::string& password, std::string& plain) {
    if (blob.size() < 60 || blob.compare(0, 4, "LSB1") != 0) return false;
    const uint8_t* p = (const uint8_t*)blob.data();
    uint8_t key[32];
    PasswordKey(password, p + 4, key);
    plain.assign(blob.size() - 60, '\0');
    int r = crypto_aead_unlock((uint8_t*)&plain[0], p + 44, key, p + 20, (const uint8_t*)"LSB1", 4, p + 60, blob.size() - 60);
    crypto_wipe(key, 32);
    if (r != 0) plain.clear();
    return r == 0;
}

static void SealKey(const uint8_t shared[32], const uint8_t eph[32], const uint8_t rcpt[32], uint8_t key[32]) {
    static const char ctx[] = "LunaShare seal v1";
    uint8_t buf[sizeof(ctx) + 96];
    memcpy(buf, ctx, sizeof(ctx));
    memcpy(buf + sizeof(ctx), shared, 32);
    memcpy(buf + sizeof(ctx) + 32, eph, 32);
    memcpy(buf + sizeof(ctx) + 64, rcpt, 32);
    crypto_blake2b(key, 32, buf, sizeof(buf));
    crypto_wipe(buf, sizeof(buf));
}

std::string SealTo(const std::string& recipientPubHex, const std::string& plain) {
    uint8_t edpk[32], rcpt[32], esk[32], epk[32], shared[32], key[32], nonce[24], mac[16];
    if (!FromHex(recipientPubHex, edpk, 32)) return {};
    crypto_eddsa_to_x25519(rcpt, edpk);
    SecureRandom(esk, 32);
    crypto_x25519_public_key(epk, esk);
    crypto_x25519(shared, esk, rcpt);
    SealKey(shared, epk, rcpt, key);
    SecureRandom(nonce, 24);
    std::string ct(plain.size(), '\0');
    crypto_aead_lock((uint8_t*)&ct[0], mac, key, nonce, epk, 32, (const uint8_t*)plain.data(), plain.size());
    crypto_wipe(esk, 32);
    crypto_wipe(shared, 32);
    crypto_wipe(key, 32);
    std::string out(1, '\x01');
    out.append((const char*)epk, 32);
    out.append((const char*)nonce, 24);
    out.append((const char*)mac, 16);
    return out + ct;
}

bool OpenSealed(const Identity& me, const std::string& sealed, std::string& plain) {
    if (sealed.size() < 73 || sealed[0] != 1) return false;
    const uint8_t* p = (const uint8_t*)sealed.data();
    // the X25519 private key that belongs to the EdDSA key: BLAKE2b-512(seed)[0..31] (Monocypher's EdDSA hash)
    uint8_t h[64], xsk[32], xpk[32], shared[32], key[32];
    crypto_blake2b(h, 64, me.sk, 32);
    memcpy(xsk, h, 32);
    crypto_eddsa_to_x25519(xpk, me.pk);
    crypto_x25519(shared, xsk, p + 1);
    SealKey(shared, p + 1, xpk, key);
    plain.assign(sealed.size() - 73, '\0');
    int r = crypto_aead_unlock((uint8_t*)&plain[0], p + 57, key, p + 33, p + 1, 32, p + 73, sealed.size() - 73);
    crypto_wipe(h, 64);
    crypto_wipe(xsk, 32);
    crypto_wipe(shared, 32);
    crypto_wipe(key, 32);
    if (r != 0) plain.clear();
    return r == 0;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string KeySeal(const uint8_t key[32], const std::string& plain) {
    std::string out(40 + plain.size(), '\0');
    uint8_t* p = (uint8_t*)&out[0];
    SecureRandom(p, 24);
    crypto_aead_lock(p + 40, p + 24, key, p, nullptr, 0, (const uint8_t*)plain.data(), plain.size());
    return out;
}

bool KeyOpen(const uint8_t key[32], const uint8_t* data, size_t len, std::string& plain) {
    if (len < 40) return false;
    plain.resize(len - 40);
    return crypto_aead_unlock(plain.empty() ? nullptr : (uint8_t*)&plain[0], data + 24, key, data, nullptr, 0, data + 40, len - 40) == 0;
}

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

std::string SecureChannel::Code() const {
    uint8_t h[32];
    static const char ctx[] = "LunaShare confirmation code";
    uint8_t buf[64 + sizeof(ctx)];
    memcpy(buf, ctx, sizeof(ctx));
    memcpy(buf + sizeof(ctx), th_, 64);
    crypto_blake2b(h, sizeof(h), buf, sizeof(buf));
    uint32_t v = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
    char out[8];
    snprintf(out, sizeof(out), "%06u", v % 1000000u);
    return std::string(out, 3) + " " + std::string(out + 3, 3);
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
