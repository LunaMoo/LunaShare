// Wire protocol. Every frame: [u32 LE length of (type + payload)] [u8 type] [payload].
// Control frames carry UTF-8 JSON. FILE_DATA is binary:
//   [u64 LE transfer id] [u64 LE offset] [u32 LE crc32 of data] [data...]
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "net.h"

namespace ls {

constexpr int kProtocolVersion = 2;  // v2: encrypted (see crypto.h)
constexpr const char* kAppId = "LunaShare";
constexpr uint16_t kDefaultTcpPort = 47820;
constexpr uint16_t kDiscoveryPort = 47821;
constexpr const char* kMulticastGroup = "239.255.71.82";
constexpr uint32_t kMaxFrame = 16u << 20;
// Bluetooth RFCOMM service UUID (also used on Android/Windows SDP).
constexpr const char* kBtServiceUuid = "6c756e61-7368-6172-6500-000000000001";

enum class Msg : uint8_t {
    Hello = 1,        // {proto, id, name, user, host, platform, macs[], app, tcpPort, pub, sig}
    Ping = 2,
    Pong = 3,
    Chat = 10,        // {mid, ts, text}
    ChatAck = 11,     // {mid}
    FileOffer = 20,   // {tid, name, rel, size, sha256, mtime, batch, batchCount, mid}
    FileAccept = 21,  // {tid, offset}  (also used to resume: receiver tells sender where to continue)
    FileReject = 22,  // {tid, reason}
    FileData = 23,    // binary
    FilePause = 24,   // {tid}
    FileResume = 25,  // {tid}  sender asks receiver to send FileAccept again
    FileCancel = 26,  // {tid, reason}
    FileDone = 27,    // {tid}  sender: all bytes sent
    FileVerify = 28,  // {tid, ok, sha256}  receiver: result of hashing the received file
    WifiCreds = 40,   // {ssid, pass}  hotspot handoff over a slow link (Bluetooth)
    CallInvite = 50,  // {call, media}
    CallAccept = 51,  // {call}
    CallEnd = 52,     // {call, reason}  (hangup / declined / busy / no answer / unsupported)
    Media = 53,       // binary: [u64 LE call id][codec payload, interpreted by the media module]
    Bye = 99,
};

struct Frame {
    Msg type = Msg::Ping;
    std::vector<uint8_t> payload;
    std::string Text() const { return std::string(payload.begin(), payload.end()); }
};

bool WriteFrame(SocketHandle s, Msg type, const void* payload, size_t len);
bool WriteFrame(SocketHandle s, Msg type, const std::string& json);
bool ReadFrame(SocketHandle s, Frame& out);

inline void PutU32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8)); }
inline void PutU64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8)); }
inline uint32_t GetU32(const uint8_t* p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (i * 8); return v; }
inline uint64_t GetU64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8); return v; }

constexpr size_t kDataHeader = 20;

}  // namespace ls
