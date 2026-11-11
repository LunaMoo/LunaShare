// LunaShare player module - portable core: a decode thread keeps ~1 s of audio and a few pictures ahead; the
// host's audio callback pulls audio (which also drives the clock) and the UI asks for the picture that belongs to
// the current position. Files without audio run on a wall clock.
// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "player_backend.h"

namespace lsp {
namespace {

double NowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

LspHost g_host{};
bool g_init = false;

void Log(const std::string& m) {
    if (g_host.log) g_host.log(g_host.user, ("player: " + m).c_str());
}

// linear resampler + channel mapping into the device format. Positions are in input frames; index -1 is the
// last frame of the previous chunk, so consecutive chunks join without clicks.
class Resampler {
public:
    void Reset() {
        phase_ = 0;
        have_ = false;
    }
    void Run(const float* in, int frames, int inRate, int inCh, int outRate, int outCh, std::vector<float>& out) {
        if (frames <= 0 || inRate <= 0 || inCh <= 0) return;
        if (inCh > 2) {  // surround: fold it into stereo first (the voices are in the centre channel)
            Downmix(in, frames, inCh, dm_);
            in = dm_.data();
            inCh = 2;
        }
        if (inRate != inRate_ || inCh != inCh_ || outCh != outCh_) {
            inRate_ = inRate;
            inCh_ = inCh;
            outCh_ = outCh;
            Reset();
        }
        auto sample = [&](int i, int c) -> float {  // output channel c of input frame i (-1 = previous chunk)
            if (i < 0) return last_[c];
            const float* f = in + (size_t)i * inCh;
            if (inCh == 1) return f[0];
            if (outCh == 1) return (f[0] + f[1]) * 0.5f;
            return f[std::min(c, inCh - 1)];
        };
        if (!have_) phase_ = 0;
        double step = (double)inRate / outRate;
        double pos = phase_;
        while (pos < frames - 1) {
            int i = pos < 0 ? -1 : (int)pos;
            double t = pos - i;
            for (int c = 0; c < outCh; c++) {
                float a = sample(i, c), b = sample(i + 1, c);
                out.push_back((float)(a + (b - a) * t));
            }
            pos += step;
        }
        phase_ = pos - frames;  // >= -1
        last_.resize((size_t)outCh);
        for (int c = 0; c < outCh; c++) last_[c] = sample(frames - 1, c);
        have_ = true;
    }

    // Surround -> stereo, in the usual channel orders (FFmpeg's and Android's): 3 = L R C, 4 = L R BL BR,
    // 5 = L R C SL SR, 6 = L R C LFE SL SR, 7 = L R C LFE BC SL SR, 8 = L R C LFE BL BR SL SR. Centre at -3 dB into
    // both sides, surrounds at -6 dB; the bass channel is left out (the main channels carry it too). Scaled so loud
    // passages don't clip, with a soft limit for the rare peak.
    static void Downmix(const float* in, int frames, int ch, std::vector<float>& out) {
        out.resize((size_t)frames * 2);
        const float c = 0.7071f, s = 0.5f, g = 1.0f / 1.45f;
        for (int i = 0; i < frames; i++) {
            const float* f = in + (size_t)i * ch;
            float L = f[0], R = f[1], C = 0, SL = 0, SR = 0;
            switch (ch) {
                case 3: C = f[2]; break;
                case 4: SL = f[2]; SR = f[3]; break;
                case 5: C = f[2]; SL = f[3]; SR = f[4]; break;
                case 6: C = f[2]; SL = f[4]; SR = f[5]; break;
                case 7: C = f[2]; SL = f[5] + f[4] * 0.7071f; SR = f[6] + f[4] * 0.7071f; break;
                default: C = f[2]; SL = f[4] + f[6]; SR = f[5] + f[7]; break;  // 8 and more
            }
            float l = (L + c * C + s * SL) * g, r = (R + c * C + s * SR) * g;
            auto soft = [](float x) { float a = std::fabs(x); return a <= 0.9f ? x : std::copysign(0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f), x); };
            out[(size_t)i * 2] = soft(l);
            out[(size_t)i * 2 + 1] = soft(r);
        }
    }

private:
    std::vector<float> dm_;
    double phase_ = 0;
    bool have_ = false;
    int inRate_ = 0, inCh_ = 0, outCh_ = 0;
    std::vector<float> last_;
};

struct Frame {
    std::vector<uint8_t> y, uv;  // tight Y + interleaved U/V
    int w = 0, h = 0, rotation = 0;
    double pts = 0;
};

struct SubEvent {
    double start = 0, end = -1;  // end < 0: until the next one (at most a while)
    std::string text;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    float x = 0, y = 0, sw = 0, sh = 0;
    bool Empty() const { return text.empty() && rgba.empty(); }
};

// ---- subtitle text: ASS override tags {\...}, HTML-ish tags <i>, line breaks \N, and ASS event fields
std::string CleanSubText(std::string s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '{') {  // {\an8} {\i1}...
            size_t e = s.find('}', i);
            if (e != std::string::npos) { i = e; continue; }
        }
        if (c == '<') {  // <i> </i> <font color=...>
            size_t e = s.find('>', i);
            if (e != std::string::npos && e - i < 64) { i = e; continue; }
        }
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == 'N' || s[i + 1] == 'n')) { o += '\n'; i++; continue; }
        if (c == '\\' && i + 1 < s.size() && s[i + 1] == 'h') { o += ' '; i++; continue; }
        if (c == '\r') continue;
        o += c;
    }
    // trim lines
    std::string r, line;
    for (size_t i = 0; i <= o.size(); i++) {
        if (i == o.size() || o[i] == '\n') {
            size_t a = line.find_first_not_of(" \t"), b = line.find_last_not_of(" \t");
            if (a != std::string::npos) r += (r.empty() ? "" : "\n") + line.substr(a, b - a + 1);
            line.clear();
        } else {
            line += o[i];
        }
    }
    return r;
}

