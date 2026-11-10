// LunaShare engine: discovery, connections, chat, resumable verified transfers, persistence.
// Thread model: network/worker threads mutate state under `mtx`; the UI locks the same mutex
// (Lock()) while it reads state for a frame. No blocking I/O is ever done while holding `mtx`.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "bluetooth.h"
#include "crypto.h"
#include "mdns.h"
#include "wan.h"
#include "model.h"
#include "net.h"
#include "protocol.h"

namespace ls {

enum class Transport { LAN = 0, Bluetooth = 1, Internet = 2 };
inline const char* TransportName(Transport t) { return t == Transport::LAN ? "LAN" : t == Transport::Internet ? "Internet" : "Bluetooth"; }
inline int TransportRank(Transport t) { return t == Transport::LAN ? 0 : t == Transport::Internet ? 1 : 2; }  // lower = preferred

struct Connection {
    uint64_t cid = 0;
    SocketHandle sock = kInvalidSocket;
    Transport transport = Transport::LAN;
    std::string addr;
    bool initiator = false;
    std::string peerId;
    bool ready = false;
    int64_t connectedAt = 0;
    std::atomic<int64_t> lastRecv{0};
    std::atomic<int64_t> bytesIn{0}, bytesOut{0};
    std::atomic<bool> closed{false};
    std::vector<std::string> caps;  // optional features the peer has (e.g. "voice"), from its Hello
    std::atomic<bool> callPriority{false};  // a call runs over this connection: keep file data small
    bool mailbox = false;  // not a real connection: frames from / to the server mailbox (device offline)
    int64_t lastChunkMs = 0;
    std::atomic<int> alive{2};  // reader + writer; the last one to exit closes the socket
    std::unique_ptr<SecureChannel> sec;

