// Voice module tests with synthetic audio (no devices needed).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

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

// quiet microphone (laptop / phone at arm's length): default call settings must bring speech to a normal level
static double SentLevelDb(int micAmp, double* inDb) {
    const LsmModule* m = lunashare_media_module();
    Net net;
    LsmHost h{LSM_API_VERSION, &net, HostSend, HostLog};
    LsmSettings s{1, 1, 1, 48000};
    m->start(&h, &s);
    int err;
    OpusDecoder* tap = opus_decoder_create(R, 1, &err);  // decodes what we send
    std::mt19937 rng(3);
    int16_t frame[F], silence[F] = {}, dec[F * 6];
    double t = 0, inSq = 0, outSq = 0;
    long inN = 0, outN = 0;
    for (int step = 0; step < 500; step++) {  // 10 s of syllables: 200 ms voiced, 100 ms pause
        for (int i = 0; i < F; i++, t += 1.0 / R) {
            double ph = std::fmod(t, 0.3), env = ph < 0.2 ? std::sin(PI * ph / 0.2) : 0;
            double f0 = 140 + 30 * std::sin(t * 2.1), v = 0;
            for (int k = 1; k <= 12; k++) v += std::sin(2 * PI * f0 * k * t) / k;  // harmonic voice
            double x = micAmp * env * v + std::normal_distribution<>(0, 10)(rng);
            frame[i] = (int16_t)std::clamp(x, -32768.0, 32767.0);
            if (step >= 300) { inSq += x * x; inN++; }
        }
        m->playback(silence, F);
        m->capture(frame, F);
        for (auto& p : net.sent) {
            int n = opus_decode(tap, p.data() + 7, (opus_int32)p.size() - 7, dec, F * 6, 0);
            if (step >= 300)
                for (int i = 0; i < n; i++) { outSq += (double)dec[i] * dec[i]; outN++; }
        }
        net.sent.clear();
    }
    m->stop();
    opus_decoder_destroy(tap);
    *inDb = 20 * std::log10(std::sqrt(inSq / std::max(1L, inN)) / 32768.0 + 1e-9);
    return 20 * std::log10(std::sqrt(outSq / std::max(1L, outN)) / 32768.0 + 1e-9);
}

static void TestLoudness() {
    printf("[loudness: speech level sent, default settings]\n");
    for (int amp : {150, 500, 2000, 8000}) {
        double in, out = SentLevelDb(amp, &in);
        printf("  mic %.1f dBFS -> sent %.1f dBFS\n", in, out);
        CHECK(out > -23 && out < -16, "speech sent at a normal level (%.1f dBFS)", out);
    }
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
    // (steady tones are not what AEC3 is tuned for - it removes less here than from speech; TestEchoRoom is the real test)
    CHECK(echoOff - echoOn > 10, "echo reduced by more than 10 dB");
    CHECK(nearOff - nearOn < 10, "own voice preserved");
}

