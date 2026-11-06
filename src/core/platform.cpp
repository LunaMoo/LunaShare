#include "platform.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windows.h>
#include <lmcons.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <uuid/uuid.h>
#endif
#if defined(__linux__) || defined(__ANDROID__)
#include <linux/if_packet.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <net/if_dl.h>
#endif
#endif

namespace fs = std::filesystem;

namespace ls {

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) { return p.u8string(); }

#ifdef _WIN32
static std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::string KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) out = Narrow(p);
    if (p) CoTaskMemFree(p);
    return out;
}
#endif

const char* PlatformName() {
#if defined(__ANDROID__)
    return "Android";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return "iOS";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

bool IsMobilePlatform() {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    return true;
#else
    return false;
#endif
}

static std::string HostNameImpl() {
#ifdef _WIN32
    wchar_t buf[256];
    DWORD n = 256;
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &n)) return Narrow(std::wstring(buf, n));
    return "windows-pc";
#else
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] && strcmp(buf, "localhost") != 0) return buf;
#if defined(__ANDROID__)
    return "android";
#else
    return "device";
#endif
#endif
}

static std::string UserNameImpl() {
#ifdef _WIN32
    wchar_t buf[UNLEN + 1];
    DWORD n = UNLEN + 1;
    if (GetUserNameW(buf, &n)) return Narrow(std::wstring(buf, n > 0 ? n - 1 : 0));
    return "user";
#else
    if (const char* u = getenv("USER")) if (*u) return u;
#if !defined(__ANDROID__)
    if (passwd* pw = getpwuid(getuid())) if (pw->pw_name) return pw->pw_name;
#endif
    return "user";
#endif
}

std::string HostName() {
    static const std::string v = HostNameImpl();
    return v;
}

std::string UserName() {
    static const std::string v = UserNameImpl();
    return v;
}

static std::mutex g_midMutex;
static std::string g_mid;
static bool g_midSet = false;

void SetMachineId(const std::string& id) {
    std::lock_guard<std::mutex> l(g_midMutex);
    g_mid = id;
    g_midSet = true;
}

std::string MachineId() {
    std::lock_guard<std::mutex> l(g_midMutex);
    if (g_midSet) return g_mid;
    std::string v;
#ifdef _WIN32
#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#endif
    char buf[128];
    DWORD n = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                     nullptr, buf, &n) == ERROR_SUCCESS)
        v = buf;
#elif defined(__APPLE__)
    uuid_t u;
    timespec wait = {1, 0};
    if (gethostuuid(u, &wait) == 0) {
        char s[40];
        uuid_unparse(u, s);
        v = s;
    }
#elif !defined(__ANDROID__)
    for (const char* p : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
        if (FILE* f = fopen(p, "r")) {
            char s[128] = {0};
            if (fgets(s, sizeof(s), f)) v = s;
            fclose(f);
        }
        while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
        if (!v.empty()) break;
    }
#endif
    g_mid = v;
    g_midSet = true;
    return v;
}

static std::mutex g_cfgMutex;
static std::string g_cfgOverride;

void SetConfigDirOverride(const std::string& dir) {
    std::lock_guard<std::mutex> l(g_cfgMutex);
    g_cfgOverride = dir;
}

std::string HomeDir() {
#ifdef _WIN32
    std::string h = KnownFolder(FOLDERID_Profile);
    return h.empty() ? "C:\\" : h;
#elif defined(__ANDROID__)
    return "/storage/emulated/0";
#else
    if (const char* h = getenv("HOME")) if (*h) return h;
    return "/";
#endif
}

std::string ConfigDir() {
    std::string dir;
    {
        std::lock_guard<std::mutex> l(g_cfgMutex);
        dir = g_cfgOverride;
    }
    if (dir.empty()) {
#ifdef _WIN32
        dir = JoinPath(KnownFolder(FOLDERID_RoamingAppData), "LunaShare");
#elif defined(__APPLE__)
        dir = JoinPath(HomeDir(), "Library/Application Support/LunaShare");
#elif defined(__ANDROID__)
        dir = "/data/local/tmp/LunaShare";  // app overrides with internal storage path at startup
#else
        const char* x = getenv("XDG_CONFIG_HOME");
        dir = JoinPath(x && *x ? std::string(x) : JoinPath(HomeDir(), ".config"), "LunaShare");
#endif
    }
    MakeDirs(dir);
    return dir;
}