    std::mutex qm;
    std::condition_variable qcv;
    std::deque<std::pair<Msg, std::string>> ctrl;
    std::vector<uint64_t> sending;  // outgoing tids being streamed
    struct PieceOut {
        std::string id;  // content id (hex)
        uint32_t idx = 0;
    };
    std::deque<PieceOut> pieces;    // shared-library pieces this peer asked for
    struct WatchOut {
        uint64_t sid = 0;
        int64_t off = 0;
        int len = 0;
    };
    std::deque<WatchOut> watchOut;  // watch together: parts of the movie this peer is waiting for
    size_t rr = 0;
};

struct OnlinePeer {
    std::string id, name, platform, ip;
    uint16_t port = 0;
    int64_t lastBeacon = 0;   // last broadcast beacon
    int64_t lastMdns = 0;     // last mDNS / Bonjour answer
    int64_t lastWan = 0;      // listed by the internet rendezvous server
    bool OnLan() const { return lastBeacon || lastMdns; }
    std::string Via() const {
        std::string v = lastBeacon && lastMdns ? "broadcast + mDNS" : lastMdns ? "mDNS" : lastBeacon ? "broadcast" : "";
        if (lastWan) v += v.empty() ? "internet" : " + internet";
        return v;
    }
};

enum class EventType { ChatReceived, IncomingOffer, TransferDone, TransferFailed, PeerOnline, WifiCreds, Info, Error,
                       CallIncoming, CallStarted, CallEnded, ChatDeleted,
                       ChatReaction,  // text = emoji, a = the message text (shortened)
                       LinkRequest,   // a device asks this one to join its account: text = confirmation code, a = name
                       LinkDone,      // linking finished: text = what happened
                       };

enum class CallState { None, OutgoingRinging, IncomingRinging, Active };
std::string CapForMedia(const std::string& media);  // capability a call type needs ("webcam" -> "video")
// ---- groups: a conversation (and calls) with several people. Key "#" + id. Members are accounts.
struct GroupMember {
    std::string acc, name;  // account key, the name the one who added them knew them by
};
struct Group {
    std::string id, name, creator;  // creator: account key (may remove others)
    std::vector<GroupMember> members;
    int64_t ver = 0;
    bool left = false;              // I left / was removed
    bool Has(const std::string& acc) const {
        for (auto& m : members)
            if (m.acc == acc) return true;
        return false;
    }
};

// ---- listening together
struct ListenItem {
    uint64_t iid = 0;          // the song's id (also its "sid" for WatchRead)
    std::string owner;         // the device it's on
    std::string by;            // who added it (name)
    std::string name, title, artist, album;  // name: the file name (for a saved copy)
    double dur = 0;
    int64_t size = 0;
    std::string path;          // (only on the owner's device; never sent)
};
struct ListenInfo {
    uint64_t sid = 0;          // 0 = not listening together
    std::string host;          // "" = I keep the queue
    std::string key;           // the conversation
    std::vector<ListenItem> items;
    int at = 0;                // the song that's on (>= items.size(): the queue played out)
    bool playing = false;
    double pos = 0;            // position at posAt (steady ms); may be < 0: the song starts in a moment
    int64_t posAt = 0;
    std::vector<std::string> people, devs;
    int64_t ver = 0;
    bool shuffle = false;      // the next song is a random one not played yet (for everyone)
    int repeat = 0;            // 0 off, 1 the whole queue, 2 the song that's on
    std::vector<uint64_t> played;  // (shuffle: songs played in this round)
    uint64_t nextIid = 0;      // (shuffle: the one picked to come next - fetched ahead by everyone)
    // transitions (everyone's): a crossfade into the next song, or remix - a part of every song blending into the next
    int crossfade = 0;         // seconds (0 = a plain change)
    bool remix = false;
    int remixLen = 45, remixFade = 6;
    double frag = 0;           // remix: where this song's part starts
    int64_t seq = 0;           // counts song starts (the same song again is a new start)
    double fade = 0;           // this start blends in over this many seconds (0 = cut)
    double FragEnd() const;    // where this song's part ends (the song's end without remix)
    int NextForFetch() const {  // the song after this one, to have it here in time (-1: none)
        int n = (int)items.size();
        if (shuffle) {
            for (int i = 0; i < n; i++)
                if (items[(size_t)i].iid == nextIid) return i;
            return -1;
        }
        if (at + 1 < n) return at + 1;
        return repeat == 1 && n ? 0 : -1;
    }
};
struct ListenInviteInfo {
    uint64_t sid = 0;
    std::string key, host;
    int64_t at = 0;
};

// ---- watch together
struct WatchInfo {
    uint64_t sid = 0;          // 0 = not watching
    std::string key;           // the conversation (person or group)
    std::string host;          // device that has the file ("" = this one)
    std::string name;          // file name
    std::string path;          // host: the file
    int64_t size = 0;
    // the shared clock (the host's; viewers follow it)
    bool playing = false;
    double pos = 0;            // seconds at `at`
    int64_t at = 0;            // steady ms
    bool hold = false;         // someone is still loading: everyone waits
    std::string waitFor;       // ... who
    std::vector<std::string> people;  // names of everyone watching
    std::vector<std::string> subFiles;  // subtitle files for the movie (host: next to it; viewer: received copies)
    std::string hostSub;       // the subtitles the host has on: "" none, "file:<name>", "track:<n>:<lang>" (n-th in the file)
    bool loop = false;         // start again at the end (everyone)
    std::string libId;         // streamed from a contact's Shared folder (content id): played here, not shared on
    double extLoaded = 0;      // ("ext:" streams: how much is there, as the app tells)
    bool saving = false;       // keeping a copy (viewer / library stream)
    bool allowSave = true;     // the host lets viewers keep a copy
    double speed = 1.0;        // playback speed (everyone's)
    double Target(int64_t now) const { return pos + (playing && !hold ? (double)(now - at) / 1000.0 * speed : 0); }
};
struct WatchInviteInfo {
    uint64_t sid = 0;
    std::string key, host, name;
    int64_t size = 0, at = 0;
};
struct WatchCache;  // watch.cpp

// ---- shared library (Shared folders of my contacts, multi-source downloads)
constexpr int64_t kPieceSize = 1 << 20;
struct LibFile {
    std::string id, rel;     // content id (hex), path inside the Shared folder ('/' separated)
    int64_t size = 0, mtime = 0;
};
struct PeerLibrary {
    std::vector<LibFile> files;
    int64_t fetched = 0;     // wall ms, 0 = never
    bool off = false;        // they don't share (or not with us)
    bool pending = false;
};
struct SwarmSource {
    bool full = false;
    std::vector<uint8_t> bits;          // partial source: which pieces it has
    std::map<uint32_t, int64_t> inflight;  // piece -> requested at (steady ms)
    int64_t bytes = 0;                  // received from this source
    int bad = 0;                        // pieces that failed the hash check
    int64_t asked = 0;
};
struct SwarmDownload {
    std::string id, rel;
    int64_t size = 0, pieceSize = kPieceSize;
    std::string hashes;                 // 32 bytes per piece (empty until a source sent the piece map)
    std::vector<uint8_t> have;          // one byte per piece (1 = verified + written)
    int64_t done = 0;
    std::string partPath, finalPath;
    std::mutex io;  // pieces from several sources arrive on several threads: one writer at a time
    enum State { Searching, Running, Paused, Done, Failed } state = Searching;
    std::string error;
    std::map<std::string, SwarmSource> sources;  // device -> what it has
    int64_t lastHave = 0, lastMapAsk = 0, created = 0;
    double speed = 0;
    int64_t speedBytes = 0, speedTs = 0;
    uint32_t Pieces() const { return (uint32_t)((size + pieceSize - 1) / pieceSize); }
    // played while it comes (movie player): pieces from where the player reads go first. `have`, the paths and
    // the state change under `sm` too, so the player's thread can read without the engine lock.
    bool keep = true;                   // false: only for playing - not kept, not passed on
    std::atomic<int64_t> streamPos{-1}; // byte the player reads (-1: not playing)
    std::atomic<bool> streamStop{false};
    std::atomic<int64_t> streamWaitAt{0}; // steady ms the player last waited for a piece
    std::mutex sm;
    std::condition_variable scv;
};

struct CallPeer {          // group calls: another device in the call
    std::string dev, acc;
    int stream = 0;          // voice stream number (stable for the call)
    bool cameraOn = false;
    int64_t talking = 0;     // (steady ms) last time voice came from them
    int hubIdx = -1;         // through the server: its number in the hub
};
struct CallInfo {
    uint64_t id = 0;
    std::string peer;        // the device (calls with one person), or the group key "#..."
    bool outgoing = false;
    CallState state = CallState::None;
    int64_t started = 0;   // ringing began (steady ms)
    int64_t answered = 0;  // became active (steady ms)
    std::string media = "voice";
    std::vector<std::string> invited;  // outgoing, ringing: every device of the person that was called
    // ---- group calls
    std::string group;       // "#id" (then peer == group)
    std::string inviter;     // incoming: the device that invited me
    bool hub = false;        // through the server
    std::string hubId, key;  // hub id (hex), call key (32 bytes) for the hub packets
    std::vector<CallPeer> peers;
    std::string spotlight;   // device whose video is shown (and sent to me)
    bool spotlightPinned = false;
    int nextStream = 1;
    std::vector<std::string> hubIds;  // through the server: device id by hub number
    int64_t aloneSince = 0;  // steady ms
};
struct Event {
    EventType type;
    std::string peer;
    std::string text;
    uint64_t tid = 0;
    std::string a, b;  // extra (ssid/pass for WifiCreds)
};

struct HotspotInfo {
    bool active = false;
    std::string ssid, pass, error;
};

class Engine {
public:
    Engine();
    ~Engine();

    bool Start();  // loads state, opens sockets, spawns threads
    void Stop();   // saves state, closes everything

    std::unique_lock<std::recursive_mutex> Lock() { return std::unique_lock<std::recursive_mutex>(mtx); }

