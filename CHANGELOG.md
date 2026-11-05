# Changelog

## Unreleased

- **Library: Shared folders.** Put files into your *Shared* folder and your contacts can browse and download them
  (*Library* tab). A file is downloaded from every contact that has it at the same time, piece by piece, each piece
  checked by its hash; what you download is offered on to others, like a torrent within your contacts. See
  docs/LIBRARY.md.
- **Clearer call buttons:** a real phone receiver icon, words under the call buttons on phones, green call buttons,
  "Answer" for incoming calls.

## 1.1.1

- **One Android app:** "LunaShare" now always includes voice and video calls (the separate "LunaShare + Calls" app is
  gone; it was only about 2 MB larger). Microphone and camera are asked for only when you first call or record.
- **Coming from "LunaShare + Calls":** in the old app save a backup (*Settings → Your account → Save backup file…*,
  e.g. to Downloads), install LunaShare and restore it there (*Restore account… → Open a backup file…*), then uninstall
  the old app. Your account, contacts and chats move over.

## 1.1.0 - people, accounts and messages that wait

Update all your devices and your server (the OPNsense plugin too). Devices with 1.0.x still talk to 1.1.0, as
separate devices without accounts.

- **Profile pictures:** choose and crop a picture in *Settings → Profile*; everyone you connect to gets it and keeps a
  copy. Shown in the chat list, the chat header and when they call.
- **Accounts:** every device now has an account - a key that belongs to you, no sign-up. Link your phone and computer
  (*Settings → Your account → Link another device…*, both show the same 6-digit code): messages to you reach all your
  devices, what you write on one appears on the others, and your contacts see one person. Calls ring all of the
  person's devices; the first to answer takes the call.
- **Recovery:** a 24-word recovery phrase and password-protected backup files bring your account back on a new device.
- **Offline messages:** when someone isn't reachable, text messages wait on your server (up to 10 days, 10 MB per
  device), sealed so the server can read neither the text nor who sent it. Messages show *waiting on the server*,
  *delivered* and *read*.
- **Background:** on phones LunaShare stays connected in the background (a small notification) and shows notifications
  for messages and calls, with a full-screen ringing call screen (Answer / Decline). Back on the home screen keeps it
  running. On computers it can keep running when the window is closed (in the tray on Windows).
- **Usernames:** claim a username on your server (*Settings → Your account*) so others can find you
  (*Connect → Find someone by username*).
- Server: offline mailboxes and usernames are kept in `-d DIR` (OPNsense: `/var/db/lunashare`); new options
  `--mailbox-days`, `--mailbox-mb`, `--mailbox-total-mb`, `--no-mailbox`.

## 1.0.3

- **Call settings during calls:** the settings button (sliders) is now in every call, voice or video. It opens one panel
  with all sound settings (microphone, speaker, echo cancellation, noise suppression, automatic level, voice quality,
  microphone boost) and, with the camera on, the camera and video quality settings.
- **Sound settings apply immediately** during a call (before, changes only took effect in the next call).
- **Switching speaker / headphones / loudspeaker** during a call no longer disturbs the other side: the echo canceller
  starts over for the new sound path instead of using what it learned for the old one (which could leave echo or cut
  into your voice), it covers longer phone audio delays (400 ms), and the automatic level no longer amplifies leftover
  echo while the other side is talking.

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
