// Video module core: packets, keyframe recovery after loss, bitrate adaptation under congestion.
// Uses the test backend (synthetic camera + trivial codec) and loops the module's packets back into itself.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "lunashare_video.h"

static int failures = 0;
#define CHECK(c, ...)                         \
    do {                                      \
        if (!(c)) {                           \
            printf("  FAIL: " __VA_ARGS__);   \
            printf("\n");                     \
            failures++;                       \
        } else {                              \
            printf("  ok: " __VA_ARGS__);     \
            printf("\n");                     \
        }                                     \
    } while (0)

struct Link {
    std::mutex m;
    std::deque<std::vector<uint8_t>> q;
    size_t bytes = 0;
    double lossRate = 0;
    int kbps = 0;  // 0 = unlimited
    size_t refuseAbove = 768 * 1024;
    std::mt19937 rng{42};
    std::atomic<int> local{0}, remote{0}, keyAsks{0}, stateOn{-1};
    std::atomic<int> remoteW{0}, remoteH{0}, remoteRot{-1}, localMirror{-1}, remoteMirror{-1}, lastCodec{-1};
};
static Link L;

static int Send(void*, const uint8_t* d, size_t n) {
    std::lock_guard<std::mutex> l(L.m);
    if (n >= 2 && d[1] == 2) L.keyAsks++;
    if (n > 13 && d[1] == 1) L.lastCodec = d[12];
    if (L.bytes > L.refuseAbove && n > 2 && d[1] == 1) return 0;
    L.q.emplace_back(d, d + n);
    L.bytes += n;
    return 1;
}
static void Image(void*, int src, const LsvImage* img) {
    if (src == LSV_LOCAL) {
        L.local++;
        L.localMirror = img->mirror;
    } else {
        L.remote++;
        L.remoteW = img->width;
        L.remoteH = img->height;
        L.remoteRot = img->rotation;
        L.remoteMirror = img->mirror;
    }
}
static void State(void*, int on) { L.stateOn = on; }
static size_t Queued(void*) {
    std::lock_guard<std::mutex> l(L.m);
    return L.bytes;
}
static void Log(void*, const char* m) { printf("    [%s]\n", m); }

int main() {
#ifdef _WIN32
    _putenv_s("LSV_FAKE_CODECS", "3");  // the test camera claims H.264 + HEVC
#else
    setenv("LSV_FAKE_CODECS", "3", 1);
#endif
    const LsvModule* m = lunashare_video_module();
    static LsvHost host{LSV_API_VERSION, nullptr, Send, Image, State, Queued, Log, nullptr, nullptr};
    CHECK(m->init(&host) == 1, "init");
    LsvCamera cams[8];
    int nc = m->list_cameras(cams, 8);
    CHECK(nc == 2, "lists cameras (%d)", nc);

    std::atomic<bool> run{true};
    // the "network": delivers packets back into the module, with optional loss and a bandwidth limit
    std::thread net([&] {
        auto last = std::chrono::steady_clock::now();
        double credit = 0;
        while (run) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            credit += L.kbps ? dt * L.kbps * 1000 / 8 : 1e12;
            if (credit > 1e12) credit = 1e12;
            for (;;) {
                std::vector<uint8_t> p;
                {
                    std::lock_guard<std::mutex> l(L.m);
                    if (L.q.empty()) { if (L.kbps) credit = std::min(credit, 2000.0); break; }
                    if ((double)L.q.front().size() > credit) break;
                    p = std::move(L.q.front());
                    L.q.pop_front();
                    L.bytes -= p.size();
                    credit -= (double)p.size();
                    if (p.size() > 2 && p[1] == 1 && std::uniform_real_distribution<double>(0, 1)(L.rng) < L.lossRate) continue;
                }
                m->receive(p.data(), p.size());
            }
        }
    });

    printf("[video: loopback]\n");
    LsvSettings s{640, 480, 20, 1000000, 1, 0};
    CHECK(m->start(&s) == 1, "start");
    CHECK(m->set_camera_on(1) == 1, "camera on");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(L.local > 10, "local preview images (%d)", L.local.load());
    CHECK(L.remote > 15, "remote images decoded (%d)", L.remote.load());
    CHECK(L.remoteW == 640 && L.remoteH == 480, "remote size %dx%d", L.remoteW.load(), L.remoteH.load());
    CHECK(L.remoteRot == 0 && L.remoteMirror == 0 && L.localMirror == 1, "only the self-view is mirrored");
    CHECK(L.stateOn == 1, "peer told camera is on");
    CHECK(L.lastCodec == 1, "both sides can do HEVC, so HEVC is used (codec %d)", L.lastCodec.load());
    LsvControl ctl[16];
    int n = m->get_controls(ctl, 16);
    CHECK(n == 1 && ctl[0].id == LSV_CTL_ZOOM, "controls");

    printf("[video: 10%% packet loss]\n");
    L.lossRate = 0.10;
    int before = L.remote, asks = L.keyAsks;
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    int got = L.remote - before;
    CHECK(L.keyAsks > asks, "keyframes requested after loss (%d)", L.keyAsks - asks);
    CHECK(got > 15, "video keeps going (%d frames in 3 s)", got);
    L.lossRate = 0;

    printf("[video: congested link]\n");
    L.kbps = 400;
    std::this_thread::sleep_for(std::chrono::milliseconds(6000));
    LsvStats st;
    m->get_stats(&st);
    printf("    sending %d kbps, %d fps, queued %zu bytes\n", st.send_kbps, st.send_fps, Queued(nullptr));
    CHECK(st.send_kbps < 450, "bitrate adapted down (%d kbps)", st.send_kbps);
    int congestedKbps = st.send_kbps;
    CHECK(Queued(nullptr) < 64 * 1024, "queue stays short (%zu bytes, under ~1 s of delay)", Queued(nullptr));
    before = L.remote;
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    CHECK(L.remote - before > 5, "still receiving pictures (%d in 2 s)", L.remote - before);

    printf("[video: link recovers]\n");
    L.kbps = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(9000));
    m->get_stats(&st);
    CHECK(st.send_kbps > congestedKbps * 3 / 2, "bitrate climbs back (%d kbps, was %d)", st.send_kbps, congestedKbps);

    printf("[video: receiver asks for a smaller picture]\n");
    {
        uint8_t lim[6] = {2, 4, 0x40, 0x01, 0xF0, 0x00};  // at most 320x240
        m->receive(lim, sizeof(lim));
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        CHECK(L.remoteW == 320 && L.remoteH == 240, "sender stepped down to %dx%d", L.remoteW.load(), L.remoteH.load());
    }

    printf("[video: camera off]\n");
    CHECK(m->set_camera_on(0) == 1, "camera off");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(L.stateOn == 0, "peer told camera is off");
    m->stop();
    run = false;
    net.join();

    printf(failures ? "\n%d VIDEO TEST(S) FAILED\n" : "\nALL VIDEO TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