// subtitle files are often in Windows-1250 (Polish, Czech...) rather than UTF-8
bool ValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n ? 0 : 1)) return false;
        for (int k = 1; k <= n; k++)
            if (((unsigned char)s[i + k] >> 6) != 2) return false;
        i += (size_t)n + 1;
    }
    return true;
}
std::string FromCp1250(const std::string& s) {
    static const uint16_t hi[128] = {
        0x20AC, 0x81, 0x201A, 0x83, 0x201E, 0x2026, 0x2020, 0x2021, 0x88, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
        0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x98, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
        0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
        0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
        0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7, 0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
        0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7, 0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
        0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7, 0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
        0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7, 0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9};
    std::string o;
    for (unsigned char c : s) {
        uint32_t u = c < 0x80 ? c : hi[c - 0x80];
        if (u < 0x80) o += (char)u;
        else if (u < 0x800) { o += (char)(0xC0 | u >> 6); o += (char)(0x80 | (u & 0x3F)); }
        else { o += (char)(0xE0 | u >> 12); o += (char)(0x80 | ((u >> 6) & 0x3F)); o += (char)(0x80 | (u & 0x3F)); }
    }
    return o;
}

// ... or in ISO-8859-2, which differs from it in a few letters (ą ś ź Ą Ś Ź): whichever has more of them wins
std::string FromLatin2(const std::string& s) {
    static const uint16_t hi[96] = {
        0x00A0, 0x0104, 0x02D8, 0x0141, 0x00A4, 0x013D, 0x015A, 0x00A7, 0x00A8, 0x0160, 0x015E, 0x0164, 0x0179, 0x00AD, 0x017D, 0x017B,
        0x00B0, 0x0105, 0x02DB, 0x0142, 0x00B4, 0x013E, 0x015B, 0x02C7, 0x00B8, 0x0161, 0x015F, 0x0165, 0x017A, 0x02DD, 0x017E, 0x017C,
        0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7, 0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
        0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7, 0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
        0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7, 0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
        0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7, 0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9};
    std::string o;
    for (unsigned char c : s) {
        uint32_t u = c < 0xA0 ? c : hi[c - 0xA0];
        if (u < 0x80) o += (char)u;
        else if (u < 0x800) { o += (char)(0xC0 | u >> 6); o += (char)(0x80 | (u & 0x3F)); }
        else { o += (char)(0xE0 | u >> 12); o += (char)(0x80 | ((u >> 6) & 0x3F)); o += (char)(0x80 | (u & 0x3F)); }
    }
    return o;
}
std::string FromEightBit(const std::string& s) {
    int latin2 = 0, cp1250 = 0;
    for (unsigned char c : s) {
        if (c == 0xB1 || c == 0xB6 || c == 0xBC || c == 0xA1 || c == 0xA6 || c == 0xAC) latin2++;
        if (c == 0xB9 || c == 0x9C || c == 0x9F || c == 0xA5 || c == 0x8C || c == 0x8F) cp1250++;
    }
    return latin2 > cp1250 ? FromLatin2(s) : FromCp1250(s);
}

