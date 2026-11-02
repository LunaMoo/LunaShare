// Built when no FFmpeg headers are available: HEVC/AV1 decoding is simply not offered.
// SPDX-License-Identifier: GPL-3.0-only
#include "ffdec.h"

namespace lsv {
bool FfDecoder::Load(const std::vector<std::string>&, std::string* err) {
    if (err) *err = "built without FFmpeg";
    return false;
}
bool FfDecoder::Loaded() { return false; }
bool FfDecoder::Open(int, bool, std::string* err) {
    if (err) *err = "built without FFmpeg";
    return false;
}
void FfDecoder::Close() {}
std::string FfDecoder::Name() const { return ""; }
bool FfDecoder::Decode(const uint8_t*, size_t, const std::function<void(const LsvImage&)>&) { return false; }
bool FfDecoder::HasSoftwareAv1() { return false; }
bool FfEncoder::Open(int, int, int, int, std::string* err) {
    if (err) *err = "built without FFmpeg";
    return false;
}
void FfEncoder::Close() {}
void FfEncoder::SetBitrate(int) {}
bool FfEncoder::Encode(const LsvImage&, bool, const std::function<void(const uint8_t*, size_t, bool)>&) { return false; }
}  // namespace lsv