    // ---- actions (thread-safe; do not call while holding Lock() unless noted)
    void SendChat(const std::string& peer, const std::string& text, const std::string& replyTo = "",
                  const std::vector<std::string>& mentions = {}, bool forwarded = false);  // mentions: groups, the @named people (account keys)
    void PinMessage(const std::string& peer, const std::string& mid, bool on);   // for everyone in the chat
    // disappearing messages: new text messages in the chat go away after this (for everyone; 0 = off)
    void SetChatTtl(const std::string& peer, int seconds);
    static int TtlChoice(int seconds);  // (one of: off, 1 hour, 1 day, 1 week)
    void ExpireMessages();
    int64_t lastExpire_ = 0;
    void ApplyChatTtl(const std::string& key, int ttl, int64_t ts);
    void SendPoll(const std::string& peer, const std::string& question, const std::vector<std::string>& answers, bool several);
    void Vote(const std::string& peer, const std::string& mid, const std::vector<int>& answers);  // [] takes my answer back
    std::string MyVoterKey();  // how my votes are kept ("@account")
    void StarMessage(const std::string& peer, const std::string& mid, bool on);  // only for me
    bool EditMessage(const std::string& peer, const std::string& mid, const std::string& text);  // my own text messages
    // Paths may be files or folders. Returns batch id (0 if nothing was queued).
    uint64_t SendPaths(const std::string& peer, const std::vector<std::string>& paths);
    // files with the relative path each gets at the other side ("Album/01 Song.mp3"): a folder made up on the way
    uint64_t SendFileList(const std::string& peer, const std::vector<std::pair<std::string, std::string>>& files, const std::string& batchName,
                          bool sync = false);
    // folder sync (settings.folderSyncs): new / changed files of a folder go to my device by themselves
    void FolderSyncTick(bool now = false);
    void FolderSyncSoon() { fsSoon_ = true; }  // (look at the folders on the next tick - not on the caller's thread)
    std::string FolderSyncStatus(const std::string& id);  // "sent|on the way"
    void FolderSyncLoad();
    void FolderSyncSave();
    struct FsPending {
        std::string id, rel, sig;
    };
    std::map<std::string, std::map<std::string, std::string>> fsIndex_;  // folder sync: id -> file -> "size:mtime" sent
    std::map<uint64_t, FsPending> fsPending_;
    bool fsLoaded_ = false, fsSoon_ = false;
    int64_t fsLastScan_ = 0;
    void Accept(uint64_t tid);
    void AcceptBatch(uint64_t batch);
    void Reject(uint64_t tid);
    void Pause(uint64_t tid);
    void Resume(uint64_t tid);
    void Cancel(uint64_t tid);
    void Retry(uint64_t tid);
    void RemoveTransfer(uint64_t tid);
    void ClearFinishedTransfers();
    void PauseAll(bool pause);

    void ConnectAddress(const std::string& host, uint16_t port);   // async
    void ConnectPeer(const std::string& peerId);                    // async, uses best known route
    void ConnectBluetooth(const std::string& btAddr);               // async
    void Disconnect(const std::string& peerId);
    void AdoptSocket(SocketHandle s, Transport t, const std::string& addr, bool initiator);  // Android bridge, BT, tests

    void SetCustomName(const std::string& peer, const std::string& name);
    void SetTrusted(const std::string& peer, bool v);
    void SetBlocked(const std::string& peer, bool v);
    void SetVerified(const std::string& peer, bool v);
    // verifying by QR code: my code shows my account and device keys; scanning someone's code from their device's page
    // compares them with the keys I hold. 1 = the same (that device and every device of their account become
    // verified), 0 = not a LunaShare code, -1 = different keys (someone may be in between)
    // adding someone by QR code: my code carries my keys and where this device listens (its addresses on the networks
    // it is on now). Using someone's code connects to them; once connected, the keys in the code are compared with
    // theirs like VerifyCode (the code was read from their screen).
    std::string ConnectCode();
    // 1 = connecting (pub: their device key, verify: the "LSV1:..." text to check them with), 0 = not such a code
    int UseConnectCode(const std::string& code, std::string& pub, std::string& verify);
    std::string ConnectedDeviceByKey(const std::string& pubHex);  // "" = not connected (yet)
    std::string VerifyCode();
    int VerifyByCode(const std::string& peer, const std::string& code);
    void AcceptNewKey(const std::string& peer);  // after a key change warning
    std::string MyPublicKey() const { return identity_.PubHex(); }