// Like a phone: the speaker takes 512 samples every 10.7 ms, the microphone delivers 40 ms at once, sound comes out
// 60 ms after it's handed over and reaches the microphone 20 ms later. (A simulated clock, 1 ms steps: the same on
// every machine - a sleeping test was at the mercy of Windows' 15 ms timer.)
static double EchoRunBursty(bool aec) {
    const LsmModule* m = lunashare_media_module();
    Net net;
    LsmHost h{LSM_API_VERSION, &net, HostSend, HostLog};
    LsmSettings s{aec ? 1 : 0, 0, 0, 32000};
    m->start(&h, &s);
    int err;
    OpusEncoder* farEnc = opus_encoder_create(R, 1, OPUS_APPLICATION_VOIP, &err);
    OpusDecoder* tap = opus_decoder_create(R, 1, &err);
    std::vector<int16_t> handed, sentPcm;  // every sample given to the speaker, in order
    std::mt19937 rng(9);
    const int outLat = R * 60 / 1000, acoustic = R * 20 / 1000, P = 512, C = R * 40 / 1000;
    int64_t played = 0, captured = 0, farSent = 0;
    const int64_t total = R * 6;  // 6 s
    for (int64_t ms = 0; captured < total; ms++) {
        int64_t now = ms * R / 1000;
        while (farSent * F < now + R / 10) {  // the far end keeps ~100 ms ahead (the jitter buffer smooths it)
            int16_t far[F];
            for (int i = 0; i < F; i++) {
                int64_t n = farSent * F + i;
                double env = 0.5 + 0.5 * std::sin(2 * PI * 3 * n / R);
                far[i] = (int16_t)(9000 * env * std::sin(2 * PI * 1000 * n / R) + (int)(rng() % 600) - 300);
            }
            uint8_t pkt[1500];
            pkt[0] = 1;
            pkt[1] = (uint8_t)farSent;
            pkt[2] = (uint8_t)(farSent >> 8);
            uint32_t ts = (uint32_t)(farSent * F);
            memcpy(pkt + 3, &ts, 4);
            int n = opus_encode(farEnc, far, F, pkt + 7, 1400);
            m->receive(pkt, (size_t)n + 7);
            farSent++;
        }
        if (played <= now) {  // the speaker wants more
            int16_t spk[P];
            m->playback(spk, P);
            handed.insert(handed.end(), spk, spk + P);
            played += P;
        }
        if (captured + C <= now) {  // the microphone hands over 40 ms
            std::vector<int16_t> mic((size_t)C);
            for (int i = 0; i < C; i++) {
                int64_t at = captured + i, src = at - outLat - acoustic;  // what the speaker sounded then
                double echo = src >= 0 && src < (int64_t)handed.size() ? handed[(size_t)src] * 0.6 : 0;
                mic[(size_t)i] = (int16_t)std::clamp(echo + (double)((int)(rng() % 200) - 100), -32767.0, 32767.0);
            }
            m->capture(mic.data(), C);
            captured += C;
            for (auto& p : net.sent) {
                int16_t dec[F * 6];
                int k = opus_decode(tap, p.data() + 7, (opus_int32)p.size() - 7, dec, F * 6, 0);
                if (k > 0) sentPcm.insert(sentPcm.end(), dec, dec + k);
            }
            net.sent.clear();
        }
    }
    m->stop();
    opus_encoder_destroy(farEnc);
    opus_decoder_destroy(tap);
    return Db(Tone(sentPcm, sentPcm.size() / 2, sentPcm.size(), 1000));
}

static void TestEchoBursty() {
    printf("[echo cancellation, phone-like timing]\n");
    double off = EchoRunBursty(false), on = EchoRunBursty(true);
    printf("  echo in what we send: %.1f dB without AEC, %.1f dB with AEC (-%.1f dB)\n", off, on, off - on);
    CHECK(off - on > 15, "echo reduced by more than 15 dB with bursty audio callbacks");
}

// Speech-like test signal: a pitch pulse train through three moving formants, in syllables and talk spurts with pauses.
struct FakeTalker {
    std::mt19937 rng;
    double f0, phase = 0, y1[3] = {}, y2[3] = {};
    double fm[3] = {500, 1500, 2500}, target[3] = {500, 1500, 2500};
    int64_t n = 0;
    FakeTalker(unsigned seed, double pitch) : rng(seed), f0(pitch) {}
    double Next() {
        double t = (double)n / R;
        if (n % (R / 5) == 0) {  // a new syllable every 200 ms: formants move somewhere else
            target[0] = 300 + rng() % 600;
            target[1] = 900 + rng() % 1400;
            target[2] = 2200 + rng() % 1000;
        }
        n++;
        double spurt = std::fmod(t, 2.3) < 1.6 ? 1.0 : 0.0;  // 1.6 s talking, 0.7 s pause
        double syl = std::max(0.0, std::sin(2 * PI * 2.5 * t));
        double pitch = f0 * (1 + 0.15 * std::sin(2 * PI * 0.7 * t));
        phase += pitch / R;
        double src = 0;
        if (phase >= 1) { phase -= 1; src = 1.0; }
        src += ((double)(rng() % 2001) - 1000) / 1000.0 * 0.05;  // breath
        double out = 0;
        for (int k = 0; k < 3; k++) {
            fm[k] += (target[k] - fm[k]) * 0.001;
            double r = 0.995, w = 2 * PI * fm[k] / R;
            double y = src + 2 * r * std::cos(w) * y1[k] - r * r * y2[k];
            y2[k] = y1[k];
            y1[k] = y;
            out += y * (k == 0 ? 1.0 : 0.5);
        }
        return out * spurt * syl;
    }
};

