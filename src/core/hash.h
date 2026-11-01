// SHA-256 (full-file integrity, before and after transfer) and CRC-32 (per data chunk).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ls {

class Sha256 {
public:
    Sha256() { Reset(); }
    void Reset();
    void Update(const void* data, size_t len);
    void Final(uint8_t out[32]);
    std::string FinalHex();

private:
    void Block(const uint8_t* p);
    uint32_t h_[8];
    uint64_t len_;
    uint8_t buf_[64];
    size_t bufLen_;
};

std::string Sha256Hex(const void* data, size_t len);

// Hashes a file. `progress` (bytes done) and `cancel` are optional. Returns empty string on error/cancel.
std::string Sha256File(const std::string& path, std::atomic<int64_t>* progress = nullptr,
                       const std::atomic<bool>* cancel = nullptr);

uint32_t Crc32(const void* data, size_t len, uint32_t crc = 0);

}  // namespace ls
