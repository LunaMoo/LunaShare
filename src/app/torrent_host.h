// LunaShare - torrents. On desktops the optional torrent module (lunashare_torrent, libtorrent inside) downloads
// .torrent files and magnet links; this class loads it, keeps the list for the UI and reads a file that is still
// downloading for the movie player.
//
// A device without the module (phones) uses one of MY computers that has it, over the own-device channel
// (Engine::OwnSend, kind "torrent"): it hands the link over ("download this"), sees how far it is, and when it's
// done gets the files sent, or watches them from the computer - while they still download too. Messages:
//   -> computer  {op:"add", cid, magnet | torrent (base64), then: "send" | "keep"}
//                {op:"list"}                          (keeps the status coming for half a minute)
//                {op:"pause", id, on} {op:"remove", id, files} {op:"send", id}
//                {op:"read", rid, id, file, off, len} (watching)
//   <- computer  {op:"status", list:[...], acks:[cid...], errors:[{cid, error}]}
//                {op:"data", rid, d (base64), eof, wait}
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "lunashare_torrent.h"

class TorrentHost {
public:
    struct Torrent {
        std::string id, name, dir, path, state, error;
        int64_t size = 0, done = 0, uploaded = 0, added = 0;
        int downRate = 0, upRate = 0, peers = 0, seeds = 0, files = 0;
        // why it may be slow (module 2+): known / swarm counts, availability, can others reach us, the trackers
        int seedsAll = 0, peersAll = 0, swarmSeeds = -1, swarmPeers = -1, trackers = 0, trackersOk = 0, port = 0, mapped = 0;
        float copies = -1;
        int64_t diskFree = -1;  // state "nospace": what's free where it saves (-1: not known)
        bool incoming = false;
        std::string trackerMsg;
        bool paused = false, single = false;
        int watchFile = -1;  // the biggest movie inside (-1: none, or not known yet)
        int64_t watchSize = 0;
        std::string watchName;
        std::string from, then;  // asked for by this device of mine; "send": its files go there when done
        bool sent = false;
        bool Complete() const { return size > 0 && done >= size && (state == "seed" || state == "done"); }
    };
    struct File {
        int index = 0;
        std::string path;
        int64_t size = 0, done = 0;
        bool want = true;
    };

    bool Load();                          // the module, when it's there
    void Attach(ls::Engine* e);           // before the engine starts (what my other devices ask comes in through it)
    void Start();                         // after it started (the settings are read by then)
    void Shutdown();
    bool Available() const { return mod_ != nullptr && started_; }
    std::string loadInfo, version;
    void ApplySettings(const ls::Settings& s);
    void Tick();  // UI thread, every frame

    static bool IsTorrentSource(const std::string& s);  // a magnet link or a *.torrent path

    // ---- here (the module)
    const std::vector<Torrent>& List();  // (asked of the module a couple of times a second)
    bool Add(const std::string& source, const std::string& dir, std::string& id, std::string& err, bool pickFirst = false);
    std::vector<File> Files(const std::string& id);
    void SetWanted(const std::string& id, const std::vector<bool>& want);
    void Pause(const std::string& id, bool paused);
    void Reannounce(const std::string& id);  // look for more peers now
    void Remove(const std::string& id, bool deleteFiles);
    std::string DefaultDir() const;
    // watching while it downloads: the file's reader for PlayerHost::OpenStream (and the name it plays under)
    std::function<int(int64_t, uint8_t*, int)> Reader(const std::string& id, int file);
    void StopWatching(const std::string& id);
    bool Active() const { return active_; }  // something is moving (the UI keeps drawing)