// "00:01:02,345" / "01:02.345" / "0:01:02.34" -> seconds; -1 if it isn't a time
double ParseTime(const std::string& t) {
    double parts[3] = {0, 0, 0};
    int n = 0;
    size_t i = 0;
    std::string cur;
    for (; i <= t.size(); i++) {
        char c = i < t.size() ? t[i] : ':';
        if (c == ':') {
            if (cur.empty() || n >= 3) return -1;
            for (auto& ch : cur) if (ch == ',') ch = '.';
            parts[n++] = atof(cur.c_str());
            cur.clear();
        } else if (isdigit((unsigned char)c) || c == '.' || c == ',') {
            cur += c;
        } else {
            return -1;
        }
    }
    if (n == 2) return parts[0] * 60 + parts[1];
    if (n == 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
    return -1;
}

// .srt / .vtt / .ass / .ssa
bool ParseSubtitleFile(const std::string& path, std::vector<SubEvent>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < (32u << 20)) s.append(buf, n);
    fclose(f);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s = s.substr(3);
    if (!ValidUtf8(s)) s = FromEightBit(s);
    std::vector<std::string> lines;
    {
        std::string l;
        for (char c : s) {
            if (c == '\n') { lines.push_back(l); l.clear(); }
            else if (c != '\r') l += c;
        }
        lines.push_back(l);
    }
    bool ass = s.find("[Events]") != std::string::npos;
    if (ass) {
        int textField = 9, startField = 1, endField = 2;
        for (auto& l : lines) {
            if (l.rfind("Format:", 0) == 0) {
                int k = 0;
                std::string tok;
                for (size_t i = 7; i <= l.size(); i++) {
                    if (i == l.size() || l[i] == ',') {
                        size_t a = tok.find_first_not_of(' ');
                        std::string name = a == std::string::npos ? "" : tok.substr(a);
                        if (name == "Start") startField = k;
                        if (name == "End") endField = k;
                        if (name == "Text") textField = k;
                        k++;
                        tok.clear();
                    } else {
                        tok += l[i];
                    }
                }
                continue;
            }
            if (l.rfind("Dialogue:", 0) != 0) continue;
            std::vector<std::string> fields;
            std::string tok;
            for (size_t i = 9; i < l.size(); i++) {
                if (l[i] == ',' && (int)fields.size() < textField) {
                    fields.push_back(tok);
                    tok.clear();
                } else {
                    tok += l[i];
                }
            }
            fields.push_back(tok);
            if ((int)fields.size() <= textField) continue;
            auto trim = [](std::string x) { size_t a = x.find_first_not_of(' '); return a == std::string::npos ? std::string() : x.substr(a); };
            SubEvent e;
            e.start = ParseTime(trim(fields[(size_t)startField]));
            e.end = ParseTime(trim(fields[(size_t)endField]));
            e.text = CleanSubText(fields[(size_t)textField]);
            if (e.start >= 0 && e.end > e.start && !e.text.empty()) out.push_back(e);
        }
    } else {  // SRT / WebVTT: a "start --> end" line, then text lines until an empty line
        for (size_t i = 0; i < lines.size(); i++) {
            size_t arrow = lines[i].find("-->");
            if (arrow == std::string::npos) continue;
            auto trim = [](std::string x) {
                size_t a = x.find_first_not_of(" \t"), b = x.find_last_not_of(" \t");
                return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
            };
            std::string a = trim(lines[i].substr(0, arrow)), b = trim(lines[i].substr(arrow + 3));
            size_t sp = b.find(' ');
            if (sp != std::string::npos) b = b.substr(0, sp);  // (WebVTT cue settings)
            SubEvent e;
            e.start = ParseTime(a);
            e.end = ParseTime(b);
            std::string text;
            for (i++; i < lines.size() && !trim(lines[i]).empty(); i++) text += (text.empty() ? "" : "\n") + lines[i];
            e.text = CleanSubText(text);
            if (e.start >= 0 && e.end > e.start && !e.text.empty()) out.push_back(e);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const SubEvent& x, const SubEvent& y) { return x.start < y.start; });
    return !out.empty();
}

}  // namespace
}  // namespace lsp

struct LspPlayer {
    std::unique_ptr<lsp::Source> src;
    LspInfo info{};
    int rate = 48000, channels = 2;
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool quit = false;
    // audio ring (device format) and the time of its first sample
    std::deque<float> audio;
    double audioPts = 0;
    bool audioPtsSet = false;
    lsp::Resampler rs;
    std::vector<float> tmp;
    // pictures
    std::deque<lsp::Frame> frames;
    lsp::Frame shown;
    bool newShown = false;
    // state
    std::atomic<int> state{LSP_PAUSED};
    bool eof = false, error = false;
    double seekTo = -1;
    // wall clock (no audio)
    double clockBase = 0, clockStart = 0;
    bool clockRunning = false;
    // tracks and subtitles
    std::vector<lsp::TrackInfo> tracks;              // the file's own (from the source)
    std::vector<std::pair<double, std::string>> chapters;  // (start s, title)
    std::vector<std::pair<lsp::TrackInfo, std::vector<lsp::SubEvent>>> external;  // subtitle files, ids from 1000
    std::vector<lsp::SubEvent> subs;                 // decoded from the file (the selected subtitle track)
    int audioId = -1, subId = -1;
    int wantAudio = -2, wantSub = -2;                // switch requests for the decode thread
    lsp::SubEvent shownSub;
    const lsp::SubEvent* lastSub = nullptr;
    int subSerial = 0;
    double subDelay = 0;
    // speed: WSOLA time stretch of the audio ring (the pitch stays; the clock runs at the speed - it counts the
    // input the stretch takes)
    double speed = 1.0;
    static constexpr int kWin = 1024, kHop = 512, kSearch = 240;  // frames (at 48 kHz: 21 ms window)
    std::vector<float> olaPrev;   // the second half of the last piece, fading out
    bool olaHave = false;
    double inPos = 0;             // where the next piece would start (frames into the ring), by the speed
    long prevNat = 0;             // where the last piece went on naturally (the next piece should look like it)
    std::vector<float> stretched; // made, not yet given out
    size_t stretchedAt = 0;

