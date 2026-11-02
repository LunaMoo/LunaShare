// Minimal MP4 writer for video messages: one H.264 track (Annex B in, avcC out) and one Opus track.
// Samples go straight to the file (mdat first); the index (moov) is written by Finish().
//
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace ls {

class Mp4Writer {
public:
    ~Mp4Writer();
    // rotation: clockwise degrees the picture must be turned to be upright (stored in the track matrix)
    bool Open(const std::string& path, int rotation, int opusPreSkip);
    // one encoded frame in Annex B format; the first one written must be a keyframe with SPS/PPS
    void AddVideo(const uint8_t* annexB, size_t n, bool key, int64_t ptsUs, int width, int height);
    // one 20 ms Opus packet (48 kHz)
    void AddAudio(const uint8_t* data, size_t n);
    // writes the index; returns the length in seconds (0 if nothing usable was recorded)
    double Finish();
    void Abort();  // closes and deletes the file
    bool HasVideo() const { return !video_.empty(); }

private:
    struct Sample {
        uint64_t offset;
        uint32_t size;
        int64_t time;  // video: microseconds; audio: unused (fixed 960 at 48 kHz)
        bool key;
    };
    std::mutex m_;
    FILE* f_ = nullptr;
    std::string path_;
    uint64_t pos_ = 0, mdatStart_ = 0;
    int rotation_ = 0, preSkip_ = 0, width_ = 0, height_ = 0;
    std::vector<uint8_t> sps_, pps_;
    std::vector<Sample> video_, audio_;
    void Write(const void* d, size_t n);
};

}  // namespace ls
