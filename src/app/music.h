// Music player: a library of the user's music folders (tags + covers read by the player module), playlists, and a
// player with its own audio device and thread, so music goes on while the app is in the background (Android:
// a media notification / lock screen controls through the bridge).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

struct LspPlayer;
struct LspModule;

namespace music {

struct Track {
    std::string path;
    std::string title, artist, album, albumArtist, genre, year;
    std::string cover;   // picture file (cached embedded cover, or cover.jpg in the folder); "" = none
    int number = 0, disc = 0;
    double duration = 0;
    int64_t size = 0, mtime = 0, added = 0;
    // volume levelling: dB to bring it to the others' loudness - from its ReplayGain tag, or measured the first
    // time it played through (gainSet: known)
    float gain = 0;
    bool gainSet = false, gainTag = false;
    bool fav = false;        // a favourite (the heart)
    int plays = 0;           // how many times it played (at least half of it, or a minute)
    int64_t lastPlayed = 0;  // (ms)
    const std::string& AlbumArtist() const { return albumArtist.empty() ? artist : albumArtist; }
};

struct Playlist {
    std::string id, name;
    std::vector<std::string> paths;  // (a song from another device not found here: "sync:" + what it is, until it is)
    int64_t mod = 0;                 // last change (ms) - syncing with my other devices: the newer one wins
};

enum Repeat { RepeatOff = 0, RepeatAll = 1, RepeatOne = 2 };

// how it sounds: equalizer, smooth transitions, remix (only the middle part of each song, one flowing into the next)
constexpr int kEqBands = 10;
extern const float kEqFreq[kEqBands];  // 31 Hz .. 16 kHz
struct Sound {
    bool eqOn = false;
    float eq[kEqBands] = {0};   // dB, -12..12
    float preamp = 0;           // dB
    std::string preset = "Flat";
    int crossfade = 0;          // seconds of overlap between songs (0 = off)
    bool remix = false;
    int remixLen = 45;          // seconds of each song
    int remixFade = 6;          // seconds one flows into the next
    bool level = false;         // even out the volume between songs (ReplayGain / measured)
    bool gapless = true;        // no gap between songs (without a transition: the next one is ready before)
};
nlohmann::json SoundToJson(const Sound& s);
Sound SoundFromJson(const nlohmann::json& j);
struct EqPreset {
    const char* name;
    float g[kEqBands];
};
extern const EqPreset kEqPresets[];
extern const int kEqPresetCount;

// what's playing (a copy, for the UI and the notification)
struct Now {
    bool active = false;   // something is loaded (playing or paused)
    bool playing = false;
    std::string path;
    Track track;
    double pos = 0, dur = 0;
    int index = -1, count = 0;
    int repeat = RepeatOff;
    bool shuffle = false;
    bool external = false;  // a song from elsewhere (listening together / a contact's Shared folder)
    bool follow = false;    // listening together: the host's clock decides
    bool loading = false;   // waiting for it to arrive
    uint64_t extId = 0;
    bool live = false;      // internet radio
    int retries = 0;        // (radio: connection attempts that failed in a row)
};

// internet radio: opens what a URL points to - the stream itself, or an HLS playlist and its segments (any thread;
// stop: set when the player gives the station up, so a waiting read returns)
struct UrlFetch {
    virtual ~UrlFetch() = default;
    virtual void* Open(const std::string& url, std::atomic<bool>* stop) = 0;
    virtual int Read(void* h, uint8_t* buf, int len) = 0;  // bytes, 0 at the end, < 0 error
    virtual void Close(void* h) = 0;
};
// a song that isn't in the queue: listening together, or one from a contact's Shared folder
struct Source {
    uint64_t id = 0;             // another id = another song
    std::string path;            // on this device, or:
    std::function<int(int64_t, uint8_t*, int)> read;  // its bytes as they come (-2: not here yet, ask again)
    int64_t size = 0;
    Track track;
    bool follow = false;         // listening together: Follow() sets the position and play / pause
    double fade = 0;             // blend in from the song that's on over this many seconds (0 = cut)
    int index = -1, count = 0;   // (shown as "3 / 10")
    // internet radio: a live stream from `url` (format "hls" when known), fetched through `io`. It has no end: when
    // it stops or can't be reached, it's tried again (at once, then after 2 s, 4 s, ... up to 30 s)
    std::string url, format;
    std::shared_ptr<UrlFetch> io;
    bool live = false;
};

class Library {
public:
    void Init(const std::string& configDir, const LspModule* mod);
    void Shutdown();

    // folders (the received-files folder is always one of them)
    std::vector<std::string> Folders();
    void AddFolder(const std::string& dir);
    void RemoveFolder(const std::string& dir);
    void SetImplicitFolders(const std::vector<std::string>& dirs);  // received files / Shared
    void Rescan();                // in the background
    bool Scanning() const { return scanning_; }
    int ScanDone() const { return scanDone_; }