    void StretchReset() {
        olaHave = false;
        inPos = 0;
        prevNat = 0;
        stretched.clear();
        stretchedAt = 0;
    }

    // one more hop of stretched sound; false = not enough in the ring yet
    bool StretchStep() {
        const int ch = channels;
        long frames = (long)(audio.size() / (size_t)ch);
        long base = (long)inPos;
        if (base + kSearch + kWin > frames) return false;
        long best = base;
        if (olaHave) {  // the piece around where the speed says that best continues the last one
            double bestC = -1e30;
            long lo = std::max(0L, base - kSearch), hi = base + kSearch;
            for (long s = lo; s <= hi; s += 2) {
                double c = 0;
                for (int i = 0; i < kHop; i += 4) {
                    size_t ia = (size_t)(s + i) * ch, ib = (size_t)(prevNat + i) * ch;
                    float xa = 0, xb = 0;
                    for (int k = 0; k < ch; k++) {
                        xa += audio[ia + k];
                        xb += audio[ib + k];
                    }
                    c += (double)xa * xb;
                }
                if (c > bestC) {
                    bestC = c;
                    best = s;
                }
            }
        }
        if (olaPrev.size() != (size_t)kHop * ch) olaPrev.assign((size_t)kHop * ch, 0.0f);
        size_t o = stretched.size();
        stretched.resize(o + (size_t)kHop * ch);
        for (int i = 0; i < kHop; i++) {
            float w = (float)i / kHop;  // (a linear cross-fade: the two halves add up to one)
            for (int k = 0; k < ch; k++) {
                float x = audio[(size_t)(best + i) * ch + k];
                stretched[o + (size_t)i * ch + k] = olaHave ? olaPrev[(size_t)i * ch + k] * (1 - w) + x * w : x * w;
            }
        }
        for (int i = 0; i < kHop; i++)
            for (int k = 0; k < ch; k++) olaPrev[(size_t)i * ch + k] = audio[(size_t)(best + kHop + i) * ch + k];
        olaHave = true;
        prevNat = best + kHop;
        inPos += kHop * speed;
        // what's behind us goes (and the clock moves by it)
        long drop = std::min((long)inPos - kSearch, prevNat);
        if (drop > 0) {
            audio.erase(audio.begin(), audio.begin() + (ptrdiff_t)((size_t)drop * ch));
            audioPts += (double)drop / rate;
            inPos -= (double)drop;
            prevNat -= drop;
        }
        return true;
    }

    const lsp::SubEvent* ActiveSubLocked(double pos) {
        const std::vector<lsp::SubEvent>* list = nullptr;
        if (subId >= 1000) {
            for (auto& e : external)
                if (e.first.id == subId) list = &e.second;
        } else if (subId >= 0) {
            list = &subs;
        }
        if (!list || list->empty()) return nullptr;
        // last event that started before pos
        auto it = std::upper_bound(list->begin(), list->end(), pos, [](double t, const lsp::SubEvent& e) { return t < e.start; });
        while (it != list->begin()) {
            --it;
            const lsp::SubEvent& e = *it;
            double end = e.end;
            if (end < 0) {  // until the next one, but not forever
                auto nx = it + 1;
                end = std::min(e.start + (e.rgba.empty() ? 7.0 : 60.0), nx != list->end() ? nx->start : 1e18);
            }
            if (pos < end) return e.Empty() ? nullptr : &e;
            if (list == &subs || e.end < 0) break;  // (file subtitles may overlap: look a little further back)
            if (pos - e.start > 30) break;
        }
        return nullptr;
    }

    double PositionLocked() {
        if (info.has_audio && audioPtsSet) return audioPts;
        if (clockRunning) return clockBase + (lsp::NowSec() - clockStart) * speed;
        return clockBase;
    }

