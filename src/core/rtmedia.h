// Real-time call media: voice and video as their own UDP datagrams instead of the connection's ordered stream.
//
// On the ordered stream (TCP on the LAN, KCP over the internet) one lost packet holds up everything behind it -
// voice included - until it is sent again. Here a lost voice packet is simply concealed (the voice module's jitter
// buffer, Opus FEC), and a lost piece of a video frame is asked for again only while it can still arrive in time;
// a frame that can't be completed is skipped (the video module then asks for a keyframe).
//
// Datagram: [u32 tag][XChaCha20-Poly1305 sealed with the call's key: 24 nonce, 16 MAC, plain]
// plain:    [u8 kind][u32 pseq][u32 send time, ms][body]
//   1 voice       body = the voice module's packet
//   2 video piece body = [u32 frame][u16 index][u16 count][bytes]
//   3 video small body = the video module's packet (camera state, keyframe requests ...)
//   4 feedback    body = [u32 highest pseq][u16 got][u16 expected][u16 queue delay ms][u16 kbit/s][u8 n][n x u32 missing]
//   5 probe       body = [u8 1: I hear you]
//   6 ping / 7 pong  body = [u32 time]
//   8 resent piece body = [u32 original pseq] + as 2
// pseq counts every datagram one way (losses and delays are measured with it); the tag says which call it is.
//
// Bitrate: the receiver reports how long datagrams queue somewhere on the way (delay above the lowest seen) and how
// many got lost; the sender keeps an estimate of what the path carries - down quickly when a queue builds or loss
// climbs, up slowly when it stays clear - and paces video out at that rate so a big keyframe doesn't hit the
// network as one burst. Calls start low and climb.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace ls {

struct RtStats {
    bool ready = false;          // media goes this way now (else over the connection)
    int lossInPct10 = 0;         // of what arrives here, lost (tenths of a percent, last few seconds)
    int lossOutPct10 = 0;        // of what I send, lost on the way (the other side's reports)
    int lateFrames = 0;          // video frames that couldn't be completed in time (skipped)
    int resent = 0;              // video pieces sent again
    int rttMs = 0;
    int estimateKbps = 0;        // what the path is thought to carry
    int queueDelayMs = 0;        // how long my datagrams wait somewhere on the way (the other side's reports)
};

class RtMedia {
public:
    using Out = std::function<bool(const uint8_t* data, size_t len)>;  // one datagram onto the network (any thread)
    using In = std::function<void(const uint8_t* data, size_t len)>;    // a media packet for the voice / video module

    static constexpr int kStartBps = 300000;  // start low: a bad connection isn't flooded in its first seconds
    static constexpr int kMinBps = 60000;

    RtMedia(const uint8_t key[32], uint64_t callId, Out out, In in);
    ~RtMedia();

    static uint32_t TagFor(uint64_t callId) { return (uint32_t)(callId ^ (callId >> 32)) | 1u; }
    uint32_t Tag() const { return tag_; }

    // A media packet from the modules (voice: first byte 1, video: 2). False: not taken (the path isn't confirmed,
    // or it's congested - the video module then skips to its next keyframe).
    bool Send(const uint8_t* d, size_t n);
    // A datagram from the network (tag included).
    void Input(const uint8_t* d, size_t n);
    // The path works both ways and recently did: send media here.
    bool Ready() const { return ready_.load(); }
    int LinkBps() const { return estimate_.load(); }  // what video and voice together may use
    size_t Queued();                                   // bytes waiting to be paced out
    RtStats Stats();

