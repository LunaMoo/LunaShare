// Thin cross-platform socket layer (Winsock / BSD sockets). Bluetooth RFCOMM sockets and
// Android bridged sockets are plain socket handles as well, so the protocol runs on all of them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ls {

using SocketHandle = intptr_t;
constexpr SocketHandle kInvalidSocket = -1;

void NetInit();
void CloseSocket(SocketHandle s);
void ShutdownSocket(SocketHandle s);  // unblocks threads stuck in recv/send
int LastNetError();

bool SendAll(SocketHandle s, const void* data, size_t len);
bool RecvAll(SocketHandle s, void* data, size_t len);
// Returns 1 readable, 0 timeout, -1 error.
int WaitReadable(SocketHandle s, int timeoutMs);

SocketHandle TcpListen(uint16_t port, uint16_t* boundPort);
SocketHandle TcpAccept(SocketHandle listener, std::string* peerIp, int timeoutMs);
SocketHandle TcpConnect(const std::string& host, uint16_t port, int timeoutMs);
void TuneStreamSocket(SocketHandle s, bool tcp);

SocketHandle UdpOpen(uint16_t port);  // bound, broadcast-enabled, address reuse
bool UdpJoinMulticast(SocketHandle s, const char* group);
bool UdpSendTo(SocketHandle s, const std::string& ip, uint16_t port, const void* data, size_t len);
int UdpRecvFrom(SocketHandle s, void* buf, size_t len, std::string* fromIp, int timeoutMs);

// Creates a connected local socket pair (used to bridge Java Bluetooth streams on Android).
bool LocalSocketPair(SocketHandle out[2]);

}  // namespace ls
