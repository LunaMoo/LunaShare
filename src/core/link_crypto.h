// LunaShare - the encryption of share links ("a link for anyone"): what a browser can undo with WebCrypto alone.
//
// The link carries a 32-byte secret S after '#' (browsers never send that part anywhere). From it:
//   enc = HMAC-SHA-512(S, "LunaShare link enc")[0..32)   AES-256-CTR key
//   mac = HMAC-SHA-512(S, "LunaShare link mac")[0..32)   HMAC-SHA-512 key
// The file goes in parts of kLinkChunk bytes; part i is
//   AES-256-CTR(enc, counter block = i as 8 bytes big-endian || 8 zero bytes) of the plain bytes, then
//   tag = HMAC-SHA-512(mac, "LSL1" || i (8 bytes BE) || last (1 byte) || ciphertext)[0..32)  appended.
// The description (name, size, number of parts, SHA-256) is part 2^64-1 with last = 1. A changed, swapped, missing or
// extra part fails its tag: the browser checks every part, so the file is whole and unchanged when it says so.
// A PIN is checked by the sending device: proof = hex(HMAC-SHA-512(mac, "pin:" || PIN)[0..16)).
#pragma once
#include <cstdint>
#include <string>

namespace ls {

constexpr size_t kLinkChunk = 1 << 20;
constexpr uint64_t kLinkMetaIndex = ~0ull;

struct LinkKeys {
    uint8_t enc[32], mac[32];
};
LinkKeys DeriveLinkKeys(const uint8_t* secret, size_t n);  // the link secret: 16 bytes

// ciphertext || 32-byte tag
std::string LinkSeal(const LinkKeys& k, uint64_t index, bool last, const uint8_t* data, size_t n);
// false: the tag doesn't match (wrong key, changed bytes, wrong place)
bool LinkOpen(const LinkKeys& k, uint64_t index, bool last, const std::string& sealed, std::string& plain);
std::string LinkPinProof(const LinkKeys& k, const std::string& pin);

// URL-safe base64 without padding (link ids, the secret in the link)
std::string B64Url(const uint8_t* d, size_t n);
bool FromB64Url(const std::string& s, std::string& out);

// AES-256 (encryption direction only: CTR needs nothing else)
struct Aes256 {
    uint8_t rk[240];
    explicit Aes256(const uint8_t key[32]);
    void Encrypt(const uint8_t in[16], uint8_t out[16]) const;
};

}  // namespace ls
