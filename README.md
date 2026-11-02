# LunaShare

A chat app built around file sharing. It is written in C++ with [Dear ImGui](https://github.com/ocornut/imgui) on SDL2 and runs on Windows, Linux, macOS and Android, with an iOS port planned.
Devices on the same network find each other automatically. Transfers can be paused and resumed at any time, and every file is
SHA-256 hashed before sending and again after receiving, so you know it arrived intact.

| Desktop | Phone |
|---|---|
| ![desktop](docs/desktop-chat.png) | ![phone](docs/phone-chat.png) |
| ![viewer](docs/viewer.png) | ![phone viewer](docs/phone-viewer.png) |
| ![file picker](docs/file-picker-gallery.png) | ![phone file picker](docs/phone-file-picker.png) |

## Features

**Sharing**
- Drag and drop files or whole folders onto the window. Folder structure is kept.
- Pause and resume from either side, at any time. Transfers also survive disconnects and app restarts:
  the receiver keeps a `.part` file and both sides continue from where they stopped.
- Integrity checks: the sender computes the SHA-256 before offering, every chunk carries a CRC-32, and the receiver computes the SHA-256 of the finished file. The chat card shows *"SHA-256 verified on both sides"*. If the hashes don't match, the partial file is discarded and you can **Retry**.
- Several files stream in parallel (configurable). There's an optional upload speed limit and a free-space check before accepting.
- Before sending, the app checks that the source file hasn't changed (size + mtime) since it was hashed.

**Chat**
- Text chat per device. Messages queue while the other device is offline and show delivered/queued state.
- Selecting and copying: click to select, Ctrl/Shift+click for several, Ctrl+C copies them (with timestamps when several are selected). Double-click or *Select text…* opens a view where you can select part of a message. On phones, long-press enters selection mode.
- Pictures appear as thumbnails in the chat (EXIF rotation is respected). Tapping one opens a **full-screen viewer**:
  swipe left/right (or arrow keys and the side buttons) to go to the previous/next picture, pinch, mouse wheel or +/- to zoom,
  double-tap to zoom in, drag to pan. Toggle between *this chat's pictures* and *all shared pictures*.
- Incoming offers appear in the chat and as notifications with Accept/Decline. Trusted devices are accepted automatically.

**Picking files**
- The built-in file picker works the same on desktop and phones. You can switch between a **list** and a **gallery of thumbnails**,
  with a *Pictures only* filter, sorting by name / newest / largest, and adjustable tile size.
- Gallery tiles load fast because they use the small preview that cameras embed in every JPEG (EXIF), with the photo's
  rotation applied. Images without one are decoded in the background, and what's on screen loads first.
- Tap tiles to select several (Ctrl/Shift-click on desktop), *Select all*, see the selected count and total size.
  Long-press (right-click on desktop) a picture to preview it large; long-press a folder to send the whole folder.
  Quick shortcuts on Android: Camera, Pictures, Screenshots, Download.

**History & re-sharing**
- Every sent and received file goes into a searchable history (name, device, hash). Select any number of entries and
  **share them with another device** in one click. You can also do this from a chat card or the picture viewer.

**Devices**
- Every device has a permanent random ID (UUID). User name, host name, platform and MAC addresses are stored as extra information.
  MACs can't be the primary key because phones randomise or hide them. If a known MAC shows up with a new ID (app reinstalled),
  the nickname is carried over.
- A list of every device you've ever connected to, with **custom nicknames**, a trusted flag (auto-accept), blocked, notes, last address, and Bluetooth address.

**Connectivity**
- **LAN, no setup:** two discovery methods running side by side, then a TCP connection (port 47820, falls back to the next free one):
  - **Bonjour / mDNS** (`_lunashare._tcp`, RFC 6762/6763). It uses the system Bonjour API on macOS/iOS and a built-in responder/browser on
    Windows, Linux and Android that shares port 5353 with the OS. Works on most routers, and is the method iPhones/iPads will use.
  - **UDP broadcast + multicast beacons** (port 47821). Fast, and works on networks that filter mDNS. Not available on iOS.

  Both can be switched on or off in Settings. Known devices are reconnected automatically when they still have work pending.
- **Phone-to-phone with no Wi-Fi:** one phone starts a hotspot (Android LocalOnlyHotspot) and shows a Wi-Fi QR code the other phone can scan
  with its camera. If the two are already connected over Bluetooth, one tap sends the hotspot login across and the other phone
  joins automatically. The app then switches to full Wi-Fi speed.
- **Bluetooth RFCOMM:** Windows (Winsock + SDP), Linux (BlueZ, fixed channel), Android (Java bridge). Good for chat, small files, and handing over hotspot logins.
- Manual connect by `ip[:port]`.
- **Over the internet (optional):** run the small **rendezvous server**. There's a ready-made **OPNsense plugin**, or it runs on
  any Linux/Windows/macOS box. Devices with the same room name and password find each other through it and then connect
  **directly peer-to-peer** (UDP hole punching + KCP, NAT-PMP / port-forward assisted). The server never relays chat or files.
  See [docs/INTERNET.md](docs/INTERNET.md).

**Security**
- Every connection is end-to-end encrypted: an X25519 key exchange per connection (forward secrecy),
  XChaCha20-Poly1305 for every frame, and a per-device EdDSA identity key ([Monocypher](https://monocypher.org)).
- The identity key signs the key exchange, so a man in the middle can't relay it. Keys are pinned to the device on first contact. If a known device later shows up
  with a different key, the connection is refused and you have to review it (*Accept new key* or *Block*).
- Each pair of devices has a **safety number** (30 digits, identical on both screens). Compare it once and mark the device as *verified*.
- Bluetooth links and hotspot logins sent over them are encrypted the same way. Only the discovery beacons (name, ID, port) are plain.

**Voice calls (optional module)**
- Calls live in a separate plug-in, `lunashare_media` (`lunashare_media.dll` / `liblunashare_media.so` / `.dylib`), which is loaded at
  startup only if it's present. Without it LunaShare is the plain chat + file app, and the call button doesn't appear. Delete the file to remove calls.
- The call button sits in the chat header. It's only shown when **both** devices have the module, since each device announces its capabilities.
- Calls go peer-to-peer over the same encrypted connection as chat (LAN, hotspot, internet/KCP, and in principle Bluetooth, though there it can be choppy).
- Audio: Opus (16 kHz, 12–48 kbit/s, in-band FEC), an adaptive jitter buffer with loss concealment, and SpeexDSP for echo cancellation, noise suppression and auto gain.
- During a call, file transfers to that device are throttled so they don't cause audio stutter.
- Call bar: timer, mic/speaker levels, link stats (loss, jitter, delay), and buttons for mute, speaker (phones) and hang up. Incoming calls get a ringtone and a full-screen answer screen on phones.
- Calls are logged in the chat (*Outgoing call – 2:31*, *Missed call*, …).
- *Settings → Calls*: choose input and output devices, turn echo cancellation, noise suppression and auto gain on or off, set quality, and run a microphone test.
- Android ships as two APKs: **lite** (no modules, no microphone or camera permission) and **calls** (`org.lunamoo.lunashare.calls`, voice + video).
- The module has a small, versioned C ABI (`src/media_api/lunashare_media.h`), so a video module can be added later the same way.

![Voice call](docs/voice-call.png)

**Video calls (optional module, Windows and Android for now)**
- A second plug-in, `lunashare_video` (`lunashare_video.dll` / `liblunashare_video.so`), sits next to the voice module. It adds the
  video call button when both devices have it, and any call between two video-capable devices can turn the camera on or off.
- The module bundles no codec library and uses the system's own H.264: Media Foundation on Windows (a GPU encoder when one works,
  otherwise Microsoft's software encoder) and MediaCodec on Android (hardware).
- **Android battery use:** the camera feeds the hardware encoder directly, so frames never pass through the CPU. The self-view is a
  small 15 fps stream, and the camera runs at a fixed frame rate.
- **Quality:** choose the resolution (360p, 540p, 720p, 1080p, 1440p or 4K) and frame rate (15, 24, 30, 60 or 120 fps) in *Settings → Calls*
  or during a call. The bitrate follows from both: about 5 Mbit/s for 1080p at 30 fps, and about 60 Mbit/s for 4K at 120 fps, which needs fast Wi-Fi.
  If a camera or encoder can't do a combination, the closest one it can do is sent, and the call stats show the actual numbers.
  On Android, 120 fps uses the camera's high-speed mode, which has no self-view stream; the other side still gets the full rate.
  Colour conversion, rotation and mirroring run on the GPU. On Windows, received video is decoded by the graphics card.
  8K (7680x4320) is offered as well. H.264 at 8K is beyond most decoders, so if the receiving device can't decode
  a size, it automatically asks the sender for 4K (or 1080p) instead. With *Lower quality when the connection is slow* on, the bitrate follows what the link can actually carry
  (measured four times a second), so the picture gets softer instead of lagging. Voice always goes ahead of video.
- **Camera panel (during a call):**
  - Phones: zoom (including the ultra-wide lens, e.g. 0.6x, where the phone has one), switching between cameras, exposure
    compensation, manual ISO and shutter, auto or manual focus, flashlight, white balance and stabilization. What's available
    depends on the camera.
  - Webcams: zoom, focus, exposure and brightness, when the camera supports them.
- **Codecs:** H.264 always works. Devices also announce whether they can do HEVC and AV1, and the sender picks the best codec both sides support
  (AV1, then HEVC, then H.264). Phones only offer codecs they encode and decode in hardware. On Windows, HEVC and AV1 are decoded with a
  trimmed FFmpeg (`avcodec-61.dll` and `avutil-59.dll` in `modules/`, LGPL). It uses the graphics card's decoder through D3D11VA
  when there is one, so the paid Microsoft HEVC extension isn't needed. Without a GPU decoder, HEVC is decoded on the CPU, and AV1 is
  only offered when the GPU can decode it. *Use only H.264* in *Settings → Calls* turns this off if something misbehaves.
- **Picture orientation:** phones read their position from the motion sensor, so the picture is upright even with auto-rotate off.
  You can also turn the received picture left or right with the buttons in the corner of the video, and that also applies to the virtual camera.
- **Lost or late video** isn't buffered for long. The receiver asks for a fresh keyframe, so the picture recovers within a moment.
- **Virtual camera (Windows 11):** turn on *Settings → Calls → Virtual camera* and the PC gets a webcam called **LunaShare Camera**.
  Teams, Zoom, OBS, Discord and other apps can pick it. It shows the video received in a call, for example your phone's
  camera, and a dark picture otherwise. Windows asks for administrator permission once, to install the small camera
  component (`lunashare_vcam.dll`) into `%ProgramData%\LunaShare`, where the Windows camera service can load it.
  It offers 360p up to 4K, at 30 or 60 fps.
  The **webcam button** in the chat header starts a camera-only session without a full call:
  - On the PC (with the virtual camera on), it asks the other device for its camera. The phone has to allow it.
  - On a phone chatting with a Windows PC, it offers the phone's camera to that PC.
  - Microphones start muted on both sides, and LunaShare Camera shows the picture.
- On phones a video call opens full screen with the call buttons at the bottom. On desktop the video sits above the chat and can be enlarged.

![Video call](docs/video-call.png)
![Camera panel](docs/video-camera-panel.png)

**Settings**
- Download folder, auto-accept rules, discovery, port, speed limit, chunk size, parallel files, Bluetooth, theme (dark/light/classic), UI scale.
- Layout: *automatic*, *desktop*, or *phone*. Phones get a vertical UI with large buttons, a bottom navigation bar, drag-to-scroll with inertia, and long-press menus.
  A narrow, tall desktop window switches to the phone layout automatically.
- **Export/import** settings to a JSON file. The export includes known devices and their custom names. Chat and file history are optional, and so is the device identity (only for restoring the same device).

## Building

Requirements: CMake 3.16+, a C++17 compiler. Dependencies are fetched automatically (SDL2 2.32, Dear ImGui 1.93, nlohmann/json, qrcodegen, stb_image, Monocypher, KCP).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/lunashare_tests        # headless end-to-end tests (two engines over loopback)
```

- **Windows:** Visual Studio 2022 or MinGW/llvm-mingw. The result is a single `LunaShare.exe`.
- **Linux:** needs X11/Wayland and GL development packages for SDL (see `.github/workflows/build.yml`).
- **macOS:** builds `LunaShare.app`.
- **Android:** open `android/` in Android Studio, or run `gradle assembleRelease` in `android/`. The Gradle build fetches SDL's Java glue
  and builds the native code through the same top-level `CMakeLists.txt`. Needs NDK 27 and minSdk 24.
  On Android 11+ the app asks for *All files access* so it can send any file and save to `Download/LunaShare`.
  There are two flavors: `gradle assembleLiteRelease` (without calls) and `gradle assembleCallsRelease` (with the voice and video modules,
  plus the microphone and camera permissions).
- Options: `-DLUNASHARE_GL_API=GL3|GLES2|GLES3`, `-DLUNASHARE_SYSTEM_SDL=ON`, `-DLUNASHARE_BUILD_APP=OFF` (engine + tests only),
  `-DLUNASHARE_BUILD_VOICE=OFF` (skip the voice module, along with Opus and SpeexDSP), `-DLUNASHARE_BUILD_VIDEO=OFF` (skip the video module),
  `-DLUNASHARE_VIDEO_TEST_MODULE=ON` (development: a video module with a test-pattern camera on systems without a real backend).
- The voice module builds next to the executable. To ship it, put it next to `LunaShare.exe` or in a `modules/` folder (Windows/Linux),
  or in `LunaShare.app/Contents/MacOS` (macOS). The video module goes in the same place. `./build/lunashare_voice_tests` checks the audio pipeline
  (packet loss, jitter, echo cancellation), and `./build/lunashare_video_tests` checks the video core (keyframe recovery after loss, bitrate adaptation on a slow link).
- Offline builds: pass `-DFETCHCONTENT_SOURCE_DIR_<NAME>=/path` for `IMGUI`, `SDL2`, `NLOHMANN_JSON`, `QRCODEGEN`, `STB`, `MONOCYPHER`, `KCP`, `OPUS`, `SPEEXDSP`.

The GitHub Actions workflow builds Windows, Linux, macOS and the Android APK on every push and uploads them as artifacts.

Command line: `--portable` (keeps settings in `LunaShareData` next to the executable), `--config <dir>`.

## Layout

```
src/core/      engine (no UI dependencies)
  engine.*       connections, discovery, chat, resumable transfers, persistence
  protocol.*     framing: [u32 len][u8 type][payload]  (JSON control frames, binary data frames)
  hash.*         SHA-256, CRC-32
  crypto.*       handshake, frame encryption, identity keys, safety numbers
  mdns.*         Bonjour/mDNS service discovery (dns_sd on Apple, built-in elsewhere)
  wan.*          internet transport: rendezvous client, hole punching, NAT-PMP, KCP sessions
  rendezvous_server.*  the optional rendezvous server (also used by the tests)
server/        lunashare-rendezvous daemon + systemd unit
opnsense/      OPNsense plugin (Services > LunaShare rendezvous) + install.sh
  bluetooth.*    RFCOMM per platform + HostBridge hooks for Android
  model.*        settings / devices / messages / transfers / history + JSON
  platform.*     paths, MACs, interfaces, UTF-8 file I/O
src/media_api/ lunashare_media.h - the C ABI between the app and media modules
modules/voice/ voice module: Opus + SpeexDSP + jitter buffer (built as lunashare_media)
modules/vcam/  Windows 11 virtual camera media source (lunashare_vcam.dll, loaded by the camera Frame Server)
modules/video/ video module (lunashare_video): video_core.cpp (packets, keyframes, bitrate), video_win.cpp (Media Foundation),
               video_android.cpp + android/app/src/calls/java/.../VideoEngine.java (Camera2 + MediaCodec), video_fake.cpp (tests)
src/app/       SDL2 + Dear ImGui front end
  media_host.*   loads the media module, audio devices, ring tones, call state
  ui.cpp         desktop & phone layouts, chat, transfers, history, devices, connect, settings
  ui_viewer.cpp  thumbnails + full-screen picture viewer
  image_cache.*  async stb_image decoding, EXIF orientation, GL texture cache
  file_browser.* built-in file picker (works the same on phones)
  android_bridge.cpp  JNI glue to android/…/LunaShareActivity.java
tests/         engine tests (pause/resume, receiver restart, corruption detection, folders, export/import,
               encryption, key pinning, mDNS packets + live mDNS-only discovery)
```

## Protocol notes

0. Key exchange (see `src/core/crypto.h`). Everything below is sent encrypted.
1. Both sides send `Hello` (id, name, user, host, platform, MACs, identity public key, and a signature over the handshake). If two devices connect to each other at the same time, the duplicate connection is dropped deterministically.
2. The sender hashes the file, then sends `FileOffer` (tid, relative path, size, sha256, batch).
3. The receiver replies `FileAccept {offset}`, where offset is the size of its `.part` file. The same message is used to resume.
4. `FileData` frames: `[tid][offset][crc32][bytes]`. If an offset or CRC is wrong, the receiver sends a new `FileAccept` from the last good offset.
5. `FileDone` → the receiver hashes the file → `FileVerify {ok, sha256}`. The file is renamed into place only after the hash matches.
6. `FilePause` can come from either side. Resuming is done with an offer carrying `resume:true`, or with an accept carrying `resume:true`.
   After a reconnect, the sender re-offers everything that isn't finished, and the receiver answers based on its own state.

## Status / known limitations

- The Android project is complete but hasn't been built or run on a device yet. Expect small fixes on the first build.
- No iOS project yet. The core and UI compile for iOS in principle, but iOS can't create hotspots and has no RFCOMM (would need MultipeerConnectivity).
- Bluetooth on macOS isn't implemented. Linux Bluetooth uses a fixed RFCOMM channel (no SDP record), so Windows/Android→Linux connections
  need the same channel.
- The identity secret is stored in `settings.json` in the user's config folder, with the same protection as an SSH key. A settings export only includes it
  when you choose *include identity*.
- Protocol v2 (encrypted) can't talk to builds from before encryption was added.
- Voice calls are tested in code (signalling, packet loss/jitter, echo cancellation) but not yet with real microphones and speakers on every platform.
  Calls are one-to-one. Video calls are tested in code (packets, loss recovery, bitrate adaptation, and the full pipeline on a test-pattern camera),
  but the Windows and Android camera/codec code hasn't been tried on real devices yet. There's no video on Linux and macOS yet.

## License

LunaShare is licensed under the [GNU General Public License v3.0](LICENSE).
Third-party components keep their own licenses: Dear ImGui (MIT), SDL2 (zlib), nlohmann/json (MIT),
QR Code generator (MIT), stb_image (MIT / public domain), Monocypher (BSD-2-Clause / CC0), KCP (MIT),
and in the voice module Opus (BSD-3-Clause) and SpeexDSP (BSD-3-Clause).
