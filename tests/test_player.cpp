// Player module test (needs FFmpeg libraries: LUNASHARE_FFMPEG_LIB=<dir>):
//   test_player <file> [expect_video 0/1] [expect_audio 0/1]
// Plays the file faster than real time by pulling audio like a sound card would, takes pictures as they fall due,
// then checks the length, A/V timing, seeking and the end state.
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lunashare_player.h"

static int failures = 0;
#define CHECK(c, ...)                              \
    do {                                           \
        bool ok_ = (c); /* evaluate once */        \
        printf(ok_ ? "  ok: " : "  FAIL: ");       \
        printf(__VA_ARGS__);                       \
        printf("\n");                              \
        if (!ok_) failures++;                      \
    } while (0)

struct Pics {
    int count = 0, w = 0, h = 0, rotation = -1;
    double lastPos = -1, maxLag = 0;
};

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const LspModule* m = lunashare_player_module();
    LspHost host{LSP_API_VERSION, nullptr, [](void*, const char* s) { printf("    [%s]\n", s); }, nullptr, nullptr};
    CHECK(m->init(&host) == 1, "init (FFmpeg loaded)");
    bool wantV = argc > 2 ? atoi(argv[2]) != 0 : true, wantA = argc > 3 ? atoi(argv[3]) != 0 : true;

    LspInfo info{};
    Pics thumb;
    CHECK(m->probe(argv[1], &info, [](void* u, const LsvImage* im) {
              auto* t = (Pics*)u;
              t->count++;
              t->w = im->width;
              t->h = im->height;
          }, &thumb) == 1, "probe");
    printf("    %.2f s, video %d (%dx%d %s), audio %d (%s)\n", info.duration, info.has_video, info.width, info.height,
           info.video_codec, info.has_audio, info.audio_codec);
    CHECK(info.has_video == wantV && info.has_audio == wantA, "streams found as expected");
    if (wantV) CHECK(thumb.count == 1 && thumb.w > 0, "thumbnail picture %dx%d (%d)", thumb.w, thumb.h, thumb.count);

    const int rate = 48000, ch = 2, block = 480;  // 10 ms
    LspPlayer* p = m->open(argv[1], rate, ch, &info);
    CHECK(p != nullptr, "open");
    if (!p) return 1;
    m->set_playing(p, 1);
    std::vector<float> buf(block * ch);
    Pics pics;
    double audioSec = 0, energy = 0;
    // with audio the clock follows what we pull; without, it's wall clock - so give it real time
    for (int i = 0; i < 200000 && m->state(p) == LSP_PLAYING; i++) {
        m->read_audio(p, buf.data(), block);
        for (float v : buf) energy += v * v;
        if (info.has_audio) audioSec += (double)block / rate;
        double pos = m->position(p);
        m->video_frame(p, [](void* u, const LsvImage* im) {
            auto* t = (Pics*)u;
            t->count++;
            t->w = im->width;
            t->h = im->height;
            t->rotation = im->rotation;
        }, &pics);
        (void)pos;
        // let the decoder keep up (it buffers ~1 s ahead)
        if (i % 4 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(info.has_audio ? 2 : 10));
        }
    }
    double end = m->position(p);
    printf("    played to %.2f s, %d pictures, audio rms %.4f\n", end, pics.count, std::sqrt(energy / std::max(1.0, audioSec * rate * ch)));
    CHECK(m->state(p) == LSP_ENDED, "reached the end (state %d)", m->state(p));
    if (info.duration > 0) CHECK(std::fabs(end - info.duration) < 0.5, "position at the end matches the length (%.2f vs %.2f)", end, info.duration);
    if (wantV) CHECK(pics.count > info.duration * 10, "pictures shown along the way (%d)", pics.count);
    if (wantA) CHECK(energy > 0, "audio is not silent");

    // seek to the middle and play a bit
    m->seek(p, info.duration * 0.5);
    m->set_playing(p, 1);
    for (int i = 0; i < 50; i++) {
        m->read_audio(p, buf.data(), block);
        m->video_frame(p, nullptr, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    double pos = m->position(p);
    CHECK(pos > info.duration * 0.45 && pos < info.duration * 0.5 + 1.0, "seek to the middle (%.2f s)", pos);
    m->close(p);
    printf(failures ? "\n%d PLAYER TEST(S) FAILED\n" : "\nALL PLAYER TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
