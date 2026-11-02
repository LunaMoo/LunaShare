// LunaShare video module - portable core.
//
//   camera -> platform H.264 encoder -> [packet] -> host -> network
//   network -> host -> [packet] -> decode thread -> platform H.264 decoder -> host (draws it)
//
// Packets (first byte 2 = video, so the host can tell them from voice):
//   [2][1][seq u32][flags u8: 1 = keyframe][rotation/90 u8][width u16][height u16][Annex B data]   one encoded frame
//   [2][2]                                                                                       please send a keyframe
//   [2][3][camera on u8]                                                                         camera state
//   [2][4][max width u16][max height u16]                                                         "too big for my decoder"
//   [2][5][decode codecs u8][encode codecs u8]                                                    what this side can do
//   [2][6][last seq u32][decode queue u8][received kbps u16]                                      receiver report (10/s)
// Byte 12 of a frame packet is the codec (0 H.264, 1 HEVC, 2 AV1). The sender picks the best codec both sides have.
//
// Loss handling: a frame refused by the congested link or a gap in sequence numbers makes the decoder wait for the
// next keyframe (asked for at most twice a second).
//
// Adaptation puts smoothness first: the bitrate follows what the link actually carries (checked four times a second,
// cut hard as soon as ~120 ms of video waits in the queue, raised slowly), and when the bitrate stays too low for the
// picture size, the camera steps down (first to 30 fps, then smaller sizes) instead of sending a blurry, stuttering
// picture; it steps back up after the link has kept up for a while.
//
// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "video_backend.h"

namespace lsv {

bool H264HasNal(const uint8_t* p, size_t n, int type) {
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && i + 4 < n && p[i + 3] == 1))) {
            size_t h = p[i + 2] == 1 ? i + 3 : i + 4;
            if (h < n && (p[h] & 0x1f) == type) return true;
            i = h;
        }
    }
    return false;
}

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void Put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
uint32_t Get16(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8; }
uint32_t Get32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

constexpr size_t kHeader = 13;

struct Packet {
    std::vector<uint8_t> data;  // Annex B (H.264/HEVC) or AV1 temporal unit
    uint32_t seq = 0;
    int codec = 0;
    bool key = false;
    int rotation = 0, width = 0, height = 0;
};

class Core : public Sink {
public:
    std::mutex m;
    LsvHost host{};
    bool hostSet = false;
    std::unique_ptr<Backend> backend;
    LsvSettings settings{640, 480, 24, 800000, 1};
    std::string cameraId;
    std::atomic<bool> running{false}, camOn{false};

    // sending
    std::atomic<uint32_t> seqOut{0};
    std::atomic<bool> needKey{false};
    std::atomic<int> curBitrate{800000};
    int calm = 0;
    int64_t lastKeyFromEncoderAsk = 0;
    std::atomic<int> framesOut{0}, bytesOut{0}, skipped{0}, sendW{0}, sendH{0};
    int64_t lastLocalImage = 0;

    // receiving
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<Packet> queue;
    uint32_t lastSeqIn = 0;
    bool haveSeq = false, waitingKey = true;
    std::atomic<int> recvRot{0}, recvW{0}, recvH{0};
    std::atomic<int> framesIn{0}, bytesIn{0};
    int64_t lastKeyAsk = 0;

    // per-second stats
    std::atomic<int> sFpsOut{0}, sKbpsOut{0}, sFpsIn{0}, sKbpsIn{0};
    std::thread decodeThread, timerThread;

    void Log(const std::string& msg) override {
        if (hostSet && host.log) host.log(host.user, ("video: " + msg).c_str());
    }

    bool Send(const uint8_t* d, size_t n) { return hostSet && host.send && host.send(host.user, d, n) != 0; }

    void SendState() {
        uint8_t p[3] = {2, 3, (uint8_t)(camOn ? 1 : 0)};
        Send(p, 3);
        uint8_t c[4] = {2, 5, (uint8_t)myDec, (uint8_t)myEnc};
        Send(c, 4);
    }