    // a snapshot of the tracks (changes after a scan)
    std::vector<Track> Tracks();
    uint64_t Version() const { return version_; }
    bool Find(const std::string& path, Track& out);

    std::vector<Playlist> Playlists();
    bool GainFor(const std::string& path, float& gain);       // volume levelling: known?
    void SetMeasuredGain(const std::string& path, float gain);  // (not over a ReplayGain tag)
    std::string CreatePlaylist(const std::string& name);
    void RenamePlaylist(const std::string& id, const std::string& name);
    void DeletePlaylist(const std::string& id);
    void AddToPlaylist(const std::string& id, const std::vector<std::string>& paths);
    void SetPlaylistPaths(const std::string& id, const std::vector<std::string>& paths);

    // my other devices: the playlists in a form that doesn't depend on where the songs are (artist, title, file
    // name, size), and merging theirs (each playlist: the newer one; deleted ones stay deleted)
    nlohmann::json SyncJson();
    struct MergeResult {
        bool changed = false;  // something here changed
        bool behind = false;   // they lack something newer from here
    };
    MergeResult SyncMerge(const nlohmann::json& j);
    bool ResolveSynced();
    void SetFavourite(const std::string& path, bool on);
    void CountPlay(const std::string& path);  // songs that weren't here before (after a scan): into the playlists
    uint64_t PlaylistsVersion() const { return plVersion_; }

    void Save();
    std::string StatePath() const { return statePath_; }

    static bool IsAudioPath(const std::string& path);
    static std::string DefaultMusicDir();

private:
    void ScanLoop();
    void ReadTags(Track& t);
    std::string CoverFor(const std::string& path, const uint8_t* data, int size);
    std::mutex m_;
    std::condition_variable cv_;
    std::thread th_;
    bool quit_ = false, want_ = false;
    std::atomic<bool> scanning_{false};
    std::atomic<int> scanDone_{0};
    std::atomic<uint64_t> version_{1};
    std::string dir_, file_, coverDir_, statePath_;
    const LspModule* mod_ = nullptr;
    std::vector<std::string> folders_, implicit_;
    std::map<std::string, Track> tracks_;  // path -> track
    std::vector<Playlist> playlists_;
    std::map<std::string, int64_t> gone_;  // deleted playlists (id -> when), so a device that missed it doesn't bring them back
    std::atomic<uint64_t> plVersion_{1};
    void Touch(Playlist& p);
    nlohmann::json Describe(const std::string& path);  // (lock held)
    std::string Resolve(const nlohmann::json& d);        // (lock held) a song here for that description, or "sync:..."
    bool dirty_ = false;
};

class Player {
public:
    void Init(const LspModule* mod, Library* lib);
    void Shutdown();

    void PlayList(const std::vector<std::string>& paths, int start, bool shuffle = false);
    void PlayNext(const std::vector<std::string>& paths);  // after the current one
    void AddToQueue(const std::vector<std::string>& paths);
    void Toggle();
    void SetPlaying(bool on);
    void Next();
    void Prev();
    void Seek(double seconds);
    void Stop();                  // nothing loaded any more
    void SetRepeat(int r);
    void SetShuffle(bool on);
    void SetVolume(float v);      // ducking (0..1)
    void SetStopAfterCurrent(bool on) { stopAfter_ = on; }  // the sleep timer: stop when this song ends
    bool StopAfterCurrent() const { return stopAfter_; }
    Sound GetSound();
    void SetSound(const Sound& s);
    // a movie / call / chat audio is on: music pauses, and goes on afterwards
    void SetInterrupted(bool on);
    // Android, during a call: the call's sound output adds the music played on purpose to its own (audio thread)
    void MixInto(int16_t* mono, int frames);
    Now GetNow();
    std::vector<std::string> Queue();  // in playing order

    // called on any change (track, play / pause, ...) from any thread: the UI wakes, the notification updates
    std::function<void()> onChange;

    // a song from elsewhere instead of the queue (the queue stays for afterwards). The UI thread never waits: the
    // player's thread opens it
    void PlayExternal(const Source& s, double pos = 0, bool playing = true);
    void Follow(double pos, bool playing);   // listening together: where the host's clock is
    void EndExternal();                      // back to own music (paused)
    bool ExternalEnded(uint64_t id);         // that song played to its end
    void SetExternalTrack(uint64_t id, const Track& t);  // radio: the song now on (its title from the stream)
    // while a song from elsewhere is on, the controls go here (1 toggle, 2 next, 3 prev, 6 stop, 7 seek s); true = handled
    std::function<bool(int cmd, double arg)> externalCtl;

