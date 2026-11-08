// Music player: a library of the user's music folders (tags + covers read by the player module), playlists, and a
// player with its own audio device and thread, so music goes on while the app is in the background (Android:
// a media notification / lock screen controls through the bridge).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

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
    const std::string& AlbumArtist() const { return albumArtist.empty() ? artist : albumArtist; }
};

struct Playlist {
    std::string id, name;
    std::vector<std::string> paths;
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
};
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
    std::string CreatePlaylist(const std::string& name);
    void RenamePlaylist(const std::string& id, const std::string& name);
    void DeletePlaylist(const std::string& id);
    void AddToPlaylist(const std::string& id, const std::vector<std::string>& paths);
    void SetPlaylistPaths(const std::string& id, const std::vector<std::string>& paths);

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
    Sound GetSound();
    void SetSound(const Sound& s);
    // a movie / call / chat audio is on: music pauses, and goes on afterwards
    void SetInterrupted(bool on);
    Now GetNow();
    std::vector<std::string> Queue();  // in playing order

    // called on any change (track, play / pause, ...) from any thread: the UI wakes, the notification updates
    std::function<void()> onChange;

    // remembered between runs (paused)
    void LoadState(const std::string& path);
    void SaveState();

private:
    void Loop();
    bool OpenAt(int index, double pos);  // (m_ held)
    void CloseLocked();
    // transitions (m_ held): the next song starts while this one fades out
    int NextIndex();
    double FragStart(double dur) const;
    double FragEnd(double dur) const;
    void StartFade(int next, double seconds);
    void FinishFade();
    void CancelFade();
    void UpdateEq();                 // coefficients from snd_ (m_ held)
    void Changed();
    static void AudioCb(void* u, uint8_t* stream, int len);
    const LspModule* mod_ = nullptr;
    Library* lib_ = nullptr;
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