    // codec negotiation
    unsigned myEnc = 1, myDec = 1;
    std::atomic<unsigned> peerDec{0};  // 0 = not heard yet (assume H.264 only)
    std::atomic<int> curCodec{0};
    std::atomic<bool> codecRecheck{false};
    int ChooseCodec() {
        unsigned m = myEnc & (peerDec ? (unsigned)peerDec : CodecBit(kH264));
        if (settings.codec_pref == 1) m &= CodecBit(kH264);
        if (m & CodecBit(kAV1)) return kAV1;
        if (m & CodecBit(kHEVC)) return kHEVC;
        return kH264;
    }

    void AskKeyframe() {
        int64_t now = NowMs();
        if (now - lastKeyAsk < 500) return;
        lastKeyAsk = now;
        uint8_t p[2] = {2, 2};
        Send(p, 2);
    }

    // ---------------------------------------------------------------- Sink
    void OnEncoded(const EncodedFrame& f) override {
        if (!camOn || !running) return;
        if (needKey && !f.key) {
            skipped++;
            int64_t now = NowMs();
            if (now - lastKeyFromEncoderAsk > 300) {
                lastKeyFromEncoderAsk = now;
                if (backend) backend->RequestKeyframe();
            }
            return;
        }
        std::vector<uint8_t> pkt(kHeader + f.len);
        uint32_t seq = seqOut++;
        pkt[0] = 2;
        pkt[1] = 1;
        Put32(&pkt[2], seq);
        pkt[6] = f.key ? 1 : 0;
        pkt[7] = (uint8_t)(((f.rotation % 360 + 360) % 360) / 90);
        Put16(&pkt[8], (uint32_t)f.width);
        Put16(&pkt[10], (uint32_t)f.height);
        pkt[12] = (uint8_t)f.codec;
        memcpy(&pkt[kHeader], f.data, f.len);
        if (!Send(pkt.data(), pkt.size())) {
            // the link is backed up: everything until the next keyframe would be undecodable, so skip to it
            skipped++;
            needKey = true;
            seqOut = seq;  // no gap for the receiver; it simply waits a little longer
            if (backend) backend->RequestKeyframe();
            return;
        }
        needKey = false;
        sentAt[seq & (kSentRing - 1)] = NowMs();
        framesOut++;
        bytesOut += (int)pkt.size();
        sentTick += (int)pkt.size();
        sendW = f.width;
        sendH = f.height;
    }

    void OnLocalImage(const LsvImage& img) override {
        int64_t now = NowMs();
        if (now - lastLocalImage < 40) return;  // the preview doesn't need more than ~25 fps
        lastLocalImage = now;
        if (hostSet && host.image) host.image(host.user, LSV_LOCAL, &img);
    }

    void OnDecoded(const LsvImage& img) override {
        LsvImage i = img;
        i.rotation = recvRot;
        i.mirror = 0;
        if (hostSet && host.image) host.image(host.user, LSV_REMOTE, &i);
    }

    void OnDecoderError() override {
        {
            std::lock_guard<std::mutex> l(qm);
            waitingKey = true;
        }
        AskKeyframe();
        // a decoder that keeps failing on a big picture probably can't do that size: ask the sender for less
        int64_t now = NowMs();
        int w = recvW, h = recvH;
        if (now - errWindowStart > 4000) {
            errWindowStart = now;
            errCount = 0;
        }
        if (++errCount >= 4 && (int64_t)w * h > 1920 * 1088 && now - lastLimitAsk > 5000) {
            lastLimitAsk = now;
            errCount = 0;
            int mw = (int64_t)w * h > 3840 * 2176 ? 3840 : 1920, mh = mw == 3840 ? 2160 : 1080;
            if (w < h) std::swap(mw, mh);
            uint8_t p[6] = {2, 4};
            Put16(p + 2, (uint32_t)mw);
            Put16(p + 4, (uint32_t)mh);
            Send(p, 6);
            Log("decoder can't keep up with " + std::to_string(w) + "x" + std::to_string(h) + ", asked for at most " +
                std::to_string(mw) + "x" + std::to_string(mh));
        }
    }
    int64_t errWindowStart = 0, lastLimitAsk = 0, overflowWindow = 0;
    int errCount = 0, overflows = 0;
    std::atomic<bool> pendingTooSlow{false};
    std::atomic<int> limitW{0}, limitH{0};

