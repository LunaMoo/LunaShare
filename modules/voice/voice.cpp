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
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <map>
#include <mutex>
#include <vector>

#include <opus.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include "lunashare_media.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static std::wstring Wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}
static FILE* OpenUtf8(const char* p) { return _wfopen(Wide(p).c_str(), L"wb"); }
static void RemoveUtf8(const char* p) { _wremove(Wide(p).c_str()); }
#else
static FILE* OpenUtf8(const char* p) { return fopen(p, "wb"); }
static void RemoveUtf8(const char* p) { remove(p); }
#endif

namespace {

constexpr int kRate = LSM_SAMPLE_RATE;
constexpr int kFrame = kRate / 50;          // 20 ms = 960 samples
constexpr int kOpusRate = 48000;             // Opus timestamps / pre-skip are always in 48 kHz units
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
    uint16_t lastArrivalSeq = 0;
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
    if (g.enc) opus_encoder_ctl(g.enc, OPUS_SET_BITRATE(std::clamp(g.settings.bitrate, 6000, 128000)));
    if (g.pre) {
        int on = g.settings.noise_suppress ? 1 : 0;
        speex_preprocess_ctl(g.pre, SPEEX_PREPROCESS_SET_DENOISE, &on);
        int suppress = -18;  // gentler than speexdsp's default: strong suppression sounds metallic / "radio-like"
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
    opus_encoder_ctl(g.enc, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(g.enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(g.enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));  // up to 20 kHz: natural voices
    opus_encoder_ctl(g.enc, OPUS_SET_COMPLEXITY(9));
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
        // from sequence numbers (20 ms each), so peers with other sample rates (older versions: 16 kHz) count right
        double d = (double)(now - g.lastArrivalUs) / 1000.0 - (double)(int16_t)(seq - g.lastArrivalSeq) * 20.0;
        g.jitterMs += (std::fabs(d) - g.jitterMs) / 16.0;
    }
    g.lastArrivalUs = now;
    g.lastArrivalTs = ts;
    g.lastArrivalSeq = seq;
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

// ---------------------------------------------------------------- voice messages: Opus in an Ogg file
// (RFC 7845). 20 ms packets, ~1 s per Ogg page, denoise + auto gain on the way in.
struct Recorder {
    std::mutex m;
    FILE* f = nullptr;
    std::string path;
    OpusEncoder* enc = nullptr;
    SpeexPreprocessState* pre = nullptr;
    std::vector<int16_t> buf;
    std::vector<uint8_t> page;       // packets of the page being filled
    std::vector<uint8_t> lacing;
    uint32_t serial = 0, pageSeq = 0;
    uint64_t granule = 0;            // in 48 kHz samples
    int preSkip = 0, packetsInPage = 0;
    double seconds = 0;
    float level = 0;
    void (*sink)(void*, const uint8_t*, int) = nullptr;  // stream mode: packets go here instead of a file
    void* sinkUser = nullptr;
    bool Active() const { return f || sink; }
};
Recorder rec;

uint32_t OggCrc(const uint8_t* d, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; k++) r = r & 0x80000000u ? (r << 1) ^ 0x04c11db7u : r << 1;
            table[i] = r;
        }
        init = true;
    }
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++) c = (c << 8) ^ table[((c >> 24) & 0xff) ^ d[i]];
    return c;
}

void OggWritePage(Recorder& r, const uint8_t* body, size_t bodyLen, const std::vector<uint8_t>& lacing, uint64_t granule, uint8_t flags) {
    std::vector<uint8_t> p(27 + lacing.size() + bodyLen);
    memcpy(&p[0], "OggS", 4);
    p[4] = 0;
    p[5] = flags;  // 2 = first page, 4 = last page
    for (int i = 0; i < 8; i++) p[6 + i] = (uint8_t)(granule >> (8 * i));
    for (int i = 0; i < 4; i++) p[14 + i] = (uint8_t)(r.serial >> (8 * i));
    for (int i = 0; i < 4; i++) p[18 + i] = (uint8_t)(r.pageSeq >> (8 * i));
    r.pageSeq++;
    p[26] = (uint8_t)lacing.size();
    memcpy(&p[27], lacing.data(), lacing.size());
    memcpy(&p[27 + lacing.size()], body, bodyLen);
    uint32_t crc = OggCrc(p.data(), p.size());
    for (int i = 0; i < 4; i++) p[22 + i] = (uint8_t)(crc >> (8 * i));
    fwrite(p.data(), 1, p.size(), r.f);
}

void OggAddLacing(std::vector<uint8_t>& lacing, size_t len) {
    while (len >= 255) {
        lacing.push_back(255);
        len -= 255;
    }
    lacing.push_back((uint8_t)len);
}

void OggFlush(Recorder& r, bool last) {
    if (r.packetsInPage == 0 && !last) return;
    OggWritePage(r, r.page.data(), r.page.size(), r.lacing, r.granule, last ? 4 : 0);
    r.page.clear();
    r.lacing.clear();
    r.packetsInPage = 0;
}