    // tests: drop this share of incoming datagrams (0..100)
    std::atomic<int> testDropPct{0};

private:
    struct Piece {  // a sent video piece, kept a moment for resending (found by its pseq)
        uint32_t pseq = 0, frame = 0;
        uint16_t idx = 0, count = 0;
        int64_t sentAt = 0, resentAt = 0;
        std::string bytes;
    };
    struct Outgoing {  // a video piece waiting to be paced out
        uint8_t kind = 2;    // 2, or 8 when sent again
        uint32_t orig = 0;   // (8: the pseq it first went out with)
        uint32_t frame = 0;
        uint16_t idx = 0, count = 0;
        std::string bytes;
        int64_t queuedAt = 0;
    };
    struct Frame {  // a video frame being put together
        uint16_t count = 0, got = 0;
        int64_t firstAt = 0;
        std::vector<std::string> parts;
    };
    struct Missing {
        int64_t since = 0, lastAsk = 0;
        int asks = 0;
    };

    void Loop();
    std::string Seal(uint8_t kind, const std::string& body, uint32_t* pseqOut = nullptr);  // m_ held (all below too)
    void Emit(const std::string& datagram);
    void SendNow(uint8_t kind, const std::string& body);
    void Pace(int64_t now);
    void TakePiece(uint32_t frame, uint16_t idx, uint16_t count, const uint8_t* p, size_t n, int64_t now);
    void FlushFrames(int64_t now, std::vector<std::string>& deliver);
    void SendFeedback(int64_t now);
    void OnFeedback(const uint8_t* b, size_t n, int64_t now);
    int FrameDeadline() const;
    void FlushOut();  // m_ NOT held: puts what Emit() collected onto the network

    uint8_t key_[32];
    uint32_t tag_;
    Out out_;
    In in_;

    std::mutex m_, outM_;
    std::condition_variable cv_;
    std::atomic<bool> run_{true}, ready_{false};
    std::thread thread_;
    std::vector<std::string> pendingOut_;

    // sending
    uint32_t pseqOut_ = 1, frameOut_ = 1;
    static constexpr uint32_t kRing = 4096;
    std::vector<Piece> ring_;
    std::deque<Outgoing> paced_;
    size_t pacedBytes_ = 0;
    double budget_ = 0;
    int64_t lastPace_ = 0;
    std::atomic<int> estimate_{kStartBps};
    int64_t lastDecrease_ = 0, holdUntil_ = 0, lastIncrease_ = 0;
    int sentBytesWindow_ = 0;
    int64_t sentWindowAt_ = 0;
    int sentBps_ = 0;
    int resent_ = 0;
    int peerQueueMs_ = 0, lossOutPct10_ = 0;

    // path
    bool heardPeer_ = false;   // something valid arrived from them
    int64_t lastConfirm_ = 0;  // they told me they hear me (probe or feedback)
    int64_t lastProbe_ = 0, lastPing_ = 0, lastFeedback_ = 0, lastHeard_ = 0;
    int rtt_ = 0;

    // receiving
    std::unordered_set<uint32_t> seen_;  // (duplicates and replays)
    std::deque<uint32_t> seenOrder_;
    bool haveHigh_ = false, wantFeedback_ = false;
    uint32_t highIn_ = 0, windowStartHigh_ = 0;
    std::map<uint32_t, Missing> missing_;
    int gotWindow_ = 0, bytesWindow_ = 0;
    int64_t qdelaySum_ = 0;
    int qdelayCount_ = 0;
    int32_t baseCur_ = INT32_MAX, basePrev_ = INT32_MAX;
    int64_t baseRotate_ = 0;
    int lossInPct10_ = 0;
    int lostCur_ = 0, expCur_ = 0, lostPrev_ = 0, expPrev_ = 0;
    int64_t lossRotate_ = 0;
    int rateBytes_ = 0, rateKbps_ = 0;
    int64_t rateSince_ = 0;
    int fbGot_ = 0, fbExp_ = 0;  // their reports, added up until there's enough to judge the loss
    int64_t fbSince_ = 0;
    std::map<uint32_t, Frame> frames_;
    bool haveNextFrame_ = false;
    uint32_t nextFrame_ = 0;
    int64_t blockedSince_ = 0;
    int late_ = 0;
    uint32_t dropRng_ = 12345;
};

}  // namespace ls