    // ---- accounts. A conversation belongs to a person: its key is "@" + their account key, or the device id for
    // devices without an account (older versions) and for my own other devices. The functions below that take a
    // `peer` accept either; device-level actions (files, calls, connect) then pick the best device of that person.
    std::string MyAccount() const { return account_.PubHex(); }
    static bool IsContactKey(const std::string& k) { return !k.empty() && k[0] == '@'; }
    std::string ChatKey(const std::string& device);          // conversation the messages of `device` belong to
    std::vector<std::string> DevicesOf(const std::string& key);  // a person's devices (just `key` for a device id)
    std::string Resolve(const std::string& key);             // best device: connected (fastest link) > online > last seen
    bool IsOnline(const std::string& key);
    std::vector<std::string> MyOtherDevices();               // devices linked to my account (not this one)
    // my other devices: things the app keeps the same everywhere (playlists, settings). SyncPush hands mine (sent to
    // those online now, and to the others when they come); SyncPop gives theirs, for the app to merge.
    void SyncPush(const std::string& kind, const nlohmann::json& data);
    // ... and something asked of ONE of them (kind "torrent": download this there, how far is it...). Only when it
    // is online now (false otherwise: the app keeps what must not be lost). What arrives goes to ownHook, on the
    // engine's thread with its lock held: it must be quick and not wait for anything. Set before Start().
    bool OwnSend(const std::string& device, const std::string& kind, const nlohmann::json& data);
    bool OwnOnline(const std::string& device);
    std::function<void(const std::string& from, const std::string& kind, const nlohmann::json& data)> ownHook;
    bool SyncPop(std::string& kind, nlohmann::json& data);
    void SetPrefsVer(int64_t ver);
    void SetUpdateState(int64_t last, const std::string& seen);    // (the update check)
    // app lock: the PIN ("" = off). `pin` (the PIN itself) re-wraps the history key when the history is encrypted;
    // turning the lock off also turns the encryption off (an encrypted history still locked is then deleted: forgotten PIN)
    void SetLock(const std::string& hash, const std::string& salt, int len, const std::string& pin = "");
    // the chat history encrypted with the PIN (pin "" = back to a plain file). Needs the history open.
    void SetHistoryEncryption(const std::string& pin);
    bool HistoryEncrypted() { return !settings.histPub.empty(); }
    bool HistoryLocked() { return historyLocked_; }  // encrypted and not opened yet (messages that arrive meanwhile are kept)
    bool UnlockHistory(const std::string& pin);       // false = wrong PIN (or the file can't be read)                                   // (the synced settings: when they changed)
    void SetMutedChats(const std::map<std::string, int64_t>& m);     // (from my other device)
    std::vector<std::string> Conversations();                // every chat key the UI can list
    const KnownDevice* DeviceFor(const std::string& key);    // the best device's entry (under Lock())
    std::map<std::string, AccountInfo> accounts;             // other people's accounts (key: account key hex)
    // ---- groups (key "#" + id: usable wherever a conversation key is)
    static bool IsGroupKey(const std::string& k) { return !k.empty() && k[0] == '#'; }
    std::map<std::string, Group> groups;                     // id -> group (under Lock())
    std::string CreateGroup(const std::string& name, const std::vector<std::string>& people);  // people: "@acc" keys; returns the key
    void AddToGroup(const std::string& key, const std::vector<std::string>& people);
    void RemoveFromGroup(const std::string& key, const std::string& acc);  // the creator removes someone
    void LeaveGroup(const std::string& key);
    void RenameGroup(const std::string& key, const std::string& name);
    const Group* GroupOf(const std::string& key);
    // Linking: on the device that has the account, LinkDevice(other) asks `other` (connected) to join it. `other`
    // gets a LinkRequest event and calls AnswerLink. Both show LinkCode(other device) - the same digits.
    bool LinkDevice(const std::string& device);
    void AnswerLink(const std::string& device, bool accept);
    std::string LinkCode(const std::string& device);
    bool LinkPending(const std::string& device);  // we offered to link it, no answer yet
    // Recovery. The phrase / backup restore this account on a device (its other devices then join it again).
    std::string RecoveryPhrase();
    bool RestoreFromPhrase(const std::string& phrase, std::string* err);
    bool SaveBackup(const std::string& path, const std::string& password, std::string* err);
    bool RestoreBackup(const std::string& path, const std::string& password, std::string* err);
    void MarkBackedUp();
    // Takes one of my other devices off my account (it gets an account of its own when it hears about it).
    void RemoveMyDevice(const std::string& device);

    // ---- usernames on the internet server (per room). Results arrive in the fields below (read under Lock()).
    void ClaimUsername(const std::string& name);  // "" releases mine
    void LookupUsername(const std::string& name);
    void ConnectLookedUp();                       // connect to the devices LookupUsername found
    std::string myUsername;                       // as the server knows it ("" = none)
    std::string usernameError;                    // last claim error ("taken", "invalid name", ...)
    bool usernamePending = false;
    struct UserLookup {
        std::string name, acc, error;
        std::vector<std::pair<std::string, std::string>> devices;  // id, name
        bool pending = false;
    } lookup;
    // ---- profile picture. jpeg: an already cropped / scaled JPEG (the app makes one, <= kMaxAvatarBytes);
    // empty removes the picture. Sent to every connected device and to others when they connect.
    static constexpr size_t kMaxAvatarBytes = 96 * 1024;
    bool SetAvatar(const std::string& jpeg, std::string* err = nullptr);
    // Cached picture of a device ("" = mine). Empty when there is none. The file name changes with the picture.
    std::string AvatarPath(const std::string& peer);
    std::string SafetyNumberWith(const std::string& peer);
    void ForgetDevice(const std::string& peer);
    void ClearChat(const std::string& peer);
    // ---- chat archives (chat_archive.cpp)
    struct ChatArchiveInfo {
        std::string path, peer, name;
        int count = 0;
        int64_t from = 0, to = 0, created = 0, bytes = 0;
    };
    // saves the conversation to an archive file (compressed); clear: then removes the archived messages (keepRecent:
    // except yesterday's and today's). Returns the file, "" = failed / nothing to archive
    std::string ArchiveChat(const std::string& key, bool clear, bool keepRecent);
    std::vector<ChatArchiveInfo> ChatArchives(const std::string& key);  // "" = all, newest first
    int RestoreChatArchive(const std::string& path);   // messages added (-1: can't read it)
    bool DeleteChatArchive(const std::string& path);
    void AskHistory(const std::string& key);           // ask the other side (and my other devices) to send it back
    struct HistoryAsk {
        std::string dev, key, name, chat;
        int64_t at = 0;
        int count = 0;
    };
    std::vector<HistoryAsk> historyAsks;               // someone asks me for our history (under Lock())
    void AnswerHistoryAsk(const std::string& dev, const std::string& key, bool send);
    // compression for archives (the app sets them; without: stored as it is)
    std::function<std::string(const std::string&)> compress, decompress;
    // Removes messages from this conversation. forEveryone: also asks the other device to remove the ones we sent
    // (delivered as soon as it is reachable).
    void DeleteMessages(const std::string& peer, const std::vector<std::string>& mids, bool forEveryone);
    // my reaction to a message (one per message; "" removes it); delivered when the device is reachable
    void React(const std::string& peer, const std::string& mid, const std::string& emoji);
    void MarkRead(const std::string& peer);
    void RemoveHistory(size_t index);
    void ClearHistory();

    Settings GetSettings();
    void ApplySettings(const Settings& s);  // restarts listeners if needed
    // appData: the app's own part (playlists, sound, movie positions) - kept in the file as it is
    bool ExportSettings(const std::string& path, bool includeHistory, bool includeIdentity, std::string* err,
                        const nlohmann::json& appData = nlohmann::json());
    bool ImportSettings(const std::string& path, bool importIdentity, std::string* err, nlohmann::json* appData = nullptr);

