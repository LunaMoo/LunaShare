# Changelog

## Unreleased

- Music: songs in a playlist can be put in another order - drag them (computer), or *Move up / down / to the top /
  to the bottom* in a song's menu (long press on a phone).
- Android: **picture in picture** - leaving the app (home button / gesture) while a movie plays keeps it playing in a
  small window (Android 8+; an option, on by default), or *Small window now* in the player's menu.
- Movie player: **speed** 0.5x - 2x (the sound keeps its pitch; watching together, it's everyone's), subtitles
  **later / sooner** in half-second steps (this movie) and their **text size** (remembered). The player module API
  is version 5 (set_speed, set_sub_delay).
- Movie player on phones: a double tap on the left / right third goes 10 s back / on; a swipe up or down changes the
  screen's brightness (left half) or the volume (right half). Previous / next sit next to play / pause on phones.
- Movies continue where you left them (alone, or as the host) - "Continued from 12:34" with *From the start* for a
  moment; the Movies tab shows how far each one was watched, and a tick on the ones seen to the end.

## 1.5.5

- Movie player options (in the sound / subtitles / picture menu, remembered for every movie):
  - **Play the next one automatically**: at the end of a movie the next one in its folder starts (watching together,
    everyone comes along);
  - **Hide the controls while playing**: the moment a movie starts playing - a new one, or after a pause - the
    controls go; only a tap / click brings them back (moving the mouse doesn't);
  - Android: **Turn the screen by the movie's shape**: a movie taller than wide plays upright, a wide one sideways.

## 1.5.4

- **Chat history** (the clock button in a chat's header), see docs/CHAT.md:
  - archive a chat (compressed, on this device) and optionally clear it - keeping yesterday's and today's messages
    if you like; files aren't copied, the messages keep their links, so files still on the disk open from a
    restored chat like before;
  - a list of the archives (of this chat or all of them): restore (only what the chat doesn't have) or delete;
  - lost both? Ask the other side (and your other devices) to send the history back - they're asked first.
- Links in messages (`https://...`, `www....`) are links: a tap opens them in the browser.
- **Movies** tab (a *Movie player* screen on phones): the videos in your folders (received files and Videos always
  included) as pictures with their length, by folder; search, sort, *Open a file...*. The Library's *Movie player*
  button goes there.
- Movie player: **<** and **>** on the sides go to the previous / next movie in the folder (or in the contact's
  Shared folder it came from); watching together, everyone comes along to the next one.
- Listening together: smooth transitions and remix work too (set in *Sound...*, for everyone) - the same part of
  every song and the same blend into the next one for everybody.
- Listening together: shuffle and repeat work (for everyone - anyone can switch them): repeat the song or the whole
  shared queue; shuffle plays the songs not played yet in random order, and the one picked to come next is fetched
  ahead. Songs from contacts' Shared folders follow the player's shuffle / repeat too.
- Android: typing with the on-screen keyboard over a field (a dialog, the account backup, the folder picker...): the
  text is shown again in a strip just above the keyboard, with its cursor - tap (or slide) in the strip to put the
  cursor there. The app also measures the keyboard's real height now (it used to guess).
- Movie player: *Autoplay* (remembered for every movie) - no need to press play. A big play / pause sign in the
  middle of the picture: tapping it plays or pauses while the controls are showing.

## 1.5.3

- Listening together:
  - starting it, you choose what to start with: the song that's playing now (it goes on from the same moment for
    everyone, followed by the rest of your queue), one of your playlists, or nothing yet;
  - *Add a playlist...* puts a whole playlist of yours into the shared queue (for everyone listening);
  - joining pauses your own music, so you hear the shared queue only;
  - no more empty "Info" notifications piling up while listening together.
- Library: a *Music player* button next to the movie player.
- The folder picker (e.g. *Add a music folder*) fits a phone's screen: *Use this folder* and *Cancel* are always
  visible.
- The separate *Remix* button under the player is gone; remix is switched in *Sound...*.

## 1.5.2

- **Listening together** (music player, stage 3, see docs/MUSIC.md): invite a person or a group; everyone adds songs
  from their own music to a shared queue, each song streams from the device it's on, and everyone hears the same
  moment. Anyone can pause, skip, seek or pick a song; songs can be saved.
- Songs in contacts' Shared folders play in the music player (while they come), with next / previous through the
  folder and *Save*.
- Remix: the part of every song now comes from a random place - still past the intro and ending well before the
  outro and the quiet at the end.
- Music: the folders window fits the phone screen (the *Add a folder* button first, the list scrolls); a new
  shuffle icon (crossing arrows); bigger buttons under the phone's player.

## 1.5.1

- Music player, stage 2 (*Sound...*, see docs/MUSIC.md):
  - a ten-band equalizer with presets and a preamp;
  - smooth transitions between songs (0-12 s crossfade);
  - **remix**: only a part of every song (30-90 s, skipping the beginnings and ends), blending into a part of the next.
- Android: the music no longer stops a moment after the app goes to the background (the system's pause of the app
  stopped all its sound while the time in the notification went on).

## 1.5.0

- **Music player** (*Music* tab; *Music player* on a phone's main screen), see docs/MUSIC.md:
  - a library of your music folders, plus everything you receive; tags and covers read from the files (or
    `cover.jpg` next to them);
  - songs, artists, albums, genres, folders and your own playlists; search and sorting;
  - repeat (the list or one song) and shuffle; a mini player at the bottom of every screen;
  - plays on in the background: on Android with the media notification, lock screen, headset buttons and pausing
    for calls; on Windows with the keyboard's media keys;
  - pauses for movies, calls and voice messages and goes on afterwards;
  - send songs, selections, whole albums (as a folder) or playlists to people and groups.
- Android: sound in the background is no longer cut off by the system pausing the app's audio (calls in the
  background keep their sound too); a movie or chat video pauses when the app goes to the background.

## 1.4.1

- Loop: the movie no longer stands still for about two seconds when it starts again on the device that plays it
  (the jump back to the start waited for the pause between two seeks).

## 1.4.0

- **One movie screen everywhere:** videos opened from a chat now play on the same full-screen movie player as the
  movie player and watching together - sound tracks, subtitles, loop, full screen, adding people. Leaving it brings
  you back to the pictures.
- **Picture settings:** brightness, contrast, gamma, saturation, colour temperature and tint in the movie's ☰ menu
  (remembered; double-click or right-click a slider to reset it).
- **Video thumbnails in the file picker:** the gallery shows a picture from each video with its length.
- Android: 10-bit videos (HDR recordings of newer phones) no longer play green; the decoder's crop is respected.
- Android: no pause when a looping movie starts again (the audio device stays open).
- Thumbnails that failed (the phone's decoders busy) are tried again.

## 1.3.1

- **Saving goes on after the movie:** a copy being saved keeps coming when you leave or the host stops the movie
  (shown under *Library → Downloads*); a movie from a Shared folder that you saved keeps downloading after closing
  the player.
- **The host can turn saving off:** *Let the others save a copy* in the movie's ☰ menu (remembered). It doesn't
  apply to movies from Shared folders.

## 1.3.0

- **Movie player:** play a movie on your own (*Library → Movie player...*, on phones on the main screen) and add a
  person or a group at any time with **+** - from then on you watch it together.
- **Play from Shared folders:** videos and music in your contacts' Shared folders have a *Play* button. They stream
  piece by piece (from every device that has them, the part being played first); the person sharing doesn't need to
  do anything. Nothing is kept unless you save it.
- **Loop:** a loop button for watching together (for everyone) and for videos and audio in chats (remembered).
- **Save a copy:** when watching someone's movie, ↓ fetches the rest in the background and saves it to Downloads;
  a movie streamed from a Shared folder is saved into your Shared folder (each piece verified).

## 1.2.10

- **Invites are easy to find:** the *Invites* section (use a code you got, invite someone) is now the first thing in
  the settings instead of under *Advanced*, and it also opens by clicking your own name at the top of the device list
  (phones: *Invite someone / use an invite code* on the main screen). *Private mode* stays under *Advanced*.
- The "cannot resolve the server address" status is translated again.

## 1.2.9

- Phones: the *Create invite code* and *Use code* buttons go under their fields on narrow screens instead of past the
  right edge, where they couldn't be seen.

## 1.2.8

- **Invited devices stay limited after reinstalling:** devices send the server a per-server fingerprint of the computer
  or phone (a hash of the system's machine id), so a fresh install or a new account on a remembered device is limited
  too. Releasing someone releases every install on that device. (Resets with an OS reinstall or factory reset.)
- **Invite rooms:** when creating an invite code you can put the person into a named room (*No room* / an existing
  room / *New room...*). Everyone invited into the same room sees each other (and you); people without a room or in
  another room still see only you. *Delete room* keeps them as your contacts but they stop seeing each other.
- **The server shows who invited whom and into which room:** the OPNsense list (and `--list-invited`) has the
  inviter's name and key and the invite room for every remembered device.

## 1.2.7

- **The server remembers invited people:** a device that joined with an invite code (and its account's other
  devices) stays limited to the people who invited it, even if it later enters the server's address by hand: it sees
  no one else, isn't listed in rooms, can't use usernames and can't invite others to the server. The admin can release
  someone (OPNsense: a new list with *Release* buttons; elsewhere `--list-invited` / `--release KEY`).
- The OPNsense plugin now always gives the server a data directory (needed to remember them).

## 1.2.6

- **Invite-joined devices don't see your server:** when someone joins with your invite code, the app doesn't show the
  server's address, rooms, usernames or username search, and they can't make invite codes to it. They get a room
  nobody else uses, so they see no one on the server but you. "Use my own server instead" drops the invite.

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