    void OnCameraError(const std::string& msg) override {
        Log("camera: " + msg);
        camOn = false;
        SendState();
    }

    // ---------------------------------------------------------------- threads
    void DecodeLoop() {
        bool ok = backend && backend->StartDecoder();
        if (!ok) Log("no H.264 decoder");
        while (running) {
            Packet p;
            {
                std::unique_lock<std::mutex> l(qm);
                qcv.wait_for(l, std::chrono::milliseconds(200), [&] { return !queue.empty() || !running; });
                if (!running) break;
                if (queue.empty()) continue;
                p = std::move(queue.front());
                queue.pop_front();
                bool gap = haveSeq && p.seq != lastSeqIn + 1;
                haveSeq = true;
                lastSeqIn = p.seq;
                if (gap && !p.key) waitingKey = true;
                if (p.key) waitingKey = false;
                if (waitingKey) {
                    l.unlock();
                    AskKeyframe();
                    continue;
                }
            }
            if (pendingTooSlow.exchange(false)) OnDecoderError();
            recvRot = p.rotation;
            recvW = p.width;
            recvH = p.height;
            framesIn++;
            bytesIn += (int)p.data.size() + (int)kHeader;
            if (ok) backend->Decode(p.data.data(), p.data.size(), p.key, p.width, p.height, p.codec);
        }
        if (backend) backend->StopDecoder();
    }

