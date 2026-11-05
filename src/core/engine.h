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
struct CallInfo {
    uint64_t id = 0;
    std::string peer;
    bool outgoing = false;
    CallState state = CallState::None;
    int64_t started = 0;   // ringing began (steady ms)
    int64_t answered = 0;  // became active (steady ms)
    std::string media = "voice";
    std::vector<std::string> invited;  // outgoing, ringing: every device of the person that was called
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
    void SendChat(const std::string& peer, const std::string& text);
    // Paths may be files or folders. Returns batch id (0 if nothing was queued).
    uint64_t SendPaths(const std::string& peer, const std::vector<std::string>& paths);
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
    std::vector<std::string> Conversations();                // every chat key the UI can list
    const KnownDevice* DeviceFor(const std::string& key);    // the best device's entry (under Lock())
    std::map<std::string, AccountInfo> accounts;             // other people's accounts (key: account key hex)
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
    bool ExportSettings(const std::string& path, bool includeHistory, bool includeIdentity, std::string* err);
    bool ImportSettings(const std::string& path, bool importIdentity, std::string* err);

    void BtRefresh(bool inquiry);  // async
    void SendWifiCreds(const std::string& peer, const std::string& ssid, const std::string& pass);

    // ---- calls (signalling only; audio/video are handled by an optional media module in the app)
    std::vector<std::string> localCaps;  // set before Start(), e.g. {"voice"} when the media module is present
    bool PeerHasCap(const std::string& peer, const std::string& cap);
    uint64_t StartCall(const std::string& peer, const std::string& media = "voice");
    void AcceptCall();
    void EndCall(const std::string& reason = "hangup");
    // thread-safe (audio/video threads). data[0] is the media kind: 1 = audio, 2 = video. Audio always goes ahead of
    // queued video; video is refused (returns false) when too much is already waiting - the sender then skips frames.
    bool SendMedia(const void* data, size_t len);
    size_t MediaQueuedBytes();  // video+audio bytes waiting on the call connection (congestion signal)
    // Called on a network thread (never with the engine lock held) for each media packet of the active call.
    std::function<void(uint64_t call, const uint8_t* data, size_t len)> onMedia;
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
    std::string WanMappedEndpoint() { return wan_.MappedEndpoint(); }
    std::atomic<int64_t> totalUp{0}, totalDown{0};

    // helpers usable under Lock()
    std::shared_ptr<Connection> ReadyConn(const std::string& peer);
    bool IsConnected(const std::string& peer) { return (bool)ReadyConn(peer); }
    std::string PeerName(const std::string& peer);
    std::vector<std::shared_ptr<Transfer>> BatchTransfers(uint64_t batch);

private:
    void ListenLoop();
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
    std::shared_ptr<Connection> CallConn();
    std::string PartDir();

    void Load();
    void Save();
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