    void BtRefresh(bool inquiry);  // async
    void SendWifiCreds(const std::string& peer, const std::string& ssid, const std::string& pass);

    // ---- calls (signalling only; audio/video are handled by an optional media module in the app)
    // ---- shared library. My Shared folder is offered to my contacts (own devices, trusted ones, people I've exchanged
    // messages or files with); I can browse theirs and download files from every contact that has them at once.
    std::string SharedDir();
    bool IsContact(const std::string& device);
    void RefreshLibrary(const std::string& peerOrKey);          // ask for a contact's list (all their devices)
    void RescanShared();                                        // look at my Shared folder now
    bool DownloadShared(const LibFile& f);                      // start (or resume) a download
    void PauseShared(const std::string& id, bool pause);
    void CancelShared(const std::string& id);
    bool HaveShared(const std::string& id);                     // in my Shared folder already
    std::vector<LibFile> myLibrary;                             // (under Lock())
    int libHashing = 0;                                         // files waiting to be hashed
    std::map<std::string, PeerLibrary> peerLibraries;           // device -> its list
    std::map<std::string, std::shared_ptr<SwarmDownload>> swarm;  // content id -> download

    std::vector<std::string> localCaps;  // set before Start(), e.g. {"voice"} when the media module is present
    std::string machineId;               // tests: instead of this computer's id (see MachineId)
    bool PeerHasCap(const std::string& peer, const std::string& cap);
    uint64_t StartCall(const std::string& peer, const std::string& media = "voice");  // peer: a device, person or group
    void SetSpotlight(const std::string& device);
    // ---- watch together (watch.cpp)
    WatchInfo watch;                          // under Lock()
    std::vector<WatchInviteInfo> watchInvites;
    // ---- listening together (listen.cpp); under Lock()
    ListenInfo listen;
    std::vector<ListenInviteInfo> listenInvites;
    uint64_t StartListen(const std::string& key);
    bool JoinListen(uint64_t sid);
    void LeaveListen();               // (the host: ends it for everyone)
    // songs of mine (path, tags) to the end of the shared queue. startAt >= 0: if nothing is playing, the first one
    // starts right away from there (the song I was listening to goes on for everyone)
    void ListenAdd(std::vector<ListenItem> items, double startAt = -1);
    void ListenControl(const std::string& op, double pos = 0, uint64_t iid = 0);  // play pause seek next prev jump remove
    void ListenEnded(uint64_t iid);   // the host's player reached the end of this song
    double ListenPos();               // where the song that's on is now (seconds)
    const ListenItem* ListenCurrent();
    bool ListenMine(uint64_t iid) { return listenOwn_.count(iid) > 0; }
    void ListenSave(uint64_t iid);    // keep a copy of someone's song (in Downloads)
    bool ListenSaving(uint64_t iid);
    double ListenLoaded(uint64_t iid);  // 0..1 here
    // the player's thread (no engine lock): bytes of someone else's song; -2 = not here yet (waited waitMs)
    int ListenRead(uint64_t iid, int64_t off, uint8_t* buf, int len, int waitMs);
    // a movie the app reads itself (a torrent still downloading, here or on one of my computers): watched alone, like
    // one from a contact's Shared folder. watch.libId is "ext:<ext>"; the app says when the reader waits for data
    // (the clock stands still meanwhile) and how much is there.
    uint64_t StartWatchExt(const std::string& ext, const std::string& name, int64_t size);
    void WatchSetExt(bool buffering, double loaded);
    uint64_t StartWatch(const std::string& key, const std::string& path);  // share a file to watch it together ("" key: alone, for now)
    void WatchInviteTo(const std::string& key);  // host: invite a person / group to the movie that's on
    uint64_t StartLibraryWatch(const LibFile& f);
    // the music player: a song from a contact's Shared folder. Returns the id to read with LibStreamReader, or ""
    // with localPath set when it's here already ("" and no path: can't)
    std::string LibStreamStart(const LibFile& f, std::string& localPath);
    void LibStreamEnd(const std::string& id);   // stops it (the pieces go unless saved)
    void LibStreamSave(const std::string& id);  // keep it (it lands in the Shared folder)
    bool LibStreamSaved(const std::string& id);
    double LibStreamLoaded(const std::string& id);  // play a file from a contact's Shared folder while it comes
    void WatchSetLoop(bool on);
    void WatchSetSpeed(double speed);  // 0.25 .. 4, for everyone watching
    // host: another movie (the next / previous one in its folder) - the people watching come along to it
    uint64_t WatchSwitch(const std::string& path);
    bool WatchFollowing() const { return watchFollow_ != 0; }  // viewer: about to come along to the host's next movie
    void WatchSave();                         // viewer / library stream: keep a copy of the whole movie
    void WatchSetAllowSave(bool on);          // host: may viewers keep a copy (remembered for next time)
    struct WatchSaveInfo {
        uint64_t sid = 0;
        std::string name, key;
        double frac = 0;
        bool connected = false;
    };
    std::vector<WatchSaveInfo> WatchSaves();  // copies still coming after their movie ended
    void CancelWatchSave(uint64_t sid);
    std::vector<std::string> watchKeys;       // host: the conversations invited (under Lock())
    bool JoinWatch(uint64_t sid);
    void LeaveWatch();
    void WatchControl(bool playing, double pos);  // play / pause / seek (pos < 0: keep the position)
    bool WatchBuffering();                    // this viewer is waiting for data
    double WatchLoaded();                     // viewer: fraction of the file here (0..1)
    // viewer's player: waits until the bytes arrive; < 0 cancelled. waitMs >= 0: gives up after that long with -2
    // (the caller checks whether it should stop and asks again)
    int WatchRead(int64_t off, uint8_t* buf, int len, int waitMs = -1);
    void WatchShareSubtitles(const std::string& path);  // host: a subtitle file picked during the movie goes to everyone
    void WatchSetHostSub(const std::string& desc);      // host: which subtitles are on here (shown to viewers as default)  // group video call: whose picture is shown big ("" = automatic)
    void AcceptCall();
    void EndCall(const std::string& reason = "hangup");
    // thread-safe (audio/video threads). data[0] is the media kind: 1 = audio, 2 = video. Audio always goes ahead of
    // queued video; video is refused (returns false) when too much is already waiting - the sender then skips frames.
    bool SendMedia(const void* data, size_t len);
    size_t MediaQueuedBytes();  // video+audio bytes waiting on the call connection (congestion signal)
    // Called on a network thread (never with the engine lock held) for each media packet of the active call.
    // stream: who it's from (0 in a call with one person; group calls: a number per person)
    std::function<void(uint64_t call, int stream, const uint8_t* data, size_t len)> onMedia;
    std::function<void(int stream)> onMediaStreamEnd;  // group calls: that person left
    CallInfo call;
    void SetHotspotInfo(const HotspotInfo& h);