    // remembered between runs (paused)
    void LoadState(const std::string& path);
    void SaveState();

private:
    void Loop();
    void LoopExternal();           // (m_ held)
    bool ExtCtl(int cmd, double arg);
    bool EnsureDevice();           // (m_ held)
    void Render(float* out, int frames);  // (audio thread)
    void UpdateMix();              // (m_ held)
    bool playInCall_ = false;      // play pressed during a call: it plays (quieter) (m_)
    int64_t interruptedAt_ = 0;
    std::atomic<bool> inCall_{false}, mixIntoCall_{false};
    struct ExtStream;              // the stream the decoder reads (alive until its player is closed)
    bool ext_ = false;             // a song from elsewhere is on (m_)
    Source ex_;                    // which (m_)
    uint64_t exOpen_ = 0;          // the one open in p_
    bool exOpening_ = false, exEnded_ = false;
    int64_t exRetryAt_ = 0;        // radio: when to try the station again
    int exRetries_ = 0;
    double exPos_ = 0;             // Follow(): the clock
    bool exPlay_ = false;
    int64_t exAt_ = 0, exLastSeek_ = 0;
    std::shared_ptr<ExtStream> exStream_, exPending_, exStreamOld_;  // (old: the song fading out)
    bool OpenAt(int index, double pos);  // (m_ held)
    void CloseLocked();
    // a player let go of is closed on a thread of its own: closing waits for its decoder, which may be waiting on the
    // network (a station) - never with m_ held, where the screen would wait too. The streams it may still be reading
    // through are kept with it until then. (m_ held)
    void Retire(LspPlayer* p, std::shared_ptr<ExtStream> also = nullptr);
    void Closer();
    struct Retired {
        LspPlayer* p;
        std::vector<std::shared_ptr<ExtStream>> keep;
    };
    std::thread closerTh_;
    std::mutex cm_;
    std::condition_variable ccv_;
    std::deque<Retired> retired_;
    bool closerQuit_ = false;
    // transitions (m_ held): the next song starts while this one fades out
    int NextIndex();
    std::atomic<bool> stopAfter_{false};
    double PickFrag(double dur) const;  // remix: a random start for a song's part
    double FragStart(double dur) const;
    double fragA_ = 0, fragB_ = 0;      // remix: where the playing / the incoming song's part starts
    double FragEnd(double dur) const;
    void StartFade(int next, double seconds);
    void FinishFade();
    void CancelFade();
    void UpdateEq();                 // coefficients from snd_ (m_ held)
    void Changed();
    static void AudioCb(void* u, uint8_t* stream, int len);
    const LspModule* mod_ = nullptr;
    Library* lib_ = nullptr;
    // volume levelling: the gain of the playing / incoming song (linear), and the playing one measured as it plays
    float gainA_ = 1, gainB_ = 1;
    double msSum_ = 0;
    int64_t msN_ = 0;
    std::string measPath_, measNext_;
    float LevelFor(const std::string& path);   // (m_ held)
    void MeasureDone();                        // (m_ held) the measured song: its loudness into the library
    std::mutex m_;      // state
    std::mutex pm_;     // the decoder, between this and the audio callback
    std::thread th_;
    std::atomic<bool> quit_{false};
    uint32_t dev_ = 0;
    LspPlayer* p_ = nullptr;
    LspPlayer* pn_ = nullptr;         // the next song, fading in (pm_)
    int nextAt_ = -1;
    double nextDur_ = 0;
    int64_t fadeTotal_ = 0, fadePos_ = 0;  // frames (pm_)
    bool gapless_ = false;                 // pn_ is the next song waiting to go on the moment this one ends (pm_)
    std::atomic<bool> fadeDone_{false};
    Sound snd_;
    // equalizer (pm_): peaking filters per band, both channels
    bool eqActive_ = false;
    float eqGain_ = 1;
    float eqC_[kEqBands][5] = {};
    float eqZ_[kEqBands][2][2] = {};
    std::vector<float> mixBuf_;
    std::vector<std::string> queue_;  // in the order they were given
    std::vector<int> order_;          // playing order (shuffled or not) -> index into queue_
    int at_ = -1;                     // position in order_
    bool playing_ = false, wantPlay_ = false, interrupted_ = false, pausedByInterrupt_ = false;
    int repeat_ = RepeatOff;
    bool shuffle_ = false;
    std::atomic<float> volume_{1.0f};
    double pendingSeek_ = -1;
    double dur_ = 0;
    std::string statePath_;
    double savedPos_ = 0;
    int64_t lastSave_ = 0;
};

Library& Lib();
Player& Play();
// Windows: the keyboard's media keys (play / pause, next, previous) while music is loaded - from anywhere
void MediaKeys(bool on);

}  // namespace music
