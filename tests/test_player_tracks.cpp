// Player module test for sound tracks and subtitles (needs FFmpeg: LUNASHARE_FFMPEG_LIB=<dir>):
//   test_player_tracks <file> [subtitle file]
// Lists the tracks, plays a few seconds of every sound track (each must be heard), turns on the file's first
// subtitle track and the extra subtitle file and checks that text shows up at the right time.
// Build: c++ -std=c++17 -Isrc/media_api tests/test_player_tracks.cpp -Lbuild-app -llunashare_player -o /tmp/tpt
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "lunashare_player.h"

static int failures = 0;
#define CHECK(c, ...)                        \
    do {                                     \
        bool ok_ = (c);                      \
        printf(ok_ ? "  ok: " : "  FAIL: "); \
        printf(__VA_ARGS__);                 \
        printf("\n");                        \
        if (!ok_) failures++;                \
    } while (0)

static const int kRate = 48000, kCh = 2, kBlock = 480;

// plays `secs` from `from`; returns the loudness (rms) and the subtitle texts seen
static double Play(const LspModule* m, LspPlayer* p, double from, double secs, std::vector<std::string>* texts) {
    m->seek(p, from);
    m->set_playing(p, 1);
    std::vector<float> buf(kBlock * kCh);
    double e = 0;
    int n = 0, last = -1;
    for (int i = 0; i < (int)(secs * 100); i++) {
        m->read_audio(p, buf.data(), kBlock);
        if (i > 20)
            for (float v : buf) e += v * v, n++;
        m->video_frame(p, nullptr, nullptr);
        LspSubtitle s;
        int k = m->subtitle(p, &s);
        if (texts && k != last && ((s.text && *s.text) || s.rgba)) {
            char at[32];
            snprintf(at, sizeof(at), "@%.1f ", m->position(p));
            texts->push_back(at + (s.rgba ? std::string("[picture]") : std::string(s.text)));
        }
        last = k;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    m->set_playing(p, 0);
    return n ? std::sqrt(e / n) : 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);
    const LspModule* m = lunashare_player_module();
    LspHost host{LSP_API_VERSION, nullptr, [](void*, const char* s) { printf("    [%s]\n", s); }, nullptr, nullptr};
    CHECK(m->api_version == LSP_API_VERSION && m->init(&host) == 1, "init");
    LspInfo info{};
    LspPlayer* p = m->open(argv[1], kRate, kCh, &info);
    CHECK(p != nullptr, "open %s", argv[1]);
    if (!p) return 1;
    LspTrack t[32];
    int n = m->tracks(p, t, 32);
    for (int i = 0; i < n; i++)
        printf("    track %d: %s id %d lang '%s' title '%s' %s %dch%s%s\n", i, t[i].kind == LSP_TRACK_AUDIO ? "sound" : "subtitles", t[i].id,
               t[i].lang, t[i].title, t[i].codec, t[i].channels, t[i].selected ? " [selected]" : "", t[i].playable ? "" : " (can't decode)");
    int audio = 0, subs = -1;
    for (int i = 0; i < n; i++) {
        if (t[i].kind == LSP_TRACK_SUBTITLE && subs < 0) subs = t[i].id;
        if (t[i].kind != LSP_TRACK_AUDIO) continue;
        audio++;
        m->select_track(p, LSP_TRACK_AUDIO, t[i].id);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        double rms = Play(m, p, 3.0, 2.0, nullptr);
        CHECK(rms > 0.02, "sound track %d (%s, %d channels) is heard: rms %.3f", t[i].id, t[i].lang, t[i].channels, rms);
    }
    CHECK(audio > 0, "%d sound tracks", audio);
    if (subs >= 0) {
        m->select_track(p, LSP_TRACK_SUBTITLE, subs);
        std::vector<std::string> seen;
        Play(m, p, 0.5, 5.0, &seen);
        for (auto& s : seen) printf("    subtitle: \"%s\"\n", s.c_str());
        CHECK(!seen.empty(), "the file's subtitles show up");
    }
    if (argc > 2) {
        int id = m->add_subtitles(p, argv[2]);
        CHECK(id >= 1000, "subtitle file read (%d)", id);
        m->select_track(p, LSP_TRACK_SUBTITLE, id);
        std::vector<std::string> seen;
        Play(m, p, 0.5, 5.0, &seen);
        for (auto& s : seen) printf("    subtitle: \"%s\"\n", s.c_str());
        CHECK(!seen.empty(), "the subtitle file shows up");
    }
    m->select_track(p, LSP_TRACK_SUBTITLE, -1);
    std::vector<std::string> none;
    Play(m, p, 0.5, 4.0, &none);
    CHECK(none.empty(), "subtitles off");
    m->close(p);
    printf(failures ? "\n%d FAILED\n" : "\nALL TRACK TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