    std::vector<Event> PopEvents();
    // Called for every event as it happens, on the thread that caused it, with the engine lock held (keep it
    // short; may call PeerName etc.). The app uses it for system notifications while its UI is in the background.
    std::function<void(const Event&)> onEvent;

    std::string configDir;  // empty = platform default (set before Start(); tests use separate dirs)

    // ---- state (read under Lock())
    std::recursive_mutex mtx;
    Settings settings;
    std::map<std::string, KnownDevice> devices;
    std::map<std::string, OnlinePeer> online;
    std::map<std::string, Conversation> chats;
    std::map<uint64_t, std::shared_ptr<Transfer>> transfers;
    std::vector<HistoryEntry> history;
    std::vector<std::shared_ptr<Connection>> conns;
    std::vector<BtDevice> btDevices;
    bool btScanning = false;
    bool btSupported = false;
    std::string btStatus;
    HotspotInfo hotspot;
    uint16_t boundPort = 0;
    std::string listenError;
    std::string MdnsStatus() { return mdns_.Running() ? mdns_.Status() : std::string("off"); }
    void RefreshDiscovery() { mdns_.QueryNow(); }
    std::string WanStatus() { return wan_.Running() ? wan_.Status() : std::string("off"); }
    std::string WanPublicEndpoint() { return wan_.PublicEndpoint(); }
    // the server answered and doesn't know private links (too old)
    bool WanServerLacksLinks() { return wan_.Running() && wan_.Registered() && !wan_.ServerHasLinks(); }
    std::string WanMappedEndpoint() { return wan_.MappedEndpoint(); }
    std::atomic<int64_t> totalUp{0}, totalDown{0};

    // helpers usable under Lock()
    std::shared_ptr<Connection> ReadyConn(const std::string& peer);
    void SendChatPoll(const std::string& peer, const std::string& text, const std::string& replyTo, const std::vector<std::string>& mentions,
                      bool forwarded, const nlohmann::json& poll);
    std::map<std::string, nlohmann::json> syncOut_;                         // kind -> mine (as last pushed)
    std::deque<std::pair<std::string, nlohmann::json>> syncIn_;            // theirs, for the app
    bool IsConnected(const std::string& peer) { return (bool)ReadyConn(peer); }
    std::string PeerName(const std::string& peer);
    // a chat without notifications: forever (minutes 0) or for a while; minutes < 0 turns them back on
    void MuteChat(const std::string& key, int minutes);
    bool ChatMuted(const std::string& key);
    int64_t ChatMutedUntil(const std::string& key);  // 0 forever, -1 not muted
    std::vector<std::shared_ptr<Transfer>> BatchTransfers(uint64_t batch);
    std::string ConfigPath();  // this engine's config folder

private:
    void ListenLoop();
    void LibraryLoop();
    void HandleLibrary(const std::shared_ptr<Connection>& c, Msg type, const json& j);  // under Lock()
    void HandlePieceData(const std::shared_ptr<Connection>& c, const Frame& f);
    void SwarmTick();  // under Lock(), from the maintenance loop
    bool ProducePiece(const std::shared_ptr<Connection>& c, std::vector<uint8_t>& buf, int64_t& wait);
    void FinishSwarm(const std::shared_ptr<SwarmDownload>& d);
    void LoadSwarm(const json& j);
    json SwarmJson();
    bool ShareAllowedNow();
    struct LibEntry {
        LibFile f;
        std::string hashes;  // 32 bytes per piece
    };
    std::map<std::string, LibEntry> libIndex_;  // content id -> entry (guarded by mtx)
    std::atomic<bool> libRescan_{true};
    std::thread libThread_;
    bool ThrottleTake(size_t& chunk, int64_t& wait);
    void DiscoveryLoop();
    void MaintenanceLoop();
    void HashLoop();
    void ReaderLoop(std::shared_ptr<Connection> c);
    void WriterLoop(std::shared_ptr<Connection> c);

