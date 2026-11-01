// LunaShare Voice - optional media module: Opus + SpeexDSP echo cancellation / noise suppression + jitter buffer.
//
// Pipeline (16 kHz mono, 20 ms frames):
//   mic -> echo canceller (reference = what we play) -> denoise/AGC -> Opus (in-band FEC) -> packet -> host
//   host -> packet -> jitter buffer -> Opus decode / FEC / loss concealment -> speaker (and AEC reference)
// Packet: [u8 version=1][u16 LE seq][u32 LE timestamp in samples][opus payload]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include <opus.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include "lunashare_media.h"

namespace {

constexpr int kRate = LSM_SAMPLE_RATE;
constexpr int kFrame = kRate / 50;          // 20 ms = 320 samples
constexpr int kEchoTail = kRate / 5;        // 200 ms echo tail
constexpr int kMaxPacket = 1500;

struct Voice {
    // ---- shared
    std::mutex m;  // everything below except where noted
    LsmHost host{};
    LsmSettings settings{1, 1, 1, 24000};
    bool running = false;
    std::atomic<bool> mute{false};

    // ---- send side (capture thread)
    OpusEncoder* enc = nullptr;
    SpeexEchoState* echo = nullptr;
    SpeexPreprocessState* pre = nullptr;
    std::vector<int16_t> capBuf;
    uint16_t seqOut = 0;
    uint32_t tsOut = 0;
    float micLevel = 0;
    int packetsOut = 0;
    int64_t bytesOut = 0;

