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
//
// Loss handling: a frame refused by the congested link or a gap in sequence numbers makes the decoder wait for the
// next keyframe (asked for at most twice a second). Bitrate adapts once a second from the host's media queue size.
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
    std::vector<uint8_t> data;  // Annex B
    uint32_t seq = 0;
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
        pkt[12] = 0;
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
    int64_t errWindowStart = 0, lastLimitAsk = 0;
    int errCount = 0;
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
            recvRot = p.rotation;
            recvW = p.width;
            recvH = p.height;
            framesIn++;
            bytesIn += (int)p.data.size() + (int)kHeader;
            if (ok) backend->Decode(p.data.data(), p.data.size(), p.key, p.width, p.height);
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
            if (!camOn || !settings.adaptive) { calmMs = 0; continue; }
            int cur = curBitrate, target = settings.bitrate;
            size_t budget = std::max<size_t>(16000, (size_t)cur / 8 * 3 / 10);  // ~300 ms of video
            if ((q > budget || skippedNow > 0) && now - lastAdjust >= 500) {
                // send a bit less than the link manages, so the queue drains
                int next = drainBps > 0 ? (int)(drainBps * 0.8) : cur / 2;
                next = std::clamp(next, minBitrate, cur * 9 / 10);
                curBitrate = next;
                lastAdjust = now;
                calmMs = 0;
                if (backend) backend->SetBitrate(next);
            } else if (q < budget / 4 && cur < target) {
                calmMs += 250;
                if (calmMs >= 1000) {  // probe upwards, a step a second
                    int next = std::min(target, std::max(cur + 50000, cur * 115 / 100));
                    curBitrate = next;
                    calmMs = 0;
                    lastAdjust = now;
                    if (backend) backend->SetBitrate(next);
                }
            } else {
                calmMs = 0;
            }
        }
    }
    static constexpr int minBitrate = 100000;
    std::atomic<int> sentTick{0};
    std::atomic<int> statsSkipped{0};

    // ---------------------------------------------------------------- API
    int Init(const LsvHost* h) {
        std::lock_guard<std::mutex> l(m);
        if (!h || h->api_version != LSV_API_VERSION) return 0;
        host = *h;
        hostSet = true;
        if (!backend) backend = CreateBackend(this, &host);
        return backend ? 1 : 0;
    }

    int Start(const LsvSettings* s) {
        std::lock_guard<std::mutex> l(m);
        if (!backend || running) return backend ? 1 : 0;
        if (s) settings = *s;
        curBitrate = settings.bitrate;
        seqOut = 0;
        needKey = false;
        haveSeq = false;
        waitingKey = true;
        statsSkipped = 0;
        queue.clear();
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
            p.data.assign(d + kHeader, d + n);
            std::lock_guard<std::mutex> l(qm);
            if (queue.size() > 20) {  // the decoder can't keep up: skip ahead to the next keyframe
                queue.clear();
                waitingKey = true;
            }
            queue.push_back(std::move(p));
            qcv.notify_one();
        } else if (d[1] == 2) {
            if (camOn && backend) backend->RequestKeyframe();
        } else if (d[1] == 4 && n >= 6) {
            limitW = (int)Get16(d + 2);  // applied by the timer thread (restarting the camera takes a moment)
            limitH = (int)Get16(d + 4);
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
        LsvSettings s = settings;
        s.bitrate = curBitrate;
        std::string err;
        if (!backend->StartCamera(id, s, &err)) {
            Log("cannot start camera: " + err);
            return false;
        }
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
