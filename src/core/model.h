// Data model shared by the engine and the UI, plus JSON (de)serialisation.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ls {

using json = nlohmann::json;

enum class UiLayout : int { Auto = 0, Desktop = 1, Mobile = 2 };

// One device of an account, as listed in the account's signed device list.
struct AccountDevice {
    std::string id, pub;  // device id + its identity key (hex)
    bool operator==(const AccountDevice& o) const { return id == o.id && pub == o.pub; }
};

// A private contact link (invite code): the two devices meet on the server through a token only they know,
// without being listed in any room - people I invite this way can't find each other (see docs/INTERNET.md).
struct WanLink {
    std::string secret;  // 32 random bytes, hex (the token sent to the server is a hash of it)
    std::string label;   // who it is for / from
    int64_t created = 0;
    bool mine = true;    // I made the code (false: someone else's invite I used)
    std::string room;    // my codes: the invite room it belongs to (its id); someone else's: the room's name
    bool operator==(const WanLink& o) const {
        return secret == o.secret && label == o.label && created == o.created && mine == o.mine && room == o.room;
    }
};

// What we know about someone else's account (from their signed device list).
struct AccountInfo {
    std::string pub;
    int64_t ver = 0;
    std::vector<AccountDevice> devices;
    std::vector<std::string> removed;  // device ids taken off the account (they must not rejoin by themselves)
    std::string sig;
};

struct Settings {
    // identity (generated once; NOT part of a normal export so two devices never share an id)
    std::string deviceId;
    std::string identitySecret;  // EdDSA seed (hex) - private key of this device
    std::string displayName;
    // profile picture (avatar.jpg in the config folder); changed only through Engine::SetAvatar
    std::string avatarHash;        // SHA-256 hex of avatar.jpg, empty = no picture
    int64_t profileVer = 0;        // bumped whenever the picture changes; peers fetch when theirs is older
    // account: a key of the person, shared by all their linked devices (see docs/ACCOUNTS.md). Its secret is
    // never part of a normal settings export; it moves only through linking and the recovery phrase / backup.
    std::string accountSecret;     // EdDSA seed (hex)
    std::vector<AccountDevice> accountDevices;  // this account's devices, signed as a whole (accountSig)
    std::vector<std::string> accountRemoved;  // device ids removed from the account (part of the signed list)
    int64_t accountVer = 0;
    std::string accountSig;
    bool accountBackedUp = false;  // the user saved the recovery phrase or a backup file

    std::string downloadDir;
    uint16_t tcpPort = 47820;
    bool discovery = true;
    bool mdns = true;              // Bonjour / mDNS (_lunashare._tcp)
    bool broadcast = true;         // UDP broadcast + multicast beacons
    bool autoConnect = true;
    bool autoAcceptTrusted = true;
    bool autoAcceptAll = false;
    bool autoResume = true;
    bool bluetooth = false;
    int btChannel = 12;
    // optional internet rendezvous (see docs/INTERNET.md)
    bool wanEnabled = false;
    std::string wanServer;         // "host[:port]" of a lunashare-rendezvous server
    std::string wanRoom;           // devices with the same room name + password see each other
    std::string wanPassword;
    bool wanNatPmp = true;         // ask the router for a port mapping (NAT-PMP)
    int wanLocalPort = 0;          // fixed local UDP port (0 = automatic)
    bool wanHidden = false;        // private mode: not listed in the room (only my account, links and those I contact)
    std::vector<WanLink> wanLinks; // private contacts (invite codes)
    std::vector<WanLink> wanInviteRooms; // my invite rooms (secret = id, label = name): people invited into one see each other
    bool wanFromInvite = false;    // the server came with someone's invite: its address, rooms and usernames stay hidden
    int wanForwardedPort = 0;      // external port forwarded to wanLocalPort on the router            // Linux RFCOMM channel (Windows/Android use SDP with the service UUID)
    int64_t speedLimitKBs = 0;     // 0 = unlimited (total upload)
    int chunkKB = 256;
    int maxParallel = 2;           // concurrently streaming files per device
    bool notifySound = true;
    bool shareLibrary = true;      // contacts may download what is in my Shared folder
    bool shareOnMobileData = false;  // phones: also while on mobile data
    bool groupsViaServer = false;    // group chats and calls go through the server: no direct connections to group
                                     // members who aren't my contacts (they don't learn my address)
    std::string sharedDir;         // empty = "Shared" inside the folder for received files
    bool backgroundRun = true;     // phones: stay connected in the background (notifications for messages and calls)
    bool closeToTray = false;      // computers: closing the window keeps LunaShare running (tray icon on Windows)
    bool keepHistory = true;

