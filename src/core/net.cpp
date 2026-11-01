#include "net.h"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ls {

void NetInit() {
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
#else
        signal(SIGPIPE, SIG_IGN);
#endif
    });
}

int LastNetError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void CloseSocket(SocketHandle s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket((SOCKET)s);
#else
    close((int)s);
#endif
}

void ShutdownSocket(SocketHandle s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    shutdown((SOCKET)s, SD_BOTH);
#else
    shutdown((int)s, SHUT_RDWR);
#endif
}

bool SendAll(SocketHandle s, const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len > 0) {
        int chunk = len > (1 << 20) ? (1 << 20) : (int)len;
#ifdef _WIN32
        int n = send((SOCKET)s, p, chunk, 0);
#elif defined(MSG_NOSIGNAL)
        int n = (int)send((int)s, p, chunk, MSG_NOSIGNAL);
#else
        int n = (int)send((int)s, p, chunk, 0);
#endif
        if (n <= 0) {
#ifndef _WIN32
            if (n < 0 && errno == EINTR) continue;
#endif
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

bool RecvAll(SocketHandle s, void* data, size_t len) {
    char* p = (char*)data;
    while (len > 0) {
        int chunk = len > (1 << 20) ? (1 << 20) : (int)len;
#ifdef _WIN32
        int n = recv((SOCKET)s, p, chunk, 0);
#else
        int n = (int)recv((int)s, p, chunk, 0);
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

int WaitReadable(SocketHandle s, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = (SOCKET)s;
    pfd.events = POLLRDNORM;
    int r = WSAPoll(&pfd, 1, timeoutMs);
#else
    pollfd pfd{};
    pfd.fd = (int)s;
    pfd.events = POLLIN;
    int r = poll(&pfd, 1, timeoutMs);
    if (r < 0 && errno == EINTR) return 0;
#endif
    if (r < 0) return -1;
    return r > 0 ? 1 : 0;
}

static void SetReuse(SocketHandle s) {
    int one = 1;
    setsockopt((decltype(socket(0, 0, 0)))s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#if defined(SO_REUSEPORT) && !defined(_WIN32)
    setsockopt((int)s, SOL_SOCKET, SO_REUSEPORT, (const char*)&one, sizeof(one));
#endif
}

SocketHandle TcpListen(uint16_t port, uint16_t* boundPort) {
    NetInit();
    auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if ((SocketHandle)s == kInvalidSocket) return kInvalidSocket;
#ifndef _WIN32
    int one = 1;  // on Windows SO_REUSEADDR lets other processes steal the port, so only use it on POSIX
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 16) != 0) {
        CloseSocket((SocketHandle)s);
        return kInvalidSocket;
    }
    if (boundPort) {
        socklen_t l = sizeof(a);
        getsockname(s, (sockaddr*)&a, &l);
        *boundPort = ntohs(a.sin_port);
    }
    return (SocketHandle)s;
}

SocketHandle TcpAccept(SocketHandle listener, std::string* peerIp, int timeoutMs) {
    if (WaitReadable(listener, timeoutMs) != 1) return kInvalidSocket;
    sockaddr_in a{};
    socklen_t l = sizeof(a);
    auto c = accept((decltype(socket(0, 0, 0)))listener, (sockaddr*)&a, &l);
    if ((SocketHandle)c == kInvalidSocket) return kInvalidSocket;
    if (peerIp) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        *peerIp = ip;
    }
    TuneStreamSocket((SocketHandle)c, true);
    return (SocketHandle)c;
}

static void SetNonBlocking(SocketHandle s, bool nb) {
#ifdef _WIN32
    u_long m = nb ? 1 : 0;
    ioctlsocket((SOCKET)s, FIONBIO, &m);
#else
    int fl = fcntl((int)s, F_GETFL, 0);
    fcntl((int)s, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

SocketHandle TcpConnect(const std::string& host, uint16_t port, int timeoutMs) {
    NetInit();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string ps = std::to_string(port);
    if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) return kInvalidSocket;
    SocketHandle result = kInvalidSocket;
    for (addrinfo* ai = res; ai && result == kInvalidSocket; ai = ai->ai_next) {
        auto s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if ((SocketHandle)s == kInvalidSocket) continue;
        SetNonBlocking((SocketHandle)s, true);
        int r = connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen);
        bool ok = r == 0;
        if (!ok) {
#ifdef _WIN32
            bool inProgress = WSAGetLastError() == WSAEWOULDBLOCK;
            WSAPOLLFD pfd{};
            pfd.fd = s;
            pfd.events = POLLWRNORM;
            if (inProgress && WSAPoll(&pfd, 1, timeoutMs) == 1 && !(pfd.revents & (POLLERR | POLLHUP))) ok = true;
#else
            bool inProgress = errno == EINPROGRESS;
            pollfd pfd{};
            pfd.fd = s;
            pfd.events = POLLOUT;
            if (inProgress && poll(&pfd, 1, timeoutMs) == 1) ok = true;
#endif
            if (ok) {
                int err = 0;
                socklen_t el = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
                ok = err == 0;
            }
        }
        if (ok) {
            SetNonBlocking((SocketHandle)s, false);
            TuneStreamSocket((SocketHandle)s, true);
            result = (SocketHandle)s;
        } else {
            CloseSocket((SocketHandle)s);
        }
    }
    freeaddrinfo(res);
    return result;
}

void TuneStreamSocket(SocketHandle s, bool tcp) {
    auto fd = (decltype(socket(0, 0, 0)))s;
    int one = 1;
    if (tcp) {
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof(one));
    }
    int buf = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof(buf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof(buf));
}

SocketHandle UdpOpen(uint16_t port) {
    NetInit();
    auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((SocketHandle)s == kInvalidSocket) return kInvalidSocket;
    SetReuse((SocketHandle)s);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
        CloseSocket((SocketHandle)s);
        return kInvalidSocket;
    }
    return (SocketHandle)s;
}