    void DecodeLoop() {
        lsp::Sink sink;
        sink.audio = [this](const float* pcm, int n, int r, int ch, double pts) {
            std::lock_guard<std::mutex> l(m);
            if (seekTo >= 0) return;
            if (!audioPtsSet || audio.empty()) {
                audioPts = pts;
                audioPtsSet = true;
            }
            tmp.clear();
            rs.Run(pcm, n, r, ch, rate, channels, tmp);
            audio.insert(audio.end(), tmp.begin(), tmp.end());
        };
        sink.video = [this](const LsvImage& im, double pts) {
            lsp::Frame f;
            f.w = im.width;
            f.h = im.height;
            f.rotation = im.rotation;
            f.pts = pts;
            int cw = (im.width + 1) / 2, ch = (im.height + 1) / 2;
            f.y.resize((size_t)im.width * im.height);
            f.uv.resize((size_t)cw * ch * 2);
            for (int r = 0; r < im.height; r++) memcpy(&f.y[(size_t)r * im.width], im.y + (size_t)r * im.y_stride, (size_t)im.width);
            for (int r = 0; r < ch; r++) {
                const uint8_t* u = im.u + (size_t)r * im.uv_stride;
                const uint8_t* v = im.v + (size_t)r * im.uv_stride;
                uint8_t* d = &f.uv[(size_t)r * cw * 2];
                if (im.uv_pixel_stride == 2 && im.v == im.u + 1) memcpy(d, u, (size_t)cw * 2);
                else
                    for (int c = 0; c < cw; c++) {
                        d[c * 2] = u[(size_t)c * im.uv_pixel_stride];
                        d[c * 2 + 1] = v[(size_t)c * im.uv_pixel_stride];
                    }
            }
            std::lock_guard<std::mutex> l(m);
            if (seekTo >= 0) return;
            frames.push_back(std::move(f));
        };
        sink.subtitle = [this](double start, double end, const std::string& text, const std::vector<uint8_t>& rgba, int w, int h, float x,
                               float y, float sw, float sh) {
            std::lock_guard<std::mutex> l(m);
            if (seekTo >= 0) return;
            lsp::SubEvent e;
            e.start = start;
            e.end = end > start ? end : -1;
            e.text = lsp::CleanSubText(text);
            e.rgba = rgba;
            e.w = w;
            e.h = h;
            e.x = x;
            e.y = y;
            e.sw = sw;
            e.sh = sh;
            for (auto it = subs.rbegin(); it != subs.rend() && it->start <= start; ++it)  // the open one before ends here
                if (it->end < 0) it->end = start;
            auto pos = std::upper_bound(subs.begin(), subs.end(), start, [](double t, const lsp::SubEvent& s) { return t < s.start; });
            subs.insert(pos, std::move(e));
            lastSub = nullptr;
            // forget what's long gone
            double now = PositionLocked();
            while (subs.size() > 50 && subs.front().start < now - 30) subs.erase(subs.begin());
        };
        bool needV = true, needA = true;
        for (;;) {
            {
                std::unique_lock<std::mutex> l(m);
                if (wantAudio != -2 || wantSub != -2) {  // another sound track / subtitles: switch, then start again here
                    int a = wantAudio, s = wantSub;
                    wantAudio = wantSub = -2;
                    double at = seekTo >= 0 ? seekTo : PositionLocked();
                    l.unlock();
                    if (a != -2) src->SelectAudio(a);
                    if (s != -2) src->SelectSubtitle(s);
                    l.lock();
                    for (auto& t : tracks)
                        if ((t.kind == LSP_TRACK_AUDIO && a != -2) || (t.kind == LSP_TRACK_SUBTITLE && s != -2))
                            t.selected = t.id == (t.kind == LSP_TRACK_AUDIO ? a : s);
                    if (a != -2) audioId = a;
                    subs.clear();
                    lastSub = nullptr;
                    seekTo = at;
                    audio.clear();
                    frames.clear();
                }
                // stay ~1 s ahead on audio and a few pictures ahead on video
                cv.wait_for(l, std::chrono::milliseconds(10), [&] {
                    if (quit || seekTo >= 0) return true;
                    if (eof || error) return false;
                    bool audioFull = !info.has_audio || audio.size() >= (size_t)rate * channels;
                    bool videoFull = !info.has_video || frames.size() >= 6;
                    return !(audioFull && videoFull);
                });
                if (quit) return;
                if (seekTo >= 0) {
                    double t = seekTo;
                    seekTo = -1;
                    audio.clear();
                    frames.clear();
                    audioPtsSet = false;
                    audioPts = t;
                    clockBase = t;
                    clockStart = lsp::NowSec();
                    eof = error = false;
                    rs.Reset();
                    subs.clear();
                    lastSub = nullptr;
                    l.unlock();
                    src->Seek(t);
                    continue;
                }
                if (eof || error) continue;
                bool audioFull = !info.has_audio || audio.size() >= (size_t)rate * channels;
                bool videoFull = !info.has_video || frames.size() >= 6;
                if (audioFull && videoFull) continue;
                needV = !videoFull;
                needA = !audioFull;
            }
            int r = src->StepNeeds(sink, needV, needA);
            if (r <= 0) {
                std::lock_guard<std::mutex> l(m);
                if (r == 0) eof = true;
                else error = true;
            }
        }
    }
};

