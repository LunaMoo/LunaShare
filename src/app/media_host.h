// Loads the optional media module (calls) and drives the audio devices for it.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"
#include "lunashare_media.h"

class MediaHost {
public:
    // Looks for lunashare_media next to the executable (or links it statically on iOS). Call before engine.Start().
    bool Load();
    bool Available() const { return mod_ != nullptr; }
    std::string ModuleName() const;
    std::vector<std::string> Caps() const;
    std::string loadInfo;  // where it was found / why not

    void Attach(ls::Engine* e);  // hooks engine.onMedia
    // Call once per frame (UI thread): follows the engine's call state (ringing tones, start/stop audio).
    void Update();
    void Shutdown();

    void SetMute(bool m);
    bool Muted() const { return mute_; }
    void SetSpeaker(bool on);  // phones: loudspeaker vs earpiece
    bool Speaker() const { return speaker_; }
    LsmStats Stats();
    std::string audioError;

    static std::vector<std::string> Devices(bool capture);

    // microphone test from the settings page
    void StartMicTest();
    void StopMicTest();
    bool MicTesting() const { return micTest_; }
    float MicTestLevel() const { return micTestLevel_; }

private:
    enum class Mode { Off, Ring, Ringback, Call, MicTest };
    void SetMode(Mode m);
    bool OpenDevices(bool capture, bool playback);
    void CloseDevices();
    static void CaptureCb(void* u, uint8_t* stream, int len);
    static void PlaybackCb(void* u, uint8_t* stream, int len);
    static void SendCb(void* u, const uint8_t* data, size_t len);
    static void LogCb(void* u, const char* msg);

    const LsmModule* mod_ = nullptr;
    void* lib_ = nullptr;
    ls::Engine* eng_ = nullptr;
    std::atomic<Mode> mode_{Mode::Off};
    uint32_t capDev_ = 0, playDev_ = 0;
    std::atomic<bool> mute_{false};
    bool speaker_ = false;
    std::atomic<bool> micTest_{false};
    std::atomic<float> micTestLevel_{0};
    uint64_t toneSample_ = 0;
    bool audioInit_ = false;
};

MediaHost& Media();