    void TimerLoop() {
        int64_t lastState = 0, lastAdjust = 0, lastStats = NowMs(), prevTick = NowMs();
        size_t prevQ = 0;
        int calmMs = 0;
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (!running) break;
            int64_t now = NowMs();
            if (now - lastStats >= 1000) {
                double sec = (now - lastStats) / 1000.0;
                lastStats = now;
                sFpsOut = (int)(framesOut.exchange(0) / sec + 0.5);
                sKbpsOut = (int)(bytesOut.exchange(0) * 8 / 1000 / sec);
                sFpsIn = (int)(framesIn.exchange(0) / sec + 0.5);
                sKbpsIn = (int)(bytesIn.exchange(0) * 8 / 1000 / sec);
            }
            if (codecRecheck.exchange(false) && camOn && ChooseCodec() != curCodec) {
                std::lock_guard<std::mutex> l(m);
                if (camOn) {
                    Log(std::string("switching to ") + CodecName(ChooseCodec()));
                    backend->StopCamera();
                    camOn = StartCameraLocked();
                    if (!camOn) SendState();
                }
            }
            if (int lw = limitW.exchange(0)) {
                int lh = limitH.exchange(0);
                // the receiver can't decode what we send: fit our capture size into its limit (orientation-free)
                int big = std::max(lw, lh), small = std::min(lw, lh);
                LsvSettings s = settings;
                if (s.width > big || s.height > small) {
                    double f = std::min((double)big / s.width, (double)small / s.height);
                    s.width = (int)(s.width * f) & ~1;
                    s.height = (int)(s.height * f) & ~1;
                    s.bitrate = (int)(s.bitrate * f * f);
                    Log("peer asked for at most " + std::to_string(big) + "x" + std::to_string(small));
                    SetSettings(&s);
                }
            }
            if (now - lastState > 2000) {  // repeat the camera state now and then, in case a packet got lost
                lastState = now;
                SendState();
            }
            // ---- bitrate adaptation from the host's media queue
            size_t q = host.queued_bytes ? host.queued_bytes(host.user) : 0;
            int sent = sentTick.exchange(0);
            double dt = std::max(0.05, (now - prevTick) / 1000.0);
            prevTick = now;
            double drainBps = ((double)prevQ + sent - (double)q) * 8 / dt;  // what the link actually carried
            prevQ = q;
            int skippedNow = skipped.exchange(0);
            statsSkipped += skippedNow;
            if (!camOn || !settings.adaptive) { calmMs = 0; lowMs = 0; fullMs = 0; continue; }
            LsvSettings run = LevelSettings(level);
            int cur = curBitrate, target = run.bitrate;
            size_t budget = std::max<size_t>(12000, (size_t)cur / 8 * 12 / 100);  // ~120 ms of video
            bool fresh = gotReport && now - lastReportIn < 1500;
            bool stalled = gotReport && !fresh && framesOut.load() > 0;    // reports stopped: the path is jammed
            bool delayed = fresh && queueDelay > 150;                       // video waits somewhere on the way
            bool slowDecoder = fresh && peerQueue >= 3;                     // their device can't keep up
            bool congested = q > budget || skippedNow > 0 || delayed || stalled;
            devMs = slowDecoder ? devMs + 250 : 0;
            if (congested && now - lastAdjust >= 250) {
                // well under what the link manages, so the queue empties quickly (smooth beats sharp)
                int next = drainBps > 0 ? (int)(drainBps * 0.7) : cur / 2;
                if (delayed || stalled) {  // our queue looks fine, the delay is further down the path
                    int byPeer = peerKbps > 0 ? peerKbps * 1000 * 8 / 10 : cur / 2;
                    next = std::min(cur * 7 / 10, std::max(minBitrate, byPeer));
                }
                next = std::clamp(next, minBitrate, cur * 3 / 4);
                curBitrate = next;
                lastAdjust = now;
                calmMs = 0;
                upHoldUntil = now + 4000;  // don't probe upwards right after congestion
                if (backend) backend->SetBitrate(next);
            } else if (!congested && q < budget / 4 && cur < target && now >= upHoldUntil) {
                calmMs += 250;
                if (calmMs >= 1500) {  // probe upwards slowly
                    int next = std::min(target, std::max(cur + 40000, cur * 108 / 100));
                    curBitrate = next;
                    calmMs = 0;
                    lastAdjust = now;
                    if (backend) backend->SetBitrate(next);
                }
            } else if (congested) {
                calmMs = 0;
            }
            // ---- picture size / frame rate: what fits the bitrate the link allows
            double bpp = (double)curBitrate / std::max(1.0, (double)run.width * run.height * run.fps);
            lowMs = bpp < 0.035 ? lowMs + 250 : 0;                                   // too few bits: blocky + stutter
            fullMs = !congested && !slowDecoder && curBitrate >= target * 95 / 100 ? fullMs + 250 : 0;  // link keeps up at full rate
            int want = level;
            bool canDown = level < kMaxLevel && !SameShape(LevelSettings(level + 1), run);
            if ((lowMs >= 2000 || devMs >= 1000) && canDown) want = level + 1;  // too few bits, or their decoder is behind
            else if (fullMs >= 12000 && level > 0 && now - lastLevelChange > 20000) want = level - 1;
            if (want != level && now - lastLevelChange > 3000) {
                std::lock_guard<std::mutex> l(m);
                if (camOn) {
                    bool up = want < level;
                    level = want;
                    LsvSettings ns = LevelSettings(want);
                    Log(std::string("quality ") + (up ? "up" : "down") + " to " + std::to_string(ns.width) + "x" + std::to_string(ns.height) +
                        " @" + std::to_string(ns.fps) + " fps");
                    if (up) curBitrate = std::min((int)curBitrate, ns.bitrate * 3 / 4);  // up: start gently
                    else curBitrate = std::min((int)curBitrate, ns.bitrate);
                    backend->StopCamera();
                    camOn = StartCameraLocked();
                    if (!camOn) SendState();
                }
                lastLevelChange = now;
                lowMs = fullMs = 0;
            }
        }
    }
    static constexpr int minBitrate = 100000;
    // receiver reports: delay that builds up anywhere on the way (also in buffers the host can't see) and a
    // decoder that can't keep up
    static constexpr uint32_t kSentRing = 1024;
    int64_t sentAt[kSentRing] = {};
    std::mutex rm;
    int64_t lastReportOut = 0, lastReportIn = 0, minRotate = 0;
    int minCur = 1 << 30, minPrev = 1 << 30;
    std::atomic<int> queueDelay{0}, peerQueue{0}, peerKbps{0};
    std::atomic<bool> gotReport{false};
    int devMs = 0;
    int lowMs = 0, fullMs = 0;
    int64_t upHoldUntil = 0, lastLevelChange = 0;

    // ---- quality ladder (level 0 = what the user picked)
    std::atomic<int> level{0};
    static constexpr int kMaxLevel = 6;
    LsvSettings LevelSettings(int lv) const {
        LsvSettings s = settings;
        if (lv <= 0) return s;
        int fps = s.fps;
        bool portrait = s.height > s.width;
        int longSide = std::max(s.width, s.height), shortSide = std::min(s.width, s.height);
        int h = shortSide;
        static const int kHeights[] = {2160, 1440, 1080, 720, 540, 360};
        for (int i = 0; i < lv; i++) {
            if (fps > 30) { fps = 30; continue; }  // first: fewer frames, same size
            int next = 0;
            for (int k : kHeights)
                if (k < h) { next = k; break; }
            if (next) { h = next; continue; }       // then the next smaller standard size, down to 360p
            if (fps > 15) fps = fps > 20 ? 20 : 15; // then fewer frames again
        }
        int w = (int)((double)longSide * h / shortSide + 0.5) & ~1;
        s.width = portrait ? h : w;
        s.height = portrait ? w : h;
        double ratio = (double)s.width * s.height * fps / ((double)settings.width * settings.height * std::max(1, settings.fps));
        s.bitrate = std::max(minBitrate * 3, (int)(settings.bitrate * std::min(1.0, ratio * 1.15)));
        s.fps = fps;
        return s;
    }
    bool SameShape(const LsvSettings& a, const LsvSettings& b) const { return a.width == b.width && a.height == b.height && a.fps == b.fps; }
    std::atomic<int> sentTick{0};
    std::atomic<int> statsSkipped{0};

    // ---------------------------------------------------------------- API
    int Init(const LsvHost* h) {
        std::lock_guard<std::mutex> l(m);
        if (!h || h->api_version != LSV_API_VERSION) return 0;
        host = *h;
        hostSet = true;
        if (!backend) backend = CreateBackend(this, &host);
        if (backend) {
            myEnc = backend->EncodeCodecs() | CodecBit(kH264);
            myDec = backend->DecodeCodecs() | CodecBit(kH264);
        }
        return backend ? 1 : 0;
    }

    int Start(const LsvSettings* s) {
        std::lock_guard<std::mutex> l(m);
        if (!backend || running) return backend ? 1 : 0;
        if (s) settings = *s;
        curBitrate = settings.bitrate;
        level = 0;
        seqOut = 0;
        gotReport = false;
        queueDelay = 0;
        peerQueue = 0;
        minCur = minPrev = 1 << 30;
        memset(sentAt, 0, sizeof(sentAt));
        needKey = false;
        haveSeq = false;
        waitingKey = true;
        statsSkipped = 0;
        queue.clear();
        peerDec = 0;
        running = true;
        decodeThread = std::thread([this] { DecodeLoop(); });
        timerThread = std::thread([this] { TimerLoop(); });
        return 1;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> l(m);
            if (!running) return;
            if (camOn) {
                camOn = false;
                backend->StopCamera();
            }
            running = false;
        }
        // join without holding m: the timer thread may be waiting for it
        qcv.notify_all();
        if (decodeThread.joinable()) decodeThread.join();
        if (timerThread.joinable()) timerThread.join();
        std::lock_guard<std::mutex> l(qm);
        queue.clear();
    }

    void Receive(const uint8_t* d, size_t n) {
        if (!running || n < 2 || d[0] != 2) return;
        if (d[1] == 1 && n > kHeader) {
            Packet p;
            p.seq = Get32(d + 2);
            p.key = d[6] & 1;
            p.rotation = (d[7] & 3) * 90;
            p.width = (int)Get16(d + 8);
            p.height = (int)Get16(d + 10);
            p.codec = d[12] <= kAV1 ? d[12] : 0;
            p.data.assign(d + kHeader, d + n);
            uint32_t seqIn = p.seq;
            std::unique_lock<std::mutex> l(qm);
            if (queue.size() > 20) {  // the decoder can't keep up: skip ahead to the next keyframe
                queue.clear();
                waitingKey = true;
                // and if that keeps happening with a big picture, ask for a smaller one
                int64_t now = NowMs();
                if (now - overflowWindow > 5000) overflowWindow = now, overflows = 0;
                if (++overflows >= 3) {
                    overflows = 0;
                    errCount = 3;  // OnDecoderError() then asks for a lower size
                    errWindowStart = now;
                    recvW = p.width;
                    recvH = p.height;
                    pendingTooSlow = true;
                }
            }
            queue.push_back(std::move(p));
            qcv.notify_one();
            // tell the sender how far we got and how busy the decoder is (it measures the delay from that)
            size_t qn = queue.size();
            l.unlock();
            int64_t now = NowMs();
            if (now - lastReportOut >= 100) {
                lastReportOut = now;
                uint8_t r[9] = {2, 6};
                Put32(r + 2, seqIn);
                r[6] = (uint8_t)std::min<size_t>(qn, 255);
                Put16(r + 7, (uint32_t)std::min(65535, (int)sKbpsIn));
                Send(r, sizeof(r));
            }
        } else if (d[1] == 2) {
            if (camOn && backend) backend->RequestKeyframe();
        } else if (d[1] == 4 && n >= 6) {
            limitW = (int)Get16(d + 2);  // applied by the timer thread (restarting the camera takes a moment)
            limitH = (int)Get16(d + 4);
        } else if (d[1] == 5 && n >= 4) {
            unsigned dec = d[2];
            if (dec != peerDec.exchange(dec)) codecRecheck = true;
        } else if (d[1] == 6 && n >= 9) {
            uint32_t seq = Get32(d + 2);
            uint32_t out = seqOut;
            if (out - seq <= kSentRing - 1 && out != seq) {
                int64_t now = NowMs();
                int64_t sent = sentAt[seq & (kSentRing - 1)];
                int dly = sent ? (int)(now - sent) : 0;  // one way + the report's way back
                std::lock_guard<std::mutex> l(rm);
                if (now - minRotate > 5000) {  // base delay: lowest of the last 5-10 s
                    minRotate = now;
                    minPrev = minCur;
                    minCur = dly;
                }
                minCur = std::min(minCur, dly);
                int base = std::min(minPrev, minCur);
                queueDelay = std::max(0, dly - base);
                peerQueue = d[6];
                peerKbps = (int)Get16(d + 7);
                lastReportIn = now;
                gotReport = true;
            }
        } else if (d[1] == 3 && n >= 3) {
            if (host.remote_state) host.remote_state(host.user, d[2] ? 1 : 0);
        }
    }

    std::string PickCamera() {
        auto cams = backend->Cameras();
        if (cams.empty()) return "";
        for (auto& c : cams)
            if (c.id == cameraId) return c.id;
        for (auto& c : cams)
            if (c.facing == LSV_FACING_FRONT) return c.id;  // calls start with the selfie camera
        return cams[0].id;
    }

    bool StartCameraLocked() {
        std::string id = PickCamera();
        if (id.empty()) {
            Log("no camera found");
            return false;
        }
        cameraId = id;
        LsvSettings s = LevelSettings(level);
        s.bitrate = std::min((int)curBitrate, s.bitrate);
        std::string err;
        int codec = ChooseCodec();
        if (!backend->StartCamera(id, s, codec, &err)) {
            if (codec == kH264 || !backend->StartCamera(id, s, kH264, &err)) {
                Log("cannot start camera: " + err);
                return false;
            }
            Log(std::string(CodecName(codec)) + " encoder failed (" + err + "), using H.264");
            codec = kH264;
        }
        curCodec = codec;
        needKey = false;
        backend->RequestKeyframe();
        return true;
    }

    int SetCameraOn(int on) {
        std::lock_guard<std::mutex> l(m);
        if (!backend) return 0;
        if (on && !camOn) {
            if (!StartCameraLocked()) return 0;
            camOn = true;
        } else if (!on && camOn) {
            camOn = false;
            backend->StopCamera();
        }
        SendState();
        return 1;
    }

    int SelectCamera(const char* id) {
        std::lock_guard<std::mutex> l(m);
        cameraId = id ? id : "";
        if (!camOn) return 1;
        camOn = false;
        backend->StopCamera();
        bool ok = StartCameraLocked();
        camOn = ok;
        if (!ok) SendState();
        return ok ? 1 : 0;
    }

    void SetSettings(const LsvSettings* s) {
        if (!s) return;
        std::lock_guard<std::mutex> l(m);
        bool sizeChanged = s->width != settings.width || s->height != settings.height || s->fps != settings.fps;
        settings = *s;
        if (sizeChanged) level = 0;  // the user picked something new: start from it
        curBitrate = std::min((int)curBitrate, s->bitrate);
        if (!s->adaptive) curBitrate = s->bitrate;
        if (!camOn || !backend) return;
        if (sizeChanged) {
            camOn = false;
            backend->StopCamera();
            camOn = StartCameraLocked();
            if (!camOn) SendState();
        } else {
            backend->SetBitrate(curBitrate);
        }
    }
};

