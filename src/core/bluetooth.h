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
    std::function<void()> requestPermissions;
    std::function<void(bool inCall)> callAudioMode;   // Android: MODE_IN_COMMUNICATION (earpiece routing, system AEC)
    // system notifications (Android) and running in the background
    std::function<void(const std::string& key, const std::string& title, const std::string& text, const std::string& avatar)> notifyMessage;
    std::function<void(const std::string& key)> clearChatNotification;
    std::function<void(const std::string& key, const std::string& title, bool video, const std::string& avatar)> notifyCall;
    std::function<void()> cancelCallNotification;
    std::function<void(bool on)> backgroundService;   // Android foreground service (keeps the process connected)
    std::function<bool()> batteryRestricted;          // Android: battery optimisation may cut the network
    std::function<void()> requestBatteryExemption;
    std::function<void()> moveToBack;                 // Android: back on the home screen -> background, don't close
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