    void Queue(const std::shared_ptr<Connection>& c, Msg m, const json& j);
    bool QueueToPeer(const std::string& peer, Msg m, const json& j);
    void CloseConn(const std::shared_ptr<Connection>& c, const char* why);
    void OnReady(const std::shared_ptr<Connection>& c);
    void HandleFrame(const std::shared_ptr<Connection>& c, Frame& f);
    void HandleData(const std::shared_ptr<Connection>& c, const Frame& f);
    json ProfileJson();  // under Lock()
    json AccountJson();  // my account's signed device list (under Lock())
    void HandleAccount(const std::shared_ptr<Connection>& c, const json& j);  // under Lock()
    void SignAccountDevices();
    void MergeChats(const std::string& device);  // after a device joined an account: its chat moves to the person's
    void QueueChatTo(const std::string& device, const std::string& key, ChatMessage& m);
    // ---- offline messages through the server mailbox
    bool MailTo(const std::string& device, Msg type, const json& j);  // seal + leave for an offline device
    bool HandleMail(const std::string& id, const std::string& dataB64);  // (WAN thread)
    void MailPending();  // leave pending messages for devices that are offline (periodically)
    int64_t lastMailScan_ = 0;
    std::string accountCert_;  // account's signature over this device
    std::map<std::string, int64_t> linkOffered_, linkAsked_, linkJoining_;  // device -> when (steady ms)
    void HandleLinkGrant(const std::shared_ptr<Connection>& c, const json& j);
    json AccountPayload(const std::string& exceptDevice);  // what a device needs to become me (link grant / backup)
    bool AdoptAccount(const json& payload, const std::string& fromDevice, bool requireSelf, std::string* err);
    void LeaveAccount();  // removed from my account by another device: start a fresh one
    void PushAccount();   // send my device list to every connected device
    void HandleServerReply(const std::string& text);
    void HandleGroupInfo(const std::shared_ptr<Connection>& c, const json& j);  // under Lock()
    void SendGroupInfo(const Group& g, const std::vector<std::string>& alsoTo = {});  // to every member device
    json GroupJson(const Group& g);
    bool GroupOnlyHidden(const std::string& device);  // "through the server" is on and it's a group-only stranger
    std::string FrameKey(const std::shared_ptr<Connection>& c, const json& j);  // conversation of an incoming chat frame
    json KeyField(const std::string& key, json j);  // adds "gid" for a group conversation (outgoing chat frames)
    Group& EnsureGroup(const std::string& gid, const std::string& senderAcc);  // a group heard of before its GroupInfo
    bool ApplyAccountList(const json& j, bool fromGroup);  // a verified, signed device list of someone's account
    void MergeChatInto(const std::string& fromKey, const std::string& toKey);
    void HandleProfile(const std::shared_ptr<Connection>& c, const json& j);  // under Lock()
    std::string DataDir();
    std::string AvatarFile(const std::string& peer, const std::string& hash);
    bool ProduceChunk(const std::shared_ptr<Connection>& c, std::vector<uint8_t>& buf, int64_t& throttleWaitMs);

    void OfferTransfer(const std::shared_ptr<Transfer>& t, bool resumeRequest);
    void StartStreaming(const std::shared_ptr<Transfer>& t, int64_t offset);
    void PumpQueued(const std::string& peer);
    void SendAcceptFor(const std::shared_ptr<Transfer>& t, bool userResume = false);
    void FinishIncoming(std::shared_ptr<Transfer> t);
    void SetFinal(const std::shared_ptr<Transfer>& t, TState st, const std::string& err);
    bool BatchFinished(uint64_t batch, std::string* name, int* count, int* ok);
    void EmitIfBatchDone(const std::shared_ptr<Transfer>& t);
    void CloseFile(Transfer& t);
    void AddHistory(const Transfer& t);
    void AddSystemMessage(const std::string& peer, const std::string& text);
    ChatMessage* FindBatchMessage(const std::string& peer, uint64_t batch);
    void Emit(Event e);
    KnownDevice& Device(const std::string& id);
    void InterruptPeerTransfers(const std::string& peer);
    void FinishCall(const std::string& reason, bool notifyPeer);
    std::atomic<uint64_t> activeCallId_{0};
    std::string activeCallPeer_;
    std::weak_ptr<Connection> activeCallConn_;  // guarded by mediaMtx_; lets audio threads send without the engine lock
    std::mutex mediaMtx_;
    std::string activeCallPeerSnapshot();
    // ---- group calls (group_calls.cpp)
    struct GcMember {  // what the media threads need to know about each other person in the call
        std::string dev;
        int stream = 0, hubIdx = -1;
        bool wantsMyVideo = false;
        uint8_t dec = 0;            // video codecs it can decode (from its [2][5] packet), 0 = not heard
        bool cameraOn = false;
        int64_t talking = 0;
        std::weak_ptr<Connection> conn;
    };
    std::vector<GcMember> gc_;      // guarded by mediaMtx_
    std::string gcSpot_;            // device whose video I'm watching (mediaMtx_)
    std::string gcKey_;             // hub call key (mediaMtx_)
    bool gcHub_ = false;
    std::string gcMyCam_, gcMyCaps_;  // my last camera-state / codecs packets (sent to whoever joins)
    std::atomic<bool> groupCall_{false};
    uint8_t gcDecSent_ = 0;
    bool QueueMediaTo(const std::shared_ptr<Connection>& c, uint64_t id, const uint8_t* data, size_t len);
    bool SendGroupMedia(uint64_t id, const uint8_t* data, size_t len);
    size_t GroupQueuedBytes();
    void GroupMediaIn(const std::string& dev, const uint8_t* data, size_t len);  // media threads
    void GroupHubIn(int from, const std::string& msg);                         // network thread
    void GroupHubMembers(int you, const std::vector<std::string>& ids);
    void SyncGroupMedia();          // call.peers -> gc_ (engine lock held)
    void GroupPeerJoined(const std::string& dev);
    void GroupPeerLeft(const std::string& dev);
    void GroupControl(const std::string& dev, Msg type, const json& j);  // to one person in the call
    void HandleGroupCall(const std::shared_ptr<Connection>& c, Msg type, const json& j);
    void GroupCallTick();
    uint64_t StartGroupCall(const std::string& key, const std::string& media);
    void AcceptGroupCall();
    void FinishGroupCall(const std::string& reason);
    void UpdateSpotlight();
    // watch together
    std::shared_ptr<WatchCache> watchCache_;  // viewer (watchCacheMtx_; the cache has its own lock)
    std::mutex watchCacheMtx_;  // (the player's threads never take the engine lock: the UI holds it while closing the player)
    std::shared_ptr<WatchCache> WatchCacheNow();
    void WatchRequestOn(const std::shared_ptr<WatchCache>& wc, int64_t from);
    struct WatchServe {  // host: the open movie files (the current one, and ended ones still being saved by someone)
        std::mutex m;
        struct File {
            FILE* f = nullptr;
            int64_t size = 0;
        };
        std::map<uint64_t, File> files;
    } watchServe_;
    struct WatchServed {  // host (engine lock): who is saving a copy of which movie
        std::set<std::string> savers;
        bool ended = false, allowSave = true;
        int64_t lastUse = 0;
    };
    std::map<uint64_t, WatchServed> watchServed_;
    void WatchServeClose(uint64_t sid);
    std::vector<std::shared_ptr<WatchCache>> watchSaves_;  // viewer: copies still coming after the movie ended (watchCacheMtx_)
    std::shared_ptr<WatchCache> WatchCacheFor(uint64_t sid);
    void WatchSavesTick();
    std::map<std::string, bool> watchBuffering_;  // host: viewer device -> still loading
    std::vector<std::string> watchViewers_;       // host: devices watching
    int64_t watchVer_ = 0, watchLastState_ = 0, watchLastReport_ = 0, watchHostSeen_ = 0;
    bool watchReportedBuffering_ = false;
    bool watchSelfBuffering_ = false;             // host of a library stream: waiting for pieces
    uint64_t watchForceSid_ = 0, watchEndNext_ = 0;  // host switching movies: the new one's id (told when the old ends)
    uint64_t watchFollow_ = 0;                    // viewer: the host's next movie, joined when its invitation comes
    int64_t watchFollowAt_ = 0;
    std::vector<std::thread> saveThreads_;        // copying saved movies out of the watch cache (joined in Stop)
    bool LibStreamBuffering(const std::shared_ptr<SwarmDownload>& d);
    void SaveWatchCopy(const std::shared_ptr<WatchCache>& wc);
public:
    // the player's read function for a library stream (any thread, no engine lock)
    std::function<int(int64_t, uint8_t*, int)> LibStreamReader(const std::string& id);
private:
    void HandleWatch(const std::shared_ptr<Connection>& c, Msg type, const json& j);
    // listening together
    std::map<uint64_t, std::string> listenOwn_;   // my songs in the session: id -> path (also in watchServe_)
    std::vector<std::string> listenMembers_;      // host: devices listening
    std::map<std::string, std::string> listenNames_;  // host: device -> name
    std::vector<std::shared_ptr<WatchCache>> listenCaches_;  // others' songs coming here (watchCacheMtx_)
    int64_t listenLastBroadcast_ = 0, listenHostSeen_ = 0;
    void HandleListen(const std::shared_ptr<Connection>& c, Msg type, const json& j);
    void HandleHistory(const std::shared_ptr<Connection>& c, Msg type, const json& j);
    json HistoryFiles(const std::vector<ChatMessage>& msgs);
    int MergeHistory(const std::string& key, std::vector<ChatMessage> msgs, const json& files, const std::string& remoteDev);
    std::map<std::string, int> historyGot_;  // messages restored so far, per chat (parts of HistoryData)
    void ListenTick();
    void ListenBroadcast();
    void ListenSetPos(double pos);    // host: the clock
    void ListenApplyCtl(const std::string& op, double pos, uint64_t iid, const std::string& who, bool host);
    void ListenGo(int at, double delay, double fade = 0);  // host: to song `at`, starting after `delay` s (or blending in)
    int ListenNextIndex(bool skip);   // host: the song after this one (shuffle / repeat); items.size() = none
    std::vector<uint64_t> listenHistory_;  // host: songs in the order they played (shuffle: "previous")
    void EndListenLocked(const std::string& why);
    void ListenApply(const json& j);  // listener: the host's state
    void ListenCaches();              // others' songs: the one that's on and the next one come here
    void ListenPeerGone(const std::string& dev);
    json ListenStateJson();
    bool ListenCacheSaveNow(const std::shared_ptr<WatchCache>& c, const std::string& dir);  // (SaveWatchCopy is private)
    int WatchReadOn(const std::shared_ptr<WatchCache>& wc, int64_t off, uint8_t* buf, int len, int waitMs);
    void HandleWatchData(const std::shared_ptr<Connection>& c, const Frame& f);
    bool ProduceWatch(const std::shared_ptr<Connection>& c, std::vector<uint8_t>& buf);
    void WatchRequest(int64_t from);   // viewer: ask the host for what's missing ahead of `from`
    void WatchBroadcast();             // host: the clock to every viewer
    void WatchTick();
    void WatchUpdateHold();
    void EndWatchLocked(const std::string& why);
    json WatchStateJson();
    void SendWatchSubs(const std::shared_ptr<Connection>& c, const std::string& path);
    std::shared_ptr<Connection> CallConn();
    std::string PartDir();