    // ---- on one of my computers (from a device without the module)
    std::vector<std::string> Computers();  // my devices that do torrents (seen with the module; online or not)
    bool RemoteAdd(const std::string& device, const std::string& source, const std::string& then, std::string& err, bool pickFirst = false);
    // the files of a torrent on that computer (asked for again while called; nullptr until it answered) and the ticks
    const std::vector<File>* RemoteFiles(const std::string& device, const std::string& id, int64_t* askedFor = nullptr);
    void RemoteSetWanted(const std::string& device, const std::string& id, const std::vector<bool>& want);
    void RemoteWatchList(const std::string& device);  // call while its list is on screen
    const std::vector<Torrent>* RemoteList(const std::string& device, bool* fresh = nullptr);
    bool RemoteOff(const std::string& device);  // that computer doesn't take torrents from my devices (its setting)
    void RemotePause(const std::string& device, const std::string& id, bool on);
    void RemoteRemove(const std::string& device, const std::string& id, bool files);
    void RemoteSend(const std::string& device, const std::string& id);
    std::function<int(int64_t, uint8_t*, int)> RemoteReader(const std::string& device, const std::string& id, int file, int64_t size);
    int PendingCount(const std::string& device);  // links waiting for that computer to come online
    std::vector<std::string> remoteErrors;        // (for the UI to show once)

private:
    void Hook(const std::string& from, const std::string& kind, const nlohmann::json& d);
    void HandleCmd(const std::string& from, const nlohmann::json& d);
    void PushStatus();
    void SendFinished();
    void ReadWorker();
    nlohmann::json SettingsJson(const ls::Settings& s) const;
    void LoadMeta();
    void SaveMeta();
    void LoadPending();
    void SavePending();
    static Torrent FromJson(const nlohmann::json& j);
    nlohmann::json ToJson(const Torrent& t) const;

    ls::Engine* eng_ = nullptr;
    const LstModule* mod_ = nullptr;
    void* lib_ = nullptr;
    LstHost host_{};
    bool started_ = false;
    std::atomic<bool> active_{false};
    std::string dir_;

    std::vector<Torrent> list_;
    int64_t listAt_ = 0;
    struct Meta {
        std::string from, then;
        bool sent = false;
    };
    std::map<std::string, Meta> meta_;  // torrent id -> who asked
    struct Watch {
        int file = -1;
        int64_t size = 0;
        std::string name;
        bool known = false;
    };
    std::map<std::string, Watch> watch_;

    // what came from my other devices (filled on the engine's thread)
    std::mutex qm_;
    std::deque<std::pair<std::string, nlohmann::json>> cmds_;
    // computer: status subscribers (device -> until), acks and errors to report, read requests
    std::map<std::string, int64_t> subs_;
    std::map<std::string, std::vector<std::string>> acks_;
    std::map<std::string, std::vector<nlohmann::json>> errs_;
    int64_t statusAt_ = 0, sendCheckAt_ = 0;
    struct ReadReq {
        std::string from, rid, id;
        int file = 0;
        int64_t off = 0;
        int len = 0;
        int64_t at = 0;
    };
    std::mutex rm_;
    std::condition_variable rcv_;
    std::deque<ReadReq> reads_;
    std::vector<std::thread> readers_;
    std::atomic<bool> quit_{false};

    // phone: what my computers report, links not handed over yet, the stream being watched
    struct Remote {
        std::vector<Torrent> list;
        int64_t at = 0, asked = 0;
        bool off = false;
    };
    std::map<std::string, Remote> remote_;
    struct RemoteFileList {
        std::vector<File> files;
        bool known = false;
        int64_t asked = 0, firstAsked = 0;
    };
    std::map<std::string, RemoteFileList> remoteFiles_;  // device + "/" + torrent id
    static std::vector<File> FilesFromJson(const nlohmann::json& j);
    struct PendingAdd {
        std::string cid, device, magnet, torrentB64, then;
        int64_t sentAt = 0;
        bool pick = false;
    };
    std::vector<PendingAdd> pending_;
    std::set<std::string> computers_;
    int64_t pendingAt_ = 0;
    struct RemoteStream {
        std::mutex m;
        std::condition_variable cv;
        std::string device, id, tag;
        int file = 0;
        int64_t size = 0;
        std::map<int64_t, std::string> blocks;   // block number -> bytes
        std::map<int64_t, int64_t> asked;        // block number -> when
        std::deque<int64_t> order;
    };
    std::mutex sm_;
    std::shared_ptr<RemoteStream> stream_;
    std::atomic<uint64_t> ridSeq_{1};
};

TorrentHost& Torrents();
