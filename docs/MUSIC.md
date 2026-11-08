# Music player

*Music* (a tab on computers; *Music player* on a phone's main screen) plays the music on this device.

## Library

- **Folders:** *Folders... → Add a folder...* adds a folder with everything under it. The folder for received files
  is always included, so music people send you - or that you download from their Shared folders - shows up by itself.
  The folders are looked through again when something new arrives, and with *Look again*.
- **Tags:** title, artist, album, album artist, genre, year, track and disc number, length - read from the files
  (MP3 ID3, MP4/M4A, FLAC and Ogg/Opus comments...) by the player module. Without tags the file and folder names are
  used.
- **Covers:** the picture inside the file (kept once per picture in `music_covers/` in the settings folder), or
  `cover.jpg` / `folder.jpg` / `front.jpg` lying next to the music.
- **Views:** Songs (sorted by title, artist, album or recently added), Artists, Albums, Genres, Folders and Playlists,
  with a search over title, artist, album and genre.
- The library is kept in `music.json`; what was playing (the queue, the position, repeat and shuffle) in
  `music_state.json`, so it can go on after a restart.

## Playing

- Tapping a song plays the list it's in from there (an album, an artist, a playlist, the search result...).
- A song's menu (right-click, or long-press on a phone): play next, add to the queue, add to a playlist, send,
  select (then send or add all selected at once).
- **Repeat:** off → the list → this song. **Shuffle:** a random order where every song plays once before any comes
  again.
- The **mini player** at the bottom (above the phone's navigation, in the desktop sidebar) shows the song with play /
  pause and next; tapping it opens the player.
- Music pauses on its own for a movie, a chat video or voice message, a call or a recording, and goes on afterwards.

## Sound

*Sound...* (next to *Folders...* on a computer; under the song on a phone's player) holds:

- **Equalizer:** ten bands from 31 Hz to 16 kHz (±12 dB) with presets (Flat, Bass boost, Treble boost, Vocal, Rock,
  Pop, Jazz, Classical, Electronic, Loudness) and a preamp. Moving a band makes it *Custom*; double-click a band to
  set it back to 0. Loud peaks are rounded off instead of clipping.
- **Smooth transition** (0-12 s): the next song starts while this one fades out (an equal-power crossfade, so the
  loudness stays even). It follows repeat and shuffle; with repeat off the last song simply ends.
- **Remix:** only a part of every song plays - the chosen length (30, 45, 60 or 90 s) from a random place, but always
  past the intro (at least 8 s / 8 % in) and with the whole part ending before the outro and the quiet at the end (at
  least 12 s / 10 % before the end) - blending into a part of the next song over the *blend* time (1-12 s). Songs too
  short for that play from about a fifth in. With repeat off the remix stops at the end of the last part. The switch
  is also right on the phone's player.

All of it is kept with the player's state in `music_state.json`.

## Listening together

*Listen together...* (next to *Sound...*; on a phone under the player, or next to *Music folders...*) picks a person or
a group; they get an invitation in the chat and on their music screen, and *Join*.

- Everyone adds songs **from their own music**: while listening together, tapping a song adds it to the **shared
  queue** (an album's or a playlist's *Add all* / *Shuffle* add the whole lot). Each song streams straight from the
  device it's on - nothing is sent first - in the same way as watching a movie together; the song that's on and the
  next one are fetched whole ahead of time, so the change from one to the next is smooth.
- The one who started it keeps the queue and the clock; everyone's player follows it (a jump when someone is more than
  a moment off). Anyone can play / pause, skip, go back, seek or tap a song in the queue to play it; you can remove
  your own songs, the host any.
- *Save* keeps a copy of the song that's on (someone else's) in your received files - so it lands in your library.
- *Leave* - your songs that haven't played leave with you. *End for everyone* (the host) ends it.
- The equalizer works as always; smooth transitions and remix are for your own queue.

## Contacts' Shared folders

In *Library*, a song in a contact's Shared folder has *Play*: it opens in the music player and plays while it comes
(like a movie in the movie player). Next / previous go through the songs of that folder; *Save* keeps it (it goes to
your Shared folder, so it's in your library from then on).

## In the background

- **Android:** the music plays on with the app in the background or the screen off. The notification (and the lock
  screen) shows the cover and title with previous / play-pause / next; headset and Bluetooth buttons work; a phone
  call or another app playing pauses it (it comes back after the call), navigation prompts make it quieter for a
  moment, and pulling out headphones pauses it. Swiping the notification away (when paused) stops the player.
- **Windows:** it plays on when the window is minimized or closed to the tray; the keyboard's media keys (play /
  pause, next, previous, stop) work from anywhere while music is loaded.
- **Linux:** it plays on when minimized.

## Sending

*Send...* on a song, a selection, an album, an artist or a playlist sends it to a person or a group like any files
(resumable, checked with SHA-256). An album, an artist or a playlist goes as one folder named after it, with the
album's cover picture when there is one next to the music; single songs and selections go as files.
