// Loads the optional player module and plays audio/video files from the chat: thumbnails + lengths (probed in the
// background), one file playing at a time through its own audio device, pictures drawn via MediaHost's GPU path.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lunashare_player.h"

class PlayerHost {
public:
    bool Load();
    bool Available() const { return mod_ != nullptr; }
    const LspModule* Module() const { return mod_; }
    std::string loadInfo;
    void Shutdown();
    void Update();  // UI thread, once per frame

    static bool IsVideoPath(const std::string& path);
    static bool IsAudioPath(const std::string& path);
    bool Playable(const std::string& path) const { return mod_ && (IsVideoPath(path) || IsAudioPath(path)); }

    struct Meta {
        bool done = false, ok = false;
        LspInfo info{};
        unsigned tex = 0;  // thumbnail (RGBA), 0 if none
        int tw = 0, th = 0;
        int tries = 0;       // probes that failed (phones: the decoders may just be busy - tried again a little later)
        uint64_t failedAt = 0;
    };
    // probes in the background the first time; returns what is known so far
    const Meta& GetMeta(const std::string& path);

    // playback of one file at a time
    bool Play(const std::string& path);  // opens if needed and plays
    void Pause();
    void Toggle(const std::string& path);
    void Seek(double seconds);
    void Stop();
    bool IsCurrent(const std::string& path) const { return !cur_.empty() && cur_ == path; }
    // watch together: a file arriving from another device. Opening reads its beginning (and maybe its end) over the
    // network, so it happens in the background: OpenState() says how far it got. `read` must stay valid until Stop().
    void OpenStream(const std::string& id, int64_t size, std::function<int(int64_t, uint8_t*, int)> read);
    enum class Opening { None, Busy, Failed, Ready };
    Opening OpenState();
    void SetPlaying(bool on);
    // chats: start again at the end (not for watching together / the movie player: their clock decides)
    void SetLoop(bool on) { loop_ = on; }
    bool Loop() const { return loop_; }
    // sound tracks and subtitles of what's playing
    std::vector<LspTrack> Tracks() const;
    void SelectTrack(int kind, int id);
    int AddSubtitles(const std::string& path);  // -1 = can't read it
    void AddSidecarSubtitles(const std::string& videoPath);  // movie.srt, movie.pl.srt... next to the file
    struct Sub {
        std::string text;
        unsigned tex = 0;  // picture subtitles
        int w = 0, h = 0;
        float x = 0, y = 0, sw = 0, sh = 0;
    };
    const Sub& Subtitle() const { return sub_; }
    bool Playing() const;
    double Position() const;
    double Duration() const { return info_.duration; }
    const LspInfo& Info() const { return info_; }
    int State() const;

private:
    void Worker();
    bool OpenAudio();
    void CloseAudio();
    static void AudioCb(void* u, uint8_t* stream, int len);

    const LspModule* mod_ = nullptr;
    void* lib_ = nullptr;
    LspHost host_{};

    // probing
    std::mutex mm_;
    std::condition_variable mcv_;
    std::map<std::string, Meta> meta_;
    struct Pending {
        std::string path;
        LspInfo info{};
        bool ok = false;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
    };
    std::deque<std::string> queue_;
    std::vector<Pending> done_;
    std::thread worker_;
    bool quit_ = false;

    // playback
    LspPlayer* p_ = nullptr;
    std::string cur_;
    LspInfo info_{};
    uint32_t dev_ = 0;
    std::mutex pm_;  // guards p_ against the audio callback
    // stream being opened in the background
    std::thread opener_;
    std::atomic<int> openState_{0};
    LspPlayer* opened_ = nullptr;
    LspInfo openedInfo_{};
    std::string openingId_;
    std::function<int(int64_t, uint8_t*, int)> streamRead_;
    std::atomic<bool> streamStop_{false};
    bool loop_ = false;
    uint64_t endedAt_ = 0;  // when the current file reached its end (the audio device is let go a bit later)  // Stop(): reads waiting for the network give up
    LspStream stream_{};
    Sub sub_;
    int subSerial_ = -1;
    void UpdateSubtitle();
};

PlayerHost& Player();