namespace lsp {
namespace {

int ApiInit(const LspHost* h) {
    if (!h || h->api_version != LSP_API_VERSION) return 0;
    g_host = *h;
    std::string err;
    g_init = InitBackend(&g_host, &err);
    if (!g_init) Log(err);
    return g_init ? 1 : 0;
}

int ApiProbe(const char* path, LspInfo* info, LspImageFn thumb, void* user) {
    if (!g_init) return 0;
    auto src = CreateSource(&g_host);
    LspInfo inf{};
    std::string err;
    if (!src || !src->Open(path, &inf, false, &err)) return 0;
    if (info) *info = inf;
    if (thumb && inf.has_video) {
        bool got = false;
        Sink s;
        s.video = [&](const LsvImage& im, double) {
            if (!got) thumb(user, &im);
            got = true;
        };
        s.audio = [](const float*, int, int, int, double) {};
        if (inf.duration > 4) src->Seek(std::min(1.0, inf.duration * 0.1));  // skip a black first frame
        for (int i = 0; i < 400 && !got; i++)
            if (src->Step(s) <= 0) break;
    }
    return 1;
}

void InitTracks(LspPlayer* p) {
    p->tracks = p->src->Tracks();
    p->chapters = p->src->Chapters();
    for (auto& t : p->tracks)
        if (t.kind == LSP_TRACK_AUDIO && t.selected) p->audioId = t.id;
}

LspPlayer* ApiOpen(const char* path, int rate, int channels, LspInfo* info) {
    if (!g_init) return nullptr;
    auto* p = new LspPlayer();
    p->src = CreateSource(&g_host);
    p->rate = rate > 0 ? rate : 48000;
    p->channels = channels == 1 ? 1 : 2;
    std::string err;
    if (!p->src || !p->src->Open(path, &p->info, true, &err)) {
        Log("cannot open " + std::string(path) + ": " + err);
        delete p;
        return nullptr;
    }
    InitTracks(p);
    if (info) *info = p->info;
    p->th = std::thread([p] { p->DecodeLoop(); });
    return p;
}

LspPlayer* ApiOpenStream(const LspStream* s, int rate, int channels, LspInfo* info) {
    if (!g_init || !s || !s->read) return nullptr;
    auto* p = new LspPlayer();
    p->src = CreateSource(&g_host);
    p->rate = rate > 0 ? rate : 48000;
    p->channels = channels == 1 ? 1 : 2;
    std::string err;
    if (!p->src || !p->src->OpenStream(s, &p->info, true, &err)) {
        Log("cannot open the stream: " + err);
        delete p;
        return nullptr;
    }
    InitTracks(p);
    if (info) *info = p->info;
    p->th = std::thread([p] { p->DecodeLoop(); });
    return p;
}

// internet radio: FFmpeg reads it through the host (on Android its sound-only side: the phone's decoders can't take
// a stream we feed in pieces)
LspPlayer* ApiOpenUrl(const char* url, const char* format, const LspUrlIo* io, int rate, int channels, LspInfo* info) {
    if (!g_init || !url || !io) return nullptr;
    auto* p = new LspPlayer();
#if defined(__ANDROID__)
#ifdef LSP_FF_SIDE
    if (InitFf(nullptr)) p->src = CreateFfSource();
#endif
#else
    p->src = CreateSource(&g_host);
#endif
    p->rate = rate > 0 ? rate : 48000;
    p->channels = channels == 1 ? 1 : 2;
    std::string err;
    if (!p->src || !p->src->OpenUrl(url, format, io, &p->info, &err)) {
        Log("cannot play " + std::string(url) + ": " + (p->src ? err : std::string("no FFmpeg")));
        delete p;
        return nullptr;
    }
    InitTracks(p);
    if (info) *info = p->info;
    p->th = std::thread([p] { p->DecodeLoop(); });
    return p;
}

void ApiClose(LspPlayer* p) {
    if (!p) return;
    {
        std::lock_guard<std::mutex> l(p->m);
        p->quit = true;
    }
    p->cv.notify_all();
    if (p->th.joinable()) p->th.join();
    delete p;
}

void ApiSetPlaying(LspPlayer* p, int playing) {
    if (!p) return;
    std::lock_guard<std::mutex> l(p->m);
    int st = p->state;
    if (st == LSP_ERROR) return;
    if (playing) {
        if (st == LSP_ENDED) {  // play again from the start
            p->seekTo = 0;
            p->clockBase = 0;
            p->cv.notify_all();
        }
        if (!p->clockRunning) {
            p->clockStart = NowSec();
            p->clockRunning = true;
        }
        p->state = LSP_PLAYING;
    } else {
        if (p->clockRunning) p->clockBase += (NowSec() - p->clockStart) * p->speed;
        p->clockRunning = false;
        if (st == LSP_PLAYING) p->state = LSP_PAUSED;
    }
}

void ApiSeek(LspPlayer* p, double t) {
    if (!p) return;
    std::lock_guard<std::mutex> l(p->m);
    t = std::max(0.0, p->info.duration > 0 ? std::min(t, p->info.duration - 0.05) : t);
    p->seekTo = t;
    p->audio.clear();
    p->frames.clear();
    p->audioPts = t;
    p->audioPtsSet = false;
    p->StretchReset();
    p->clockBase = t;
    p->clockStart = NowSec();
    if (p->state == LSP_ENDED) p->state = LSP_PAUSED;
    p->cv.notify_all();
}

void ApiReadAudio(LspPlayer* p, float* out, int n) {
    size_t want = (size_t)n * (size_t)(p ? p->channels : 2);
    if (!p) {
        memset(out, 0, want * sizeof(float));
        return;
    }
    std::lock_guard<std::mutex> l(p->m);
    size_t got = 0;
    if (p->state == LSP_PLAYING && p->seekTo < 0 && p->speed != 1.0) {  // faster / slower, the pitch kept
        while (got < want) {
            if (p->stretchedAt >= p->stretched.size()) {
                p->stretched.clear();
                p->stretchedAt = 0;
                if (!p->StretchStep()) break;
            }
            size_t k = std::min(want - got, p->stretched.size() - p->stretchedAt);
            std::copy(p->stretched.begin() + (ptrdiff_t)p->stretchedAt, p->stretched.begin() + (ptrdiff_t)(p->stretchedAt + k), out + got);
            p->stretchedAt += k;
            got += k;
        }
        if (got < want && p->eof && (p->frames.empty() || !p->info.has_video)) p->state = LSP_ENDED;
    } else if (p->state == LSP_PLAYING && p->seekTo < 0) {
        got = std::min(want, p->audio.size());
        std::copy(p->audio.begin(), p->audio.begin() + (ptrdiff_t)got, out);
        p->audio.erase(p->audio.begin(), p->audio.begin() + (ptrdiff_t)got);
        p->audioPts += (double)(got / (size_t)p->channels) / p->rate;
        if (got < want && p->eof && (p->frames.empty() || !p->info.has_video)) p->state = LSP_ENDED;
    }
    if (got < want) memset(out + got, 0, (want - got) * sizeof(float));
    p->cv.notify_all();
}

int ApiVideoFrame(LspPlayer* p, LspImageFn fn, void* user) {
    if (!p) return 0;
    std::unique_lock<std::mutex> l(p->m);
    double pos = p->PositionLocked();
    bool changed = false;
    // newest picture that is due (drop late ones), or the first one after a seek
    while (!p->frames.empty() && (p->frames.front().pts <= pos + 0.005 || !p->shown.w)) {
        p->shown = std::move(p->frames.front());
        p->frames.pop_front();
        changed = true;
    }
    if (!p->info.has_audio && p->eof && p->frames.empty() && p->state == LSP_PLAYING) p->state = LSP_ENDED;
    if (p->info.duration > 0 && !p->info.has_audio && pos >= p->info.duration && p->state == LSP_PLAYING) p->state = LSP_ENDED;
    p->cv.notify_all();
    if (!changed || !fn) return 0;
    lsp::Frame& f = p->shown;
    LsvImage im{f.w, f.h, f.y.data(), f.uv.data(), f.uv.data() + 1, f.w, ((f.w + 1) / 2) * 2, 2, f.rotation, 0};
    fn(user, &im);
    return 1;
}

double ApiPosition(LspPlayer* p) {
    if (!p) return 0;
    std::lock_guard<std::mutex> l(p->m);
    return p->PositionLocked();
}

int ApiState(LspPlayer* p) {
    if (!p) return LSP_ERROR;
    std::lock_guard<std::mutex> l(p->m);
    if (p->error && p->audio.empty() && p->frames.empty()) return LSP_ERROR;
    return p->state;
}

static void CopyStr(char* dst, size_t n, const std::string& s) {
    snprintf(dst, n, "%s", s.c_str());
}

int ApiChapters(LspPlayer* p, LspChapter* out, int max) {
    if (!p) return 0;
    std::lock_guard<std::mutex> l(p->m);
    int n = 0;
    for (auto& [start, title] : p->chapters) {
        if (n >= max) break;
        out[n].start = start;
        snprintf(out[n].title, sizeof(out[n].title), "%s", title.c_str());
        n++;
    }
    return n;
}

int ApiTracks(LspPlayer* p, LspTrack* out, int max) {
    if (!p) return 0;
    std::lock_guard<std::mutex> l(p->m);
    int n = 0;
    auto put = [&](const TrackInfo& t, bool ext) {
        if (n >= max) return;
        LspTrack& o = out[n++];
        memset(&o, 0, sizeof(o));
        o.kind = t.kind;
        o.id = t.id;
        o.selected = t.kind == LSP_TRACK_AUDIO ? t.id == p->audioId : t.id == p->subId;
        o.playable = t.playable;
        o.channels = t.channels;
        o.external = ext;
        CopyStr(o.lang, sizeof(o.lang), t.lang);
        CopyStr(o.title, sizeof(o.title), t.title);
        CopyStr(o.codec, sizeof(o.codec), t.codec);
    };
    for (auto& t : p->tracks) put(t, false);
    for (auto& e : p->external) put(e.first, true);
    return n;
}

int ApiSelectTrack(LspPlayer* p, int kind, int id) {
    if (!p) return 0;
    std::lock_guard<std::mutex> l(p->m);
    if (kind == LSP_TRACK_AUDIO) {
        if (id == p->audioId) return 1;
        p->wantAudio = id;
    } else {
        int before = p->subId;
        p->subId = id;
        p->lastSub = nullptr;
        bool embeddedBefore = before >= 0 && before < 1000, embeddedNow = id >= 0 && id < 1000;
        if (embeddedNow && id != before) p->wantSub = id;              // decode it from here on
        else if (!embeddedNow && embeddedBefore) p->wantSub = -1;      // stop decoding the old one
    }
    p->cv.notify_all();
    return 1;
}

int ApiAddSubtitles(LspPlayer* p, const char* path) {
    if (!p || !path) return -1;
    std::vector<SubEvent> ev;
    if (!ParseSubtitleFile(path, ev)) return -1;
    std::lock_guard<std::mutex> l(p->m);
    TrackInfo t;
    t.kind = LSP_TRACK_SUBTITLE;
    t.id = 1000 + (int)p->external.size();
    std::string name = path;
    size_t k = name.find_last_of("/\\");
    t.title = k == std::string::npos ? name : name.substr(k + 1);
    size_t dot = t.title.find_last_of('.');
    t.codec = dot == std::string::npos ? "" : t.title.substr(dot + 1);
    // movie.pl.srt / movie.eng.forced.srt: the language from the name
    {
        std::string stem = dot == std::string::npos ? t.title : t.title.substr(0, dot);
        size_t a = 0;
        while ((a = stem.find('.', a)) != std::string::npos) {
            size_t b = stem.find('.', a + 1);
            std::string tok = stem.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            for (auto& ch : tok) ch = (char)tolower((unsigned char)ch);
            if ((tok.size() == 2 || tok.size() == 3) && std::all_of(tok.begin(), tok.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; }))
                t.lang = tok;
            a++;
        }
    }
    p->external.push_back({t, std::move(ev)});
    return t.id;
}

int ApiSubtitle(LspPlayer* p, LspSubtitle* out) {
    if (!p) return 0;
    std::lock_guard<std::mutex> l(p->m);
    const SubEvent* e = p->ActiveSubLocked(p->PositionLocked() - p->subDelay);
    if (e != p->lastSub || (!e && !p->shownSub.Empty())) {
        p->lastSub = e;
        p->shownSub = e ? *e : SubEvent();
        p->subSerial++;
    }
    if (out) {
        memset(out, 0, sizeof(*out));
        out->text = p->shownSub.text.c_str();
        out->rgba = p->shownSub.rgba.empty() ? nullptr : p->shownSub.rgba.data();
        out->w = p->shownSub.w;
        out->h = p->shownSub.h;
        out->x = p->shownSub.x;
        out->y = p->shownSub.y;
        out->sw = p->shownSub.sw;
        out->sh = p->shownSub.sh;
    }
    return p->subSerial;
}

int ApiTags(const char* path, LspTags* out, LspCoverFn cover, void* user) {
    if (!path || !out) return 0;
#if defined(LSP_FF_SIDE) || !defined(__ANDROID__)
    return ReadTags(path, out, cover, user) ? 1 : 0;
#else
    (void)cover;
    (void)user;
    return 0;
#endif
}

void ApiSetSpeed(LspPlayer* p, double speed) {
    if (!p) return;
    std::lock_guard<std::mutex> l(p->m);
    speed = std::clamp(speed, 0.25, 4.0);
    if (std::fabs(speed - 1.0) < 0.001) speed = 1.0;
    if (speed == p->speed) return;
    if (p->clockRunning) {  // (files without sound: the wall clock - up to now at the old speed)
        p->clockBase += (NowSec() - p->clockStart) * p->speed;
        p->clockStart = NowSec();
    }
    p->speed = speed;
    p->StretchReset();
}

void ApiSetSubDelay(LspPlayer* p, double s) {
    if (!p) return;
    std::lock_guard<std::mutex> l(p->m);
    p->subDelay = std::clamp(s, -600.0, 600.0);
}

const LspModule kModule = {LSP_API_VERSION, "LunaShare player", "1.5", ApiInit, ApiProbe, ApiOpen, ApiClose, ApiSetPlaying,
                           ApiSeek, ApiReadAudio, ApiVideoFrame, ApiPosition, ApiState, ApiOpenStream,
                           ApiTracks, ApiSelectTrack, ApiAddSubtitles, ApiSubtitle, ApiTags, ApiSetSpeed, ApiSetSubDelay, ApiChapters, ApiOpenUrl};

}  // namespace
}  // namespace lsp

extern "C" LSP_EXPORT const LspModule* lunashare_player_module(void) { return &lsp::kModule; }
