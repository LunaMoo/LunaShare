// Voice module tests with synthetic audio (no devices needed).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include <opus.h>

#include "lunashare_media.h"
#include "mp4_writer.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

constexpr int R = LSM_SAMPLE_RATE, F = R / 50;
constexpr double PI = 3.14159265358979;

// Power of a frequency component (Goertzel), normalised by length.
static double Tone(const std::vector<int16_t>& x, size_t from, size_t to, double hz) {
    double w = 2 * PI * hz / R, c = 2 * std::cos(w), s1 = 0, s2 = 0;
    for (size_t i = from; i < to && i < x.size(); i++) {
        double s = x[i] / 32768.0 + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return p / (double)(to - from) / (double)(to - from);
}
static double Db(double p) { return 10 * std::log10(p + 1e-20); }

struct Net {
    std::vector<std::vector<uint8_t>> sent;
};
static void HostSend(void* u, const uint8_t* d, size_t n) { ((Net*)u)->sent.emplace_back(d, d + n); }
static void HostLog(void*, const char*) {}

static void TestLoopback(double loss) {
    printf("[voice loopback, %.0f%% loss + jitter]\n", loss * 100);
    const LsmModule* m = lunashare_media_module();
    Net net;
    LsmHost h{LSM_API_VERSION, &net, HostSend, HostLog};
    LsmSettings s{0, 0, 0, 24000};  // no AEC/NS: we loop our own voice back
    CHECK(m->start(&h, &s) == 0, "start");
    std::mt19937 rng(1);
    std::deque<std::pair<int, std::vector<uint8_t>>> inflight;  // (deliver at step, packet)
    std::vector<int16_t> out;
    int16_t frame[F];
    int lost = 0, total = 0;
    for (int step = 0; step < 250; step++) {  // 5 s
        for (int i = 0; i < F; i++) frame[i] = (int16_t)(8000 * std::sin(2 * PI * 440 * (step * F + i) / R));
        m->capture(frame, F);
        for (auto& p : net.sent) {
            total++;
            if (std::uniform_real_distribution<>(0, 1)(rng) < loss) { lost++; continue; }
            inflight.push_back({step + 1 + (int)(rng() % 3), p});  // 20..60 ms network delay
        }
        net.sent.clear();
        for (auto it = inflight.begin(); it != inflight.end();) {
            if (it->first <= step) { m->receive(it->second.data(), it->second.size()); it = inflight.erase(it); }
            else ++it;
        }
        int16_t spk[F];
        m->playback(spk, F);
        out.insert(out.end(), spk, spk + F);
    }
    LsmStats st;
    m->get_stats(&st);
    m->stop();
    size_t a = (size_t)R * 2, b = out.size();  // ignore the first 2 s
    double sig = Tone(out, a, b, 440), other = Tone(out, a, b, 1234) + Tone(out, a, b, 2000) + 1e-12;
    long silent = 0;
    for (size_t i = a; i < b; i += F) {
        double e = 0;
        for (int k = 0; k < F; k++) e += std::fabs((double)out[i + k]);
        if (e / F < 300) silent++;
    }
    printf("  440 Hz: %.1f dB above other tones, silent frames %ld/%ld, sent %d lost %d, stats: loss %.1f%% jitter %d ms buffered %d ms\n",
           Db(sig) - Db(other), silent, (long)((b - a) / F), total, lost, st.loss_percent, st.jitter_ms, st.buffered_ms);
    CHECK(Db(sig) - Db(other) > 25, "tone survives");
    CHECK(silent < (long)((b - a) / F) / 20, "no dropouts (%ld silent frames)", silent);
    CHECK(st.packets_out == 250, "one packet per 20 ms (%d)", st.packets_out);
    CHECK(st.buffered_ms < 250, "bounded latency (%d ms)", st.buffered_ms);
}

static double EchoRun(bool aec, double* nearOut, bool ns = true) {
    const LsmModule* m = lunashare_media_module();
    Net net;
    LsmHost h{LSM_API_VERSION, &net, HostSend, HostLog};
    LsmSettings s{aec ? 1 : 0, aec && ns ? 1 : 0, 0, 32000};
    m->start(&h, &s);
    // far end: someone talking (1000 Hz tone with a syllable-like envelope), encoded by "their" Opus
    int err;
    OpusEncoder* farEnc = opus_encoder_create(R, 1, OPUS_APPLICATION_VOIP, &err);
    OpusDecoder* tap = opus_decoder_create(R, 1, &err);  // decodes what we send, to measure it
    std::vector<int16_t> spkHist, sentPcm;
    std::mt19937 rng(7);
    const int echoDelay = R * 30 / 1000;  // 30 ms acoustic path speaker -> mic
    for (int step = 0; step < 400; step++) {  // 8 s
        int16_t far[F];
        for (int i = 0; i < F; i++) {
            int n = step * F + i;
            double env = 0.5 + 0.5 * std::sin(2 * PI * 3 * n / R);
            far[i] = (int16_t)(9000 * env * std::sin(2 * PI * 1000 * n / R) + (int)(rng() % 600) - 300);
        }
        uint8_t pkt[1500];
        pkt[0] = 1;
        pkt[1] = (uint8_t)step; pkt[2] = (uint8_t)(step >> 8);
        uint32_t ts = (uint32_t)(step * F);
        memcpy(pkt + 3, &ts, 4);
        int n = opus_encode(farEnc, far, F, pkt + 7, 1400);
        m->receive(pkt, (size_t)n + 7);
        int16_t spk[F];
        m->playback(spk, F);
        spkHist.insert(spkHist.end(), spk, spk + F);
        // microphone = our own voice (300 Hz, quieter) + the far end coming back from our speaker
        int16_t mic[F];
        for (int i = 0; i < F; i++) {
            size_t idx = spkHist.size() - F + i;
            double echo = idx >= (size_t)echoDelay ? spkHist[idx - echoDelay] * 0.6 : 0;
            int nn = step * F + i;
            // our voice: syllable-like bursts (speech is not stationary; a constant tone would look like noise)
            double nenv = std::max(0.0, std::sin(2 * PI * 2.3 * nn / R + 1.0));
            double near = 3000 * nenv * std::sin(2 * PI * 300 * nn / R);
            mic[i] = (int16_t)std::clamp(near + echo + (double)((int)(rng() % 200) - 100), -32767.0, 32767.0);
        }
        m->capture(mic, F);
        for (auto& p : net.sent) {
            int16_t dec[F * 6];
            int k = opus_decode(tap, p.data() + 7, (opus_int32)p.size() - 7, dec, F * 6, 0);
            if (k > 0) sentPcm.insert(sentPcm.end(), dec, dec + k);
        }
        net.sent.clear();
    }
    m->stop();
    opus_encoder_destroy(farEnc);
    opus_decoder_destroy(tap);
    size_t a = sentPcm.size() / 2, b = sentPcm.size();  // after convergence
    *nearOut = Db(Tone(sentPcm, a, b, 300));
    return Db(Tone(sentPcm, a, b, 1000));
}

static void TestEcho() {
    printf("[echo cancellation]\n");
    double nearOff, nearOn, nearNoNs;
    double echoOff = EchoRun(false, &nearOff), echoOn = EchoRun(true, &nearOn), echoNoNs = EchoRun(true, &nearNoNs, false);
    printf("  far-end echo in what we send: %.1f dB without AEC, %.1f dB with AEC (-%.1f dB); own voice %.1f -> %.1f dB\n", echoOff,
           echoOn, echoOff - echoOn, nearOff, nearOn);
    printf("  AEC without noise suppression: echo -%.1f dB, own voice %.1f dB\n", echoOff - echoNoNs, nearNoNs);
    CHECK(echoOff - echoOn > 15, "echo reduced by more than 15 dB");
    CHECK(nearOff - nearOn < 10, "own voice preserved");
}

static void TestVoiceMessage() {
    printf("[voice message: Ogg Opus file]\n");
    const LsmModule* m = lunashare_media_module();
    std::string path = "voice_message_test.ogg";
    CHECK(m->rec_start(path.c_str(), 24000) == 0, "recording starts");
    std::vector<int16_t> pcm(F);
    double t = 0;
    for (int f = 0; f < 150; f++) {  // 3 s, speech-like warble
        for (auto& v : pcm) {
            v = (int16_t)(8000 * std::sin(t * 2 * 3.14159265 * (200 + 60 * std::sin(t * 3))) * (0.6 + 0.4 * std::sin(t * 7)));
            t += 1.0 / R;
        }
        m->rec_push(pcm.data(), (int)pcm.size());
    }
    double secs = m->rec_stop(1);
    CHECK(std::fabs(secs - 3.0) < 0.05, "3 s recorded (%.2f)", secs);
    FILE* f = fopen(path.c_str(), "rb");
    std::vector<uint8_t> data;
    if (f) {
        uint8_t b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) data.insert(data.end(), b, b + n);
        fclose(f);
    }
    CHECK(data.size() > 4000 && data.size() < 20000, "file size fits 24 kbit/s (%zu bytes)", data.size());
    CHECK(data.size() > 36 && memcmp(data.data(), "OggS", 4) == 0 && memcmp(data.data() + 28, "OpusHead", 8) == 0, "Ogg Opus headers");
    int pages = 0;
    for (size_t i = 0; i + 4 < data.size(); i++)
        if (memcmp(&data[i], "OggS", 4) == 0) pages++;
    CHECK(pages >= 4, "%d Ogg pages", pages);
    printf("  %.2f s, %zu bytes, %d pages\n", secs, data.size(), pages);
    m->rec_start("voice_message_cancel.ogg", 24000);  // cancelled recordings leave nothing behind
    m->rec_push(pcm.data(), (int)pcm.size());
    m->rec_stop(0);
    FILE* g = fopen("voice_message_cancel.ogg", "rb");
    CHECK(g == nullptr, "cancelled recording deleted");
    if (g) fclose(g);
}

