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
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>
#else
#include <windows.h>
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

char g_crashPath[1024] = {0};  // (read in the crash handler: a plain buffer, written before any crash can happen)

void SetDir(const std::string& dir) {
    if (!dir.empty() && dir.size() + 24 < sizeof(g_crashPath))
        snprintf(g_crashPath, sizeof(g_crashPath), "%s/lunashare-crash.txt", dir.c_str());
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

// ------------------------------------------------------------------------------------------------ crash reports

namespace {
void CrashWrite(const std::string& text) {
    if (!g_crashPath[0]) return;
    FILE* f = fopen(g_crashPath, "ab");
    if (!f) return;
    fputs(text.c_str(), f);
    fclose(f);
}

#if !defined(_WIN32)
struct sigaction g_oldAct[NSIG];

struct Bt {
    uintptr_t* pcs;
    int n, max;
};
_Unwind_Reason_Code BtStep(_Unwind_Context* c, void* arg) {
    Bt* b = (Bt*)arg;
    uintptr_t pc = _Unwind_GetIP(c);
    if (pc) {
        if (b->n >= b->max) return _URC_END_OF_STACK;
        b->pcs[b->n++] = pc;
    }
    return _URC_NO_REASON;
}

void WriteAll(int fd, const char* s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

void WritePc(int fd, int i, uintptr_t pc) {
    char b[600];
    Dl_info di;
    if (dladdr((void*)pc, &di) && di.dli_fname) {
        const char* f = strrchr(di.dli_fname, '/');
        f = f ? f + 1 : di.dli_fname;
        snprintf(b, sizeof(b), "  #%02d %s+0x%lx%s%s%s\n", i, f, (unsigned long)(pc - (uintptr_t)di.dli_fbase),
                 di.dli_sname ? " (" : "", di.dli_sname ? di.dli_sname : "", di.dli_sname ? ")" : "");
    } else {
        snprintf(b, sizeof(b), "  #%02d 0x%lx\n", i, (unsigned long)pc);
    }
    WriteAll(fd, b);
}

const char* SigName(int s) {
    switch (s) {
        case SIGSEGV: return "SIGSEGV (bad memory access)";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        case SIGABRT: return "SIGABRT (abort)";
        default: return "?";
    }
}

void OnSignal(int sig, siginfo_t* si, void* ucv) {
    int fd = g_crashPath[0] ? open(g_crashPath, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
    if (fd >= 0) {
        char b[400];
        char thread[32] = {0};
        prctl(PR_GET_NAME, thread, 0, 0, 0);
        snprintf(b, sizeof(b), "\n==== LunaShare " LUNASHARE_VERSION " crashed (time %ld): signal %d %s, address %p, thread \"%s\"\n",
                 (long)time(nullptr), sig, SigName(sig), si ? si->si_addr : nullptr, thread);
        WriteAll(fd, b);
        uintptr_t pc = 0;
        ucontext_t* uc = (ucontext_t*)ucv;
        if (uc) {
#if defined(__aarch64__)
            pc = (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__arm__)
            pc = (uintptr_t)uc->uc_mcontext.arm_pc;
#elif defined(__x86_64__)
            pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__i386__)
            pc = (uintptr_t)uc->uc_mcontext.gregs[REG_EIP];
#endif
        }
        if (pc) {
            WriteAll(fd, "where:\n");
            WritePc(fd, 0, pc);
        }
        // A jump to a garbage address (a corrupted function pointer or return address) stops the unwinder right
        // there - so also: who called (the link register), and the return addresses still on the stack.
        uintptr_t sp = 0, lr = 0;
        if (uc) {
#if defined(__aarch64__)
            lr = (uintptr_t)uc->uc_mcontext.regs[30];
            sp = (uintptr_t)uc->uc_mcontext.sp;
            WriteAll(fd, "registers:\n");
            for (int i = 0; i < 31; i += 4) {
                char r[200];
                int k = 0;
                for (int j = i; j < i + 4 && j < 31; j++)
                    k += snprintf(r + k, sizeof(r) - k, " x%-2d %016lx", j, (unsigned long)uc->uc_mcontext.regs[j]);
                snprintf(r + k, sizeof(r) - k, "\n");
                WriteAll(fd, r);
            }
#elif defined(__arm__)
            lr = (uintptr_t)uc->uc_mcontext.arm_lr;
            sp = (uintptr_t)uc->uc_mcontext.arm_sp;
#elif defined(__x86_64__)
            sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
            lr = sp ? *(uintptr_t*)sp : 0;  // (x86: the return address is the top of the stack when a call went wild)
#endif
        }
        if (lr) {
            WriteAll(fd, "called from (lr):\n");
            WritePc(fd, 0, lr);
        }
        if (sp) {
            WriteAll(fd, "return addresses on the stack (most recent first; some may be stale):\n");
            const uintptr_t* st = (const uintptr_t*)sp;
            int found = 0;
            for (int i = 0; i < 768 && found < 40; i++) {
                uintptr_t v = st[i];
                Dl_info di;
                if (v < 0x10000 || !dladdr((void*)v, &di) || !di.dli_fname) continue;
                if (!strstr(di.dli_fname, "lunashare") && !strstr(di.dli_fname, "libmain") && !strstr(di.dli_fname, "libSDL2") &&
                    !strstr(di.dli_fname, "libc++"))
                    continue;  // (our own code is what matters here)
                char pre[48];
                snprintf(pre, sizeof(pre), "  [sp+0x%x]", i * (int)sizeof(uintptr_t));
                WriteAll(fd, pre);
                WritePc(fd, found++, v);
            }
        }
        uintptr_t pcs[48];
        Bt bt{pcs, 0, 48};
        _Unwind_Backtrace(BtStep, &bt);
        WriteAll(fd, "backtrace:\n");
        for (int i = 0; i < bt.n; i++) WritePc(fd, i, pcs[i]);
        close(fd);
    }
    // the system's own handling, as before (Android: its crash dialog and report)
    sigaction(sig, &g_oldAct[sig], nullptr);
    raise(sig);
}
#else
LONG WINAPI OnException(EXCEPTION_POINTERS* ep) {
    char b[600];
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod))
        GetModuleFileNameA(mod, name, sizeof(name));
    const char* f = strrchr(name, '\\');
    snprintf(b, sizeof(b), "\n==== LunaShare " LUNASHARE_VERSION " crashed (time %ld): exception 0x%08lx at %s+0x%llx\n", (long)time(nullptr),
             (unsigned long)ep->ExceptionRecord->ExceptionCode, f ? f + 1 : name,
             (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
    CrashWrite(b);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
}  // namespace

void CrashReportsInit() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
#if !defined(_WIN32)
    static char altStack[64 * 1024];  // (a crash from running out of stack still gets reported)
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = OnSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(s, &sa, &g_oldAct[s]);
#else
    SetUnhandledExceptionFilter(OnException);
#endif
}

void CrashText(const std::string& text) {
    CrashWrite("\n==== LunaShare " LUNASHARE_VERSION " crashed (time " + std::to_string((long)time(nullptr)) + "): " + text + "\n");
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