// A room: speaker distortion, 70 ms to the microphone, then reverb (decaying reflections over 150 ms).
// Phase 1 (0-8 s): only the far end talks - the echo canceller learns the room, then we measure what echo is left.
// Phase 2 (8-14 s): both talk at once - how much of our own voice survives.
static void EchoRoomRun(bool aec, bool withEcho, bool withNear, double* farOnly, double* doubleTalk) {
    const LsmModule* m = lunashare_media_module();
    Net net;
    LsmHost h{LSM_API_VERSION, &net, HostSend, HostLog};
    LsmSettings s{aec ? 1 : 0, 0, 0, 64000};
    m->start(&h, &s);
    int err;
    OpusEncoder* farEnc = opus_encoder_create(R, 1, OPUS_APPLICATION_VOIP, &err);
    opus_encoder_ctl(farEnc, OPUS_SET_BITRATE(64000));
    OpusDecoder* tap = opus_decoder_create(R, 1, &err);
    FakeTalker far(11, 120), near(23, 210);
    std::vector<double> ir((size_t)(R * 150 / 1000));
    std::mt19937 rng(5);
    for (size_t i = 0; i < ir.size(); i++)
        ir[i] = (i == 0 ? 0.5 : 0.0) + ((double)(rng() % 2001) - 1000) / 1000.0 * 0.08 * std::exp(-(double)i / (R * 0.03));
    const int delay = R * 70 / 1000;
    std::vector<double> spk;  // distorted speaker output
    std::vector<int16_t> sent;
    const int steps = 14 * 50, dtFrom = 8 * 50;
    for (int step = 0; step < steps; step++) {
        int16_t f[F];
        for (int i = 0; i < F; i++) f[i] = (int16_t)std::clamp(far.Next() * 2500, -32000.0, 32000.0);
        uint8_t pkt[1500];
        pkt[0] = 1;
        pkt[1] = (uint8_t)step; pkt[2] = (uint8_t)(step >> 8);
        uint32_t ts = (uint32_t)(step * F);
        memcpy(pkt + 3, &ts, 4);
        int n = opus_encode(farEnc, f, F, pkt + 7, 1400);
        m->receive(pkt, (size_t)n + 7);
        int16_t out[F];
        m->playback(out, F);
        for (int i = 0; i < F; i++) {
            double x = out[i] / 32768.0;
            spk.push_back(std::tanh(2.0 * x) / 2.0);  // a small speaker pushed a bit too hard
        }
        int16_t mic[F];
        for (int i = 0; i < F; i++) {
            int64_t at = (int64_t)spk.size() - F + i - delay;
            double e = 0;
            if (withEcho)
                for (size_t k = 0; k < ir.size() && at - (int64_t)k >= 0; k += 1) e += ir[k] * spk[(size_t)(at - (int64_t)k)];
            double v = withNear && step >= dtFrom ? near.Next() * 1200 : (near.Next(), 0.0);
            mic[i] = (int16_t)std::clamp(e * 32768.0 + v + (double)((int)(rng() % 60) - 30), -32767.0, 32767.0);
        }
        m->capture(mic, F);
        for (auto& p : net.sent) {
            int16_t dec[F * 6];
            int k = opus_decode(tap, p.data() + 7, (opus_int32)p.size() - 7, dec, F * 6, 0);
            if (k > 0) sent.insert(sent.end(), dec, dec + k);
        }
        net.sent.clear();
    }
    m->stop();
    opus_encoder_destroy(farEnc);
    opus_decoder_destroy(tap);
    auto energy = [&](size_t a, size_t b) {
        double e = 0;
        for (size_t i = a; i < b && i < sent.size(); i++) e += (double)sent[i] * sent[i];
        return Db(e / (double)std::max<size_t>(1, b - a) / (32768.0 * 32768.0));
    };
    *farOnly = energy((size_t)R * 4, (size_t)R * 8);
    *doubleTalk = energy((size_t)R * 9, (size_t)R * 14);
}

