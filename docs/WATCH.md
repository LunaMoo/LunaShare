# Watch together

The ▶ button in a chat (with a person or a group) picks a movie on your device and starts watching it together.
Everyone gets an invitation in the chat (*Watch*); whoever joins sees the same moment of the movie as everyone else.
Anyone can play, pause or move to another moment, and it happens for everyone. If someone is still loading - they
just joined, someone jumped ahead, or their connection is slow - everyone waits for them ("Waiting for Kasia...")
and then all go on together. *Back* returns to the chat while the movie goes on; a group or voice call works at
the same time (mute and hang up are on the movie screen).

## Movie player

*Library → Movie player...* (phones: *Movie player* on the main screen) plays a movie on its own, on the same screen.
Videos opened from a chat play on it too (back returns to the pictures).
The **+** button (*Add a person*) invites a person or a group at any moment; from then on it is watching together,
as above - you can add more people later too. A movie from a contact's Shared folder can be played the same way
(*Play* next to it in the Library, see [LIBRARY.md](LIBRARY.md)).

## Picture

The ☰ menu has the picture settings: brightness, contrast, gamma, saturation, colour temperature (cool - warm) and
tint (green - magenta). They apply to every movie on this device and are remembered; double-click or right-click a
slider to put it back, *Reset picture* for all. (Done on the graphics card, where the picture is turned from YUV to RGB.)

## The Movies tab, previous / next

- *Movies* (a tab on computers; *Movie player* on a phone's main screen and in Library): the videos in your folders -
  received files and the system's Videos folder (on Android: Movies and DCIM) always, plus any you add in *Movie
  folders...* - as pictures from the movies with their length, by folder (natural order: "Episode 2" before
  "Episode 10"), or all together by date / size; a search; *Open a file...* for anything else.
- In the player, **<** and **>** on the left and right go to the previous / next movie in the same folder (or in the
  contact's Shared folder it was started from). Watching together, the host's switch takes everyone along: their
  player goes straight on to the next movie.

## Speed and subtitles

In the sound / subtitles / picture menu: *Speed* 0.5x, 0.75x, 1x, 1.25x, 1.5x, 2x - the sound keeps its pitch, and
watching together it's everyone's speed. Under the subtitles: *-0.5 s* / *+0.5 s* when they're out of step with the
voices (this device, this movie) and the subtitles' text size (remembered).

## Gestures (phones)

- Double tap the left / right third of the picture: 10 seconds back / on (for everyone, watching together).
- Swipe up / down on the left half: the screen's brightness (only while the movie shows); on the right half: the
  volume.

## Where you left it

A movie you open again (alone, or as the host) continues where you stopped - "Continued from 12:34", with *From the
start* for a few seconds. Played to the end (95 %, or the last minute of a long one), it's marked as seen and starts
from the beginning next time. The Movies tab shows a red bar for how far each movie got and a tick on those seen.
(Kept in `movie_positions.json` in the settings folder.)

## Autoplay and play / pause

Options in the player's sound / subtitles / picture menu, remembered for every movie:

- *Autoplay* - and *Play the next one automatically*: at the end, the next movie in the folder (watching together,
  everyone comes along).
- *Hide the controls while playing*: they go the moment it starts playing (a new movie, or after a pause); only a tap
  or a click brings them back.
- Android: *Turn the screen by the movie's shape*: a movie taller than wide (phone videos) plays upright, others
  sideways.

- *Autoplay* (in the movie player's sound / subtitles / picture menu, remembered for every movie): a movie you open -
  alone, together or from a Shared folder - starts playing on its own.
- With the controls showing, a tap (or click) in the middle of the picture - on the big play / pause sign - plays or
  pauses. On a phone, a tap elsewhere hides the controls; on a computer a click anywhere still plays / pauses, a
  double-click is full screen.

## Loop and saving

- The loop button starts the movie again at the end - for everyone (any of you can turn it on or off). Videos and
  audio in chats have their own loop button (remembered).
- Viewers can **save a copy** (↓): the rest of the movie is fetched in the background next to what's being played,
  then copied to *Downloads* ("saving: 63%" under the seek bar). Leaving, or the host stopping the movie, doesn't stop
  it: the rest keeps coming from the host's device (shown under *Library → Downloads*, where it can be cancelled) as
  long as the host's app runs; it waits while the host is offline.
- The host decides: *Let the others save a copy* in the ☰ menu (on by default, remembered). Turning it off hides the
  button for everyone and stops copies still coming, also of movies that already ended.
- A movie played from someone's Shared folder is always saveable - sharing is what that folder is for. It's saved
  into your Shared folder like any library download (each piece checked against its hash), and closing the player
  doesn't stop it.

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