    void Load();
    void Save();
    void ApplyHistoryJson(const json& j, bool merge);  // history.json's content; merge = keep what's there, add what's new
    std::atomic<bool> historyLocked_{false};
    std::string histSec_;  // the history's private key (hex) once opened
    void MarkDirty() { dirty_ = true; }
    void StartBtServer();

    std::atomic<bool> running_{false};
    std::atomic<bool> dirty_{false};
    SocketHandle listener_ = kInvalidSocket;
    SocketHandle udp_ = kInvalidSocket;
    std::thread listenThread_, discoveryThread_, maintThread_, hashThread_;
    void Spawn(std::function<void()> fn);  // detached helper, counted so Stop() can wait for it
    std::atomic<int> liveThreads_{0};
    std::atomic<bool> restartListener_{false};
    void SendBeacon(const std::string* unicastIp);
    std::mutex hashMtx_;
    std::condition_variable hashCv_;
    std::deque<std::shared_ptr<Transfer>> hashQueue_;
    std::vector<Event> events_;
    std::atomic<uint64_t> nextCid_{1};
    std::set<std::string> connecting_;
    std::map<std::string, int64_t> lastAutoConnect_;
    Bluetooth bt_;
    Mdns mdns_;
    void StartMdns();
    WanTransport wan_;
    void StartWan();
    std::map<std::string, int64_t> lastWanConnect_;
    std::atomic<bool> restartMdns_{false}, restartWan_{false};
    // Common path for every discovery mechanism. Returns true when the peer was not known before.
    bool OnDiscovered(const std::string& id, const std::string& name, const std::string& platform, const std::string& ip,
                      uint16_t port, bool viaMdns);
    Identity identity_;
    Identity account_;

    // global upload throttle (token bucket)
    std::mutex throttleMtx_;
    double tokens_ = 0;
    int64_t tokenTs_ = 0;
};

}  // namespace ls