    UiLayout layout = UiLayout::Auto;
    float uiScale = 0.0f;          // 0 = automatic (DPI based)
    int theme = 0;                 // 0 dark, 1 light, 2 classic
    std::string lastBrowseDir;
    // calls (used only when the optional media module is installed)
    bool callEchoCancel = true;
    bool callNoiseSuppress = true;
    bool callAutoGain = true;
    int callBitrate = 48000;
    int callMicBoost = 0;
    bool wanRelay = true;          // internet: let the server relay (still end-to-end encrypted) when direct fails
    int callVideoRes = 1;          // 0 360p 1 540p 2 720p 3 1080p 4 1440p 5 4K 6 8K
    int callVideoFps = 24;         // 15 / 24 / 30 / 60 / 120
    bool callVideoAdaptive = true; // lower the quality automatically when the connection can't keep up
    bool playerLoop = false;
    bool watchAllowSave = true;  // watching together: people watching my movie may keep a copy  // videos / audio in chats: start again at the end
    bool fullscreenBorderless = false;  // Windows: movies full screen as a borderless window (no game HDR)
    bool callFullPreview = false;  // self-view in the camera's full size and frame rate (calls, video messages)
    std::string callCamera;        // preferred camera id (empty: front camera)
    bool virtualCamera = false;
    bool callVideoH264Only = false; // troubleshooting: never use HEVC/AV1    // Windows 11: offer received video as "LunaShare Camera" to other apps      // dB added to the microphone before processing (quiet PC mics)
    std::string callInputDevice, callOutputDevice;  // empty = system default
    // file picker
    bool browserGrid = false;      // gallery tiles instead of a list
    std::string language;          // UI language code ("pl"), empty = the system's
    std::string emojiRecent;       // emoji picker: recently used, '\n' separated
    int emojiTone = 0;             // emoji picker: skin tone 0 (none) .. 5
    bool browserPicturesOnly = false;
    int browserSort = 0;           // 0 name, 1 newest first, 2 largest first
    int browserTile = 130;         // tile size (unscaled px)
};

struct KnownDevice {
    std::string id;
    std::string name;        // name the device announces
    std::string customName;  // local nickname set by the user
    std::string user, host, platform;
    std::vector<std::string> macs;
    std::string lastIp;
    uint16_t lastPort = 0;
    std::string btAddr;
    int64_t firstSeen = 0, lastSeen = 0;
    bool trusted = false;
    bool blocked = false;
    std::string notes;
    std::string pubKey;      // pinned identity key (hex), set on first encrypted contact
    bool verified = false;   // user compared the safety number
    std::string pendingKey;  // a DIFFERENT key was presented: connection refused until the user accepts it
    std::string avatarHash;  // their profile picture (verified, cached in avatars/), empty = none
    int64_t profileVer = 0;  // version of the profile we have
    std::string accountPub;  // the account this device belongs to (verified certificate), empty = none / old version
    bool viaGroup = false;   // known only as a member of a group (not listed as a contact by itself)
    std::string DisplayName() const { return customName.empty() ? (name.empty() ? id.substr(0, 8) : name) : customName; }
    std::string Fingerprint() const;  // short, readable form of the identity key (or id if unknown)
};

