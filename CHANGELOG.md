# Changelog

## 1.2.5

- **Private contacts:** invite people with a code (*Settings → Internet → Private contacts*) instead of sharing your
  room. Each of them can reach you through the server, but people you invite separately never see each other.
  **Private mode** keeps your device out of the room's list. Needs the updated server (see docs/INTERNET.md).

## 1.2.4

- **Japanese and other scripts on Android:** characters that none of the app's fonts has (Japanese audio and
  subtitle track names, for example) are now drawn with the phone's own text renderer, which can use every
  system font. Before, they showed as boxes on phones whose CJK font couldn't be read.

## 1.2.3

- **Full screen without HDR (Windows):** if movies in full screen get too bright or washed out (Windows Auto HDR or
  the graphics driver's HDR for games, e.g. NVIDIA RTX HDR), turn on *Full screen without HDR* in the ☰ menu of the
  movie screen: full screen is then a borderless window that leaves one row of pixels uncovered, so it isn't
  taken for a game.
- **No macOS download for now:** only chat and files worked there; it can still be built from source.

## 1.2.2

- **Japanese, Chinese, Korean, Thai... on Android:** the phone's own fonts for other scripts are found through its
  font list (file names differ between phone makers), so e.g. "日本語" sound tracks and Japanese titles show their
  letters. On Windows the Japanese, Thai and Indian system fonts are used too.

## 1.2.1 - watching movies together, fixed

- **Voices in 5.1 movies:** surround sound is mixed down to stereo properly - before, only the front left and right
  channels were played, so the music came through but the dialogue (in the centre channel) didn't.
- **Sound tracks and subtitles** (watching together): the ☰ button picks the language of the sound and turns on
  subtitles - from the movie itself (text, and the picture subtitles of Blu-ray / DVD rips), from a file next to it
  (`movie.srt`, `movie.pl.srt`... - sent to everyone watching) or from any .srt / .ass / .vtt file. Polish subtitle
  files in Windows-1250 or ISO-8859-2 show their letters right. Each person chooses their own.
- **More movies play** on computers: AC-3, E-AC-3, DTS, TrueHD, MP2 and WMA sound, MPEG-2 and VC-1 video, 10-bit
  video (HDR / anime releases), .ts / .m2ts / .vob / .flv / .wmv files. When the main sound track can't be decoded,
  another one is used instead of none.
- **Subtitles at the right time:** subtitles from a movie file were shifted by the moment of their first line (often
  a minute or more), so they seemed not to show at all.
- **No more freezing when a movie changes:** when the host stopped or swapped the movie (or the player closed while
  the movie was still loading), the app could hang with an empty screen - the player waited for the app and the app
  for the player. The movie is now read without waiting for the rest of the app, and a closing player never waits
  for a device that went away.
- **MKV subtitles on computers:** FFmpeg on Windows / Linux now has zlib - mkvmerge compresses subtitle tracks with
  it, and without it they couldn't be read.
- **The host's subtitles for everyone:** viewers see the subtitles the host has on - also a file the host picked -
  as *Host's subtitles* (the default), and can still choose others for themselves.
- **Android: sound and subtitles from MKV files:** sound tracks and subtitles are read with FFmpeg (built into the
  app), the picture still by the phone's decoder - so AC-3 / DTS sound plays on phones without those decoders, and
  ASS and Blu-ray / DVD subtitles show.
- **Movie screen:** the picture fills the screen, the controls lie on top and hide after 3 seconds (tap / move the
  mouse to bring them back; on phones one tap shows them, another hides them). Phones turn sideways and hide the system bars (also with rotation locked); computers
  get *Full screen* (F or double-click). The screen stays on while the movie plays.

## 1.2.0 - groups, group calls and watching together

Groups, group calls and watching together work between devices that all have 1.2.0. Group calls *through the
server* also need the server updated to 1.2.0 (the OPNsense plugin's binary too); everything else works with a 1.1
server.

- **Library: Shared folders.** Put files into your *Shared* folder and your contacts can browse and download them
  (*Library* tab). A file is downloaded from every contact that has it at the same time, piece by piece, each piece
  checked by its hash; what you download is offered on to others, like a torrent within your contacts. See
  docs/LIBRARY.md.
- **Group chats.** *New group*: a name and the people. Messages go to every device of everyone in the group, also to
  people who have never met each other (the group vouches for their devices) and through the server's mailbox when
  someone is offline. Anyone in the group can add people and rename it, everyone can leave, whoever created it can
  remove people. Option *Group chats and calls through the server*: group members you don't know otherwise never
  connect to your device directly.
- **Watch together.** The ▶ button in a chat streams a movie from your device to the person or the whole group:
  everyone sees the same moment, anyone can pause or jump and it happens for everyone, and when someone is still
  loading, everyone waits for them. The original file is played (no re-encoding, nothing sent in advance); what
  arrived is deleted afterwards. Works together with a (group) call. See docs/WATCH.md.
- **Group calls (voice and video).** The call buttons in a group call everyone in it; whoever answers joins, everyone
  hears everyone (the voice module mixes them). In a video call you watch one person at a time: the one with the
  camera on who talks, or whoever you tap in the row of people (only the people watching you get your video, so it
  stays light). With *through the server* on, every device sends its voice and video once to the server, which
  passes it on - end-to-end encrypted with a key only the people in the call have; pictures are split into pieces
  with parity, so one lost piece in eight is repaired. Group video is limited to 720p / 30 fps.
- **Own picture in full quality:** a setting (and a switch in the camera panel) to see your own camera in the same
  quality as it's sent.
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
