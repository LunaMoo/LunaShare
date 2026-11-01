#include "bluetooth.h"

#include <cstdio>
#include <cstring>

#include "platform.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#if __has_include(<ws2bth.h>)
#include <ws2bth.h>
#define LS_BT_WINDOWS 1
#endif
#elif defined(__linux__) && !defined(__ANDROID__)
#include <sys/socket.h>
#include <unistd.h>
#define LS_BT_LINUX 1
#endif

namespace ls {

HostBridge& Bridge() {
    static HostBridge b;
    return b;
}

static bool ParseBtAddr(const std::string& s, uint8_t out[6]) {
    unsigned v[6];
    if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

static std::string BtAddrToString(const uint8_t a[6]) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
    return buf;
}

// ---------------------------------------------------------------- Windows
#if defined(LS_BT_WINDOWS)
// {6c756e61-7368-6172-6500-000000000001}
static const GUID kGuid = {0x6c756e61, 0x7368, 0x6172, {0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

static std::string WinBtAddr(BTH_ADDR a) {
    uint8_t b[6];
    for (int i = 0; i < 6; i++) b[i] = (uint8_t)(a >> (8 * (5 - i)));
    return BtAddrToString(b);
}

static BTH_ADDR WinParse(const std::string& s) {
    uint8_t b[6];
    if (!ParseBtAddr(s, b)) return 0;
    BTH_ADDR a = 0;
    for (int i = 0; i < 6; i++) a = (a << 8) | b[i];
    return a;
}
#endif

// ---------------------------------------------------------------- Linux
#if defined(LS_BT_LINUX)
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define LS_BTPROTO_RFCOMM 3
struct ls_sockaddr_rc {
    sa_family_t rc_family;
    uint8_t rc_bdaddr[6];  // little endian (reversed)
    uint8_t rc_channel;
};
#endif

bool Bluetooth::Supported() const {
#if defined(__ANDROID__)
    return Bridge().btAvailable && Bridge().btAvailable();
#elif defined(LS_BT_WINDOWS)
    NetInit();
    SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (s == INVALID_SOCKET) return false;
    closesocket(s);
    return true;
#elif defined(LS_BT_LINUX)
    int s = socket(AF_BLUETOOTH, SOCK_STREAM, LS_BTPROTO_RFCOMM);
    if (s < 0) return false;
    close(s);
    return true;
#else
    return false;
#endif
}

bool Bluetooth::StartServer(int channel, OnSocket cb, std::string* err) {
    StopServer();
    cb_ = std::move(cb);
    stop_ = false;
#if defined(__ANDROID__)
    (void)channel;
    if (!Bridge().btSetListening || !Bridge().btSetListening(true)) {
        if (err) *err = "Bluetooth unavailable or permission denied";
        return false;
    }
    listening_ = true;
    return true;
#elif defined(LS_BT_WINDOWS)
    (void)channel;
    NetInit();
    SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (s == INVALID_SOCKET) { if (err) *err = "No Bluetooth adapter"; return false; }
    SOCKADDR_BTH sa{};
    sa.addressFamily = AF_BTH;
    sa.port = BT_PORT_ANY;
    if (bind(s, (sockaddr*)&sa, sizeof(sa)) != 0 || listen(s, 4) != 0) {
        closesocket(s);
        if (err) *err = "bind/listen failed (" + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    int len = sizeof(sa);
    getsockname(s, (sockaddr*)&sa, &len);
    // Register SDP record so other devices can find us by service UUID.
    auto* reg = new char[sizeof(WSAQUERYSETW) + sizeof(CSADDR_INFO) + sizeof(SOCKADDR_BTH)]();
    auto* qs = (WSAQUERYSETW*)reg;
    auto* csa = (CSADDR_INFO*)(reg + sizeof(WSAQUERYSETW));
    auto* local = (SOCKADDR_BTH*)(reg + sizeof(WSAQUERYSETW) + sizeof(CSADDR_INFO));
    *local = sa;
    static GUID guid = kGuid;
    csa->LocalAddr.iSockaddrLength = sizeof(SOCKADDR_BTH);
    csa->LocalAddr.lpSockaddr = (sockaddr*)local;
    csa->iSocketType = SOCK_STREAM;
    csa->iProtocol = BTHPROTO_RFCOMM;
    qs->dwSize = sizeof(WSAQUERYSETW);
    qs->lpszServiceInstanceName = (LPWSTR)L"LunaShare";
    qs->lpServiceClassId = &guid;
    qs->dwNameSpace = NS_BTH;
    qs->dwNumberOfCsAddrs = 1;
    qs->lpcsaBuffer = csa;
    if (WSASetServiceW(qs, RNRSERVICE_REGISTER, 0) != 0) {
        // Not fatal: peers can still connect if they know the channel.
    }
    registration_ = reg;
    server_ = (SocketHandle)s;
#elif defined(LS_BT_LINUX)
    int s = socket(AF_BLUETOOTH, SOCK_STREAM, LS_BTPROTO_RFCOMM);
    if (s < 0) { if (err) *err = "No Bluetooth adapter / BlueZ"; return false; }
    ls_sockaddr_rc a{};
    a.rc_family = AF_BLUETOOTH;
    a.rc_channel = (uint8_t)(channel > 0 && channel <= 30 ? channel : 12);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {
        close(s);
        if (err) *err = "bind/listen on RFCOMM channel " + std::to_string(a.rc_channel) + " failed";
        return false;
    }
    server_ = s;
#else
    (void)channel;
    if (err) *err = "Bluetooth is not supported on this platform yet";
    return false;
#endif
#if !defined(__ANDROID__)
    listening_ = true;
    thread_ = std::thread([this] { ServerLoop(); });
    return true;
#endif
}

void Bluetooth::ServerLoop() {
    while (!stop_) {
        if (WaitReadable(server_, 300) != 1) continue;
#if defined(LS_BT_WINDOWS)
        SOCKADDR_BTH ra{};
        int l = sizeof(ra);
        SOCKET c = accept((SOCKET)server_, (sockaddr*)&ra, &l);
        if (c == INVALID_SOCKET) continue;
        TuneStreamSocket((SocketHandle)c, false);
        if (cb_) cb_((SocketHandle)c, WinBtAddr(ra.btAddr));
#elif defined(LS_BT_LINUX)
        ls_sockaddr_rc ra{};
        socklen_t l = sizeof(ra);
        int c = accept((int)server_, (sockaddr*)&ra, &l);
        if (c < 0) continue;
        uint8_t b[6];
        for (int i = 0; i < 6; i++) b[i] = ra.rc_bdaddr[5 - i];
        if (cb_) cb_(c, BtAddrToString(b));
#else
        break;
#endif
    }
}

void Bluetooth::StopServer() {
    stop_ = true;
#if defined(__ANDROID__)
    if (listening_ && Bridge().btSetListening) Bridge().btSetListening(false);
#endif
    if (thread_.joinable()) thread_.join();
    if (server_ != kInvalidSocket) {
        CloseSocket(server_);
        server_ = kInvalidSocket;
    }
#if defined(LS_BT_WINDOWS)
    if (registration_) {
        WSASetServiceW((WSAQUERYSETW*)registration_, RNRSERVICE_DELETE, 0);
        delete[] (char*)registration_;
        registration_ = nullptr;
    }
#endif
    listening_ = false;
}

std::vector<BtDevice> Bluetooth::Devices(bool inquiry) {
    std::vector<BtDevice> out;
#if defined(__ANDROID__)
    if (Bridge().btDevices) out = Bridge().btDevices(inquiry);
#elif defined(LS_BT_WINDOWS)
    NetInit();
    WSAQUERYSETW q{};
    q.dwSize = sizeof(q);
    q.dwNameSpace = NS_BTH;
    HANDLE h = nullptr;
    DWORD flags = LUP_CONTAINERS | LUP_RETURN_NAME | LUP_RETURN_ADDR | (inquiry ? LUP_FLUSHCACHE : 0);
    if (WSALookupServiceBeginW(&q, flags, &h) != 0) return out;
    std::vector<char> buf(8192);
    for (;;) {
        DWORD len = (DWORD)buf.size();
        auto* r = (WSAQUERYSETW*)buf.data();
        if (WSALookupServiceNextW(h, flags, &len, r) != 0) {
            if (WSAGetLastError() == WSAEFAULT) { buf.resize(len); continue; }
            break;
        }
        BtDevice d;
        if (r->lpszServiceInstanceName) {
            int n = WideCharToMultiByte(CP_UTF8, 0, r->lpszServiceInstanceName, -1, nullptr, 0, nullptr, nullptr);
            d.name.resize(n > 0 ? n - 1 : 0);
            WideCharToMultiByte(CP_UTF8, 0, r->lpszServiceInstanceName, -1, d.name.data(), n, nullptr, nullptr);
        }
        if (r->lpcsaBuffer && r->lpcsaBuffer->RemoteAddr.lpSockaddr)
            d.addr = WinBtAddr(((SOCKADDR_BTH*)r->lpcsaBuffer->RemoteAddr.lpSockaddr)->btAddr);
        d.paired = !inquiry;
        if (!d.addr.empty()) out.push_back(d);
    }
    WSALookupServiceEnd(h);
#elif defined(LS_BT_LINUX)
    if (inquiry) {
        FILE* p = popen("bluetoothctl --timeout 8 scan on >/dev/null 2>&1", "r");
        if (p) pclose(p);
    }
    for (int pass = 0; pass < 2; pass++) {
        FILE* p = popen(pass == 0 ? "bluetoothctl devices Paired 2>/dev/null" : "bluetoothctl devices 2>/dev/null", "r");
        if (!p) break;
        char line[512];
        while (fgets(line, sizeof(line), p)) {
            char addr[32] = {}, name[400] = {};
            if (sscanf(line, "Device %31s %399[^\n]", addr, name) >= 1) {
                bool dup = false;
                for (auto& d : out) if (d.addr == addr) dup = true;
                if (!dup) out.push_back({addr, name, pass == 0});
            }
        }
        pclose(p);
    }
#else
    (void)inquiry;
#endif
    return out;
}

SocketHandle Bluetooth::Connect(const std::string& addr, int channel, std::string* err, bool* asyncStarted) {
    if (asyncStarted) *asyncStarted = false;
#if defined(__ANDROID__)
    (void)channel;
    if (!Bridge().btConnectAsync) { if (err) *err = "Bluetooth unavailable"; return kInvalidSocket; }
    Bridge().btConnectAsync(addr);
    if (asyncStarted) *asyncStarted = true;
    return kInvalidSocket;
#elif defined(LS_BT_WINDOWS)
    NetInit();
    SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (s == INVALID_SOCKET) { if (err) *err = "No Bluetooth adapter"; return kInvalidSocket; }
    SOCKADDR_BTH sa{};
    sa.addressFamily = AF_BTH;
    sa.btAddr = WinParse(addr);
    if (channel > 0) sa.port = (ULONG)channel;
    else sa.serviceClassId = kGuid;
    if (connect(s, (sockaddr*)&sa, sizeof(sa)) != 0) {
        if (err) *err = "connect failed (" + std::to_string(WSAGetLastError()) + ") - is LunaShare running with Bluetooth enabled there?";
        closesocket(s);
        return kInvalidSocket;
    }
    TuneStreamSocket((SocketHandle)s, false);
    return (SocketHandle)s;
#elif defined(LS_BT_LINUX)
    int s = socket(AF_BLUETOOTH, SOCK_STREAM, LS_BTPROTO_RFCOMM);
    if (s < 0) { if (err) *err = "No Bluetooth adapter / BlueZ"; return kInvalidSocket; }
    uint8_t b[6];
    if (!ParseBtAddr(addr, b)) { close(s); if (err) *err = "bad address"; return kInvalidSocket; }
    ls_sockaddr_rc a{};
    a.rc_family = AF_BLUETOOTH;
    for (int i = 0; i < 6; i++) a.rc_bdaddr[i] = b[5 - i];
    a.rc_channel = (uint8_t)(channel > 0 && channel <= 30 ? channel : 12);
    if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
        close(s);
        if (err) *err = "connect to RFCOMM channel " + std::to_string(a.rc_channel) + " failed";
        return kInvalidSocket;
    }
    return s;
#else
    (void)addr; (void)channel;
    if (err) *err = "Bluetooth is not supported on this platform yet";
    return kInvalidSocket;
#endif
}

}  // namespace ls
