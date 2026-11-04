// End-to-end encryption for LunaShare connections (Monocypher).
//
// Every device owns a long-term EdDSA identity key (Curve25519 + BLAKE2b).
// Each connection:
//   1. Both sides send in clear: "LSH2" | ephemeral X25519 public key (32) | random nonce (32)
//   2. th   = BLAKE2b-512("LunaShare v2 handshake" | eph_init | eph_resp | nonce_init | nonce_resp)
//      keys = BLAKE2b-512(key = X25519(eph_mine, eph_theirs), msg = th) -> 32 bytes initiator->responder,
//                                                                         32 bytes responder->initiator
//   3. All later frames are XChaCha20-Poly1305: [u32 len][ciphertext][16-byte MAC], the length is
//      authenticated, the nonce is a per-direction counter (never reused: fresh keys per connection).
//   4. The first encrypted frame (Hello) carries the identity public key and a signature over
//      ("LunaShare auth" | role | th). It proves the sender owns the key AND is the endpoint of this exact
//      key exchange, so a man in the middle cannot relay it. The receiver pins the key per device id
//      (trust on first use) and refuses a different key later. Users can compare safety numbers to rule out
//      a MITM on the very first contact.
// Forward secrecy: ephemeral keys are wiped after the handshake.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "net.h"
#include "protocol.h"

namespace ls {

void SecureRandom(void* buf, size_t n);
std::string ToHex(const uint8_t* p, size_t n);
bool FromHex(const std::string& h, uint8_t* out, size_t n);

struct Identity {
    uint8_t sk[64] = {};
    uint8_t pk[32] = {};
    bool valid = false;
    std::string seedHex;  // persisted secret (32-byte seed)
    void Generate();
    bool LoadSeed(const std::string& hex);
    std::string PubHex() const { return ToHex(pk, 32); }
    ~Identity();
};

// Detached signatures with an identity key (profiles, and later account statements / sealed messages).
// `context` separates uses: a signature made for one purpose never verifies for another.
std::string SignData(const Identity& id, const std::string& context, const std::string& data);  // hex
bool VerifyData(const std::string& pubHex, const std::string& context, const std::string& data, const std::string& sigHex);

// Account recovery: the 32-byte account seed as 24 words (BIP-39 English list, 8-bit SHA-256 checksum) and back.
std::string SeedToPhrase(const std::string& seedHex);
bool PhraseToSeed(const std::string& phrase, std::string& seedHex, std::string* err);
// Password-protected blobs (account backup files): Argon2id (32 MiB, 3 passes) + XChaCha20-Poly1305.
std::string PasswordEncrypt(const std::string& plain, const std::string& password);
bool PasswordDecrypt(const std::string& blob, const std::string& password, std::string& plain);

std::string Base64Encode(const std::string& bin);
bool Base64Decode(const std::string& text, std::string& out);

// Short human readable fingerprint of a public key, e.g. "K7Q2-9MXA-P4RT-WD3E".
std::string KeyFingerprint(const std::string& pubHex);
// Symmetric 30-digit code both devices display identically ("12345 67890 ..."), for verification.
std::string SafetyNumber(const std::string& pubA, const std::string& pubB);

class SecureChannel {
public:
    ~SecureChannel();
    // Blocking key exchange; both sides call it right after the socket is connected.
    // `running` (optional) aborts the wait as soon as it becomes false.
    bool Handshake(SocketHandle s, bool initiator, int timeoutMs, std::string* err, const std::atomic<bool>* running = nullptr);
    // Only one thread may Send and only one thread may Recv (each direction has its own counter).
    bool Send(SocketHandle s, Msg type, const void* payload, size_t len);
    bool Send(SocketHandle s, Msg type, const std::string& s2) { return Send(s, type, s2.data(), s2.size()); }
    bool Recv(SocketHandle s, Frame& out);

    std::string Sign(const Identity& id) const;  // hex signature for our role
    // 6 digits both ends of THIS connection compute identically (from the handshake hash): shown on both screens
    // before something important (linking a device), so a man in the middle would be noticed.
    std::string Code() const;
    bool VerifyPeer(const std::string& pubHex, const std::string& sigHex) const;

private:
    void AuthMessage(bool initiatorRole, uint8_t* msg, size_t* len) const;
    uint8_t tx_[32] = {}, rx_[32] = {};
    uint8_t th_[64] = {};
    uint64_t txCtr_ = 0, rxCtr_ = 0;
    bool initiator_ = false;
    std::vector<uint8_t> sendBuf_;
};

}  // namespace ls