// video message: Opus packets from the stream recorder plus (synthetic) H.264 into an MP4
static ls::Mp4Writer* g_mp4 = nullptr;
static int g_streamPackets = 0;
static void StreamPacket(void*, const uint8_t* d, int n) {
    g_streamPackets++;
    if (g_mp4) g_mp4->AddAudio(d, (size_t)n);
}

static void TestVideoMessageFile() {
    printf("[video message: MP4 with H.264 + Opus]\n");
    const LsmModule* m = lunashare_media_module();
    ls::Mp4Writer w;
    int preSkip = 0;
    CHECK(m->rec_start_stream(32000, StreamPacket, nullptr, &preSkip) == 0 && preSkip > 0, "stream recorder starts (pre-skip %d)", preSkip);
    CHECK(m->rec_start("x.ogg", 24000) != 0, "only one recording at a time");
    std::string path = "video_message_test.mp4";
    CHECK(w.Open(path, 90, preSkip), "mp4 opens");
    g_mp4 = &w;
    const uint8_t sps[] = {0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f, 0xda, 0x01, 0x40};
    const uint8_t pps[] = {0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};
    std::vector<int16_t> pcm(R / 30);  // one video frame of audio
    for (int i = 0; i < 60; i++) {  // 2 s at 30 fps, keyframe every second
        bool key = i % 30 == 0;
        std::vector<uint8_t> au;
        if (key) {
            au.insert(au.end(), sps, sps + sizeof(sps));
            au.insert(au.end(), pps, pps + sizeof(pps));
        }
        uint8_t slice[] = {0, 0, 1, (uint8_t)(key ? 0x65 : 0x41), 0x88, (uint8_t)i, 0x10, 0x20};
        au.insert(au.end(), slice, slice + sizeof(slice));
        if (i == 0) w.AddVideo(slice, sizeof(slice), false, 0, 1280, 720);  // a non-keyframe first is ignored
        w.AddVideo(au.data(), au.size(), key, (int64_t)i * 33333, 1280, 720);
        for (size_t k = 0; k < pcm.size(); k++) pcm[k] = (int16_t)(6000 * std::sin((i * (int)pcm.size() + (int)k) * 0.17 * 16000 / R));
        m->rec_push(pcm.data(), (int)pcm.size());
    }
    m->rec_stop(0);
    g_mp4 = nullptr;
    double secs = w.Finish();
    CHECK(std::fabs(secs - 2.0) < 0.1, "2 s written (%.2f)", secs);
    CHECK(g_streamPackets >= 95, "%d Opus packets", g_streamPackets);
    FILE* f = fopen(path.c_str(), "rb");
    std::vector<uint8_t> data;
    if (f) {
        uint8_t b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) data.insert(data.end(), b, b + n);
        fclose(f);
    }
    auto has = [&](const char* tag) {
        return std::search(data.begin(), data.end(), tag, tag + 4) != data.end();
    };
    CHECK(data.size() > 32 && memcmp(&data[4], "ftyp", 4) == 0, "starts with ftyp");
    CHECK(has("moov") && has("avcC") && has("dOps") && has("stss") && has("co64"), "index boxes present");
    // the first video sample right after the mdat header: 4-byte length + IDR slice (SPS/PPS moved into avcC)
    size_t mdat = 0;
    for (size_t i = 0; i + 8 < data.size(); i++)
        if (memcmp(&data[i], "mdat", 4) == 0) { mdat = i + 12; break; }
    CHECK(mdat && data.size() > mdat + 5 && data[mdat] == 0 && data[mdat + 3] == 5 && data[mdat + 4] == 0x65, "Annex B converted to length prefixes");
    remove(path.c_str());
    ls::Mp4Writer w2;  // discarded recordings leave nothing behind
    w2.Open("video_message_cancel.mp4", 0, 312);
    w2.Abort();
    FILE* g = fopen("video_message_cancel.mp4", "rb");
    CHECK(g == nullptr, "discarded video message deleted");
    if (g) fclose(g);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const LsmModule* m = lunashare_media_module();
    CHECK(m && m->api_version == LSM_API_VERSION && strcmp(m->caps, "voice") == 0, "module descriptor");
    TestLoopback(0.0);
    TestLoopback(0.10);
    TestEcho();
    TestVoiceMessage();
    TestVideoMessageFile();
    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL VOICE TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