// encoder + preprocessor shared by both recorder modes (rec.m held)
bool RecOpen(int bitrate) {
    int err = 0;
    rec.enc = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (!rec.enc) return false;
    opus_encoder_ctl(rec.enc, OPUS_SET_BITRATE(bitrate > 0 ? bitrate : 48000));
    opus_encoder_ctl(rec.enc, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(rec.enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
    opus_int32 look = 0;
    opus_encoder_ctl(rec.enc, OPUS_GET_LOOKAHEAD(&look));
    rec.preSkip = look * (kOpusRate / kRate);  // in 48 kHz samples
    rec.pre = speex_preprocess_state_init(kFrame, kRate);
    int on = 1, sup = -25;
    float lvl = 12000;
    speex_preprocess_ctl(rec.pre, SPEEX_PREPROCESS_SET_DENOISE, &on);
    speex_preprocess_ctl(rec.pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &sup);
    speex_preprocess_ctl(rec.pre, SPEEX_PREPROCESS_SET_AGC, &on);
    speex_preprocess_ctl(rec.pre, SPEEX_PREPROCESS_SET_AGC_LEVEL, &lvl);
    rec.seconds = 0;
    rec.level = 0;
    rec.buf.clear();
    return true;
}

int RecStartStream(int bitrate, void (*packet)(void*, const uint8_t*, int), void* user, int* preSkip) {
    std::lock_guard<std::mutex> l(rec.m);
    if (rec.Active() || !packet) return -1;
    if (!RecOpen(bitrate)) return -3;
    rec.sink = packet;
    rec.sinkUser = user;
    if (preSkip) *preSkip = rec.preSkip;
    return 0;
}

int RecStart(const char* path, int bitrate) {
    std::lock_guard<std::mutex> l(rec.m);
    if (rec.Active()) return -1;
    rec.f = OpenUtf8(path);
    if (!rec.f) return -2;
    if (!RecOpen(bitrate)) {
        fclose(rec.f);
        rec.f = nullptr;
        return -3;
    }
    rec.path = path;
    rec.serial = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
    rec.pageSeq = 0;
    rec.granule = 0;
    rec.page.clear();
    rec.lacing.clear();
    rec.packetsInPage = 0;
    // OpusHead + OpusTags, each on its own page
    uint8_t head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1};
    head[10] = (uint8_t)rec.preSkip;
    head[11] = (uint8_t)(rec.preSkip >> 8);
    uint32_t inRate = kRate;
    for (int i = 0; i < 4; i++) head[12 + i] = (uint8_t)(inRate >> (8 * i));
    std::vector<uint8_t> lac;
    OggAddLacing(lac, sizeof(head));
    OggWritePage(rec, head, sizeof(head), lac, 0, 2);
    const char vendor[] = "LunaShare";
    std::vector<uint8_t> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's', (uint8_t)(sizeof(vendor) - 1), 0, 0, 0};
    tags.insert(tags.end(), vendor, vendor + sizeof(vendor) - 1);
    tags.insert(tags.end(), {0, 0, 0, 0});
    lac.clear();
    OggAddLacing(lac, tags.size());
    OggWritePage(rec, tags.data(), tags.size(), lac, 0, 0);
    return 0;
}

void RecPush(const int16_t* pcm, int n) {
    std::lock_guard<std::mutex> l(rec.m);
    if (!rec.Active()) return;
    rec.buf.insert(rec.buf.end(), pcm, pcm + n);
    size_t off = 0;
    while (rec.buf.size() - off >= (size_t)kFrame) {
        int16_t frame[kFrame];
        memcpy(frame, &rec.buf[off], sizeof(frame));
        off += kFrame;
        speex_preprocess_run(rec.pre, frame);
        rec.level = rec.level * 0.7f + Level(frame, kFrame) * 0.3f;
        uint8_t pkt[kMaxPacket];
        int len = opus_encode(rec.enc, frame, kFrame, pkt, kMaxPacket);
        if (len <= 0) continue;
        rec.seconds += (double)kFrame / kRate;
        if (rec.sink) {
            rec.sink(rec.sinkUser, pkt, len);
            continue;
        }
        rec.page.insert(rec.page.end(), pkt, pkt + len);
        OggAddLacing(rec.lacing, (size_t)len);
        rec.granule += kFrame * (kOpusRate / kRate);
        rec.packetsInPage++;
        if (rec.packetsInPage >= 50 || rec.lacing.size() > 200) OggFlush(rec, false);
    }
    rec.buf.erase(rec.buf.begin(), rec.buf.begin() + (ptrdiff_t)off);
}

float RecLevel() { return rec.level; }

double RecStop(int keep) {
    std::lock_guard<std::mutex> l(rec.m);
    if (!rec.Active()) return 0;
    bool file = rec.f != nullptr;
    if (file) {
        OggFlush(rec, true);
        fclose(rec.f);
        rec.f = nullptr;
    }
    rec.sink = nullptr;
    rec.sinkUser = nullptr;
    opus_encoder_destroy(rec.enc);
    rec.enc = nullptr;
    speex_preprocess_state_destroy(rec.pre);
    rec.pre = nullptr;
    double s = rec.seconds;
    if (file && !keep) RemoveUtf8(rec.path.c_str());
    return s;
}

const LsmModule kModule = {
    LSM_API_VERSION, "LunaShare Voice", "1.2", "voice",
    Start, Stop, Capture, Playback, Receive, SetMute, SetSettings, GetStats,
    RecStart, RecPush, RecLevel, RecStop, RecStartStream,
};

}  // namespace

extern "C" LSM_EXPORT const LsmModule* lunashare_media_module(void) { return &kModule; }
