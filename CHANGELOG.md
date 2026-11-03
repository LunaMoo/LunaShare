# Changelog

## 1.0.2

- **Louder calls:** the automatic gain levelled every voice at about -30 dBFS, which is quiet; speech is now sent at
  about -20 dBFS (around 9 dB louder) whatever the microphone level, with a soft limiter so it doesn't clip. Voice and
  video messages too.
- **Android:** the volume buttons during a call now change the call's actual volume (they changed an unused stream).
- **Video messages** are recorded in the resolution and frame rate set for video calls (was always 720p30).

## 1.0.1

- **Much better call audio:** voice is now 48 kHz fullband Opus (was 16 kHz wideband, which sounded like a radio) at
  48 kbit/s by default (adjustable 16–128 under *Advanced*), with gentler noise suppression. Voice and video messages
  are recorded in fullband too. Update both devices to hear the difference; older versions can still talk to 1.0.1.

## 1.0.0 - first release

LunaShare is a chat app built around sharing files, for Windows, Linux, macOS and Android, with optional voice/video
calls and an optional internet rendezvous server (also as an OPNsense plugin). Everything is end-to-end encrypted.

### What's in it
- **Chat and files:** text chat with emoji (colour, picker, reactions), deleting messages for yourself or for everyone,
  sending files and folders with pause / resume, SHA-256 checks on both sides, a shared-files history, picture and video
  thumbnails with a full-screen viewer, audio and video files playable in the chat, voice and video messages.
- **Finding each other:** local network (mDNS + broadcast), phone-to-phone over a Wi-Fi hotspot (QR code or Bluetooth
  handover), Bluetooth, address, and over the internet through a rendezvous server: direct peer-to-peer connections
  (hole punching, UPnP / NAT-PMP port mappings) with an encrypted relay through the server when no direct path exists.
- **Calls (optional modules):** voice calls (Opus, echo cancellation), video calls up to 4K120 / 8K30 on Windows, Linux
  and Android with H.264 / HEVC / AV1, smoothness-first quality adaptation, camera controls (zoom, exposure, focus, white
  balance...), using a phone as a webcam ("LunaShare Camera" on Windows 11 and on Linux with v4l2loopback).
- **Settings:** only your name and the server need setting; everything else is automatic and under *Advanced*.
- **Languages:** English and Polish, switchable at any time.

### Tested on real devices
- **Android ↔ Android video calls: tested, work well** (smooth picture, camera settings, hanging up from the call screen).
- Windows PC ↔ Android phone: chat, file transfers, voice calls.
- Over the internet through the rendezvous server on an OPNsense router (phone on mobile data ↔ PC at home).
- Sending and viewing pictures, videos and audio files in the chat; deleting messages, including file messages.

### Not yet tried on real hardware
- Linux video: V4L2 webcams, GPU encoding / decoding (NVENC, VA-API, NVDEC) and the v4l2loopback virtual camera. The
  encoder and decoder were tested end to end with a test picture, and the GPU paths fall back to the CPU automatically.
- The Windows 11 virtual camera, and Windows ↔ Linux / Linux ↔ Android video calls.
- The server relay between two networks that can't reach each other directly (tested in code).
- macOS has no video calls yet; iOS is planned.

### Downloads
- `LunaShare-<version>-windows-x64.zip`, `-linux-x64.zip`, `-macos-arm64.zip`: unzip and run. The `modules/` folder holds
  the optional parts (calls, video, player, emoji); delete it for a chat + files only install.
- Android: `LunaShare-<version>-calls-release.apk` (with calls) or `LunaShare-<version>-lite-release.apk` (chat and files
  only, no microphone/camera permission). The APKs are signed with a development key, so Android asks to allow installing from this source.
- `lunashare-opnsense.tar.gz`: the rendezvous server as an OPNsense plugin (see docs/INTERNET.md).