bool UdpJoinMulticast(SocketHandle s, const char* group) {
    ip_mreq m{};
    inet_pton(AF_INET, group, &m.imr_multiaddr);
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    unsigned char loop = 1;
    setsockopt((decltype(socket(0, 0, 0)))s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
    return setsockopt((decltype(socket(0, 0, 0)))s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof(m)) == 0;
}

bool UdpSendTo(SocketHandle s, const std::string& ip, uint16_t port, const void* data, size_t len) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return false;
    return sendto((decltype(socket(0, 0, 0)))s, (const char*)data, (int)len, 0, (sockaddr*)&a, sizeof(a)) == (int)len;
}

int UdpRecvFrom(SocketHandle s, void* buf, size_t len, std::string* fromIp, int timeoutMs) {
    if (WaitReadable(s, timeoutMs) != 1) return 0;
    sockaddr_in a{};
    socklen_t l = sizeof(a);
    int n = (int)recvfrom((decltype(socket(0, 0, 0)))s, (char*)buf, (int)len, 0, (sockaddr*)&a, &l);
    if (n > 0 && fromIp) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        *fromIp = ip;
    }
    return n;
}

bool LocalSocketPair(SocketHandle out[2]) {
#ifdef _WIN32
    // Emulate with a loopback TCP pair.
    uint16_t port = 0;
    SocketHandle l = TcpListen(0, &port);
    if (l == kInvalidSocket) return false;
    SocketHandle a = TcpConnect("127.0.0.1", port, 2000);
    SocketHandle b = TcpAccept(l, nullptr, 2000);
    CloseSocket(l);
    if (a == kInvalidSocket || b == kInvalidSocket) { CloseSocket(a); CloseSocket(b); return false; }
    out[0] = a; out[1] = b;
    return true;
#else
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return false;
    out[0] = sv[0]; out[1] = sv[1];
    return true;
#endif
}

}  // namespace ls
