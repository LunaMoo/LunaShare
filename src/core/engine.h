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

enum class EventType { ChatReceived, IncomingOffer, TransferDone, TransferFailed, PeerOnline, WifiCreds, Info, Error };
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
    std::string SafetyNumberWith(const std::string& peer);
    void ForgetDevice(const std::string& peer);
    void ClearChat(const std::string& peer);
    void MarkRead(const std::string& peer);
    void RemoveHistory(size_t index);
    void ClearHistory();

    Settings GetSettings();
    void ApplySettings(const Settings& s);  // restarts listeners if needed
    bool ExportSettings(const std::string& path, bool includeHistory, bool includeIdentity, std::string* err);
    bool ImportSettings(const std::string& path, bool importIdentity, std::string* err);

    void BtRefresh(bool inquiry);  // async
    void SendWifiCreds(const std::string& peer, const std::string& ssid, const std::string& pass);
    void SetHotspotInfo(const HotspotInfo& h);

    std::vector<Event> PopEvents();

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

    // global upload throttle (token bucket)
    std::mutex throttleMtx_;
    double tokens_ = 0;
    int64_t tokenTs_ = 0;
};

}  // namespace ls
