#include "applog.h"

#include <SDL.h>

#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace applog {
namespace {
std::mutex g_m;
std::deque<std::string> g_lines;
size_t g_bytes = 0;
constexpr size_t kMaxBytes = 300 * 1024;  // (fits a clipboard and a chat message)
SDL_LogOutputFunction g_prev = nullptr;
void* g_prevData = nullptr;
std::atomic<bool> g_init{false};
FILE* g_file = nullptr;  // (g_m)
std::string g_path;
int64_t g_fileBytes = 0;

std::string Stamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    snprintf(b, sizeof(b), "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return b;
}

void SdlOut(void* ud, int category, SDL_LogPriority prio, const char* msg) {
    Add(msg ? msg : "");
    if (g_prev) g_prev(g_prevData, category, prio, msg);
}

#if !defined(_WIN32)
// the engine writes to stderr: read through a pipe, a line at a time (and passed on to where stderr went)
void CaptureStderr() {
    int fds[2];
    if (pipe(fds) != 0) return;
    int orig = dup(2);
    if (dup2(fds[1], 2) < 0) {
        close(fds[0]);
        close(fds[1]);
        if (orig >= 0) close(orig);
        return;
    }
    close(fds[1]);
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread([rd = fds[0], orig] {
        std::string cur;
        char buf[1024];
        for (;;) {
            ssize_t n = read(rd, buf, sizeof(buf));
            if (n <= 0) break;
            if (orig >= 0) {
                ssize_t w = write(orig, buf, (size_t)n);
                (void)w;
            }
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    if (!cur.empty()) {
#if defined(__ANDROID__)
                        __android_log_write(ANDROID_LOG_INFO, "LunaShare", cur.c_str());
#endif
                        Add(cur);
                    }
                    cur.clear();
                } else if (cur.size() < 2000) {
                    cur += buf[i];
                }
            }
        }
    }).detach();
}
#endif
}  // namespace

bool On() {
#if defined(LUNASHARE_DIAG)
    return true;
#else
    return false;
#endif
}

void SetDir(const std::string& dir) {
    if (!On() || dir.empty()) return;
    std::string path = dir + "/lunashare-log.txt";
    std::string text;
    {
        std::lock_guard<std::mutex> g(g_m);
        if (path == g_path) return;
        if (g_file) fclose(g_file);
        g_path = path;
        g_file = fopen(path.c_str(), "ab");
        g_fileBytes = 0;
        if (!g_file) return;
        fseek(g_file, 0, SEEK_END);
        g_fileBytes = (int64_t)ftell(g_file);
        for (auto& l : g_lines) text += l + "\n";  // (what came before the folder was known)
        fputs("\n======== LunaShare " LUNASHARE_VERSION " ========\n", g_file);
        fputs(text.c_str(), g_file);
        fflush(g_file);
    }
}

void Init() {
    if (!On()) return;
    if (g_init.exchange(true)) return;
    SDL_LogGetOutputFunction(&g_prev, &g_prevData);
    SDL_LogSetOutputFunction(SdlOut, nullptr);
#if !defined(_WIN32)
    CaptureStderr();
#endif
    Add(std::string("LunaShare ") + LUNASHARE_VERSION + " - log started");
}

void Add(const std::string& line) {
    if (!On()) return;
    std::string l = Stamp() + line;
    std::lock_guard<std::mutex> g(g_m);
    if (g_file) {
        fputs(l.c_str(), g_file);
        fputc('\n', g_file);
        fflush(g_file);
        g_fileBytes += (int64_t)l.size() + 1;
        if (g_fileBytes > 8 * 1024 * 1024) {  // (a long one: the older half goes to lunashare-log.old.txt)
            fclose(g_file);
            std::string old = g_path.substr(0, g_path.size() - 4) + ".old.txt";
            remove(old.c_str());
            rename(g_path.c_str(), old.c_str());
            g_file = fopen(g_path.c_str(), "ab");
            g_fileBytes = 0;
        }
    }
    g_lines.push_back(l);
    g_bytes += l.size() + 1;
    while (g_bytes > kMaxBytes && !g_lines.empty()) {
        g_bytes -= g_lines.front().size() + 1;
        g_lines.pop_front();
    }
}

std::string Text() {
    std::lock_guard<std::mutex> g(g_m);
    std::string out;
    out.reserve(g_bytes);
    for (auto& l : g_lines) {
        out += l;
        out += '\n';
    }
    return out;
}
}  // namespace applog