std::string DefaultDownloadDir() {
#ifdef _WIN32
    std::string d = KnownFolder(FOLDERID_Downloads);
    if (d.empty()) d = JoinPath(HomeDir(), "Downloads");
    return JoinPath(d, "LunaShare");
#elif defined(__ANDROID__)
    return "/storage/emulated/0/Download/LunaShare";
#else
    return JoinPath(JoinPath(HomeDir(), "Downloads"), "LunaShare");
#endif
}

static std::string MacToString(const unsigned char* m, int len) {
    char buf[64];
    std::string s;
    for (int i = 0; i < len; i++) {
        snprintf(buf, sizeof(buf), i ? ":%02x" : "%02x", m[i]);
        s += buf;
    }
    return s;
}

static bool UsableMac(const unsigned char* m, int len) {
    if (len != 6) return false;
    bool allZero = true;
    for (int i = 0; i < len; i++) if (m[i]) allZero = false;
    if (allZero) return false;
    // 02:00:00:00:00:00 is what Android reports when access is denied
    if (m[0] == 2 && !m[1] && !m[2] && !m[3] && !m[4] && !m[5]) return false;
    return true;
}

std::vector<std::string> MacAddresses() {
    std::vector<std::string> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = (IP_ADAPTER_ADDRESSES*)buf.data();
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &size) != NO_ERROR) return out;
    }
    for (auto* a = aa; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        if (UsableMac(a->PhysicalAddress, (int)a->PhysicalAddressLength)) out.push_back(MacToString(a->PhysicalAddress, (int)a->PhysicalAddressLength));
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return out;
    for (ifaddrs* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || (i->ifa_flags & IFF_LOOPBACK)) continue;
#if defined(__linux__) || defined(__ANDROID__)
        if (i->ifa_addr->sa_family == AF_PACKET) {
            auto* ll = (sockaddr_ll*)i->ifa_addr;
            if (UsableMac(ll->sll_addr, ll->sll_halen)) out.push_back(MacToString(ll->sll_addr, ll->sll_halen));
        }
#elif defined(__APPLE__)
        if (i->ifa_addr->sa_family == AF_LINK) {
            auto* dl = (sockaddr_dl*)i->ifa_addr;
            if (UsableMac((unsigned char*)LLADDR(dl), dl->sdl_alen)) out.push_back(MacToString((unsigned char*)LLADDR(dl), dl->sdl_alen));
        }
#endif
    }
    freeifaddrs(ifs);
#endif
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<LocalAddress> LocalIPv4Addresses() {
    std::vector<LocalAddress> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = (IP_ADAPTER_ADDRESSES*)buf.data();
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
            if (sin->sin_family != AF_INET) continue;
            LocalAddress la;
            la.iface = Narrow(a->FriendlyName);
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            la.ip = ip;
            uint32_t addr = ntohl(sin->sin_addr.s_addr);
            int pl = u->OnLinkPrefixLength;
            if (pl > 0 && pl < 32) {
                uint32_t mask = 0xFFFFFFFFu << (32 - pl);
                in_addr b;
                b.s_addr = htonl((addr & mask) | ~mask);
                inet_ntop(AF_INET, &b, ip, sizeof(ip));
                la.broadcast = ip;
            }
            out.push_back(la);
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return out;
    for (ifaddrs* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
        LocalAddress la;
        la.iface = i->ifa_name ? i->ifa_name : "";
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_addr)->sin_addr, ip, sizeof(ip));
        la.ip = ip;
        if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr) {
            inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_broadaddr)->sin_addr, ip, sizeof(ip));
            la.broadcast = ip;
        }
        out.push_back(la);
    }
    freeifaddrs(ifs);