enum class MsgKind : int { Text = 0, Files = 1, System = 2 };

struct ChatMessage {
    std::string mid;
    bool outgoing = false;
    int64_t ts = 0;
    MsgKind kind = MsgKind::Text;
    std::string text;
    std::vector<uint64_t> tids;  // for Files
    uint64_t batch = 0;
    bool delivered = false;
    std::string myReaction, theirReaction;  // emoji (UTF-8), empty = none
    bool reactionSent = true;               // false until the other device confirmed myReaction
    std::vector<std::string> pendingTo;     // devices that still have to get this message (contact's + my other ones)
    std::vector<std::string> mailedTo;      // ... of those, the ones it was left for in the server mailbox
    std::string from;                       // groups: who wrote it ("@account" key), empty = me / not a group
    bool read = false;                      // outgoing: the other person has seen it
    bool readSent = true;                   // incoming: false until we told the sender it was seen
};

struct Conversation {
    std::string peer;
    std::vector<ChatMessage> messages;
    int unread = 0;
    std::vector<std::string> pendingDeletes;  // "delete for everyone" not yet confirmed by the other device
    bool readPending = false;                 // incoming messages whose read receipt wasn't sent yet
};

enum class TState : int {
    Hashing = 0,     // sender: computing SHA-256 before offering
    Offered,         // sender: offer sent / waiting for the device to come online
    Pending,         // receiver: waiting for the user to accept
    Queued,          // accepted, waiting for a free slot
    Active,
    Paused,
    Interrupted,     // connection lost; resumes automatically
    Verifying,       // hashing the received file / waiting for the receiver's verdict
    Completed,
    Failed,
    Rejected,
    Cancelled,
};
const char* TStateName(TState s);
bool TStateFinal(TState s);

struct Transfer {
    uint64_t tid = 0;
    uint64_t batch = 0;
    std::string batchName;
    int batchCount = 1;
    std::string peer;
    bool outgoing = false;
    std::string name;       // base name
    std::string rel;        // relative path ("Folder/sub/file.bin" or just name)
    std::string localPath;  // outgoing: source; incoming: final destination once complete
    std::string partPath;   // incoming: partial file
    int64_t size = 0;
    int64_t mtime = 0;
    std::string sha256;     // sender's hash (before)
    std::string recvSha;    // receiver's hash (after)
    TState state = TState::Hashing;
    bool pausedByPeer = false;
    std::string error;
    int64_t created = 0, finished = 0;

    // runtime (not persisted)
    std::atomic<int64_t> done{0};
    std::atomic<int64_t> hashDone{0};
    std::atomic<bool> hashCancel{false};
    std::mutex io;           // guards fp / sendOffset / resync
    FILE* fp = nullptr;
    int64_t sendOffset = 0;
    bool resyncRequested = false;
    double speed = 0;        // bytes/s, smoothed
    int64_t lastDone = 0;
    int64_t lastTick = 0;
    bool wantResume = false;
};

struct HistoryEntry {
    uint64_t tid = 0;
    std::string peer;
    std::string peerName;
    bool outgoing = false;
    std::string name;
    std::string path;
    int64_t size = 0;
    std::string sha256;
    bool verified = false;
    int64_t ts = 0;
};

json ToJson(const Settings& s, bool withIdentity);
void FromJson(const json& j, Settings& s, bool withIdentity);
json ToJson(const KnownDevice& d);
KnownDevice DeviceFromJson(const json& j);
json ToJson(const ChatMessage& m);
ChatMessage MessageFromJson(const json& j);
json ToJson(const Transfer& t);
void TransferFromJson(const json& j, Transfer& t);
json ToJson(const HistoryEntry& h);
HistoryEntry HistoryFromJson(const json& j);

bool SaveJsonFile(const std::string& path, const json& j);
bool LoadJsonFile(const std::string& path, json& j);

}  // namespace ls
