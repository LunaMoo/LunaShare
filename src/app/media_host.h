// Loads the optional media module (calls) and drives the audio devices for it.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"
#include "lunashare_media.h"
#include "lunashare_video.h"

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
    // Phones: where call audio goes (earpiece, loudspeaker, Bluetooth, wired headset). Empty on desktop.
    struct Route { int id; std::string kind, name; };
    void RefreshRoutes();
    const std::vector<Route>& Routes() const { return routes_; }
    int CurrentRoute() const { return currentRoute_; }
    const Route* Current() const;
    void SetRoute(int id);
    static std::string RouteLabel(const Route& r);
    LsmStats Stats();
    std::string audioError;

    static std::vector<std::string> Devices(bool capture);

    // ---- video (optional second module, lunashare_video)
    bool VideoAvailable() const { return vmod_ != nullptr; }
    std::string videoLoadInfo;
    bool VideoRunning() const { return videoRunning_; }   // a call where both sides can do video
    void WantCameraAtStart(bool on) { wantCamera_ = on; } // video call: camera turns on when the call connects
    bool CameraOn() const { return camOn_; }
    bool SetCameraOn(bool on);
    bool RemoteCameraOn() const { return remoteCam_; }
    std::vector<LsvCamera> Cameras();
    void SelectCamera(const std::string& id);
    void NextCamera();                                     // flip between front and back
    std::string CurrentCameraName();
    std::vector<LsvControl> Controls();
    void SetControl(int id, float v);
    LsvStats VideoStats();
    void ApplyVideoQuality();                              // after the quality setting changed
    static LsvSettings Preset(int quality, bool adaptive);
    static const char* QualityName(int q);
    struct VideoTex {
        unsigned tex = 0;
        int w = 0, h = 0;
        bool valid = false;
        int64_t lastMs = 0;
    };
    const VideoTex& RemoteVideo() const { return tex_[LSV_REMOTE]; }
    const VideoTex& LocalVideo() const { return tex_[LSV_LOCAL]; }

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
    // video
    struct Slot {
        std::mutex m;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool fresh = false;
    };
    void LoadVideo();
    void StartVideo();
    void StopVideo();
    void UploadVideo();
    static void Convert(const LsvImage& img, Slot& s);
    static int VSend(void* u, const uint8_t* d, size_t n);
    static void VImage(void* u, int src, const LsvImage* img);
    static void VState(void* u, int on);
    static size_t VQueued(void* u);
    static void VLog(void* u, const char* m);
    const LsvModule* vmod_ = nullptr;
    void* vlib_ = nullptr;
    LsvHost vhost_{};
    std::atomic<bool> videoRunning_{false};
    bool wantCamera_ = false;
    std::atomic<bool> camOn_{false}, remoteCam_{false};
    std::string camId_;
    Slot slots_[2];
    VideoTex tex_[2];

    std::vector<Route> routes_;
    int currentRoute_ = -1;
    uint32_t routesTick_ = 0;
    std::atomic<float> micGain_{1.0f};
    uint32_t settingsTick_ = 0;
    void ApplyMicGain(int16_t* pcm, int n);
    std::atomic<bool> micTest_{false};
    std::atomic<float> micTestLevel_{0};
    uint64_t toneSample_ = 0;
    bool audioInit_ = false;
};

MediaHost& Media();
