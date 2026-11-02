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

struct Settings {
    // identity (generated once; NOT part of a normal export so two devices never share an id)
    std::string deviceId;
    std::string identitySecret;  // EdDSA seed (hex) - private key of this device
    std::string displayName;

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
    int wanForwardedPort = 0;      // external port forwarded to wanLocalPort on the router            // Linux RFCOMM channel (Windows/Android use SDP with the service UUID)
    int64_t speedLimitKBs = 0;     // 0 = unlimited (total upload)
    int chunkKB = 256;
    int maxParallel = 2;           // concurrently streaming files per device
    bool notifySound = true;
    bool keepHistory = true;

    UiLayout layout = UiLayout::Auto;
    float uiScale = 0.0f;          // 0 = automatic (DPI based)
    int theme = 0;                 // 0 dark, 1 light, 2 classic
    std::string lastBrowseDir;
    // calls (used only when the optional media module is installed)
    bool callEchoCancel = true;
    bool callNoiseSuppress = true;
    bool callAutoGain = true;
    int callBitrate = 24000;
    int callMicBoost = 0;
    int callVideoRes = 1;          // 0 360p 1 540p 2 720p 3 1080p 4 1440p 5 4K 6 8K
    int callVideoFps = 24;         // 15 / 24 / 30 / 60 / 120
    bool callVideoAdaptive = true; // lower the quality automatically when the connection can't keep up
    std::string callCamera;        // preferred camera id (empty: front camera)
    bool virtualCamera = false;    // Windows 11: offer received video as "LunaShare Camera" to other apps      // dB added to the microphone before processing (quiet PC mics)
    std::string callInputDevice, callOutputDevice;  // empty = system default
    // file picker
    bool browserGrid = false;      // gallery tiles instead of a list
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
};

struct Conversation {
    std::string peer;
    std::vector<ChatMessage> messages;
    int unread = 0;
    std::vector<std::string> pendingDeletes;  // "delete for everyone" not yet confirmed by the other device
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