#endif
    return out;
}

std::vector<std::string> DefaultGateways() {
    std::vector<std::string> out;
    auto add = [&](const std::string& g) {
        if (!g.empty() && g != "0.0.0.0" && std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
    };
#ifdef _WIN32
    ULONG size = 0;
    GetIpForwardTable(nullptr, &size, FALSE);
    std::vector<unsigned char> buf(size ? size : 1);
    auto* t = (MIB_IPFORWARDTABLE*)buf.data();
    if (size && GetIpForwardTable(t, &size, TRUE) == NO_ERROR)
        for (DWORD i = 0; i < t->dwNumEntries; i++)
            if (t->table[i].dwForwardDest == 0) {
                in_addr a;
                a.s_addr = t->table[i].dwForwardNextHop;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &a, ip, sizeof(ip));
                add(ip);
            }
#elif defined(__linux__) || defined(__ANDROID__)
    if (FILE* f = fopen("/proc/net/route", "r")) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char iface[64];
            unsigned dest = 1, gw = 0, flags = 0;
            if (sscanf(line, "%63s %x %x %x", iface, &dest, &gw, &flags) == 4 && dest == 0 && gw) {
                in_addr a;
                a.s_addr = gw;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &a, ip, sizeof(ip));
                add(ip);
            }
        }
        fclose(f);
    }
#endif
    for (auto& la : LocalIPv4Addresses()) {
        size_t dot = la.ip.rfind('.');
        if (dot != std::string::npos) add(la.ip.substr(0, dot) + ".1");
    }
    return out;
}

bool MakeDirs(const std::string& path) {
    std::error_code ec;
    if (path.empty()) return false;
    fs::create_directories(P(path), ec);
    return fs::is_directory(P(path), ec);
}

bool FileExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(P(path), ec);
}

bool IsDirectory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(P(path), ec);
}

int64_t FileSize(const std::string& path) {
    std::error_code ec;
    auto s = fs::file_size(P(path), ec);
    return ec ? -1 : (int64_t)s;
}

int64_t FileMTime(const std::string& path) {
    std::error_code ec;
    auto t = fs::last_write_time(P(path), ec);
    if (ec) return 0;
    // file_clock -> seconds; the epoch offset differs between implementations but is stable on one machine,
    // which is all we use it for (detecting that a source file changed between pause and resume).
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

bool RenameFile(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(P(from), P(to), ec);
    return !ec;
}

bool RemoveFile(const std::string& path) {
    std::error_code ec;
    return fs::remove(P(path), ec);
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
#ifdef _WIN32
    return a + "\\" + b;
#else
    return a + "/" + b;
#endif
}

std::string BaseName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string DirName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    if (p == std::string::npos) return "";
    if (p == 0) return path.substr(0, 1);
    return path.substr(0, p);
}

std::string SanitizeRelativePath(const std::string& rel) {
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&]() {
        // trim spaces and dots at the end (Windows hates them)
        while (!cur.empty() && (cur.back() == ' ' || cur.back() == '.')) cur.pop_back();
        while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
        if (!cur.empty() && cur != "." && cur != "..") parts.push_back(cur);
        cur.clear();
    };
    for (char c : rel) {
        if (c == '/' || c == '\\') { flush(); continue; }
        unsigned char uc = (unsigned char)c;
        if (uc < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        cur += c;
    }
    flush();
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += '/';
        out += parts[i];
    }
    if (out.empty()) out = "file";
    return out;
}

std::string UniquePath(const std::string& path) {
    if (!FileExists(path)) return path;
    std::string dir = DirName(path), base = BaseName(path), ext;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        ext = base.substr(dot);
        base = base.substr(0, dot);
    }
    for (int i = 1; i < 100000; i++) {
        std::string cand = JoinPath(dir, base + " (" + std::to_string(i) + ")" + ext);
        if (!FileExists(cand)) return cand;
    }
    return path + "." + RandomHex(4);
}