    // ---- receive side
    OpusDecoder* dec = nullptr;
    std::map<uint16_t, std::vector<uint8_t>> jitter;  // seq -> packet (opus payload)
    bool playing = false;       // playout started
    uint16_t nextSeq = 0;
    int targetFrames = 3;       // playout delay target (adaptive, 60 ms start)
    std::deque<int16_t> out;    // decoded samples waiting for the speaker
    float spkLevel = 0;
    int packetsIn = 0;
    // loss/jitter statistics (rolling)
    int recvWindow = 0, lostWindow = 0;
    float lossPct = 0;
    double jitterMs = 0;
    int64_t lastArrivalUs = 0;
    uint32_t lastArrivalTs = 0;
    int underruns = 0;
};

Voice g;

int64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void Log(const char* msg) {
    if (g.host.log) g.host.log(g.host.user, msg);
}

float Level(const int16_t* p, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += (double)p[i] * p[i];
    double rms = std::sqrt(s / std::max(1, n)) / 32768.0;
    // map -60..0 dBFS to 0..1
    double db = rms > 1e-6 ? 20.0 * std::log10(rms) : -100.0;
    return (float)std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
}

void ApplySettingsLocked() {
    if (g.enc) opus_encoder_ctl(g.enc, OPUS_SET_BITRATE(std::clamp(g.settings.bitrate, 6000, 64000)));
    if (g.pre) {
        int on = g.settings.noise_suppress ? 1 : 0;
        speex_preprocess_ctl(g.pre, SPEEX_PREPROCESS_SET_DENOISE, &on);
        int suppress = -25;
        speex_preprocess_ctl(g.pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress);
        int agc = g.settings.auto_gain ? 1 : 0;
        speex_preprocess_ctl(g.pre, SPEEX_PREPROCESS_SET_AGC, &agc);
        float level = 12000;
        speex_preprocess_ctl(g.pre, SPEEX_PREPROCESS_SET_AGC_LEVEL, &level);
        speex_preprocess_ctl(g.pre, SPEEX_PREPROCESS_SET_ECHO_STATE, g.settings.echo_cancel ? g.echo : nullptr);
    }
}

void FreeLocked() {
    if (g.enc) opus_encoder_destroy(g.enc);
    if (g.dec) opus_decoder_destroy(g.dec);
    if (g.echo) speex_echo_state_destroy(g.echo);
    if (g.pre) speex_preprocess_state_destroy(g.pre);
    g.enc = nullptr;
    g.dec = nullptr;
    g.echo = nullptr;
    g.pre = nullptr;
}

int Start(const LsmHost* host, const LsmSettings* s) {
    std::lock_guard<std::mutex> l(g.m);
    FreeLocked();
    g.host = *host;
    if (s) g.settings = *s;
    int err = 0;
    g.enc = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) { Log("opus encoder failed"); FreeLocked(); return -1; }
    opus_encoder_ctl(g.enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(g.enc, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(g.enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(g.enc, OPUS_SET_COMPLEXITY(8));
    g.dec = opus_decoder_create(kRate, 1, &err);
    if (err != OPUS_OK) { Log("opus decoder failed"); FreeLocked(); return -1; }
    g.echo = speex_echo_state_init(kFrame, kEchoTail);
    int rate = kRate;
    speex_echo_ctl(g.echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    g.pre = speex_preprocess_state_init(kFrame, kRate);
    ApplySettingsLocked();
    g.capBuf.clear();
    g.seqOut = 0;
    g.tsOut = 0;
    g.jitter.clear();
    g.out.clear();
    g.playing = false;
    g.targetFrames = 3;
    g.packetsIn = g.packetsOut = 0;
    g.bytesOut = 0;
    g.recvWindow = g.lostWindow = 0;
    g.lossPct = 0;
    g.jitterMs = 0;
    g.lastArrivalUs = 0;
    g.underruns = 0;
    g.running = true;
    Log("voice started (Opus 16 kHz, speexdsp AEC/NS)");
    return 0;
}

void Stop() {
    std::lock_guard<std::mutex> l(g.m);
    g.running = false;
    FreeLocked();
    g.jitter.clear();
    g.out.clear();
}

// ---------------------------------------------------------------- send
void EncodeFrameLocked(int16_t* frame) {
    int16_t clean[kFrame];
    if (g.settings.echo_cancel && g.echo) {
        speex_echo_capture(g.echo, frame, clean);  // uses the playback history fed by speex_echo_playback()
    } else {
        memcpy(clean, frame, sizeof(clean));
    }
    if (g.pre) speex_preprocess_run(g.pre, clean);
    if (g.mute) memset(clean, 0, sizeof(clean));
    g.micLevel = g.micLevel * 0.7f + Level(clean, kFrame) * 0.3f;
    uint8_t pkt[kMaxPacket];
    pkt[0] = 1;
    pkt[1] = (uint8_t)g.seqOut;
    pkt[2] = (uint8_t)(g.seqOut >> 8);
    pkt[3] = (uint8_t)g.tsOut;
    pkt[4] = (uint8_t)(g.tsOut >> 8);
    pkt[5] = (uint8_t)(g.tsOut >> 16);
    pkt[6] = (uint8_t)(g.tsOut >> 24);
    int n = opus_encode(g.enc, clean, kFrame, pkt + 7, kMaxPacket - 7);
    g.seqOut++;
    g.tsOut += kFrame;
    if (n <= 0) return;
    g.packetsOut++;
    g.bytesOut += n + 7;
    if (g.host.send) g.host.send(g.host.user, pkt, (size_t)(n + 7));
}

void Capture(const int16_t* pcm, int samples) {
    std::lock_guard<std::mutex> l(g.m);
    if (!g.running) return;
    g.capBuf.insert(g.capBuf.end(), pcm, pcm + samples);
    size_t off = 0;
    while (g.capBuf.size() - off >= (size_t)kFrame) {
        EncodeFrameLocked(&g.capBuf[off]);
        off += kFrame;
    }
    g.capBuf.erase(g.capBuf.begin(), g.capBuf.begin() + (ptrdiff_t)off);
    if (g.capBuf.size() > (size_t)kRate) g.capBuf.clear();  // never accumulate more than a second
}

// ---------------------------------------------------------------- receive
void Receive(const uint8_t* data, size_t len) {
    if (len < 8 || data[0] != 1) return;
    uint16_t seq = (uint16_t)(data[1] | data[2] << 8);
    uint32_t ts = (uint32_t)data[3] | (uint32_t)data[4] << 8 | (uint32_t)data[5] << 16 | (uint32_t)data[6] << 24;
    std::lock_guard<std::mutex> l(g.m);
    if (!g.running) return;
    g.packetsIn++;
    // RFC 3550 style inter-arrival jitter
    int64_t now = NowUs();
    if (g.lastArrivalUs) {
        double d = (double)(now - g.lastArrivalUs) / 1000.0 - (double)(int32_t)(ts - g.lastArrivalTs) * 1000.0 / kRate;
        g.jitterMs += (std::fabs(d) - g.jitterMs) / 16.0;
    }
    g.lastArrivalUs = now;
    g.lastArrivalTs = ts;
    if (g.playing && (int16_t)(seq - g.nextSeq) < 0) return;  // too late, already concealed
    if (g.jitter.size() > 100) g.jitter.erase(g.jitter.begin());
    g.jitter[seq].assign(data + 7, data + len);
    if (!g.playing && (int)g.jitter.size() >= g.targetFrames) {
        g.playing = true;
        g.nextSeq = g.jitter.begin()->first;
    }
}

// Decodes the next 20 ms into g.out (lock held).
void DecodeNextLocked() {
    int16_t pcm[kFrame * 6];
    int n = 0;
    auto it = g.jitter.find(g.nextSeq);
    g.recvWindow++;
    if (it != g.jitter.end()) {
        n = opus_decode(g.dec, it->second.data(), (opus_int32)it->second.size(), pcm, kFrame * 6, 0);
        g.jitter.erase(it);
    } else {
        g.lostWindow++;
        auto nx = g.jitter.find((uint16_t)(g.nextSeq + 1));
        if (nx != g.jitter.end())  // recover the lost frame from the next packet's forward error correction
            n = opus_decode(g.dec, nx->second.data(), (opus_int32)nx->second.size(), pcm, kFrame, 1);
        else
            n = opus_decode(g.dec, nullptr, 0, pcm, kFrame, 0);  // packet loss concealment
    }
    g.nextSeq++;
    if (n > 0) g.out.insert(g.out.end(), pcm, pcm + n);
    if (g.recvWindow >= 100) {  // every ~2 s
        g.lossPct = 100.0f * (float)g.lostWindow / (float)g.recvWindow;
        g.recvWindow = g.lostWindow = 0;
        // adapt playout delay to jitter (20 ms frames), 40..300 ms
        int want = std::clamp((int)std::ceil((g.jitterMs * 2.0 + 20.0) / 20.0) + (g.lossPct > 5 ? 1 : 0), 2, 15);
        g.targetFrames = want;
    }
}

void Playback(int16_t* outBuf, int samples) {
    std::lock_guard<std::mutex> l(g.m);
    if (!g.running) {
        memset(outBuf, 0, (size_t)samples * sizeof(int16_t));
        return;
    }
    if (g.playing) {
        // keep latency bounded: if far too much is buffered (e.g. after a stall), skip ahead
        int buffered = (int)g.jitter.size();
        if (buffered > g.targetFrames + 8) {
            while ((int)g.jitter.size() > g.targetFrames) g.jitter.erase(g.jitter.begin());
            g.nextSeq = g.jitter.begin()->first;
        }
        while ((int)g.out.size() < samples) {
            if (g.jitter.empty()) {
                // nothing at all arrived: conceal a little, then fall silent and re-buffer
                if (++g.underruns > 10) {
                    g.playing = false;
                    g.underruns = 0;
                    break;
                }
            } else {
                g.underruns = 0;
            }
            DecodeNextLocked();
        }
    }
    int have = std::min<int>(samples, (int)g.out.size());
    for (int i = 0; i < have; i++) {
        outBuf[i] = g.out.front();
        g.out.pop_front();
    }
    if (have < samples) memset(outBuf + have, 0, (size_t)(samples - have) * sizeof(int16_t));
    g.spkLevel = g.spkLevel * 0.7f + Level(outBuf, samples) * 0.3f;
    // feed what goes to the speaker to the echo canceller, in exact 20 ms frames
    if (g.settings.echo_cancel && g.echo) {
        static std::vector<int16_t> ref;
        ref.insert(ref.end(), outBuf, outBuf + samples);
        size_t off = 0;
        while (ref.size() - off >= (size_t)kFrame) {
            speex_echo_playback(g.echo, &ref[off]);
            off += kFrame;
        }
        ref.erase(ref.begin(), ref.begin() + (ptrdiff_t)off);
    }
}

void SetMute(int m) { g.mute = m != 0; }

void SetSettings(const LsmSettings* s) {
    std::lock_guard<std::mutex> l(g.m);
    if (s) g.settings = *s;
    ApplySettingsLocked();
}

void GetStats(LsmStats* o) {
    std::lock_guard<std::mutex> l(g.m);
    memset(o, 0, sizeof(*o));
    o->mic_level = g.micLevel;
    o->speaker_level = g.spkLevel;
    o->loss_percent = g.lossPct;
    o->jitter_ms = (int)g.jitterMs;
    o->buffered_ms = (int)(g.jitter.size() * 20 + g.out.size() * 1000 / kRate);
    o->bitrate_kbps = g.settings.bitrate / 1000;
    o->packets_in = g.packetsIn;
    o->packets_out = g.packetsOut;
}

const LsmModule kModule = {
    LSM_API_VERSION, "LunaShare Voice", "1.0", "voice",
    Start, Stop, Capture, Playback, Receive, SetMute, SetSettings, GetStats,
};

}  // namespace

extern "C" LSM_EXPORT const LsmModule* lunashare_media_module(void) { return &kModule; }
