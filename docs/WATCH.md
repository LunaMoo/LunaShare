# Watch together

The ▶ button in a chat (with a person or a group) picks a movie on your device and starts watching it together.
Everyone gets an invitation in the chat (*Watch*); whoever joins sees the same moment of the movie as everyone else.
Anyone can play, pause or move to another moment, and it happens for everyone. If someone is still loading - they
just joined, someone jumped ahead, or their connection is slow - everyone waits for them ("Waiting for Kasia...")
and then all go on together. *Back* returns to the chat while the movie goes on; a group or voice call works at
the same time (mute and hang up are on the movie screen).

## Movie player

*Library → Movie player...* (phones: *Movie player* on the main screen) plays a movie on its own, on the same screen.
The **+** button (*Add a person*) invites a person or a group at any moment; from then on it is watching together,
as above - you can add more people later too. A movie from a contact's Shared folder can be played the same way
(*Play* next to it in the Library, see [LIBRARY.md](LIBRARY.md)).

## Loop and saving

- The loop button starts the movie again at the end - for everyone (any of you can turn it on or off). Videos and
  audio in chats have their own loop button (remembered).
- Viewers can **save a copy** (↓): the rest of the movie is fetched in the background next to what's being played,
  then copied to *Downloads* ("saving: 63%" under the seek bar). Leaving or the host stopping before it's all here
  cancels it. A movie played from someone's Shared folder is saved into your Shared folder instead, like any library
  download (each piece checked against its hash).

## Sound, subtitles, full screen

- The ☰ button: the sound track (language) and subtitles. Subtitles come from the movie itself (text, or the
  picture subtitles of Blu-ray / DVD rips), from files next to the movie with the same name (`movie.srt`,
  `movie.pl.srt`, `movie.eng.ass`... - the device sharing it sends them to everyone) or from any subtitle file you
  pick. Everyone chooses their own. Subtitle files in Windows-1250 / ISO-8859-2 (common for Polish) are recognised.
- Surround sound (5.1, 7.1) is mixed down to stereo with the centre channel - the voices - in both speakers.
- The controls hide after 3 seconds while the movie plays; tap the screen or move the mouse to show them. Phones turn
  sideways and hide the status / navigation bars; on computers *Full screen* (F, double-click; Esc leaves it).

## How it works

- Nothing is sent before watching. The movie stays on the device that shared it; everyone else's player reads it
  through the connection: the parts it needs, in 256 KB blocks, up to 32 MB ahead of where it plays. What arrived is
  kept in a temporary file, deleted when you leave. It's the original file - no re-encoding, the same quality, and
  no work for the phone that shares it beyond reading the file.
- The device that shared the movie keeps the clock (playing / paused, and the position). Viewers send it their
  play / pause / seek; it tells everyone. Every second it sends the clock again; players more than a second off jump
  to it. Viewers tell it whether they have enough loaded (4 MB ahead after waiting); while someone doesn't, the clock
  stands still.
- It needs a connection to the device with the movie (on the same network, or over the internet - directly or relayed
  by your server). A movie the player can play from a file can be watched together (on phones: what Android plays).

## Messages

`WatchInvite {sid, name, size[, gid]}`, `WatchJoin`, `WatchLeave {end}`, `WatchCtl {playing, pos[, loop]}`,
`WatchState {playing, pos, hold, wait, people, hsub, loop}`, `WatchReport {buffering}`, `WatchRead {off, len}` and the binary
`WatchData [sid u64][offset u64][data]` (sent before other bulk data on the connection). The player module reads
through `open_stream()` (player API 2): FFmpeg with a custom AVIOContext on computers, a `MediaDataSource` on Android.