std::vector<DirEntry> ListDir(const std::string& path) {
    std::vector<DirEntry> out;
    std::error_code ec;
    // file_clock has an implementation-defined epoch (C++17 has no clock_cast): convert via "now" on both clocks.
    auto fileNow = fs::file_time_type::clock::now();
    auto sysNow = std::chrono::system_clock::now();
    for (auto it = fs::directory_iterator(P(path), fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        DirEntry e;
        e.name = S(it->path().filename());
        std::error_code ec2;
        e.isDir = it->is_directory(ec2);
        if (!e.isDir) {
            auto sz = it->file_size(ec2);
            e.size = ec2 ? 0 : (int64_t)sz;
        }
        auto t = it->last_write_time(ec2);
        if (!ec2) {
            auto sys = sysNow + std::chrono::duration_cast<std::chrono::system_clock::duration>(t - fileNow);
            e.mtime = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        std::string la = a.name, lb = b.name;
        for (auto& c : la) c = (char)tolower((unsigned char)c);
        for (auto& c : lb) c = (char)tolower((unsigned char)c);
        return la < lb;
    });
    return out;
}

void CollectFiles(const std::string& root, std::vector<std::pair<std::string, std::string>>& out) {
    std::error_code ec;
    fs::path r = P(root);
    if (!fs::is_directory(r, ec)) {
        out.emplace_back(root, BaseName(root));
        return;
    }
    std::string top = S(r.filename());
    if (top.empty()) top = S(r.parent_path().filename());
    for (auto it = fs::recursive_directory_iterator(r, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_regular_file(ec2)) continue;
        std::string rel = S(fs::relative(it->path(), r, ec2));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        out.emplace_back(S(it->path()), top + "/" + rel);
    }
}

void OpenInFileManager(const std::string& path) {
#ifdef _WIN32
    if (IsDirectory(path)) {
        ShellExecuteW(nullptr, L"open", Widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        std::wstring args = L"/select,\"" + Widen(path) + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
    std::string cmd = (IsDirectory(path) ? "open \"" : "open -R \"") + path + "\" &";
    (void)system(cmd.c_str());
#elif defined(__linux__) && !defined(__ANDROID__)
    std::string dir = IsDirectory(path) ? path : DirName(path);
    std::string cmd = "xdg-open \"" + dir + "\" >/dev/null 2>&1 &";
    (void)system(cmd.c_str());
#else
    (void)path;  // mobile: handled by the app layer (Android intent)
#endif
}

void OpenWithDefaultApp(const std::string& path) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", Widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
    std::string cmd = "open \"" + path + "\" &";
    (void)system(cmd.c_str());
#elif defined(__linux__) && !defined(__ANDROID__)
    std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
    (void)system(cmd.c_str());
#else
    (void)path;
#endif
}

FILE* OpenFile(const std::string& path, const char* mode) {
#ifdef _WIN32
    return _wfopen(Widen(path).c_str(), Widen(mode).c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

bool FileSeek(FILE* f, int64_t off) {
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string FormatTime(int64_t ms, bool withDate) {
    time_t t = (time_t)(ms / 1000);
    tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), withDate ? "%Y-%m-%d %H:%M" : "%H:%M", &lt);
    return buf;
}

std::string FormatBytes(double b) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (b >= 1024.0 && u < 4) { b /= 1024.0; u++; }
    char buf[32];
    snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", b, units[u]);
    return buf;
}

static std::mt19937_64& Rng() {
    static thread_local std::mt19937_64 rng([] {
        std::random_device rd;
        uint64_t seed = ((uint64_t)rd() << 32) ^ rd() ^ (uint64_t)NowMs() ^ (uint64_t)(uintptr_t)&rd;
        return seed;
    }());
    return rng;
}

uint64_t RandomU64() {
    uint64_t v = 0;
    while (v == 0) v = Rng()();
    return v;
}

std::string RandomHex(int bytes) {
    std::string s;
    char buf[3];
    for (int i = 0; i < bytes; i++) {
        snprintf(buf, sizeof(buf), "%02x", (unsigned)(Rng()() & 0xFF));
        s += buf;
    }
    return s;
}

}  // namespace ls
