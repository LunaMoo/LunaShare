// Loads the optional media module (calls) and drives the audio devices for it.
#pragma once
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "engine.h"
#include "lunashare_media.h"
#include "lunashare_video.h"
#include "imgui.h"

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
    static LsvSettings Preset(int res, int fps, bool adaptive);
    static constexpr int kResCount = 7;
    static const char* ResName(int r);       // "720p (1280x720)" ...
    static const int* FpsList(int* n);       // 15, 24, 30, 60, 120
    static std::string RateText(int bps);    // "5.0 Mbit/s"
    struct VideoTex {
        unsigned tex = 0;    // RGBA (CPU path) or the Y plane (GPU path)
        unsigned uvTex = 0;  // GPU path: interleaved U/V plane
        bool gpu = false;
        int w = 0, h = 0;    // upright display size
        int sw = 0, sh = 0;  // texture size
        int rotation = 0;
        bool mirror = false;
        bool valid = false;
        int64_t lastMs = 0;
    };
    // Draws a video picture into [p0,p1]: fit (letterbox) or cover (crop). Converts YUV on the GPU when it can.
    void DrawVideo(ImDrawList* dl, const VideoTex& t, ImVec2 p0, ImVec2 p1, bool cover, int extraRotation = 0);
    // turn the received picture by 90 degrees (dir -1 left, +1 right); also applies to the virtual camera
    void RotateRemote(int dir) { remoteRotAdj_ = ((remoteRotAdj_ + dir * 90) % 360 + 360) % 360; }
    int RemoteRotation() const { return remoteRotAdj_; }
    void SetGlsl(const char* glslVersion);  // after the GL context exists; enables the GPU path
    const VideoTex& RemoteVideo() const { return tex_[LSV_REMOTE]; }
    const VideoTex& LocalVideo() const { return tex_[LSV_LOCAL]; }

    // virtual camera (Windows 11): received video shows up as "LunaShare Camera" in other apps
    bool VcamAvailable() const { return vcamAvail_; }
    void SetVirtualCamera(bool on);  // runs in the background (may show a one-time admin prompt)
    bool VcamOn() const { return vcamOn_; }
    bool VcamBusy() const { return vcamBusy_; }
    std::string VcamStatus();

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
        bool fresh = false;
        bool gpu = false;
        std::vector<uint8_t> rgba;  // CPU path: upright RGBA, w x h
        int w = 0, h = 0;
        std::vector<uint8_t> y, uv; // GPU path: tight Y (sw x sh) and U/V pairs ((sw+1)/2 x (sh+1)/2)
        int sw = 0, sh = 0, rotation = 0;
        bool mirror = false;
    };
    static void CopyPlanes(const LsvImage& img, Slot& s);
    std::atomic<bool> gpuYuv_{false};
    std::string glsl_;
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
    bool vcamAvail_ = false, vcamChecked_ = false;
    std::atomic<int> remoteRotAdj_{0};
    std::atomic<bool> vcamOn_{false}, vcamBusy_{false};
    std::mutex vcamM_;
    std::string vcamStatus_;
    std::thread vcamThread_;

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
