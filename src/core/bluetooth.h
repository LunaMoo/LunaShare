// Bluetooth RFCOMM transport. Produces ordinary socket handles which the engine treats like TCP.
//  - Windows: Winsock AF_BTH + SDP service registration (service UUID kBtServiceUuid).
//  - Linux:   BlueZ RFCOMM sockets on a fixed channel (Settings::btChannel), device list via bluetoothctl.
//  - Android: Java BluetoothServerSocket/BluetoothSocket bridged into a native socketpair (see HostBridge).
//  - macOS/iOS: not implemented yet (IOBluetooth / no RFCOMM on iOS) -> Supported() == false.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "net.h"

namespace ls {

struct BtDevice {
    std::string addr;  // "AA:BB:CC:DD:EE:FF"
    std::string name;
    bool paired = false;
};

// Hooks provided by the app layer on platforms where native code cannot reach the radio directly
// (Android via JNI). All optional.
struct HostBridge {
    std::function<bool()> btAvailable;
    std::function<bool(bool enable)> btSetListening;
    std::function<std::vector<BtDevice>(bool inquiry)> btDevices;
    std::function<void(const std::string& addr)> btConnectAsync;  // result arrives via Engine::AdoptSocket
    std::function<bool()> hotspotStart;                           // result via Engine::SetHotspotInfo
    std::function<void()> hotspotStop;
    std::function<void(const std::string& ssid, const std::string& pass)> wifiJoin;
    std::function<void(const std::string& path)> openFile;
    std::function<void(const std::string& path)> openFolder;
    std::function<void(const std::string& text)> shareText;  // Android: the system's "Share" sheet with this text (an invite)
    std::function<void()> requestPermissions;
    std::function<void()> openAppSettings;  // Android: this app's page in the system settings (permissions)
    std::function<void(bool inCall)> callAudioMode;   // Android: MODE_IN_COMMUNICATION (earpiece routing, system AEC)
    // system notifications (Android) and running in the background
    std::function<void(const std::string& key, const std::string& title, const std::string& text, const std::string& avatar)> notifyMessage;
    std::function<void(const std::string& key)> clearChatNotification;
    std::function<void(const std::string& key, const std::string& title, bool video, const std::string& avatar)> notifyCall;
    std::function<void()> cancelCallNotification;
    std::function<void(bool on)> backgroundService;   // Android foreground service (keeps the process connected)
    // the music player's state for the system (Android: media notification, lock screen, headset buttons)
    std::function<void(bool active, bool playing, const std::string& title, const std::string& artist, const std::string& album,
                       const std::string& cover, double dur, double pos)>
        musicState;
    std::function<bool()> batteryRestricted;          // Android: battery optimisation may cut the network
    std::function<void()> requestBatteryExemption;
    std::function<void()> moveToBack;                 // Android: back on the home screen -> background, don't close
    // watching a movie: Android turns sideways (landscape, even with rotation locked) and hides the system bars;
    // computers go full screen (setFullscreen); keepAwake: no screen saver / screen off while it plays
    std::function<void(bool on)> movieMode;
    std::function<void(bool portrait)> movieOrientation;  // Android: upright / sideways while a movie shows
    std::function<float()> mediaVolume;                 // Android: the media volume 0..1
    std::function<void(float)> setMediaVolume;
    std::function<void(float)> setScreenBrightness;     // Android: this window's brightness 0..1 (< 0: the system's)
    std::function<void(bool allowed, int w, int h)> moviePip;  // Android: leaving the app keeps the movie in a small window
    std::function<void()> enterPip;
    std::function<bool()> inPip;
    std::function<bool(const std::string& url, std::string& body)> httpsGet;
    // Android: a URL read as it comes (internet radio) - open: > 0 handle, with the status, the headers ("name: value"
    // lines) and the address after redirects; read: bytes, 0 at the end, < 0 on an error; any thread
    std::function<int(const std::string& url, bool icy, int& status, std::string& headers, std::string& finalUrl, std::string& err)> netOpen;
    std::function<int(int id, uint8_t* buf, int len)> netRead;
    std::function<void(int id)> netClose;
    std::function<bool()> biometricAvailable;                                         // Android: fingerprint / face set up
    std::function<void(const std::string& title, const std::string& cancel)> biometricAsk;  // success: notify action "unlock"
    std::function<void(bool on)> secureWindow;  // Android: no screenshots / app switcher preview (the app lock is on)  // Android: a small HTTPS GET (any thread)
    // borderless: a window that leaves one pixel of the screen uncovered (Windows: then neither Windows nor the
    // graphics driver treat it as a full-screen game, so no Auto HDR / RTX HDR)
    std::function<void(bool on, bool borderless)> setFullscreen;
    std::function<void(bool on)> keepAwake;
    std::function<float()> keyboardFraction;          // Android: the part of the app's height the on-screen keyboard covers (0 = none)
    std::function<bool()> isMetered;                  // on mobile data (or another metered network)
    // Android call audio routes: "current id\n" + lines "id\tkind\tname" (kind: earpiece/speaker/bluetooth/wired/usb)
    std::function<std::string()> callRoutes;
    std::function<void(int id)> setCallRoute;
    // Android: JavaVM* and a global ref to the VideoEngine class (calls build only), for the video module
    std::function<void*()> javaVm;
    std::function<void*()> videoClass;
    std::function<void*()> playerClass;  // global ref to PlayerEngine (all builds)
    std::function<void*()> emojiClass;   // global ref to EmojiRenderer (all builds)
};
HostBridge& Bridge();

class Bluetooth {
public:
    using OnSocket = std::function<void(SocketHandle s, const std::string& addr)>;
    ~Bluetooth() { StopServer(); }

    bool Supported() const;
    bool StartServer(int channel, OnSocket cb, std::string* err);
    void StopServer();
    bool Listening() const { return listening_; }
    std::vector<BtDevice> Devices(bool inquiry);  // may block for several seconds when inquiry == true
    // Blocking connect. On Android returns kInvalidSocket and connects asynchronously (asyncStarted = true).
    SocketHandle Connect(const std::string& addr, int channel, std::string* err, bool* asyncStarted);

private:
    void ServerLoop();
    std::atomic<bool> listening_{false};
    std::atomic<bool> stop_{false};
    SocketHandle server_ = kInvalidSocket;
    std::thread thread_;
    OnSocket cb_;
    void* registration_ = nullptr;  // Windows SDP record
};

}  // namespace ls