Core& G() {
    static Core c;
    return c;
}

int ApiInit(const LsvHost* h) { return G().Init(h); }
int ApiStart(const LsvSettings* s) { return G().Start(s); }
void ApiStop() { G().Stop(); }
void ApiReceive(const uint8_t* d, size_t n) { G().Receive(d, n); }
int ApiCameraOn(int on) { return G().SetCameraOn(on); }
int ApiListCameras(LsvCamera* out, int max) {
    Core& c = G();
    std::lock_guard<std::mutex> l(c.m);
    if (!c.backend) return 0;
    auto cams = c.backend->Cameras();
    int n = 0;
    for (auto& cam : cams)
        if (n < max) out[n++] = cam;
    return n;
}
int ApiSelectCamera(const char* id) { return G().SelectCamera(id); }
void ApiSetSettings(const LsvSettings* s) { G().SetSettings(s); }
int ApiGetControls(LsvControl* out, int max) {
    Core& c = G();
    std::lock_guard<std::mutex> l(c.m);
    if (!c.backend || !c.camOn) return 0;
    auto v = c.backend->Controls();
    int n = 0;
    for (auto& x : v)
        if (n < max) out[n++] = x;
    return n;
}
void ApiSetControl(int id, float value) {
    Core& c = G();
    std::lock_guard<std::mutex> l(c.m);
    if (c.backend && c.camOn) c.backend->SetControl(id, value);
}
void ApiStats(LsvStats* o) {
    Core& c = G();
    memset(o, 0, sizeof(*o));
    o->send_width = c.camOn ? c.sendW.load() : 0;
    o->send_height = c.camOn ? c.sendH.load() : 0;
    o->send_fps = c.sFpsOut;
    o->send_kbps = c.sKbpsOut;
    o->recv_width = c.recvW;
    o->recv_height = c.recvH;
    o->recv_fps = c.sFpsIn;
    o->recv_kbps = c.sKbpsIn;
    o->frames_skipped = c.statsSkipped;
    std::lock_guard<std::mutex> l(c.m);
    if (c.backend) {
        snprintf(o->encoder, sizeof(o->encoder), "%s", c.backend->EncoderName().c_str());
        snprintf(o->decoder, sizeof(o->decoder), "%s", c.backend->DecoderName().c_str());
    }
}

int ApiVcamAvailable() {
    Core& c = G();
    std::lock_guard<std::mutex> l(c.m);
    return c.backend && c.backend->VcamAvailable() ? 1 : 0;
}
int ApiVcamEnable(int on, char* msg, int len) {
    Core& c = G();  // no core lock: this can wait for the user's admin prompt
    std::string err;
    bool ok = c.backend && c.backend->VcamEnable(on != 0, &err);
    if (msg && len > 0) snprintf(msg, (size_t)len, "%s", err.c_str());
    return ok ? 1 : 0;
}
void ApiVcamPush(const LsvImage* img) {
    Core& c = G();
    if (c.backend && img) c.backend->VcamPush(*img);
}

const LsvModule kModule = {
    LSV_API_VERSION, "LunaShare video", "1.1",
    ApiInit, ApiStart, ApiStop, ApiReceive, ApiCameraOn, ApiListCameras, ApiSelectCamera,
    ApiSetSettings, ApiGetControls, ApiSetControl, ApiStats,
    ApiVcamAvailable, ApiVcamEnable, ApiVcamPush,
};

}  // namespace
}  // namespace lsv

extern "C" LSV_EXPORT const LsvModule* lunashare_video_module(void) { return &lsv::kModule; }