static void TestEchoRoom() {
    printf("[echo cancellation: speech-like, reverberant room, distorting speaker, double talk]\n");
    double echoOff, dtOff, echoOn, dtOn, nearAlone, unused;
    EchoRoomRun(false, true, true, &echoOff, &dtOff);
    EchoRoomRun(true, true, true, &echoOn, &dtOn);
    EchoRoomRun(false, false, true, &unused, &nearAlone);
    printf("  echo while only the far end talks: %.1f dB without AEC, %.1f dB with AEC (-%.1f dB)\n", echoOff, echoOn,
           echoOff - echoOn);
    printf("  both talking: own voice alone %.1f dB, with echo + AEC %.1f dB (%+.1f dB)\n", nearAlone, dtOn, dtOn - nearAlone);
    CHECK(echoOff - echoOn > 20, "echo in a reverberant room reduced by more than 20 dB");
    CHECK(dtOn - nearAlone > -6, "own voice kept while both talk");
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

// Group calls: two people talking at once (440 Hz and 1000 Hz) on two streams come out mixed; one leaving stops theirs.
static void TestMix() {
    printf("[group call: mixing two people]\n");
    const LsmModule* m = lunashare_media_module();
    Net net;
    LsmHost h{LSM_API_VERSION, &net, HostSend, HostLog};
    LsmSettings s{0, 0, 0, 32000};
    CHECK(m->start(&h, &s) == 0, "start");
    int err;
    OpusEncoder* e1 = opus_encoder_create(R, 1, OPUS_APPLICATION_VOIP, &err);
    OpusEncoder* e2 = opus_encoder_create(R, 1, OPUS_APPLICATION_VOIP, &err);
    std::vector<int16_t> out;
    for (int step = 0; step < 300; step++) {
        for (int who = 0; who < 2; who++) {
            if (who == 1 && step >= 150) {  // the second person leaves halfway
                if (step == 150) m->end_stream(7);
                continue;
            }
            int16_t f[F];
            double hz = who ? 1000 : 440;
            for (int i = 0; i < F; i++) f[i] = (int16_t)(7000 * std::sin(2 * PI * hz * (step * F + i) / R));
            uint8_t pkt[1500];
            pkt[0] = 1;
            pkt[1] = (uint8_t)step;
            pkt[2] = (uint8_t)(step >> 8);
            uint32_t ts = (uint32_t)(step * F);
            memcpy(pkt + 3, &ts, 4);
            int n = opus_encode(who ? e2 : e1, f, F, pkt + 7, 1400);
            m->receive_from(who ? 7 : 3, pkt, (size_t)n + 7);
        }
        int16_t spk[F];
        m->playback(spk, F);
        out.insert(out.end(), spk, spk + F);
    }
    m->stop();
    opus_encoder_destroy(e1);
    opus_encoder_destroy(e2);
    double a440 = Db(Tone(out, (size_t)R, (size_t)R * 2, 440)), a1000 = Db(Tone(out, (size_t)R, (size_t)R * 2, 1000));
    double b440 = Db(Tone(out, (size_t)R * 4, (size_t)R * 5, 440)), b1000 = Db(Tone(out, (size_t)R * 4, (size_t)R * 5, 1000));
    printf("  both talking: 440 Hz %.1f dB, 1000 Hz %.1f dB; after one left: %.1f / %.1f dB\n", a440, a1000, b440, b1000);
    CHECK(a440 > -30 && a1000 > -30, "both people are heard");
    CHECK(b440 > -30 && b1000 < a1000 - 30, "the one who left is gone");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestMix();
    const LsmModule* m = lunashare_media_module();
    CHECK(m && m->api_version == LSM_API_VERSION && strcmp(m->caps, "voice") == 0, "module descriptor");
    TestLoopback(0.0);
    TestLoopback(0.10);
    TestLoudness();
    TestEcho();
    TestEchoBursty();
    TestEchoRoom();
    TestVoiceMessage();
    TestVideoMessageFile();
    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL VOICE TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
