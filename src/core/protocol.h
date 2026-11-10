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
    Hello = 1,        // {proto, id, name, user, host, platform, macs[], app, tcpPort, pub, sig, caps[], pv, avh,
                      //  acc (account key), acs (account's signature over "id\npub"), av (account device list version)}
    Ping = 2,
    Pong = 3,
    Chat = 10,        // {mid, ts, text}
    ChatAck = 11,     // {mid}
    ChatDelete = 12,  // {mids[]}  "delete for everyone": remove these messages the sender sent earlier
    ChatDeleteAck = 13,  // {mids[]}
    ChatReact = 14,   // {mid, emoji}  my reaction to a message (either side's); "" removes it. One per side.
    ChatReactAck = 15,  // {mid, emoji}
    ProfileGet = 16,  // {}  send me your current profile (my copy is older than the version in your Hello)
    ProfileData = 17, // {ver, name, avh, img (base64 JPEG, "" = no picture), sig}  also pushed when it changes
    AccountGet = 18,  // {}  send me your account's device list (Hello said a newer version exists)
    AccountData = 19, // {pub, ver, devices[{id, pub}], sig}  signed by the account key; pushed to own devices too
    ChatSync = 30,    // {chat, mid, ts, text, out}  between devices of the SAME account: a message of conversation `chat`
    ChatSyncAck = 31, // {mid}
    LinkOffer = 32,   // {}  "join my account" (the user picked this device); both show the connection's Code()
    LinkAnswer = 33,  // {accept}
    ChatRead = 36,    // {mids[], chat}  "seen": to the sender's devices (and to my own, which then clear their badge)
    LinkGrant = 34,
    // ---- watch together: one device streams a file, everyone's player plays it at the same moment
    WatchInvite = 80,  // {sid, name, size[, gid]}            host -> the people it's shared with
    WatchJoin = 81,    // {sid}                               -> host
    WatchLeave = 82,   // {sid[, end]}                        a viewer leaves / the host ends it for everyone
    WatchCtl = 83,     // {sid, playing, pos}                 a viewer plays / pauses / seeks -> host
    WatchState = 84,   // {sid, playing, pos, hold, wait, people, ver}  host -> viewers (on change and every second)
    WatchReport = 85,  // {sid, buffering}                    viewer -> host
    WatchRead = 86,    // {sid, off, len}                     viewer -> host: send me these bytes
    WatchData = 87,    // binary: [sid u64][offset u64][data] host -> viewer
    WatchSubs = 88,    // {sid, name, data (base64)}          host -> viewer: a subtitle file lying next to the movie
    // ---- listening together: one device keeps the shared queue and the clock; everyone adds songs from their own
    // music, and each song streams from the device it's on (WatchRead / WatchData with the song's id as "sid")
    ListenInvite = 90,  // {sid[, gid]}                          host -> the people it's shared with
    ListenJoin = 91,    // {sid, name}                            -> host
    ListenLeave = 92,   // {sid[, end]}                           a listener leaves / the host ends it for everyone
    ListenCtl = 93,     // {sid, op, pos, iid}                    listener -> host: play/pause/seek/next/prev/jump/remove
    ListenState = 94,   // {sid, items[], at, playing, pos, people[], devs[], ver}  host -> listeners
    // ---- chat history sent back (when it was lost here): asked for, then sent (after the other person agrees)
    ChatEdit = 102,     // {mid, text, ts[, gid] | chat (my other devices)}  a message changed by who wrote it
    ChatEditAck = 103,  // {mid[, gid]}
    SyncData = 104,     // my own devices: {kind, data} - playlists / settings, merged by the app
    OwnCmd = 108,       // my own devices: {kind, data} - something asked of this one device (torrents on my computer)
    ChatTtl = 107,      // {ttl (s), ts[, gid | chat]} - disappearing messages in that chat; the later change wins
    ChatVote = 106,     // {mid, o: [answers], ts[, gid | chat]} - my answers to a poll; the later change wins
    ChatPin = 105,      // {mid, on, ts[, gid | chat]} - a message (un)pinned in its chat; the later change wins
    HistoryAsk = 100,   // {chat[, gid]}                                   -> the other side (or my other devices)
    HistoryData = 101,  // {chat, msgs[], files[], last, from[, declined, empty][, gid]}  in parts
    ListenAdd = 95,     // {sid, items[{iid, name, title, artist, album, dur, size}]}  listener -> host: songs of mine
    GroupInfo = 70,   // {gid, name, creator, ver, members[{acc, name, devs[{id, pub}]}]}  to every member device
                      // (chat frames of a group carry "gid": Chat, ChatAck, ChatSync(chat="#gid", from), ChatRead(chat),
                      //  ChatReact(Ack), ChatDelete(Ack))   // {secret, account{...AccountData}, name, avh, img, pv, devices[], accounts[], chats{}}
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
    CallJoin = 54,    // group call: {call, gid, reply}  "I'm in" (to every member device; answered with reply=true)
    CallWant = 55,    // group call: {call, want}  "send me your video" / "stop"
    // Group calls through the server: CallInvite carries {gid, hub, key}; everyone joins the hub, which passes each
    // packet to the people it is meant for. Hub messages are sealed with the call key: ['M'][media packet] or
    // ['C'][u8 Msg][json] (CallWant, CallEnd).
    // shared library: the files in each contact's Shared folder, downloaded piece by piece from everyone who has them
    LibGet = 60,      // {}  send me the list of your Shared folder
    LibList = 61,     // {files[{id, rel, size, mtime}], off}  (id: content id, hex SHA-256 of the piece hashes + size)
    LibChanged = 62,  // {}  my Shared folder changed
    PieceHave = 63,   // {ids[]}  which of these do you have?
    PieceHaveReply = 64,  // {have[{id, full, bits (base64 bitmap, partial downloads)}]}
    PieceMapGet = 65, // {id}
    PieceMap = 66,    // {id, size, ps (piece size), h (base64: 32-byte SHA-256 per piece)}
    PieceGet = 67,    // {id, idx}
    PieceData = 68,   // binary: [32-byte content id][u32 LE piece index][data]
    PieceNo = 69,     // {id, idx}  can't send that piece (don't have it / not sharing now)
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
